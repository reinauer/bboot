/*
 * BBoot boot loader support for e500 machines (AmigaOne A1222 kernel)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QEMU's e500 machines (ppce500, mpc8544ds) start a -kernel the ePAPR way:
 * r3 points to the flattened device tree, r6 holds the ePAPR magic and r7
 * the size of the one TLB1 entry mapping memory from 0.  We take memory,
 * initrd and bootargs from the tree, map memory and CCSR the way U-Boot
 * leaves them on the A1222, and hand the A1222 loader the U-Boot board
 * info, extended with a pointer to a copy of the tree.
 */

#include <stdio.h>
#include <string.h>
#include <types.h>
#include "u-boot.h"
#include "bboot.h"

/* The A1222 loader takes the board info with this magic at 0x3c and the
 * device tree at 0x44 */
typedef struct {
    bd_t bd;                /* 0x00-0x37 */
    uint32_t pad38;
    uint32_t magic;         /* 0x3c */
    uint32_t pad40;
    void *fdt;              /* 0x44 */
    uint32_t pad48[6];
} a1222_bd_t;

#define A1222_BD_MAGIC 0x12345678

static a1222_bd_t bd;
static const uint8_t *fdt;
static uint64_t ccsr_base;

/* Flattened device tree reading, just enough for /memory, /chosen and the
 * soc node */
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

static uint32_t be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* Find property name in the first node whose name, up to any @unit-address,
 * is node, at depth 1.  Returns its value and stores its length. */
static const void *fdt_prop(const char *node, const char *name, int *lenp)
{
    const uint8_t *st = fdt + be32(fdt + 8);
    const char *strs = (const char *)fdt + be32(fdt + 12);
    const uint8_t *p = st;
    int depth = 0, match = 0, nlen = strlen(node);

    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *n = (const char *)p;
            depth++;
            if (depth == 2)
                match = !strncmp(n, node, nlen) && (n[nlen] == '\0' || n[nlen] == '@');
            p += ROUND_UP(strlen(n) + 1, 4);
            break;
        }
        case FDT_END_NODE:
            if (depth == 2) match = 0;
            depth--;
            break;
        case FDT_PROP: {
            uint32_t len = be32(p), off = be32(p + 4);
            p += 8;
            if (match && depth == 2 && !strcmp(strs + off, name)) {
                if (lenp) *lenp = len;
                return p;
            }
            p += ROUND_UP(len, 4);
            break;
        }
        case FDT_NOP:
            break;
        default:
            return NULL;
        }
    }
}

/* A number of one or two cells */
static uint64_t fdt_num(const void *p, int cells)
{
    return cells == 2 ? (uint64_t)be32(p) << 32 | be32((const uint8_t *)p + 4) : be32(p);
}

/* Write a TLB1 entry; size is log2(size)-10, which must be even on e500v2 */
static void tlb1_write(int esel, uint32_t va, uint64_t pa, int tsize, uint32_t wimge)
{
    uint32_t mas0 = 0x10000000 | (esel << 16);
    uint32_t mas1 = 0x80000000 | 0x40000000 | (tsize / 2) << 8;   /* V, IPROT */
    uint32_t mas2 = va | wimge;
    uint32_t mas3 = (uint32_t)pa | 0x3f;                              /* UX SX UW SW UR SR */
    uint32_t mas7 = pa >> 32;
    asm volatile("mtspr 624,%0; mtspr 625,%1; mtspr 626,%2; mtspr 627,%3; mtspr 944,%4;"
                 "isync; msync; tlbwe; isync"
                 :: "r"(mas0), "r"(mas1), "r"(mas2), "r"(mas3), "r"(mas7) : "memory");
}

static void *e500_claim(void *addr, unsigned int size)
{
    return (unsigned long)addr + size < bd.bd.bi_memsize ? addr : NULL;
}

static unsigned long initrd_start, initrd_end;
/* A copy of the bootargs: the loader copies them to the kernel only after
 * putting the kernel where QEMU put the tree */
static char bootargs[256];

