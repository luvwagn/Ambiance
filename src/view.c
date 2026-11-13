/*
 * AmigaMCE - the full-screen picture viewer
 * Copyright (c) 2026 Brian King
 *
 * Pictures are shown on AmigaMCE's own screen, so a monitor never sees a
 * mode change. On a screen with a colour table (native, 8-bit RTG) the
 * picture gets that table while it is shown:
 *
 *   direct: a picture with no more colours than the screen (ILBM, GIF,
 *           most PNG) is shown in its own colours, register for register,
 *           not remapped at all: exactly as drawn;
 *   own:    a deeper one (JPEG, true-colour PNG, HAM on a non-HAM screen)
 *           is made the size it is shown and given the best colours of
 *           its own for every register but 0, which stays black for the
 *           border (thumb.c: shrunk first, so far quicker than having the
 *           datatype remap it at full size);
 *   remap:  if that cannot be done (memory), the datatype remaps it into
 *           every pen but pen 0.
 *
 * The info bar then uses the picture's own darkest and lightest colours,
 * so it takes none away from it. Pictures fade in and out through black,
 * and the UI's colours come back when the viewer closes. On true-colour
 * screens nothing needs sharing: pictures are drawn in their colours and
 * the bar in the UI's.
 *
 * Pixels keep their shape: a picture's pixel aspect (its ILBM display mode,
 * or square for other formats) against the screen's sets a whole scale,
 * like 2 x 1 for lowres on a 640 x 256 screen, as large as fits; one too
 * big for that is shrunk to fit.
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <graphics/modeid.h>
#include <graphics/gfxbase.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/datatypes.h>
#include <clib/alib_protos.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>

extern struct GfxBase *GfxBase;
extern struct Library *DataTypesBase;
extern struct Library *CyberGfxBase;

int view_transition = VT_FADE;

#define FADE_STEPS 8

static BOOL clut;               /* the screen has a colour table we take over */
static UWORD ncolors;           /* its size */
static ULONG *saved;            /* the UI's colours: LoadRGB32 table */
static ULONG *shown;            /* the colours on screen now (count*3) */
static ULONG *work;             /* LoadRGB32 table for fades */
static Object *obj;             /* remapped picture: holds its pens */
static BOOL pen0_held, black_held;
static LONG black_pen = -1;     /* true colour: a black pen for the border */
static UWORD *blank;            /* an empty mouse pointer (chip RAM) */
static BOOL ptr_shown;          /* the mouse moved: the pointer shown till the bar goes */
static BOOL btns, btn_playing;  /* the bar's buttons (the mouse's), the slideshow running */
static int btn_full;            /* full screen's: 0 not shown, 1 to it, 2 back from it */
static int btn_n;               /* buttons shown */
static WORD btn_x[VB_COUNT], btn_cw;   /* each button's left, and width */
static UBYTE pen_bg, pen_fg, pen_dim, pen_border;
static BOOL has_pic;            /* a picture is up */
static WORD pic_x, pic_y, pic_w, pic_h;     /* where (true colour: what a transition leaves) */
static BOOL kept;               /* the last one left up, for the transition to the next */
static struct BitMap *bar_bm;   /* what the bar covers */
static WORD bar_y, bar_h;
static WORD spin_x = -1;            /* the bar's spinner: its centre (-1: none) */
static struct BitMap *spin_bm;      /* what the corner spinner covers */
static WORD spin_cx, spin_cy, spin_s;

/* ---- colours */
static void load_table(struct Gfx *g, const ULONG *rgb, ULONG scale)
{
    UWORD i;
    work[0] = ((ULONG)ncolors << 16) | 0;
    for (i = 0; i < ncolors * 3; i++) {
        ULONG v = ((rgb[i] >> 24) * scale) >> 3;     /* scale 0..8 */
        work[1 + i] = v * 0x01010101UL;
    }
    work[1 + ncolors * 3] = 0;
    LoadRGB32(&g->scr->ViewPort, work);
}

static void fade(struct Gfx *g, const ULONG *rgb, BOOL in)
{
    ULONG s;
    if (view_transition == VT_CUT) {        /* (at once) */
        load_table(g, rgb, in ? FADE_STEPS : 0);
        return;
    }
    for (s = 1; s <= FADE_STEPS; s++) {
        WaitTOF();
        load_table(g, rgb, in ? s : FADE_STEPS - s);
    }
}

static void all_black(struct Gfx *g)
{
    memset(shown, 0, ncolors * 3 * sizeof(ULONG));
    load_table(g, shown, 0);
}

static ULONG luma(const ULONG *rgb)
{
    return (rgb[0] >> 24) * 299 + (rgb[1] >> 24) * 587 + (rgb[2] >> 24) * 114;
}

/* The bar's pens, from the given pens of the picture: its darkest colour
 * behind the text, its lightest for the name, and one in between (if it
 * stands out enough) for the rest. */
static void pick_bar_pens(const UBYTE *pens, LONG n)
{
    LONG i;
    ULONG lo = ~0UL, hi = 0, want, best = ~0UL;
    pen_bg = pen_fg = pen_dim = 0;
    for (i = 0; i < n; i++) {
        ULONG l = luma(shown + pens[i] * 3);
        if (l < lo) {
            lo = l;
            pen_bg = pens[i];
        }
        if (l >= hi) {
            hi = l;
            pen_fg = pens[i];
        }
    }
    pen_dim = pen_fg;
    want = lo + (hi - lo) * 2 / 3;
    for (i = 0; i < n; i++) {
        ULONG l = luma(shown + pens[i] * 3), d = l > want ? l - want : want - l;
        if (l >= lo + (hi - lo) / 2 && d < best) {
            best = d;
            pen_dim = pens[i];
        }
    }
}

/* ---- pens */
static void pens_release(struct Gfx *g)
{
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    int i;
    if (!g->pens_shared)
        return;
    for (i = 0; i < NUM_UI_PENS; i++)
        ReleasePen(cm, i);
    for (i = POINTER_PEN0; i < POINTER_PEN0 + 3; i++)
        ReleasePen(cm, i);
}

