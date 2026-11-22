/*
 * Ambiance - an ANIM's frames drawn as 15, 16 or 32-bit pixels (animdraw.h)
 * Copyright (c) 2026 Brian King
 *
 * Bitplanes as pixels in a graphics card's format, a row at a time: the
 * colours from a colour table, extra halfbrite, HAM worked out pixel by
 * pixel; scaled as written. Plain C with no Amiga headers, as animdec.c,
 * so the tests run it on the host and against HAM worked out by hand.
 */
#include <string.h>
#include "animdraw.h"

/* CyberGraphX's pixel formats (cybergraphics.h) */
enum { PIXFMT_LUT8, PIXFMT_RGB15, PIXFMT_BGR15, PIXFMT_RGB15PC, PIXFMT_BGR15PC, PIXFMT_RGB16,
       PIXFMT_BGR16, PIXFMT_RGB16PC, PIXFMT_BGR16PC, PIXFMT_RGB24, PIXFMT_BGR24, PIXFMT_ARGB32,
       PIXFMT_BGRA32, PIXFMT_RGBA32 };

/* A plane's byte as 8 pixels' bits, that plane's bit of each (4 + 4):
 * a table a plane, so a row's 8 pixels are its planes' entries or'd, no
 * shifts (16 KB). */
static unsigned int p2c[8][256][2];
static unsigned int l32[256], hk32[256], hs32[256];  /* doubled: two pixels a long */

void ad_init(void)
{
    int b, i, p;
    if (p2c[0][255][0])
        return;
    for (p = 0; p < 8; p++)
        for (b = 0; b < 256; b++) {
            unsigned int hi = 0, lo = 0;
            for (i = 0; i < 4; i++) {
                if (b & (0x80 >> i))
                    hi |= 1U << (24 - 8 * i + p);
                if (b & (0x08 >> i))
                    lo |= 1U << (24 - 8 * i + p);
            }
            p2c[p][b][0] = hi;
            p2c[p][b][1] = lo;
        }
}

/* Byte column x of a row of planes (pp[p]: plane p's) as 8 pixels'
 * colour numbers: h the first four (top byte first), l the next four. The
 * table indexed whole each time, not through a pointer to its entry: so
 * GCC folds the plane's offset into the addressing mode (through a
 * pointer, -O2 adds it to the index register first: see
 * tests/gcc-m68k-displacement-repro.c). */
#define GATHER(pp, x, depth, h, l)                                                      \
    do {                                                                                \
        unsigned int c_;                                                                \
        h = l = 0;                                                                      \
        switch (depth) {                                                                \
        case 8: c_ = pp[7][x]; h |= p2c[7][c_][0]; l |= p2c[7][c_][1]; /* fall through */ \
        case 7: c_ = pp[6][x]; h |= p2c[6][c_][0]; l |= p2c[6][c_][1]; /* fall through */ \
        case 6: c_ = pp[5][x]; h |= p2c[5][c_][0]; l |= p2c[5][c_][1]; /* fall through */ \
        case 5: c_ = pp[4][x]; h |= p2c[4][c_][0]; l |= p2c[4][c_][1]; /* fall through */ \
        case 4: c_ = pp[3][x]; h |= p2c[3][c_][0]; l |= p2c[3][c_][1]; /* fall through */ \
        case 3: c_ = pp[2][x]; h |= p2c[2][c_][0]; l |= p2c[2][c_][1]; /* fall through */ \
        case 2: c_ = pp[1][x]; h |= p2c[1][c_][0]; l |= p2c[1][c_][1]; /* fall through */ \
        default: c_ = pp[0][x]; h |= p2c[0][c_][0]; l |= p2c[0][c_][1];                  \
        }                                                                               \
    } while (0)

/* A row of interleaved planes (plane p at row + p * rb) as w bytes. */
void ad_p2c_row(const unsigned char *row, unsigned long rb, unsigned char depth, unsigned long w, unsigned char *out)
{
    const unsigned char *pp[8];
    unsigned long x, i;
    for (i = 0; i < depth && i < 8; i++)
        pp[i] = row + i * rb;
    for (x = 0; x < rb; x++) {
        unsigned int h, l;
        unsigned char t[8];
        GATHER(pp, x, depth, h, l);
        t[0] = h >> 24; t[1] = h >> 16; t[2] = h >> 8; t[3] = h;
        t[4] = l >> 24; t[5] = l >> 16; t[6] = l >> 8; t[7] = l;
        for (i = 0; i < 8 && x * 8 + i < w; i++)
            out[x * 8 + i] = t[i];
    }
}

