/*
 * rtgbench - AmigaMCE's drawing on an RTG screen, 8-bit against 16-bit
 * Copyright (c) 2026 Brian King, MIT
 *
 *   rtgbench [WIDTH n] [HEIGHT n] [DEEP] [QUICK]
 *
 * Opens an RTG screen of the size asked (640 x 480 if none) at 8 bits,
 * then at 16 (and with DEEP at 24/32 too), and times on each what
 * AmigaMCE does: clearing, text, a page of list rows, the scroll a glide
 * makes, whole-screen copies (double buffering's), flips, covers and
 * full-screen pictures (8-bit: colours reduced, WriteChunkyPixels; deeper:
 * WritePixelArray from RGB and WriteLUTPixelArray as thumbnails are), the
 * spectrum's bars, and the CPU writing to the card's memory itself. The
 * table goes to the Shell and to rtgbench.txt beside the program.
 *
 * Each test runs for about a second (QUICK: a third) after one run not
 * timed; times are per run, from the EClock.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/diskfont.h>
#include <proto/timer.h>
#include <proto/cybergraphics.h>
#include <string.h>
#include <stdio.h>
#include "../../src/animdraw.h"

static const char version[] = "$VER: rtgbench 1.1 (08.11.2026)";

struct Library *CyberGfxBase, *DiskfontBase;
struct Device *TimerBase;
extern struct ExecBase *SysBase;
extern struct GfxBase *GfxBase;

static struct timerequest treq;
static ULONG efreq;
static ULONG run_ms = 1000;

#define TESTS 24
static LONG result[4][TESTS];           /* microseconds a run, -1 not run, -2 not here */
static char out[8192];
static int outn;

static void say(const char *s)
{
    int n = strlen(s);
    PutStr((CONST_STRPTR)s);
    if (outn + n < (int)sizeof(out)) {
        memcpy(out + outn, s, n);
        outn += n;
    }
}

static ULONG now_us(void)
{
    struct EClockVal e;
    ReadEClock(&e);
    /* 64-bit ticks to microseconds (efreq is about 709 kHz) */
    return (ULONG)((((unsigned long long)e.ev_hi << 32) | e.ev_lo) * 1000000ULL / efreq);
}

/* ---- the screen under test */
static struct Screen *scr;
static struct RastPort *rp;
static struct TextFont *font;
static WORD W, H, depth;
static UBYTE pen_bg = 0, pen_text = 1, pen_hi = 2, pen_dim = 3, pen_bar = 4;

/* An RTG mode of this size and depth: the display database walked, the
 * foreign (graphics card) ones only, so no native AGA mode is taken. */
static ULONG find_mode(WORD w, WORD h, int d, char *name, int namemax)
{
    ULONG id = INVALID_ID, best = INVALID_ID;
    while ((id = NextDisplayInfo(id)) != INVALID_ID) {
        struct DisplayInfo di;
        struct DimensionInfo dim;
        struct NameInfo ni;
        DisplayInfoHandle hd = FindDisplayInfo(id);
        int md;
        if (!hd || !GetDisplayInfoData(hd, (UBYTE *)&di, sizeof(di), DTAG_DISP, 0) ||
            !GetDisplayInfoData(hd, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, 0))
            continue;
        if (!(di.PropertyFlags & DIPF_IS_FOREIGN) || di.NotAvailable)
            continue;
        if (dim.Nominal.MaxX - dim.Nominal.MinX + 1 != w ||
            dim.Nominal.MaxY - dim.Nominal.MinY + 1 != h)
            continue;
        md = dim.MaxDepth;
        if (!(md == d || (d == 16 && md == 15) || (d == 32 && md == 24)))
            continue;
        best = id;
        if (name) {
            if (GetDisplayInfoData(hd, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, 0))
                strncpy(name, (char *)ni.Name, namemax - 1);
            else
                sprintf(name, "%08lx", (unsigned long)id);
            name[namemax - 1] = 0;
        }
        if (md == d)
            break;
    }
    return best;
}

