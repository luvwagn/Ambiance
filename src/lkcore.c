/*
 * Ambiance - the layouts' drawing kit (see lkcore.h)
 * Copyright (c) 2026 Brian King
 */
#include "lkcore.h"

/* 4 x 4 ordered dither: added to a channel before the screen keeps its
 * top 5 (or 6) bits, so on average it keeps the colour asked for */
static const unsigned char bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

void lk_clip(struct LkOut *o, long x0, long y0, long x1, long y1)
{
    o->cx0 = x0;
    o->cy0 = y0;
    o->cx1 = x1;
    o->cy1 = y1;
}

unsigned long lk_mix(unsigned long a, unsigned long b, long num, long den)
{
    unsigned long out = 0;
    int sh;
    if (den <= 0 || num <= 0)
        return a & 0xFFFFFFUL;
    if (num >= den)
        return b & 0xFFFFFFUL;
    for (sh = 16; sh >= 0; sh -= 8) {
        long ca = (long)((a >> sh) & 255), cb = (long)((b >> sh) & 255);
        out |= (unsigned long)(ca + (cb - ca) * num / den) << sh;
    }
    return out;
}

unsigned long lk_vgrad_at(unsigned long top, unsigned long bottom, long h, long r)
{
    return lk_mix(top, bottom, r, h - 1);
}

unsigned long lk_bg_at(const struct LkBg *bg, long y)
{
    if (bg->top == bg->bottom || bg->h <= 1)
        return bg->top;
    return lk_mix(bg->top, bg->bottom, y - bg->y, bg->h - 1);
}

void lk_bg_flat(struct LkBg *bg, unsigned long rgb)
{
    bg->top = bg->bottom = rgb;
    bg->y = 0;
    bg->h = 1;
}

/* the part of x, y, w, h inside the clip; 0 if none */
static int clip(const struct LkOut *o, long *x, long *y, long *w, long *h)
{
    long x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < o->cx0) x0 = o->cx0;
    if (y0 < o->cy0) y0 = o->cy0;
    if (x1 > o->cx1) x1 = o->cx1;
    if (y1 > o->cy1) y1 = o->cy1;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

/* pixels made for x, y (w x h, mod a row) sent, clipped */
static void put_rows(struct LkOut *o, long x, long y, long w, long h, const unsigned long *px, long mod)
{
    long cx = x, cy = y, cw = w, ch = h;
    if (!clip(o, &cx, &cy, &cw, &ch))
        return;
    o->rows(o->ctx, cx, cy, cw, ch, px + (cy - y) * mod + (cx - x), mod);
}

void lk_fill(struct LkOut *o, long x, long y, long w, long h, unsigned long rgb)
{
    if (clip(o, &x, &y, &w, &h))
        o->fill(o->ctx, x, y, w, h, rgb & 0xFFFFFFUL);
}

int lk_copy(struct LkOut *o, long x, long y, long w, long h, long dx, long dy)
{
    long sx, sy, sw, sh;
    if (!o->copy)
        return 0;
    /* the source inside the clip, then its destination inside it too */
    if (!clip(o, &x, &y, &w, &h))
        return 1;
    sx = x + dx;
    sy = y + dy;
    sw = w;
    sh = h;
    if (!clip(o, &sx, &sy, &sw, &sh))
        return 1;
    return o->copy(o->ctx, sx - dx, sy - dy, sw, sh, dx, dy);
}

/* the colour c as the four dithered pixels of row y (x = 0..3 mod 4) */
static void dither4(const struct LkOut *o, unsigned long c, long y, unsigned long *d)
{
    int k, gstep = o->bits == 16 ? 4 : 8;
    const unsigned char *b = bayer + ((y & 3) << 2);
    for (k = 0; k < 4; k++) {
        unsigned long r = (c >> 16) & 255, g = (c >> 8) & 255, bl = c & 255;
        r += (unsigned long)(b[k] * 8) >> 4;
        g += (unsigned long)(b[k] * gstep) >> 4;
        bl += (unsigned long)(b[k] * 8) >> 4;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (bl > 255) bl = 255;
        d[k] = r << 16 | g << 8 | bl;
    }
}

