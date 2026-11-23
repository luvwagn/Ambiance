/*
 * Ambiance - pictures made small, in colours of their own
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
#include "amb.h"

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
    if (!o)
        return FALSE;
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

struct Thumb *thumb_new(ULONG dw, ULONG dh)
{
    ULONG mod = (dw + 15) & ~15UL;
    struct Thumb *t = AllocVec(sizeof(struct Thumb) + mod * dh + 32, MEMF_ANY | MEMF_CLEAR);
    /* + 32: gfx_chunky reads 32 pixels at a time, past a row's end */
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

struct Thumb *thumb_pick(const struct Thumb *s, ULONG dw, ULONG dh)
{
    struct Thumb *t;
    UWORD *xs;
    ULONG x, y;
    if (!s || !s->px || !dw || !dh || !(t = thumb_new(dw, dh)))
        return NULL;
    if (!(xs = AllocVec(dw * sizeof(UWORD), MEMF_ANY))) {
        thumb_free(t);
        return NULL;
    }
    for (x = 0; x < dw; x++)
        xs[x] = (UWORD)((x * s->w + s->w / 2) / dw);
    for (y = 0; y < dh; y++) {
        const UBYTE *sr = s->px + ((y * s->h + s->h / 2) / dh) * s->mod;
        UBYTE *dr = t->px + y * t->mod;
        for (x = 0; x < dw; x++)
            dr[x] = sr[xs[x]];
    }
    FreeVec(xs);
    CopyMem((APTR)s->pal, t->pal, sizeof(t->pal));
    t->ncol = s->ncol;
    return t;
}