static void pens_reclaim(struct Gfx *g)
{
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    int i;
    if (!g->pens_shared)
        return;
    for (i = 0; i < NUM_UI_PENS; i++)
        ObtainPen(cm, i, 0, 0, 0, PEN_EXCLUSIVE | PEN_NO_SETCOLOR);
    for (i = POINTER_PEN0; i < POINTER_PEN0 + 3; i++)
        ObtainPen(cm, i, 0, 0, 0, PEN_EXCLUSIVE | PEN_NO_SETCOLOR);
}

/* (pen is a real one: the picture's, or black obtained) */
static void fill(struct Gfx *g, UBYTE pen)
{
    gfx_dirty();
    SetWriteMask(g->rp, 0xFF);
    SetAPen(g->rp, pen);
    RectFill(g->rp, 0, 0, g->w - 1, g->h - 1);
}

BOOL view_begin(struct Gfx *g)
{
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    clut = !g->truecolor;
    ncolors = clut ? 1 << (g->depth > 8 ? 8 : g->depth) : 0;
    gfx_picture_free(g);                    /* the preview's pens go */
    has_pic = FALSE;
    obj = NULL;
    bar_bm = NULL;
    if ((blank = AllocMem(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR)))
        SetPointer(g->win, blank, 1, 16, 0, 0);
    ptr_shown = FALSE;
    btns = FALSE;
    if (clut) {
        LONG n = ncolors * 3;
        saved = AllocVec((n + 2) * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
        shown = AllocVec(n * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
        work = AllocVec((n + 2) * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
        if (!saved || !shown || !work) {
            FreeVec(saved);
            FreeVec(shown);
            FreeVec(work);
            saved = shown = work = NULL;
            if (blank) {
                ClearPointer(g->win);
                FreeMem(blank, 6 * sizeof(UWORD));
                blank = NULL;
            }
            return FALSE;
        }
        saved[0] = (ULONG)ncolors << 16;
        GetRGB32(cm, 0, ncolors, saved + 1);
        saved[1 + n] = 0;
        fade(g, saved + 1, FALSE);
        all_black(g);
        pens_release(g);
        pen0_held = ObtainPen(cm, 0, 0, 0, 0, PEN_EXCLUSIVE) == 0;
        fill(g, 0);
    } else {
        black_pen = ObtainBestPen(cm, 0, 0, 0, OBP_Precision, PRECISION_EXACT, TAG_END);
        fill(g, black_pen >= 0 ? black_pen : GPEN(PEN_BG));
    }
    return TRUE;
}

/* ---- the bar */
BOOL view_bar_shown(void)
{
    return bar_bm != NULL;
}

void view_bar_hide(struct Gfx *g)
{
    spin_x = -1;
    if (!bar_bm)
        return;
    gfx_dirty();
    BltBitMapRastPort(bar_bm, 0, 0, g->rp, 0, bar_y, g->w, bar_h, 0xC0);
    WaitBlit();
    FreeBitMap(bar_bm);
    bar_bm = NULL;
}

static void bar_buttons(struct Gfx *g, WORD x0, UBYTE fg);

void view_bar(struct Gfx *g, const char *title, const char *detail, const char *hint)
{
    UBYTE bg = clut ? pen_bg : PEN_BAR, fg = clut ? pen_fg : PEN_WHITE,
          dim = clut ? pen_dim : PEN_TEXT;
    WORD x = GU(g, 6), ty, tw, hw = 0;
    if (clut && !has_pic) {                 /* nothing up: the "loading" pens */
        bg = 0;
        fg = dim = ncolors - 1;
    }
    bar_h = g->font_h + GU(g, 5);
    bar_y = g->h - bar_h;
    if (!bar_bm) {
        bar_bm = AllocBitMap(g->w, bar_h, g->depth, 0, g->rp->BitMap);
        if (bar_bm)
            BltBitMap(g->rp->BitMap, 0, bar_y, bar_bm, 0, 0, g->w, bar_h, 0xC0, 0xFF, NULL);
    }
    gfx_dirty();
    SetWriteMask(g->rp, 0xFF);
    SetAPen(g->rp, GPEN(bg));
    RectFill(g->rp, 0, bar_y, g->w - 1, g->h - 1);
    if (!clut) {
        SetAPen(g->rp, GPEN(PEN_RULE));
        RectFill(g->rp, 0, bar_y, g->w - 1, bar_y + GU(g, 1) - 1);
    }
    ty = bar_y + (bar_h - g->font_h + GU(g, 1)) / 2;
    tw = gfx_textlen(g, title, TS_BOLD);
    if (btns) {
        /* the mouse's: previous, play or pause, next at the right end,
         * where the keys' hint would be */
        WORD b0;
        btn_cw = bar_h + GU(g, 4);
        btn_n = btn_full ? VB_COUNT : VB_FULL;
        b0 = g->w - GU(g, 4) - btn_n * btn_cw;
        bar_buttons(g, b0, fg);
        hw = g->w - b0;                     /* (what the text keeps clear of) */
        hint = NULL;
    } else if (hint && hint[0])
        hw = gfx_textlen(g, hint, TS_SMALL);
    gfx_text(g, x, ty, title, fg, bg, g->w - x - (btns ? hw + GU(g, 6) : x), TS_BOLD);
    x += tw + GU(g, 10);
    if (detail && detail[0] && x < g->w - GU(g, 6) - (btns ? hw : 0)) {
        WORD dw = gfx_textlen(g, detail, TS_SMALL);
        gfx_text(g, x, ty, detail, dim, bg, g->w - GU(g, 6) - (btns ? hw : 0) - x, TS_SMALL);
        x += dw + GU(g, 12);
    }
    if (hint && hw && x + hw <= g->w - GU(g, 6))
        gfx_text(g, g->w - GU(g, 6) - hw, ty, hint, dim, bg, 0, TS_SMALL);
    /* the spinner's place, if one turns: left of the hint (or the
     * buttons), after the text */
    spin_x = g->w - (btns ? hw : GU(g, 6) + (hw ? hw + GU(g, 10) : 0)) - bar_h / 2;
    if (spin_x - bar_h / 2 < x)
        spin_x = -1;
}

/* The buttons from x0: previous, play (the slideshow paused) or pause,
 * next, in pen fg, drawn as video's are. */
static void bar_buttons(struct Gfx *g, WORD x0, UBYTE fg)
{
    WORD s = bar_h - GU(g, 7), cy = bar_y + (bar_h + GU(g, 1)) / 2, t = s > 14 ? 2 : 1, i, j;
    if (s < 6)
        s = 6;
    SetAPen(g->rp, GPEN(fg));
    for (i = 0; i < btn_n; i++) {
        WORD ix = x0 + i * btn_cw + (btn_cw - s) / 2, iy = cy - s / 2;
        btn_x[i] = x0 + i * btn_cw;
        switch (i) {
        case VB_PREV:                       /* a bar, then a triangle pointing left */
            RectFill(g->rp, ix + s / 5, iy + s / 6, ix + s / 5 + t, iy + s - s / 6);
            for (j = 0; j <= s / 3; j++) {  /* (its base at the right, its tip at the bar) */
                Move(g->rp, ix + s - s / 5 - j, cy - (s / 3 - j));
                Draw(g->rp, ix + s - s / 5 - j, cy + (s / 3 - j));
            }
            break;
        case VB_PLAY:
            if (!btn_playing)               /* paused: play */
                for (j = 0; j <= s / 3; j++) {
                    Move(g->rp, ix + s / 3 + j, iy + s / 6 + j);
                    Draw(g->rp, ix + s / 3 + j, iy + s - s / 6 - j);
                }
            else {
                RectFill(g->rp, ix + s / 4, iy + s / 6, ix + s / 4 + t, iy + s - s / 6);
                RectFill(g->rp, ix + s - s / 4 - t, iy + s / 6, ix + s - s / 4, iy + s - s / 6);
            }
            break;
        case VB_NEXT:                       /* a triangle pointing right, then a bar */
            for (j = 0; j <= s / 3; j++) {
                Move(g->rp, ix + s / 5 + j, cy - (s / 3 - j));
                Draw(g->rp, ix + s / 5 + j, cy + (s / 3 - j));
            }
            RectFill(g->rp, ix + s - s / 5 - t, iy + s / 6, ix + s - s / 5, iy + s - s / 6);
            break;
        case VB_FULL: {                     /* four corners, as video's: out, or in to come back */
            WORD a = s / 5, L = s / 4, c, ccx = ix + s / 2, ccy = iy + s / 2, gap = s / 8 + t;
            for (c = 0; c < 4; c++) {
                WORD dx = c & 1 ? -1 : 1, dy = c & 2 ? -1 : 1, px, py;
                if (btn_full == 1) {
                    px = dx > 0 ? ix + a : ix + s - 1 - a;
                    py = dy > 0 ? iy + a : iy + s - 1 - a;
                } else {
                    px = ccx - dx * gap;
                    py = ccy - dy * gap;
                    dx = -dx;
                    dy = -dy;
                }
                RectFill(g->rp, dx > 0 ? px : px - L + 1, dy > 0 ? py : py - t + 1,
                         dx > 0 ? px + L - 1 : px, dy > 0 ? py + t - 1 : py);
                RectFill(g->rp, dx > 0 ? px : px - t + 1, dy > 0 ? py : py - L + 1,
                         dx > 0 ? px + t - 1 : px, dy > 0 ? py + L - 1 : py);
            }
            break;
        }
        }
    }
}

void view_bar_buttons(BOOL on, BOOL playing, int full)
{
    btns = on;
    btn_playing = playing;
    btn_full = full;
}

BOOL view_bar_has_buttons(void)
{
    return bar_bm && btns;
}

int view_bar_hit(struct Gfx *g, WORD x, WORD y)
{
    int i;
    if (!bar_bm || !btns || y < bar_y || y >= g->h)
        return -1;
    for (i = 0; i < btn_n; i++)
        if (x >= btn_x[i] && x < btn_x[i] + btn_cw)
            return i;
    return -1;
}

BOOL view_bar_at(struct Gfx *g, WORD y)
{
    (void)g;
    return bar_bm && y >= bar_y;
}

/* The pointer while the mouse is in use (it moved), hidden again with the
 * bar. */
void view_pointer(struct Gfx *g, BOOL on)
{
    if (on == ptr_shown || !blank)
        return;
    if (on)
        ClearPointer(g->win);
    else
        SetPointer(g->win, blank, 1, 16, 0, 0);
    ptr_shown = on;
}

BOOL view_pointer_shown(void)
{
    return ptr_shown;
}

/* Eight dots round (cx, cy), radius r, the one at phase lit and larger. */
static void spin_dots(struct Gfx *g, WORD cx, WORD cy, WORD r, UWORD phase, UBYTE fg, UBYTE dim)
{
    static const BYTE dx[8] = { 0, 70, 100, 70, 0, -70, -100, -70 };
    static const BYTE dy[8] = { -100, -70, 0, 70, 100, 70, 0, -70 };
    WORD d = r / 4 > 1 ? r / 4 : 1, i;
    for (i = 0; i < 8; i++) {
        WORD x = cx + dx[i] * r / 100, y = cy + dy[i] * r / 100;
        BOOL lit = i == (phase & 7);
        WORD h = lit ? d : (d > 1 ? d - 1 : 0);
        SetAPen(g->rp, GPEN(lit ? fg : dim));
        RectFill(g->rp, x - h, y - h, x + h, y + h);
    }
}

static void bar_pens(UBYTE *bg, UBYTE *fg, UBYTE *dim)
{
    *bg = clut ? pen_bg : PEN_BAR;
    *fg = clut ? pen_fg : PEN_WHITE;
    *dim = clut ? pen_dim : PEN_TEXT;
    if (clut && !has_pic) {
        *bg = 0;
        *fg = *dim = ncolors - 1;
    }
}

/* A turning spinner while the next picture comes: in the bar when it is
 * up, else small in the bottom right corner, over the picture shown (put
 * back by view_spin_hide), so a slideshow's picture stays in view. */
void view_spin(struct Gfx *g, UWORD phase)
{
    UBYTE bg, fg, dim;
    bar_pens(&bg, &fg, &dim);
    gfx_dirty();
    SetWriteMask(g->rp, 0xFF);
    if (bar_bm || (!has_pic && spin_x >= 0 && bar_h)) {
        WORD r = bar_h / 3;
        if (spin_x < 0)
            return;
        SetAPen(g->rp, GPEN(bg));
        RectFill(g->rp, spin_x - r - 2, bar_y + 2, spin_x + r + 2, bar_y + bar_h - 1);
        spin_dots(g, spin_x, bar_y + bar_h / 2, r, phase, fg, dim);
        return;
    }
    if (!spin_bm) {
        spin_s = g->font_h + GU(g, 4);
        spin_cx = g->w - GU(g, 6) - spin_s / 2;
        spin_cy = g->h - GU(g, 4) - spin_s / 2;
        if (!(spin_bm = AllocBitMap(spin_s, spin_s, g->depth, 0, g->rp->BitMap)))
            return;
        BltBitMap(g->rp->BitMap, spin_cx - spin_s / 2, spin_cy - spin_s / 2, spin_bm, 0, 0,
                  spin_s, spin_s, 0xC0, 0xFF, NULL);
    }
    SetAPen(g->rp, GPEN(bg));
    RectFill(g->rp, spin_cx - spin_s / 2, spin_cy - spin_s / 2, spin_cx - spin_s / 2 + spin_s - 1,
             spin_cy - spin_s / 2 + spin_s - 1);
    spin_dots(g, spin_cx, spin_cy, spin_s * 3 / 8, phase, fg, dim);
}

/* The corner spinner gone, what it covered put back. */
void view_spin_hide(struct Gfx *g)
{
    if (!spin_bm)
        return;
    gfx_dirty();
    BltBitMapRastPort(spin_bm, 0, 0, g->rp, spin_cx - spin_s / 2, spin_cy - spin_s / 2, spin_s,
                      spin_s, 0xC0);
    WaitBlit();
    FreeBitMap(spin_bm);
    spin_bm = NULL;
}

/* ---- scale */
/* Pixel shape in units where a lowres PAL pixel is 4 x 4: width 4 lowres,
 * 2 hires, 1 superhires; height 4 non-laced, 2 laced. */
static void src_units(const struct PicInfo *pi, ULONG *wu, ULONG *hu)
{
    BOOL ilbm = amce_stricmp(pi->fmt, "ILBM") == 0;
    ULONG m = pi->mode;
    *wu = 4;
    *hu = 4;
    if (ilbm) {
        if ((m & SUPER_KEY) == SUPER_KEY || pi->w >= 1280)
            *wu = 1;
        else if ((m & HIRES_KEY) || pi->w >= 640)
            *wu = 2;
        if ((m & LORESLACE_KEY) || pi->h >= 400)
            *hu = 2;
    }
    /* AGS-style grabs: 2.5:1 or wider is every other line of a screen */
    if ((ULONG)pi->w * *hu * 2 >= (ULONG)pi->h * *wu * 5)
        *hu *= 2;
}

static ULONG gcd(ULONG a, ULONG b)
{
    while (b) {
        ULONG t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

void view_fit(UWORD gw, UWORD gh, BOOL tall, const struct PicInfo *pi, ULONG *dw, ULONG *dh,
              char *scale, LONG max)
{
    ULONG wu, hu, swu = 1, shu = tall ? 2 : 1, bx, by, c, k, ew, eh;
    char t[24];
    src_units(pi, &wu, &hu);
    bx = wu * shu;
    by = hu * swu;
    c = gcd(bx, by);
    bx /= c;
    by /= c;
    ew = (ULONG)pi->w * bx;
    eh = (ULONG)pi->h * by;
    k = gw / ew;
    if (gh / eh < k)
        k = gh / eh;
    if (k >= 1) {
        *dw = ew * k;
        *dh = eh * k;
        if (bx == by)
            sprintf(t, "%lux", k);
        else
            sprintf(t, "%lu x %lu", bx * k, by * k);
    } else {
        *dw = gw;
        *dh = eh * gw / ew;
        if (*dh > gh) {
            *dh = gh;
            *dw = ew * gh / eh;
        }
        sprintf(t, "%lu%%", *dw * 100 / (ULONG)pi->w);
    }
    if (!*dw)
        *dw = 1;
    if (!*dh)
        *dh = 1;
    if (scale)
        amce_strlcpy(scale, t, max);
}

static void fit(struct Gfx *g, const struct PicInfo *pi, ULONG *dw, ULONG *dh, char *scale, LONG max)
{
    view_fit(g->w, g->h, gfx_mode_tall(g->mode), pi, dw, dh, scale, max);
}

/* ---- showing a picture */
static void drop_picture(struct Gfx *g)
{
    view_bar_hide(g);
    if (clut && has_pic)
        fade(g, shown, FALSE);
    if (obj) {
        DisposeDTObject(obj);
        obj = NULL;
    }
    has_pic = FALSE;
    if (clut) {
        all_black(g);
        fill(g, 0);
    } else {
        fill(g, black_pen >= 0 ? black_pen : GPEN(PEN_BG));
    }
}

/* The picture's own pixels as a bitmap the screen can take, for the direct
 * route: the datatype's planar bitmap on a native screen, else its pixels
 * read as colour numbers and written into a bitmap like the screen's. */
static struct BitMap *direct_bitmap(struct Gfx *g, Object *o, ULONG sw, ULONG sh, BOOL *ours)
{
    struct BitMap *src = NULL, *bm;
    UBYTE *buf;
    struct RastPort trp;
    struct pdtBlitPixelArray pbpa;
    *ours = FALSE;
    GetDTAttrs(o, PDTA_BitMap, (ULONG)&src, TAG_END);
    if (src && g->planar)
        return src;
    bm = AllocBitMap(sw, sh, g->depth, BMF_CLEAR, g->rp->BitMap);
    if (!bm)
        return NULL;
    if (src) {                              /* planar source on a chunky screen */
        BltBitMap(src, 0, 0, bm, 0, 0, sw, sh, 0xC0, 0xFF, NULL);
        WaitBlit();
        *ours = TRUE;
        return bm;
    }
    if (GfxBase->LibNode.lib_Version < 40 || !(buf = AllocVec(sw * sh, MEMF_ANY))) {
        FreeBitMap(bm);
        return NULL;
    }
    pbpa.MethodID = PDTM_READPIXELARRAY;
    pbpa.pbpa_PixelData = buf;
    pbpa.pbpa_PixelFormat = PBPAFMT_LUT8;
    pbpa.pbpa_PixelArrayMod = sw;
    pbpa.pbpa_Left = 0;
    pbpa.pbpa_Top = 0;
    pbpa.pbpa_Width = sw;
    pbpa.pbpa_Height = sh;
    if (!DoMethodA(o, (Msg)&pbpa)) {       /* not DoDTMethodA: see gfx.c own_remap */
        FreeVec(buf);
        FreeBitMap(bm);
        return NULL;
    }
    InitRastPort(&trp);
    trp.BitMap = bm;
    WriteChunkyPixels(&trp, 0, 0, sw - 1, sh - 1, buf, sw);
    FreeVec(buf);
    *ours = TRUE;
    return bm;
}

static void show_message(struct Gfx *g, const char *s)
{
    if (kept) {                             /* (the one left up for a transition) */
        kept = FALSE;
        fill(g, black_pen >= 0 ? black_pen : GPEN(PEN_BG));
    }
    if (clut) {
        ULONG grey = 0xAAAAAAAAUL;
        SetRGB32(&g->scr->ViewPort, ncolors - 1, grey, grey, grey);
        shown[(ncolors - 1) * 3] = shown[(ncolors - 1) * 3 + 1] = shown[(ncolors - 1) * 3 + 2] = grey;
    }
    view_bar(g, s, "", "");
    if (bar_bm) {                           /* nothing under it worth keeping */
        FreeBitMap(bar_bm);
        bar_bm = NULL;
    }
}

/* What the spinners cover: worth nothing once a new picture comes. */
static void spin_forget(void)
{
    if (spin_bm) {
        WaitBlit();
        FreeBitMap(spin_bm);
        spin_bm = NULL;
    }
}

/* ---- transitions (true colour): from what is on the screen in u (the
 * last picture, or black) to nb, the next one as u is to look. Tiles: nb
 * copied a tile at a time, at random or swept across, over TR_TILES_MS.
 * Fades: the pixels mixed by the CPU, as many steps as fit in TR_FADE_MS
 * (a slow Amiga: few); one that takes too long goes on in tiles. */
#define TR_TILES_MS 600
#define TR_FADE_MS 800
#define TR_SLOW_MS 300          /* a fade's step longer than this: tiles instead */

static void tr_show(struct Gfx *g, WORD x, WORD y, WORD w, WORD h)
{
    gfx_dirty();
    if (g->wb)
        gfx_window_show_rect(g, x, y, x + w - 1, y + h - 1);   /* (a window: its buffer, copied in) */
}

static void tr_tiles(struct Gfx *g, struct BitMap *nb, WORD ux, WORD uy, WORD uw, WORD uh,
                     BOOL sweep, ULONG ms)
{
    WORD t = g->h >= 480 ? 32 : 16, nx = (uw + t - 1) / t, ny = (uh + t - 1) / t;
    LONG n = (LONG)nx * ny, i, done = 0;
    UWORD *order = AllocVec(n * sizeof(UWORD), MEMF_ANY);
    ULONG t0 = time_ms(), seed = t0 | 1;
    if (!order) {
        BltBitMapRastPort(nb, 0, 0, g->rp, ux, uy, uw, uh, 0xC0);
        tr_show(g, ux, uy, uw, uh);
        return;
    }
    if (sweep) {
        /* by diagonals, top left to bottom right */
        LONG d, k = 0;
        for (d = 0; d < nx + ny - 1; d++)
            for (i = 0; i < nx; i++)
                if (d - i >= 0 && d - i < ny)
                    order[k++] = (UWORD)((d - i) * nx + i);
    } else {
        for (i = 0; i < n; i++)
            order[i] = (UWORD)i;
        for (i = n - 1; i > 0; i--) {       /* shuffled */
            LONG j;
            UWORD x;
            seed = seed * 1103515245UL + 12345;
            j = (LONG)((seed >> 8) % (ULONG)(i + 1));
            x = order[i];
            order[i] = order[j];
            order[j] = x;
        }
    }
    while (done < n) {
        ULONG el = time_ms() - t0;
        LONG upto = el >= ms ? n : (LONG)(n * el / ms) + 1;
        if (upto > n)
            upto = n;
        for (; done < upto; done++) {
            WORD tx = order[done] % nx, ty = order[done] / nx, w = t, h = t;
            if (tx * t + w > uw)
                w = uw - tx * t;
            if (ty * t + h > uh)
                h = uh - ty * t;
            BltBitMapRastPort(nb, tx * t, ty * t, g->rp, ux + tx * t, uy + ty * t, w, h, 0xC0);
        }
        tr_show(g, ux, uy, uw, uh);
        WaitTOF();
    }
    FreeVec(order);
}

/* FALSE: no memory (or no cybergraphics), nothing done. */
static BOOL tr_fade(struct Gfx *g, struct BitMap *nb, WORD ux, WORD uy, WORD uw, WORD uh,
                    BOOL through)
{
    ULONG n = (ULONG)uw * uh * 3, t0, i;
    UBYTE *o, *nw, *out, ta[256], tb[256];
    struct RastPort nrp;
    char m[100];
    int steps = 0;
    if (!CyberGfxBase || !(o = AllocVec(n * 3, MEMF_ANY)))
        return FALSE;
    nw = o + n;
    out = nw + n;
    InitRastPort(&nrp);
    nrp.BitMap = nb;
    ReadPixelArray(o, 0, 0, uw * 3, g->rp, ux, uy, uw, uh, RECTFMT_RGB);
    ReadPixelArray(nw, 0, 0, uw * 3, &nrp, 0, 0, uw, uh, RECTFMT_RGB);
    t0 = time_ms();
    for (;;) {
        ULONG el = time_ms() - t0, k, ts = time_ms();
        if (el >= TR_FADE_MS)
            break;
        k = el * 256 / TR_FADE_MS;          /* 0..255: how far */
        if (through) {
            /* the old to black, then black to the new */
            ULONG a = k < 128 ? 255 - k * 2 : 0, b = k < 128 ? 0 : (k - 128) * 2;
            for (i = 0; i < 256; i++) {
                ta[i] = (UBYTE)(i * a >> 8);
                tb[i] = (UBYTE)(i * b >> 8);
            }
        } else {
            for (i = 0; i < 256; i++) {
                ta[i] = (UBYTE)(i * (256 - k) >> 8);
                tb[i] = (UBYTE)(i * k >> 8);
            }
        }
        for (i = 0; i < n; i++)
            out[i] = (UBYTE)(ta[o[i]] + tb[nw[i]]);
        WritePixelArray(out, 0, 0, uw * 3, g->rp, ux, uy, uw, uh, RECTFMT_RGB);
        tr_show(g, ux, uy, uw, uh);
        steps++;
        if (time_ms() - ts > TR_SLOW_MS) {
            /* too slow here for a fade worth the name: the rest in tiles */
            sprintf(m, "a fade's step took %lu ms (%ld x %ld): the rest in tiles",
                    (unsigned long)(time_ms() - ts), (long)uw, (long)uh);
            amce_log("viewer", m);
            FreeVec(o);
            tr_tiles(g, nb, ux, uy, uw, uh, FALSE, TR_TILES_MS / 2);
            return TRUE;
        }
    }
    FreeVec(o);
    BltBitMapRastPort(nb, 0, 0, g->rp, ux, uy, uw, uh, 0xC0);   /* the last step: the new one */
    tr_show(g, ux, uy, uw, uh);
    sprintf(m, "%s: %ld steps in %lu ms (%ld x %ld)", through ? "fade" : "cross-fade",
            (long)steps, (unsigned long)(time_ms() - t0), (long)uw, (long)uh);
    amce_log("viewer", m);
    return TRUE;
}

/* The new picture (tmp, dw x dh) shown at its place by the transition
 * chosen, from what is on screen; FALSE if it could not be (no memory):
 * the caller draws it at once. */
static BOOL transition(struct Gfx *g, struct BitMap *tmp, WORD dw, WORD dh)
{
    WORD nx = (g->w - dw) / 2, ny = (g->h - dh) / 2, ux = nx, uy = ny, ux1 = nx + dw,
         uy1 = ny + dh, uw, uh;
    struct BitMap *nb;
    if (view_transition == VT_CUT || clut)
        return FALSE;
    if (kept) {                             /* the old picture's place too */
        if (pic_x < ux)
            ux = pic_x;
        if (pic_y < uy)
            uy = pic_y;
        if (pic_x + pic_w > ux1)
            ux1 = pic_x + pic_w;
        if (pic_y + pic_h > uy1)
            uy1 = pic_y + pic_h;
    }
    uw = ux1 - ux;
    uh = uy1 - uy;
    if (!(nb = AllocBitMap(uw, uh, g->depth, BMF_CLEAR, g->rp->BitMap)))
        return FALSE;
    /* (around it black: a cleared bitmap is black in true colour) */
    BltBitMap(tmp, 0, 0, nb, nx - ux, ny - uy, dw, dh, 0xC0, 0xFF, NULL);
    WaitBlit();
    if (view_transition == VT_TILES || view_transition == VT_WIPE ||
        !tr_fade(g, nb, ux, uy, uw, uh, view_transition == VT_FADE))
        tr_tiles(g, nb, ux, uy, uw, uh, view_transition == VT_WIPE, TR_TILES_MS);
    WaitBlit();
    FreeBitMap(nb);
    return TRUE;
}

BOOL view_show(struct Gfx *g, const char *path, const char *label, struct PicInfo *pi,
               char *scale, LONG scalemax, APTR pre, BOOL pre_remapped,
               struct Thumb *pre_thumb, const struct PicInfo *pre_info)
{
    struct BitMapHeader *bmh = NULL;
    struct BitMap *src = NULL, *tmp;
    struct BitScaleArgs bsa;
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    BOOL direct = FALSE, own_src = FALSE, made = FALSE;
    Object *deep = NULL;                    /* decoded, for the own route */
    ULONG *target = NULL, sw, sh, dw, dh;
    UBYTE pens[256];
    LONG npens = 0, i;
    Object *o = NULL;
    char msg[160];

    if (has_pic && !clut && view_transition != VT_CUT) {
        /* the bar and the spinner off it first, and seen to be: a window
         * shows only the picture's part during the transition, which
         * drew into a bar still showing beside it */
        view_spin_hide(g);
        view_bar_hide(g);
        gfx_present(g);
    }
    spin_forget();
    if (has_pic && !clut && view_transition != VT_CUT) {
        /* left up, for the transition to this one */
        if (obj) {
            DisposeDTObject(obj);
            obj = NULL;
        }
        has_pic = FALSE;
        kept = TRUE;
    } else {
        kept = FALSE;
        drop_picture(g);
    }
    if (!DataTypesBase) {
        pic_dispose(pre);
        return FALSE;
    }
    if (pre_thumb && !clut) {
        thumb_free(pre_thumb);
        pre_thumb = NULL;
    }
    if (!pre && !pre_thumb && !kept) {
        sprintf(msg, _("Loading %.80s"), label);
        show_message(g, msg);
    }

    /* direct: a picture that fits the colour table, not remapped (pre: one
     * the loader decoded ahead, unremapped on a colour-table screen) */
    if (clut && !pre_thumb && (!pre || !pre_remapped)) {
        if (pre) {
            o = pre;
            pre = NULL;
        } else {
            o = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE, PDTA_Remap, FALSE,
                            PDTA_DestMode, PMODE_V43, TAG_END);
            if (o)
                DoDTMethod(o, NULL, NULL, DTM_PROCLAYOUT, NULL, (ULONG)TRUE);
        }
        if (o) {
            ULONG mode = 0, ncol = 0, *cregs = NULL;
            GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, PDTA_ModeID, (ULONG)&mode,
                       PDTA_CRegs, (ULONG)&cregs, PDTA_NumColors, (ULONG)&ncol, TAG_END);
            if (mode == (ULONG)INVALID_ID)
                mode = 0;
            if (bmh && bmh->bmh_Width && bmh->bmh_Height && cregs && ncol &&
                bmh->bmh_Depth <= 8 && (1UL << bmh->bmh_Depth) <= ncolors && !(mode & HAM_KEY)) {
                sw = bmh->bmh_Width;
                sh = bmh->bmh_Height;
                src = direct_bitmap(g, o, sw, sh, &own_src);
                if (src) {
                    LONG n = 1L << bmh->bmh_Depth;
                    pic_info(o, bmh, pi);
                    target = AllocVec(ncolors * 3 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
                    if (target) {
                        if (ncol > (ULONG)n)
                            ncol = n;
                        CopyMem(cregs, target, ncol * 3 * sizeof(ULONG));
                        if ((mode & EXTRAHALFBRITE_KEY) && n == 64 && ncol <= 32)
                            for (i = 0; i < 32 * 3; i++)
                                target[32 * 3 + i] = (target[i] >> 1) & 0x7F7F7F7FUL;
                        for (i = 0; i < (ncol > 32 || n <= 32 ? (LONG)ncol : n); i++)
                            pens[npens++] = i;
                        /* the border: a spare register set to black, or
                         * the picture's own darkest colour */
                        if (n < ncolors) {
                            pen_border = ncolors - 1;
                        } else {
                            ULONG lo = ~0UL;
                            for (i = 0; i < npens; i++)
                                if (luma(target + pens[i] * 3) < lo) {
                                    lo = luma(target + pens[i] * 3);
                                    pen_border = pens[i];
                                }
                        }
                        direct = TRUE;
                    }
                }
            }
            if (!direct) {
                if (src && own_src)
                    FreeBitMap(src);
                src = NULL;
                own_src = FALSE;
                deep = o;                   /* for the own route */
                o = NULL;
            }
        }
    }

    /* own: its own colours at the size it is shown */
    if (!direct && (deep || pre_thumb)) {
        struct Thumb *t = NULL;
        bmh = NULL;
        if (deep)
            GetDTAttrs(deep, PDTA_BitMapHeader, (ULONG)&bmh, TAG_END);
        if (pre_thumb || (bmh && bmh->bmh_Width && bmh->bmh_Height)) {
            if (pre_thumb) {
                *pi = *pre_info;
                fit(g, pi, &dw, &dh, scale, scalemax);
                t = pre_thumb;              /* made ahead, by the loader */
                pre_thumb = NULL;
                dw = t->w;
                dh = t->h;
            } else {
                pic_info(deep, bmh, pi);
                fit(g, pi, &dw, &dh, scale, scalemax);
                t = thumb_make(deep, dw, dh, ncolors - 1);
            }
            if (t &&
                (target = AllocVec(ncolors * 3 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR)) &&
                (src = AllocBitMap(dw, dh, g->depth, BMF_CLEAR, g->rp->BitMap))) {
                ULONG n = (ULONG)t->mod * t->h;
                for (i = 0; i < t->ncol; i++) {
                    ULONG c = t->pal[i];
                    target[(i + 1) * 3] = ((c >> 16) & 255) * 0x01010101UL;
                    target[(i + 1) * 3 + 1] = ((c >> 8) & 255) * 0x01010101UL;
                    target[(i + 1) * 3 + 2] = (c & 255) * 0x01010101UL;
                    pens[npens++] = i + 1;
                }
                for (i = 0; i < (LONG)n; i++)
                    t->px[i]++;             /* register 0 is the border's */
                gfx_chunky(g, src, t->px, dw, dh, t->mod);
                own_src = TRUE;
                pen_border = 0;
                sw = dw;
                sh = dh;
                direct = made = TRUE;
            } else {
                FreeVec(target);
                target = NULL;
            }
            thumb_free(t);
        }
        if (deep)
            DisposeDTObject(deep);
        deep = NULL;
    }
    thumb_free(pre_thumb);                  /* not used (cannot be) */

    /* remap: into the screen's free pens (or its true colour) */
    if (!direct) {
        bmh = NULL;
        if (pre && pre_remapped && !clut) {
            o = pre;                        /* decoded ahead into true colour */
            pre = NULL;
        } else {
            pic_dispose(pre);
            pre = NULL;
            o = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE, PDTA_Screen, (ULONG)g->scr,
                            PDTA_Remap, TRUE, PDTA_DestMode, PMODE_V43,
                            OBP_Precision, PRECISION_IMAGE, PDTA_FreeSourceBitMap, TRUE, TAG_END);
            if (o && !DoDTMethod(o, NULL, NULL, DTM_PROCLAYOUT, NULL, (ULONG)TRUE)) {
                DisposeDTObject(o);
                o = NULL;
            }
        }
        if (!o && !clut && (src = jpeg_bitmap(path, g->w, g->h, g->rp->BitMap, &sw, &sh, pi))) {
            /* no datatype took it (a camera's Exif JPEG, say): read by
             * AmigaMCE itself, in true colour */
            own_src = TRUE;
            amce_log("viewer", "no datatype took it: the JPEG read by AmigaMCE (jdec)");
        } else {
            if (!o) {
                sprintf(msg, _("Could not show %.80s"), label);
                show_message(g, msg);
                return FALSE;
            }
            GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, PDTA_DestBitMap, (ULONG)&src, TAG_END);
            if (!src)
                GetDTAttrs(o, PDTA_BitMap, (ULONG)&src, TAG_END);
            if (!src || !bmh || !bmh->bmh_Width || !bmh->bmh_Height) {
                DisposeDTObject(o);
                sprintf(msg, _("Could not show %.80s"), label);
                show_message(g, msg);
                return FALSE;
            }
            sw = bmh->bmh_Width;
            sh = bmh->bmh_Height;
            pic_info(o, bmh, pi);
        }
        if (clut) {
            UBYTE *alloc = NULL;
            ULONG na = 0;
            target = AllocVec(ncolors * 3 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
            if (target)
                GetRGB32(cm, 0, ncolors, target);
            GetDTAttrs(o, PDTA_NumAlloc, (ULONG)&na, PDTA_Allocated, (ULONG)&alloc, TAG_END);
            for (i = 0; alloc && i < (LONG)na && npens < 256; i++)
                pens[npens++] = alloc[i];
            if (target) {
                target[0] = target[1] = target[2] = 0;      /* pen 0: the border */
                if (!npens)
                    for (i = 1; i < ncolors; i++)
                        pens[npens++] = i;
            }
            pen_border = 0;
        }
    }

    if (!made)
        fit(g, pi, &dw, &dh, scale, scalemax);
    tmp = made ? src : AllocBitMap(dw, dh, g->depth, BMF_CLEAR, g->rp->BitMap);
    if (!tmp) {
        if (own_src)
            FreeBitMap(src);
        if (o)
            DisposeDTObject(o);
        FreeVec(target);
        sprintf(msg, _("Not enough memory for %.70s"), label);
        show_message(g, msg);
        return FALSE;
    }
    memset(&bsa, 0, sizeof(bsa));
    bsa.bsa_DestWidth = dw;
    bsa.bsa_DestHeight = dh;
    bsa.bsa_SrcWidth = sw;
    bsa.bsa_SrcHeight = sh;
    bsa.bsa_XSrcFactor = sw;
    bsa.bsa_YSrcFactor = sh;
    bsa.bsa_XDestFactor = dw;
    bsa.bsa_YDestFactor = dh;
    bsa.bsa_SrcBitMap = src;
    bsa.bsa_DestBitMap = tmp;
    if (made)
        own_src = FALSE;                    /* already its size: tmp is it */
    else
        BitMapScale(&bsa);

    if (clut) {
        all_black(g);                       /* drawn unseen, then faded in */
        fill(g, pen_border);
        if (direct) {
            CopyMem(target, shown, ncolors * 3 * sizeof(ULONG));
            if (pen_border == ncolors - 1 && (1UL << pi->depth) < ncolors)
                shown[pen_border * 3] = shown[pen_border * 3 + 1] = shown[pen_border * 3 + 2] = 0;
        } else if (target) {
            CopyMem(target, shown, ncolors * 3 * sizeof(ULONG));
        }
        pick_bar_pens(pens, npens);
    } else if (!kept && view_transition != VT_CUT) {
        fill(g, black_pen >= 0 ? black_pen : GPEN(PEN_BG));    /* (faded in from black) */
    } else if (view_transition == VT_CUT) {
        fill(g, black_pen >= 0 ? black_pen : GPEN(PEN_BG));
    }
    gfx_dirty();
    if (!transition(g, tmp, (WORD)bsa.bsa_DestWidth, (WORD)bsa.bsa_DestHeight)) {
        if (kept)
            fill(g, black_pen >= 0 ? black_pen : GPEN(PEN_BG));
        BltBitMapRastPort(tmp, 0, 0, g->rp, (g->w - bsa.bsa_DestWidth) / 2,
                          (g->h - bsa.bsa_DestHeight) / 2, bsa.bsa_DestWidth, bsa.bsa_DestHeight,
                          0xC0);
    }
    kept = FALSE;
    pic_x = (WORD)((g->w - bsa.bsa_DestWidth) / 2);
    pic_y = (WORD)((g->h - bsa.bsa_DestHeight) / 2);
    pic_w = (WORD)bsa.bsa_DestWidth;
    pic_h = (WORD)bsa.bsa_DestHeight;
    WaitBlit();
    FreeBitMap(tmp);
    if (own_src)
        FreeBitMap(src);
    FreeVec(target);
    if (direct && o) {
        DisposeDTObject(o);                 /* its colours are in ours now */
        o = NULL;
    }
    obj = o;
    has_pic = TRUE;
    if (clut)
        fade(g, shown, TRUE);
    return TRUE;
}

void view_end(struct Gfx *g)
{
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    view_spin_hide(g);
    view_bar_hide(g);
    if (clut && has_pic)
        fade(g, shown, FALSE);
    if (obj) {
        DisposeDTObject(obj);
        obj = NULL;
    }
    has_pic = FALSE;
    if (clut) {
        all_black(g);
        if (pen0_held)
            ReleasePen(cm, 0);
        pen0_held = FALSE;
        pens_reclaim(g);
    } else if (black_pen >= 0) {
        ReleasePen(cm, black_pen);
        black_pen = -1;
    }
    (void)black_held;
    if (blank) {
        ClearPointer(g->win);
        FreeMem(blank, 6 * sizeof(UWORD));
        blank = NULL;
    }
}

void view_restore(struct Gfx *g)
{
    if (clut && saved) {
        fade(g, saved + 1, TRUE);
        LoadRGB32(&g->scr->ViewPort, saved);
    }
    FreeVec(saved);
    FreeVec(shown);
    FreeVec(work);
    saved = shown = work = NULL;
}
