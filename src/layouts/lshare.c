/*
 * Ambiance - what the layouts share, compiled into each one (lshare.h)
 * Copyright (c) 2026 Brian King
 *
 * Moved from Horizon and Showcase, where they were the same (20 Nov 2026).
 */
#include <stddef.h>
#include "lshare.h"

struct Hit hits[MAX_HITS];
int nhits;
WORD dcell[4];
const char *para[64];
char parabuf[2048];
const UBYTE pm_still[3] = { 66, 100, 50 };
UBYTE pm_lv[3] = { 66, 100, 50 };
LONG sb_cur, sb_page, sb_last;
ULONG split_key;
LONG tl_h[TL_N], tl_n = -1, tl_w, tl_total;
const void *tl_of;
struct SBar sbar[4];
int nsbar;
int sb_drag = -1;
LONG sb_off;
static struct LkBg bg_flat;    /* (flat's) */
LONG spin_x = -1, spin_y;
ULONG acc_ms;
int acc_dir, acc_run;

/* ---------------------------------------------------------------- little helpers */
LONG X(LONG v) { return v * W / 800; }

LONG Y(LONG v) { return v * HT / 600; }

int accel(int dir)
{
    ULONG now = H->now_ms();
    if (dir == acc_dir && now - acc_ms < 300)
        acc_run++;
    else
        acc_run = 0;
    acc_dir = dir;
    acc_ms = now;
    return acc_run;
}

/* ---------------------------------------------------------------- the frame of a screen */
void background(void)
{
    H->clip(0, 0, W, HT);
    H->vgrad(0, 0, W, HT, ls_top, ls_bottom);
}

BOOL ct_right(int b)
{
    return b == LNB_LOOP || b == LNB_MUTE || b == LNB_VDOWN || b == LNB_VUP || b == LNB_FULL;
}

WORD digit_cell(int face)
{
    if (face < 0 || face > 3)
        face = 0;
    if (!dcell[face]) {
        char d[2] = { '0', 0 };
        for (; d[0] <= '9'; d[0]++) {
            WORD w = H->textlen(d, 1, face);
            if (w > dcell[face])
                dcell[face] = w;
        }
    }
    return dcell[face];
}

int disc_angle(LONG dx, LONG dy)
{
    LONG ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, deg;
    if (!ax && !ay)
        return 0;
    deg = ax >= ay ? ay * 45 / ax : 90 - ax * 45 / ay;
    if (dx < 0 && dy >= 0)
        deg = 180 - deg;
    else if (dx < 0 && dy < 0)
        deg = 180 + deg;
    else if (dx >= 0 && dy < 0)
        deg = 360 - deg;
    return (int)((deg * 2 + 7) / 15) % 48;
}

/* The colour of a disc at an angle (48ths of a turn): silver with a
 * rainbow sheen (as Showcase's) */
ULONG disc_sector(int a)
{
    static const WORD c2[12] = { 256, 222, 128, 0, -128, -222, -256, -222, -128, 0, 128, 222 };
    LONG lit = (c2[a % 12] + 256) / 2, w = c2[(a + 9) % 12];
    ULONG silver = mix(0x8C96A2, 0xF2F5F8, lit, 256), hue;
    int h = (a * 2) % 12, k;
    ULONG r, g, b;
    if (w <= 0)
        return silver;
    k = h % 2 ? 128 : 0;
    switch (h / 2) {
    case 0: r = 255; g = k; b = 0; break;
    case 1: r = 255 - k; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = k; break;
    case 3: r = 0; g = 255 - k; b = 255; break;
    case 4: r = k; g = 0; b = 255; break;
    default: r = 255; g = 0; b = 255 - k; break;
    }
    hue = (r << 16) | (g << 8) | b;
    return mix(silver, hue, w * w / 256, 256 * 2);
}

const struct LkBg *flat(ULONG c)
{
    bg_flat.top = bg_flat.bottom = c;
    bg_flat.y = 0;
    bg_flat.h = 1;
    return &bg_flat;
}

LONG glide(struct Glide *g, LONG at, LONG to, ULONG ms)
{
    LONG p;
    if (to != g->to || ms - g->t0 > 4 * GLIDE_MS) {
        g->from = at;
        g->to = to;
        g->t0 = ms - 40;        /* (a tick's worth in already: it moves at once) */
    }
    p = (LONG)(ms - g->t0) * 256 / GLIDE_MS;
    if (p >= 256)
        return to;
    p = 256 - (256 - p) * (256 - p) / 256;
    return g->from + (to - g->from) * p / 256;
}

