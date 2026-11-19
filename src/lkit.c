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
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <graphics/copper.h>
#include <graphics/view.h>
#include <graphics/sprite.h>
#include <graphics/videocontrol.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct Library *CyberGfxBase;

/* Time spent (Debug's load line): showing an 8-bit canvas (WriteChunkyPixels),
 * cutting pictures' colours down, how many pixels shown, looks for free pens */
BOOL lk_copper_ok = TRUE;               /* (Ambiance/LayoutCopper: 0, the dithered ramp) */
BOOL lk_blit_ok = TRUE;                 /* (Ambiance/LayoutBlit: 0, a copy c2p'd again) */
static ULONG lkt_show, lkt_pic, lkt_px, lkt_gc, lkt_short, lkt_free = 999;
static ULONG lkt_blits, lkt_bpx;
static ULONG lkt_same;                  /* groups of 32 left as the planes had them (ck_c2p) */        /* copies the blitter made, pixels they moved */
void lk_blits(ULONG *n, ULONG *kpx)
{
    *n = lkt_blits;
    *kpx = lkt_bpx / 1000;
    lkt_blits = lkt_bpx = 0;
}
void lk_pens(ULONG *short_of, ULONG *least_free)
{
    *short_of = lkt_short;          /* pictures drawn with the nearest pens: none free */
    *least_free = lkt_free;         /* the fewest free pens a picture found */
    lkt_short = 0;
    lkt_free = 999;
}
void lk_timing(ULONG *show_ms, ULONG *pic_ms, ULONG *kpx, ULONG *gcs)
{
    *show_ms = lkt_show;
    *pic_ms = lkt_pic;
    *kpx = lkt_px / 1000;
    *gcs = lkt_gc;
    lkt_show = lkt_pic = lkt_px = lkt_gc = 0;
}
extern struct GfxBase *GfxBase;

/* Where the 8-bit drawing's time goes, by E-clock ticks: each kind's calls
 * and time (text: its glyphs made and drawn, its rows within it) */
