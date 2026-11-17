/*
 * Ambiance - Flow, Now Playing's visualisation for graphics cards: the
 * picture of a moment ago carried a little further each frame (zoomed,
 * turned, swirled, rippled), blurred and fading as it goes, with the
 * sound's wave drawn into it and its colours from a palette that drifts
 * and jumps with the beat. MilkDrop's idea, done with a table where it
 * would use a 3D card: each pixel of a frame is the average of four of
 * the last frame's, from where that table says.
 * Copyright (c) 2026 Brian King
 *
 * Worked out small (about 256 pixels across, one byte of brightness a
 * pixel) and drawn scaled up in the screen's own pixels (15, 16, 24 or
 * 32-bit: a 256-colour table in its format): each row's pixels made once
 * in fast memory, then copied to the rows it covers, into a bitmap like
 * the screen's, blitted. RTG only: on a palette screen there are no
 * colours to spare for it.
 */
#include "amce.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

extern struct Library *CyberGfxBase;

#define MAXPIX  (512UL * 384)   /* the most worked out (2 MB with its tables) */
#define MAPROWS 12              /* rows of the next table made a frame */

/* a palette: brightness 0, 85, 170 and 255 as RGB, the rest between */
static const ULONG pals[][4] = {
    { 0x000008, 0x0A2A8C, 0x22B4E6, 0xF0FFFF },     /* deep sea */
    { 0x080004, 0x7A0A6E, 0xF0641E, 0xFFF0B4 },     /* sunset */
    { 0x000804, 0x064A3A, 0x3CD282, 0xE6FFD2 },     /* aurora */
    { 0x040008, 0x3C1496, 0xC83CDC, 0xFFDCFF },     /* violet */
    { 0x080400, 0x6E2800, 0xE6A014, 0xFFFFDC },     /* embers */
};
#define NPALS ((int)(sizeof(pals) / sizeof(pals[0])))

/* how the picture moves on: zoom (256 none; less, it grows outwards),
 * a turn a frame and a swirl (1024 a circle), ripples (their depth,
 * 256ths, and how many), a drift (internal pixels a frame, 8.8), and
 * how the wave is drawn: 0 across, 1 round, 2 two mirrored */
struct Preset {
    WORD zoom, turn, swirl, rip_amp, rip_n, drift_x, drift_y, wave;
};
static const struct Preset presets[] = {
    { 246, 3, 0, 0, 0, 0, 0, 1 },           /* a slow turn outwards */
    { 250, 0, 40, 0, 0, 0, 0, 0 },          /* a whirlpool */
    { 244, -2, 0, 10, 6, 0, 0, 1 },         /* rings rippling out */
    { 262, 4, -30, 0, 0, 0, 0, 2 },         /* falling in, turning */
    { 252, 0, 0, 6, 3, 0, -160, 2 },        /* rising, waving */
    { 248, -5, 24, 4, 9, 0, 0, 1 },         /* a spiral */
};
#define NPRESETS ((int)(sizeof(presets) / sizeof(presets[0])))

static struct {
    BOOL on, failed;
    WORD logged_w, logged_h;    /* (the size last logged: logged again only as it changes) */
    UBYTE coarse;               /* slow here: worked out this much coarser (vf_draw) */
    struct Gfx *g;
    struct Screen *scr;
    WORD w, h;                  /* the place drawn, pixels */
    WORD iw, ih;                /* worked out at */
    APTR mem;                   /* (all below from it) */
    UBYTE *a, *b;               /* this frame's brightness and the last's (swapped each frame) */
    ULONG *map, *next;          /* the warp: where each pixel comes from */
    UBYTE *colrun, *rowrun;     /* each internal column's and row's pixels on screen */
    UBYTE fade[1024];           /* four pixels' sum: their average, dimmed */
    WORD sine[1024];
    UBYTE *line;                /* a row on screen in its pixels (fast memory) */
    ULONG lut[256];             /* the brightnesses as the screen's pixels */
    ULONG fmt, bpp;             /* PIXFMT_, bytes a pixel */
    struct BitMap *bm;          /* drawn into, then blitted */
    BOOL direct;                /* bm's memory locked and written; else WritePixelArray */
    ULONG *argb;                /* (WritePixelArray's rows) */
    int preset, pal, pal_to;    /* the preset; the palette and the one it goes to */
    WORD pal_mix;               /* 0-256 of the way there */
    int map_row;                /* the next table made this far (-1: done) */
    int next_preset;
    ULONG frames, t_preset, t_beat, t_pal;
    LONG bass_avg;              /* (16 times) */
    UBYTE hue_kick;
    ULONG ms;                   /* (timing) */
    ULONG slow;                 /* frames over budget, in a row */
} vf;

