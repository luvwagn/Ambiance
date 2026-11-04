/*
 * AmigaMCE - screen, palette, font and drawing primitives
 * Copyright (c) 2026 Brian King
 *
 * Sizes are design units (a 256-line layout) times the screen's scale,
 * GU(g, v); see amce.h. The pixel style enlarges its artwork and font by
 * whole pixels; the smooth style draws pre-rendered anti-aliased art
 * (smooth.c).
 */
#include "amce.h"

#include <string.h>
#include <graphics/modeid.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/diskfont.h>
#include <proto/datatypes.h>
#include <clib/alib_protos.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>
#include <proto/layers.h>
#include <stdio.h>

extern struct GfxBase *GfxBase;
extern struct Library *DiskfontBase, *DataTypesBase, *LayersBase;
struct Library *CyberGfxBase;   /* true-colour screens: thumbnails (WriteLUTPixelArray) */
static BOOL dirty;              /* drawn since the last gfx_present (double buffering) */

static Object *pic_obj;         /* datatype object of the picture on screen */

static void cache_art(struct Gfx *g);
static void letters_free(void);

/* Main menu labels, set by gfx_menu_labels(): the menu column is made just
 * wide enough for the longest one. */
static const char *const *menu_labels;
static int menu_nlabels;

void gfx_menu_labels(const char *const *labels, int n)
{
    menu_labels = labels;
    menu_nlabels = n;
}

static void set_menu_w(struct Gfx *g, UWORD w)
{
    g->menu_w = w;
    g->pane_x = w + GU(g, 6);
    g->pane_w = g->w - g->pane_x - GU(g, 4);
}

static void layout_metrics(struct Gfx *g)
{
    set_menu_w(g, GU(g, 76));               /* until the font is open */
    g->top = GU(g, 26);
    g->row_h = GU(g, 18);
    g->list_top = GU(g, 44);
    g->list_row_h = GU(g, 14);
    g->footer_y = g->h - GU(g, 12);
}

/* Icon at 3, label at 23, then a margin of 6 after the longest label,
 * measured in the menu's own face. */
static void fit_menu(struct Gfx *g)
{
    WORD longest = 0, w;
    int i;
    for (i = 0; i < menu_nlabels; i++)
        if ((w = gfx_textlen(g, menu_labels[i], TS_BOLD)) > longest)
            longest = w;
    if (longest)
        set_menu_w(g, GU(g, 23) + longest + GU(g, 6));
}

static void open_font(struct Gfx *g)
{
    struct TextAttr ta;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;

    g->font = NULL;
    g->font_is_ours = FALSE;
    if (DiskfontBase) {
        ta.ta_Name = (STRPTR)"PROGDIR:Fonts/amce-pixel.font";
        ta.ta_YSize = 12 * g->kint;         /* 12, 24 or 36: whole-pixel sizes */
        ta.ta_Style = FS_NORMAL;
        ta.ta_Flags = 0;
        me->pr_WindowPtr = (APTR)-1;
        g->font = OpenDiskFont(&ta);
        me->pr_WindowPtr = oldwin;
        g->font_is_ours = g->font != NULL;
    }
    if (!g->font) {
        ta.ta_Name = (STRPTR)"topaz.font";
        ta.ta_YSize = 8;
        ta.ta_Style = FS_NORMAL;
        ta.ta_Flags = FPF_ROMFONT;
        g->font = OpenFont(&ta);
    }
    if (g->font) {
        SetFont(g->rp, g->font);
        g->baseline = g->font->tf_Baseline;
        g->font_h = g->font->tf_YSize;
    }
}

/* ------------------------------------------------------------ modes */
enum { K_LORES, K_HIRES, K_LACED, K_RTG };

static const struct {
    const char *key, *label;
    UWORD w, h;
    UBYTE kind, pal;
} modes[SM_COUNT] = {
    { "auto",       "Automatic",                 0,   0,   0,       0 },
    { "ntsc-lores", "320 x 200  NTSC",           320, 200, K_LORES, 0 },
    { "pal-lores",  "320 x 256  PAL",            320, 256, K_LORES, 1 },
    { "ntsc-hires", "640 x 200  NTSC hires",     640, 200, K_HIRES, 0 },
    { "pal-hires",  "640 x 256  PAL hires",      640, 256, K_HIRES, 1 },
    { "ntsc-laced", "640 x 400  NTSC hires laced", 640, 400, K_LACED, 0 },
    { "pal-laced",  "640 x 512  PAL hires laced",  640, 512, K_LACED, 1 },
    { "rtg-640",    "640 x 480  RTG",            640, 480, K_RTG,   0 },
    { "rtg-800",    "800 x 600  RTG",            800, 600, K_RTG,   0 },
    { "rtg-1024",   "1024 x 768  RTG",           1024, 768, K_RTG,  0 },
};

const char *gfx_mode_key(int sm) { return modes[sm].key; }
BOOL gfx_mode_is_rtg(int sm) { return modes[sm].kind == K_RTG; }
BOOL gfx_mode_tall(int sm) { return modes[sm].kind == K_HIRES; }

UWORD gfx_mode_depth(int sm)
{
    UWORD d = 0;
    if (modes[sm].kind == K_RTG)
        caps_find_rtg_mode(modes[sm].w, modes[sm].h, &d);
    return d;
}
const char *gfx_mode_label(int sm) { return modes[sm].label; }

int gfx_mode_from_key(const char *key)
{
    int i;
    for (i = 0; i < SM_COUNT; i++)
        if (amce_stricmp(key, modes[i].key) == 0)
            return i;
    return -1;
}

static ULONG mode_id(int sm)
{
    ULONG mon = modes[sm].pal ? PAL_MONITOR_ID : NTSC_MONITOR_ID;
    switch (modes[sm].kind) {
    case K_LORES: return mon | LORES_KEY;
    case K_HIRES: return mon | HIRES_KEY;
    case K_LACED: return mon | HIRESLACE_KEY;
    default:      return caps_find_rtg_mode(modes[sm].w, modes[sm].h, NULL);
    }
}

/* Why a mode cannot be used here, or NULL when it can. Hires modes need
 * AGA: the UI and logo use 32 pens, more than ECS hires can show. */
const char *gfx_mode_problem(const struct Caps *c, int sm)
{
    ULONG id;
    if (sm == SM_AUTO)
        return NULL;
    if (modes[sm].kind == K_RTG && !c->rtg)
        return "needs a graphics card";
    if ((modes[sm].kind == K_HIRES || modes[sm].kind == K_LACED) && !c->aga)
        return "needs AGA";
    id = mode_id(sm);
    if (id == (ULONG)INVALID_ID || ModeNotAvailable(id))
        return "not available";
    return NULL;
}

/* What "Automatic" means: hires on AGA, lowres otherwise, in the
 * machine's own video standard, falling back to the other standard. */
int gfx_mode_auto(const struct Caps *c)
{
    int first = c->aga ? (c->pal ? SM_PAL_HIRES : SM_NTSC_HIRES)
                       : (c->pal ? SM_PAL_LORES : SM_NTSC_LORES);
    int other = c->aga ? (c->pal ? SM_NTSC_HIRES : SM_PAL_HIRES)
                       : (c->pal ? SM_NTSC_LORES : SM_PAL_LORES);
    if (!gfx_mode_problem(c, first))
        return first;
    if (!gfx_mode_problem(c, other))
        return other;
    return first;
}

/* Can this screen mode show this style? Smooth needs a 15-bit or deeper
 * RTG mode and its pre-rendered file. */
BOOL gfx_style_possible(const struct Caps *c, int smode, int style)
{
    char path[48];
    BPTR l;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    if (style == STYLE_PIXEL)
        return TRUE;
    if (smode == SM_AUTO || gfx_mode_problem(c, smode))
        smode = gfx_mode_auto(c);
    if (modes[smode].kind != K_RTG || gfx_mode_depth(smode) < 8)
        return FALSE;
    sprintf(path, "PROGDIR:Fonts/amce-smooth/%lu", (unsigned long)modes[smode].h);
    me->pr_WindowPtr = (APTR)-1;
    l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = oldwin;
    if (l)
        UnLock(l);
    return l != 0;
}

static void blk_reserve(struct Gfx *g);

