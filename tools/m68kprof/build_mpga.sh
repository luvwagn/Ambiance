#!/bin/sh
# build_mpga.sh DIR [020|060] [CFLAGS]: m68kprof's ambmpega program
# (work_mpga.c, mpga.c for the host's path, libmad and the 68k assembler as
# ambmpega.library or ambmpega060.library builds them) into /tmp/m68kprof/DIR
set -e
O=/tmp/m68kprof; D=$O/$1; mkdir -p $D/o
V=${2:-060}; F=${3:-}
M=$(cd "$(dirname "$0")"/../../extras/mpga && pwd)
MAD=$M/third_party/libmad
if [ "$V" = 060 ]; then CPU=-m68060; FPM="-DFPM_68060 -DOPT_SPEED"; S060=1; else CPU=-m68020; FPM="-DFPM_64BIT -DOPT_SPEED"; S060=0; fi
FLAGS="-DHAVE_CONFIG_H $FPM -DASO_ZEROCHECK -I$M/src/port -I$MAD -I$M/include"
CC="m68k-amigaos-gcc -O2 $CPU -noixemul -nostartfiles -fomit-frame-pointer -fno-strict-aliasing -w $FLAGS $F"
cd "$(dirname "$0")"
m68k-amigaos-as -m68020 -o $D/start.o start.s
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -fno-builtin -fno-tree-loop-distribute-patterns -Wall -c -o $D/t68k.o t68k.c
$CC -c -o $D/work.o work_mpga.c
$CC -U__amigaos__ -DMPGA_STATIC -DMPGA_OUT_ASM -c -o $D/o/mpga.o $M/src/mpga.c
OB="$D/o/mpga.o"
for c in bit fixed frame huffman layer12 layer3 stream synth timer; do
    X=""; [ $c = synth ] && X="-DMPGA_WINDOW_ASM -DMPGA_DCT_ASM"; [ $c = layer3 ] && X="-DMPGA_IMDCT_ASM"
    [ $c = timer ] && X="-Dsprintf=mpga_nosprintf"
    $CC $X -c -o $D/o/$c.o $MAD/$c.c; OB="$OB $D/o/$c.o"
done
for s in synth_68k out_68k; do m68k-amigaos-as $CPU -o $D/o/$s.o $M/src/$s.s; OB="$OB $D/o/$s.o"; done
for s in dct32_68k layer3_68k; do m68k-amigaos-as $CPU --defsym MPGA060=$S060 -o $D/o/$s.o $M/src/$s.s; OB="$OB $D/o/$s.o"; done
m68k-amigaos-gcc $CPU -noixemul -nostartfiles -nostdlib -o $D/t68k $D/start.o $D/t68k.o $D/work.o $OB -lgcc
python3 syms.py $D/t68k $D/t68k.o $D/work.o $OB > $D/t68k.syms
echo "built $D"
