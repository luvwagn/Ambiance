/*
 * Ambiance - Showcase, the first layout (layout.h)
 * Copyright (c) 2026 Brian King
 *
 * After Kodi's look, without its fanart: a menu down the left, solid
 * panels, a light blue focus and a big clock, on a dark blue-grey
 * gradient. Nothing is blended and nothing large is decoded (see
 * claude/layout-engines.md, and the mockups' Kodi-style pages).
 *
 * Laid out on an 800 x 600 design, scaled to the screen (across and down
 * apart, so a wide screen is used across); the text is the smooth fonts'
 * one size, rows at least a line and a margin tall. The keys are the
 * program's (LR_UNUSED): Home's menu and the lists go up and down as in
 * the classic layout.
 */
#include <stddef.h>
#include <exec/types.h>
#include "../layout.h"
#include "lshare.h"

const char LibName[] = "showcase.layout";
const char LibIdString[] = "showcase.layout 1.11 (18.11.2026) Showcase, for Ambiance";
static const char __attribute__((used)) LibVerTag[] =
    "$VER: showcase.layout 1.11 (18.11.2026) Showcase, for Ambiance";
const UWORD LibVersion = 1;
const UWORD LibRevision = 11;

/* the colours (the mockups') */
#define C_TOP    0x0F1D2CUL     /* the gradient */
#define C_BOT    0x08111AUL
#define C_PANEL  0x132131UL
#define C_PANEL2 0x0E1A27UL
#define C_FOCUS  0x1E3A55UL
#define C_LINE   0x22364BUL
#define C_ACC    0x7FD3FFUL
#define C_PRESS  0xFFD24AUL     /* a button pressed by its key or a click: a yellow glow */
#define C_SUB    0xB9C7D3UL
#define C_DIM    0x5F7183UL
#define C_TEXT   0xE6EEF4UL
#define C_BODY   0xD9E3EAUL
#define C_WHITE  0xFFFFFFUL
#define C_NOWBG  0x02040AUL
#define C_COVER  0x8A4B2AUL     /* no cover: a copper square */
#define C_NEXT   0x9FB0BFUL
#define C_HOVER  0x18293CUL     /* a row under the pointer (between the panel and the focus) */
#define C_LETTER 0x2C4864UL     /* the big letter: faint, over the list */

const ULONG ls_top = C_TOP, ls_bottom = C_BOT, ls_acc = C_ACC;   /* (lshare.c's) */
const struct LayoutHost *H;
WORD W, HT, FH, TH;      /* the screen; a line of text; a title's (LF_TITLE) */
static void inner_glow(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG bg, ULONG face);
#define LOGO_X     X(18)            /* Ambiance's logo: the same place on every screen */
#define LOGO_Y     Y(12)
#define NOW_BH     Y(38)            /* Now playing: its buttons' height (at the bottom) */
#define NOW_PANEL  Y(392)           /* Now playing: its panel's top; the bars end above it */
static int drawn = -1;          /* the screen drawn last */
static ULONG bars_at;           /* Now playing's bars, drawn then */
static LONG list_top;           /* the generic page's first row shown */
static LONG list_count = -1;
static const void *list_of;

/* What a click lands on: the places drawn last, noted as they are drawn
 * (sc_pointer looks them up). */
enum { HT_SECTION, HT_TILE, HT_ROW, HT_CRUMB, HT_NOWPLAYING, HT_PAUSE, HT_HOME, HT_BUTTON, HT_SEEK, HT_SCROLL };
static LONG hit_sel = -1;       /* the list's selected row, as drawn */
static LONG click_row = -1;     /* a double-click: the row clicked last, and when */
static ULONG click_ms;

/* What the pointer is over (a row, tile, section or button: HT_*, its
 * arg), drawn with a glow; -1 none (the keys in use) */
static int hov_what = -1;
static LONG hov_arg = -1;
#define HOVER(w, a) (hov_what == (w) && hov_arg == (a))

static struct LkBg bg_grad;
static const struct LkBg panel_bg = { C_PANEL, C_PANEL, 0, 1, 0 };    /* (as flat(C_PANEL)) */
const struct LkBg *const ls_lines_bg = &panel_bg;
const ULONG ls_body = C_BODY;
const ULONG ls_glow = C_ACC;    /* the glow round the focus and the hover: the accent, 5 units wide */
const WORD ls_glow_w = 5, ls_glow_min = 3;
const BOOL ls_blank_half = FALSE;

/* a panel: rounded, its edge a line's colour, on bg (what the corners show) */
static void panel(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG fill, ULONG edge, ULONG bg)
{
    H->rrect((WORD)x, (WORD)y, (WORD)w, (WORD)h, (WORD)r, edge, bg);
    H->rrect((WORD)(x + 1), (WORD)(y + 1), (WORD)(w - 2), (WORD)(h - 2), (WORD)(r > 1 ? r - 1 : 0),
             fill, edge);
}

static LONG pb_x = -1, pb_y, pb_s;  /* the top right's, as drawn (-1: none) */
static BOOL pb_dark;

/* top right: what plays (if anything) and the clock */
static LONG tr_left;            /* top_right's left edge, as drawn: a title stops short of it */
static LONG dbg_left = 0x7fff;  /* the Debug line's under the clock (W: none): it too */
static void top_right_as(BOOL dark, BOOL with_hit);
static void top_right(BOOL dark)
{
    top_right_as(dark, TRUE);
}

/* It again alone (the time and the clock went on: LC_PLAYING), over what
 * was there; its click place as it was */
static void top_right_again(BOOL dark)
{
    LONG y = Y(16), x0 = tr_left - X(8), y0 = y + (FH - TH) / 2 - Y(2), h = TH + Y(4);
    if (H->debug && *H->debug) {
        LONG dl = W - X(24) - tnum_len(H->debug, LF_SMALL) - X(8);
        if (dl < x0)
            x0 = dl;
        h = y + TH + Y(1) + FH + Y(2) - y0;
    }
    if (x0 < 0)
        x0 = 0;
    {
        /* what is under it (Now playing's bars, a page's panels) is drawn
         * after it and over it: it ends where they begin, as drawn whole */
        LONG lim = dark ? Y(60) : Y(64);
        if (y0 + h > lim)
            h = lim - y0;
        H->clip((WORD)x0, (WORD)y0, W, (WORD)(y0 + h));
        if (dark)
            H->fill((WORD)x0, (WORD)y0, (WORD)(W - x0), (WORD)h, C_NOWBG);
        else
            H->vgrad(0, 0, W, HT, C_TOP, C_BOT);    /* (the screen's one gradient, as drawn whole) */
        top_right_as(dark, FALSE);
        H->clip(0, 0, W, HT);
    }
}

static void top_right_as(BOOL dark, BOOL with_hit)
{
    const struct LNow *n = H->model(LS_NOW);
    LONG x = W - X(24), cw = tnum_len(H->clock, LF_TITLE), y = Y(16);
    const struct LkBg *bg = dark ? flat(C_NOWBG) : &bg_grad;
    x -= cw;
    tnum(x, y + (FH - TH) / 2, H->clock, LF_TITLE, C_WHITE, bg);     /* the big clock */
    tr_left = x;
    dbg_left = W;
    if (H->debug && *H->debug) {        /* Settings > Debug: the CPU and memory, under the clock */
        dbg_left = W - X(24) - tnum_len(H->debug, LF_SMALL);
        tnum(dbg_left, y + TH + Y(1), H->debug, LF_SMALL, C_ACC, bg);
    }
    pb_x = -1;
    if (n && n->kind != LN_NONE && n->title && *n->title) {
        char b[16];
        LONG tw, room;
        const char *t;
        if (n->stopped) {                   /* (no time to show: that it is stopped) */
            t = H->tr("Stopped");
            x -= X(18) + tlen(t, LF_SMALL);
            text(x, y, t, LF_SMALL, C_SUB, bg, tlen(t, LF_SMALL));
        } else {
            t = mmss(n->pos_ms, b);
            x -= X(18) + tnum_len(t, LF_SMALL);
            tnum(x, y, t, LF_SMALL, C_SUB, bg);
        }
        tr_left = x;
        /* the title in what is left of the right half: none at all where
         * only a few letters of it would fit */
        room = x - X(10) - X(8) - FH - (W / 2 + X(20));
        tw = tlen(n->title, LF_SMALL);
        if (tw > room)
            tw = room;
        if (tw < X(70))
            tw = 0;
        if (tw > 0) {
            x -= X(10) + tw;
            text(x, y, n->title, LF_SMALL, C_SUB, bg, tw);
            if (with_hit)
                hit(x - X(8) - FH, y - Y(4), W - X(24) - cw - (x - X(8) - FH), FH + Y(8), HT_NOWPLAYING, 0, 0);
            tr_left = x - X(8) - FH;
        }
        if (!n->stopped) {
            /* the playing bars, moving with the sound (sc_tick) */
            LONG s = FH * 2 / 3, bw = 3 * (s / 4 > 1 ? s / 4 : 2) + 4;
            pb_x = x - X(8) - bw;
            pb_y = y + (FH - s) / 2;
            pb_s = s;
            pb_dark = dark;
            play_bars(x - X(8), pb_y, s, pm_lv);
            if (tw <= 0 && with_hit)
                hit(pb_x - X(6), y - Y(4), W - X(24) - cw - (pb_x - X(6)), FH + Y(8), HT_NOWPLAYING, 0, 0);
            tr_left = pb_x;
        } else if (tw <= 0 && with_hit) {
            hit(x - X(6), y - Y(4), W - X(24) - cw - (x - X(6)), FH + Y(8), HT_NOWPLAYING, 0, 0);
        }
    }
}

/* Where a title stops: short of what is at the top right (what plays, the
 * clock, the Debug line under it, which a large title reaches down to),
 * the right two fifths left to them at least */
static LONG title_lim(void)
{
    LONG r = (dbg_left < tr_left ? dbg_left : tr_left) - X(24);
    return r < W * 3 / 5 ? r : W * 3 / 5;
}

/* The top left: Ambiance's logo, then the page's title (none: the logo
 * alone, the section's name being beside the list) and the way to it */
static LONG title_cy;           /* the title line's middle, as drawn */
static LONG title_line(const char *title, const char *sub)
{
    LONG x = X(24), y = Y(16), w;
    const struct LImage *logo = H->logo ? H->logo(grad(LOGO_Y + Y(10))) : NULL;
    if (logo && logo->w < W / 3) {
        H->image(logo, (WORD)LOGO_X, (WORD)LOGO_Y);
        hit(LOGO_X - X(4), LOGO_Y - Y(4), logo->w + X(8), logo->h + Y(8), HT_HOME, 0, 0);
        x = LOGO_X + logo->w + X(16);
        y = LOGO_Y + logo->h / 2 - FH / 2;  /* (the title beside it, on its middle) */
    }
    title_cy = y + FH / 2;
    {
        /* up to what is at the top right (what plays, the clock), never
         * over it; the right half at most */
        LONG lim = title_lim();
        if (title && *title && lim - x > X(40)) {
            w = text(x, y + (FH - TH) / 2, title, LF_TITLE, C_WHITE, &bg_grad, lim - x);
            x += w + X(14);
        }
        if (sub && *sub && lim - x > X(60))
            x += text(x, y, sub, LF_SMALL, C_SUB, &bg_grad, lim - x) + X(14);
    }
    return x;
}

