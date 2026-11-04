/*
 * AmigaMCE - smooth style: anti-aliased text, icons, markers and logo
 * Copyright (c) 2026 Brian King
 *
 * Used on RTG screens of 15 bits or more. Everything is pre-rendered by
 * tools/mksmooth.py at the exact size this screen needs, from Rubik and the
 * vector icon shapes, into PROGDIR:Fonts/amce-smooth/<lines>; nothing is
 * scaled or rasterised here.
 *
 * Glyphs, icons and markers are coverage maps (4 bits a pixel). Drawing one
 * sorts its pixels into SMOOTH_LEVELS - 1 one-bit masks by coverage step
 * and stamps each with BltTemplate in a pen whose colour lies that far
 * between the foreground and the background: the same path as the pixel
 * style's text, so it clips to the window like any other drawing and needs
 * no RTG library calls. The in-between pens are taken from 32 up as each
 * (foreground, background) pair is first used; on a truecolour screen a
 * pen is only a colour to draw with, so this costs nothing on screen.
 *
 * The logo is a small palette image: one mask per colour, like the pixel
 * style's logo.
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <graphics/view.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>

#define NFACES      3
#define GLYPH_BYTES 10
#define FIRST_PEN   32          /* below: UI pens 0-15, the screen's 16-31 */
#define MAX_TEXT_W  2048        /* widest text drawn in one go, in pixels */

struct Face {
    UWORD lo, hi;
    const UBYTE *gl;            /* GLYPH_BYTES per character lo..hi */
    const UBYTE *blob;          /* coverage data */
    UWORD widest_cap;           /* widest A-Z, in pixels */
};

struct Smooth {
    UBYTE *data;
    ULONG size;
    UWORD k, cell_h, base;
    struct Face face[NFACES];
    UWORD icon_n, icon_cnt, mark_n, mark_cnt;
    const UBYTE *icons, *marks;
    ULONG icon_bytes, mark_bytes;
    UWORD logo_w, logo_h, logo_nc;
    const UBYTE *logo_pal, *logo_idx;
    /* while a screen is open */
    struct Gfx *g;
    UBYTE *scratch;             /* chip RAM: SMOOTH_LEVELS masks */
    ULONG scratch_size;
    UWORD bpr;                  /* of the masks being built */
    ULONG plane;                /* bytes per mask */
    UBYTE *pl[SMOOTH_LEVELS];   /* each mask */
    UBYTE used;                 /* bit n: mask n has pixels */
    UBYTE blend[NUM_UI_PENS][NUM_UI_PENS][SMOOTH_LEVELS];
    UWORD next_pen, last_pen;
};

/* coverage 0..15 -> step 0..SMOOTH_LEVELS-1 */
static const UBYTE step_of[16] = { 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7 };

static UWORD rd16(const UBYTE *p) { return (UWORD)((p[0] << 8) | p[1]); }
static ULONG rd32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static UBYTE nib(const UBYTE *d, ULONG i)
{
    UBYTE b = d[i >> 1];
    return (i & 1) ? (b & 15) : (b >> 4);
}

/* ------------------------------------------------------------ loading */
static BOOL parse(struct Smooth *s)
{
    const UBYTE *p = s->data, *end = s->data + s->size;
    int f;
    if (s->size < 14 || memcmp(p, "AMSM", 4) || rd16(p + 4) != 1 || rd16(p + 12) != NFACES)
        return FALSE;
    s->k = rd16(p + 6);
    s->cell_h = rd16(p + 8);
    s->base = rd16(p + 10);
    p += 14;
    for (f = 0; f < NFACES; f++) {
        struct Face *fc = &s->face[f];
        ULONG n;
        UBYTE c;
        if (p + 4 > end)
            return FALSE;
        fc->lo = rd16(p);
        fc->hi = rd16(p + 2);
        if (fc->hi < fc->lo || fc->lo > '?' || fc->hi < 'Z')
            return FALSE;
        p += 4;
        fc->gl = p;
        p += (ULONG)(fc->hi - fc->lo + 1) * GLYPH_BYTES;
        if (p + 4 > end)
            return FALSE;
        n = rd32(p);
        p += 4;
        fc->blob = p;
        p += n;
        if (p > end)
            return FALSE;
        fc->widest_cap = 0;
        for (c = 'A'; c <= 'Z'; c++) {
            UBYTE w = fc->gl[(c - fc->lo) * GLYPH_BYTES + 3];
            if (w > fc->widest_cap)
                fc->widest_cap = w;
        }
    }
    if (p + 4 > end)
        return FALSE;
    s->icon_n = rd16(p);
    s->icon_cnt = rd16(p + 2);
    p += 4;
    s->icons = p;
    s->icon_bytes = ((ULONG)s->icon_n * s->icon_n + 1) / 2;
    p += s->icon_bytes * s->icon_cnt;
    if (p + 4 > end)
        return FALSE;
    s->mark_n = rd16(p);
    s->mark_cnt = rd16(p + 2);
    p += 4;
    s->marks = p;
    s->mark_bytes = ((ULONG)s->mark_n * s->mark_n + 1) / 2;
    p += s->mark_bytes * s->mark_cnt;
    if (p + 6 > end)
        return FALSE;
    s->logo_w = rd16(p);
    s->logo_h = rd16(p + 2);
    s->logo_nc = rd16(p + 4);
    p += 6;
    s->logo_pal = p;
    p += 3 * (ULONG)s->logo_nc;
    s->logo_idx = p;
    p += (ULONG)s->logo_w * s->logo_h;
    return p <= end && s->icon_cnt >= ICON_COUNT && s->mark_cnt >= MARK_RADIO_ON &&
           s->logo_nc <= 32;
}