BOOL gfx_open(struct Gfx *g, const struct Caps *c, int smode, int style)
{
    static UWORD pens[] = { (UWORD)~0 };
    ULONG mode;
    int i;

    if (smode == SM_AUTO || gfx_mode_problem(c, smode))
        smode = gfx_mode_auto(c);
    mode = mode_id(smode);
    g->mode = smode;
    g->w = modes[smode].w;
    g->h = modes[smode].h;
    if (modes[smode].kind == K_RTG)
        g->depth = gfx_mode_depth(smode);
    else
        g->depth = c->aga ? 8 : 5;
    g->truecolor = g->depth > 8;
    if (mode == (ULONG)INVALID_ID)
        return FALSE;
    if (g->truecolor && !CyberGfxBase)
        CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 41);
    /* Scale (see amce.h): native 1x; RTG a whole factor of its lines/256,
     * or exactly lines/256 in the smooth style. */
    g->kint = 1;
    g->style = STYLE_PIXEL;
    g->sm = NULL;
    if (modes[smode].kind == K_RTG) {
        g->kint = g->h / RTG_LINES;
        if (g->kint < 1)
            g->kint = 1;
        if (g->kint > 3)
            g->kint = 3;
    }
    g->k = (UWORD)g->kint << 8;
    if (style == STYLE_SMOOTH && modes[smode].kind == K_RTG && g->depth >= 8 &&
        (g->sm = sm_load(g->h))) {
        g->style = STYLE_SMOOTH;
        g->k = sm_scale(g->sm);             /* as the file was rendered for */
    }
    layout_metrics(g);

    for (;;) {
        g->scr = OpenScreenTags(NULL,
                                SA_Width, g->w, SA_Height, g->h, SA_Depth, g->depth,
                                SA_DisplayID, mode, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
                                SA_Type, CUSTOMSCREEN, SA_Title, (ULONG)"AmigaMCE",
                                SA_Pens, (ULONG)pens, SA_SharePens, TRUE,
                                SA_Draggable, FALSE, SA_Interleaved, TRUE,
                                /* deep RTG: room for the smooth style's in-between pens */
                                g->truecolor ? SA_ColorMapEntries : TAG_IGNORE, 256,
                                TAG_END);
        /* Some RTG drivers want depth 24 for their 32-bit modes. */
        if (g->scr || g->depth != 32)
            break;
        g->depth = 24;
    }
    if (!g->scr) {
        amce_log("OpenScreen failed", modes[smode].label);
        sm_free(g->sm);
        g->sm = NULL;
        return FALSE;
    }
    g->planar = (GetBitMapAttr(g->scr->RastPort.BitMap, BMA_FLAGS) & BMF_STANDARD) != 0;
    amce_log("screen open", modes[smode].label);
    amce_log("style", g->style == STYLE_SMOOTH ? "smooth" : "pixel");

    LoadRGB32(&g->scr->ViewPort, (ULONG *)ui_palette32);
    g->pens_shared = FALSE;
    if (g->depth == 8) {
        /* Keep the UI pens 0-15 for ourselves, and hold the pointer's pens
         * 17-19 without touching their colours, so a picture's datatype
         * never repaints the mouse pointer. The rest stay free for it. */
        for (i = 0; i < NUM_UI_PENS; i++)
            ObtainPen(g->scr->ViewPort.ColorMap, i, ui_palette32[1 + i * 3],
                      ui_palette32[2 + i * 3], ui_palette32[3 + i * 3], PEN_EXCLUSIVE);
        for (i = POINTER_PEN0; i < POINTER_PEN0 + 3; i++)
            ObtainPen(g->scr->ViewPort.ColorMap, i, 0, 0, 0, PEN_EXCLUSIVE | PEN_NO_SETCOLOR);
        g->pens_shared = TRUE;
    }

    g->win = OpenWindowTags(NULL,
                            WA_CustomScreen, (ULONG)g->scr, WA_Left, 0, WA_Top, 0,
                            WA_Width, g->w, WA_Height, g->h, WA_Backdrop, TRUE,
                            WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
                            WA_SimpleRefresh, TRUE, WA_NoCareRefresh, TRUE,
                            WA_ReportMouse, TRUE,     /* moves tell the mouse is in use */
                            WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE |
                                      IDCMP_INACTIVEWINDOW,
                            TAG_END);
    if (!g->win) {
        CloseScreen(g->scr);
        g->scr = NULL;
        sm_free(g->sm);
        g->sm = NULL;
        return FALSE;
    }
    g->rp = g->win->RPort;
    SetDrMd(g->rp, JAM1);
    open_font(g);
    if (g->sm) {
        sm_attach(g->sm, g);
        g->font_h = sm_cell_h(g->sm);
        g->baseline = sm_baseline(g->sm);
        g->icon_px = sm_icon_px(g->sm);
        g->mark_px = sm_mark_px(g->sm);
    } else {
        g->icon_px = 16 * g->kint;
        g->mark_px = MARK_SIZE * g->kint;
    }
    fit_menu(g);
    cache_art(g);
    gfx_fill(g, 0, 0, g->w - 1, g->h - 1, PEN_BG);
    blk_reserve(g);
    gfx_dbuf_start(g);
    return TRUE;
}

static UBYTE pic_pens[256];     /* pens the picture on screen holds (own remap) */
static UWORD pic_npens;

/* ---- the preview's two blocks of pens (256-colour screens).
 * As AGS does, the preview's colours go straight into pens kept for it,
 * not matched one by one to free pens (ObtainBestPen, a scan of the whole
 * palette per colour: most of the 0.5 s a preview took to draw). Two
 * blocks, so a new picture is drawn in one while the old one is still in
 * the other. */
#define BLK_MAX  112            /* pens a block, at most */
#define BLK_KEEP 108            /* free pens left for everyone else (the smooth
                                 * style's shades, taken as they are first drawn:
                                 * 6 a colour pair, and some 15 pairs are used;
                                 * with 48 the menu's highlight ran out) */
#define BLK_TWO   64            /* two blocks only if each gets this many */
static UBYTE blk_pen[2][BLK_MAX];
static UWORD blk_n;             /* pens in each block; 0: no blocks */
static int blk_count;           /* 2, or 1 when pens are short (the smooth style's
                                 * shades on an 8-bit screen): then the old picture
                                 * is cleared before the new one takes the block */
static ULONG blk_rgb[2][BLK_MAX];   /* each block's picture's colours, 0xRRGGBB */
static UWORD blk_ncol[2];
static int blk_on = -1;         /* the block of the picture on screen */

/* A block's pens set to its picture's colours. */
static void blk_set(struct Gfx *g, int b)
{
    struct ViewPort *vp = &g->scr->ViewPort;
    UWORD i;
    for (i = 0; i < blk_ncol[b]; i++) {
        ULONG c = blk_rgb[b][i];
        SetRGB32(vp, blk_pen[b][i], ((c >> 16) & 255) * 0x01010101UL,
                 ((c >> 8) & 255) * 0x01010101UL, (c & 255) * 0x01010101UL);
    }
}

static void blk_reserve(struct Gfx *g)
{
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    struct PaletteExtra *pe = cm->PalExtra;
    LONG n, i, b, got;
    blk_n = 0;
    blk_on = -1;
    if (!g->pens_shared || g->truecolor || g->depth < 8 || !pe)
        return;
    n = ((LONG)pe->pe_NFree - BLK_KEEP) / 2;
    blk_count = 2;
    if (n < BLK_TWO) {
        n = (LONG)pe->pe_NFree - BLK_KEEP;
        blk_count = 1;
    }
    if (n > BLK_MAX)
        n = BLK_MAX;
    if (n < 16)
        return;
    for (b = 0; b < blk_count; b++)
        for (i = 0; i < n; i++) {
            got = ObtainPen(cm, -1, 0, 0, 0, PEN_EXCLUSIVE | PEN_NO_SETCOLOR);
            if (got < 0) {
                /* fewer than hoped: give them all back, share pens as before */
                while (--i >= 0)
                    ReleasePen(cm, blk_pen[b][i]);
                while (--b >= 0)
                    for (i = 0; i < n; i++)
                        ReleasePen(cm, blk_pen[b][i]);
                return;
            }
            blk_pen[b][i] = (UBYTE)got;
        }
    blk_n = n;
}

static void blk_release(struct Gfx *g)
{
    int b, i;
    if (blk_n && g->scr)
        for (b = 0; b < blk_count; b++)
            for (i = 0; i < blk_n; i++)
                ReleasePen(g->scr->ViewPort.ColorMap, blk_pen[b][i]);
    blk_n = 0;
    blk_on = -1;
}

static void release_pens(struct Gfx *g, UBYTE *pens, UWORD n)
{
    UWORD i;
    for (i = 0; i < n; i++)
        ReleasePen(g->scr->ViewPort.ColorMap, pens[i]);
}

/* ---- changing a picture on a planar screen without a flash.
 * The blitter changes a planar screen a plane at a time, and a picture
 * there is seen half changed: its pixels' pen numbers are partly old and
 * partly new (or cleared), so for a frame they are other pens' colours, the
 * UI's, other pictures'. So a picture is drawn or cleared in bands of rows,
 * each band while the beam is not showing it nor about to: it goes from
 * the old picture to the new in one step, as the screen sees it. */
#define BAND_ROWS 8
#define BAND_LEAD 40            /* lines the beam may come in a band's time */

struct Beam {
    BOOL on;                    /* planar: worth timing */
    WORD top;                   /* the beam's line for row 0 */
    UBYTE lace, dbl;            /* rows per line halved; lines per row doubled */
};

static void beam_init(struct Gfx *g, struct Beam *b)
{
    struct ViewPort *vp = &g->scr->ViewPort;
    struct DisplayInfo di;
    ULONG id;
    b->on = g->planar && g->depth > 1 && GfxBase->ActiView;
    if (!b->on)
        return;
    id = GetVPModeID(vp);
    b->lace = (vp->Modes & LACE) ? 1 : 0;
    b->dbl = id != (ULONG)INVALID_ID &&
             GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, id) &&
             (di.PropertyFlags & DIPF_IS_SCANDBL) ? 1 : 0;
    b->top = GfxBase->ActiView->DyOffset + ((vp->DyOffset >> b->lace) << b->dbl);
}

static LONG beam_line(const struct Beam *b, WORD row)
{
    return b->top + (((LONG)row >> b->lace) << b->dbl);
}

/* Wait until the beam is clear of rows r0..r1 for a band's time. */
static void beam_wait(const struct Beam *b, WORD r0, WORD r1)
{
    LONG l0 = beam_line(b, r0) - BAND_LEAD, l1 = beam_line(b, r1) + 2, v;
    ULONG n = 0;
    if (!b->on)
        return;
    do
        v = VBeamPos();
    while (v >= l0 && v <= l1 && ++n < 200000);     /* never for ever */
}

/* Fill x0,y0..x1,y1 with the background, band by band. */
static void band_fill(struct Gfx *g, const struct Beam *b, WORD x0, WORD y0, WORD x1, WORD y1)
{
    dirty = TRUE;
    WORD r, r1;
    if (x1 < x0 || y1 < y0)
        return;
    SetAPen(g->rp, PEN_BG);
    SetWriteMask(g->rp, 0xFF);
    for (r = y0; r <= y1; r = r1 + 1) {
        r1 = b->on && r + BAND_ROWS - 1 < y1 ? r + BAND_ROWS - 1 : y1;
        beam_wait(b, r, r1);
        RectFill(g->rp, x0, r, x1, r1);
        WaitBlit();
    }
}

/* Copy bm (w x h) to x, y on the screen, band by band. */
static void band_blit(struct Gfx *g, const struct Beam *b, struct BitMap *bm, WORD x, WORD y,
                      WORD w, WORD h)
{
    dirty = TRUE;
    WORD r, n;
    for (r = 0; r < h; r += n) {
        n = b->on && h - r > BAND_ROWS ? BAND_ROWS : h - r;
        beam_wait(b, y + r, y + r + n - 1);
        BltBitMapRastPort(bm, 0, r, g->rp, x, y + r, w, n, 0xC0);
        WaitBlit();
    }
}

/* The parts of the picture on screen inside x0,y0..x1,y1 and outside the
 * rectangle nx,ny,nw,nh (nw 0: none) cleared. */
