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
    BOOL  aga, ecs, pal, ehb, rtg, net, mp3_mhi, mp3_mpega, amissl, ahi, flacdec;
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
    PEN_SELDIM, PEN_ACCENT, PEN_KEYCAP, PEN_FAINT, PEN_YELLOW,
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

/* The fonts the laced native screens (640 x 400/512) can use, Settings >
 * Style there (gfx.c). */
struct LacedFont {
    const char *name;   /* as Settings shows it, and as it is saved */
    const char *file;   /* in PROGDIR:Fonts */
    UBYTE ysize;        /* its line height: the layout is ysize/12 */
    UBYTE kint;         /* the artwork enlarged this many times */
};
#define LFONT_COUNT   6
#define LFONT_DEFAULT 2 /* Galmuri11 */
extern const struct LacedFont gfx_laced_fonts[LFONT_COUNT];
extern int gfx_laced_font;          /* set before gfx_open / gfx_restyle */
#define RTG_LINES 384           /* keep in step with tools/mksmooth.py */

/* Text faces (gfx_text, gfx_textlen). Pixel style has one font for all. */
enum { TS_BODY, TS_BOLD, TS_SMALL, TS_FACES,
       TS_OPAQUE = 0x10 };  /* also fill the background behind the text */

/* Screen modes offered in Settings. */
enum { SM_AUTO, SM_NTSC_LORES, SM_PAL_LORES, SM_NTSC_HIRES, SM_PAL_HIRES,
       SM_NTSC_LACED, SM_PAL_LACED, SM_RTG_640, SM_RTG_800, SM_RTG_1024, SM_COUNT };

/* What a decoded picture is (gfx_picture, view_show). */
struct PicInfo {
    UWORD w, h, depth;          /* depth > 8: true colour */
    ULONG mode;                 /* its display mode ID (HAM, EHB, hires...) */
    char  fmt[16];              /* the datatype's name: "ILBM", "GIF"... */
    char  cls[24];              /* the class that read it: "warpjpeg"... ("" ilbm.c) */
};
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
    struct PicInfo pic;         /* and what it is */
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
BOOL gfx_laced(int smode);
BOOL gfx_style_possible(const struct Caps *c, int sm, int style);
void gfx_menu_labels(const char *const *labels, int n); /* before gfx_open */
void gfx_menu_refit(struct Gfx *g);
BOOL gfx_menu_narrow(struct Gfx *g, BOOL narrow);
WORD gfx_pane_w_wide(struct Gfx *g);
void gfx_close(struct Gfx *g);
BOOL gfx_restyle(struct Gfx *g, int style);    /* same screen, new style (gfx.c) */
void gfx_fill(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1, UBYTE pen);
void gfx_fill_bg(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1);
void gfx_dot(struct Gfx *g, WORD cx, WORD cy, WORD r, UBYTE pen, UBYTE bg);   /* filled circle */
/* Text in a cell font_h tall at y; bg is what it is drawn over (smooth
 * edges blend into it). maxw > 0 cuts it to fit; ts is TS_* (| TS_OPAQUE). */
void gfx_text(struct Gfx *g, WORD x, WORD y, const char *s, UBYTE fg, UBYTE bg, WORD maxw, int ts);
WORD gfx_textlen(struct Gfx *g, const char *s, int ts);
void gfx_text_rp(struct Gfx *g, struct RastPort *rp, WORD x, WORD y, const char *s, UBYTE fg,
                 WORD maxw);

/* lang.c: translations. _("English") is its translation in the catalog
 * open (the Workbench's language, or Settings'), or itself; N_() marks a
 * string in a table for tools/catalog.py, translated where it is used. */
const char *amce_tr(const char *s);
ULONG amce_tr_id(const char *s);
#define _(s) amce_tr(s)
#define N_(s) (s)
void amce_lang_scan(void);
int amce_lang_count(void);
const char *amce_lang_name(int i);
const char *amce_lang_wb(void);
void amce_lang_open(const char *lang);
void amce_lang_close(void);
void amce_lang_quit(void);
const char *amce_lang_now(void);
const char *amce_lang_iso(void);

/* cjk.c: Japanese, Chinese and Korean text, kept in strings as three bytes
 * a character (see there), drawn from glyph packs or shown as a tag */
int cjk_dec(const UBYTE *s, ULONG n, ULONG *cp);
BOOL cjk_any(const char *s, ULONG n);
ULONG cjk_tagged(const char *s, ULONG n, char *out, ULONG max);
void cjk_open(struct Gfx *g);
void cjk_close(void);
BOOL cjk_tags(void);
WORD cjk_textlen(struct Gfx *g, struct RastPort *rp, const char *s, ULONG n, int face);
ULONG cjk_fit(struct Gfx *g, struct RastPort *rp, const char *s, ULONG n, int face, WORD maxw);
WORD cjk_text(struct Gfx *g, struct RastPort *rp, WORD x, WORD y, const char *s, ULONG n,
              UBYTE fg, UBYTE bg, BOOL opaque, int face, WORD maxw);
