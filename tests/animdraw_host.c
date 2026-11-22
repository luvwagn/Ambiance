/*
 * Ambiance tests: an ANIM's frames drawn as 16 and 32-bit pixels (src/animdraw.c),
 * on the host, against every pixel worked out by hand: random planes of
 * each depth, widths not a whole number of words, plain, extra halfbrite,
 * HAM6 and HAM8 colours, whole and part scales, four pixel formats; and a
 * row drawn from its first changed column (ad_row_span) as it would be
 * whole.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/animdraw.h"

static unsigned long seed = 12345;
static unsigned rnd(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (unsigned)(seed >> 16) & 0x7FFF;
}

static int fails;

/* The row's colours by hand: a pixel's planes, then its colour (HAM step
 * by step from colour 0), then the screen's format. */
static void by_hand(const unsigned char *row, int rb, int depth, int w, int ham, const unsigned long *lut,
                    unsigned short *out)
{
    unsigned long rgb = lut[0];
    int x, p;
    for (x = 0; x < w; x++) {
        unsigned c = 0;
        for (p = 0; p < depth; p++)
            if (row[p * rb + (x >> 3)] & (0x80 >> (x & 7)))
                c |= 1U << p;
        if (ham) {
            int bits = depth == 8 ? 6 : 4, ctl = c >> bits;
            unsigned v = c & ((1U << bits) - 1);
            v = bits == 4 ? v * 17 : (v << 2) | (v >> 4);
            if (ctl == 0)
                rgb = lut[c & ((1U << bits) - 1)];
            else if (ctl == 1)
                rgb = (rgb & 0xFFFF00) | v;
            else if (ctl == 2)
                rgb = (rgb & 0x00FFFF) | ((unsigned long)v << 16);
            else
                rgb = (rgb & 0xFF00FF) | ((unsigned long)v << 8);
            out[x] = ad_rgb16(rgb);
        } else
            out[x] = ad_rgb16(lut[c]);
    }
}

static void one(int depth, int w, int ham, int ehb, int scale, unsigned long fmt)
{
    int rb = ((w + 15) >> 4) << 1, dw = w * scale / 100, fx = dw % w ? 0 : dw / w, i, y, x;
    unsigned char cmap[768], row[8 * 64], old[8 * 64];
    unsigned long lut[256];
    unsigned short l16[256], xo[513], want[512], got[2048], whole[2048];
    unsigned long camg = (ham ? 0x800 : 0) | (ehb ? 0x80 : 0);
    char what[120];
    for (i = 0; i < 768; i++)
        cmap[i] = (unsigned char)rnd();
    ad_lut(cmap, ham ? (depth == 8 ? 192 : 48) : 3U << depth, camg, (unsigned char)depth, lut);
    ad_pix_tables(fmt);
    ad_lut16(lut, l16);
    if (ham)
        ad_ham(l16, (unsigned char)depth);
    for (i = 0; i <= w; i++)
        xo[i] = (unsigned short)(i * dw / w);
    sprintf(what, "depth %d, %d wide, %s, scale %d%%, format %lu", depth, w,
            ham ? "HAM" : ehb ? "EHB" : "plain", scale, fmt);
    for (y = 0; y < 20; y++) {
        unsigned long x0, x1;
        for (i = 0; i < rb * depth; i++)
            row[i] = (unsigned char)rnd();
        by_hand(row, rb, depth, w, ham, lut, want);
        memset(got, 0xEE, sizeof(got));
        ad_row_put(row, rb, (unsigned char)depth, w, 0, rb, (unsigned short)fx, xo, ham, lut, l16,
                   (unsigned char *)got);
        for (x = 0; x < w; x++) {
            int k;
            for (k = xo[x]; k < xo[x + 1]; k++)
                if (got[k] != want[x]) {
                    if (fails++ < 10)
                        printf("FAIL %s: row %d pixel %d (at %d) %04x, by hand %04x\n", what, y, x, k,
                               got[k], want[x]);
                    x = w;
                    break;
                }
        }
        if (got[dw] != 0xEEEE && fails++ < 10)
            printf("FAIL %s: written past the row's end\n", what);
        /* only part of it changed: drawn from its first changed column,
         * over the old row's pixels, as the whole new one */
        memcpy(old, row, rb * depth);
        for (i = 0; i < 3; i++)
            old[(rnd() % depth) * rb + rnd() % rb] ^= (unsigned char)(1 + rnd() % 255);
        memcpy(whole, got, sizeof(whole));
        memset(got, 0xEE, sizeof(got));
        ad_row_put(old, rb, (unsigned char)depth, w, 0, rb, (unsigned short)fx, xo, ham, lut, l16,
                   (unsigned char *)got);
        if (ad_row_span(row, old, rb, (unsigned char)depth, &x0, &x1)) {
            if (ham)
                x1 = rb;
            ad_row_put(row, rb, (unsigned char)depth, w, x0, x1, (unsigned short)fx, xo, ham, lut,
                       l16, (unsigned char *)got);
            if (memcmp(got, whole, dw * 2) && fails++ < 10)
                printf("FAIL %s: row %d drawn from column %lu to %lu is not the whole row's\n", what,
                       y, x0, x1);
        }
    }
}