static void clear_old(struct Gfx *g, const struct Beam *b, WORD x0, WORD y0, WORD x1, WORD y1,
                      WORD nx, WORD ny, WORD nw, WORD nh)
{
    WORD ox0 = g->pic_x, oy0 = g->pic_y, ox1 = g->pic_x + g->pic_w - 1,
         oy1 = g->pic_y + g->pic_h - 1, ny1, nx1;
    if (!g->pic_w || !g->pic_h)
        return;
    if (ox0 < x0) ox0 = x0;
    if (oy0 < y0) oy0 = y0;
    if (ox1 > x1) ox1 = x1;
    if (oy1 > y1) oy1 = y1;
    if (ox1 < ox0 || oy1 < oy0)
        return;
    if (!nw || !nh) {
        band_fill(g, b, ox0, oy0, ox1, oy1);
        return;
    }
    ny1 = ny + nh - 1;
    nx1 = nx + nw - 1;
    band_fill(g, b, ox0, oy0, ox1, (ny - 1 < oy1 ? ny - 1 : oy1));          /* above */
    {
        WORD a = ny > oy0 ? ny : oy0, z = ny1 < oy1 ? ny1 : oy1;
        if (a <= z) {
            band_fill(g, b, ox0, a, (nx - 1 < ox1 ? nx - 1 : ox1), z);     /* left */
            band_fill(g, b, (nx1 + 1 > ox0 ? nx1 + 1 : ox0), a, ox1, z);  /* right */
        }
    }
    band_fill(g, b, ox0, (ny1 + 1 > oy0 ? ny1 + 1 : oy0), ox1, oy1);        /* below */
}

/* Copy bm (w x h) to x, y on the screen without a half-drawn frame. */
void gfx_blit_beam(struct Gfx *g, struct BitMap *bm, WORD x, WORD y, WORD w, WORD h)
{
    struct Beam b;
    beam_init(g, &b);
    band_blit(g, &b, bm, x, y, w, h);
}

/* Clear the picture on screen (as far as it is inside x0,y0..x1,y1),
 * then let its pens go: in that order, so it never shows in pens given to
 * another picture meanwhile. */
void gfx_picture_clear(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1)
{
    struct Beam b;
    if (g->pic_w && g->scr) {
        beam_init(g, &b);
        clear_old(g, &b, x0, y0, x1, y1, 0, 0, 0, 0);
    }
    gfx_picture_free(g);
}

void gfx_picture_free(struct Gfx *g)
{
    g->pic_w = g->pic_h = 0;
    blk_on = -1;                            /* its block is free for the next */
    if (pic_obj) {
        DisposeDTObject(pic_obj);
        pic_obj = NULL;
    }
    if (pic_npens && g->scr)
        release_pens(g, pic_pens, pic_npens);
    pic_npens = 0;
}

/* The artwork, fonts and smooth style a screen was opened with, let go
 * (the screen itself stays). */
static void style_free(struct Gfx *g)
{
    WaitBlit();
    if (g->logo_masks) {
        FreeMem(g->logo_masks, (ULONG)g->logo_npens * g->logo_h * g->logo_bpr);
        g->logo_masks = NULL;
    }
    if (g->icon_masks) {
        FreeMem(g->icon_masks, (ULONG)ICON_COUNT * g->icon_px * g->icon_bpr);
        g->icon_masks = NULL;
    }
    if (g->mark_masks) {
        FreeMem(g->mark_masks, (ULONG)MARK_PLAY * g->mark_px * g->mark_bpr);
        g->mark_masks = NULL;
    }
    letters_free();
    if (g->sm) {
        sm_free(g->sm);
        g->sm = NULL;
    }
}

/* Change the style (pixel, smooth) on the screen that is open: its
 * artwork, fonts and layout are made again, the screen and window stay
 * (no mode switch, no flash). FALSE: smooth could not be loaded; the pixel
 * style is set instead. The caller draws everything again. */
BOOL gfx_restyle(struct Gfx *g, int style)
{
    BOOL ok = TRUE;
    BOOL dbuf = gfx_dbuf_active();
    if (!g->scr || !g->win)
        return FALSE;
    gfx_dbuf_stop(g);                       /* the font goes on the window's RastPort */
    gfx_picture_free(g);
    style_free(g);
    if (g->font) {
        CloseFont(g->font);
        g->font = NULL;
    }
    g->k = (UWORD)g->kint << 8;
    g->style = STYLE_PIXEL;
    if (style == STYLE_SMOOTH && modes[g->mode].kind == K_RTG && g->depth >= 8) {
        if ((g->sm = sm_load(g->h))) {
            g->style = STYLE_SMOOTH;
            g->k = sm_scale(g->sm);
        } else {
            ok = FALSE;
        }
    }
    layout_metrics(g);
    SetDrMd(g->rp, JAM1);
    open_font(g);
    if (g->sm) {
        sm_attach(g->sm, g);
        g->font_h = sm_cell_h(g->sm);
        g->baseline = sm_baseline(g->sm);
        g->icon_px = sm_icon_px(g->sm);
        g->mark_px = sm_mark_px(g->sm);
    } else {
        g->icon_px = 16 * g->kint;
        g->mark_px = MARK_SIZE * g->kint;
    }
    fit_menu(g);
    cache_art(g);
    gfx_fill(g, 0, 0, g->w - 1, g->h - 1, PEN_BG);
    amce_log("style", g->style == STYLE_SMOOTH ? "smooth (same screen)" : "pixel (same screen)");
    if (dbuf)
        gfx_dbuf_start(g);
    return ok;
}

void gfx_close(struct Gfx *g)
{
    int i;
    gfx_dbuf_stop(g);
    gfx_picture_free(g);
    blk_release(g);
    if (CyberGfxBase) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = NULL;
    }
    style_free(g);
    if (g->win) {
        amce_log("CloseWindow", "");
        CloseWindow(g->win);
        g->win = NULL;
    }
    if (g->font) {
        CloseFont(g->font);
        g->font = NULL;
    }
    if (g->scr) {
        BOOL ok;
        if (g->pens_shared) {
            for (i = 0; i < NUM_UI_PENS; i++)
                ReleasePen(g->scr->ViewPort.ColorMap, i);
            for (i = POINTER_PEN0; i < POINTER_PEN0 + 3; i++)
                ReleasePen(g->scr->ViewPort.ColorMap, i);
        }
        amce_log("CloseScreen", "");
        ok = CloseScreen(g->scr);
        amce_log("CloseScreen returned", ok ? "TRUE" : "FALSE (windows still open)");
        g->scr = NULL;
    }
}

void gfx_fill(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1, UBYTE pen)
{
    dirty = TRUE;
    if (x1 < x0 || y1 < y0)
        return;
    SetAPen(g->rp, pen);
    RectFill(g->rp, x0, y0, x1, y1);
}

/* Clear to the background, high planes first. A planar fill writes one
 * plane at a time; clearing planes 3+ first means a pixel caught halfway
 * shows one of the dark UI pens 0-7, not a stray colour such as pen 8. */
/* A filled circle of radius r (pixels across) centred on (cx, cy), over
 * bg: round on the monitor, so half as many pixels tall on a screen with
 * tall pixels (640 x 200/256). In the smooth style its edge is
 * anti-aliased (4 x 4 samples a pixel, in the text's blend pens); in the
 * pixel style it is whole pixels. */
void gfx_dot(struct Gfx *g, WORD cx, WORD cy, WORD r, UBYTE pen, UBYTE bg)
{
    dirty = TRUE;
    LONG rx8, ry8, lim;                     /* radii in eighths of a pixel */
    WORD x, y, ry;
    if (r < 1)
        r = 1;
    rx8 = 8L * r;
    ry8 = gfx_mode_tall(g->mode) ? 4L * r : 8L * r;
    ry = (WORD)((ry8 + 7) / 8);
    lim = rx8 * rx8 * ry8 * ry8;
#define INSIDE(px, py) ((px) * (px) * ry8 * ry8 + (py) * (py) * rx8 * rx8 <= lim)
    SetWriteMask(g->rp, 0xFF);
    if (!g->sm) {
        /* whole pixels: a run per row, inside if the pixel's centre is */
        SetAPen(g->rp, pen);
        for (y = -ry; y < ry; y++) {
            LONG py = 8 * y + 4, half = 0;
            while (half < r && INSIDE(8 * half + 4, py))
                half++;
            if (half)
                RectFill(g->rp, cx - half, cy + y, cx + half - 1, cy + y);
        }
        return;
    }
    for (y = -ry; y < ry; y++)
        for (x = -r; x < r; x++) {
            int i, j, n = 0;
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    LONG px = 8 * x + 2 * i + 1, py = 8 * y + 2 * j + 1;
                    if (INSIDE(px, py))
                        n++;
                }
            if (n) {
                SetAPen(g->rp, sm_pen(g->sm, g, pen, bg, (UBYTE)((n * (SMOOTH_LEVELS - 1) + 8) / 16)));
                WritePixel(g->rp, cx + x, cy + y);
            }
        }
#undef INSIDE
}

void gfx_fill_bg(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1)
{
    dirty = TRUE;
    if (x1 < x0 || y1 < y0)
        return;
    SetAPen(g->rp, PEN_BG);
    if (g->planar && g->depth > 3) {
        SetWriteMask(g->rp, 0xF8);
        RectFill(g->rp, x0, y0, x1, y1);
        SetWriteMask(g->rp, 0xFF);
    }
    RectFill(g->rp, x0, y0, x1, y1);
}

WORD gfx_textlen(struct Gfx *g, const char *s, int ts)
{
    if (g->sm)
        return sm_textlen(g->sm, s, strlen(s), ts & 0x0F);
    return TextLength(g->rp, (CONST_STRPTR)s, strlen(s));
}

