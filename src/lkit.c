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
 * drawn, ordered-dithered (4 x 4) to a palette of the layout's own: a
 * 6 x 6 x 6 cube at pens 40-255, 8 greys at 20-27, and at 28-39 twelve
 * steps of the layout's background gradient (lk_ramp: its rows are drawn
 * with them, dithered only between neighbours, so it looks smooth); 0-19
 * kept (the classic's own pens, the pointer's at 17-19). The pens go to a
 * canvas a byte a pixel, shown with WriteChunkyPixels. */
#define CK_GREY0 20
#define CK_RAMP0 28
#define CK_RAMPN 12
#define CK_CUBE0 40
static const UBYTE ck_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
static UBYTE ck_q6[16][256];         /* a value's level of 6, by the dither's step */
static UBYTE ck_q8[16][256];         /* ...of 8 (the greys) */
static BOOL ck_made;

static void ck_tables(void)
{
    int d, v;
    if (ck_made)
        return;
    for (d = 0; d < 16; d++)
        for (v = 0; v < 256; v++) {
            ck_q6[d][v] = (UBYTE)((v * 5 * 16 + d * 255 + 8) / (255 * 16));
            ck_q8[d][v] = (UBYTE)((v * 7 * 16 + d * 255 + 8) / (255 * 16));
        }
    ck_made = TRUE;
}

static UBYTE ck_pen(unsigned long rgb, int d)
{
    int r = (rgb >> 16) & 255, gg = (rgb >> 8) & 255, b = rgb & 255;
    int mx = r > gg ? (r > b ? r : b) : (gg > b ? gg : b), mn = r < gg ? (r < b ? r : b) : (gg < b ? gg : b);
    if (mx - mn <= 10)                  /* about grey: the finer ramp */
        return (UBYTE)(CK_GREY0 + ck_q8[d][(r * 77 + gg * 150 + b * 29) >> 8]);
    return (UBYTE)(CK_CUBE0 + ck_q6[d][r] * 36 + ck_q6[d][gg] * 6 + ck_q6[d][b]);
}

/* Is c row y's background colour (within a step or two: a gradient over
 * part of the screen, made from the whole one's colours, rounds a little
 * differently)? */
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
    UBYTE pat[4][4];
    long i, j;
    ck_touch(t, x, y, w, h);
    for (j = 0; j < 4; j++)             /* the colour's 4 x 4 pattern, once */
        for (i = 0; i < 4; i++)
            pat[j][i] = ck_pen(rgb, ck_bayer[j][i]);
    for (j = 0; j < h; j++) {
        UBYTE *d = t->ch + (y + j) * t->cw + x;
        const UBYTE *pr = pat[(y + j) & 3];
        if (ck_is_bg(t, y + j, rgb)) {
            for (i = 0; i < w; i++)     /* the background's own colour there: the ramp */
                d[i] = ck_ramp_pen(t, x + i, y + j);
            continue;
        }
        for (i = 0; i < w; i++)
            d[i] = pr[(x + i) & 3];
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
        const UBYTE *br = ck_bayer[(y + j) & 3];
        for (i = 0; i < w; i++)
            d[i] = ck_is_bg(t, y + j, s[i]) ? ck_ramp_pen(t, x + i, y + j) : ck_pen(s[i], br[(x + i) & 3]);
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
    pal = AllocVec((1 + 256 * 3 + 1) * 4, MEMF_ANY);
    o->ctx = t;
    if (!t->ch || !t->pal_was || !pal) {
        if (pal)
            FreeVec(pal);
        lk_close(o);
        return FALSE;
    }
    ck_tables();
    /* the colours as they were (put back at lk_close), then the layout's */
    vp = &g->scr->ViewPort;
    t->pal_was[0] = 256UL << 16;
    GetRGB32(vp->ColorMap, 0, 256, t->pal_was + 1);
    t->pal_was[1 + 256 * 3] = 0;
    memcpy(pal, t->pal_was, (1 + 256 * 3 + 1) * 4);
    for (i = 0; i < 8; i++) {
        ULONG v = (ULONG)(i * 255 / 7) * 0x01010101UL;
        pal[1 + (CK_GREY0 + i) * 3] = pal[2 + (CK_GREY0 + i) * 3] = pal[3 + (CK_GREY0 + i) * 3] = v;
    }
    for (i = 0; i < 216; i++) {
        int k = CK_CUBE0 + i;
        pal[1 + k * 3] = (ULONG)((i / 36) * 51) * 0x01010101UL;
        pal[2 + k * 3] = (ULONG)((i / 6 % 6) * 51) * 0x01010101UL;
        pal[3 + k * 3] = (ULONG)((i % 6) * 51) * 0x01010101UL;
    }
    LoadRGB32(vp, pal);
    FreeVec(pal);
    o->fill = ck_fill;
    o->rows = ck_rows;
    o->copy = ck_copy;
    o->bits = 24;                       /* (dithered here, as each pixel is made a pen) */
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
    }
    pal[1 + CK_RAMPN * 3] = 0;
    LoadRGB32(&t->g->scr->ViewPort, pal);
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