static void colours(void)
{
    /* a dark blue interface, as AmigaMCE's */
    static const ULONG rgb[8][3] = { { 0x00, 0x10, 0x30 }, { 0xEE, 0xEE, 0xEE }, { 0x30, 0x70, 0xE0 },
                                     { 0x80, 0x90, 0xB0 }, { 0x10, 0x20, 0x50 }, { 0xFF, 0xB0, 0x00 },
                                     { 0x40, 0xC0, 0x60 }, { 0xC0, 0x30, 0x30 } };
    int i;
    for (i = 0; i < 8; i++)
        SetRGB32(&scr->ViewPort, i, rgb[i][0] * 0x01010101UL, rgb[i][1] * 0x01010101UL,
                 rgb[i][2] * 0x01010101UL);
}

/* ---- test data: a "cover" in RGB, its 48-colour version, a thumbnail */
#define CW 256
#define CH 256
static UBYTE *cover_rgb;                /* CW x CH x 3 */
static UBYTE *cover_chunky;             /* CW x CH, 48 colours (pens 16-63) */
static ULONG cover_pal[48], full_pal[48];
static ULONG lut[256];                  /* a thumbnail's colour table (WriteLUTPixelArray) */
static ULONG load48[48 * 3 + 2];        /* LoadRGB32's: 48 colours from pen 16 */
static UBYTE *full_rgb, *full_chunky;   /* the screen's size */

static void make_picture(UBYTE *rgb, int w, int h)
{
    int x, y;
    ULONG seed = 12345;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            UBYTE *p = rgb + (y * w + x) * 3;
            seed = seed * 1103515245UL + 12345;
            p[0] = (UBYTE)((x * 255 / w + (seed >> 28)) & 0xFF);
            p[1] = (UBYTE)((y * 255 / h + ((seed >> 24) & 15)) & 0xFF);
            p[2] = (UBYTE)(((x + y) * 128 / (w + h) + 64 + ((seed >> 20) & 15)) & 0xFF);
        }
}

/* AmigaMCE's colour reduction, in outline: count the 12-bit colours, keep
 * the most used ncol, map each pixel to its nearest (a cache per 12-bit
 * colour). Only an 8-bit screen needs it. */
static void reduce(const UBYTE *rgb, int n, UBYTE *chunky, ULONG *pal, int ncol, UBYTE first)
{
    static UWORD count[4096];
    static UBYTE map[4096];
    static UBYTE mapped[4096 / 8];
    UWORD top[64];
    int i, j, k;
    memset(count, 0, sizeof(count));
    memset(mapped, 0, sizeof(mapped));
    for (i = 0; i < n; i++) {
        const UBYTE *p = rgb + i * 3;
        UWORD c = ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4);
        if (count[c] < 65535)
            count[c]++;
    }
    for (k = 0; k < ncol; k++) {           /* the most used, one by one */
        UWORD bi = 0, bc = 0;
        for (j = 0; j < 4096; j++)
            if (count[j] > bc) {
                bc = count[j];
                bi = j;
            }
        top[k] = bi;
        count[bi] = 0;
        pal[k] = ((ULONG)((bi >> 8) * 17) << 16) | ((ULONG)(((bi >> 4) & 15) * 17) << 8) |
                 ((bi & 15) * 17);
    }
    for (i = 0; i < n; i++) {
        const UBYTE *p = rgb + i * 3;
        UWORD c = ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4);
        if (!(mapped[c >> 3] & (1 << (c & 7)))) {
            LONG bd = 0x7FFFFFFF;
            int r = c >> 8, g = (c >> 4) & 15, b = c & 15;
            for (k = 0; k < ncol; k++) {
                int dr = r - (top[k] >> 8), dg = g - ((top[k] >> 4) & 15), db = b - (top[k] & 15);
                LONG d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < bd) {
                    bd = d;
                    map[c] = (UBYTE)k;
                }
            }
            mapped[c >> 3] |= 1 << (c & 7);
        }
        chunky[i] = first + map[c];
    }
}

