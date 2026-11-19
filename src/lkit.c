/*
 * Ambiance - the layouts' drawing kit on a graphics card's screen (lkit.h)
 * Copyright (c) 2026 Brian King
 */
#include "amb.h"
#include "lkit.h"

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <graphics/gfxbase.h>

#include <stdlib.h>
#include <string.h>

extern struct Library *CyberGfxBase;
extern struct GfxBase *GfxBase;

/* Where an output draws: the screen (rp NULL: the Gfx's RastPort of the
 * moment) or the canvas; what has changed on the canvas since lk_show. */
struct LkTarget {
    struct Gfx *g;
    struct BitMap *bm;
    struct RastPort rp;
    long x0, y0, x1, y1;        /* changed: x0 > x1 when nothing */
    long oy;                    /* a strip's (lk_open_strip): the screen's row its top is */
    BOOL own;                   /* bm is the output's own (the canvas), freed with it */
    UBYTE *ch;                  /* an 8-bit screen's (lk_open_chunky): its pens, a byte a
                                 * pixel, the screen's size; shown with WriteChunkyPixels */
    long cw, chh;
    ULONG *pal_was;             /* ...the screen's colours before, put back at lk_close */
    ULONG *rowc;                /* ...the background's colour on each row (lk_ramp), drawn
                                 * with the ramp's pens; NULL: none yet */
    unsigned long ramp_a, ramp_b;
    UBYTE *pool;                /* ...the pen for each 5-5-5 colour asked for (0: not yet) */
    UBYTE *near;                /* ...the nearest pen for each 4-4-4 colour (pictures of
                                 * many colours, drawn with the pens there are) */
    UBYTE live[256];            /* ...the pool's pens made a colour and maybe on screen */
    UBYTE lives[256];           /* ...those pens, and the greys and ramp's: what is looked
                                 * through for the nearest */
    int nlive, nfree, hunt;
    BOOL asked;                 /* ...pens made or wanted since ck_gc last looked */
    BOOL no_new;                /* a picture of many colours drawn (a photo, a reflection):
                                 * the nearest pens there are, none made for it */
    ULONG rgbof[256];           /* each pen's colour (greys, ramp, the pool's) */
    ULONG *hist;                /* a picture's: how many of each 5-5-5 colour (lk_picture) */
    UWORD *list;                /* ...the colours it has */
    UBYTE *pmapb;               /* ...each one's pen */
    UBYTE *pmap;                /* pmapb while a picture is drawn, else NULL */
};

static struct RastPort *target_rp(struct LkTarget *t)
{
    return t->bm ? &t->rp : t->g->rp;
}

static void touched(struct LkTarget *t, long x, long y, long w, long h)
{
    if (!t->bm) {
        gfx_dirty();
        return;
    }
    if (t->x0 > t->x1) {
        t->x0 = x;
        t->y0 = y;
        t->x1 = x + w - 1;
        t->y1 = y + h - 1;
        return;
    }
    if (x < t->x0)
        t->x0 = x;
    if (y < t->y0)
        t->y0 = y;
    if (x + w - 1 > t->x1)
        t->x1 = x + w - 1;
    if (y + h - 1 > t->y1)
        t->y1 = y + h - 1;
}

static void rtg_fill(void *ctx, long x, long y, long w, long h, unsigned long rgb)
{
    struct LkTarget *t = ctx;
    touched(t, x, y, w, h);
    FillPixelArray(target_rp(t), (UWORD)x, (UWORD)(y - t->oy), (UWORD)w, (UWORD)h, (ULONG)rgb);
}

static void rtg_rows(void *ctx, long x, long y, long w, long h, const unsigned long *px, long mod)
{
    struct LkTarget *t = ctx;
    touched(t, x, y, w, h);
    WritePixelArray((APTR)px, 0, 0, (UWORD)(mod * 4), target_rp(t), (UWORD)x, (UWORD)(y - t->oy), (UWORD)w,
                    (UWORD)h, RECTFMT_ARGB);
}

/* A block moved (a layout's wall gliding): the card's blitter, the canvas
 * to itself (BltBitMap minds the overlap), or the screen to itself */
static int rtg_copy(void *ctx, long x, long y, long w, long h, long dx, long dy)
{
    struct LkTarget *t = ctx;
    if (t->bm) {
        BltBitMap(t->bm, (WORD)x, (WORD)(y - t->oy), t->bm, (WORD)(x + dx), (WORD)(y + dy - t->oy), (WORD)w,
                  (WORD)h, 0xC0, 0xFF, NULL);
        touched(t, x + dx, y + dy, w, h);
    } else {
        ClipBlit(t->g->rp, (WORD)x, (WORD)y, t->g->rp, (WORD)(x + dx), (WORD)(y + dy), (WORD)w, (WORD)h, 0xC0);
        gfx_dirty();
    }
    return 1;
}


