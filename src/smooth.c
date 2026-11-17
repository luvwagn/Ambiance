/*
 * Ambiance - smooth style: anti-aliased text, icons, markers and logo
 * Copyright (c) 2026 Brian King
 *
 * Used on RTG screens, 8-bit ones too. Everything is pre-rendered by
 * tools/mksmooth.py at the exact size this screen needs, from Rubik and the
 * vector icon shapes, into PROGDIR:Fonts/amb-smooth/<lines>; nothing is
 * scaled or rasterised here. Greek, which Rubik lacks, comes from
 * Geologica, sized to Rubik's: its letters are cjk.c's two bytes.
 *
 * Glyphs, icons and markers are coverage maps (4 bits a pixel). Drawing one
 * sorts its pixels into SMOOTH_LEVELS - 1 one-bit masks by coverage step
 * and stamps each with BltTemplate in a pen whose colour lies that far
 * between the foreground and the background: the same path as the pixel
 * style's text, so it clips to the window like any other drawing and needs
 * no RTG library calls. The in-between pens are taken from 32 up as each
 * (foreground, background) pair is first used; on a truecolour screen a
 * pen is only a colour to draw with, so this costs nothing on screen. On
 * an 8-bit one the pens are the screen's own: each shade takes a free pen (3 shades a colour pair there, not 6)
 * of its own (ObtainPen) when it is first drawn, only those really used,
 * and lets them go when the screen closes. The log says how many.
 *
 * On a chunky (RTG) screen the masks are not used: each BltTemplate is a
 * pass over the whole box in software there, up to eight of them for a
 * line of text, and a full list redraw took 100-120 ms on a 68030. The
 * box is built as pen numbers instead (each pixel its coverage step's
 * pen, the uncovered ones the background) and written in one
 * WriteChunkyPixels. The background pixels are written too, so the text
 * is always drawn as if opaque over its own box: the callers draw it on
 * the flat colour they name as its background. It was no faster in
 * practice (the glide's time was its speed curve, not drawing) and the
 * text looked worse, so it is only used with PROGDIR:Ambiance.chunkytext.
 *
 * On a fast Amiga with a graphics card's 15 to 32-bit screen (as
 * measured, sm_alpha_allow: not by the CPU's name, a 25 MHz 68030 keeps
 * the masks) the text is blended instead: all 16 coverage steps, each
 * pixel the colour that far from what is under it to the ink, read and
 * written back in one ReadPixelArray and one WritePixelArray (an opaque
 * box: no reading, the background's colour). Truer edges than 7 pens
 * can give, on any background (a picture, Flow).
 *
 * The logo is a small palette image: one mask per colour, like the pixel
 * style's logo.
 */
#include "amb.h"

#include <string.h>
#include <stdio.h>
#include <graphics/view.h>
#include <graphics/clip.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

extern struct Library *CyberGfxBase;
static BOOL alpha_allowed;      /* fast enough (main.c, by the speed measured) */

void sm_alpha_allow(BOOL on)
{
    alpha_allowed = on;
    amb_log("smooth style", on ? "text blended in 16 steps where the screen is 15 to 32-bit (fast here)"
                                : "text with masks (not fast enough here, or Ambiance/TextBlend off)");
}

#define NFACES      3
#define GLYPH_BYTES 10          /* u16 advance (1/16 px), s8 xoff, s8 yoff, u8 w, u8 h, u32 offset */
#define G_X 2
#define G_Y 3
#define G_W 4
#define G_H 5
#define PHASES      4           /* each glyph at 0, 1/4, 1/2, 3/4 of a pixel */
#define FIRST_PEN   32          /* below: UI pens 0-15, the screen's 16-31 */
#define WB_SPARE    8           /* Workbench's pens left free for everyone else
                                 * (a video overlay's colour key needs one, other
                                 * programs theirs): shades past that are the
                                 * nearest colours */
#define MAX_TEXT_W  2048        /* widest text drawn in one go, in pixels */

struct Face {
    UWORD lo, hi;
    const UBYTE *gl;            /* GLYPH_BYTES per character lo..hi */
    const UBYTE *blob;          /* coverage data */
    UWORD widest_cap;           /* widest A-Z, in pixels */
    UWORD glo, ghi;             /* Greek code points, if the file has them */
    const UBYTE *ggl, *gblob;   /*   their glyphs and coverage data (NULL none) */
};

struct Smooth {
    UBYTE *data;
    ULONG size;
    UWORD k, cell_h, base;
    struct Face face[NFACES + 1];   /* the last: the layouts' title face (TITL), if there */
    UWORD title_h, title_base;      /* its cell, 0: none */
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
    BOOL ran_out;               /* said so in the log */
    UWORD held_low;             /* ...of them below FIRST_PEN, kept unused */
    ULONG got_rgb[256];         /* ...and their colours, 0xRRGGBB */
    UBYTE got[256];             /* 8-bit screens: the pens obtained, */
    UWORD ngot;                 /*   to let go */
    BOOL chunky;                /* RTG: one WriteChunkyPixels a box */
    UBYTE *cbuf;                /* its pixels: a step, then a pen, each */
    ULONG cbuf_size;
    UWORD cmod;                 /* bytes a row of cbuf (a multiple of 16) */
    BOOL alpha_ok;              /* a graphics card's 15-32 bit screen: blending possible */
    UBYTE read_order;           /* ReadPixelArray's ARGB as written (0), red and blue swapped
                                   (1: some drivers' 16-bit modes), or not to be trusted (2) */
    BOOL alpha;                 /* this box blended (cbuf: coverage 0-15) */
    ULONG *argb;                /* its pixels, read and written */
    ULONG argb_size;
    BOOL direct_said;           /* (alpha_direct's first: logged) */
};

/* coverage 0..15 -> how much ink, 0..15, blended: the faintest fringe
 * left out and the nearly full made full, as step_of does, linear
 * between in sixteen steps */
static const UBYTE alpha_of[16] = { 0, 0, 0, 1, 3, 4, 6, 7, 8, 10, 11, 13, 14, 15, 15, 15 };