struct Smooth *sm_load(UWORD lines)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    struct Smooth *s;
    char path[48];
    BPTR fh;
    LONG size;

    sprintf(path, "PROGDIR:Fonts/amce-smooth/%lu", (unsigned long)lines);
    me->pr_WindowPtr = (APTR)-1;            /* no "insert volume" requester */
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh) {
        amce_log("smooth style: no file", path);
        return NULL;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    s = AllocVec(sizeof(*s), MEMF_ANY | MEMF_CLEAR);
    if (s && size > 0 && (s->data = AllocVec(size, MEMF_ANY))) {
        s->size = size;
        if (Read(fh, s->data, size) == size && parse(s)) {
            Close(fh);
            amce_log("smooth style loaded", path);
            return s;
        }
    }
    Close(fh);
    amce_log("smooth style: bad file", path);
    sm_free(s);
    return NULL;
}

void sm_free(struct Smooth *s)
{
    if (!s)
        return;
    sm_detach(s);
    if (s->data)
        FreeVec(s->data);
    FreeVec(s);
}

void sm_attach(struct Smooth *s, struct Gfx *g)
{
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    UWORD count = cm ? cm->Count : 0;
    s->g = g;
    memset(s->blend, 0, sizeof(s->blend));
    s->next_pen = FIRST_PEN;
    s->last_pen = count > 256 ? 255 : count ? count - 1 : 0;
}

void sm_detach(struct Smooth *s)
{
    if (s->scratch) {
        WaitBlit();
        FreeMem(s->scratch, s->scratch_size);
    }
    s->scratch = NULL;
    s->scratch_size = 0;
    s->g = NULL;
}

UWORD sm_icon_px(const struct Smooth *s) { return s->icon_n; }
UWORD sm_mark_px(const struct Smooth *s) { return s->mark_n; }
UWORD sm_cell_h(const struct Smooth *s) { return s->cell_h; }
UWORD sm_scale(const struct Smooth *s) { return s->k; }
UWORD sm_baseline(const struct Smooth *s) { return s->base; }

/* ------------------------------------------------------------ pens */
static ULONG ui_rgb(UBYTE pen, int c)
{
    return ui_palette32[1 + pen * 3 + c] >> 24;
}

static UBYTE alloc_rgb(struct Smooth *s, ULONG r, ULONG gg, ULONG b)
{
    UBYTE pen;
    if (!s->g || s->next_pen > s->last_pen)
        return 0;
    pen = (UBYTE)s->next_pen++;
    SetRGB32(&s->g->scr->ViewPort, pen, r * 0x01010101UL, gg * 0x01010101UL, b * 0x01010101UL);
    return pen;
}

/* The pen for coverage step lv of fg drawn over bg. */
UBYTE sm_pen(struct Smooth *s, struct Gfx *g, UBYTE fg, UBYTE bg, UBYTE lv)
{
    UBYTE *slot, pen;
    int c;
    ULONG rgb[3];
    (void)g;
    if (lv == 0)
        return bg;
    if (lv >= SMOOTH_LEVELS - 1 || fg == bg)
        return fg;
    if (fg >= NUM_UI_PENS || bg >= NUM_UI_PENS)
        return lv * 2 >= SMOOTH_LEVELS - 1 ? fg : bg;
    slot = &s->blend[fg][bg][lv];
    if (*slot)
        return *slot;
    for (c = 0; c < 3; c++) {
        LONG a = (LONG)ui_rgb(bg, c), d = (LONG)ui_rgb(fg, c) - a;
        rgb[c] = (ULONG)(a + d * lv / (SMOOTH_LEVELS - 1));
    }
    pen = alloc_rgb(s, rgb[0], rgb[1], rgb[2]);
    if (!pen)                               /* out of pens: nearest end */
        return lv * 2 >= SMOOTH_LEVELS - 1 ? fg : bg;
    *slot = pen;
    return pen;
}