/* ---- an 8-bit screen (an experiment: AGA, or a card's 8-bit mode). The
 * layout draws in true colour as ever; each pixel is made a pen as it is
 * drawn - never dithered. Pens 0-19 are kept (the classic's own, the
 * pointer's at 17-19); 20-27 are 8 greys; 28-39 twelve steps of the
 * layout's background gradient (lk_ramp: its rows drawn with them,
 * blended only between neighbours, so it looks smooth); 40-255 a pool
 * made the colours asked for as they are first drawn (a tile's, a
 * shade's, a picture's cut down to a few). A pen of the pool is made
 * another colour only once nothing on screen shows it (ck_gc, looking
 * through the canvas as shown): the palette never changes under what is
 * on screen, so nothing flashes. Short of pens: the nearest there is.
 * The pens go to a canvas a byte a pixel, shown with WriteChunkyPixels. */
#define CK_GREY0 20
#define CK_RAMP0 28
#define CK_RAMPN 12
#define CK_POOL0 40
#define CK_POOLN 216                /* (40-255) */
#define CK_TOL   (9 * 7 * 7)        /* as near as this (about 7 a channel): the same pen */
static const UBYTE ck_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

#define CK555(c) ((((c) >> 9) & 0x7C00) | (((c) >> 6) & 0x03E0) | (((c) >> 3) & 0x001F))
#define CK444(c) ((((c) >> 12) & 0xF00) | (((c) >> 8) & 0x0F0) | (((c) >> 4) & 0x00F))

/* The pens looked through for the nearest: the greys, the ramp's (once it
 * is made), the pool's made a colour */
static void ck_lives(struct LkTarget *t)
{
    int k, n = 0;
    for (k = CK_GREY0; k < CK_RAMP0; k++)
        t->lives[n++] = (UBYTE)k;
    if (t->rowc)
        for (k = CK_RAMP0; k < CK_RAMP0 + CK_RAMPN; k++)
            t->lives[n++] = (UBYTE)k;
    for (k = CK_POOL0; k < 256; k++)
        if (t->live[k])
            t->lives[n++] = (UBYTE)k;
    t->nlive = n;
}

/* The pool's pens nothing on screen shows freed (the canvas is what is on
 * screen: at a whole draw's start, or just shown), and what was looked up
 * forgotten */
static void ck_gc(struct LkTarget *t)
{
    UBYTE seen[256];
    const UBYTE *p = t->ch, *e = t->ch + t->cw * t->chh;
    int k;
    memset(seen, 0, sizeof(seen));
    while (p + 8 <= e) {
        seen[p[0]] = 1; seen[p[1]] = 1; seen[p[2]] = 1; seen[p[3]] = 1;
        seen[p[4]] = 1; seen[p[5]] = 1; seen[p[6]] = 1; seen[p[7]] = 1;
        p += 8;
    }
    while (p < e)
        seen[*p++] = 1;
    t->nfree = 0;
    for (k = CK_POOL0; k < 256; k++) {
        t->live[k] = seen[k];
        if (!seen[k])
            t->nfree++;
    }
    memset(t->pool, 0, 32768);
    memset(t->near, 0, 4096);
    t->asked = FALSE;
    ck_lives(t);
}

/* The nearest pen there is to rgb, and how near (weighted) */
static UBYTE ck_search(const struct LkTarget *t, unsigned long rgb, LONG *dist)
{
    LONG best = 0x7FFFFFFF, d;
    int i, r = (rgb >> 16) & 255, gg = (rgb >> 8) & 255, b = rgb & 255;
    UBYTE pen = CK_GREY0;
    for (i = 0; i < t->nlive; i++) {
        ULONG c = t->rgbof[t->lives[i]];
        LONG dr = r - (LONG)((c >> 16) & 255), dg = gg - (LONG)((c >> 8) & 255), db = b - (LONG)(c & 255);
        d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < best) {
            best = d;
            pen = t->lives[i];
            if (!d)
                break;
        }
    }
    if (dist)
        *dist = best;
    return pen;
}

/* A free pen of the pool made rgb (0: none free) */
static UBYTE ck_new(struct LkTarget *t, unsigned long rgb)
{
    int n;
    UBYTE pen;
    t->asked = TRUE;
    if (t->nfree <= 0)
        return 0;
    for (n = 0; n < CK_POOLN; n++) {
        pen = (UBYTE)(CK_POOL0 + (t->hunt + n) % CK_POOLN);
        if (!t->live[pen])
            break;
    }
    if (n == CK_POOLN)
        return 0;
    t->hunt = (pen - CK_POOL0 + 1) % CK_POOLN;
    t->live[pen] = 1;
    t->nfree--;
    t->lives[t->nlive++] = pen;
    t->rgbof[pen] = rgb & 0xFFFFFF;
    SetRGB32(&t->g->scr->ViewPort, pen, ((rgb >> 16) & 255) * 0x01010101UL,
             ((rgb >> 8) & 255) * 0x01010101UL, (rgb & 255) * 0x01010101UL);
    return pen;
}