/* The bottom: no bar at rest. A message is a toast, a pill with a glow,
 * for TOAST_MS after it comes (while the page waits for the network, as
 * long as it waits); the keys' hint shows for HINT_MS after a key, never
 * for the pointer. Kept here (copies) so that a toast or hint ending is
 * drawn away by the tick, the band alone. */
#define TOAST_MS 4000
#define HINT_MS  4000
static char ft_msg[160], ft_hint[160];
static ULONG ft_hash, ft_at, ft_key;
static BOOL ft_keep;            /* the page waits: the toast stays */
static int ft_shown;            /* what the band shows: 0 nothing, 1 the toast, 2 the hint */
static ULONG ft_shown_hash;
static BOOL ft_screen;          /* the screen drawn has the band */

static LONG band_top(void)
{
    return HT - Y(10) - (FH + Y(14)) - X(6) - 2;
}

static void draw_wall_body(const struct LPage *m);
static void footer_paint(int want);

/* The band drawn as it should be now; force: whatever it showed */
static void footer_draw(BOOL force)
{
    ULONG now = H->now_ms();
    int want = 0;
    if (ft_msg[0] && (ft_keep || now - ft_at < TOAST_MS))
        want = 1;
    else if (ft_hint[0] && ft_key && now - ft_key < HINT_MS)
        want = 2;
    if (!force && want == ft_shown && (want != 1 || ft_shown_hash == ft_hash))
        return;
    H->clip(0, 0, W, HT);
    if (drawn == LS_ALBUMS) {
        /* the wall runs under the band: drawn again there, not cleared */
        const struct LPage *wm = H->model(LS_ALBUMS);
        if (wm)
            draw_wall_body(wm);
    } else {
        LONG y0 = band_top();
        H->vgrad(0, (WORD)y0, W, (WORD)(HT - y0), grad(y0), C_BOT);
    }
    footer_paint(want);
}

/* The band's toast or hint, over what is there */
static void footer_paint(int want)
{
    ULONG now;
    (void)now;
    if (want == 1) {
        LONG tw = tlen(ft_msg, LF_REGULAR), h = FH + Y(14), w = tw + X(36), x, y = HT - Y(10) - h;
        if (w > W - X(60)) {
            w = W - X(60);
            tw = w - X(36);
        }
        x = (W - w) / 2;
        glow(x, y, w, h, h / 2, grad(y));
        rshape(x, y, w, h, h / 2, C_FOCUS);
        text(x + X(18), y + Y(7), ft_msg, LF_REGULAR, C_WHITE, flat(C_FOCUS), tw);
    } else if (want == 2) {
        /* the keys' hint: a quieter pill, over whatever is under it */
        LONG tw = tlen(ft_hint, LF_SMALL), h = FH + Y(10), w = tw + X(28), x, y = HT - Y(10) - h;
        if (w > W - X(60)) {
            w = W - X(60);
            tw = w - X(28);
        }
        x = (W - w) / 2;
        rshape(x, y, w, h, h / 2, C_PANEL2);
        text(x + X(14), y + Y(5), ft_hint, LF_SMALL, C_SUB, flat(C_PANEL2), tw);
    }
    ft_shown = want;
    ft_shown_hash = ft_hash;
}

static void footer_keep(const char *hint, const char *message, BOOL keep)
{
    ULONG h = shash(message);
    if (message && *message) {
        if (h != ft_hash || !ft_msg[0])
            ft_at = H->now_ms();        /* a new message: its toast from now */
    }
    ft_hash = message && *message ? h : 0;
    scopy(ft_msg, message, sizeof(ft_msg));
    scopy(ft_hint, hint, sizeof(ft_hint));
    ft_keep = keep;
    ft_screen = TRUE;
    footer_draw(TRUE);
}
static void footer(const char *hint, const char *message)
{
    footer_keep(hint, message, FALSE);
}

/* ---------------------------------------------------------------- Home */
static void scroll_bar(LONG bx, LONG by, LONG bh, LONG ty, LONG th, ULONG bg);
static LONG page_trail(const struct LPage *m, BOOL named);
static LONG home_cols = 1, home_top;   /* Home's tiles: across, the first row shown */
static void draw_home(void)
{
    const struct LHome *m = H->model(LS_HOME);
    LONG mw = X(204), ih = Y(48), gap = Y(4), n, y0, i, ip = H->icon_px, top = Y(20);
    const struct LImage *logo;
    if (ih < FH + Y(16))
        ih = FH + Y(16);
    background();
    top_right(FALSE);
    if (!m)
        return;
    /* the menu: its own solid column, Ambiance's logo at its top */
    H->fill(0, 0, (WORD)mw, HT, C_PANEL2);
    H->fill((WORD)(mw - 1), 0, 1, HT, C_LINE);
    logo = H->logo ? H->logo(C_PANEL2) : NULL;
    if (logo && logo->w <= mw - X(18)) {
        H->image(logo, (WORD)LOGO_X, (WORD)LOGO_Y);
        top = LOGO_Y + logo->h + Y(10);
    }
    n = m->n;
    while (n > 1 && n * ih + (n - 1) * gap > HT - top - Y(20))
        ih--;                   /* (a short screen: rows a little closer) */
    y0 = top + (HT - top - Y(20) - (n * ih + (n - 1) * gap)) / 2;
    for (i = 0; i < n; i++) {
        BOOL sel = i == m->sel;
        LONG x = X(18), y = y0 + i * (ih + gap), w = X(172), tx = x + X(14);
        ULONG bg = sel ? (m->tile_sel >= 0 ? C_HOVER : C_FOCUS) : C_PANEL2;  /* (tiles in use: the
                                                                             * section dimmer) */
        hit(x, y, w, ih, HT_SECTION, 0, i);
        if (sel) {
            H->rrect((WORD)x, (WORD)y, (WORD)w, (WORD)ih, 4, bg, C_PANEL2);
            H->fill((WORD)x, (WORD)(y + 2), 3, (WORD)(ih - 4), C_ACC);
        } else if (HOVER(HT_SECTION, i)) {
            bg = C_HOVER;
            inner_glow(x, y, w, ih, 4, C_PANEL2, bg);
        }
        if (ip) {
            H->icon(m->sections[i], (WORD)tx, (WORD)(y + (ih - ip) / 2), sel ? C_WHITE : C_SUB, flat(bg));
            tx += ip + X(14);
        }
        text(tx, y + (ih - FH) / 2, m->names[i], sel ? LF_BOLD : LF_REGULAR, sel ? C_WHITE : C_SUB,
             flat(bg), x + w - tx - 4);
    }
    /* the chosen section's rows: tiles */
    {
        LONG x0 = X(228), y = Y(72), aw = W - x0 - X(24), r;
        for (r = 0; r < m->nrows; r++) {
            const struct LRow *row = &m->rows[r];
            LONG cw = X(170), ch = FH * 2 + Y(22), g = X(12), cols, k, first, last, peek, bottom = HT - Y(12);
            if (cw < tlen("Mmmmmmmmmm", LF_REGULAR))
                cw = tlen("Mmmmmmmmmm", LF_REGULAR);
            cols = (aw + g) / (cw + g);
            if (cols < 1)
                cols = 1;
            cw = (aw - (cols - 1) * g) / cols;
            text(x0, y, row->title, LF_REGULAR, C_SUB, &bg_grad, aw);
            y += FH + Y(10);
            home_cols = cols;
            {
                /* the rows of tiles that fit; the chosen one kept among them;
                 * more under them: as much of the next as there is room for,
                 * cut off at the foot, so it is seen that there are more */
                LONG fit = (bottom - y + g) / (ch + g);
                if (fit < 1)
                    fit = 1;
                if (m->tile_sel >= 0) {
                    if (m->tile_sel / cols < home_top)
                        home_top = m->tile_sel / cols;
                    if (m->tile_sel / cols >= home_top + fit)
                        home_top = m->tile_sel / cols - fit + 1;
                } else {
                    home_top = 0;
                }
                first = home_top * cols;
                last = first + fit * cols;
                peek = last;
                if (row->n > last && y + fit * (ch + g) + Y(16) <= bottom)
                    peek = last + cols;
            }
            H->clip(0, 0, W, (WORD)bottom);
            for (k = first; k < row->n && k < peek; k++) {
                const struct LItem *it = row->item(k);
                LONG cx = x0 + ((k - first) % cols) * (cw + g), cy = y + ((k - first) / cols) * (ch + g), tx;
                LONG hh = cy + ch > bottom ? bottom - cy : ch;     /* (the row cut off: as much as shows) */
                BOOL on = k == m->tile_sel && m->tile_shown;
                if (it->flags & LIF_HEAD) {
                    text(cx + X(4), cy + (ch - FH) / 2, it->label, LF_SMALL, C_SUB, &bg_grad, cw);
                    continue;
                }
                if (on || HOVER(HT_TILE, k))    /* chosen with the keys, or the pointer over it */
                    glow(cx, cy, cw, ch, 4, grad(cy));
                panel(cx, cy, cw, ch, 4, on ? C_FOCUS : C_PANEL, on || HOVER(HT_TILE, k) ? C_ACC : C_LINE,
                      grad(cy));
                if (!(it->flags & LIF_DISABLED))
                    hit(cx, cy, cw, hh, HT_TILE, it->kind, k);
                tx = cx + X(12);
                if (it->kind == LI_DISC) {  /* a DVD: a disc where a picture goes */
                    LONG s = ch - Y(12);
                    H->fill((WORD)(cx + Y(6)), (WORD)(cy + Y(6)), (WORD)s, (WORD)s, on ? C_FOCUS : C_PANEL);
                    disc(cx + Y(6), cy + Y(6), s, on ? C_FOCUS : C_PANEL);
                    tx = cx + Y(6) + s + X(10);
                } else if (it->art) {   /* its picture: a square at the left */
                    LONG s = ch - Y(12);
                    const struct LImage *im = H->picture(it->art, (UWORD)s, (UWORD)s);
                    if (im)
                        H->image(im, (WORD)(cx + Y(6) + (s - im->w) / 2), (WORD)(cy + Y(6) + (s - im->h) / 2));
                    else
                        H->fill((WORD)(cx + Y(6)), (WORD)(cy + Y(6)), (WORD)s, (WORD)s, C_PANEL2);
                    tx = cx + Y(6) + s + X(10);
                }
                text(tx, cy + Y(8), it->label, on ? LF_BOLD : LF_REGULAR,
                     (it->flags & LIF_DISABLED) ? C_DIM : on ? C_WHITE : C_TEXT, flat(on ? C_FOCUS : C_PANEL),
                     cx + cw - tx - X(12));
                if (it->tag || it->sub)
                    text(tx, cy + Y(8) + FH + Y(4), it->tag ? it->tag : it->sub, LF_SMALL,
                         on ? C_ACC : C_SUB, flat(on ? C_FOCUS : C_PANEL), cx + cw - tx - X(12));
            }
            H->clip(0, 0, W, HT);
            if (row->n > last - first || first > 0) {   /* more than fit: where among them */
                LONG nr = (row->n + cols - 1) / cols, fr = (last - first) / cols, bh = bottom - y;
                LONG th = bh * fr / (nr > 0 ? nr : 1), ty;
                if (th < Y(16))
                    th = Y(16);
                ty = y + (bh - th) * home_top / (nr - fr > 0 ? nr - fr : 1);
                if (ty > y + bh - th)
                    ty = y + bh - th;
                scroll_units(m->tile_sel >= 0 ? m->tile_sel : home_top * cols, fr * cols, row->n - 1);
                scroll_bar(W - X(12), y, bh, ty, th, grad(y));
            }
            y += ((last - first + cols - 1) / cols) * (ch + g) + Y(14);
        }
    }
}

