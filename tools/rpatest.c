/*
 * rpatest - what an RTG driver gives back of what was written: known
 * colours written with WritePixelArray (ARGB), WriteRGBPixel and
 * FillPixelArray, then read back with ReadPixelArray (ARGB, RGBA, RGB),
 * ReadRGBPixel and straight from the bitmap's memory (LockBitMapTags),
 * on bitmaps like Workbench's of several sizes and in a window on it.
 * AmigaMCE's diagnostic for its blended text (ReadPixelArray's colours
 * coming back wrong on an emulated ZZ9000's 16-bit Workbench).
 *
 *   rpatest            writes to the console and to rpatest.log here (or,
 *                      where that cannot be written, T:rpatest.log)
 *
 * MIT, (c) 2026 Brian King. Build: m68k-amigaos-gcc -O2 -m68020 -noixemul
 *   -o rpatest tools/rpatest.c
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

struct Library *CyberGfxBase;
static BPTR logf;

#define N 8
static const ULONG colours[N] = {
    0x00F80000, /* red */
    0x0000FC00, /* green */
    0x000000F8, /* blue */
    0x00FFFFFF, /* white */
    0x00123456, /* mixed */
    0x000A1530, /* AmigaMCE's navy */
    0x00000000, /* black */
    0x00808080, /* grey */
};
static const char *const names[N] = { "red", "green", "blue", "white", "123456", "navy", "black", "grey" };

static void say(const char *fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    fputs(b, stdout);
    fflush(stdout);
    if (logf)
        FPuts(logf, (CONST_STRPTR)b);
}

static const char *fmtname(ULONG f)
{
    static const char *const n[] = { "LUT8", "RGB15", "BGR15", "RGB15PC", "BGR15PC", "RGB16", "BGR16",
                                     "RGB16PC", "BGR16PC", "RGB24", "BGR24", "ARGB32", "BGRA32", "RGBA32" };
    return f < 14 ? n[f] : "?";
}

/* a raw pixel (bpp bytes at p) as RGB, by the format said */
static ULONG decode(const UBYTE *p, ULONG fmt)
{
    ULONG v, r, g, b;
    switch (fmt) {
    case PIXFMT_RGB16: case PIXFMT_BGR16: case PIXFMT_RGB15: case PIXFMT_BGR15:
        v = p[0] << 8 | p[1];
        break;
    case PIXFMT_RGB16PC: case PIXFMT_BGR16PC: case PIXFMT_RGB15PC: case PIXFMT_BGR15PC:
        v = p[1] << 8 | p[0];
        break;
    case PIXFMT_RGB24:  return (ULONG)p[0] << 16 | p[1] << 8 | p[2];
    case PIXFMT_BGR24:  return (ULONG)p[2] << 16 | p[1] << 8 | p[0];
    case PIXFMT_ARGB32: return (ULONG)p[1] << 16 | p[2] << 8 | p[3];
    case PIXFMT_BGRA32: return (ULONG)p[2] << 16 | p[1] << 8 | p[0];
    case PIXFMT_RGBA32: return (ULONG)p[0] << 16 | p[1] << 8 | p[2];
    default: return 0xFFFFFFFF;
    }
    if (fmt == PIXFMT_RGB16 || fmt == PIXFMT_RGB16PC || fmt == PIXFMT_BGR16 || fmt == PIXFMT_BGR16PC) {
        r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    } else {
        r = ((v >> 10) & 31) << 3, g = ((v >> 5) & 31) << 3, b = (v & 31) << 3;
    }
    if (fmt == PIXFMT_BGR16 || fmt == PIXFMT_BGR16PC || fmt == PIXFMT_BGR15 || fmt == PIXFMT_BGR15PC) {
        ULONG t = r;
        r = b;
        b = t;
    }
    return r << 16 | g << 8 | b;
}