void gfx_logo(struct Gfx *g, WORD x, WORD y);
void gfx_icon(struct Gfx *g, int icon, WORD x, WORD y, UBYTE fg, UBYTE bg);
void gfx_cycle_glyph(struct Gfx *g, WORD x, WORD y, WORD px, UBYTE pen);
void gfx_icon_sized(struct Gfx *g, int icon, WORD x, WORD y, UBYTE fg, WORD px);
struct TextFont *gfx_open_laced_font(int i);
void gfx_mark(struct Gfx *g, int mark, WORD x, WORD y, UBYTE fg, UBYTE bg); /* MARK_* icon */
extern UBYTE gfx_eq_frame;      /* MARK_PLAYING's bars: the frame of their dance */
extern BOOL gfx_eq_still;       /*   and still (paused, stopped) */
extern UBYTE gfx_eq_kind;       /*   0 the dance, 1 four bars of gfx_eq_lev (the
                                 *   spectrum's), 2 two across (the meters') */
extern UBYTE gfx_eq_lev[4];     /*   0-11 */
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
UWORD sm_pens_used(const struct Smooth *s);     /* pens its shades and logo took */
/* A picture fitted into w x h at (x, y), centred across; whole: at a whole
 * scale (1x, 2x...) when 1x fits. *drawn_h (may be NULL): its height. */
BOOL gfx_picture(struct Gfx *g, const char *path, WORD x, WORD y, WORD w, WORD h, BOOL whole,
                 WORD *drawn_h);
void gfx_picture_free(struct Gfx *g);
/* The picture on screen cleared (as far as it is inside x0,y0..x1,y1),
 * then its pens let go; without a flash on planar screens. */
void gfx_picture_clear(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1);
BOOL gfx_picture_move(struct Gfx *g, WORD x, WORD y);   /* pens kept */
/* bm (w x h, like the screen's) to x, y, band by band with the beam: no
 * half-drawn frame is seen on a planar screen. */
void gfx_blit_beam(struct Gfx *g, struct BitMap *bm, WORD x, WORD y, WORD w, WORD h);
void gfx_clear_beam(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1);
/* The same in two halves: decoding (any process; remap FALSE takes no
 * pens) and drawing what was decoded (the object stays the caller's, to
 * draw again, unless *taken). */
APTR pic_decode(const char *path, struct Screen *scr, BOOL remap, BOOL *remapped,
                struct PicInfo *pi);
BOOL gfx_picture_obj(struct Gfx *g, APTR obj, BOOL remapped, WORD x, WORD y, WORD w, WORD h,
                     BOOL whole, WORD *drawn_h, BOOL *taken);
/* The size a sw x sh picture is drawn at in a w x h box (tall: the
 * screen has tall pixels; whole: a whole scale when 1x fits). */
void pic_fit(ULONG sw, ULONG sh, BOOL tall, WORD w, WORD h, BOOL whole, ULONG *dw, ULONG *dh);
/* A picture made small, in colours of its own (thumb.c): colour numbers
 * and their colours, no pens. Made in any process, drawn by gfx_thumb. */
struct Thumb {
    UWORD w, h, mod;            /* mod: bytes a row of px (w rounded up to 16) */
    UWORD ncol;
    ULONG pal[256];             /* 0xRRGGBB */
    UBYTE *px;
};
struct Thumb *thumb_make(APTR obj, ULONG dw, ULONG dh, LONG maxcol);  /* NULL: could not */
void thumb_free(struct Thumb *t);
struct Thumb *thumb_new(ULONG w, ULONG h);  /* cleared, no colours yet */
BOOL thumb_reduce(struct Thumb *t, LONG maxcol);    /* colours in use cut to maxcol */
void thumb_times(ULONG *read, ULONG *cells, ULONG *cut, ULONG *map);  /* the last one made */
/* An IFF ILBM read by AmigaMCE itself (ilbm.c), straight into a thumbnail
 * for a w x h box (pic_fit): no datatype. NULL: not an ILBM it reads (HAM,
 * deep, PBM...) or no memory; try the datatype then. */
struct Thumb *ilbm_thumb(const char *path, WORD w, WORD h, BOOL tall, BOOL whole, LONG maxcol,
                         struct PicInfo *pi);
BOOL gfx_thumbs(struct Gfx *g);
struct BitMap *gfx_thumb_bitmap(struct Gfx *g, const struct Thumb *t);  /* see gfx.c */
/* Double buffering (RTG; prototype: PROGDIR:AmigaMCE.dbuf): drawing goes
 * into the buffer not shown; gfx_present shows it, if gfx_dirty was said. */
extern BOOL gfx_dbuf_wanted;
extern ULONG gfx_present_ms;
extern ULONG gfx_flips;
void gfx_dbuf_start(struct Gfx *g);
void gfx_dbuf_stop(struct Gfx *g);
BOOL gfx_present(struct Gfx *g);     /* TRUE: it flipped (a frame waited) */
void gfx_dirty(void);
BOOL gfx_dbuf_active(void);
BOOL gfx_quiet_begin(void);     /* around a small update shown without a flip */
void gfx_quiet_end(struct Gfx *g, BOOL was, WORD x0, WORD y0, WORD x1, WORD y1);
extern ULONG gfx_draw_ms[3];    /* the last gfx_thumb's time: colours, pixels, copy up */         /* the screen can show them (gfx_thumb) */
LONG gfx_thumb_colours(struct Gfx *g);
void gfx_chunky(struct Gfx *g, struct BitMap *bm, UBYTE *buf, ULONG w, ULONG h, ULONG mod);  /* colours a thumbnail made now may have */
BOOL gfx_thumb(struct Gfx *g, const struct Thumb *t, const struct PicInfo *pi, WORD x, WORD y,
               WORD w, WORD h, WORD *drawn_h);