/* A CMAP as 0xRRGGBB colours (old 4-bit ones widened); extra halfbrite's
 * 32-63 the first 32 at half. */
void ad_lut(const unsigned char *c, unsigned long len, unsigned long camg, unsigned char depth, unsigned long *lut)
{
    unsigned long i, n = len / 3, lo = 0;
    if (n > 256)
        n = 256;
    for (i = 0; i < n * 3; i++)
        lo |= c[i] & 0x0F;
    for (i = 0; i < 256; i++) {
        unsigned long r = 0, gg = 0, b = 0;
        if (i < n) {
            r = c[i * 3];
            gg = c[i * 3 + 1];
            b = c[i * 3 + 2];
            if (!lo) {
                r |= r >> 4;
                gg |= gg >> 4;
                b |= b >> 4;
            }
        }
        lut[i] = (r << 16) | (gg << 8) | b;
    }
    if ((camg & 0x80) && depth == 6)
        for (i = 32; i < 64; i++)
            lut[i] = (lut[i - 32] >> 1) & 0x7F7F7F;
}

/* The screen's own pixels: a 15 or 16-bit format (2 back) or a 32-bit
 * one (4 back; else 0, not written straight), and its red, green and
 * blue as parts of a pixel, already in its byte order, so a pixel is
 * rt[r] | gt[g] | bt[b] (32 bits: rt4..., alpha opaque in rt4's). */
static unsigned short rt[256], gt[256], bt[256];
static unsigned int rt4[256], gt4[256], bt4[256];

int ad_pix_tables(unsigned long fmt)
{
    unsigned long v, i;
    int rs, gs, bs, rbits = 5, gbits;
    int pc = 0;
    unsigned int alpha = 0;
    switch (fmt) {
    /* 32 bits, as a big-endian long (the 68k's): ARGB is 0xAARRGGBB */
    case PIXFMT_ARGB32: rs = 16; gs = 8; bs = 0; alpha = 0xFF000000U; goto deep;
    case PIXFMT_BGRA32: rs = 8; gs = 16; bs = 24; alpha = 0xFFU; goto deep;
    case PIXFMT_RGBA32: rs = 24; gs = 16; bs = 8; alpha = 0xFFU; goto deep;
    case PIXFMT_RGB16PC: pc = 1; /* fall through */
    case PIXFMT_RGB16: rs = 11; gs = 5; bs = 0; gbits = 6; break;
    case PIXFMT_BGR16PC: pc = 1; /* fall through */
    case PIXFMT_BGR16: rs = 0; gs = 5; bs = 11; gbits = 6; break;
    case PIXFMT_RGB15PC: pc = 1; /* fall through */
    case PIXFMT_RGB15: rs = 10; gs = 5; bs = 0; gbits = 5; break;
    case PIXFMT_BGR15PC: pc = 1; /* fall through */
    case PIXFMT_BGR15: rs = 0; gs = 5; bs = 10; gbits = 5; break;
    default: return 0;
    }
    for (i = 0; i < 256; i++) {
        v = (i >> (8 - rbits)) << rs;
        rt[i] = pc ? (unsigned short)(((v & 0xFF) << 8) | (v >> 8)) : (unsigned short)v;
        v = (i >> (8 - gbits)) << gs;
        gt[i] = pc ? (unsigned short)(((v & 0xFF) << 8) | (v >> 8)) : (unsigned short)v;
        v = (i >> (8 - rbits)) << bs;
        bt[i] = pc ? (unsigned short)(((v & 0xFF) << 8) | (v >> 8)) : (unsigned short)v;
    }
    return 2;
deep:
    for (i = 0; i < 256; i++) {
        rt4[i] = ((unsigned int)i << rs) | alpha;
        gt4[i] = (unsigned int)i << gs;
        bt4[i] = (unsigned int)i << bs;
    }
    return 4;
}

unsigned int ad_rgb32(unsigned long rgb)
{
    return rt4[(rgb >> 16) & 255] | gt4[(rgb >> 8) & 255] | bt4[rgb & 255];
}

void ad_lut32(const unsigned long *lut, unsigned int *l4)
{
    int i;
    for (i = 0; i < 256; i++)
        l4[i] = rt4[(lut[i] >> 16) & 255] | gt4[(lut[i] >> 8) & 255] | bt4[lut[i] & 255];
}

