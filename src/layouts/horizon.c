/*
 * Ambiance - Horizon, the second layout (layout.h)
 * Copyright (c) 2026 Brian King
 *
 * After the Media Center mockups (claude/kodi-client-nfo.md §4): a deep
 * blue gradient, light lowercase words, a soft blue glow on what is
 * chosen, no boxes. Home is a strip: the sections one above the other,
 * the chosen one's rows running across to the right on a line of light
 * (the horizon). Walls of covers scroll sideways; pictures stand on their
 * reflections; what plays sits in an inset at the bottom right.
 *
 * Laid out on an 800 x 600 design, scaled to the screen, as Showcase is;
 * its keys, clicks, hover, toasts and scroll bars work as Showcase's do.
 */
#include <stddef.h>
#include <exec/types.h>
#include "../layout.h"

const char LibName[] = "horizon.layout";
const char LibIdString[] = "horizon.layout 1.0 (18.11.2026) Horizon, for Ambiance";
static const char __attribute__((used)) LibVerTag[] =
    "$VER: horizon.layout 1.0 (18.11.2026) Horizon, for Ambiance";
const UWORD LibVersion = 1;
const UWORD LibRevision = 0;

/* the colours */
#define C_TOP    0x0B3270UL     /* the gradient: a deep blue */
#define C_BOT    0x03122DUL
#define C_FOCUS  0x1C67C4UL     /* what is chosen: a blue pill */
#define C_FOCUS2 0x3E8FE6UL     /* ...its lit top edge */
#define C_ACC    0x63C9FFUL     /* the glow, and what is lit */
#define C_HOVER  0x174A8AUL     /* under the pointer: a quieter pill */
#define C_TILE   0x0F3B78UL     /* a tile with no picture */
#define C_TILE2  0x1A4C8FUL
#define C_INSET  0x072452UL     /* what plays, at the bottom right */
#define C_LINE   0x2A5A99UL
#define C_SUB    0xA8C4EAUL
#define C_DIM    0x5F80B0UL
#define C_FAINT  0x34558AUL
#define C_TEXT   0xEEF5FFUL
#define C_BODY   0xD6E4F7UL
#define C_WHITE  0xFFFFFFUL
#define C_COVER  0x8A4B2AUL
#define C_LETTER 0x2B5E9EUL     /* the big letter: faint, over the list */
#define C_BTN    0x15488DUL     /* Now playing's buttons */

static const struct LayoutHost *H;
static WORD W, HT, FH, TH;      /* the screen; a line of text; a title's (LF_TITLE) */
static int drawn = -1;          /* the screen drawn last */

/* What a click lands on, noted as it is drawn */
enum { HT_STRIP, HT_SECTION, HT_TILE, HT_ROW, HT_CRUMB, HT_NOWPLAYING, HT_PAUSE, HT_HOME, HT_BUTTON, HT_SEEK, HT_SCROLL };
struct Hit {
    WORD x, y, w, h;
    UBYTE what;                 /* HT_* */
    UBYTE kind;                 /* a row's or tile's LI_* */
    LONG arg;                   /* its index (a crumb: its level) */
};
#define MAX_HITS 80
static struct Hit hits[MAX_HITS];
static int nhits, hits_base;
static LONG hit_sel = -1;       /* the list's selected row, as drawn */
static LONG click_row = -1;     /* a double-click: the row clicked last, and when */
static ULONG click_ms;

/* What the pointer is over (HT_*, its arg), drawn as a quieter pill or a
 * glow; -1 none (the keys in use) */
static int hov_what = -1;
static LONG hov_arg = -1;
#define HOVER(w, a) (hov_what == (w) && hov_arg == (a))

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

static WORD tlen(const char *s, int face)
{
    return s && *s ? H->textlen(s, slen(s), face) : 0;
}

/* The style's words are lowercase ("music", "now playing"): a copy with
 * A-Z lowered (other letters as they are: a catalogue's may be more than
 * a byte) */
