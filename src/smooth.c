/*
 * AmigaMCE - smooth style: anti-aliased text, icons, markers and logo
 * Copyright (c) 2026 Brian King
 *
 * Used on RTG screens, 8-bit ones too. Everything is pre-rendered by
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
 * pen is only a colour to draw with, so this costs nothing on screen. On
 * an 8-bit one the pens are the screen's own: each shade takes a free pen
 * of its own (ObtainPen) when it is first drawn, only those really used,
 * and lets them go when the screen closes. The log says how many.
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
#define GLYPH_BYTES 10          /* u16 advance (1/16 px), s8 xoff, s8 yoff, u8 w, u8 h, u32 offset */
#define G_X 2
#define G_Y 3
#define G_W 4
#define G_H 5
#define PHASES      4           /* each glyph at 0, 1/4, 1/2, 3/4 of a pixel */
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
    UWORD nletters, letter_cell_w;
    const UBYTE *letter[27];    /* A-Z, '#': u16 w, h, u32 bytes, runs */
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
    UBYTE got[256];             /* 8-bit screens: the pens obtained, */
    UWORD ngot;                 /*   to let go */
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
    if (s->size < 14 || memcmp(p, "AMSM", 4) || rd16(p + 4) != 3 || rd16(p + 12) != NFACES)
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
            UBYTE w = fc->gl[(c - fc->lo) * GLYPH_BYTES + G_W];
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
    if (p + 4 > end)
        return FALSE;
    s->nletters = rd16(p);
    s->letter_cell_w = rd16(p + 2);
    p += 4;
    if (s->nletters != 27)
        return FALSE;
    for (f = 0; f < 27; f++) {
        if (p + 8 > end)
            return FALSE;
        s->letter[f] = p;
        p += 8 + rd32(p + 4);
    }
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
    s->ngot = 0;
}

UWORD sm_pens_used(const struct Smooth *s)
{
    return s->g && !s->g->truecolor ? s->ngot : s->next_pen - FIRST_PEN;
}