/* ---- the tests: each a function doing one run */
static ULONG arg;                       /* a counter for the runs */

static void t_clear(void)
{
    SetAPen(rp, (arg & 1) ? pen_bar : pen_bg);
    RectFill(rp, 0, 0, W - 1, H - 1);
}

static const char *words = "The quick brown fox jumps over the lazy dog 0123456789 Artists Albums";

static void t_text(void)
{
    int y, rows = H / (font->tf_YSize + 2);
    SetDrMd(rp, JAM2);
    SetBPen(rp, pen_bg);
    SetAPen(rp, pen_text);
    for (y = 0; y < rows; y++) {
        Move(rp, 8, y * (font->tf_YSize + 2) + font->tf_Baseline + 1);
        Text(rp, (STRPTR)words + (y + arg) % 20, 40);
    }
}

static void t_rows(void)                 /* a page of list rows, one highlighted */
{
    int rh = font->tf_YSize + 7, rows = (H - 60) / rh, y;
    SetDrMd(rp, JAM1);
    for (y = 0; y < rows; y++) {
        WORD ry = 40 + y * rh;
        SetAPen(rp, y == (int)(arg % rows) ? pen_hi : pen_bg);
        RectFill(rp, W / 6, ry, W - 20, ry + rh - 1);
        SetAPen(rp, pen_text);
        Move(rp, W / 6 + 8, ry + 3 + font->tf_Baseline);
        Text(rp, (STRPTR)words + (y + arg) % 25, 28);
        SetAPen(rp, pen_dim);
        Move(rp, W - 80, ry + 3 + font->tf_Baseline);
        Text(rp, (STRPTR)"12:34", 5);
    }
}

static void t_scroll(void)               /* a glide's step: the list moved a row */
{
    int rh = font->tf_YSize + 7;
    SetBPen(rp, pen_bg);
    ScrollRaster(rp, 0, (arg & 1) ? rh : -rh, W / 6, 40, W - 20, H - 30);
}

static struct BitMap *off;              /* a screen-sized bitmap of the screen's kind */

static void t_copy(void)                 /* double buffering's whole-screen copy */
{
    BltBitMapRastPort(off, 0, 0, rp, 0, 0, W, H, 0xC0);
    WaitBlit();
}

static void load_pal(const ULONG *pal)
{
    int k;
    load48[0] = (48UL << 16) | 16;
    for (k = 0; k < 48; k++) {
        load48[1 + k * 3] = ((pal[k] >> 16) & 0xFF) * 0x01010101UL;
        load48[2 + k * 3] = ((pal[k] >> 8) & 0xFF) * 0x01010101UL;
        load48[3 + k * 3] = (pal[k] & 0xFF) * 0x01010101UL;
    }
    load48[1 + 48 * 3] = 0;
    LoadRGB32(&scr->ViewPort, load48);
}

static void t_cover8(void)               /* 8-bit: colours reduced, loaded, written */
{
    reduce(cover_rgb, CW * CH, cover_chunky, cover_pal, 48, 16);
    load_pal(cover_pal);
    WriteChunkyPixels(rp, W - CW - 20, 40, W - 21, 40 + CH - 1, cover_chunky, CW);
}

static void t_cover8_write(void)         /* ...the writing alone */
{
    WriteChunkyPixels(rp, W - CW - 20, 40, W - 21, 40 + CH - 1, cover_chunky, CW);
}

static void t_cover_rgb(void)            /* deep: the RGB as it is */
{
    WritePixelArray(cover_rgb, 0, 0, CW * 3, rp, W - CW - 20, 40, CW, CH, RECTFMT_RGB);
}

static void t_cover_lut(void)            /* deep: a thumbnail's colours and indexes */
{
    WriteLUTPixelArray(cover_chunky, 0, 0, CW, rp, lut, W - CW - 20, 40, CW, CH, CTABFMT_XRGB8);
}

