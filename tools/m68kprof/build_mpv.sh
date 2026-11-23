#!/bin/sh
# build_mpv.sh DIR [CFLAGS]: m68kprof's mpegv program (work_mpv.c and
# mpegv built static, as its library is: -O2 -m68020-60, its assembler)
# into /tmp/m68kprof/DIR. CFLAGS: more for mpegv's C (-mno-bitfield)
set -e
O=/tmp/m68kprof; D=$O/$1; mkdir -p $D/o
F=${2:-}
M=$(cd "$(dirname "$0")"/../../extras/mpegv && pwd)
L2=$M/third_party/libmpeg2
INC="-I$M/include -I$M/src -I$L2/include -I$L2/libmpeg2"
CC="m68k-amigaos-gcc -O2 -m68020-60 -noixemul -fomit-frame-pointer -w -DMPV_STATIC $INC"
cd "$(dirname "$0")"
m68k-amigaos-as -m68020 -o $D/start.o start.s
m68k-amigaos-gcc -O2 -m68020-60 -noixemul -fno-builtin -fno-tree-loop-distribute-patterns -Wall -c -o $D/t68k.o t68k.c
$CC -c -o $D/work.o work_mpv.c
OB=""
for c in $M/src/mpegv.c $M/src/convert.c $L2/libmpeg2/alloc.c $L2/libmpeg2/cpu_accel.c $L2/libmpeg2/cpu_state.c \
         $L2/libmpeg2/decode.c $L2/libmpeg2/header.c $L2/libmpeg2/idct.c $L2/libmpeg2/motion_comp.c $L2/libmpeg2/slice.c; do
    $CC $F -c -o $D/o/$(basename $c .c).o $c; OB="$OB $D/o/$(basename $c .c).o"
done
for s in $M/src/convert_68k.s $L2/libmpeg2/idct_68k.s $L2/libmpeg2/idct_aan_68k.s $L2/libmpeg2/vlc_68k.s; do
    m68k-amigaos-as -m68020 -o $D/o/$(basename $s .s).o $s; OB="$OB $D/o/$(basename $s .s).o"
done
m68k-amigaos-gcc -m68020-60 -noixemul -nostartfiles -nostdlib -o $D/t68k $D/start.o $D/t68k.o $D/work.o $OB -lgcc
python3 syms.py $D/t68k $D/t68k.o $D/work.o $OB > $D/t68k.syms
echo "built $D"