/* ------------------------------------------------------------ the screen's pixels */

static ULONG pix(ULONG rgb)
{
    ULONG r = (rgb >> 16) & 255, gg = (rgb >> 8) & 255, b = rgb & 255, v;
    switch (vf.fmt) {
    case PIXFMT_RGB15: case PIXFMT_RGB15PC:
        v = (r >> 3) << 10 | (gg >> 3) << 5 | (b >> 3);
        break;
    case PIXFMT_BGR15: case PIXFMT_BGR15PC:
        v = (b >> 3) << 10 | (gg >> 3) << 5 | (r >> 3);
        break;
    case PIXFMT_RGB16: case PIXFMT_RGB16PC:
        v = (r >> 3) << 11 | (gg >> 2) << 5 | (b >> 3);
        break;
    case PIXFMT_BGR16: case PIXFMT_BGR16PC:
        v = (b >> 3) << 11 | (gg >> 2) << 5 | (r >> 3);
        break;
    case PIXFMT_BGRA32:
        return b << 24 | gg << 16 | r << 8 | 0xFF;
    case PIXFMT_RGBA32:
        return r << 24 | gg << 16 | b << 8 | 0xFF;
    case PIXFMT_BGR24:
        return b << 16 | gg << 8 | r;
    default:                    /* ARGB32, RGB24, and WritePixelArray's ARGB */
        return rgb;
    }
    if (vf.fmt == PIXFMT_RGB15PC || vf.fmt == PIXFMT_BGR15PC || vf.fmt == PIXFMT_RGB16PC ||
        vf.fmt == PIXFMT_BGR16PC)
        v = ((v & 0xFF) << 8) | (v >> 8);
    return v;
}

static ULONG mix(ULONG c0, ULONG c1, LONG t, LONG n)
{
    ULONG r = 0;
    int s;
    for (s = 0; s <= 16; s += 8) {
        LONG a = (LONG)((c0 >> s) & 255), b = (LONG)((c1 >> s) & 255);
        r |= (ULONG)(a + (b - a) * t / n) << s;
    }
    return r;
}

/* palette p's colour at brightness i (0-255) */
static ULONG pal_at(int p, int i)
{
    int k = i / 85;
    if (k >= 3)
        return pals[p][3];
    return mix(pals[p][k], pals[p][k + 1], i - k * 85, 85);
}

/* the table of the screen's pixels: the palette, on its way to the next,
 * and a kick of light with the beat */
static void make_lut(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        int j = i + vf.hue_kick * (255 - i) / 255;
        ULONG c = pal_at(vf.pal, j);
        if (vf.pal_mix)
            c = mix(c, pal_at(vf.pal_to, j), vf.pal_mix, 256);
        vf.lut[i] = vf.direct ? pix(c) : c;
    }
}

/* ------------------------------------------------------------ the warp */

