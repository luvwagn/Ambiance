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

const char LibName[] = "showcase.layout";
const char LibIdString[] = "showcase.layout 1.8 (17.11.2026) Showcase, for Ambiance";
static const char __attribute__((used)) LibVerTag[] =
    "$VER: showcase.layout 1.8 (17.11.2026) Showcase, for Ambiance";
const UWORD LibVersion = 1;
const UWORD LibRevision = 8;

/* the colours (the mockups') */
#define C_TOP    0x0F1D2CUL     /* the gradient */
#define C_BOT    0x08111AUL
#define C_PANEL  0x132131UL
#define C_PANEL2 0x0E1A27UL
#define C_FOCUS  0x1E3A55UL
#define C_LINE   0x22364BUL
#define C_ACC    0x7FD3FFUL
#define C_SUB    0xB9C7D3UL
#define C_DIM    0x5F7183UL
#define C_TEXT   0xE6EEF4UL
#define C_BODY   0xD9E3EAUL
#define C_WHITE  0xFFFFFFUL
#define C_NOWBG  0x02040AUL
#define C_COVER  0x8A4B2AUL     /* no cover: a copper square */
#define C_NEXT   0x9FB0BFUL
#define C_LETTER 0x2C4864UL     /* the big letter: faint, over the list */

static const struct LayoutHost *H;
static WORD W, HT, FH, TH;      /* the screen; a line of text; a title's (LF_TITLE) */
static int drawn = -1;          /* the screen drawn last */
static ULONG bars_at;           /* Now playing's bars, drawn then */
static LONG list_top;           /* the generic page's first row shown */
static LONG list_count = -1;
static const void *list_of;

/* What a click lands on: the places drawn last, noted as they are drawn
 * (sc_pointer looks them up). */
enum { HT_SECTION, HT_TILE, HT_ROW, HT_CRUMB, HT_NOWPLAYING, HT_PAUSE };
struct Hit {
    WORD x, y, w, h;
    UBYTE what;                 /* HT_* */
    UBYTE kind;                 /* a row's or tile's LI_* */
    LONG arg;                   /* its index (a crumb: its level) */
};
#define MAX_HITS 64
static struct Hit hits[MAX_HITS];
static int nhits;
static LONG hit_sel = -1;       /* the list's selected row, as drawn */
static LONG click_row = -1;     /* a double-click: the row clicked last, and when */
static ULONG click_ms;

static void hit(LONG x, LONG y, LONG w, LONG h, int what, int kind, LONG arg)
{
    if (nhits < MAX_HITS && w > 0 && h > 0) {
        struct Hit *t = &hits[nhits++];
        t->x = (WORD)x;
        t->y = (WORD)y;
        t->w = (WORD)w;
        t->h = (WORD)h;
        t->what = (UBYTE)what;
        t->kind = (UBYTE)kind;
        t->arg = arg;
    }
}

/* ---------------------------------------------------------------- little helpers */
static LONG X(LONG v) { return v * W / 800; }
static LONG Y(LONG v) { return v * HT / 600; }

static LONG slen(const char *s)
{
    LONG n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}

static ULONG mix(ULONG a, ULONG b, LONG num, LONG den)
{
    ULONG o = 0;
    int sh;
    if (den <= 0)
        return a;
    for (sh = 0; sh <= 16; sh += 8) {
        LONG ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        o |= (ULONG)(ca + (cb - ca) * num / den) << sh;
    }
    return o;
}

/* the gradient's colour at row y */
static ULONG grad(LONG y)
{
    return mix(C_TOP, C_BOT, y, HT - 1);
}

static struct LkBg bg_grad, bg_flat;

static const struct LkBg *flat(ULONG c)
{
    bg_flat.top = bg_flat.bottom = c;
    bg_flat.y = 0;
    bg_flat.h = 1;
    return &bg_flat;
}

static WORD text(LONG x, LONG y, const char *s, int face, ULONG fg, const struct LkBg *bg, LONG maxw)
{
    if (!s || !*s || maxw < 0)
        return 0;
    return H->text((WORD)x, (WORD)y, s, slen(s), face, fg, bg, (WORD)maxw);
}

static WORD tlen(const char *s, int face)
{
    return s && *s ? H->textlen(s, slen(s), face) : 0;
}

/* a panel: rounded, its edge a line's colour, on bg (what the corners show) */
static void panel(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG fill, ULONG edge, ULONG bg)
{
    H->rrect((WORD)x, (WORD)y, (WORD)w, (WORD)h, (WORD)r, edge, bg);
    H->rrect((WORD)(x + 1), (WORD)(y + 1), (WORD)(w - 2), (WORD)(h - 2), (WORD)(r > 1 ? r - 1 : 0),
             fill, edge);
}

