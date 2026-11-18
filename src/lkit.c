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

/* A block moved (a layout's wall gliding): the card's blitter, the canvas
 * to itself (BltBitMap minds the overlap), or the screen to itself */
static int rtg_copy(void *ctx, long x, long y, long w, long h, long dx, long dy)
{
    struct LkTarget *t = ctx;
    if (t->bm) {
        BltBitMap(t->bm, (WORD)x, (WORD)y, t->bm, (WORD)(x + dx), (WORD)(y + dy), (WORD)w, (WORD)h, 0xC0, 0xFF,
                  NULL);
        touched(t, x + dx, y + dy, w, h);
    } else {
        ClipBlit(t->g->rp, (WORD)x, (WORD)y, t->g->rp, (WORD)(x + dx), (WORD)(y + dy), (WORD)w, (WORD)h, 0xC0);
        gfx_dirty();
    }
    return 1;
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
    o->copy = rtg_copy;
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
