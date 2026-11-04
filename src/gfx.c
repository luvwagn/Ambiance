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
#include <stdio.h>

extern struct GfxBase *GfxBase;
extern struct Library *DiskfontBase, *DataTypesBase;

static Object *pic_obj;         /* datatype object of the picture on screen */

static void cache_art(struct Gfx *g);

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
    if (modes[smode].kind != K_RTG || gfx_mode_depth(smode) <= 8)
        return FALSE;
    sprintf(path, "PROGDIR:Fonts/amce-smooth/%lu", (unsigned long)modes[smode].h);
    me->pr_WindowPtr = (APTR)-1;
    l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = oldwin;
    if (l)
        UnLock(l);
    return l != 0;
}

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
    /* Scale (see amce.h): native 1x; RTG a whole factor of its lines/256,
     * or exactly lines/256 in the smooth style. */
    g->kint = 1;
    g->style = STYLE_PIXEL;
    g->sm = NULL;
    if (modes[smode].kind == K_RTG) {
        g->kint = g->h / 256;
        if (g->kint < 1)
            g->kint = 1;
        if (g->kint > 3)
            g->kint = 3;
    }
    g->k = (UWORD)g->kint << 8;
    if (style == STYLE_SMOOTH && modes[smode].kind == K_RTG && g->truecolor &&
        (g->sm = sm_load(g->h))) {
        g->style = STYLE_SMOOTH;
        g->k = g->h;                        /* lines / 256, x 256 */
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
    return TRUE;
}

void gfx_picture_free(struct Gfx *g)
{
    (void)g;
    if (pic_obj) {
        DisposeDTObject(pic_obj);
        pic_obj = NULL;
    }
}

void gfx_close(struct Gfx *g)
{
    int i;
    gfx_picture_free(g);
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
        FreeMem(g->mark_masks, (ULONG)MARK_RADIO_ON * g->mark_px * g->mark_bpr);
        g->mark_masks = NULL;
    }
    if (g->sm) {
        sm_free(g->sm);
        g->sm = NULL;
    }
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
    if (x1 < x0 || y1 < y0)
        return;
    SetAPen(g->rp, pen);
    RectFill(g->rp, x0, y0, x1, y1);
}

/* Clear to the background, high planes first. A planar fill writes one
 * plane at a time; clearing planes 3+ first means a pixel caught halfway
 * shows one of the dark UI pens 0-7, not a stray colour such as pen 8. */
void gfx_fill_bg(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1)
{
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
    g->icon_masks = enlarge_words(&icons16[0][0], ICON_COUNT, 16, k, &g->icon_bpr);
    g->mark_masks = enlarge_words(&marks16[0][0], MARK_RADIO_ON, MARK_SIZE, k, &g->mark_bpr);
}

void gfx_logo(struct Gfx *g, WORD x, WORD y)
{
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
void gfx_mark(struct Gfx *g, int mark, WORD x, WORD y, UBYTE fg, UBYTE bg)
{
    PLANEPTR m;
    if (mark < MARK_ARROW || mark > MARK_RADIO_ON)
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
/* The list overlay's letter, its ink about ink_h pixels tall. Pixel style:
 * the UI font's own glyph blown up in whole pixels. Smooth: smooth.c. */
BOOL gfx_bigletter(struct Gfx *g, UBYTE c, WORD ink_h, struct BigLetter *bl)
{
    struct TextFont *f = g->font;
    const UBYTE *data;
    ULONG lw;
    UWORD off, gw, mod, x, yy;
    LONG idx;
    WORD top = -1, bot = -1, sc;
    gfx_bigletter_free(bl);
    if (!c)
        return FALSE;
    if (g->sm)
        return sm_bigletter(g->sm, c, ink_h, bl);
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
    bl->nlev = 1;
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

void gfx_bigletter_draw(struct Gfx *g, const struct BigLetter *bl, WORD x, WORD y,
                        UBYTE fg, UBYTE bg)
{
    UBYTE lv;
    ULONG plane = (ULONG)bl->bpr * bl->h;
    UBYTE *m = bl->mask;
    if (!m)
        return;
    SetDrMd(g->rp, JAM1);
    for (lv = 1; lv <= bl->nlev; lv++, m += plane) {
        UBYTE pen = bl->nlev == 1 ? fg : sm_pen(g->sm, g, fg, bg, lv);
        SetAPen(g->rp, pen);
        BltTemplate((PLANEPTR)m, 0, bl->bpr, g->rp, x, y, bl->w, bl->h);
    }
}

void gfx_bigletter_free(struct BigLetter *bl)
{
    if (bl->mask) {
        WaitBlit();
        FreeMem(bl->mask, bl->size);
    }
    bl->mask = NULL;
    bl->w = bl->h = 0;
}

/* Decode a picture through the registered datatypes and draw it scaled to
 * fit (w x h), keeping its aspect. Lowres pictures on a hires screen are
 * widened 2:1 so they keep their shape. */
BOOL gfx_picture(struct Gfx *g, const char *path, WORD x, WORD y, WORD w, WORD h)
{
    struct BitMapHeader *bmh = NULL;
    struct BitMap *bm = NULL, *tmp;
    struct BitScaleArgs bsa;
    ULONG sw, sh, sxe, dw, dh;

    gfx_picture_free(g);
    if (!DataTypesBase || !(g->pens_shared || g->truecolor))
        return FALSE;
    pic_obj = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE,
                          PDTA_Screen, (ULONG)g->scr, PDTA_Remap, TRUE,
                          PDTA_DestMode, PMODE_V43, OBP_Precision, PRECISION_IMAGE,
                          TAG_END);
    if (!pic_obj)
        return FALSE;
    if (!DoDTMethod(pic_obj, NULL, NULL, DTM_PROCLAYOUT, NULL, (ULONG)TRUE)) {
        gfx_picture_free(g);
        return FALSE;
    }
    GetDTAttrs(pic_obj, PDTA_BitMapHeader, (ULONG)&bmh, PDTA_DestBitMap, (ULONG)&bm, TAG_END);
    if (!bm)
        GetDTAttrs(pic_obj, PDTA_BitMap, (ULONG)&bm, TAG_END);
    if (!bm || !bmh || !bmh->bmh_Width || !bmh->bmh_Height) {
        gfx_picture_free(g);
        return FALSE;
    }
    sw = bmh->bmh_Width;
    sh = bmh->bmh_Height;
    /* A lowres picture on a tall-pixel hires screen: double its width to
     * keep its shape. */
    sxe = (gfx_mode_tall(g->mode) && sw <= 384) ? 2 : 1;
    /* fit (sw*sxe) x sh into w x h */
    dw = w;
    dh = (sh * w) / (sw * sxe);
    if (dh > (ULONG)h) {
        dh = h;
        dw = (sw * sxe * h) / sh;
    }
    if (!dw || !dh)
        return FALSE;
    tmp = AllocBitMap(dw, dh, g->depth, BMF_CLEAR, g->rp->BitMap);
    if (!tmp)
        return FALSE;
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
    BltBitMapRastPort(tmp, 0, 0, g->rp, x + (w - bsa.bsa_DestWidth) / 2, y,
                      bsa.bsa_DestWidth, bsa.bsa_DestHeight, 0xC0);
    WaitBlit();
    FreeBitMap(tmp);
    return TRUE;
}
