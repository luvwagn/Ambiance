/*
 * Ambiance - MPEG video's pictures as AGA HAM8 (ham8.c, ham8_68k.s)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * HAM8: each pixel a base colour (one of 64 registers) or the pixel
 * before it with one channel's top 6 bits changed. A chunky byte here is
 * as the planes take it: its control (2 bits: 0 a base colour, 1 blue,
 * 2 red, 3 green) above its data (6 bits): planes 6 and 7 the control,
 * as on an Intuition HAM8 screen (AGA's own document has them the lowest
 * two, wrongly).
 * No palette is chosen from the video: the base colours are fixed, and a
 * stream is shown as well as a file.
 */
#ifndef AMB_HAM8_H
#define AMB_HAM8_H

#ifndef AMB_HOST
#include <exec/types.h>
#else
#include <stdint.h>
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef int16_t WORD;
typedef int BOOL;
#endif

#define HAM8_BASES 64
#define HAM8_MAXW 1024                  /* the widest row (pixels) */

/* The row's tables, together (ham8_68k.s knows their offsets). A
 * channel is kept as its HAM8 byte (its control bits, red 2, green 3,
 * blue 1, << 6, and its 6 bits), so two of a channel differ by their 6
 * bits' difference d (-63 to 63): q[ch][d + 64] is d's step 0-15,
 * already in its place in dec's index (red's << 8, green's << 4);
 * dec, by those: 0 green, 1 red, 2 blue (the channel to set, the
 * likeliest first), 3 a base colour; then where a Cb, Cr pair's red,
 * green and blue are in lum (green: gu[Cb] + gv[Cr] bytes on). */
struct Ham8Step {
    UWORD q[3][128];
    UBYTE dec[4096];
    const UBYTE *rp[256], *bp[256], *gu[256];   /* by Cr, Cb, Cb */
    WORD gv[256];                               /* by Cr */
};

struct Ham8 {
    UBYTE base[HAM8_BASES][3];          /* the registers' colours, 6 bits a channel */
    UBYTE hbase[HAM8_BASES][3];         /* ...as HAM8 bytes a channel (see Ham8Step) */
    UBYTE near[4096];                   /* a 4:4:4 colour's nearest base register */
    UBYTE lum[3][1024];                 /* [Y + part + 512]: a channel as its HAM8 byte
                                         * (red, green, blue; a part: Cb, Cr's, in Y's
                                         * units) */
    struct Ham8Step s;
};

/* The tables (about 18 KB): the base colours a colour cube and a grey
 * ramp, registers 17-19 left alone (the mouse pointer's). */
void ham8_init(struct Ham8 *h);

/* Register r's colour as LoadRGB32 wants it (8 bits a channel). */
void ham8_rgb(const struct Ham8 *h, int r, ULONG *red, ULONG *green, ULONG *blue);

/* A row of a 4:2:0 picture (w pixels, even: Y, and Cb, Cr at half
 * width) as HAM8 bytes: one a pixel, or two (twice: a hires screen's,
 * the second a further step). The pixel before the row's first is
 * register 0 (black: the screen to its left). */
void ham8_row(struct Ham8 *h, const UBYTE *Y, const UBYTE *Cb, const UBYTE *Cr, ULONG w, UBYTE *out,
              BOOL twice);

#endif
