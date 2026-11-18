/*
 * Ambiance - the layout engines' interface
 * Copyright (c) 2026 Brian King
 *
 * A layout draws every screen of Ambiance in a style of its own, on a
 * graphics card's 15 to 32-bit screen: Home, a generic page (any list
 * with its details: Settings, Radio, a folder...), and the dedicated
 * screens it has (an album wall, an artist, an album, Now playing...).
 * Each is a library, PROGDIR:Layouts/<name>.layout, opened only while it
 * is in use; the classic list layout is Ambiance's own, built in.
 *
 * Only Ambiance's own layouts, built from the same tree with the same
 * compiler: the interface is internal and may change at any time. The
 * library's one function, GetLayout(abi), gives its table only if abi is
 * the program's LAYOUT_ABI (a checksum of this file, made by the
 * Makefile), so a layout built against another version of this file is
 * never used. Calls through the tables are plain C calls (arguments on
 * the stack), as the visualisations' are.
 *
 * Who does what: the program keeps every source, the page stack, the
 * selection, playback and launching; it builds a model of what a screen
 * shows (host->model) and does what it is asked (host->act). The layout
 * draws (through host's drawing calls, lkcore.h's), decides what Up or
 * Right mean on its screens, and asks the host to move, open or play.
 * Keys a layout leaves (LR_UNUSED) are the program's, as in the classic
 * layout.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include <exec/types.h>
#include "lkcore.h"

/* ---------------------------------------------------------------- screens */
enum {
    LS_HOME,            /* the sections, and the chosen one's rows */
    LS_PAGE,            /* the generic page: any list and its details */
    LS_ALBUMS,          /* a list of albums as covers (the album wall) */
    LS_ARTIST,          /* an artist: picture, facts, biography, albums */
    LS_ALBUM,           /* an album: cover, facts, review, tracks */
    LS_NOW,             /* Now playing */
    LS_COUNT
};
#define LS_BIT(s) (1UL << (s))

/* The sections, in Home's order (the program's MOD_* as these). */
enum { LSEC_GAMES, LSEC_DEMOS, LSEC_MUSIC, LSEC_MODS, LSEC_PICTURES, LSEC_VIDEOS, LSEC_RADIO,
       LSEC_SETTINGS, LSEC_OTHER };

/* ---------------------------------------------------------------- models */
/* A row of a list, made by the host. Its strings are the host's, good
 * until the layout's call returns. */
enum { LI_OTHER, LI_FOLDER, LI_ALBUM, LI_ARTIST, LI_TRACK, LI_MODULE, LI_STATION, LI_PICTURE,
       LI_VIDEO, LI_GAME, LI_ACTION, LI_SETTING,
       LI_DISC };                       /* a DVD in a drive: plays as a video, drawn as a disc */
enum { LM_NONE, LM_ARROW, LM_PLAY, LM_PLAYING, LM_CHECK_ON, LM_CHECK_OFF, LM_RADIO_ON, LM_RADIO_OFF,
       LM_CYCLE };
#define LIF_HEAD     1  /* a heading among the rows (not chosen) */
#define LIF_DISABLED 2  /* greyed out */
#define LIF_NOW      4  /* what is playing */

struct LItem {
    const char *label;
    const char *sub;            /* a second line (an album's artist and year), or NULL */
    const char *tag;            /* at the row's right (a length, a value), or NULL */
    const char *art;            /* its picture's file (a cover, a screenshot), or NULL */
    UBYTE kind;                 /* LI_* */
    UBYTE mark;                 /* LM_* */
    UBYTE flags;                /* LIF_* */
    UBYTE letter;               /* its letter as the list is sorted (The Dogs: D, where the
                                 * server sorts without "The"); 0 none */
};

/* A list: count rows, the selected one and the first shown (as the
 * host's page has them). item(i) makes row i; the pointer is good until
 * 32 more item() calls. */