/* coverage 0..15 -> step 0..SMOOTH_LEVELS-1, with some contrast: the
 * faintest fringe (under a fifth covered) left out, the nearly full
 * (over four fifths) made full, linear between. Crisper than the plain
 * ramp, which looked too soft, and still smooth. */
static const UBYTE step_of[16] = { 0, 0, 0, 0, 1, 2, 2, 3, 4, 5, 5, 6, 7, 7, 7, 7 };

/* Each glyph starts on a whole pixel (its stems on whole pixels, not
 * spread over two at half strength); the advances are still kept in
 * sixteenths, so the spacing stays true over a line. */
#define SNAP_Q(pen16) ((((pen16) + 8) >> 4) << 2)  /* quarter pixels, phase 0 */

static UBYTE nib(const UBYTE *d, ULONG i)
{
    UBYTE b = d[i >> 1];
    return (i & 1) ? (b & 15) : (b >> 4);
}

/* ------------------------------------------------------------ loading */
/* Face face (TS_*, or SM_TITLE: the layouts' title face, else bold) */
static const struct Face *face_of(const struct Smooth *s, int face)
{
    if (face == SM_TITLE)
        return &s->face[s->title_h ? NFACES : 1];
    return &s->face[face >= 0 && face < NFACES ? face : 0];
}

static BOOL parse(struct Smooth *s)
{
    const UBYTE *p = s->data, *end = s->data + s->size;
    int f;
    if (s->size < 14 || memcmp(p, "AMSM", 4) || amb_be16(p + 4) != 3 || amb_be16(p + 12) != NFACES)
        return FALSE;
    s->k = amb_be16(p + 6);
    s->cell_h = amb_be16(p + 8);
    s->base = amb_be16(p + 10);
    p += 14;
    for (f = 0; f < NFACES; f++) {
        struct Face *fc = &s->face[f];
        ULONG n;
        UBYTE c;
        if (p + 4 > end)
            return FALSE;
        fc->lo = amb_be16(p);
        fc->hi = amb_be16(p + 2);
        if (fc->hi < fc->lo || fc->lo > '?' || fc->hi < 'Z')
            return FALSE;
        p += 4;
        fc->gl = p;
        p += (ULONG)(fc->hi - fc->lo + 1) * GLYPH_BYTES;
        if (p + 4 > end)
            return FALSE;
        n = amb_be32(p);
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
    s->icon_n = amb_be16(p);
    s->icon_cnt = amb_be16(p + 2);
    p += 4;
    s->icons = p;
    s->icon_bytes = ((ULONG)s->icon_n * s->icon_n + 1) / 2;
    p += s->icon_bytes * s->icon_cnt;
    if (p + 4 > end)
        return FALSE;
    s->mark_n = amb_be16(p);
    s->mark_cnt = amb_be16(p + 2);
    p += 4;
    s->marks = p;
    s->mark_bytes = ((ULONG)s->mark_n * s->mark_n + 1) / 2;
    p += s->mark_bytes * s->mark_cnt;
    if (p + 6 > end)
        return FALSE;
    s->logo_w = amb_be16(p);
    s->logo_h = amb_be16(p + 2);
    s->logo_nc = amb_be16(p + 4);
    p += 6;
    s->logo_pal = p;
    p += 3 * (ULONG)s->logo_nc;
    s->logo_idx = p;
    p += (ULONG)s->logo_w * s->logo_h;
    if (p + 4 > end)
        return FALSE;
    s->nletters = amb_be16(p);
    s->letter_cell_w = amb_be16(p + 2);
    p += 4;
    if (s->nletters != 27)
        return FALSE;
    for (f = 0; f < 27; f++) {
        if (p + 8 > end)
            return FALSE;
        s->letter[f] = p;
        p += 8 + amb_be32(p + 4);
    }
    /* Greek glyphs, if the file has them: "GREK", then per face u16 lo, hi,
     * the glyphs, u32 bytes, the coverage data (as above). A file
     * without them is drawn as before, its Greek from cjk.c's pack. */
    if (p + 4 <= end && !memcmp(p, "GREK", 4)) {
        const UBYTE *q = p + 4;
        struct Face gf[NFACES];
        for (f = 0; f < NFACES; f++) {
            ULONG n;
            if (q + 4 > end)
                break;
            gf[f].glo = amb_be16(q);
            gf[f].ghi = amb_be16(q + 2);
            if (gf[f].ghi < gf[f].glo || gf[f].glo < 0x380 || gf[f].ghi > 0x3FF)
                break;
            q += 4;
            gf[f].ggl = q;
            q += (ULONG)(gf[f].ghi - gf[f].glo + 1) * GLYPH_BYTES;
            if (q + 4 > end)
                break;
            n = amb_be32(q);
            q += 4;
            gf[f].gblob = q;
            q += n;
            if (q > end)
                break;
        }
        if (f == NFACES) {
            for (f = 0; f < NFACES; f++) {
                s->face[f].glo = gf[f].glo;
                s->face[f].ghi = gf[f].ghi;
                s->face[f].ggl = gf[f].ggl;
                s->face[f].gblob = gf[f].gblob;
            }
            p = q;
        }
    }
    /* the layouts' title face, if the file has it: "TITL", u16 its cell's
     * height and baseline, then a face as above */
    s->title_h = 0;
    if (p + 12 <= end && !memcmp(p, "TITL", 4)) {
        struct Face *fc = &s->face[NFACES];
        UWORD th = amb_be16(p + 4), tb = amb_be16(p + 6);
        const UBYTE *q = p + 8;
        ULONG n;
        fc->lo = amb_be16(q);
        fc->hi = amb_be16(q + 2);
        q += 4;
        fc->gl = q;
        q += (ULONG)(fc->hi - fc->lo + 1) * GLYPH_BYTES;
        if (fc->hi >= fc->lo && fc->lo <= '?' && fc->hi >= 'Z' && q + 4 <= end) {
            n = amb_be32(q);
            fc->blob = q + 4;
            if (fc->blob + n <= end) {
                UBYTE c;
                fc->widest_cap = 0;
                for (c = 'A'; c <= 'Z'; c++)
                    if (fc->gl[(c - fc->lo) * GLYPH_BYTES + G_W] > fc->widest_cap)
                        fc->widest_cap = fc->gl[(c - fc->lo) * GLYPH_BYTES + G_W];
                fc->glo = fc->ghi = 0;
                fc->ggl = fc->gblob = NULL;
                s->title_h = th;
                s->title_base = tb;
            }
        }
    }
    return p <= end && s->icon_cnt >= ICON_COUNT && s->mark_cnt >= MARK_RADIO_ON &&
           s->logo_nc <= 32;
}

UWORD sm_title_h(const struct Smooth *s)
{
    return s && s->title_h ? s->title_h : s ? s->cell_h : 0;
}

BOOL sm_greek(const struct Smooth *s)
{
    return s && s->face[0].ggl != NULL;
}

struct Smooth *sm_load(UWORD lines)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    struct Smooth *s;
    char path[48];
    BPTR fh;
    LONG size;

    sprintf(path, "PROGDIR:Fonts/amb-smooth/%lu", (unsigned long)lines);
    me->pr_WindowPtr = (APTR)-1;            /* no "insert volume" requester */
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh) {
        amb_log("smooth style: no file", path);
        return NULL;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    s = AllocVec(sizeof(*s), MEMF_ANY | MEMF_CLEAR);
    if (s && size > 0 && (s->data = AllocVec(size, MEMF_ANY))) {
        s->size = size;
        if (Read(fh, s->data, size) == size && parse(s)) {
            Close(fh);
            amb_log("smooth style loaded", path);
            return s;
        }
    }
    Close(fh);
    amb_log("smooth style: bad file", path);
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
    s->held_low = 0;
    s->ran_out = FALSE;
    s->chunky = FALSE;
    if (!g->planar && GfxBase->LibNode.lib_Version >= 40) {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR oldwin = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        l = Lock((CONST_STRPTR)"PROGDIR:Ambiance.chunkytext", SHARED_LOCK);
        me->pr_WindowPtr = oldwin;
        if (l)
            UnLock(l);
        s->chunky = l != 0;
    }
    s->alpha_ok = g->truecolor && !g->planar && CyberGfxBase && g->depth >= 15;
    s->read_order = 2;
    if (s->alpha_ok) {
        /* what ReadPixelArray gives back of what WritePixelArray wrote, on
         * a bitmap like the screen's: on some 16-bit modes its red and
         * blue come back swapped (text on a navy row turned brown) */
        struct BitMap *bm = AllocBitMap(4, 1, GetBitMapAttr(g->rp->BitMap, BMA_DEPTH), BMF_MINPLANES,
                                        g->rp->BitMap);
        if (bm) {
            struct RastPort trp;
            /* (4 wide: rtg.library 43.787 reads the last width mod 4 of a
             * 16-bit row wrongly, which masks_end reads round) */
            ULONG wr[4] = { 0x00F80000UL, 0x000000F8UL, 0, 0 }, rd[4] = { 0, 0, 0, 0 };
            InitRastPort(&trp);
            trp.BitMap = bm;
            WritePixelArray(wr, 0, 0, 16, &trp, 0, 0, 4, 1, RECTFMT_ARGB);
            ReadPixelArray(rd, 0, 0, 16, &trp, 0, 0, 4, 1, RECTFMT_ARGB);
            WaitBlit();
            FreeBitMap(bm);
            if ((rd[0] & 0xF00000) == 0xF00000 && !(rd[0] & 0xF0) && (rd[1] & 0xF0) == 0xF0 &&
                !(rd[1] & 0xF00000))
                s->read_order = 0;
            else if ((rd[0] & 0xF0) == 0xF0 && !(rd[0] & 0xF00000) && (rd[1] & 0xF00000) == 0xF00000 &&
                     !(rd[1] & 0xF0))
                s->read_order = 1;
            {
                char m[100];
                sprintf(m, "ReadPixelArray gives back %08lx %08lx of 00f80000 000000f8: %s", rd[0], rd[1],
                        s->read_order == 0 ? "as written" : s->read_order == 1 ? "red and blue swapped"
                                                                             : "not used");
                amb_log("smooth style", m);
            }
        }
    }
    amb_log("smooth style", s->chunky ? "text drawn as chunky pixels"
                             : s->alpha_ok && alpha_allowed ? "text blended (16 steps, over what is there)"
                                                            : "text drawn with masks");
}