/* rows from vf.map_row on of preset p's table, made into vf.next */
static void map_rows(int p, int rows)
{
    const struct Preset *s = &presets[p];
    LONG cx = vf.iw / 2, cy = vf.ih / 2, r2max = cx * cx + cy * cy;
    int y, x, end = vf.map_row + rows;
    if (end > vf.ih)
        end = vf.ih;
    for (y = vf.map_row; y < end; y++) {
        ULONG *m = vf.next + (ULONG)y * vf.iw;
        LONG dy = y - cy;
        for (x = 0; x < vf.iw; x++) {
            LONG dx = x - cx, q = (dx * dx + dy * dy) * 1024 / r2max;     /* 0-1024 out to the corners */
            int ang = (s->turn + (int)(s->swirl * q / 1024)) & 1023;
            LONG cs = vf.sine[(ang + 256) & 1023], sn = vf.sine[ang];        /* 16383 = 1 */
            LONG z = s->zoom, sx, sy;
            if (s->rip_amp)
                z += (LONG)s->rip_amp * vf.sine[(q * s->rip_n) & 1023] / 16383;
            sx = ((dx * cs - dy * sn) >> 6) * z;        /* (14 - 6) + 8 fraction bits */
            sy = ((dx * sn + dy * cs) >> 6) * z;
            sx = (sx >> 16) + cx + (s->drift_x >> 8);
            sy = (sy >> 16) + cy + (s->drift_y >> 8);
            if (sx < 0)
                sx = 0;
            else if (sx > vf.iw - 2)
                sx = vf.iw - 2;
            if (sy < 0)
                sy = 0;
            else if (sy > vf.ih - 2)
                sy = vf.ih - 2;
            m[x] = (ULONG)(sy * vf.iw + sx);
        }
    }
    vf.map_row = end;
}

/* the last frame carried on into this one: each pixel the dimmed average
 * of four from where the table says */
static void warp(void)
{
    const UBYTE *src = vf.b, *f = vf.fade;
    UBYTE *dst = vf.a;
    const ULONG *m = vf.map;
    ULONG n = (ULONG)vf.iw * vf.ih, iw = vf.iw;
    while (n--) {
        const UBYTE *p = src + *m++;
        *dst++ = f[p[0] + p[1] + p[iw] + p[iw + 1]];
    }
}

/* ------------------------------------------------------------ the wave */

static void plot(int x, int y, UBYTE v)
{
    if (x >= 0 && y >= 0 && x < vf.iw && y < vf.ih) {
        UBYTE *p = vf.a + y * vf.iw + x;
        if (*p < v)
            *p = v;
    }
}

static void line(int x0, int y0, int x1, int y1, UBYTE v)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx - dy, k = dx + dy + 1;
    while (k-- > 0) {
        plot(x0, y0, v);
        if (x0 == x1 && y0 == y1)
            break;
        if (2 * e > -dy) {
            e -= dy;
            x0 += sx;
        }
        if (2 * e < dx) {
            e += dx;
            y0 += sy;
        }
    }
}

/* the sound's n samples (about 8 kHz) drawn in, as the preset says */
static void wave(const WORD *w, int n, int loud)
{
    int i, x0 = 0, y0 = 0, mode = presets[vf.preset].wave;
    if (mode == 1 && vf.iw > 3 * vf.ih)
        mode = 2;                           /* (a low strip: a ring would be a dot) */
    LONG cx = vf.iw / 2, cy = vf.ih / 2, amp = vf.ih / 4 + loud * vf.ih / 512;
    UBYTE v = (UBYTE)(160 + loud > 255 ? 255 : 160 + loud);
    for (i = 0; i < n; i++) {
        LONG s = w[i] / 64;                 /* +-512 at full scale */
        int x, y;
        if (mode == 1) {
            /* round: the wave about a circle a third of the height across */
            LONG r = vf.ih / 3 + s * amp / 1536;
            int a = i * 1024 / n;
            x = (int)(cx + r * vf.sine[(a + 256) & 1023] / 16383);
            y = (int)(cy + r * vf.sine[a] / 16383);
        } else {
            x = i * (vf.iw - 1) / (n - 1);
            y = (int)(cy + s * amp / 512);
        }
        if (i)
            line(x0, y0, x, y, v);
        if (mode == 2)
            line(x0, (int)(2 * cy - y0), x, (int)(2 * cy - y), v);
        x0 = x;
        y0 = y;
    }
    if (mode == 1 && n > 1)
        line(x0, y0, (int)(cx + (vf.ih / 3 + w[0] / 64 * amp / 1536)), (int)cy, v);
}

/* ------------------------------------------------------------ setting up */

