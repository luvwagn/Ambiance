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
 *   remap:  a deeper one (JPEG, true-colour PNG, HAM on a non-HAM screen)
 *           is remapped by its datatype into every pen but pen 0, which
 *           stays black for the border.
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

extern struct GfxBase *GfxBase;
extern struct Library *DataTypesBase;

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
static UBYTE pen_bg, pen_fg, pen_dim, pen_border;
static BOOL has_pic;            /* a picture is up */
static struct BitMap *bar_bm;   /* what the bar covers */
static WORD bar_y, bar_h;

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

static void fill(struct Gfx *g, UBYTE pen)
{
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
        fill(g, black_pen >= 0 ? black_pen : PEN_BG);
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
    if (!bar_bm)
        return;
    BltBitMapRastPort(bar_bm, 0, 0, g->rp, 0, bar_y, g->w, bar_h, 0xC0);
    WaitBlit();
    FreeBitMap(bar_bm);
    bar_bm = NULL;
}

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
    SetWriteMask(g->rp, 0xFF);
    SetAPen(g->rp, bg);
    RectFill(g->rp, 0, bar_y, g->w - 1, g->h - 1);
    if (!clut) {
        SetAPen(g->rp, PEN_RULE);
        RectFill(g->rp, 0, bar_y, g->w - 1, bar_y + GU(g, 1) - 1);
    }
    ty = bar_y + (bar_h - g->font_h + GU(g, 1)) / 2;
    tw = gfx_textlen(g, title, TS_BOLD);
    if (hint && hint[0])
        hw = gfx_textlen(g, hint, TS_SMALL);
    gfx_text(g, x, ty, title, fg, bg, g->w - 2 * x, TS_BOLD);
    x += tw + GU(g, 10);
    if (detail && detail[0] && x < g->w - GU(g, 6)) {
        WORD dw = gfx_textlen(g, detail, TS_SMALL);
        gfx_text(g, x, ty, detail, dim, bg, g->w - GU(g, 6) - x, TS_SMALL);
        x += dw + GU(g, 12);
    }
    if (hw && x + hw <= g->w - GU(g, 6))
        gfx_text(g, g->w - GU(g, 6) - hw, ty, hint, dim, bg, 0, TS_SMALL);
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

static void fit(struct Gfx *g, const struct PicInfo *pi, ULONG *dw, ULONG *dh, char *scale, LONG max)
{
    ULONG wu, hu, swu = 1, shu = gfx_mode_tall(g->mode) ? 2 : 1, bx, by, c, k, ew, eh;
    char t[24];
    src_units(pi, &wu, &hu);
    bx = wu * shu;
    by = hu * swu;
    c = gcd(bx, by);
    bx /= c;
    by /= c;
    ew = (ULONG)pi->w * bx;
    eh = (ULONG)pi->h * by;
    k = g->w / ew;
    if (g->h / eh < k)
        k = g->h / eh;
    if (k >= 1) {
        *dw = ew * k;
        *dh = eh * k;
        if (bx == by)
            sprintf(t, "%lux", k);
        else
            sprintf(t, "%lu x %lu", bx * k, by * k);
    } else {
        *dw = g->w;
        *dh = eh * g->w / ew;
        if (*dh > g->h) {
            *dh = g->h;
            *dw = ew * g->h / eh;
        }
        sprintf(t, "%lu%%", *dw * 100 / (ULONG)pi->w);
    }
    if (!*dw)
        *dw = 1;
    if (!*dh)
        *dh = 1;
    amce_strlcpy(scale, t, max);
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
        fill(g, black_pen >= 0 ? black_pen : PEN_BG);
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

static BOOL is_jpeg(const char *path)
{
    const char *f = pics_format(path);
    return f && strcmp(f, "JPEG") == 0;
}

static void show_message(struct Gfx *g, const char *s)
{
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

BOOL view_show(struct Gfx *g, const char *path, const char *label, struct PicInfo *pi,
               char *scale, LONG scalemax, APTR pre, BOOL pre_remapped)
{
    struct BitMapHeader *bmh = NULL;
    struct BitMap *src = NULL, *tmp;
    struct BitScaleArgs bsa;
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    BOOL direct = FALSE, own_src = FALSE;
    ULONG *target = NULL, sw, sh, dw, dh;
    UBYTE pens[256];
    LONG npens = 0, i;
    Object *o = NULL;
    char msg[96];

    drop_picture(g);
    if (!DataTypesBase) {
        pic_dispose(pre);
        return FALSE;
    }
    if (!pre) {
        sprintf(msg, "Loading %.80s", label);
        show_message(g, msg);
    }

    /* direct: a picture that fits the colour table, not remapped (pre: one
     * the loader decoded ahead, unremapped on a colour-table screen) */
    if (clut && (pre ? !pre_remapped : !is_jpeg(path))) {
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
                DisposeDTObject(o);
                o = NULL;
            }
        }
    }

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
        if (!o) {
            sprintf(msg, "Could not show %.80s", label);
            show_message(g, msg);
            return FALSE;
        }
        GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, PDTA_DestBitMap, (ULONG)&src, TAG_END);
        if (!src)
            GetDTAttrs(o, PDTA_BitMap, (ULONG)&src, TAG_END);
        if (!src || !bmh || !bmh->bmh_Width || !bmh->bmh_Height) {
            DisposeDTObject(o);
            sprintf(msg, "Could not show %.80s", label);
            show_message(g, msg);
            return FALSE;
        }
        sw = bmh->bmh_Width;
        sh = bmh->bmh_Height;
        pic_info(o, bmh, pi);
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

    fit(g, pi, &dw, &dh, scale, scalemax);
    tmp = AllocBitMap(dw, dh, g->depth, BMF_CLEAR, g->rp->BitMap);
    if (!tmp) {
        if (own_src)
            FreeBitMap(src);
        if (o)
            DisposeDTObject(o);
        FreeVec(target);
        sprintf(msg, "Not enough memory for %.70s", label);
        show_message(g, msg);
        return FALSE;
    }
    memset(&bsa, 0, sizeof(bsa));
    bsa.bsa_SrcWidth = sw;
    bsa.bsa_SrcHeight = sh;
    bsa.bsa_XSrcFactor = sw;
    bsa.bsa_YSrcFactor = sh;
    bsa.bsa_XDestFactor = dw;
    bsa.bsa_YDestFactor = dh;
    bsa.bsa_SrcBitMap = src;
    bsa.bsa_DestBitMap = tmp;
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
    } else {
        fill(g, black_pen >= 0 ? black_pen : PEN_BG);
    }
    BltBitMapRastPort(tmp, 0, 0, g->rp, (g->w - bsa.bsa_DestWidth) / 2,
                      (g->h - bsa.bsa_DestHeight) / 2, bsa.bsa_DestWidth, bsa.bsa_DestHeight, 0xC0);
    WaitBlit();
    FreeBitMap(tmp);
    if (own_src)
        FreeBitMap(src);
    FreeVec(target);
    if (direct) {
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