/* close enough (a 16-bit pixel loses its low bits) */
static BOOL near(ULONG a, ULONG b)
{
    int s;
    for (s = 0; s <= 16; s += 8) {
        int x = (a >> s) & 255, y = (b >> s) & 255;
        if (x - y > 8 || y - x > 8)
            return FALSE;
    }
    return TRUE;
}

static void raw_row(struct BitMap *bm, WORD x, WORD y, int n, const char *what, const ULONG *want)
{
    APTR base = NULL, lk;
    ULONG bpr = 0, bpp = 0, fmt = 0, i, k;
    UBYTE raw[N * 4];
    if (!bm || !GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX)) {
        say("  raw (%s): not a CyberGraphX bitmap\n", what);
        return;
    }
    /* copied out while locked, said after: nothing that draws (the
     * console's text) while a bitmap is locked, or it waits for ever */
    memset(raw, 0, sizeof(raw));
    lk = LockBitMapTags(bm, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr,
                        LBMI_BYTESPERPIX, (ULONG)&bpp, LBMI_PIXFMT, (ULONG)&fmt, TAG_DONE);
    if (lk) {
        if (base && bpp >= 1 && bpp <= 4)
            for (i = 0; i < (ULONG)n; i++)
                for (k = 0; k < bpp; k++)
                    raw[i * 4 + k] = ((const UBYTE *)base)[(ULONG)y * bpr + (x + i) * bpp + k];
        UnLockBitMap(lk);
    }
    if (!lk) {
        say("  raw (%s): LockBitMapTags failed\n", what);
        return;
    }
    say("  raw (%s): base %08lx, %lu bytes a row, %lu a pixel, %s\n", what, (ULONG)base, bpr, bpp,
        fmtname(fmt));
    for (i = 0; i < (ULONG)n && base && bpp <= 4; i++) {
        const UBYTE *p = raw + i * 4;
        ULONG d = decode(p, fmt);
        say("    %-6s bytes", names[i]);
        for (k = 0; k < bpp; k++)
            say(" %02lx", (ULONG)p[k]);
        say("  = %06lx%s\n", d, want ? (near(d, want[i]) ? "  ok" : "  WRONG") : "");
    }
}

static void show(const char *what, const ULONG *got, const ULONG *want, BOOL rgb3, BOOL rgba)
{
    int i;
    say("  %s:\n", what);
    for (i = 0; i < N; i++) {
        ULONG v = got[i];
        if (rgba)
            v >>= 8;
        say("    %-6s %08lx%s\n", names[i], got[i], near(v & 0xFFFFFF, want[i]) ? "  ok" : "  WRONG");
    }
    (void)rgb3;
}