void gfx_text(struct Gfx *g, WORD x, WORD y, const char *s, UBYTE fg, UBYTE bg, WORD maxw, int ts)
{
    dirty = TRUE;
    struct TextExtent te;
    ULONG n = strlen(s);
    BOOL opaque = (ts & TS_OPAQUE) != 0;
    int face = ts & 0x0F;
    if (!n)
        return;
    if (g->sm) {
        /* too long: as much as fits, then "..." */
        if (maxw > 0 && sm_textlen(g->sm, s, n, face) > maxw) {
            char buf[256];
            WORD dots = sm_textlen(g->sm, "...", 3, face);
            LONG k = maxw > dots ? sm_fit(g->sm, s, n, face, maxw - dots) : 0;
            if (k > (LONG)sizeof(buf) - 4)
                k = sizeof(buf) - 4;
            while (k > 0 && s[k - 1] == ' ')
                k--;
            if (!k)
                return;
            memcpy(buf, s, k);
            memcpy(buf + k, "...", 4);
            sm_text(g->sm, g, x, y, buf, k + 3, face, fg, bg, opaque);
        } else {
            sm_text(g->sm, g, x, y, s, n, face, fg, bg, opaque);
        }
        return;
    }
    if (maxw > 0)
        n = TextFit(g->rp, (CONST_STRPTR)s, n, &te, NULL, 1, maxw, g->font_h + 1);
    if (!n)
        return;
    SetAPen(g->rp, fg);
    if (opaque) {
        SetBPen(g->rp, bg);
        SetDrMd(g->rp, JAM2);
    }
    Move(g->rp, x, y + g->baseline);
    Text(g->rp, (CONST_STRPTR)s, n);
    if (opaque)
        SetDrMd(g->rp, JAM1);
}

/* The logo and icons are stamped with BltTemplate: one-pen masks drawn
 * straight onto the screen, the same path as text. Plotting them with
 * WritePixel lost them on RTG screens (the card's fills of the header and
 * menu rows landed afterwards), and so did a cached off-screen bitmap
 * under Amiberry's uaegfx, where short fills into RTG off-screen memory
 * went missing. The logo becomes one mask per pen it uses (pens never
 * overlap, so no pixel is drawn twice); the bar pen is skipped because the
 * header is already that colour. */
/* Set the k x k block of design pixel (i, j) in a mask with bpr bytes a row. */
static void put_block(UBYTE *m, UWORD bpr, UWORD i, UWORD j, UWORD k)
{
    UWORD a, b;
    for (b = 0; b < k; b++) {
        UBYTE *row = m + (ULONG)(j * k + b) * bpr;
        for (a = 0; a < k; a++) {
            UWORD x = i * k + a;
            row[x >> 3] |= 0x80 >> (x & 7);
        }
    }
}

/* Masks of n one-word-a-row bitmaps (rows x 16 bits each), enlarged k
 * times: count masks of (16k or less) x rows*k, bpr bytes a row. */
static UBYTE *enlarge_words(const UWORD *src, UWORD count, UWORD rows, UWORD k, UWORD *bpr_out)
{
    UWORD bpr = ((16 * k + 15) >> 4) << 1, n, r, c;
    ULONG one = (ULONG)rows * k * bpr;
    UBYTE *m = AllocMem(one * count, MEMF_CHIP | MEMF_CLEAR);
    *bpr_out = bpr;
    if (!m)
        return NULL;
    for (n = 0; n < count; n++)
        for (r = 0; r < rows; r++) {
            UWORD bits = src[(ULONG)n * rows + r];
            for (c = 0; c < 16; c++)
                if (bits & (0x8000 >> c))
                    put_block(m + one * n, bpr, c, r, k);
        }
    return m;
}

/* Pixel style: the logo (one mask per pen), menu icons and row markers,
 * enlarged kint times. Smooth style: its own logo (smooth.c); icons and
 * markers are drawn straight from its coverage maps. */
static void cache_art(struct Gfx *g)
{
    const UBYTE *px = logo_lo;
    UWORD w = logo_lo_w, h = logo_lo_h, k = g->kint, i, j, p;
    ULONG size, n;

    if (g->sm) {
        if (!sm_logo(g->sm, g))
            g->logo_masks = NULL;
        g->icon_masks = g->mark_masks = NULL;
        return;
    }
    /* a deep RTG screen shows the smooth logo in the pixel style too */
    g->logo_masks = NULL;
    if (g->truecolor && sm_logo_file(g, k))
        goto icons;
    g->logo_npens = 0;
    for (n = 0; n < (ULONG)w * h; n++) {
        UBYTE pen = px[n];
        if (pen == PEN_BAR)
            continue;
        for (p = 0; p < g->logo_npens && g->logo_pens[p] != pen; p++)
            ;
        if (p == g->logo_npens && p < sizeof(g->logo_pens))
            g->logo_pens[g->logo_npens++] = pen;
    }
    g->logo_w = w * k;
    g->logo_h = h * k;
    g->logo_bpr = ((g->logo_w + 15) / 16) * 2;
    size = (ULONG)g->logo_npens * g->logo_h * g->logo_bpr;
    /* BltTemplate reads its source with the blitter: chip RAM, even rows. */
    g->logo_masks = size ? AllocMem(size, MEMF_CHIP | MEMF_CLEAR) : NULL;
    if (g->logo_masks)
        for (p = 0; p < g->logo_npens; p++) {
            UBYTE *m = g->logo_masks + (ULONG)p * g->logo_h * g->logo_bpr;
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++)
                    if (px[j * w + i] == g->logo_pens[p])
                        put_block(m, g->logo_bpr, i, j, k);
        }
icons:
    g->icon_masks = enlarge_words(&icons16[0][0], ICON_COUNT, 16, k, &g->icon_bpr);
    g->mark_masks = enlarge_words(&marks16[0][0], MARK_PLAY, MARK_SIZE, k, &g->mark_bpr);
}

void gfx_logo(struct Gfx *g, WORD x, WORD y)
{
    dirty = TRUE;
    UWORD p;
    if (!g->logo_masks)
        return;
    SetDrMd(g->rp, JAM1);
    for (p = 0; p < g->logo_npens; p++) {
        PLANEPTR m = (PLANEPTR)(g->logo_masks + (ULONG)p * g->logo_h * g->logo_bpr);
        SetAPen(g->rp, g->logo_pens[p]);
        BltTemplate(m, 0, g->logo_bpr, g->rp, x, y, g->logo_w, g->logo_h);
    }
}

/* A list row marker (sub-menu chevron, checkbox, radio button): every one
 * is MARK_SIZE design units square, so a state change never moves text. */
/* MARK_PLAYING: four bars, a level meter dancing (frames of heights, in
 * the marker's 11 design units), low and still when the music is paused. */
UBYTE gfx_eq_frame;
BOOL gfx_eq_still;

static void eq_bars(struct Gfx *g, WORD x, WORD y, UBYTE fg, UBYTE bg)
{
    static const UBYTE dance[8][4] = {
        { 4, 9, 6, 10 }, { 7, 5, 9, 6 }, { 10, 7, 4, 8 }, { 6, 10, 7, 4 },
        { 8, 4, 10, 7 }, { 5, 8, 6, 11 }, { 9, 6, 8, 4 }, { 6, 11, 5, 9 }
    };
    static const UBYTE still[4] = { 3, 5, 4, 6 };
    WORD n = g->mark_px, bw = n * 2 / 11, gap, i, left;
    const UBYTE *h = gfx_eq_still ? still : dance[gfx_eq_frame & 7];
    if (bw < 1)
        bw = 1;
    gap = (n - 4 * bw) / 3;
    if (gap < 1)
        gap = 1;
    left = x + (n - (4 * bw + 3 * gap)) / 2;
    gfx_fill(g, x, y, x + n - 1, y + n - 1, bg);
    for (i = 0; i < 4; i++) {
        WORD bh = (WORD)((LONG)n * h[i] / 11), bx = left + i * (bw + gap);
        if (bh < 1)
            bh = 1;
        gfx_fill(g, bx, y + n - bh, bx + bw - 1, y + n - 1, fg);
    }
}

void gfx_mark(struct Gfx *g, int mark, WORD x, WORD y, UBYTE fg, UBYTE bg)
{
    dirty = TRUE;
    PLANEPTR m;
    if (mark == MARK_PLAYING) {
        eq_bars(g, x, y, fg, bg);
        return;
    }
    if (mark < MARK_ARROW || mark > MARK_PLAY)
        return;
    if (g->sm) {
        sm_mark(g->sm, g, mark, x, y, fg, bg);
        return;
    }
    if (!g->mark_masks)
        return;
    m = (PLANEPTR)(g->mark_masks + (ULONG)(mark - 1) * g->mark_px * g->mark_bpr);
    SetAPen(g->rp, fg);
    SetDrMd(g->rp, JAM1);
    BltTemplate(m, 0, g->mark_bpr, g->rp, x, y, g->mark_px, g->mark_px);
}

void gfx_icon(struct Gfx *g, int icon, WORD x, WORD y, UBYTE fg, UBYTE bg)
{
    dirty = TRUE;
    PLANEPTR m;
    if (g->sm) {
        sm_icon(g->sm, g, icon, x, y, fg, bg);
        return;
    }
    if (!g->icon_masks)
        return;
    m = (PLANEPTR)(g->icon_masks + (ULONG)icon * g->icon_px * g->icon_bpr);
    SetAPen(g->rp, fg);
    SetDrMd(g->rp, JAM1);
    BltTemplate(m, 0, g->icon_bpr, g->rp, x, y, g->icon_px, g->icon_px);
}

/* ------------------------------------------------------------ big letter */
/* The overlay's letters: A-Z and '#' (accented capitals file under their
 * base letter), built on first use at the size asked and kept until the
 * screen closes or the size changes. */
#define LETTERS (26 + 1)
static struct BigLetter letters[LETTERS];
static WORD letters_ink;

static int letter_slot(UBYTE c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c == '#')
        return 26;
    return -1;
}

static void letters_free(void)
{
    int i;
    WaitBlit();
    for (i = 0; i < LETTERS; i++) {
        if (letters[i].mask)
            FreeMem(letters[i].mask, letters[i].size);
        letters[i].mask = NULL;
    }
    letters_ink = 0;
}

