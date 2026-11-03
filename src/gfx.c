/*
 * AmigaMCE - screen, palette, font and drawing primitives (native chipset)
 * Copyright (c) 2026 Brian King
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

extern struct GfxBase *GfxBase;
extern struct Library *DiskfontBase, *DataTypesBase;

static Object *pic_obj;         /* datatype object of the picture on screen */

static void cache_art(struct Gfx *g);

static void layout_metrics(struct Gfx *g)
{
    UBYTE s = g->sx;
    g->menu_w = s == 2 ? 200 : 104;
    g->top = 26;
    g->row_h = 18;
    g->pane_x = g->menu_w + 6 * s;
    g->pane_w = g->w - g->pane_x - 4 * s;
    g->list_top = 44;
    g->list_row_h = 14;
    g->footer_y = g->h - 12;
}

static void open_font(struct Gfx *g)
{
    struct TextAttr ta;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;

    g->font = NULL;
    g->font_is_ours = FALSE;
    if (DiskfontBase) {
        ta.ta_Name = (STRPTR)(g->layout == LAYOUT_HIRES ? "PROGDIR:Fonts/amce-pixel-hi.font"
                                                        : "PROGDIR:Fonts/amce-pixel.font");
        ta.ta_YSize = 12;
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
    { "rtg-800",    "800 x 600  RTG",            800, 600, K_RTG,   0 },
};

const char *gfx_mode_key(int sm) { return modes[sm].key; }
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
    default:      return caps_find_rtg_mode(800, 600);
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

BOOL gfx_open(struct Gfx *g, const struct Caps *c, int sm)
{
    static UWORD pens[] = { (UWORD)~0 };
    ULONG mode;
    int i;

    if (sm == SM_AUTO || gfx_mode_problem(c, sm))
        sm = gfx_mode_auto(c);
    mode = mode_id(sm);
    g->mode = sm;
    g->w = modes[sm].w;
    g->h = modes[sm].h;
    if (modes[sm].kind == K_HIRES) {
        g->layout = LAYOUT_HIRES;
        g->sx = 2;
    } else {
        g->layout = LAYOUT_LORES;   /* square pixels: lowres artwork 1:1 */
        g->sx = 1;
    }
    g->depth = c->aga || modes[sm].kind == K_RTG ? 8 : 5;
    if (mode == (ULONG)INVALID_ID)
        return FALSE;
    layout_metrics(g);

    g->scr = OpenScreenTags(NULL,
                            SA_Width, g->w, SA_Height, g->h, SA_Depth, g->depth,
                            SA_DisplayID, mode, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
                            SA_Type, CUSTOMSCREEN, SA_Title, (ULONG)"AmigaMCE",
                            SA_Pens, (ULONG)pens, SA_SharePens, TRUE,
                            SA_Draggable, FALSE, SA_Interleaved, TRUE, TAG_END);
    if (!g->scr)
        return FALSE;
    g->planar = (GetBitMapAttr(g->scr->RastPort.BitMap, BMA_FLAGS) & BMF_STANDARD) != 0;

    LoadRGB32(&g->scr->ViewPort, (ULONG *)ui_palette32);
    g->pens_shared = FALSE;
    if (g->depth == 8) {
        /* Keep the UI pens for ourselves; 32..255 stay free for datatypes. */
        for (i = 0; i < NUM_UI_PENS; i++)
            ObtainPen(g->scr->ViewPort.ColorMap, i, ui_palette32[1 + i * 3],
                      ui_palette32[2 + i * 3], ui_palette32[3 + i * 3], PEN_EXCLUSIVE);
        g->pens_shared = TRUE;
    }

    g->win = OpenWindowTags(NULL,
                            WA_CustomScreen, (ULONG)g->scr, WA_Left, 0, WA_Top, 0,
                            WA_Width, g->w, WA_Height, g->h, WA_Backdrop, TRUE,
                            WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
                            WA_SimpleRefresh, TRUE, WA_NoCareRefresh, TRUE,
                            WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE,
                            TAG_END);
    if (!g->win) {
        CloseScreen(g->scr);
        g->scr = NULL;
        return FALSE;
    }
    g->rp = g->win->RPort;
    SetDrMd(g->rp, JAM1);
    open_font(g);
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
    if (g->logo_bm) {
        FreeBitMap(g->logo_bm);
        g->logo_bm = NULL;
    }
    if (g->icon_masks) {
        FreeMem(g->icon_masks, (ULONG)ICON_COUNT * 16 * g->icon_bpr);
        g->icon_masks = NULL;
    }
    if (g->win) {
        CloseWindow(g->win);
        g->win = NULL;
    }
    if (g->font) {
        CloseFont(g->font);
        g->font = NULL;
    }
    if (g->scr) {
        if (g->pens_shared)
            for (i = 0; i < NUM_UI_PENS; i++)
                ReleasePen(g->scr->ViewPort.ColorMap, i);
        CloseScreen(g->scr);
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

WORD gfx_textlen(struct Gfx *g, const char *s)
{
    return TextLength(g->rp, (CONST_STRPTR)s, strlen(s));
}

void gfx_text(struct Gfx *g, WORD x, WORD y, const char *s, UBYTE pen, WORD maxw)
{
    struct TextExtent te;
    ULONG n = strlen(s);
    if (!n)
        return;
    if (maxw > 0)
        n = TextFit(g->rp, (CONST_STRPTR)s, n, &te, NULL, 1, maxw, g->font_h + 1);
    if (!n)
        return;
    SetAPen(g->rp, pen);
    Move(g->rp, x, y + g->baseline);
    Text(g->rp, (CONST_STRPTR)s, n);
}

/* The logo and icons go through the same blitter / RTG path as all other
 * drawing. Plotting them pixel by pixel from the CPU (WritePixel) left
 * them missing on RTG screens, where the card's own fills of the header
 * and menu rows landed after the CPU's pixels. The logo is rendered once
 * per screen into a bitmap in the screen's format and blitted from there. */
static void logo_art(const struct Gfx *g, const UBYTE **px, UWORD *w, UWORD *h)
{
    BOOL hi = g->layout == LAYOUT_HIRES;
    *px = hi ? logo_hi : logo_lo;
    *w = hi ? logo_hi_w : logo_lo_w;
    *h = hi ? logo_hi_h : logo_lo_h;
}

static void cache_art(struct Gfx *g)
{
    const UBYTE *px;
    UWORD w, h, i, j;
    ULONG size;

    logo_art(g, &px, &w, &h);
    g->logo_bm = AllocBitMap(w, h, g->depth, BMF_CLEAR, g->rp->BitMap);
    if (g->logo_bm) {
        struct RastPort rp;
        InitRastPort(&rp);
        rp.BitMap = g->logo_bm;
        if (GfxBase->LibNode.lib_Version >= 40) {
            WriteChunkyPixels(&rp, 0, 0, w - 1, h - 1, (UBYTE *)px, w);
        } else {
            for (j = 0; j < h; j++)
                for (i = 0; i < w; i++) {
                    SetAPen(&rp, px[j * w + i]);
                    WritePixel(&rp, i, j);
                }
        }
    }

    /* BltTemplate reads its source with the blitter: chip RAM, even rows. */
    g->icon_bpr = g->sx == 2 ? 4 : 2;
    size = (ULONG)ICON_COUNT * 16 * g->icon_bpr;
    g->icon_masks = AllocMem(size, MEMF_CHIP);
    if (g->icon_masks)
        CopyMem(g->sx == 2 ? (APTR)icons32 : (APTR)icons16, g->icon_masks, size);
}

void gfx_logo(struct Gfx *g, WORD x, WORD y)
{
    const UBYTE *px;
    UWORD w, h;
    logo_art(g, &px, &w, &h);
    if (g->logo_bm)
        BltBitMapRastPort(g->logo_bm, 0, 0, g->rp, x, y, w, h, 0xC0);
}

void gfx_icon(struct Gfx *g, int icon, WORD x, WORD y, UBYTE pen)
{
    if (!g->icon_masks)
        return;
    SetAPen(g->rp, pen);
    SetDrMd(g->rp, JAM1);
    BltTemplate((PLANEPTR)(g->icon_masks + (ULONG)icon * 16 * g->icon_bpr), 0, g->icon_bpr,
                g->rp, x, y, 16 * g->sx, 16);
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
    if (!DataTypesBase || !g->pens_shared)
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
    sxe = (g->sx == 2 && sw <= 384) ? 2 : 1;
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