struct LList {
    LONG count, sel, top;
    const struct LItem *(*item)(LONG i);
};

/* The generic page (LS_PAGE), and what every list screen also has. */
#define LCRUMBS 8
struct LPage {
    UBYTE section;              /* LSEC_* */
    UBYTE text;                 /* one long text (details only), not a list;
                                 * list.top: its first line shown */
    WORD ncrumb;
    const char *crumb[LCRUMBS]; /* the path, the top level first */
    struct LList list;
    WORD ndetails;
    const char *const *details; /* the selected row's details, as the classic pane's lines */
    const char *picture;        /* the selected row's picture's file, or NULL */
    const char *hint;           /* the keys' hint for the footer */
    const char *message;        /* a message to show now, or NULL */
    char letter;                /* the big letter up while the list glides or jumps (host->letter
                                 * draws it), 0 none */
    UBYTE text_off;             /* text: how much of its first line (list.top) is scrolled
                                 * past, in 256ths of a line (it glides by pixels) */
    ULONG text_frac;            /* text: how far through it, 0 (its start) to 65536 (its
                                 * end shown): a layout wrapping it its own way goes by this */
    const char *waiting;        /* its rows asked for over the network and not come yet:
                                 * what to say in their place ("Searching..."), or NULL */
    UBYTE wait_phase;           /* waiting: a spinner's step, 0-7 (changed(LC_WAIT) as
                                 * it turns) */
    /* text: under the text, a note (a paragraph, NULL none) over picture
     * (above: the page's own, as Credits' AGS logo), and at the bottom
     * Plex's "Works with" mark (host->plexmark) */
    const char *note;
    UBYTE plexmark;
    UBYTE frames;               /* a wall (LS_ALBUMS) of pictures and videos: its tiles
                                 * 4:3 frames, not square covers */
};

/* A row on Home: a title and a few items (recently added albums, the
 * sources...). */
struct LRow {
    const char *title;
    LONG n;
    const struct LItem *(*item)(LONG i);   /* row i (as LList's) */
};

struct LHome {
    WORD n, sel;                /* the sections, and the one chosen */
    const UBYTE *sections;      /* LSEC_* each */
    const char *const *names;   /* their names, translated */
    WORD nrows;
    const struct LRow *rows;    /* the chosen section's rows (may be none yet): its own
                                 * list, as tiles */
    LONG tile_sel;              /* the section's list in use (its first level): the tile
                                 * chosen; -1 the sections' menu in use */
    UBYTE tile_shown;           /* ...drawn as chosen: the keys in use (the pointer in use:
                                 * its hover alone shows) */
};

/* Pairs of label and value: facts[2 * i], facts[2 * i + 1]. */
struct LFacts {
    WORD n;
    const char *const *facts;
};

struct LAlbum {
    struct LPage page;          /* its tracks are page.list */
    const char *title, *artist, *line;  /* line: year, label, genres */
    const char *review;         /* album.nfo's, or NULL */
    const char *rating;         /* "8.5", or NULL */
    const char *art;            /* the cover's file, or NULL */
    struct LFacts facts;
};

struct LArtist {
    struct LPage page;          /* its albums are page.list */
    const char *name, *line;
    const char *bio;            /* artist.nfo's, or NULL */
    const char *art;            /* the artist's own picture, or NULL */
    const char *logo;           /* logo.png, or NULL */
    struct LFacts facts;
};