static void t_full8(void)
{
    reduce(full_rgb, W * H, full_chunky, full_pal, 48, 16);
    load_pal(full_pal);
    WriteChunkyPixels(rp, 0, 0, W - 1, H - 1, full_chunky, W);
}

static void t_full_rgb(void)
{
    WritePixelArray(full_rgb, 0, 0, W * 3, rp, 0, 0, W, H, RECTFMT_RGB);
}

static void t_spectrum(void)             /* Now Playing's 32 bars, a frame */
{
    int i, bw = (W - 40) / 32, base = H - 40, top = H / 2;
    SetAPen(rp, pen_bg);
    RectFill(rp, 20, top, 20 + 32 * bw - 1, base);
    for (i = 0; i < 32; i++) {
        int hgt = (int)(((i * 37 + arg * 13) % 97) * (base - top) / 97);
        SetAPen(rp, pen_bar);
        RectFill(rp, 20 + i * bw, base - hgt, 20 + i * bw + bw - 3, base);
    }
}

static void t_cpu(void)                  /* the CPU writing the card's memory itself */
{
    APTR base = NULL;
    ULONG bpr = 0, bpp = 0, y;
    APTR h = LockBitMapTags(rp->BitMap, LBMI_BASEADDRESS, (ULONG)&base,
                            LBMI_BYTESPERROW, (ULONG)&bpr, LBMI_BYTESPERPIX, (ULONG)&bpp, TAG_DONE);
    if (!h)
        return;
    for (y = 0; y < (ULONG)CH; y++) {
        ULONG *p = (ULONG *)((UBYTE *)base + (40 + y) * bpr + 20 * bpp), n = CW * bpp / 4, v = arg * 0x01010101UL;
        while (n--)
            *p++ = v;
    }
    UnLockBitMap(h);
}

/* ---- an animation's frame from its planes (6, 384 x 290): by the card
 * (BltBitMap from a planar bitmap, Picasso96 converting it through the
 * screen's colours), against AmigaMCE's own (animdraw.c, its 68020
 * assembler) into a bitmap of the card's, then blitted */
#define PW 384
#define PH 290
#define PD 6
static struct BitMap *planar;           /* a standard planar bitmap */
static UBYTE *planes;                   /* the same, rows interleaved (animdraw's) */
static struct BitMap *pbm;              /* the card's, the size of the frame */
static unsigned long plut[256];
static unsigned short pl16[256], pxo[PW + 1];
static BOOL p_ok;

static void p_setup(void)
{
    ULONG rb = ((PW + 15) >> 4) << 1, x, y, p, fmt = 0, bpr = 0;
    APTR base = NULL, h;
    p_ok = FALSE;
    if (!(planar = AllocBitMap(PW, PH, PD, BMF_CLEAR | BMF_DISPLAYABLE, NULL)) ||
        !(planes = AllocVec(rb * PD * PH, MEMF_ANY)) ||
        !(pbm = AllocBitMap(PW, PH, depth, BMF_CLEAR | BMF_MINPLANES, scr->RastPort.BitMap)))
        return;
    for (y = 0; y < PH; y++)                /* a busy picture: every pen, changing along */
        for (p = 0; p < PD; p++)
            for (x = 0; x < rb; x++) {
                UBYTE v = (UBYTE)((x * 37 + y * 11 + p * 73) ^ (y >> (p + 1)) ^ (x << p));
                planes[(y * PD + p) * rb + x] = v;
                if (x < (ULONG)planar->BytesPerRow)
                    planar->Planes[p][y * planar->BytesPerRow + x] = v;
            }
    for (x = 0; x < 64; x++) {
        ULONG r = (x * 4) & 255, g = (x * 37) & 255, b = (255 - x * 4) & 255;
        plut[x] = (r << 16) | (g << 8) | b;
        SetRGB32(&scr->ViewPort, x, r * 0x01010101UL, g * 0x01010101UL, b * 0x01010101UL);
    }
    for (x = 0; x <= PW; x++)
        pxo[x] = (unsigned short)x;
    h = LockBitMapTags(pbm, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr,
                       LBMI_PIXFMT, (ULONG)&fmt, TAG_DONE);
    if (h)
        UnLockBitMap(h);
    ad_init();
    if (!h || !ad_pix_tables(fmt))
        return;
    ad_lut16(plut, pl16);
    ad_ham(pl16, PD);
    p_ok = TRUE;
}