static char lowbuf[4][160];
static int lown;
static const char *lower(const char *s)
{
    char *b = lowbuf[lown++ & 3];
    LONG i = 0;
    if (!s)
        return NULL;
    for (; s[i] && i < (LONG)sizeof(lowbuf[0]) - 1; i++)
        b[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    b[i] = 0;
    return b;
}

/* Numbers that change as they are shown (a time, the clock): each digit
 * in a cell the widest digit's width, so nothing beside them moves */
static WORD dcell[4];
static WORD digit_cell(int face)
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
static LONG tnum_len(const char *s, int face)
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
static LONG tnum(LONG x, LONG y, const char *s, int face, ULONG fg, const struct LkBg *bg)
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

static void scopy(char *d, const char *s, LONG max)
{
    LONG i = 0;
    if (s)
        for (; s[i] && i < max - 1; i++)
            d[i] = s[i];
    d[i] = 0;
}
static ULONG shash(const char *s)
{
    ULONG h = 5381;
    if (s)
        for (; *s; s++)
            h = h * 33 + (UBYTE)*s;
    return h;
}

static LONG isqrt(LONG v)
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

/* ---------------------------------------------------------------- shapes */
/* A rounded box filled a row at a time, nothing outside it touched */
/* How far in from the side a rounded box's row j is (h rows, corners of
 * radius r), in 16ths of a pixel: each row measured at its middle */
static LONG rinset16(LONG j, LONG h, LONG r)
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

static void rshape(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG c)
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

/* A soft glow round a rounded box: rings a pixel each, the accent fading
 * into bg outwards (wider than Showcase's: this style's selection is its
 * glow) */
#define LABEL_GAP (X(7) + 5)    /* words under a tile: below its glow (drawn opaque, they cut it) */
static void glow(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG bg)
{
    LONG k, n = X(7) > 4 ? X(7) : 4;
    for (k = n; k >= 1; k--)
        rshape(x - k, y - k, w + 2 * k, h + 2 * k, r + k, mix(mix(C_ACC, C_FOCUS, 1, 2), bg, k, n + 1));
}

/* A rounded box shaded from top to bottom, a row at a time */
static void rgrad(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG top, ULONG bot)
{
    LONG j;
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    for (j = 0; j < h; j++) {
        LONG in = (rinset16(j, h, r) + 8) / 16;
        if (w - 2 * in > 0)
            H->fill((WORD)(x + in), (WORD)(y + j), (WORD)(w - 2 * in), 1, mix(top, bot, j, h - 1));
    }
}

/* A rounded box shaded top to bottom with smooth edges: each row's end
 * pixels blended with what is outside it (out; ~0: the gradient) by how
 * much of them it covers */
#define OUT_GRAD 0xFFFFFFFFUL
static void rsmooth(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG top, ULONG bot, ULONG out)
{
    LONG j;
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    for (j = 0; j < h; j++) {
        LONG f = rinset16(j, h, r), full = (f + 15) / 16, cov = full * 16 - f;
        ULONG c = mix(top, bot, j, h - 1), o = out == OUT_GRAD ? grad(y + j) : out;
        if (w - 2 * full > 0)
            H->fill((WORD)(x + full), (WORD)(y + j), (WORD)(w - 2 * full), 1, c);
        if (cov > 0 && full > 0 && w - 2 * full + 2 > 0) {
            ULONG e = mix(o, c, cov, 16);
            H->fill((WORD)(x + full - 1), (WORD)(y + j), 1, 1, e);
            H->fill((WORD)(x + w - full), (WORD)(y + j), 1, 1, e);
        }
    }
}

/* What is chosen: a blue pill in its glow, lighter at its top (glassy),
 * its top edge lit; hovered: a quieter pill, no glow. pill_bg: what the
 * words on the pill last drawn stand on. */
static struct LkBg pill_bg;
static void pill(LONG x, LONG y, LONG w, LONG h, BOOL chosen, ULONG bg)
{
    LONG r = h / 4 > 3 ? h / 4 : 3;
    if (chosen) {
        ULONG top = mix(C_FOCUS2, C_FOCUS, 1, 3);
        glow(x, y, w, h, r, bg);
        rgrad(x, y, w, h, r, top, C_FOCUS);
        H->fill((WORD)(x + r), (WORD)y, (WORD)(w - 2 * r), 1, mix(C_ACC, C_FOCUS2, 1, 2));
        pill_bg.top = top;
        pill_bg.bottom = C_FOCUS;
        pill_bg.y = y;
        pill_bg.h = h;
        pill_bg.rows_flat = 1;          /* (rgrad: a colour a row, no dither) */
    } else {
        rshape(x, y, w, h, r, C_HOVER);
        pill_bg.top = pill_bg.bottom = C_HOVER;
        pill_bg.y = 0;
        pill_bg.h = 1;
    }
}

/* The horizon: a line of light, brightest in its middle, fading to bg at
 * both ends */
static void horizon_line(LONG x, LONG y, LONG w)
{
    ULONG mid = mix(C_ACC, grad(y), 1, 3);
    H->hgrad((WORD)x, (WORD)y, (WORD)(w / 2), 1, grad(y), mid);
    H->hgrad((WORD)(x + w / 2), (WORD)y, (WORD)(w - w / 2), 1, mid, grad(y));
    H->hgrad((WORD)(x + w / 6), (WORD)(y + 1), (WORD)(w / 3), 1, grad(y + 1), mix(mid, grad(y + 1), 1, 2));
    H->hgrad((WORD)(x + w / 2), (WORD)(y + 1), (WORD)(w / 3), 1, mix(mid, grad(y + 1), 1, 2), grad(y + 1));
}

/* A triangle pointing right (dir 1) or left (-1), its flat side at x */
static void tri(LONG x, LONG cy, LONG h, int dir, ULONG c)
{
    LONG i, w = h * 4 / 5;
    for (i = 0; i < h; i++) {
        LONG d = i < h / 2 ? i : h - 1 - i, len = w * (2 * d + 1) / h + 1;
        if (len > w)
            len = w;
        H->fill((WORD)(dir > 0 ? x : x - len), (WORD)(cy - h / 2 + i), (WORD)len, 1, c);
    }
}

/* ---------------------------------------------------------------- pictures */
/* A picture's reflection under it: its bottom rows upside down, fading
 * into the background (bg at its top, bg2 at its bottom), h tall at most.
 * Made in a buffer of its own, a row of the picture at a time. */
#define REFL_W 480
#define REFL_H 96
static ULONG refl_px[REFL_W * REFL_H];
static void reflect(const struct LImage *im, LONG x, LONG y, LONG h)
{
    struct LImage r;
    LONG i, j, w = im->w;
    if (h > REFL_H)
        h = REFL_H;
    if (h > im->h)
        h = im->h;
    if (w > REFL_W)
        w = REFL_W;
    if (h <= 0 || w <= 0)
        return;
    for (j = 0; j < h; j++) {
        const ULONG *src = im->px + (ULONG)(im->h - 1 - j) * im->mod;
        ULONG bg = grad(y + j);
        LONG a = 90 - 90 * j / h;       /* (out of 256: about a third at its top) */
        for (i = 0; i < w; i++)
            refl_px[j * REFL_W + i] = mix(bg, src[i] & 0xFFFFFFUL, a, 256);
    }
    r.w = (UWORD)w;
    r.h = (UWORD)h;
    r.mod = REFL_W;
    r.px = refl_px;
    H->image(&r, (WORD)x, (WORD)y);
}

/* The colour of a disc at an angle (48ths of a turn): silver with a
 * rainbow sheen (as Showcase's) */
static ULONG disc_sector(int a)
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
static int disc_angle(LONG dx, LONG dy)
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
/* A DVD: a shiny disc s square at x, y over bg */
static void disc(LONG x, LONG y, LONG s, ULONG bg)
{
    LONG R = s / 2 - 1, cx = x + s / 2, cy = y + s / 2, j, i;
    LONG hub = R * 40 / 100, clear = R * 30 / 100, hole = R * 12 / 100;
    ULONG sec[48];
    if (R < 4)
        return;
    for (i = 0; i < 48; i++)
        sec[i] = i % 2 ? mix(disc_sector(i / 2), disc_sector((i / 2 + 1) % 24), 1, 2) : disc_sector(i / 2);
    for (j = -R; j <= R; j++) {
        LONG half = isqrt(R * R - j * j), run = -half;
        ULONG cur = 0;
        BOOL have = FALSE;
        for (i = -half; i <= half + 1; i++) {
            ULONG c = bg;
            if (i <= half) {
                LONG d2 = i * i + j * j;
                if (d2 >= (R - 1) * (R - 1))
                    c = 0x9AA4AE;
                else if (d2 >= hub * hub)
                    c = sec[disc_angle(i, j)];
                else if (d2 >= (hub - 1) * (hub - 1) || (d2 < (clear + 1) * (clear + 1) && d2 >= clear * clear))
                    c = 0x7C8692;
                else if (d2 >= clear * clear)
                    c = mix(0xC9D0D8, bg, 1, 3);
                else if (d2 >= hole * hole)
                    c = 0xB4BCC6;
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

/* A cover w x h at x, y (fitted, the rest left as it is); none yet: a
 * tile with the name's first letter. refl: its reflection under it, that
 * tall. The picture drawn, or NULL. */
static const struct LImage *cover_at(const char *art, const char *label, LONG x, LONG y, LONG w, LONG h,
                                     LONG refl)
{
    const struct LImage *im = art ? H->picture(art, (UWORD)w, (UWORD)h) : NULL;
    if (im) {
        LONG ix = x + (w - im->w) / 2, iy = y + h - im->h;     /* (standing on its line) */
        if (im->w < w || im->h < h)
            H->vgrad((WORD)x, (WORD)y, (WORD)w, (WORD)h, grad(y), grad(y + h));
        H->image(im, (WORD)ix, (WORD)iy);
        if (refl > 0)
            reflect(im, ix, y + h + 1, refl);
        return im;
    }
    H->vgrad((WORD)x, (WORD)y, (WORD)w, (WORD)h, C_TILE2, C_TILE);
    if (label && *label)
        H->text((WORD)(x + (w - H->textlen(label, 1, LF_TITLE)) / 2), (WORD)(y + (h - TH) / 2), label, 1,
                LF_TITLE, C_DIM, flat(mix(C_TILE2, C_TILE, 1, 2)), 0);
    if (refl > 0)
        H->vgrad((WORD)x, (WORD)(y + h + 1), (WORD)w, (WORD)(refl / 2), mix(C_TILE, grad(y + h), 2, 3),
                 grad(y + h + refl / 2));
    return NULL;
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

/* Lines of text from line from, each wrapped at w, down to bottom, over
 * the gradient; the first in first's colour (bold if it differs). The y
 * after them. */
static LONG draw_lines(const char *const *s, LONG n, LONG from, LONG x, LONG y, LONG w, LONG bottom,
                       ULONG first, ULONG rest)
{
    LONG i, lh = FH + Y(4);
    for (i = from; i < n && y + lh <= bottom; i++) {
        const char *p = s[i];
        LONG len = slen(p);
        int face = i == 0 && first != rest ? LF_BOLD : LF_REGULAR;
        ULONG c = i == 0 ? first : rest;
        if (!len) {
            y += lh / 2;
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
            H->text((WORD)x, (WORD)y, p, k, face, c, &bg_grad, 0);
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

/* Lines s takes wrapped to w */
static LONG wrapped(const char *p, LONG w, int face)
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

/* The playing mark's bars, percent of its height: low, middle, high (a
 * row's: still; the top right's: moved in the ticks as the sound goes) */
static const UBYTE pm_still[3] = { 66, 100, 50 };
static UBYTE pm_lv[3] = { 66, 100, 50 };

/* Three bars, their right edge at x, s tall from y, as high as lv says;
 * their width */
static LONG play_bars(LONG x, LONG y, LONG s, const UBYTE *lv)
{
    LONG bw = s / 4 > 1 ? s / 4 : 2, k;
    for (k = 0; k < 3; k++) {
        LONG bh = s * lv[k] / 100;
        if (bh < 2)
            bh = 2;
        H->fill((WORD)(x - (3 - k) * bw - (2 - k) * 2), (WORD)(y + s - bh), (WORD)bw, (WORD)bh, C_ACC);
    }
    return 3 * bw + 4;
}

/* a row's mark, its right edge at x, centred on cy; its width */
static LONG mark(int m, LONG x, LONG cy, ULONG fg, ULONG bg)
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
            H->fill((WORD)(x - s + 3), (WORD)(y + 3), (WORD)(s - 6), (WORD)(s - 6), C_ACC);
        return s;
    case LM_RADIO_ON: case LM_RADIO_OFF:
        H->rrect((WORD)(x - s), (WORD)y, (WORD)s, (WORD)s, (WORD)(s / 2), fg, bg);
        H->rrect((WORD)(x - s + 1), (WORD)(y + 1), (WORD)(s - 2), (WORD)(s - 2), (WORD)(s / 2 - 1), bg, fg);
        if (m == LM_RADIO_ON)
            H->rrect((WORD)(x - s + 4), (WORD)(y + 4), (WORD)(s - 8), (WORD)(s - 8), (WORD)(s / 2 - 4), C_ACC, bg);
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

/* ---------------------------------------------------------------- scroll bars */
/* As Showcase's: a click target wider than they look; pressed on the
 * thumb, it is taken and the list follows (LAC_SCROLLTO) until the button
 * is let go; pressed above or below it (left or right), a page that way.
 * Across (a wall that scrolls sideways) or down. Each knows where its
 * list is (cur: the row chosen, or a text's first line), a page of it,
 * and its last place (LAC_SCROLLTO's whole way). */
static struct { LONG at, len, t, tl; BOOL across; LONG cur, page, last; } sbar[4];
static LONG sb_cur, sb_page, sb_last;   /* the next bar's: set before drawing it */
static void scroll_units(LONG cur, LONG page, LONG last)
{
    sb_cur = cur;
    sb_page = page > 1 ? page : 1;
    sb_last = last;
}
static int nsbar;
static int sb_drag = -1;
static LONG sb_off;
static void scroll_bar(LONG bx, LONG by, LONG blen, LONG t, LONG tl, BOOL across)
{
    int k = nsbar++ & 3;
    BOOL drag = sb_drag == k;
    sbar[k].at = across ? bx : by;
    sbar[k].len = blen;
    sbar[k].t = t;
    sbar[k].tl = tl;
    sbar[k].across = across;
    sbar[k].cur = sb_cur;
    sbar[k].page = sb_page;
    sbar[k].last = sb_last;
    if (across) {
        H->fill((WORD)bx, (WORD)(by + 1), (WORD)blen, 1, C_FAINT);
        rshape(t, by - (drag ? 1 : 0), tl, drag ? 5 : 3, 1, drag ? C_WHITE : C_ACC);
        hit(bx, by - Y(8), blen, 3 + Y(16), HT_SCROLL, 0, k);
    } else {
        H->fill((WORD)(bx + 1), (WORD)by, 1, (WORD)blen, C_FAINT);
        rshape(bx - (drag ? 1 : 0), t, drag ? 5 : 3, tl, 1, drag ? C_WHITE : C_ACC);
        hit(bx - X(8), by, 3 + X(16), blen, HT_SCROLL, 0, k);
    }
}
static void scroll_page(int k, int dir)
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
static void scroll_to(LONG p)
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

/* ---------------------------------------------------------------- the frame of a screen */
static void background(void)
{
    H->clip(0, 0, W, HT);
    H->vgrad(0, 0, W, HT, C_TOP, C_BOT);
}

/* The background again where x0..x1, y0..y1: the screen's one gradient,
 * clipped there, so it is as it was drawn whole (a part's own gradient
 * rounds differently) */
static void back_at(LONG x0, LONG y0, LONG x1, LONG y1)
{
    H->clip((WORD)x0, (WORD)y0, (WORD)x1, (WORD)y1);
    H->vgrad(0, 0, W, HT, C_TOP, C_BOT);
    H->clip(0, 0, W, HT);
}

/* Top left: the logo (Home, clicked); its right edge */
#define LOGO_X X(24)
#define LOGO_Y Y(14)
static LONG logo_r, logo_h;
static LONG TITLE_Y, BODY_Y;    /* a page's title, and what is under it: under the logo */
static void top_left(void)
{
    const struct LImage *logo = H->logo ? H->logo(grad(LOGO_Y + Y(10))) : NULL;
    logo_r = LOGO_X;
    logo_h = FH;
    if (logo && logo->w < W / 3) {
        H->image(logo, (WORD)LOGO_X, (WORD)LOGO_Y);
        hit(LOGO_X - X(4), LOGO_Y - Y(4), logo->w + X(8), logo->h + Y(8), HT_HOME, 0, 0);
        logo_r = LOGO_X + logo->w;
        logo_h = logo->h;
    }
    TITLE_Y = LOGO_Y + logo_h + Y(4);
    if (TITLE_Y < Y(48))
        TITLE_Y = Y(48);
    BODY_Y = TITLE_Y + TH + Y(18);
}

/* Top right: what plays, if anything (its bars moving with the sound,
 * its title; clicked: Now playing), then the clock, light (and the Debug
 * line under it). Not on Now playing itself. */
static LONG tr_left, dbg_left;
static LONG pb_x = -1, pb_y, pb_s;  /* the bars' place, as drawn (-1: none) */
static void top_right_as(BOOL with_hit)
{
    const struct LNow *n = H->model(LS_NOW);
    LONG cw = tnum_len(H->clock, LF_REGULAR), x = W - X(28) - cw, y = LOGO_Y + logo_h / 2 - FH / 2;
    tnum(x, y, H->clock, LF_REGULAR, C_SUB, &bg_grad);
    tr_left = dbg_left = x;
    pb_x = -1;
    if (drawn != LS_NOW && n && n->kind != LN_NONE && !n->stopped && n->title && *n->title) {
        LONG s = FH * 2 / 3, bw = 3 * (s / 4 > 1 ? s / 4 : 2) + 4, right = x - X(16);
        LONG room = right - bw - X(10) - (W / 2 + X(20)), tw = tlen(n->title, LF_SMALL);
        if (tw > room)
            tw = room;
        if (tw < X(70))
            tw = 0;                 /* (only a few letters of it would fit: the bars alone) */
        if (tw > 0) {
            text(right - tw, y, n->title, LF_SMALL, C_SUB, &bg_grad, tw);
            right -= tw + X(10);
        }
        pb_x = right - bw;
        pb_y = y + (FH - s) / 2;
        pb_s = s;
        play_bars(right, pb_y, s, pm_lv);
        if (with_hit)
            hit(pb_x - X(6), y - Y(4), x - X(8) - (pb_x - X(6)), FH + Y(8), HT_NOWPLAYING, 0, 0);
        tr_left = pb_x;
    }
    if (H->debug && *H->debug) {
        dbg_left = W - X(28) - tnum_len(H->debug, LF_SMALL);
        if (TITLE_Y > y + FH + Y(2) + FH)
            dbg_left = W;       /* (the line ends above the title) */
        tnum(W - X(28) - tnum_len(H->debug, LF_SMALL), y + FH + Y(2), H->debug, LF_SMALL, C_ACC, &bg_grad);
    }
}

static void top_right(void)
{
    top_right_as(TRUE);
}

/* The clock again (a minute on), over what was there */
static void top_right_again(void)
{
    LONG y = LOGO_Y + logo_h / 2 - FH / 2, x0 = tr_left - X(8), h = FH + 4;
    if (H->debug && *H->debug) {
        LONG dl = W - X(28) - tnum_len(H->debug, LF_SMALL) - X(8);
        if (dl < x0)
            x0 = dl;
        h += FH + Y(2);         /* (the Debug line under it, as it changes) */
    }
    back_at(x0, y - 2, W, y - 2 + h);
    top_right_as(FALSE);
}

/* What plays, in an inset at the bottom right of the browsing screens
 * (clicked: Now playing); its top, HT where none */
#define INSET_W X(260)
#define INSET_H Y(62)
static BOOL inset_off;          /* Home's row has what plays as a tile of its own: no inset */
static LONG inset_top(void)
{
    const struct LNow *n = H->model(LS_NOW);
    if (inset_off)
        return HT;
    return n && n->kind != LN_NONE ? HT - Y(16) - INSET_H : HT;
}
static void now_inset_as(BOOL with_hit);
static void now_inset(void)
{
    now_inset_as(TRUE);
}
static void now_inset_as(BOOL with_hit)
{
    if (inset_off)
        return;
    const struct LNow *n = H->model(LS_NOW);
    LONG w = INSET_W, h = INSET_H, x = W - X(24) - w, y = HT - Y(16) - h, s = h - Y(12), tx, tw;
    char b[16];
    if (!n || n->kind == LN_NONE)
        return;
    if (HOVER(HT_NOWPLAYING, 0))
        glow(x, y, w, h, Y(8), grad(y));
    rshape(x, y, w, h, Y(8), C_INSET);
    H->fill((WORD)(x + Y(8)), (WORD)y, (WORD)(w - 2 * Y(8)), 1, C_LINE);
    if (with_hit)
        hit(x, y, w, h, HT_NOWPLAYING, 0, 0);
    {
        const struct LImage *im = n->art ? H->picture(n->art, (UWORD)s, (UWORD)s) : NULL;
        if (im)
            H->image(im, (WORD)(x + Y(6) + (s - im->w) / 2), (WORD)(y + Y(6) + (s - im->h) / 2));
        else
            H->vgrad((WORD)(x + Y(6)), (WORD)(y + Y(6)), (WORD)s, (WORD)s, C_TILE2, C_TILE);
    }
    tx = x + Y(6) + s + X(10);
    tw = x + w - X(10) - tx;
    text(tx, y + Y(6), n->title, LF_BOLD, C_WHITE, flat(C_INSET), tw);
    text(tx, y + Y(6) + FH, n->artist && *n->artist ? n->artist : n->source, LF_SMALL, C_SUB, flat(C_INSET), tw);
    {
        const char *t = n->stopped ? lower(H->tr("Stopped")) : n->paused ? lower(H->tr("Paused"))
                      : mmss(n->pos_ms, b);
        if (n->stopped || n->paused)
            text(tx, y + Y(6) + FH * 2, t, LF_SMALL, C_ACC, flat(C_INSET), tw);
        else
            tnum(tx, y + Y(6) + FH * 2, t, LF_SMALL, C_ACC, flat(C_INSET));
    }
}

/* The page's way here, small and lowercase, after the logo (each step a
 * click back there), and its title, large and lowercase, under it */
static LONG crumbs_line(const struct LPage *m, LONG upto)
{
    LONG x = logo_r + X(20), y = LOGO_Y + logo_h / 2 - FH / 2, i, w;
    LONG lim = tr_left - X(24);
    for (i = 0; i < upto && lim - x > X(30); i++) {
        if (i)
            x += text(x, y, "\xBB", LF_SMALL, C_DIM, &bg_grad, 0) + X(6);   /* (a guillemet) */
        w = text(x, y, lower(m->crumb[i]), LF_SMALL, C_SUB, &bg_grad, lim - x);
        hit(x - X(2), y - Y(4), w + X(4), FH + Y(8), HT_CRUMB, 0, i);
        x += w + X(6);
    }
    return x;
}
static LONG title_end;          /* the title's right edge, as drawn */
static void page_head(const struct LPage *m, BOOL named)
{
    top_left();
    top_right();
    if (!m)
        return;
    crumbs_line(m, named ? m->ncrumb - 1 : m->ncrumb - 1);
    title_end = X(30);
    if (named && m->ncrumb)
        title_end += text(X(30), TITLE_Y, lower(m->crumb[m->ncrumb - 1]), LF_TITLE, C_TEXT, &bg_grad,
                          dbg_left - X(30) - X(20));
}

/* ---------------------------------------------------------------- the band: toasts and the keys' hint */
#define TOAST_MS 4000
#define HINT_MS  4000
static char ft_msg[160], ft_hint[160];
static ULONG ft_hash, ft_at, ft_key;
static BOOL ft_keep;
static int ft_shown;
static ULONG ft_shown_hash;
static BOOL ft_screen;
static void redraw_under_band(void);

static LONG band_y(void)
{
    return HT - Y(16) - (FH + Y(14));
}
static void footer_paint(int want)
{
    LONG right = inset_top() < HT ? W - X(24) - INSET_W - X(16) : W - X(24);
    LONG room = right - X(24), cx = X(24) + room / 2;
    if (want == 1) {
        LONG tw = tlen(ft_msg, LF_REGULAR), h = FH + Y(14), w = tw + X(36), y = band_y();
        if (w > room) {
            w = room;
            tw = w - X(36);
        }
        glow(cx - w / 2, y, w, h, h / 2, grad(y));
        rshape(cx - w / 2, y, w, h, h / 2, C_FOCUS);
        text(cx - w / 2 + X(18), y + Y(7), ft_msg, LF_REGULAR, C_WHITE, flat(C_FOCUS), tw);
    } else if (want == 2) {
        LONG tw = tlen(ft_hint, LF_SMALL), h = FH + Y(10), w = tw + X(28), y = band_y() + Y(4);
        if (w > room) {
            w = room;
            tw = w - X(28);
        }
        rshape(cx - w / 2, y, w, h, h / 2, C_INSET);
        text(cx - w / 2 + X(14), y + Y(5), ft_hint, LF_SMALL, C_SUB, flat(C_INSET), tw);
    }
    ft_shown = want;
    ft_shown_hash = ft_hash;
}
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
    if (ft_shown || force)
        redraw_under_band();
    footer_paint(want);
}
static void footer_keep(const char *hint, const char *message, BOOL keep)
{
    ULONG h = shash(message);
    if (message && *message && (h != ft_hash || !ft_msg[0]))
        ft_at = H->now_ms();
    ft_hash = message && *message ? h : 0;
    scopy(ft_msg, message, sizeof(ft_msg));
    scopy(ft_hint, hint, sizeof(ft_hint));
    ft_keep = keep;
    ft_screen = TRUE;
    ft_shown = 0;
    footer_paint(0);
    footer_draw(FALSE);
}

/* ---------------------------------------------------------------- the waiting spinner */
static LONG spin_x = -1, spin_y;
static void wait_spinner(const struct LPage *m)
{
    static const signed char dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const signed char dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    LONG d = FH / 4 > 3 ? FH / 4 : 3, r = FH / 2, i;
    if (spin_x < 0)
        return;
    H->vgrad((WORD)(spin_x - r - d), (WORD)(spin_y - r - d), (WORD)(2 * (r + d) + 1), (WORD)(2 * (r + d) + 1),
             grad(spin_y - r - d), grad(spin_y + r + d));
    for (i = 0; i < 8; i++) {
        int back = (m->wait_phase - i) & 7;
        ULONG c = back == 0 ? C_ACC : back == 1 ? C_SUB : back == 2 ? C_DIM : C_FAINT;
        LONG cx = spin_x + dx[i] * r / 10, cy = spin_y + dy[i] * r / 10;
        rshape(cx - d / 2, cy - d / 2, d, d, d / 2, c);
    }
}
static void waiting_or_none(const struct LPage *m, LONG x, LONG y, LONG w)
{
    if (m->waiting) {
        spin_x = x + FH / 2 + FH / 4;
        spin_y = y + FH / 2;
        wait_spinner(m);
        text(spin_x + FH, y, lower(m->waiting), LF_REGULAR, C_SUB, &bg_grad, w - FH * 2);
    } else {
        text(x, y, lower(H->tr("Nothing here")), LF_REGULAR, C_SUB, &bg_grad, w);
    }
}

/* ---------------------------------------------------------------- Home: the strip */
/* The sections one above the other, the chosen one on the horizon with
 * its rows running across to the right as tiles (gliding: home_px). Menu
 * in use: the chosen section's name in the pill; its tiles in use: the
 * chosen tile glows. */
static LONG home_px, home_tgt, home_vis = 1;
static int home_sec = -1;
static LONG tile_w, tile_h, tile_x0, tile_y0, tile_gap;

/* How far the row can glide: its end at the screen's right */
static LONG home_max(const struct LHome *m)
{
    LONG mx = m->nrows ? m->rows[0].n * (tile_w + tile_gap) - (W - tile_x0 - X(24)) : 0;
    return mx > 0 ? mx : 0;
}

/* Home's row: the band it is drawn in */
#define HOME_X0 (tile_x0 - X(10))
#define HOME_Y0 (tile_y0 - X(10))
#define HOME_Y1 (tile_y0 + tile_h + FH * 2 + LABEL_GAP + Y(8))


/* Tile k where it is now (no clearing: its place is) */
static void home_tile(const struct LHome *m, LONG k)
{
    const struct LRow *row = &m->rows[0];
    const struct LItem *it = row->item(k);
    LONG cx = tile_x0 + k * (tile_w + tile_gap) - home_px, y = tile_y0;
    BOOL on = m->tile_sel >= 0 && m->tile_shown && k == m->tile_sel, hov = HOVER(HT_TILE, k);
    if (it->flags & LIF_HEAD)
        return;
    if (on || hov)
        glow(cx - 2, y - 2, tile_w + 4, tile_h + 4, Y(4), grad(y));
    {
        /* every tile alike: a glassy tile, what it has to show inside it
         * (a picture fitted, a disc, else the section's sign), its words
         * under it */
        ULONG t1 = on ? mix(C_TILE2, C_FOCUS2, 1, 2) : C_TILE2, t2 = on ? mix(C_TILE, C_FOCUS, 1, 2) : C_TILE;
        LONG pad = Y(6);
        const struct LImage *im;
        H->vgrad((WORD)cx, (WORD)y, (WORD)tile_w, (WORD)tile_h, t1, t2);
        H->fill((WORD)cx, (WORD)y, (WORD)tile_w, 1, mix(t1, C_WHITE, 1, 4));     /* (its lit top edge) */
        if (it->kind == LI_DISC) {
            disc(cx + (tile_w - tile_h) / 2 + pad, y + pad, tile_h - 2 * pad, mix(t1, t2, 1, 2));
        } else if (it->art && (im = H->picture(it->art, (UWORD)(tile_w - 2 * pad), (UWORD)(tile_h - 2 * pad)))) {
            H->image(im, (WORD)(cx + (tile_w - im->w) / 2), (WORD)(y + (tile_h - im->h) / 2));
        } else if (H->icon && m->sections && m->sel >= 0) {
            const struct LkBg bgt = { t1, t2, (WORD)y, (WORD)tile_h, 0 };
            H->icon(m->sections[m->sel], (WORD)(cx + (tile_w - H->icon_px) / 2),
                    (WORD)(y + (tile_h - H->icon_px) / 2), (it->flags & LIF_DISABLED) ? C_DIM : C_SUB, &bgt);
        }
    }
    if (on)
        H->frame((WORD)(cx - 1), (WORD)(y - 1), (WORD)(tile_w + 2), (WORD)(tile_h + 2), 2, C_ACC);
    text(cx, y + tile_h + LABEL_GAP, lower(it->label), on ? LF_BOLD : LF_REGULAR,
         (it->flags & LIF_DISABLED) ? C_DIM : on ? C_WHITE : C_TEXT, &bg_grad, tile_w);
    if (it->tag || it->sub)
        text(cx, y + tile_h + LABEL_GAP + FH, it->tag ? it->tag : it->sub, LF_SMALL, C_SUB, &bg_grad, tile_w);
}

/* The row's tiles that cross xa..xb, over the screen's own background */
static void home_tiles_in(const struct LHome *m, LONG xa, LONG xb)
{
    const struct LRow *row = m->nrows ? &m->rows[0] : NULL;
    LONG k, pitch = tile_w + tile_gap;
    if (xa < HOME_X0)
        xa = HOME_X0;
    if (xb > W)
        xb = W;
    if (xb <= xa)
        return;
    H->clip((WORD)xa, (WORD)HOME_Y0, (WORD)xb, (WORD)HOME_Y1);
    H->vgrad(0, 0, W, HT, C_TOP, C_BOT);
    if (row)
        for (k = (xa - tile_x0 + home_px - tile_w - X(10)) / pitch; k < row->n; k++) {
            LONG cx = tile_x0 + k * pitch - home_px;
            if (k < 0)
                continue;
            if (cx - X(10) >= xb)
                break;
            home_tile(m, k);
        }
    H->clip(0, 0, W, HT);
}

/* The row's click places where its tiles are now */
static void home_hits(const struct LHome *m)
{
    const struct LRow *row = m->nrows ? &m->rows[0] : NULL;
    LONG k;
    nhits = hits_base;
    if (!row || !row->n)
        return;
    hit(HOME_X0, HOME_Y0, W - HOME_X0, HOME_Y1 - HOME_Y0, HT_STRIP, 0, 0);
    for (k = home_px / (tile_w + tile_gap); k < row->n; k++) {
        const struct LItem *it = row->item(k);
        LONG cx = tile_x0 + k * (tile_w + tile_gap) - home_px;
        if (cx >= W)
            break;
        if (!(it->flags & (LIF_HEAD | LIF_DISABLED)))
            hit(cx, tile_y0, tile_w, tile_h + FH * 2 + LABEL_GAP, HT_TILE, it->kind, k);
    }
}

static void home_tiles(const struct LHome *m)
{
    LONG maxpx = home_max(m);
    if (home_px > maxpx)
        home_px = maxpx;
    home_hits(m);
    home_tiles_in(m, HOME_X0, W);
}

/* A step of the row's glide from old: what is drawn moved along, the strip
 * that comes in drawn */
static void home_glide_step(const struct LHome *m, LONG old)
{
    LONG d = home_px - old;
    if (!d)
        return;
    H->clip((WORD)HOME_X0, (WORD)HOME_Y0, W, (WORD)HOME_Y1);
    if (!H->copy || d >= W - HOME_X0 || -d >= W - HOME_X0 ||
        !H->copy((WORD)HOME_X0, (WORD)HOME_Y0, (WORD)(W - HOME_X0), (WORD)(HOME_Y1 - HOME_Y0), (WORD)-d, 0)) {
        home_tiles(m);
        return;
    }
    if (d > 0)
        home_tiles_in(m, W - d, W);
    else
        home_tiles_in(m, HOME_X0, HOME_X0 - d);
    home_hits(m);
}

/* Where the tiles go, and where the row glides to for the chosen one */
static BOOL home_tmode;         /* drawn with the tiles in use (the menu's pill off) */
static BOOL home_was_tiles;     /* (home_place's last: the tiles in use) */
static void home_place(const struct LHome *m)
{
    /* the tiles: 4:3, a fifth of the screen tall */
    tile_h = Y(118);
    tile_w = tile_h * 4 / 3;
    tile_gap = X(16);
    tile_x0 = X(250);
    tile_y0 = Y(232) - tile_h / 2;     /* (centred on the horizon's height) */
    if (m->sel != home_sec) {
        home_sec = m->sel;
        home_px = home_tgt = 0;     /* another section: its row from its start */
    }
    home_vis = (W - X(24) - tile_x0 + tile_gap) / (tile_w + tile_gap);
    if (home_vis < 1)
        home_vis = 1;
    if (m->tile_sel >= 0 && m->tile_shown) {
        LONG l = m->tile_sel * (tile_w + tile_gap), r = l + tile_w;
        if (l - tile_gap < home_tgt)
            home_tgt = l > 0 ? l - tile_w / 3 : 0;
        if (r + tile_w / 3 > home_tgt + W - X(24) - tile_x0)
            home_tgt = r + tile_w / 3 - (W - X(24) - tile_x0);
        if (home_tgt < 0)
            home_tgt = 0;
        home_tgt &= ~3L;            /* (whole 16-bit dither steps: the copied row matches) */
    } else if (m->tile_sel < 0 && home_was_tiles) {
        home_tgt = 0;               /* (back to the menu from the tiles: the row from its start) */
    }                               /* (else where the mouse left it) */
    home_was_tiles = m->tile_sel >= 0;
}

static void draw_home(void)
{
    const struct LHome *m = H->model(LS_HOME);
    LONG pitch = TH + Y(22), ys, i, nx = X(56), lab = FH * 2 + LABEL_GAP + Y(4);
    background();
    top_left();
    top_right();
    if (!m)
        return;
    home_place(m);
    ys = tile_y0 + tile_h / 2;
    home_tmode = m->tile_sel >= 0;
    /* the sections: above the horizon and below it, fainter further off */
    for (i = 0; i < m->n; i++) {
        LONG d = i - m->sel, cy;
        ULONG c = d == 0 ? C_WHITE : (d == 1 || d == -1) ? C_SUB : (d == 2 || d == -2) ? C_DIM : C_FAINT;
        if (d < 0)
            cy = ys - tile_h / 2 - Y(26) - (-d - 1) * pitch - TH / 2;
        else if (d > 0)
            cy = ys + tile_h / 2 + lab + Y(22) + (d - 1) * pitch + TH / 2;
        else
            cy = ys;
        if (cy - TH / 2 < LOGO_Y + logo_h + Y(10) || cy + TH / 2 > HT - Y(20))
            continue;
        if (d == 0) {
            LONG w = tlen(lower(m->names[i]), LF_TITLE) + X(32);
            BOOL menu = m->tile_sel < 0;
            if (menu || HOVER(HT_SECTION, i))
                pill(nx - X(16), cy - TH / 2 - Y(8), w, TH + Y(16), menu, grad(cy));
            text(nx, cy - TH / 2, lower(m->names[i]), LF_TITLE, C_WHITE,
                 menu || HOVER(HT_SECTION, i) ? &pill_bg : &bg_grad, tile_x0 - nx - X(24));
            hit(nx - X(16), cy - TH / 2 - Y(8), w, TH + Y(16), HT_SECTION, 0, i);
        } else {
            if (HOVER(HT_SECTION, i))
                pill(nx - X(16), cy - TH / 2 - Y(6), tlen(lower(m->names[i]), LF_TITLE) + X(32), TH + Y(12),
                     FALSE, grad(cy));
            text(nx, cy - TH / 2, lower(m->names[i]), LF_TITLE, c,
                 HOVER(HT_SECTION, i) ? &pill_bg : &bg_grad, W - nx - X(40));
            hit(nx - X(16), cy - TH / 2 - Y(6), tlen(lower(m->names[i]), LF_TITLE) + X(32), TH + Y(12),
                HT_SECTION, 0, i);
        }
    }
    horizon_line(X(30), ys + tile_h / 2 + lab + Y(6), W - X(60));
    {
        /* what plays is a tile of this row (Music's): the tile, not the inset too */
        static BOOL was_off;
        const struct LRow *row = m->nrows ? &m->rows[0] : NULL;
        LONG k;
        if (!row || !row->n) {
            inset_off = was_off;    /* (the row still coming: as it was, not a flash of the inset) */
        } else {
            for (k = 0; k < row->n && k < 8 && !inset_off; k++)
                if (row->item(k)->flags & LIF_NOW)
                    inset_off = TRUE;
        }
        was_off = inset_off;
    }
    now_inset();                /* (before the tiles: their hits drawn again alone, the last) */
    hits_base = nhits;
    home_tiles(m);
}

/* ---------------------------------------------------------------- the generic page */
static LONG list_top, list_count = -1;
static const void *list_of;
static int body_n = -1;         /* drawn with details (1), without (0), a text (-1) */

/* Whether a page has its details' column: once a list has shown details,
 * it keeps the column while the next row's are fetched (a move on Games
 * would otherwise widen the list and narrow it again each step) */
static ULONG split_key;
static ULONG page_key(const struct LPage *m)
{
    ULONG k = (ULONG)m->list.count * 31 + m->section + (ULONG)m->ncrumb * 7;
    const char *s = m->ncrumb > 0 ? m->crumb[m->ncrumb - 1] : NULL;
    if (s)
        while (*s)
            k = k * 33 + (UBYTE)*s++;
    return k | 1;
}
static int page_split(const struct LPage *m)
{
    ULONG k = page_key(m);
    if (m->ndetails > 0 || m->picture || m->aside) {
        split_key = k;
        return 1;
    }
    return k == split_key ? 1 : 0;
}

static LONG body_bottom(void)
{
    LONG t = inset_top();
    return t < HT ? t - Y(14) : HT - Y(30) - FH;
}

/* A list's rows. The last drawn is kept (rows_*): a move draws again only
 * the rows that changed - the one left, the one come to and theirs either
 * side (the glow reaches into them) - and, scrolled, copies the rest up or
 * down (H->copy) and draws the ones come in. The host refuses an up or
 * down copy over a gradient that would show (a graphics card's): then,
 * and for anything else changed, all of them as before. */
static const void *rows_of;
static LONG rows_count = -1, rows_top, rows_sel, rows_x, rows_y, rows_w, rows_h;
static BOOL rows_ok;                    /* (FALSE: drawn by nothing yet, or over) */
static BOOL rows_hov;                   /* (a row under the pointer when they were drawn) */

static void draw_row(const struct LPage *m, LONG idx, LONG rx, LONG ry, LONG rw, LONG rh)
{
    const struct LList *l = &m->list;
    const struct LItem *it = l->item(idx);
    BOOL sel = idx == l->sel, hov = HOVER(HT_ROW, idx) && !(it->flags & LIF_DISABLED);
    LONG right = rx + rw - X(14), tw, mw;
    const struct LkBg *tb = &bg_grad;
    ULONG bgc = grad(ry + rh / 2);
    if (it->flags & LIF_HEAD) {
        text(rx + X(14), ry + rh - FH - Y(2), lower(it->label), LF_SMALL, C_ACC, &bg_grad, rw);
        return;
    }
    if (sel || hov) {
        pill(rx, ry + 2, rw, rh - 4, sel, bgc);
        tb = &pill_bg;
        bgc = sel ? C_FOCUS : C_HOVER;
    }
    mw = mark(it->mark, right, ry + rh / 2, sel ? C_WHITE : C_SUB, bgc);
    if (mw)
        right -= mw + X(10);
    if (it->tag && *it->tag) {
        tw = tlen(it->tag, LF_REGULAR);
        if (tw > rw / 2)
            tw = rw / 2;
        text(right - tw, ry + (rh - FH) / 2, it->tag, LF_REGULAR, sel ? C_WHITE : C_SUB, tb, tw);
        right -= tw + X(14);
    }
    text(rx + X(14), ry + (rh - FH) / 2, it->label, sel ? LF_BOLD : LF_REGULAR,
         (it->flags & LIF_DISABLED) ? C_DIM : sel ? C_WHITE : C_TEXT, tb, right - rx - X(14));
}

/* Part of the rows' background again, exactly as the whole of it is drawn
 * (the same gradient over the same place, clipped to the part) */
static void rows_bg(LONG x, LONG y, LONG w, LONG h, LONG px, LONG py, LONG pw, LONG ph)
{
    H->clip((WORD)px, (WORD)py, (WORD)(px + pw), (WORD)(py + ph));
    H->vgrad((WORD)(x - X(12)), (WORD)(y - X(8)), (WORD)(w + X(24)), (WORD)(h + X(16)), grad(y - X(8)),
             grad(y + h + X(8)));
    H->clip(0, 0, W, HT);
}

static void draw_rows(const struct LPage *m, LONG x, LONG y, LONG w, LONG h)
{
    LONG rh = FH + Y(18), fit, i, d = 0, rw;
    const struct LList *l = &m->list;
    BOOL bar, part;
    fit = h / rh;
    if (fit < 1)
        fit = 1;
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
    bar = l->count > fit;
    rw = w - (bar ? X(16) : 0);
    /* the rows that changed only: the same list, place and pointer, scrolled
     * less than a page (copied, if the host can) */
    part = rows_ok && rows_of == (const void *)l->item && rows_count == l->count && rows_x == x &&
           rows_y == y && rows_w == w && rows_h == h && hov_what != HT_ROW && !rows_hov;
    if (part) {
        d = list_top - rows_top;
        if (d >= fit || -d >= fit)
            part = FALSE;
        else if (d) {
            LONG cx = x - X(12), cw = rw + X(12) + X(7);
            part = d > 0 ? H->copy && H->copy((WORD)cx, (WORD)(y + d * rh), (WORD)cw, (WORD)((fit - d) * rh), 0,
                                              (WORD)(-d * rh))
                         : H->copy && H->copy((WORD)cx, (WORD)y, (WORD)cw, (WORD)((fit + d) * rh), 0,
                                              (WORD)(-d * rh));
        }
    }
    if (!part) {
        H->vgrad((WORD)(x - X(12)), (WORD)(y - X(8)), (WORD)(w + X(24)), (WORD)(h + X(16)), grad(y - X(8)),
                 grad(y + h + X(8)));
        for (i = 0; i < fit && list_top + i < l->count; i++)
            draw_row(m, list_top + i, x, y + i * rh, rw, rh);
    } else {
        /* the slots to draw: come in (scrolled), the one left and the one
         * come to, and theirs either side; the selection's last (its glow
         * over them) */
        UBYTE want[64];
        LONG os = rows_sel - list_top, ns = l->sel - list_top, k, cx = x - X(12), cw = rw + X(12) + X(7);
        if (fit > 64)
            fit = 64;
        for (k = 0; k < 64; k++)
            want[k] = 0;
        if (d > 0)
            for (k = fit - d; k < fit; k++)
                want[k] = 1;
        else if (d < 0)
            for (k = 0; k < -d; k++)
                want[k] = 1;
        for (k = -1; k <= 1; k++) {
            if (os + k >= 0 && os + k < fit)
                want[os + k] = 1;
            if (ns + k >= 0 && ns + k < fit)
                want[ns + k] = 1;
        }
        /* (the margins above and below: a first or last row's glow reaches them) */
        rows_bg(x, y, w, h, cx, y - X(8), cw, X(8));
        rows_bg(x, y, w, h, cx, y + fit * rh, cw, h + X(8) - fit * rh);
        for (k = 0; k < fit; k++)
            if (want[k])
                rows_bg(x, y, w, h, cx, y + k * rh, cw, rh);
        for (k = 0; k < fit && list_top + k < l->count; k++)
            if (want[k] && k != ns)
                draw_row(m, list_top + k, x, y + k * rh, rw, rh);
        if (ns >= 0 && ns < fit && list_top + ns < l->count)
            draw_row(m, list_top + ns, x, y + ns * rh, rw, rh);
    }
    for (i = 0; i < fit && list_top + i < l->count; i++) {    /* (where a click goes: every row) */
        const struct LItem *it = l->item(list_top + i);
        if (!(it->flags & (LIF_HEAD | LIF_DISABLED)))
            hit(x, y + i * rh, rw, rh, HT_ROW, it->kind, list_top + i);
    }
    if (bar) {
        LONG bh = fit * rh, th = bh * fit / l->count, ty, bx = x + w - X(6);
        if (th < Y(16))
            th = Y(16);
        ty = y + (bh - th) * list_top / (l->count - fit);
        if (part)                       /* (its column: the thumb moved) */
            rows_bg(x, y, w, h, bx - X(3), y, X(9) + 6, bh);
        scroll_units(l->sel, fit, l->count - 1);
        scroll_bar(bx, y, bh, ty, th, FALSE);
    }
    rows_of = (const void *)l->item;
    rows_count = l->count;
    rows_top = list_top;
    rows_sel = l->sel;
    rows_x = x;
    rows_y = y;
    rows_w = w;
    rows_h = h;
    rows_hov = hov_what == HT_ROW;
    rows_ok = TRUE;
}

/* The details at the right of a list: the picture standing on its
 * reflection, the facts under it */
static void details_at(const char *picture, const char *const *lines, LONG nl, LONG x, LONG y, LONG w,
                       LONG bottom)
{
    if (picture) {
        LONG pw = w, ph = pw * 3 / 4, refl;
        if (ph > (bottom - y) * 45 / 100) {
            ph = (bottom - y) * 45 / 100;
            pw = ph * 4 / 3;
        }
        refl = ph / 4;
        cover_at(picture, NULL, x + (w - pw) / 2, y, pw, ph, refl);
        y += ph + refl / 2 + Y(10);
    }
    draw_lines(lines, nl, 0, x, y, w, bottom, C_WHITE, C_BODY);
}

/* What the details' column showed (drawn again only when it changes: a
 * move redraws the rows; the column, its picture and text, only when they
 * are another's). ~0: drawn by nothing yet (a whole draw sets it so). */
static ULONG col_key = ~0UL;
static ULONG details_key(const struct LPage *m)
{
    return (ULONG)m->picture ^ ((ULONG)m->ndetails << 24) ^ (m->ndetails ? (ULONG)m->details[0] : 0) ^
           ((ULONG)(UBYTE)m->letter << 16) ^ ((ULONG)body_n << 30);
}

static void draw_page_body(const struct LPage *m)
{
    LONG x = X(30), y = BODY_Y, bottom = body_bottom(), w = W - x - X(30), n, lw;
    ULONG key;
    n = page_split(m);
    body_n = (int)n;
    nhits = hits_base;
    lw = n ? w * 50 / 100 : w;
    if (!(m->waiting && m->list.count))
        spin_x = -1;
    if (m->list.count) {
        draw_rows(m, x, y, lw, bottom - y);
    } else {
        H->vgrad((WORD)x, (WORD)y, (WORD)lw, (WORD)(FH * 2), grad(y), grad(y + FH * 2));
        waiting_or_none(m, x + X(14), y + Y(6), lw);
    }
    if (n && (key = details_key(m)) != col_key) {
        LONG dx = x + lw + X(36), dw = x + w - dx;
        col_key = key;
        H->vgrad((WORD)(dx - X(8)), (WORD)(y - X(8)), (WORD)(dw + X(16)), (WORD)(bottom - y + X(16)),
                 grad(y - X(8)), grad(bottom + X(8)));
        if (m->letter && H->letter)
            /* gliding: the letter in the details' place (none are read
             * meanwhile), the rows clear of it - drawn once a letter */
            H->letter((WORD)(dx + dw * 3 / 4), (WORD)(y + (bottom - y) / 2), C_LETTER);
        else
            details_at(m->picture, m->details, m->ndetails, dx, y + Y(6), dw, bottom);
    }
    if (!n && m->letter && H->letter) {
        H->letter((WORD)(x + lw - X(40)), (WORD)(y + (bottom - y) / 2), C_LETTER);
        rows_ok = FALSE;                /* (over the rows: all of them again next) */
    }
}

/* A long text as this layout wraps it: each line's height (a blank one
 * half), and the whole; worked out once a text and width. The host scrolls
 * its own wrapping of it (the classic's lines and box), so where it is goes
 * by the fraction of the way (LPage.text_frac): its end is the end here. */
#define TL_N 256
static LONG tl_h[TL_N], tl_n = -1, tl_w, tl_total;
static const void *tl_of;
static void text_measure(const struct LPage *m, LONG w, LONG lh)
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
        tl_h[i] = p && *p ? wrapped(p, w, LF_REGULAR) * lh : lh / 2;
        tl_total += tl_h[i];
    }
}
/* Where it is drawn from for a view vh tall: its first line, and the y
 * (from the view's top) that line starts at */
static LONG text_from(const struct LPage *m, LONG vh, LONG *dy)
{
    LONG off = tl_total > vh ? (LONG)((m->text_frac >> 4) * (ULONG)(tl_total - vh) / 4096) : 0, acc = 0, i = 0;
    while (i < tl_n && acc + tl_h[i] <= off)
        acc += tl_h[i++];
    *dy = -(off - acc);
    return i;
}

/* A note, wrapped to w and each line centred in it, from y; its colour c
 * over bg */
static void note_centred(const char *t, LONG x, LONG y, LONG w, ULONG c, const struct LkBg *bg)
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

/* One long text, scrolled by pixels; a note, a picture and Plex's mark
 * under it on a page that has them. full: all of it; else (a glide) the
 * text and its bar alone. */
static LONG tx_bottom;
static void draw_text_body_as(const struct LPage *m, BOOL full)
{
    LONG x = X(30), w = W - x - X(30), top = BODY_Y, bottom = body_bottom(), lh = FH + Y(4), vis, sbx = x + w - X(6);
    if (full) {
        const struct LImage *pm = m->plexmark && H->plexmark ? H->plexmark(grad(bottom - Y(40))) : NULL;
        LONG under = bottom;
        if (pm) {
            LONG fine = H->font_h(LF_TINY), my = under - fine - Y(4) - pm->h, mx = x;
            const char *with = H->tr("Works with:");
            text(mx, my + (pm->h - FH) / 2, with, LF_REGULAR, C_SUB, &bg_grad, w / 2);
            mx += tlen(with, LF_REGULAR) + X(10);
            H->image(pm, (WORD)mx, (WORD)my);
            text(mx + pm->w + X(2), my, "*", LF_SMALL, C_SUB, &bg_grad, 0);
            text(x, under - fine, "* used under license from Plex", LF_TINY, C_DIM, &bg_grad, w);
            under = my - Y(12);
        }
        if (m->picture || m->note) {
            LONG nh = m->note ? wrapped(m->note, w, LF_REGULAR) * lh : 0;
            LONG ph = (under - top) * 2 / 5 - nh, y;
            const struct LImage *im = NULL;
            if (ph < Y(24))
                ph = 0;
            if (ph && m->picture)
                im = H->picture(m->picture, (UWORD)w, (UWORD)ph);
            y = under - ph - nh - Y(8);
            horizon_line(x, y - Y(8), w);
            if (m->note)            /* centred, over the picture */
                note_centred(m->note, x, y, w, C_SUB, &bg_grad);
            if (im)
                H->image(im, (WORD)(x + (w - im->w) / 2), (WORD)(y + nh + Y(4) + (ph - im->h) / 2));
            under = y - Y(16);
        }
        tx_bottom = under;
    }
    {
        LONG vh = tx_bottom - top, dy, from;
        (void)vis;
        text_measure(m, sbx - X(14) - x, lh);
        from = text_from(m, vh, &dy);
        back_at(x, top, x + w + 2, tx_bottom);
        H->clip((WORD)x, (WORD)top, (WORD)(sbx - X(6)), (WORD)tx_bottom);
        draw_lines(m->details, m->ndetails, from, x, top + dy, sbx - X(14) - x, tx_bottom + lh, C_BODY, C_BODY);
        H->clip(0, 0, W, HT);
        if (tl_total > vh && vh > 0) {
            LONG bh = vh, th = bh * vh / tl_total, ty;
            if (th < Y(16))
                th = Y(16);
            ty = top + (LONG)((bh - th) * (m->text_frac >> 4) / 4096);
            if (ty > top + bh - th)
                ty = top + bh - th;
            scroll_units((LONG)(m->text_frac * 1000 / 65536), vh * 1000 / (tl_total - vh), 1000);
            scroll_bar(sbx, top, bh, ty, th, FALSE);
        }
    }
}

static void draw_page(void)
{
    const struct LPage *m = H->model(LS_PAGE);
    background();
    page_head(m, TRUE);
    if (!m)
        return;
    spin_x = -1;
    if (m->waiting && m->list.count) {
        spin_x = title_end + X(16) + FH / 2;
        spin_y = TITLE_Y + TH / 2;
        wait_spinner(m);
    }
    hits_base = nhits;
    if (m->text) {
        body_n = -1;
        draw_text_body_as(m, TRUE);
    } else {
        col_key = ~0UL;                 /* (a whole draw: the column too) */
        rows_ok = FALSE;                /* (and every row) */
        draw_page_body(m);
    }
    now_inset();
    footer_keep(m->hint, m->message, m->waiting != NULL);
}

/* ---------------------------------------------------------------- the wall: covers, sideways */
/* Its covers in columns of as many as fit down, the columns running on to
 * the right past the screen's edge; a move glides it sideways (wall_px)
 * so the chosen cover's column shows, a little of the next beside it.
 * Drawn whole only when it must be: a glide moves what is drawn along
 * (H->copy) and draws the strip that comes in; a move, the pointer or a
 * cover coming draw the covers they touch. */
static LONG wall_rows = 1, wall_px, wall_tgt, wall_count = -1;
static const void *wall_of;
static struct {
    LONG x0, y0, bottom, viewh, gap, cs, th, ch, ncol, maxpx, viewe;
} wg;
static LONG wall_sel_drawn = -1, wall_hov_drawn = -1;  /* drawn chosen, under the pointer */
static LONG wall_lead = -1;     /* a letter's first, gone to with the keys: it leads the wall
                                 * (its column at the left, the letter filling the view) */
static char wall_letter;                /* the big letter drawn over it (0 none) */
static LONG wall_lx0, wall_lx1;         /* ...where */
#define WALL_PH 64
static LONG wall_ph[WALL_PH];           /* covers drawn waiting for their picture */
static int wall_nph;

/* Where everything goes; where the glide is going (the chosen one shown) */
static void wall_geom(const struct LPage *m)
{
    const struct LList *l = &m->list;
    LONG cs, th, ch, rows, sel = l->sel;
    wg.x0 = X(30);
    wg.y0 = BODY_Y;
    wg.bottom = body_bottom();
    wg.viewh = wg.bottom - wg.y0;
    wg.gap = X(18);
    wg.viewe = W - X(24) - wg.x0;
    cs = Y(118);
    th = m->frames ? cs * 3 / 4 : cs;
    ch = th + LABEL_GAP + FH * 2 + Y(12);
    rows = (wg.viewh + Y(8)) / ch;
    if (rows < 1)
        rows = 1;
    if (rows > 1 && rows * ch > wg.viewh)       /* (a little smaller, to fit) */
        cs = (wg.viewh / rows - LABEL_GAP - FH * 2 - Y(12));
    th = m->frames ? cs * 3 / 4 : cs;
    if (m->frames)
        cs = th * 4 / 3;
    wg.cs = cs;
    wg.th = th;
    wg.ch = wg.viewh / rows;
    wall_rows = rows;
    wg.ncol = (l->count + rows - 1) / rows;
    wg.maxpx = wg.ncol * (cs + wg.gap) - wg.viewe;
    if (wg.maxpx < 0)
        wg.maxpx = 0;
    if (sel >= 0) {
        LONG left = (sel / rows) * (cs + wg.gap), peek = cs / 3;
        if (sel == wall_lead) {
            wall_tgt = left - (sel >= rows ? peek : 0);     /* the letter fills the view */
            wall_lead = -1;
        }
        if (left - (sel >= rows ? peek : 0) < wall_tgt)
            wall_tgt = left - (sel >= rows ? peek : 0);
        if (left + cs + peek > wall_tgt + wg.viewe)
            wall_tgt = left + cs + peek - wg.viewe;
    }
    if (wall_tgt > wg.maxpx)
        wall_tgt = wg.maxpx;
    if (wall_tgt < 0)
        wall_tgt = 0;
    wall_tgt &= ~3L;                    /* (whole steps of the 16-bit dither: a copied wall
                                         * and what is drawn beside it match) */
    if (wall_of != (const void *)l->item || wall_count < 0 || l->count < wall_count) {
        wall_px = wall_tgt;             /* another list: there at once */
        wall_of = (const void *)l->item;
    }
    wall_count = l->count;
}

/* the band the covers are drawn in (the bar under it apart) */
#define WALL_TOP (wg.y0 - Y(10))
#define WALL_GLOW (X(7) + 3)

/* The band's background where x0..x1, y0..y1 (inside it): the one
 * gradient the whole is drawn with, so a piece drawn again is as it was */
static void wall_bg(LONG xa, LONG ya, LONG xb, LONG yb)
{
    if (ya < WALL_TOP)
        ya = WALL_TOP;
    if (yb > wg.bottom)
        yb = wg.bottom;
    if (xa < 0)
        xa = 0;
    if (xb > W)
        xb = W;
    if (xb <= xa || yb <= ya)
        return;
    H->clip((WORD)xa, (WORD)ya, (WORD)xb, (WORD)yb);
    H->vgrad(0, (WORD)WALL_TOP, W, (WORD)(wg.viewh + Y(20)), grad(WALL_TOP), grad(wg.bottom + Y(10)));
}

static LONG wall_cx(LONG i) { return wg.x0 + (i / wall_rows) * (wg.cs + wg.gap) - wall_px; }
static LONG wall_cy(LONG i) { return wg.y0 + (i % wall_rows) * wg.ch; }

/* Cover i where it is now; clear: its place painted first */
static void wall_tile(const struct LPage *m, LONG i, BOOL clear)
{
    const struct LItem *it;
    LONG cx = wall_cx(i), cy = wall_cy(i), cs = wg.cs, th = wg.th;
    BOOL on = i == m->list.sel, hov = HOVER(HT_ROW, i);
    if (i < 0 || i >= m->list.count || cx >= W || cx + cs + WALL_GLOW <= 0)
        return;
    if (clear) {
        wall_bg(cx - WALL_GLOW, cy - WALL_GLOW, cx + cs + WALL_GLOW, cy + th + LABEL_GAP + FH * 2 + 2);
        H->clip(0, (WORD)WALL_TOP, W, (WORD)wg.bottom);
    }
    it = m->list.item(i);
    if (it->flags & LIF_HEAD)
        return;
    if (on || hov)
        glow(cx - 2, cy - 2, cs + 4, th + 4, Y(4), grad(cy));
    if (it->kind == LI_DISC) {
        disc(cx + (cs - th) / 2, cy, th, grad(cy + th / 2));
    } else if (!cover_at(it->art, it->label, cx, cy, cs, th, 0) && it->art && wall_nph < WALL_PH) {
        int k;
        for (k = 0; k < wall_nph && wall_ph[k] != i; k++)
            ;
        if (k == wall_nph)
            wall_ph[wall_nph++] = i;    /* (drawn again when pictures come) */
    }
    if (on)
        H->frame((WORD)(cx - 1), (WORD)(cy - 1), (WORD)(cs + 2), (WORD)(th + 2), 2, C_ACC);
    text(cx, cy + th + LABEL_GAP, it->label, on ? LF_BOLD : LF_REGULAR, on ? C_WHITE : C_TEXT, &bg_grad, cs);
    if (it->sub || it->tag)
        text(cx, cy + th + LABEL_GAP + FH, it->sub ? it->sub : it->tag, LF_SMALL, C_SUB, &bg_grad, cs);
}

/* The covers whose places cross x .. x1 (the band clipped there first) */
static void wall_tiles_in(const struct LPage *m, LONG xa, LONG xb)
{
    LONG c, r, pitch = wg.cs + wg.gap;
    if (xa < 0)
        xa = 0;
    if (xb > W)
        xb = W;
    if (xb <= xa)
        return;
    wall_bg(xa, WALL_TOP, xb, wg.bottom);       /* (and clipped there: the covers in it) */
    for (c = (xa - wg.x0 + wall_px - wg.cs - WALL_GLOW) / pitch; c * pitch + wg.x0 - wall_px - WALL_GLOW < xb; c++) {
        if (c < 0)
            continue;
        for (r = 0; r < wall_rows; r++)
            wall_tile(m, c * wall_rows + r, FALSE);
    }
}

/* The hits of the covers where they are now, and the bar under them */
static void wall_hits_bar(const struct LPage *m)
{
    LONG i, sel = m->list.sel;
    nhits = hits_base;
    nsbar = 0;
    hit_sel = sel;
    for (i = (wall_px / (wg.cs + wg.gap)) * wall_rows; i < m->list.count; i++) {
        LONG cx = wall_cx(i);
        if (cx >= W)
            break;
        if (!(m->list.item(i)->flags & LIF_HEAD))
            hit(cx, wall_cy(i), wg.cs, wg.th + FH * 2 + LABEL_GAP, HT_ROW, m->list.item(i)->kind, i);
    }
    H->clip(0, (WORD)wg.bottom, W, (WORD)(wg.bottom + Y(10)));
    H->vgrad(0, (WORD)WALL_TOP, W, (WORD)(wg.viewh + Y(20)), grad(WALL_TOP), grad(wg.bottom + Y(10)));
    if (wg.maxpx > 0) {
        LONG bw = wg.viewe, all = wg.ncol * (wg.cs + wg.gap), tl = bw * wg.viewe / (all > 0 ? all : 1), t;
        if (tl < X(24))
            tl = X(24);
        if (tl > bw)
            tl = bw;
        t = wg.x0 + (bw - tl) * wall_px / wg.maxpx;
        scroll_units(sel, wall_rows * (wg.viewe / (wg.cs + wg.gap) > 1 ? wg.viewe / (wg.cs + wg.gap) : 1),
                     m->list.count - 1);
        scroll_bar(wg.x0, wg.bottom + Y(2), bw, t, tl, TRUE);
    }
    H->clip(0, 0, W, HT);
}

/* The big letter over the covers, as the host has it up; the covers under
 * where it was drawn again first (also where a glide moved it to: by) */
static void wall_letter_update(const struct LPage *m, LONG by)
{
    if (wall_letter) {
        LONG a = wall_lx0, b = wall_lx1;
        if (by < 0)
            a += by;
        else
            b += by;
        wall_letter = 0;
        wall_tiles_in(m, a, b);
    }
    if (m->letter && H->letter) {
        LONG w;
        H->clip(0, (WORD)WALL_TOP, W, (WORD)wg.bottom);
        w = H->letter((WORD)(W - X(60)), (WORD)(wg.y0 + wg.viewh / 2), C_LETTER);
        if (w > 0) {
            wall_letter = m->letter;
            wall_lx0 = W - X(60) - w - 2;
            wall_lx1 = W - X(60) + 2;
        }
    }
    H->clip(0, 0, W, HT);
}

static void draw_wall_body(const struct LPage *m)
{
    const struct LList *l = &m->list;
    LONG i;
    wall_geom(m);
    wall_nph = 0;
    wall_letter = 0;
    wall_sel_drawn = l->sel;
    wall_hov_drawn = hov_what == HT_ROW ? hov_arg : -1;
    H->clip(0, (WORD)WALL_TOP, W, (WORD)(wg.bottom + Y(10)));
    H->vgrad(0, (WORD)WALL_TOP, W, (WORD)(wg.viewh + Y(20)), grad(WALL_TOP), grad(wg.bottom + Y(10)));
    spin_x = -1;
    nhits = hits_base;
    if (!l->count) {
        waiting_or_none(m, wg.x0, wg.y0, wg.viewe);
        H->clip(0, 0, W, HT);
        return;
    }
    H->clip(0, (WORD)WALL_TOP, W, (WORD)wg.bottom);
    for (i = (wall_px / (wg.cs + wg.gap)) * wall_rows; i < l->count && wall_cx(i) < W; i++)
        wall_tile(m, i, FALSE);
    wall_hits_bar(m);
    wall_letter_update(m, 0);
}

/* The chosen one moved, or the pointer: those covers alone (the glide,
 * if it must move, follows in the ticks) */
static void wall_moved(const struct LPage *m)
{
    LONG was = wall_sel_drawn, hw = wall_hov_drawn, hn = hov_what == HT_ROW ? hov_arg : -1, k;
    LONG redo[4];
    int n = 0, j;
    wall_geom(m);
    redo[n++] = was;
    redo[n++] = m->list.sel;
    redo[n++] = hw;
    redo[n++] = hn;
    wall_sel_drawn = m->list.sel;
    wall_hov_drawn = hn;
    H->clip(0, (WORD)WALL_TOP, W, (WORD)wg.bottom);
    for (j = 0; j < n; j++) {
        for (k = 0; k < j && redo[k] != redo[j]; k++)
            ;
        if (k == j && redo[j] >= 0)
            wall_tile(m, redo[j], TRUE);
    }
    wall_hits_bar(m);
    if (wall_letter || m->letter)
        wall_letter_update(m, 0);
}

/* Pictures came: the covers that waited for one */
static void wall_pictures(const struct LPage *m)
{
    LONG ph[WALL_PH];
    int n = wall_nph, k;
    for (k = 0; k < n; k++)
        ph[k] = wall_ph[k];
    wall_nph = 0;
    wall_geom(m);
    H->clip(0, (WORD)WALL_TOP, W, (WORD)wg.bottom);
    for (k = 0; k < n; k++)
        if (wall_cx(ph[k]) < W && wall_cx(ph[k]) + wg.cs > 0)
            wall_tile(m, ph[k], TRUE);
    if (wall_letter)
        wall_letter_update(m, 0);
    H->clip(0, 0, W, HT);
}

/* A step of the glide, from where it was drawn (old): what is drawn moved
 * along, the strip that comes in drawn; without the host's copy, all */
static void wall_glide_step(const struct LPage *m, LONG old)
{
    LONG d = wall_px - old;
    if (!d)
        return;
    if (!H->copy || d >= W || -d >= W || wall_nph >= WALL_PH) {
        draw_wall_body(m);
        return;
    }
    H->clip(0, (WORD)WALL_TOP, W, (WORD)wg.bottom);
    if (!H->copy(0, (WORD)WALL_TOP, W, (WORD)(wg.bottom - WALL_TOP), (WORD)-d, 0)) {
        draw_wall_body(m);
        return;
    }
    {
        /* what waited for a picture moved too: still waiting, where it is now */
        int k, n = 0;
        for (k = 0; k < wall_nph; k++)
            if (wall_cx(wall_ph[k]) < W && wall_cx(wall_ph[k]) + wg.cs + WALL_GLOW > 0)
                wall_ph[n++] = wall_ph[k];
        wall_nph = n;
    }
    if (d > 0)
        wall_tiles_in(m, W - d, W);
    else
        wall_tiles_in(m, 0, -d);
    if (wall_letter) {
        wall_lx0 -= d;              /* (the copy took it along: drawn away from there, and up again) */
        wall_lx1 -= d;
        wall_letter_update(m, d);
    } else if (m->letter) {
        wall_letter_update(m, 0);
    }
    wall_hits_bar(m);
}

static void draw_wall(void)
{
    const struct LPage *m = H->model(LS_ALBUMS);
    background();
    page_head(m, TRUE);
    if (!m)
        return;
    now_inset();                /* (before the wall: its hits drawn again alone, the last) */
    hits_base = nhits;
    draw_wall_body(m);
    footer_keep(m->hint, m->message, m->waiting != NULL);
}

/* ---------------------------------------------------------------- an album */
/* Its cover at the left on its reflection, the review under it; at the
 * right its name, who, the facts, and its tracks */
static LONG album_lx, album_ly, album_lw, album_lh;
static void album_rows(void)
{
    const struct LAlbum *a = H->model(LS_ALBUM);
    if (!a)
        return;
    nhits = hits_base;
    if (a->page.list.count)
        draw_rows(&a->page, album_lx, album_ly, album_lw, album_lh);
    if (a->page.letter && H->letter)
        H->letter((WORD)(album_lx + album_lw - X(40)), (WORD)(album_ly + album_lh / 2), C_LETTER);
}

static void draw_album(void)
{
    const struct LAlbum *a = H->model(LS_ALBUM);
    LONG x = X(30), y, bottom, cs, tx, tw, ty, i, refl;
    background();
    page_head(a ? &a->page : NULL, FALSE);
    y = TITLE_Y + Y(8);
    if (!a)
        return;
    bottom = body_bottom();
    cs = X(230);
    if (cs > (bottom - y) * 55 / 100)
        cs = (bottom - y) * 55 / 100;
    refl = cs / 4;
    cover_at(a->art, a->title, x, y, cs, cs, refl);
    if (a->review && *a->review) {
        LONG np = paras(a->review);
        draw_lines(para, np, 0, x, y + cs + refl / 2 + Y(12), cs, bottom, C_BODY, C_BODY);
    }
    tx = x + cs + X(36);
    tw = W - tx - X(30);
    ty = y;
    text(tx, ty, a->title, LF_TITLE, C_WHITE, &bg_grad, tw);
    ty += TH + Y(2);
    if (a->artist && *a->artist) {
        text(tx, ty, lower(a->artist), LF_REGULAR, C_ACC, &bg_grad, tw);
        ty += FH + Y(4);
    }
    {
        char b[200];
        b[0] = 0;
        join(b, sizeof(b), a->line, "  \xB7  ", a->rating && *a->rating ? a->rating : "");
        for (i = 0; i < a->facts.n && i < 2; i++)
            join(b, sizeof(b), b, "  \xB7  ", a->facts.facts[2 * i + 1]);
        if (b[0]) {
            text(tx, ty, b, LF_SMALL, C_SUB, &bg_grad, tw);
            ty += FH + Y(4);
        }
    }
    ty += Y(10);
    album_lx = tx - X(14);
    album_ly = ty;
    album_lw = tw + X(14);
    album_lh = bottom - ty;
    hits_base = nhits;
    album_rows();
    now_inset();
    footer_keep(a->page.hint, a->page.message, FALSE);
}

/* ---------------------------------------------------------------- an artist */
static LONG strip_first, strip_cols = 1, strip_y;
static void draw_strip(const struct LArtist *a)
{
    LONG x0 = X(30), aw = W - x0 - X(30), bottom = body_bottom(), gap = X(18);
    LONG cs = X(110), cols, i, sel = a->page.list.sel, y = strip_y;
    const struct LList *l = &a->page.list;
    if (cs > bottom - y - FH * 2 - Y(12))
        cs = bottom - y - FH * 2 - Y(12);
    if (cs < FH * 2)
        cs = FH * 2;
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
    H->vgrad(0, (WORD)(y - Y(10)), W, (WORD)(bottom - y + Y(10)), grad(y - Y(10)), grad(bottom));
    horizon_line(X(30), y - Y(8), W - X(60));
    for (i = strip_first; i < l->count && i < strip_first + cols; i++) {
        const struct LItem *it = l->item(i);
        LONG cx = x0 + (i - strip_first) * (cs + gap);
        BOOL on = i == sel, hov = HOVER(HT_ROW, i);
        if (it->flags & LIF_HEAD)
            continue;
        hit(cx, y, cs, cs + FH * 2 + LABEL_GAP, HT_ROW, it->kind, i);
        if (on || hov)
            glow(cx - 2, y - 2, cs + 4, cs + 4, Y(4), grad(y));
        cover_at(it->art, it->label, cx, y, cs, cs, 0);
        if (on)
            H->frame((WORD)(cx - 1), (WORD)(y - 1), (WORD)(cs + 2), (WORD)(cs + 2), 2, C_ACC);
        text(cx, y + cs + LABEL_GAP, it->label, on ? LF_BOLD : LF_REGULAR, on ? C_WHITE : C_TEXT, &bg_grad, cs);
        if (it->tag || it->sub)
            text(cx, y + cs + LABEL_GAP + FH, it->tag ? it->tag : it->sub, LF_SMALL, C_SUB, &bg_grad, cs);
    }
}

static void draw_artist(void)
{
    const struct LArtist *a = H->model(LS_ARTIST);
    LONG x = X(30), y, ps = X(170), tx, tw, ty, i, np, bottom;
    background();
    page_head(a ? &a->page : NULL, FALSE);
    y = TITLE_Y + Y(8);
    if (!a)
        return;
    bottom = body_bottom();
    cover_at(a->art, a->name, x, y, ps, ps, ps / 5);
    tx = x + ps + X(36);
    tw = W - tx - X(30);
    ty = y;
    text(tx, ty, lower(a->name), LF_TITLE, C_WHITE, &bg_grad, tw);
    ty += TH + Y(4);
    if (a->line && *a->line) {
        text(tx, ty, a->line, LF_SMALL, C_ACC, &bg_grad, tw);
        ty += FH + Y(4);
    }
    for (i = 0; i < a->facts.n; i++) {
        char b[140];
        join(b, sizeof(b), a->facts.facts[2 * i], ": ", a->facts.facts[2 * i + 1]);
        text(tx, ty, b, LF_SMALL, C_SUB, &bg_grad, tw);
        ty += FH + Y(2);
    }
    strip_y = y + ps + ps / 5 + Y(24);
    if (strip_y > bottom - FH * 5)
        strip_y = bottom - FH * 5;
    np = paras(a->bio);
    if (np)
        draw_lines(para, np, 0, tx, ty + Y(8), tw, strip_y - Y(16), C_BODY, C_BODY);
    hits_base = nhits;
    draw_strip(a);
    now_inset();
    footer_keep(a->page.hint, a->page.message, FALSE);
}

/* ---------------------------------------------------------------- Now playing */
/* The cover large at the left on its reflection; at the right what plays,
 * its time, and the spectrum under them; the buttons along the bottom,
 * round and glassy */
static const ULONG bar_col[12] = {
    0x2F7BFF, 0x3A8CFF, 0x449CFF, 0x4FADFF, 0x5ABDFF, 0x63C9FF,
    0x6FD2FF, 0x7ADBFF, 0x86E3FF, 0x93EAFF, 0xA0F0FF, 0xB0F5FF
};
#define NBARS 48
static WORD bar_h[NBARS];
static ULONG bars_at;
static LONG bars_x, bars_y, bars_w, bars_h;
#define NOW_BH Y(40)

static void draw_bars(const struct LNow *n, BOOL full)
{
    LONG nb = n && n->nlevels >= 8 ? n->nlevels : 32, gap = 3, bw, i, fall, bottom = bars_y + bars_h;
    static const UBYTE shape[12] = { 9, 12, 15, 13, 11, 14, 10, 8, 12, 9, 7, 10 };
    BOOL spectrum = n && n->nlevels >= 8;
    if (bars_w <= 0 || bars_h <= 0)
        return;
    if (nb > NBARS)
        nb = NBARS;
    if (bars_w < nb * 4)
        nb = bars_w / 4;
    if (nb < 1)
        return;
    bw = (bars_w - (nb - 1) * gap) / nb;
    fall = bars_h / 20 > 1 ? bars_h / 20 : 1;
    if (full) {
        back_at(bars_x, bars_y, bars_x + bars_w, bottom);
        for (i = 0; i < NBARS; i++)
            bar_h[i] = 0;
    }
    for (i = 0; i < nb; i++) {
        LONG want = 0, old = bar_h[i], x = bars_x + i * (bw + gap);
        if (n && n->nlevels && n->levels && !n->paused && !n->stopped) {
            LONG lv = n->levels[i * n->nlevels / nb];
            want = spectrum ? bars_h * lv / 255 : bars_h * lv * shape[(i * 7) % 12] / (255 * 15);
            if (want < 2)
                want = 2;
        }
        if (want > bars_h)
            want = bars_h;
        if (want < old - fall)
            want = old - fall;
        if (want > old)             /* (one fill: a gradient a row is many calls a frame) */
            H->fill((WORD)x, (WORD)(bottom - want), (WORD)bw, (WORD)(want - old), bar_col[i * 12 / nb]);
        else if (want < old)        /* (the screen's own gradient: a part's own left marks) */
            back_at(x, bottom - old, x + bw, bottom - want);
        bar_h[i] = (WORD)want;
    }
}

/* A button's glyph, s tall, centred at cx, cy (as Showcase's) */
static void nb_glyph(int b, BOOL playing, LONG cx, LONG cy, LONG s, ULONG c)
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

/* The buttons: round, the transport in the middle, the volume at the right */
/* A button: a glass bead s across at x, y (big: Play's, the chosen blue),
 * a lit rim, a face lighter at its top, a gleam; foc: in its glow */
static void bead(LONG x, LONG y, LONG s, BOOL big, BOOL foc)
{
    ULONG bgc = grad(y + s / 2), face = big ? C_FOCUS : foc ? C_HOVER : C_BTN;
    ULONG rim = mix(C_ACC, face, 2, 5), outc = foc ? mix(mix(C_ACC, C_FOCUS, 1, 2), bgc, 1, 8) : OUT_GRAD;
    LONG gw = s * 3 / 5, gh = s / 4;
    if (foc)
        glow(x, y, s, s, s / 2, bgc);
    rsmooth(x, y, s, s, s / 2, mix(rim, C_WHITE, 1, 4), rim, outc);
    rsmooth(x + 1, y + 1, s - 2, s - 2, s / 2 - 1, mix(face, C_WHITE, 1, 5), mix(face, C_BOT, 1, 3), rim);
    rsmooth(x + (s - gw) / 2, y + s / 14 + 1, gw, gh, gh / 2, mix(face, C_WHITE, 2, 5), mix(face, C_WHITE, 1, 8),
            mix(face, C_WHITE, 1, 5));
}

static void now_buttons(const struct LNow *n, LONG by, LONG bh)
{
    static const signed char order[LNB_COUNT] = { LNB_PREV, LNB_BACK, LNB_PLAY, LNB_STOP, LNB_FWD,
                                                  LNB_NEXT, LNB_LOOP, LNB_MUTE, LNB_VDOWN, LNB_VUP };
    LONG gap = X(12), i, x, vw = X(64), big = bh + bh / 3;
    LONG tw = 5 * bh + big + 5 * gap, rx = W - X(28) - (4 * bh + 3 * gap + vw + gap);
    LONG x0 = (W - tw) / 2;
    H->vgrad(0, (WORD)(by - bh / 6 - X(8)), W, (WORD)(big + X(16)), grad(by - bh / 6 - X(8)),
             grad(by - bh / 6 + big + X(8)));
    if (x0 + tw + gap > rx)
        x0 = rx - gap - tw;
    for (i = 0; i < LNB_COUNT; i++) {
        int b = order[i];
        BOOL off = (n->off & LNB_BIT(b)) != 0, on = (n->on & LNB_BIT(b)) != 0;
        BOOL isbig = b == LNB_PLAY, foc = n->btn == b || HOVER(HT_BUTTON, b);
        LONG s = isbig ? big : bh, yy = by + bh / 2 - s / 2;
        ULONG fg = off ? C_DIM : on ? C_ACC : C_WHITE;
        if (i < 6) {
            x = x0;
            x0 += s + gap;
        } else {
            x = rx;
            rx += s + gap;
            if (b == LNB_VDOWN) {
                LONG vy = by + bh / 2 - 1, f = vw * n->volume / 64;
                H->fill((WORD)rx, (WORD)vy, (WORD)vw, 2, C_FAINT);
                if (f > 0)
                    H->fill((WORD)rx, (WORD)vy, (WORD)f, 2, (n->on & LNB_BIT(LNB_MUTE)) ? C_DIM : C_ACC);
                rsmooth(rx + f - 4, vy - 3, 9, 8, 4, C_WHITE, C_SUB, OUT_GRAD);
                rx += vw + gap;
            }
        }
        bead(x, yy, s, isbig, foc);
        nb_glyph(b, isbig && !n->paused && !n->stopped, x + s / 2, yy + s / 2, s * 2 / 5, fg);
        if (!off)
            hit(x, yy, s, s, HT_BUTTON, 0, b);
    }
}

/* Now playing's time and its bar (drawn again alone as it goes: LC_PLAYING) */
static LONG nt_x, nt_y = -1, nt_w;
static void now_time(const struct LNow *n, BOOL with_hit)
{
    LONG tx = nt_x, ty = nt_y, tw = nt_w;
    char b1[16], b2[16];
    if (ty < 0)
        return;
    if (!with_hit)
        back_at(tx, ty - Y(4), tx + tw, ty + FH + Y(4));
    if (!n->stopped) {
        const char *a = mmss(n->pos_ms, b1), *z = n->len_ms ? mmss(n->len_ms, b2) : "";
        LONG aw = tnum_len(a, LF_SMALL), zw = tnum_len(z, LF_SMALL), bx = tx + aw + X(10);
        LONG bw = tx + tw - zw - X(10) - bx, bby = ty + FH / 2 - 1;
        tnum(tx, ty, a, LF_SMALL, C_BODY, &bg_grad);
        if (bw > 8) {
            if (n->len_ms && with_hit)
                hit(bx, ty - Y(2), bw, FH + Y(4), HT_SEEK, 0, 0);
            H->fill((WORD)bx, (WORD)bby, (WORD)bw, 2, C_FAINT);
            if (n->len_ms) {
                LONG f = (LONG)((n->pos_ms / 100) * bw / (n->len_ms / 100 ? n->len_ms / 100 : 1));
                if (f > bw)
                    f = bw;
                if (f > 0) {
                    H->fill((WORD)bx, (WORD)bby, (WORD)f, 2, C_ACC);
                    rshape(bx + f - 4, bby - 3, 8, 8, 4, C_WHITE);
                }
            }
        }
        tnum(tx + tw - zw, ty, z, LF_SMALL, C_BODY, &bg_grad);
    } else {
        text(tx, ty, lower(H->tr("Stopped: Play starts it again")), LF_SMALL, C_SUB, &bg_grad, tw);
    }
}

/* ---------------------------------------------------------------- a slideshow's and a video's controls
 * Along the foot of the screen, as Now playing's: the gradient there and a
 * horizon over it; what is shown, and a video's time and bar, in a line;
 * under it the buttons, glass beads, the transport in the middle, the
 * volume and full screen at the right. */
#define CT_BH Y(34)
static LONG ct_tx, ct_ty = -1, ct_tw;   /* the time row, as drawn */
static WORD hz_controls_h(BOOL timed)
{
    (void)timed;
    return (WORD)(Y(14) + FH + Y(14) + CT_BH / 6 + CT_BH + CT_BH / 6 + Y(12));
}

static BOOL ct_right(int b)
{
    return b == LNB_LOOP || b == LNB_MUTE || b == LNB_VDOWN || b == LNB_VUP || b == LNB_FULL;
}

/* A video's time: where it is, a bar, how long (the bar: its seek place) */
static void ct_time(struct LControls *c)
{
    char b1[16], b2[16];
    const char *a = mmss(c->at_ms, b1), *z = c->total_ms ? mmss(c->total_ms, b2) : "";
    LONG aw = tnum_len(a, LF_SMALL), zw = tnum_len(z, LF_SMALL), bx = ct_tx + aw + X(10);
    LONG bw = ct_tx + ct_tw - zw - X(10) - bx, bby = ct_ty + FH / 2 - 1;
    tnum(ct_tx, ct_ty, a, LF_SMALL, C_BODY, &bg_grad);
    c->seek_w = 0;
    if (bw > 8) {
        H->fill((WORD)bx, (WORD)bby, (WORD)bw, 2, C_FAINT);
        if (c->total_ms) {
            LONG f = (LONG)((c->at_ms / 100) * bw / (c->total_ms / 100 ? c->total_ms / 100 : 1));
            if (f > bw)
                f = bw;
            if (f > 0)
                H->fill((WORD)bx, (WORD)bby, (WORD)f, 2, C_ACC);
            rshape(bx + f - 4, bby - 3, 8, 8, 4, C_WHITE);
            c->seek_x = (WORD)bx;
            c->seek_y = (WORD)(ct_ty - Y(2));
            c->seek_w = (WORD)bw;
            c->seek_h = (WORD)(FH + Y(4));
        }
    }
    if (*z)
        tnum(ct_tx + ct_tw - zw, ct_ty, z, LF_SMALL, C_BODY, &bg_grad);
}

static void hz_controls(struct LControls *c, ULONG what)
{
    LONG x0 = c->x, y0 = c->y, w = c->w, ty = y0 + Y(14), bh = CT_BH, big = bh + bh / 3;
    LONG by = ty + FH + Y(14) + bh / 6, gap = X(12), vw = X(64), cw = 0, rw = 0, i, x, rx, lx = x0 + X(28);
    if (what == LC_PLAYING) {
        if (c->timed && ct_ty >= 0) {   /* the time alone, over what was there (clear of the glows) */
            back_at(ct_tx, ct_ty - Y(4), ct_tx + ct_tw, ct_ty + FH + Y(2));
            ct_time(c);
        }
        return;
    }
    back_at(x0, y0, x0 + w, y0 + c->h);
    horizon_line(x0, y0, w);
    /* the line: what it is, then the time, or the picture's details at the right */
    {
        LONG tw = tlen(c->title ? c->title : "", LF_BOLD), most = c->timed ? w * 35 / 100 : w / 2;
        if (tw > most)
            tw = most;
        text(lx, ty, c->title, LF_BOLD, C_TEXT, &bg_grad, tw);
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
                text(x0 + w - X(28) - dw, ty, c->detail, LF_SMALL, C_SUB, &bg_grad, dw);
        }
    }
    /* the buttons: the transport's width, and the right's (the volume between - and +) */
    for (i = 0; i < c->n; i++) {
        LONG s = c->btn[i] == LNB_PLAY ? big : bh;
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
        BOOL off = (c->off & LNB_BIT(b)) != 0, on = (c->on & LNB_BIT(b)) != 0, isbig = b == LNB_PLAY;
        BOOL foc = !off && (c->focus == i || c->hover == i);
        LONG s = isbig ? big : bh, yy = by + bh / 2 - s / 2, bx;
        ULONG fg = off ? C_DIM : on && b != LNB_FULL ? C_ACC : C_WHITE;
        if (ct_right(b)) {
            bx = rx;
            rx += s + gap;
            if (b == LNB_VDOWN && c->has_volume) {
                LONG vy = by + bh / 2 - 1, f = vw * c->volume / 64;
                H->fill((WORD)rx, (WORD)vy, (WORD)vw, 2, C_FAINT);
                if (f > 0)
                    H->fill((WORD)rx, (WORD)vy, (WORD)f, 2, C_ACC);
                rsmooth(rx + f - 4, vy - 3, 9, 8, 4, C_WHITE, C_SUB, OUT_GRAD);
                rx += vw + gap;
            }
        } else {
            bx = x;
            x += s + gap;
        }
        bead(bx, yy, s, isbig, foc);
        nb_glyph(b, b == LNB_FULL ? on : isbig && !c->paused, bx + s / 2, yy + s / 2, s * 2 / 5, fg);
        c->at[i].x = (WORD)bx;
        c->at[i].y = (WORD)yy;
        c->at[i].w = (WORD)s;
        c->at[i].h = (WORD)s;
    }
}

static void draw_now(void)
{
    const struct LNow *n = H->model(LS_NOW);
    LONG cs = Y(290), x = X(46), y, tx, tw, ty, bh = NOW_BH, by = HT - Y(22) - bh, refl;
    char line[200];
    nt_y = -1;
    background();
    top_left();
    top_right();
    text(logo_r + X(20), LOGO_Y + logo_h / 2 - FH / 2, lower(H->tr("Now playing")), LF_SMALL, C_SUB, &bg_grad,
         W / 2);
    y = TITLE_Y + Y(20);
    if (cs > by - Y(30) - y)
        cs = by - Y(30) - y;
    refl = cs / 4;
    if (n && n->kind != LN_NONE) {
        cover_at(n->art, n->title, x, y, cs, cs, refl);
        hit(x, y, cs, cs, HT_PAUSE, 0, 0);
    }
    tx = x + cs + X(40);
    tw = W - tx - X(40);
    bars_w = 0;
    if (!n || n->kind == LN_NONE) {
        text(tx, y, lower(H->tr("Nothing is playing")), LF_TITLE, C_SUB, &bg_grad, tw);
        return;
    }
    ty = y + Y(6);
    text(tx, ty, n->title, LF_TITLE, C_WHITE, &bg_grad, tw);
    ty += TH + Y(4);
    if (n->artist && *n->artist) {
        text(tx, ty, n->artist, LF_REGULAR, C_ACC, &bg_grad, tw);
        ty += FH + Y(2);
    }
    join(line, sizeof(line), n->album, "  \xB7  ", n->year);
    if (line[0]) {
        text(tx, ty, line, LF_REGULAR, C_SUB, &bg_grad, tw);
        ty += FH + Y(2);
    }
    ty += Y(14);
    nt_x = tx;
    nt_y = ty;
    nt_w = tw;
    now_time(n, TRUE);
    ty += FH + Y(10);
    line[0] = 0;
    if (n->nnext > 0)
        join(line, sizeof(line), lower(H->tr("Next:")), " ", n->next[0]);
    join(line, sizeof(line), line, "  \xB7  ", n->format);
    text(tx, ty, line, LF_SMALL, C_DIM, &bg_grad, tw);
    ty += FH + Y(16);
    /* the spectrum, in the rest of the column */
    bars_x = tx;
    bars_y = ty;
    bars_w = tw;
    bars_h = y + cs - ty;
    if (bars_h > Y(130))
        bars_h = Y(130);
    if (bars_h < Y(30))
        bars_w = 0;
    draw_bars(n, TRUE);
    bars_at = H->now_ms();
    if (bars_w)
        hit(bars_x, bars_y, bars_w, bars_h, HT_PAUSE, 0, 0);
    now_buttons(n, by, bh);
}

/* ---------------------------------------------------------------- what is under the band */
static void redraw_under_band(void)
{
    const void *m;
    LONG y0 = band_y() - X(10);
    if (drawn == LS_ALBUMS && (m = H->model(LS_ALBUMS))) {
        draw_wall_body(m);
        now_inset();
        return;
    }
    if (drawn == LS_PAGE && (m = H->model(LS_PAGE)) && !((const struct LPage *)m)->text) {
        draw_page_body(m);
        now_inset();
        return;
    }
    if (drawn == LS_ALBUM) {
        album_rows();
        now_inset();
        return;
    }
    if (drawn == LS_ARTIST && (m = H->model(LS_ARTIST))) {
        draw_strip(m);
        now_inset();
        return;
    }
    H->vgrad(0, (WORD)y0, (WORD)(inset_top() < HT ? W - X(24) - INSET_W - X(4) : W), (WORD)(HT - y0),
             grad(y0), C_BOT);
}

/* ---------------------------------------------------------------- the engine */
static BOOL hz_open(const struct LayoutHost *host)
{
    H = host;
    dcell[0] = dcell[1] = dcell[2] = dcell[3] = 0;
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
    home_sec = -1;
    return TRUE;
}

static void hz_close(void)
{
    H = NULL;
}

static void hz_draw(int screen)
{
    drawn = screen;
    pb_x = -1;                      /* (the playing bars: where top_right puts them, if it does) */
    inset_off = FALSE;
    ft_screen = FALSE;
    nhits = 0;
    hits_base = 0;
    nsbar = 0;
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

static void hz_changed_body(int screen, ULONG what)
{
    const struct LPage *m;
    if (what == LC_WAIT) {
        if (screen == drawn && (screen == LS_PAGE || screen == LS_ALBUMS) && spin_x >= 0 &&
            (m = H->model(screen)) && m->waiting) {
            H->clip(0, 0, W, HT);
            wait_spinner(m);
        }
        return;
    }
    if (what == LC_PLAYING && screen == drawn) {
        /* the time went on: it alone (the clock with it) */
        const struct LNow *n = H->model(LS_NOW);
        H->clip(0, 0, W, HT);
        top_right_again();
        if (screen == LS_NOW) {
            if (n && n->kind != LN_NONE)
                now_time(n, FALSE);
            return;
        }
        if (screen != LS_HOME || n) {
            now_inset_as(FALSE);
            return;
        }
    }
    if (what == LC_IMAGE && screen == drawn) {
        H->clip(0, 0, W, HT);
        if (screen == LS_ALBUMS && (m = H->model(LS_ALBUMS)) && wall_count == m->list.count) {
            wall_pictures(m);           /* the covers that waited for them */
            return;
        }
        if (screen == LS_HOME) {
            const struct LHome *h = H->model(LS_HOME);
            if (h && h->sel == home_sec) {
                nhits = hits_base;
                home_tiles(h);
                return;
            }
        }
        if (screen == LS_PAGE && (m = H->model(LS_PAGE)) && !m->text && body_n >= 0 &&
            page_split(m) == body_n) {
            /* the details' picture came: the column alone (was a whole
             * draw: 370 ms without the JIT, each game) */
            col_key = ~0UL;
            draw_page_body(m);
            now_inset();
            return;
        }
    }
    if (what == LC_DETAILS && screen == drawn && screen == LS_PAGE && (m = H->model(LS_PAGE)) && !m->text &&
        body_n >= 0 && page_split(m) == body_n) {
        H->clip(0, 0, W, HT);
        draw_page_body(m);              /* the details came: the body alone, as a move draws it */
        now_inset();
        return;
    }
    if (what == LC_SELECT && screen == drawn) {
        H->clip(0, 0, W, HT);
        switch (screen) {
        case LS_HOME: {
            const struct LHome *h = H->model(LS_HOME);
            if (h && h->sel == home_sec && (h->tile_sel >= 0) == home_tmode) {
                home_place(h);          /* (the row glides there in the ticks) */
                nhits = hits_base;
                home_tiles(h);
                return;
            }
            break;
        }
        case LS_ALBUMS:
            if ((m = H->model(LS_ALBUMS)) && m->list.count && wall_count == m->list.count) {
                wall_moved(m);          /* (the covers it touches: the glide follows) */
                return;
            }
            break;
        case LS_ALBUM:
            album_rows();
            return;
        case LS_ARTIST: {
            const struct LArtist *a = H->model(LS_ARTIST);
            if (a) {
                draw_strip(a);
                return;
            }
            break;
        }
        case LS_PAGE:
            if (!(m = H->model(LS_PAGE)))
                break;
            if (m->text && body_n < 0) {
                draw_text_body_as(m, FALSE);
                return;
            }
            if (!m->text && body_n >= 0 && page_split(m) == body_n) {
                draw_page_body(m);
                now_inset();
                return;
            }
            break;
        }
    }
    hz_draw(screen);
}
static void hz_changed(int screen, ULONG what)
{
    hz_changed_body(screen, what);
    if (ft_screen && screen == drawn)
        footer_draw(FALSE);
}

/* the glides (the wall, Home's row), toasts ending, Now playing's bars */
/* A glide: from where it was to where it is going in GLIDE_MS, fast at
 * first and settling (however long each frame takes to draw: a slow
 * screen takes fewer steps, not longer). A new goal starts one from
 * where it is. */
#define GLIDE_MS 180
struct Glide { LONG from, to; ULONG t0; };
static LONG glide(struct Glide *g, LONG at, LONG to, ULONG ms)
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
static struct Glide wall_g, home_g;
static ULONG pm_at;

/* The playing mark's bars from what plays: the spectrum's low, middle and
 * high thirds (or the meters); falling a little a step. TRUE if moved. */
static BOOL pm_step(const struct LNow *n)
{
    UBYTE want[3];
    LONG k, i;
    BOOL moved = FALSE;
    if (!n->levels || !n->nlevels || n->paused || n->stopped) {
        want[0] = 66;
        want[1] = 100;
        want[2] = 50;
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
static void hz_tick(ULONG ms)
{
    if (drawn == LS_ALBUMS && wall_px != wall_tgt) {
        const struct LPage *m = H->model(LS_ALBUMS);
        LONG old = wall_px;
        wall_px = glide(&wall_g, wall_px, wall_tgt, ms);
        if (wall_px != wall_tgt)
            wall_px &= ~3L;             /* (whole dither steps, as its goal) */
        if (m) {
            H->clip(0, 0, W, HT);
            wall_glide_step(m, old);    /* (what is drawn moved along; the strip that comes in) */
        }
    }
    if (drawn == LS_HOME && home_px != home_tgt) {
        const struct LHome *m = H->model(LS_HOME);
        LONG old = home_px;
        home_px = glide(&home_g, home_px, home_tgt, ms);
        if (home_px != home_tgt)
            home_px &= ~3L;             /* (whole dither steps, as its goal) */
        if (m) {
            H->clip(0, 0, W, HT);
            home_glide_step(m, old);    /* (what is drawn moved along; the strip that comes in) */
        }
    }
    if (pb_x >= 0 && (LONG)(ms - pm_at) >= 100) {
        /* the top right's playing bars, as the sound goes: they alone */
        const struct LNow *n = H->model(LS_NOW);
        pm_at = ms;
        if (n && pm_step(n)) {
            LONG bw = 3 * (pb_s / 4 > 1 ? pb_s / 4 : 2) + 4;
            back_at(pb_x, pb_y, pb_x + bw, pb_y + pb_s);
            play_bars(pb_x + bw, pb_y, pb_s, pm_lv);
        }
    }
    if (ft_screen)
        footer_draw(FALSE);
    if (drawn != LS_NOW || (LONG)(ms - bars_at) < 40)
        return;
    bars_at = ms;
    H->clip(0, 0, W, HT);
    draw_bars(H->model(LS_NOW), FALSE);
}

/* The keys: the program's, but on Home's tiles Left and Right move along
 * the row and Up and Down to the sections above and below; on the wall
 * Up and Down move in a column, Left and Right a column; on an artist's
 * strip Left and Right */
/* A key held on a wall (its repeats coming close together): after a few
 * moves, several at a time; held longer, a letter at a time (to the first
 * of the next letter, or back to the first of this one, then the last) */
static ULONG acc_ms;
static int acc_dir, acc_run;
static int accel(int dir)
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
static char initial(const struct LList *l, LONG i)
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
static LONG letter_jump(const struct LList *l, LONG sel, int dir)
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

static int hz_input(int screen, int action, LONG arg)
{
    const struct LPage *m;
    LONG sel, n, to;
    (void)arg;
    hov_what = -1;
    hov_arg = -1;
    ft_key = H->now_ms();
    if (screen == LS_HOME) {
        const struct LHome *h = H->model(LS_HOME);
        if (!h || h->tile_sel < 0 || !h->nrows)
            return LR_UNUSED;
        sel = h->tile_sel;
        n = h->rows[0].n;
        switch (action) {
        case LA_LEFT:
            if (sel == 0)
                return LR_UNUSED;           /* (as Back: to the sections) */
            H->act(LAC_SELECT, sel - 1);
            return LR_DONE;
        case LA_RIGHT:
            if (sel + 1 < n)
                H->act(LAC_SELECT, sel + 1);
            return LR_DONE;
        case LA_UP:
            if (h->sel > 0)
                H->act(LAC_SECTION, h->sel - 1);   /* (the section above, the menu in use) */
            return LR_DONE;
        case LA_DOWN:
            if (h->sel + 1 < h->n)
                H->act(LAC_SECTION, h->sel + 1);
            return LR_DONE;
        }
        return LR_UNUSED;
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
        if (screen == LS_ALBUMS ? sel < wall_rows : sel == 0)
            return LR_UNUSED;               /* the left edge: back, as ever */
        if (screen == LS_ALBUMS) {
            int run = accel(-1);
            to = run >= 10 ? letter_jump(&m->list, sel, -1) : sel - wall_rows * (run >= 4 ? 3 : 1);
            if (to < 0)
                to = 0;
            if (run >= 10)
                wall_lead = to;             /* (a letter: it leads the wall, at the left) */
        } else {
            to = sel - 1;
        }
        break;
    case LA_RIGHT:
        if (screen == LS_ALBUMS) {
            int run = accel(1);
            if (run >= 10)
                wall_lead = to = letter_jump(&m->list, sel, 1);
            else if (sel + wall_rows * (run >= 4 ? 3 : 1) < n)
                to = sel + wall_rows * (run >= 4 ? 3 : 1);
            else if (sel / wall_rows < (n - 1) / wall_rows)
                to = n - 1;                 /* (into a short last column) */
        } else if (sel + 1 < n) {
            to = sel + 1;
        }
        break;
    case LA_UP:
        if (screen != LS_ALBUMS)
            return LR_UNUSED;
        if (sel % wall_rows)
            to = sel - 1;
        break;
    case LA_DOWN:
        if (screen != LS_ALBUMS)
            return LR_UNUSED;
        if (sel % wall_rows < wall_rows - 1 && sel + 1 < n)
            to = sel + 1;
        break;
    default:
        return LR_UNUSED;
    }
    if (to != sel)
        H->act(LAC_SELECT, to);
    return LR_DONE;
}

static BOOL playable(int kind)
{
    return kind == LI_TRACK || kind == LI_MODULE || kind == LI_STATION || kind == LI_PICTURE ||
           kind == LI_VIDEO || kind == LI_GAME || kind == LI_DISC;
}

/* The pointer's hover moved: what shows it drawn again */
static int hov_was = -1;        /* what the pointer was over (HT_*), before this move */
static void hover_redraw(int screen)
{
    const void *m;
    if (screen != drawn || !(m = H->model(screen)))
        return;
    H->clip(0, 0, W, HT);
    switch (screen) {
    case LS_HOME:
        if (hov_was != HT_SECTION && hov_what != HT_SECTION && home_sec == ((const struct LHome *)m)->sel) {
            nhits = hits_base;          /* (a tile's, or none: the row alone) */
            home_tiles(m);
        } else {
            hz_draw(LS_HOME);
        }
        break;
    case LS_PAGE:
        if (body_n >= 0 && !((const struct LPage *)m)->text)
            draw_page_body(m);
        now_inset();
        break;
    case LS_ALBUMS:
        if (((const struct LPage *)m)->list.count && wall_count == ((const struct LPage *)m)->list.count)
            wall_moved(m);              /* (the covers under the pointer, before and now) */
        else
            draw_wall_body(m);
        break;
    case LS_ALBUM:
        album_rows();
        now_inset();
        break;
    case LS_ARTIST:
        draw_strip(m);
        now_inset();
        break;
    case LS_NOW: {
        LONG by = HT - Y(22) - NOW_BH;
        now_buttons(m, by, NOW_BH);
        break;
    }
    }
    if (ft_shown)
        footer_paint(ft_shown);
}

/* Home's row taken by the mouse: dragged along, flung (it glides on the
 * way it was going), or, not moved, a click on the tile pressed */
static BOOL strip_held, strip_moved;
static LONG strip_x0, strip_px0, strip_tile = -1, strip_lx, strip_v;
static int strip_kind;
static ULONG strip_lt;
static void strip_follow(LONG x)
{
    const struct LHome *m = H->model(LS_HOME);
    ULONG now = H->now_ms();
    LONG px, mx;
    if (!m)
        return;
    if (!strip_moved && (x - strip_x0 > X(6) || strip_x0 - x > X(6)))
        strip_moved = TRUE;
    if (!strip_moved)
        return;
    if (now != strip_lt)            /* (pixels a 64th of a second, the last move's) */
        strip_v = (strip_lx - x) * 16 / (LONG)(now - strip_lt > 0 ? now - strip_lt : 1);
    strip_lx = x;
    strip_lt = now;
    mx = home_max(m);
    px = strip_px0 - (x - strip_x0);
    if (px < 0)
        px = 0;
    if (px > mx)
        px = mx;
    px &= ~3L;                      /* (whole dither steps) */
    if (px == home_px)
        return;
    {
        LONG old = home_px;
        home_px = home_tgt = px;
        H->clip(0, 0, W, HT);
        home_glide_step(m, old);    /* (dragged: moved along, the strip that comes in drawn) */
    }
}

static int hz_pointer(int screen, WORD x, WORD y, BOOL click)
{
    const struct Hit *t = NULL;
    int i;
    BOOL dbl;
    ULONG now;
    if (!H)
        return LR_UNUSED;
    if (click == LP_RELEASE) {
        if (sb_drag >= 0) {
            sb_drag = -1;
            return LR_DONE;
        }
        if (strip_held) {
            const struct LHome *m = H->model(LS_HOME);
            strip_held = FALSE;
            if (!strip_moved) {
                if (strip_tile >= 0) {      /* a click on a tile */
                    H->act(LAC_ENTER, 0);
                    H->act(playable(strip_kind) ? LAC_SELECT : LAC_OPEN, strip_tile);
                }
            } else if (m && H->now_ms() - strip_lt < 120) {
                LONG to = home_px + strip_v * 12, mx = home_max(m);   /* (flung: on a little way) */
                home_tgt = (to < 0 ? 0 : to > mx ? mx : to) & ~3L;
            }
            return LR_DONE;
        }
        return LR_UNUSED;
    }
    if (!click && strip_held) {
        strip_follow(x);
        return LR_DONE;
    }
    if (!click && sb_drag >= 0) {
        scroll_to(sbar[sb_drag].across ? x : y);
        return LR_DONE;
    }
    if (!click) {
        int w = -1;
        LONG a = -1;
        for (i = nhits - 1; i >= 0; i--)
            if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) {
                if (hits[i].what == HT_ROW || hits[i].what == HT_TILE || hits[i].what == HT_SECTION ||
                    hits[i].what == HT_BUTTON || hits[i].what == HT_NOWPLAYING) {
                    w = hits[i].what;
                    a = hits[i].arg;
                }
                break;
            }
        if (w == hov_what && a == hov_arg)
            return LR_UNUSED;
        hov_was = hov_what;
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
        LONG p = sbar[k].across ? x : y;
        if (p < sbar[k].t || p >= sbar[k].t + sbar[k].tl) {
            scroll_page(k, p < sbar[k].t ? -1 : 1);     /* off the thumb: a page that way */
            return LR_DONE;
        }
        sb_drag = k;
        sb_off = p - sbar[k].t;
        return LR_DONE;
    }
    if (screen == LS_HOME && (t->what == HT_TILE || t->what == HT_STRIP)) {
        strip_held = TRUE;          /* (what it is: seen at its release) */
        strip_moved = FALSE;
        strip_x0 = strip_lx = x;
        strip_px0 = home_px;
        strip_lt = H->now_ms();
        strip_v = 0;
        strip_tile = t->what == HT_TILE ? t->arg : -1;
        strip_kind = t->kind;
        return LR_DONE;
    }
    now = H->now_ms();
    dbl = (t->what == HT_ROW || t->what == HT_TILE) && t->arg == click_row && now - click_ms < 500;
    click_row = t->what == HT_ROW || t->what == HT_TILE ? t->arg : -1;
    click_ms = now;
    switch (t->what) {
    case HT_SECTION:
        home_tgt = 0;               /* (its row back to its start, gliding) */
        H->act(LAC_SECTION, t->arg);
        H->act(LAC_ENTER, 0);
        click_row = -1;
        break;
    case HT_TILE:
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
    case HT_CRUMB: {
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
    case HT_HOME:
        H->act(LAC_HOME, 0);
        break;
    case HT_BUTTON:
        H->act(LAC_BUTTON, t->arg);
        break;
    case HT_SEEK:
        H->act(LAC_SEEK, (x - t->x) * 65536 / (t->w > 0 ? t->w : 1));
        break;
    }
    return LR_DONE;
}

const struct LayoutEngine layout_engine = {
    LAYOUT_ABI, "Horizon", 15, 480,
    LS_BIT(LS_HOME) | LS_BIT(LS_PAGE) | LS_BIT(LS_NOW) | LS_BIT(LS_ALBUMS) | LS_BIT(LS_ALBUM) |
        LS_BIT(LS_ARTIST),
    hz_open, hz_close, hz_draw, hz_changed, hz_tick, hz_input, hz_pointer, hz_controls_h, hz_controls
};
