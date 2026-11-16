/*
 * rpabug - ReadPixelArray(RECTFMT_ARGB) from a 16-bit bitmap, at widths 1
 * to 8, then RECTFMT_RAW at the same widths: each read is checked against
 * what WritePixelArray wrote (and the bitmap's own memory, which is right).
 *
 * Seen with rtg.library 43.787 (6.6.2026), cybergraphics.library 42.7, on
 * a 16-bit (RGB16) Workbench: widths that are a multiple of 4 read back
 * right; in any other, the last (width mod 4) pixels come back as 16-bit
 * words (each pixel's low half, GGBB) packed 2 bytes apart instead of 4
 * bytes each, so they and the bytes after are wrong.
 *
 * Run it from a Shell on a 15/16-bit Workbench. MIT, (c) 2026 Brian King.
 *   m68k-amigaos-gcc -O2 -noixemul -o rpabug rpabug.c
 */
#include <exec/types.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <stdio.h>

#ifndef RECTFMT_RAW
#define RECTFMT_RAW 5                   /* the bitmap's own pixels (CyberGraphX 4; not in the NDK) */
#endif

struct Library *CyberGfxBase;

int main(void)
{
    static const ULONG want[8] = { 0x00F80000, 0x0000FC00, 0x000000F8, 0x00F8FCF8,
                                   0x00103440, 0x00081430, 0x00808080, 0x00F8FC00 };   /* (exact in 16 bits) */
    struct Screen *scr;
    struct BitMap *bm;
    struct RastPort rp;
    ULONG got[12], w, i, bugs = 0;
    UWORD raw[8];
    APTR lk, base = NULL;
    ULONG bpr = 0;

    if (!(CyberGfxBase = OpenLibrary("cybergraphics.library", 40)))
        return 20;
    if (!(scr = LockPubScreen(NULL)))
        return 20;
    printf("Workbench: %lu-bit, pixel format %lu (5 = RGB16); cybergraphics.library %u.%u\n",
           GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH),
           GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_PIXFMT), CyberGfxBase->lib_Version,
           CyberGfxBase->lib_Revision);
    bm = AllocBitMap(16, 1, GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH), BMF_MINPLANES | BMF_CLEAR,
                     scr->RastPort.BitMap);
    UnlockPubScreen(NULL, scr);
    if (!bm)
        return 20;
    InitRastPort(&rp);
    rp.BitMap = bm;
    WritePixelArray((APTR)want, 0, 0, sizeof(want), &rp, 0, 0, 8, 1, RECTFMT_ARGB);
    WaitBlit();

    /* the bitmap's own memory (copied out while locked; said after) */
    if ((lk = LockBitMapTags(bm, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr, TAG_DONE))) {
        for (i = 0; i < 8; i++)
            raw[i] = base ? ((UWORD *)base)[i] : 0;
        UnLockBitMap(lk);
        printf("in memory after WritePixelArray:");
        for (i = 0; i < 8; i++)
            printf(" %04x", raw[i]);
        printf("\n");
    }

    for (w = 1; w <= 8; w++) {
        for (i = 0; i < 12; i++)
            got[i] = 0xEEEEEEEE;
        ReadPixelArray(got, 0, 0, w * 4, &rp, 0, 0, w, 1, RECTFMT_ARGB);
        WaitBlit();
        printf("width %lu:", w);
        for (i = 0; i < w; i++)
            printf(" %08lx", got[i]);
        for (i = 0; i < w && (got[i] & 0xFFFFFF) == want[i]; i++)
            ;
        if (i < w || got[w] != 0xEEEEEEEE) {
            printf("   WRONG (pixel %lu)", i);
            bugs++;
        } else {
            printf("   ok");
        }
        printf("\n");
    }
    /* the same widths in RECTFMT_RAW: the bitmap's own 16-bit pixels, as
     * in memory (no conversion) */
    for (w = 1; w <= 8; w++) {
        UWORD r16[16];
        ULONG n;
        for (i = 0; i < 16; i++)
            r16[i] = 0xEEEE;
        n = ReadPixelArray(r16, 0, 0, w * 2, &rp, 0, 0, w, 1, RECTFMT_RAW);
        WaitBlit();
        printf("RAW width %lu (said %lu):", w, n);
        for (i = 0; i < w; i++)
            printf(" %04x", r16[i]);
        for (i = 0; i < w && r16[i] == raw[i]; i++)
            ;
        if (i < w || r16[w] != 0xEEEE) {
            printf("   WRONG (pixel %lu)", i);
            bugs++;
        } else {
            printf("   ok");
        }
        printf("\n");
    }
    printf("%s\n", bugs ? "ReadPixelArray gave back other than what is in memory." : "All read back right.");
    WaitBlit();
    FreeBitMap(bm);
    CloseLibrary(CyberGfxBase);
    return bugs ? 5 : 0;
}