/* The pen for a colour (not the background's): one as near as makes no
 * difference, or one made it while there are pens (not for a picture of
 * many colours drawn with what there is: no_new), else the nearest */
static UBYTE ck_pen_for(struct LkTarget *t, unsigned long rgb)
{
    UWORD k = (UWORD)CK555(rgb);
    UBYTE pen, made;
    LONG d;
    if ((pen = t->pool[k]))
        return pen;
    if (t->no_new) {
        UWORD n = (UWORD)CK444(rgb);
        if (!t->near[n])
            t->near[n] = ck_search(t, rgb, NULL);
        return t->near[n];
    }
    pen = ck_search(t, rgb, &d);
    if (d > CK_TOL && (made = ck_new(t, rgb)))
        pen = made;
    t->pool[k] = pen;
    return pen;
}

static BOOL ck_is_bg(const struct LkTarget *t, long y, unsigned long c)
{
    ULONG b;
    int dr, dg, db;
    if (!t->rowc)
        return FALSE;
    b = t->rowc[y];
    dr = (int)((c >> 16) & 255) - (int)((b >> 16) & 255);
    dg = (int)((c >> 8) & 255) - (int)((b >> 8) & 255);
    db = (int)(c & 255) - (int)(b & 255);
    return dr >= -2 && dr <= 2 && dg >= -2 && dg <= 2 && db >= -2 && db <= 2;
}

/* Row y's background colour, drawn with the ramp: its pen at x, y */
static UBYTE ck_ramp_pen(const struct LkTarget *t, long x, long y)
{
    long pos = t->chh > 1 ? y * (CK_RAMPN - 1) * 16 / (t->chh - 1) : 0, i = pos >> 4;
    if (i >= CK_RAMPN - 1)
        return CK_RAMP0 + CK_RAMPN - 1;
    return (UBYTE)(CK_RAMP0 + i + (ck_bayer[y & 3][x & 3] < (pos & 15) ? 1 : 0));
}

static void ck_touch(struct LkTarget *t, long x, long y, long w, long h)
{
    if (t->x0 > t->x1) {
        t->x0 = x;
        t->y0 = y;
        t->x1 = x + w - 1;
        t->y1 = y + h - 1;
        return;
    }
    if (x < t->x0)
        t->x0 = x;
    if (y < t->y0)
        t->y0 = y;
    if (x + w - 1 > t->x1)
        t->x1 = x + w - 1;
    if (y + h - 1 > t->y1)
        t->y1 = y + h - 1;
}

static void ck_fill(void *ctx, long x, long y, long w, long h, unsigned long rgb)
{
    struct LkTarget *t = ctx;
    UBYTE pen = 0;
    long i, j;
    ck_touch(t, x, y, w, h);
    for (j = 0; j < h; j++) {
        UBYTE *d = t->ch + (y + j) * t->cw + x;
        if (ck_is_bg(t, y + j, rgb)) {
            for (i = 0; i < w; i++)     /* the background's own colour there: the ramp */
                d[i] = ck_ramp_pen(t, x + i, y + j);
            continue;
        }
        if (!pen)
            pen = ck_pen_for(t, rgb);
        memset(d, pen, (size_t)w);      /* solid: never dithered */
    }
}

static void ck_rows(void *ctx, long x, long y, long w, long h, const unsigned long *px, long mod)
{
    struct LkTarget *t = ctx;
    long i, j;
    ck_touch(t, x, y, w, h);
    for (j = 0; j < h; j++) {
        UBYTE *d = t->ch + (y + j) * t->cw + x;
        const unsigned long *s = px + j * mod;
        unsigned long last = 0xFFFFFFFFUL;
        UBYTE lp = 0;
        for (i = 0; i < w; i++) {
            if (ck_is_bg(t, y + j, s[i])) {
                d[i] = ck_ramp_pen(t, x + i, y + j);
                continue;
            }
            if (t->pmap) {              /* a picture's own colours */
                d[i] = t->pmap[CK555(s[i])];
                continue;
            }
            if (s[i] != last) {         /* (runs of a colour: looked up once) */
                last = s[i];
                lp = ck_pen_for(t, s[i]);
            }
            d[i] = lp;
        }
    }
}