/* ------------------------------------------------------------ masks */
/* Room for SMOOTH_LEVELS masks of w x h, cleared. */
static BOOL masks_begin(struct Smooth *s, UWORD w, UWORD h)
{
    ULONG need;
    s->bpr = ((w + 15) >> 4) << 1;
    s->plane = (ULONG)s->bpr * h;
    need = s->plane * SMOOTH_LEVELS;
    if (need > s->scratch_size) {
        if (s->scratch) {
            WaitBlit();
            FreeMem(s->scratch, s->scratch_size);
        }
        s->scratch_size = 0;
        /* BltTemplate reads its source with the blitter: chip RAM */
        if (!(s->scratch = AllocMem(need, MEMF_CHIP)))
            return FALSE;
        s->scratch_size = need;
    } else {
        WaitBlit();                         /* the last stamps may still read it */
    }
    memset(s->scratch, 0, need);
    {
        UBYTE lv, *m = s->scratch;
        for (lv = 0; lv < SMOOTH_LEVELS; lv++, m += s->plane)
            s->pl[lv] = m;
    }
    s->used = 0;
    return TRUE;
}

/* Sort a coverage map (nibbles from d, w x h) into the masks at (x, y). */
static void masks_put(struct Smooth *s, const UBYTE *d, UWORD w, UWORD h, WORD x, WORD y)
{
    UWORD i, j, bpr = s->bpr;
    ULONG n = 0, roff = (ULONG)y * bpr;
    UBYTE used = s->used;
    for (j = 0; j < h; j++, roff += bpr) {
        for (i = 0; i < w; i++, n++) {
            UBYTE lv = step_of[nib(d, n)];
            if (lv) {
                UWORD bx = x + i;
                s->pl[lv][roff + (bx >> 3)] |= 0x80 >> (bx & 7);
                used |= 1 << lv;
            }
        }
    }
    s->used = used;
}

/* Stamp the masks at (x, y). Opaque: the uncovered pixels of the box get
 * bg too, in the same pass, so the text needs no clearing first. */
static void masks_end(struct Smooth *s, struct Gfx *g, WORD x, WORD y, UWORD w, UWORD h,
                      UBYTE fg, UBYTE bg, BOOL opaque)
{
    struct RastPort *rp = g->rp;
    UBYTE lv;
    SetDrMd(rp, JAM1);
    if (opaque) {
        UBYTE last = (w & 7) ? (UBYTE)(0xFF << (8 - (w & 7))) : 0xFF;
        UWORD nb = (w + 7) >> 3, i, j;
        ULONG roff = 0;
        for (j = 0; j < h; j++, roff += s->bpr) {
            for (i = 0; i < nb; i++) {
                UBYTE b = 0;
                for (lv = 1; lv < SMOOTH_LEVELS; lv++)
                    b |= s->pl[lv][roff + i];
                s->pl[0][roff + i] = (UBYTE)~b & (i == nb - 1 ? last : 0xFF);
            }
        }
        SetAPen(rp, bg);
        BltTemplate((PLANEPTR)s->pl[0], 0, s->bpr, rp, x, y, w, h);
    }
    for (lv = 1; lv < SMOOTH_LEVELS; lv++)
        if (s->used & (1 << lv)) {
            UBYTE pen = sm_pen(s, g, fg, bg, lv);
            PLANEPTR m = (PLANEPTR)s->pl[lv];
            SetAPen(rp, pen);
            BltTemplate(m, 0, s->bpr, rp, x, y, w, h);
        }
}

/* ------------------------------------------------------------ text */
static const UBYTE *glyph(const struct Face *f, UBYTE c)
{
    if (c < f->lo || c > f->hi)
        c = '?';
    return f->gl + (ULONG)(c - f->lo) * GLYPH_BYTES;
}

WORD sm_textlen(struct Smooth *s, const char *str, LONG n, int face)
{
    const struct Face *f = &s->face[face < NFACES ? face : 0];
    WORD w = 0;
    while (n-- > 0)
        w += glyph(f, (UBYTE)*str++)[0];
    return w;
}