static void p_free(void)
{
    WaitBlit();
    if (planar)
        FreeBitMap(planar);
    if (pbm)
        FreeBitMap(pbm);
    FreeVec(planes);
    planar = pbm = NULL;
    planes = NULL;
}

static void t_p2d(void)                  /* the card's: planar to the screen */
{
    BltBitMapRastPort(planar, 0, 0, rp, 20, 40, PW, PH, 0xC0);
}

static void ours(int ham)
{
    ULONG rb = ((PW + 15) >> 4) << 1, bpr = 0, y;
    UBYTE *base = NULL;
    APTR h = LockBitMapTags(pbm, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr,
                            TAG_DONE);
    if (!h)
        return;
    for (y = 0; y < PH; y++)
        ad_row_put(planes + y * rb * PD, rb, PD, PW, 0, rb, 1, pxo, ham, plut, pl16, base + y * bpr);
    UnLockBitMap(h);
    BltBitMapRastPort(pbm, 0, 0, rp, 20, 40, PW, PH, 0xC0);
}

static void t_ours(void)                 /* AmigaMCE's: 64 colours */
{
    ours(0);
}

static void t_ours_ham(void)             /* AmigaMCE's: HAM6 (the card cannot) */
{
    ours(1);
}

static struct ScreenBuffer *sb[2];
static struct MsgPort *safe;
static BOOL safe_wait;

static void t_flip(void)                 /* a double-buffered flip, waited for */
{
    int k = (arg & 1);
    if (safe_wait) {
        while (!GetMsg(safe))
            Wait(1UL << safe->mp_SigBit);
        safe_wait = FALSE;
    }
    while (!ChangeScreenBuffer(scr, sb[k]))
        WaitTOF();
    safe_wait = TRUE;
}

typedef void (*TestFn)(void);
struct Test {
    const char *name;
    TestFn fn;
    int when;                           /* 0 every depth, 8 only 8-bit, 16 only deeper */
};
static const struct Test tests[] = {
    { "Clear the screen", t_clear, 0 },
    { "Text, a screenful", t_text, 0 },
    { "List page (rows + text)", t_rows, 0 },
    { "Glide step (ScrollRaster)", t_scroll, 0 },
    { "Whole-screen copy (blit)", t_copy, 0 },
    { "Spectrum frame (32 bars)", t_spectrum, 0 },
    { "Cover 256x256: reduce+write", t_cover8, 8 },
    { "Cover 256x256: write 8-bit", t_cover8_write, 8 },
    { "Cover 256x256: RGB direct", t_cover_rgb, 16 },
    { "Cover 256x256: LUT (thumb)", t_cover_lut, 16 },
    { "Full screen pic: reduce+write", t_full8, 8 },
    { "Full screen pic: RGB direct", t_full_rgb, 16 },
    { "CPU writes card RAM 256x256", t_cpu, 0 },
    { "Double-buffer flip", t_flip, 0 },
    { "Planes 384x290x6: card (P2D)", t_p2d, 16 },
    { "Planes 384x290x6: ours, 68k", t_ours, 16 },
    { "Planes 384x290 HAM6: ours", t_ours_ham, 16 },
};
#define NT ((int)(sizeof(tests) / sizeof(tests[0])))

static LONG time_test(TestFn fn)
{
    ULONG t0, t, runs = 0;
    arg = 0;
    fn();                               /* once untimed: caches, first-time work */
    WaitBlit();
    t0 = now_us();
    do {
        arg++;
        fn();
        runs++;
        t = now_us() - t0;
    } while (t < run_ms * 1000 || runs < 3);
    WaitBlit();
    t = now_us() - t0;
    return (LONG)(t / runs);
}