static int ck_copy(void *ctx, long x, long y, long w, long h, long dx, long dy)
{
    struct LkTarget *t = ctx;
    long j;
    if (dy > 0) {                       /* (rows from the bottom: they overlap) */
        for (j = h - 1; j >= 0; j--)
            memmove(t->ch + (y + j + dy) * t->cw + x + dx, t->ch + (y + j) * t->cw + x, (size_t)w);
    } else {
        for (j = 0; j < h; j++)
            memmove(t->ch + (y + j + dy) * t->cw + x + dx, t->ch + (y + j) * t->cw + x, (size_t)w);
    }
    ck_touch(t, x + dx, y + dy, w, h);
    return 1;
}

BOOL lk_open_chunky(struct Gfx *g, struct LkOut *o)
{
    struct LkTarget *t;
    struct ViewPort *vp;
    ULONG *pal;
    int i;
    o->buf = NULL;
    o->ctx = NULL;
    if (!g->scr || !g->rp || g->truecolor || g->depth != 8 || g->wb || GfxBase->LibNode.lib_Version < 40)
        return FALSE;
    if (!(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    t->g = g;
    t->x0 = 1;
    t->x1 = 0;
    t->cw = g->w;
    t->chh = g->h;
    t->ch = AllocVec((ULONG)g->w * g->h, MEMF_ANY | MEMF_CLEAR);
    t->pal_was = AllocVec((1 + 256 * 3 + 1) * 4, MEMF_ANY);
    t->pool = AllocVec(32768, MEMF_ANY | MEMF_CLEAR);
    t->near = AllocVec(4096, MEMF_ANY | MEMF_CLEAR);
    t->hist = AllocVec(32768 * sizeof(ULONG), MEMF_ANY);
    t->list = AllocVec(32768 * sizeof(UWORD), MEMF_ANY);
    t->pmapb = AllocVec(32768, MEMF_ANY | MEMF_CLEAR);
    pal = AllocVec((1 + 256 * 3 + 1) * 4, MEMF_ANY);
    o->ctx = t;
    if (!t->ch || !t->pal_was || !pal || !t->pool || !t->near || !t->hist || !t->list || !t->pmapb) {
        if (pal)
            FreeVec(pal);
        lk_close(o);
        return FALSE;
    }
    /* the colours as they were (put back at lk_close), then the layout's */
    vp = &g->scr->ViewPort;
    t->pal_was[0] = 256UL << 16;
    GetRGB32(vp->ColorMap, 0, 256, t->pal_was + 1);
    t->pal_was[1 + 256 * 3] = 0;
    memcpy(pal, t->pal_was, (1 + 256 * 3 + 1) * 4);
    for (i = 0; i < 8; i++) {
        ULONG v = (ULONG)(i * 255 / 7);
        pal[1 + (CK_GREY0 + i) * 3] = pal[2 + (CK_GREY0 + i) * 3] = pal[3 + (CK_GREY0 + i) * 3] =
            v * 0x01010101UL;
        t->rgbof[CK_GREY0 + i] = (v << 16) | (v << 8) | v;
    }
    t->nfree = CK_POOLN;
    ck_lives(t);
    LoadRGB32(vp, pal);
    FreeVec(pal);
    o->fill = ck_fill;
    o->rows = ck_rows;
    o->copy = ck_copy;
    o->bits = 24;                       /* (made pens here, as each pixel is drawn) */
    o->cov_steps = 4;                   /* text's edges in 4 steps: few pens, none dithered */
    o->bufn = (long)g->w * 16;
    if (!(o->buf = AllocVec((ULONG)o->bufn * 4, MEMF_ANY))) {
        lk_close(o);
        return FALSE;
    }
    lk_clip(o, 0, 0, g->w, g->h);
    return TRUE;
}

/* The layout's background: a gradient over the whole screen, top to
 * bottom (an 8-bit screen's: its twelve ramp pens made those colours, and
 * the background's rows drawn with them) */
void lk_ramp(struct LkOut *o, unsigned long top, unsigned long bottom)
{
    struct LkTarget *t = o->ctx;
    ULONG pal[1 + CK_RAMPN * 3 + 1];
    long y, i;
    if (!t || !t->ch || (t->rowc && t->ramp_a == top && t->ramp_b == bottom))
        return;
    if (!t->rowc && !(t->rowc = AllocVec((ULONG)t->chh * sizeof(ULONG), MEMF_ANY)))
        return;
    t->ramp_a = top;
    t->ramp_b = bottom;
    for (y = 0; y < t->chh; y++)
        t->rowc[y] = lk_vgrad_at(top, bottom, t->chh, y) & 0xFFFFFF;
    pal[0] = ((ULONG)CK_RAMPN << 16) | CK_RAMP0;
    for (i = 0; i < CK_RAMPN; i++) {
        unsigned long c = lk_vgrad_at(top, bottom, CK_RAMPN, i);
        pal[1 + i * 3] = ((c >> 16) & 255) * 0x01010101UL;
        pal[2 + i * 3] = ((c >> 8) & 255) * 0x01010101UL;
        pal[3 + i * 3] = (c & 255) * 0x01010101UL;
        t->rgbof[CK_RAMP0 + i] = c & 0xFFFFFF;
    }
    pal[1 + CK_RAMPN * 3] = 0;
    LoadRGB32(&t->g->scr->ViewPort, pal);
    memset(t->pool, 0, 32768);          /* (nearest looked up again, the ramp among them) */
    memset(t->near, 0, 4096);
    ck_lives(t);
}

/* A whole draw begins (everything drawn again): on an 8-bit screen, the
 * pens nothing on screen shows freed for it */
void lk_fresh(struct LkOut *o)
{
    struct LkTarget *t = o->ctx;
    if (t && t->ch && t->pool)
        ck_gc(t);
}

/* A picture on an 8-bit screen: its colours cut down to as many as there
 * are pens for (96 a big one, 32 a small one; median cut, by 5-5-5 colour),
 * each a pen made exactly its colour, every pixel the nearest of them -
 * nothing dithered. A picture of many colours when no pens are left (a
 * reflection, a wall's later covers): the nearest pens there are. */
static int ck_axis;
static int ck_cmp(const void *a, const void *b)
{
    UWORD ka = *(const UWORD *)a, kb = *(const UWORD *)b;
    int va = ck_axis == 0 ? ka >> 10 : ck_axis == 1 ? (ka >> 5) & 31 : ka & 31;
    int vb = ck_axis == 0 ? kb >> 10 : ck_axis == 1 ? (kb >> 5) & 31 : kb & 31;
    return va - vb;
}

struct CkBox {
    long at, n;                 /* in t->list */
    ULONG pop;
    int range, axis;
};

static void ck_box_measure(const struct LkTarget *t, struct CkBox *b)
{
    int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 }, a;
    long i;
    b->pop = 0;
    for (i = 0; i < b->n; i++) {
        UWORD k = t->list[b->at + i];
        int v[3];
        v[0] = k >> 10;
        v[1] = (k >> 5) & 31;
        v[2] = k & 31;
        for (a = 0; a < 3; a++) {
            if (v[a] < lo[a]) lo[a] = v[a];
            if (v[a] > hi[a]) hi[a] = v[a];
        }
        b->pop += t->hist[k];
    }
    b->axis = 0;
    b->range = hi[0] - lo[0];
    for (a = 1; a < 3; a++)
        if (hi[a] - lo[a] > b->range) {
            b->range = hi[a] - lo[a];
            b->axis = a;
        }
}

void lk_picture(struct LkOut *o, long x, long y, const unsigned long *px, long w, long h, long mod)
{
    struct LkTarget *t = o->ctx;
    static struct CkBox box[128];
    long i, j, n = 0, want, nb;
    if (!t || !t->ch || !t->hist) {
        lk_image(o, x, y, px, w, h, mod);
        return;
    }
    /* how many colours: a big picture (Now playing's cover, a page's) up to
     * 96, a small one (a wall's) 24 or 16, while pens are free, leaving some
     * for what is drawn after; a strip (a reflection, faded into the
     * background) none of its own */
    if (w * h >= 40000)
        want = t->nfree - 24 < 96 ? t->nfree - 24 : 96;
    else
        want = t->nfree >= 96 ? 24 : t->nfree >= 40 ? 16 : t->nfree - 24;
    if (w >= 3 * h)
        want = 0;
    if (want < 6) {
        t->no_new = TRUE;               /* (no pens to spare: the nearest) */
        lk_image(o, x, y, px, w, h, mod);
        t->no_new = FALSE;
        return;
    }
    memset(t->hist, 0, 32768 * sizeof(ULONG));
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            ULONG k = CK555(px[j * mod + i]);
            if (!t->hist[k]++)
                t->list[n++] = (UWORD)k;
        }
    if (n <= want) {                    /* few colours (a game's screenshot): each its own */
        lk_image(o, x, y, px, w, h, mod);
        return;
    }
    box[0].at = 0;
    box[0].n = n;
    ck_box_measure(t, &box[0]);
    for (nb = 1; nb < want; nb++) {
        long bi = -1, k, acc, half;
        ULONG bestv = 0;
        for (k = 0; k < nb; k++)        /* the box to cut: the most pixels over the widest span */
            if (box[k].n > 1 && box[k].range > 0 && (ULONG)box[k].range * box[k].pop >= bestv) {
                bestv = (ULONG)box[k].range * box[k].pop;
                bi = k;
            }
        if (bi < 0)
            break;
        ck_axis = box[bi].axis;
        qsort(t->list + box[bi].at, (size_t)box[bi].n, sizeof(UWORD), ck_cmp);
        half = box[bi].pop / 2;
        for (k = 0, acc = 0; k < box[bi].n - 1; k++) {
            acc += t->hist[t->list[box[bi].at + k]];
            if (acc >= half)
                break;
        }
        box[nb].at = box[bi].at + k + 1;
        box[nb].n = box[bi].n - k - 1;
        box[bi].n = k + 1;
        ck_box_measure(t, &box[bi]);
        ck_box_measure(t, &box[nb]);
    }
    for (j = 0; j < nb; j++) {          /* each box: its pixels' average, a pen */
        ULONG sr = 0, sg = 0, sb = 0, pop = 0;
        UBYTE pen;
        for (i = 0; i < box[j].n; i++) {
            UWORD k = t->list[box[j].at + i];
            ULONG c = t->hist[k];
            sr += ((k >> 10) * 8 + 4) * c;
            sg += (((k >> 5) & 31) * 8 + 4) * c;
            sb += ((k & 31) * 8 + 4) * c;
            pop += c;
        }
        if (!pop)
            pop = 1;
        pen = ck_pen_for(t, ((sr / pop) << 16) | ((sg / pop) << 8) | (sb / pop));
        for (i = 0; i < box[j].n; i++)
            t->pmapb[t->list[box[j].at + i]] = pen;
    }
    t->pmap = t->pmapb;
    lk_image(o, x, y, px, w, h, mod);
    t->pmap = NULL;
}