/* How many characters of str fit in maxw pixels. */
LONG sm_fit(struct Smooth *s, const char *str, LONG n, int face, WORD maxw)
{
    const struct Face *f = &s->face[face < NFACES ? face : 0];
    LONG i;
    WORD w = 0;
    for (i = 0; i < n; i++) {
        w += glyph(f, (UBYTE)str[i])[0];
        if (w > maxw)
            break;
    }
    return i;
}

void sm_text(struct Smooth *s, struct Gfx *g, WORD x, WORD y, const char *str, LONG n,
             int face, UBYTE fg, UBYTE bg, BOOL opaque)
{
    const struct Face *f = &s->face[face < NFACES ? face : 0];
    WORD pen = 0, minx = 0, maxx = 0, miny = 0, maxy = s->cell_h;
    LONG i;
    if (n <= 0)
        return;
    /* the box every glyph's ink falls in (accents may rise above the cell) */
    for (i = 0; i < n; i++) {
        const UBYTE *G = glyph(f, (UBYTE)str[i]);
        WORD gx = pen + (BYTE)G[1], gy = (BYTE)G[2];
        if (G[3]) {
            if (gx < minx)
                minx = gx;
            if (gx + G[3] > maxx)
                maxx = gx + G[3];
            if (gy < miny)
                miny = gy;
            if (gy + G[4] > maxy)
                maxy = gy + G[4];
        }
        pen += G[0];
    }
    if (opaque && pen > maxx)
        maxx = pen;
    if (maxx - minx <= 0 || maxx - minx > MAX_TEXT_W || !masks_begin(s, maxx - minx, maxy - miny))
        return;
    pen = 0;
    for (i = 0; i < n; i++) {
        const UBYTE *G = glyph(f, (UBYTE)str[i]);
        if (G[3])
            masks_put(s, f->blob + rd32(G + 6), G[3], G[4], pen + (BYTE)G[1] - minx,
                      (BYTE)G[2] - miny);
        pen += G[0];
    }
    masks_end(s, g, x + minx, y + miny, maxx - minx, maxy - miny, fg, bg, opaque);
}

void sm_icon(struct Smooth *s, struct Gfx *g, int icon, WORD x, WORD y, UBYTE fg, UBYTE bg)
{
    if (icon < 0 || icon >= s->icon_cnt || !masks_begin(s, s->icon_n, s->icon_n))
        return;
    masks_put(s, s->icons + s->icon_bytes * icon, s->icon_n, s->icon_n, 0, 0);
    masks_end(s, g, x, y, s->icon_n, s->icon_n, fg, bg, FALSE);
}

void sm_mark(struct Smooth *s, struct Gfx *g, int mark, WORD x, WORD y, UBYTE fg, UBYTE bg)
{
    int i = mark - MARK_ARROW;
    if (i < 0 || i >= s->mark_cnt || !masks_begin(s, s->mark_n, s->mark_n))
        return;
    masks_put(s, s->marks + s->mark_bytes * i, s->mark_n, s->mark_n, 0, 0);
    masks_end(s, g, x, y, s->mark_n, s->mark_n, fg, bg, FALSE);
}

/* ------------------------------------------------------------ logo */
/* Out of pens: the UI pen closest to this colour. */
static UBYTE nearest_ui_pen(const UBYTE *rgb)
{
    UBYTE best = PEN_WHITE, p;
    ULONG bestd = 0xFFFFFFFFUL;
    for (p = 0; p <= PEN_FAINT; p++) {
        ULONG d = 0;
        int c;
        for (c = 0; c < 3; c++) {
            LONG e = (LONG)ui_rgb(p, c) - rgb[c];
            d += (ULONG)(e < 0 ? -e : e);
        }
        if (d < bestd) {
            bestd = d;
            best = p;
        }
    }
    return best;
}

BOOL sm_logo(struct Smooth *s, struct Gfx *g)
{
    UWORD w = s->logo_w, h = s->logo_h, i, j, c;
    UWORD bpr = ((w + 15) >> 4) << 1;
    ULONG size = (ULONG)s->logo_nc * h * bpr;
    UBYTE *m;
    if (!size || !(m = AllocMem(size, MEMF_CHIP | MEMF_CLEAR)))
        return FALSE;
    for (c = 0; c < s->logo_nc; c++) {
        const UBYTE *rgb = s->logo_pal + 3 * c;
        UBYTE pen = alloc_rgb(s, rgb[0], rgb[1], rgb[2]);
        g->logo_pens[c] = pen ? pen : nearest_ui_pen(rgb);
    }
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            UBYTE v = s->logo_idx[(ULONG)j * w + i];
            if (v < s->logo_nc)
                m[((ULONG)v * h + j) * bpr + (i >> 3)] |= 0x80 >> (i & 7);
        }
    g->logo_masks = m;
    g->logo_npens = (UBYTE)s->logo_nc;
    g->logo_bpr = bpr;
    g->logo_w = w;
    g->logo_h = h;
    return TRUE;
}