static void test_rp(struct RastPort *rp, struct BitMap *bm, WORD x, WORD y, const char *title)
{
    ULONG rd[N];
    UBYTE rd3[N * 3];
    int i;
    say("\n== %s (at %ld,%ld)\n", title, (LONG)x, (LONG)y);
    if (bm)
        say("  bitmap: %ld x %ld, depth %lu, CyberGraphX %s, format %s, %lu bytes a pixel, linear memory %s\n",
            (LONG)GetBitMapAttr(bm, BMA_WIDTH), (LONG)GetBitMapAttr(bm, BMA_HEIGHT),
            GetBitMapAttr(bm, BMA_DEPTH), GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX) ? "yes" : "no",
            fmtname(GetCyberMapAttr(bm, CYBRMATTR_PIXFMT)), GetCyberMapAttr(bm, CYBRMATTR_BPPIX),
            GetCyberMapAttr(bm, CYBRMATTR_ISLINEARMEM) ? "yes" : "no");

    /* 1: WritePixelArray ARGB, read back every way */
    WritePixelArray((APTR)colours, 0, 0, N * 4, rp, x, y, N, 1, RECTFMT_ARGB);
    WaitBlit();
    say(" written with WritePixelArray (ARGB):\n");
    raw_row(bm, x, y, N, "the bitmap's memory", colours);
    memset(rd, 0xEE, sizeof(rd));
    ReadPixelArray(rd, 0, 0, N * 4, rp, x, y, N, 1, RECTFMT_ARGB);
    show("ReadPixelArray ARGB", rd, colours, FALSE, FALSE);
    memset(rd, 0xEE, sizeof(rd));
    ReadPixelArray(rd, 0, 0, N * 4, rp, x, y, N, 1, RECTFMT_RGBA);
    show("ReadPixelArray RGBA (shown >> 8)", rd, colours, FALSE, TRUE);
    memset(rd3, 0xEE, sizeof(rd3));
    ReadPixelArray(rd3, 0, 0, N * 3, rp, x, y, N, 1, RECTFMT_RGB);
    say("  ReadPixelArray RGB:\n");
    for (i = 0; i < N; i++) {
        ULONG v = (ULONG)rd3[i * 3] << 16 | rd3[i * 3 + 1] << 8 | rd3[i * 3 + 2];
        say("    %-6s %06lx%s\n", names[i], v, near(v, colours[i]) ? "  ok" : "  WRONG");
    }
    say("  ReadRGBPixel:\n");
    for (i = 0; i < N; i++) {
        ULONG v = ReadRGBPixel(rp, x + i, y);
        say("    %-6s %08lx%s\n", names[i], v, near(v & 0xFFFFFF, colours[i]) ? "  ok" : "  WRONG");
    }

    /* 2: WriteRGBPixel one at a time, then the memory and ReadPixelArray */
    for (i = 0; i < N; i++)
        WriteRGBPixel(rp, x + i, y + 1, colours[N - 1 - i]);
    WaitBlit();
    {
        ULONG rev[N];
        for (i = 0; i < N; i++)
            rev[i] = colours[N - 1 - i];
        say(" written with WriteRGBPixel (reversed order) on the next row:\n");
        raw_row(bm, x, y + 1, N, "the bitmap's memory", rev);
        memset(rd, 0xEE, sizeof(rd));
        ReadPixelArray(rd, 0, 0, N * 4, rp, x, y + 1, N, 1, RECTFMT_ARGB);
        say("  ReadPixelArray ARGB:\n");
        for (i = 0; i < N; i++)
            say("    %-6s %08lx%s\n", names[N - 1 - i], rd[i], near(rd[i] & 0xFFFFFF, rev[i]) ? "  ok" : "  WRONG");
    }

    /* 3: FillPixelArray navy */
    FillPixelArray(rp, x, y + 2, N, 1, 0x000A1530);
    WaitBlit();
    say(" FillPixelArray navy (0a1530) on the third row:\n");
    raw_row(bm, x, y + 2, 1, "the bitmap's memory", &colours[5]);
    memset(rd, 0xEE, sizeof(rd));
    ReadPixelArray(rd, 0, 0, N * 4, rp, x, y + 2, 1, 1, RECTFMT_ARGB);
    say("  ReadPixelArray ARGB: %08lx%s\n", rd[0], near(rd[0] & 0xFFFFFF, 0x0A1530) ? "  ok" : "  WRONG");
}

/* ReadPixelArray (ARGB) at each width 1-24, at x 0 to 3: a row of known
 * colours written, read back, each pixel checked; the first wrong said */