/* "m:ss" */
static char *mmss(ULONG ms, char *b)
{
    ULONG s = ms / 1000, m = s / 60;
    char t[12];
    int n = 0, i = 0;
    s %= 60;
    do {
        t[n++] = (char)('0' + m % 10);
        m /= 10;
    } while (m);
    while (n)
        b[i++] = t[--n];
    b[i++] = ':';
    b[i++] = (char)('0' + s / 10);
    b[i++] = (char)('0' + s % 10);
    b[i] = 0;
    return b;
}

/* s + sep + t into b (max bytes) */
static const char *join(char *b, LONG max, const char *s, const char *sep, const char *t)
{
    LONG n = 0;
    const char *p;
    for (p = s; p && *p && n < max - 1; p++)
        b[n++] = *p;
    if (s && *s && t && *t)
        for (p = sep; *p && n < max - 1; p++)
            b[n++] = *p;
    for (p = t; p && *p && n < max - 1; p++)
        b[n++] = *p;
    b[n] = 0;
    return b;
}

/* ---------------------------------------------------------------- shared parts */
static void background(void)
{
    H->clip(0, 0, W, HT);
    H->vgrad(0, 0, W, HT, C_TOP, C_BOT);
}

/* top right: what plays (if anything) and the clock */
static void top_right(BOOL dark)
{
    const struct LNow *n = H->model(LS_NOW);
    LONG x = W - X(24), cw = tlen(H->clock, LF_TITLE), y = Y(16);
    const struct LkBg *bg = dark ? flat(C_NOWBG) : &bg_grad;
    x -= cw;
    text(x, y + (FH - TH) / 2, H->clock, LF_TITLE, C_WHITE, bg, 0);   /* the big clock */
    if (n && n->kind != LN_NONE && n->title && *n->title) {
        char b[16];
        LONG tw, room = x - X(40) - W / 2;
        const char *t = mmss(n->pos_ms, b);
        x -= X(18) + tlen(t, LF_SMALL);
        text(x, y, t, LF_SMALL, C_SUB, bg, 0);
        tw = tlen(n->title, LF_SMALL);
        if (tw > room)
            tw = room;
        if (tw > 0) {
            x -= X(10) + tw;
            text(x, y, n->title, LF_SMALL, C_SUB, bg, tw);
            hit(x - X(8) - FH, y - Y(4), W - X(24) - cw - (x - X(8) - FH), FH + Y(8), HT_NOWPLAYING, 0, 0);
            /* the play triangle */
            {
                LONG s = FH / 2, ty = y + (FH - s) / 2, i;
                for (i = 0; i < s; i++)
                    H->fill((WORD)(x - X(8) - s + i / 2), (WORD)(ty + i / 2), 1, (WORD)(s - i), C_ACC);
            }
        }
    }
}

static void title_line(const char *title, const char *sub)
{
    LONG x = X(30), y = Y(16);
    WORD w = text(x, y + (FH - TH) / 2, title, LF_TITLE, C_WHITE, &bg_grad, W / 2);
    if (sub && *sub)
        text(x + w + X(14), y, sub, LF_SMALL, C_SUB, &bg_grad, W / 2 - w - X(14));
}

static void footer(const char *hint, const char *message)
{
    LONG y = HT - Y(18) - FH;
    if (message && *message)
        text(X(30), y, message, LF_SMALL, C_ACC, &bg_grad, W - X(60));
    else
        text(X(30), y, hint, LF_SMALL, C_SUB, &bg_grad, W - X(60));
}

