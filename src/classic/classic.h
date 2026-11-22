/*
 * Ambiance - the program and its built-in view, the classic layout
 * Copyright (c) 2026 Brian King
 *
 * Step 3 of "Classic as a layout" (claude/classic-as-layout.md): the
 * classic's drawing lives in src/classic/, behind the view table (struct
 * UiView) and the entry details' view (struct DetView, details.h), as a
 * library layout lives behind struct LayoutEngine. This is what the two
 * sides share: the program's state the classic reads (the page stack, the
 * focus, the menu, the message, Now playing's state, the big letter...),
 * the program's functions it calls, and what the classic gives the program
 * (its tables). Nothing here is for the library layouts: theirs is
 * layout.h.
 */
#ifndef CLASSIC_H
#define CLASSIC_H

#include "../amb.h"
#include "../radiocsv.h"
#include "../plexxml.h"
#include "../iconify.h"
#include "../vprobe.h"
#include "../video.h"
#include "../glyph.h"
#include "../dvdplay.h"
#include "../dvddisc.h"
#include "../dvdiso.h"
#include "../igame.h"
#include "../unarc.h"
#include "../whdinst.h"
#include "../lhost.h"
#include "../lkit.h"
#include "../app.h"
#include "../details.h"
#include "../lvhost.h"
#include "../copper.h"

#define MAXDEPTH 12
#include <stdio.h>
#include <string.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <devices/inputevent.h>
#include <dos/dostags.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/lowlevel.h>
#include <proto/locale.h>
#include <proto/asl.h>
#include <libraries/asl.h>
#include <workbench/startup.h>
#include <proto/icon.h>
#include <libraries/gadtools.h>

/* ------------------------------------------------------------ modules */
enum { MOD_GAMES, MOD_MODS, MOD_DEMOS, MOD_VIDEOS, MOD_PICTURES, MOD_MUSIC,
       MOD_RADIO, MOD_SETTINGS, MOD_ABOUT,
       MOD_NOWPLAYING,      /* the menu's first entry while something plays (or is paused) */
       MOD_COUNT };

enum { FOCUS_MENU, FOCUS_CONTENT,
       FOCUS_PREVIEW };     /* the preview pane's fact sheet: Up/Down scroll it */

#define FACT_LINES 24

/* The view driven (Classic's, or a layout's): what the program asks to be
 * shown again. The program's own state (where the list is scrolled, the
 * details asked for) is done before, in the program's functions of the
 * same names (draw_pane...); the view only shows, or notes for its next
 * frame. Classic's: the cl_ functions; a layout's: the ly_ ones. */
struct UiView {
    void (*header)(void);
    void (*menu)(void);
    void (*menu_row)(int i);
    void (*footer)(void);
    void (*title)(struct Page *p);
    void (*title_count)(struct Page *p);
    void (*list_row)(struct Page *p, LONG idx, WORD col);
    void (*gutter_planes)(struct Page *p, UBYTE planes);
    void (*list_rows)(struct Page *p);
    void (*list_move)(struct Page *p, LONG old_sel, LONG old_top);
    void (*text_picture)(struct Page *p);
    void (*pane)(BOOL full);
    void (*np)(BOOL full);
    void (*all)(void);
    void (*pane_preview)(BOOL first);
    /* program logic's calls into the view (step 3a): each turn of the main
     * loop (the spinners), a busy wait's spinner, the header's readouts,
     * the big letter, Now playing's parts. The classic draws them; a
     * layout is told (a note) or has nothing to do. */
    void (*tick)(void);
    void (*busy)(ULONG now);
    void (*header_np)(BOOL quiet, BOOL on);
    void (*clock)(void);
    void (*status)(const char *show);       /* NULL: the Debug line off */
    void (*letter_show)(struct Page *p);
    void (*letter_relabel)(char c);
    void (*letter_hide)(struct Page *p);
    void (*np_station)(void);
    void (*eq_tick)(void);
    void (*np_visual)(void);
    void (*np_art)(void);
    /* step 3c: the rest of program logic's drawing (a page left, Now
     * playing's time and bitrate, the cycle gadget's hover, the room for
     * its cover, the menu's slide, a Plex summary come) */
    void (*page_leave)(struct Page *gone, struct Page *under);
    void (*np_update)(void);
    void (*np_rate)(void);                  /* the station's lines again, over themselves */
    void (*np_cycle)(void);
    void (*np_art_room)(BOOL none);
    void (*menu_slide)(UWORD from, UWORD to);
    void (*pane_summary)(const char *text);
    /* step 4: a mouse event on the screen (Classic's hit tests: input.c;
     * a layout's: lv_mouse) */
    void (*mouse)(struct IntuiMessage *im);
    void (*wheel)(LONG d, WORD x, WORD y);  /* a wheel notch under the pointer (a layout: lv_key's) */
    void (*letter_place)(void);             /* the big letter (ov.bl) placed on the view's screen */
    void (*readouts)(BOOL clock, BOOL status);  /* once a second: the header's clock, Debug line */
    /* the engine: its name (the log), and its entry details' view, whose
     * shots NULL means it asks for its own pictures, at its own sizes */
    const char *name;
    const struct DetView *details;
};