enum { LKP_FILL, LKP_ROWS, LKP_TEXT, LKP_C2P, LKP_COPY, LKP_GLYPHS, LKP_GRAD, LKP_N };
static ULONG lkp_n[LKP_N], lkp_t[LKP_N], lkp_hz;
#define LKP_BEGIN ULONG lkp_t0 = time_ticks(&lkp_hz)
#define LKP_END(k) (lkp_n[k]++, lkp_t[k] += time_ticks(&lkp_hz) - lkp_t0)
void lk_prof(char *buf)
{
    static const char *const name[LKP_N] = { "fills", "rows", "text", "c2p", "copies", "glyphs", "gradients" };
    int k;
    *buf = 0;
    for (k = 0; k < LKP_N; k++) {
        if (!lkp_n[k])
            continue;
        sprintf(buf + strlen(buf), "%s%s %lu (%lu ms)", *buf ? ", " : "", name[k], (unsigned long)lkp_n[k],
                lkp_hz ? (unsigned long)(lkp_t[k] / (lkp_hz / 1000 ? lkp_hz / 1000 : 1)) : 0UL);
        lkp_n[k] = lkp_t[k] = 0;
    }
    if (lkt_same) {
        sprintf(buf + strlen(buf), "%sunchanged %lu K pixels not converted", *buf ? ", " : "",
                (unsigned long)(lkt_same * 32 / 1000));
        lkt_same = 0;
    }
}

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
    BOOL copper;                /* ...the background one pen (CK_RAMP0), its colour set by
                                 * the copper each line (ck_copper_on): smooth, and a row
                                 * copied up or down takes the shade of where it lands */
    struct { WORD x, y, w, h, dx, dy; } bq[8];  /* copies on the canvas still to be made on
                                         * the screen by the blitter (lk_show) */
    WORD nbq;
    BOOL blitting;              /* a copy by the blitter (ck_blit) maybe still under way:
                                 * WaitBlit before the CPU writes the planes */
    UBYTE *pool;                /* ...the pen for each 5-5-5 colour asked for (0: not yet) */
    UBYTE *near;                /* ...the nearest pen for each 4-4-4 colour (pictures of
                                 * many colours, drawn with the pens there are) */
    UBYTE live[256];            /* ...the pool's pens made a colour and maybe on screen */
    UBYTE lives[256];           /* ...those pens, and the greys and ramp's: what is looked
                                 * through for the nearest */
    int nlive, nfree, hunt;
    BOOL asked;                 /* ...pens made or wanted since ck_gc last looked */
    UWORD *tmp;                 /* ...a box's colours, sorted (lk_picture) */
    BOOL no_new;                /* a picture of many colours drawn (a photo, a reflection):
                                 * the nearest pens there are, none made for it */
    ULONG rgbof[256];           /* each pen's colour (greys, ramp, the pool's) */
    ULONG *hist;                /* a picture's: how many of each 5-5-5 colour (lk_picture) */
    UWORD *list;                /* ...the colours it has */
    UBYTE *pmapb;               /* ...each one's pen */
    UBYTE *pmap;                /* pmapb while a picture is drawn, else NULL */
    UBYTE *sh;                  /* what the screen's planes show, a byte a pixel (ck_c2p):
                                 * a group of 32 the same not converted again; NULL: none */
    WORD *rx0, *rx1;            /* ...each row's changed span (rx1 < rx0: none): rows
                                 * not drawn on not even compared */
    BOOL sh_all;                /* ...the next show converts all it shows (a whole draw:
                                 * the screen may have been drawn over by others) */
    UBYTE held[256];            /* pens kept from the pool: the sprites' colours (AGA) */
    /* the big letter by hardware sprites (lk_letter_sprites): over the
     * picture, never drawn into it - a glide under it moves nothing of it */
    struct ExtSprite *sp_sd[8];
    WORD sp_num[8], sp_n, sp_x, sp_y;
    const UBYTE *sp_of;
    ULONG sp_rgb;
    BOOL sp_based;              /* the sprites' colours moved to CK_SPBASE (sp_eb, sp_ob were) */
    ULONG sp_eb, sp_ob;
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
    if (dy)
        return 0;                       /* (up or down over the gradient: a row would carry
                                         * the shade of where it was) */
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
#define CK_SPRITE0 20              /* 20-31: sprites 2-7's colours (the big letter by them, AGA) */
#define CK_GREY0 32
#define CK_RAMP0 40
#define CK_RAMPN 12
#define CK_POOL0 52
#define CK_POOLN 204                /* (52-255) */
#define CK_TOL   (9 * 7 * 7)        /* as near as this (about 7 a channel): the same pen */
static const UBYTE ck_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* The background darkened (a cover's reflection of black, a shadow): pens
 * 29-32, the copper's too, each line's shade times these (256ths) */
#define CK_SHADES 4
static const UWORD ck_shade[CK_SHADES] = { 236, 210, 184, 158 };
#define CK555(c) ((((c) >> 9) & 0x7C00) | (((c) >> 6) & 0x03E0) | (((c) >> 3) & 0x001F))
#define CK444(c) ((((c) >> 12) & 0xF00) | (((c) >> 8) & 0x0F0) | (((c) >> 4) & 0x00F))

/* The pens looked through for the nearest: the greys, the ramp's (once it
 * is made), the pool's made a colour */
static void ck_lives(struct LkTarget *t)
{
    int k, n = 0;
    for (k = CK_GREY0; k < CK_RAMP0; k++)
        t->lives[n++] = (UBYTE)k;
    if (t->rowc && !t->copper)          /* (the copper's: its pen no one colour, and the ramp's
                                         * steps unused - a line's shade is not theirs) */
        for (k = CK_RAMP0; k < CK_RAMP0 + CK_RAMPN; k++)
            t->lives[n++] = (UBYTE)k;
    for (k = CK_POOL0; k < 256; k++)
        if (t->live[k] && !t->held[k])
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
    lkt_gc++;
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
        t->live[k] = seen[k] | t->held[k];
        if (!t->live[k])
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
    if (t->copper && t->no_new) {
        /* a picture faded into the background (a cover's reflection), drawn
         * with the pens there are: its faint part the background's own pen,
         * the copper's shade of each line - not a pen of one colour, which
         * showed as a block of the wrong shade */
        return dr >= -12 && dr <= 12 && dg >= -12 && dg <= 12 && db >= -12 && db <= 12;
    }
    return dr >= -2 && dr <= 2 && dg >= -2 && dg <= 2 && db >= -2 && db <= 2;
}

/* Row y's background colour, drawn with the ramp: its pen at x, y */
static UBYTE ck_ramp_pen(const struct LkTarget *t, long x, long y)
{
    long pos, i;
    if (t->copper)
        return CK_RAMP0;                /* (its colour the copper's, line by line) */
    pos = t->chh > 1 ? y * (CK_RAMPN - 1) * 16 / (t->chh - 1) : 0;
    i = pos >> 4;
    if (i >= CK_RAMPN - 1)
        return CK_RAMP0 + CK_RAMPN - 1;
    return (UBYTE)(CK_RAMP0 + i + (ck_bayer[y & 3][x & 3] < (pos & 15) ? 1 : 0));
}

/* The pen for a colour that is the background's at row y (the ramp's, or
 * the copper's), or the background darkened by one of the copper's shades
 * (CK_SHADES); 0: neither */
static UBYTE ck_bg_pen(const struct LkTarget *t, long x, long y, unsigned long c)
{
    ULONG b;
    LONG cs, bs, f, tol;
    int k;
    if (!t->rowc)
        return 0;
    if (ck_is_bg(t, y, c))
        return ck_ramp_pen(t, x, y);
    if (!t->copper)
        return 0;
    b = t->rowc[y];
    cs = (LONG)((c >> 16) & 255) + (LONG)((c >> 8) & 255) + (LONG)(c & 255);
    bs = (LONG)((b >> 16) & 255) + (LONG)((b >> 8) & 255) + (LONG)(b & 255);
    if (!bs || cs >= bs)
        return 0;
    f = cs * 256 / bs;                      /* how dark: its share of the background */
    tol = t->no_new ? 10 : 3;
    for (k = 0; k < CK_SHADES; k++)
        if (f >= ck_shade[k] - 13 && f < ck_shade[k] + 13) {
            ULONG v = ((((b >> 16) & 255) * ck_shade[k] >> 8) << 16) |
                      ((((b >> 8) & 255) * ck_shade[k] >> 8) << 8) | ((b & 255) * ck_shade[k] >> 8);
            LONG dr = (LONG)((c >> 16) & 255) - (LONG)((v >> 16) & 255),
                 dg = (LONG)((c >> 8) & 255) - (LONG)((v >> 8) & 255), db = (LONG)(c & 255) - (LONG)(v & 255);
            if (dr >= -tol && dr <= tol && dg >= -tol && dg <= tol && db >= -tol && db <= tol)
                return (UBYTE)(CK_RAMP0 + 1 + k);
            return 0;
        }
    return 0;
}

static void ck_touch(struct LkTarget *t, long x, long y, long w, long h)
{
    if (t->rx0 && w > 0 && h > 0) {
        long r, r1 = y + h, xe = x + w - 1;
        if (y < 0) y = 0;
        if (r1 > t->chh) r1 = t->chh;
        for (r = y; r < r1; r++) {
            if (x < t->rx0[r]) t->rx0[r] = (WORD)(x < 0 ? 0 : x);
            if (xe > t->rx1[r]) t->rx1[r] = (WORD)(xe >= t->cw ? t->cw - 1 : xe);
        }
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

static void ck_fill_(void *ctx, long x, long y, long w, long h, unsigned long rgb);
static void ck_fill(void *ctx, long x, long y, long w, long h, unsigned long rgb)
{
    /* (thousands a second: one in eight timed, eight times that counted -
     * the clock read each time cost more than many of them) */
    static UBYTE every;
    if (++every & 7) {
        ck_fill_(ctx, x, y, w, h, rgb);
        lkp_n[LKP_FILL]++;
        return;
    }
    {
        ULONG t0 = time_ticks(&lkp_hz);
        ck_fill_(ctx, x, y, w, h, rgb);
        lkp_n[LKP_FILL]++;
        lkp_t[LKP_FILL] += (time_ticks(&lkp_hz) - t0) * 8;
    }
}
static void ck_fill_(void *ctx, long x, long y, long w, long h, unsigned long rgb)
{
    struct LkTarget *t = ctx;
    UBYTE pen = 0;
    long i, j;
    ck_touch(t, x, y, w, h);
    for (j = 0; j < h; j++) {
        UBYTE *d = t->ch + (y + j) * t->cw + x;
        UBYTE bp = ck_bg_pen(t, x, y + j, rgb);
        if (bp) {
            /* the background's own colour there: the copper's (or its
             * shade), a pen the whole row; the ramp's, dithered in fours
             * (once a pixel, the background drawn again was most of a
             * glide's time) */
            if (t->copper) {
                memset(d, bp, (size_t)w);
            } else {
                UBYTE q[4];
                for (i = 0; i < 4; i++)
                    q[i] = ck_bg_pen(t, x + i, y + j, rgb);
                for (i = 0; i < w; i++)
                    d[i] = q[i & 3];
            }
            continue;
        }
        if (!pen)
            pen = ck_pen_for(t, rgb);
        memset(d, pen, (size_t)w);      /* solid: never dithered */
    }
}

static void ck_rows_(void *ctx, long x, long y, long w, long h, const unsigned long *px, long mod);
static void ck_rows(void *ctx, long x, long y, long w, long h, const unsigned long *px, long mod)
{
    LKP_BEGIN;
    ck_rows_(ctx, x, y, w, h, px, mod);
    LKP_END(LKP_ROWS);
}
static void ck_rows_(void *ctx, long x, long y, long w, long h, const unsigned long *px, long mod)
{
    struct LkTarget *t = ctx;
    long i, j;
    /* the host's own picture drawn with its own colours (a cover, a
     * screenshot): not looked at pixel by pixel for the background's
     * colour - that is a reflection's (faded into it), a layout's own */
    BOOL opaque = t->pmap && lk_pic_own;
    ck_touch(t, x, y, w, h);
    for (j = 0; j < h; j++) {
        UBYTE *d = t->ch + (y + j) * t->cw + x;
        const unsigned long *s = px + j * mod;
        unsigned long last = 0xFFFFFFFFUL, mc = 0xFFFFFFFFUL;
        UBYTE lp = 0, mp = 0;
        for (i = 0; i < w; i++) {
            UBYTE bp;
            /* (the background's or not, a colour's once a row: only the
             * ramp's dither differs along it) */
            if (opaque)
                bp = 0;                 /* (a cover's own colours: no background in it) */
            else if (s[i] == mc && (!mp || t->copper))
                bp = mp;
            else
                mc = s[i], bp = mp = ck_bg_pen(t, x + i, y + j, s[i]);
            if (bp) {
                d[i] = bp;
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

static int ck_copy_(void *ctx, long x, long y, long w, long h, long dx, long dy);
static BOOL ck_blit(struct LkTarget *t, long x, long y, long w, long h, long dx, long dy);
static int ck_copy(void *ctx, long x, long y, long w, long h, long dx, long dy)
{
    int r;
    LKP_BEGIN;
    r = ck_copy_(ctx, x, y, w, h, dx, dy);
    LKP_END(LKP_COPY);
    return r;
}
static int ck_copy_(void *ctx, long x, long y, long w, long h, long dx, long dy)
{
    struct LkTarget *t = ctx;
    long j;
    if (dy && !t->copper)
        return 0;                       /* (up or down over the dithered ramp: it would show) */
    if (ck_blit(t, x, y, w, h, dx, dy))
        return 1;                       /* (the screen's planes moved by the blitter too) */
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
    t->hist = AllocVec(32768 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);   /* (kept clear) */
    t->tmp = AllocVec(32768 * sizeof(UWORD), MEMF_ANY);
    t->list = AllocVec(32768 * sizeof(UWORD), MEMF_ANY);
    t->pmapb = AllocVec(32768, MEMF_ANY | MEMF_CLEAR);
    if (lk_skip_ok) {
        t->sh = AllocVec((ULONG)g->w * g->h, MEMF_ANY);     /* (none: everything converted) */
        if ((t->rx0 = AllocVec((ULONG)g->h * 2 * sizeof(WORD), MEMF_ANY))) {
            t->rx1 = t->rx0 + g->h;
            for (i = 0; i < g->h; i++) {
                t->rx0[i] = 0x7FFF;
                t->rx1[i] = -1;
            }
        }
    }
    t->sh_all = TRUE;
    pal = AllocVec((1 + 256 * 3 + 1) * 4, MEMF_ANY);
    o->ctx = t;
    if (!t->ch || !t->pal_was || !pal || !t->pool || !t->near || !t->hist || !t->list || !t->pmapb || !t->tmp) {
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
/* ---- the background's gradient by the copper (AGA): one pen (CK_RAMP0)
 * made each line's colour by a user copper list on the screen's ViewPort -
 * 24 bits (BPLCON3's LOCT: the high nibbles, then the low), every line.
 * No dither, and a row copied up or down (a list scrolled by a blit) takes
 * the shade of the line it lands on. BPLCON3's other bits are the
 * system's, read from the copper list it shows. */
#define CK_CUSTOM(off) ((APTR)(0xDFF000UL + (off)))
#define CK_BPLCON3 0x106
#define CK_COLOR(n) (0x180 + (n) * 2)

static BOOL ck_bplcon3(UWORD *base)
{
    struct View *v = GfxBase->ActiView;
    UWORD *c;
    LONG i;
    if (!v || !v->LOFCprList || !(c = v->LOFCprList->start))
        return FALSE;
    for (i = 0; i < v->LOFCprList->MaxCount && i < 8000; i++, c += 2) {
        if (c[0] == 0xFFFF && c[1] == 0xFFFE)
            break;
        if (c[0] == CK_BPLCON3) {
            *base = c[1] & (UWORD)~0xE200;  /* (its bank and LOCT are ours to set) */
            return TRUE;
        }
    }
    return FALSE;
}

/* A background colour and its copper shades (sc[0] the colour itself) */
static void ck_shades(ULONG c, ULONG *sc)
{
    int k;
    sc[0] = c & 0xFFFFFF;
    for (k = 0; k < CK_SHADES; k++)
        sc[1 + k] = ((((c >> 16) & 255) * ck_shade[k] >> 8) << 16) |
                    ((((c >> 8) & 255) * ck_shade[k] >> 8) << 8) | ((c & 255) * ck_shade[k] >> 8);
}

static void ck_copper_off(struct LkTarget *t)
{
    struct ViewPort *vp;
    if (!t->copper)
        return;
    t->copper = FALSE;
    if (!t->g->scr)
        return;
    vp = &t->g->scr->ViewPort;
    if (vp->UCopIns) {
        FreeVPortCopLists(vp);              /* (the user list with the rest: made again) */
        RemakeDisplay();
    }
}

static BOOL ck_copper_on(struct LkTarget *t)
{
    struct ViewPort *vp = &t->g->scr->ViewPort;
    struct UCopList *ucl;
    UWORD base;
    LONG lines, step, y;
    char m[96];
    if (!(GfxBase->ChipRevBits0 & GFXF_AA_ALICE) || !t->rowc || vp->UCopIns || !ck_bplcon3(&base))
        return FALSE;
    /* a user list's lines are the screen's (laced: MrgCop gives each field
     * its own half; taken as a field's, the gradient covered the top half
     * and stopped). Only what changed since the line before in the same
     * field is written: all thirteen moves a line ran into the picture, its
     * left part showing the high nibbles alone. The pens start at line 0's
     * colours (the ColorMap's, loaded before the screen's first line). */
    lines = t->chh;
    step = (vp->Modes & LACE) ? 2 : 1;
    if (!(ucl = AllocMem(sizeof(*ucl), MEMF_PUBLIC | MEMF_CLEAR)))
        return FALSE;
    if (!UCopperListInit(ucl, lines * (6 + 2 * (1 + CK_SHADES)) + 4)) {
        FreeMem(ucl, sizeof(*ucl));
        return FALSE;
    }
    {
        ULONG was[2][1 + CK_SHADES], sc[1 + CK_SHADES], pal[1 + 3 * (1 + CK_SHADES) + 1];
        int k, f;
        ck_shades(t->rowc[0], sc);
        pal[0] = ((ULONG)(1 + CK_SHADES) << 16) | CK_RAMP0;
        for (k = 0; k <= CK_SHADES; k++) {
            was[0][k] = was[1][k] = sc[k];
            pal[1 + k * 3] = ((sc[k] >> 16) & 255) * 0x01010101UL;
            pal[2 + k * 3] = ((sc[k] >> 8) & 255) * 0x01010101UL;
            pal[3 + k * 3] = (sc[k] & 255) * 0x01010101UL;
        }
        pal[1 + 3 * (1 + CK_SHADES)] = 0;
        LoadRGB32(vp, pal);
        for (y = 1; y < lines; y++) {
            UBYTE hi = 0, lo = 0;       /* (bits: the pens whose high / low nibbles changed) */
            UWORD cur = 0xFFFF;
            f = step == 2 ? (int)(y & 1) : 0;
            ck_shades(t->rowc[y], sc);
            for (k = 0; k <= CK_SHADES; k++) {
                if ((sc[k] & 0xF0F0F0) != (was[f][k] & 0xF0F0F0))
                    hi |= (UBYTE)(1 << k), lo |= (UBYTE)(1 << k);   /* (a high write sets both) */
                else if ((sc[k] & 0x0F0F0F) != (was[f][k] & 0x0F0F0F))
                    lo |= (UBYTE)(1 << k);
                was[f][k] = sc[k];
            }
            if (!lo)
                continue;
            CWait(ucl, y, 0);
            CBump(ucl);
            for (k = 0; k <= CK_SHADES; k++)
                if (hi & (1 << k)) {
                    UWORD p = (UWORD)(CK_RAMP0 + k), v = base | (UWORD)((p >> 5) << 13);
                    ULONG c = sc[k];
                    if (v != cur) {
                        CMove(ucl, CK_CUSTOM(CK_BPLCON3), v);
                        CBump(ucl);
                        cur = v;
                    }
                    CMove(ucl, CK_CUSTOM(CK_COLOR(p & 31)),
                          (UWORD)((((c >> 20) & 15) << 8) | (((c >> 12) & 15) << 4) | ((c >> 4) & 15)));
                    CBump(ucl);
                }
            for (k = 0; k <= CK_SHADES; k++)
                if (lo & (1 << k)) {
                    UWORD p = (UWORD)(CK_RAMP0 + k), v = base | 0x0200 | (UWORD)((p >> 5) << 13);
                    ULONG c = sc[k];
                    if (v != cur) {
                        CMove(ucl, CK_CUSTOM(CK_BPLCON3), v);
                        CBump(ucl);
                        cur = v;
                    }
                    CMove(ucl, CK_CUSTOM(CK_COLOR(p & 31)),
                          (UWORD)((((c >> 16) & 15) << 8) | (((c >> 8) & 15) << 4) | (c & 15)));
                    CBump(ucl);
                }
            CMove(ucl, CK_CUSTOM(CK_BPLCON3), base);    /* (the system's again) */
            CBump(ucl);
        }
    }
    CWait(ucl, 10000, 255);                 /* (CEND) */
    CBump(ucl);
    Forbid();
    vp->UCopIns = ucl;
    Permit();
    MakeScreen(t->g->scr);
    RethinkDisplay();
    t->copper = TRUE;
    sprintf(m, "the background by the copper: %ld lines, BPLCON3 %04lx", (long)lines, (unsigned long)base);
    amb_log("layout", m);
    return TRUE;
}

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
    ck_copper_off(t);                   /* (another gradient: its list made again) */
    if (lk_copper_ok)
        ck_copper_on(t);
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
    if (t && t->ch)
        t->sh_all = TRUE;
}

/* A picture on an 8-bit screen: its colours cut down to as many as there
 * are pens for (16 to 96, by its size; median cut, by 5-5-5
 * colour), each a pen made its colour, every pixel its box's pen -
 * nothing dithered. A picture of many colours when no pens are left (a
 * reflection, a wall's later covers): the nearest pens there are.
 *
 * What a cut made is kept (CK_CUTS of them, CK_CUT_BYTES at the most):
 * each colour of the picture and its pen. A picture drawn again (a wall's
 * glide brings a cover's strip in, a move draws its glow) is drawn with
 * them as long as its pens are still those colours - not cut again. */
#define CK_CUTS      48
#define CK_CUT_BYTES (240L * 1024)
struct CkCut {
    const unsigned long *px;    /* the picture (NULL: a free slot) */
    long w, h, mod;
    ULONG sum;                  /* (16 of its pixels: the same picture at the address) */
    ULONG used;                 /* (the clock: the oldest goes) */
    UWORD n;                    /* its colours */
    UWORD *key;                 /* ...each one's 5-5-5 bits */
    UBYTE *pen;                 /* ...and pen */
    UBYTE npens;
    UBYTE pens[96];             /* the pens it took, and their colours then: */
    ULONG prgb[96];             /* (still so: the cut still good) */
};
static struct CkCut ck_cut[CK_CUTS];
static ULONG ck_cut_clock, ck_cut_bytes;

static void ck_cut_drop(struct CkCut *c)
{
    if (c->key) {
        FreeVec(c->key);
        ck_cut_bytes -= (ULONG)c->n * 3;
    }
    memset(c, 0, sizeof(*c));
}

/* All cuts forgotten (the target closed) */
static void ck_cuts_free(void)
{
    int i;
    for (i = 0; i < CK_CUTS; i++)
        ck_cut_drop(&ck_cut[i]);
    ck_cut_bytes = 0;
}

static BOOL ck_cut_good(const struct LkTarget *t, const struct CkCut *c)
{
    int i;
    for (i = 0; i < c->npens; i++) {
        UBYTE p = c->pens[i];
        if ((p >= CK_POOL0 && !t->live[p]) || t->rgbof[p] != c->prgb[i])
            return FALSE;
    }
    return TRUE;
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
        int v0 = k >> 10, v1 = (k >> 5) & 31, v2 = k & 31;
        if (v0 < lo[0]) lo[0] = v0;
        if (v0 > hi[0]) hi[0] = v0;
        if (v1 < lo[1]) lo[1] = v1;
        if (v1 > hi[1]) hi[1] = v1;
        if (v2 < lo[2]) lo[2] = v2;
        if (v2 > hi[2]) hi[2] = v2;
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

/* A box's colours in order along its axis (counting: 32 values) */
static void ck_box_sort(struct LkTarget *t, const struct CkBox *b)
{
    ULONG cnt[33];
    long i;
    int sh = b->axis == 0 ? 10 : b->axis == 1 ? 5 : 0;
    UWORD *l = t->list + b->at;
    memset(cnt, 0, sizeof(cnt));
    for (i = 0; i < b->n; i++)
        cnt[((l[i] >> sh) & 31) + 1]++;
    for (i = 1; i < 33; i++)
        cnt[i] += cnt[i - 1];
    for (i = 0; i < b->n; i++)
        t->tmp[cnt[(l[i] >> sh) & 31]++] = l[i];
    memcpy(l, t->tmp, (size_t)b->n * sizeof(UWORD));
}

static void lk_picture_(struct LkOut *o, long x, long y, const unsigned long *px, long w, long h, long mod);
void lk_picture(struct LkOut *o, long x, long y, const unsigned long *px, long w, long h, long mod)
{
    ULONG t0 = time_ms();
    lk_picture_(o, x, y, px, w, h, mod);
    lkt_pic += time_ms() - t0;
}

static void ck_draw_mapped(struct LkOut *o, struct LkTarget *t, long x, long y, const unsigned long *px,
                           long w, long h, long mod)
{
    t->pmap = t->pmapb;
    lk_image(o, x, y, px, w, h, mod);
    t->pmap = NULL;
}

static void lk_picture_(struct LkOut *o, long x, long y, const unsigned long *px, long w, long h, long mod)
{
    struct LkTarget *t = o->ctx;
    static struct CkBox box[128];
    struct CkCut *c = NULL;
    long i, j, n = 0, want, nb;
    ULONG sum = 0;
    if (w <= 0 || h <= 0)
        return;
    if (!t || !t->ch || !t->hist) {
        lk_image(o, x, y, px, w, h, mod);
        return;
    }
    for (j = 0; j < 16; j++)
        sum = sum * 31 + px[(j * (h - 1) / 15) * mod + (j * 7 % 16) * (w - 1) / 15];
    for (i = 0; i < CK_CUTS; i++)       /* cut before, its pens still its colours: drawn so */
        if (ck_cut[i].px == px && ck_cut[i].w == w && ck_cut[i].h == h && ck_cut[i].mod == mod &&
            ck_cut[i].sum == sum) {
            c = &ck_cut[i];
            if (!ck_cut_good(t, c)) {
                ck_cut_drop(c);
                c = NULL;
                break;
            }
            c->used = ++ck_cut_clock;
            for (j = 0; j < c->n; j++)
                t->pmapb[c->key[j]] = c->pen[j];
            ck_draw_mapped(o, t, x, y, px, w, h, mod);
            return;
        }
    /* how many colours: one for each 300 pixels, 16 to 96 (a wall's cover
     * at 90 x 90, 27; a game's screenshot in a page's details at 200 x 160,
     * 96 - it had 24, as a small one, its shades posterised), while pens
     * are free, leaving some for what is drawn after; a strip (a
     * reflection, faded into the background) none of its own */
    want = w * h / 300;
    if (want < 16)
        want = 16;
    if (want > 96)
        want = 96;
    if (want > t->nfree - 24)
        want = t->nfree - 24;
    if ((ULONG)t->nfree < lkt_free)
        lkt_free = (ULONG)t->nfree;
    if (w >= 3 * h && !lk_pic_own)
        want = 0;
    else if (want < 6)
        lkt_short++;
    if (want < 6) {
        t->no_new = TRUE;               /* (no pens to spare: the nearest) */
        lk_image(o, x, y, px, w, h, mod);
        t->no_new = FALSE;
        return;
    }
    for (j = 0; j < h; j++) {           /* its colours, and how many of each */
        const unsigned long *r = px + j * mod;
        for (i = 0; i < w; i++) {
            ULONG k = CK555(r[i]);
            if (!t->hist[k]++)
                t->list[n++] = (UWORD)k;
        }
    }
    if (n <= want) {                    /* few colours (a game's screenshot): each its own */
        for (i = 0; i < n; i++)
            t->hist[t->list[i]] = 0;
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
        ck_box_sort(t, &box[bi]);
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
    /* kept: the oldest slot (or one free), if there is room */
    if (ck_cut_bytes + (ULONG)n * 3 <= CK_CUT_BYTES) {
        long k = 0;
        for (i = 1; i < CK_CUTS; i++)
            if (ck_cut[i].used < ck_cut[k].used)
                k = i;
        c = &ck_cut[k];
        ck_cut_drop(c);
        if ((c->key = AllocVec((ULONG)n * 3, MEMF_ANY))) {
            c->pen = (UBYTE *)(c->key + n);
            c->n = (UWORD)n;
            ck_cut_bytes += (ULONG)n * 3;
            c->px = px;
            c->w = w;
            c->h = h;
            c->mod = mod;
            c->sum = sum;
            c->used = ++ck_cut_clock;
        } else {
            c = NULL;
        }
    }
    for (j = 0; j < nb; j++) {          /* each box: its pixels' average, a pen */
        ULONG sr = 0, sg = 0, sb = 0, pop = 0;
        UBYTE pen;
        for (i = 0; i < box[j].n; i++) {
            UWORD k = t->list[box[j].at + i];
            ULONG cnt = t->hist[k];
            sr += ((k >> 10) * 8 + 4) * cnt;
            sg += (((k >> 5) & 31) * 8 + 4) * cnt;
            sb += ((k & 31) * 8 + 4) * cnt;
            pop += cnt;
        }
        if (!pop)
            pop = 1;
        pen = ck_pen_for(t, ((sr / pop) << 16) | ((sg / pop) << 8) | (sb / pop));
        for (i = 0; i < box[j].n; i++) {
            UWORD k = t->list[box[j].at + i];
            t->pmapb[k] = pen;
            t->hist[k] = 0;             /* (cleared as it goes: none left for the next) */
            if (c) {
                c->key[box[j].at + i] = k;
                c->pen[box[j].at + i] = pen;
            }
        }
        if (c && c->npens < 96) {
            c->pens[c->npens] = pen;
            c->prgb[c->npens++] = t->rgbof[pen];
        }
    }
    ck_draw_mapped(o, t, x, y, px, w, h, mod);
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

/* ---- showing an 8-bit canvas on an AGA screen: chunky to planar here,
 * straight into the screen's planes (WriteChunkyPixels converts a pixel
 * at a time: the Home row's and the wall's glides moved some 150,000
 * pixels a step). 32 pixels at a time: eight longwords of pens made the
 * eight planes' longwords by five rounds of swapping bits between pairs
 * (each swaps one bit of which longword with one bit of where in it). */
#define CK_MERGE(x, y, s, m) { ULONG t_ = (((x) >> (s)) ^ (y)) & (m); (y) ^= t_; (x) ^= t_ << (s); }
static void ck_c2p32(const UBYTE *px, ULONG *out)
{
    ULONG a0, a1, a2, a3, a4, a5, a6, a7;
    const ULONG *q = (const ULONG *)px;     /* (big-endian: the first pixel the top byte) */
    a0 = q[7]; a1 = q[6]; a2 = q[5]; a3 = q[4];
    a4 = q[3]; a5 = q[2]; a6 = q[1]; a7 = q[0];
    CK_MERGE(a0, a4, 16, 0x0000FFFFUL); CK_MERGE(a1, a5, 16, 0x0000FFFFUL);
    CK_MERGE(a2, a6, 16, 0x0000FFFFUL); CK_MERGE(a3, a7, 16, 0x0000FFFFUL);
    CK_MERGE(a0, a2, 8, 0x00FF00FFUL); CK_MERGE(a1, a3, 8, 0x00FF00FFUL);
    CK_MERGE(a4, a6, 8, 0x00FF00FFUL); CK_MERGE(a5, a7, 8, 0x00FF00FFUL);
    CK_MERGE(a0, a1, 4, 0x0F0F0F0FUL); CK_MERGE(a2, a3, 4, 0x0F0F0F0FUL);
    CK_MERGE(a4, a5, 4, 0x0F0F0F0FUL); CK_MERGE(a6, a7, 4, 0x0F0F0F0FUL);
    CK_MERGE(a0, a4, 2, 0x33333333UL); CK_MERGE(a1, a5, 2, 0x33333333UL);
    CK_MERGE(a2, a6, 2, 0x33333333UL); CK_MERGE(a3, a7, 2, 0x33333333UL);
    CK_MERGE(a0, a2, 1, 0x55555555UL); CK_MERGE(a1, a3, 1, 0x55555555UL);
    CK_MERGE(a4, a6, 1, 0x55555555UL); CK_MERGE(a5, a7, 1, 0x55555555UL);
    /* longword (j2 j1 j0) holds plane j0 j2 j1 */
    out[0] = a0; out[2] = a4; out[4] = a1; out[6] = a5;
    out[1] = a2; out[3] = a6; out[5] = a3; out[7] = a7;
}

/* Whether the canvas can go straight into the screen's planes: 8 planes
 * of a standard bitmap, longword-aligned, the window's layer not covered
 * by anything (a requester over it: WriteChunkyPixels, which minds it).
 * TRUE: the layer (if any) left locked; the canvas's (0,0) is at ox, oy
 * in the bitmap */
static BOOL ck_direct(struct LkTarget *t, struct BitMap **bmp, struct Layer **lyp, long *ox, long *oy)
{
    struct RastPort *rp = t->g->rp;
    struct BitMap *bm = rp ? rp->BitMap : NULL;
    struct Layer *ly = rp ? rp->Layer : NULL;
    long p;
    *ox = *oy = 0;
    if (!bm || bm->Depth != 8 || (t->cw & 31) || (bm->BytesPerRow & 3) ||
        !(GetBitMapAttr(bm, BMA_FLAGS) & BMF_STANDARD))
        return FALSE;
    for (p = 0; p < 8; p++)
        if (!bm->Planes[p] || ((ULONG)bm->Planes[p] & 3))
            return FALSE;
    if (ly) {
        LockLayerRom(ly);
        if (!ly->ClipRect || ly->ClipRect->Next || ly->ClipRect->obscured ||
            ly->ClipRect->bounds.MinX != ly->bounds.MinX || ly->ClipRect->bounds.MinY != ly->bounds.MinY ||
            ly->ClipRect->bounds.MaxX != ly->bounds.MaxX || ly->ClipRect->bounds.MaxY != ly->bounds.MaxY ||
            (ly->bounds.MinX & 31)) {
            UnlockLayerRom(ly);
            return FALSE;
        }
        *ox = ly->bounds.MinX;
        *oy = ly->bounds.MinY;
        if (*ox + t->cw - 1 > ly->bounds.MaxX || *oy + t->chh - 1 > ly->bounds.MaxY) {
            UnlockLayerRom(ly);
            return FALSE;
        }
    }
    *bmp = bm;
    *lyp = ly;
    return TRUE;
}

/* Shown by ck_c2p, if the screen is one it can write to (ck_direct) */
static BOOL ck_c2p_(struct LkTarget *t, long x0, long y0, long x1, long y1);
static BOOL ck_c2p(struct LkTarget *t, long x0, long y0, long x1, long y1)
{
    BOOL r;
    LKP_BEGIN;
    r = ck_c2p_(t, x0, y0, x1, y1);
    LKP_END(LKP_C2P);
    return r;
}
static BOOL ck_c2p_(struct LkTarget *t, long x0, long y0, long x1, long y1)
{
    struct BitMap *bm;
    struct Layer *ly;
    long ox, oy, x, y, p;
    ULONG out[8];
    if (!ck_direct(t, &bm, &ly, &ox, &oy))
        return FALSE;
    if (t->blitting) {
        WaitBlit();                     /* (a copy by the blitter still under way: the CPU's
                                         * writes after it, not under it) */
        t->blitting = FALSE;
    }
    x0 &= ~31L;
    x1 |= 31L;
    if (x1 >= t->cw)
        x1 = t->cw - 1;
    for (y = lk_show_up ? y1 : y0; lk_show_up ? y >= y0 : y <= y1; y += lk_show_up ? -1 : 1) {
        const UBYTE *s = t->ch + y * t->cw;
        ULONG row = (ULONG)(y + oy) * bm->BytesPerRow;
        ULONG *sl = t->sh ? (ULONG *)(t->sh + y * t->cw) : NULL;
        long xa = x0, xb = x1;
        if (t->rx0) {                   /* (this row's own changed span) */
            if (t->rx1[y] < t->rx0[y])
                continue;
            if (t->rx0[y] > xa)
                xa = t->rx0[y] & ~31L;
            if (t->rx1[y] < xb)
                xb = t->rx1[y] | 31L;
            t->rx0[y] = 0x7FFF;
            t->rx1[y] = -1;
        }
        for (x = xa; x + 31 <= xb; x += 32) {
            ULONG at = row + (ULONG)(x + ox) / 8;
            if (sl) {
                /* the same 32 the planes show already: left (most of a
                 * list's rows is its background, the same pen) */
                const ULONG *c = (const ULONG *)(s + x);
                ULONG *m = sl + x / 4;
                if (!t->sh_all && c[0] == m[0] && c[1] == m[1] && c[2] == m[2] && c[3] == m[3] &&
                    c[4] == m[4] && c[5] == m[5] && c[6] == m[6] && c[7] == m[7]) {
                    lkt_same++;
                    continue;
                }
                m[0] = c[0]; m[1] = c[1]; m[2] = c[2]; m[3] = c[3];
                m[4] = c[4]; m[5] = c[5]; m[6] = c[6]; m[7] = c[7];
            }
            ck_c2p32(s + x, out);
            for (p = 0; p < 8; p++)
                *(ULONG *)(bm->Planes[p] + at) = out[p];
        }
    }
    if (x0 == 0 && y0 == 0 && x1 >= t->cw - 1 && y1 >= t->chh - 1)
        t->sh_all = FALSE;              /* (all of it as the canvas now: groups compared after) */
    if (ly)
        UnlockLayerRom(ly);
    return TRUE;
}

/* A copy on the canvas made on the screen too, by the blitter: what is
 * there already moved in chip RAM instead of c2p'd again (a glide's step
 * moves some 200,000 pixels: the CPU's c2p of them 20-25 ms without the
 * JIT; the blitter's alongside the CPU). The canvas moved now; the
 * screen's blit kept for lk_show, made there just before what was drawn
 * is shown - blitted at once, the rows copied showed a moment before
 * those drawn over them (a list's old highlight twice, while it
 * scrolled). What waits to be shown is moved with the copy: its place
 * and where it went both shown. FALSE: not here (the window covered,
 * Ambiance/LayoutBlit 0, too many waiting): the caller's way */
static BOOL ck_blit(struct LkTarget *t, long x, long y, long w, long h, long dx, long dy)
{
    struct BitMap *bm;
    struct Layer *ly;
    long ox, oy, j;
    /* across only (a glide): a list scrolled up or down by the blitter
     * showed, while it moved, its old highlight copied along before the
     * rows over it were drawn - the CPU's c2p of the whole shows it as it
     * ends up, top to bottom */
    if (dy || !lk_blit_ok || w <= 0 || h <= 0 || t->nbq >= 8 || !ck_direct(t, &bm, &ly, &ox, &oy))
        return FALSE;
    if (ly)
        UnlockLayerRom(ly);
    if (dy > 0) {                       /* (the canvas as the screen will be) */
        for (j = h - 1; j >= 0; j--)
            memmove(t->ch + (y + j + dy) * t->cw + x + dx, t->ch + (y + j) * t->cw + x, (size_t)w);
    } else {
        for (j = 0; j < h; j++)
            memmove(t->ch + (y + j + dy) * t->cw + x + dx, t->ch + (y + j) * t->cw + x, (size_t)w);
    }
    if (t->x0 <= t->x1)                 /* (what waits, where the copy took it too) */
        ck_touch(t, t->x0 + dx, t->y0 + dy, t->x1 - t->x0 + 1, t->y1 - t->y0 + 1);
    t->bq[t->nbq].x = (WORD)x;
    t->bq[t->nbq].y = (WORD)y;
    t->bq[t->nbq].w = (WORD)w;
    t->bq[t->nbq].h = (WORD)h;
    t->bq[t->nbq].dx = (WORD)dx;
    t->bq[t->nbq].dy = (WORD)dy;
    t->nbq++;
    lkt_blits++;
    lkt_bpx += (ULONG)w * (ULONG)h;
    return TRUE;
}

/* The copies kept (ck_blit) made on the screen, in order; FALSE if they
 * cannot be now (the window covered since): their places to be shown
 * from the canvas instead */
static BOOL ck_blits_run(struct LkTarget *t)
{
    struct BitMap *bm;
    struct Layer *ly;
    long ox, oy;
    WORD i;
    if (!t->nbq)
        return TRUE;
    if (!ck_direct(t, &bm, &ly, &ox, &oy)) {
        for (i = 0; i < t->nbq; i++)
            ck_touch(t, t->bq[i].x + t->bq[i].dx, t->bq[i].y + t->bq[i].dy, t->bq[i].w, t->bq[i].h);
        t->nbq = 0;
        return FALSE;
    }
    if (t->blitting)
        WaitBlit();
    CacheClearU();                      /* (the CPU's writes to the planes out to chip RAM first) */
    if (t->sh)                          /* (what the planes show, moved as they are) */
        for (i = 0; i < t->nbq; i++) {
            long j, x = t->bq[i].x, y = t->bq[i].y, w = t->bq[i].w, h = t->bq[i].h, dx = t->bq[i].dx,
                 dy = t->bq[i].dy;
            if (dy > 0)
                for (j = h - 1; j >= 0; j--)
                    memmove(t->sh + (y + j + dy) * t->cw + x + dx, t->sh + (y + j) * t->cw + x, (size_t)w);
            else
                for (j = 0; j < h; j++)
                    memmove(t->sh + (y + j + dy) * t->cw + x + dx, t->sh + (y + j) * t->cw + x, (size_t)w);
        }
    for (i = 0; i < t->nbq; i++)        /* (one bitmap, overlapping: BltBitMap picks the direction) */
        BltBitMap(bm, (WORD)(t->bq[i].x + ox), (WORD)(t->bq[i].y + oy), bm,
                  (WORD)(t->bq[i].x + t->bq[i].dx + ox), (WORD)(t->bq[i].y + t->bq[i].dy + oy), t->bq[i].w,
                  t->bq[i].h, 0xC0, 0xFF, NULL);
    t->blitting = TRUE;                 /* (the c2p after waits for it) */
    t->nbq = 0;
    if (ly)
        UnlockLayerRom(ly);
    return TRUE;
}

void lk_show(struct LkOut *o)
{
    struct LkTarget *t = o->ctx;
    if (t && t->ch) {
        if (t->nbq) {
            ULONG tb = time_ms();
            gfx_dirty();
            ck_blits_run(t);
            lkt_show += time_ms() - tb;
        }
        if (t->x0 > t->x1)
            return;
        if (t->x0 < 0) t->x0 = 0;
        if (t->y0 < 0) t->y0 = 0;
        if (t->x1 >= t->cw) t->x1 = t->cw - 1;
        if (t->y1 >= t->chh) t->y1 = t->chh - 1;
        ULONG t0 = time_ms();
        gfx_dirty();
        if (!ck_c2p(t, t->x0, t->y0, t->x1, t->y1)) {
            if (t->blitting) {
                WaitBlit();
                t->blitting = FALSE;
            }
            WriteChunkyPixels(t->g->rp, (ULONG)t->x0, (ULONG)t->y0, (ULONG)t->x1, (ULONG)t->y1,
                              t->ch + t->y0 * t->cw + t->x0, t->cw);
            t->sh_all = TRUE;           /* (covered: what shows is not known - all, next time) */
        }
        lkt_show += time_ms() - t0;
        lkt_px += (ULONG)(t->x1 - t->x0 + 1) * (ULONG)(t->y1 - t->y0 + 1);
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

/* ---- the big letter by hardware sprites (AGA) ------------------------- */
BOOL lk_sprites_ok = TRUE;
BOOL lk_skip_ok = TRUE;         /* unchanged groups of 32 not converted (Ambiance/LayoutSkip=0: all) */
BOOL lk_pic_own;                /* the picture drawn is the host's: its own pens, however wide */
BOOL lk_show_up;                /* shown from the bottom up (a list's selection went up) */

static ULONG ck_vc_get(struct ViewPort *vp, ULONG tag, ULONG dflt)
{
    struct TagItem tg[2];
    tg[0].ti_Tag = tag;
    tg[0].ti_Data = dflt;
    tg[1].ti_Tag = TAG_DONE;
    tg[1].ti_Data = 0;
    if (VideoControl(vp->ColorMap, tg))
        return dflt;
    return tg[0].ti_Data;
}

static void ck_sprites_free(struct LkTarget *t)
{
    WORD i;
    for (i = 0; i < t->sp_n; i++) {
        if (t->sp_num[i] >= 0)
            FreeSprite(t->sp_num[i]);
        if (t->sp_sd[i])
            FreeSpriteData(t->sp_sd[i]);
        t->sp_sd[i] = NULL;
        t->sp_num[i] = -1;
    }
    t->sp_n = 0;
    t->sp_of = NULL;
}

void lk_letter_sprites_off(struct LkOut *o)
{
    struct LkTarget *t = o ? o->ctx : NULL;
    if (t && t->sp_n)
        ck_sprites_free(t);
}

/* The big letter (mask w x h, bpr bytes a row; cw the widest letter's
 * width, so the sprites' count stays as letters change) with its top left
 * at x, y in rgb, by sprites 2-7 over the screen: TRUE if so (nothing
 * drawn), FALSE if it must be drawn (not AGA, not enough sprites, sprites
 * finer than the screen's pixels, their colours not at 16-31). A sprite
 * pixel covers several of the screen's (a lores one two, a laced screen's
 * two lines): how much of it the letter covers picks one of three shades
 * between the background and the letter - its edges smooth, not blocks. */
BOOL lk_letter_sprites(struct LkOut *o, const UBYTE *mask, UWORD w, UWORD h, UWORD bpr, UWORD cw, WORD x,
                       WORD y, ULONG rgb)
{
    struct LkTarget *t = o ? o->ctx : NULL;
    struct ViewPort *vp;
    LONG res, xl, spx, sw, ly, sh, n, i, r, c, eb, ob;
    char m[140];
    if (!t || !t->ch || !lk_sprites_ok || !t->g->scr || !(GfxBase->ChipRevBits0 & GFXF_AA_ALICE) ||
        GfxBase->LibNode.lib_Version < 39 || !mask)
        return FALSE;
    vp = &t->g->scr->ViewPort;
    xl = (vp->Modes & SUPERHIRES) ? 4 : (vp->Modes & HIRES) ? 2 : 1;   /* (the screen's pixels a lores one) */
    res = (LONG)ck_vc_get(vp, VTAG_SPRITERESN_GET, (ULONG)SPRITERESN_DEFAULT);
    if (res == SPRITERESN_DEFAULT)
        res = (LONG)ck_vc_get(vp, VTAG_DEFSPRITERESN_GET, (ULONG)SPRITERESN_ECS);
    spx = res == SPRITERESN_35NS ? xl / 4 : res == SPRITERESN_70NS ? xl / 2 : xl;   /* a sprite pixel's */
    sw = GfxBase->SpriteWidth ? (LONG)GfxBase->SpriteWidth * 16 : 16;
    ly = (vp->Modes & LACE) ? 2 : 1;
    eb = (LONG)ck_vc_get(vp, VTAG_SPEVEN_BASE_GET, 16);
    ob = (LONG)ck_vc_get(vp, VTAG_SPODD_BASE_GET, 16);
    if (spx < 1 || eb != 16 || ob != 16) {
        if (!t->sp_of && lk_sprites_ok) {
            sprintf(m, "the big letter drawn: sprites' pixels %ld, their colours from %ld/%ld", (long)spx,
                    (long)eb, (long)ob);
            amb_log("layout", m);
        }
        lk_sprites_ok = FALSE;
        return FALSE;
    }
    if (cw < w)
        cw = w;
    n = ((cw + spx - 1) / spx + sw - 1) / sw;
    sh = (h + ly - 1) / ly;
    if (n > 6 || sh < 1)
        return FALSE;
    if (t->sp_n && t->sp_n != n)
        ck_sprites_free(t);
    if (mask != t->sp_of) {
        for (i = 0; i < n; i++) {
            struct BitMap *bm = AllocBitMap((ULONG)sw, (ULONG)sh, 2, BMF_CLEAR, NULL);
            struct ExtSprite *sd;
            struct TagItem tg[2];
            if (!bm) {
                ck_sprites_free(t);
                return FALSE;
            }
            for (r = 0; r < sh; r++) {
                UBYTE *d0 = bm->Planes[0] + r * bm->BytesPerRow, *d1 = bm->Planes[1] + r * bm->BytesPerRow;
                for (c = 0; c < sw; c++) {
                    LONG sx = (i * sw + c) * spx, a, b, k = 0, all = 0, v;
                    for (b = 0; b < ly; b++) {
                        const UBYTE *src = mask + (ULONG)(r * ly + b) * bpr;
                        if (r * ly + b >= (LONG)h)
                            break;
                        for (a = 0; a < spx; a++, all++)
                            if (sx + a < (LONG)w && (src[(sx + a) >> 3] & (0x80 >> ((sx + a) & 7))))
                                k++;
                    }
                    if (!k || !all)
                        continue;
                    v = k * 3 / all;            /* 0-3: how much is covered (a sliver: the faintest) */
                    if (v < 1)
                        v = 1;
                    if (v & 1)
                        d0[c >> 3] |= (UBYTE)(0x80 >> (c & 7));
                    if (v & 2)
                        d1[c >> 3] |= (UBYTE)(0x80 >> (c & 7));
                }
            }
            tg[0].ti_Tag = SPRITEA_Width;
            tg[0].ti_Data = (ULONG)sw;
            tg[1].ti_Tag = TAG_DONE;
            sd = AllocSpriteDataA(bm, tg);
            WaitBlit();
            FreeBitMap(bm);
            if (!sd) {
                ck_sprites_free(t);
                return FALSE;
            }
            if (i < t->sp_n && t->sp_sd[i]) {
                ChangeExtSpriteA(vp, t->sp_sd[i], sd, NULL);
                FreeSpriteData(t->sp_sd[i]);
                t->sp_sd[i] = sd;
            } else {
                /* sprites 2-7 only (0 and 1 are the pointer's, and its colours) */
                LONG num = -1, k;
                for (k = 2; k < 8 && num < 0; k++) {
                    struct TagItem gt[2];
                    gt[0].ti_Tag = GSTAG_SPRITE_NUM;
                    gt[0].ti_Data = (ULONG)k;
                    gt[1].ti_Tag = TAG_DONE;
                    num = GetExtSpriteA(sd, gt);
                }
                if (num < 0) {
                    FreeSpriteData(sd);
                    t->sp_n = (WORD)i;
                    ck_sprites_free(t);
                    amb_log("layout", "the big letter: not enough sprites free, drawn instead");
                    lk_sprites_ok = FALSE;
                    return FALSE;
                }
                t->sp_sd[i] = sd;
                t->sp_num[i] = (WORD)num;
                t->sp_n = (WORD)(i + 1);
            }
        }
        if (!t->sp_of) {
            sprintf(m, "the big letter by %ld sprites %ld wide (a sprite pixel %ld, laced %ld): %ld %ld %ld",
                    (long)n, (long)sw, (long)spx, (long)(ly - 1), (long)t->sp_num[0],
                    (long)(n > 1 ? t->sp_num[1] : -1), (long)(n > 2 ? t->sp_num[2] : -1));
            amb_log("layout", m);
        }
        t->sp_of = mask;
        t->sp_x = -1000;                /* (placed again) */
        t->sp_rgb = ~rgb;
    }
    if (rgb != t->sp_rgb) {
        /* three shades, the background (at the letter's middle) to the letter */
        ULONG bg = t->rowc ? t->rowc[(y + (LONG)h / 2) < t->chh ? (y + (LONG)h / 2 > 0 ? y + h / 2 : 0)
                                                               : t->chh - 1]
                           : 0;
        t->sp_rgb = rgb;
        for (i = 0; i < n; i++) {
            LONG pen = 16 + ((t->sp_num[i] & 6) << 1), k;
            for (k = 1; k <= 3; k++) {
                LONG f = k == 3 ? 256 : k == 2 ? 170 : 85, ch[3], q;
                for (q = 0; q < 3; q++) {
                    LONG a = (LONG)((bg >> (16 - 8 * q)) & 255), b = (LONG)((rgb >> (16 - 8 * q)) & 255);
                    ch[q] = a + (b - a) * f / 256;
                }
                SetRGB32(vp, (ULONG)(pen + k), (ULONG)ch[0] * 0x01010101UL, (ULONG)ch[1] * 0x01010101UL,
                         (ULONG)ch[2] * 0x01010101UL);
            }
        }
    }
    if (x != t->sp_x || y != t->sp_y) {
        /* (where: the screen's own pixels and lines, as the ViewPort has them) */
        t->sp_x = x;
        t->sp_y = y;
        for (i = 0; i < n; i++)
            MoveSprite(vp, (struct SimpleSprite *)t->sp_sd[i], (WORD)(x + i * sw * spx), y);
    }
    return TRUE;
}

void lk_close(struct LkOut *o)
{
    struct LkTarget *t = o->ctx;
    if (t) {
        ck_copper_off(t);               /* (the copper's gradient off the screen first) */
        if (t->blitting)
            WaitBlit();                 /* (a copy of ours still under way) */
        ck_sprites_free(t);
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
        if (t->sh)
            FreeVec(t->sh);
        if (t->rx0)
            FreeVec(t->rx0);
        if (t->tmp)
            FreeVec(t->tmp);
        if (t->ch)
            ck_cuts_free();             /* (the cuts' pens are this screen's) */
        lk_cover_cache_free();          /* (the font may go with it) */
        if (t->pal_was) {
            if (t->g->scr) {            /* (the classic's colours back: first the screen
                                         * cleared, or the layout's pens show in them
                                         * a moment - quitting, a flash of garbage) */
                if (t->g->rp) {
                    SetAPen(t->g->rp, 0);
                    RectFill(t->g->rp, 0, 0, (WORD)(t->cw - 1), (WORD)(t->chh - 1));
                }
                LoadRGB32(&t->g->scr->ViewPort, t->pal_was);
            }
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

/* Text's coverage straight into the canvas: each row's sixteen shades
 * (the background under it to fg) given their pens once, then a pen a
 * pixel by its coverage - not made colours a pixel at a time and each
 * looked up again (lk_cover then ck_rows: four fifths of the text's
 * time). FALSE: not here (the dithered ramp: its pen a pixel's own) */
static BOOL ck_cover(struct LkOut *o, long x, long y, const UBYTE *cov, long w, long h, long mod,
                     unsigned long fg, const struct LkBg *bg)
{
    struct LkTarget *t = o->ctx;
    long x0 = x, x1 = x + w, y0 = y, y1 = y + h, i, j, k;
    UBYTE pl[16];
    ULONG last = 0xFFFFFFFFUL;
    if (!bg || (t->rowc && !t->copper))
        return FALSE;
    if (x0 < o->cx0) x0 = o->cx0;
    if (y0 < o->cy0) y0 = o->cy0;
    if (x1 > o->cx1) x1 = o->cx1;
    if (y1 > o->cy1) y1 = o->cy1;
    if (x1 <= x0 || y1 <= y0)
        return TRUE;
    ck_touch(t, x0, y0, x1 - x0, y1 - y0);
    for (j = y0; j < y1; j++) {
        ULONG b = lk_bg_at(bg, j) & 0xFFFFFF;
        const UBYTE *c = cov + (j - y) * mod + (x0 - x);
        UBYTE *d = t->ch + j * t->cw + x0;
        if (b != last || t->copper) {   /* (the copper's: whether a shade is the background's
                                         * is the row's own) */
            long pa = -1;
            for (k = 0; k < 16; k++) {
                long a = k;
                ULONG cc;
                UBYTE pen;
                if (o->cov_steps > 1)
                    a = (k * (o->cov_steps - 1) + 7) / 15 * 15 / (o->cov_steps - 1);
                if (a == pa) {
                    pl[k] = pl[k - 1];      /* (the same step as the one before) */
                    continue;
                }
                pa = a;
                cc = lk_mix(b, fg, a, 15) & 0xFFFFFF;
                if (!(pen = ck_bg_pen(t, x0, j, cc)))
                    pen = ck_pen_for(t, cc);
                pl[k] = pen;
            }
            last = b;
        }
        for (i = 0; i < x1 - x0; i++)
            d[i] = pl[c[i] & 15];
    }
    return TRUE;
}

/* A gradient's colour: its own pen while there are plenty (a tile's
 * gradient in three bands, its colours a 5-5-5 cell's apart, came out
 * striped); short of them, the pool's as near as makes no difference */
static UBYTE ck_pen_exact(struct LkTarget *t, unsigned long rgb)
{
    int i;
    rgb &= 0xFFFFFF;
    for (i = 0; i < t->nlive; i++)
        if (t->lives[i] >= CK_POOL0 && t->rgbof[t->lives[i]] == rgb)
            return t->lives[i];
    if (t->nfree > 120) {
        UBYTE made = ck_new(t, rgb);
        if (made)
            return made;
    }
    return ck_pen_for(t, rgb);
}

/* A vertical gradient straight onto the 8-bit canvas: a row at a time,
 * each the background's pen where it is the copper's line (or its shade),
 * else a pen of its own - not a fill a row (a list's rows drawn again
 * made some 400 fills a move). FALSE: not here (the dithered ramp) */
BOOL lk_vgrad8(struct LkOut *o, long x, long y, long w, long h, unsigned long a, unsigned long b)
{
    struct LkTarget *t = o ? o->ctx : NULL;
    long x0 = x, x1 = x + w, y0 = y, y1 = y + h, j;
    ULONG last = 0xFFFFFFFFUL;
    UBYTE pen = 0;
    LKP_BEGIN;
    if (!t || !t->ch || o->fill != ck_fill || (t->rowc && !t->copper) || h <= 1 || a == b)
        return FALSE;
    if (x0 < o->cx0) x0 = o->cx0;
    if (y0 < o->cy0) y0 = o->cy0;
    if (x1 > o->cx1) x1 = o->cx1;
    if (y1 > o->cy1) y1 = o->cy1;
    if (x1 <= x0 || y1 <= y0)
        return TRUE;
    ck_touch(t, x0, y0, x1 - x0, y1 - y0);
    for (j = y0; j < y1; j++) {
        ULONG c = lk_vgrad_at(a, b, h, j - y) & 0xFFFFFF;
        UBYTE bp = ck_bg_pen(t, x0, j, c);      /* (the copper's: the row's own) */
        if (!bp) {
            if (c != last || !pen) {
                pen = ck_pen_exact(t, c);
                last = c;
            }
            bp = pen;
        }
        memset(t->ch + j * t->cw + x0, bp, (size_t)(x1 - x0));
    }
    LKP_END(LKP_GRAD);
    return TRUE;
}

/* Text's coverage as sm_cover made it, kept for the same text again (a
 * list's labels drawn again as its rows move): 48 of them, the oldest
 * going; another font, all go */
#define LKC_N 48
static struct {
    const void *sm;
    int face;
    LONG n;
    char s[96];
    UWORD w, h, mod;
    WORD dx, dy;
    UBYTE *cov;
    ULONG used;
} lkc[LKC_N];
static ULONG lkc_clock;
static const UBYTE *lk_cover_of(struct Gfx *g, const char *s, LONG n, int face, WORD *dx, WORD *dy, UWORD *w,
                                UWORD *h, UWORD *mod)
{
    int i, old = 0;
    const UBYTE *cov;
    ULONG bytes;
    if (n > 0 && n < (LONG)sizeof(lkc[0].s))
        for (i = 0; i < LKC_N; i++) {
            if (lkc[i].cov && lkc[i].sm == (const void *)g->sm && lkc[i].face == face && lkc[i].n == n &&
                !memcmp(lkc[i].s, s, (size_t)n)) {
                lkc[i].used = ++lkc_clock;
                *dx = lkc[i].dx;
                *dy = lkc[i].dy;
                *w = lkc[i].w;
                *h = lkc[i].h;
                *mod = lkc[i].mod;
                return lkc[i].cov;
            }
            if (lkc[i].used < lkc[old].used)
                old = i;
        }
    if (!(cov = sm_cover(g->sm, s, n, face, dx, dy, w, h, mod)))
        return NULL;
    bytes = (ULONG)*mod * *h;
    if (n > 0 && n < (LONG)sizeof(lkc[0].s) && bytes <= 16384) {
        if (lkc[old].cov)
            FreeVec(lkc[old].cov);
        if ((lkc[old].cov = AllocVec(bytes, MEMF_ANY))) {
            memcpy(lkc[old].cov, cov, bytes);
            lkc[old].sm = (const void *)g->sm;
            lkc[old].face = face;
            lkc[old].n = n;
            memcpy(lkc[old].s, s, (size_t)n);
            lkc[old].w = *w;
            lkc[old].h = *h;
            lkc[old].mod = *mod;
            lkc[old].dx = *dx;
            lkc[old].dy = *dy;
            lkc[old].used = ++lkc_clock;
        }
    }
    return cov;
}

void lk_cover_cache_free(void)
{
    int i;
    for (i = 0; i < LKC_N; i++) {
        if (lkc[i].cov)
            FreeVec(lkc[i].cov);
        lkc[i].cov = NULL;
        lkc[i].sm = NULL;
    }
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
    {
        /* all of it outside where drawing goes (a cover's label beside a
         * strip drawn again): not made, only measured */
        long hm = (long)sm_title_h(g->sm) + (long)sm_cell_h(g->sm);
        if (y - hm >= o->cy1 || y + 2 * hm <= o->cy0 || x - 8 >= o->cx1 ||
            (maxw > 0 && x + maxw + 8 <= o->cx0))
            return sm_textlen(g->sm, s, n, face);
    }
    {
        LKP_BEGIN;
        ULONG tg = time_ticks(&lkp_hz);
        if (!(cov = lk_cover_of(g, s, n, face, &dx, &dy, &w, &h, &mod)))
            return 0;
        if (o->ctx && o->fill == ck_fill)
            lkp_n[LKP_GLYPHS]++, lkp_t[LKP_GLYPHS] += time_ticks(&lkp_hz) - tg;
        if (!(o->ctx && o->fill == ck_fill && ck_cover(o, x + dx, y + dy, cov, w, h, mod, fg, bg)))
            lk_cover(o, x + dx, y + dy, cov, w, h, mod, fg, bg);
        if (o->ctx && o->fill == ck_fill)
            LKP_END(LKP_TEXT);
    }
    return sm_textlen(g->sm, s, n, face);
}

WORD lk_icon(struct Gfx *g, struct LkOut *o, int icon, WORD x, WORD y, ULONG fg,
             const struct LkBg *bg)
{
    const UBYTE *cov;
    UWORD n, mod;
    if (!g->sm || !(cov = sm_icon_cover(g->sm, icon, &n, &mod)))
        return 0;
    if (!(o->ctx && o->fill == ck_fill && ck_cover(o, x, y, cov, n, n, mod, fg, bg)))
        lk_cover(o, x, y, cov, n, n, mod, fg, bg);
    return (WORD)n;
}

WORD lk_icon_px(struct Gfx *g)
{
    return g->sm ? (WORD)sm_icon_px(g->sm) : 0;
}