/* a row's mark, its right edge at x, centred on cy; its width */
static LONG mark(int m, LONG x, LONG cy, ULONG fg, ULONG bg)
{
    LONG s = FH * 2 / 3, y = cy - s / 2, i;
    if (s < 6)
        s = 6;
    switch (m) {
    case LM_ARROW:              /* a chevron */
        for (i = 0; i < s / 2; i++) {
            H->fill((WORD)(x - s / 2 + i / 2), (WORD)(y + i), 2, 1, fg);
            H->fill((WORD)(x - s / 2 + i / 2), (WORD)(y + s - 1 - i), 2, 1, fg);
        }
        return s / 2 + 2;
    case LM_CHECK_ON: case LM_CHECK_OFF:
        H->frame((WORD)(x - s), (WORD)y, (WORD)s, (WORD)s, 1, fg);
        if (m == LM_CHECK_ON)
            H->fill((WORD)(x - s + 3), (WORD)(y + 3), (WORD)(s - 6), (WORD)(s - 6), C_ACC);
        return s;
    case LM_RADIO_ON: case LM_RADIO_OFF:
        H->rrect((WORD)(x - s), (WORD)y, (WORD)s, (WORD)s, (WORD)(s / 2), fg, bg);
        H->rrect((WORD)(x - s + 1), (WORD)(y + 1), (WORD)(s - 2), (WORD)(s - 2), (WORD)(s / 2 - 1),
                 bg, fg);
        if (m == LM_RADIO_ON)
            H->rrect((WORD)(x - s + 4), (WORD)(y + 4), (WORD)(s - 8), (WORD)(s - 8),
                     (WORD)(s / 2 - 4), C_ACC, bg);
        return s;
    case LM_PLAY: case LM_PLAYING:  /* a play triangle; playing: three bars */
        if (m == LM_PLAYING) {
            LONG bw = s / 4 > 1 ? s / 4 : 2;
            H->fill((WORD)(x - 3 * bw - 4), (WORD)(y + s / 3), (WORD)bw, (WORD)(s - s / 3), C_ACC);
            H->fill((WORD)(x - 2 * bw - 2), (WORD)y, (WORD)bw, (WORD)s, C_ACC);
            H->fill((WORD)(x - bw), (WORD)(y + s / 2), (WORD)bw, (WORD)(s - s / 2), C_ACC);
            return 3 * bw + 4;
        }
        for (i = 0; i < s; i++)
            H->fill((WORD)(x - s + i / 2), (WORD)(y + i / 2), 1, (WORD)(s - i), fg);
        return s;
    case LM_CYCLE:              /* two small arrows: < > */
        for (i = 0; i < s / 2; i++) {
            H->fill((WORD)(x - s + s / 2 - i / 2 - 1), (WORD)(y + i), 1, 1, fg);
            H->fill((WORD)(x - s + s / 2 - i / 2 - 1), (WORD)(y + s - 1 - i), 1, 1, fg);
            H->fill((WORD)(x - s / 2 + i / 2), (WORD)(y + i), 1, 1, fg);
            H->fill((WORD)(x - s / 2 + i / 2), (WORD)(y + s - 1 - i), 1, 1, fg);
        }
        return s;
    }
    return 0;
}

/* ---------------------------------------------------------------- Home */
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
        H->image(logo, (WORD)X(14), (WORD)Y(10));
        top = Y(10) + logo->h + Y(10);
    }
    n = m->n;
    while (n > 1 && n * ih + (n - 1) * gap > HT - top - Y(20))
        ih--;                   /* (a short screen: rows a little closer) */
    y0 = top + (HT - top - Y(20) - (n * ih + (n - 1) * gap)) / 2;
    for (i = 0; i < n; i++) {
        BOOL sel = i == m->sel;
        LONG x = X(18), y = y0 + i * (ih + gap), w = X(172), tx = x + X(14);
        ULONG bg = sel ? C_FOCUS : C_PANEL2;
        hit(x, y, w, ih, HT_SECTION, 0, i);
        if (sel) {
            H->rrect((WORD)x, (WORD)y, (WORD)w, (WORD)ih, 4, C_FOCUS, C_PANEL2);
            H->fill((WORD)x, (WORD)(y + 2), 3, (WORD)(ih - 4), C_ACC);
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
            LONG cw = X(170), ch = FH * 2 + Y(22), g = X(12), cols, k;
            if (cw < tlen("Mmmmmmmmmm", LF_REGULAR))
                cw = tlen("Mmmmmmmmmm", LF_REGULAR);
            cols = (aw + g) / (cw + g);
            if (cols < 1)
                cols = 1;
            cw = (aw - (cols - 1) * g) / cols;
            text(x0, y, row->title, LF_REGULAR, C_SUB, &bg_grad, aw);
            y += FH + Y(10);
            for (k = 0; k < row->n; k++) {
                const struct LItem *it = &row->items[k];
                LONG cx = x0 + (k % cols) * (cw + g), cy = y + (k / cols) * (ch + g), tx;
                if (cy + ch > HT - Y(24))
                    break;
                if (it->flags & LIF_HEAD) {
                    text(cx + X(4), cy + (ch - FH) / 2, it->label, LF_SMALL, C_SUB, &bg_grad, cw);
                    continue;
                }
                panel(cx, cy, cw, ch, 4, C_PANEL, C_LINE, grad(cy));
                if (!(it->flags & LIF_DISABLED))
                    hit(cx, cy, cw, ch, HT_TILE, it->kind, k);
                tx = cx + X(12);
                if (it->art) {          /* its picture: a square at the left */
                    LONG s = ch - Y(12);
                    const struct LImage *im = H->picture(it->art, (UWORD)s, (UWORD)s);
                    if (im)
                        H->image(im, (WORD)(cx + Y(6) + (s - im->w) / 2), (WORD)(cy + Y(6) + (s - im->h) / 2));
                    else
                        H->fill((WORD)(cx + Y(6)), (WORD)(cy + Y(6)), (WORD)s, (WORD)s, C_PANEL2);
                    tx = cx + Y(6) + s + X(10);
                }
                text(tx, cy + Y(8), it->label, LF_REGULAR,
                     (it->flags & LIF_DISABLED) ? C_DIM : C_TEXT, flat(C_PANEL), cx + cw - tx - X(12));
                if (it->tag || it->sub)
                    text(tx, cy + Y(8) + FH + Y(4), it->tag ? it->tag : it->sub, LF_SMALL,
                         C_SUB, flat(C_PANEL), cx + cw - tx - X(12));
                if (it->flags & LIF_NOW)
                    mark(LM_PLAYING, cx + cw - X(10), cy + Y(8) + FH / 2, C_ACC, C_PANEL);
            }
            y += ((row->n + cols - 1) / cols) * (ch + g) + Y(14);
        }
    }
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
        H->rrect((WORD)bx, (WORD)by, 4, (WORD)bh, 2, C_LINE, C_PANEL);
        H->rrect((WORD)bx, (WORD)ty, 4, (WORD)th, 2, C_ACC, C_LINE);
    }
}

