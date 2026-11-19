/*
 * Ambiance - the program's side of the layout engines (layout.h)
 * Copyright (c) 2026 Brian King
 */
#ifndef LHOST_H
#define LHOST_H

#include "layout.h"

struct LayoutInfo {
    char file[32];              /* "showcase.layout" */
    char name[32];              /* "Showcase" */
    UWORD min_bits, min_h;
};

/* The layouts in PROGDIR:Layouts that this program can use (each opened,
 * its table asked for with this build's LAYOUT_ABI, closed again): up to
 * max of them into out; how many. */
int layout_list(struct LayoutInfo *out, int max);

/* PROGDIR:Layouts/<file> opened and its table taken (NULL: not there,
 * or built against another layout.h; the log says which). Kept open till
 * layout_unload. */
const struct LayoutEngine *layout_load(const char *file);
void layout_unload(void);                       /* closed, out of memory */
const struct LayoutEngine *layout_loaded(void); /* the one open, or NULL */

/* ---- main.c's side: the layout as a view over the classic program.
 * While a layout is shown, gfx_off is set: the classic drawing is done
 * nowhere, only counted (gfx_off_hits), and the layout draws the screen
 * again from its models (lv_frame, once a pass of the main loop, before
 * the screen is shown). Everything else (pages, selection, keys,
 * playback) stays the program's. */
struct Page;
struct IntuiMessage;
void lv_frame(void);            /* chosen, possible, shown: drawn if anything changed */
void lv_timing(ULONG *ms, ULONG *whole, ULONG *parts);   /* its time since last asked (Debug) */
BOOL lv_on(void);               /* a layout draws the screen now */
BOOL lv_key(struct IntuiMessage *im);    /* TRUE: the layout's (or the wheel's), done */
BOOL lv_mouse(struct IntuiMessage *im);  /* TRUE: taken (the classic hit tests would be wrong) */
void lv_hold(void);             /* the screen about to be taken over: classic drawing back */
void lv_release(void);          /* ...and the layout's memory let go (a game's launch) */
extern BOOL layout_stale;       /* a layout's older copy is still in memory, open (a restart helps) */
void lv_resume(void);           /* back from the viewer or a video: on again before drawing */
void lv_build_page(struct Page *p);  /* Settings > Display > Layout */
void lv_pick(int k);            /* its row k: 0 classic, then the layouts found */
const char *lv_label(void);     /* the one chosen, for the Display page */

/* A slideshow's and a video's controls, drawn by the layout chosen in its
 * own style (LControls) on a graphics card's screen of 15 to 32 bits, or
 * an AGA one of 256 colours (in the registers kept for them): the
 * screen as it is now (a window, full screen). lv_controls_ok: it can (the
 * layout opened for them, if a viewer or video closed it); _h: their
 * height; _draw: into bm (the screen's width x c->h, a friend of its
 * bitmap) standing for the screen's rows from c->y, what 0 all or
 * LC_PLAYING the time (FALSE: not drawn); _end: done with them. */
struct BitMap;
BOOL lv_controls_ok(void);
WORD lv_controls_h(BOOL timed);
BOOL lv_controls_draw(struct LControls *c, ULONG what, struct BitMap *bm);
void lv_controls_end(void);
/* On an AGA screen with a layout chosen, the controls are drawn in
 * VW_BAR_PENS colours of their own at registers VW_BAR_PEN0.. (over the
 * sprites' 16-31, so the pointer keeps its colours), which the copper
 * makes theirs from the bar's top line down (copper.c): the picture keeps
 * every register. lv_controls_keep: they can be; lv_controls_rgb: the
 * colours the last full draw chose. */
#define VW_BAR_PENS 64
#define VW_BAR_PEN0 32
BOOL lv_controls_keep(void);
const ULONG *lv_controls_rgb(void);
const WORD *lv_controls_first(void);    /* ...each one's first row in them (-1: not used) */

#endif
