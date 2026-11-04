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
BOOL caps_mode_is_rtg(ULONG id);
ULONG caps_find_rtg_mode(UWORD w, UWORD h, UWORD *depth);   /* deepest first */
const char *caps_release_name(UWORD version);

/* ---------------------------------------------------------------- pens */
enum {
    PEN_BG = 0, PEN_BAR, PEN_RULE, PEN_TEXT, PEN_DIM, PEN_WHITE, PEN_HL,
    PEN_SELDIM, PEN_ACCENT, PEN_KEYCAP, PEN_FAINT,
    NUM_UI_PENS = 16,    /* AmigaMCE's pens are 0-15; 16-31 keep the screen's colours */
    POINTER_PEN0 = 17    /* 17-19: the mouse pointer's colours, from the user's prefs */
};

/* ---------------------------------------------------------------- gfx */
/* The layout is designed for a 256-line screen, in design units. A screen
 * draws it k/256 times larger: 1x on every native mode (640x200/256 hires
 * looks like hires laced, on tall pixels), and on RTG, sized as if the
 * screen had RTG_LINES lines:
 *   pixel style:  a whole factor, lines / RTG_LINES rounded down
 *                 (1024x768 2x, smaller 1x), the pixel art in whole pixels;
 *   smooth style: exactly lines / RTG_LINES (1024x768 2x, 800x600 1.56x,
 *                 640x480 1.25x), anti-aliased text, icons and logo
 *                 pre-rendered for that size (Fonts/amce-smooth/<lines>),
 *                 on 15/16/24/32-bit screens only. */
enum { STYLE_PIXEL, STYLE_SMOOTH };
#define RTG_LINES 384           /* keep in step with tools/mksmooth.py */

/* Text faces (gfx_text, gfx_textlen). Pixel style has one font for all. */
enum { TS_BODY, TS_BOLD, TS_SMALL, TS_FACES,
       TS_OPAQUE = 0x10 };  /* also fill the background behind the text */

/* Screen modes offered in Settings. */
enum { SM_AUTO, SM_NTSC_LORES, SM_PAL_LORES, SM_NTSC_HIRES, SM_PAL_HIRES,
       SM_NTSC_LACED, SM_PAL_LACED, SM_RTG_640, SM_RTG_800, SM_RTG_1024, SM_COUNT };

struct Gfx {
    struct Screen   *scr;
    struct Window   *win;
    struct RastPort *rp;
    struct TextFont *font;
    BOOL  font_is_ours;
    UWORD w, h, depth;
    UWORD k;            /* layout scale x 256: GU(g, design units) = pixels */
    UBYTE kint;         /* whole factor the pixel artwork is enlarged by */
    UBYTE style;        /* STYLE_* in use (smooth may fall back to pixel) */
    struct Smooth *sm;  /* smooth-style artwork, NULL in pixel style */
    UWORD menu_w, top, row_h, list_top, list_row_h, footer_y, pane_x, pane_w;
    UWORD baseline, font_h;
    UWORD icon_px, mark_px;     /* menu icon and row marker size on screen */
    UWORD logo_w, logo_h;
    BOOL  pens_shared;  /* datatypes may remap into pens 32..255 */
    BOOL  planar;       /* native planar bitmap (FALSE on RTG) */
    BOOL  truecolor;    /* RTG 15/16/24/32-bit: no pen sharing needed for pictures */
    UBYTE *logo_masks;          /* one chip-RAM mask per logo pen, for BltTemplate */
    UBYTE logo_pens[32], logo_npens;
    UWORD logo_bpr;             /* bytes per logo mask row */
    UBYTE *icon_masks;          /* pixel style: menu icon masks in chip RAM */
    UWORD icon_bpr, mark_bpr;   /* bytes per icon / marker mask row */
    UBYTE *mark_masks;          /* pixel style: list row marker masks */
    BYTE  mode;         /* SM_* actually open, never SM_AUTO */
    WORD  pic_x, pic_y, pic_w, pic_h;   /* the picture on screen (pic_w 0: none) */
};

/* Design units to pixels on this screen. */
#define GU(g, v) ((WORD)(((LONG)(v) * (g)->k + 128) >> 8))

/* A big letter for the list's letter overlay: one mask in chip RAM for
 * BltTemplate. Built once per letter and size, kept until the screen
 * closes (gfx_bigletter), so scrolling never rebuilds one it has shown. */
struct BigLetter {
    UBYTE *mask;
    ULONG size;
    UWORD w, h, bpr;
    UWORD cell_w;               /* the widest letter at this size */
};