static void vf_free(void)
{
    if (vf.bm) {
        WaitBlit();
        FreeBitMap(vf.bm);
    }
    if (vf.mem)
        FreeVec(vf.mem);
    memset(&vf.mem, 0, sizeof(vf) - offsetof(__typeof__(vf), mem));
    vf.on = FALSE;
}

void vf_stop(void)
{
    vf_free();
    vf.failed = FALSE;
}

BOOL vf_possible(const struct Gfx *g)
{
    return g && g->truecolor && !g->planar && CyberGfxBase && g->depth >= 15 && !vf.failed;
}

/* Its memory for w x h pixels at g: FALSE if not (no memory: tried no
 * more till vf_stop). */
static BOOL vf_start(struct Gfx *g, WORD w, WORD h)
{
    ULONG s, n, i, mem;
    UBYTE *m;
    if (vf.on && vf.g == g && vf.scr == g->scr && vf.w == w && vf.h == h)
        return TRUE;
    vf_free();
    if (w < 16 || h < 8)
        return FALSE;
    /* half the screen's pixels each way where this Amiga is fast (as
     * measured), else a quarter; coarser again if its frames come slowly
     * (vf_draw); never more than MAXPIX in all */
    s = (plex_tier() >= 2 ? 2 : 4) + vf.coarse;
    if (s > (ULONG)h / 24)
        s = (ULONG)h / 24;                  /* a low strip (Now playing's): finer, not 11 rows */
    if (s < 1)
        s = 1;
    while (((ULONG)w / s) * ((ULONG)h / s) > MAXPIX)
        s++;
    vf.iw = (WORD)(w / s);
    vf.ih = (WORD)(h / s);
    if (vf.iw < 8 || vf.ih < 8)
        return FALSE;
    n = (ULONG)vf.iw * vf.ih;
    mem = ((2 * n + 3) & ~3UL) + 2 * n * 4 + vf.iw + vf.ih + (ULONG)w * 4 + 16;
    if (!(m = AllocVec(mem, MEMF_ANY | MEMF_CLEAR))) {
        vf.failed = TRUE;
        return FALSE;
    }
    vf.mem = m;
    vf.a = m;
    vf.b = m + n;
    vf.map = (ULONG *)(m + ((2 * n + 3) & ~3UL));
    vf.next = vf.map + n;
    vf.line = (UBYTE *)(vf.next + n);
    vf.colrun = vf.line + (ULONG)w * 4;
    vf.rowrun = vf.colrun + vf.iw;
    /* each internal column and row's share of the screen's (the
     * remainder spread across) */
    for (i = 0; i < (ULONG)vf.iw; i++)
        vf.colrun[i] = (UBYTE)((i + 1) * w / vf.iw - i * w / vf.iw);
    for (i = 0; i < (ULONG)vf.ih; i++)
        vf.rowrun[i] = (UBYTE)((i + 1) * h / vf.ih - i * h / vf.ih);
    vis_tables(vf.sine);
    for (i = 0; i < 1024; i++) {
        ULONG v = i / 4;                    /* the four's average */
        v = v * 243 / 256;                  /* dimmed: about a second to fade */
        vf.fade[i] = (UBYTE)(v > 2 ? v - 2 : 0);
    }
    /* a bitmap like the screen's, its memory written straight where the
     * graphics system allows; else WritePixelArray's ARGB */
    vf.bm = AllocBitMap((ULONG)w, (ULONG)h, GetBitMapAttr(g->rp->BitMap, BMA_DEPTH), BMF_MINPLANES,
                        g->rp->BitMap);
    vf.direct = FALSE;
    vf.fmt = PIXFMT_ARGB32;
    vf.bpp = 4;
    if (vf.bm && GetCyberMapAttr(vf.bm, CYBRMATTR_ISCYBERGFX)) {
        ULONG f = GetCyberMapAttr(vf.bm, CYBRMATTR_PIXFMT);
        if (f >= PIXFMT_RGB15 && f <= PIXFMT_RGBA32) {
            vf.fmt = f;
            vf.bpp = f <= PIXFMT_BGR16PC ? 2 : f <= PIXFMT_BGR24 ? 3 : 4;
            vf.direct = TRUE;
        }
    }
    if (!vf.direct) {
        if (vf.bm) {
            FreeBitMap(vf.bm);
            vf.bm = NULL;
        }
        vf.argb = (ULONG *)vf.line;
    }
    vf.g = g;
    vf.scr = g->scr;
    vf.w = w;
    vf.h = h;
    vf.preset = (int)(time_ms() / 1000 % NPRESETS);
    vf.pal = (int)(time_ms() / 7000 % NPALS);
    vf.pal_to = vf.pal;
    vf.map_row = 0;
    map_rows(vf.preset, vf.ih);
    memcpy(vf.map, vf.next, n * 4);
    vf.map_row = -1;
    vf.t_preset = vf.t_pal = vf.t_beat = time_ms();
    vf.on = TRUE;
    if (w != vf.logged_w || h != vf.logged_h) {
        char msg[120];
        vf.logged_w = w;
        vf.logged_h = h;
        sprintf(msg, "Flow: %ld x %ld, worked out at %ld x %ld, %s (format %lu)", (long)w, (long)h,
                (long)vf.iw, (long)vf.ih, vf.direct ? "written straight" : "by WritePixelArray",
                (unsigned long)vf.fmt);
        amce_log("visualisation", msg);
    }
    return TRUE;
}