void sm_detach(struct Smooth *s)
{
    if (s->g && s->g->scr && s->ngot) {
        UWORD i;
        char m[48];
        sprintf(m, "%lu pens for its shades and logo", (unsigned long)s->ngot);
        amce_log("smooth style", m);
        for (i = 0; i < s->ngot; i++)
            ReleasePen(s->g->scr->ViewPort.ColorMap, s->got[i]);
    }
    s->ngot = 0;
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
    if (!s->g)
        return 0;
    if (!s->g->truecolor) {
        /* a real pen: any free one, held while the screen is open */
        LONG p;
        if (s->ngot >= sizeof(s->got))
            return 0;
        p = (LONG)ObtainPen(s->g->scr->ViewPort.ColorMap, -1, r * 0x01010101UL,
                                 gg * 0x01010101UL, b * 0x01010101UL, PEN_EXCLUSIVE);
        if (p <= 0)
            return 0;                       /* none left: the nearest end */
        s->got[s->ngot++] = (UBYTE)p;
        return (UBYTE)p;
    }
    if (s->next_pen > s->last_pen)
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

/* Text is laid out in 1/16 pixels, each glyph drawn from the stored
 * quarter-pixel position nearest its true one. */
static UWORD adv16(const UBYTE *G) { return rd16(G); }

WORD sm_textlen(struct Smooth *s, const char *str, LONG n, int face)
{
    const struct Face *f = &s->face[face < NFACES ? face : 0];
    ULONG w16 = 0;
    while (n-- > 0)
        w16 += adv16(glyph(f, (UBYTE)*str++));
    return (WORD)((w16 + 8) >> 4);
}

/* How many characters of str fit in maxw pixels. */
LONG sm_fit(struct Smooth *s, const char *str, LONG n, int face, WORD maxw)
{
    const struct Face *f = &s->face[face < NFACES ? face : 0];
    LONG i;
    ULONG w16 = 0;
    for (i = 0; i < n; i++) {
        w16 += adv16(glyph(f, (UBYTE)str[i]));
        if ((WORD)((w16 + 8) >> 4) > maxw)
            break;
    }
    return i;
}

void sm_text(struct Smooth *s, struct Gfx *g, WORD x, WORD y, const char *str, LONG n,
             int face, UBYTE fg, UBYTE bg, BOOL opaque)
{
    const struct Face *f = &s->face[face < NFACES ? face : 0];
    ULONG pen16 = 0;
    WORD minx = 0, maxx = 0, miny = 0, maxy = s->cell_h;
    LONG i;
    if (n <= 0)
        return;
    /* the box every glyph's ink falls in (accents may rise above the cell) */
    for (i = 0; i < n; i++) {
        const UBYTE *G = glyph(f, (UBYTE)str[i]);
        ULONG q = (pen16 + 2) >> 2;             /* in quarter pixels */
        WORD gx = (WORD)(q >> 2) + (BYTE)G[G_X], gy = (BYTE)G[G_Y];
        if (G[G_W]) {
            if (gx < minx)
                minx = gx;
            if (gx + G[G_W] > maxx)
                maxx = gx + G[G_W];
            if (gy < miny)
                miny = gy;
            if (gy + G[G_H] > maxy)
                maxy = gy + G[G_H];
        }
        pen16 += adv16(G);
    }
    if (opaque && (WORD)((pen16 + 8) >> 4) > maxx)
        maxx = (WORD)((pen16 + 8) >> 4);
    if (maxx - minx <= 0 || maxx - minx > MAX_TEXT_W || !masks_begin(s, maxx - minx, maxy - miny))
        return;
    pen16 = 0;
    for (i = 0; i < n; i++) {
        const UBYTE *G = glyph(f, (UBYTE)str[i]);
        ULONG q = (pen16 + 2) >> 2;
        UWORD w = G[G_W], h = G[G_H];
        if (w) {
            UWORD bytes = (UWORD)(((ULONG)w * h + 1) >> 1), ph = (UWORD)(q & 3);
            const UBYTE *d = f->blob + rd32(G + 6);
            while (ph--)
                d += bytes;
            masks_put(s, d, w, h, (WORD)(q >> 2) + (BYTE)G[G_X] - minx, (BYTE)G[G_Y] - miny);
        }
        pen16 += adv16(G);
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

/* Logo masks (one per colour) from a palette image; pens[] has a pen
 * for each colour. */
static BOOL logo_masks(struct Gfx *g, UWORD w, UWORD h, UWORD nc, const UBYTE *idx,
                       const UBYTE *pens)
{
    UWORD i, j, c, bpr = ((w + 15) >> 4) << 1;
    ULONG size = (ULONG)nc * h * bpr;
    UBYTE *m;
    if (!size || nc > sizeof(g->logo_pens) || !(m = AllocMem(size, MEMF_CHIP | MEMF_CLEAR)))
        return FALSE;
    for (c = 0; c < nc; c++)
        g->logo_pens[c] = pens[c];
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            UBYTE v = idx[(ULONG)j * w + i];
            if (v < nc)
                m[((ULONG)v * h + j) * bpr + (i >> 3)] |= 0x80 >> (i & 7);
        }
    g->logo_masks = m;
    g->logo_npens = (UBYTE)nc;
    g->logo_bpr = bpr;
    g->logo_w = w;
    g->logo_h = h;
    return TRUE;
}

BOOL sm_logo(struct Smooth *s, struct Gfx *g)
{
    UBYTE pens[32];
    UWORD c;
    if (s->logo_nc > 32)
        return FALSE;
    for (c = 0; c < s->logo_nc; c++) {
        const UBYTE *rgb = s->logo_pal + 3 * c;
        UBYTE pen = alloc_rgb(s, rgb[0], rgb[1], rgb[2]);
        pens[c] = pen ? pen : nearest_ui_pen(rgb);
    }
    return logo_masks(g, s->logo_w, s->logo_h, s->logo_nc, s->logo_idx, pens);
}

/* The pixel style on a deep RTG screen: the smooth logo at its whole scale
 * (PROGDIR:Fonts/amce-smooth/logo<scale>), its colours in pens 32 up. */
BOOL sm_logo_file(struct Gfx *g, UWORD scale)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    struct ColorMap *cm = g->scr->ViewPort.ColorMap;
    UBYTE *data = NULL, pens[32];
    char path[48];
    BPTR fh;
    LONG size;
    BOOL ok = FALSE;
    sprintf(path, "PROGDIR:Fonts/amce-smooth/logo%lu", (unsigned long)scale);
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return FALSE;
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size > 12 && (data = AllocVec(size, MEMF_ANY)) && Read(fh, data, size) == size &&
        !memcmp(data, "AMLG", 4) && rd16(data + 4) == 1) {
        UWORD w = rd16(data + 6), h = rd16(data + 8), nc = rd16(data + 10), c;
        const UBYTE *pal = data + 12, *idx = pal + 3 * (ULONG)nc;
        if (nc <= 32 && idx + (ULONG)w * h <= data + size &&
            cm && cm->Count >= FIRST_PEN + nc) {
            for (c = 0; c < nc; c++) {
                const UBYTE *rgb = pal + 3 * c;
                pens[c] = (UBYTE)(FIRST_PEN + c);
                SetRGB32(&g->scr->ViewPort, FIRST_PEN + c, rgb[0] * 0x01010101UL,
                         rgb[1] * 0x01010101UL, rgb[2] * 0x01010101UL);
            }
            ok = logo_masks(g, w, h, nc, idx, pens);
        }
    }
    if (data)
        FreeVec(data);
    Close(fh);
    return ok;
}