/* lines of text from line from, each wrapped at w (at a space where
 * there is one), down to bottom; the first in first's colour (bold if it
 * differs) */
static void draw_lines(const char *const *s, LONG n, LONG from, LONG x, LONG y, LONG w, LONG bottom,
                       ULONG first, ULONG rest)
{
    LONG i, lh = FH + Y(4);
    for (i = from; i < n && y + lh <= bottom; i++) {
        const char *p = s[i];
        LONG len = slen(p);
        int face = i == 0 && first != rest ? LF_BOLD : LF_REGULAR;
        ULONG c = i == 0 ? first : rest;
        if (!len) {
            y += lh;
            continue;
        }
        while (len > 0 && y + lh <= bottom) {
            LONG k = H->fit(p, len, face, (WORD)w), j;
            if (k <= 0)
                k = 1;
            if (k < len) {
                for (j = k; j > 0 && p[j] != ' '; j--)
                    ;
                if (j > 0)
                    k = j;
            }
            H->text((WORD)x, (WORD)y, p, k, face, c, flat(C_PANEL), 0);
            y += lh;
            p += k;
            len -= k;
            while (len > 0 && *p == ' ') {
                p++;
                len--;
            }
        }
    }
}

static int hits_base;           /* the click targets before the page's body's */

/* One long text: all of the panel, scrolled by pixels (text_off) */
static void draw_text_body(const struct LPage *m)
{
    LONG py = Y(64), bottom = HT - Y(30) - FH - Y(12), x = X(236), w = W - x - X(24);
    LONG top = py + Y(14), lh = FH + Y(4);
    panel(x, py, w, bottom - py, 6, C_PANEL, C_LINE, grad(py));
    H->clip((WORD)(x + 2), (WORD)(py + Y(6)), (WORD)(x + w - 2), (WORD)(bottom - Y(6)));
    draw_lines(m->details, m->ndetails, m->list.top, x + X(18), top - lh * m->text_off / 256,
               w - X(36), bottom - Y(6) + lh, C_BODY, C_BODY);
    H->clip(0, 0, W, HT);
}
static int body_n = -1;         /* drawn with a details panel (1) or not (0); -1 text */
static void draw_page_body(const struct LPage *m);

static void draw_lines(const char *const *s, LONG n, LONG from, LONG x, LONG y, LONG w, LONG bottom,
                       ULONG first, ULONG rest);

static void draw_page(void)
{
    const struct LPage *m = H->model(LS_PAGE);
    char sub[160];
    LONG i, py = Y(64), bottom = HT - Y(30) - FH - Y(12);
    LONG lx = X(24), lw = X(196);
    background();
    top_right(FALSE);
    if (!m)
        return;
    /* the title: the page, and the path to it */
    sub[0] = 0;
    for (i = 0; i + 1 < m->ncrumb; i++)
        join(sub, sizeof(sub), sub, " / ", m->crumb[i]);
    title_line(m->ncrumb ? m->crumb[m->ncrumb - 1] : "", sub);
    /* left: the section and the way here */
    panel(lx, py, lw, bottom - py, 6, C_PANEL2, C_PANEL2, grad(py));
    {
        LONG y = py + Y(14), ip = H->icon_px, tx = lx + X(16), rh = FH + Y(18);
        if (ip) {
            H->icon(m->section, (WORD)tx, (WORD)y, C_ACC, flat(C_PANEL2));
            tx += ip + X(10);
        }
        text(tx, y + (ip > FH ? (ip - FH) / 2 : 0), m->ncrumb ? m->crumb[0] : "", LF_BOLD, C_WHITE,
             flat(C_PANEL2), lx + lw - tx - X(8));
        y += (ip > FH ? ip : FH) + Y(16);
        for (i = 1; i < m->ncrumb && y + rh < bottom - Y(8); i++, y += rh + Y(2)) {
            BOOL last = i == m->ncrumb - 1;
            if (last)
                H->rrect((WORD)(lx + X(8)), (WORD)y, (WORD)(lw - X(16)), (WORD)rh, 4, C_FOCUS, C_PANEL2);
            else
                hit(lx + X(8), y, lw - X(16), rh, HT_CRUMB, 0, i);
            text(lx + X(8) + X(12) + (i - 1) * X(6), y + (rh - FH) / 2, m->crumb[i],
                 LF_REGULAR, last ? C_WHITE : C_SUB, flat(last ? C_FOCUS : C_PANEL2),
                 lw - X(40) - (i - 1) * X(6));
        }
    }
    hits_base = nhits;
    if (m->text) {
        body_n = -1;
        draw_text_body(m);
        footer(m->hint, m->message);
        return;
    }
    draw_page_body(m);
    footer(m->hint, m->message);
}