/* The scroll bars drawn (a few a screen): their track and thumb, each a
 * click target wider than it looks; a press on one drags it (or, off its
 * thumb, puts the thumb's middle there), moves follow until release */
/* (each bar knows where its list is: the row chosen, or a text's first
 * line; a page of it; and its last place, LAC_SCROLLTO's whole way. Set
 * before it is drawn.) */
static void scroll_bar(LONG bx, LONG by, LONG bh, LONG ty, LONG th, ULONG bg)
{
    int k = nsbar++ & 3;
    BOOL drag = sb_drag == k;
    sbar[k].at = by;
    sbar[k].len = bh;
    sbar[k].t = ty;
    sbar[k].tl = th;
    sbar[k].cur = sb_cur;
    sbar[k].page = sb_page;
    sbar[k].last = sb_last;
    H->rrect((WORD)bx, (WORD)by, 4, (WORD)bh, 2, C_LINE, bg);
    H->rrect((WORD)(drag ? bx - 1 : bx), (WORD)ty, (WORD)(drag ? 6 : 4), (WORD)th, 2,
             drag ? C_WHITE : C_ACC, C_LINE);
    hit(bx - X(8), by, 4 + X(16), bh, HT_SCROLL, 0, k);
}

/* ---------------------------------------------------------------- the generic page */
static void draw_rows(const struct LPage *m, LONG x, LONG y, LONG w, LONG h)
{
    LONG rh = Y(40), fit, i, pad = Y(8);
    const struct LList *l = &m->list;
    if (rh < FH + Y(14))
        rh = FH + Y(14);
    fit = (h - 2 * pad) / rh;
    if (fit < 1)
        fit = 1;
    rh = (h - 2 * pad) / fit;   /* (the rows to fill the panel) */
    /* the rows shown: the selection kept in view, the rest where they were */
    if (list_of != (const void *)l->item || list_count != l->count) {
        list_of = (const void *)l->item;
        list_count = l->count;
        list_top = l->top;
    }
    if (l->sel < list_top)
        list_top = l->sel;
    if (l->sel >= list_top + fit)
        list_top = l->sel - fit + 1;
    if (list_top > l->count - fit)
        list_top = l->count - fit;
    if (list_top < 0)
        list_top = 0;
    hit_sel = l->sel;
    for (i = 0; i < fit && list_top + i < l->count; i++) {
        const struct LItem *it = l->item(list_top + i);
        BOOL sel = list_top + i == l->sel;
        LONG ry = y + pad + i * rh, rx = x + X(8), rw = w - X(16) - (l->count > fit ? X(10) : 0);
        LONG right = rx + rw - X(14), tw = 0, mw;
        ULONG bg = sel ? C_FOCUS : C_PANEL;
        if (it->flags & LIF_HEAD) {
            text(rx + X(8), ry + rh - FH - Y(4), it->label, LF_SMALL, C_SUB, flat(C_PANEL), rw);
            continue;
        }
        if (!(it->flags & LIF_DISABLED))
            hit(rx, ry, rw, rh, HT_ROW, it->kind, list_top + i);
        if (sel) {
            H->rrect((WORD)rx, (WORD)ry, (WORD)rw, (WORD)(rh - 2), 4, C_FOCUS, C_PANEL);
            H->frame((WORD)rx, (WORD)ry, (WORD)rw, (WORD)(rh - 2), 2, C_ACC);
        } else if (HOVER(HT_ROW, list_top + i) && !(it->flags & LIF_DISABLED)) {
            bg = C_HOVER;       /* the pointer over it: a glow inside its edge */
            inner_glow(rx, ry, rw, rh - 2, 4, C_PANEL, bg);
        } else if (i + 1 < fit && list_top + i + 1 < l->count) {
            H->fill((WORD)(rx + X(8)), (WORD)(ry + rh - 1), (WORD)(rw - X(16)), 1, C_LINE);
        }
        mw = mark(it->mark, right, ry + (rh - 2) / 2, sel ? C_WHITE : C_SUB, bg);
        if (mw)
            right -= mw + X(10);
        if (it->tag && *it->tag) {
            tw = tlen(it->tag, LF_REGULAR);
            if (tw > rw / 2)
                tw = rw / 2;
            text(right - tw, ry + (rh - 2 - FH) / 2, it->tag, LF_REGULAR, sel ? C_ACC : C_SUB, flat(bg),
                 tw);
            right -= tw + X(14);
        }
        text(rx + X(14), ry + (rh - 2 - FH) / 2, it->label, sel ? LF_BOLD : LF_REGULAR,
             (it->flags & LIF_DISABLED) ? C_DIM : sel ? C_WHITE : C_TEXT, flat(bg), right - rx - X(14));
    }
    if (l->count > fit) {       /* where in the list */
        LONG bx = x + w - X(14), by = y + pad, bh = h - 2 * pad;
        LONG th = bh * fit / l->count, ty = by + (bh - th) * list_top / (l->count - fit);
        if (th < Y(16))
            th = Y(16);
        if (ty + th > by + bh)
            ty = by + bh - th;
        scroll_units(l->sel, fit, l->count - 1);
        scroll_bar(bx, by, bh, ty, th, C_PANEL);
    }
}


static int hits_base;           /* the click targets before the page's body's */

/* One long text: all of the panel, scrolled by pixels (text_off) */

/* One long text: its panel's top part, scrolled by pixels (text_off),
 * with a scroll bar where it is longer than shown; under it (a page that
 * has them) a note, a picture, and Plex's "Works with" mark. full: all of
 * it; else (a glide) the text and its bar alone. */
static LONG tx_bottom;          /* the text's part's bottom, as drawn whole */
static void draw_text_body_as(const struct LPage *m, BOOL full)
{
    LONG py = Y(64), bottom = HT - Y(30) - FH - Y(12), x = X(24), w = W - x - X(24);
    LONG top = py + Y(14), lh = FH + Y(4), tb = bottom - Y(6), vis, sbx = x + w - X(12);
    if (full) {
        const struct LImage *pm = m->plexmark && H->plexmark ? H->plexmark(C_PANEL) : NULL;
        LONG under = bottom - Y(10);
        panel(x, py, w, bottom - py, 6, C_PANEL, C_LINE, grad(py));
        if (pm) {               /* "Works with:" Plex's mark, and its attribution under it */
            LONG fine = H->font_h(LF_TINY), my = under - fine - Y(4) - pm->h, mx = x + X(18);
            const char *with = H->tr("Works with:");
            text(mx, my + (pm->h - FH) / 2, with, LF_REGULAR, C_SUB, flat(C_PANEL), w / 2);
            mx += tlen(with, LF_REGULAR) + X(10);
            H->image(pm, (WORD)mx, (WORD)my);
            text(mx + pm->w + X(2), my, "*", LF_SMALL, C_SUB, flat(C_PANEL), 0);
            text(x + X(18), under - fine, "* used under license from Plex", LF_TINY, C_DIM, flat(C_PANEL),
                 w - X(36));
            under = my - Y(12);
        }
        if (m->picture || m->note) {
            /* the picture (as large as two fifths of the panel allows) and
             * the note over it, a rule above them both */
            LONG nh = m->note ? wrapped(m->note, w - X(36), LF_REGULAR) * lh : 0;
            LONG ph = (under - py) * 2 / 5 - nh, y;
            const struct LImage *im = NULL;
            if (ph < Y(24))
                ph = 0;
            if (ph && m->picture)
                im = H->picture(m->picture, (UWORD)(w - X(36)), (UWORD)ph);
            y = under - (ph ? ph : 0) - nh - Y(8);
            H->fill((WORD)(x + X(18)), (WORD)(y - Y(6)), (WORD)(w - X(36)), 1, C_LINE);
            if (m->note)            /* centred, over the picture */
                note_centred(m->note, x + X(18), y, w - X(36), C_SUB, flat(C_PANEL));
            if (im)
                H->image(im, (WORD)(x + (w - im->w) / 2), (WORD)(y + nh + Y(4) + (ph - im->h) / 2));
            under = y - Y(12);
        }
        tx_bottom = under;
    }
    tb = tx_bottom;
    vis = (tb - top) / lh;
    (void)vis;
    H->fill((WORD)(x + X(10)), (WORD)(py + Y(6)), (WORD)(w - X(20)), (WORD)(tb - py - Y(6)), C_PANEL);
    {
        LONG vh = tb - top, dy, from, tw = sbx - X(8) - x - X(18);
        text_measure(m, tw, lh);
        from = text_from(m, vh, &dy);
        H->clip((WORD)(x + 2), (WORD)(py + Y(6)), (WORD)(sbx - X(4)), (WORD)tb);
        draw_lines(m->details, m->ndetails, from, x + X(18), top + dy, tw, tb + lh, C_BODY, C_BODY);
        H->clip(0, 0, W, HT);
        if (tl_total > vh && vh > 0) {          /* where in it: the bar */
            LONG bh = vh, th = bh * vh / tl_total, ty;
            if (th < Y(16))
                th = Y(16);
            ty = top + (LONG)((bh - th) * (m->text_frac >> 4) / 4096);
            if (ty > top + bh - th)
                ty = top + bh - th;
            scroll_units((LONG)(m->text_frac * 1000 / 65536), vh * 1000 / (tl_total - vh), 1000);
            scroll_bar(sbx, top, bh, ty, th, C_PANEL);
        }
    }
}
static void draw_text_body(const struct LPage *m)
{
    draw_text_body_as(m, TRUE);
}
static int body_n = -1;         /* drawn with a details panel (1) or not (0); -1 text */