/* Pixel style: the UI font's own glyph blown up in whole pixels. */
static BOOL pixel_letter(struct Gfx *g, UBYTE c, WORD ink_h, struct BigLetter *bl)
{
    struct TextFont *f = g->font;
    const UBYTE *data;
    ULONG lw;
    UWORD off, gw, mod, x, yy;
    LONG idx;
    WORD top = -1, bot = -1, sc;
    if (!f)
        return FALSE;
    idx = (c < f->tf_LoChar || c > f->tf_HiChar) ? f->tf_HiChar - f->tf_LoChar + 1
                                                   : c - f->tf_LoChar;
    lw = ((ULONG *)f->tf_CharLoc)[idx];
    off = (UWORD)(lw >> 16);
    gw = (UWORD)lw;
    data = (const UBYTE *)f->tf_CharData;
    mod = f->tf_Modulo;
    if (!gw)
        return FALSE;
#define FONTBIT(xx, yy2) (data[(ULONG)(yy2) * mod + ((off + (xx)) >> 3)] & (0x80 >> ((off + (xx)) & 7)))
    for (yy = 0; yy < f->tf_YSize; yy++)
        for (x = 0; x < gw; x++)
            if (FONTBIT(x, yy)) {
                if (top < 0)
                    top = yy;
                bot = yy;
                break;
            }
    if (top < 0)
        return FALSE;
    sc = ink_h / (bot - top + 1);
    if (sc < 2)
        sc = 2;
    if (sc > 16)
        sc = 16;
    bl->w = gw * sc;
    bl->h = (bot - top + 1) * sc;
    bl->bpr = ((bl->w + 15) / 16) * 2;
    bl->size = (ULONG)bl->bpr * bl->h;
    bl->cell_w = (UWORD)(f->tf_XSize + 1) * sc;
    bl->mask = AllocMem(bl->size, MEMF_CHIP | MEMF_CLEAR);
    if (!bl->mask)
        return FALSE;
    for (yy = top; yy <= (UWORD)bot; yy++)
        for (x = 0; x < gw; x++)
            if (FONTBIT(x, yy))
                put_block(bl->mask, bl->bpr, x, yy - top, sc);
#undef FONTBIT
    return TRUE;
}

const struct BigLetter *gfx_bigletter(struct Gfx *g, UBYTE c, WORD ink_h)
{
    int i = letter_slot(c);
    struct BigLetter *bl;
    BOOL ok;
    if (i < 0 || ink_h <= 0)
        return NULL;
    if (ink_h != letters_ink) {
        letters_free();
        letters_ink = ink_h;
    }
    bl = &letters[i];
    if (bl->mask)
        return bl;
    ok = g->sm ? sm_bigletter(g->sm, c, ink_h, bl) : pixel_letter(g, c, ink_h, bl);
    if (!ok && bl->mask) {
        FreeMem(bl->mask, bl->size);
        bl->mask = NULL;
    }
    return bl->mask ? bl : NULL;
}

/* One stamp: the overlay is a faint letter behind the list, so even in the
 * smooth style its edges are not worth a stamp per shade. */
void gfx_bigletter_draw(struct Gfx *g, const struct BigLetter *bl, WORD x, WORD y, UBYTE pen)
{
    dirty = TRUE;
    if (!bl || !bl->mask)
        return;
    SetDrMd(g->rp, JAM1);
    SetAPen(g->rp, pen);
    BltTemplate((PLANEPTR)bl->mask, 0, bl->bpr, g->rp, x, y, bl->w, bl->h);
}

/* What the datatype object o holds. */
void pic_info(APTR o, const struct BitMapHeader *bmh, struct PicInfo *pi)
{
    struct DataType *dtn = NULL;
    ULONG mode = 0;
    pi->w = bmh->bmh_Width;
    pi->h = bmh->bmh_Height;
    pi->depth = bmh->bmh_Depth;
    GetDTAttrs(o, PDTA_ModeID, (ULONG)&mode, DTA_DataType, (ULONG)&dtn, TAG_END);
    pi->mode = mode == (ULONG)INVALID_ID ? 0 : mode;
    pi->fmt[0] = 0;
    pi->cls[0] = 0;
    if (dtn && dtn->dtn_Header && dtn->dtn_Header->dth_Name)
        amce_strlcpy(pi->fmt, (const char *)dtn->dtn_Header->dth_Name, sizeof(pi->fmt));
    if (dtn && dtn->dtn_Header && dtn->dtn_Header->dth_BaseName)
        amce_strlcpy(pi->cls, (const char *)dtn->dtn_Header->dth_BaseName, sizeof(pi->cls));
}

void pic_describe(const struct PicInfo *pi, char *out, LONG max)
{
    char c[24];
    if (pi->mode & HAM_KEY)
        sprintf(c, "HAM%lu", (unsigned long)pi->depth);
    else if (pi->depth > 8)
        strcpy(c, "true colour");
    else if ((pi->mode & EXTRAHALFBRITE_KEY) && pi->depth == 6)
        strcpy(c, "64 colours, EHB");
    else
        sprintf(c, "%lu colours", 1UL << pi->depth);
    char t[64];
    sprintf(t, "%lu x %lu, %s", (unsigned long)pi->w, (unsigned long)pi->h, c);
    amce_strlcpy(out, t, max);
}

/* Decode a picture through the registered datatypes. remap: into scr's
 * pens or true colour (the datatype allocates the pens). Without remap the
 * picture keeps its own colour numbers and takes no pens; gfx_picture_obj
 * maps them when it is drawn, so a picture decoded ahead (loader.c) never
 * takes pens from the one on screen. A deeper picture than 8 bits can only
 * be drawn remapped; given scr, it is then remapped after all; without, it
 * is kept as it is, for thumb_make (thumb.c). Safe to call from the loader
 * process. */
APTR pic_decode(const char *path, struct Screen *scr, BOOL remap, BOOL *remapped,
                struct PicInfo *pi)
{
    struct BitMapHeader *bmh = NULL;
    Object *o;
    if (!DataTypesBase)
        return NULL;
    if (remap)
        o = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE,
                        PDTA_Screen, (ULONG)scr, PDTA_Remap, TRUE,
                        PDTA_DestMode, PMODE_V43, OBP_Precision, PRECISION_IMAGE,
                        TAG_END);
    else
        o = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE, PDTA_Remap, FALSE,
                        PDTA_DestMode, PMODE_V43, TAG_END);
    if (!o)
        return NULL;
    if (!DoDTMethod(o, NULL, NULL, DTM_PROCLAYOUT, NULL, (ULONG)TRUE) && remap) {
        DisposeDTObject(o);
        return NULL;
    }
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_END);
    if (!bmh || !bmh->bmh_Width || !bmh->bmh_Height) {
        DisposeDTObject(o);
        return NULL;
    }
    if (!remap && bmh->bmh_Depth > 8 && scr) {
        DisposeDTObject(o);
        return pic_decode(path, scr, TRUE, remapped, pi);
    }
    pic_info(o, bmh, pi);
    *remapped = remap;
    return o;
}

/* A picture decoded without remapping, as a bitmap like the screen's in
 * pens obtained for its colours now (pens[], *npens: to release). */
static struct BitMap *own_remap(struct Gfx *g, Object *o, ULONG sw, ULONG sh,
                                UBYTE *pens, UWORD *npens)
{
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    ULONG *cregs = NULL, ncol = 0, mod = (sw + 15) & ~15UL, i;
    struct BitMap *src = NULL, *bm, *row = NULL;
    struct RastPort srp, trp;
    struct pdtBlitPixelArray pbpa;
    UBYTE *buf, map[256], used[256];
    BOOL got = FALSE;
    *npens = 0;
    GetDTAttrs(o, PDTA_CRegs, (ULONG)&cregs, PDTA_NumColors, (ULONG)&ncol,
               PDTA_BitMap, (ULONG)&src, TAG_END);
    if (!cregs || !ncol)
        return NULL;
    if (ncol > 256)
        ncol = 256;
    if (!(buf = AllocVec(mod * sh, MEMF_ANY)))
        return NULL;
    pbpa.MethodID = PDTM_READPIXELARRAY;
    pbpa.pbpa_PixelData = buf;
    pbpa.pbpa_PixelFormat = PBPAFMT_LUT8;
    pbpa.pbpa_PixelArrayMod = mod;
    pbpa.pbpa_Left = 0;
    pbpa.pbpa_Top = 0;
    pbpa.pbpa_Width = sw;
    pbpa.pbpa_Height = sh;
    /* DoMethodA, not DoDTMethodA: that goes through DoGadgetMethodA, which
     * puts a GadgetInfo (NULL here) in every message's second longword --
     * here the pixel buffer's address, so the pixels went to address 0 */
    got = DoMethodA(o, (Msg)&pbpa) != 0;
    if (!got && src && (row = AllocBitMap(sw, 1, GetBitMapAttr(src, BMA_DEPTH), 0, NULL))) {
        InitRastPort(&srp);
        srp.BitMap = src;
        trp = srp;
        trp.Layer = NULL;
        trp.BitMap = row;
        ReadPixelArray8(&srp, 0, 0, sw - 1, sh - 1, buf, &trp);
        WaitBlit();
        FreeBitMap(row);
        row = NULL;
        got = TRUE;
    }
    if (!got || !(bm = AllocBitMap(sw, sh, g->depth, BMF_CLEAR, g->rp->BitMap))) {
        FreeVec(buf);
        return NULL;
    }
    memset(used, 0, sizeof(used));
    for (i = 0; i < mod * sh; i++)
        used[buf[i]] = 1;
    for (i = 0; i < 256; i++) {
        map[i] = PEN_BG;
        if (used[i] && i < ncol) {
            LONG pen = ObtainBestPen(cm, cregs[i * 3], cregs[i * 3 + 1], cregs[i * 3 + 2],
                                     OBP_Precision, PRECISION_IMAGE, TAG_END);
            if (pen >= 0) {
                map[i] = pen;
                pens[(*npens)++] = pen;
            }
        }
    }
    for (i = 0; i < mod * sh; i++)
        buf[i] = map[buf[i]];
    InitRastPort(&srp);
    srp.BitMap = bm;
    if (GfxBase->LibNode.lib_Version >= 40) {
        WriteChunkyPixels(&srp, 0, 0, sw - 1, sh - 1, buf, mod);
    } else if ((row = AllocBitMap(sw, 1, g->depth, 0, bm))) {
        trp = srp;
        trp.BitMap = row;
        WritePixelArray8(&srp, 0, 0, sw - 1, sh - 1, buf, &trp);
        WaitBlit();
        FreeBitMap(row);
    }
    FreeVec(buf);
    return bm;
}

/* The size a sw x sh picture is drawn at in a w x h box (tall: on a
 * tall-pixel screen). */