/* Design units (a 256-line layout) to pixels on this screen. */
#define U(v) GU(&g, v)
/* A lowres-narrow display, for the layout's choices (the letter column,
 * the gutter, the screenshot, the cycle gadget's label). A screen tests its
 * pixels, as ever: lowres is the one under 640 (a laced screen in a large
 * font is as few design units across, but its pixels are fine ones). A
 * window, whose pixels no longer say how much fits, tests design units:
 * 512 across at any text size, 480 at the least. */
#define DW ((WORD)((LONG)g.w * 256 / g.k))
#define NARROW (g.wb ? DW < 400 : g.w < 640)

/* ---- the spinner: eight round dots on a circle, the bright one going
 * round with a fading tail, and "Loading..." under it. In the details pane
 * while its picture comes, and over the content pane for any navigation
 * that keeps you waiting (busy_*), after SPIN_AFTER ms. */
#define SPIN_AFTER 200
#define SPIN_FRAME  80              /* ms a step: a turn in 0.64 s */

/* Pictures of the server's (an album's cover): asked for (art_fetch), kept
 * in RAM: (plex_art), then made for their place by the loader */
/* AHEAD: a Plex list's neighbours' pictures, fetched while the selection
 * rests, so moving to one shows it at once (plex_ahead) */
enum { ART_NP, ART_PV, ART_VW, ART_VWNEXT, ART_AHEAD, ART_LAYOUT,
       ART_KINDS };   /* VW: the viewer's photo, and its next; LAYOUT: a layout's covers */

/* Where the music playing was started: its section (MOD_*), the rows
 * chosen down to its list, and that list's title. Now playing opened from
 * another section is a page on its own there, "Now playing" and where it
 * is from, a link that goes there; Esc goes back where you were. */
#define CRUMB_SRC MAXDEPTH          /* crumb_at/crumb_hover: that link */

/* ---- a long list's column at its right edge: a scroll bar, always, from
 * the moment the list is shown and whatever moves it (keyboard, joystick,
 * pad or mouse): its thumb says how long the list is and where in it this
 * is. The mouse can click above or below the thumb to page, or drag it;
 * while the mouse is in use the thumb is bright, at rest it is dimmer. */
/* the column: gap, bar, gap; on a lowres screen (320 wide, its pixels
 * twice as wide as hires ones) narrower, 1 + 3 + 1, for the rows' room */
#define GUT_LO  NARROW
#define GUT_W   (GUT_LO ? 5 : U(9))
#define GUT_GAP (GUT_LO ? 1 : U(2))
#define GUT_BAR (GUT_LO ? 3 : U(5))

enum { NB_PREV, NB_BACK, NB_PLAY, NB_STOP, NB_FWD, NB_NEXT, NB_LOOP, NB_MUTE, NB_VDOWN, NB_VUP,
       NB_COUNT };

#define NB_SLIDER  NB_COUNT         /* np.hov: the slider */
enum { REP_MOUSE, REP_JOY };       /* who holds a Now playing button (np.rep_src) */
#define NB_CYCLE   (NB_COUNT + 1)   /* np.hov: the visualization's cycle gadget */

/* ---- an artist's biography, an album's review: left out of a library's
 * long lists (excludeFields), so asked for alone when the pane shows one,
 * and kept for the last few shown */
#define SUM_KEPT 8

struct Overlay {
    BOOL  on;
    struct Page *page;
    char  letter;
    const struct BigLetter *bl;     /* gfx.c owns it */
    WORD  x, y;
    WORD  strip_x;          /* left edge of the letter's strip; rows stop before it */
    UWORD linger;           /* timer ticks left before hiding, 0 = not counting */
};

struct Busy { int depth; BOOL on; ULONG since, last; UWORD phase; WORD cx, cy; };

struct PlexOrig { char codec[16]; ULONG kbps, ch, rate; BOOL conv; };

struct CrumbAt { WORD x0, x1; };

struct Gutter { BOOL drag; WORD off; };

