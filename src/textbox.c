/*
 * Ambiance - smooth-scrolling, flicker-free text box with a scroll bar
 * Copyright (c) 2026 Brian King
 *
 * The text is rendered once into an off-screen bitmap. Scrolling then
 * never clears or redraws on screen: each frame copies the visible window
 * of that bitmap to the screen with one blit started right after the
 * vertical blank, so the beam never sees a half-drawn box.
 *
 * Everything in the box, text and scroll bar alike, uses pens below 8, so
 * the box only ever needs bitplanes 0-2; planes 3+ are cleared once when
 * the page appears. The bitmap has three planes and the blit is masked to
 * them: on an 8-plane AGA screen that is 3/8 of the blitter time a full
 * copy would need, which keeps a step well inside one frame.
 *
 * Planar fills and blits write one plane at a time, so the beam can catch
 * a pixel between planes. Keeping to pens 0-7 and clearing high planes
 * first means such in-between colours are always one of the dark UI pens,
 * never a stray bright one (pen 9 -> 0 used to pass through orange pen 8).
 *
 * A picture can follow the text (tb->pic, a bitmap like the screen's): it
 * scrolls with it. The text's bitmap stays three planes; the picture is
 * blitted on its own, all planes, over just the rows of it in view, and
 * the text is not blitted over it (a full-depth copy of the whole box each
 * frame took longer than a frame on an 8-plane laced screen: the picture
 * was caught half copied, and the CPU waited on the blitter). Where the
 * picture was and is no longer, its high planes are cleared.
 *
 * Scrolling glides (eases to where it is heading) only where that is
 * cheap and worth it: on an RTG screen, and for text more than a couple
 * of boxes long. A native screen's copy is the blitter's, and on an
 * 8-plane laced screen one copy of the box takes longer than a frame, so
 * a glide kept the CPU waiting on the blitter the whole way; there, and
 * for short text anywhere, a scroll goes straight to its place (jump).
 *
 * On an RTG screen there is no off-screen bitmap: the text is drawn
 * straight onto the screen, clipped to the box by a layers clip region.
 * Each scroll step moves what is already there with ScrollRaster (a
 * screen-to-screen copy the card accelerates) and draws only the strip
 * that scrolled into view. That keeps truecolor screens from holding a
 * 32-bit copy of every page in graphics memory, and avoids off-screen RTG
 * bitmaps, which lost short drawing operations under Amiberry's uaegfx.
 * The same direct mode is the fallback when chip RAM for the bitmap runs
 * out on a native screen.
 */
#include "amce.h"

#include <string.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>
#include <graphics/regions.h>

extern struct GfxBase *GfxBase;

#define TB_PLANES     3
#define TB_PLANE_MASK 0x07
#define TB_MAX_ROWS   2048      /* tallest pre-rendered text, in pixels */

static LONG max_pos(const struct TextBox *tb)
{
    LONG m = tb->content_h - tb->h;
    return m > 0 ? m : 0;
}

static LONG clamp_pos(const struct TextBox *tb, LONG v)
{
    LONG m = max_pos(tb);
    return v < 0 ? 0 : v > m ? m : v;
}

void tb_close(struct TextBox *tb)
{
    if (tb->bm) {
        WaitBlit();
        FreeBitMap(tb->bm);
    }
    tb->bm = NULL;
    tb->open = FALSE;
}

BOOL tb_open(struct Gfx *g, struct TextBox *tb, WORD x, WORD y, WORD w, WORD h,
             const struct ItemList *lines, BOOL first_white)
{
    struct RastPort rp;
    LONG i, rows;

    tb_close(tb);
    tb->x = x;
    tb->y = y;
    tb->w = w;
    tb->h = h;
    tb->line_h = g->font_h + GU(g, 1);
    tb->lines = lines;
    tb->first_white = first_white;
    tb->content_h = lines->count * tb->line_h;
    if (tb->pic) {
        tb->pic_y = tb->content_h + tb->line_h / 2;     /* half a line's gap */
        tb->content_h = tb->pic_y + tb->pic_h;
    }
    if (g->planar && tb->content_h > TB_MAX_ROWS)     /* the bitmap's limit */
        tb->content_h = TB_MAX_ROWS - TB_MAX_ROWS % tb->line_h;
    tb->bar_w = tb->content_h > h ? GU(g, 5) : 0;
    tb->jump = g->planar || tb->content_h <= 3 * h;
    tb->bar_x = x + w + GU(g, 2);
    tb->pos = clamp_pos(tb, tb->pos);
    tb->target = clamp_pos(tb, tb->target);
    tb->drawn_pos = -1;
    tb->drawn = FALSE;
    tb->thumb_y = tb->thumb_h = -1;
    tb->dragging = FALSE;
    tb->open = TRUE;

    rows = tb->content_h > h ? tb->content_h : h;
    tb->mask = TB_PLANE_MASK;
    tb->pic_a = 1;
    tb->pic_z = 0;                          /* none on screen yet */
    if (!g->planar)
        return TRUE;    /* RTG: drawn directly, see above */
    tb->bm = AllocBitMap(w, rows, TB_PLANES, BMF_CLEAR, NULL);
    if (!tb->bm)
        return FALSE;   /* low on chip RAM: drawn directly instead */

    InitRastPort(&rp);
    rp.BitMap = tb->bm;
    SetFont(&rp, g->font);
    SetDrMd(&rp, JAM1);
    for (i = 0; i < lines->count && (i + 1) * tb->line_h <= tb->content_h; i++) {
        const char *s = lines->items[i].label;
        if (!s[0])
            continue;
        gfx_text_rp(g, &rp, 0, i * tb->line_h, s, (first_white && i == 0) ? PEN_WHITE : PEN_TEXT,
                    w);
    }
    return TRUE;
}

