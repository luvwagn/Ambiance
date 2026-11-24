/*
 * Ambiance - MPEG video's pictures as AGA HAM8
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Each pixel's colour from its Y, Cb, Cr (BT.601, studio range) to 6 bits
 * a channel, by tables: a pair's Cb and Cr give three places in one table
 * of the scaled and clamped Y (lum), each channel a byte's lookup. Then,
 * against the pixel before it as HAM8 left it, by how far each channel is
 * off: the one furthest off set exactly, or, when the next furthest is
 * off by more than HAM8_FAR as well, the nearest base colour. No channel
 * is compared with another per pixel: two tables (q, dec) decide. Twice
 * across (a hires screen's two pixels for one), the second pixel steps
 * again from what the first left.
 *
 * On the Amiga the row's loop is ham8_68k.s; the C here is the same, for
 * the host test (tests/ham8_host.c) and other CPUs.
 */
#include "ham8.h"

#include <string.h>

#define HAM8_FAR 12                     /* (6 bits a channel) a base colour beyond this */

#ifndef AMB_HOST
void ham8_row68k(const UBYTE *Y, const UBYTE *Cb, LONG cr_less_cb, UBYTE *out, ULONG pairs,
                 const struct Ham8Step *s, ULONG twice, const UBYTE *nearest, const UBYTE *base);
#endif

void ham8_init(struct Ham8 *h)
{
    static const UBYTE lo[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 28, 36, 48 };
    static const UBYTE mid[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 14, 18, 24, 32, 42, 56 };
    int i, n = 0, r, g, b;
    UBYTE reg[HAM8_BASES], q[64];
    /* the registers to fill: 0-16 and 20-63 (17-19: the pointer's) */
    for (i = 0; i < HAM8_BASES; i++)
        if (i < 17 || i > 19)
            reg[n++] = (UBYTE)i;
    memset(h->base, 0, sizeof(h->base));
    /* a cube 4 x 4 x 3 (48, black first: register 0), then 13 greys
     * between its own */
    i = 0;
    for (r = 0; r < 4; r++)
        for (g = 0; g < 4; g++)
            for (b = 0; b < 3; b++) {
                h->base[reg[i]][0] = (UBYTE)(r * 21);
                h->base[reg[i]][1] = (UBYTE)(g * 21);
                h->base[reg[i]][2] = (UBYTE)(b * 63 / 2);
                i++;
            }
    for (g = 1; i < n; g++, i++)
        h->base[reg[i]][0] = h->base[reg[i]][1] = h->base[reg[i]][2] = (UBYTE)(g * 63 / 14);
    for (i = 0; i < HAM8_BASES; i++) {
        h->hbase[i][0] = (UBYTE)(h->base[i][0] | 0x80);
        h->hbase[i][1] = (UBYTE)(h->base[i][1] | 0xC0);
        h->hbase[i][2] = (UBYTE)(h->base[i][2] | 0x40);
    }
    /* each 4:4:4 colour's nearest (of those filled) */
    for (i = 0; i < 4096; i++) {
        LONG cr = ((i >> 8) & 15) * 4 + 2, cg = ((i >> 4) & 15) * 4 + 2, cb = (i & 15) * 4 + 2;
        LONG best = 0x7FFFFFFF, k, bi = 0;
        for (k = 0; k < n; k++) {
            const UBYTE *c = h->base[reg[k]];
            LONG dr = c[0] - cr, dg = c[1] - cg, db = c[2] - cb;
            LONG d = dr * dr + dg * dg + db * db;
            if (d < best) {
                best = d;
                bi = reg[k];
            }
        }
        h->near[i] = (UBYTE)bi;
    }
    /* RGB = 1.164 (Y - 16) + (R 1.596 Cr; G -0.392 Cb -0.813 Cr; B
     * 2.017 Cb), Cb and Cr less 128: here 1.164 (Y + part - 16), each
     * part divided by 1.164, so one table (lum) does Y and the clamp */
    for (i = 0; i < 256; i++) {
        LONG c = i - 128;
        h->s.rp[i] = h->lum[0] + 512 + (1596 * c * 1000 / 1164 + (c < 0 ? -500 : 500)) / 1000;
        h->s.gu[i] = h->lum[1] + 512 + (-392 * c * 1000 / 1164 + (c < 0 ? 500 : -500)) / 1000;
        h->s.gv[i] = (WORD)((-813 * c * 1000 / 1164 + (c < 0 ? 500 : -500)) / 1000);
        h->s.bp[i] = h->lum[2] + 512 + (2017 * c * 1000 / 1164 + (c < 0 ? -500 : 500)) / 1000;
    }
    for (i = 0; i < 1024; i++) {
        LONG v = (1164 * (i - 512 - 16) + 500) / 1000;
        UBYTE v6 = (UBYTE)(v < 0 ? 0 : v > 255 ? 63 : v >> 2);
        h->lum[0][i] = (UBYTE)(v6 | 0x80);         /* red: control 2 */
        h->lum[1][i] = (UBYTE)(v6 | 0xC0);         /* green: 3 */
        h->lum[2][i] = (UBYTE)(v6 | 0x40);         /* blue: 1 */
    }
    /* each difference's step: exact up to 8, then wider */
    for (i = 0, r = 0; i < 64; i++) {
        while (r < 15 && i >= lo[r + 1])
            r++;
        q[i] = (UBYTE)r;
    }
    memset(h->s.q, 0, sizeof(h->s.q));
    for (i = -63; i <= 63; i++) {
        UWORD s = q[i < 0 ? -i : i];
        h->s.q[0][i + 64] = (UWORD)(s << 8);
        h->s.q[1][i + 64] = (UWORD)(s << 4);
        h->s.q[2][i + 64] = s;
    }
    for (i = 0; i < 4096; i++) {
        int ar = mid[i >> 8], ag = mid[(i >> 4) & 15], ab = mid[i & 15], ch, sec;
        if (ag >= ar && ag >= ab) {
            ch = 1;
            sec = ar > ab ? ar : ab;
        } else if (ar >= ab) {
            ch = 0;
            sec = ag > ab ? ag : ab;
        } else {
            ch = 2;
            sec = ar > ag ? ar : ag;
        }
        h->s.dec[i] = (UBYTE)(sec > HAM8_FAR ? 3 : ch == 1 ? 0 : ch == 0 ? 1 : 2);
    }
}

