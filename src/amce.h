/*
 * AmigaMCE - full-screen media launcher for AmigaOS 3.1+
 * Copyright (c) 2026 Brian King
 */
#ifndef AMCE_H
#define AMCE_H

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/text.h>
#include <graphics/rastport.h>

/* The Makefile passes these from git describe and the build date. */
#ifndef AMCE_VERSION
#define AMCE_VERSION "0.1.0"
#endif
#ifndef AMCE_DATE
#define AMCE_DATE    "2.11.2026"
#endif

/* ---------------------------------------------------------------- caps */
struct Caps {
    UWORD cpu;          /* 0, 10, 20, 30, 40, 60 */
    UWORD kick, kick_rev; /* ROM: exec version, SysBase->SoftVer */
    UWORD wb, wb_rev;     /* version.library on disk, 0 if absent */
    BOOL  aga, ecs, pal, ehb, rtg, net, mp3_mhi, mp3_mpega;
    BOOL  emulator;     /* UAE family: Amiberry, WinUAE, FS-UAE */
    ULONG chipfree, fastfree;
};
void caps_detect(struct Caps *c);
const char *caps_release_name(UWORD version);

/* ---------------------------------------------------------------- pens */
enum {
    PEN_BG = 0, PEN_BAR, PEN_RULE, PEN_TEXT, PEN_DIM, PEN_WHITE, PEN_HL,
    PEN_SELDIM, PEN_ACCENT, PEN_KEYCAP, PEN_FAINT,
    PEN_LOGO0 = 16,      /* 16..23 extra logo shades */
    NUM_UI_PENS = 32
};

/* ---------------------------------------------------------------- gfx */
enum { LAYOUT_LORES = 0, LAYOUT_HIRES = 1 };

struct Gfx {
    struct Screen   *scr;
    struct Window   *win;
    struct RastPort *rp;
    struct TextFont *font;
    BOOL  font_is_ours;
    UWORD w, h, depth;
    UBYTE layout;       /* LAYOUT_LORES or LAYOUT_HIRES */
    UBYTE sx;           /* 1 lowres, 2 hires: horizontal pixels per design pixel */
    UWORD menu_w, top, row_h, list_top, list_row_h, footer_y, pane_x, pane_w;
    UWORD baseline, font_h;
    BOOL  pens_shared;  /* datatypes may remap into pens 32..255 */
};

BOOL gfx_open(struct Gfx *g, const struct Caps *c);
void gfx_close(struct Gfx *g);
void gfx_fill(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1, UBYTE pen);
void gfx_text(struct Gfx *g, WORD x, WORD y, const char *s, UBYTE pen, WORD maxw);
WORD gfx_textlen(struct Gfx *g, const char *s);
void gfx_logo(struct Gfx *g, WORD x, WORD y);
void gfx_icon(struct Gfx *g, int icon, WORD x, WORD y, UBYTE pen);
BOOL gfx_picture(struct Gfx *g, const char *path, WORD x, WORD y, WORD w, WORD h);
void gfx_picture_free(struct Gfx *g);

/* ---------------------------------------------------------------- ags */
enum { IT_DIR = 1, IT_RUN, IT_TEXT, IT_ACTION };

struct Item {
    char  *label;       /* what the user sees */
    char  *path;        /* full path: folder, or entry base without .run */
    char  *sortkey;
    UBYTE  kind;
    UBYTE  action;
};

struct ItemList {
    APTR          pool;
    struct Item  *items;
    LONG          count, cap;
};

struct Ags {
    BOOL found;
    BOOL set_assigns;           /* we had to create AGS: ourselves */
    char root[256];             /* "AGS:" when present */
    char hw[16], adult[4], expertperm[4];
    struct ItemList top;            /* top-level .ags folders */
};

BOOL ags_init(struct Ags *a);
void ags_free(struct Ags *a);
BOOL list_init(struct ItemList *l);
void list_free(struct ItemList *l);
struct Item *list_add(struct ItemList *l, const char *label, const char *path, const char *sortkey, UBYTE kind);
void list_sort(struct ItemList *l);
LONG ags_read_dir(const char *dir, struct ItemList *out, BOOL adult);
void ags_clean_name(const char *raw, char *out, LONG max);
BOOL ags_is_separator(const char *clean);
LONG ags_read_txt(const char *base, char lines[][96], LONG maxlines);
BOOL ags_screenshot_path(const char *base, char *out, LONG max);
BOOL ags_write_launch_script(const struct Ags *a, const char *base, const char *script);

/* ---------------------------------------------------------------- textbox */
struct TextBox {
    BOOL  open, first_white, dragging;
    WORD  x, y, w, h;           /* text area on screen */
    WORD  bar_x, bar_w;         /* scroll bar column; bar_w 0 when all text fits */
    WORD  line_h, drag_off;
    LONG  content_h;            /* pixels of text */
    LONG  pos, target;          /* scroll offset in pixels, now and heading to */
    LONG  drawn_pos, thumb_y, thumb_h;
    const struct ItemList *lines;
    struct BitMap *bm;          /* pre-rendered text, 3 planes */
};

BOOL tb_open(struct Gfx *g, struct TextBox *tb, WORD x, WORD y, WORD w, WORD h,
             const struct ItemList *lines, BOOL first_white);
void tb_close(struct TextBox *tb);
void tb_draw(struct Gfx *g, struct TextBox *tb);
BOOL tb_animating(const struct TextBox *tb);
void tb_scroll_by(struct TextBox *tb, LONG dpx);
void tb_step(struct Gfx *g, struct TextBox *tb);
BOOL tb_mouse_down(struct TextBox *tb, WORD mx, WORD my);
void tb_mouse_move(struct TextBox *tb, WORD my);
void tb_mouse_up(struct TextBox *tb);
void tb_refresh_bar(struct Gfx *g, struct TextBox *tb);

/* ---------------------------------------------------------------- art */
extern const UBYTE logo_lo[], logo_hi[];
extern const UWORD logo_lo_w, logo_lo_h, logo_hi_w, logo_hi_h;
extern const UWORD icons16[][16];   /* lowres, hand-drawn */
extern const ULONG icons32[][16];   /* hires, from vector shapes */
extern const ULONG ui_palette32[];
enum { ICON_GAMES, ICON_MODS, ICON_DEMOS, ICON_VIDEOS, ICON_PICTURES, ICON_MUSIC,
       ICON_RADIO, ICON_SETTINGS, ICON_ABOUT, ICON_COUNT };

/* ---------------------------------------------------------------- util */
int  amce_stricmp(const char *a, const char *b);
void amce_strlcpy(char *d, const char *s, LONG n);
void amce_strlcat(char *d, const char *s, LONG n);

#endif