/* Colours as they fit in: shrunk (or enlarged) pixel for pixel. */
static struct Thumb *exact(Object *o, ULONG sw, ULONG sh, ULONG dw, ULONG dh,
                           const ULONG *rgb, ULONG n)
{
    struct Thumb *t;
    UBYTE *row;
    ULONG x, y, sy, last = ~0UL;
    UWORD *xs;
    if (!(t = thumb_new(dw, dh)))
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

/* ---- median cut on a 15-bit histogram, over the cells in use
 *
 * The colours a cover uses are a few thousand of the histogram's 32768
 * cells. They are listed as they are first counted (occ), and a box is a
 * run of that list: shrinking, splitting and averaging a box go through
 * its cells in use only, not every cell of its volume (a 68030 spent most
 * of a cover's time there). */
struct Box {
    UBYTE lo[3], hi[3];
    UWORD a, z;                 /* its cells: occ[a .. z-1] */
    ULONG n;                    /* the pixels in them */
};

#define CELL(r, g, b) (((ULONG)(r) << 10) | ((ULONG)(g) << 5) | (b))
#define COMP(c, ax) ((ax) == 0 ? (c) >> 10 : (ax) == 1 ? ((c) >> 5) & 31 : (c) & 31)

static void box_shrink(const ULONG *hist, const UWORD *occ, struct Box *bx)
{
    UBYTE lo0 = 31, lo1 = 31, lo2 = 31, hi0 = 0, hi1 = 0, hi2 = 0;
    ULONG n = 0;
    UWORD i;
    for (i = bx->a; i < bx->z; i++) {
        UWORD c = occ[i];
        UBYTE r = c >> 10, g = (c >> 5) & 31, b = c & 31;
        n += hist[c];
        if (r < lo0) lo0 = r;
        if (r > hi0) hi0 = r;
        if (g < lo1) lo1 = g;
        if (g > hi1) hi1 = g;
        if (b < lo2) lo2 = b;
        if (b > hi2) hi2 = b;
    }
    bx->n = n;
    if (n) {
        bx->lo[0] = lo0; bx->lo[1] = lo1; bx->lo[2] = lo2;
        bx->hi[0] = hi0; bx->hi[1] = hi1; bx->hi[2] = hi2;
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

/* Split boxes until there are maxcol, or none can be split. occ (nocc
 * cells in use) is reordered so each box's cells stay a run; tmp is room
 * for nocc more. */
static LONG median_cut(const ULONG *hist, UWORD *occ, UWORD *tmp, UWORD nocc, struct Box *box,
                       LONG maxcol)
{
    LONG nb = 1, i;
    box[0].a = 0;
    box[0].z = nocc;
    box_shrink(hist, occ, &box[0]);
    if (!box[0].n)
        return 0;
    while (nb < maxcol) {
        ULONG best = 0, cnt[32], cum;
        LONG pick = -1, a, m;
        UWORD w, h, k;
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
        for (k = s->a; k < s->z; k++)
            cnt[COMP(occ[k], a)] += hist[occ[k]];
        cum = 0;
        for (m = s->lo[a]; m < s->hi[a]; m++) {
            cum += cnt[m];
            if (cum * 2 >= s->n)
                break;
        }
        if (m >= s->hi[a])
            m = s->hi[a] - 1;
        /* the run parted: up to m first, the rest after */
        w = s->a;
        h = 0;
        for (k = s->a; k < s->z; k++) {
            UWORD c = occ[k];
            if ((LONG)COMP(c, a) <= m)
                occ[w++] = c;
            else
                tmp[h++] = c;
        }
        CopyMem(tmp, occ + w, (ULONG)h * sizeof(UWORD));
        t = &box[nb++];
        *t = *s;
        s->z = w;
        t->a = w;
        box_shrink(hist, occ, s);
        box_shrink(hist, occ, t);
    }
    return nb;
}

/* A box's colour (the average of its cells), its cells' colour in lut. */
static ULONG box_colour(const ULONG *hist, const UWORD *occ, const struct Box *bx, UBYTE *lut,
                        LONG i)
{
    ULONG sr = 0, sg = 0, sb = 0, n = 0;
    UWORD k;
    for (k = bx->a; k < bx->z; k++) {
        UWORD c = occ[k];
        ULONG h = hist[c], r = c >> 10, g = (c >> 5) & 31, b = c & 31;
        lut[c] = i;
        sr += h * ((r << 3) | (r >> 2));
        sg += h * ((g << 3) | (g >> 2));
        sb += h * ((b << 3) | (b >> 2));
        n += h;
    }
    if (!n)
        n = 1;
    return ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
}

static const UBYTE bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

/* A gun plus its dither (0..7, bayer / 2) to 5 bits: q5[v + d], once
 * per gun, not a compare and a shift (the dither centred: - 4). */
static UBYTE q5[256 + 8];
static void q5_init(void)
{
    static BOOL done;
    LONG i;
    if (done)
        return;
    for (i = 0; i < 256 + 8; i++) {
        LONG v = i - 4;
        q5[i] = v < 0 ? 0 : v > 255 ? 31 : v >> 3;
    }
    done = TRUE;
}

/* Where a picture's time went, the last one made (thumb_times). */
static ULONG tm_read, tm_cells, tm_cut, tm_map;

void thumb_times(ULONG *read, ULONG *cells, ULONG *cut, ULONG *map)
{
    *read = tm_read;
    *cells = tm_cells;
    *cut = tm_cut;
    *map = tm_map;
}

#define ROWS_AT_ONCE 16         /* a picture as big as its thumbnail: read this many rows a call */

/* Averaged down, then maxcol colours of its own. rgb: the colour table of
 * a colour-table picture (read as LUT8), NULL for a deep one (read as
 * RGB). */
/* rows from a decoded picture in memory (mem: RGB, sw * 3 a row), or
 * from the object */
static BOOL get_rows(Object *o, const UBYTE *mem, UBYTE *buf, ULONG sw, ULONG top, ULONG n, BOOL rgb)
{
    if (mem) {
        CopyMem((APTR)(mem + top * sw * 3), buf, n * sw * 3);
        return TRUE;
    }
    return read_rows(o, buf, sw, top, n, rgb);
}

static struct Thumb *quantise(Object *o, const UBYTE *mem, ULONG sw, ULONG sh, ULONG dw, ULONG dh,
                              const ULONG *rgb, LONG maxcol)
{
    struct Thumb *t = NULL;
    UBYTE *rows = NULL, *lut = NULL;
    UWORD *cells = NULL, *xb = NULL, *occ = NULL, *tmp = NULL, nocc = 0;
    ULONG *sum = NULL, *hist = NULL, x, y, rmax, nmax, t0, t1;
    struct Box *box = NULL;
    LONG nb, i;
    ULONG last_n = 0, rec = 0;
    BOOL deep = rgb == NULL, same = deep && sw == dw && sh == dh;

    q5_init();
    t0 = time_ms();
    rmax = (sh + dh - 1) / dh + 1;
    if (same)
        rmax = ROWS_AT_ONCE;
    nmax = dw * dh < 32768 ? dw * dh : 32768;   /* cells that can be in use */
    rows = AllocVec(sw * rmax * (deep ? 3 : 1) + (deep ? 0 : sw * 3), MEMF_ANY);
    cells = AllocVec(dw * dh * sizeof(UWORD), MEMF_ANY);
    xb = AllocVec((dw + 1) * sizeof(UWORD), MEMF_ANY);
    sum = AllocVec(dw * 4 * sizeof(ULONG), MEMF_ANY);
    hist = AllocVec(32768 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
    lut = AllocVec(32768, MEMF_ANY);
    occ = AllocVec(nmax * sizeof(UWORD), MEMF_ANY);
    tmp = AllocVec(nmax * sizeof(UWORD), MEMF_ANY);
    box = AllocVec(256 * sizeof(struct Box), MEMF_ANY);
    if (!rows || !cells || !xb || !sum || !hist || !lut || !occ || !tmp || !box)
        goto out;
    for (x = 0; x <= dw; x++)
        xb[x] = x * sw / dw;

    if (same) {
        /* the size it is shown at (a Plex cover, asked for so): each
         * pixel its own cell, no averaging; rows read many at a time */
        for (y = 0; y < dh; y += rmax) {
            ULONG k = dh - y < rmax ? dh - y : rmax, r;
            t1 = time_ms();
            if (!get_rows(o, mem, rows, sw, y, k, TRUE))
                goto out;
            tm_read += time_ms() - t1;
            for (r = 0; r < k; r++) {
                const UBYTE *p = rows + r * sw * 3, *bay = bayer[(y + r) & 3];
                UWORD *c = cells + (y + r) * dw;
                for (x = 0; x < dw; x++, p += 3) {
                    UBYTE d = bay[x & 3] >> 1;
                    UWORD v = ((UWORD)q5[p[0] + d] << 10) | ((UWORD)q5[p[1] + d] << 5) | q5[p[2] + d];
                    c[x] = v;
                    if (!hist[v]++)
                        occ[nocc++] = v;
                }
            }
        }
    } else {
        /* each small pixel: the average of the big ones it covers (one
         * big one, picked, when enlarging) */
        for (y = 0; y < dh; y++) {
            ULONG sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh, r;
            UWORD *c = cells + y * dw;
            if (sy1 <= sy0)
                sy1 = sy0 + 1;
            if (sy1 > sh)
                sy1 = sh;
            if (sy1 - sy0 > rmax)
                sy1 = sy0 + rmax;
            t1 = time_ms();
            if (!get_rows(o, mem, rows, sw, sy0, sy1 - sy0, deep))
                goto out;
            tm_read += time_ms() - t1;
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
                UBYTE d = bayer[y & 3][x & 3] >> 1;
                UWORD v;
                if (n != last_n) {
                    last_n = n;             /* a multiply, not a divide, per pixel */
                    rec = 65536UL / n + 1;
                }
                v = ((UWORD)q5[((s[0] * rec) >> 16 > 255 ? 255 : (s[0] * rec) >> 16) + d] << 10) |
                    ((UWORD)q5[((s[1] * rec) >> 16 > 255 ? 255 : (s[1] * rec) >> 16) + d] << 5) |
                    q5[((s[2] * rec) >> 16 > 255 ? 255 : (s[2] * rec) >> 16) + d];
                c[x] = v;
                if (!hist[v]++)
                    occ[nocc++] = v;
            }
        }
    }
    tm_cells = time_ms() - t0 - tm_read;

    t1 = time_ms();
    if ((nb = median_cut(hist, occ, tmp, nocc, box, maxcol)) <= 0 || !(t = thumb_new(dw, dh)))
        goto out;
    for (i = 0; i < nb; i++)
        t->pal[i] = box_colour(hist, occ, &box[i], lut, i);
    t->ncol = nb;
    tm_cut = time_ms() - t1;
    t1 = time_ms();
    for (y = 0; y < dh; y++) {
        UBYTE *d = t->px + y * t->mod;
        const UWORD *c = cells + y * dw;
        for (x = 0; x < dw; x++)
            d[x] = lut[c[x]];
    }
    tm_map = time_ms() - t1;
out:
    FreeVec(rows);
    FreeVec(cells);
    FreeVec(xb);
    FreeVec(sum);
    FreeVec(hist);
    FreeVec(lut);
    FreeVec(occ);
    FreeVec(tmp);
    FreeVec(box);
    return t;
}

/* A palette for many pictures (a video's, from pictures sampled through
 * it): their 15-bit histogram (hist, 32768 cells, counted by the caller)
 * cut to at most maxcol colours (median cut, as a thumbnail's), pal their
 * colours (0xRRGGBB), lut (32768) each cell's colour number: the cells
 * not counted too, each given its nearest counted cell's (a flood out
 * from them through the cube, a step at a time), so any colour has one.
 * Returns the colours made, 0 if none (nothing counted, no memory). */
LONG thumb_cut(const ULONG *hist, LONG maxcol, ULONG *pal, UBYTE *lut)
{
    UWORD *occ, *tmp, *q, nocc = 0;
    struct Box *box;
    UBYTE *done;
    ULONG i, qh, qt;
    LONG nb = 0, k;
    occ = AllocVec(32768 * sizeof(UWORD), MEMF_ANY);
    tmp = AllocVec(32768 * sizeof(UWORD), MEMF_ANY);
    q = AllocVec(32768 * sizeof(UWORD), MEMF_ANY);
    box = AllocVec(256 * sizeof(struct Box), MEMF_ANY);
    done = AllocVec(32768, MEMF_ANY | MEMF_CLEAR);
    if (!occ || !tmp || !q || !box || !done || maxcol < 1)
        goto out;
    if (maxcol > 256)
        maxcol = 256;
    for (i = 0; i < 32768; i++)
        if (hist[i])
            occ[nocc++] = (UWORD)i;
    if (!nocc || (nb = median_cut(hist, occ, tmp, nocc, box, maxcol)) <= 0) {
        nb = 0;
        goto out;
    }
    for (k = 0; k < nb; k++)
        pal[k] = box_colour(hist, occ, &box[k], lut, k);
    /* the cells not counted: a breadth-first flood from those that were */
    qh = qt = 0;
    for (i = 0; i < nocc; i++) {
        done[occ[i]] = 1;
        q[qt++] = occ[i];
    }
    while (qh < qt) {
        UWORD c = q[qh++], n[6];
        int j, nn = 0;
        UBYTE r = c >> 10, g = (c >> 5) & 31, b = c & 31;
        if (r > 0) n[nn++] = c - 1024;
        if (r < 31) n[nn++] = c + 1024;
        if (g > 0) n[nn++] = c - 32;
        if (g < 31) n[nn++] = c + 32;
        if (b > 0) n[nn++] = c - 1;
        if (b < 31) n[nn++] = c + 1;
        for (j = 0; j < nn; j++)
            if (!done[n[j]]) {
                done[n[j]] = 1;
                lut[n[j]] = lut[c];
                q[qt++] = n[j];
            }
    }
out:
    FreeVec(occ);
    FreeVec(tmp);
    FreeVec(q);
    FreeVec(box);
    FreeVec(done);
    return nb;
}

/* A thumbnail with more colours in use than maxcol: given maxcol of its
 * own. FALSE: no memory. */
BOOL thumb_reduce(struct Thumb *t, LONG maxcol)
{
    UBYTE used[256], *lut;
    UWORD *cells, *occ, *tmp, nocc = 0;
    ULONG *hist, x, y, nused = 0, i, nmax = (ULONG)t->w * t->h < 32768 ? (ULONG)t->w * t->h : 32768;
    struct Box *box;
    LONG nb;
    ULONG pal[256];
    if (maxcol <= THUMB_DEEP)
        return TRUE;                        /* (true colour asked for: as many as it has) */
    q5_init();
    memset(used, 0, sizeof(used));
    for (y = 0; y < t->h; y++) {
        const UBYTE *p = t->px + y * t->mod;
        for (x = 0; x < t->w; x++)
            used[p[x]] = 1;
    }
    for (i = 0; i < 256; i++)
        nused += used[i];
    if (nused <= (ULONG)maxcol)
        return TRUE;                        /* only those get pens (gfx_thumb) */
    cells = AllocVec((ULONG)t->w * t->h * sizeof(UWORD), MEMF_ANY);
    hist = AllocVec(32768 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
    lut = AllocVec(32768, MEMF_ANY);
    box = AllocVec(256 * sizeof(struct Box), MEMF_ANY);
    occ = AllocVec(nmax * sizeof(UWORD), MEMF_ANY);
    tmp = AllocVec(nmax * sizeof(UWORD), MEMF_ANY);
    if (!cells || !hist || !lut || !box || !occ || !tmp) {
        FreeVec(cells);
        FreeVec(hist);
        FreeVec(lut);
        FreeVec(box);
        FreeVec(occ);
        FreeVec(tmp);
        return FALSE;
    }
    for (y = 0; y < t->h; y++) {
        const UBYTE *p = t->px + y * t->mod;
        UWORD *c = cells + y * t->w;
        for (x = 0; x < t->w; x++) {
            ULONG v = t->pal[p[x]];
            LONG d = (LONG)bayer[y & 3][x & 3] / 2 - 4, r, g, b;
            r = (LONG)((v >> 16) & 255) + d;
            g = (LONG)((v >> 8) & 255) + d;
            b = (LONG)(v & 255) + d;
            r = r < 0 ? 0 : r > 255 ? 31 : r >> 3;
            g = g < 0 ? 0 : g > 255 ? 31 : g >> 3;
            b = b < 0 ? 0 : b > 255 ? 31 : b >> 3;
            c[x] = CELL(r, g, b);
            if (!hist[c[x]]++)
                occ[nocc++] = c[x];
        }
    }
    nb = median_cut(hist, occ, tmp, nocc, box, maxcol);
    for (i = 0; i < (ULONG)nb; i++)
        pal[i] = box_colour(hist, occ, &box[i], lut, i);
    for (y = 0; y < t->h; y++) {
        UBYTE *p = t->px + y * t->mod;
        const UWORD *c = cells + y * t->w;
        for (x = 0; x < t->w; x++)
            p[x] = lut[c[x]];
    }
    CopyMem(pal, t->pal, nb * sizeof(ULONG));
    t->ncol = nb;
    FreeVec(cells);
    FreeVec(hist);
    FreeVec(lut);
    FreeVec(box);
    FreeVec(occ);
    FreeVec(tmp);
    return TRUE;
}

/* A deep picture made small in its own colours (THUMB_DEEP: the layouts',
 * drawn in true colour, so not cut down to a palette): each small pixel
 * the average of the big ones it covers (one, picked, when enlarging),
 * as quantise averages. o or mem as get_rows takes them. */
static struct Thumb *deep_thumb(Object *o, const UBYTE *mem, ULONG sw, ULONG sh, ULONG dw, ULONG dh)
{
    struct Thumb *t;
    UBYTE *rows = NULL;
    UWORD *xb = NULL;
    ULONG *sum = NULL, x, y, rmax = (sh + dh - 1) / dh + 1, t0 = time_ms(), last_n = 0, rec = 0;
    if (!dw || !dh || !(t = AllocVec(sizeof(struct Thumb) + dw * dh * 4, MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    t->w = dw;
    t->h = dh;
    t->mod = dw;
    t->argb = (ULONG *)(t + 1);
    rows = AllocVec(sw * rmax * 3, MEMF_ANY);
    xb = AllocVec((dw + 1) * sizeof(UWORD), MEMF_ANY);
    sum = AllocVec(dw * 4 * sizeof(ULONG), MEMF_ANY);
    if (!rows || !xb || !sum)
        goto fail;
    for (x = 0; x <= dw; x++)
        xb[x] = x * sw / dw;
    for (y = 0; y < dh; y++) {
        ULONG sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh, r, *d = t->argb + y * dw;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        if (sy1 > sh)
            sy1 = sh;
        if (sy1 - sy0 > rmax)
            sy1 = sy0 + rmax;
        if (!get_rows(o, mem, rows, sw, sy0, sy1 - sy0, TRUE))
            goto fail;
        if (sw == dw && sy1 - sy0 == 1) {
            const UBYTE *p = rows;
            for (x = 0; x < dw; x++, p += 3)
                d[x] = ((ULONG)p[0] << 16) | ((ULONG)p[1] << 8) | p[2];
            continue;
        }
        memset(sum, 0, dw * 4 * sizeof(ULONG));
        for (r = 0; r < sy1 - sy0; r++) {
            const UBYTE *p = rows + r * sw * 3;
            for (x = 0; x < dw; x++) {
                ULONG k = xb[x], e = xb[x + 1], *s = sum + x * 4;
                const UBYTE *q = p + k * 3;
                if (e <= k)
                    e = k + 1;
                s[3] += e - k;
                for (; k < e; k++, q += 3) {
                    s[0] += q[0];
                    s[1] += q[1];
                    s[2] += q[2];
                }
            }
        }
        for (x = 0; x < dw; x++) {
            /* a multiply by 65536/n, not three divides: on a real 68060 the
             * divides were about a third of a photo's time at 1280 wide */
            ULONG *s = sum + x * 4, n = s[3] ? s[3] : 1, h = n >> 1, r, g, b;
            if (n != last_n) {
                last_n = n;
                rec = 65536UL / n + 1;
            }
            r = ((s[0] + h) * rec) >> 16;
            g = ((s[1] + h) * rec) >> 16;
            b = ((s[2] + h) * rec) >> 16;
            d[x] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    }
    tm_cells = time_ms() - t0;
    FreeVec(rows);
    FreeVec(xb);
    FreeVec(sum);
    return t;
fail:
    FreeVec(rows);
    FreeVec(xb);
    FreeVec(sum);
    FreeVec(t);
    return NULL;
}

/* A thumbnail of a picture decoded into memory (RGB, sw * 3 a row):
 * jpgthumb.c's, made as thumb_make makes a deep picture's */
struct Thumb *thumb_from_rgb(const UBYTE *rgb, ULONG sw, ULONG sh, ULONG dw, ULONG dh, LONG maxcol)
{
    tm_read = tm_cells = tm_cut = tm_map = 0;
    if (maxcol <= THUMB_DEEP)
        return deep_thumb(NULL, rgb, sw, sh, dw, dh);
    return quantise(NULL, rgb, sw, sh, dw, dh, NULL, maxcol);
}

struct Thumb *thumb_make(APTR obj, ULONG dw, ULONG dh, LONG maxcol)
{
    Object *o = obj;
    struct BitMapHeader *bmh = NULL;
    ULONG *cregs = NULL, ncol = 0, mode = 0, sw, sh, i, n;
    ULONG rgb[256];
    BOOL deep;
    BOOL true_col = maxcol <= THUMB_DEEP;
    tm_read = tm_cells = tm_cut = tm_map = 0;
    if (!o || !dw || !dh)
        return NULL;
    if (maxcol > 256)
        maxcol = 256;
    if (maxcol < 2 && !true_col)
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
        return true_col ? deep_thumb(o, NULL, sw, sh, dw, dh) : quantise(o, NULL, sw, sh, dw, dh, NULL, maxcol);
    if (true_col && (dw < sw || dh < sh)) {
        /* true colour (a layout's) made smaller: averaged, as a deep one
         * (picked pixel for pixel, a game's screenshot's dithered shades
         * and fine lines broke into specks: Chaos Engine's stone) */
        struct Thumb *t = deep_thumb(o, NULL, sw, sh, dw, dh);
        if (t)
            return t;
    }

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
    {
        /* picked pixel for pixel, as it fits or not: its colours are
         * then cut down, if need be, on the small picture (far quicker
         * than averaging the big one, read through its colour table) */
        struct Thumb *t = exact(o, sw, sh, dw, dh, rgb, n);
        if (t && !thumb_reduce(t, maxcol)) {
            thumb_free(t);
            t = NULL;
        }
        return t;
    }
}
