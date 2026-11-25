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

/* Local access windows: the A1222's AmigaOS kernel finds its PCIe
 * controllers by them, so set them up as U-Boot does, at the 36-bit
 * addresses the kernel has built in for the first controller, where QEMU's
 * a1222 machine has its PCI controller */
#define CCSR_VA 0xffe00000
#define LAW_TRGT_PCIE1 0x02
#define LAW_TRGT_DDR 0x0f

static void law_set(int n, uint64_t base, int log2size, int target)
{
    volatile uint32_t *law = (volatile uint32_t *)(CCSR_VA + 0xc08 + n * 0x20);
    law[0] = base >> 12;                                /* LAWBARn */
    law[2] = 0x80000000 | target << 20 | (log2size - 1);  /* LAWARn */
    asm volatile("sync; isync");
}

/* An outbound window of the first PCIe controller, as U-Boot sets them:
 * the kernel takes them as it finds them */
#define PCIE1_REGS (CCSR_VA + 0xa000)
#define POWAR_EN 0x80000000
#define POWAR_MEM 0x00044000        /* memory read and write */
#define POWAR_IO 0x00088000         /* I/O read and write */

static void pcie_outbound(int n, uint64_t cpu, uint64_t bus, int log2size, uint32_t type)
{
    volatile uint32_t *ow = (volatile uint32_t *)(PCIE1_REGS + 0xc00 + n * 0x20);
    ow[0] = bus >> 12;                                  /* POTARn */
    ow[1] = bus >> 44;                                  /* POTEARn */
    ow[2] = cpu >> 12;                                  /* POWBARn */
    ow[4] = POWAR_EN | type | (log2size - 1);           /* POWARn */
    asm volatile("sync; isync");
}

/* PCI configuration through the controller: the address register is big
 * endian, the data register little endian */
static uint32_t pcie_cfg_read(int devfn, int reg)
{
    volatile uint32_t *cfg = (volatile uint32_t *)PCIE1_REGS;
    cfg[0] = 0x80000000 | devfn << 8 | (reg & 0xfc);
    asm volatile("sync");
    return __builtin_bswap32(cfg[1]);
}

static void pcie_cfg_write(int devfn, int reg, uint32_t val)
{
    volatile uint32_t *cfg = (volatile uint32_t *)PCIE1_REGS;
    cfg[0] = 0x80000000 | devfn << 8 | (reg & 0xfc);
    asm volatile("sync");
    cfg[1] = __builtin_bswap32(val);
    asm volatile("sync");
}

/* Give the devices on the bus their memory and I/O ranges in the windows,
 * and enable them, as U-Boot does: the kernel keeps what it finds.  Each
 * range is aligned to its size, the bigger ones first. */
static void pcie_assign(uint32_t mem, uint32_t mem_end, uint32_t io, uint32_t io_end)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int devfn = 8; devfn < 256; devfn++) {
            uint32_t id = pcie_cfg_read(devfn, 0);
            if ((id & 0xffff) == 0xffff) {
                if (!(devfn & 7)) devfn += 7;
                continue;
            }
            int multi = pcie_cfg_read(devfn, 0xc) & 0x00800000;
            if (((pcie_cfg_read(devfn, 0xc) >> 16) & 0x7f) == 0) {
                for (int reg = 0x10; reg <= 0x24; reg += 4) {
                    uint32_t old = pcie_cfg_read(devfn, reg);
                    pcie_cfg_write(devfn, reg, 0xffffffff);
                    uint32_t v = pcie_cfg_read(devfn, reg);
                    int is64 = !(v & 1) && (v & 6) == 4;
                    if (v == 0 || v == 0xffffffff) {
                        pcie_cfg_write(devfn, reg, old);
                    } else if (v & 1) {
                        uint32_t size = (~(v & 0xfffc) + 1) & 0xffff;
                        uint32_t base = ROUND_UP(io, size);
                        if (pass == 1 && base + size <= io_end) {
                            pcie_cfg_write(devfn, reg, base);
                            io = base + size;
                        } else {
                            pcie_cfg_write(devfn, reg, old);
                        }
                    } else {
                        uint32_t size = ~(v & 0xfffffff0) + 1;
                        uint32_t base = ROUND_UP(mem, size);
                        /* The big ones (frame buffers) in the first pass */
                        if ((pass == 0) == (size >= 0x100000) && base + size <= mem_end && base >= mem) {
                            pcie_cfg_write(devfn, reg, base);
                            mem = base + size;
                        } else {
                            pcie_cfg_write(devfn, reg, old);
                        }
                    }
                    if (is64) {
                        reg += 4;
                        pcie_cfg_write(devfn, reg, 0);
                    }
                }
                if (pass == 1)
                    pcie_cfg_write(devfn, 4, pcie_cfg_read(devfn, 4) | 7); /* I/O, memory, master */
            }
            if (!(devfn & 7) && !multi) devfn += 7;
        }
    }
}

static int log2_up(uint64_t v)
{
    int l = 0;
    while ((1ULL << l) < v) l++;
    return l;
}

/* An inbound window, as U-Boot sets PIW1: PCI addresses from 0 to local
 * memory, so that bus masters (the LSI SCSI SCRIPTS, ...) reach RAM */
#define PIWAR_EN_PF_LOCAL_SNOOP 0xa0f55000

static void pcie_inbound(uint64_t size)
{
    volatile uint32_t *iw = (volatile uint32_t *)(PCIE1_REGS + 0xde0);
    iw[0] = 0;                                          /* PITAR1 */
    iw[2] = 0;                                          /* PIWBAR1 */
    iw[3] = 0;                                          /* PIWBEAR1 */
    iw[4] = PIWAR_EN_PF_LOCAL_SNOOP | (log2_up(size) - 1);  /* PIWAR1 */
    asm volatile("sync; isync");
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

    law_set(0, 0xc40000000ULL, 29, LAW_TRGT_PCIE1);    /* 512 MiB memory */
    law_set(1, 0xfffc20000ULL, 16, LAW_TRGT_PCIE1);    /* 64 KiB I/O */
    law_set(2, 0, log2_up(memsize), LAW_TRGT_DDR);
    pcie_outbound(1, 0xc40000000ULL, 0xe0000000, 29, POWAR_MEM);
    pcie_outbound(2, 0xfffc20000ULL, 0, 16, POWAR_IO);
    pcie_assign(0xe0000000, 0xffffffff, 0x1000, 0x10000);
    pcie_inbound(memsize);

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
 * the stack, as scratch space.  The kernel expects SPE enabled in the MSR
 * from the start: its SPE unavailable handler calls SPE code itself. */
void e500_start_loader(void *entry, const char *id, void *kicklist, void *info, char *args)
{
    unsigned long sp = ROUND_DOWN(bd.bd.bi_memsize - 0x10000, 16);
    register unsigned long r3 asm("r3") = (unsigned long)id;
    register unsigned long r4 asm("r4") = (unsigned long)kicklist;
    register unsigned long r5 asm("r5") = (unsigned long)info;
    register unsigned long r6 asm("r6") = (unsigned long)args;
    unsigned long msr;
    asm volatile("mfmsr %0" : "=r"(msr));
    msr |= 0x02000000;                      /* MSR[SPE] */
    asm volatile("mtmsr %0; isync" :: "r"(msr));
    asm volatile("mtctr %4; mr 1,%5; li 0,0; stw 0,0(1); bctrl"
                 : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
                 : "r"(entry), "r"(sp - 16)
                 : "r0", "ctr", "lr", "memory");
}