const char *gfx_mode_key(int sm);
const char *gfx_mode_label(int sm);
int  gfx_mode_from_key(const char *key);                 /* -1 if unknown */
const char *gfx_mode_problem(const struct Caps *c, int sm);  /* NULL if usable */
BOOL gfx_mode_is_rtg(int sm);
BOOL  gfx_mode_tall(int sm);                             /* 640x200/256: tall pixels */
UWORD gfx_mode_depth(int sm);                            /* RTG: bits per pixel found */
int  gfx_mode_auto(const struct Caps *c);
BOOL gfx_open(struct Gfx *g, const struct Caps *c, int sm, int style);
BOOL gfx_style_possible(const struct Caps *c, int sm, int style);
void gfx_menu_labels(const char *const *labels, int n); /* before gfx_open */
void gfx_close(struct Gfx *g);
void gfx_fill(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1, UBYTE pen);
void gfx_fill_bg(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1);
/* Text in a cell font_h tall at y; bg is what it is drawn over (smooth
 * edges blend into it). maxw > 0 cuts it to fit; ts is TS_* (| TS_OPAQUE). */
void gfx_text(struct Gfx *g, WORD x, WORD y, const char *s, UBYTE fg, UBYTE bg, WORD maxw, int ts);
WORD gfx_textlen(struct Gfx *g, const char *s, int ts);
void gfx_logo(struct Gfx *g, WORD x, WORD y);
void gfx_icon(struct Gfx *g, int icon, WORD x, WORD y, UBYTE fg, UBYTE bg);
void gfx_mark(struct Gfx *g, int mark, WORD x, WORD y, UBYTE fg, UBYTE bg); /* MARK_* icon */
const struct BigLetter *gfx_bigletter(struct Gfx *g, UBYTE c, WORD ink_h);  /* NULL: none */
void gfx_bigletter_draw(struct Gfx *g, const struct BigLetter *bl, WORD x, WORD y, UBYTE pen);

/* ---------------------------------------------------------------- smooth */
#define SMOOTH_LEVELS 8         /* coverage steps: 0 none .. 7 full colour */
struct Smooth *sm_load(UWORD lines);    /* PROGDIR:Fonts/amce-smooth/<lines> */
void sm_free(struct Smooth *s);
void sm_attach(struct Smooth *s, struct Gfx *g);    /* screen open: pens, metrics */
void sm_detach(struct Smooth *s);
WORD sm_textlen(struct Smooth *s, const char *str, LONG n, int face);
LONG sm_fit(struct Smooth *s, const char *str, LONG n, int face, WORD maxw);
void sm_text(struct Smooth *s, struct Gfx *g, WORD x, WORD y, const char *str, LONG n,
             int face, UBYTE fg, UBYTE bg, BOOL opaque);
void sm_icon(struct Smooth *s, struct Gfx *g, int icon, WORD x, WORD y, UBYTE fg, UBYTE bg);
void sm_mark(struct Smooth *s, struct Gfx *g, int mark, WORD x, WORD y, UBYTE fg, UBYTE bg);
UWORD sm_icon_px(const struct Smooth *s);
UWORD sm_mark_px(const struct Smooth *s);
UWORD sm_cell_h(const struct Smooth *s);
UWORD sm_scale(const struct Smooth *s);                 /* x 256 */
UWORD sm_baseline(const struct Smooth *s);
BOOL sm_logo(struct Smooth *s, struct Gfx *g);      /* fills g->logo_* masks and pens */
BOOL sm_logo_file(struct Gfx *g, UWORD scale);      /* pixel style, deep RTG: smooth logo */
BOOL sm_bigletter(struct Smooth *s, UBYTE c, WORD ink_h, struct BigLetter *bl);
UBYTE sm_pen(struct Smooth *s, struct Gfx *g, UBYTE fg, UBYTE bg, UBYTE level);
/* A picture fitted into w x h at (x, y), centred across; whole: at a whole
 * scale (1x, 2x...) when 1x fits. *drawn_h (may be NULL): its height. */
BOOL gfx_picture(struct Gfx *g, const char *path, WORD x, WORD y, WORD w, WORD h, BOOL whole,
                 WORD *drawn_h);
void gfx_picture_free(struct Gfx *g);

/* ---------------------------------------------------------------- ags */
enum { IT_DIR = 1, IT_RUN, IT_TEXT, IT_ACTION };
/* What the left-hand marker column shows for a row (struct Item.mark). */
enum { MARK_NONE, MARK_ARROW, MARK_CHECK_OFF, MARK_CHECK_ON, MARK_RADIO_OFF, MARK_RADIO_ON };