/* Whether a page has its details' column: once a list has shown details,
 * it keeps the column while the next row's are fetched (a move on Games
 * would otherwise widen the list and narrow it again each step) */

static void draw_page_body(const struct LPage *m);


/* lshare.c's wait_spinner, over spin_bg (the panel, or the title's
 * gradient row); its dots rounded boxes, the dimmest a line's colour */
static ULONG spin_bg = C_PANEL;
const ULONG ls_spin[4] = { C_ACC, C_SUB, C_DIM, C_LINE };
void spin_back(LONG x, LONG y, LONG s)
{
    H->fill((WORD)x, (WORD)y, (WORD)s, (WORD)s, spin_bg);
}
void spin_dot(LONG x, LONG y, LONG d, ULONG c)
{
    H->rrect((WORD)x, (WORD)y, (WORD)d, (WORD)d, (WORD)(d / 2), c, spin_bg);
}

static void draw_page(void)
{
    const struct LPage *m = H->model(LS_PAGE);
    char sub[160];
    LONG i, lx = X(24), lw = X(196);
    background();
    top_right(FALSE);
    if (!m)
        return;
    /* the top: the logo, the page's title, and the way here (each step a
     * click back); the page itself all the width below */
    (void)sub;
    (void)lx;
    (void)lw;
    i = page_trail(m, TRUE);
    spin_x = -1;
    if (m->waiting && m->list.count) {
        /* rows there, and more coming (a folder still being read): the
         * spinner beside the title, the rows left as they are */
        spin_x = i + FH / 2;
        spin_y = title_cy;
        spin_bg = grad(spin_y);
        wait_spinner(m);
    }
    hits_base = nhits;
    if (m->text) {
        body_n = -1;
        draw_text_body(m);
        footer(m->hint, m->message);
        return;
    }
    draw_page_body(m);
    footer_keep(m->hint, m->message, m->waiting != NULL);
}

/* The list's panel and the details' under it: all a move needs drawing
 * again (changed(LC_SELECT)), as long as the details' panel is still
 * there or still not */
static void draw_page_body(const struct LPage *m)
{
    LONG n, py = Y(64), bottom = HT - Y(30) - FH - Y(12), x = X(24), w = W - x - X(24);
    n = page_split(m);
    body_n = (int)n;
    nhits = hits_base;          /* the rows' click targets made again */
    {
        /* with details (a game's screenshot and facts): the list at the
         * left, they at the right, side by side; else the list alone */
        LONG lw = n ? w * 46 / 100 : w, lh = bottom - py;
        LONG dx = x + lw + X(14), dw = x + w - dx;
        panel(x, py, lw, lh, 6, C_PANEL, C_LINE, grad(py));
        if (!(m->waiting && m->list.count))
            spin_x = -1;            /* (rows there and still coming: the title's) */
        if (m->list.count) {
            draw_rows(m, x, py, lw, lh);
        } else if (m->waiting) {
            spin_bg = C_PANEL;
            /* still asking: the spinner and its word, not "Nothing here" */
            spin_x = x + X(22) + FH / 2 + FH / 4;
            spin_y = py + Y(18) + FH / 2;
            wait_spinner(m);
            text(spin_x + FH, py + Y(18), m->waiting, LF_REGULAR, C_SUB, flat(C_PANEL),
                 lw - (spin_x + FH - x) - X(12));
        } else {
            text(x + X(22), py + Y(18), H->tr("Nothing here"), LF_REGULAR, C_SUB, flat(C_PANEL), lw);
        }
        if (n && dw > X(120)) {
            LONG tx = dx + X(18), tw = dw - X(36), ty = py + Y(16);
            panel(dx, py, dw, lh, 6, C_PANEL, C_LINE, grad(py));
            if (m->picture) {
                /* the picture as wide as the panel (4:3), at most half its height */
                LONG pw = tw, ph = pw * 3 / 4;
                const struct LImage *im;
                if (ph > lh / 2) {
                    ph = lh / 2;
                    pw = ph * 4 / 3;
                }
                im = H->picture(m->picture, (UWORD)pw, (UWORD)ph);
                if (im)
                    H->image(im, (WORD)(tx + (tw - im->w) / 2), (WORD)(ty + (ph - im->h) / 2));
                else
                    H->rrect((WORD)(tx + (tw - pw) / 2), (WORD)ty, (WORD)pw, (WORD)ph, 4, C_PANEL2, C_PANEL);
                ty += ph + Y(16);
            }
            draw_lines(m->details, m->ndetails, 0, tx, ty, tw, py + lh - Y(10), C_WHITE, C_BODY);
        }
        if (m->letter && H->letter)     /* the big letter, as the list glides or jumps */
            H->letter((WORD)(x + lw - X(40)), (WORD)(py + lh / 2), C_LETTER);
    }
}

/* ---------------------------------------------------------------- shared by the music screens */
/* The page's title and its path, the left panel's way back left out */
/* The top of a page: the logo, its name large (named: the page says it
 * large itself, an album or an artist, so not again), and the way to it
 * small after, each step a click back to there */
static LONG page_trail(const struct LPage *m, BOOL named)
{
    LONG x = title_line(named && m->ncrumb ? m->crumb[m->ncrumb - 1] : NULL, NULL), i, y, w;
    LONG lim = title_lim();
    y = title_cy - FH / 2;
    for (i = 0; i + 1 < m->ncrumb && lim - x > X(30); i++) {
        LONG at = x;
        if (i)
            at += tlen("/", LF_SMALL) + X(6);
        /* a step whole, or cut where enough of it shows to read: else none */
        if (tlen(m->crumb[i], LF_SMALL) > lim - at && lim - at < X(60))
            break;
        if (i)
            text(x, y, "/", LF_SMALL, C_DIM, &bg_grad, 0);
        x = at;
        w = text(x, y, m->crumb[i], LF_SMALL, C_SUB, &bg_grad, lim - x);
        hit(x - X(2), y - Y(4), w + X(4), FH + Y(8), HT_CRUMB, 0, i);
        x += w + X(6);
    }
    return x;
}
static void page_path(const struct LPage *m)
{
    page_trail(m, FALSE);
}
static void page_title(const struct LPage *m)
{
    page_trail(m, TRUE);
}


/* A cover w x h at x, y: the picture (fitted, the rest bg), or a panel
 * with the name's first letter until it is ready (or if it has none) */
static void cover_wh(const char *art, const char *label, LONG x, LONG y, LONG w, LONG h, ULONG bg)
{
    const struct LImage *im = art ? H->picture(art, (UWORD)w, (UWORD)h) : NULL;
    if (im) {
        if (im->w < w || im->h < h)
            H->fill((WORD)x, (WORD)y, (WORD)w, (WORD)h, bg);
        H->image(im, (WORD)(x + (w - im->w) / 2), (WORD)(y + (h - im->h) / 2));
        return;
    }
    H->rrect((WORD)x, (WORD)y, (WORD)w, (WORD)h, 4, C_PANEL, bg);
    if (label && *label)                /* its first letter, until its picture comes */
        H->text((WORD)(x + (w - H->textlen(label, 1, LF_BOLD)) / 2), (WORD)(y + (h - FH) / 2), label, 1,
                LF_BOLD, C_DIM, flat(C_PANEL), 0);
}
static void cover(const char *art, const char *label, LONG x, LONG y, LONG s, ULONG bg)
{
    cover_wh(art, label, x, y, s, s, bg);
}

/* ---------------------------------------------------------------- the album wall */
static LONG wall_cols = 1, wall_top;    /* its columns, as drawn; its first row shown */
static LONG wall_lead = -1;     /* a letter's first, gone to with the keys: it leads the wall
                                 * (its row at the top, the letter filling the view) */

/* The wall scrolls by pixels: wall_px now, wall_tgt where it is going
 * (the chosen cover's row in view, a little of the next showing); the
 * tick eases it there. Its rows run on past the bottom of the screen. */