static int dithered(const struct LkOut *o)
{
    return o->bits < 24;
}

/* rows a chunk can hold, at w pixels a row */
static long chunk_rows(const struct LkOut *o, long w)
{
    long k = w > 0 ? o->bufn / w : 0;
    return k < 1 ? 1 : k;
}

void lk_vgrad(struct LkOut *o, long x, long y, long w, long h, unsigned long top, unsigned long bottom)
{
    long vx = x, vy = y, vw = w, vh = h, r, r1;
    if (top == bottom || h <= 1) {
        lk_fill(o, x, y, w, h, top);
        return;
    }
    if (!clip(o, &vx, &vy, &vw, &vh))
        return;
    if (!dithered(o) || o->bufn < vw) {
        /* rows of the same colour as one fill */
        for (r = vy - y; r < vy - y + vh; r = r1) {
            unsigned long c = lk_vgrad_at(top, bottom, h, r);
            for (r1 = r + 1; r1 < vy - y + vh && lk_vgrad_at(top, bottom, h, r1) == c; r1++)
                ;
            o->fill(o->ctx, vx, y + r, vw, r1 - r, c);
        }
        return;
    }
    {
        long k = chunk_rows(o, vw), j, i;
        for (r = vy - y; r < vy - y + vh; r += k) {
            long n = vy - y + vh - r < k ? vy - y + vh - r : k;
            for (j = 0; j < n; j++) {
                unsigned long d[4], *p = o->buf + j * vw;
                dither4(o, lk_vgrad_at(top, bottom, h, r + j), y + r + j, d);
                for (i = 0; i < vw; i++)
                    p[i] = d[(vx + i) & 3];
            }
            o->rows(o->ctx, vx, y + r, vw, n, o->buf, vw);
        }
    }
}

void lk_hgrad(struct LkOut *o, long x, long y, long w, long h, unsigned long left, unsigned long right)
{
    long vx = x, vy = y, vw = w, vh = h, k, j, i, r;
    if (left == right || w <= 1) {
        lk_fill(o, x, y, w, h, left);
        return;
    }
    if (!clip(o, &vx, &vy, &vw, &vh))
        return;
    k = chunk_rows(o, vw);
    if (dithered(o)) {
        k &= ~3L;                       /* the pattern's period: each chunk starts in step */
        if (k < 4)
            k = 4;
    }
    if (k > vh)
        k = vh;
    if (k * vw > o->bufn)
        return;                         /* (no room for even one period: not drawn) */
    for (j = 0; j < k; j++) {
        unsigned long *p = o->buf + j * vw;
        for (i = 0; i < vw; i++) {
            unsigned long c = lk_mix(left, right, vx - x + i, w - 1);
            if (dithered(o)) {
                unsigned long d[4];
                dither4(o, c, vy + j, d);
                c = d[(vx + i) & 3];
            }
            p[i] = c;
        }
    }
    for (r = 0; r < vh; r += k)
        o->rows(o->ctx, vx, vy + r, vw, vh - r < k ? vh - r : k, o->buf, vw);
}

/* how much of corner pixel (i, j) of a quarter circle of radius r is
 * inside it, 0-16 (4 x 4 samples): (0, 0) is the box's corner */
static int corner_cov(long i, long j, long r)
{
    long c = 8 * r, lim = c * c, n = 0, a, b;
    for (b = 0; b < 4; b++) {
        long dy = 8 * j + 2 * b + 1 - c;
        for (a = 0; a < 4; a++) {
            long dx = 8 * i + 2 * a + 1 - c;
            if (dx * dx + dy * dy <= lim)
                n++;
        }
    }
    return (int)n;
}