static BOOL run_depth(int slot, int d, WORD w, WORD h, char *modename)
{
    ULONG id = find_mode(w, h, d, modename, 64);
    char line[300];
    int i;
    if (id == INVALID_ID) {
        sprintf(line, "  %ld-bit: no RTG mode of %ld x %ld\n", (long)d, (long)w, (long)h);
        say(line);
        for (i = 0; i < NT; i++)
            result[slot][i] = -2;
        return FALSE;
    }
    scr = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, w, SA_Height, h, SA_Depth, d,
                         SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Type, CUSTOMSCREEN,
                         SA_Title, (ULONG)"rtgbench", TAG_DONE);
    if (!scr && d == 32)
        scr = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, w, SA_Height, h, SA_Depth, 24,
                             SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_DONE);
    if (!scr) {
        sprintf(line, "  %ld-bit: the screen did not open (%s)\n", (long)d, modename);
        say(line);
        for (i = 0; i < NT; i++)
            result[slot][i] = -2;
        return FALSE;
    }
    W = w;
    H = h;
    depth = GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH);
    rp = &scr->RastPort;
    colours();
    SetFont(rp, font);
    off = AllocBitMap(w, h, depth, BMF_CLEAR, scr->RastPort.BitMap);
    sb[0] = AllocScreenBuffer(scr, NULL, SB_SCREEN_BITMAP);
    sb[1] = AllocScreenBuffer(scr, NULL, SB_COPY_BITMAP);
    safe = CreateMsgPort();
    if (sb[0] && sb[1] && safe) {
        sb[0]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe;
        sb[1]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe;
    }
    safe_wait = FALSE;
    if (depth > 8 && CyberGfxBase)
        p_setup();
    sprintf(line, "  %ld-bit: %s (bitmap depth %ld)\n", (long)d, modename, (long)depth);
    say(line);
    for (i = 0; i < NT; i++) {
        BOOL deep = depth > 8;
        if ((tests[i].when == 8 && deep) || (tests[i].when == 16 && !deep) ||
            (tests[i].fn == t_copy && !off) || (tests[i].fn == t_flip && !(sb[0] && sb[1] && safe)) ||
            ((tests[i].fn == t_cpu || tests[i].fn == t_cover_rgb || tests[i].fn == t_cover_lut ||
              tests[i].fn == t_full_rgb) && !CyberGfxBase) ||
            ((tests[i].fn == t_cover8 || tests[i].fn == t_cover8_write || tests[i].fn == t_full8) &&
             GfxBase->LibNode.lib_Version < 40) ||
            ((tests[i].fn == t_full8 || tests[i].fn == t_full_rgb) && (!full_rgb || !full_chunky)) ||
            ((tests[i].fn == t_p2d || tests[i].fn == t_ours || tests[i].fn == t_ours_ham) && !p_ok)) {
            result[slot][i] = -1;
            continue;
        }
        if (tests[i].fn == t_flip || tests[i].fn == t_cpu) {
            SetAPen(rp, pen_bg);
            RectFill(rp, 0, 0, w - 1, h - 1);
        }
        result[slot][i] = time_test(tests[i].fn);
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            say("  stopped (Ctrl-C)\n");
            break;
        }
    }
    if (safe_wait && safe) {
        while (!GetMsg(safe))
            Wait(1UL << safe->mp_SigBit);
        safe_wait = FALSE;
    }
    if (sb[1]) {
        if (sb[0]) {                    /* the screen's own bitmap shown before closing */
            while (!ChangeScreenBuffer(scr, sb[0]))
                WaitTOF();
            if (safe) {
                while (!GetMsg(safe))
                    Wait(1UL << safe->mp_SigBit);
            }
        }
        FreeScreenBuffer(scr, sb[1]);
    }
    if (sb[0])
        FreeScreenBuffer(scr, sb[0]);
    sb[0] = sb[1] = NULL;
    if (safe)
        DeleteMsgPort(safe);
    safe = NULL;
    if (off) {
        WaitBlit();
        FreeBitMap(off);
        off = NULL;
    }
    p_free();
    CloseScreen(scr);
    scr = NULL;
    return TRUE;
}