static LONG wall_px, wall_tgt;
static const void *wall_of;
static LONG wall_count = -1;
static void draw_wall_body(const struct LPage *m)
{
    LONG x0 = X(30), y0 = Y(70), aw = W - x0 - X(30), bottom = HT;
    LONG gap = X(18), cs = X(124), cols, ch, rows, i, sel = m->list.sel, th, viewh, nr, maxpx, r0;
    const struct LList *l = &m->list;
    if (cs < FH * 4)
        cs = FH * 4;
    cols = (aw + gap) / (cs + gap);
    if (cols < 1)
        cols = 1;
    cs = (aw - (cols - 1) * gap) / cols;
    th = m->frames ? cs * 3 / 4 : cs;  /* pictures and videos: 4:3 frames */
    ch = th + Y(8) + FH + Y(2) + FH + Y(14);
    viewh = bottom - y0;
    rows = viewh / ch;
    if (rows < 1)
        rows = 1;
    wall_cols = cols;
    nr = (l->count + cols - 1) / cols;
    maxpx = nr * ch - viewh + Y(10);
    if (maxpx < 0)
        maxpx = 0;
    if (sel >= 0) {
        LONG top = (sel / cols) * ch, peek = ch / 4;
        if (sel == wall_lead) {
            wall_tgt = top - (sel >= cols ? peek : 0);      /* the letter fills the view */
            wall_lead = -1;
        }
        if (top - (sel >= cols ? peek : 0) < wall_tgt)
            wall_tgt = top - (sel >= cols ? peek : 0);
        if (top + ch + peek > wall_tgt + viewh)
            wall_tgt = top + ch + peek - viewh;
    }
    if (wall_tgt > maxpx)
        wall_tgt = maxpx;
    if (wall_tgt < 0)
        wall_tgt = 0;
    if (wall_of != (const void *)l->item || wall_count < 0 || l->count < wall_count) {
        wall_px = wall_tgt;             /* another list: there at once */
        wall_of = (const void *)l->item;
    }
    wall_count = l->count;
    wall_top = wall_px / ch;
    r0 = wall_top;
    nhits = hits_base;
    hit_sel = sel;
    H->clip((WORD)(x0 - X(10)), (WORD)(y0 - Y(6)), (WORD)(W - X(10)), (WORD)bottom);
    H->vgrad((WORD)(x0 - X(10)), (WORD)(y0 - Y(6)), (WORD)(W - x0), (WORD)(bottom - y0 + Y(6)),
             grad(y0 - Y(6)), grad(bottom));
    spin_x = -1;
    if (!l->count) {
        /* nothing yet: its albums being asked for (the spinner), or none */
        if (m->waiting) {
            spin_x = x0 + FH / 2 + FH / 4;
            spin_y = y0 + FH / 2;
            spin_bg = grad(spin_y);
            wait_spinner(m);
            text(spin_x + FH, y0, m->waiting, LF_REGULAR, C_SUB, &bg_grad, aw - FH * 2);
        } else {
            text(x0, y0, H->tr("Nothing here"), LF_REGULAR, C_SUB, &bg_grad, aw);
        }
        H->clip(0, 0, W, HT);
        return;
    }
    for (i = r0 * cols; i < l->count && (i / cols) * ch - wall_px < viewh; i++) {
        const struct LItem *it = l->item(i);
        LONG cx = x0 + (i % cols) * (cs + gap), cy = y0 + (i / cols) * ch - wall_px;
        BOOL on = i == sel;
        ULONG bg = grad(cy);
        if (it->flags & LIF_HEAD)
            continue;
        if (cy + ch - Y(10) > y0)           /* (the part below the title clicks) */
            hit(cx, cy > y0 - Y(6) ? cy : y0 - Y(6), cs, cy + ch - Y(10) - (cy > y0 - Y(6) ? cy : y0 - Y(6)),
                HT_ROW, it->kind, i);
        if (it->kind == LI_DISC) {
            H->fill((WORD)cx, (WORD)cy, (WORD)cs, (WORD)th, bg);
            H->fill((WORD)(cx + (cs - th) / 2), (WORD)cy, (WORD)th, (WORD)th, bg);
            disc(cx + (cs - th) / 2, cy, th, bg);
        } else {
            cover_wh(it->art, it->label, cx, cy, cs, th, bg);
        }
        if (on)
            H->frame((WORD)(cx - 3), (WORD)(cy - 3), (WORD)(cs + 6), (WORD)(th + 6), 3, C_ACC);
        else if (HOVER(HT_ROW, i)) {        /* the pointer over it: a glow round it */
            LONG k, n = X(5) > 3 ? X(5) : 3;
            for (k = n; k >= 1; k--)
                H->frame((WORD)(cx - k), (WORD)(cy - k), (WORD)(cs + 2 * k), (WORD)(th + 2 * k), 1,
                         mix(C_ACC, bg, k, n + 1));
        }
        text(cx, cy + th + Y(8), it->label, on ? LF_BOLD : LF_REGULAR, on ? C_WHITE : C_TEXT,
             flat(grad(cy + th + Y(8))), cs);
        if (it->sub || it->tag)
            text(cx, cy + th + Y(8) + FH + Y(2), it->sub ? it->sub : it->tag, LF_SMALL, C_SUB,
                 flat(grad(cy + th + Y(8) + FH)), cs);
    }
    if (maxpx > 0) {                    /* where in the wall */
        LONG bx = W - X(16), bh = HT - Y(16) - y0, tot = nr * ch + Y(10);
        LONG bt = bh * viewh / (tot > 0 ? tot : 1), ty;
        if (bt < Y(16))
            bt = Y(16);
        if (bt > bh)
            bt = bh;
        ty = y0 + (bh - bt) * wall_px / maxpx;
        scroll_units(sel, cols * (viewh / ch > 1 ? viewh / ch : 1), l->count - 1);
        scroll_bar(bx, y0, bh, ty, bt, grad(y0));
    }
    if (m->letter && H->letter)
        H->letter((WORD)(W - X(60)), (WORD)(y0 + (HT - Y(40) - y0) / 2), C_LETTER);
    H->clip(0, 0, W, HT);
}

static void draw_wall(void)
{
    const struct LPage *m = H->model(LS_ALBUMS);
    background();
    top_right(FALSE);
    if (!m)
        return;
    page_title(m);
    hits_base = nhits;
    draw_wall_body(m);
    footer(m->hint, m->message);
}

/* ---------------------------------------------------------------- an album */
static LONG album_x, album_w;           /* its tracks' panel, as drawn */

static void draw_album(void)
{
    /* as a list with details (a game's): its tracks at the left, and at
     * the right a panel of the album, its cover with what is said of it
     * beside it, and its review under them */
    const struct LAlbum *a = H->model(LS_ALBUM);
    LONG py = Y(64), bottom = HT - Y(30) - FH - Y(12), x = X(24), w = W - x - X(24);
    LONG lw = w * 46 / 100, lh = bottom - py, dx, dw, cs, tx, tw, y, i;
    background();
    top_right(FALSE);
    if (!a)
        return;
    page_path(&a->page);        /* (its name is in its panel, large) */
    album_x = x;
    album_w = lw;
    hits_base = nhits;
    panel(x, py, lw, lh, 6, C_PANEL, C_LINE, grad(py));
    if (a->page.list.count)
        draw_rows(&a->page, x, py, lw, lh);
    if (a->page.letter && H->letter)
        H->letter((WORD)(x + lw - X(40)), (WORD)(py + lh / 2), C_LETTER);
    dx = x + lw + X(14);
    dw = x + w - dx;
    panel(dx, py, dw, lh, 6, C_PANEL, C_LINE, grad(py));
    /* its name across the top of the panel (long ones need the room),
     * then the cover with the rest beside it */
    y = py + Y(14);
    if (text(dx + X(18), y, a->title, LF_TITLE, C_WHITE, flat(C_PANEL), dw - X(36)))
        y += TH + Y(10);
    cs = (dw - X(36)) * 45 / 100;
    if (cs > lh * 42 / 100)
        cs = lh * 42 / 100;
    cover(a->art, a->title, dx + X(18), y, cs, C_PANEL);
    tx = dx + X(18) + cs + X(16);
    tw = dx + dw - X(18) - tx;
    if (a->artist && *a->artist) {
        text(tx, y, a->artist, LF_REGULAR, C_ACC, flat(C_PANEL), tw);
        y += FH + Y(4);
    }
    if (a->line && *a->line) {
        text(tx, y, a->line, LF_SMALL, C_SUB, flat(C_PANEL), tw);
        y += FH + Y(2);
    }
    if (a->rating && *a->rating) {
        char b[24];
        join(b, sizeof(b), H->tr("Rating"), ": ", a->rating);
        text(tx, y, b, LF_SMALL, C_SUB, flat(C_PANEL), tw);
        y += FH + Y(2);
    }
    for (i = 0; i < a->facts.n && y + FH <= py + Y(14) + TH + Y(10) + cs; i++) {
        char b[140];
        join(b, sizeof(b), a->facts.facts[2 * i], ": ", a->facts.facts[2 * i + 1]);
        text(tx, y, b, LF_SMALL, C_SUB, flat(C_PANEL), tw);
        y += FH + Y(2);
    }
    if (a->review && *a->review) {
        LONG ry = py + Y(14) + TH + Y(10) + cs + Y(16), np = paras(a->review);
        if (y + Y(8) > ry)
            ry = y + Y(8);
        draw_lines(para, np, 0, dx + X(18), ry, dw - X(36), py + lh - Y(10), C_BODY, C_BODY);
    }
    footer(a->page.hint, a->page.message);
}

/* a move on an album: its tracks' panel alone */
static void album_rows(void)
{
    const struct LAlbum *a = H->model(LS_ALBUM);
    LONG py = Y(64), bottom = HT - Y(30) - FH - Y(12), lh = bottom - py;
    if (!a)
        return;
    nhits = hits_base;
    panel(album_x, py, album_w, lh, 6, C_PANEL, C_LINE, grad(py));
    if (a->page.list.count)
        draw_rows(&a->page, album_x, py, album_w, lh);
    if (a->page.letter && H->letter)
        H->letter((WORD)(album_x + album_w - X(40)), (WORD)(py + lh / 2), C_LETTER);
}

/* ---------------------------------------------------------------- an artist */
static LONG strip_first, strip_cols = 1;   /* the albums' strip: its first shown, how many fit */
static LONG strip_y;

static void draw_strip(const struct LArtist *a)
{
    LONG x0 = X(30), aw = W - x0 - X(30), bottom = HT - Y(30) - FH - Y(12), gap = X(18);
    LONG cs = X(124), cols, i, sel = a->page.list.sel, y = strip_y;
    const struct LList *l = &a->page.list;
    if (cs > bottom - y - FH * 2 - Y(20))
        cs = bottom - y - FH * 2 - Y(20);
    cols = (aw + gap) / (cs + gap);
    if (cols < 1)
        cols = 1;
    strip_cols = cols;
    if (sel < strip_first)
        strip_first = sel;
    if (sel >= strip_first + cols)
        strip_first = sel - cols + 1;
    if (strip_first > l->count - cols)
        strip_first = l->count - cols;
    if (strip_first < 0)
        strip_first = 0;
    nhits = hits_base;
    hit_sel = sel;
    H->vgrad((WORD)(x0 - X(10)), (WORD)(y - Y(6)), (WORD)(aw + X(20)), (WORD)(bottom - y + Y(6)),
             grad(y - Y(6)), grad(bottom));
    for (i = strip_first; i < l->count && i < strip_first + cols; i++) {
        const struct LItem *it = l->item(i);
        LONG cx = x0 + (i - strip_first) * (cs + gap);
        BOOL on = i == sel;
        if (it->flags & LIF_HEAD)
            continue;
        hit(cx, y, cs, cs + FH * 2 + Y(10), HT_ROW, it->kind, i);
        cover(it->art, it->label, cx, y, cs, grad(y));
        if (on)
            H->frame((WORD)(cx - 3), (WORD)(y - 3), (WORD)(cs + 6), (WORD)(cs + 6), 3, C_ACC);
        text(cx, y + cs + Y(8), it->label, on ? LF_BOLD : LF_REGULAR, on ? C_WHITE : C_TEXT,
             flat(grad(y + cs + Y(8))), cs);
        if (it->sub || it->tag)
            text(cx, y + cs + Y(8) + FH + Y(2), it->tag ? it->tag : it->sub, LF_SMALL, C_SUB,
                 flat(grad(y + cs + Y(8) + FH)), cs);
    }
    if (strip_first > 0)
        mark(LM_ARROW, x0 - X(12), y + cs / 2, C_SUB, grad(y));    /* (more to the left: a chevron) */
}

static void draw_artist(void)
{
    const struct LArtist *a = H->model(LS_ARTIST);
    LONG py = Y(64), cx = X(30), ps = X(180), tx, tw, y, i, np;
    background();
    top_right(FALSE);
    if (!a)
        return;
    page_path(&a->page);       /* (the name is beside the picture, large) */
    cover(a->art, a->name, cx, py, ps, grad(py));
    tx = cx + ps + X(24);
    tw = W - tx - X(30);
    y = py;
    text(tx, y, a->name, LF_TITLE, C_WHITE, &bg_grad, tw);
    y += TH + Y(4);
    if (a->line && *a->line) {
        text(tx, y, a->line, LF_SMALL, C_ACC, &bg_grad, tw);
        y += FH + Y(4);
    }
    for (i = 0; i < a->facts.n; i++) {
        char b[140];
        join(b, sizeof(b), a->facts.facts[2 * i], ": ", a->facts.facts[2 * i + 1]);
        text(tx, y, b, LF_SMALL, C_SUB, &bg_grad, tw);
        y += FH + Y(2);
    }
    y += Y(8);
    np = paras(a->bio);
    if (np) {
        LONG bh = py + ps + Y(30) - y;
        if (bh > FH * 2) {
            panel(tx - X(12), y, tw + X(12), bh, 6, C_PANEL, C_LINE, grad(y));
            draw_lines(para, np, 0, tx, y + Y(10), tw - X(12), y + bh - Y(8), C_BODY, C_BODY);
        }
    }
    strip_y = py + ps + Y(50);
    hits_base = nhits;
    draw_strip(a);
    footer(a->page.hint, a->page.message);
}

