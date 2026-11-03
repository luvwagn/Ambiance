/*
 * AmigaMCE - screen, palette, font and drawing primitives (native chipset)
 * Copyright (c) 2026 Brian King
 */
#include "amce.h"

#include <string.h>
#include <graphics/modeid.h>
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

extern struct Library *DiskfontBase, *DataTypesBase;

static Object *pic_obj;         /* datatype object of the picture on screen */

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

BOOL gfx_open(struct Gfx *g, const struct Caps *c)
{
    static UWORD pens[] = { (UWORD)~0 };
    ULONG mon = c->pal ? PAL_MONITOR_ID : NTSC_MONITOR_ID;
    ULONG mode;
    int i;

    g->h = c->pal ? 256 : 200;
    if (c->aga) {
        g->layout = LAYOUT_HIRES;
        g->sx = 2;
        g->w = 640;
        g->depth = 8;
        mode = mon | HIRES_KEY;
    } else {
        g->layout = LAYOUT_LORES;
        g->sx = 1;
        g->w = 320;
        g->depth = 5;
        mode = mon | LORES_KEY;
    }
    if (ModeNotAvailable(mode))
        mode = g->layout == LAYOUT_HIRES ? HIRES_KEY : LORES_KEY;
    layout_metrics(g);

    g->scr = OpenScreenTags(NULL,
                            SA_Width, g->w, SA_Height, g->h, SA_Depth, g->depth,
                            SA_DisplayID, mode, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
                            SA_Type, CUSTOMSCREEN, SA_Title, (ULONG)"AmigaMCE",
                            SA_Pens, (ULONG)pens, SA_SharePens, TRUE,
                            SA_Draggable, FALSE, SA_Interleaved, TRUE, TAG_END);
    if (!g->scr)
        return FALSE;

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

void gfx_logo(struct Gfx *g, WORD x, WORD y)
{
    const UBYTE *px = g->layout == LAYOUT_HIRES ? logo_hi : logo_lo;
    UWORD w = g->layout == LAYOUT_HIRES ? logo_hi_w : logo_lo_w;
    UWORD h = g->layout == LAYOUT_HIRES ? logo_hi_h : logo_lo_h;
    UWORD i, j;
    UBYTE last = 255;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            UBYTE pen = px[j * w + i];
            if (pen == PEN_BAR)
                continue;               /* background already painted */
            if (pen != last) {
                SetAPen(g->rp, pen);
                last = pen;
            }
            WritePixel(g->rp, x + i, y + j);
        }
}

void gfx_icon(struct Gfx *g, int icon, WORD x, WORD y, UBYTE pen)
{
    UWORD r, b;
    SetAPen(g->rp, pen);
    for (r = 0; r < 16; r++) {
        if (g->sx == 2) {
            ULONG bits = icons32[icon][r];
            for (b = 0; b < 32; b++)
                if (bits & (0x80000000UL >> b))
                    WritePixel(g->rp, x + b, y + r);
        } else {
            UWORD bits = icons16[icon][r];
            for (b = 0; b < 16; b++)
                if (bits & (0x8000 >> b))
                    WritePixel(g->rp, x + b, y + r);
        }
    }
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