static void draw_bar(struct Gfx *g, struct TextBox *tb)
{
    LONG range = max_pos(tb), th, ty;
    WORD x0 = tb->bar_x, x1 = tb->bar_x + tb->bar_w - 1, y0 = tb->y, y1 = tb->y + tb->h - 1;
    if (!tb->bar_w)
        return;
    th = (LONG)tb->h * tb->h / tb->content_h;
    if (th < GU(g, 8))
        th = GU(g, 8);
    ty = range ? (tb->h - th) * tb->pos / range : 0;
    if (ty == tb->thumb_y && th == tb->thumb_h)
        return;
    tb->thumb_y = ty;
    tb->thumb_h = th;
    /* Three fills that never overlap: no pixel is drawn twice. */
    gfx_fill(g, x0, y0, x1, y0 + ty - 1, PEN_SELDIM);
    gfx_fill(g, x0, y0 + ty, x1, y0 + ty + th - 1,
             (tb->dragging || tb->focused) ? PEN_TEXT : PEN_DIM);
    gfx_fill(g, x0, y0 + ty + th, x1, y1, PEN_SELDIM);
}

/* Rows sy0..sy1, columns sx0..sx1 of the box from the text's bitmap. */
static void blit_text(struct Gfx *g, struct TextBox *tb, WORD sx0, WORD sx1, WORD sy0, WORD sy1)
{
    if (sx1 < sx0 || sy1 < sy0)
        return;
    BltBitMap(tb->bm, sx0 - tb->x, tb->pos + (sy0 - tb->y), g->rp->BitMap, sx0, sy0,
              sx1 - sx0 + 1, sy1 - sy0 + 1, 0xC0, tb->mask, NULL);
}

static void blit_window(struct Gfx *g, struct TextBox *tb)
{
    gfx_dirty();
    struct Layer *l = g->rp->Layer;    /* the window's, or a double buffer's */
    WORD y0 = tb->y, y1 = tb->y + tb->h - 1, x0 = tb->x, x1 = tb->x + tb->w - 1;
    LockLayerRom(l);
    if (!tb->pic) {
        /* The bitmap is at least h rows below pos (cleared past the text),
         * so a full-height copy also blanks whatever the box showed before. */
        BltBitMap(tb->bm, 0, tb->pos, g->rp->BitMap, tb->x, tb->y, tb->w, tb->h,
                  0xC0, tb->mask, NULL);
    } else {
        LONG ptop = tb->y + tb->pic_y - tb->pos;
        WORD a = ptop > y0 ? (WORD)ptop : y0;
        WORD z = ptop + tb->pic_h - 1 < y1 ? (WORD)(ptop + tb->pic_h - 1) : y1;
        WORD px0 = tb->x + (tb->w - tb->pic_w) / 2, px1 = px0 + tb->pic_w - 1;
        if (a > z) {
            blit_text(g, tb, x0, x1, y0, y1);           /* the picture out of view */
        } else {
            blit_text(g, tb, x0, x1, y0, a - 1);        /* above it */
            blit_text(g, tb, x0, x1, z + 1, y1);        /* below it */
            blit_text(g, tb, x0, px0 - 1, a, z);        /* beside it */
            blit_text(g, tb, px1 + 1, x1, a, z);
            BltBitMap(tb->pic, 0, a - ptop, g->rp->BitMap, px0, a, tb->pic_w, z - a + 1, 0xC0,
                      0xFF, NULL);
        }
        /* the picture's high planes cleared where it was and is no longer */
        if (tb->pic_a <= tb->pic_z) {
            WORD oa = tb->pic_a, oz = tb->pic_z;
            SetWriteMask(g->rp, 0xFF & ~TB_PLANE_MASK);
            SetAPen(g->rp, GPEN(PEN_BG));
            if (a > z) {
                RectFill(g->rp, px0, oa, px1, oz);
            } else {
                if (oa < a)
                    RectFill(g->rp, px0, oa, px1, (oz < a - 1 ? oz : a - 1));
                if (oz > z)
                    RectFill(g->rp, px0, (oa > z + 1 ? oa : z + 1), px1, oz);
            }
            SetWriteMask(g->rp, 0xFF);
        }
        tb->pic_a = a;
        tb->pic_z = z;
    }
    UnlockLayerRom(l);
    tb->drawn_pos = tb->pos;
}