/* ------------------------------------------------------------ drawing */

/* internal row r's pixels across the screen's width into vf.line */
static void make_line(const UBYTE *row)
{
    const ULONG *lut = vf.lut;
    const UBYTE *run = vf.colrun;
    int x, k;
    if (vf.bpp == 2) {
        UWORD *d = (UWORD *)vf.line;
        for (x = 0; x < vf.iw; x++) {
            UWORD v = (UWORD)lut[row[x]];
            for (k = run[x]; k; k--)
                *d++ = v;
        }
    } else if (vf.bpp == 4) {
        ULONG *d = (ULONG *)vf.line;
        for (x = 0; x < vf.iw; x++) {
            ULONG v = lut[row[x]];
            for (k = run[x]; k; k--)
                *d++ = v;
        }
    } else {
        UBYTE *d = vf.line;
        for (x = 0; x < vf.iw; x++) {
            ULONG v = lut[row[x]];
            for (k = run[x]; k; k--) {
                *d++ = (UBYTE)(v >> 16);
                *d++ = (UBYTE)(v >> 8);
                *d++ = (UBYTE)v;
            }
        }
    }
}

/* the rows shown stop above this (the screen's own y): what is below
 * kept for the controls over the picture */
static WORD vf_bottom = 0x7FFF;

void vf_clip(WORD bottom)
{
    vf_bottom = bottom;
}

static void show(struct Gfx *g, WORD x, WORD y)
{
    int r, k;
    ULONG wb = (ULONG)vf.w * vf.bpp;
    WORD hh = vf_bottom - y < vf.h ? vf_bottom - y : vf.h;
    if (hh <= 0)
        return;
    if (vf.direct) {
        APTR base = NULL, lk;
        ULONG bpr = 0;
        UBYTE *d;
        if (!(lk = LockBitMapTags(vf.bm, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr,
                                  TAG_DONE)))
            return;
        d = (UBYTE *)base;
        for (r = 0; r < vf.ih && base; r++) {
            make_line(vf.a + r * vf.iw);
            for (k = vf.rowrun[r]; k; k--, d += bpr)
                CopyMem(vf.line, d, wb);
        }
        UnLockBitMap(lk);
        BltBitMapRastPort(vf.bm, 0, 0, g->rp, x, y, vf.w, hh, 0xC0);
    } else {
        /* a row at a time through WritePixelArray, ARGB */
        WORD yy = y;
        for (r = 0; r < vf.ih; r++) {
            make_line(vf.a + r * vf.iw);
            for (k = vf.rowrun[r]; k && yy < y + hh; k--, yy++)
                WritePixelArray(vf.argb, 0, 0, (UWORD)(vf.w * 4), g->rp, x, yy, (UWORD)vf.w, 1,
                                RECTFMT_ARGB);
        }
    }
}