/* ------------------------------------------------------------ big letter */
/* The letter overlay's letter: the bold face's glyph, enlarged with
 * bilinear filtering so its ink is ink_h pixels tall, one mask per
 * coverage step. 16 x 16-bit multiplies only (the 68000 has those). */
BOOL sm_bigletter(struct Smooth *s, UBYTE c, WORD ink_h, struct BigLetter *bl)
{
    static UBYTE lv_of[241];            /* bilinear value 0..240 -> step */
    const struct Face *f = &s->face[TS_BOLD];
    const UBYTE *G = glyph(f, c), *d = f->blob + rd32(G + 6);
    UWORD gw = G[3], gh = G[4], ow, oh, ox, oy, sc;    /* sc: scale x 64 */
    ULONG plane;
    WORD *col;
    UBYTE *pl[SMOOTH_LEVELS];
    int i;
    if (!gw || !gh || ink_h <= 0)
        return FALSE;
    if (!lv_of[240])
        for (i = 0; i <= 240; i++)
            lv_of[i] = (UBYTE)((i * (SMOOTH_LEVELS - 1) + 120) / 240);
    sc = (UWORD)(((ULONG)ink_h << 6) / gh);
    if (sc < 128)
        sc = 128;
    if (sc > 64 * 16)
        sc = 64 * 16;
    ow = (UWORD)(((ULONG)gw * sc) >> 6);
    oh = (UWORD)(((ULONG)gh * sc) >> 6);
    bl->bpr = ((ow + 15) >> 4) << 1;
    plane = (ULONG)bl->bpr * oh;
    bl->size = plane * (SMOOTH_LEVELS - 1);
    bl->mask = AllocMem(bl->size, MEMF_CHIP | MEMF_CLEAR);
    if (!bl->mask)
        return FALSE;
    col = AllocVec((ULONG)ow * sizeof(WORD), MEMF_ANY);
    if (!col) {
        FreeMem(bl->mask, bl->size);
        bl->mask = NULL;
        return FALSE;
    }
    /* source column of each output column, in 1/64 pixels (divides once) */
    for (ox = 0; ox < ow; ox++)
        col[ox] = (WORD)((((ULONG)ox << 6) + 32) * 64 / sc) - 32;
    pl[0] = NULL;
    for (i = 1; i < SMOOTH_LEVELS; i++)
        pl[i] = bl->mask + plane * (i - 1);
    bl->w = ow;
    bl->h = oh;
    bl->nlev = SMOOTH_LEVELS - 1;
    bl->cell_w = (UWORD)(((ULONG)f->widest_cap * sc) >> 6);
    for (oy = 0; oy < oh; oy++) {
        /* source position in 1/64 pixels, centred on the output pixel */
        WORD sy = (WORD)((((ULONG)oy << 6) + 32) * 64 / sc) - 32;
        WORD y0 = sy >> 6;
        UWORD fy = sy & 63;
        ULONG roff = (ULONG)oy * bl->bpr;
        /* source rows y0 and y0 + 1 (-1: outside the glyph) */
        LONG r0 = (y0 >= 0 && y0 < (WORD)gh) ? (LONG)y0 * gw : -1;
        LONG r1 = (y0 + 1 >= 0 && y0 + 1 < (WORD)gh) ? (LONG)(y0 + 1) * gw : -1;
        for (ox = 0; ox < ow; ox++) {
            WORD sx = col[ox];
            WORD x0 = sx >> 6;
            UWORD fx = sx & 63, v00, v10, v01, v11, top, bot, v;
            UBYTE lv;
#define COV(xx, r) (((r) < 0 || (xx) < 0 || (xx) >= (WORD)gw) ? 0 : nib(d, (ULONG)((r) + (xx))))
            v00 = COV(x0, r0);
            v10 = COV(x0 + 1, r0);
            v01 = COV(x0, r1);
            v11 = COV(x0 + 1, r1);
#undef COV
            top = (UWORD)(v00 * (UWORD)(64 - fx) + v10 * fx);      /* 0..960 */
            bot = (UWORD)(v01 * (UWORD)(64 - fx) + v11 * fx);
            v = (UWORD)(((ULONG)top * (UWORD)(64 - fy) + (ULONG)bot * fy) >> 8);   /* 0..240 */
            lv = lv_of[v];
            if (lv)
                pl[lv][roff + (ox >> 3)] |= 0x80 >> (ox & 7);
        }
    }
    FreeVec(col);
    return TRUE;
}