enum { LN_NONE, LN_TRACK, LN_MODULE, LN_STREAM };
struct LNow {
    UBYTE kind;                 /* LN_* */
    UBYTE paused;
    WORD nnext;
    const char *title, *artist, *album, *year;
    const char *format;         /* "FLAC, 44.1 kHz" */
    const char *source;         /* "Music files", a station's name... */
    const char *art;            /* the cover's file, or NULL */
    ULONG pos_ms, len_ms;       /* len 0: not known (a stream) */
    const char *const *next;    /* what plays next */
    WORD nlevels;
    const UBYTE *levels;        /* the spectrum, 0-255 a band, or NULL */
    UBYTE stopped;              /* there, not playing (stopped, or left so by the last run):
                                 * Play starts it */
    UBYTE volume;               /* 0-64 */
    WORD btn;                   /* the button chosen with the keys (LNB_*), -1 none shown */
    UWORD off;                  /* LNB_BIT()s greyed (nothing to do for what plays) */
    UWORD on;                   /* LNB_BIT()s lit as on: Loop, Mute */
};

/* Now playing's buttons, as the classic layout's (LAC_BUTTON presses one) */
enum { LNB_PREV, LNB_BACK, LNB_PLAY, LNB_STOP, LNB_FWD, LNB_NEXT, LNB_LOOP, LNB_MUTE, LNB_VDOWN,
       LNB_VUP, LNB_COUNT };
#define LNB_BIT(b) (1U << (b))

/* A picture, prepared by the host at the size asked for (0x00RRGGBB). */
struct LImage {
    UWORD w, h;
    ULONG mod;                  /* pixels a row */
    const ULONG *px;
};

/* ---------------------------------------------------------------- actions */
/* What the layout asks of the host (act's what). */
enum {
    LAC_SELECT,                 /* arg: the row to select on the screen's list */
    LAC_OPEN,                   /* arg: the row to open (Fire/Right in the classic layout) */
    LAC_PLAY,                   /* arg: the row to play (Return) */
    LAC_BACK,                   /* back a level */
    LAC_SECTION,                /* arg: Home's section to choose */
    LAC_ENTER,                  /* into the chosen section */
    LAC_PAUSE,                  /* playback: pause or play */
    LAC_NEXT, LAC_PREV,         /* playback: the next or previous track */
    LAC_NOWPLAYING,             /* Now playing */
    LAC_HOME,                   /* Home: every level closed, the sections' menu */
    LAC_BUTTON,                 /* arg: Now playing's button pressed (LNB_*) */
    LAC_SEEK,                   /* arg: to there in what plays, in 65536ths of its length */
    LAC_SCROLLTO                /* arg: the list (to that row) or text (that far) at that
                                 * part of its length, in 65536ths: a scroll bar dragged */
};

/* Input given to the layout (input's action): the program's keys, the
 * joystick and the mouse wheel, as the classic layout knows them. */
enum { LA_UP, LA_DOWN, LA_LEFT, LA_RIGHT, LA_FIRE, LA_RETURN, LA_BACK, LA_PAGEUP, LA_PAGEDOWN,
       LA_FIRST, LA_LAST, LA_LETTER /* arg: the letter */, LA_SPACE };
/* What input or pointer did with it. */
enum { LR_UNUSED, LR_DONE };
/* pointer()'s click: a move, the left button pressed, released (a drag ends) */
enum { LP_MOVE, LP_PRESS, LP_RELEASE };

/* What changed (changed's what): the screen is drawn again in part. */
#define LC_LIST     1           /* the list (rows came, a letter was read) */
#define LC_SELECT   2           /* the selection moved */
#define LC_DETAILS  4           /* the details came */
#define LC_IMAGE    8           /* a picture asked for is ready */
#define LC_PLAYING 16           /* the track or its time */
#define LC_MESSAGE 32           /* the message */
#define LC_WAIT    64           /* LPage.wait_phase: the spinner alone */

/* ---------------------------------------------------------------- the tables */
enum { LF_REGULAR, LF_BOLD, LF_SMALL, LF_TITLE, LF_TINY };  /* the smooth fonts' faces (LF_TITLE:
                                                   * larger, LF_TINY: fine print, a quarter
                                                   * under small; each its own height:
                                                   * host->font_h) */