/* The list's panel and the details' under it: all a move needs drawing
 * again (changed(LC_SELECT)), as long as the details' panel is still
 * there or still not */
static void draw_page_body(const struct LPage *m)
{
    LONG n, py = Y(64), bottom = HT - Y(30) - FH - Y(12), x = X(236), w = W - x - X(24);
    n = m->ndetails > 0 || m->picture ? 1 : 0;
    body_n = (int)n;
    nhits = hits_base;          /* the rows' click targets made again */
    {
        LONG lh = n ? (bottom - py) * 62 / 100 : bottom - py;
        LONG dy = py + lh + Y(12), dh = bottom - dy;
        panel(x, py, w, lh, 6, C_PANEL, C_LINE, grad(py));
        if (m->list.count)
            draw_rows(m, x, py, w, lh);
        else
            text(x + X(22), py + Y(18), H->tr("Nothing here"), LF_REGULAR, C_SUB, flat(C_PANEL), w);
        if (n && dh > FH * 2) {
            LONG tx = x + X(18);
            panel(x, dy, w, dh, 6, C_PANEL, C_LINE, grad(dy));
            if (m->picture) {
                LONG ph = dh - Y(24), pw = ph * 4 / 3;
                const struct LImage *im = H->picture(m->picture, (UWORD)pw, (UWORD)ph);
                if (im) {
                    H->image(im, (WORD)(tx + (pw - im->w) / 2), (WORD)(dy + Y(12) + (ph - im->h) / 2));
                } else {
                    H->fill((WORD)tx, (WORD)(dy + Y(12)), (WORD)pw, (WORD)ph, C_PANEL2);
                }
                tx += pw + X(18);
            }
            draw_lines(m->details, m->ndetails, 0, tx, dy + Y(12), x + w - tx - X(18),
                       dy + dh - Y(8), C_WHITE, C_BODY);
        }
        if (m->letter && H->letter)     /* the big letter, as the list glides or jumps */
            H->letter((WORD)(x + w - X(40)), (WORD)(py + lh / 2), C_LETTER);
    }
}

/* ---------------------------------------------------------------- shared by the music screens */
/* The page's title and its path, the left panel's way back left out */
static void page_title(const struct LPage *m)
{
    char sub[160];
    LONG i;
    sub[0] = 0;
    for (i = 0; i + 1 < m->ncrumb; i++)
        join(sub, sizeof(sub), sub, " / ", m->crumb[i]);
    title_line(m->ncrumb ? m->crumb[m->ncrumb - 1] : "", sub);
}

/* A cover s square at x, y: the picture, or a panel with the name's
 * first letter until it is ready (or if it has none) */
static void cover(const char *art, const char *label, LONG x, LONG y, LONG s, ULONG bg)
{
    const struct LImage *im = art ? H->picture(art, (UWORD)s, (UWORD)s) : NULL;
    if (im) {
        if (im->w < s || im->h < s)
            H->fill((WORD)x, (WORD)y, (WORD)s, (WORD)s, bg);
        H->image(im, (WORD)(x + (s - im->w) / 2), (WORD)(y + (s - im->h) / 2));
        return;
    }
    H->rrect((WORD)x, (WORD)y, (WORD)s, (WORD)s, 4, C_PANEL, bg);
    if (label && *label)
        text(x + (s - tlen("M", LF_BOLD)) / 2, y + (s - FH) / 2, label, LF_BOLD, C_DIM, flat(C_PANEL),
             tlen("M", LF_BOLD) + 2);
}