struct OnScreen { char key[300]; WORD x, y, w, h; };

struct SumKept { char key[120]; char text[300]; };

struct NowPlaying {
    struct ItemList list;       /* the playlist: the modules of the folder played from */
    LONG idx;                   /* the one loaded */
    char folder[64];
    int btn;                    /* the button chosen with the keyboard or joystick */
    BOOL ended_seen;            /* the song's end acted on */
    /* where things are on the page */
    WORD x0, w, title_y, info_y, prog_y, prog_h, time_y, meter_y, meter_h, name_w;
    WORD bx[NB_COUNT], by, bw, bh, vol_x, samples_y;
    WORD art_x, art_y, art_s;               /* a Plex track's cover: its square (0 none) */
    WORD art_h;                             /* its rows (half on hires: tall pixels) */
    WORD bar_w, pos_y;                      /* the bar's width (the time right of it); the
                                             * position line, under the buttons */
    LONG seek_to;                           /* a track's seek waiting (ms, -1 none) */
    BOOL seeked;                            /* the track started again part way (a seek) */
    ULONG seek_due;
    ULONG fail_due;                         /* a track that failed: the next from then (time_ms), 0 none */
    /* what is drawn there now: only changes are drawn again */
    LONG s_ms, s_total, s_order, s_row, s_fill;
    int s_state, s_vol, s_btn, s_flash, s_steps, s_loop;
    BOOL s_focus;
    int flash;                  /* the button just pressed (-1 none): lit briefly */
    ULONG flash_until;
    int hov, s_hov;             /* what the mouse is over: a button, NB_SLIDER, -1 */
    int rep, rep_src, s_rep;    /* the button held down, repeating (-1 none) */
    ULONG rep_next;
    BOOL sl_on, sl_drag, s_slhot; /* the volume slider (when there is room) */
    WORD sl_x0, sl_x1;
    int rows;                   /* the meters' rows laid out for: the page as tall as they need */
    WORD cyc_x0, cyc_x1, cyc_y0, cyc_y1;    /* the visualization's cycle gadget (V) */
    BOOL cyc_off;               /* ...greyed: one only can be drawn for this music (MHI) */
    BOOL cyc_label;             /* ...with its name (not on lowres: the glyph alone) */
    ULONG s_rseq;               /* a station: its title and state drawn */
    int s_rstate;
};

