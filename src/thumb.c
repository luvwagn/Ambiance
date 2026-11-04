/*
 * AmigaMCE - pictures made small, in colours of their own
 * Copyright (c) 2026 Brian King
 *
 * A picture with more colours than a colour-table screen can give it (a
 * JPEG, a true-colour PNG or GIF, HAM) used to be remapped by its datatype
 * at full size: every pixel of a 768 x 572 picture matched to the screen's
 * pens, seconds on a 68030, and then most of them thrown away when it was
 * shrunk to the preview. Here it is shrunk first, each small pixel the
 * average of the big ones it stands for, and only then given colours: the
 * best few for that small picture (median cut), found on a 15-bit
 * histogram so each pixel's colour is one table look-up. A light ordered
 * dither keeps gradients from banding.
 *
 * A picture that fits within the colours allowed keeps its own, exactly:
 * it is only shrunk (or enlarged), pixel for pixel.
 *
 * What comes out (struct Thumb) holds no pens: colour numbers and their
 * colours. gfx_thumb draws it, taking pens then. So it can be made in
 * the loader process, and kept ahead, for little memory.
 */
#include "amce.h"

#include <string.h>
#include <graphics/modeid.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <proto/exec.h>
#include <proto/datatypes.h>
#include <clib/alib_protos.h>

/* Read rows top..top+n-1 of the picture as LUT8 (one byte each) or RGB
 * (three). DoMethodA, not DoDTMethodA: see gfx.c own_remap. */
static BOOL read_rows(Object *o, UBYTE *buf, ULONG sw, ULONG top, ULONG n, BOOL rgb)
{
    struct pdtBlitPixelArray pbpa;
    pbpa.MethodID = PDTM_READPIXELARRAY;
    pbpa.pbpa_PixelData = buf;
    pbpa.pbpa_PixelFormat = rgb ? PBPAFMT_RGB : PBPAFMT_LUT8;
    pbpa.pbpa_PixelArrayMod = rgb ? sw * 3 : sw;
    pbpa.pbpa_Left = 0;
    pbpa.pbpa_Top = top;
    pbpa.pbpa_Width = sw;
    pbpa.pbpa_Height = n;
    return DoMethodA(o, (Msg)&pbpa) != 0;
}

static struct Thumb *thumb_alloc(ULONG dw, ULONG dh)
{
    ULONG mod = (dw + 15) & ~15UL;
    struct Thumb *t = AllocVec(sizeof(struct Thumb) + mod * dh, MEMF_ANY | MEMF_CLEAR);
    if (!t)
        return NULL;
    t->w = dw;
    t->h = dh;
    t->mod = mod;
    t->px = (UBYTE *)(t + 1);
    return t;
}

void thumb_free(struct Thumb *t)
{
    FreeVec(t);
}

/* Colours as they fit in: shrunk (or enlarged) pixel for pixel. */
static struct Thumb *exact(Object *o, ULONG sw, ULONG sh, ULONG dw, ULONG dh,
                           const ULONG *rgb, ULONG n)
{
    struct Thumb *t;
    UBYTE *row;
    ULONG x, y, sy, last = ~0UL;
    UWORD *xs;
    if (!(t = thumb_alloc(dw, dh)))
        return NULL;
    row = AllocVec(sw, MEMF_ANY);
    xs = AllocVec(dw * sizeof(UWORD), MEMF_ANY);
    if (!row || !xs) {
        FreeVec(row);
        FreeVec(xs);
        FreeVec(t);
        return NULL;
    }
    for (x = 0; x < dw; x++)
        xs[x] = (x * sw + sw / 2) / dw;
    for (y = 0; y < dh; y++) {
        UBYTE *d = t->px + y * t->mod;
        sy = (y * sh + sh / 2) / dh;
        if (sy != last && !read_rows(o, row, sw, sy, 1, FALSE)) {
            FreeVec(row);
            FreeVec(xs);
            FreeVec(t);
            return NULL;
        }
        last = sy;
        for (x = 0; x < dw; x++)
            d[x] = row[xs[x]];
    }
    FreeVec(row);
    FreeVec(xs);
    t->ncol = n;
    CopyMem((APTR)rgb, t->pal, n * sizeof(ULONG));
    return t;
}

/* ---- median cut on a 15-bit histogram */
struct Box {
    UBYTE lo[3], hi[3];
    ULONG n;
};

#define CELL(r, g, b) (((ULONG)(r) << 10) | ((ULONG)(g) << 5) | (b))