/* A colour (0xRRGGBB) as a pixel of the screen's format. */
unsigned short ad_rgb16(unsigned long rgb)
{
    return rt[(rgb >> 16) & 255] | gt[(rgb >> 8) & 255] | bt[rgb & 255];
}

void ad_lut16(const unsigned long *lut, unsigned short *l16)
{
    int i;
    for (i = 0; i < 256; i++)
        l16[i] = rt[(lut[i] >> 16) & 255] | gt[(lut[i] >> 8) & 255] | bt[lut[i] & 255];
    for (i = 0; i < 256; i++)
        l32[i] = ((unsigned int)l16[i] << 16) | l16[i];
}

/* HAM as one step a pixel: pixel = (the one before & hk[c]) | hs[c]. A
 * base colour keeps nothing and sets its colour; a modify keeps the other
 * two of red, green and blue and sets its own (all in the screen's format,
 * byte order and all, so they are its masks too). */
static unsigned short hk[256], hs[256];

void ad_ham(const unsigned short *l16, unsigned char depth)
{
    unsigned long c, bits = depth == 8 ? 6 : 4, lo = (1UL << bits) - 1;
    for (c = 0; c < 256; c++) {
        unsigned long v = c & lo, ctl = (c >> bits) & 3;
        v = bits == 4 ? v * 17 : (v << 2) | (v >> 4);
        switch (ctl) {
        case 0:
            hk[c] = 0;
            hs[c] = l16[c & lo];
            break;
        case 1:                             /* blue */
            hk[c] = (unsigned short)~bt[255];
            hs[c] = bt[v];
            break;
        case 2:                             /* red */
            hk[c] = (unsigned short)~rt[255];
            hs[c] = rt[v];
            break;
        default:                            /* green */
            hk[c] = (unsigned short)~gt[255];
            hs[c] = gt[v];
            break;
        }
    }
    for (c = 0; c < 256; c++) {
        hk32[c] = ((unsigned int)hk[c] << 16) | hk[c];
        hs32[c] = ((unsigned int)hs[c] << 16) | hs[c];
    }
}

/* HAM's steps at 32 bits, as ad_ham's: a modify keeps the other two and
 * the alpha (rt4[0] is the alpha alone) */
static unsigned int hk4[256], hs4[256];

void ad_ham32(const unsigned int *l4, unsigned char depth)
{
    unsigned long c, bits = depth == 8 ? 6 : 4, lo = (1UL << bits) - 1;
    for (c = 0; c < 256; c++) {
        unsigned long v = c & lo, ctl = (c >> bits) & 3;
        v = bits == 4 ? v * 17 : (v << 2) | (v >> 4);
        switch (ctl) {
        case 0:
            hk4[c] = 0;
            hs4[c] = l4[c & lo];
            break;
        case 1:                             /* blue */
            hk4[c] = ~bt4[255];
            hs4[c] = bt4[v];
            break;
        case 2:                             /* red */
            hk4[c] = ~(rt4[255] & ~rt4[0]);
            hs4[c] = rt4[v] & ~rt4[0];
            break;
        default:                            /* green */
            hk4[c] = ~gt4[255];
            hs4[c] = gt4[v];
            break;
        }
    }
}

/* Where a row of planes (plane p at a + p * rb) differs from b's: its
 * first byte column and one past its last; 0 if nowhere. */
int ad_row_span(const unsigned char *a, const unsigned char *b, unsigned long rb, unsigned char depth, unsigned long *x0, unsigned long *x1)
{
    unsigned long lo = rb, hi = 0, x;
    unsigned char p;
    if (!memcmp(a, b, rb * depth))
        return 0;
    for (p = 0; p < depth; p++, a += rb, b += rb) {
        for (x = 0; x < lo; x++)
            if (a[x] != b[x]) {
                lo = x;
                break;
            }
        for (x = rb; x > hi; x--)
            if (a[x - 1] != b[x - 1]) {
                hi = x;
                break;
            }
    }
    *x0 = lo;
    *x1 = hi;
    return lo < hi;
}

/* Byte columns x0 to x1 (one past) of a row of planes into the screen's
 * memory at out (the row's first pixel). Source pixel px goes to pixels
 * xo[px] to xo[px + 1] - 1; fx, if not 0, the whole number that is the
 * same (and quicker). HAM: each pixel hangs on the one before, so the row
 * is worked out from its start, and written from x0 to its end. */
