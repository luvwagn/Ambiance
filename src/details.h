/*
 * Ambiance - the selected entry's details (details.c): its text (a game's
 * fact sheet, a file's details) and its picture, read by the loader once
 * the selection rests; the entries next to it read ahead. No drawing: the
 * view that is driven (Classic's pane, or a layout) is told what came
 * through a DetView, and asks for pictures at its own size.
 * Copyright (c) 2026 Brian King
 */
#ifndef DETAILS_H
#define DETAILS_H

#include "app.h"

/* What the driven view does with the details. Every member may be NULL
 * (nothing to do) but has_pane. */
struct DetView {
    BOOL (*has_pane)(struct Page *p);       /* p's entries show details */
    void (*start)(struct Page *p);          /* the entry's details about to be asked for (its
                                             * picture's place worked out: LJ_SHOT's size) */
    BOOL (*shots)(void);                    /* the view takes the service's pictures (else its own) */
    void (*job)(struct LJob *j);            /* a new job: the screen's settings (LJ_SHOT: its size) */
    BOOL (*keeps)(struct Page *p, LONG idx);    /* idx would show the picture on screen now */
    void (*clear_picture)(struct Page *p);  /* the picture on screen goes (the next entry's isn't it) */
    void (*clear_all)(struct Page *p);      /* the details shown go (the next aren't read yet) */
    void (*style)(struct Page *p);          /* Settings > Style's preview (no reading) */
    void (*begin)(struct Page *p, BOOL keep);   /* the entry's details asked for (det.tj, det.sj) */
    void (*plex)(struct Page *p, BOOL was_shown);  /* a Plex entry: the server's details */
    void (*empty)(struct Page *p);          /* the entry has none */
    void (*no_picture)(struct Page *p);     /* the entry has no picture after all (its place goes) */
    void (*text)(struct Page *p);           /* det.tj came */
    void (*picture)(struct Page *p);        /* det.sj came (after the text) */
    void (*tick)(struct Page *p);           /* begun: waiting (Classic's spinner) */
};

struct Det {
    struct Page *page;          /* what the details are of: page and entry */
    LONG sel;
    BOOL shown;                 /* FALSE once the details shown are cleared or left */
    BOOL pending;               /* asked for, waiting for the selection to rest (due) */
    ULONG due, since;           /* when to read; when the selection came to rest on it */
    struct LJob *tj, *sj;       /* its text and picture: shown or waited for */
    BOOL text_shown, pic_shown; /* ...given to the view */
    BOOL has_box;               /* it has a picture (a place for one) */
    BOOL clicked;               /* the selection made by a click (a double-click's time first) */
    LONG dir;                   /* the way the list last moved: read ahead that way */
    LONG ready, waited;         /* found read ahead, or waited for (log) */
};
extern struct Det det;

void det_view(const struct DetView *v);    /* the view driven now */

/* The selection moved (or the list was shown): its details once it rests */
void det_touch(struct Page *p);
/* The main loop's: begun when due; Plex's neighbours at rest */
void det_tick(void);
/* Begin now (preview_draw): asked for, given to the view as they come */
void det_begin(struct Page *p);
/* Jobs back from the loader (ldr_collect done): given to the view */
void det_arrived(struct Page *p);
BOOL det_due(void);                         /* one back, not yet given to the view */
void det_progress(struct Page *p);          /* what came given to the view; then read ahead */

/* The job for (type, mode, path): one out or back, else a new one (NULL:
 * every slot busy and ahead). first: waited for, so ahead of the queue. */
struct LJob *det_request(int type, UBYTE mode, const char *path, BOOL ahead, BOOL first);
void det_release(struct LJob *j);
void det_flush(void);                       /* every job back and freed (a screen closes) */
void det_let_go(BOOL orphan);               /* ...not waited for: those out orphaned (the viewer) */
BOOL det_roomy(void);                       /* memory to spare: read more ahead, keep more */
/* What entry idx's details are: its text job's type and path (0: none),
 * and how its picture is found (*shot SHOT_*, 0 none) */
int det_kind(struct Page *p, LONG idx, char *path, LONG max, UBYTE *shot);
/* A folder's or song's picture file, if a job back says (key "" if not) */
void det_first(UBYTE shot, const char *path, char *key);

#endif
