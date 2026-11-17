/*
 * AmigaMCE - the layouts' drawing kit on a graphics card's screen
 * Copyright (c) 2026 Brian King
 *
 * lkcore.c's drawing sent to the screen AmigaMCE has open: fills by
 * FillPixelArray, blocks of pixels by WritePixelArray, both on the Gfx's
 * RastPort of the moment (a double buffer's while one is used), so the
 * layer clips them as it clips everything else. Only on 15 to 32-bit
 * screens of a graphics card: the layouts need nothing else (min_depth).
 */
#ifndef LKIT_H
#define LKIT_H

#include "lkcore.h"

struct Gfx;

/* An output for g's screen, its scratch allocated (the screen's width x
 * 16 rows). FALSE: not a 15-32 bit screen of a graphics card, or no
 * memory; nothing to free then. */
BOOL lk_open(struct Gfx *g, struct LkOut *o);
void lk_close(struct LkOut *o);

/* Text in the smooth fonts (face: 0 regular, 1 bold, 2 small), its
 * cell's top left at x, y, in fg over bg (opaque: each pixel worked out
 * from the colour under it, nothing read back), cut to maxw pixels
 * (0: no limit) at a whole character. Its width; 0 if nothing was drawn
 * (no smooth fonts on this screen). */
WORD lk_text(struct Gfx *g, struct LkOut *o, WORD x, WORD y, const char *s, LONG n, int face,
             ULONG fg, const struct LkBg *bg, WORD maxw);
/* How wide that would be. */
WORD lk_textlen(struct Gfx *g, const char *s, LONG n, int face);

#endif