/* The full-screen picture viewer, on AmigaMCE's own screen (view.c). On
 * 256-colour and native screens a picture gets the screen's whole palette
 * while it is shown, and the info bar takes its colours from the picture.
 * view_end leaves the screen black; redraw it, then view_restore. */
BOOL view_begin(struct Gfx *g);
/* pre: decoded ahead; or pre_thumb (made ahead for the screen, given
 * up to view_show), pre_info: what it is */
BOOL view_show(struct Gfx *g, const char *path, const char *label, struct PicInfo *pi,
               char *scale, LONG scalemax, APTR pre, BOOL pre_remapped,
               struct Thumb *pre_thumb, const struct PicInfo *pre_info);
/* The size a picture is shown at full screen (gw x gh, tall pixels), and
 * its scale as text (scale may be NULL). */
void view_fit(UWORD gw, UWORD gh, BOOL tall, const struct PicInfo *pi, ULONG *dw, ULONG *dh,
              char *scale, LONG max);
void view_bar(struct Gfx *g, const char *title, const char *detail, const char *hint);
void view_bar_hide(struct Gfx *g);
void view_spin(struct Gfx *g, UWORD phase);    /* the next picture coming: in the bar, or a corner */
void view_spin_hide(struct Gfx *g);
BOOL view_bar_shown(void);
void view_end(struct Gfx *g);
void view_restore(struct Gfx *g);
void pic_describe(const struct PicInfo *pi, char *out, LONG max);   /* "320 x 256, 32 colours" */
struct BitMapHeader;
void pic_info(APTR o, const struct BitMapHeader *bmh, struct PicInfo *pi);   /* o: a datatype */

/* ---------------------------------------------------------------- ags */
enum { IT_DIR = 1, IT_RUN, IT_TEXT, IT_ACTION, IT_PIC, IT_MOD, IT_NET, IT_STATION,
       IT_PLEXDIR, IT_PLEXTRACK, IT_PLEXSRV,      /* Plex (plex.c) */
       IT_SONG,                                   /* a music file here: MP3, FLAC (mods.c) */
       IT_PLEXPHOTO };                            /* a photo of a Plex photo library */
/* What the left-hand marker column shows for a row (struct Item.mark). */
enum { MARK_NONE, MARK_ARROW, MARK_CHECK_OFF, MARK_CHECK_ON, MARK_RADIO_OFF, MARK_RADIO_ON,
       MARK_PLAY /* a folder Fire plays (slideshow); Right opens it */,
       MARK_PLAYING /* the music playing now: dancing bars, drawn (not a bitmap) */,
       MARK_CYCLE /* a setting Fire steps through (a cycle gadget's glyph, drawn) */ };

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
#define ITEM_HEAD    16 /* a Settings page's heading over a group: not selectable */
#define ITEM_FLAT     2 /* a folder of folders opened as one list (All Games) */
#define ITEM_SPLIT    4 /* AGS entry: path is its folder (shared), sortkey its file
                         * name; item_path() gives the whole path */
#define ITEM_GONE     8 /* its letter's names were dropped to save memory (the
                         * label is the letter): read again before use */

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
    const char   *dir_src;      /* list_copy: last folder string copied, */
    char         *dir_dst;      /* and its copy in this pool */
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
BOOL list_init_puddle(struct ItemList *l, ULONG puddle);   /* small lists: less slack */
void list_free(struct ItemList *l);
struct Item *list_add(struct ItemList *l, const char *label, const char *path, const char *sortkey, UBYTE kind);
struct Item *list_add_first(struct ItemList *l, const char *label, const char *path,
                            const char *sortkey, UBYTE kind);
void list_drop_first(struct ItemList *l);
void list_sort(struct ItemList *l);
char *list_strdup(struct ItemList *l, const char *s);   /* kept until list_free */
struct Item *list_append(struct ItemList *l, const struct Item *src);
BOOL list_set(struct ItemList *l, LONG idx, const struct Item *src);   /* as it is */
struct Item *list_add_split(struct ItemList *l, const char *label, char *dir,
                            const char *name, UBYTE kind);  /* ITEM_SPLIT; dir in l's pool */
void list_drop_items(struct ItemList *l);   /* free the entries, keep the strings */
struct Item *list_copy(struct ItemList *l, const struct Item *it, const char *label);
char *item_path(const struct Item *it, char *out, LONG max);    /* an entry's whole path: out */
LONG ags_read_dir(const char *dir, struct ItemList *out, BOOL adult);
LONG ags_read_dir_flat(const char *dir, struct ItemList *out, BOOL adult);   /* folders merged */
LONG ags_read_menu(const char *dir, struct ItemList *out, BOOL adult);  /* A-Z folders merged */
BOOL ags_letter_menu(const struct ItemList *subs);   /* its folders are all A-Z... */
void ags_clean_name(const char *raw, char *out, LONG max);
BOOL ags_is_separator(const char *clean);
LONG ags_read_txt(const char *base, char lines[][96], LONG maxlines);
BOOL ags_screenshot_path(const char *base, char *out, LONG max);
void ags_unquote_path(const char *in, char *out, LONG max);
BOOL ags_write_launch_script(const struct Ags *a, const char *base, const char *script);