struct LayoutHost {
    /* the screen */
    UWORD w, h;                 /* its size */
    UBYTE bits;                 /* 15, 16, 24 or 32 */
    UBYTE tier;                 /* the CPU's speed: 0 slow .. 3 fast (Plex's tiers) */
    /* drawing (lkcore.h's), colours 0xRRGGBB */
    void (*clip)(WORD x0, WORD y0, WORD x1, WORD y1);       /* x1, y1 one past; the screen: (0,0,w,h) */
    void (*fill)(WORD x, WORD y, WORD w, WORD h, ULONG rgb);
    void (*vgrad)(WORD x, WORD y, WORD w, WORD h, ULONG top, ULONG bottom);
    void (*hgrad)(WORD x, WORD y, WORD w, WORD h, ULONG left, ULONG right);
    void (*rrect)(WORD x, WORD y, WORD w, WORD h, WORD r, ULONG rgb, ULONG bg);
    void (*frame)(WORD x, WORD y, WORD w, WORD h, WORD t, ULONG rgb);
    void (*image)(const struct LImage *im, WORD x, WORD y);
    WORD (*text)(WORD x, WORD y, const char *s, LONG n, int face, ULONG fg, const struct LkBg *bg,
                 WORD maxw);
    WORD (*textlen)(const char *s, LONG n, int face);
    WORD (*fit)(const char *s, LONG n, int face, WORD maxw);    /* bytes that fit */
    WORD (*font_h)(int face);   /* a line's height */
    void (*icon)(int section, WORD x, WORD y, ULONG fg, const struct LkBg *bg);   /* a section's icon */
    WORD icon_px;
    /* pictures: prepared in the background, NULL until ready (then
     * changed(LC_IMAGE)); good until the layout's call returns */
    const struct LImage *(*picture)(const char *file, UWORD w, UWORD h);
    /* Ambiance's logo, at the screen's smooth size, over bg (0xRRGGBB); NULL
     * if there is none. Good until the layout closes. */
    const struct LImage *(*logo)(ULONG bg);
    /* the big letter that is up (LPage.letter), in fg, its right edge at
     * xr and its middle at ym: its width, 0 if none is up */
    WORD (*letter)(WORD xr, WORD ym, ULONG fg);
    /* the content */
    const void *(*model)(int screen);   /* struct LHome, LPage, LAlbum, LArtist or LNow */
    void (*act)(int what, LONG arg);    /* LAC_* */
    /* the rest */
    const char *(*tr)(const char *english);     /* the catalogue's _() */
    void (*log)(const char *what);
    ULONG (*now_ms)(void);
    const char *clock;          /* the clock's text, as the header shows it */
    /* Plex's logo ("Works with", Credits' LPage.plexmark) about two lines
     * tall, over bg; NULL if none */
    const struct LImage *(*plexmark)(ULONG bg);
    const char *debug;          /* Settings > Debug on: the CPU and memory line to show
                                 * at the top ("" off); as the clock, kept up to date */
    /* what is drawn at x, y (w x h) moved by dx, dy, inside the clip (a
     * glide: the rest drawn again, not all); FALSE if it cannot be */
    BOOL (*copy)(WORD x, WORD y, WORD w, WORD h, WORD dx, WORD dy);
};

struct LayoutEngine {
    ULONG abi;                  /* LAYOUT_ABI, as built */
    const char *name;           /* "Showcase" */
    UWORD min_bits, min_h;      /* below these the classic layout is used */
    ULONG screens;              /* LS_BIT()s of the screens it draws (the rest: LS_PAGE) */
    BOOL (*open)(const struct LayoutHost *host);
    void (*close)(void);
    void (*draw)(int screen);   /* all of it */
    void (*changed)(int screen, ULONG what);    /* LC_*: what to draw again */
    void (*tick)(ULONG ms);     /* about 25 times a second while shown */
    int (*input)(int screen, int action, LONG arg);  /* LR_* */
    int (*pointer)(int screen, WORD x, WORD y, BOOL click);   /* click: LP_* */
};

#endif