/* ---------------------------------------------------------------- Now playing */
static const ULONG bar_col[12] = {
    0x1E5BFF, 0x2F7BFF, 0x3FA0FF, 0x4FC3F7, 0x56E0C8, 0x7FE38A,
    0xC6E35A, 0xFFD166, 0xFFAA33, 0xFF7A45, 0xFF4D6D, 0xE040A0
};

/* The bars: the spectrum's bands (many levels), or four meters spread
 * into a shape. Only what changed is drawn: a bar's growth in its colour,
 * its fall in the background (falling a little a frame, as the classic's);
 * full: the area cleared and every bar drawn from nothing. */
#define NBARS 64
static WORD bar_h[NBARS];
static void draw_bars(const struct LNow *n, BOOL full)
{
    LONG x0 = X(20), y0 = Y(60), aw = W - 2 * x0, ah = NOW_PANEL - Y(12) - y0, nb, gap, bw, i, fall;
    LONG bottom = y0 + ah;
    static const UBYTE shape[12] = { 9, 12, 15, 13, 11, 14, 10, 8, 12, 9, 7, 10 };
    BOOL spectrum = n && n->nlevels >= 8;
    nb = spectrum ? n->nlevels : 48;
    if (nb > NBARS)
        nb = NBARS;
    gap = nb > 40 ? 3 : 4;
    if (aw < nb * 4)
        nb = aw / 4;
    bw = (aw - (nb - 1) * gap) / nb;
    fall = ah / 24 > 1 ? ah / 24 : 1;
    if (full) {
        H->fill((WORD)x0, (WORD)y0, (WORD)aw, (WORD)ah, C_NOWBG);
        for (i = 0; i < NBARS; i++)
            bar_h[i] = 0;
    }
    for (i = 0; i < nb; i++) {
        LONG want = 0, old = bar_h[i], x = x0 + i * (bw + gap);
        if (n && n->nlevels && n->levels && !n->paused) {
            LONG lv = n->levels[i * n->nlevels / nb];
            want = spectrum ? ah * lv / 255 : ah * lv * shape[(i * 7) % 12] / (255 * 15);
            if (want < 2)
                want = 2;
        }
        if (want > ah)
            want = ah;
        if (want < old - fall)
            want = old - fall;      /* it falls away, a little a frame */
        if (want > old)
            H->fill((WORD)x, (WORD)(bottom - want), (WORD)bw, (WORD)(want - old), bar_col[i * 12 / nb]);
        else if (want < old)
            H->fill((WORD)x, (WORD)(bottom - old), (WORD)bw, (WORD)(old - want), C_NOWBG);
        bar_h[i] = (WORD)want;
    }
}

/* Now playing's top left: the logo (Home, clicked), and what this is */
static void title_now(void)
{
    LONG x = X(24), y = Y(16);
    const struct LImage *logo = H->logo ? H->logo(C_NOWBG) : NULL;
    if (logo && logo->w < W / 3) {
        H->image(logo, (WORD)LOGO_X, (WORD)LOGO_Y);
        hit(LOGO_X - X(4), LOGO_Y - Y(4), logo->w + X(8), logo->h + Y(8), HT_HOME, 0, 0);
        x = LOGO_X + logo->w + X(16);
        y = LOGO_Y + logo->h / 2 - FH / 2;
    }
    text(x, y, H->tr("Now playing"), LF_SMALL, C_NEXT, flat(C_NOWBG), W / 2 - x);
}

/* A rounded box filled a row at a time, nothing outside it touched (the
 * host's rrect paints its corners in a background colour: over a glow,
 * that showed as a square) */

/* A glow inside a rounded box's edge: the accent at its edge fading into
 * face inwards; the rest face (a row under the pointer) */
static void inner_glow(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG bg, ULONG face)
{
    LONG k, n = X(4) > 2 ? X(4) : 2;
    H->rrect((WORD)x, (WORD)y, (WORD)w, (WORD)h, (WORD)r, mix(C_ACC, face, 1, 2), bg);
    for (k = 1; k <= n; k++)
        rshape(x + k, y + k, w - 2 * k, h - 2 * k, r > k ? r - k : 1, mix(C_ACC, face, k + 1, n + 2));
    rshape(x + n + 1, y + n + 1, w - 2 * n - 2, h - 2 * n - 2, r > n ? r - n : 1, face);
}


/* A button w x h at x, y on the panel: Play's (big) the accent, the rest
 * darker; foc: in its glow */
static void sbutton_p(LONG x, LONG y, LONG w, LONG h, BOOL big, BOOL foc, BOOL pressed)
{
    LONG r = big ? h / 2 : h / 4;
    ULONG face = big ? C_ACC : C_PANEL2;
    if (pressed) {                      /* a yellow glow, its face darker: pushed in */
        LONG k, n = X(5) > 3 ? X(5) : 3;
        for (k = n; k >= 1; k--)
            rshape(x - k, y - k, w + 2 * k, h + 2 * k, r + k, mix(C_PRESS, C_PANEL, k, n + 1));
        rshape(x, y, w, h, r, mix(face, C_NOWBG, 1, 3));
        return;
    }
    if (foc) {
        glow(x, y, w, h, r, C_PANEL);
        rshape(x, y, w, h, r, big ? face : C_FOCUS);
    } else {
        H->rrect((WORD)x, (WORD)y, (WORD)w, (WORD)h, (WORD)r, face, C_PANEL);
    }
}
static void sbutton(LONG x, LONG y, LONG w, LONG h, BOOL big, BOOL foc)
{
    sbutton_p(x, y, w, h, big, foc, FALSE);
}

/* The buttons' row: the transport in the middle, the volume's at the
 * right, the state at the left; each a click target */
static void now_buttons(const struct LNow *n, LONG by, LONG bh)
{
    static const signed char order[LNB_COUNT] = { LNB_PREV, LNB_BACK, LNB_PLAY, LNB_STOP, LNB_FWD,
                                                  LNB_NEXT, LNB_LOOP, LNB_MUTE, LNB_VDOWN, LNB_VUP };
    LONG gap = X(10), bw = bh, i, x, vw = X(64);
    LONG tw = 6 * bw + 5 * gap + bh / 2, rx = W - X(28) - (4 * bw + 3 * gap + vw + gap);
    LONG x0 = (W - tw) / 2;
    if (x0 + tw + gap > rx)
        x0 = rx - gap - tw;     /* (narrow: the transport moved left) */
    for (i = 0; i < LNB_COUNT; i++) {
        int b = order[i];
        BOOL off = (n->off & LNB_BIT(b)) != 0, on = (n->on & LNB_BIT(b)) != 0;
        BOOL big = b == LNB_PLAY, foc = n->btn == b || HOVER(HT_BUTTON, b);
        LONG w = big ? bw + bh / 2 : bw;
        ULONG fg = big ? C_NOWBG : off ? C_DIM : on ? C_ACC : C_TEXT;
        if (i < 6) {
            x = x0;
            x0 += w + gap;
        } else {
            x = rx;
            rx += w + gap;
            if (b == LNB_VDOWN) {   /* the volume between - and + */
                LONG vy = by + bh / 2 - 2, f = vw * n->volume / 64;
                H->rrect((WORD)(rx), (WORD)vy, (WORD)vw, 4, 2, C_LINE, C_PANEL);
                if (f > 0)
                    H->rrect((WORD)(rx), (WORD)vy, (WORD)f, 4, 2, (n->on & LNB_BIT(LNB_MUTE)) ? C_DIM : C_ACC,
                             C_LINE);
                rx += vw + gap;
            }
        }
        sbutton(x, by, w, bh, big, foc);
        nb_glyph(b, big && !n->paused && !n->stopped, x + w / 2, by + bh / 2, bh * 9 / 20, fg);
        if (!off)
            hit(x, by, w, bh, HT_BUTTON, 0, b);
    }
    /* what it is doing, when not playing */
    if (n->stopped || n->paused) {
        const char *st = n->stopped ? H->tr("Stopped") : H->tr("Paused");
        LONG lx = X(28), room = (W - tw) / 2 - X(40);
        text(lx, by + (bh - FH) / 2, st, LF_BOLD, C_ACC, flat(C_PANEL), room);
    }
}

/* Now playing's time and its bar (drawn again alone as it goes: LC_PLAYING) */
static LONG nt_x, nt_y = -1, nt_w;
/* The time row's bar (lshare.c's time_row): a rounded track, what has
 * played lit in it (f -1: no length, the track alone) */
void time_bar(LONG bx, LONG ty, LONG bw, LONG f, BOOL knob_at_start)
{
    LONG by = ty + FH / 2 - 2;
    (void)knob_at_start;
    H->rrect((WORD)bx, (WORD)by, (WORD)bw, 4, 2, C_LINE, C_PANEL);
    if (f > 0)
        H->rrect((WORD)bx, (WORD)by, (WORD)f, 4, 2, C_ACC, C_LINE);
}
static void now_time(const struct LNow *n, BOOL with_hit)
{
    LONG tx = nt_x, y = nt_y, tw = nt_w;
    if (y < 0)
        return;
    if (!with_hit)
        H->fill((WORD)tx, (WORD)(y - Y(3)), (WORD)tw, (WORD)(FH + Y(6)), C_PANEL);
    if (!n->stopped) {
        LONG bx, bw;
        time_row(tx, y, tw, n->pos_ms, n->len_ms, FALSE, &bx, &bw);
        if (bw && with_hit)                 /* clicked: there (a band taller than the bar, to hit) */
            hit(bx, y - Y(2), bw, FH + Y(4), HT_SEEK, 0, 0);
    } else {
        text(tx, y, H->tr("Stopped: Play starts it again"), LF_SMALL, C_SUB, flat(C_PANEL), tw);
    }
}

/* ---------------------------------------------------------------- a slideshow's and a video's controls
 * Along the foot of the screen, as Now playing's panel: what is shown, and
 * a video's time and bar, in a line; under it the buttons, the transport
 * in the middle, the volume and full screen at the right. */
