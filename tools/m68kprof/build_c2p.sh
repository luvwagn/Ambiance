#!/bin/sh
# build_c2p.sh DIR [C2P.s] [CFLAGS]: m68kprof's c2p program (work_c2p.c and
# Ambiance's src/c2p68k.s, or the given one) into /tmp/m68kprof/DIR
set -e
O=/tmp/m68kprof; D=$O/$1; mkdir -p $D
S=${2:-../../src/c2p68k.s}; F=${3:-}
cd "$(dirname "$0")"
m68k-amigaos-as -m68020 -o $D/start.o start.s
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -fno-builtin -fno-tree-loop-distribute-patterns -Wall -c -o $D/t68k.o t68k.c
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -Wall $F -c -o $D/work.o work_c2p.c
sed "s/^\tsection\tcode,code/\tsection\t.text,code/" $S > $D/c2p.s
vasmm68k_mot -Fhunk -m68020 -quiet -o $D/c2p.o $D/c2p.s
m68k-amigaos-gcc -m68020-60 -noixemul -nostartfiles -nostdlib -o $D/t68k $D/start.o $D/t68k.o $D/work.o $D/c2p.o -lgcc
python3 syms.py $D/t68k $D/t68k.o $D/work.o $D/c2p.o > $D/t68k.syms
echo "built $D"
