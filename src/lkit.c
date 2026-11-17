/*
 * AmigaMCE - the layouts' drawing kit on a graphics card's screen (lkit.h)
 * Copyright (c) 2026 Brian King
 */
#include "amce.h"
#include "lkit.h"

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

extern struct Library *CyberGfxBase;

static void rtg_fill(void *ctx, long x, long y, long w, long h, unsigned long rgb)
{
    struct Gfx *g = ctx;
    gfx_dirty();
    FillPixelArray(g->rp, (UWORD)x, (UWORD)y, (UWORD)w, (UWORD)h, (ULONG)rgb);
}

static void rtg_rows(void *ctx, long x, long y, long w, long h, const unsigned long *px, long mod)
{
    struct Gfx *g = ctx;
    gfx_dirty();
    WritePixelArray((APTR)px, 0, 0, (UWORD)(mod * 4), g->rp, (UWORD)x, (UWORD)y, (UWORD)w, (UWORD)h,
                    RECTFMT_ARGB);
}

BOOL lk_open(struct Gfx *g, struct LkOut *o)
{
    struct BitMap *bm = g->rp ? g->rp->BitMap : NULL;
    ULONG bits;
    if (!CyberGfxBase || !bm || g->planar || !g->truecolor || !GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX))
        return FALSE;
    bits = GetCyberMapAttr(bm, CYBRMATTR_DEPTH);
    if (bits < 15 || bits > 32)
        return FALSE;
    o->ctx = g;
    o->fill = rtg_fill;
    o->rows = rtg_rows;
    o->bits = (int)bits;
    o->bufn = (long)g->w * 16;
    if (!(o->buf = AllocVec((ULONG)o->bufn * 4, MEMF_ANY)))
        return FALSE;
    lk_clip(o, 0, 0, g->w, g->h);
    return TRUE;
}

void lk_close(struct LkOut *o)
{
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
