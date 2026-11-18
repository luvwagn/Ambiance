/*
 * Ambiance - the layouts' drawing kit on a graphics card's screen
 * Copyright (c) 2026 Brian King
 *
 * lkcore.c's drawing sent to the screen Ambiance has open: fills by
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
 * memory; nothing to free then.
 * canvas: draw into a bitmap of the screen's size in the card's memory
 * (a friend of the screen's) and copy what changed to the screen in one
 * blit at lk_show, so a frame drawn in layers is never seen half done.
 * If there is no memory for it, the drawing goes straight to the screen
 * (lk_canvas() says which). */
BOOL lk_open(struct Gfx *g, struct LkOut *o, BOOL canvas);
void lk_close(struct LkOut *o);
/* Everything drawn since the last lk_show, onto the screen (the Gfx's
 * RastPort of the moment). Nothing to do when drawing went straight to
 * the screen. */
void lk_show(struct LkOut *o);
/* An output onto bm, a w x h strip standing for the screen's rows y0 on
 * (a slideshow's or video's controls, drawn by a layout): not shown by
 * lk_show, bm left to the caller at lk_close */
/* An 8-bit screen (an experiment: AGA, or a card's 8-bit mode): the
 * layout's true colour dithered to a palette of its own (pens 20-35 and
 * 40-255, the screen's colours put back at lk_close), on a canvas shown
 * with WriteChunkyPixels (graphics.library 40) */
BOOL lk_open_chunky(struct Gfx *g, struct LkOut *o);
/* the layout's background gradient over the whole screen (an 8-bit
 * screen's: drawn with pens of its own, smooth; elsewhere nothing) */
void lk_ramp(struct LkOut *o, unsigned long top, unsigned long bottom);
/* a whole draw begins: an 8-bit screen's pens for flat colours free again */
void lk_fresh(struct LkOut *o);
BOOL lk_open_strip(struct Gfx *g, struct LkOut *o, struct BitMap *bm, WORD y0, WORD w, WORD h);
BOOL lk_canvas(const struct LkOut *o);

/* Text in the smooth fonts (face: 0 regular, 1 bold, 2 small), its
 * cell's top left at x, y, in fg over bg (opaque: each pixel worked out
 * from the colour under it, nothing read back), cut to maxw pixels
 * (0: no limit) at a whole character. Its width; 0 if nothing was drawn
 * (no smooth fonts on this screen). */
WORD lk_text(struct Gfx *g, struct LkOut *o, WORD x, WORD y, const char *s, LONG n, int face,
             ULONG fg, const struct LkBg *bg, WORD maxw);
/* An icon (ICON_*) of the smooth fonts, its top left at x, y; its size
 * (0: none drawn). */
WORD lk_icon(struct Gfx *g, struct LkOut *o, int icon, WORD x, WORD y, ULONG fg,
             const struct LkBg *bg);
/* The icons' size on this screen (0: none). */
WORD lk_icon_px(struct Gfx *g);
/* How wide that would be. */
WORD lk_textlen(struct Gfx *g, const char *s, LONG n, int face);
LONG lk_fit(struct Gfx *g, const char *s, LONG n, int face, WORD maxw);    /* the bytes that fit */

#endif
