/*
 * Ambiance - the program's state that more than main.c reads: its pages,
 * and the few questions about where the program is that the services
 * (details.c) ask. Nothing here draws.
 * Copyright (c) 2026 Brian King
 */
#ifndef APP_H
#define APP_H

#include "amb.h"

enum { PT_LIST, PT_TEXT, PT_DETAIL, PT_PLAYER };

struct Chunks;

struct Page {
    UBYTE type;
    char title[64];
    struct ItemList list;
    LONG sel, top;
    char base[256];
    WORD facts_y;
    struct TextBox tb;          /* PT_TEXT and PT_DETAIL */
    UBYTE aside;                /* letter column: 0 not worked out yet, 1 none, 2 yes */
    UBYTE style_pv;             /* Settings > Style on a laced screen: the aside shows
                                 * the Home menu in the selected font */
    UBYTE pics;                 /* a Pictures folder (pics.c): IT_DIR and IT_PIC rows */
    UBYTE mods;                 /* a Mods folder (mods.c): IT_DIR and IT_MOD rows */
    UBYTE radio;                /* a Radio list (radio.c): IT_NET and IT_STATION rows; */
    UBYTE plex;                 /* a Plex list (plex.c; radio too when asked over the network) */
    UBYTE plexlink;             /* PT_TEXT: signing in at plex.tv (its code, then the outcome) */
    char plex_label[96];        /* a Plex album's page: the album (its details pane) */
    UBYTE setp;                 /* a Settings page: SETP_* */
    UBYTE songs;                /* Music's own files: IT_DIR and IT_SONG rows (mods.c) */
    UBYTE ags;                  /* a Games or Demos list (AGS's menus) */
    UBYTE igame;                /* iGame's list (igame.c): 1, 2 most played first */
    LONG plex_more, plex_total; /* a long Plex list: its parts read (count), its total */
    ULONG plex_parts[4];        /* which parts of PLEX_PART entries are read (a bit each) */
    BOOL plex_letters;          /* its first characters asked for (placeholders labelled) */
    LONG plex_n0;               /* entries of its first part shown (some may be left out) */
    UBYTE photos;               /* a Plex photo library's (Pictures): albums, IT_PLEXPHOTO */
    char plex_meta[720];
    ULONG net_seq;              /*   its request out (base: the query), 0 none */
    ULONG more_seq;             /*   a long list's part out (not waited for: it shows) */
    struct Chunks *ch;          /* a long list held a letter at a time (see Chunk) */
    UBYTE title_wide;           /* title drawn with room for a growing count */
    char pic[160];              /* PT_TEXT: a picture under the text box ("" none), */
    struct BitMap *pic_bm;      /*   made for the screen when the page is first drawn, */
    WORD pic_w, pic_h;          /*   as large as fits there, */
    WORD pic_x, pic_y;          /*   drawn there (pic_y < 0: not drawn) */
    const char *note;           /*   and a line or two above it (fixed, not scrolled) */
    UBYTE plexmark;             /* PT_TEXT: "Works with" Plex's logo at the bottom (Plex in Ambiance) */
};

/* main.c's answers */
struct Page *app_cur(void);             /* the page shown (NULL: Home's menu, nothing open) */
BOOL app_moving(void);                  /* the selection gliding (a key held) or the big letter up */
BOOL app_pane_pending(void);            /* the menu moved, its section not built yet */
ULONG app_dc_time(void);                /* a double-click's time, ms */
BOOL app_plex_art_kept(struct Page *p, LONG idx);  /* a Plex entry's picture fetched ahead */
BOOL app_plex_movable(struct Page *p, LONG idx);   /* ...its cover on screen, only moved */
void app_plex_ahead(struct Page *p);    /* at rest: Plex's neighbours' pictures asked for */

#endif
