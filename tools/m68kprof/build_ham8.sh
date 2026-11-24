#!/bin/sh
# build_ham8.sh DIR [CFLAGS]: m68kprof's AGA video program (work_ham8.c with
# ham8.c, or -DPENS with pens68k.s) into /tmp/m68kprof/DIR
set -e
O=/tmp/m68kprof; D=$O/$1; mkdir -p $D
F=${2:-}
cd "$(dirname "$0")"
m68k-amigaos-as -m68020 -o $D/start.o start.s
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -fno-builtin -fno-tree-loop-distribute-patterns -Wall -c -o $D/t68k.o t68k.c
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -Wall $F -c -o $D/work.o work_ham8.c
# (-DASM: ham8.c as Ambiance builds it, its row by ham8_68k.s)
case "$F" in *-DASM*) H="";; *) H="-DAMB_HOST";; esac
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -Wall $H -c -o $D/ham8.o ../../src/ham8.c
sed "s/^\tsection\tcode,code/\tsection\t.text,code/" ../../src/ham8_68k.s > $D/hasm.s
vasmm68k_mot -Fhunk -m68020 -quiet -o $D/hasm.o $D/hasm.s
sed "s/^\tsection\tcode,code/\tsection\t.text,code/" ../../src/pens68k.s > $D/pens.s
vasmm68k_mot -Fhunk -m68020 -quiet -o $D/pens.o $D/pens.s
m68k-amigaos-gcc -m68020-60 -noixemul -nostartfiles -nostdlib -o $D/t68k $D/start.o $D/t68k.o $D/work.o $D/ham8.o $D/hasm.o $D/pens.o -lgcc
python3 syms.py $D/t68k $D/t68k.o $D/work.o $D/ham8.o $D/hasm.o $D/pens.o > $D/t68k.syms
echo "built $D"