/* ---------------------------------------------------------------- pictures */
const char *pics_format(const char *name);              /* "GIF".. by name, NULL if none */
LONG pics_read_dir(const char *dir, struct ItemList *out);  /* -1 if it cannot be read */
/* Pictures in dir, its letter folders' included (-1: unreadable); *ndirs:
 * its other folders; first: the path of its first picture by name. */
LONG pics_scan(const char *dir, LONG *ndirs, char *first, LONG firstmax);

/* ---------------------------------------------------------------- unpack */
/* Packed files (PowerPacker, XPK, ...) through xfdmaster.library (unpack.c). */
UBYTE *unpack_read_whole(const char *path, ULONG *len);     /* AllocVec, NULL if not */
UBYTE *unpack_buffer(UBYTE *buf, ULONG *len, char *packer, LONG pmax, const char **why);

/* ---------------------------------------------------------------- mods */
/* Folders of modules (mods.c): IT_DIR and IT_MOD rows. */
LONG songs_read_dir(const char *dir, struct ItemList *out);  /* folders with music, music files */
const char *song_format(const char *path);
void songs_can_play(BOOL mp3, BOOL flac);   /* what the lists show (caps) */
BOOL song_name_ok(const char *name);        /* a music file's name, of a kind played */
/* songinfo.c: what a music file holds */
struct SongInfo {
    char title[100], artist[100], band[100], album[100], year[12], track[12], genre[40];
    char fmt[8];                /* MP3, FLAC, WAV, AIFF, 8SVX */
    ULONG rate, channels, bits, kbps, ms, size;
    BOOL art;                   /* a picture: its folder's or its own */
};
enum { PCM_WAV = 1, PCM_AIFF, PCM_8SVX };
struct PcmFmt {
    UBYTE kind;
    BOOL big, signed8, svx_stereo;  /* big-endian; 8-bit signed; 8SVX's two halves */
    ULONG rate, frames, off, bytes;  /* samples: from off, bytes long */
    UWORD channels, bits, align;
};
BOOL pcm_header(const UBYTE *b, LONG n, struct PcmFmt *f);
BOOL song_info(const char *path, struct SongInfo *si, char *art, LONG artmax, BOOL extract);
BOOL song_folder_art(const char *dir, char *out, LONG max);
int song_lines(const char *path, const struct SongInfo *si, char lines[][96], int max);
int song_dir_lines(const char *dir, char lines[][96], int max, char *art, LONG artmax,
                   BOOL extract);
int song_details(const char *path, char lines[][96], int max, char *art, LONG artmax);
BOOL song_art_of(const char *path, char *art, LONG artmax, BOOL extract);
LONG mods_read_dir(const char *dir, struct ItemList *out);  /* -1 if it cannot be read */
BOOL mods_is_module(const char *dir, const char *name);
void mods_label(const char *name, char *out, LONG max);

/* Playing them (modplay.c, ptcore.c): ProTracker modules on Paula, timed
 * by a CIA timer interrupt. Errors come back as text for the user. */
struct MpStatus {
    WORD order, row, songlen, pattern, speed, bpm;
    ULONG elapsed_ms, total_ms;
    UBYTE level[4];             /* the channel meters, 0-64 */
    UBYTE sample[4];            /* the sample each channel last started (1-31) */
    WORD sub, subs;             /* subsong (1..) of how many */
    BOOL running, paused;
};
BOOL mp_load(const char *path, char *err, LONG errmax);   /* stops what plays */
BOOL mp_play(char *err, LONG errmax);   /* from where it is (the start once loaded) */
void mp_pause(BOOL on);
void mp_stop(void);                     /* stopped, back at the start */
void mp_rewind(void);
void mp_seek(int pos);                  /* to a position in the song */
void mp_shutdown(void);                 /* hardware and module let go */
void mp_set_volume(int v);              /* 0-64 */
int  mp_volume(void);
BOOL mp_loaded(void);
BOOL mp_playing(void);
BOOL mp_paused(void);
BOOL mp_running(void);                  /* playing or paused */
BOOL mp_ended(void);                    /* the song has come round (or F00) */
const char *mp_path(void);
const char *mp_title(void);
const char *mp_tag(void);               /* "M.K.", "ST15"... */
int  mp_samples(void);
const char *mp_sample_name(int i, BOOL *used);
ULONG mp_position_ms(int pos);
void mp_status(struct MpStatus *s);     /* also lets the meters fall: call ~25 a second */
BOOL mp_step(int d);
BOOL mp_can_step(int d);
void mp_tempo_check(void);   /* once a second: the tempo timer's rate, logged if off */                    /* back/on a position: FALSE if it cannot */
void mp_subsongs(int *cur, int *n);
BOOL mp_subsong_step(int d);            /* FALSE: no such subsong */
const char *mp_author(void);
BOOL mp_seek_ms(ULONG ms);
/* a station (radio.c's item): its stream played (stream.c) as an engine */
BOOL mp_load_stream(const char *url, const char *name, const char *tag, UWORD cpu, char *err,
                    LONG errmax);