int e500_init(void *dt)
{
    fdt = dt;
    if (be32(fdt) != 0xd00dfeed) return -1;

    int len;
    const uint8_t *p = fdt_prop("memory", "reg", &len);
    /* QEMU's e500 trees have #address-cells and #size-cells 2 at the root */
    uint64_t memsize = p && len >= 16 ? fdt_num(p + 8, 2) : 256 << 20;
    if (memsize > 0x80000000ULL) memsize = 0x80000000ULL;

    p = fdt_prop("chosen", "linux,initrd-start", &len);
    if (p) initrd_start = fdt_num(p, len / 4);
    p = fdt_prop("chosen", "linux,initrd-end", &len);
    if (p) initrd_end = fdt_num(p, len / 4);
    p = fdt_prop("chosen", "bootargs", &len);
    if (p && len > 1) {
        strncpy(bootargs, (const char *)p, sizeof(bootargs) - 1);
        bootargs[sizeof(bootargs) - 1] = '\0';
    }

    /* CCSR is the soc node's ranges: one cell of child address, two of
     * parent address and one of size */
    p = fdt_prop("soc", "ranges", &len);
    ccsr_base = p && len == 16 ? fdt_num(p + 4, 2) : 0xe0000000;

    /* All memory up to 2 GiB, as U-Boot maps it, in place of QEMU's one
     * entry for the first few MiB: entry 0 is the one we run in, so it's
     * only ever made bigger.  Then CCSR, uncached and guarded, at the
     * virtual address U-Boot uses on the P1022, 0xffe00000. */
    tlb1_write(0, 0, 0, 20, 0x4);                   /* 1 GiB, M */
    if (memsize > 0x40000000)
        tlb1_write(1, 0x40000000, 0x40000000, 20, 0x4);
    tlb1_write(2, 0xffe00000, ccsr_base, 10, 0xa);  /* 1 MiB, I G */
    if ((uint32_t)ccsr_base == ccsr_base && ccsr_base != 0xffe00000)
        tlb1_write(3, (uint32_t)ccsr_base, ccsr_base, 10, 0xa);
    /* The loader prints its errors to the DUART at 0xfe11c500, where it is
     * on the P5020 of the X5000 */
    tlb1_write(4, 0xfe11c000, ccsr_base + 0x4000, 2, 0xa);  /* 4 KiB, I G */

    brd.claim = &e500_claim;
    brd.setup = NULL;
    brd.exec_addr = (void *)0x1000000;
    brd.serial_base = 0xffe04500;
    brd.info = &bd;

    bd.bd.bi_memstart = 0;
    bd.bd.bi_memsize = memsize;
    bd.bd.bi_flashstart = 0xfff00000;
    bd.bd.bi_flashsize = 1;
    bd.bd.bi_bootflags = 1;
    bd.bd.bi_intfreq = 1066000000;
    bd.bd.bi_busfreq = 400000000;
    bd.bd.bi_baudrate = 115200;
    bd.magic = A1222_BD_MAGIC;
    return 0;
}

void e500_get_initrd(unsigned long *start, unsigned long *len)
{
    *start = initrd_start;
    *len = initrd_end > initrd_start ? initrd_end - initrd_start : 0;
}

char *e500_bootargs(void)
{
    return bootargs;
}

/* The loader puts the kernel above 24 MiB, where QEMU put the tree, so
 * give it a copy at avail */
unsigned long e500_place_fdt(unsigned long avail)
{
    unsigned long size = be32(fdt + 4);
    avail = ROUND_UP(avail, 64);
    if (!e500_claim((void *)avail, size)) return 0;
    memcpy((void *)avail, fdt, size);
    bd.fdt = (void *)avail;
    printf("ccsr 0x%lx%08lx, memory %lu MiB, device tree at 0x%lx (%lu bytes)\n",
           (unsigned long)(ccsr_base >> 32), (unsigned long)ccsr_base,
           (unsigned long)(bd.bd.bi_memsize >> 20), avail, size);
    return avail + size;
}

/* Call the loader on a stack near the top of memory, where U-Boot's is: the
 * loader uses the memory below its own code at 16 MiB, where QEMU starts
 * the stack, as scratch space */
void e500_start_loader(void *entry, const char *id, void *kicklist, void *info, char *args)
{
    unsigned long sp = ROUND_DOWN(bd.bd.bi_memsize - 0x10000, 16);
    register unsigned long r3 asm("r3") = (unsigned long)id;
    register unsigned long r4 asm("r4") = (unsigned long)kicklist;
    register unsigned long r5 asm("r5") = (unsigned long)info;
    register unsigned long r6 asm("r6") = (unsigned long)args;
    asm volatile("mtctr %4; mr 1,%5; li 0,0; stw 0,0(1); bctrl"
                 : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
                 : "r"(entry), "r"(sp - 16)
                 : "r0", "ctr", "lr", "memory");
}