BOOL lk_open(struct Gfx *g, struct LkOut *o, BOOL canvas)
{
    struct BitMap *bm = g->rp ? g->rp->BitMap : NULL;
    struct LkTarget *t;
    ULONG bits;
    o->buf = NULL;
    o->ctx = NULL;
    if (!CyberGfxBase || !bm || g->planar || !g->truecolor || !GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX))
        return FALSE;
    bits = GetCyberMapAttr(bm, CYBRMATTR_DEPTH);
    if (bits < 15 || bits > 32)
        return FALSE;
    if (!(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    t->g = g;
    t->x0 = 1;
    t->x1 = 0;
    if (canvas && (t->bm = AllocBitMap(g->w, g->h, GetBitMapAttr(bm, BMA_DEPTH), BMF_MINPLANES, bm))) {
        if (!GetCyberMapAttr(t->bm, CYBRMATTR_ISCYBERGFX)) {
            FreeBitMap(t->bm);   /* not on the card: no faster than the screen */
            t->bm = NULL;
        } else {
            InitRastPort(&t->rp);
            t->rp.BitMap = t->bm;
            t->own = TRUE;
        }
    }
    o->ctx = t;
    o->fill = rtg_fill;
    o->rows = rtg_rows;
    o->copy = rtg_copy;
    o->bits = (int)bits;
    o->cov_steps = 0;
    o->bufn = (long)g->w * 16;
    if (!(o->buf = AllocVec((ULONG)o->bufn * 4, MEMF_ANY))) {
        lk_close(o);
        return FALSE;
    }
    lk_clip(o, 0, 0, g->w, g->h);
    return TRUE;
}

/* An output onto bm, a strip of the screen (a friend of its bitmap, w x
 * h): drawn at the screen's rows y0..y0+h-1, it lands in bm's 0..h-1, the
 * clip kept inside it. bm stays the caller's. */
BOOL lk_open_strip(struct Gfx *g, struct LkOut *o, struct BitMap *bm, WORD y0, WORD w, WORD h)
{
    struct BitMap *sb = g->rp ? g->rp->BitMap : NULL;
    struct LkTarget *t;
    ULONG bits;
    o->buf = NULL;
    o->ctx = NULL;
    if (!CyberGfxBase || !bm || !sb || g->planar || !g->truecolor || !GetCyberMapAttr(sb, CYBRMATTR_ISCYBERGFX))
        return FALSE;
    bits = GetCyberMapAttr(sb, CYBRMATTR_DEPTH);
    if (bits < 15 || bits > 32)
        return FALSE;
    if (!(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    t->g = g;
    t->x0 = 1;
    t->x1 = 0;
    t->bm = bm;
    t->oy = y0;
    InitRastPort(&t->rp);
    t->rp.BitMap = bm;
    o->ctx = t;
    o->fill = rtg_fill;
    o->rows = rtg_rows;
    o->copy = rtg_copy;
    o->bits = (int)bits;
    o->cov_steps = 0;
    o->bufn = (long)w * 16;
    if (!(o->buf = AllocVec((ULONG)o->bufn * 4, MEMF_ANY))) {
        lk_close(o);
        return FALSE;
    }
    lk_clip(o, 0, y0, w, y0 + h);
    return TRUE;
}

void lk_show(struct LkOut *o)
{
    struct LkTarget *t = o->ctx;
    if (t && t->ch) {
        if (t->x0 > t->x1)
            return;
        if (t->x0 < 0) t->x0 = 0;
        if (t->y0 < 0) t->y0 = 0;
        if (t->x1 >= t->cw) t->x1 = t->cw - 1;
        if (t->y1 >= t->chh) t->y1 = t->chh - 1;
        gfx_dirty();
        WriteChunkyPixels(t->g->rp, (ULONG)t->x0, (ULONG)t->y0, (ULONG)t->x1, (ULONG)t->y1,
                          t->ch + t->y0 * t->cw + t->x0, t->cw);
        t->x0 = 1;
        t->x1 = 0;
        if (t->asked && t->nfree < 64)  /* (the canvas is what is on screen now: the pens
                                         * nothing shows freed) */
            ck_gc(t);
        return;
    }
    if (!t || !t->bm || !t->own || t->x0 > t->x1)
        return;
    gfx_dirty();
    BltBitMapRastPort(t->bm, (WORD)t->x0, (WORD)t->y0, t->g->rp, (WORD)t->x0, (WORD)t->y0,
                      (WORD)(t->x1 - t->x0 + 1), (WORD)(t->y1 - t->y0 + 1), 0xC0);
    t->x0 = 1;
    t->x1 = 0;
}

BOOL lk_canvas(const struct LkOut *o)
{
    const struct LkTarget *t = o->ctx;
    return t && (t->bm || t->ch);
}

void lk_close(struct LkOut *o)
{
    struct LkTarget *t = o->ctx;
    if (t) {
        if (t->bm && t->own) {
            WaitBlit();
            FreeBitMap(t->bm);
        }
        if (t->ch)
            FreeVec(t->ch);
        if (t->rowc)
            FreeVec(t->rowc);
        if (t->pool)
            FreeVec(t->pool);
        if (t->near)
            FreeVec(t->near);
        if (t->hist)
            FreeVec(t->hist);
        if (t->list)
            FreeVec(t->list);
        if (t->pmapb)
            FreeVec(t->pmapb);
        if (t->pal_was) {
            if (t->g->scr)              /* (the classic's colours back) */
                LoadRGB32(&t->g->scr->ViewPort, t->pal_was);
            FreeVec(t->pal_was);
        }
        FreeVec(t);
    }
    o->ctx = NULL;
    if (o->buf)
        FreeVec(o->buf);
    o->buf = NULL;
    o->bufn = 0;
}

/* ---- text: the smooth font's, and Japanese, Chinese and Korean
 * characters (cjk.c's three bytes) from the glyph packs, which are pixel
 * fonts: each a whole number of times larger as the face's line is
 * (a title's twice where its line is), set on the line's middle */
static WORD cjk_scale(struct Gfx *g, int face, UWORD ch)
{
    UWORD cell = face == SM_TITLE ? sm_title_h(g->sm) : face == SM_TINY ? sm_tiny_h(g->sm) : sm_cell_h(g->sm);
    WORD k = ch ? (WORD)((cell + ch / 2) / ch) : 1;
    return k < 1 ? 1 : k;
}

/* A character's advance at this face ("?"'s if no pack has it) */
static WORD cjk_adv(struct Gfx *g, ULONG cp, int face)
{
    UWORD cw, ch, asc, adv;
    if (cjk_glyph(cp, &cw, &ch, &asc, &adv))
        return (WORD)(adv * cjk_scale(g, face, ch));
    return sm_textlen(g->sm, "?", 1, face);
}

/* Mixed text measured, or fitted in maxw (> 0: the bytes that fit, whole
 * characters) */
static LONG mixed(struct Gfx *g, const char *s, LONG n, int face, WORD maxw, LONG *fit)
{
    const UBYTE *p = (const UBYTE *)s;
    LONG i = 0, w = 0;
    ULONG cp;
    while (i < n) {
        LONG r = (LONG)cjk_latin_run(g, s + i, (ULONG)(n - i));
        int k;
        if (r) {
            LONG rw = sm_textlen(g->sm, s + i, r, face);
            if (maxw > 0 && w + rw > maxw) {
                *fit = i + sm_fit(g->sm, s + i, r, face, (WORD)(maxw - w));
                return w;
            }
            w += rw;
            i += r;
        }
        if (i >= n)
            break;
        if ((k = cjk_dec(p + i, (ULONG)(n - i), &cp))) {
            WORD cw = cjk_adv(g, cp, face);
            if (maxw > 0 && w + cw > maxw) {
                *fit = i;
                return w;
            }
            w += cw;
            i += k;
        } else {
            i++;                        /* cut short: nothing */
        }
    }
    *fit = n;
    return w;
}

WORD lk_textlen(struct Gfx *g, const char *s, LONG n, int face)
{
    LONG fit;
    if (!g->sm || n <= 0)
        return 0;
    if (!cjk_any(s, (ULONG)n))
        return sm_textlen(g->sm, s, n, face);
    return (WORD)mixed(g, s, n, face, 0, &fit);
}

LONG lk_fit(struct Gfx *g, const char *s, LONG n, int face, WORD maxw)
{
    LONG fit;
    if (!g->sm || n <= 0)
        return 0;
    if (!cjk_any(s, (ULONG)n))
        return sm_fit(g->sm, s, n, face, maxw);
    mixed(g, s, n, face, maxw, &fit);
    return fit;
}

/* One character's glyph at x, the line's top y: 1-bit rows made coverage
 * (0 or 15), k times larger, over bg */
static WORD cjk_draw(struct Gfx *g, struct LkOut *o, WORD x, WORD y, ULONG cp, int face, ULONG fg,
                     const struct LkBg *bg)
{
    static UBYTE cov[64 * 64];
    UWORD cw, ch, asc, adv, cell, r, c;
    const UWORD *rows = cjk_glyph(cp, &cw, &ch, &asc, &adv);
    WORD k, w, h, top;
    if (!rows)
        return lk_text(g, o, x, y, "?", 1, face, fg, bg, 0);
    k = cjk_scale(g, face, ch);
    w = (WORD)(cw * k);
    h = (WORD)(ch * k);
    if ((LONG)w * h > (LONG)sizeof(cov))
        return (WORD)(adv * k);
    for (r = 0; r < ch; r++)
        for (c = 0; c < cw; c++) {
            UBYTE v = (rows[r] & (0x8000U >> c)) ? 15 : 0;
            WORD a, b;
            for (b = 0; b < k; b++)
                for (a = 0; a < k; a++)
                    cov[(r * k + b) * w + c * k + a] = v;
        }
    cell = face == SM_TITLE ? sm_title_h(g->sm) : face == SM_TINY ? sm_tiny_h(g->sm) : sm_cell_h(g->sm);
    top = y + ((WORD)cell - h) / 2;
    lk_cover(o, x, top, cov, w, h, w, fg, bg);
    return (WORD)(adv * k);
}

WORD lk_text(struct Gfx *g, struct LkOut *o, WORD x, WORD y, const char *s, LONG n, int face,
             ULONG fg, const struct LkBg *bg, WORD maxw)
{
    const UBYTE *cov;
    WORD dx, dy;
    UWORD w, h, mod;
    if (!g->sm || n <= 0)
        return 0;
    if (cjk_any(s, (ULONG)n)) {
        const UBYTE *p = (const UBYTE *)s;
        LONG i = 0;
        WORD x0 = x;
        ULONG cp;
        if (maxw > 0)
            n = lk_fit(g, s, n, face, maxw);
        while (i < n) {
            LONG r = (LONG)cjk_latin_run(g, s + i, (ULONG)(n - i));
            int k;
            if (r) {
                x += lk_text(g, o, x, y, s + i, r, face, fg, bg, 0);
                i += r;
            }
            if (i >= n)
                break;
            if ((k = cjk_dec(p + i, (ULONG)(n - i), &cp))) {
                x += cjk_draw(g, o, x, y, cp, face, fg, bg);
                i += k;
            } else {
                i++;
            }
        }
        return x - x0;
    }
    if (maxw > 0)
        n = sm_fit(g->sm, s, n, face, maxw);
    if (!(cov = sm_cover(g->sm, s, n, face, &dx, &dy, &w, &h, &mod)))
        return 0;
    lk_cover(o, x + dx, y + dy, cov, w, h, mod, fg, bg);
    return sm_textlen(g->sm, s, n, face);
}

WORD lk_icon(struct Gfx *g, struct LkOut *o, int icon, WORD x, WORD y, ULONG fg,
             const struct LkBg *bg)
{
    const UBYTE *cov;
    UWORD n, mod;
    if (!g->sm || !(cov = sm_icon_cover(g->sm, icon, &n, &mod)))
        return 0;
    lk_cover(o, x, y, cov, n, n, mod, fg, bg);
    return (WORD)n;
}

WORD lk_icon_px(struct Gfx *g)
{
    return g->sm ? (WORD)sm_icon_px(g->sm) : 0;
}