static void fmt_time(char *s, LONG us)
{
    if (us == -1)
        strcpy(s, "-");
    else if (us == -2)
        strcpy(s, "n/a");
    else if (us >= 10000)
        sprintf(s, "%ld ms", (long)((us + 500) / 1000));
    else
        sprintf(s, "%ld.%02ld ms", (long)(us / 1000), (long)(us % 1000 / 10));
}

int main(void)
{
    static const char tmpl[] = "WIDTH/N,HEIGHT/N,DEEP/S,QUICK/S";
    LONG args[4] = { 0, 0, 0, 0 };
    struct RDArgs *ra = ReadArgs((CONST_STRPTR)tmpl, args, NULL);
    struct TextAttr ta;
    char line[320], names[3][64];
    WORD w = 640, h = 480;
    int nd, i, d;
    static const int depths[3] = { 8, 16, 32 };
    BPTR fh;

    (void)version;
    if (ra) {
        if (args[0])
            w = (WORD)*(LONG *)args[0];
        if (args[1])
            h = (WORD)*(LONG *)args[1];
        if (args[3])
            run_ms = 330;
    }
    nd = ra && args[2] ? 3 : 2;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&treq, 0)) {
        PutStr((CONST_STRPTR)"rtgbench: timer.device did not open\n");
        return 20;
    }
    TimerBase = treq.tr_node.io_Device;
    {
        struct EClockVal e;
        efreq = ReadEClock(&e);
    }
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 41);
    DiskfontBase = OpenLibrary((CONST_STRPTR)"diskfont.library", 36);
    /* AmigaMCE's font at its RTG size, from beside the program (the build
     * folder holds the AmigaMCE drawer), else topaz */
    ta.ta_Name = (STRPTR)"PROGDIR:AmigaMCE/Fonts/amce-pixel.font";
    ta.ta_YSize = h >= 512 ? 24 : 12;
    ta.ta_Style = 0;
    ta.ta_Flags = 0;
    font = DiskfontBase ? OpenDiskFont(&ta) : NULL;
    if (!font) {
        ta.ta_Name = (STRPTR)"PROGDIR:Fonts/amce-pixel.font";
        font = DiskfontBase ? OpenDiskFont(&ta) : NULL;
    }
    if (!font) {
        ta.ta_Name = (STRPTR)"topaz.font";
        ta.ta_YSize = 8;
        font = OpenFont(&ta);
    }
    cover_rgb = AllocVec(CW * CH * 3, MEMF_ANY);
    cover_chunky = AllocVec(CW * CH, MEMF_ANY);
    full_rgb = AllocVec((ULONG)w * h * 3, MEMF_ANY);
    full_chunky = AllocVec((ULONG)w * h, MEMF_ANY);
    if (!cover_rgb || !cover_chunky || !font) {
        PutStr((CONST_STRPTR)"rtgbench: not enough memory\n");
        goto out;
    }
    make_picture(cover_rgb, CW, CH);
    if (full_rgb)
        make_picture(full_rgb, w, h);
    reduce(cover_rgb, CW * CH, cover_chunky, cover_pal, 48, 16);
    for (i = 0; i < 48; i++)
        lut[16 + i] = cover_pal[i];

    say("AmigaMCE RTG benchmark 1.0\n");
    sprintf(line, "68%s0%s, Kickstart %ld, graphics %ld, cybergraphics %s, fast RAM %ld KB, chip %ld KB\n",
            SysBase->AttnFlags & (1 << 7) ? "06" : SysBase->AttnFlags & AFF_68040 ? "04" :
            SysBase->AttnFlags & AFF_68030 ? "03" : SysBase->AttnFlags & AFF_68020 ? "02" : "00",
            SysBase->AttnFlags & AFF_68881 ? " + FPU" : "",
            (long)SysBase->LibNode.lib_Version, (long)GfxBase->LibNode.lib_Version,
            CyberGfxBase ? "yes" : "no",
            (long)(AvailMem(MEMF_FAST | MEMF_TOTAL) >> 10), (long)(AvailMem(MEMF_CHIP | MEMF_TOTAL) >> 10));
    say(line);
    sprintf(line, "Font %s %ld, screen %ld x %ld, about %ld ms a test\n", (char *)ta.ta_Name,
            (long)font->tf_YSize, (long)w, (long)h, (long)run_ms);
    say(line);
    for (i = 0; i < nd; i++)
        run_depth(i, depths[i], w, h, names[i]);

    say("\nTest                              ");
    for (d = 0; d < nd; d++) {
        sprintf(line, "%6ld-bit   ", (long)depths[d]);
        say(line);
    }
    say(" 16/8\n");
    for (i = 0; i < NT; i++) {
        char a[24];
        sprintf(line, "%-33s ", tests[i].name);
        say(line);
        for (d = 0; d < nd; d++) {
            fmt_time(a, result[d][i]);
            sprintf(line, "%10s   ", a);
            say(line);
        }
        if (result[0][i] > 0 && result[1][i] > 0) {
            LONG r = result[1][i] * 100 / result[0][i];
            sprintf(line, "%ld.%02ldx", (long)(r / 100), (long)(r % 100));
            say(line);
        }
        say("\n");
    }
    /* the two ways to show a picture, side by side */
    {
        int c8 = 6, f8 = 10, crgb = 8, frgb = 11;
        if (result[0][c8] > 0 && result[1][crgb] > 0) {
            sprintf(line, "\nA cover: 8-bit (reduced + written) %ld ms, 16-bit (RGB) %ld ms\n",
                    (long)(result[0][c8] / 1000), (long)(result[1][crgb] / 1000));
            say(line);
        }
        if (result[0][f8] > 0 && result[1][frgb] > 0) {
            sprintf(line, "A full-screen picture: 8-bit %ld ms, 16-bit %ld ms\n",
                    (long)(result[0][f8] / 1000), (long)(result[1][frgb] / 1000));
            say(line);
        }
        for (d = 1; d < nd; d++) {
            int pc = NT - 3, po = NT - 2, ph = NT - 1;
            if (result[d][pc] > 0 && result[d][po] > 0) {
                sprintf(line, "An animation's frame, 384 x 290 x 6, %ld-bit: the card's planar blit %ld.%01ld ms, "
                        "AmigaMCE's 68k %ld.%01ld ms (HAM6 %ld.%01ld ms)\n", (long)depths[d],
                        (long)(result[d][pc] / 1000), (long)(result[d][pc] % 1000 / 100),
                        (long)(result[d][po] / 1000), (long)(result[d][po] % 1000 / 100),
                        (long)(result[d][ph] / 1000), (long)(result[d][ph] % 1000 / 100));
                say(line);
            }
        }
    }
    if ((fh = Open((CONST_STRPTR)"PROGDIR:rtgbench.txt", MODE_NEWFILE))) {
        Write(fh, out, outn);
        Close(fh);
        PutStr((CONST_STRPTR)"\nWritten to rtgbench.txt\n");
    }
out:
    FreeVec(cover_rgb);
    FreeVec(cover_chunky);
    FreeVec(full_rgb);
    FreeVec(full_chunky);
    if (font)
        CloseFont(font);
    if (DiskfontBase)
        CloseLibrary(DiskfontBase);
    if (CyberGfxBase)
        CloseLibrary(CyberGfxBase);
    CloseDevice((struct IORequest *)&treq);
    if (ra)
        FreeArgs(ra);
    return 0;
}