static void box_shrink(const ULONG *hist, struct Box *bx)
{
    UBYTE lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
    ULONG r, g, b, n = 0;
    for (r = bx->lo[0]; r <= bx->hi[0]; r++)
        for (g = bx->lo[1]; g <= bx->hi[1]; g++) {
            const ULONG *h = hist + CELL(r, g, 0);
            for (b = bx->lo[2]; b <= bx->hi[2]; b++)
                if (h[b]) {
                    n += h[b];
                    if (r < lo[0]) lo[0] = r;
                    if (r > hi[0]) hi[0] = r;
                    if (g < lo[1]) lo[1] = g;
                    if (g > hi[1]) hi[1] = g;
                    if (b < lo[2]) lo[2] = b;
                    if (b > hi[2]) hi[2] = b;
                }
        }
    bx->n = n;
    if (n) {
        memcpy(bx->lo, lo, 3);
        memcpy(bx->hi, hi, 3);
    }
}

static int longest(const struct Box *bx)
{
    int a = 0, i;
    for (i = 1; i < 3; i++)
        if (bx->hi[i] - bx->lo[i] > bx->hi[a] - bx->lo[a])
            a = i;
    return a;
}

/* Split boxes until there are maxcol, or none can be split. */
static LONG median_cut(const ULONG *hist, struct Box *box, LONG maxcol)
{
    LONG nb = 1, i;
    box[0].lo[0] = box[0].lo[1] = box[0].lo[2] = 0;
    box[0].hi[0] = box[0].hi[1] = box[0].hi[2] = 31;
    box_shrink(hist, &box[0]);
    if (!box[0].n)
        return 0;
    while (nb < maxcol) {
        ULONG best = 0, cnt[32], cum, r, g, b;
        LONG pick = -1, a, m;
        struct Box *s, *t;
        for (i = 0; i < nb; i++) {
            int ax = longest(&box[i]);
            ULONG side = box[i].hi[ax] - box[i].lo[ax], score = box[i].n * side;
            if (side && score > best) {
                best = score;
                pick = i;
            }
        }
        if (pick < 0)
            break;
        s = &box[pick];
        a = longest(s);
        memset(cnt, 0, sizeof(cnt));
        for (r = s->lo[0]; r <= s->hi[0]; r++)
            for (g = s->lo[1]; g <= s->hi[1]; g++)
                for (b = s->lo[2]; b <= s->hi[2]; b++) {
                    ULONG c = hist[CELL(r, g, b)];
                    if (c)
                        cnt[a == 0 ? r : a == 1 ? g : b] += c;
                }
        cum = 0;
        for (m = s->lo[a]; m < s->hi[a]; m++) {
            cum += cnt[m];
            if (cum * 2 >= s->n)
                break;
        }
        if (m >= s->hi[a])
            m = s->hi[a] - 1;
        t = &box[nb++];
        *t = *s;
        s->hi[a] = m;
        t->lo[a] = m + 1;
        box_shrink(hist, s);
        box_shrink(hist, t);
    }
    return nb;
}

static const UBYTE bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

/* Averaged down, then maxcol colours of its own. rgb: the colour table of
 * a colour-table picture (read as LUT8), NULL for a deep one (read as
 * RGB). */