struct Item {
    char  *label;       /* what the user sees */
    char  *path;        /* full path: folder, or entry base without .run */
    char  *sortkey;
    UBYTE  kind;
    UBYTE  action;
    UBYTE  mark;        /* MARK_*: folders get MARK_ARROW automatically */
    UBYTE  flags;       /* ITEM_* */
};
#define ITEM_DISABLED 1 /* shown greyed out, cannot be opened */
#define ITEM_FLAT     2 /* a folder of folders opened as one list (All Games) */

/* Keep struct Item a power of two in size: then count * sizeof is a shift.
 * At 18 bytes it became a __mulsi3 call that GCC moved between loading
 * CopyMem's registers and the call, so CopyMem got garbage (see
 * tools/check_syscalls.py). This line fails to compile otherwise. */
typedef char amce_item_size_is_power_of_two
    [(sizeof(struct Item) & (sizeof(struct Item) - 1)) == 0 ? 1 : -1];

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
    char added[160];            /* assigns AmigaMCE made because they were missing */
    struct ItemList top;            /* top-level .ags folders */
};

BOOL ags_init(struct Ags *a);
void ags_free(struct Ags *a);
BOOL list_init(struct ItemList *l);
void list_free(struct ItemList *l);
struct Item *list_add(struct ItemList *l, const char *label, const char *path, const char *sortkey, UBYTE kind);
void list_sort(struct ItemList *l);
LONG ags_read_dir(const char *dir, struct ItemList *out, BOOL adult);
LONG ags_read_dir_flat(const char *dir, struct ItemList *out, BOOL adult);   /* folders merged */
void ags_clean_name(const char *raw, char *out, LONG max);
BOOL ags_is_separator(const char *clean);
LONG ags_read_txt(const char *base, char lines[][96], LONG maxlines);
BOOL ags_screenshot_path(const char *base, char *out, LONG max);
void ags_unquote_path(const char *in, char *out, LONG max);
BOOL ags_write_launch_script(const struct Ags *a, const char *base, const char *script);

/* ---------------------------------------------------------------- textbox */
struct TextBox {
    BOOL  open, first_white, dragging, drawn;
    WORD  x, y, w, h;           /* text area on screen */
    WORD  bar_x, bar_w;         /* scroll bar column; bar_w 0 when all text fits */
    WORD  line_h, drag_off;
    UBYTE mask;                 /* bitplanes the copy touches */
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
LONG tb_remaining(const struct TextBox *tb, LONG dir);
void tb_step(struct Gfx *g, struct TextBox *tb);
BOOL tb_mouse_down(struct TextBox *tb, WORD mx, WORD my);
void tb_mouse_move(struct TextBox *tb, WORD my);
void tb_mouse_up(struct TextBox *tb);
void tb_refresh_bar(struct Gfx *g, struct TextBox *tb);

/* ---------------------------------------------------------------- art */
extern const UBYTE logo_lo[];
extern const UWORD logo_lo_w, logo_lo_h;
extern const UWORD icons16[][16];   /* menu icons, hand-drawn */
extern const UWORD marks16[][11];   /* list row markers (MARK_ARROW..) */
#define MARK_SIZE 11                /* marker icons are 11 x 11 design pixels */
extern const ULONG ui_palette32[];
enum { ICON_GAMES, ICON_MODS, ICON_DEMOS, ICON_VIDEOS, ICON_PICTURES, ICON_MUSIC,
       ICON_RADIO, ICON_SETTINGS, ICON_ABOUT, ICON_COUNT };

/* ---------------------------------------------------------------- log */
void amce_log_open(BOOL force);     /* force, or PROGDIR:AmigaMCE.debug exists */
const char *amce_log_path(void);    /* where it is going, or NULL when off */

/* ---------------------------------------------------------------- debug */
BOOL  dbg_cpu_start(void);
void  dbg_cpu_stop(void);
LONG  dbg_cpu_sample(void);         /* 0..100 once a second, -1 at first */
void  dbg_set_timer(struct Device *timer);
ULONG time_ms(void);                /* E-clock milliseconds, 0 without a timer */
void  dbg_sleep_begin(void);        /* around every Wait / WaitTOF */
void  dbg_sleep_end(void);
LONG  dbg_app_sample(void);         /* AmigaMCE's own CPU share, 0..100, -1 at first */
void  dbg_mem_baseline(void);
ULONG dbg_mem_used(ULONG *chip);
void amce_log(const char *what, const char *detail);
void amce_log_close(void);

/* ---------------------------------------------------------------- util */
int  amce_stricmp(const char *a, const char *b);
void amce_strlcpy(char *d, const char *s, LONG n);
void amce_strlcat(char *d, const char *s, LONG n);

#endif