static LONG ct_tx, ct_ty = -1, ct_tw;   /* the time row, as drawn */
static WORD sc_controls_h(BOOL timed)
{
    (void)timed;
    return (WORD)(Y(12) + FH + Y(14) + NOW_BH + Y(12));
}

/* A video's time: where it is, a bar, how long (the bar: its seek place) */
static void ct_time(struct LControls *c)
{
    LONG bx, bw;
    time_row(ct_tx, ct_ty, ct_tw, c->at_ms, c->total_ms, TRUE, &bx, &bw);
    c->seek_w = 0;
    if (bw) {
        c->seek_x = (WORD)bx;
        c->seek_y = (WORD)(ct_ty - Y(2));
        c->seek_w = (WORD)bw;
        c->seek_h = (WORD)(FH + Y(4));
    }
}

static void sc_controls(struct LControls *c, ULONG what)
{
    LONG x0 = c->x, y0 = c->y, w = c->w, ty = y0 + Y(12), bh = NOW_BH, by = ty + FH + Y(14);
    LONG gap = X(10), vw = X(64), cw = 0, rw = 0, i, x, rx, lx = x0 + X(28);
    if (what == LC_PLAYING) {
        if (c->timed && ct_ty >= 0) {   /* the time alone, over what was there */
            H->fill((WORD)ct_tx, (WORD)(ct_ty - Y(3)), (WORD)ct_tw, (WORD)(FH + Y(5)), C_PANEL);
            ct_time(c);
        }
        return;
    }
    H->fill((WORD)x0, (WORD)y0, (WORD)w, (WORD)c->h, C_PANEL);
    H->fill((WORD)x0, (WORD)y0, (WORD)w, 1, C_LINE);
    {
        LONG tw = tlen(c->title ? c->title : "", LF_BOLD), most = c->timed ? w * 35 / 100 : w / 2;
        if (tw > most)
            tw = most;
        text(lx, ty, c->title, LF_BOLD, C_WHITE, flat(C_PANEL), tw);
        ct_ty = -1;
        if (c->timed) {
            ct_tx = lx + tw + X(24);
            ct_ty = ty;
            ct_tw = x0 + w - X(28) - ct_tx;
            ct_time(c);
        } else if (c->detail && *c->detail) {
            LONG dw = tlen(c->detail, LF_SMALL), room = x0 + w - X(28) - (lx + tw + X(24));
            if (dw > room)
                dw = room;
            if (dw > 0)
                text(x0 + w - X(28) - dw, ty, c->detail, LF_SMALL, C_SUB, flat(C_PANEL), dw);
        }
    }
    for (i = 0; i < c->n; i++) {
        LONG s = c->btn[i] == LNB_PLAY ? bh + bh / 2 : bh;
        if (ct_right(c->btn[i]))
            rw += s + gap + (c->btn[i] == LNB_VDOWN && c->has_volume ? vw + gap : 0);
        else
            cw += s + gap;
    }
    cw -= gap;
    rx = x0 + w - X(28) - (rw - gap);
    x = x0 + (w - cw) / 2;
    if (x + cw + gap > rx)
        x = rx - gap - cw;
    if (x < lx)
        x = lx;
    for (i = 0; i < c->n; i++) {
        int b = c->btn[i];
        BOOL off = (c->off & LNB_BIT(b)) != 0, on = (c->on & LNB_BIT(b)) != 0, big = b == LNB_PLAY;
        BOOL foc = !off && (c->focus == i || c->hover == i);
        LONG s = big ? bh + bh / 2 : bh, bx;
        ULONG fg = big ? C_NOWBG : off ? C_DIM : on && b != LNB_FULL ? C_ACC : C_TEXT;
        if (ct_right(b)) {
            bx = rx;
            rx += s + gap;
            if (b == LNB_VDOWN && c->has_volume) {
                LONG vy = by + bh / 2 - 2, f = vw * c->volume / 64;
                H->rrect((WORD)rx, (WORD)vy, (WORD)vw, 4, 2, C_LINE, C_PANEL);
                if (f > 0)
                    H->rrect((WORD)rx, (WORD)vy, (WORD)f, 4, 2, C_ACC, C_LINE);
                rx += vw + gap;
            }
        } else {
            bx = x;
            x += s + gap;
        }
        {
            BOOL pr = !off && (c->pressed & LNB_BIT(b)) != 0;
            sbutton_p(bx, by, s, bh, big, foc, pr);
            nb_glyph(b, b == LNB_FULL ? on : big && !c->paused, bx + s / 2, by + bh / 2 + (pr ? 1 : 0),
                     bh * 9 / 20, fg);
        }
        c->at[i].x = (WORD)bx;
        c->at[i].y = (WORD)by;
        c->at[i].w = (WORD)s;
        c->at[i].h = (WORD)bh;
    }
}

static void draw_now(void)
{
    const struct LNow *n = H->model(LS_NOW);
    LONG py = NOW_PANEL, s = Y(112), cx = X(28), cy = py + Y(18), tx, tw, y;
    LONG bh = NOW_BH, by = HT - Y(14) - bh;
    char line[200];
    nt_y = -1;
    H->clip(0, 0, W, HT);
    H->fill(0, 0, W, HT, C_NOWBG);
    title_now();
    top_right(TRUE);
    draw_bars(n, TRUE);
    bars_at = H->now_ms();
    if (n && n->kind != LN_NONE)
        hit(X(20), Y(60), W - 2 * X(20), NOW_PANEL - Y(12) - Y(60), HT_PAUSE, 0, 0);
    H->fill(0, (WORD)py, W, (WORD)(HT - py), C_PANEL);
    H->fill(0, (WORD)py, W, 1, C_LINE);
    if (s > by - Y(12) - cy)
        s = by - Y(12) - cy;
    {
        const struct LImage *im = n && n->art ? H->picture(n->art, (UWORD)s, (UWORD)s) : NULL;
        if (im)
            H->image(im, (WORD)(cx + (s - im->w) / 2), (WORD)(cy + (s - im->h) / 2));
        else
            H->fill((WORD)cx, (WORD)cy, (WORD)s, (WORD)s, C_COVER);
    }
    tx = cx + s + X(24);
    tw = W - tx - X(28);
    if (!n || n->kind == LN_NONE) {
        text(tx, cy, H->tr("Nothing is playing"), LF_BOLD, C_WHITE, flat(C_PANEL), tw);
        return;
    }
    y = cy;
    text(tx, y, n->title, LF_TITLE, C_WHITE, flat(C_PANEL), tw);
    y += TH + Y(2);
    join(line, sizeof(line), n->artist, " \xB7 ", n->album);
    if (n->year && *n->year)
        join(line, sizeof(line), line, " ", n->year);
    text(tx, y, line, LF_REGULAR, C_ACC, flat(C_PANEL), tw);
    y += FH + Y(10);
    nt_x = tx;
    nt_y = y;
    nt_w = tw;
    now_time(n, TRUE);
    y += FH + Y(8);
    line[0] = 0;
    if (n->nnext > 0)
        join(line, sizeof(line), H->tr("Next:"), " ", n->next[0]);
    join(line, sizeof(line), line, " \xB7 ", n->format);
    join(line, sizeof(line), line, " \xB7 ", n->source);
    if (y + FH <= by - Y(6))
        text(tx, y, line, LF_SMALL, C_NEXT, flat(C_PANEL), tw);
    now_buttons(n, by, bh);
}

/* ---------------------------------------------------------------- the engine */
static BOOL sc_open(const struct LayoutHost *host)
{
    H = host;
    dcell[0] = dcell[1] = dcell[2] = dcell[3] = 0;     /* (another text size: measured again) */
    W = host->w;
    HT = host->h;
    FH = host->font_h(LF_REGULAR);
    if (FH <= 0)
        FH = 16;
    TH = host->font_h(LF_TITLE);
    if (TH < FH)
        TH = FH;
    bg_grad.top = C_TOP;
    bg_grad.bottom = C_BOT;
    bg_grad.y = 0;
    bg_grad.h = HT;
    drawn = -1;
    list_count = -1;
    return TRUE;
}

static void sc_close(void)
{
    H = NULL;
}

static void sc_draw(int screen)
{
    drawn = screen;
    pb_x = -1;                  /* (the playing bars: where top_right puts them, if it does) */
    ft_screen = FALSE;          /* (footer() says it has the band) */
    nhits = 0;
    hit_sel = -1;
    switch (screen) {
    case LS_HOME: draw_home(); break;
    case LS_NOW: draw_now(); break;
    case LS_ALBUMS: draw_wall(); break;
    case LS_ALBUM: draw_album(); break;
    case LS_ARTIST: draw_artist(); break;
    default: draw_page(); break;
    }
}

static void sc_changed_body(int screen, ULONG what);
static void sc_changed(int screen, ULONG what)
{
    sc_changed_body(screen, what);
    if (ft_screen && screen == drawn)
        footer_draw(FALSE);     /* (a key's hint, a toast's end) */
}
static void sc_changed_body(int screen, ULONG what)
{
    const struct LPage *m;
    if (what == LC_PLAYING && screen == drawn) {
        /* the time went on (the clock, the Debug line): they alone */
        const struct LNow *n = H->model(LS_NOW);
        H->clip(0, 0, W, HT);
        top_right_again(screen == LS_NOW);
        if (screen == LS_NOW && n && n->kind != LN_NONE)
            now_time(n, FALSE);
        return;
    }
    if (what == LC_WAIT) {
        if (screen == drawn && (screen == LS_PAGE || screen == LS_ALBUMS) && spin_x >= 0 &&
            (m = H->model(screen)) && m->waiting) {
            H->clip(0, 0, W, HT);
            wait_spinner(m);    /* the spinner's step: it alone */
        }
        return;
    }
    if ((what == LC_SELECT || what == LC_IMAGE) && screen == drawn && screen == LS_ALBUMS &&
        (m = H->model(LS_ALBUMS))) {
        draw_wall_body(m);      /* a move on the wall, or covers came: the wall alone */
        return;
    }
    if (what == LC_SELECT && screen == drawn && screen == LS_ALBUM) {
        H->clip(0, 0, W, HT);
        album_rows();
        return;
    }
    if (what == LC_SELECT && screen == drawn && screen == LS_ARTIST) {
        const struct LArtist *a = H->model(LS_ARTIST);
        if (a) {
            H->clip(0, 0, W, HT);
            draw_strip(a);
            return;
        }
    }
    if (screen == LS_PAGE && drawn == LS_PAGE && what == LC_SELECT && body_n < 0 &&
        (m = H->model(LS_PAGE)) && m->text) {
        draw_text_body_as(m, FALSE);    /* a text gliding: its lines and bar alone */
        return;
    }
    if (screen == LS_PAGE && drawn == LS_PAGE && (what == LC_SELECT || what == LC_DETAILS) && body_n >= 0 &&
        (m = H->model(LS_PAGE)) && !m->text && page_split(m) == body_n) {
        H->clip(0, 0, W, HT);
        draw_page_body(m);      /* a move: the list and its details, not the whole screen */
        return;
    }
    sc_draw(screen);
}