/* Paragraphs (a line each, "\n" between) into lines for draw_lines */
static const char *para[64];
static char parabuf[2048];
static LONG paras(const char *t)
{
    LONG n = 0, k = 0;
    if (!t)
        return 0;
    while (*t && n < 64 && k < (LONG)sizeof(parabuf) - 1) {
        para[n++] = &parabuf[k];
        while (*t && *t != '\n' && k < (LONG)sizeof(parabuf) - 1)
            parabuf[k++] = *t++;
        parabuf[k++] = 0;
        if (*t == '\n')
            t++;
    }
    return n;
}

/* ---------------------------------------------------------------- the album wall */
static LONG wall_cols = 1, wall_top;    /* its columns, as drawn; its first row shown */

static void draw_wall_body(const struct LPage *m)
{
    LONG x0 = X(30), y0 = Y(60), aw = W - x0 - X(30), bottom = HT - Y(30) - FH - Y(12);
    LONG gap = X(18), cs = X(124), cols, ch, rows, i, sel = m->list.sel;
    const struct LList *l = &m->list;
    if (cs < FH * 4)
        cs = FH * 4;
    cols = (aw + gap) / (cs + gap);
    if (cols < 1)
        cols = 1;
    cs = (aw - (cols - 1) * gap) / cols;
    ch = cs + Y(8) + FH + Y(2) + FH + Y(14);
    rows = (bottom - y0) / ch;
    if (rows < 1)
        rows = 1;
    wall_cols = cols;
    if (sel / cols < wall_top)
        wall_top = sel / cols;
    if (sel / cols >= wall_top + rows)
        wall_top = sel / cols - rows + 1;
    if (wall_top < 0)
        wall_top = 0;
    nhits = hits_base;
    hit_sel = sel;
    H->clip((WORD)(x0 - X(10)), (WORD)(y0 - Y(6)), (WORD)(W - X(10)), (WORD)bottom);
    H->vgrad((WORD)(x0 - X(10)), (WORD)(y0 - Y(6)), (WORD)(W - x0), (WORD)(bottom - y0 + Y(6)),
             grad(y0 - Y(6)), grad(bottom));
    for (i = wall_top * cols; i < l->count && i < (wall_top + rows) * cols; i++) {
        const struct LItem *it = l->item(i);
        LONG cx = x0 + (i % cols) * (cs + gap), cy = y0 + (i / cols - wall_top) * ch;
        BOOL on = i == sel;
        ULONG bg = grad(cy);
        if (it->flags & LIF_HEAD)
            continue;
        hit(cx, cy, cs, ch - Y(10), HT_ROW, it->kind, i);
        cover(it->art, it->label, cx, cy, cs, bg);
        if (on)
            H->frame((WORD)(cx - 3), (WORD)(cy - 3), (WORD)(cs + 6), (WORD)(cs + 6), 3, C_ACC);
        if (it->flags & LIF_NOW)
            mark(LM_PLAYING, cx + cs - X(8), cy + cs - FH, C_ACC, bg);
        text(cx, cy + cs + Y(8), it->label, on ? LF_BOLD : LF_REGULAR, on ? C_WHITE : C_TEXT,
             flat(grad(cy + cs + Y(8))), cs);
        if (it->sub || it->tag)
            text(cx, cy + cs + Y(8) + FH + Y(2), it->sub ? it->sub : it->tag, LF_SMALL, C_SUB,
                 flat(grad(cy + cs + Y(8) + FH)), cs);
    }
    if (l->count > rows * cols) {            /* where in the wall */
        LONG nr = (l->count + cols - 1) / cols, bx = W - X(16), bh = bottom - y0;
        LONG th = bh * rows / nr, ty = y0 + (bh - (th < Y(16) ? Y(16) : th)) * wall_top / (nr - rows > 0 ? nr - rows : 1);
        if (th < Y(16))
            th = Y(16);
        H->rrect((WORD)bx, (WORD)y0, 4, (WORD)bh, 2, C_LINE, grad(y0));
        H->rrect((WORD)bx, (WORD)ty, 4, (WORD)th, 2, C_ACC, C_LINE);
    }
    if (m->letter && H->letter)
        H->letter((WORD)(W - X(60)), (WORD)(y0 + (bottom - y0) / 2), C_LETTER);
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
    const struct LAlbum *a = H->model(LS_ALBUM);
    LONG py = Y(64), bottom = HT - Y(30) - FH - Y(12), cx = X(30), cs = X(240), y, i, n;
    LONG x, w;
    background();
    top_right(FALSE);
    if (!a)
        return;
    page_title(&a->page);
    if (cs > bottom - py - FH * 6)
        cs = bottom - py - FH * 6;
    cover(a->art, a->title, cx, py, cs, grad(py));
    y = py + cs + Y(14);
    y += text(cx, y, a->title, LF_TITLE, C_WHITE, &bg_grad, cs) ? TH + Y(2) : 0;
    if (a->artist && *a->artist) {
        text(cx, y, a->artist, LF_REGULAR, C_ACC, &bg_grad, cs);
        y += FH + Y(4);
    }
    if (a->line && *a->line) {
        text(cx, y, a->line, LF_SMALL, C_SUB, &bg_grad, cs);
        y += FH + Y(2);
    }
    if (a->rating && *a->rating) {
        char b[24];
        join(b, sizeof(b), H->tr("Rating"), ": ", a->rating);
        text(cx, y, b, LF_SMALL, C_SUB, &bg_grad, cs);
        y += FH + Y(2);
    }
    for (i = 0; i < a->facts.n && y + FH <= bottom; i++) {
        char b[140];
        join(b, sizeof(b), a->facts.facts[2 * i], ": ", a->facts.facts[2 * i + 1]);
        text(cx, y, b, LF_SMALL, C_SUB, &bg_grad, cs);
        y += FH + Y(2);
    }
    x = cx + cs + X(24);
    w = W - x - X(24);
    album_x = x;
    album_w = w;
    hits_base = nhits;
    n = a->review && *a->review ? 1 : 0;
    {
        LONG lh = n ? (bottom - py) * 60 / 100 : bottom - py;
        panel(x, py, w, lh, 6, C_PANEL, C_LINE, grad(py));
        if (a->page.list.count)
            draw_rows(&a->page, x, py, w, lh);
        if (n) {
            LONG dy = py + lh + Y(12), np = paras(a->review);
            panel(x, dy, w, bottom - dy, 6, C_PANEL, C_LINE, grad(dy));
            draw_lines(para, np, 0, x + X(18), dy + Y(12), w - X(36), bottom - Y(8), C_BODY, C_BODY);
        }
        if (a->page.letter && H->letter)
            H->letter((WORD)(x + w - X(40)), (WORD)(py + lh / 2), C_LETTER);
    }
    footer(a->page.hint, a->page.message);
}

