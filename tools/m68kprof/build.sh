#!/bin/sh
# build.sh DIR [JDEC_CFLAGS] [SHRINK_CFLAGS]: m68kprof's 68k program into
# /tmp/m68kprof/DIR (t68k, t68k.syms), jdec and shrink built as Ambiance
# builds them unless flags say otherwise (-DSHRINK_C: shrink's C)
set -e
O=/tmp/m68kprof; D=$O/$1; mkdir -p $D
J=${2:--mno-bitfield}; S=${3:-}
cd "$(dirname "$0")"
m68k-amigaos-as -m68020 -o $D/start.o start.s
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -fno-builtin -fno-tree-loop-distribute-patterns -Wall -c -o $D/t68k.o t68k.c
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -Wall -DSHRINK_HOST -I../../src -I../../src/jdec -c -o $D/work.o work.c
m68k-amigaos-gcc -O2 -m68020-60 $J -noixemul -Wall -Wextra -c -o $D/jdec.o ../../src/jdec/jdec.c
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -Wall -DSHRINK_HOST $S -c -o $D/shrink.o ../../src/shrink.c
vasmm68k_mot -Fhunk -m68020 -quiet -o $D/shrink68k.o ../../src/shrink68k.s
rm -f $D/jasm_*.o
for s in ../../src/jdec/*.s; do [ -f "$s" ] && vasmm68k_mot -Fhunk -m68020 -quiet -o $D/jasm_$(basename $s .s).o $s; done
EXTRA=$(ls $D/jasm_*.o 2>/dev/null || true)
m68k-amigaos-gcc -m68020-60 -noixemul -nostartfiles -nostdlib -o $D/t68k $D/start.o $D/t68k.o $D/work.o $D/shrink.o $D/shrink68k.o $EXTRA $D/jdec.o -lgcc
python3 syms.py $D/t68k $D/t68k.o $D/work.o $D/shrink.o $D/shrink68k.o $EXTRA $D/jdec.o > $D/t68k.syms
echo "built $D"