void pic_fit(ULONG sw, ULONG sh, BOOL tall, WORD w, WORD h, BOOL whole, ULONG *pdw, ULONG *pdh)
{
    ULONG sxe, sye, ew, eh, kw, kh, k, dw, dh;
    /* A lowres picture on a tall-pixel hires screen: double its width to
     * keep its shape. */
    sxe = (tall && sw <= 384) ? 2 : 1;
    /* AGS's screenshots are 320 x 128: every other line of a 320 x 256
     * screen. Anything 2.5:1 or wider is taken as such and shown twice as
     * tall, the shape of the screen it came from. */
    sye = sw * 2 >= sh * 5 ? 2 : 1;
    if (sxe == 2 && sye == 2)
        sxe = sye = 1;                      /* the same shape, half the size */
    /* fit (sw*sxe) x (sh*sye) into w x h: a whole scale if 1x fits, so
     * pixels stay square-edged, else shrunk to fit */
    ew = sw * sxe;
    eh = sh * sye;
    kw = (ULONG)w / ew;
    kh = (ULONG)h / eh;
    k = kw < kh ? kw : kh;
    if (whole && k >= 1) {
        dw = ew * k;
        dh = eh * k;
    } else {
        dw = w;
        dh = (eh * w) / ew;
        if (dh > (ULONG)h) {
            dh = h;
            dw = (ew * h) / eh;
        }
    }
    *pdw = dw ? dw : 1;
    *pdh = dh ? dh : 1;
}

/* 32 chunky pixels (8 longs, long-aligned) to a long of each of 8
 * bitplanes (out[k]: bit k of each, the first pixel in the top bit). The
 * merge method c2p routines use (as in c2p.library, Kalms's c2p): five
 * rounds of masked swaps between pairs of longs, on longs only, never a
 * byte. Checked against a plain bit-by-bit conversion on 200,000 random
 * inputs. */
#define MERGE(lo, hi, sh, m) \
    do { ULONG t_ = (((hi) >> (sh)) ^ (lo)) & (m); (lo) ^= t_; (hi) ^= t_ << (sh); } while (0)
static void c2p32(const ULONG *c, ULONG *out)
{
    ULONG a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3], a4 = c[4], a5 = c[5], a6 = c[6], a7 = c[7];
    MERGE(a0, a4, 16, 0x0000FFFFUL);
    MERGE(a1, a5, 16, 0x0000FFFFUL);
    MERGE(a2, a6, 16, 0x0000FFFFUL);
    MERGE(a3, a7, 16, 0x0000FFFFUL);
    MERGE(a0, a2, 8, 0x00FF00FFUL);
    MERGE(a1, a3, 8, 0x00FF00FFUL);
    MERGE(a4, a6, 8, 0x00FF00FFUL);
    MERGE(a5, a7, 8, 0x00FF00FFUL);
    MERGE(a0, a1, 4, 0x0F0F0F0FUL);
    MERGE(a2, a3, 4, 0x0F0F0F0FUL);
    MERGE(a4, a5, 4, 0x0F0F0F0FUL);
    MERGE(a6, a7, 4, 0x0F0F0F0FUL);
    MERGE(a0, a4, 2, 0x33333333UL);
    MERGE(a1, a5, 2, 0x33333333UL);
    MERGE(a2, a6, 2, 0x33333333UL);
    MERGE(a3, a7, 2, 0x33333333UL);
    MERGE(a0, a2, 1, 0x55555555UL);
    MERGE(a1, a3, 1, 0x55555555UL);
    MERGE(a4, a6, 1, 0x55555555UL);
    MERGE(a5, a7, 1, 0x55555555UL);
    out[0] = a7;
    out[1] = a5;
    out[2] = a3;
    out[3] = a1;
    out[4] = a6;
    out[5] = a4;
    out[6] = a2;
    out[7] = a0;
}
#undef MERGE

/* Chunky pixels (mod bytes a row, a multiple of 16) into bm. A planar
 * bitmap is written here, 32 pixels at a time (c2p32): graphics.library's
 * WriteChunkyPixels took ~300 ms for a 273 x 218 preview on a 68030. */
void gfx_chunky(struct Gfx *g, struct BitMap *bm, UBYTE *buf, ULONG w, ULONG h,
                         ULONG mod)
{
    struct RastPort rp, trp;
    struct BitMap *row;
    if (g->planar && (GetBitMapAttr(bm, BMA_FLAGS) & BMF_STANDARD) && bm->Depth <= 8 &&
        !(bm->BytesPerRow & 3) && bm->BytesPerRow >= ((w + 31) >> 5) << 2) {
        /* 32 pixels a step, a long to each plane: a quarter of the writes
         * to chip RAM, which is slow with the display fetching 8 planes.
         * (buf has 32 bytes to spare past its end for the last step.) */
        ULONG y, x, bpr = bm->BytesPerRow, nl = (w + 31) >> 5, d = bm->Depth, k;
        ULONG pl[8], *dst[8];
        WaitBlit();                         /* nothing of the blitter's in it */
        for (y = 0; y < h; y++) {
            const ULONG *c = (const ULONG *)(buf + y * mod);
            for (k = 0; k < d; k++)
                dst[k] = (ULONG *)((UBYTE *)bm->Planes[k] + y * bpr);
            for (x = 0; x < nl; x++, c += 8) {
                c2p32(c, pl);
                for (k = 0; k < d; k++)
                    *dst[k]++ = pl[k];
            }
        }
        return;
    }
    InitRastPort(&rp);
    rp.BitMap = bm;
    if (GfxBase->LibNode.lib_Version >= 40) {
        WriteChunkyPixels(&rp, 0, 0, w - 1, h - 1, buf, mod);
    } else if ((row = AllocBitMap(w, 1, g->depth, 0, bm))) {
        trp = rp;
        trp.BitMap = row;
        WritePixelArray8(&rp, 0, 0, w - 1, h - 1, buf, &trp);
        WaitBlit();
        FreeBitMap(row);
    }
}

/* How many colours a thumbnail made now may have: half the pens free to
 * pictures, the other half for the next one (drawn while this one is
 * still up, in other pens). */
ULONG gfx_draw_ms[3];   /* the last gfx_thumb: colours, pixels, copy to the screen */
ULONG gfx_present_ms;   /* the last gfx_present: flip and copy */
/* ---- double buffering (RTG screens; a prototype, PROGDIR:AmigaMCE.dbuf)
 * Two buffers of the screen (AllocScreenBuffer): everything is drawn into
 * the one not shown, through a RastPort of our own on it (g->rp), and
 * gfx_present shows it at the next vertical blank (ChangeScreenBuffer):
 * nothing is ever seen half drawn, scrolling does not tear. Then, once
 * the other one is no longer shown ("safe to write"), what is now shown
 * is copied into it in one blit, and drawing goes on there: the next
 * frame's changes start from this frame's picture. */
BOOL gfx_dbuf_wanted;
static struct ScreenBuffer *dsb[2];
static struct Layer_Info *dli[2];
static struct Layer *dlay[2];       /* a layer on each: its RastPort clips to the
                                     * screen, as the window's did */
static struct MsgPort *dsafe;
static int dback;               /* the buffer drawn into */
static BOOL dsafe_due;          /* a "safe to write" message is still to come */
static BOOL dbuf_on;

void gfx_dirty(void)
{
    dirty = TRUE;
}

BOOL gfx_dbuf_active(void)
{
    return dbuf_on;
}

static void dbuf_wait_safe(void)
{
    if (!dsafe_due)
        return;
    while (!GetMsg(dsafe))
        WaitPort(dsafe);
    dsafe_due = FALSE;
}

