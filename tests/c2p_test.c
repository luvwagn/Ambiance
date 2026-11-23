/* va_c2p68k (src/c2p68k.s, written by tools/c2pgen/c2pgen.py) against a
 * plain bit-by-bit chunky-to-planar, on random rows; and va_c2p2x68k (each
 * pen twice across: half size video) against the same of its pens
 * doubled: run on a 68020 by vamos (make test-c2p). */
#include <stdio.h>
#include <string.h>

typedef unsigned char UBYTE;
typedef unsigned long ULONG;

void va_c2p68k(const UBYTE *src, ULONG groups, UBYTE **planes);
void va_c2p2x68k(const UBYTE *src, ULONG groups, UBYTE **planes);
void va_c2p68k_sb(const UBYTE *src, ULONG groups, UBYTE **planes);
void va_c2p2x68k_twice(const UBYTE *src, ULONG groups, UBYTE **planes, ULONG bpr);

static ULONG seed = 12345;
static ULONG rnd(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return seed >> 8;
}

int main(void)
{
    static ULONG srcl[64 * 8 + 1];
    static ULONG out[8][66], want[8][66];
    UBYTE *src = (UBYTE *)srcl;
    UBYTE *planes[8];
    ULONG t, g, k, x, bad = 0, n = 0;
    for (t = 0; t < 800; t++) {
        ULONG groups = 1 + rnd() % 64;
        for (x = 0; x < groups * 32; x++)
            src[x] = (UBYTE)rnd();
        memset(out, 0xA5, sizeof(out));
        memset(want, 0, sizeof(want));
        for (x = 0; x < groups * 32; x++)
            for (k = 0; k < 8; k++)
                if (src[x] & (1 << k))
                    want[k][x >> 5] |= 0x80000000UL >> (x & 31);
        for (k = 0; k < 8; k++)
            planes[k] = (UBYTE *)&out[k][1];
        if (t & 1)
            va_c2p68k_sb(src, groups, planes);  /* (the store buffer's, every other) */
        else
            va_c2p68k(src, groups, planes);
        for (k = 0; k < 8; k++) {
            if (out[k][0] != 0xA5A5A5A5UL || out[k][groups + 1] != 0xA5A5A5A5UL)
                bad++;                      /* (written outside its row) */
            for (g = 0; g < groups; g++, n++)
                if (out[k][g + 1] != want[k][g])
                    bad++;
        }
    }
    printf("c2p68k: %lu longs checked, %lu failed\n", n, bad);
    {
        ULONG bad2 = 0, n2 = 0;
        for (t = 0; t < 400; t++) {
            ULONG groups = 1 + rnd() % 32;
            for (x = 0; x < groups * 16; x++)
                src[x] = (UBYTE)rnd();
            memset(out, 0xA5, sizeof(out));
            memset(want, 0, sizeof(want));
            for (x = 0; x < groups * 32; x++)  /* (pixel x: pen x / 2) */
                for (k = 0; k < 8; k++)
                    if (src[x >> 1] & (1 << k))
                        want[k][x >> 5] |= 0x80000000UL >> (x & 31);
            for (k = 0; k < 8; k++)
                planes[k] = (UBYTE *)&out[k][1];
            va_c2p2x68k(src, groups, planes);
            for (k = 0; k < 8; k++) {
                if (out[k][0] != 0xA5A5A5A5UL || out[k][groups + 1] != 0xA5A5A5A5UL)
                    bad2++;
                for (g = 0; g < groups; g++, n2++)
                    if (out[k][g + 1] != want[k][g])
                        bad2++;
            }
        }
        printf("c2p2x68k (each pen twice): %lu longs checked, %lu failed\n", n2, bad2);
        bad += bad2;
    }
    {
        /* two rows at once: out[k] and out[k + 4] apart by sizeof(out[0]) */
        static ULONG o2[8][2][66];
        ULONG bad3 = 0, n3 = 0, r;
        for (t = 0; t < 400; t++) {
            ULONG groups = 1 + rnd() % 32;
            for (x = 0; x < groups * 16; x++)
                src[x] = (UBYTE)rnd();
            memset(o2, 0xA5, sizeof(o2));
            memset(want, 0, sizeof(want));
            for (x = 0; x < groups * 32; x++)
                for (k = 0; k < 8; k++)
                    if (src[x >> 1] & (1 << k))
                        want[k][x >> 5] |= 0x80000000UL >> (x & 31);
            for (k = 0; k < 8; k++)
                planes[k] = (UBYTE *)&o2[k][0][1];
            va_c2p2x68k_twice(src, groups, planes, sizeof(o2[0][0]));
            for (k = 0; k < 8; k++)
                for (r = 0; r < 2; r++) {
                    if (o2[k][r][0] != 0xA5A5A5A5UL || o2[k][r][groups + 1] != 0xA5A5A5A5UL)
                        bad3++;
                    for (g = 0; g < groups; g++, n3++)
                        if (o2[k][r][g + 1] != want[k][g])
                            bad3++;
                }
        }
        printf("c2p2x68k_twice (two rows): %lu longs checked, %lu failed\n", n3, bad3);
        bad += bad3;
    }
    return bad ? 20 : 0;
}
