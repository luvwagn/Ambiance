/* The layouts' drawing calls (struct LayoutHost's): what a layout draws
 * with, onto the canvas the host gives it (lkcore/lkit). Main sets lvh -
 * the screen, the canvas, its clip for a viewer's controls, the big
 * letter up and its own pictures - and lvh_calls fills a host's calls.
 * (Classic as a layout, step 2: the drawing API apart from main.c.) */
#include "amb.h"
#include "layout.h"
#include "lkit.h"
#include "lvhost.h"
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>

struct LvHost lvh;

/* Ambiance's logo for a layout: the smooth one, over the layout's colour */
static struct LImage lv_logo_im, lv_plex_im;
static ULONG *lv_logo_px, lv_logo_bg;

static const struct LImage *lv_logo(ULONG bg)
{
    UWORD w, h, nc;
    const UBYTE *pal, *idx = sm_logo_data(lvh.g->sm, &w, &h, &nc, &pal);
    ULONG i, n;
    if (!idx)
        return NULL;
    if (lv_logo_px && lv_logo_bg == bg)
        return &lv_logo_im;
    n = (ULONG)w * h;
    if (!lv_logo_px && !(lv_logo_px = AllocVec(n * 4, MEMF_ANY)))
        return NULL;
    {
        /* the logo was made over the classic bar's dark navy (0, 0, 34): its
         * glow and its edges are that navy and the ink mixed. Each colour's
         * share of ink (by its blue, the navy's 34 to the badge's 204) put
         * over the layout's colour instead - over a lighter background the
         * navy showed as a black halo */
        ULONG col[256];
        UWORD c;
        for (c = 0; c < nc && c < 256; c++) {
            LONG r = pal[3 * c], gg = pal[3 * c + 1], b = pal[3 * c + 2], a = (b - 34) * 256 / (204 - 34), k;
            LONG br = (LONG)((bg >> 16) & 255), bgg = (LONG)((bg >> 8) & 255), bb = (LONG)(bg & 255);
            if (a < 0)
                a = 0;
            if (a > 256)
                a = 256;
            k = 256 - a;                    /* (the navy's share: the layout's colour there) */
            r += (br - 0) * k / 256;
            gg += (bgg - 0) * k / 256;
            b += (bb - 34) * k / 256;
            col[c] = ((ULONG)(r < 0 ? 0 : r > 255 ? 255 : r) << 16) |
                     ((ULONG)(gg < 0 ? 0 : gg > 255 ? 255 : gg) << 8) | (ULONG)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
        for (i = 0; i < n; i++) {
            UBYTE v = idx[i];
            lv_logo_px[i] = v < nc ? col[v] : bg;
        }
    }
    lv_logo_bg = bg;
    lv_logo_im.w = w;
    lv_logo_im.h = h;
    lv_logo_im.mod = w;
    lv_logo_im.px = lv_logo_px;
    return &lv_logo_im;
}

/* The big letter up (the classic one's mask: lvh.letter), in rgb: by sprites (AGA), else a fill a run */
static WORD h_letter(WORD xr, WORD ym, ULONG rgb)
{
    const struct BigLetter *bl = lvh.letter ? lvh.letter() : NULL;
    WORD x0, y0, i, j, r;
    if (!bl || !bl->mask)
        return 0;
    x0 = xr - bl->w;
    y0 = ym - bl->h / 2;
    if (lk_letter_sprites(lvh.out, bl->mask, bl->w, bl->h, bl->bpr, bl->cell_w, x0, y0, rgb)) {
        *lvh.sp_up = TRUE;
        return 0;               /* (by sprites, over the picture: nothing drawn, nothing to
                                 * draw again under it as what is under it moves) */
    }
    for (j = 0; j < (WORD)bl->h; j++) {
        const UBYTE *row = bl->mask + (ULONG)j * bl->bpr;
        for (i = 0; i < (WORD)bl->w; i = r) {
            BOOL on = (row[i >> 3] >> (7 - (i & 7))) & 1;
            for (r = i + 1; r < (WORD)bl->w && (BOOL)((row[r >> 3] >> (7 - (r & 7))) & 1) == on; r++)
                ;
            if (on)
                lk_fill(lvh.out, x0 + i, y0 + j, r - i, 1, rgb);
        }
    }
    return (WORD)bl->w;
}

/* ---- the drawing calls, onto the canvas (lvh.out) */
static void h_clip(WORD x0, WORD y0, WORD x1, WORD y1)
{
    if (lvh.ctl->on) {
        x0 = x0 > lvh.ctl->x0 ? x0 : lvh.ctl->x0;
        y0 = y0 > lvh.ctl->y0 ? y0 : lvh.ctl->y0;
        x1 = x1 < lvh.ctl->x1 ? x1 : lvh.ctl->x1;
        y1 = y1 < lvh.ctl->y1 ? y1 : lvh.ctl->y1;
    }
    lk_clip(lvh.out, x0, y0, x1, y1);
}
static void h_fill(WORD x, WORD y, WORD w, WORD h, ULONG rgb) { lk_fill(lvh.out, x, y, w, h, rgb); }
static BOOL h_copy(WORD x, WORD y, WORD w, WORD h, WORD dx, WORD dy)
{
    return lk_copy(lvh.out, x, y, w, h, dx, dy) ? TRUE : FALSE;
}
static void h_vgrad(WORD x, WORD y, WORD w, WORD h, ULONG a, ULONG b)
{
    if (x == 0 && y == 0 && w == (WORD)lvh.g->w && h == (WORD)lvh.g->h)
        lk_ramp(lvh.out, a, b);     /* (the background: an 8-bit screen draws it smooth) */
    if (!lk_vgrad8(lvh.out, x, y, w, h, a, b))
        lk_vgrad(lvh.out, x, y, w, h, a, b);
}
static void h_hgrad(WORD x, WORD y, WORD w, WORD h, ULONG a, ULONG b) { lk_hgrad(lvh.out, x, y, w, h, a, b); }
static void h_rrect(WORD x, WORD y, WORD w, WORD h, WORD r, ULONG rgb, ULONG bg)
{
    lk_rrect(lvh.out, x, y, w, h, r, rgb, bg);
}
static void h_frame(WORD x, WORD y, WORD w, WORD h, WORD t, ULONG rgb) { lk_frame(lvh.out, x, y, w, h, t, rgb); }
static void h_image(const struct LImage *im, WORD x, WORD y)
{
    if (im && im->px) {
        /* the host's own (the logo, Plex's mark, a picture made for the
         * layout) its own pens, however wide: a layout's own wide one is a
         * reflection, drawn with the pens there are (the logo, 181 x 40,
         * had come out grey, no blue pen up yet to be nearest) */
        lk_pic_own = im == &lv_logo_im || im == &lv_plex_im || (lvh.own && lvh.own(im));
        lk_picture(lvh.out, x, y, (const unsigned long *)im->px, im->w, im->h, (long)im->mod);
        lk_pic_own = FALSE;
    }
}
static WORD h_text(WORD x, WORD y, const char *s, LONG n, int face, ULONG fg, const struct LkBg *bg,
                   WORD maxw)
{
    return lk_text(lvh.g, lvh.out, x, y, s, n, face, fg, bg, maxw);
}
static WORD h_textlen(const char *s, LONG n, int face) { return lk_textlen(lvh.g, s, n, face); }
static WORD h_fit(const char *s, LONG n, int face, WORD maxw)
{
    return (WORD)lk_fit(lvh.g, s, n, face, maxw);     /* (Far East characters too) */
}
static WORD h_font_h(int face)
{
    return !lvh.g->sm ? 0 : face == LF_TITLE ? (WORD)sm_title_h(lvh.g->sm) : face == LF_TINY ? (WORD)sm_tiny_h(lvh.g->sm)
                                                                                   : (WORD)sm_cell_h(lvh.g->sm);
}
static void h_icon(int section, WORD x, WORD y, ULONG fg, const struct LkBg *bg)
{
    lk_icon(lvh.g, lvh.out, lvh.icon_of ? lvh.icon_of(section) : section, x, y, fg, bg);
}
static const char *h_tr(const char *s) { return amb_tr(s); }

/* Plex's logo (art.c's, in its pens) for a layout: k times larger, about
 * two lines tall, in its colours over bg */
static ULONG *lv_plex_px, lv_plex_bg;
UBYTE lv_mark_kind;                     /* 1 Plex's, 2 Jellyfin's (main, with LPage.plexmark) */
static UBYTE lv_plex_kind;

/* "Jellyfin" (art.c's jf_hi, Rubik Bold: not Jellyfin's logo, which third
 * parties do not use) two lines tall, in Jellyfin's gradient left to
 * right (#AA5CC3 to #00A4DC, free to use), over bg */
static const struct LImage *h_jfmark(ULONG bg)
{
    UWORD h = (UWORD)(lvh.g->sm ? sm_cell_h(lvh.g->sm) * 2 : jf_hi_h / 2), w;
    LONG x, y;
    if (h < 8)
        h = 8;
    if (h > jf_hi_h)
        h = jf_hi_h;
    w = (UWORD)(((LONG)jf_hi_w * h + jf_hi_h / 2) / jf_hi_h);
    if (lv_plex_px && lv_plex_bg == bg && lv_plex_im.w == w && lv_plex_kind == 2)
        return &lv_plex_im;
    if (lv_plex_px) {
        FreeVec(lv_plex_px);
        lv_plex_px = NULL;
    }
    if (!(lv_plex_px = AllocVec((ULONG)w * h * 4, MEMF_ANY)))
        return NULL;
    for (y = 0; y < h; y++) {
        LONG y0 = y * jf_hi_h / h, y1 = (y + 1) * jf_hi_h / h;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (x = 0; x < w; x++) {
            LONG x0 = x * jf_hi_w / w, x1 = (x + 1) * jf_hi_w / w, i, j, ink = 0, n, sh;
            ULONG c = 0, ink_c = 0;
            if (x1 <= x0)
                x1 = x0 + 1;
            for (j = y0; j < y1; j++)
                for (i = x0; i < x1; i++)
                    ink += jf_hi[j * jf_hi_w + i];
            n = (y1 - y0) * (x1 - x0) * 255;
            for (sh = 0; sh <= 16; sh += 8) {
                /* the gradient's colour here, then over bg by coverage */
                LONG a = (LONG)((0xAA5CC3UL >> sh) & 255), b = (LONG)((0x00A4DCUL >> sh) & 255);
                LONG v = a + (b - a) * x / (w > 1 ? w - 1 : 1), bv = (LONG)((bg >> sh) & 255);
                ink_c |= (ULONG)v << sh;
                c |= (ULONG)((bv * (n - ink) + v * ink) / n) << sh;
            }
            (void)ink_c;
            lv_plex_px[(ULONG)y * w + x] = c;
        }
    }
    lv_plex_bg = bg;
    lv_plex_kind = 2;
    lv_plex_im.w = w;
    lv_plex_im.h = h;
    lv_plex_im.mod = w;
    lv_plex_im.px = lv_plex_px;
    return &lv_plex_im;
}

static const struct LImage *h_plexmark(ULONG bg)
{
    /* the wordmark's coverage (art.c's plex_hi, 64 high) averaged down to
     * two lines' height, white and Plex's yellow over bg: smooth, as the
     * smooth fonts are (the pens' pixel logo, made larger, was blocky) */
    UWORD h = (UWORD)(lvh.g->sm ? sm_cell_h(lvh.g->sm) * 2 : plex_hi_h / 2), w;
    LONG x, y;
    if (h < 8)
        h = 8;
    if (h > plex_hi_h)
        h = plex_hi_h;
    if (lv_mark_kind == 2)
        return h_jfmark(bg);
    w = (UWORD)(((LONG)plex_hi_w * h + plex_hi_h / 2) / plex_hi_h);
    if (lv_plex_px && lv_plex_bg == bg && lv_plex_im.w == w && lv_plex_kind == 1)
        return &lv_plex_im;
    if (lv_plex_px && (lv_plex_im.w != w || lv_plex_kind != 1)) {
        FreeVec(lv_plex_px);
        lv_plex_px = NULL;
    }
    if (!lv_plex_px && !(lv_plex_px = AllocVec((ULONG)w * h * 4, MEMF_ANY)))
        return NULL;
    for (y = 0; y < h; y++) {
        LONG y0 = y * plex_hi_h / h, y1 = (y + 1) * plex_hi_h / h;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (x = 0; x < w; x++) {
            LONG x0 = x * plex_hi_w / w, x1 = (x + 1) * plex_hi_w / w, i, j, ink = 0, yel = 0, n;
            ULONG c;
            if (x1 <= x0)
                x1 = x0 + 1;
            for (j = y0; j < y1; j++)
                for (i = x0; i < x1; i++) {
                    UBYTE v = plex_hi[j * plex_hi_w + i];
                    ink += v >> 4;
                    yel += v & 15;
                }
            n = (y1 - y0) * (x1 - x0) * 15;
            /* bg where no ink, white where the ink is not the chevron's,
             * Plex's yellow where it is */
            if (yel > ink)
                yel = ink;
            {
                int sh;
                c = 0;
                for (sh = 0; sh <= 16; sh += 8) {
                    LONG b = (LONG)((bg >> sh) & 255), yv = (LONG)((0xEBAF00UL >> sh) & 255);
                    c |= (ULONG)((b * (n - ink) + 255 * (ink - yel) + yv * yel) / n) << sh;
                }
            }
            lv_plex_px[(ULONG)y * w + x] = c;
        }
    }
    lv_plex_bg = bg;
    lv_plex_kind = 1;
    lv_plex_im.w = w;
    lv_plex_im.h = h;
    lv_plex_im.mod = w;
    lv_plex_im.px = lv_plex_px;
    return &lv_plex_im;
}
static void h_log(const char *s) { amb_log("layout", s); }
static ULONG h_now(void) { return time_ms(); }

void lvh_calls(struct LayoutHost *h)
{
    h->clip = h_clip;
    h->fill = h_fill;
    h->copy = h_copy;
    h->vgrad = h_vgrad;
    h->hgrad = h_hgrad;
    h->rrect = h_rrect;
    h->frame = h_frame;
    h->image = h_image;
    h->text = h_text;
    h->textlen = h_textlen;
    h->fit = h_fit;
    h->font_h = h_font_h;
    h->icon = h_icon;
    h->logo = lv_logo;
    h->letter = h_letter;
    h->tr = h_tr;
    h->log = h_log;
    h->now_ms = h_now;
    h->plexmark = h_plexmark;
}

void lvh_free(void)
{
    if (lv_logo_px) {
        FreeVec(lv_logo_px);
        lv_logo_px = NULL;
    }
    if (lv_plex_px) {
        FreeVec(lv_plex_px);
        lv_plex_px = NULL;
    }
}