/* the gradient's colour at row y */
ULONG grad(LONG y)
{
    return mix(ls_top, ls_bottom, y, HT - 1);
}

void hit(LONG x, LONG y, LONG w, LONG h, int what, int kind, LONG arg)
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

char initial(const struct LList *l, LONG i)
{
    const struct LItem *it = l->item(i);
    const char *s = it->label;
    if (it->letter)
        return (char)it->letter;        /* (the host's: as the list is sorted) */
    if (!s)
        return 0;
    while (*s && !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')))
        s++;
    if (*s >= '0' && *s <= '9')
        return '#';
    return (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
}

/* s + sep + t into b (max bytes) */
const char *join(char *b, LONG max, const char *s, const char *sep, const char *t)
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

LONG letter_jump(const struct LList *l, LONG sel, int dir)
{
    LONG i = sel, n = l->count, lim = 2000;
    char c = initial(l, sel);
    if (dir > 0) {
        while (i < n - 1 && lim-- > 0 && initial(l, i) == c)
            i++;
        return i;
    }
    while (i > 0 && lim-- > 0 && initial(l, i - 1) == c)
        i--;
    if (i < sel)
        return i;                       /* (to this letter's first) */
    if (i > 0) {
        c = initial(l, --i);
        while (i > 0 && lim-- > 0 && initial(l, i - 1) == c)
            i--;
    }
    return i;
}

/* a row's mark, its right edge at x, centred on cy; its width */
LONG mark(int m, LONG x, LONG cy, ULONG fg, ULONG bg)
{
    LONG s = FH * 2 / 3, y = cy - s / 2, i;
    if (s < 6)
        s = 6;
    switch (m) {
    case LM_ARROW:
        for (i = 0; i < s / 2; i++) {
            H->fill((WORD)(x - s / 2 + i / 2), (WORD)(y + i), 2, 1, fg);
            H->fill((WORD)(x - s / 2 + i / 2), (WORD)(y + s - 1 - i), 2, 1, fg);
        }
        return s / 2 + 2;
    case LM_CHECK_ON: case LM_CHECK_OFF:
        H->frame((WORD)(x - s), (WORD)y, (WORD)s, (WORD)s, 1, fg);
        if (m == LM_CHECK_ON)
            H->fill((WORD)(x - s + 3), (WORD)(y + 3), (WORD)(s - 6), (WORD)(s - 6), ls_acc);
        return s;
    case LM_RADIO_ON: case LM_RADIO_OFF:
        H->rrect((WORD)(x - s), (WORD)y, (WORD)s, (WORD)s, (WORD)(s / 2), fg, bg);
        H->rrect((WORD)(x - s + 1), (WORD)(y + 1), (WORD)(s - 2), (WORD)(s - 2), (WORD)(s / 2 - 1), bg, fg);
        if (m == LM_RADIO_ON)
            H->rrect((WORD)(x - s + 4), (WORD)(y + 4), (WORD)(s - 8), (WORD)(s - 8), (WORD)(s / 2 - 4), ls_acc, bg);
        return s;
    case LM_PLAY: case LM_PLAYING:
        if (m == LM_PLAYING)
            return play_bars(x, y, s, pm_still);   /* (still: the top right's move) */
        for (i = 0; i < s; i++)
            H->fill((WORD)(x - s + i / 2), (WORD)(y + i / 2), 1, (WORD)(s - i), fg);
        return s;
    case LM_CYCLE:
        for (i = 0; i < s / 2; i++) {
            H->fill((WORD)(x - s * 3 / 2 + s / 4 - i / 2), (WORD)(y + i), 2, 1, fg);
            H->fill((WORD)(x - s * 3 / 2 + s / 4 - i / 2), (WORD)(y + s - 1 - i), 2, 1, fg);
            H->fill((WORD)(x - 2 - s / 4 + i / 2), (WORD)(y + i), 2, 1, fg);
            H->fill((WORD)(x - 2 - s / 4 + i / 2), (WORD)(y + s - 1 - i), 2, 1, fg);
        }
        return s * 3 / 2;
    }
    return 0;
}

/* "m:ss" */
char *mmss(ULONG ms, char *b)
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

/* A button's glyph, s tall, centred at cx, cy (as Showcase's) */
void nb_glyph(int b, BOOL playing, LONG cx, LONG cy, LONG s, ULONG c)
{
    LONG t = s / 6 > 2 ? s / 6 : 2, w = s * 4 / 5;
    switch (b) {
    case LNB_PLAY:
        if (playing) {
            H->fill((WORD)(cx - w / 2), (WORD)(cy - s / 2), (WORD)(w / 3), (WORD)s, c);
            H->fill((WORD)(cx + w / 2 - w / 3), (WORD)(cy - s / 2), (WORD)(w / 3), (WORD)s, c);
        } else {
            tri(cx - w / 2 + s / 10, cy, s, 1, c);
        }
        break;
    case LNB_STOP:
        H->fill((WORD)(cx - s * 2 / 5), (WORD)(cy - s * 2 / 5), (WORD)(s * 4 / 5), (WORD)(s * 4 / 5), c);
        break;
    case LNB_PREV:
        H->fill((WORD)(cx - w / 2), (WORD)(cy - s / 2), (WORD)t, (WORD)s, c);
        tri(cx + w / 2, cy, s, -1, c);
        break;
    case LNB_NEXT:
        tri(cx - w / 2, cy, s, 1, c);
        H->fill((WORD)(cx + w / 2 - t), (WORD)(cy - s / 2), (WORD)t, (WORD)s, c);
        break;
    case LNB_BACK:
        tri(cx, cy, s * 3 / 4, -1, c);
        tri(cx + s * 3 / 5, cy, s * 3 / 4, -1, c);
        break;
    case LNB_FWD:
        tri(cx - s * 3 / 5, cy, s * 3 / 4, 1, c);
        tri(cx, cy, s * 3 / 4, 1, c);
        break;
    case LNB_LOOP:
        H->frame((WORD)(cx - w / 2), (WORD)(cy - s / 3), (WORD)w, (WORD)(s * 2 / 3), t * 2 / 3 > 1 ? t * 2 / 3 : 1, c);
        tri(cx + w / 6, cy - s / 3, s / 2, 1, c);
        break;
    case LNB_MENU:              /* three lines */
        H->fill((WORD)(cx - w / 2), (WORD)(cy - s / 3 - t / 2), (WORD)w, (WORD)t, c);
        H->fill((WORD)(cx - w / 2), (WORD)(cy - t / 2), (WORD)w, (WORD)t, c);
        H->fill((WORD)(cx - w / 2), (WORD)(cy + s / 3 - t / 2), (WORD)w, (WORD)t, c);
        break;
    case LNB_FULL: {            /* four corners: out (to full screen), or in (playing: back) */
        LONG L = s / 3, k, a = s / 2, g2 = s / 8 + t / 2;
        for (k = 0; k < 4; k++) {
            LONG dx = k & 1 ? 1 : -1, dy = k & 2 ? 1 : -1, ex, ey, hx, vy;
            if (!playing) {     /* out: the corners' points outwards, arms inwards */
                ex = cx + dx * a;
                ey = cy + dy * a;
                dx = -dx;
                dy = -dy;
            } else {            /* in: their points by the middle, arms outwards */
                ex = cx + dx * g2;
                ey = cy + dy * g2;
            }
            hx = dx > 0 ? ex : ex - L + 1;
            vy = dy > 0 ? ey : ey - L + 1;
            H->fill((WORD)hx, (WORD)(dy > 0 ? ey : ey - t + 1), (WORD)L, (WORD)t, c);
            H->fill((WORD)(dx > 0 ? ex : ex - t + 1), (WORD)vy, (WORD)t, (WORD)L, c);
        }
        break;
    }
    case LNB_MUTE: case LNB_VDOWN: case LNB_VUP: {
        LONG gw = s * 6 / 5, bx = cx - gw / 2, a = s / 5 > 2 ? s / 5 : 2, cw = s * 2 / 5, j;
        LONG sx = bx + a + cw + s / 6, sw = bx + gw - sx;
        H->fill((WORD)bx, (WORD)(cy - s / 6), (WORD)a, (WORD)(s / 3 > 2 ? s / 3 : 2), c);
        for (j = 0; j < cw; j++) {
            LONG hh = s / 3 + (s - s / 3) * (j + 1) / cw;
            H->fill((WORD)(bx + a + j), (WORD)(cy - hh / 2), 1, (WORD)hh, c);
        }
        if (b == LNB_MUTE) {
            H->fill((WORD)sx, (WORD)(cy - s / 5), (WORD)t, (WORD)(s * 2 / 5), c);
            H->fill((WORD)(sx + t * 2), (WORD)(cy - s * 2 / 5), (WORD)t, (WORD)(s * 4 / 5), c);
        } else {
            H->fill((WORD)sx, (WORD)(cy - t / 2), (WORD)sw, (WORD)t, c);
            if (b == LNB_VUP)
                H->fill((WORD)(sx + (sw - t) / 2), (WORD)(cy - sw / 2), (WORD)t, (WORD)sw, c);
        }
        break;
    }
    }
}

/* A note, wrapped to w and each line centred in it, from y; its colour c
 * over bg */
void note_centred(const char *t, LONG x, LONG y, LONG w, ULONG c, const struct LkBg *bg)
{
    LONG len = slen(t), lh = FH + Y(4);
    while (len > 0) {
        LONG k = H->fit(t, len, LF_REGULAR, (WORD)w), j, tw;
        if (k <= 0)
            k = 1;
        if (k < len) {
            for (j = k; j > 0 && t[j] != ' '; j--)
                ;
            if (j > 0)
                k = j;
        }
        tw = H->textlen(t, k, LF_REGULAR);
        H->text((WORD)(x + (tw < w ? (w - tw) / 2 : 0)), (WORD)y, t, k, LF_REGULAR, c, bg, 0);
        y += lh;
        t += k;
        len -= k;
        while (len > 0 && *t == ' ') {
            t++;
            len--;
        }
    }
}

ULONG page_key(const struct LPage *m)
{
    ULONG k = (ULONG)m->list.count * 31 + m->section + (ULONG)m->ncrumb * 7;
    const char *s = m->ncrumb > 0 ? m->crumb[m->ncrumb - 1] : NULL;
    if (s)
        while (*s)
            k = k * 33 + (UBYTE)*s++;
    return k | 1;
}

int page_split(const struct LPage *m)
{
    ULONG k = page_key(m);
    if (m->ndetails > 0 || m->picture || m->aside) {
        split_key = k;
        return 1;
    }
    return k == split_key ? 1 : 0;
}

LONG paras(const char *t)
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

/* Three bars, their right edge at x, s tall from y, as high as lv says;
 * their width */
LONG play_bars(LONG x, LONG y, LONG s, const UBYTE *lv)
{
    LONG bw = s / 4 > 1 ? s / 4 : 2, k;
    for (k = 0; k < 3; k++) {
        LONG bh = s * lv[k] / 100;
        if (bh < 2)
            bh = 2;
        H->fill((WORD)(x - (3 - k) * bw - (2 - k) * 2), (WORD)(y + s - bh), (WORD)bw, (WORD)bh, ls_acc);
    }
    return 3 * bw + 4;
}

BOOL playable(int kind)
{
    return kind == LI_TRACK || kind == LI_MODULE || kind == LI_STATION || kind == LI_PICTURE ||
           kind == LI_VIDEO || kind == LI_GAME || kind == LI_DISC;
}

/* How far in from the side a rounded box's row j is (h rows, corners of
 * radius r), in 16ths of a pixel: each row measured at its middle */
LONG rinset16(LONG j, LONG h, LONG r)
{
    LONG dy2, v;
    if (j < r)
        dy2 = 2 * (r - j) - 1;
    else if (j >= h - r)
        dy2 = 2 * (j - (h - r)) + 1;
    else
        return 0;
    v = 256 * r * r - 64 * dy2 * dy2;
    return 16 * r - isqrt(v > 0 ? v : 0);
}

void rshape(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG c)
{
    LONG j;
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    for (j = 0; j < h; j++) {
        LONG in = (rinset16(j, h, r) + 8) / 16;
        if (w - 2 * in > 0)
            H->fill((WORD)(x + in), (WORD)(y + j), (WORD)(w - 2 * in), 1, c);
    }
}

void scopy(char *d, const char *s, LONG max)
{
    LONG i = 0;
    if (s)
        for (; s[i] && i < max - 1; i++)
            d[i] = s[i];
    d[i] = 0;
}

void scroll_page(int k, int dir)
{
    LONG to = sbar[k].cur + dir * sbar[k].page, last = sbar[k].last;
    if (last <= 0)
        return;
    if (to < 0)
        to = 0;
    if (to > last)
        to = last;
    H->act(LAC_SCROLLTO, (to * 65536 + last - 1) / last);
}

void scroll_units(LONG cur, LONG page, LONG last)
{
    sb_cur = cur;
    sb_page = page > 1 ? page : 1;
    sb_last = last;
}

ULONG shash(const char *s)
{
    ULONG h = 5381;
    if (s)
        for (; *s; s++)
            h = h * 33 + (UBYTE)*s;
    return h;
}

LONG slen(const char *s)
{
    LONG n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}

WORD text(LONG x, LONG y, const char *s, int face, ULONG fg, const struct LkBg *bg, LONG maxw)
{
    LONG n;
    if (!s || !*s || maxw < 0)
        return 0;
    n = slen(s);
    if (maxw > 0 && H->textlen(s, n, face) > maxw) {
        /* too long: cut where "..." still fits after it, and the "..." */
        LONG ew = H->textlen("...", 3, face), k = maxw > ew ? H->fit(s, n, face, (WORD)(maxw - ew)) : 0;
        WORD w;
        while (k > 0 && s[k - 1] == ' ')
            k--;
        w = k > 0 ? H->text((WORD)x, (WORD)y, s, k, face, fg, bg, 0) : 0;
        return (WORD)(w + H->text((WORD)(x + w), (WORD)y, "...", 3, face, fg, bg, 0));
    }
    return H->text((WORD)x, (WORD)y, s, n, face, fg, bg, (WORD)maxw);
}

/* Where it is drawn from for a view vh tall: its first line, and the y
 * (from the view's top) that line starts at */
LONG text_from(const struct LPage *m, LONG vh, LONG *dy)
{
    LONG off = tl_total > vh ? (LONG)((m->text_frac >> 4) * (ULONG)(tl_total - vh) / 4096) : 0, acc = 0, i = 0;
    while (i < tl_n && acc + tl_h[i] <= off)
        acc += tl_h[i++];
    *dy = -(off - acc);
    return i;
}

WORD tlen(const char *s, int face)
{
    return s && *s ? H->textlen(s, slen(s), face) : 0;
}

LONG tnum(LONG x, LONG y, const char *s, int face, ULONG fg, const struct LkBg *bg)
{
    LONG x0 = x, cell = digit_cell(face);
    if (!s)
        return 0;
    while (*s) {
        LONG k = 0;
        if (*s == '\a') {      /* (FIG_SPACE: a digit's room, empty) */
            x += cell;
            s++;
            continue;
        }
        if (*s >= '0' && *s <= '9') {
            LONG w = H->textlen(s, 1, face);
            H->text((WORD)(x + (cell - w) / 2), (WORD)y, s, 1, face, fg, bg, 0);
            x += cell;
            s++;
            continue;
        }
        while (s[k] && !(s[k] >= '0' && s[k] <= '9') && s[k] != '\a')
            k++;
        H->text((WORD)x, (WORD)y, s, k, face, fg, bg, 0);
        x += H->textlen(s, k, face);
        s += k;
    }
    return x - x0;
}

LONG tnum_len(const char *s, int face)
{
    LONG w = 0;
    if (!s)
        return 0;
    while (*s) {
        LONG k = 0;
        if ((*s >= '0' && *s <= '9') || *s == '\a') {
            w += digit_cell(face);
            s++;
            continue;
        }
        while (s[k] && !(s[k] >= '0' && s[k] <= '9') && s[k] != '\a')
            k++;
        w += H->textlen(s, k, face);
        s += k;
    }
    return w;
}

/* A triangle pointing right (dir 1) or left (-1), its flat side at x */
void tri(LONG x, LONG cy, LONG h, int dir, ULONG c)
{
    LONG i, w = h * 4 / 5;
    for (i = 0; i < h; i++) {
        LONG d = i < h / 2 ? i : h - 1 - i, len = w * (2 * d + 1) / h + 1;
        if (len > w)
            len = w;
        H->fill((WORD)(dir > 0 ? x : x - len), (WORD)(cy - h / 2 + i), (WORD)len, 1, c);
    }
}

/* Lines s takes wrapped to w */
LONG wrapped(const char *p, LONG w, int face)
{
    LONG len = slen(p), n = 0;
    while (len > 0) {
        LONG k = H->fit(p, len, face, (WORD)w), j;
        if (k <= 0)
            k = 1;
        if (k < len) {
            for (j = k; j > 0 && p[j] != ' '; j--)
                ;
            if (j > 0)
                k = j;
        }
        n++;
        p += k;
        len -= k;
        while (len > 0 && *p == ' ') {
            p++;
            len--;
        }
    }
    return n;
}

ULONG mix(ULONG a, ULONG b, LONG num, LONG den)
{
    ULONG o = 0;
    int sh;
    if (den <= 0)
        return a;
    if (num < 0)
        num = 0;
    if (num > den)
        num = den;
    for (sh = 0; sh <= 16; sh += 8) {
        LONG ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        o |= (ULONG)(ca + (cb - ca) * num / den) << sh;
    }
    return o;
}

LONG isqrt(LONG v)
{
    LONG r = 0, b = 1L << 30;
    if (v <= 0)
        return 0;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* The bars from what plays: the spectrum's low, middle and high thirds;
 * falling a little a step. TRUE if moved. */
BOOL pm_step(const struct LNow *n)
{
    UBYTE want[3];
    LONG k, i;
    BOOL moved = FALSE;
    if (!n->levels || !n->nlevels || n->paused || n->stopped) {
        want[0] = pm_still[0];
        want[1] = pm_still[1];
        want[2] = pm_still[2];
    } else {
        for (k = 0; k < 3; k++) {
            LONG a = k * n->nlevels / 3, b = (k + 1) * n->nlevels / 3, mx = 0;
            if (b <= a)
                b = a + 1;
            for (i = a; i < b && i < n->nlevels; i++)
                if (n->levels[i] > mx)
                    mx = n->levels[i];
            want[k] = (UBYTE)(15 + mx * 85 / 255);
        }
    }
    for (k = 0; k < 3; k++) {
        LONG v = want[k];
        if (v < pm_lv[k] - 15 && !(n->paused || n->stopped))
            v = pm_lv[k] - 15;
        if (v != pm_lv[k]) {
            pm_lv[k] = (UBYTE)v;
            moved = TRUE;
        }
    }
    return moved;
}

void text_measure(const struct LPage *m, LONG w, LONG lh)
{
    LONG i, n = m->ndetails < TL_N ? m->ndetails : TL_N;
    if ((const void *)m->details == tl_of && n == tl_n && w == tl_w)
        return;
    tl_of = (const void *)m->details;
    tl_n = n;
    tl_w = w;
    tl_total = 0;
    for (i = 0; i < n; i++) {
        const char *p = m->details[i];
        tl_h[i] = p && *p ? wrapped(p, w, LF_REGULAR) * lh : ls_blank_half ? lh / 2 : lh;  /* (as draw_lines) */
        tl_total += tl_h[i];
    }
}

void scroll_to(LONG p)
{
    LONG span, pos;
    if (sb_drag < 0)
        return;
    span = sbar[sb_drag].len - sbar[sb_drag].tl;
    pos = p - sb_off - sbar[sb_drag].at;
    if (pos < 0)
        pos = 0;
    if (pos > span)
        pos = span;
    H->act(LAC_SCROLLTO, span > 0 ? pos * 65536 / span : 0);
}

/* Lines of text from line from, each wrapped at w, down to bottom, over
 * the gradient; the first in first's colour (bold if it differs). The y
 * after them. */
LONG draw_lines(const char *const *s, LONG n, LONG from, LONG x, LONG y, LONG w, LONG bottom,
                       ULONG first, ULONG rest)
{
    LONG i, lh = FH + Y(4);
    for (i = from; i < n && y + lh <= bottom; i++) {
        const char *p = s[i];
        LONG len = slen(p);
        int face = i == 0 && first != rest ? LF_BOLD : LF_REGULAR;
        ULONG c = i == 0 ? first : rest;
        if (!len) {
            y += ls_blank_half ? lh / 2 : lh;
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
            H->text((WORD)x, (WORD)y, p, k, face, c, ls_lines_bg, 0);
            y += lh;
            p += k;
            len -= k;
            while (len > 0 && *p == ' ') {
                p++;
                len--;
            }
        }
    }
    return y;
}

/* A shiny disc (a DVD) s square at x, y over bg: its data side in silver
 * and rainbow by angle, a darker rim, the clear ring round the hub, the
 * hub's ring, and the hole; drawn a run of one colour at a time */
void disc(LONG x, LONG y, LONG s, ULONG bg)
{
    LONG R = s / 2 - 1, cx = x + s / 2, cy = y + s / 2, j, i;
    LONG hub = R * 40 / 100, clear = R * 30 / 100, hole = R * 12 / 100;
    ULONG sec[48];
    if (R < 4)
        return;
    for (i = 0; i < 48; i++)            /* the 24 colours, and halfway between them */
        sec[i] = i % 2 ? mix(disc_sector(i / 2), disc_sector((i / 2 + 1) % 24), 1, 2)
                       : disc_sector(i / 2);
    for (j = -R; j <= R; j++) {
        LONG half = isqrt(R * R - j * j), run = -half;
        ULONG cur = 0;
        BOOL have = FALSE;
        for (i = -half; i <= half + 1; i++) {
            ULONG c = bg;
            if (i <= half) {
                LONG d2 = i * i + j * j;
                if (d2 >= (R - 1) * (R - 1))
                    c = 0x9AA4AE;                       /* the rim */
                else if (d2 >= hub * hub)
                    c = sec[disc_angle(i, j)];          /* the data side */
                else if (d2 >= (hub - 1) * (hub - 1) || (d2 < (clear + 1) * (clear + 1) && d2 >= clear * clear))
                    c = 0x7C8692;                       /* the hub's rings */
                else if (d2 >= clear * clear)
                    c = mix(0xC9D0D8, bg, 1, 3);        /* the clear plastic */
                else if (d2 >= hole * hole)
                    c = 0xB4BCC6;                       /* the hub */
            }
            if (!have) {
                cur = c;
                run = i;
                have = TRUE;
            } else if (c != cur || i > half) {
                if (cur != bg)
                    H->fill((WORD)(cx + run), (WORD)(cy + j), (WORD)(i - run), 1, cur);
                cur = c;
                run = i;
            }
        }
    }
}

/* A time row at x, y, w wide (Now playing's, the viewer controls'): where
 * it is, the layout's bar (time_bar), and how long, in tabular digits over
 * ls_lines_bg. The bar's place comes back in *pbx, *pbw when a click on it
 * can seek (the length known); else *pbw is 0. */
void time_row(LONG x, LONG y, LONG w, ULONG at, ULONG total, BOOL knob_at_start, LONG *pbx, LONG *pbw)
{
    char b1[16], b2[16];
    const char *a = mmss(at, b1), *z = total ? mmss(total, b2) : "";
    LONG aw = tnum_len(a, LF_SMALL), zw = tnum_len(z, LF_SMALL), bx = x + aw + X(10);
    LONG bw = x + w - zw - X(10) - bx;
    *pbx = bx;
    *pbw = 0;
    tnum(x, y, a, LF_SMALL, ls_body, ls_lines_bg);
    if (bw > 8) {
        LONG f = -1;
        if (total) {
            f = (LONG)((at / 100) * bw / (total / 100 ? total / 100 : 1));
            if (f > bw)
                f = bw;
            *pbw = bw;
        }
        time_bar(bx, y, bw, f, knob_at_start);
    }
    if (*z)
        tnum(x + w - zw, y, z, LF_SMALL, ls_body, ls_lines_bg);
}

/* A soft glow round a rounded box (what is chosen, the pointer's hover):
 * rings a pixel each, the layout's ls_glow fading into bg outwards over
 * ls_glow_w design units (ls_glow_min pixels at least); the box itself is
 * then drawn over its middle */
void glow(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG bg)
{
    LONG k, n = X(ls_glow_w) > ls_glow_min ? X(ls_glow_w) : ls_glow_min;
    for (k = n; k >= 1; k--)
        rshape(x - k, y - k, w + 2 * k, h + 2 * k, r + k, mix(ls_glow, bg, k, n + 1));
}

/* Waiting for a list's rows: a small spinner, eight dots in a ring at
 * spin_x, spin_y (the one at the step lit, the ones before it fading:
 * ls_spin), over the layout's spin_back, each dot its spin_dot */
void wait_spinner(const struct LPage *m)
{
    static const signed char dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const signed char dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    LONG d = FH / 4 > 3 ? FH / 4 : 3, r = FH / 2, i;
    if (spin_x < 0)
        return;
    spin_back(spin_x - r - d, spin_y - r - d, 2 * (r + d) + 1);
    for (i = 0; i < 8; i++) {
        int back = (m->wait_phase - i) & 7;
        LONG cx = spin_x + dx[i] * r / 10, cy = spin_y + dy[i] * r / 10;
        spin_dot(cx - d / 2, cy - d / 2, d, ls_spin[back < 3 ? back : 3]);
    }
}
