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

extern struct Library *CyberGfxBase;

/* Where an output draws: the screen (rp NULL: the Gfx's RastPort of the
 * moment) or the canvas; what has changed on the canvas since lk_show. */
struct LkTarget {
    struct Gfx *g;
    struct BitMap *bm;
    struct RastPort rp;
    long x0, y0, x1, y1;        /* changed: x0 > x1 when nothing */
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
    FillPixelArray(target_rp(t), (UWORD)x, (UWORD)y, (UWORD)w, (UWORD)h, (ULONG)rgb);
}

static void rtg_rows(void *ctx, long x, long y, long w, long h, const unsigned long *px, long mod)
{
    struct LkTarget *t = ctx;
    touched(t, x, y, w, h);
    WritePixelArray((APTR)px, 0, 0, (UWORD)(mod * 4), target_rp(t), (UWORD)x, (UWORD)y, (UWORD)w,
                    (UWORD)h, RECTFMT_ARGB);
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
        }
    }
    o->ctx = t;
    o->fill = rtg_fill;
    o->rows = rtg_rows;
    o->bits = (int)bits;
    o->bufn = (long)g->w * 16;
    if (!(o->buf = AllocVec((ULONG)o->bufn * 4, MEMF_ANY))) {
        lk_close(o);
        return FALSE;
    }
    lk_clip(o, 0, 0, g->w, g->h);
    return TRUE;
}

void lk_show(struct LkOut *o)
{
    struct LkTarget *t = o->ctx;
    if (!t || !t->bm || t->x0 > t->x1)
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
    return t && t->bm;
}

void lk_close(struct LkOut *o)
{
    struct LkTarget *t = o->ctx;
    if (t) {
        if (t->bm) {
            WaitBlit();
            FreeBitMap(t->bm);
        }
        FreeVec(t);
    }
    o->ctx = NULL;
    if (o->buf)
        FreeVec(o->buf);
    o->buf = NULL;
    o->bufn = 0;
}

WORD lk_textlen(struct Gfx *g, const char *s, LONG n, int face)
{
    return g->sm ? sm_textlen(g->sm, s, n, face) : 0;
}

WORD lk_text(struct Gfx *g, struct LkOut *o, WORD x, WORD y, const char *s, LONG n, int face,
             ULONG fg, const struct LkBg *bg, WORD maxw)
{
    const UBYTE *cov;
    WORD dx, dy;
    UWORD w, h, mod;
    if (!g->sm || n <= 0)
        return 0;
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
