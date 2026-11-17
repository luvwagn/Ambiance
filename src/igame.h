/*
 * Ambiance - iGame's list of games and demos (igame.c): found, read, shown
 * under Games and Demos, and launched. Beside AGS, or in its place.
 */
#ifndef AMCE_IGAME_H
#define AMCE_IGAME_H

#include "amce.h"

#define IGF_FAV     1       /* a favourite in iGame */
#define IGF_DEMO    2       /* listed under Demos, not Games */
#define IGF_SLAVE   4       /* a WHDLoad slave (else a program run as it is) */

struct IgGame {
    char *title, *path;
    const char *genre, *chipset;    /* shared: one copy of each name */
    UWORD year, played;
    UBYTE players, flags;
};

struct IgCat {
    APTR pool;
    struct IgGame *g;
    LONG n, cap;
    const char **names;             /* genres and chipsets, one copy each */
    LONG nnames, capnames;
    LONG games, demos;              /* how many of each */
    char dir[256];                  /* iGame's drawer, "" if none read */
};

/* A list page of iGame's has a path that begins so: never a real path. */
#define IG_PREFIX "\001ig:"

/* Ambiance/iGame: iGame's drawer; "none" (or "n/a") for no list; unset
 * until found or chosen. */
#define IG_VAR "Ambiance/iGame"
/* Ambiance/iGameLooked: "1" once the hard disks were searched (the first
 * start): later starts look only at the assign and the usual drawers. */
#define IG_LOOKED_VAR "Ambiance/iGameLooked"

BOOL ig_folder_ok(const char *dir);         /* gameslist.csv in it */
BOOL ig_discover(char *out, LONG max, BOOL deep);   /* iGame looked for: its drawer;
                                                * deep: hard disks' tops too */
LONG ig_load(struct IgCat *c, const char *dir);    /* entries; -1 not readable */
void ig_free(struct IgCat *c);
LONG ig_count(const struct IgCat *c, BOOL demos);

BOOL ig_page(const char *path);             /* an iGame list page's path */
void ig_page_path(char *out, LONG max, BOOL demos, const char *query);
/* The page's rows: folders (IT_DIR, their paths ig_page_path's) or games
 * (IT_RUN, their paths the slave's or program's). *ordered: TRUE when the
 * rows are not in title order (most played first). -1: no list. */
LONG ig_read(const struct IgCat *c, const char *page, struct ItemList *out, BOOL *ordered);

const struct IgGame *ig_lookup(const struct IgCat *c, const char *path);
void ig_shot_base(const char *path, char *out, LONG max);     /* <its folder>/igame */
LONG ig_facts(const struct IgGame *gm, char lines[][96], LONG maxlines);
BOOL ig_write_launch_script(const struct IgGame *gm, const char *script, char *why, LONG wmax);

#endif
