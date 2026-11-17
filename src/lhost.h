/*
 * AmigaMCE - the program's side of the layout engines (layout.h)
 * Copyright (c) 2026 Brian King
 */
#ifndef LHOST_H
#define LHOST_H

#include "layout.h"

struct LayoutInfo {
    char file[32];              /* "kodilite.layout" */
    char name[32];              /* "Kodi lite" */
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
BOOL lv_on(void);               /* a layout draws the screen now */
BOOL lv_key(struct IntuiMessage *im);    /* TRUE: the layout's (or the wheel's), done */
BOOL lv_mouse(struct IntuiMessage *im);  /* TRUE: taken (the classic hit tests would be wrong) */
void lv_hold(void);             /* the screen about to be taken over: classic drawing back */
void lv_release(void);          /* ...and the layout's memory let go (a game's launch) */
void lv_build_page(struct Page *p);  /* Settings > Display > Layout */
void lv_pick(int k);            /* its row k: 0 classic, then the layouts found */
const char *lv_label(void);     /* the one chosen, for the Display page */

#endif
