#!/bin/bash
# Build bboot with the ppc-amigaos cross compiler, soft-float so that it
# also runs on e500 cores, which have no classic FPU.
set -e
export PATH=/opt/amiga-ppc-16.2/bin:$PATH
F="-mbig-endian -mcpu=powerpc -msoft-float"
make -s CROSS=ppc-amigaos- ARCH_CFLAGS="$F" \
  LDLIBS="$(ppc-amigaos-gcc $F -mcrt=clib2 -print-libgcc-file-name)" Depend bboot "$@"