static void width_sweep(struct RastPort *rp, const char *title)
{
    static ULONG row[32], rd[40];
    int w, x0, i;
    say("\n== ReadPixelArray ARGB by width, %s (ok, or the first pixel wrong)\n", title);
    for (i = 0; i < 32; i++)
        row[i] = colours[i % N] ^ (i << 2);
    for (x0 = 0; x0 < 4; x0++) {
        say("  x %ld:", (LONG)x0);
        for (w = 1; w <= 24; w++) {
            int bad = -1;
            WritePixelArray(row, 0, 0, 32 * 4, rp, x0, 20, 32, 1, RECTFMT_ARGB);
            WaitBlit();
            for (i = 0; i < 40; i++)
                rd[i] = 0xEEEEEEEE;
            ReadPixelArray(rd, 0, 0, w * 4, rp, x0, 20, w, 1, RECTFMT_ARGB);
            WaitBlit();
            for (i = 0; i < w && bad < 0; i++)
                if (!near(rd[i] & 0xFFFFFF, row[i] & 0xFFFFFF))
                    bad = i;
            if (bad < 0 && rd[w] != 0xEEEEEEEE)
                bad = 100;                  /* written past the end */
            if (bad < 0)
                say(" %ld", (LONG)w);
            else if (bad == 100)
                say(" %ld:past(%08lx)", (LONG)w, rd[w]);
            else
                say(" %ld:[%ld]%08lx", (LONG)w, (LONG)bad, rd[bad]);
        }
        say("\n");
    }
    /* and two rows at once (the modulo): 5 wide, 2 high */
    {
        int bad = -1;
        WritePixelArray(row, 0, 0, 32 * 4, rp, 0, 20, 5, 1, RECTFMT_ARGB);
        WritePixelArray(row + 5, 0, 0, 32 * 4, rp, 0, 21, 5, 1, RECTFMT_ARGB);
        WaitBlit();
        for (i = 0; i < 40; i++)
            rd[i] = 0xEEEEEEEE;
        ReadPixelArray(rd, 0, 0, 5 * 4, rp, 0, 20, 5, 2, RECTFMT_ARGB);
        WaitBlit();
        for (i = 0; i < 10 && bad < 0; i++)
            if (!near(rd[i] & 0xFFFFFF, row[i] & 0xFFFFFF))
                bad = i;
        say("  5 x 2: %s", bad < 0 ? "ok" : "WRONG");
        if (bad >= 0)
            say(" (pixel %ld: %08lx, wanted %06lx)", (LONG)bad, rd[bad], row[bad] & 0xFFFFFF);
        say("\n");
    }
}

static void test_bitmap(struct Screen *scr, UWORD w, UWORD h, WORD x, WORD y)
{
    struct BitMap *bm = AllocBitMap(w, h, GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH), BMF_MINPLANES | BMF_CLEAR,
                                    scr->RastPort.BitMap);
    struct RastPort rp;
    char t[80];
    if (!bm) {
        say("\n== a %u x %u bitmap like Workbench's: could not be allocated\n", w, h);
        return;
    }
    InitRastPort(&rp);
    rp.BitMap = bm;
    sprintf(t, "a %u x %u bitmap like Workbench's (friend)", w, h);
    test_rp(&rp, bm, x, y, t);
    WaitBlit();
    FreeBitMap(bm);
}