/* The 8 pixels of h and l, k their place (0-7), c the colour number;
 * the compiler makes k constant and the shifts fixed. */
#define EIGHT(...)                                                      \
    do {                                                                \
        unsigned int c;                                                 \
        c = h >> 24;         { const int k = 0; __VA_ARGS__ }                     \
        c = (h >> 16) & 255; { const int k = 1; __VA_ARGS__ }                     \
        c = (h >> 8) & 255;  { const int k = 2; __VA_ARGS__ }                     \
        c = h & 255;         { const int k = 3; __VA_ARGS__ }                     \
        c = l >> 24;         { const int k = 4; __VA_ARGS__ }                     \
        c = (l >> 16) & 255; { const int k = 5; __VA_ARGS__ }                     \
        c = (l >> 8) & 255;  { const int k = 6; __VA_ARGS__ }                     \
        c = l & 255;         { const int k = 7; __VA_ARGS__ }                     \
    } while (0)

#ifdef AD_ASM
/* animdraw68k.s: whole columns at 1 or 2 pixels a pixel (its kinds) */
unsigned long ad_cols68k(const unsigned char *col, unsigned long rb, unsigned long depth,
                         unsigned long ncols, void *out, const void *ta, const void *tb,
                         unsigned long pix, unsigned long kind, const void *p2c);
#endif

void ad_row_put(const unsigned char *row, unsigned long rb, unsigned char depth, unsigned long w,
                unsigned long x0, unsigned long x1, unsigned short fx, const unsigned short *xo,
                int ham, const unsigned long *lut, const unsigned short *l16, unsigned char *out)
{
    const unsigned char *pp[8];
    unsigned long x, i;
    unsigned int pix = 0;
    unsigned short *o16 = (unsigned short *)out;
    unsigned long wx = x0;                  /* HAM: written from here */
    (void)lut;
    for (i = 0; i < depth && i < 8; i++)
        pp[i] = row + i * rb;
    if (ham) {
        x0 = 0;
        x1 = rb;
        pix = l16[0];                       /* each row starts from colour 0 */
    }
#ifdef AD_ASM
    if ((fx == 1 || fx == 2) && depth >= 1 && depth <= 8) {
        /* the whole columns by hand (animdraw68k.s); the rest below */
        unsigned long full = w / 8, top = (unsigned long)(depth - 1) * rb;
        if (full > x1)
            full = x1;
        if (ham) {
            unsigned long nw = wx < full ? wx : full;   /* left of what changed */
            if (nw)
                pix = ad_cols68k(row + top, rb, depth, nw, 0, hk, hs, pix, 4, p2c);
            if (full > nw)
                pix = ad_cols68k(row + top + nw, rb, depth, full - nw, o16 + xo[nw * 8],
                                 fx == 2 ? (const void *)hk32 : hk, fx == 2 ? (const void *)hs32 : hs,
                                 pix, fx == 2 ? 3 : 2, p2c);
            x0 = full;
        } else if (full > x0) {
            ad_cols68k(row + top + x0, rb, depth, full - x0, o16 + xo[x0 * 8],
                       fx == 2 ? (const void *)l32 : l16, 0, 0, fx == 2 ? 1 : 0, p2c);
            x0 = full;
        }
    }
#endif
    for (x = x0; x < x1; x++) {
        unsigned int h, l;
        long n = (long)w - (long)(x * 8);   /* its pixels in the picture: a row's last */
        const unsigned short *xp = xo + x * 8;  /* (a word's padding) may have none */
        if (n <= 0)
            break;
        GATHER(pp, x, depth, h, l);
        if (n >= 8) {                       /* all 8: unrolled, the colours from registers */
            if (ham) {
                if (x < wx)                 /* left of what changed: worked out, not written */
                    EIGHT(pix = (pix & hk[c]) | hs[c]; (void)k;);
                else if (fx == 2) {
                    unsigned int *o2 = (unsigned int *)(o16 + xp[0]);
                    EIGHT(pix = (pix & hk[c]) | hs[c]; o2[k] = pix | (pix << 16););
                } else if (fx == 1) {
                    unsigned short *o1 = o16 + xp[0];
                    EIGHT(pix = (pix & hk[c]) | hs[c]; o1[k] = (unsigned short)pix;);
                } else
                    EIGHT(unsigned short *q = o16 + xp[k], *e = o16 + xp[k + 1];
                          pix = (pix & hk[c]) | hs[c];
                          while (q < e) *q++ = (unsigned short)pix;);
            } else if (fx == 2) {
                unsigned int *o2 = (unsigned int *)(o16 + xp[0]);
                EIGHT(unsigned int v = l16[c]; o2[k] = v | (v << 16););
            } else if (fx == 1) {
                unsigned short *o1 = o16 + xp[0];
                EIGHT(o1[k] = l16[c];);
            } else
                EIGHT(unsigned short v = l16[c], *q = o16 + xp[k], *e = o16 + xp[k + 1];
                      while (q < e) *q++ = v;);
        } else {                            /* the row's last few: one at a time */
            unsigned char t[8];
            t[0] = h >> 24; t[1] = h >> 16; t[2] = h >> 8; t[3] = h;
            t[4] = l >> 24; t[5] = l >> 16; t[6] = l >> 8; t[7] = l;
            for (i = 0; i < (unsigned long)n; i++) {
                unsigned short v, *q = o16 + xp[i], *e = o16 + xp[i + 1];
                if (ham) {
                    pix = (pix & hk[t[i]]) | hs[t[i]];
                    if (x < wx)
                        continue;
                    v = (unsigned short)pix;
                } else
                    v = l16[t[i]];
                while (q < e)
                    *q++ = v;
            }
        }
    }
}

