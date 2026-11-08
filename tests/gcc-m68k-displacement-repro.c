/*
 * m68k-amigaos-gcc: a constant table offset is added to the index register
 * at -O1/-O2/-O3, not folded into the (bd,Xn.l*8) displacement as -Os does.
 *
 * Compiler: m68k-amigaos-gcc (AmigaDev 16.2-rc23) 16.2.1b 20261010182711
 * Build:    m68k-amigaos-gcc -O2 -m68020-60 -S -o - repro.c
 *           (the same at -O1 and -O3, and with -m68020, -m68030, -m68040)
 *
 * t[6][c] is at _t + 6*256*8 + c*8. The 68020's (bd,Xn.l*SCALE) mode takes
 * the constant in its displacement, so each load is one instruction, as
 * -Os gives:
 *
 *     clr.l d1
 *     move.b (a1),d1
 *     clr.l d0
 *     move.b 1(a1),d0
 *     move.l _t+12292(d1.l*8),d2
 *     or.l _t+10244(d0.l*8),d2
 *     move.l _t+12288(d1.l*8),d1
 *     or.l _t+10240(d0.l*8),d1
 *
 * -O2 instead adds the first index into the register (6 bytes and an extra
 * instruction per table, 76 bytes against 64 for this function):
 *
 *     clr.l d1
 *     move.b (a1),d1
 *     add.l #1536,d1            <-- not needed
 *     clr.l d0
 *     move.b 1(a1),d0
 *     add.l #1280,d0            <-- not needed
 *     move.l _t+4(d1.l*8),d2
 *     or.l _t+4(d0.l*8),d2
 *     move.l _t(d1.l*8),d1
 *     or.l _t(d0.l*8),d1
 *
 * It happens when the entry's address is taken once (e) and used for two
 * loads; written as t[6][p[0]][0] | t[5][p[1]][0] directly, -O2 folds it.
 * Seen in a bitplane-to-chunky inner loop (8 such lookups per 8 pixels).
 */
extern unsigned int t[8][256][2];

void f(const unsigned char *p, unsigned int *out)
{
    unsigned int h = 0, l = 0;
    const unsigned int *e;
    e = t[6][p[0]]; h |= e[0]; l |= e[1];
    e = t[5][p[1]]; h |= e[0]; l |= e[1];
    out[0] = h;
    out[1] = l;
}