/* The same at 32 bits a pixel (ad_row_put32), against the same hand */
static void by_hand32(const unsigned char *row, int rb, int depth, int w, int ham,
                      const unsigned long *lut, unsigned int *out)
{
    unsigned long rgb = lut[0];
    int x, p;
    for (x = 0; x < w; x++) {
        unsigned c = 0;
        for (p = 0; p < depth; p++)
            if (row[p * rb + (x >> 3)] & (0x80 >> (x & 7)))
                c |= 1U << p;
        if (ham) {
            int bits = depth == 8 ? 6 : 4, ctl = c >> bits;
            unsigned v = c & ((1U << bits) - 1);
            v = bits == 4 ? v * 17 : (v << 2) | (v >> 4);
            if (ctl == 0)
                rgb = lut[c & ((1U << bits) - 1)];
            else if (ctl == 1)
                rgb = (rgb & 0xFFFF00) | v;
            else if (ctl == 2)
                rgb = (rgb & 0x00FFFF) | ((unsigned long)v << 16);
            else
                rgb = (rgb & 0xFF00FF) | ((unsigned long)v << 8);
        } else
            rgb = lut[c];
        out[x] = ad_rgb32(rgb);
    }
}

static void one32(int depth, int w, int ham, int ehb, int scale, unsigned long fmt)
{
    int rb = ((w + 15) >> 4) << 1, dw = w * scale / 100, fx = dw % w ? 0 : dw / w, i, y, x;
    unsigned char cmap[768], row[8 * 64], old[8 * 64];
    unsigned long lut[256];
    unsigned int l4[256], want[512], got[2048], whole[2048];
    unsigned short xo[513];
    unsigned long camg = (ham ? 0x800 : 0) | (ehb ? 0x80 : 0);
    char what[120];
    for (i = 0; i < 768; i++)
        cmap[i] = (unsigned char)rnd();
    ad_lut(cmap, ham ? (depth == 8 ? 192 : 48) : 3U << depth, camg, (unsigned char)depth, lut);
    if (ad_pix_tables(fmt) != 4 && fails++ < 10)
        printf("FAIL format %lu: not 4 bytes a pixel\n", fmt);
    ad_lut32(lut, l4);
    if (ham)
        ad_ham32(l4, (unsigned char)depth);
    for (i = 0; i <= w; i++)
        xo[i] = (unsigned short)(i * dw / w);
    sprintf(what, "32-bit: depth %d, %d wide, %s, scale %d%%, format %lu", depth, w,
            ham ? "HAM" : ehb ? "EHB" : "plain", scale, fmt);
    for (y = 0; y < 20; y++) {
        unsigned long x0, x1;
        for (i = 0; i < rb * depth; i++)
            row[i] = (unsigned char)rnd();
        by_hand32(row, rb, depth, w, ham, lut, want);
        memset(got, 0xEE, sizeof(got));
        ad_row_put32(row, rb, (unsigned char)depth, w, 0, rb, (unsigned short)fx, xo, ham, l4,
                     (unsigned char *)got);
        for (x = 0; x < w; x++) {
            int k;
            for (k = xo[x]; k < xo[x + 1]; k++)
                if (got[k] != want[x]) {
                    if (fails++ < 10)
                        printf("FAIL %s: row %d pixel %d (at %d) %08x, by hand %08x\n", what, y, x,
                               k, got[k], want[x]);
                    x = w;
                    break;
                }
        }
        if (got[dw] != 0xEEEEEEEEU && fails++ < 10)
            printf("FAIL %s: written past the row's end\n", what);
        memcpy(old, row, rb * depth);
        for (i = 0; i < 3; i++)
            old[(rnd() % depth) * rb + rnd() % rb] ^= (unsigned char)(1 + rnd() % 255);
        memcpy(whole, got, sizeof(whole));
        memset(got, 0xEE, sizeof(got));
        ad_row_put32(old, rb, (unsigned char)depth, w, 0, rb, (unsigned short)fx, xo, ham, l4,
                     (unsigned char *)got);
        if (ad_row_span(row, old, rb, (unsigned char)depth, &x0, &x1)) {
            if (ham)
                x1 = rb;
            ad_row_put32(row, rb, (unsigned char)depth, w, x0, x1, (unsigned short)fx, xo, ham, l4,
                         (unsigned char *)got);
            if (memcmp(got, whole, dw * 4) && fails++ < 10)
                printf("FAIL %s: row %d drawn from column %lu to %lu is not the whole row's\n",
                       what, y, x0, x1);
        }
    }
}

