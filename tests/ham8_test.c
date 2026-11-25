/*
 * ham8_68k.s against ham8.c's C, on a 68020 (vamos): the same bytes for
 * rows of made pictures (random, smooth, edges), once and twice across.
 */
#define AMB_HOST 1
#include "../src/ham8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ham8_row68k(const UBYTE *Y, const UBYTE *Cb, LONG cr_less_cb, UBYTE *out, ULONG pairs,
                 const struct Ham8Step *s, ULONG twice, const UBYTE *nearest, const UBYTE *base);

static struct Ham8 h;
static UBYTE Y[640], U[320], V[320], a[1280], b[1280];

int main(void)
{
    int row, x, fail = 0, n = 0, tw;
    unsigned long seed = 12345;
    ham8_init(&h);
    for (row = 0; row < 60; row++) {
        for (x = 0; x < 640; x++) {
            seed = seed * 1103515245UL + 12345;
            Y[x] = (UBYTE)(row % 3 == 0 ? (seed >> 16) : row % 3 == 1 ? 16 + x * 219 / 639
                                                                         : ((x / 13) & 1) * 200 + 20);
        }
        for (x = 0; x < 320; x++) {
            seed = seed * 1103515245UL + 12345;
            U[x] = (UBYTE)(row % 2 ? (seed >> 16) : 128 + (x % 64) - 32);
            V[x] = (UBYTE)(row % 2 ? (seed >> 8) : 200 - x % 150);
        }
        for (tw = 0; tw < 2; tw++) {
            int w = row % 4 == 3 ? 318 : 640;
            ham8_row(&h, Y, U, V, (ULONG)w, a, tw);         /* (the C) */
            memset(b, 0x55, sizeof(b));
            ham8_row68k(Y, U, (LONG)(V - U), b, (ULONG)w / 2, &h.s, (ULONG)tw, h.near, &h.hbase[0][0]);
            n++;
            if (memcmp(a, b, (size_t)(tw ? 2 * w : w))) {
                for (x = 0; x < (tw ? 2 * w : w) && a[x] == b[x]; x++)
                    ;
                printf("row %d twice %d: byte %d is %02x, the C's %02x\n", row, tw, x, b[x], a[x]);
                fail++;
            }
        }
    }
    printf("%d rows, %d failed\n", n, fail);
    return fail ? 10 : 0;
}
