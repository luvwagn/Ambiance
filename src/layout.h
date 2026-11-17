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
       LI_VIDEO, LI_GAME, LI_ACTION, LI_SETTING };
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
    UBYTE pad;
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
};

/* A row on Home: a title and a few items (recently added albums, the
 * sources...). */
struct LRow {
    const char *title;
    WORD n;
    const struct LItem *items;
};

struct LHome {
    WORD n, sel;                /* the sections, and the one chosen */
    const UBYTE *sections;      /* LSEC_* each */
    const char *const *names;   /* their names, translated */
    WORD nrows;
    const struct LRow *rows;    /* the chosen section's rows (may be none yet) */
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
};

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
    LAC_NOWPLAYING              /* Now playing */
};

/* Input given to the layout (input's action): the program's keys, the
 * joystick and the mouse wheel, as the classic layout knows them. */
enum { LA_UP, LA_DOWN, LA_LEFT, LA_RIGHT, LA_FIRE, LA_RETURN, LA_BACK, LA_PAGEUP, LA_PAGEDOWN,
       LA_FIRST, LA_LAST, LA_LETTER /* arg: the letter */, LA_SPACE };
/* What input or pointer did with it. */
enum { LR_UNUSED, LR_DONE };

/* What changed (changed's what): the screen is drawn again in part. */
#define LC_LIST     1           /* the list (rows came, a letter was read) */
#define LC_SELECT   2           /* the selection moved */
#define LC_DETAILS  4           /* the details came */
#define LC_IMAGE    8           /* a picture asked for is ready */
#define LC_PLAYING 16           /* the track or its time */
#define LC_MESSAGE 32           /* the message */

/* ---------------------------------------------------------------- the tables */
enum { LF_REGULAR, LF_BOLD, LF_SMALL };  /* the smooth fonts' faces */

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
    /* the content */
    const void *(*model)(int screen);   /* struct LHome, LPage, LAlbum, LArtist or LNow */
    void (*act)(int what, LONG arg);    /* LAC_* */
    /* the rest */
    const char *(*tr)(const char *english);     /* the catalogue's _() */
    void (*log)(const char *what);
    ULONG (*now_ms)(void);
    const char *clock;          /* the clock's text, as the header shows it */
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
    int (*pointer)(int screen, WORD x, WORD y, BOOL click);
};

#endif