/* ------------------------------------------------------------ big letter */
/* Set bits x0..x1-1 of a mask row. */
static void set_bits(UBYTE *row, UWORD x0, UWORD x1)
{
    while (x0 < x1 && (x0 & 7)) {
        row[x0 >> 3] |= 0x80 >> (x0 & 7);
        x0++;
    }
    if (x1 - x0 >= 8) {
        memset(row + (x0 >> 3), 0xFF, (x1 - x0) >> 3);
        x0 += (x1 - x0) & ~7;
    }
    while (x0 < x1) {
        row[x0 >> 3] |= 0x80 >> (x0 & 7);
        x0++;
    }
}

/* The letter overlay's letter (A-Z or '#'), pre-rendered by mksmooth.py at
 * the size the overlay uses on this screen, as one mask: unpacking its
 * runs is all the work here. ink_h is what the caller wants; the file's
 * own size is used (it was made for this screen). */
BOOL sm_bigletter(struct Smooth *s, UBYTE c, WORD ink_h, struct BigLetter *bl)
{
    const UBYTE *L, *r;
    UWORD w, h, y;
    ULONG roff;
    (void)ink_h;
    if (c >= 'A' && c <= 'Z')
        L = s->letter[c - 'A'];
    else if (c == '#')
        L = s->letter[26];
    else
        return FALSE;
    w = rd16(L);
    h = rd16(L + 2);
    r = L + 8;
    if (!w || !h)
        return FALSE;
    bl->bpr = ((w + 15) >> 4) << 1;
    bl->size = (ULONG)bl->bpr * h;
    bl->mask = AllocMem(bl->size, MEMF_CHIP | MEMF_CLEAR);
    if (!bl->mask)
        return FALSE;
    bl->w = w;
    bl->h = h;
    bl->cell_w = s->letter_cell_w;
    for (y = 0, roff = 0; y < h; y++, roff += bl->bpr) {
        UWORD x = 0;
        BOOL on = FALSE;
        while (x < w) {
            UWORD n = *r++;
            if (on && n)
                set_bits(bl->mask + roff, x, x + n > w ? w : x + n);
            x += n;
            on = !on;
        }
    }
    return TRUE;
}