/* Direct mode: draw the text that belongs in screen rows sy0..sy1 of the
 * box, clipped to them. Without a clip region (no memory for one) only
 * lines that fit entirely are drawn, so nothing spills outside the box. */
static void draw_rows(struct Gfx *g, struct TextBox *tb, WORD sy0, WORD sy1)
{
    gfx_dirty();
    struct Rectangle r;
    struct Region *rg, *old = NULL;
    LONG i, top;
    BOOL clipped = FALSE;

    if (sy1 < sy0)
        return;
    r.MinX = tb->x;
    r.MinY = sy0;
    r.MaxX = tb->x + tb->w - 1;
    r.MaxY = sy1;
    if ((rg = NewRegion())) {
        if (OrRectRegion(rg, &r)) {
            old = InstallClipRegion(g->rp->Layer, rg);
            clipped = TRUE;
        }
    }
    gfx_fill(g, r.MinX, sy0, r.MaxX, sy1, PEN_BG);
    for (i = (tb->pos + (sy0 - tb->y)) / tb->line_h; i < tb->lines->count; i++) {
        top = tb->y + i * tb->line_h - tb->pos;
        if (top > sy1)
            break;
        if (!clipped && (top < sy0 || top + tb->line_h - 1 > sy1))
            continue;
        gfx_text(g, tb->x, top, tb->lines->items[i].label,
                 (tb->first_white && i == 0) ? PEN_WHITE : PEN_TEXT, PEN_BG, tb->w, TS_BODY);
    }
    if (tb->pic) {
        /* the picture's rows in sy0..sy1 */
        LONG ptop = tb->y + tb->pic_y - tb->pos, a = ptop > sy0 ? ptop : sy0,
             z = ptop + tb->pic_h - 1 < sy1 ? ptop + tb->pic_h - 1 : sy1;
        if (a <= z)
            BltBitMapRastPort(tb->pic, 0, a - ptop, g->rp, tb->x + (tb->w - tb->pic_w) / 2, a,
                              tb->pic_w, z - a + 1, 0xC0);
    }
    if (clipped)
        InstallClipRegion(g->rp->Layer, old);
    if (rg)
        DisposeRegion(rg);
}

/* Direct mode scroll: move what is on screen, draw the strip exposed. */
static void scroll_direct(struct Gfx *g, struct TextBox *tb)
{
    gfx_dirty();
    LONG d = tb->pos - tb->drawn_pos;
    WORD y1 = tb->y + tb->h - 1;
    if (!d)
        return;
    if (d >= tb->h || -d >= tb->h || tb->drawn_pos < 0) {
        draw_rows(g, tb, tb->y, y1);
    } else {
        SetBPen(g->rp, GPEN(PEN_BG));
        ScrollRaster(g->rp, 0, d, tb->x, tb->y, tb->x + tb->w - 1, y1);
        if (d > 0)
            draw_rows(g, tb, y1 - d + 1, y1);
        else
            draw_rows(g, tb, tb->y, tb->y - d - 1);
    }
    tb->drawn_pos = tb->pos;
}

/* Whole box, e.g. after the page opens or the screen comes back. Nothing
 * is blanked first: the new text is copied straight over the old. */