BOOL mp_is_stream(void);            /* a station, or a track (also mp_is_track) */
BOOL mp_load_track(const char *url, const char *title, const char *by, const char *tag,
                   ULONG total_ms, UWORD cpu, ULONG from_ms, ULONG range_from, char *err,
                   LONG errmax);
BOOL mp_is_track(void);
const char *mp_track_by(void);
int mp_meter_count(void);

/* Now Playing's visualizations (vis.c): plug-ins drawn in the meters'
 * place, each from what the playing engine can tell (mp_vis_feed). */
enum { VF_LEVELS = 1, VF_VOICES = 2, VF_PCM = 4 };
#define VIS_PCM 128                 /* samples a PCM feed gives */
struct VisVoice {                   /* a Paula voice where it plays now */
    const BYTE *pos, *loop;         /* NULL: silent */
    LONG left, replen;              /* bytes to the end, and of the loop */
    UWORD period;
    UBYTE vol;                      /* 0-64 */
};
struct VisFeed {
    UWORD have;                     /* VF_*: what this engine can give */
    const struct MpStatus *st;      /* the meters' levels and samples (VF_LEVELS) */
    struct VisVoice voice[4];       /* VF_VOICES: Paula's four */
    WORD pcm[VIS_PCM];              /* VF_PCM: the latest played, mono, about 8 kHz */
    UWORD pcm_n;
    ULONG rate;                     /* pcm's rate */
};
UWORD mp_vis_have(void);            /* VF_*: what mp_vis_feed could fill now */
ULONG mp_serial(void);              /* changes with each module, track or station */
void mp_vis_feed(struct VisFeed *f, UWORD want);

struct VisCtx {                     /* where a visualization draws */
    struct Gfx *g;
    WORD x, y, w;                   /* its left, top and width */
    WORD row_h, rows, gap;          /* laid out in rows (the meters'), gap between */
    WORD label_w;                   /* the meters' channel names, left */
};
enum { VIS_METERS, VIS_SPECTRUM, VIS_COUNT };
const char *vis_name(int v);        /* N_(): translate where shown */
void vis_select(int v);             /* (also lifts a pause for load) */
BOOL vis_paused(void);              /* paused: the music needed the CPU */
int  vis_selected(void);
int  vis_active(void);              /* the one drawn: the selected, or the meters */
int  vis_rows(void);                /* rows the active one wants (0: the meters' own) */
void vis_draw(const struct VisCtx *c, const struct MpStatus *st, BOOL full);
void vis_stop(void);                /* its memory back (a game, quitting) */
int  vis_marker(UBYTE *lev);        /* the playing marker's: VIS_*, lev 0-11 (4 or 2) */
void vis_timing(ULONG *frames, ULONG *ms100);  /* spectrum frames, their time (0.1 ms) */
void vis_tables(WORD *sine);        /* vistab.c: the FFT's sine (1024) */
void vis_loud_table(ULONG *t);      /* ...and the 64 loudness steps */
void mp_paula_check(void);          /* once a second: audio DMA left on while stopped */           /* level meters: 4 (Paula's channels), 2 stereo, 1 mono */
const char *mp_stream_name(void);

/* stream.c: internet radio, MP3 over HTTP decoded by mpega.library */
enum { ST_ENDED, ST_CONNECTING, ST_BUFFERING, ST_PLAYING, ST_FAILED };
struct StInfo {
    int state;
    const char *name, *genre;       /* what the station calls itself (icy-name, icy-genre) */
    LONG bitrate, freq, channels, dec_freq, dec_channels;
    ULONG buffer_ms, played_ms, underruns;
    ULONG avg_kbps;                 /* the bitrate on average (3 s on), 0 not yet */
    UBYTE peak_l, peak_r;
    BOOL ahi;                       /* played through AHI, not Paula */
    BOOL mhi;                       /* MP3 decoded and played by an MHI driver */
};
void st_set_buffer(int level);
void st_use_ahi(BOOL on);                  /* Settings > Sound: AHI for streams */
BOOL st_ahi(void);
void st_use_mhi(const char *driver);
void st_set_stop_url(const char *url);     /* asked before the next stream (Plex: stop) */       /* Settings > Sound: MP3 by MHI ("" mpega) */
BOOL st_mhi(void);      /* 0 short, 1 normal, 2 long: the next stream */
BOOL st_mhi_now(void);  /* the stream playing is decoded by MHI: no samples to see */
UWORD st_pcm(WORD *out, UWORD n, ULONG *rate);  /* the last n played (spectrum), 0 none */
ULONG st_length_ms(void);           /* a track's length as its stream says, 0 not known */
ULONG st_size(void);                /* ...its size in bytes, 0 not known */
BOOL st_start(const char *url, UWORD master, UWORD cpu, BOOL finite, ULONG range_from,
              ULONG skip_ms, char *err, LONG errmax);
void st_stop(void);
void st_pause(BOOL on);
void st_volume(int v);
int st_state(void);
int st_channels(void);
const char *st_error(void);
const char *st_title(void);         /* what plays now (ICY StreamTitle) */
ULONG st_title_seq(void);
void st_info(struct StInfo *i);
ULONG st_avg_kbps(void);              /* mixed modules only */

