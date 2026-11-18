/*
 * Ambiance - the layouts' drawing kit: fills, gradients, rounded boxes,
 * frames, pictures and text over a known background
 * Copyright (c) 2026 Brian King
 *
 * Plain C with no Amiga headers, so the tests build it on the host too.
 * Everything is drawn as two kinds of output, a plain fill or a block of
 * pixels: lkit.c sends them to a graphics card's screen (FillPixelArray,
 * WritePixelArray), the tests to a picture in memory. Colours are
 * 0xRRGGBB. Gradients are dithered on 15 and 16-bit screens (a 4 x 4
 * ordered pattern), so they do not band.
 *
 * Text comes in as coverage (0-15 a pixel, from the smooth fonts) and is
 * drawn opaque over the colour that is known to be under it (a panel's,
 * or a vertical gradient's at each row), so nothing is read back from
 * the screen.
 */
#ifndef LKCORE_H
#define LKCORE_H

#define LK_NOBG 0xFFFFFFFFUL    /* no background colour known: edges hard, not blended */

struct LkOut {
    void *ctx;                  /* the output's own (lkit.c: the Gfx) */
    /* a w x h block of one colour at x, y (already clipped) */
    void (*fill)(void *ctx, long x, long y, long w, long h, unsigned long rgb);
    /* w x h pixels (0x00RRGGBB) at x, y (already clipped), mod pixels a row */
    void (*rows)(void *ctx, long x, long y, long w, long h, const unsigned long *px, long mod);
    /* the w x h block at x, y moved by dx, dy (both inside the screen); 0
     * if it cannot be (NULL: never) */
    int (*copy)(void *ctx, long x, long y, long w, long h, long dx, long dy);
    int bits;                   /* the screen's: 15, 16, 24 or 32 */
    long cx0, cy0, cx1, cy1;    /* drawn only inside x cx0..cx1-1, y cy0..cy1-1 */
    unsigned long *buf;         /* scratch pixels for rows being made, */
    long bufn;                  /*   bufn of them (at least the widest row) */
};

/* What is under text: a vertical gradient from top (at row y) to bottom
 * (at row y + h - 1), or one colour (top == bottom). */
struct LkBg {
    unsigned long top, bottom;
    long y, h;
};

void lk_clip(struct LkOut *o, long x0, long y0, long x1, long y1);   /* x1, y1: one past */

/* a + (b - a) * num / den, channel by channel */
unsigned long lk_mix(unsigned long a, unsigned long b, long num, long den);
/* row r (0..h-1) of a vertical gradient from top to bottom */
unsigned long lk_vgrad_at(unsigned long top, unsigned long bottom, long h, long r);
/* what is under text at screen row y */
unsigned long lk_bg_at(const struct LkBg *bg, long y);
void lk_bg_flat(struct LkBg *bg, unsigned long rgb);

void lk_fill(struct LkOut *o, long x, long y, long w, long h, unsigned long rgb);
/* what is drawn at x, y (w x h) moved by dx, dy, inside the clip (what would
 * come from outside it is left out): 0 if the output cannot */
int lk_copy(struct LkOut *o, long x, long y, long w, long h, long dx, long dy);
void lk_vgrad(struct LkOut *o, long x, long y, long w, long h, unsigned long top, unsigned long bottom);
void lk_hgrad(struct LkOut *o, long x, long y, long w, long h, unsigned long left, unsigned long right);
/* r: the corners' radius; bg: the colour around the box, for smooth
 * corners (LK_NOBG: hard ones, nothing outside them drawn) */
void lk_rrect(struct LkOut *o, long x, long y, long w, long h, long r, unsigned long rgb, unsigned long bg);
/* an outline t pixels thick, inside x, y, w, h */
void lk_frame(struct LkOut *o, long x, long y, long w, long h, long t, unsigned long rgb);
/* a picture's pixels (0x00RRGGBB), mod pixels a row */
void lk_image(struct LkOut *o, long x, long y, const unsigned long *px, long w, long h, long mod);
/* coverage cov (0-15 a pixel, w x h, mod bytes a row) in fg, opaque over bg */
void lk_cover(struct LkOut *o, long x, long y, const unsigned char *cov, long w, long h, long mod,
              unsigned long fg, const struct LkBg *bg);

#endif