/* a move on an album: its tracks' panel alone */
static void album_rows(void)
{
    const struct LAlbum *a = H->model(LS_ALBUM);
    LONG py = Y(64), bottom = HT - Y(30) - FH - Y(12);
    LONG lh = a && a->review && *a->review ? (bottom - py) * 60 / 100 : bottom - py;
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
    page_title(&a->page);
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

static void draw_bars(const struct LNow *n)
{
    LONG x0 = X(20), y0 = Y(52), aw = W - 2 * x0, ah = Y(412) - y0, nb = 48, gap = 3, bw, i;
    static const UBYTE shape[12] = { 9, 12, 15, 13, 11, 14, 10, 8, 12, 9, 7, 10 };
    if (aw < nb * 4)
        nb = aw / 4;
    bw = (aw - (nb - 1) * gap) / nb;
    H->fill((WORD)x0, (WORD)y0, (WORD)aw, (WORD)ah, C_NOWBG);
    if (!n || !n->nlevels || !n->levels || n->paused)
        return;
    for (i = 0; i < nb; i++) {
        LONG lv = n->levels[i * n->nlevels / nb], h = ah * lv * shape[(i * 7) % 12] / (255 * 15);
        if (h < 2)
            h = 2;
        H->fill((WORD)(x0 + i * (bw + gap)), (WORD)(y0 + ah - h), (WORD)bw, (WORD)h,
                bar_col[i * 12 / nb]);
    }
}

static void draw_now(void)
{
    const struct LNow *n = H->model(LS_NOW);
    LONG py = Y(448), s = Y(104), cx = X(28), cy = Y(472), tx, tw, y;
    char b1[16], b2[16], line[200];
    H->clip(0, 0, W, HT);
    H->fill(0, 0, W, HT, C_NOWBG);
    text(X(30), Y(18), H->tr("Now playing"), LF_SMALL, C_NEXT, flat(C_NOWBG), W / 2);
    top_right(TRUE);
    draw_bars(n);
    bars_at = H->now_ms();
    if (n && n->kind != LN_NONE)
        hit(X(20), Y(52), W - 2 * X(20), Y(412) - Y(52), HT_PAUSE, 0, 0);
    H->fill(0, (WORD)py, W, (WORD)(HT - py), C_PANEL);
    H->fill(0, (WORD)py, W, 1, C_LINE);
    if (s > HT - cy - Y(12))
        s = HT - cy - Y(12);
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
    {
        const char *a = mmss(n->pos_ms, b1), *z = n->len_ms ? mmss(n->len_ms, b2) : "";
        LONG aw = tlen(a, LF_SMALL), zw = tlen(z, LF_SMALL), bx = tx + aw + X(10);
        LONG bw = tx + tw - zw - X(10) - bx, by = y + FH / 2 - 2;
        text(tx, y, a, LF_SMALL, C_BODY, flat(C_PANEL), 0);
        if (bw > 8) {
            H->rrect((WORD)bx, (WORD)by, (WORD)bw, 4, 2, C_LINE, C_PANEL);
            if (n->len_ms) {
                LONG f = (LONG)((n->pos_ms / 100) * bw / (n->len_ms / 100 ? n->len_ms / 100 : 1));
                if (f > bw)
                    f = bw;
                if (f > 0)
                    H->rrect((WORD)bx, (WORD)by, (WORD)f, 4, 2, C_ACC, C_LINE);
            }
        }
        text(tx + tw - zw, y, z, LF_SMALL, C_BODY, flat(C_PANEL), 0);
    }
    y += FH + Y(8);
    line[0] = 0;
    if (n->nnext > 0)
        join(line, sizeof(line), H->tr("Next:"), " ", n->next[0]);
    join(line, sizeof(line), line, " \xB7 ", n->format);
    join(line, sizeof(line), line, " \xB7 ", n->source);
    if (y + FH <= HT)
        text(tx, y, line, LF_SMALL, C_NEXT, flat(C_PANEL), tw);
    if (n->paused)
        text(X(30), Y(412) - FH, H->tr("Paused"), LF_BOLD, C_WHITE, flat(C_NOWBG), W / 2);
}

/* ---------------------------------------------------------------- the engine */
static BOOL sc_open(const struct LayoutHost *host)
{
    H = host;
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

static void sc_changed(int screen, ULONG what)
{
    const struct LPage *m;
    if (what == LC_SELECT && screen == drawn && screen == LS_ALBUMS && (m = H->model(LS_ALBUMS))) {
        draw_wall_body(m);      /* a move on the wall: the wall alone */
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
        draw_text_body(m);      /* a text gliding: its panel alone */
        return;
    }
    if (screen == LS_PAGE && drawn == LS_PAGE && what == LC_SELECT && body_n >= 0 &&
        (m = H->model(LS_PAGE)) && !m->text && (m->ndetails > 0 || m->picture ? 1 : 0) == body_n) {
        H->clip(0, 0, W, HT);
        draw_page_body(m);      /* a move: the list and its details, not the whole screen */
        return;
    }
    sc_draw(screen);
}

/* Now playing's bars, ~12 a second (the rest is drawn when it changes) */
static void sc_tick(ULONG ms)
{
    if (drawn != LS_NOW || (LONG)(ms - bars_at) < 80)
        return;
    bars_at = ms;
    H->clip(0, 0, W, HT);
    draw_bars(H->model(LS_NOW));
}

/* The keys: the program's, as in the classic layout, but on the wall and
 * an artist's strip of albums Left and Right move along them, and on the
 * wall Up and Down a row */
static int sc_input(int screen, int action, LONG arg)
{
    const struct LPage *m;
    LONG sel, n, to;
    (void)arg;
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
        to = sel - wall_cols >= 0 ? sel - wall_cols : sel;
        break;
    case LA_DOWN:
        if (screen != LS_ALBUMS)
            return LR_UNUSED;
        to = sel + wall_cols < n ? sel + wall_cols
           : sel / wall_cols < (n - 1) / wall_cols ? n - 1 : sel;  /* (into a short last row) */
        break;
    default:
        return LR_UNUSED;
    }
    if (to != sel)
        H->act(LAC_SELECT, to);
    return LR_DONE;
}

/* What can be played (a double-click plays it; one click selects it), as
 * the classic layout has it; anything else opens with one click. */
static BOOL playable(int kind)
{
    return kind == LI_TRACK || kind == LI_MODULE || kind == LI_STATION || kind == LI_PICTURE ||
           kind == LI_VIDEO || kind == LI_GAME;
}

/* A click, as in the classic layout: a section goes in; a row or tile is
 * opened, or (what plays) selected, and played with a double-click; a
 * crumb goes back to that level; what plays (top right) opens Now
 * playing, and on Now playing the bars pause and play. */
static int sc_pointer(int screen, WORD x, WORD y, BOOL click)
{
    const struct Hit *t = NULL;
    int i;
    BOOL dbl;
    ULONG now;
    if (!click || !H)
        return LR_UNUSED;
    for (i = nhits - 1; i >= 0; i--)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) {
            t = &hits[i];
            break;
        }
    if (!t)
        return LR_UNUSED;
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
    }
    return LR_DONE;
}

const struct LayoutEngine layout_engine = {
    LAYOUT_ABI, "Showcase", 15, 480,
    LS_BIT(LS_HOME) | LS_BIT(LS_PAGE) | LS_BIT(LS_NOW) | LS_BIT(LS_ALBUMS) | LS_BIT(LS_ALBUM) |
        LS_BIT(LS_ARTIST),
    sc_open, sc_close, sc_draw, sc_changed, sc_tick, sc_input, sc_pointer
};