/* Modules mixed by modplug.library, streamed to Paula (mixplay.c). */
BOOL mx_available(void);
BOOL mx_load(const char *path, UBYTE *mod, ULONG len, char *err, LONG errmax);  /* mod taken */
BOOL mx_start(UWORD master, char *err, LONG errmax);
void mx_stop(void);
void mx_pause(BOOL on);
void mx_volume(int v);
BOOL mx_ended(void);
void mx_seek_ms(ULONG ms);
BOOL mx_step(int d);
BOOL mx_can_step(void);
void mx_rewind(void);
void mx_status(struct MpStatus *s, UBYTE *lev_l, UBYTE *lev_r);
UWORD mx_pcm(WORD *out, UWORD n, ULONG *rate);  /* the last n played (spectrum), 0 none */
BOOL mx_loaded(void);
void mx_unload(void);
const char *mx_title(void);
const char *mx_kind(void);
int mx_names(void);
const char *mx_name(int i);
void mx_close_library(void);
void mp_tick_count(UWORD c);            /* eagle.c: the timer, in CIA counts */
void mp_tick_run(BOOL on);

/* EaglePlayer and DeliTracker players, hosted (eagle.c, eaglehost.s). */
struct EpInfo {
    const char *player, *song, *author;
    WORD sub, subs;             /* 1..subs */
    WORD sub_raw, sub_min, sub_max;
    WORD pos, poslen, row;      /* -1: not known */
};
LONG ep_players(void);
BOOL ep_load(const char *path, UBYTE *mod, ULONG len, char *err, LONG errmax);  /* mod: AllocVec, taken */
BOOL ep_start(UWORD *count);    /* TRUE: AmigaMCE's timer calls ep_tick, every *count */
void ep_tick(void);
void ep_stop(void);
void ep_pause(BOOL on);
void ep_unload(void);
BOOL ep_loaded(void);
BOOL ep_own_timer(void);
BOOL ep_song_ended(void);
void ep_set_volume(int v);
BOOL ep_step_position(int d);
BOOL ep_can_step(int d);
BOOL ep_subsong(int n);
void ep_info(struct EpInfo *in);
/* A Paula voice as an EaglePlayer player last set it (its UPS, or its
 * PokeAdr/PokeLen/PokePer), for the spectrum: a note started (trig) at
 * adr, len words, then its loop. */
struct EpVoice {
    const BYTE *adr, *loop;
    UWORD len, replen, per;
    UBYTE vol;
};
/* hit, vol: the meters'; v (or NULL): where the voices are, trig the ones
 * whose notes started since the last call. FALSE: the player says nothing
 * of where its samples are (no UPS, no PokeAdr). */
BOOL ep_levels(UBYTE *hit, UBYTE *vol, struct EpVoice *v, UBYTE *trig);

/* ---------------------------------------------------------------- loader */
/* Work for the loader process (loader.c): a fact sheet and screenshot, a
 * picture, a folder of pictures (its first picture) or an AGS folder's
 * entries, read off the UI's back. The UI owns the job; it comes back marked done. */
#define LJ_FACTS 24
enum { LJ_ENTRY = 1, LJ_PICTURE, LJ_PICFOLDER, LJ_SHOT, LJ_FOLDER, LJ_PICDIR, LJ_WRITE, LJ_SONG,
       LJ_QUIT };              /* LJ_SONG: a music file's (or folder's) details, picture in first */
enum { SHOT_AGS = 1, SHOT_FILE, SHOT_FOLDER, SHOT_SONG };  /* LJ_SHOT: path is an entry base, a
                                 * picture, a folder, a music file or folder (its picture) */
struct LJob {
    struct Message msg;
    UBYTE type;
    volatile UBYTE cancel;      /* not wanted any more: skip it if not started */
    UBYTE remap;                /* decode remapped into scr (true-colour screens) */
    UBYTE want_pic;             /* decode the picture too */
    UBYTE mode;                 /* LJ_SHOT: SHOT_* */
    volatile UBYTE busy, done;
    UBYTE ran;                  /* the loader did it (not skipped) */
    UBYTE orphan;               /* nobody waits for it: freed when back (ldr_orphan) */
    UBYTE spent;                /* its picture was handed to the screen: read again to reuse */
    ULONG stamp;                /* last used (the UI's cache order) */
    struct Screen *scr;
    char path[300];             /* entry base, picture or folder */
    /* results */
    APTR obj;                   /* the decoded picture (gfx_picture_obj), or NULL */
    UWORD tw, th, maxcol;       /* LJ_SHOT on a colour-table screen: made into a */
    UBYTE tall, viewfit;        /*   thumbnail for a tw x th box (pic_fit; viewfit:
                                 *   the viewer's screen, view_fit, and only if it
                                 *   has more colours than maxcol + 1), */
    struct Thumb *thumb;        /*   obj then NULL */
    ULONG ms_decode, ms_thumb;  /* how long each took */
    ULONG ms_parts[4];          /* ...the thumbnail's: reading, counting, choosing, mapping */
    UBYTE own;                  /* read by ilbm.c, not a datatype */
    UBYTE urgent;               /* the UI waits for it: run above the read-ahead */
    ULONG t_sent, t_begun;      /* when it was sent, when the loader began it */
    BOOL remapped;
    struct PicInfo info;
    LONG nlines;                /* LJ_ENTRY: the fact sheet (np: 1 if it has a screenshot) */
    char lines[LJ_FACTS][96];
    LONG size, days;            /* LJ_PICTURE: file size, date (DateStamp days) */
    LONG np, nd;                /* LJ_PICFOLDER: pictures, folders; its first picture */
    char first[300];
    UBYTE adult;                /* LJ_FOLDER: an AGS folder's entries, sorted, into
                                 * list (LJ_PICDIR: a Pictures folder's, pics.c) */
    struct ItemList list;       /* list (its pool then the caller's); np: count, -1 */
    char cache[300];            /*   from the index cache when cache[0] (off, len), */
    ULONG off, len;             /*   else from the folder, and its date: */
    struct DateStamp date;
    APTR buf;                   /* LJ_WRITE: len bytes into the file path */
};
BOOL ldr_start(void);
void ldr_stop(void);            /* no job may be out */
void ldr_submit(struct LJob *j);
void ldr_submit_first(struct LJob *j);  /* ahead of the queue (or moved there) */
struct LJob *ldr_collect(void); /* one that came back (now done), or NULL */
void ldr_wait(struct LJob *j);  /* until j is back */
void ldr_orphan(struct LJob *j);    /* give it up: freed (results and all) when back */
ULONG ldr_sigmask(void);        /* signalled when one comes back */
LONG ldr_out(void);             /* jobs out now */
void pic_dispose(APTR obj);
/* System Information: which datatype reads each picture format (a line each). */
void pic_datatypes(void (*out)(void *ctx, const char *line), void *ctx);