/* ---- the program's, as the classic reads them (main.c) */
BOOL art_fetch(int kind, const char *thumb, WORD w, WORD h, char *file);
extern struct NetJob *art_nj[ART_KINDS];
extern WORD aside_w;
WORD aside_width(void);
WORD aside_x(void);
extern BOOL bar_up;
BOOL ch_reading(const struct Page *p);
WORD clock_left(void);
void clock_text(char *buf);
extern int crumb_first;
extern int crumb_hover;
BOOL cv_has_pane(struct Page *p);
void cv_job(struct LJob *j);
BOOL date_ymd(void);
extern int depth;
void draw_clock(void);
void draw_footer(void);
void draw_gutter(struct Page *p);
void draw_gutter_planes(struct Page *p, UBYTE planes);
void draw_header(void);
void draw_list_row(struct Page *p, LONG idx, WORD col);
void draw_list_rows(struct Page *p);
void draw_menu(void);
void draw_menu_row(int i);
void draw_pane(BOOL full);
void draw_status(void);
void draw_title(struct Page *p);
void draw_title_count(struct Page *p);
extern char fact_buf[FACT_LINES][96];
extern int focus;
extern BOOL foot_hidden, foot_over, foot_redraw;
extern ULONG foot_since;
extern struct Gfx g;
void gut_geom(struct Page *p, WORD *bx0, WORD *bx1, WORD *ty, WORD *th);
BOOL has_aside(struct Page *p);
extern BOOL hdr_np_on;
extern const char *hint_shown;
const char *hint_text(void);
BOOL hold_active(void);
extern WORD hover_menu;
extern WORD hover_row;
extern struct IgCat igcat;
BOOL is_dvd_image(const struct Item *it);
BOOL is_dvd_row(const struct Item *it);
WORD list_edge(struct Page *p);
BOOL list_follow(struct Page *p);
BOOL list_overflows(struct Page *p);
WORD list_visible(void);
WORD list_xr(struct Page *p);
extern BOOL lv_playing_hit;
WORD mark_column(const struct Page *p);
extern int menu[MOD_COUNT], nmenu, menu_sel;
extern ULONG menu_draw_ms;
extern const char *mod_names[MOD_COUNT];
extern const char *const mon_abbr[];
extern BOOL mouse_mode;
extern BOOL np_local_art, np_art_want;
int np_meter_rows(void);
extern char msg[160];
extern struct LJob *np_aj;
extern BOOL np_art_drawn;
extern char np_art_file[120];
void np_art_load(const char *file);
BOOL np_away(void);
void np_draw(BOOL full);
BOOL np_focus_shown(void);
WORD np_knob_w(void);
void np_layout(void);
BOOL np_list_on(void);
BOOL np_local(void);
BOOL np_muted(void);
BOOL np_on(void);
BOOL np_row(const struct Item *it);
extern int np_src;
extern char np_src_list[64];
extern WORD np_src_x0, np_src_x1;
int np_state(void);
void np_update(void);
void open_textbox(struct Page *p, WORD y0);
extern WORD ov_bw, ov_bh;
extern struct BitMap *ov_bm;
void ov_bm_free(void);
BOOL ov_build(char ch);
extern struct Page pages[MAXDEPTH];
extern BOOL pane_pending;
void pane_settle(void);
BOOL pic_take(const char *file, WORD x, WORD y, WORD w, WORD h);
extern BOOL play_loop;
WORD plex_art_side(void);
int plex_colours(void);
BOOL plex_movable(struct Page *p, LONG idx);
BOOL plex_pv_src(struct Page *p, LONG idx, struct Item *out);
extern char plex_shown[260];
extern struct NetJob *plex_sj;
WORD plexmark_h(void);
void preview_draw(struct Page *p);
extern WORD pv_box_y, pv_box_h;
extern int pv_colour_cap;
extern struct ItemList pv_list;
BOOL pv_pictures(void);
extern struct TextBox pv_tb;
void radio_done(struct NetJob *j);
BOOL radio_waiting(void);
const char *row_tag(const struct Page *p, const struct Item *it);
void set_msg(const char *m);
extern WORD spin_bw, spin_bh;
extern struct BitMap *spin_bm;
void spin_free(void);
extern struct TextFont *spv_font;
extern int spv_idx;
extern char status_shown[96];
BOOL sum_key(const struct Item *it, char *out, LONG max);
extern char sum_want[120];
extern WORD sum_y;
WORD text_box_h(struct Page *p, WORD y0, WORD *note_lines);
WORD textbox_w(void);
extern const struct UiView *ui;
void wrap_into(struct ItemList *l, const char *s, WORD maxw);
struct Page *cur(void);
extern struct Overlay ov;
extern struct Busy busy;
extern struct PlexOrig plex_orig;
extern struct CrumbAt crumb[MAXDEPTH];
extern struct Gutter gut;
extern struct OnScreen pv_onscr;
extern struct SumKept sum_kept[SUM_KEPT];
extern struct NowPlaying np;

void draw_list_move(struct Page *p, LONG old_sel, LONG old_top);

void draw_all(void);

void pane_preview(BOOL first);

/* ---- the classic's, as the program drives it (classic.c) */
extern const struct UiView classic_ui, layout_ui;
extern const struct DetView classic_view;
/* The view drives the screen: Classic (classic_ui) or the library layout
 * shown (layout_ui). ui is set only by view_use. */
void view_use(const struct UiView *v);
void cl_mouse(struct IntuiMessage *im);        /* (input.c) */
void cl_wheel(LONG d, WORD x, WORD y);         /* (input.c) */
BOOL section_inert(void);
/* (step 4: what Classic's input reads and calls of the program's) */
void act_back(void);
void act_move(LONG d);
void act_open(BOOL play);
void ch_wait_item(struct Page *p, LONG idx, BOOL shown);
BOOL coast_interrupt(void);
struct TextBox *cur_tb(void);
ULONG dc_time(void);
void disarm(void);
BOOL is_heading(struct Page *p, LONG i);
BOOL menu_layout(void);
void menu_select_now(int i, BOOL now);
void menu_unpin(void);
void nav_moved(void);
void next_visual(void);
void np_goto_source(void);
void np_press(int b);
void np_rep_start(int b, int src);
void np_rep_stop(void);
void np_seek_part(ULONG n, ULONG d);
void np_set_volume(int v);
void open_dir(struct Item *it);
char ov_letter_in(const struct Item *it, LONG n, LONG i);
void ov_relabel(char c);
void page_pop(void);
void pointer_show(void);
void set_focus(int f);
void set_mouse_mode(BOOL on);
extern BOOL debug_on;
extern BOOL hover_strip;
extern ULONG mouse_last;
void hover_set(WORD row, WORD mi);
int np_button_at(WORD x, WORD y);
#define view_own_pictures() (!ui->details->shots)

#endif