int main(void)
{
    struct Screen *scr;
    struct Window *win;
    ULONG id;
    static const char *logname = "rpatest.log";
    if (!(logf = Open((CONST_STRPTR)logname, MODE_NEWFILE)))
        logf = Open((CONST_STRPTR)(logname = "T:rpatest.log"), MODE_NEWFILE);
    if (!(CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40))) {
        say("rpatest: no cybergraphics.library 40\n");
        goto out;
    }
    say("rpatest: what this RTG driver gives back of what was written\n");
    say("cybergraphics.library %ld.%ld, graphics.library %ld.%ld\n", (LONG)CyberGfxBase->lib_Version,
        (LONG)CyberGfxBase->lib_Revision, (LONG)GfxBase->LibNode.lib_Version, (LONG)GfxBase->LibNode.lib_Revision);
    {
        struct Library *rtg = OpenLibrary((CONST_STRPTR)"rtg.library", 0);
        if (rtg) {
            say("rtg.library %ld.%ld (%s)\n", (LONG)rtg->lib_Version, (LONG)rtg->lib_Revision,
                rtg->lib_IdString ? (char *)rtg->lib_IdString : "");
            CloseLibrary(rtg);
        }
    }
    if (!(scr = LockPubScreen(NULL))) {
        say("no default public screen\n");
        goto out;
    }
    id = GetVPModeID(&scr->ViewPort);
    say("Workbench: %ld x %ld, mode %08lx, CyberGraphX mode %s, depth %ld\n", (LONG)scr->Width,
        (LONG)scr->Height, id, IsCyberModeID(id) ? "yes" : "no",
        (LONG)GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH));
    if (IsCyberModeID(id))
        say("  its mode: %ld bits, format %s\n", (LONG)GetCyberIDAttr(CYBRIDATTR_DEPTH, id),
            fmtname(GetCyberIDAttr(CYBRIDATTR_PIXFMT, id)));

    {
        /* AmigaMCE's own test, as it does it: a 4 x 1 friend, red and blue */
        struct BitMap *bm = AllocBitMap(4, 1, GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH), BMF_MINPLANES,
                                        scr->RastPort.BitMap);
        if (bm) {
            struct RastPort trp;
            ULONG wr[2] = { 0x00F80000UL, 0x000000F8UL }, rd[2] = { 0, 0 };
            InitRastPort(&trp);
            trp.BitMap = bm;
            WritePixelArray(wr, 0, 0, 8, &trp, 0, 0, 2, 1, RECTFMT_ARGB);
            ReadPixelArray(rd, 0, 0, 8, &trp, 0, 0, 2, 1, RECTFMT_ARGB);
            WaitBlit();
            say("\n== AmigaMCE's own test (a 4 x 1 friend): wrote 00f80000 000000f8, read %08lx %08lx\n", rd[0],
                rd[1]);
            say("  bitmap: %ld x %ld, CyberGraphX %s, format %s\n", (LONG)GetBitMapAttr(bm, BMA_WIDTH),
                (LONG)GetBitMapAttr(bm, BMA_HEIGHT), GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX) ? "yes" : "no",
                fmtname(GetCyberMapAttr(bm, CYBRMATTR_PIXFMT)));
            raw_row(bm, 0, 0, 2, "the bitmap's memory", NULL);
            FreeBitMap(bm);
        }
    }
    test_bitmap(scr, 8, 3, 0, 0);       /* tiny */
    test_bitmap(scr, 64, 16, 8, 4);
    test_bitmap(scr, 640, 32, 100, 10);
    {
        struct BitMap *bm = AllocBitMap(64, 32, GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH),
                                        BMF_MINPLANES | BMF_CLEAR, scr->RastPort.BitMap);
        if (bm) {
            struct RastPort trp;
            InitRastPort(&trp);
            trp.BitMap = bm;
            width_sweep(&trp, "a 64 x 32 bitmap like Workbench's");
            WaitBlit();
            FreeBitMap(bm);
        }
    }

    win = OpenWindowTags(NULL, WA_PubScreen, (ULONG)scr, WA_Left, 40, WA_Top, 40, WA_InnerWidth, 200,
                         WA_InnerHeight, 40, WA_Title, (ULONG)"rpatest", WA_DragBar, TRUE,
                         WA_Activate, FALSE, WA_GimmeZeroZero, TRUE, TAG_DONE);
    if (win) {
        test_rp(win->RPort, NULL, 10, 10, "a window on Workbench (its RastPort, layered)");
        say("  (the window's pixels in the screen's memory, at its place:)\n");
        raw_row(scr->RastPort.BitMap, win->LeftEdge + win->BorderLeft + 10, win->TopEdge + win->BorderTop + 10, N,
                "Workbench's bitmap", colours);
        width_sweep(win->RPort, "a window on Workbench");
        Delay(50);
        CloseWindow(win);
    } else {
        say("\n(no window could be opened)\n");
    }
    UnlockPubScreen(NULL, scr);
    say("\ndone: %s\n", logf ? logname : "(no log could be written)");
out:
    if (CyberGfxBase)
        CloseLibrary(CyberGfxBase);
    if (logf)
        Close(logf);
    return 0;
}