int main(void)
{
    static const int widths[] = { 16, 24, 320, 352, 376, 384, 370, 321, 100, 9 };
    static const int scales[] = { 100, 200, 300, 113, 227, 250 };
    static const unsigned long fmts[] = { 7, 5, 1, 4 };
    int d, wi, si, fi, n = 0;
    ad_init();
    for (fi = 0; fi < 4; fi++)
        for (wi = 0; wi < 10; wi++)
            for (si = 0; si < 6; si++) {
                for (d = 1; d <= 8; d++, n++)
                    one(d, widths[wi], 0, 0, scales[si], fmts[fi]);
                one(6, widths[wi], 0, 1, scales[si], fmts[fi]);
                one(6, widths[wi], 1, 0, scales[si], fmts[fi]);
                one(8, widths[wi], 1, 0, scales[si], fmts[fi]);
                n += 3;
            }
    {
        static const unsigned long f32[] = { 11, 12, 13 };      /* ARGB32, BGRA32, RGBA32 */
        for (fi = 0; fi < 3; fi++)
            for (wi = 0; wi < 10; wi++)
                for (si = 0; si < 6; si++) {
                    for (d = 1; d <= 8; d++, n++)
                        one32(d, widths[wi], 0, 0, scales[si], f32[fi]);
                    one32(6, widths[wi], 0, 1, scales[si], f32[fi]);
                    one32(6, widths[wi], 1, 0, scales[si], f32[fi]);
                    one32(8, widths[wi], 1, 0, scales[si], f32[fi]);
                    n += 3;
                }
    }
    /* the alpha kept opaque through HAM's steps */
    ad_pix_tables(11);
    if ((ad_rgb32(0x123456) >> 24) != 0xFF && fails++ < 10)
        printf("FAIL ARGB32: alpha not opaque\n");
    printf("%d kinds of row, %d failed\n", n, fails);
    return fails != 0;
}