void tb_draw(struct Gfx *g, struct TextBox *tb)
{
    if (gfx_off) {
        gfx_off_hits++;
        return;
    }
    WORD x1 = tb->bar_x + GU(g, 3) - 1, y1 = tb->y + tb->h - 1;
    if (!tb->open)
        return;
    tb->thumb_y = tb->thumb_h = -1;
    tb->drawn = TRUE;
    if (tb->bm) {
        /* Planes 3+ to zero (a screenshot may have been here), then the
         * text, then the gap and bar column, all within planes 0-2. */
        if (g->planar && g->depth > TB_PLANES) {
            SetWriteMask(g->rp, 0xFF & ~TB_PLANE_MASK);
            gfx_fill(g, tb->x, tb->y, x1, y1, PEN_BG);
            SetWriteMask(g->rp, 0xFF);
        }
        tb->pic_a = 1;
        tb->pic_z = 0;                      /* its high planes cleared with the rest */
        blit_window(g, tb);
        gfx_fill(g, tb->x + tb->w, tb->y, tb->bar_x - 1, y1, PEN_BG);
        if (!tb->bar_w)
            gfx_fill(g, tb->bar_x, tb->y, x1, y1, PEN_BG);
    } else {
        if (g->planar && g->depth > TB_PLANES) {
            SetWriteMask(g->rp, 0xFF & ~TB_PLANE_MASK);
            gfx_fill(g, tb->x, tb->y, x1, y1, PEN_BG);
            SetWriteMask(g->rp, 0xFF);
        }
        draw_rows(g, tb, tb->y, y1);
        gfx_fill(g, tb->x + tb->w, tb->y, tb->bar_x - 1, y1, PEN_BG);
        if (!tb->bar_w)
            gfx_fill(g, tb->bar_x, tb->y, x1, y1, PEN_BG);
        tb->drawn_pos = tb->pos;
    }
    draw_bar(g, tb);
}

BOOL tb_animating(const struct TextBox *tb)
{
    return tb->open && tb->pos != tb->target;
}

/* Pixels still to scroll in direction dir (+1 down, -1 up) from where
 * the box is heading. */
LONG tb_remaining(const struct TextBox *tb, LONG dir)
{
    if (!tb->open)
        return 0;
    return dir > 0 ? max_pos(tb) - tb->target : tb->target;
}

void tb_scroll_by(struct TextBox *tb, LONG dpx)
{
    if (tb->open)
        tb->target = clamp_pos(tb, tb->target + dpx);
}

/* One animation frame: ease a quarter of the way to the target (at least
 * one pixel; all of it when jumping or dragging), then show it. Call right
 * after WaitTOF(). */
void tb_step(struct Gfx *g, struct TextBox *tb)
{
    if (gfx_off) {              /* a layout draws: jump, count once */
        if (tb->pos != tb->target) {
            tb->pos = tb->target;
            gfx_off_hits++;
        }
        return;
    }
    LONG d = tb->target - tb->pos, step;
    if (!d)
        return;
    step = (tb->dragging || tb->jump) ? d : d / 4;  /* a drag follows the mouse */
    if (!step)
        step = d > 0 ? 1 : -1;
    tb->pos += step;
    if (tb->bm)
        blit_window(g, tb);
    else
        scroll_direct(g, tb);
    draw_bar(g, tb);
}

/* Mouse on the scroll bar. Returns TRUE when the click belonged to it. */
BOOL tb_mouse_down(struct TextBox *tb, WORD mx, WORD my)
{
    WORD ry;
    if (!tb->open || !tb->bar_w || mx < tb->bar_x - 2 || mx > tb->bar_x + tb->bar_w + 1 ||
        my < tb->y || my >= tb->y + tb->h)
        return FALSE;
    ry = my - tb->y;
    if (ry >= tb->thumb_y && ry < tb->thumb_y + tb->thumb_h) {
        tb->dragging = TRUE;
        tb->drag_off = ry - tb->thumb_y;
        tb->thumb_y = -1;           /* repaint the thumb highlighted */
    } else {
        LONG page = tb->h - tb->line_h;
        tb_scroll_by(tb, ry < tb->thumb_y ? -page : page);
    }
    return TRUE;
}

void tb_mouse_move(struct TextBox *tb, WORD my)
{
    LONG span = tb->h - tb->thumb_h, ty;
    if (!tb->dragging || span <= 0)
        return;
    ty = my - tb->y - tb->drag_off;
    if (ty < 0)
        ty = 0;
    if (ty > span)
        ty = span;
    tb->target = clamp_pos(tb, ty * max_pos(tb) / span);
}

void tb_mouse_up(struct TextBox *tb)
{
    if (tb->dragging) {
        tb->dragging = FALSE;
        tb->thumb_y = -1;           /* repaint the thumb normally */
    }
}

/* Repaint a thumb whose highlight changed without the position moving. */
void tb_refresh_bar(struct Gfx *g, struct TextBox *tb)
{
    if (gfx_off)
        return;
    if (tb->open)
        draw_bar(g, tb);
}