/* One frame in w x h at x, y: wave the last sound (n samples about 8 kHz),
 * lev its spectrum (nlev bands, 0-64 each, lowest first). FALSE: not
 * drawn (no memory, not a graphics card's screen). */
BOOL vf_draw(struct Gfx *g, WORD x, WORD y, WORD w, WORD h, const WORD *wv, int n, const UBYTE *lev,
             int nlev, BOOL full)
{
    ULONG now = time_ms(), t0 = now;
    LONG bass = 0, loud = 0;
    int i;
    UBYTE *t;
    if (!vf_possible(g) || !vf_start(g, w, h))
        return FALSE;
    if (full)
        memset(vf.b, 0, (ULONG)vf.iw * vf.ih);
    /* the beat: the low bands well above their own recent average */
    for (i = 1; i < 4 && i < nlev; i++)
        if (lev[i] > bass)
            bass = lev[i];
    for (i = 0; i < nlev; i++)
        loud += lev[i];
    loud = nlev ? loud * 4 / nlev : 0;        /* 0-256 */
    if (loud > 95)
        loud = 95;
    if (bass * 16 > vf.bass_avg * 5 / 4 + 48 && now - vf.t_beat > 280) {
        vf.t_beat = now;
        vf.hue_kick = 90;
        if (now - vf.t_preset > 12000 && vf.map_row < 0) {
            /* a beat after a while: the next way of moving, its table
             * made a few rows a frame */
            vf.next_preset = (vf.preset + 1 + (int)(now % (NPRESETS - 1))) % NPRESETS;
            vf.map_row = 0;
        }
    }
    vf.bass_avg += bass - vf.bass_avg / 16;
    if (vf.hue_kick)
        vf.hue_kick = vf.hue_kick > 12 ? vf.hue_kick - 12 : 0;
    if (now - vf.t_preset > 30000 && vf.map_row < 0) {
        vf.next_preset = (vf.preset + 1) % NPRESETS;  /* no beat: on anyway */
        vf.map_row = 0;
    }
    if (vf.map_row >= 0) {
        map_rows(vf.next_preset, MAPROWS);
        if (vf.map_row >= vf.ih) {
            ULONG *s = vf.map;
            vf.map = vf.next;
            vf.next = s;
            vf.preset = vf.next_preset;
            vf.map_row = -1;
            vf.t_preset = now;
        }
    }
    /* the palette: a new one every 40 s, over 4 */
    if (vf.pal_to == vf.pal && now - vf.t_pal > 40000) {
        vf.pal_to = (vf.pal + 1) % NPALS;
        vf.t_pal = now;
    }
    if (vf.pal_to != vf.pal) {
        LONG d = (LONG)(now - vf.t_pal);
        vf.pal_mix = (WORD)(d >= 4000 ? 256 : d * 256 / 4000);
        if (vf.pal_mix >= 256) {
            vf.pal = vf.pal_to;
            vf.pal_mix = 0;
            vf.t_pal = now;
        }
    }
    make_lut();
    t = vf.a;
    vf.a = vf.b;
    vf.b = t;                       /* the last frame in b, this one into a */
    warp();
    if (n > 1)
        wave(wv, n, (int)loud);
    show(g, x, y);
    gfx_dirty();                    /* (a double-buffered screen flips to it; a window's copy) */
    vf.frames++;
    vf.ms += time_ms() - t0;
    if (h > 120 && time_ms() - t0 > 50) {
        /* the whole screen's coming slowly (under 20 a second for a second):
         * coarser from the next frame */
        if (++vf.slow >= 20 && vf.coarse < 4) {
            char m[80];
            vf.coarse++;
            sprintf(m, "Flow: frames slow here (%lu ms): worked out coarser", (unsigned long)(time_ms() - t0));
            amce_log("visualisation", m);
            vf_free();
        }
    } else {
        vf.slow = 0;
    }
    return TRUE;
}

/* Frames drawn and their time (ms) since asked last. */
void vf_timing(ULONG *frames, ULONG *ms)
{
    *frames = vf.frames;
    *ms = vf.ms;
    vf.frames = vf.ms = 0;
}