static struct Thumb *quantise(Object *o, ULONG sw, ULONG sh, ULONG dw, ULONG dh,
                              const ULONG *rgb, LONG maxcol)
{
    struct Thumb *t = NULL;
    UBYTE *rows = NULL, *lut = NULL;
    UWORD *cells = NULL, *xb = NULL;
    ULONG *sum = NULL, *hist = NULL, x, y, rmax;
    struct Box *box = NULL;
    LONG nb, i;
    BOOL deep = rgb == NULL;

    rmax = (sh + dh - 1) / dh + 1;
    rows = AllocVec(sw * rmax * (deep ? 3 : 1) + (deep ? 0 : sw * 3), MEMF_ANY);
    cells = AllocVec(dw * dh * sizeof(UWORD), MEMF_ANY);
    xb = AllocVec((dw + 1) * sizeof(UWORD), MEMF_ANY);
    sum = AllocVec(dw * 4 * sizeof(ULONG), MEMF_ANY);
    hist = AllocVec(32768 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
    lut = AllocVec(32768, MEMF_ANY);
    box = AllocVec(256 * sizeof(struct Box), MEMF_ANY);
    if (!rows || !cells || !xb || !sum || !hist || !lut || !box)
        goto out;
    for (x = 0; x <= dw; x++)
        xb[x] = x * sw / dw;

    /* each small pixel: the average of the big ones it covers (one big
     * one, picked, when enlarging) */
    for (y = 0; y < dh; y++) {
        ULONG sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh, r;
        UWORD *c = cells + y * dw;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        if (sy1 > sh)
            sy1 = sh;
        if (sy1 - sy0 > rmax)
            sy1 = sy0 + rmax;
        if (!read_rows(o, rows, sw, sy0, sy1 - sy0, deep))
            goto out;
        memset(sum, 0, dw * 4 * sizeof(ULONG));
        for (r = 0; r < sy1 - sy0; r++) {
            const UBYTE *p;
            if (deep) {
                p = rows + r * sw * 3;
            } else {
                /* colour numbers to colours, into the spare row at the end */
                const UBYTE *s = rows + r * sw;
                UBYTE *q = rows + sw * rmax;
                ULONG k;
                for (k = 0; k < sw; k++) {
                    ULONG v = rgb[s[k]];
                    *q++ = v >> 16;
                    *q++ = v >> 8;
                    *q++ = v;
                }
                p = rows + sw * rmax;
            }
            for (x = 0; x < dw; x++) {
                ULONG k = xb[x], e = xb[x + 1], *s = sum + x * 4;
                const UBYTE *q = p + k * 3;
                if (e <= k)
                    e = k + 1;
                for (; k < e; k++, q += 3) {
                    s[0] += q[0];
                    s[1] += q[1];
                    s[2] += q[2];
                }
                s[3] += e - xb[x] > 0 ? e - xb[x] : 1;
            }
        }
        for (x = 0; x < dw; x++) {
            ULONG *s = sum + x * 4, n = s[3] ? s[3] : 1;
            LONG d = (LONG)bayer[y & 3][x & 3] / 2 - 4, v[3];
            for (i = 0; i < 3; i++) {
                v[i] = (LONG)(s[i] / n) + d;
                v[i] = v[i] < 0 ? 0 : v[i] > 255 ? 31 : v[i] >> 3;
            }
            c[x] = CELL(v[0], v[1], v[2]);
            hist[c[x]]++;
        }
    }

    if ((nb = median_cut(hist, box, maxcol)) <= 0 || !(t = thumb_alloc(dw, dh)))
        goto out;
    for (i = 0; i < nb; i++) {
        ULONG r, g, b, sr = 0, sg = 0, sb = 0, n = 0;
        struct Box *bx = &box[i];
        for (r = bx->lo[0]; r <= bx->hi[0]; r++)
            for (g = bx->lo[1]; g <= bx->hi[1]; g++)
                for (b = bx->lo[2]; b <= bx->hi[2]; b++) {
                    ULONG cell = CELL(r, g, b), h = hist[cell];
                    if (h) {
                        lut[cell] = i;
                        sr += h * ((r << 3) | (r >> 2));
                        sg += h * ((g << 3) | (g >> 2));
                        sb += h * ((b << 3) | (b >> 2));
                        n += h;
                    }
                }
        if (!n)
            n = 1;
        t->pal[i] = ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
    }
    t->ncol = nb;
    for (y = 0; y < dh; y++) {
        UBYTE *d = t->px + y * t->mod;
        const UWORD *c = cells + y * dw;
        for (x = 0; x < dw; x++)
            d[x] = lut[c[x]];
    }
out:
    FreeVec(rows);
    FreeVec(cells);
    FreeVec(xb);
    FreeVec(sum);
    FreeVec(hist);
    FreeVec(lut);
    FreeVec(box);
    return t;
}

struct Thumb *thumb_make(APTR obj, ULONG dw, ULONG dh, LONG maxcol)
{
    Object *o = obj;
    struct BitMapHeader *bmh = NULL;
    ULONG *cregs = NULL, ncol = 0, mode = 0, sw, sh, i, n;
    ULONG rgb[256];
    BOOL deep;
    if (!o || !dw || !dh)
        return NULL;
    if (maxcol > 256)
        maxcol = 256;
    if (maxcol < 2)
        maxcol = 2;
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, PDTA_CRegs, (ULONG)&cregs,
               PDTA_NumColors, (ULONG)&ncol, PDTA_ModeID, (ULONG)&mode, TAG_END);
    if (!bmh || !bmh->bmh_Width || !bmh->bmh_Height)
        return NULL;
    if (mode == (ULONG)INVALID_ID)
        mode = 0;
    sw = bmh->bmh_Width;
    sh = bmh->bmh_Height;
    deep = bmh->bmh_Depth > 8 || (mode & HAM_KEY) || !cregs || !ncol;
    if (deep)
        return quantise(o, sw, sh, dw, dh, NULL, maxcol);

    /* the colour table, 8 bits a gun; extra halfbrite: the dark half too */
    memset(rgb, 0, sizeof(rgb));
    if (ncol > 256)
        ncol = 256;
    for (i = 0; i < ncol; i++)
        rgb[i] = ((cregs[i * 3] >> 24) << 16) | ((cregs[i * 3 + 1] >> 24) << 8) |
                 (cregs[i * 3 + 2] >> 24);
    n = ncol;
    if ((mode & EXTRAHALFBRITE_KEY) && ncol <= 32 && bmh->bmh_Depth == 6) {
        for (i = 0; i < 32; i++)
            rgb[32 + i] = (rgb[i] >> 1) & 0x7F7F7F;
        n = 64;
    }
    if (n <= (ULONG)maxcol)
        return exact(o, sw, sh, dw, dh, rgb, n);
    return quantise(o, sw, sh, dw, dh, rgb, maxcol);
}