void lk_rrect(struct LkOut *o, long x, long y, long w, long h, long r, unsigned long rgb, unsigned long bg)
{
    long j, i;
    if (w <= 0 || h <= 0)
        return;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r <= 0) {
        lk_fill(o, x, y, w, h, rgb);
        return;
    }
    lk_fill(o, x, y + r, w, h - 2 * r, rgb);
    for (j = 0; j < r; j++) {
        long yt = y + j, yb = y + h - 1 - j;
        if (bg == LK_NOBG || o->bufn < w) {
            /* hard: the pixels at least half inside */
            long inset = 0;
            while (inset < r && corner_cov(inset, j, r) < 8)
                inset++;
            if (w - 2 * inset > 0) {
                lk_fill(o, x + inset, yt, w - 2 * inset, 1, rgb);
                lk_fill(o, x + inset, yb, w - 2 * inset, 1, rgb);
            }
            continue;
        }
        /* smooth: the corners' pixels that far between bg and rgb, the
         * row made once and sent for the top and the bottom */
        for (i = 0; i < w; i++)
            o->buf[i] = rgb & 0xFFFFFFUL;
        for (i = 0; i < r; i++) {
            unsigned long c = lk_mix(bg, rgb, corner_cov(i, j, r), 16);
            o->buf[i] = c;
            o->buf[w - 1 - i] = c;
        }
        put_rows(o, x, yt, w, 1, o->buf, w);
        put_rows(o, x, yb, w, 1, o->buf, w);
    }
}

void lk_frame(struct LkOut *o, long x, long y, long w, long h, long t, unsigned long rgb)
{
    if (t <= 0 || w <= 0 || h <= 0)
        return;
    if (2 * t >= w || 2 * t >= h) {
        lk_fill(o, x, y, w, h, rgb);
        return;
    }
    lk_fill(o, x, y, w, t, rgb);
    lk_fill(o, x, y + h - t, w, t, rgb);
    lk_fill(o, x, y + t, t, h - 2 * t, rgb);
    lk_fill(o, x + w - t, y + t, t, h - 2 * t, rgb);
}

void lk_image(struct LkOut *o, long x, long y, const unsigned long *px, long w, long h, long mod)
{
    if (w > 0 && h > 0)
        put_rows(o, x, y, w, h, px, mod);
}

void lk_cover(struct LkOut *o, long x, long y, const unsigned char *cov, long w, long h, long mod,
              unsigned long fg, const struct LkBg *bg)
{
    long k, r, j, i;
    unsigned long lut[16], last = LK_NOBG;
    if (w <= 0 || h <= 0 || o->bufn < w)
        return;
    k = chunk_rows(o, w);
    for (r = 0; r < h; r += k) {
        long n = h - r < k ? h - r : k;
        for (j = 0; j < n; j++) {
            const unsigned char *c = cov + (r + j) * mod;
            unsigned long *p = o->buf + j * w, b = lk_bg_at(bg, y + r + j);
            if (dithered(o) && bg && bg->top != bg->bottom && bg->h > 1 && !bg->rows_flat) {
                /* over a dithered gradient (lk_vgrad on 15/16-bit): the same
                 * dither under the text, or it sits in a box of its own */
                unsigned long d[4];
                dither4(o, b, y + r + j, d);
                for (i = 0; i < w; i++) {
                    int a = c[i] & 15;
                    unsigned long under = d[(x + i) & 3];
                    p[i] = a == 0 ? under : a == 15 ? fg : lk_mix(under, fg, a, 15);
                }
                continue;
            }
            if (b != last) {
                for (i = 0; i < 16; i++)
                    lut[i] = lk_mix(b, fg, i, 15);
                last = b;
            }
            for (i = 0; i < w; i++)
                p[i] = lut[c[i] & 15];
        }
        put_rows(o, x, y + r, w, n, o->buf, w);
    }
}