void gfx_dbuf_start(struct Gfx *g)
{
    int i;
    if (dbuf_on || !gfx_dbuf_wanted || g->planar || !g->scr || !g->win)
        return;
    if (!(dsafe = CreateMsgPort()))
        return;
    dsb[0] = AllocScreenBuffer(g->scr, NULL, SB_SCREEN_BITMAP);
    dsb[1] = dsb[0] ? AllocScreenBuffer(g->scr, NULL, SB_COPY_BITMAP) : NULL;
    if (!dsb[1]) {
        if (dsb[0])
            FreeScreenBuffer(g->scr, dsb[0]);
        dsb[0] = NULL;
        DeleteMsgPort(dsafe);
        dsafe = NULL;
        amce_log("double buffering", "could not get a second buffer");
        return;
    }
    for (i = 0; i < 2; i++) {
        dsb[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = dsafe;
        dsb[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = NULL;
        dli[i] = LayersBase ? NewLayerInfo() : NULL;
        dlay[i] = dli[i] ? CreateUpfrontLayer(dli[i], dsb[i]->sb_BitMap, 0, 0, g->w - 1, g->h - 1,
                                              LAYERSIMPLE, NULL) : NULL;
        if (!dlay[i]) {
            amce_log("double buffering", "could not make its layers");
            dbuf_on = TRUE;                 /* so that stop undoes it all */
            dback = 1;
            gfx_dbuf_stop(g);
            return;
        }
        SetFont(dlay[i]->rp, g->rp->Font);
        SetDrMd(dlay[i]->rp, JAM1);
    }
    dback = 1;
    dsafe_due = FALSE;
    g->rp = dlay[1]->rp;                    /* a copy of what is shown (SB_COPY_BITMAP) */
    dbuf_on = TRUE;
    dirty = FALSE;
    amce_log("double buffering", "on");
}

/* Show what was drawn, if anything was: TRUE if it flipped (which waited
 * for the vertical blank). */
BOOL gfx_present(struct Gfx *g)
{
    int front;
    ULONG t0;
    if (!dbuf_on || !dirty)
        return FALSE;
    dirty = FALSE;
    WaitBlit();
    t0 = time_ms();
    while (!ChangeScreenBuffer(g->scr, dsb[dback]))
        WaitTOF();                          /* the last change not done yet */
    dsafe_due = TRUE;
    front = dback;
    dback ^= 1;
    dbuf_wait_safe();                       /* the other no longer shown */
    BltBitMap(dsb[front]->sb_BitMap, 0, 0, dsb[dback]->sb_BitMap, 0, 0, g->w, g->h, 0xC0, 0xFF,
              NULL);
    WaitBlit();
    g->rp = dlay[dback]->rp;
    gfx_present_ms = time_ms() - t0;
    return TRUE;
}

/* A small update that needs no flip (the header's clock, the debug
 * readout): drawn into the hidden buffer as usual between these two, then
 * copied straight into the shown one. A flip shows the mouse pointer again
 * on some RTG systems (a software pointer), a blink a second otherwise. */
BOOL gfx_quiet_begin(void)
{
    return dirty;
}

void gfx_quiet_end(struct Gfx *g, BOOL was, WORD x0, WORD y0, WORD x1, WORD y1)
{
    (void)g;
    if (!dbuf_on)
        return;
    if (x1 >= x0 && y1 >= y0) {
        WaitBlit();
        BltBitMap(dsb[dback]->sb_BitMap, x0, y0, dsb[dback ^ 1]->sb_BitMap, x0, y0, x1 - x0 + 1,
                  y1 - y0 + 1, 0xC0, 0xFF, NULL);
        WaitBlit();
    }
    dirty = was;
}

/* Back to drawing straight onto the screen (the viewer, closing). */
void gfx_dbuf_stop(struct Gfx *g)
{
    int i;
    if (!dbuf_on)
        return;
    if (dlay[0] && dlay[1])
        gfx_present(g);
    dbuf_wait_safe();
    if (dback == 0) {
        /* the copy is shown: the screen's own bitmap back, with the same */
        BltBitMap(dsb[1]->sb_BitMap, 0, 0, dsb[0]->sb_BitMap, 0, 0, g->w, g->h, 0xC0, 0xFF, NULL);
        WaitBlit();
        while (!ChangeScreenBuffer(g->scr, dsb[0]))
            WaitTOF();
        dsafe_due = TRUE;
        dbuf_wait_safe();
    }
    for (i = 0; i < 2; i++) {
        if (dlay[i])
            DeleteLayer(0, dlay[i]);
        if (dli[i])
            DisposeLayerInfo(dli[i]);
        dlay[i] = NULL;
        dli[i] = NULL;
    }
    FreeScreenBuffer(g->scr, dsb[1]);
    FreeScreenBuffer(g->scr, dsb[0]);
    dsb[0] = dsb[1] = NULL;
    DeleteMsgPort(dsafe);
    dsafe = NULL;
    dbuf_on = FALSE;
    g->rp = g->win->RPort;
}


/* Can this screen show thumbnails (gfx_thumb)? */
BOOL gfx_thumbs(struct Gfx *g)
{
    return g->truecolor ? CyberGfxBase != NULL : g->pens_shared;
}

LONG gfx_thumb_colours(struct Gfx *g)
{
    struct PaletteExtra *pe = g->scr ? g->scr->ViewPort.ColorMap->PalExtra : NULL;
    LONG all = g->depth >= 8 ? 256 : 1L << g->depth;
    LONG n = ((pe ? (LONG)pe->pe_NFree : all / 2) + pic_npens) / 2;
    if (g->truecolor)
        return 256;                         /* any colours: no pens */
    if (blk_n)
        return blk_n;                       /* a block's worth */
    if (n > all)
        n = all;
    return n < 16 ? 16 : n;
}

/* Draw a thumbnail (thumb.c) centred across in the w x h box at x, y, in
 * pens obtained now; the one on screen is replaced as by gfx_picture_obj.
 * pi: what the picture is (shown in the pane). */
static BOOL thumb_draw(struct Gfx *g, const struct Thumb *t, const struct PicInfo *pi, WORD x,
                       WORD y, WORD w, WORD h, WORD *drawn_h, struct BitMap **keep);

BOOL gfx_thumb(struct Gfx *g, const struct Thumb *t, const struct PicInfo *pi, WORD x, WORD y,
               WORD w, WORD h, WORD *drawn_h)
{
    return thumb_draw(g, t, pi, x, y, w, h, drawn_h, NULL);
}

/* A thumbnail as a bitmap like the screen's, in pens taken now as the
 * picture on screen's (gfx_picture_free lets them go): for a text box to
 * copy from as it scrolls. NULL: could not. FreeBitMap it after. */
struct BitMap *gfx_thumb_bitmap(struct Gfx *g, const struct Thumb *t)
{
    struct BitMap *bm = NULL;
    struct PicInfo pi;
    memset(&pi, 0, sizeof(pi));
    if (!thumb_draw(g, t, &pi, 0, 0, g->w, g->h, NULL, &bm))
        return NULL;
    return bm;
}

static BOOL thumb_draw(struct Gfx *g, const struct Thumb *t, const struct PicInfo *pi, WORD x,
                       WORD y, WORD w, WORD h, WORD *drawn_h, struct BitMap **keep)
{
    struct ColorMap *cm;
    struct BitMap *tmp;
    UBYTE *buf, map[256], used[256], pens[256];
    UWORD npens = 0, nused = 0;
    ULONG i, n;
    WORD nx;
    int blk = -1;
    ULONG t_0;
    if (!t || !gfx_thumbs(g))
        return FALSE;
    if (g->truecolor) {
        /* its colour numbers and their colours, written as they are: off
         * screen, then up in one blit (written straight to the screen it
         * was seen going down, line by line) */
        struct Beam b;
        ULONG t0 = time_ms();
        nx = x + (w - (WORD)t->w) / 2;
        if ((tmp = AllocBitMap(t->w, t->h, GetBitMapAttr(g->rp->BitMap, BMA_DEPTH), BMF_MINPLANES,
                               g->rp->BitMap))) {
            struct RastPort trp;
            InitRastPort(&trp);
            trp.BitMap = tmp;
            WriteLUTPixelArray(t->px, 0, 0, t->mod, &trp, (APTR)t->pal, 0, 0, t->w, t->h,
                               CTABFMT_XRGB8);
            if (keep) {
                *keep = tmp;                /* the caller's to copy from */
                gfx_picture_free(g);
                return TRUE;
            }
            gfx_draw_ms[1] = time_ms() - t0;
            t0 = time_ms();
            BltBitMapRastPort(tmp, 0, 0, g->rp, nx, y, t->w, t->h, 0xC0);
            WaitBlit();
            FreeBitMap(tmp);
        } else if (keep) {
            return FALSE;
        } else {
            WriteLUTPixelArray(t->px, 0, 0, t->mod, g->rp, (APTR)t->pal, nx, y, t->w, t->h,
                               CTABFMT_XRGB8);
        }
        gfx_draw_ms[0] = 0;
        gfx_draw_ms[2] = time_ms() - t0;
        beam_init(g, &b);
        clear_old(g, &b, x, y, x + w - 1, y + h - 1, nx, y, t->w, t->h);
        gfx_picture_free(g);
        g->pic = *pi;
        g->pic_x = nx;
        g->pic_y = y;
        g->pic_w = t->w;
        g->pic_h = t->h;
        if (drawn_h)
            *drawn_h = t->h;
        return TRUE;
    }
    n = (ULONG)t->mod * t->h;
    cm = g->scr->ViewPort.ColorMap;
    t_0 = time_ms();
    if (!(buf = AllocVec(n + 32, MEMF_ANY)))     /* + 32: gfx_chunky reads 32 at a time */
        return FALSE;
    if (!(tmp = AllocBitMap(t->w, t->h, g->depth, BMF_CLEAR, g->rp->BitMap))) {
        FreeVec(buf);
        return FALSE;
    }
    memset(used, 0, sizeof(used));
    for (i = 0; i < n; i++)
        used[t->px[i]] = 1;
    for (i = 0; i < 256; i++)
        if (used[i] && i < t->ncol)
            nused++;
    if (blk_n && nused <= blk_n) {
        /* into the free block (not the old picture's): its pens set to
         * this one's colours before it is drawn. With one block, the old
         * picture goes first (it would change colour) */
        blk = blk_count == 2 && blk_on == 0 ? 1 : 0;
        if (blk == blk_on && g->pic_w) {
            struct Beam b;
            beam_init(g, &b);
            clear_old(g, &b, x, y, x + w - 1, y + h - 1, 0, 0, 0, 0);
            gfx_picture_free(g);
        }
        blk_ncol[blk] = 0;
        for (i = 0; i < 256; i++) {
            map[i] = PEN_BG;
            if (used[i] && i < t->ncol) {
                blk_rgb[blk][blk_ncol[blk]] = t->pal[i];
                map[i] = blk_pen[blk][blk_ncol[blk]++];
            }
        }
        blk_set(g, blk);
    }
    for (i = 0; i < 256 && blk < 0; i++) {
        map[i] = PEN_BG;
        if (used[i] && i < t->ncol) {
            ULONG c = t->pal[i];
            LONG pen = ObtainBestPen(cm, ((c >> 16) & 255) * 0x01010101UL,
                                     ((c >> 8) & 255) * 0x01010101UL, (c & 255) * 0x01010101UL,
                                     OBP_Precision, PRECISION_IMAGE, TAG_END);
            if (pen >= 0) {
                map[i] = pen;
                pens[npens++] = pen;
            }
        }
    }
    for (i = 0; i < n; i++)
        buf[i] = map[t->px[i]];
    gfx_draw_ms[0] = time_ms() - t_0;
    t_0 = time_ms();
    gfx_chunky(g, tmp, buf, t->w, t->h, t->mod);
    FreeVec(buf);
    gfx_draw_ms[1] = time_ms() - t_0;
    t_0 = time_ms();
    nx = x + (w - (WORD)t->w) / 2;
    if (keep) {
        *keep = tmp;                        /* the caller's to copy from */
        tmp = NULL;
    } else {
        struct Beam b;
        beam_init(g, &b);
        band_blit(g, &b, tmp, nx, y, t->w, t->h);
        clear_old(g, &b, x, y, x + w - 1, y + h - 1, nx, y, t->w, t->h);
    }
    gfx_draw_ms[2] = time_ms() - t_0;
    if (tmp)
        FreeBitMap(tmp);
    gfx_picture_free(g);                    /* now the old one's pens can go */
    CopyMem(pens, pic_pens, npens);
    pic_npens = npens;
    if (blk >= 0)
        blk_on = blk;
    if (keep)
        return TRUE;                        /* its pens held; no place on screen */
    g->pic = *pi;
    g->pic_x = nx;
    g->pic_y = y;
    g->pic_w = t->w;
    g->pic_h = t->h;
    if (drawn_h)
        *drawn_h = t->h;
    return TRUE;
}

/* Draw a decoded picture (pic_decode) fitted into w x h at (x, y), centred
 * across, keeping its aspect. Lowres pictures on a hires screen are
 * widened 2:1 so they keep their shape. The caller keeps the object (it
 * can be drawn again) unless *taken: one the datatype remapped into a
 * colour table holds those pens, so it stays with the screen until the
 * next picture replaces it. */
BOOL gfx_picture_obj(struct Gfx *g, APTR obj, BOOL remapped, WORD x, WORD y, WORD w, WORD h,
                     BOOL whole, WORD *drawn_h, BOOL *taken)
{
    dirty = TRUE;
    Object *o = obj;
    struct BitMapHeader *bmh = NULL;
    struct BitMap *bm = NULL, *tmp, *own = NULL;
    struct BitScaleArgs bsa;
    ULONG sw, sh, dw, dh;
    UBYTE pens[256];
    UWORD npens = 0;

    /* The picture already on screen stays, pens and all, until the new one
     * is drawn over it: the new one is in other pens, so the old one never
     * changes colour, and nothing is cleared first. Drawn band by band
     * with the beam (band_blit): a planar screen changes plane by plane,
     * and a half-drawn picture flashes in other pens' colours. */
    *taken = FALSE;
    if (!o || !(g->pens_shared || g->truecolor))
        return FALSE;
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_END);
    if (!bmh || !bmh->bmh_Width || !bmh->bmh_Height)
        return FALSE;
    sw = bmh->bmh_Width;
    sh = bmh->bmh_Height;
    pic_info(o, bmh, &g->pic);
    if (remapped) {
        GetDTAttrs(o, PDTA_DestBitMap, (ULONG)&bm, TAG_END);
        if (!bm)
            GetDTAttrs(o, PDTA_BitMap, (ULONG)&bm, TAG_END);
    } else {
        bm = own = own_remap(g, o, sw, sh, pens, &npens);
    }
    if (!bm)
        return FALSE;
    pic_fit(sw, sh, gfx_mode_tall(g->mode), w, h, whole, &dw, &dh);
    tmp = dw && dh ? AllocBitMap(dw, dh, g->depth, BMF_CLEAR, g->rp->BitMap) : NULL;
    if (!tmp) {
        if (own)
            FreeBitMap(own);
        release_pens(g, pens, npens);
        return FALSE;
    }
    memset(&bsa, 0, sizeof(bsa));
    bsa.bsa_SrcWidth = sw;
    bsa.bsa_SrcHeight = sh;
    bsa.bsa_XSrcFactor = sw;
    bsa.bsa_YSrcFactor = sh;
    bsa.bsa_XDestFactor = dw;
    bsa.bsa_YDestFactor = dh;
    bsa.bsa_SrcBitMap = bm;
    bsa.bsa_DestBitMap = tmp;
    BitMapScale(&bsa);
    WaitBlit();
    {
        struct Beam b;
        WORD nx = x + (w - bsa.bsa_DestWidth) / 2;
        beam_init(g, &b);
        band_blit(g, &b, tmp, nx, y, bsa.bsa_DestWidth, bsa.bsa_DestHeight);
        /* what the new one does not cover of the old, within the box */
        clear_old(g, &b, x, y, x + w - 1, y + h - 1, nx, y, bsa.bsa_DestWidth,
                  bsa.bsa_DestHeight);
    }
    if (drawn_h)
        *drawn_h = bsa.bsa_DestHeight;
    FreeBitMap(tmp);
    gfx_picture_free(g);                    /* now the old one's pens can go */
    if (own) {
        FreeBitMap(own);
        CopyMem(pens, pic_pens, npens);     /* its colours are in our pens now */
        pic_npens = npens;
    } else if (!g->truecolor) {
        pic_obj = o;                        /* holds the pens it is drawn in */
        *taken = TRUE;
    }
    g->pic_x = x + (w - bsa.bsa_DestWidth) / 2;
    g->pic_y = y;
    g->pic_w = bsa.bsa_DestWidth;
    g->pic_h = bsa.bsa_DestHeight;
    return TRUE;
}

void pic_dispose(APTR obj)
{
    if (obj)
        DisposeDTObject((Object *)obj);
}

BOOL gfx_picture(struct Gfx *g, const char *path, WORD x, WORD y, WORD w, WORD h, BOOL whole,
                 WORD *drawn_h)
{
    struct PicInfo pi;
    BOOL remapped = FALSE;
    APTR o;
    struct Thumb *t;
    BOOL taken, ok;
    if (!(g->pens_shared || g->truecolor))
        return FALSE;
    if (gfx_thumbs(g) &&
        (t = ilbm_thumb(path, w, h, gfx_mode_tall(g->mode), whole, gfx_thumb_colours(g), &pi))) {
        ok = gfx_thumb(g, t, &pi, x, y, w, h, drawn_h);     /* an ILBM: no datatype */
        thumb_free(t);
        return ok;
    }
    o = pic_decode(path, g->scr, g->truecolor, &remapped, &pi);
    if (!o)
        return FALSE;
    ok = gfx_picture_obj(g, o, remapped, x, y, w, h, whole, drawn_h, &taken);
    if (!taken)
        DisposeDTObject((Object *)o);
    return ok;
}

/* ---- which datatype reads each picture format (System Information).
 * datatypes.library is asked about a tiny file of each format, written to
 * T: and deleted again: the descriptor it picks, and the class behind it
 * (with its version), are what AmigaMCE will get for such a picture. */
static const UBYTE s_ilbm[] = { 'F','O','R','M', 0,0,0,46, 'I','L','B','M',
    'B','M','H','D', 0,0,0,20, 0,1, 0,1, 0,0, 0,0, 1, 0, 0, 0, 0,0, 10, 11, 0,1, 0,1,
    'C','M','A','P', 0,0,0,6, 0,0,0, 255,255,255, 'B','O','D','Y', 0,0,0,2, 0,0 };
static const UBYTE s_gif[] = { 'G','I','F','8','9','a', 1,0, 1,0, 0x80, 0, 0,
    0,0,0, 255,255,255, ',', 0,0, 0,0, 1,0, 1,0, 0, 2, 2, 0x44, 1, 0, ';' };
static const UBYTE s_jpeg[] = { 0xFF,0xD8, 0xFF,0xE0, 0,16, 'J','F','I','F',0, 1,1, 0, 0,1, 0,1,
    0,0, 0xFF,0xD9 };
static const UBYTE s_png[] = { 0x89,'P','N','G',13,10,26,10, 0,0,0,13, 'I','H','D','R',
    0,0,0,1, 0,0,0,1, 8, 2, 0, 0, 0, 0x90,0x77,0x53,0xDE, 0,0,0,0, 'I','E','N','D',
    0xAE,0x42,0x60,0x82 };
static const UBYTE s_bmp[] = { 'B','M', 58,0,0,0, 0,0,0,0, 54,0,0,0, 40,0,0,0, 1,0,0,0, 1,0,0,0,
    1,0, 24,0, 0,0,0,0, 4,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 255,255,255,0 };
static const UBYTE s_pcx[] = { 10, 5, 1, 8, 0,0, 0,0, 0,0, 0,0, 72,0, 72,0 };
static const UBYTE s_tga[] = { 0, 0, 2, 0,0, 0,0, 0, 0,0, 0,0, 1,0, 1,0, 24, 0, 255,255,255 };

void pic_datatypes(void (*out)(void *ctx, const char *line), void *ctx)
{
    static const struct { const char *fmt, *ext; const UBYTE *data; UWORD len; } s[] = {
        { "ILBM", "iff", s_ilbm, sizeof(s_ilbm) }, { "GIF", "gif", s_gif, sizeof(s_gif) },
        { "JPEG", "jpg", s_jpeg, sizeof(s_jpeg) }, { "PNG", "png", s_png, sizeof(s_png) },
        { "BMP", "bmp", s_bmp, sizeof(s_bmp) }, { "PCX", "pcx", s_pcx, sizeof(s_pcx) },
        { "TGA", "tga", s_tga, sizeof(s_tga) },
    };
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    struct Library *lib;
    char line[160], file[40];
    LONG i;
    if (!DataTypesBase) {
        out(ctx, "datatypes.library: missing (only IFF ILBM pictures can be shown)");
        return;
    }
    sprintf(line, "datatypes.library %lu.%lu", (unsigned long)DataTypesBase->lib_Version,
            (unsigned long)DataTypesBase->lib_Revision);
    if ((lib = OpenLibrary((CONST_STRPTR)"datatypes/picture.datatype", 0))) {
        sprintf(line + strlen(line), ", picture.datatype %lu.%lu",
                (unsigned long)lib->lib_Version, (unsigned long)lib->lib_Revision);
        CloseLibrary(lib);
    }
    out(ctx, line);
    me->pr_WindowPtr = (APTR)-1;
    for (i = 0; i < (LONG)(sizeof(s) / sizeof(s[0])); i++) {
        BPTR fh, l;
        struct DataType *dtn = NULL;
        sprintf(file, "T:AmigaMCE-probe.%s", s[i].ext);
        if ((fh = Open((CONST_STRPTR)file, MODE_NEWFILE))) {
            Write(fh, (APTR)s[i].data, s[i].len);
            Close(fh);
        }
        if ((l = Lock((CONST_STRPTR)file, SHARED_LOCK))) {
            dtn = ObtainDataTypeA(DTST_FILE, (APTR)l, NULL);
            UnLock(l);
        }
        DeleteFile((CONST_STRPTR)file);
        if (!dtn || !dtn->dtn_Header || dtn->dtn_Header->dth_GroupID != GID_PICTURE) {
            sprintf(line, "  %s: no datatype", s[i].fmt);
        } else {
            const struct DataTypeHeader *h = dtn->dtn_Header;
            char cls[64];
            const char *base = h->dth_BaseName ? (const char *)h->dth_BaseName : "?";
            sprintf(cls, "datatypes/%.40s.datatype", base);
            if ((lib = OpenLibrary((CONST_STRPTR)cls, 0))) {
                sprintf(line, "  %s: %.40s.datatype %lu.%lu (descriptor %.30s)", s[i].fmt, base,
                        (unsigned long)lib->lib_Version, (unsigned long)lib->lib_Revision,
                        h->dth_Name ? (const char *)h->dth_Name : "?");
                CloseLibrary(lib);
            } else {
                sprintf(line, "  %s: %.40s.datatype, not found (descriptor %.30s)", s[i].fmt, base,
                        h->dth_Name ? (const char *)h->dth_Name : "?");
            }
        }
        if (dtn)
            ReleaseDataType(dtn);
        if (i == 0)
            amce_strlcat(line, "; previews read by AmigaMCE", sizeof(line));
        out(ctx, line);
    }
    me->pr_WindowPtr = oldwin;
}