/* ad_row_put at 32 bits a pixel (l4: ad_lut32's), in C: the same rows,
 * columns and scales, four bytes a pixel */
void ad_row_put32(const unsigned char *row, unsigned long rb, unsigned char depth, unsigned long w,
                  unsigned long x0, unsigned long x1, unsigned short fx, const unsigned short *xo,
                  int ham, const unsigned int *l4, unsigned char *out)
{
    const unsigned char *pp[8];
    unsigned long x, i;
    unsigned int pix = 0;
    unsigned int *o4 = (unsigned int *)out;
    unsigned long wx = x0;                  /* HAM: written from here */
    for (i = 0; i < depth && i < 8; i++)
        pp[i] = row + i * rb;
    if (ham) {
        x0 = 0;
        x1 = rb;
        pix = l4[0];                        /* each row starts from colour 0 */
    }
    for (x = x0; x < x1; x++) {
        unsigned int h, l;
        long n = (long)w - (long)(x * 8);
        const unsigned short *xp = xo + x * 8;
        if (n <= 0)
            break;
        GATHER(pp, x, depth, h, l);
        if (n >= 8) {
            if (ham) {
                if (x < wx)
                    EIGHT(pix = (pix & hk4[c]) | hs4[c]; (void)k;);
                else if (fx == 2) {
                    unsigned int *o2 = o4 + xp[0];
                    EIGHT(pix = (pix & hk4[c]) | hs4[c]; o2[2 * k] = o2[2 * k + 1] = pix;);
                } else if (fx == 1) {
                    unsigned int *o1 = o4 + xp[0];
                    EIGHT(pix = (pix & hk4[c]) | hs4[c]; o1[k] = pix;);
                } else
                    EIGHT(unsigned int *q = o4 + xp[k], *e = o4 + xp[k + 1];
                          pix = (pix & hk4[c]) | hs4[c];
                          while (q < e) *q++ = pix;);
            } else if (fx == 2) {
                unsigned int *o2 = o4 + xp[0];
                EIGHT(unsigned int v = l4[c]; o2[2 * k] = o2[2 * k + 1] = v;);
            } else if (fx == 1) {
                unsigned int *o1 = o4 + xp[0];
                EIGHT(o1[k] = l4[c];);
            } else
                EIGHT(unsigned int v = l4[c], *q = o4 + xp[k], *e = o4 + xp[k + 1];
                      while (q < e) *q++ = v;);
        } else {
            unsigned char t[8];
            t[0] = h >> 24; t[1] = h >> 16; t[2] = h >> 8; t[3] = h;
            t[4] = l >> 24; t[5] = l >> 16; t[6] = l >> 8; t[7] = l;
            for (i = 0; i < (unsigned long)n; i++) {
                unsigned int v, *q = o4 + xp[i], *e = o4 + xp[i + 1];
                if (ham) {
                    pix = (pix & hk4[t[i]]) | hs4[t[i]];
                    if (x < wx)
                        continue;
                    v = pix;
                } else
                    v = l4[t[i]];
                while (q < e)
                    *q++ = v;
            }
        }
    }
}
