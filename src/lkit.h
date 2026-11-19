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
/* Classic's own pens on any screen (its UI colours, the nearest of them for
 * any other): no canvas, drawn straight on the RastPort (classic.layout) */
BOOL lk_open_pens(struct Gfx *g, struct LkOut *o);
/* A strip drawn into memory (px: w x h, the caller's; row 0 the screen's
 * y0), then made n pens (64 at most) pen0.. into bm's rows from by: pick,
 * the n colours chosen from it into rgb[] (else rgb[] as before). A
 * layout's controls over a picture on an AGA screen, in registers the
 * copper makes theirs. */
BOOL lk_open_mem(struct Gfx *g, struct LkOut *o, ULONG *px, WORD y0, WORD w, WORD h);
BOOL lk_mem_pens(const ULONG *px, const ULONG *probe, WORD w, WORD h, struct BitMap *bm, WORD by, UBYTE pen0,
                 int n, ULONG *rgb, BOOL pick, WORD *first, ULONG *rowbg, WORD *domp);
/* (probe: the strip drawn as it may come to show - its buttons pressed -
 * its colours chosen too, and its pens' first rows; NULL none) */
/* (first[]: each pen's first row, -1 unused; rowbg[h]: each row's own colour
 * of the commonest pen, *domp which that is; any of them NULL: not wanted) */
/* the layout's background gradient over the whole screen (an 8-bit
 * screen's: drawn with pens of its own, smooth; elsewhere nothing) */
void lk_ramp(struct LkOut *o, unsigned long top, unsigned long bottom);
/* a whole draw begins: an 8-bit screen's pens for flat colours free again */
void lk_fresh(struct LkOut *o);
extern BOOL lk_copper_ok;            /* an AGA screen's background by the copper (default) */
extern BOOL lk_blit_ok;              /* an AGA screen's copies by the blitter (default) */
void lk_blits(ULONG *n, ULONG *kpx); /* copies the blitter made, K pixels moved (since last) */
void lk_pens(ULONG *short_of, ULONG *least_free);
void lk_prof(char *buf);
void lk_cover_cache_free(void);  /* text coverage kept (lk_text): let go (another font) */
BOOL lk_vgrad8(struct LkOut *o, long x, long y, long w, long h, unsigned long a, unsigned long b);
/* the big letter by hardware sprites (AGA, an 8-bit layout): TRUE if so */
extern BOOL lk_beam_ok;     /* (changes made after the display's beam: Ambiance/LayoutBeam=0 off) */
extern BOOL lk_sprites_ok;
extern BOOL lk_show_up;
extern BOOL lk_pic_own;
extern BOOL lk_skip_ok;          /* c2p leaves groups of 32 the planes show already (Ambiance/LayoutSkip=0: not) */          /* lk_picture: the host's own picture (pens of its own, however wide) */          /* the next lk_show from the bottom up (a list's selection went up) */
BOOL lk_letter_sprites(struct LkOut *o, const UBYTE *mask, UWORD w, UWORD h, UWORD bpr, UWORD cw, WORD x,
                       WORD y, ULONG rgb);
void lk_letter_sprites_off(struct LkOut *o);         /* the 8-bit drawing by kind: calls and ms since the last (text: its rows within) */
void lk_timing(ULONG *show_ms, ULONG *pic_ms, ULONG *kpx, ULONG *gcs);
/* a picture (as lk_image; an 8-bit screen's of few colours: pens of its own) */
void lk_picture(struct LkOut *o, long x, long y, const unsigned long *px, long w, long h, long mod);
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