/* ---------------------------------------------------------------- textbox */
struct TextBox {
    BOOL  open, first_white, dragging, drawn;
    BOOL  focused;              /* the keyboard scrolls it: thumb shown brighter */
    BOOL  jump;                 /* scrolls go straight there, no glide (tb_open) */
    WORD  x, y, w, h;           /* text area on screen */
    WORD  bar_x, bar_w;         /* scroll bar column; bar_w 0 when all text fits */
    WORD  line_h, drag_off;
    UBYTE mask;                 /* bitplanes the copy touches */
    LONG  content_h;            /* pixels of text */
    LONG  pos, target;          /* scroll offset in pixels, now and heading to */
    LONG  drawn_pos, thumb_y, thumb_h;
    const struct ItemList *lines;
    struct BitMap *bm;          /* pre-rendered text, 3 planes (all with a picture) */
    struct BitMap *pic;         /* a picture under the text (the caller's; set it
                                 * before tb_open), like the screen's, pic_w x pic_h */
    WORD  pic_w, pic_h, pic_y;  /* pic_y: its top in the content (tb_open) */
    WORD  pic_a, pic_z;         /* the screen rows it covers now (pic_a > pic_z: none) */
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
LONG st_busy_sample(void);         /* stream.c: the radio process, % busy (debug) */
ULONG time_ms(void);                /* E-clock milliseconds, 0 without a timer */
void  dbg_sleep_begin(void);        /* around every Wait / WaitTOF */
void  dbg_sleep_end(void);
LONG  dbg_app_sample(void);         /* AmigaMCE's own CPU share, 0..100, -1 at first */
void  dbg_mem_baseline(void);
ULONG dbg_mem_used(ULONG *chip);
void amce_log(const char *what, const char *detail);
const char *fmt_khz(ULONG hz, char *out);    /* "44.1kHz" (out: 16 bytes) */
void amce_log_close(void);

/* ---------------------------------------------------------------- index */
/* The index cache of long lists (index.c). */
struct IdxLetter {
    struct DateStamp date;      /* its folder's date when it was read */
    LONG  count;
    ULONG off, len;             /* its block in the file; len 0: only the count */
    char  name[56];             /* its folder's name */
};
struct Idx { LONG n; struct IdxLetter *l; };
struct IdxOut { UBYTE *buf; LONG len, cap, n; };
void idx_name(const char *menu, BOOL adult, char *out, LONG max);   /* its file */
BOOL idx_date(const char *dir, struct DateStamp *ds);
BOOL idx_load(const char *file, struct Idx *ix);    /* the table only */
void idx_free(struct Idx *ix);
LONG idx_read_block(const char *file, ULONG off, ULONG len, const char *dir,
                    struct ItemList *out);          /* a letter's entries; -1 on error */
BOOL idx_begin(struct IdxOut *o, LONG letters);
BOOL idx_letter(struct IdxOut *o, LONG k, const struct DateStamp *date, const char *name,
                LONG count, const struct Item *items, const UBYTE *block, ULONG blen);
void idx_abandon(struct IdxOut *o);
BOOL idx_save(const char *file, const UBYTE *buf, LONG len);
UBYTE *idx_block(const char *file, ULONG off, ULONG len);  /* FreeVec it */

/* ---------------------------------------------------------------- dirscan */
/* Every entry of dir, a buffer full per file-system call (ExAll); fn is
 * given each name and whether it is a folder. -1 if dir cannot be read. */
typedef void (*DirFn)(void *ctx, const char *name, BOOL isdir);
LONG dir_scan(const char *dir, DirFn fn, void *ctx);

/* ---------------------------------------------------------------- util */
int  amce_stricmp(const char *a, const char *b);
int  amce_strnicmp(const char *a, const char *b, LONG n);

/* ---------------------------------------------------------------- network
 * net.c: a process of its own does HTTP requests (bsdsocket.library);
 * the UI sends a job and collects it when its signal comes. */
struct NetJob {
    struct Message msg;
    volatile UBYTE cancel;      /* given up: it stops at its next wait */
    UBYTE busy, done, quit, orphan;
    UBYTE gdm;                  /* not a request: Plex servers looked for on the network */
    UWORD port;                 /* 0: 80 */
    char host[80];              /* "" the radio-browser.info API (found by DNS) */
    char path[400];
    ULONG seq;                  /* the UI's, to know whose answer it is */
    /* results */
    LONG status;                /* HTTP status (200), 0 none */
    char err[96];               /* why not, "" if fine */
    UBYTE *body;                /* AllocVec'd, 0-terminated, len bytes; NULL on error */
    ULONG len, ms;
    char server[64];            /* the server that answered */
    /* a Plex list (parse set): read into list in the network process, so
     * the UI goes on (its spinner turning) meanwhile; radio_done takes it */
    UBYTE parse, parsed;
    char pkey[300];             /* the key it was asked by */
    struct ItemList list;
    LONG n, total;              /* entries read (-1 not Plex's XML), the server's total */
    LONG start;                 /* a long list's later part: from this entry (0 its first) */
    ULONG ms_read;
};
BOOL net_start(void);
void net_stop(void);            /* waits for jobs out (cancel them first) */
void net_submit(struct NetJob *j);
struct NetJob *net_collect(void);
void net_cancel(struct NetJob *j);
/* The network interfaces (Settings > Networking): the stack's own list
 * (Roadshow's interface API, else SIOCGIFCONF), without loopback. */
struct NetIf {
    char  name[20];
    char  dev[40];              /* its SANA-II device (Roadshow), "" not known */
    LONG  unit;
    ULONG ip;                   /* 0: no address yet */
    BOOL  up;
};
int net_interfaces(struct NetIf *out, int max, const char *toward, ULONG *in_use);
                                /* -1: the stack lists none; in_use: the address used
                                 * toward that host (a UDP connect: nothing is sent) */
struct NetMirror { char name[64]; ULONG addr; LONG ms; };
int net_radio_mirrors(struct NetMirror *out, int max);  /* -1: not found (DNS) */
void net_radio_server(const char *name);    /* the station-list server asked first; "" any */
void net_bind_to(ULONG ip);     /* connections from this address; 0 the stack's choice */
ULONG net_sigmask(void);

/* radio.c: radio-browser.info's lists into pages */
#define RADIO_TAGS      "#tags"
#define RADIO_COUNTRIES "#countries"
void radio_request(const char *query, BOOL https, char *out, LONG max);
void radio_set_max_kbps(UWORD kbps);    /* the highest bitrate listed; 0 any */
void radio_set_home(const char *iso);   /* this Amiga's country, first in Countries */
LONG radio_fill(const char *query, UBYTE *body, ULONG len, struct ItemList *l, BOOL https,
                LONG *skipped);
char *radio_field(const struct Item *it, int i, char *out, LONG max);

/* plex.c: a Plex Media Server's music */
struct PlexConf {
    char host[64];              /* "" none chosen */
    UWORD port;
    char name[64], token[64], cid[40];
    BOOL convert;               /* every track converted for this Amiga by the server */
    BOOL on;                    /* Settings > Plex */
    UWORD cpu;                  /* caps.cpu (plex_load) */
    BOOL flac;                  /* flacdec.library installed: FLAC plays as stored too */
    BOOL mhi;                   /* MP3 by an MHI driver: no decoding here, any quality */
    char library[16];           /* the music library used ("" not known yet) */
    UWORD libraries;            /*   of how many on the server */
};
extern struct PlexConf plex;
void plex_load(UWORD cpu);
void plex_set_server(const char *host, UWORD port, const char *name);
void plex_request(const char *key, char *out, LONG max);
void plex_request_from(const char *key, LONG start, char *out, LONG max);
void plex_track_url(const struct Item *it, UWORD cpu, ULONG from_ms, char *out, LONG max);
ULONG plex_track_byte(const struct Item *it, ULONG from_ms);
BOOL plex_converts(const struct Item *it);
void plex_set_convert(BOOL on);
void plex_set_on(BOOL on);
void plex_set_library(const char *key, UWORD count);
const char *plex_photo_libs(void);
void plex_set_photo_libs(const char *v);
BOOL plex_photo_sections(const char *key);
int plex_tier(void);
UWORD plex_decoder_cpu(void);
void plex_section_menu(const char *sec, struct ItemList *l);
BOOL plex_summary(UBYTE *body, ULONG len, char *out, LONG max);  /* an item's own answer */
LONG plex_fill(const char *key, UBYTE *body, ULONG len, struct ItemList *l, LONG *total);
LONG plex_fill_servers(UBYTE *body, ULONG len, struct ItemList *l);
const char *plex_why(const struct Item *it);
ULONG plex_stream_rate(UBYTE *body, ULONG len);
BOOL plex_art(const char *thumb, WORD w, WORD h, char *req, LONG rmax, char *file, LONG fmax);
BOOL plex_art_save(const char *file, const UBYTE *body, ULONG len);
void amce_strlcpy(char *d, const char *s, LONG n);
void amce_strlcat(char *d, const char *s, LONG n);

#endif