UWORD sm_pens_used(const struct Smooth *s)
{
    return s->g && (!s->g->truecolor || s->g->wb) ? s->ngot : s->next_pen - FIRST_PEN;
}

void sm_detach(struct Smooth *s)
{
    if (s->g && s->g->scr && s->ngot) {
        UWORD i;
        char m[80];
        sprintf(m, "%lu pens for its shades and logo (%lu below 32 kept unused)",
                (unsigned long)(s->ngot - s->held_low), (unsigned long)s->held_low);
        amb_log("smooth style", m);
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
    if (s->cbuf)
        FreeVec(s->cbuf);
    s->cbuf = NULL;
    s->cbuf_size = 0;
    if (s->argb)
        FreeVec(s->argb);
    s->argb = NULL;
    s->argb_size = 0;
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
    if (!s->g->truecolor || s->g->wb) {
        /* (a window on Workbench likewise: its pens are everyone's, and
         * one set to a shade would change what other programs draw)
         * a real pen: any free one from 32 up, held while the screen is
         * open. The UI's own pens (0-15) and the pointer's (16-31) are not
         * obtained by anyone (the screen shares its pens), so ObtainPen
         * can hand them out; a shade there was changed back under the
         * text whenever the palette or the pointer was set again. Those
         * are kept (so they are not offered again) but not used. */
        for (;;) {
            LONG p;
            if (s->ngot >= sizeof(s->got))
                return 0;
            if (s->g->wb && s->g->scr->ViewPort.ColorMap->PalExtra &&
                s->g->scr->ViewPort.ColorMap->PalExtra->pe_NFree <= WB_SPARE)
                return 0;                   /* (the rest left for others) */
            p = (LONG)ObtainPen(s->g->scr->ViewPort.ColorMap, -1, r * 0x01010101UL,
                                     gg * 0x01010101UL, b * 0x01010101UL, PEN_EXCLUSIVE);
            if (p < 0)
                return 0;                   /* none left: the nearest end */
            s->got_rgb[s->ngot] = p >= FIRST_PEN ? (r << 16) | (gg << 8) | b : 0xFFFFFFFFUL;
            s->got[s->ngot++] = (UBYTE)p;
            if (p >= FIRST_PEN)
                return (UBYTE)p;
            s->held_low++;
        }
    }
    if (s->next_pen > s->last_pen)
        return 0;
    pen = (UBYTE)s->next_pen++;
    SetRGB32(&s->g->scr->ViewPort, pen, r * 0x01010101UL, gg * 0x01010101UL, b * 0x01010101UL);
    return pen;
}

/* Out of pens: the nearest colour among the UI's and the shades' pens
 * (much softer than the nearest end of the ramp). */
static UBYTE nearest_pen(struct Smooth *s, ULONG r, ULONG gg, ULONG b, UBYTE fallback)
{
    ULONG best = 0xFFFFFFFFUL;
    UBYTE pen = fallback;
    UWORD i;
    for (i = 0; i < NUM_UI_PENS + s->ngot; i++) {
        ULONG c, d;
        LONG dr, dg, db;
        UBYTE p;
        if (i < NUM_UI_PENS) {
            p = (UBYTE)i;
            c = (ui_rgb(p, 0) << 16) | (ui_rgb(p, 1) << 8) | ui_rgb(p, 2);
        } else {
            p = s->got[i - NUM_UI_PENS];
            c = s->got_rgb[i - NUM_UI_PENS];
            if (c == 0xFFFFFFFFUL)
                continue;
        }
        dr = (LONG)((c >> 16) & 255) - (LONG)r;
        dg = (LONG)((c >> 8) & 255) - (LONG)gg;
        db = (LONG)(c & 255) - (LONG)b;
        d = (ULONG)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
        if (d < best) {
            best = d;
            pen = p;
        }
    }
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
    if (s->g && !s->g->truecolor) {
        /* 8-bit: pens are scarce, so 3 shades a pair, not 6 (1/4, 1/2 and
         * 3/4 of the way: steps 1-2, 3-4, 5-6) */
        UBYTE q = (UBYTE)((lv + 1) / 2);
        slot = &s->blend[fg][bg][q];
        if (*slot)
            return *slot;
        for (c = 0; c < 3; c++) {
            LONG a = (LONG)ui_rgb(bg, c), d = (LONG)ui_rgb(fg, c) - a;
            rgb[c] = (ULONG)(a + d * q / 4);
        }
    } else {
        slot = &s->blend[fg][bg][lv];
        if (*slot)
            return *slot;
        for (c = 0; c < 3; c++) {
            LONG a = (LONG)ui_rgb(bg, c), d = (LONG)ui_rgb(fg, c) - a;
            rgb[c] = (ULONG)(a + d * lv / (SMOOTH_LEVELS - 1));
        }
    }
    pen = alloc_rgb(s, rgb[0], rgb[1], rgb[2]);
    if (!pen) {                             /* out of pens: the nearest colour */
        pen = s->g && !s->g->truecolor
                  ? nearest_pen(s, rgb[0], rgb[1], rgb[2], lv * 2 >= SMOOTH_LEVELS - 1 ? fg : bg)
                  : (lv * 2 >= SMOOTH_LEVELS - 1 ? fg : bg);
        if (!s->ran_out) {
            s->ran_out = TRUE;
            amb_log("smooth style", "out of pens for shades: the nearest colours used");
        }
    }
    *slot = pen;
    return pen;
}

/* ------------------------------------------------------------ masks */
/* Room for SMOOTH_LEVELS masks of w x h, cleared. */
static BOOL masks_begin(struct Smooth *s, UWORD w, UWORD h, BOOL opaque)
{
    ULONG need;
    /* blended: over what is there where ReadPixelArray can be trusted,
     * else only an opaque box (from its background's colour) */
    s->alpha = s->alpha_ok && alpha_allowed && !s->chunky;
    (void)opaque;   /* (masks_end blends it in the bitmap's own pixels, else by Read/WritePixelArray
                       where ReadPixelArray can be trusted, else draws it as an opaque box) */
    if (s->alpha) {
        need = (ULONG)((w + 3) & ~3) * h * 4;      /* (read 4 pixels at a time: see masks_end) */
        if (need > s->argb_size) {
            if (s->argb)
                FreeVec(s->argb);
            s->argb_size = 0;
            if (!(s->argb = AllocVec(need, MEMF_ANY)))
                s->alpha = FALSE;           /* (no memory: the masks) */
            else
                s->argb_size = need;
        }
    }
    if (s->chunky || s->alpha) {
        s->cmod = (w + 15) & ~15;
        need = (ULONG)s->cmod * h;
        if (need > s->cbuf_size) {
            if (s->cbuf)
                FreeVec(s->cbuf);
            s->cbuf_size = 0;
            if (!(s->cbuf = AllocVec(need, MEMF_ANY)))
                return FALSE;
            s->cbuf_size = need;
        }
        memset(s->cbuf, 0, need);
        s->used = 0;
        return TRUE;
    }
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
    if (s->chunky || s->alpha) {
        const UBYTE *map = s->alpha ? alpha_of : step_of;
        UBYTE *row = s->cbuf + (ULONG)y * s->cmod + x;
        for (j = 0; j < h; j++, row += s->cmod) {
            for (i = 0; i < w; i++, n++) {
                UBYTE lv = map[nib(d, n)];
                if (lv > row[i]) {          /* glyphs that touch: the more ink */
                    row[i] = lv;
                    used |= 1 << (lv & 7);
                }
            }
        }
        s->used = used;
        return;
    }
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

/* ------------------------------------------------------------ blended, in the bitmap's own pixels */

/* a pixel of format fmt at p as r, g, b (0-255 each, the low bits as
 * stored: 5 or 6 bits shifted up) */
static BOOL px_get(ULONG fmt, const UBYTE *p, ULONG *r, ULONG *gg, ULONG *b)
{
    ULONG v;
    switch (fmt) {
    case PIXFMT_RGB16: case PIXFMT_BGR16: case PIXFMT_RGB15: case PIXFMT_BGR15:
        v = (ULONG)p[0] << 8 | p[1];
        break;
    case PIXFMT_RGB16PC: case PIXFMT_BGR16PC: case PIXFMT_RGB15PC: case PIXFMT_BGR15PC:
        v = (ULONG)p[1] << 8 | p[0];
        break;
    case PIXFMT_RGB24:  *r = p[0]; *gg = p[1]; *b = p[2]; return TRUE;
    case PIXFMT_BGR24:  *r = p[2]; *gg = p[1]; *b = p[0]; return TRUE;
    case PIXFMT_ARGB32: *r = p[1]; *gg = p[2]; *b = p[3]; return TRUE;
    case PIXFMT_BGRA32: *r = p[2]; *gg = p[1]; *b = p[0]; return TRUE;
    case PIXFMT_RGBA32: *r = p[0]; *gg = p[1]; *b = p[2]; return TRUE;
    default: return FALSE;
    }
    if (fmt == PIXFMT_RGB16 || fmt == PIXFMT_BGR16 || fmt == PIXFMT_RGB16PC || fmt == PIXFMT_BGR16PC) {
        *r = (v >> 11) << 3;
        *gg = ((v >> 5) & 63) << 2;
    } else {
        *r = ((v >> 10) & 31) << 3;
        *gg = ((v >> 5) & 31) << 3;
    }
    *b = (v & 31) << 3;
    if (fmt == PIXFMT_BGR16 || fmt == PIXFMT_BGR16PC || fmt == PIXFMT_BGR15 || fmt == PIXFMT_BGR15PC) {
        ULONG t = *r;
        *r = *b;
        *b = t;
    }
    return TRUE;
}

static void px_put(ULONG fmt, UBYTE *p, ULONG r, ULONG gg, ULONG b)
{
    ULONG v;
    switch (fmt) {
    case PIXFMT_RGB24:  p[0] = (UBYTE)r; p[1] = (UBYTE)gg; p[2] = (UBYTE)b; return;
    case PIXFMT_BGR24:  p[0] = (UBYTE)b; p[1] = (UBYTE)gg; p[2] = (UBYTE)r; return;
    case PIXFMT_ARGB32: p[0] = 0xFF; p[1] = (UBYTE)r; p[2] = (UBYTE)gg; p[3] = (UBYTE)b; return;
    case PIXFMT_BGRA32: p[0] = (UBYTE)b; p[1] = (UBYTE)gg; p[2] = (UBYTE)r; p[3] = 0xFF; return;
    case PIXFMT_RGBA32: p[0] = (UBYTE)r; p[1] = (UBYTE)gg; p[2] = (UBYTE)b; p[3] = 0xFF; return;
    default: break;
    }
    if (fmt == PIXFMT_BGR16 || fmt == PIXFMT_BGR16PC || fmt == PIXFMT_BGR15 || fmt == PIXFMT_BGR15PC) {
        ULONG t = r;
        r = b;
        b = t;
    }
    if (fmt == PIXFMT_RGB16 || fmt == PIXFMT_BGR16 || fmt == PIXFMT_RGB16PC || fmt == PIXFMT_BGR16PC)
        v = (r >> 3) << 11 | (gg >> 2) << 5 | (b >> 3);
    else
        v = (r >> 3) << 10 | (gg >> 3) << 5 | (b >> 3);
    if (fmt == PIXFMT_RGB16PC || fmt == PIXFMT_BGR16PC || fmt == PIXFMT_RGB15PC || fmt == PIXFMT_BGR15PC) {
        p[0] = (UBYTE)v;
        p[1] = (UBYTE)(v >> 8);
    } else {
        p[0] = (UBYTE)(v >> 8);
        p[1] = (UBYTE)v;
    }
}

/* The box blended straight in the bitmap's memory, in its own pixels (no
 * ReadPixelArray, no ARGB on the way): where the RastPort's layer is the
 * only one on its bitmap, at its corner, with no clip region (Ambiance's
 * window buffer, its double buffers), so its pixels are the bitmap's.
 * FALSE: not so here (the caller goes by Read/WritePixelArray). Nothing
 * else is drawn while the bitmap is locked. */
static BOOL alpha_direct(struct Smooth *s, struct RastPort *rp, WORD x, WORD y, UWORD w, UWORD h,
                         const ULONG *f, UBYTE bg, BOOL opaque)
{
    struct Layer *l = rp->Layer;
    struct BitMap *bm = rp->BitMap;
    APTR lk, base = NULL;
    ULONG bpr = 0, bpp = 0, fmt = 0, r, gg, b, k;
    ULONG lut[16][3];
    const UBYTE *c = s->cbuf;
    UWORD i, j;
    if (!bm || x < 0 || y < 0)
        return FALSE;
    if (l && (l->ClipRegion || l->front || l->back || l->bounds.MinX || l->bounds.MinY ||
              x + w - 1 > l->bounds.MaxX || y + h - 1 > l->bounds.MaxY))
        return FALSE;
    if ((ULONG)(x + w) > GetBitMapAttr(bm, BMA_WIDTH) || (ULONG)(y + h) > GetBitMapAttr(bm, BMA_HEIGHT) ||
        !GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX))
        return FALSE;
    if (opaque)
        for (k = 0; k < 16; k++) {
            int ch;
            for (ch = 0; ch < 3; ch++) {
                LONG bc = (LONG)ui_rgb(bg, ch);
                lut[k][ch] = (ULONG)(bc + ((LONG)f[ch] - bc) * (LONG)k / 15);
            }
        }
    if (!(lk = LockBitMapTags(bm, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr,
                              LBMI_BYTESPERPIX, (ULONG)&bpp, LBMI_PIXFMT, (ULONG)&fmt, TAG_DONE)))
        return FALSE;
    if (!base || bpp < 2 || bpp > 4 || !px_get(fmt, (const UBYTE *)base, &r, &gg, &b)) {
        UnLockBitMap(lk);
        return FALSE;
    }
    for (j = 0; j < h; j++, c += s->cmod) {
        UBYTE *p = (UBYTE *)base + (ULONG)(y + j) * bpr + (ULONG)x * bpp;
        for (i = 0; i < w; i++, p += bpp) {
            ULONG a = c[i];
            if (opaque) {
                px_put(fmt, p, lut[a][0], lut[a][1], lut[a][2]);
                continue;
            }
            if (!a)
                continue;
            px_get(fmt, p, &r, &gg, &b);
            r = (ULONG)((LONG)r + ((LONG)f[0] - (LONG)r) * (LONG)a / 15);
            gg = (ULONG)((LONG)gg + ((LONG)f[1] - (LONG)gg) * (LONG)a / 15);
            b = (ULONG)((LONG)b + ((LONG)f[2] - (LONG)b) * (LONG)a / 15);
            px_put(fmt, p, r, gg, b);
        }
    }
    UnLockBitMap(lk);
    if (!s->direct_said) {
        char m[100];
        s->direct_said = TRUE;
        sprintf(m, "text blended straight in the bitmap's own pixels (format %lu, %lu bytes a pixel)",
                (unsigned long)fmt, (unsigned long)bpp);
        amb_log("smooth style", m);
    }
    return TRUE;
}

/* Stamp the masks at (x, y). Opaque: the uncovered pixels of the box get
 * bg too, in the same pass, so the text needs no clearing first. */
static void masks_end(struct Smooth *s, struct Gfx *g, WORD x, WORD y, UWORD w, UWORD h,
                      UBYTE fg, UBYTE bg, BOOL opaque)
{
    gfx_dirty();
    struct RastPort *rp = g->rp;
    UBYTE lv;
    if (s->alpha && fg < NUM_UI_PENS && bg < NUM_UI_PENS) {
        /* blended: each pixel that far from what is under it (an opaque
         * box: from bg's colour) to fg's, in sixteen steps */
        ULONG *px = s->argb, k, f[3];
        const UBYTE *c = s->cbuf;
        UWORD i, j;
        int ch;
        for (ch = 0; ch < 3; ch++)
            f[ch] = ui_rgb(fg, ch);
        if (alpha_direct(s, rp, x, y, w, h, f, bg, opaque))
            return;
        if (!opaque && s->read_order >= 2)
            goto fallback;                  /* (not readable here: an opaque box, below) */
        if (opaque) {
            ULONG lut[16];
            for (k = 0; k < 16; k++) {
                ULONG v = 0;
                for (ch = 0; ch < 3; ch++) {
                    LONG b = (LONG)ui_rgb(bg, ch);
                    v = (v << 8) | (ULONG)(b + ((LONG)f[ch] - b) * (LONG)k / 15);
                }
                lut[k] = v;
            }
            for (j = 0; j < h; j++, c += s->cmod)
                for (i = 0; i < w; i++)
                    *px++ = lut[c[i]];
        } else {
            /* what is there read a multiple of 4 pixels wide: rtg.library
             * 43.787 gives the last (width mod 4) of a 16-bit row back as
             * packed words (tools/rpabug.c), so the box is read wider (to
             * the left at the bitmap's right edge) and its pixels taken
             * from the middle */
            BOOL swap = s->read_order == 1;
            UWORD rw = (w + 3) & ~3, ofs = 0;
            WORD rx = x;
            if (rx + rw > g->w) {
                rx = g->w - rw;
                if (rx < 0)
                    rx = 0;
                ofs = (UWORD)(x - rx);
            }
            if (ofs + w > rw)
                ofs = 0, rw = w;                /* (narrower than 4 at all: as it is) */
            ReadPixelArray(px, 0, 0, (UWORD)(rw * 4), rp, rx, y, rw, h, RECTFMT_ARGB);
            for (j = 0; j < h; j++, c += s->cmod) {
                px = s->argb + (ULONG)j * rw + ofs;
                for (i = 0; i < w; i++, px++) {
                    ULONG a = c[i], p = *px, v = 0;
                    if (swap)
                        *px = p = (p & 0xFF00FF00UL) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
                    if (!a)
                        continue;
                    for (ch = 16; ch >= 0; ch -= 8) {
                        LONG b = (LONG)((p >> ch) & 255);
                        v = (v << 8) | (ULONG)(b + ((LONG)f[2 - ch / 8] - b) * (LONG)a / 15);
                    }
                    *px = v;
                }
            }
            WritePixelArray(s->argb, ofs, 0, (UWORD)(rw * 4), rp, x, y, w, h, RECTFMT_ARGB);
            return;
        }
        WritePixelArray(s->argb, 0, 0, (UWORD)(w * 4), rp, x, y, w, h, RECTFMT_ARGB);
        return;
    }
fallback:
    if (s->alpha) {
        /* (a colour not of the UI's, or nothing to blend over: the steps
         * as the chunky path's, an opaque box) */
        UBYTE *c = s->cbuf;
        ULONG k, n = (ULONG)s->cmod * h;
        for (k = 0; k < n; k++)
            c[k] = step_of[c[k]];
        s->used = 0xFE;
        s->alpha = FALSE;                   /* (or this again, and again: the stack gone) */
        s->chunky = TRUE;
        masks_end(s, g, x, y, w, h, fg, bg, opaque);
        s->chunky = FALSE;
        return;
    }
    if (s->chunky) {
        /* each step to its pen, then the box in one write */
        UBYTE pen[SMOOTH_LEVELS], *c = s->cbuf;
        ULONG k, n = (ULONG)s->cmod * h;
        pen[0] = GPEN(bg);
        for (lv = 1; lv < SMOOTH_LEVELS; lv++)
            pen[lv] = GPEN((s->used & (1 << lv)) ? sm_pen(s, g, fg, bg, lv) : bg);
        for (k = 0; k < n; k++)
            c[k] = pen[c[k]];
        WriteChunkyPixels(rp, x, y, x + w - 1, y + h - 1, s->cbuf, s->cmod);
        return;
    }
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
        SetAPen(rp, GPEN(bg));
        BltTemplate((PLANEPTR)s->pl[0], 0, s->bpr, rp, x, y, w, h);
    }
    for (lv = 1; lv < SMOOTH_LEVELS; lv++)
        if (s->used & (1 << lv)) {
            UBYTE pen = sm_pen(s, g, fg, bg, lv);
            PLANEPTR m = (PLANEPTR)s->pl[lv];
            SetAPen(rp, GPEN(pen));
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

/* The glyph at str[*i]: a Latin-1 byte, or a Greek letter's two bytes
 * (cjk.c); its entry, and in *blob the coverage data its offset is in.
 * *i is moved past it. A Greek letter without a glyph is a "?"; a first
 * byte left alone is the Latin-1 table's (empty) entry for it. */
static const UBYTE *glyph_at(const struct Face *f, const char *str, LONG n, LONG *i,
                             const UBYTE **blob)
{
    UBYTE c = (UBYTE)str[*i];
    *blob = f->blob;
    if ((c == 0x80 || c == 0x81) && f->ggl && *i + 1 < n) {
        UBYTE c2 = (UBYTE)str[*i + 1];
        if (c2 >= 0x80 && c2 <= 0xBF) {
            UWORD cp = (UWORD)(0x380 + ((c & 1) << 6) + (c2 & 63));
            *i += 2;
            if (cp >= f->glo && cp <= f->ghi) {
                const UBYTE *G = f->ggl + (ULONG)(cp - f->glo) * GLYPH_BYTES;
                if (amb_be16(G)) {
                    *blob = f->gblob;
                    return G;
                }
            }
            return glyph(f, '?');
        }
    }
    (*i)++;
    return glyph(f, c);
}

/* Text is laid out in 1/16 pixels, each glyph drawn from the stored
 * quarter-pixel position nearest its true one. */
static UWORD adv16(const UBYTE *G) { return amb_be16(G); }

WORD sm_textlen(struct Smooth *s, const char *str, LONG n, int face)
{
    const struct Face *f = face_of(s, face);
    const UBYTE *b;
    ULONG w16 = 0;
    LONG i = 0;
    while (i < n)
        w16 += adv16(glyph_at(f, str, n, &i, &b));
    return (WORD)((w16 + 8) >> 4);
}

/* How many bytes of str fit in maxw pixels (whole characters). */
LONG sm_fit(struct Smooth *s, const char *str, LONG n, int face, WORD maxw)
{
    const struct Face *f = face_of(s, face);
    const UBYTE *b;
    LONG i = 0, at;
    ULONG w16 = 0;
    while (i < n) {
        at = i;
        w16 += adv16(glyph_at(f, str, n, &i, &b));
        if ((WORD)((w16 + 8) >> 4) > maxw)
            return at;
    }
    return n;
}

/* The box every glyph's ink falls in (accents may rise above the cell),
 * from the pen position: minx..maxx, miny..maxy; opaque: at least as
 * wide as the text's advance. */
static void text_box(const struct Smooth *s, const struct Face *f, const char *str, LONG n, BOOL opaque,
                     WORD *minx, WORD *maxx, WORD *miny, WORD *maxy)
{
    ULONG pen16 = 0;
    LONG i;
    *minx = *maxx = *miny = 0;
    *maxy = f == &s->face[NFACES] ? s->title_h : s->cell_h;
    for (i = 0; i < n;) {
        const UBYTE *b, *G = glyph_at(f, str, n, &i, &b);
        ULONG q = SNAP_Q(pen16);                /* in quarter pixels */
        WORD gx = (WORD)(q >> 2) + (BYTE)G[G_X], gy = (BYTE)G[G_Y];
        if (G[G_W]) {
            if (gx < *minx)
                *minx = gx;
            if (gx + G[G_W] > *maxx)
                *maxx = gx + G[G_W];
            if (gy < *miny)
                *miny = gy;
            if (gy + G[G_H] > *maxy)
                *maxy = gy + G[G_H];
        }
        pen16 += adv16(G);
    }
    if (opaque && (WORD)((pen16 + 8) >> 4) > *maxx)
        *maxx = (WORD)((pen16 + 8) >> 4);
}

/* Each glyph's coverage into the masks (or cbuf), the box's corner at
 * minx, miny from the pen position. */
static void text_put(struct Smooth *s, const struct Face *f, const char *str, LONG n, WORD minx, WORD miny)
{
    ULONG pen16 = 0;
    LONG i;
    for (i = 0; i < n;) {
        const UBYTE *b, *G = glyph_at(f, str, n, &i, &b);
        ULONG q = SNAP_Q(pen16);
        UWORD w = G[G_W], h = G[G_H];
        if (w) {
            UWORD bytes = (UWORD)(((ULONG)w * h + 1) >> 1), ph = (UWORD)(q & 3);
            const UBYTE *d = b + amb_be32(G + 6);
            while (ph--)
                d += bytes;
            masks_put(s, d, w, h, (WORD)(q >> 2) + (BYTE)G[G_X] - minx, (BYTE)G[G_Y] - miny);
        }
        pen16 += adv16(G);
    }
}

void sm_text(struct Smooth *s, struct Gfx *g, WORD x, WORD y, const char *str, LONG n,
             int face, UBYTE fg, UBYTE bg, BOOL opaque)
{
    const struct Face *f = face_of(s, face);
    WORD minx, maxx, miny, maxy;
    if (n <= 0)
        return;
    text_box(s, f, str, n, opaque, &minx, &maxx, &miny, &maxy);
    if (maxx - minx <= 0 || maxx - minx > MAX_TEXT_W || !masks_begin(s, maxx - minx, maxy - miny, opaque))
        return;
    text_put(s, f, str, n, minx, miny);
    masks_end(s, g, x + minx, y + miny, maxx - minx, maxy - miny, fg, bg, opaque);
}

/* The text's coverage, 0-15 a pixel, for the layouts' drawing kit to
 * draw in colours of its own (lkit.c): a box *w x *h, rows *mod bytes
 * apart, its corner (*dx, *dy) from the pen position (the cell's top
 * left). The buffer is the Smooth's own, good until its next drawing.
 * NULL: nothing to draw (or no memory). */
const UBYTE *sm_cover(struct Smooth *s, const char *str, LONG n, int face, WORD *dx, WORD *dy,
                      UWORD *w, UWORD *h, UWORD *mod)
{
    const struct Face *f = face_of(s, face);
    WORD minx, maxx, miny, maxy;
    BOOL alpha = s->alpha, chunky = s->chunky;
    ULONG need;
    if (n <= 0)
        return NULL;
    text_box(s, f, str, n, FALSE, &minx, &maxx, &miny, &maxy);
    if (maxx - minx <= 0 || maxx - minx > MAX_TEXT_W)
        return NULL;
    *w = (UWORD)(maxx - minx);
    *h = (UWORD)(maxy - miny);
    s->cmod = (*w + 15) & ~15;
    need = (ULONG)s->cmod * *h;
    if (need > s->cbuf_size) {
        if (s->cbuf)
            FreeVec(s->cbuf);
        s->cbuf_size = 0;
        if (!(s->cbuf = AllocVec(need, MEMF_ANY)))
            return NULL;
        s->cbuf_size = need;
    }
    memset(s->cbuf, 0, need);
    s->alpha = TRUE;                        /* (masks_put: coverage in 16 steps, into cbuf) */
    s->chunky = FALSE;
    text_put(s, f, str, n, minx, miny);
    s->alpha = alpha;
    s->chunky = chunky;
    *dx = minx;
    *dy = miny;
    *mod = s->cmod;
    return s->cbuf;
}

/* An icon (ICON_*) as coverage 0-15, n x n, rows mod bytes apart, as
 * sm_cover's text (lkit.c). NULL: no such icon, or no memory. */
const UBYTE *sm_icon_cover(struct Smooth *s, int icon, UWORD *n, UWORD *mod)
{
    BOOL alpha = s->alpha, chunky = s->chunky;
    ULONG need;
    if (icon < 0 || icon >= s->icon_cnt)
        return NULL;
    s->cmod = (s->icon_n + 15) & ~15;
    need = (ULONG)s->cmod * s->icon_n;
    if (need > s->cbuf_size) {
        if (s->cbuf)
            FreeVec(s->cbuf);
        s->cbuf_size = 0;
        if (!(s->cbuf = AllocVec(need, MEMF_ANY)))
            return NULL;
        s->cbuf_size = need;
    }
    memset(s->cbuf, 0, need);
    s->alpha = TRUE;
    s->chunky = FALSE;
    masks_put(s, s->icons + s->icon_bytes * icon, s->icon_n, s->icon_n, 0, 0);
    s->alpha = alpha;
    s->chunky = chunky;
    *n = s->icon_n;
    *mod = s->cmod;
    return s->cbuf;
}

void sm_icon(struct Smooth *s, struct Gfx *g, int icon, WORD x, WORD y, UBYTE fg, UBYTE bg)
{
    if (icon < 0 || icon >= s->icon_cnt || !masks_begin(s, s->icon_n, s->icon_n, FALSE))
        return;
    masks_put(s, s->icons + s->icon_bytes * icon, s->icon_n, s->icon_n, 0, 0);
    masks_end(s, g, x, y, s->icon_n, s->icon_n, fg, bg, FALSE);
}

void sm_mark(struct Smooth *s, struct Gfx *g, int mark, WORD x, WORD y, UBYTE fg, UBYTE bg)
{
    int i = mark - MARK_ARROW;
    if (i < 0 || i >= s->mark_cnt || !masks_begin(s, s->mark_n, s->mark_n, FALSE))
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

const UBYTE *sm_logo_data(struct Smooth *s, UWORD *w, UWORD *h, UWORD *nc, const UBYTE **pal)
{
    if (!s || !s->logo_w || !s->logo_h)
        return NULL;
    *w = s->logo_w;
    *h = s->logo_h;
    *nc = s->logo_nc;
    *pal = s->logo_pal;
    return s->logo_idx;
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
        pens[c] = pen ? pen : GPEN(nearest_ui_pen(rgb));   /* the pen drawn with */
    }
    return logo_masks(g, s->logo_w, s->logo_h, s->logo_nc, s->logo_idx, pens);
}

/* The pixel style on a deep RTG screen: the smooth logo at its whole scale
 * (PROGDIR:Fonts/amb-smooth/logo<scale>), its colours in pens 32 up. */
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
    sprintf(path, "PROGDIR:Fonts/amb-smooth/logo%lu", (unsigned long)scale);
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return FALSE;
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size > 12 && (data = AllocVec(size, MEMF_ANY)) && Read(fh, data, size) == size &&
        !memcmp(data, "AMLG", 4) && amb_be16(data + 4) == 1) {
        UWORD w = amb_be16(data + 6), h = amb_be16(data + 8), nc = amb_be16(data + 10), c;
        const UBYTE *pal = data + 12, *idx = pal + 3 * (ULONG)nc;
        if (nc <= 32 && idx + (ULONG)w * h <= data + size &&
            cm && cm->Count >= FIRST_PEN + nc) {
            for (c = 0; c < nc; c++) {
                const UBYTE *rgb = pal + 3 * c;
                if (g->wb) {                /* Workbench's pens: obtained, not set */
                    pens[c] = gfx_obtain_rgb(g, rgb[0], rgb[1], rgb[2]);
                    continue;
                }
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
    w = amb_be16(L);
    h = amb_be16(L + 2);
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