/* Now playing's bars, up to 25 a second, only their changes (the rest is
 * drawn when it changes) */
static struct Glide wall_g;
static ULONG pm_at;
static void sc_tick(ULONG ms)
{
    if (pb_x >= 0 && (LONG)(ms - pm_at) >= 100) {
        /* the top right's playing bars, as the sound goes: they alone, over
         * what is under them as drawn whole */
        const struct LNow *n = H->model(LS_NOW);
        pm_at = ms;
        if (n && pm_step(n)) {
            LONG bw = 3 * (pb_s / 4 > 1 ? pb_s / 4 : 2) + 4;
            H->clip((WORD)pb_x, (WORD)pb_y, (WORD)(pb_x + bw), (WORD)(pb_y + pb_s));
            if (pb_dark)
                H->fill((WORD)pb_x, (WORD)pb_y, (WORD)bw, (WORD)pb_s, C_NOWBG);
            else
                H->vgrad(0, 0, W, HT, C_TOP, C_BOT);    /* (the screen's one gradient) */
            play_bars(pb_x + bw, pb_y, pb_s, pm_lv);
            H->clip(0, 0, W, HT);
        }
    }
    if (drawn == LS_ALBUMS && wall_px != wall_tgt) {
        /* the wall gliding to the chosen cover's row, the band's toast over
         * it again */
        const struct LPage *m = H->model(LS_ALBUMS);
        wall_px = glide(&wall_g, wall_px, wall_tgt, ms);
        if (m) {
            H->clip(0, 0, W, HT);
            draw_wall_body(m);
            if (ft_shown)
                footer_paint(ft_shown);
        }
    }
    if (ft_screen)
        footer_draw(FALSE);     /* a toast or a hint over: drawn away */
    if (drawn != LS_NOW || (LONG)(ms - bars_at) < 40)
        return;
    bars_at = ms;
    H->clip(0, 0, W, HT);
    draw_bars(H->model(LS_NOW), FALSE);
}

/* The keys: the program's, as in the classic layout, but on the wall and
 * an artist's strip of albums Left and Right move along them, and on the
 * wall Up and Down a row */

static int sc_input(int screen, int action, LONG arg)
{
    const struct LPage *m;
    LONG sel, n, to;
    (void)arg;
    hov_what = -1;              /* the keys in use: no hover shown */
    hov_arg = -1;
    ft_key = H->now_ms();       /* ...and their hint, for a while */
    if (screen == LS_HOME) {
        /* the section's list as tiles in use: the arrows move among them
         * (Left at the left edge, as Back, to the menu) */
        const struct LHome *h = H->model(LS_HOME);
        if (!h || h->tile_sel < 0 || !h->nrows)
            return LR_UNUSED;
        sel = h->tile_sel;
        n = h->rows[0].n;
        to = sel;
        switch (action) {
        case LA_LEFT:
            if (sel % home_cols == 0)
                return LR_UNUSED;
            to = sel - 1;
            break;
        case LA_RIGHT:
            to = sel + 1 < n ? sel + 1 : sel;
            break;
        case LA_UP:
            to = sel - home_cols >= 0 ? sel - home_cols : sel;
            break;
        case LA_DOWN:
            to = sel + home_cols < n ? sel + home_cols
               : sel / home_cols < (n - 1) / home_cols ? n - 1 : sel;
            break;
        default:
            return LR_UNUSED;
        }
        if (to != sel)
            H->act(LAC_SELECT, to);
        return LR_DONE;
    }
    if (screen != LS_ALBUMS && screen != LS_ARTIST)
        return LR_UNUSED;
    if (!(m = H->model(screen)))
        return LR_UNUSED;
    sel = m->list.sel;
    n = m->list.count;
    to = sel;
    switch (action) {
    case LA_LEFT:
        if (screen == LS_ALBUMS ? sel % wall_cols == 0 : sel == 0)
            return LR_UNUSED;           /* the left edge: back, as ever */
        to = sel - 1;
        break;
    case LA_RIGHT:
        if (sel + 1 >= n)
            return LR_DONE;
        to = sel + 1;
        break;
    case LA_UP:
        if (screen != LS_ALBUMS)
            return LR_UNUSED;
        {
            int run = accel(-1);
            LONG st = wall_cols * (run >= 4 ? 3 : 1);
            to = run >= 10 ? letter_jump(&m->list, sel, -1) : sel - st >= 0 ? sel - st : sel - wall_cols >= 0 ? 0 : sel;
            if (run >= 10)
                wall_lead = to;
        }
        break;
    case LA_DOWN:
        if (screen != LS_ALBUMS)
            return LR_UNUSED;
        {
            int run = accel(1);
            LONG st = wall_cols * (run >= 4 ? 3 : 1);
            to = run >= 10 ? letter_jump(&m->list, sel, 1)
               : sel + st < n ? sel + st
               : sel / wall_cols < (n - 1) / wall_cols ? n - 1 : sel;  /* (into a short last row) */
            if (run >= 10)
                wall_lead = to;
        }
        break;
    default:
        return LR_UNUSED;
    }
    if (to != sel)
        H->act(LAC_SELECT, to);
    return LR_DONE;
}

/* A click, as in the classic layout: a section goes in; a row or tile is
 * opened, or (what plays) selected, and played with a double-click; a
 * crumb goes back to that level; what plays (top right) opens Now
 * playing, and on Now playing the bars pause and play. */
/* The pointer's hover moved: what shows it drawn again, as a move is */
static void hover_redraw(int screen)
{
    const void *m;
    if (screen != drawn || !(m = H->model(screen)))
        return;
    H->clip(0, 0, W, HT);
    switch (screen) {
    case LS_HOME:
        sc_draw(LS_HOME);
        break;
    case LS_PAGE:
        if (body_n >= 0 && !((const struct LPage *)m)->text)
            draw_page_body((const struct LPage *)m);
        break;
    case LS_ALBUMS:
        draw_wall_body((const struct LPage *)m);
        break;
    case LS_ALBUM:
        album_rows();           /* its tracks (a move redraws them so too) */
        break;
    case LS_ARTIST:
        draw_strip((const struct LArtist *)m);
        break;
    case LS_NOW: {
        LONG by = HT - Y(14) - NOW_BH, e = X(6) + 2;
        H->fill(0, (WORD)(by - e), W, (WORD)(NOW_BH + 2 * e), C_PANEL);
        now_buttons((const struct LNow *)m, by, NOW_BH);
        break;
    }
    }
}

static int sc_pointer(int screen, WORD x, WORD y, BOOL click)
{
    const struct Hit *t = NULL;
    int i;
    BOOL dbl;
    ULONG now;
    if (!H)
        return LR_UNUSED;
    if (click == LP_RELEASE) {
        if (sb_drag >= 0) {
            sb_drag = -1;           /* (the thumb drawn as at rest with the next frame) */
            return LR_DONE;
        }
        return LR_UNUSED;
    }
    if (!click && sb_drag >= 0) {
        scroll_to(y);               /* a scroll bar dragged: the list or text follows */
        return LR_DONE;
    }
    if (!click) {
        /* a move: what it is over, glowing (rows, tiles, sections, buttons) */
        int w = -1;
        LONG a = -1;
        for (i = nhits - 1; i >= 0; i--)
            if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) {
                if (hits[i].what == HT_ROW || hits[i].what == HT_TILE || hits[i].what == HT_SECTION ||
                    hits[i].what == HT_BUTTON) {
                    w = hits[i].what;
                    a = hits[i].arg;
                }
                break;
            }
        if (w == hov_what && a == hov_arg)
            return LR_UNUSED;
        hov_what = w;
        hov_arg = a;
        hover_redraw(screen);
        return LR_DONE;
    }
    for (i = nhits - 1; i >= 0; i--)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) {
            t = &hits[i];
            break;
        }
    if (!t)
        return LR_UNUSED;
    if (t->what == HT_SCROLL) {
        int k = (int)t->arg & 3;
        if (y < sbar[k].t || y >= sbar[k].t + sbar[k].tl) {
            scroll_page(k, y < sbar[k].t ? -1 : 1);    /* off the thumb: a page that way */
            return LR_DONE;
        }
        sb_drag = k;
        sb_off = y - sbar[k].t;
        return LR_DONE;
    }
    now = H->now_ms();
    dbl = (t->what == HT_ROW || t->what == HT_TILE) && t->arg == click_row && now - click_ms < 500;
    click_row = t->what == HT_ROW || t->what == HT_TILE ? t->arg : -1;
    click_ms = now;
    switch (t->what) {
    case HT_SECTION:
        H->act(LAC_SECTION, t->arg);
        H->act(LAC_ENTER, 0);
        click_row = -1;
        break;
    case HT_TILE:               /* the chosen section's: into it, at that row */
        H->act(LAC_ENTER, 0);
        H->act(playable(t->kind) ? LAC_SELECT : LAC_OPEN, t->arg);
        click_row = -1;
        break;
    case HT_ROW:
        if (!playable(t->kind)) {
            H->act(LAC_OPEN, t->arg);
            click_row = -1;
        } else if (dbl && t->arg == hit_sel) {
            H->act(LAC_OPEN, t->arg);
            click_row = -1;
        } else {
            H->act(LAC_SELECT, t->arg);
        }
        break;
    case HT_CRUMB: {            /* back to that level */
        const struct LPage *m = H->model(screen);
        LONG n = m ? m->ncrumb - 1 - t->arg : 0;
        while (n-- > 0)
            H->act(LAC_BACK, 0);
        break;
    }
    case HT_NOWPLAYING:
        H->act(LAC_NOWPLAYING, 0);
        break;
    case HT_PAUSE:
        H->act(LAC_PAUSE, 0);
        break;
    case HT_HOME:               /* the logo: Home */
        H->act(LAC_HOME, 0);
        break;
    case HT_BUTTON:             /* Now playing's */
        H->act(LAC_BUTTON, t->arg);
        break;
    case HT_SEEK:               /* its bar: to there */
        H->act(LAC_SEEK, (x - t->x) * 65536 / (t->w > 0 ? t->w : 1));
        break;
    }
    return LR_DONE;
}

const struct LayoutEngine layout_engine = {
    LAYOUT_ABI, "Showcase", 15, 480,
    LS_BIT(LS_HOME) | LS_BIT(LS_PAGE) | LS_BIT(LS_NOW) | LS_BIT(LS_ALBUMS) | LS_BIT(LS_ALBUM) |
        LS_BIT(LS_ARTIST),
    sc_open, sc_close, sc_draw, sc_changed, sc_tick, sc_input, sc_pointer, sc_controls_h, sc_controls
};
