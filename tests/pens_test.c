/* va_pens68k (src/pens68k.s) against the C it stands for, on random rows
 * and tables: run on a 68020 by vamos (make test-pens). */
#include <stdio.h>
#include <string.h>

typedef unsigned char UBYTE;
typedef unsigned short UWORD;
typedef unsigned long ULONG;

void va_pens68k(const UBYTE *y, const UWORD *cl, UBYTE *dst, ULONG quads, const UWORD *yt4, const UBYTE *lut);

static ULONG seed = 777;
static ULONG rnd(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return seed >> 8;
}

int main(void)
{
    static ULONG yl[129];
    static UWORD cl[260], yt4[256 * 4];
    static UBYTE lut[8192], got[520], want[520];
    UBYTE *y = (UBYTE *)yl;
    ULONG t, i, bad = 0, n = 0;
    for (i = 0; i < 8192; i++)
        lut[i] = (UBYTE)rnd();
    for (t = 0; t < 300; t++) {
        ULONG quads = 1 + rnd() % 128;
        for (i = 0; i < 256 * 4; i++)
            yt4[i] = (UWORD)((rnd() & 31) << 8);
        for (i = 0; i < quads * 4; i++)
            y[i] = (UBYTE)rnd();
        for (i = 0; i < quads * 2; i++)
            cl[i] = (UWORD)(rnd() & 0xFF);
        memset(got, 0x5A, sizeof(got));
        for (i = 0; i < quads * 4; i++)
            want[i] = lut[yt4[y[i] * 4 + (i & 3)] | cl[i >> 1]];
        va_pens68k(y, cl, got, quads, yt4, lut);
        for (i = 0; i < quads * 4; i++, n++)
            if (got[i] != want[i])
                bad++;
        if (got[quads * 4] != 0x5A)
            bad++;                          /* (written past its row) */
    }
    printf("pens68k: %lu pens checked, %lu failed\n", n, bad);
    return bad ? 20 : 0;
}