void ham8_rgb(const struct Ham8 *h, int r, ULONG *red, ULONG *green, ULONG *blue)
{
    const UBYTE *c = h->base[r & 63];
    *red = (ULONG)((c[0] << 2) | (c[0] >> 4)) * 0x01010101UL;
    *green = (ULONG)((c[1] << 2) | (c[1] >> 4)) * 0x01010101UL;
    *blue = (ULONG)((c[2] << 2) | (c[2] >> 4)) * 0x01010101UL;
}

void ham8_row(struct Ham8 *h, const UBYTE *Y, const UBYTE *Cb, const UBYTE *Cr, ULONG w, UBYTE *out,
              BOOL twice)
{
    ULONG pairs = w >> 1;
#ifndef AMB_HOST
    ham8_row68k(Y, Cb, (LONG)(Cr - Cb), out, pairs, &h->s, twice ? 1 : 0, h->near, &h->hbase[0][0]);
#else
    static const UBYTE chan[3] = { 1, 0, 2 };   /* green, red, blue */
    const struct Ham8Step *s = &h->s;
    UBYTE c[3], t[3];
    ULONG x;
    int k;
    memcpy(c, h->hbase[0], 3);
    for (x = 0; x < pairs * 2; x++) {
        ULONG u = Cb[x >> 1], v = Cr[x >> 1];
        t[0] = s->rp[v][Y[x]];
        t[1] = (s->gu[u] + s->gv[v])[Y[x]];
        t[2] = s->bp[u][Y[x]];
        for (k = 0; k <= (twice ? 1 : 0); k++) {
            ULONG d = s->dec[s->q[0][(LONG)t[0] - c[0] + 64] | s->q[1][(LONG)t[1] - c[1] + 64] |
                             s->q[2][(LONG)t[2] - c[2] + 64]];
            if (d == 3) {
                UBYTE bi = h->near[((t[0] & 0x3C) << 6) | ((t[1] & 0x3C) << 2) | ((t[2] & 0x3C) >> 2)];
                memcpy(c, h->hbase[bi], 3);
                *out++ = bi;                    /* a base colour */
            } else {
                c[chan[d]] = t[chan[d]];
                *out++ = t[chan[d]];
            }
        }
    }
#endif
}
