/*
 * Ambiance - main program: Home Screen, navigation, input and launching
 * Copyright (c) 2026 Brian King
 */
#include "zz9k.h"
#include "amb.h"
#include "radiocsv.h"
#include "plexxml.h"
#include "jfjson.h"
#include "iconify.h"
#include "vprobe.h"
#include "video.h"
#include "glyph.h"
#include "dvdplay.h"
#include "dvddisc.h"
#include "dvdiso.h"
#include "igame.h"
#include "unarc.h"
#include "whdinst.h"
#include "lhost.h"
#include "lkit.h"
#include "app.h"
#include "classic/classic.h"
#include "details.h"
#include "lvhost.h"
#include "copper.h"

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

static const char version_tag[] __attribute__((used)) =
    "$VER: Ambiance " AMB_VERSION " (" AMB_DATE ")";

struct IntuitionBase *IntuitionBase;
struct Library *AslBase;            /* opened for Settings > Music folders' requester */
struct GfxBase *GfxBase;
struct Library *DiskfontBase, *DataTypesBase, *LowLevelBase, *LayersBase;

const char *mod_names[MOD_COUNT] = {
    N_("Games"), N_("Mods"), N_("Demos"), N_("Videos"), N_("Pictures"), N_("Music"), N_("Radio"),
    N_("Settings"), N_("About"), N_("Now playing")
};


static void ch_free(struct Chunks *ch);

const struct UiView *ui = &classic_ui;

#define SLIDE_MS  5000          /* slideshow: each picture this long */
#define BAR_MS    3000          /* the viewer's info bar, after a change */

struct Gfx g;
static struct Caps caps;
static struct Ags ags;
struct IgCat igcat;      /* iGame's list (igame.c): igcat.n 0 when none */
static char ig_setting[256];     /* Ambiance/iGame as read: "" unset, "none", or a drawer */
static BOOL ig_off(void);
struct Page pages[MAXDEPTH];
int depth;
int menu[MOD_COUNT], nmenu, menu_sel;
int focus = FOCUS_MENU;
static BOOL plex_shortcut_fix(struct Page *p);  /* a list's Now playing row, as playback is */
static BOOL vw_active;          /* the viewer is up: its list's rows keep their places */
static BOOL running = TRUE, quit_armed;
char msg[160];
char fact_buf[FACT_LINES][96];
static LONG last_minute = -1;
static int screen_mode = SM_AUTO;   /* as chosen in Settings, may be SM_AUTO */
static int ui_style = STYLE_PIXEL;  /* Settings > Style (smooth only on RTG screens) */
BOOL pane_pending;           /* menu moved, its section not built yet (see pane_settle) */
struct TextBox pv_tb;        /* the preview pane's fact sheet (scrolls) */
struct ItemList pv_list;     /* its wrapped lines */
BOOL mouse_mode;             /* the mouse was used last */
/* Mouse mode: the row under the pointer, highlighted softly (PEN_RULE;
 * the selection stays where it is until a click). Kept as a place on
 * screen, so a list scrolling under a still pointer stays right. */
WORD hover_row = -1;         /* content list: screen row, -1 none */
BOOL hover_strip;            /* the pointer in the menu's column (menu_layout) */
WORD hover_menu = -1;        /* main menu: entry, -1 none */
/* The keyboard or joystick moving: the pointer hides (an empty sprite)
 * and so does its highlight, until the mouse moves. */
static struct Window *ptr_hidden;   /* the window whose pointer is hidden, or NULL */
static UWORD *ptr_blank;            /* the empty pointer (chip RAM) */


/* Big-letter overlay on long content lists while they glide (see below). */
struct Overlay ov;
BOOL debug_on;               /* Settings > Debug: header CPU/memory readout and log */

/* IT_ACTION codes (struct Item.action) */
enum { ACT_SCREENMODES = 1, ACT_SYSINFO, ACT_CREDITS, ACT_DEBUG, ACT_STYLES,
       ACT_STYLE_PIXEL, ACT_STYLE_SMOOTH, ACT_LFONT0, ACT_LFONT_END = ACT_LFONT0 + LFONT_COUNT - 1, ACT_RFONT0, ACT_RFONT_END = ACT_RFONT0 + LFONT_COUNT - 1, ACT_JUMP, ACT_DBUF, ACT_VISUAL, ACT_ANIMSCR, ACT_ZZVIDEO, ACT_VOVERLAY, ACT_TRANS, ACT_NOWPLAYING, ACT_HTTPS, ACT_PLEXHELP, ACT_PLEXCONV, ACT_PLEXON, ACT_PLEXSET, ACT_SOUNDSET, ACT_SND_PAULA, ACT_SND_P14, ACT_SND_AHI, ACT_SND_MPEGA, ACT_SND_OWNMPEGA, ACT_SND_MHI, ACT_SND_MHI0, ACT_SND_MHI_END = ACT_SND_MHI0 + 5, ACT_SND_MQ0, ACT_SND_MQ_END = ACT_SND_MQ0 + 2, ACT_FULLVIS,
       ACT_GENSET, ACT_NETSET, ACT_SCREENSET, ACT_VIDEOSET, ACT_NICSET,
       ACT_RADIOSET, ACT_RKBPSSET, ACT_RSRVSET, ACT_BUFSET, ACT_RKBPS0, ACT_RKBPS_END = ACT_RKBPS0 + 5,
       ACT_BUF0, ACT_BUF_END = ACT_BUF0 + 2, ACT_RSRVAUTO, ACT_RSRVFAST, ACT_RSRV0,
       ACT_RSRV_END = ACT_RSRV0 + 7, ACT_NICAUTO, ACT_NIC0, ACT_NIC_END = ACT_NIC0 + 7,
       ACT_FOLDERS, ACT_FSSET0, ACT_FSSET_END = ACT_FSSET0 + 3, ACT_FSASSIGN, ACT_FSADD, ACT_FSDIR0,
       ACT_FSDIR_END = ACT_FSDIR0 + 7,
       ACT_PLEXSLIDES, ACT_LANGSET, ACT_LANGWB, ACT_LANGEN, ACT_LANG0, ACT_LANG_END = ACT_LANG0 + 15,
       ACT_PLEXLINK, ACT_PLEXOUT, ACT_WINDOW, ACT_TSIZES, ACT_FONTS, ACT_TSIZE0,
       ACT_TSIZE_END = ACT_TSIZE0 + TSIZE_COUNT - 1, ACT_COLOURS, ACT_COLOUR0,
       ACT_COLOUR_END = ACT_COLOUR0 + UI_SCHEMES - 1, ACT_GOPLEX,
       ACT_IGSET, ACT_IGFIND, ACT_IGCHOOSE, ACT_IGNONE, ACT_WHDPAGE, ACT_WHDGO, ACT_WHDNO,
       ACT_LAYOUTS, ACT_LAYOUT0, ACT_LAYOUT_END = ACT_LAYOUT0 + 8, ACT_LVNATIVE,    /* Classic + LV_MAX */
       ACT_FULLVISHELP, ACT_DEPTHS, ACT_DEPTH16, ACT_DEPTH32,
       ACT_JFSET, ACT_JFON, ACT_JFFIND, ACT_JFADDR, ACT_JFSIGNIN, ACT_JFOUT, ACT_GOJF,
       ACT_JFVSIZE, ACT_JFSRV0, ACT_JFSRV_END = ACT_JFSRV0 + 7, ACT_JFHELP, ACT_ENTRY,  /* Jellyfin (jellyfin.c) */
       ACT_COUNT,
       /* + SM_*: well past the rest (at 16 it had come to share numbers with
        * Now playing, HTTPS and the Plex rows, as actions were added; moved
        * from 160 to 220 for Jellyfin's rows) */
       ACT_MODE0 = 220 };
typedef char act_mode0_clear[ACT_COUNT <= ACT_MODE0 && ACT_MODE0 + SM_COUNT <= 255 ? 1 : -1];

/* ------------------------------------------------------------ pages */
struct Page *cur(void) { return depth ? &pages[depth - 1] : NULL; }

void nav_moved(void);

static struct Page *page_push(UBYTE type, const char *title)
{
    nav_moved();
    ov.on = FALSE;
    tb_close(&pv_tb);
    det.shown = FALSE;
    det.tj = det.sj = NULL;
    if (focus == FOCUS_PREVIEW)
        focus = FOCUS_CONTENT;
    struct Page *p;
    if (depth >= MAXDEPTH)
        return NULL;
    p = &pages[depth];
    memset(p, 0, sizeof(*p));
    p->pic_y = -1;
    if (!list_init(&p->list))
        return NULL;
    p->type = type;
    amb_strlcpy(p->title, title, sizeof(p->title));
    depth++;
    return p;
}

WORD textbox_w(void);
BOOL has_aside(struct Page *p);
WORD aside_x(void);

/* A text page's picture (Page.pic), made for this screen: as wide as the
 * text and no taller than the box, in pens of its own (gfx_thumb_bitmap). */
static void page_picture(struct Page *p, WORD w, WORD h)
{
    struct PicInfo pi;
    struct Thumb *t;
    BOOL tall = gfx_mode_tall(g.mode);
    if (!gfx_thumbs(&g)) {
        amb_log("page picture", "this screen shows no pictures");
        return;
    }
    if (!(t = ilbm_thumb(p->pic, w, h, tall, FALSE, gfx_thumb_colours(&g), &pi)) &&
        !(t = jpeg_thumb(p->pic, w, h, tall, FALSE, gfx_thumb_colours(&g), &pi, "picture"))) {
        BOOL remapped = FALSE;
        APTR o = pic_decode_fit(p->pic, NULL, FALSE, &remapped, &pi, (ULONG)w, (ULONG)h);
        if (o) {
            ULONG dw, dh;
            pic_fit(pi.w, pi.h, tall, w, h, FALSE, &dw, &dh);    /* smaller or larger: to fit */
            t = thumb_make(o, dw, dh, gfx_thumb_colours(&g));
            pic_dispose(o);
        }
    }
    if (!t) {
        amb_log("page picture: could not be read", p->pic);
        return;
    }
    if ((p->pic_bm = gfx_thumb_bitmap(&g, t))) {
        char m[200];
        p->pic_w = t->w;
        p->pic_h = t->h;
        sprintf(m, "%s, %ld x %ld", p->pic, (long)t->w, (long)t->h);
        amb_log("page picture", m);
    } else {
        amb_log("page picture: no memory for its bitmap", p->pic);
    }
    thumb_free(t);
}

static void radio_page_gone(struct Page *p);
static void style_preview_free(void);
BOOL has_aside(struct Page *p);
WORD aside_x(void);
static void radio_stop(void);

static void plex_pos_save(const struct Page *p);

void page_pop(void)
{
    if (depth) {
        radio_page_gone(&pages[depth - 1]);     /* its request, if out, given up */
        plex_pos_save(&pages[depth - 1]);       /* a Plex list: where it was left */
    }
    ov.on = FALSE;
    tb_close(&pv_tb);
    det.shown = FALSE;
    det.tj = det.sj = NULL;
    if (focus == FOCUS_PREVIEW)
        focus = FOCUS_CONTENT;
    if (!depth)
        return;
    if (pages[depth - 1].style_pv)
        style_preview_free();
    ui->page_leave(&pages[depth - 1], depth >= 2 ? &pages[depth - 2] : NULL);
    pages[depth - 1].pic_y = -1;
    tb_close(&pages[depth - 1].tb);
    if (pages[depth - 1].pic_bm) {
        WaitBlit();
        FreeBitMap(pages[depth - 1].pic_bm);    /* its pens went with gfx_picture_clear */
        pages[depth - 1].pic_bm = NULL;
    }
    ch_free(pages[depth - 1].ch);
    pages[depth - 1].ch = NULL;
    list_free(&pages[--depth].list);
}

static void pages_clear(void)
{
    while (depth)
        page_pop();
}

/* ---- long AGS lists (All Games, menus split A-Z), a letter at a time.
 * Every entry has its place in the page's list from the moment its letter
 * folder is first read, so positions and the count never change; its
 * strings (name, path) live in that letter's own pool (struct Chunk). The
 * letters are read in order by the loader process (loader.c), the first
 * screens before the list is shown, the rest while it is used. With
 * memory to spare every letter stays (speed first). Short of memory, the
 * letters furthest from the selection let their strings go (ITEM_GONE:
 * the rows show blank, the label is the letter) and are read again before
 * they are reached: the ones on screen first, then the letters either
 * side of the selection, the one the list is moving towards first. */
#define CH_LOW   (512UL * 1024)     /* tight: drop letters below this much free */
#define CH_FLOOR (128UL * 1024)     /* stop reading new ones below this */
struct Chunk {
    char path[256];                 /* the letter folder */
    char letter[2];                 /* the label its rows show while gone */
    LONG first, count;              /* its entries' place in the list (first -1: unread) */
    struct ItemList names;          /* their strings, while held (pool) */
    BOOL held;
    struct DateStamp date;          /* its folder's, when read (for the index cache) */
    BOOL cached;                    /* its entries are in the index file, at: */
    ULONG coff, clen;
    ULONG noff, nlen;               /* where the file being written puts them */
};
struct Chunks {
    LONG n, next;                   /* next: the next letter to read the first time */
    struct Chunk *c;
    struct LJob *job;               /* the one being read (one at a time) */
    LONG job_k;
    struct LJob *wjob;              /* the index file being written */
    char idx[48];                   /* the index file (index.c) */
    BOOL adult, own, disorder, dropped, frozen, cut;
    BOOL laying;                    /* being laid out: nothing read on the side */
    BOOL dirty;                     /* a letter was read from its folder: save the index */
    ULONG t0;                       /* when it was opened (log) */
};

void set_msg(const char *m);

/* ---- the view driven: Classic (src/classic/) or a layout (lv.on), one at
 * a time, through ui (struct UiView, chosen by view_use). The program does
 * what it keeps (where a list is scrolled to, a text box's lines, what the
 * preview asks the loader for, the big letter's state), then calls the
 * view: Classic draws, a layout is told (lv_note) or has nothing to do.
 * main.c draws nothing of Classic's itself. gfx_off (set by view_use)
 * stays as a safety net: a classic drawing call that still comes under a
 * layout is not done, and is counted and logged ("unseen"). The text
 * boxes' glide (tb_step) is the one expected there. (The mini player and
 * Flow on the whole screen are modes of their own, with no layout up.) */
static BOOL lv_chosen(void);                /* a layout is chosen (on or not) */
/* Settings > Display > "Layouts on native screens (experimental)": a
 * layout on an 8-bit screen too (AGA, or a card's 8-bit mode), dithered
 * to a palette of its own (lk_open_chunky) */
static BOOL lv_native;
#define LVNATIVE_VAR "Ambiance/LayoutNative"
static void lv_native_changed(void);
static void lv_redraw(void);                /* the screen itself changed: all of it, shown whole */
static void lv_note(void);                  /* anything: the layout works out what */
static BOOL lp_open_all(void);              /* Home's launchpad (below): its "All" tile opened */
static BOOL lp_back(void);                  /* ...back from its list to the row */
static void lp_note_open(const struct Page *p); /* ...a file opened: a recent one */
static void lv_note_what(ULONG what);       /* LC_*: what it was (LC_SELECT, LC_DETAILS...) */
static void lv_who_report(void);

static BOOL np_full;                /* Flow on the whole screen: the page draws nothing */
WORD list_visible(void);
void draw_list_rows(struct Page *p);
void draw_title(struct Page *p);
void draw_title_count(struct Page *p);


BOOL has_aside(struct Page *p);
WORD aside_x(void);
void draw_pane(BOOL full);


/* Each step is drawn off screen, then copied up in one go with the beam
 * (gfx_blit_beam): cleared and drawn dot by dot on the screen, it was
 * seen half done, a flicker. */
struct BitMap *spin_bm;
WORD spin_bw, spin_bh;

void spin_free(void)
{
    if (spin_bm) {
        WaitBlit();
        FreeBitMap(spin_bm);
        spin_bm = NULL;
    }
}


BOOL radio_waiting(void)
{
    struct Page *p = cur();
    return p && p->type == PT_LIST && p->radio && p->net_seq && !pane_pending &&
           focus != FOCUS_MENU;
}


/* Navigation that may keep you waiting: busy_begin ... busy_end around it,
 * waits inside it through ui_wait. After SPIN_AFTER ms the spinner turns
 * in the middle of the content pane; at the end the pane is drawn again
 * where it was (redraw), unless what comes next covers the screen. */
struct Busy busy;
static BOOL ui_aborted;             /* a wait in busy gave way to input */
static ULONG ui_joy0;               /* the joystick when the busy began */

static void busy_begin(void)
{
    if (busy.depth++ == 0) {
        busy.on = FALSE;
        busy.since = time_ms();
        ui_aborted = FALSE;
        ui_joy0 = LowLevelBase ? gfx_joy(&g) : 0;
    }
}

static BOOL lv_busy;                /* a layout is drawing a busy wait's spinner */
static void lv_busy_frame(void);

/* a layout draws: its own small spinner, where the list goes */
static void ly_busy(ULONG now)
{
    if (!busy.on) {
        if ((LONG)(now - busy.since) < SPIN_AFTER)
            return;
        busy.on = TRUE;
        lv_busy = TRUE;
    }
    lv_busy_frame();
}

static void busy_tick(void)
{
    if (!busy.depth || !g.win)
        return;
    ui->busy(time_ms());
}


/* The Plex details on screen under the cover (their entry's name and
 * cover), or "" when that part of the pane was cleared. */
char plex_shown[260];

static BOOL busy_was_shown;         /* the last busy_end took a spinner off the screen */

/* A folder being read in this task (dirscan.c's dir_tick): the spinner
 * turns as its entries come, at most every 40 ms, so a slow one (on a
 * network share) shows it is working. */
static void busy_dir_tick(void)
{
    static ULONG last;
    ULONG now = time_ms();
    if (!busy.depth || (LONG)(now - last) < 40)
        return;
    last = now;
    busy_tick();
    gfx_present(&g);
}

static void busy_end(BOOL redraw)
{
    if (busy.depth && --busy.depth == 0 && busy.on) {
        busy.on = FALSE;
        lv_busy = FALSE;
        busy_was_shown = TRUE;
        plex_shown[0] = 0;                  /* the spinner may have been over them */
        if (redraw && g.win && cur())
            draw_pane(TRUE);                /* where the spinner was */
    }
}

/* Input that navigates is waiting (a key, a click, the wheel, the
 * joystick moved or pressed): what keeps you waiting gives way to it. It
 * stays queued, to be acted on; mouse moves and key releases meanwhile
 * are let go. */
static BOOL ui_input_waiting(void)
{
    struct MsgPort *mp = g.win ? g.win->UserPort : NULL;
    if (mp)
        for (;;) {
            struct IntuiMessage *im;
            BOOL nav;
            Forbid();
            im = (struct IntuiMessage *)mp->mp_MsgList.lh_Head;
            if (!im->ExecMessage.mn_Node.ln_Succ)
                im = NULL;
            nav = im && ((im->Class == IDCMP_RAWKEY && !(im->Code & IECODE_UP_PREFIX)) ||
                         im->Class == IDCMP_VANILLAKEY || im->Class == IDCMP_CLOSEWINDOW ||
                         (im->Class == IDCMP_MOUSEBUTTONS &&
                          (im->Code == SELECTDOWN || im->Code == MENUDOWN)));
            Permit();
            if (!im)
                break;
            if (nav)
                return TRUE;
            if ((im = (struct IntuiMessage *)GetMsg(mp))) {
                ULONG cl = im->Class;
                UWORD code = im->Code;
                ReplyMsg((struct Message *)im);
                gfx_window_msg(&g, cl, code);   /* a window uncovered: shown again */
            }
        }
    if (LowLevelBase) {
        ULONG st = gfx_joy(&g), mask = JPF_JOY_UP | JPF_JOY_DOWN | JPF_JOY_LEFT | JPF_JOY_RIGHT |
                                          JPF_BUTTON_RED | JPF_BUTTON_BLUE |
                                          JPF_BUTTON_FORWARD | JPF_BUTTON_REVERSE;
        if ((st & JP_TYPE_MASK) != JP_TYPE_NOTAVAIL && (st & mask & ~ui_joy0))
            return TRUE;
        ui_joy0 &= st;                      /* let go of: a new press counts */
    }
    return FALSE;
}

/* Wait for a loader job, the spinner turning meanwhile. Inside a busy
 * (navigation), input gives up the wait: FALSE, ui_aborted set, the job
 * left to come back later. */
static BOOL ui_wait(struct LJob *j)
{
    while (j->busy) {
        while (ldr_collect())
            ;
        if (!j->busy)
            break;
        if (busy.depth && (ui_aborted || ui_input_waiting())) {
            ui_aborted = TRUE;
            return FALSE;
        }
        if (!gfx_present(&g)) {             /* shows the spinner; the flip paces it */
            dbg_sleep_begin();
            WaitTOF();
            dbg_sleep_end();
        }
        busy_tick();
    }
    return TRUE;
}

/* ---- Pictures folders, read by the loader and kept for the session
 * (with memory to spare): a folder seen before comes back at once while
 * its date is unchanged. A read given up for input (ui_wait) goes on, and
 * is taken up again if the same folder is asked for next. */
#define PICS_KEEP 6
static struct {
    char path[300];
    struct DateStamp date;
    struct ItemList list;
    ULONG stamp;
    BOOL vids;                  /* read as a Videos folder (its .iff files looked inside) */
} pkeep[PICS_KEEP];
static ULONG pkeep_clock;
static struct LJob *pics_job;       /* a folder being read */
static ULONG pics_t0;

static void pics_forget(void)
{
    int k;
    for (k = 0; k < PICS_KEEP; k++)
        if (pkeep[k].list.pool)
            list_free(&pkeep[k].list);
    if (pics_job) {
        ldr_orphan(pics_job);
        pics_job = NULL;
    }
}

static void pics_copy(const struct ItemList *from, struct ItemList *out)
{
    LONG i;
    for (i = 0; i < from->count; i++)
        list_copy(out, &from->items[i], from->items[i].label);
}

/* Read a Pictures folder into out: from memory if it is kept and has not
 * changed, else by the loader, the spinner turning. -2: given up for
 * input (the read goes on), -1: cannot be read. */
static LONG pics_read_ui(const char *dir, struct ItemList *out)
{
    BOOL vids = cur() && cur()->pics == 2;  /* a Videos page's (pushed already) */
    struct DateStamp ds;
    BOOL dated = idx_date(dir, &ds);
    struct LJob *j;
    LONG n;
    int k, slot = 0;
    char m[80];
    for (k = 0; k < PICS_KEEP; k++)
        if (pkeep[k].list.pool && dated && !amb_stricmp(pkeep[k].path, dir) &&
            !CompareDates(&ds, &pkeep[k].date) && pkeep[k].vids == vids) {
            list_free(out);
            list_init(out);
            pics_copy(&pkeep[k].list, out);
            pkeep[k].stamp = ++pkeep_clock;
            return out->count;
        }
    if (pics_job && (strcmp(pics_job->path, dir) != 0 || (pics_job->mode == 1) != vids)) {
        ldr_orphan(pics_job);               /* another folder's: not wanted now */
        pics_job = NULL;
    }
    if (!pics_job) {
        if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
            return pics_read_dir(dir, out, vids);
        j->type = LJ_PICDIR;
        j->mode = vids ? 1 : 0;
        amb_strlcpy(j->path, dir, sizeof(j->path));
        ldr_submit_first(j);
        pics_job = j;
        pics_t0 = time_ms();
    }
    j = pics_job;
    if (!ui_wait(j))
        return -2;
    pics_job = NULL;
    n = j->ran ? j->np : -1;
    sprintf(m, "%ld entries in %ld ms", (long)n, (long)(time_ms() - pics_t0));
    amb_log("pictures read", m);
    if (n >= 0 && j->list.pool) {
        list_free(out);
        list_init(out);
        pics_copy(&j->list, out);
        if (dated && det_roomy()) {          /* kept for the next visit */
            for (k = 1; k < PICS_KEEP; k++)
                if (!pkeep[k].list.pool ||
                    (pkeep[slot].list.pool && pkeep[k].stamp < pkeep[slot].stamp))
                    slot = k;
            if (pkeep[slot].list.pool)
                list_free(&pkeep[slot].list);
            amb_strlcpy(pkeep[slot].path, dir, sizeof(pkeep[slot].path));
            pkeep[slot].date = ds;
            pkeep[slot].vids = vids;
            pkeep[slot].list = j->list;
            pkeep[slot].stamp = ++pkeep_clock;
            j->list.pool = NULL;
        }
    }
    if (j->list.pool)
        list_free(&j->list);
    FreeVec(j);
    return n;
}

/* Letters still to be read the first time: the count is still growing
 * (only when there was no index cache for it). */
BOOL ch_reading(const struct Page *p)
{
    return p->ch && !p->ch->cut && p->ch->next < p->ch->n;
}

static BOOL ch_all_held(const struct Chunks *ch)
{
    LONG k;
    for (k = 0; k < ch->n; k++)
        if (ch->c[k].first >= 0 && ch->c[k].count && !ch->c[k].held)
            return FALSE;
    return TRUE;
}

static void ch_free(struct Chunks *ch)
{
    LONG k;
    if (!ch)
        return;
    ldr_orphan(ch->job);                    /* still out: freed when it comes back */
    ldr_orphan(ch->wjob);                   /* (the index is still written) */
    for (k = 0; k < ch->n; k++)
        list_free(&ch->c[k].names);
    FreeVec(ch->c);
    FreeVec(ch);
}

/* The letter holding entry idx, -1 if none (own entries, not read yet). */
static LONG ch_of(const struct Chunks *ch, LONG idx)
{
    LONG k;
    for (k = 0; k < ch->n; k++)
        if (ch->c[k].first >= 0 && idx >= ch->c[k].first && idx < ch->c[k].first + ch->c[k].count)
            return k;
    return -1;
}

/* Let a letter's strings go: its rows show blank until it is read again. */
static void ch_drop(struct Page *p, LONG k)
{
    struct Chunk *c = &p->ch->c[k];
    LONG i;
    for (i = 0; i < c->count; i++) {
        struct Item *it = &p->list.items[c->first + i];
        it->label = c->letter;
        it->sortkey = (char *)"";
        it->path = c->path;
        it->flags = (it->flags & ~ITEM_SPLIT) | ITEM_GONE;
    }
    list_free(&c->names);
    c->held = FALSE;
    p->ch->dropped = TRUE;
}

/* Short of memory: the letters furthest from the selection go, never one
 * on screen or next to the selection's. */
static void ch_trim(struct Page *p)
{
    struct Chunks *ch = p->ch;
    LONG at, vis = list_visible();
    if (!ch || ch->frozen || det_roomy())
        return;
    at = ch_of(ch, p->sel);
    while (AvailMem(MEMF_ANY) < CH_LOW) {
        LONG k, far = -1, fd = -1;
        for (k = 0; k < ch->n; k++) {
            struct Chunk *c = &ch->c[k];
            LONG d = at < 0 ? k : (k > at ? k - at : at - k);
            if (!c->held || (at >= 0 && d <= 1) ||
                (c->first < p->top + vis && c->first + c->count > p->top))
                continue;
            if (d > fd) {
                fd = d;
                far = k;
            }
        }
        if (far < 0)
            break;
        ch_drop(p, far);
    }
}

static void ch_save(struct Page *p);

/* A letter's entries came back from the loader. */
static void ch_take(struct Page *p, LONG k, struct ItemList *got)
{
    struct Chunks *ch = p->ch;
    struct Chunk *c = &ch->c[k];
    LONG i;
    if (c->first < 0) {                     /* read the first time: their places */
        c->first = p->list.count;
        c->count = 0;
        for (i = 0; i < got->count; i++)
            if (list_append(&p->list, &got->items[i]))
                c->count++;
        if (c->first > 0 && c->count) {
            const struct Item *a = &p->list.items[c->first - 1], *b = &p->list.items[c->first];
            if (!(a->flags & ITEM_GONE) && amb_stricmp(a->sortkey, b->sortkey) > 0)
                ch->disorder = TRUE;        /* filed under another letter */
        }
        ch->next = k + 1;
    } else {                                /* read again (or from the index): the strings */
        LONG n = got->count < c->count ? got->count : c->count;
        for (i = 0; i < n; i++) {
            struct Item *it = &p->list.items[c->first + i];
            it->label = got->items[i].label;
            it->sortkey = got->items[i].sortkey;
            it->path = got->items[i].path;
            it->kind = got->items[i].kind;
            it->mark = got->items[i].mark;
            it->flags = got->items[i].flags | (it->flags & ITEM_FLAT);
        }
    }
    list_drop_items(got);                   /* only the strings are kept */
    list_free(&c->names);
    c->names = *got;
    c->held = TRUE;
    got->pool = NULL;
    if (ch->next >= ch->n && (ch->own || ch->disorder) && ch_all_held(ch) && !ch->frozen) {
        /* all read, and not in order letter by letter: sort it whole (the
         * letters no longer map to places, so none is let go from now;
         * the index is saved first, while they still do) */
        ch_save(p);
        list_sort(&p->list);
        ch->frozen = TRUE;
    }
}

/* Send the loader for letter k. */
static void ch_send(struct Page *p, LONG k)
{
    struct Chunks *ch = p->ch;
    struct LJob *j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR);
    if (!j)
        return;
    j->type = LJ_FOLDER;
    j->adult = ch->adult;
    amb_strlcpy(j->path, ch->c[k].path, sizeof(j->path));
    if (ch->c[k].cached) {                  /* from the index file: one read */
        amb_strlcpy(j->cache, ch->idx, sizeof(j->cache));
        j->off = ch->c[k].coff;
        j->len = ch->c[k].clen;
    }
    ch->job = j;
    ch->job_k = k;
    if ((ch->c[k].first <= p->sel && p->sel < ch->c[k].first + ch->c[k].count) ||
        (ch->c[k].first < p->top + list_visible() && ch->c[k].first + ch->c[k].count > p->top))
        ldr_submit_first(j);                /* (on screen: before pictures, read-aheads) */
    else
        ldr_submit(j);
}

/* What to read next, if anything: a letter on screen not held (let go,
 * or laid out from the index and not read yet), then those either side of
 * the selection, the way the list moves first, then the next one not read
 * at all; with memory to spare, then every other one, nearest first. */
static void ch_pump(struct Page *p)
{
    struct Chunks *ch = p ? p->ch : NULL;
    LONG k, at, vis;
    BOOL roomy;
    if (!ch || ch->job || ch->laying || ch->frozen)
        return;
    vis = list_visible();
    roomy = det_roomy();
    for (k = 0; k < ch->n; k++) {
        struct Chunk *c = &ch->c[k];
        if (c->first >= 0 && c->count && !c->held && c->first < p->top + vis &&
            c->first + c->count > p->top) {
            ch_send(p, k);
            return;
        }
    }
    if ((at = ch_of(ch, p->sel)) >= 0) {
        LONG d = det.dir >= 0 ? 1 : -1, s;
        for (s = 0; s < 2; s++, d = -d) {
            struct Chunk *c;
            if (at + d < 0 || at + d >= ch->n)
                continue;
            c = &ch->c[at + d];
            if (c->first >= 0 && c->count && !c->held &&
                (roomy || (s == 0 && AvailMem(MEMF_ANY) >= CH_LOW))) {
                ch_send(p, at + d);
                return;
            }
        }
    }
    if (ch->next < ch->n && !ch->cut) {
        if (AvailMem(MEMF_ANY) < CH_FLOOR) {
            char m[160];
            ch_trim(p);
            if (AvailMem(MEMF_ANY) < CH_FLOOR) {
                ch->cut = TRUE;
                sprintf(m, _("Low on memory: list stops at %ld entries"), (long)p->list.count);
                set_msg(m);
                return;
            }
        }
        ch_send(p, ch->next);
        return;
    }
    if (roomy) {
        LONG best = -1, bd = 0;
        if (at < 0)
            at = 0;
        for (k = 0; k < ch->n; k++) {
            struct Chunk *c = &ch->c[k];
            LONG d = k > at ? k - at : at - k;
            if (c->first >= 0 && c->count && !c->held && (best < 0 || d < bd)) {
                best = k;
                bd = d;
            }
        }
        if (best >= 0)
            ch_send(p, best);
    }
}

/* Save the index of a list read whole (some letter from its folder): each
 * letter's date and count, and its entries where they are to be had (held,
 * or carried over from the old file). Written by the loader. */
static void ch_save(struct Page *p)
{
    struct Chunks *ch = p->ch;
    struct IdxOut o;
    struct LJob *j;
    LONG k;
    if (!ch->dirty || ch->wjob || ch->frozen || ch->next < ch->n || ch->cut || !ch->n)
        return;
    if (!idx_begin(&o, ch->n))
        return;
    for (k = 0; k < ch->n; k++) {
        struct Chunk *c = &ch->c[k];
        const char *name = FilePart((STRPTR)c->path);
        BOOL ok;
        LONG before = o.len;
        if (c->held) {
            ok = idx_letter(&o, k, &c->date, name, c->count, &p->list.items[c->first], NULL, 0);
        } else if (c->cached) {
            UBYTE *b = idx_block(ch->idx, c->coff, c->clen);
            ok = idx_letter(&o, k, &c->date, name, c->count, NULL, b, b ? c->clen : 0);
            FreeVec(b);
        } else {
            ok = idx_letter(&o, k, &c->date, name, c->count, NULL, NULL, 0);
        }
        if (!ok) {
            idx_abandon(&o);
            return;
        }
        c->noff = o.len > before ? before : 0;
        c->nlen = o.len - before;
    }
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        idx_abandon(&o);
        return;
    }
    j->type = LJ_WRITE;
    amb_strlcpy(j->path, ch->idx, sizeof(j->path));
    j->buf = o.buf;
    j->len = o.len;
    for (k = 0; k < ch->n; k++)
        ch->c[k].cached = FALSE;            /* the file is being replaced: not until it is */
    ch->dirty = FALSE;
    ch->wjob = j;
    ldr_submit(j);
}

/* The index file is written: letters read from it now come from there. */
static void ch_saved(struct Page *p)
{
    struct Chunks *ch = p->ch;
    struct LJob *j = ch->wjob;
    LONG k;
    ch->wjob = NULL;
    if (j->ran && j->np > 0)
        for (k = 0; k < ch->n; k++) {
            ch->c[k].coff = ch->c[k].noff;
            ch->c[k].clen = ch->c[k].nlen;
            ch->c[k].cached = ch->c[k].nlen > 0;
        }
    amb_log("index cache", j->ran && j->np > 0 ? "saved" : "could not be saved");
    FreeVec(j->buf);
    FreeVec(j);
}

/* The loader brought page p's letter. */
static void ch_arrived(struct Page *p, BOOL shown)
{
    struct Chunks *ch = p->ch;
    struct LJob *j = ch->job;
    LONG k = ch->job_k, before = p->list.count, vis = list_visible();
    BOOL was_reading = ch_reading(p), was_held = ch->c[k].held, frozen = ch->frozen;
    ch->job = NULL;
    if (j->ran && j->np >= 0 && !j->cache[0]) {
        ch->c[k].date = j->date;            /* read from its folder: the index is out of date */
        ch->dirty = TRUE;
    }
    if (j->ran && j->np >= 0) {
        ch_take(p, k, &j->list);
    } else if (ch->c[k].first < 0) {
        ch->c[k].first = p->list.count;     /* unreadable: nothing in it */
        ch->c[k].count = 0;
        ch->next = k + 1;
    }
    list_free(&j->list);
    FreeVec(j);
    if (was_reading && !ch_reading(p) && !ch->laying) {
        char m[64];
        sprintf(m, "%ld entries in %ld ms", (long)p->list.count, (long)(time_ms() - ch->t0));
        amb_log("list read", m);
    }
    ch_save(p);                             /* complete: keep its index (before letting go) */
    ch_trim(p);
    if (shown) {
        struct Chunk *c = &ch->c[k];
        if (!was_reading || !ch_reading(p) || p->list.count != before)
            draw_title_count(p);            /* the count (and the title, once complete) */
        if (ch->frozen != frozen || before < p->top + vis ||
            (!was_held && c->first < p->top + vis && c->first + c->count > p->top))
            draw_list_rows(p);              /* rows that were empty or blank, or re-sorted */
        if (p->sel >= c->first && p->sel < c->first + c->count) {
            if (det.page == p && det.sel == p->sel && !det.tj && !det.sj)
                det.shown = FALSE;          /* given none while not held: asked again */
            det_touch(p);
        }
    }
    ch_pump(p);
}

/* Wait for the loader's letter to come back (the list must have it now).
 * shown: the page is on screen, so draw what changes. */
static BOOL ch_wait_one(struct Page *p, BOOL shown)
{
    ch_pump(p);
    if (!p->ch || !p->ch->job)
        return FALSE;
    if (!ui_wait(p->ch->job))
        return FALSE;                       /* given up for input: it comes later */
    ch_arrived(p, shown);
    return TRUE;
}

/* The list read at least to count entries (or as far as it goes). */
static void ch_wait_until(struct Page *p, LONG count, BOOL shown)
{
    while (ch_reading(p) && p->list.count < count)
        if (!ch_wait_one(p, shown))
            break;
}

/* Entry idx held (its letter read again if it was let go). */
void ch_wait_item(struct Page *p, LONG idx, BOOL shown)
{
    LONG k;
    if (!p->ch || idx >= p->list.count || !(p->list.items[idx].flags & ITEM_GONE))
        return;
    if ((k = ch_of(p->ch, idx)) < 0)
        return;
    if (p->ch->job && p->ch->job_k != k) {
        if (!ui_wait(p->ch->job))           /* the one already out first */
            return;
        ch_arrived(p, shown);
    }
    if (!p->ch->c[k].held) {
        if (!p->ch->job)
            ch_send(p, k);
        if (p->ch->job) {
            if (!ui_wait(p->ch->job))
                return;
            ch_arrived(p, shown);
        }
    }
}

/* Lay a list out from its index file: every letter whose folder has not
 * changed gets its places at once (blank until its entries are read, from
 * the file), so the count is the whole list's from the start; a letter
 * that has changed is read from its folder now. FALSE if there is no
 * index for this list (or it is not this list's any more). */
static BOOL ch_from_index(struct Page *p)
{
    struct Chunks *ch = p->ch;
    struct Idx ix;
    LONG k, i, fresh = 0;
    char m[64];
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    if (!idx_load(ch->idx, &ix)) {
        me->pr_WindowPtr = oldwin;
        return FALSE;
    }
    me->pr_WindowPtr = oldwin;
    if (ix.n != ch->n) {
        idx_free(&ix);
        return FALSE;
    }
    for (k = 0; k < ch->n; k++)
        if (amb_stricmp(ix.l[k].name, (const char *)FilePart((STRPTR)ch->c[k].path)) != 0) {
            idx_free(&ix);
            return FALSE;
        }
    ch->laying = TRUE;
    for (k = 0; k < ch->n; k++) {
        struct Chunk *c = &ch->c[k];
        struct DateStamp ds;
        if (idx_date(c->path, &ds) && !CompareDates(&ds, &ix.l[k].date)) {
            struct Item ph;
            memset(&ph, 0, sizeof(ph));
            ph.label = c->letter;
            ph.sortkey = (char *)"";
            ph.path = c->path;
            ph.kind = IT_RUN;
            ph.flags = ITEM_GONE;
            c->date = ix.l[k].date;
            c->first = p->list.count;
            c->count = 0;
            for (i = 0; i < ix.l[k].count; i++)
                if (list_append(&p->list, &ph))
                    c->count++;
            c->coff = ix.l[k].off;
            c->clen = ix.l[k].len;
            c->cached = c->clen > 0;
            ch->next = k + 1;
        } else {
            ch->next = k;                   /* changed: read it now, from its folder */
            ch_send(p, k);
            if (ch->job) {
                if (!ui_wait(ch->job)) {    /* given up for input: the page goes */
                    ch->laying = FALSE;
                    idx_free(&ix);
                    return TRUE;
                }
                ch_arrived(p, FALSE);
            }
            if (c->first < 0) {             /* could not: nothing there */
                c->first = p->list.count;
                c->count = 0;
            }
            ch->next = k + 1;
            fresh++;
        }
    }
    ch->laying = FALSE;
    idx_free(&ix);
    sprintf(m, "%ld entries, %ld letters changed, %ld ms", (long)p->list.count, (long)fresh,
            (long)(time_ms() - ch->t0));
    amb_log("list from index", m);
    return TRUE;
}

/* Open an AGS folder as a list page: a letter at a time when it is a
 * long flat one (first: entries to have before it is shown). */
static LONG page_read_ags(struct Page *p, const struct Item *it, LONG first)
{
    struct ItemList subs;
    struct Chunks *ch;
    char path[300];
    LONG n, i, k;
    BOOL adult = ags.adult[0] == '1';
    if (!list_init(&subs))
        return -1;
    n = ags_read_dir(item_path(it, path, sizeof(path)), &subs, adult);
    if (n < 0) {
        list_free(&subs);
        return -1;
    }
    if (!(it->flags & ITEM_FLAT) && !ags_letter_menu(&subs)) {
        for (i = 0; i < subs.count; i++)    /* a menu of named folders: as it is */
            list_copy(&p->list, &subs.items[i], subs.items[i].label);
        list_free(&subs);
        return p->list.count;
    }
    for (i = 0, n = 0; i < subs.count; i++)
        if (subs.items[i].kind == IT_DIR)
            n++;
    ch = AllocVec(sizeof(*ch), MEMF_ANY | MEMF_CLEAR);
    if (!ch || (n && !(ch->c = AllocVec(n * sizeof(struct Chunk), MEMF_ANY | MEMF_CLEAR)))) {
        FreeVec(ch);
        list_free(&subs);
        return -1;
    }
    ch->n = n;
    ch->adult = adult;
    for (i = 0, k = 0; i < subs.count; i++) {
        struct Item *s = &subs.items[i];
        if (s->kind != IT_DIR) {
            list_copy(&p->list, s, s->label);   /* its own entries */
            ch->own = TRUE;
            continue;
        }
        item_path(s, ch->c[k].path, sizeof(ch->c[k].path));
        ch->c[k].letter[0] = s->label[0];
        ch->c[k].first = -1;
        k++;
    }
    list_free(&subs);
    p->ch = ch;
    ch->t0 = time_ms();
    idx_name(path, adult, ch->idx, sizeof(ch->idx));
    if (ch_from_index(p)) {
        /* every place laid out: read what the first screens show */
        if (!ui_aborted)
            ch_wait_item(p, 0, FALSE);
        if (p->list.count && !ui_aborted)
            ch_wait_item(p, (p->list.count < first ? p->list.count : first) - 1, FALSE);
    } else {
        ch_wait_until(p, first, FALSE);     /* not drawn yet */
    }
    if (ui_aborted)
        return -2;                          /* given up for input */
    ch_pump(p);                             /* and on with the rest */
    return p->list.count;
}

static void text_line(struct Page *p, const char *s)
{
    list_add(&p->list, s, "", "", IT_TEXT);
}

/* Text box width: the pane less room for the scroll bar (gap + bar + gap). */
WORD textbox_w(void)
{
    /* wrapped to the pane beside the menu's labels: it fits either way */
    WORD w = gfx_pane_w_wide(&g);
    return (w < g.pane_w ? w : g.pane_w) - U(9);
}

/* Marks a Japanese or Chinese line does not begin with: commas, full
 * stops, closing brackets, the long vowel mark, small kana (kinsoku). */
static BOOL no_line_start(ULONG c)
{
    static const UWORD marks[] = { 0x3001, 0x3002, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011,
                                   0x3015, 0x30FC, 0x30FB, 0xFF01, 0xFF09, 0xFF0C, 0xFF0E,
                                   0xFF1A, 0xFF1B, 0xFF1F, 0xFF3D, 0xFF5D, 0x309D, 0x309E,
                                   0x30FD, 0x30FE, 0x3005 };
    static const UWORD small_kana[] = { 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083,
                                        0x3085, 0x3087, 0x308E, 0x30A1, 0x30A3, 0x30A5, 0x30A7,
                                        0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE, 0x30F5,
                                        0x30F6 };
    unsigned i;
    for (i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        if (c == marks[i])
            return TRUE;
    for (i = 0; i < sizeof(small_kana) / sizeof(small_kana[0]); i++)
        if (c == small_kana[i])
            return TRUE;
    return FALSE;
}

/* Word-wrap a paragraph into l, maxw pixels wide. A Japanese, Chinese or
 * Korean character is a word of its own, joined without a space (their
 * text has none to break at). */
/* A wrapped line, marked as one (the paragraph's first, or a later one) so
 * that it can be wrapped again at another width (rewrap_list). */
static void wrap_line(struct ItemList *l, const char *line, BOOL *more)
{
    struct Item *it = list_add(l, line, "", "", IT_TEXT);
    if (it)
        it->flags |= ITEM_PARA | (*more ? ITEM_WRAP : 0);
    *more = TRUE;
}

void wrap_into(struct ItemList *l, const char *s, WORD maxw)
{
    char line[256], word[96];
    LONG last = 0;                      /* where line's last word begins */
    BOOL more = FALSE;                  /* a line of it added already */
    line[0] = 0;
    while (*s) {
        LONG n = 0;
        char tryline[352];
        ULONG cp;
        BOOL spaced = FALSE;
        int k;
        while (*s == ' ')
            s++, spaced = TRUE;
        if ((k = cjk_far_east((const UBYTE *)s, strlen(s), &cp))) {
            /* a Far East character is a word of its own (Greek is not) */
            memcpy(word, s, k);
            n = k;
            s += k;
        } else {
            while (*s && *s != ' ' && n < (LONG)sizeof(word) - 1 &&
                   !(n && cjk_far_east((const UBYTE *)s, strlen(s), &cp)))
                word[n++] = *s++;
        }
        word[n] = 0;
        if (!n)
            break;
        amb_strlcpy(tryline, line, sizeof(tryline));
        if (tryline[0] && spaced)
            amb_strlcat(tryline, " ", sizeof(tryline));
        amb_strlcat(tryline, word, sizeof(tryline));
        if (line[0] && gfx_textlen(&g, tryline, TS_BODY) > maxw) {
            /* a mark that may not begin a line takes the character
             * before it along (kinsoku): a closing bracket or stop right
             * after Far East text, Latin or not */
            if (!spaced && last > 0 && cjk_any(line, strlen(line)) &&
                (strchr(")]},.:;!?", word[0]) ||
                 (n == 3 && cjk_far_east((const UBYTE *)word, 3, &cp) && no_line_start(cp)))) {
                char carry[96];
                amb_strlcpy(carry, line + last, sizeof(carry));
                line[last] = 0;
                while (last > 0 && line[last - 1] == ' ')
                    line[--last] = 0;
                wrap_line(l, line, &more);
                amb_strlcpy(line, carry, sizeof(line));
                amb_strlcat(line, word, sizeof(line));
                last = 0;
                continue;
            }
            wrap_line(l, line, &more);
            amb_strlcpy(line, word, sizeof(line));
            last = 0;
        } else {
            last = strlen(tryline) - n;
            amb_strlcpy(line, tryline, sizeof(line));
        }
    }
    if (line[0])
        wrap_line(l, line, &more);
}

/* A text page's lines wrapped again to maxw (the window resized): each
 * paragraph wrap_into made joined again (a space between its lines, but
 * none between Far East characters) and wrapped anew; any other line kept
 * as it is. FALSE: no memory, as it was. */
static BOOL rewrap_list(struct ItemList *l, WORD maxw)
{
    struct ItemList nl;
    LONG i = 0;
    if (!list_init(&nl))
        return FALSE;
    while (i < l->count) {
        struct Item *it = &l->items[i];
        if (it->flags & ITEM_PARA) {
            LONG j, len = 0;
            char *para;
            ULONG cp;
            for (j = i; j < l->count && (j == i || (l->items[j].flags & ITEM_WRAP)); j++)
                len += strlen(l->items[j].label) + 1;
            if (!(para = AllocVec(len + 1, MEMF_ANY))) {
                list_free(&nl);
                return FALSE;
            }
            para[0] = 0;
            for (j = i; j < l->count && (j == i || (l->items[j].flags & ITEM_WRAP)); j++) {
                const char *t = l->items[j].label;
                LONG pl = strlen(para);
                if (j > i && pl && t[0]) {
                    /* a space unless both sides are Far East characters */
                    LONG k = pl - 1;
                    while (k > 0 && ((UBYTE)para[k] & 0xC0) == 0x80)
                        k--;
                    if (!(cjk_far_east((const UBYTE *)para + k, pl - k, &cp) &&
                          cjk_far_east((const UBYTE *)t, strlen(t), &cp)))
                        strcat(para, " ");
                }
                strcat(para, t);
            }
            wrap_into(&nl, para, maxw);
            FreeVec(para);
            i = j;
        } else {
            struct Item *n = list_add(&nl, it->label, it->path ? it->path : "",
                                      it->sortkey ? it->sortkey : "", it->kind);
            if (n) {
                n->action = it->action;
                n->mark = it->mark;
                n->flags = it->flags;
            }
            i++;
        }
    }
    list_free(l);
    *l = nl;
    return TRUE;
}

/* Word-wrap a paragraph to the text box width. */
static void text_para(struct Page *p, const char *s)
{
    wrap_into(&p->list, s, textbox_w());
}

/* ------------------------------------------------------------ AGS views */
static BOOL starts_nocase(const char *s, const char *pre, const char **rest)
{
    char tmp[64];
    LONG n = strlen(pre);
    amb_strlcpy(tmp, s, n + 1);
    if (amb_stricmp(tmp, pre) != 0)
        return FALSE;
    if (rest)
        *rest = s + n;
    return TRUE;
}

/* Which module an AGS top-level menu belongs to, its label there and its
 * sort priority. Returns -1 for AGS's own utility menus. */
static int classify(const char *label, char *out, LONG max, int *prio)
{
    static const char *skip[] = { "Welcome To AGS!", "Add-On Files", "Documentation",
                                  "Filters", "Options", "Search", "Themes", NULL };
    const char *rest;
    int i;
    for (i = 0; skip[i]; i++)
        if (amb_stricmp(label, skip[i]) == 0)
            return -1;
    amb_strlcpy(out, label, max);
    if (starts_nocase(label, "Games - ", &rest) || starts_nocase(label, "Demos - ", &rest)) {
        int mod = (label[0] == 'G' || label[0] == 'g') ? MOD_GAMES : MOD_DEMOS;
        if (amb_stricmp(rest, "0-Z") == 0) {
            /* one list of every title (ITEM_FLAT), not a folder per letter */
            amb_strlcpy(out, mod == MOD_GAMES ? _("All Games") : _("All Demos"), max);
            *prio = 1;
        } else {
            amb_strlcpy(out, rest, max);
            *prio = 2;
        }
        return mod;
    }
    if (amb_stricmp(label, "Disk Magazines") == 0) {
        *prio = 2;
        return MOD_DEMOS;
    }
    if (amb_stricmp(label, "Favourites") == 0) {
        *prio = 0;
        return MOD_GAMES;
    }
    *prio = 3;                  /* Extra Games, Emulators, anything new */
    return MOD_GAMES;
}

static void build_ags_root(struct Page *p, int mod)
{
    int prio, want;
    LONG i;
    char label[96];
    for (want = 0; want <= 3; want++)
        for (i = 0; i < ags.top.count; i++) {
            struct Item *it = &ags.top.items[i];
            if (it->kind != IT_DIR)
                continue;
            if (classify(it->label, label, sizeof(label), &prio) == mod && prio == want) {
                struct Item *n = list_copy(&p->list, it, label);
                if (n && prio == 1)
                    n->flags |= ITEM_FLAT;      /* Games/Demos - 0-Z */
            }
        }
}

static void no_ags_text(struct Page *p, int mod)
{
    if (igcat.n) {
        /* iGame's list is read, with none of this kind in it */
        text_para(p, mod == MOD_DEMOS ? _("iGame's list has no demos in it, and Amiga Game Selector "
                                          "was not found.")
                                      : _("iGame's list has no games in it, and Amiga Game Selector "
                                          "was not found."));
        text_line(p, "");
        text_para(p, _("Ambiance takes an entry as a demo when iGame's genre for it is Demo, Intro "
                     "or Diskmag, or when it is in a drawer named Demos."));
        return;
    }
    text_line(p, _("Amiga Game Selector was not found."));
    text_line(p, "");
    text_para(p, _("Amiga Game Selector 3 (AGS) is a ready-made collection of WHDLoad games and "
                 "demos, with screenshots and descriptions. With it installed, they are listed "
                 "here and start from Ambiance."));
    text_line(p, "");
    text_para(p, _("Ambiance looks for the AGS: assign, then for an AGS2 drawer next to its own "
                 "drawer and in WHDLoad:. Install Ambiance on an AGS 3 system, or start AGS once "
                 "so its assigns exist."));
    text_line(p, "");
    text_line(p, "");
    text_para(p, _("iGame's list is shown here too, or in AGS's place: Ambiance looks for iGame "
                 "when it starts, or Settings > Folders > iGame says where its drawer is."));
    text_line(p, "");
    text_para(p, _("Music, Mods, Pictures, Videos and Radio work without it: Settings > Folders "
                 "says where your music, modules, pictures and animations are."));
}

static void dt_line(void *ctx, const char *s)
{
    text_line((struct Page *)ctx, s);
}

/* MP3 by Ambiance's own ambmpega.library (Settings > Sound, load_sound_out) */
static BOOL own_mpega;

static void build_sysinfo(struct Page *p)
{
    char line[200];
    text_line(p, _("Found when Ambiance started:"));
    text_line(p, "");
    sprintf(line, _("Machine type: %s"), caps.emulator ? _("emulator (UAE family)") : _("real Amiga"));
    text_line(p, line);
    /* amiga.lib's sprintf is RawDoFmt: every number must be %l with a long. */
    sprintf(line, "CPU: 680%02lu", (unsigned long)caps.cpu);
    text_line(p, line);
    sprintf(line, "Kickstart %s (%lu.%lu)", caps_release_name(caps.kick),
            (unsigned long)caps.kick, (unsigned long)caps.kick_rev);
    text_line(p, line);
    if (caps.wb) {
        sprintf(line, "Workbench %s (%lu.%lu)", caps_release_name(caps.wb),
                (unsigned long)caps.wb, (unsigned long)caps.wb_rev);
        text_line(p, line);
    }
    sprintf(line, _("Chipset: %s%s, %s"), caps.aga ? "AGA" : caps.ecs ? "ECS" : "OCS",
            caps.ehb ? _(" with EHB") : "", caps.pal ? "PAL" : "NTSC");
    text_line(p, line);
    {
        /* Read the mode back from the open screen, not from what we asked for. */
        struct NameInfo ni;
        char mode[DISPLAYNAMELEN + 1];
        ULONG id = GetVPModeID(&g.scr->ViewPort);
        amb_strlcpy(mode, _("unnamed mode"), sizeof(mode));
        if (id != (ULONG)INVALID_ID &&
            GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, id) > 0)
            amb_strlcpy(mode, (char *)ni.Name, sizeof(mode));
        sprintf(line, _("Screen: %s"), mode);
        text_line(p, line);
        {
            /* Bits per pixel from the bitmap itself: an RTG bitmap's Depth
             * field is not reliable, GetBitMapAttr is. */
            ULONG d = GetBitMapAttr(g.scr->RastPort.BitMap, BMA_DEPTH);
            char colours[64];
            if (d > 8)
                sprintf(colours, _("%lu-bit colour"), (unsigned long)d);
            else
                sprintf(colours, _("%lu colours"), (unsigned long)(1UL << d));
            sprintf(line, _("  %lu x %lu, %s, mode ID $%08lx"),
                    (unsigned long)g.scr->Width, (unsigned long)g.scr->Height, colours,
                    (unsigned long)id);
        }
        text_line(p, line);
        if (g.style == STYLE_SMOOTH)
            sprintf(line, _("Style: smooth, %lu.%02lux"), (unsigned long)(g.k >> 8),
                    (unsigned long)(((g.k & 255) * 100 + 128) >> 8));
        else
            sprintf(line, _("Style: pixel, %lux%s"), (unsigned long)g.kint,
                    gfx_mode_tall(g.mode) ? _(" (tall pixels: dense)") : "");
        text_line(p, line);
        if (g.sm && !g.truecolor) {
            sprintf(line, _("  its shades and logo: %lu pens so far"),
                    (unsigned long)sm_pens_used(g.sm));
            text_line(p, line);
        }
    }
    sprintf(line, _("Graphics card (RTG): %s"), caps.rtg ? _("yes") : _("no"));
    text_line(p, line);
    if (caps.zz9000) {
        sprintf(line, _("ZZ9000: on Zorro %s; JPEG %s, PNG %s"), caps.zz9000 == 3 ? "III" : "II",
                plex.jpeg_card ? _("decoded on the card") : _("decoded here"),
                pic_on_card("PNG") ? _("decoded on the card") : _("decoded here"));
        text_line(p, line);
        if (caps.zz_rev) {
            sprintf(line, _("  zz9k.library 2.%lu: one-shot JPEG %s, pictures read back %s"),
                    (unsigned long)caps.zz_rev, caps.zz_jpeg1 ? _("yes") : _("no"),
                    caps.zz_cpusurf ? _("yes") : _("no"));
            text_line(p, line);
        }
    }
    sprintf(line, _("Free memory: %luKB chip, %luKB fast"),
            (unsigned long)(caps.chipfree >> 10), (unsigned long)(caps.fastfree >> 10));
    text_line(p, line);
    sprintf(line, _("Font: %s"), g.font_is_ours ? _("amb-pixel (bundled)") : _("topaz 8 (fallback)"));
    text_line(p, line);
    text_line(p, "");
    text_line(p, _("Datatypes (which one reads each picture format):"));
    pic_datatypes(dt_line, p);
    text_line(p, "");
    sprintf(line, _("AGS: %s"), ags.found ? _("found") : _("not found"));
    text_line(p, line);
    if (ags.added[0]) {
        /* S:AGS-Stuff did not run at boot; Ambiance filled the gaps. */
        text_line(p, _("Assigns Ambiance added (AGS-Stuff did not run):"));
        text_para(p, ags.added);
    }
    if (igcat.n)
        sprintf(line, _("iGame: %.120s, %ld games, %ld demos"), igcat.dir, (long)igcat.games,
                (long)igcat.demos);
    else
        sprintf(line, _("iGame: %s"), ig_off() ? _("none") : ig_setting[0] ? _("no list read")
                                                                          : _("not found"));
    text_line(p, line);
    {
        char where[64];
        sprintf(line, _("WHDLoad: %s"), whd_present(where, sizeof(where)) ? where : _("not installed"));
        text_line(p, line);
        sprintf(line, _("Archives unpacked by: %s"), unarc_how() ? unarc_name(unarc_how()) : _("nothing"));
        text_line(p, line);
    }
    sprintf(line, _("AGS machine setting (HW): %s"), ags.hw[0] ? ags.hw : _("not set"));
    text_line(p, line);
    sprintf(line, _("Adult entries: %s"), ags.adult[0] == '1' ? _("shown") : _("hidden"));
    text_line(p, line);
    sprintf(line, _("Network stack: %s"), caps.net ? _("running") : _("none"));
    text_line(p, line);
    sprintf(line, _("MP3: %s"), caps.mp3_mhi ? _("MHI driver")
                                : own_mpega ? _("ambmpega.library, Ambiance's own")
                                : caps.mpega_zz ? _("mpega.library, the ZZ9000 SDK's (decoded on the card)")
                                : caps.mp3_mpega ? "mpega.library" : _("none"));
    text_line(p, line);
    /* FLAC: the library, and whether this Amiga decodes it in time (measured
     * at each start: the same system image on a slow and a fast Amiga) */
    sprintf(line, _("FLAC: %s"), !caps.flacdec ? _("not installed (Aminet: util/libs/flacdec)")
                                 : plex_tier() < 2 ? _("flacdec.library, too slow here")
                                 : _("flacdec.library, fast enough"));
    text_line(p, line);
    if (caps.flacdec && plex_tier() < 2)
        text_line(p, _("  Plex: FLAC converted to MP3 here"));
    if (ep_installed())
        sprintf(line, _("Module players: %ld (EaglePlayer, DeliTracker)"), (long)ep_players());
    else
        strcpy(line, _("Module players: none (Aminet: mus/play/Eagleplayer)"));
    text_line(p, line);
    text_line(p, "");
    sprintf(line, "Debug log: %s", amb_log_path() ? amb_log_path() : "off");
    text_line(p, line);
}

/* ------------------------------------------------------------ settings */
#define MODE_VAR "Ambiance/ScreenMode"
#define WINDOW_VAR "Ambiance/Windowed"  /* "1" a window on Workbench, "0" full screen */
#define DEPTH_VAR "Ambiance/ColourDepth"   /* "32": RTG screens 32 (or 24) bits deep where offered */
#define TSIZE_VAR "Ambiance/TextSize"   /* the window's: small, medium, large */
#define COLOURS_VAR "Ambiance/Colours"  /* the colour scheme: midnight, graphite, wb13, emerald, amethyst */
#define WINBOX_VAR "Ambiance/WindowBox"  /* left top width height scale: where it was left */

/* The icon's WINDOWED tooltype (shipped as "(WINDOWED)": in brackets, off),
 * or WINDOWED from a Shell: the window unless Settings has saved a choice,
 * which comes first (load_screen_mode). */
extern struct WBStartup *_WBenchMsg;
struct Library *IconBase;
static BOOL start_windowed;
static const char *win_chosen_by = "nothing: full screen";

/* The window's Workbench menus (gfx.c lays them out): their labels in
 * the language chosen, except Japanese, Chinese and Korean, which
 * Workbench's own font has no letters for. */
enum { WM_ABOUT = 1, WM_SETTINGS, WM_FULL = GFX_MENU_FULL, WM_QUIT, WM_ICONIFY = GFX_MENU_ICONIFY };
static struct NewMenu wb_menus[7];

static BOOL wb_text_english(void);

static void window_menus_make(void)
{
    BOOL en = wb_text_english();

    struct NewMenu nm[7] = {
        { NM_TITLE, NULL, NULL, 0, 0, NULL },
        { NM_ITEM, NULL, NULL, 0, 0, (APTR)WM_ABOUT },
        { NM_ITEM, NULL, NULL, 0, 0, (APTR)WM_SETTINGS },
        { NM_ITEM, NULL, (STRPTR)"F", 0, 0, (APTR)WM_FULL },   /* Amiga-F */
        { NM_ITEM, NULL, (STRPTR)"I", 0, 0, (APTR)WM_ICONIFY }, /* Amiga-I */
        { NM_ITEM, NULL, (STRPTR)"Q", 0, 0, (APTR)WM_QUIT },
        { NM_END, NULL, NULL, 0, 0, NULL } };
    nm[0].nm_Label = (STRPTR)(en ? "Project" : _("Project"));
    nm[1].nm_Label = (STRPTR)(en ? "About..." : _("About..."));
    nm[2].nm_Label = (STRPTR)(en ? "Settings..." : _("Settings..."));
    nm[3].nm_Label = (STRPTR)(en ? "Full screen" : _("Full screen"));
    nm[4].nm_Label = (STRPTR)(en ? "Iconify" : _("Iconify"));
    if (!icon_available())
        nm[4].nm_Flags = NM_ITEMDISABLED;   /* (no workbench.library) */
    nm[5].nm_Label = (STRPTR)(en ? "Quit" : _("Quit"));
    memcpy(wb_menus, nm, sizeof(nm));
    gfx_window_menus(wb_menus);
}

static void read_start_args(void)
{
    if (_WBenchMsg) {
        if (_WBenchMsg->sm_NumArgs > 0 &&
            (IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 37))) {
            struct WBArg *a = &_WBenchMsg->sm_ArgList[0];   /* the program's own icon */
            BPTR old = CurrentDir(a->wa_Lock);
            struct DiskObject *d = GetDiskObject(a->wa_Name);
            if (d) {
                start_windowed = FindToolType((CONST_STRPTR *)d->do_ToolTypes,
                                              (CONST_STRPTR)"WINDOWED") != NULL;
                FreeDiskObject(d);
            }
            CurrentDir(old);
            CloseLibrary(IconBase);
            IconBase = NULL;
        }
    } else {
        LONG arg[1] = { 0 };
        struct RDArgs *rd = ReadArgs((CONST_STRPTR)"WINDOWED/S", arg, NULL);
        if (rd) {
            start_windowed = arg[0] != 0;
            FreeArgs(rd);
        }
    }
}

static void ensure_dir(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!l)
        l = CreateDir((CONST_STRPTR)path);
    if (l)
        UnLock(l);
}

/* Remembered in ENV: and ENVARC: (Ambiance/ScreenMode), like AGS's own
 * settings. Delete ENVARC:Ambiance/ScreenMode to go back to Automatic. */
static void load_screen_mode(void)
{
    char buf[32];
    int m;
    if (GetVar((CONST_STRPTR)DEPTH_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        caps_rtg_bits = buf[0] == '3' ? 32 : 16;
    if (GetVar((CONST_STRPTR)MODE_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
        (m = gfx_mode_from_key(buf)) >= 0)
        screen_mode = m;
    /* A window on Workbench instead of a screen of our own (gfx.c):
     * Settings > Display > Window, saved; without a saved choice, the
     * icon's tooltype or the Shell's argument. Workbench must be 15-bit or
     * deeper, on the card, or the screen opens as ever. */
    if (GetVar((CONST_STRPTR)WINDOW_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        gfx_window_wanted = buf[0] == '1';
        win_chosen_by = gfx_window_wanted ? "Settings: a window" : "Settings: full screen";
    } else if (start_windowed) {
        gfx_window_wanted = TRUE;
        win_chosen_by = _WBenchMsg ? "the icon's WINDOWED tooltype" : "WINDOWED from the Shell";
    }
    if (GetVar((CONST_STRPTR)WINBOX_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        LONG v[5], n = 0;
        char *q = buf;
        while (n < 5 && *q) {
            LONG x = 0, neg = *q == '-';
            if (neg)
                q++;
            while (*q >= '0' && *q <= '9')
                x = x * 10 + (*q++ - '0');
            v[n++] = neg ? -x : x;
            while (*q == ' ')
                q++;
        }
        if (n == 5 && v[2] > 0 && v[3] > 0)
            gfx_window_box_set((WORD)v[0], (WORD)v[1], (WORD)v[2], (WORD)v[3], (UWORD)v[4]);
    }
    if (GetVar((CONST_STRPTR)TSIZE_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        gfx_text_size = !amb_stricmp(buf, "small") ? TSIZE_SMALL
                        : !amb_stricmp(buf, "large") ? TSIZE_LARGE
                        : !amb_stricmp(buf, "xlarge") ? TSIZE_XLARGE : TSIZE_MEDIUM;
    /* the colour scheme, before the screen or window opens in it */
    {
        int k = UI_MIDNIGHT, i;
        if (GetVar((CONST_STRPTR)COLOURS_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            for (i = 0; i < UI_SCHEMES; i++)
                if (!amb_stricmp(buf, ui_scheme_keys[i]))
                    k = i;
        ui_scheme_apply(k);
    }
}

static void save_setting(const char *var, const char *value)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    ensure_dir("ENV:Ambiance");
    ensure_dir("ENVARC:Ambiance");
    SetVar((CONST_STRPTR)var, (CONST_STRPTR)value, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    me->pr_WindowPtr = oldwin;
}

static void save_screen_mode(void)
{
    save_setting(MODE_VAR, gfx_mode_key(screen_mode));
}

#define DEBUG_VAR "Ambiance/Debug"
#define STYLE_VAR "Ambiance/Style"
#define LFONT_VAR "Ambiance/LacedFont"
#define RFONT_VAR "Ambiance/RTGFont"    /* RTG's pixel style: a font of gfx_laced_fonts */
#define JUMP_VAR  "Ambiance/JumpScroll"
#define ANIMSCR_VAR "Ambiance/AnimScreen"  /* "own": animations on their own screen */
#define VDEC_VAR "Ambiance/VideoDecoder"   /* "CPU": MPEG by the CPU on a ZZ9000 too (video.c) */
#define VOVL_VAR "Ambiance/VideoOverlay"   /* "off": the CPU's pictures drawn, not on the overlay */
#define TRANS_VAR "Ambiance/Transition"    /* a slideshow's: off fade crossfade tiles wipe */
static const char *const trans_keys[VT_COUNT] = { "off", "fade", "crossfade", "tiles", "wipe" };
static const char *const trans_names[VT_COUNT] = { N_("Off"), N_("Fade"), N_("Cross-fade"),
                                                   N_("Tiles"), N_("Wipe") };
#define DBUF_VAR  "Ambiance/DoubleBuffer"
#define VISUAL_VAR "Ambiance/Visual"    /* Now playing: "meters", "spectrum" or "flow" */
#define HTTPS_VAR "Ambiance/RadioHTTPS"
#define NETIF_VAR "Ambiance/NetInterface"

/* Settings > Networking > the interface to go out from: its name ("" the
 * stack's choice); the stack's list as last asked. */
static char net_if[20];
static struct NetIf nics[8];
static int nnic;
static ULONG nic_in_use;

static void ip_text(ULONG ip, char *out)
{
    const UBYTE *b = (const UBYTE *)&ip;   /* in network order: a.b.c.d as it is */
    sprintf(out, "%lu.%lu.%lu.%lu", (unsigned long)b[0], (unsigned long)b[1],
            (unsigned long)b[2], (unsigned long)b[3]);
}

/* The interface chosen, as the stack lists it now: connections go out from
 * its address (none found, or not up: the stack's choice). */
static void net_if_apply(void)
{
    int i;
    ULONG ip = 0;
    char m[80];
    nnic = caps.net ? net_interfaces(nics, 8, plex.host, &nic_in_use) : -1;
    for (i = 0; i < nnic && net_if[0]; i++)
        if (!strcmp(nics[i].name, net_if) && nics[i].up)
            ip = nics[i].ip;
    net_bind_to(ip);
    if (net_if[0]) {
        sprintf(m, "%s: %s", net_if, ip ? "connections go out from it" : "not up: the stack's choice");
        amb_log("network interface", m);
    }
}

static ULONG strtoul_(const char *s);

/* ---- Settings > Networking > Radio, and the stream buffer */
#define RKBPS_VAR "Ambiance/RadioMaxKbps"   /* "" automatic, a number, "any" */
#define RSRV_VAR  "Ambiance/RadioServer"    /* "" automatic, "fastest:<name>", "<name>" */
#define BUF_VAR   "Ambiance/StreamBuffer"   /* short, normal, long */

static const UWORD rkbps_choice[6] = { 0xFFFF, 64, 96, 128, 192, 0 }; /* automatic .. any */
static int rkbps_sel;               /* 0 automatic */
static int buf_sel = 1;             /* normal */
static char rsrv[80];               /* as kept */
static struct NetMirror mirrors[8];
static int nmirror = -2;            /* -2 not asked yet, -1 could not be */
static const char *const buf_names[3] = { N_("Short"), N_("Normal"), N_("Long") };
static const char *const buf_notes[3] = { N_("starts in 0.6s"), N_("1.2s"),
                                          N_("3s, more held against dropouts") };

/* Automatic: by how fast this Amiga is (plex_tier). Paula plays no more
 * than 28 kHz, and the decoder is told to make what it can play, so a
 * 68030 copes with 128k stations; above that is work for nothing. */
static UWORD rkbps_auto(void)
{
    int t = plex_tier();
    if (plex.mhi || plex.mp3_card)
        return 0;                       /* MP3 not decoded by the 68k: any bitrate */
    return t == 0 ? 128 : t == 1 ? 192 : 0;
}

static UWORD rkbps_now(void)
{
    return rkbps_sel == 0 ? rkbps_auto() : rkbps_choice[rkbps_sel];
}

static void rkbps_text(UWORD k, char *out)
{
    if (k)
        sprintf(out, "%luk", (unsigned long)k);
    else
        strcpy(out, "any");
}

/* A server's short name: de1 for de1.api.radio-browser.info. */
static void mirror_short(const char *name, char *out, LONG max)
{
    char *d;
    amb_strlcpy(out, name, max);
    if ((d = strchr(out, '.')))
        *d = 0;
}

static void rsrv_apply(void)
{
    const char *n = rsrv;
    if (!strncmp(n, "fastest:", 8))
        n += 8;
    net_radio_server(n);
}

/* This Amiga's country, by its Locale prefs' telephone code (the country
 * names there are each in their own language): its ISO code, "" not known. */
static const char *home_country(void)
{
    static const struct { UWORD tel; char iso[3]; } t[] = {
        { 1, "US" }, { 7, "RU" }, { 20, "EG" }, { 27, "ZA" }, { 30, "GR" }, { 31, "NL" },
        { 32, "BE" }, { 33, "FR" }, { 34, "ES" }, { 36, "HU" }, { 39, "IT" }, { 40, "RO" },
        { 41, "CH" }, { 43, "AT" }, { 44, "GB" }, { 45, "DK" }, { 46, "SE" }, { 47, "NO" },
        { 48, "PL" }, { 49, "DE" }, { 51, "PE" }, { 52, "MX" }, { 54, "AR" }, { 55, "BR" },
        { 56, "CL" }, { 57, "CO" }, { 58, "VE" }, { 60, "MY" }, { 61, "AU" }, { 62, "ID" },
        { 63, "PH" }, { 64, "NZ" }, { 65, "SG" }, { 66, "TH" }, { 81, "JP" }, { 82, "KR" },
        { 84, "VN" }, { 86, "CN" }, { 90, "TR" }, { 91, "IN" }, { 92, "PK" }, { 98, "IR" },
        { 351, "PT" }, { 352, "LU" }, { 353, "IE" }, { 354, "IS" }, { 356, "MT" },
        { 357, "CY" }, { 358, "FI" }, { 359, "BG" }, { 370, "LT" }, { 371, "LV" },
        { 372, "EE" }, { 380, "UA" }, { 381, "RS" }, { 385, "HR" }, { 386, "SI" },
        { 387, "BA" }, { 420, "CZ" }, { 421, "SK" }, { 852, "HK" }, { 886, "TW" },
        { 972, "IL" },
    };
    static char iso[3];
    struct Library *LocaleBase = OpenLibrary((CONST_STRPTR)"locale.library", 38);
    ULONG tel = 0;
    UWORD i;
    iso[0] = 0;
    if (LocaleBase) {
        struct Locale *l = OpenLocale(NULL);
        if (l) {
            tel = l->loc_TelephoneCode;
            CloseLocale(l);
        }
        CloseLibrary(LocaleBase);
    }
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (t[i].tel == tel)
            amb_strlcpy(iso, t[i].iso, sizeof(iso));
    return iso;
}

static void load_radio_settings(void)
{
    char buf[80], m[160];
    int i;
    rkbps_sel = 0;
    if (GetVar((CONST_STRPTR)RKBPS_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        if (!amb_stricmp(buf, "any"))
            rkbps_sel = 5;
        for (i = 1; i < 5; i++)
            if ((UWORD)strtoul_(buf) == rkbps_choice[i])
                rkbps_sel = i;
    }
    buf_sel = 1;
    if (GetVar((CONST_STRPTR)BUF_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        for (i = 0; i < 3; i++)
            if (!amb_stricmp(buf, buf_names[i]))
                buf_sel = i;
    st_set_buffer(buf_sel);
    rsrv[0] = 0;
    /* the bitrate is set as each list is asked for (radio_list_kbps): its
     * Automatic times this Amiga (plex_tier), which needs the timer, not
     * open yet here */
    if (GetVar((CONST_STRPTR)RSRV_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        amb_strlcpy(rsrv, buf, sizeof(rsrv));
    rsrv_apply();
    radio_set_home(home_country());
    if (rkbps_sel)
        rkbps_text(rkbps_choice[rkbps_sel], buf);
    else
        strcpy(buf, "automatic");
    sprintf(m, "stations up to %.9s, buffer %s, list server %.60s, country %s", buf,
            buf_names[buf_sel], rsrv[0] ? rsrv : "automatic",
            home_country()[0] ? home_country() : "not known");
    amb_log("radio", m);
}

/* ---- Folders of music files, modules, pictures and animations (Settings >
 * Folders): each kind's assign (MUSIC:, MODS:, PICTURES:, VIDEOS:), used when it is there
 * unless turned off, and up to FS_DIRS folders added. AGS's own places
 * (ST-00:Modules, Media:Images...) are found by themselves as well, so on
 * AGS there is nothing to set; on another Amiga an assign or a folder or
 * two fills Mods and Pictures as they fill Music. */
enum { FS_MUSIC, FS_MODS, FS_PICS, FS_VIDEOS, FS_COUNT };
#define FS_DIRS 8
static struct Folders {
    const char *var, *avar;     /* the folders, one a line; "off": the assign left out */
    const char *assign;         /* MUSIC: */
    const char *name, *title;   /* N_: the kind (Settings > Folders' row), its page */
    char dir[FS_DIRS][160];
    int n;
    BOOL use_assign;
} fs[FS_COUNT] = {
    { "Ambiance/MusicFolders", "Ambiance/MusicAssign", "MUSIC:", N_("Music"), N_("Music folders"),
      { { 0 } }, 0, TRUE },
    { "Ambiance/ModFolders", "Ambiance/ModAssign", "MODS:", N_("Modules"), N_("Module folders"),
      { { 0 } }, 0, TRUE },
    { "Ambiance/PictureFolders", "Ambiance/PictureAssign", "PICTURES:", N_("Pictures"),
      N_("Picture folders"), { { 0 } }, 0, TRUE },
    { "Ambiance/VideoFolders", "Ambiance/VideoAssign", "VIDEOS:", N_("Videos"),
      N_("Video folders"), { { 0 } }, 0, TRUE },
};
#define music_dirs (fs[FS_MUSIC].dir)
#define nmusic_dirs (fs[FS_MUSIC].n)
#define music_assign (fs[FS_MUSIC].use_assign)
static int fs_page;             /* the kind Settings > Folders has open */

static void load_folders(void)
{
    static char buf[FS_DIRS * 162];
    char *p, *e;
    int k;
    for (k = 0; k < FS_COUNT; k++) {
        struct Folders *f = &fs[k];
        f->n = 0;
        f->use_assign = !(GetVar((CONST_STRPTR)f->avar, (STRPTR)buf, 8, GVF_GLOBAL_ONLY) > 0 &&
                          !amb_stricmp(buf, "off"));
        if (GetVar((CONST_STRPTR)f->var, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) <= 0)
            continue;
        for (p = buf; *p && f->n < FS_DIRS; p = e) {
            LONG n;
            e = strchr(p, '\n');
            n = e ? e - p : (LONG)strlen(p);
            if (n > 0 && n < (LONG)sizeof(f->dir[0])) {
                memcpy(f->dir[f->n], p, n);
                f->dir[f->n++][n] = 0;
            }
            e = e ? e + 1 : p + n;
        }
    }
}

static void save_folders(int k)
{
    static char buf[FS_DIRS * 162];
    int i;
    buf[0] = 0;
    for (i = 0; i < fs[k].n; i++) {
        if (i)
            amb_strlcat(buf, "\n", sizeof(buf));
        amb_strlcat(buf, fs[k].dir[i], sizeof(buf));
    }
    save_setting(fs[k].var, buf);
}

/* A drawer or assign is there (no requester asking for it if not). */
static BOOL place_there(const char *path)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR l;
    me->pr_WindowPtr = (APTR)-1;
    l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = old;
    if (!l)
        return FALSE;
    UnLock(l);
    return TRUE;
}

/* The kind's assign, if it is to be used and is there now. */
static BOOL fs_assign_used(int k)
{
    return fs[k].use_assign && place_there(fs[k].assign);
}
#define music_assign_there() place_there("MUSIC:")

/* AGS's own places for modules and pictures: listed when they are there. */
#define MODS_ROOT  "ST-00:Modules"
#define MODS_MENU  "AGS:Mods"
#define MODS_SID   "ST-00:C64Music"
#define PICS_ROOT  "Media:Images"
#define VIDS_ROOT  "Media:Animations"
#define VIDS_ROOT2 "Media:Videos"     /* (AGS's too, where videos are kept) */

/* Settings > Sound: what streams play through (AHI, where installed). */
#define SOUND_VAR "Ambiance/SoundOutput"
static BOOL sound_ahi;
/* Paula at 14 bits (SoundOutput "paula14"): all four channels, each side's
 * high byte at volume 64 and its next 6 bits at volume 1; MP3, FLAC and a
 * video's sound (stream.c), not modules */
static BOOL sound_p14;

/* MHI: its drivers (the libraries in LIBS:MHI), and the one MP3 goes to ("":
 * a decoder with mpega's functions: Ambiance's own ambmpega.library,
 * own_mpega, or LIBS:mpega.library), Ambiance/MP3Decoder ("ambmpega",
 * "mpega" or the driver's name; not set: ours, unless LIBS:mpega.library is
 * the ZZ9000's, which decodes on the card). */
#define MP3_VAR "Ambiance/MP3Decoder"
/* MPEG audio's quality (Ambiance/MP3Quality "high" or "medium"; not set:
 * automatic, by how fast the decoder measures at its first stream each
 * run, stream.c's mpa_choose; the measurement itself is never saved) */
#define MP3Q_VAR "Ambiance/MP3Quality"
static int mp3_q;                   /* 0 automatic, 1 high, 2 medium */
static const char *const mp3q_val[3] = { "auto", "high", "medium" };
static char mhi_names[6][48];
static int nmhi;
static char mhi_sel[48];

static void mhi_cb(void *ctx, const char *name, BOOL isdir)
{
    LONG n = strlen(name);
    (void)ctx;
    if (!isdir && nmhi < 6 && n > 8 && n < 48 && !amb_stricmp(name + n - 8, ".library"))
        strcpy(mhi_names[nmhi++], name);
}

/* The MHI driver chosen, to stream.c (its path) and Plex (any quality). */
static void mhi_apply(void)
{
    char path[80];
    if (mhi_sel[0]) {
        sprintf(path, "LIBS:MHI/%.48s", mhi_sel);
        st_use_mhi(path);
    } else {
        st_use_mhi("");
    }
    plex.mhi = mhi_sel[0] != 0;
    /* the ZZ9000 SDK's mpega.library decodes on the card: full quality */
    st_use_card_mpega(caps.mpega_zz);
    st_use_own_mpega(own_mpega && !mhi_sel[0]);
    plex.mp3_card = caps.mpega_zz && !mhi_sel[0] && !own_mpega;
}

static void load_sound_out(void)
{
    char v[48];
    int i;
    v[0] = 0;
    GetVar((CONST_STRPTR)SOUND_VAR, (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY);
    sound_ahi = caps.ahi && !amb_stricmp(v, "ahi");
    sound_p14 = !amb_stricmp(v, "paula14");
    st_use_ahi(sound_ahi);
    st_use_paula14(sound_p14);
    nmhi = 0;
    mhi_sel[0] = 0;
    if (caps.mp3_mhi) {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR old = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;
        dir_scan("LIBS:MHI", mhi_cb, NULL);
        me->pr_WindowPtr = old;
    }
    own_mpega = caps.mpega_own && !caps.mpega_zz;     /* (not chosen yet) */
    mp3_q = 0;
    if (GetVar((CONST_STRPTR)MP3Q_VAR, (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0)
        mp3_q = !amb_stricmp(v, "high") ? 1 : !amb_stricmp(v, "medium") ? 2 : 0;
    st_mp3_quality(mp3_q);
    if (GetVar((CONST_STRPTR)MP3_VAR, (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0) {
        if (!amb_stricmp(v, "ambmpega"))
            own_mpega = caps.mpega_own;
        else if (!amb_stricmp(v, "mpega"))
            own_mpega = !caps.mp3_mpega && caps.mpega_own;     /* (else nothing to play it) */
        for (i = 0; i < nmhi; i++)
            if (!amb_stricmp(v, mhi_names[i]))
                amb_strlcpy(mhi_sel, mhi_names[i], sizeof(mhi_sel));
    }
    if (!caps.mp3_mpega && !caps.mpega_own && !mhi_sel[0] && nmhi)
        amb_strlcpy(mhi_sel, mhi_names[0], sizeof(mhi_sel));  /* no mpega: MHI it is */
    mhi_apply();
    {
        char m[160];
        sprintf(m, "%s; MP3 by %s, %s quality; %ld MHI driver%s in LIBS:MHI",
                sound_ahi ? "AHI" : sound_p14 ? "Paula, 14 bits" : "Paula",
                mhi_sel[0] ? mhi_sel : own_mpega ? "ambmpega.library (Ambiance's own)"
                : caps.mpega_zz ? "mpega.library (ZZ9000 SDK: on the card)" : "mpega.library",
                mp3_q == 1 ? "high" : mp3_q == 2 ? "medium" : "automatic (measured)",
                (long)nmhi, nmhi == 1 ? "" : "s");
        amb_log("sound", m);
    }
}

static void load_net_if(void)
{
    char buf[20];
    net_if[0] = 0;
    if (GetVar((CONST_STRPTR)NETIF_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        amb_strlcpy(net_if, buf, sizeof(net_if));
    if (net_if[0] && caps.net)
        net_if_apply();
}

/* Settings > Networking > HTTPS: secure connections by AmiSSL 6 or 5 (tls.c),
 * off by default, and only with AmiSSL installed: Radio lists and plays
 * https: stations, and Plex signs in at plex.tv. */
static BOOL radio_https;

static BOOL https_ok(void)
{
    return radio_https && caps.amissl;
}

/* The network and playback processes told (they read it as they connect). */
static void https_apply(void)
{
    net_use_https(https_ok());
    st_use_https(https_ok());
}

/* Settings > Jump scrolling: held Up/Down in a very long list goes a
 * letter at a time once past cruising speed (off: it glides on). */
static BOOL jump_scroll;
/* Animations on a screen of their own even on a 16-bit RTG screen (HAM and
 * extra halfbrite by the chipset, the classic look); native screens always */
static BOOL anim_classic;
/* Settings > Video > MPEG video on the ZZ9000 (with one): off, the CPU
 * decodes it (mpegv.library), as without one */
static BOOL video_cpu;
static BOOL video_no_overlay;

void set_msg(const char *m);

/* Smooth chosen, the screen could show it, but its file would not load
 * (missing, or from another Ambiance version): say so, rather than
 * quietly drawing the pixel style. TRUE when it said something. */
static BOOL note_style(void)
{
    if (ui_style == STYLE_SMOOTH && g.style != STYLE_SMOOTH &&
        gfx_style_possible(&caps, g.mode, STYLE_SMOOTH)) {
        set_msg(_("Smooth: Fonts/amb-smooth does not match this Ambiance"));
        return TRUE;
    }
    return FALSE;
}

static void load_style(void)
{
    char buf[32];
    int i;
    if (GetVar((CONST_STRPTR)STYLE_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        ui_style = amb_stricmp(buf, "smooth") == 0 ? STYLE_SMOOTH : STYLE_PIXEL;
    if (GetVar((CONST_STRPTR)LFONT_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        for (i = 0; i < LFONT_COUNT; i++)
            if (!amb_stricmp(buf, gfx_laced_fonts[i].name))
                gfx_laced_font = i;
    if (GetVar((CONST_STRPTR)RFONT_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        for (i = 0; i < LFONT_COUNT; i++)
            if (!amb_stricmp(buf, gfx_laced_fonts[i].name))
                gfx_rtg_font = i;
}

/* F on Now playing: Flow, or one of AmigaAMP's visual plug-ins (amphost.c)
 * from PROGDIR:Visuals or AmigaAMP:Plugins, Ambiance/FullVisual its path
 * ("" or not set: Flow) */
#define FULLVIS_VAR "Ambiance/FullVisual"
#define FULLVIS_MAX 16
static char fullvis[200];
static char fv_paths[FULLVIS_MAX][160];
static int fv_n;
static const char *fv_dir;

static void fv_cb(void *ctx, const char *name, BOOL isdir)
{
    LONG n = strlen(name);
    (void)ctx;
    if (isdir || fv_n >= FULLVIS_MAX || (n > 5 && !amb_stricmp(name + n - 5, ".info")) ||
        (n > 4 && (!amb_stricmp(name + n - 4, ".txt") || !amb_stricmp(name + n - 4, ".doc"))) ||
        (n > 7 && !amb_stricmp(name + n - 7, ".readme")))
        return;
    sprintf(fv_paths[fv_n++], "%.80s%.78s", fv_dir, name);
}

/* the plug-ins there now */
static void fullvis_scan(void)
{
    static const char *const dirs[] = { "PROGDIR:Visuals/", "AmigaAMP:Plugins/" };
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR l;
    int i;
    fv_n = 0;
    me->pr_WindowPtr = (APTR)-1;            /* (no "insert volume" for AmigaAMP:) */
    for (i = 0; i < 2; i++) {
        if (!(l = Lock((CONST_STRPTR)dirs[i], SHARED_LOCK)))
            continue;
        UnLock(l);
        fv_dir = dirs[i];
        dir_scan(dirs[i], fv_cb, NULL);
    }
    me->pr_WindowPtr = old;
}

static void fullvis_next(void)
{
    int i, at = -1;
    fullvis_scan();
    for (i = 0; i < fv_n; i++)
        if (!amb_stricmp(fv_paths[i], fullvis))
            at = i;
    if (at + 1 < fv_n)
        amb_strlcpy(fullvis, fv_paths[at + 1], sizeof(fullvis));
    else
        fullvis[0] = 0;                     /* round to Flow */
    save_setting(FULLVIS_VAR, fullvis[0] ? fullvis : "flow");
    set_msg(fullvis[0] ? _("F on Now playing starts this AmigaAMP plug-in")
            : !fv_n ? _("Flow (AmigaAMP plug-ins: put them in Ambiance's Visuals drawer)")
                    : _("Flow"));
}

/* Settings > Display > Visualisation: the level meters or the
 * spectrum (vis.c); V on Now playing goes to the next. */
static void load_visual(void)
{
    char buf[16];
    vis_set_gfx(&g);
    fullvis[0] = 0;
    if (GetVar((CONST_STRPTR)FULLVIS_VAR, (STRPTR)fullvis, sizeof(fullvis), GVF_GLOBAL_ONLY) <= 0 ||
        !amb_stricmp(fullvis, "flow"))
        fullvis[0] = 0;
    amp_host_start();                   /* AmigaAMP's plug-ins: they find Ambiance */
    if (GetVar((CONST_STRPTR)VISUAL_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        vis_select(!amb_stricmp(buf, "spectrum") ? VIS_SPECTRUM
                   : !amb_stricmp(buf, "flow") ? VIS_FLOW : VIS_METERS);
    if (GetVar((CONST_STRPTR)"Ambiance/Loop", (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        play_loop = buf[0] == '1';
    /* the volume as it was left (kept at quit; muted, the volume before) */
    if (GetVar((CONST_STRPTR)"Ambiance/Volume", (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
        buf[0] >= '0' && buf[0] <= '9') {
        LONG v = 0;
        StrToLong((CONST_STRPTR)buf, &v);
        if (v >= 0 && v <= 64)
            mp_set_volume((int)v);
    }
}

void np_draw(BOOL full);
BOOL np_on(void);


void next_visual(void)
{
    if (mp_loaded() && !vis_available(VIS_SPECTRUM)) {
        /* this music allows the meters only: the gadget is greyed */
        set_msg(mp_is_stream() ? _("No spectrum under MHI: it plays the MP3 itself")
                               : _("No spectrum: this player does not say what it plays"));
        return;
    }
    {
        /* the next this screen can show (Flow on graphics cards; nothing
         * playing yet, all of them) */
        int v = vis_selected(), k;
        for (k = 0; k < VIS_COUNT; k++) {
            v = (v + 1) % VIS_COUNT;
            if (v != VIS_FLOW || vf_possible(&g))
                break;
        }
        vis_select(v);
    }
    save_setting(VISUAL_VAR, vis_selected() == VIS_SPECTRUM ? "spectrum"
                 : vis_selected() == VIS_FLOW ? "flow" : "meters");
    set_msg(vis_selected() == VIS_FLOW ? _("Flow: F shows it on the whole screen")
            : _(vis_name(vis_selected())));
    ui->np_visual();
}

static void load_jump(void)
{
    char buf[16];                       /* ("crossfade") */
    jump_scroll = GetVar((CONST_STRPTR)JUMP_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
                  buf[0] == '1';
    anim_classic = GetVar((CONST_STRPTR)ANIMSCR_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
                   !strcmp(buf, "own");
    video_cpu = GetVar((CONST_STRPTR)VDEC_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
                (buf[0] == 'c' || buf[0] == 'C');
    video_no_overlay = GetVar((CONST_STRPTR)VOVL_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
                       !strcmp(buf, "off");
    if (GetVar((CONST_STRPTR)TRANS_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        int i;
        for (i = 0; i < VT_COUNT; i++)
            if (!strcmp(buf, trans_keys[i]))
                view_transition = i;
    }
}

/* Settings > Double buffering (RTG screens): on, by the setting; without
 * one, the old way (PROGDIR:Ambiance.dbuf there). */
static void load_dbuf(void)
{
    char buf[8];
    if (GetVar((CONST_STRPTR)DBUF_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        gfx_dbuf_wanted = buf[0] == '1';
    } else {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR oldwin = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        l = Lock((CONST_STRPTR)"PROGDIR:Ambiance.dbuf", SHARED_LOCK);
        me->pr_WindowPtr = oldwin;
        if (l)
            UnLock(l);
        gfx_dbuf_wanted = l != 0;
    }
}

static void load_https(void)
{
    char buf[8];
    radio_https = GetVar((CONST_STRPTR)HTTPS_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
                  buf[0] == '1';
}   /* told to the processes once AmiSSL is looked for (https_apply) */

static void load_debug(void)
{
    char buf[8];
    debug_on = GetVar((CONST_STRPTR)DEBUG_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
               buf[0] == '1';
}

static struct Item *action_item(struct Page *p, const char *label, const char *tag, UBYTE act,
                                UBYTE mark)
{
    struct Item *it = list_add(&p->list, label, tag, "", IT_ACTION);
    if (it) {
        it->action = act;
        it->mark = mark;
    }
    return it;
}

/* A heading over a group of rows (one choice of several in each): shown
 * dim at the left, passed over by the cursor. */
static void heading_item(struct Page *p, const char *label)
{
    struct Item *it = list_add(&p->list, label, "", "", IT_TEXT);
    if (it)
        it->flags |= ITEM_HEAD;
}

BOOL is_heading(struct Page *p, LONG i)
{
    return i >= 0 && i < p->list.count && (p->list.items[i].flags & ITEM_HEAD);
}

/* The selection off a heading: on to the next row in direction d, else
 * back the other way. */
static void sel_off_heading(struct Page *p, LONG d)
{
    LONG s = p->sel;
    while (is_heading(p, s))
        s += d;
    if (s < 0 || s >= p->list.count)
        for (s = p->sel; is_heading(p, s); s -= d)
            ;
    if (s >= 0 && s < p->list.count)
        p->sel = s;
}

enum { SETP_ROOT = 1, SETP_GENERAL, SETP_SCREEN, SETP_SOUND, SETP_NET, SETP_PLEX, SETP_NIC,
       SETP_RADIO, SETP_RKBPS, SETP_BUF, SETP_RSRV, SETP_FOLDSET, SETP_LANG, SETP_FOLDERS,
       SETP_VIDEO, SETP_IGAME, SETP_WHD, SETP_FONTS, SETP_FULLVIS, SETP_JF };
/* Each Settings page's title, by SETP_* (made again in another language) */
static const char *const setp_titles[] = { NULL, N_("Settings"), N_("General"), N_("Display"),
    N_("Sound"), N_("Networking"), N_("Plex"), N_("Network interface"), N_("Radio"),
    N_("Highest station bitrate"), N_("Stream buffer"), N_("Station list server"),
    N_("Folders"), N_("Language"), N_("Folders"),      /* SETP_FOLDSET: titled by its kind */
    N_("Video"), N_("iGame"), N_("WHDLoad"), N_("Fonts"),
    N_("Full screen visual"), "Jellyfin" };
static void build_lang_settings(struct Page *p);
static void build_radio_settings(struct Page *p);
static void build_rkbps_settings(struct Page *p);
static void build_buf_settings(struct Page *p);
static void build_rsrv_settings(struct Page *p);
static void build_general_settings(struct Page *p);
static void build_net_settings(struct Page *p);
static void build_nic_settings(struct Page *p);
static void build_jf_settings(struct Page *p);

/* ---- the language: the Workbench's (Locale prefs), English, or one
 * whose catalog is in PROGDIR:Catalogs (lang.c) */
#define LANG_VAR "Ambiance/Language"    /* "" the Workbench's, "english", or a language */
static char lang_set[32];

/* A language's name as it calls itself ("deutsch": Deutsch). */
static void lang_name(const char *lang, char *out)
{
    if (!lang || !lang[0] || !amb_stricmp(lang, "english")) {
        strcpy(out, "English");
        return;
    }
    if (!amb_stricmp(lang, "nihongo")) {
        /* the Japanese catalog's own name for its language, which no
         * Workbench 3.x has: "Nihongo" in its own characters (cjk.c's
         * three bytes each) */
        strcpy(out, "\214\267\245\214\274\254\221\252\236");
        return;
    }
    if (!amb_stricmp(lang, "korean")) {
        /* "Hangugeo" in Hangul (cjk.c's three bytes each) */
        strcpy(out, "\232\265\234\225\265\255\230\266\264");
        return;
    }
    if (!amb_stricmp(lang, "greek") || !amb_stricmp(lang, "ellinika")) {
        /* "Ellinika" in Greek letters (cjk.c's two bytes each) */
        strcpy(out, "\200\225\200\273\200\273\200\267\200\275\200\271\200\272\200\254");
        return;
    }
    amb_strlcpy(out, lang, 32);
    if ((UBYTE)out[0] >= 'a' && (UBYTE)out[0] <= 'z')
        out[0] -= 32;
}

/* A language's name in the language shown ("deutsch": German, or
 * Allemand in French), "" for one Ambiance does not know. */
static const char *lang_here(const char *lang)
{
    static const char *const names[][2] = {
        { "english", N_("English") }, { "deutsch", N_("German") },
        { "fran\347ais", N_("French") }, { "italiano", N_("Italian") },
        { "espa\361ol", N_("Spanish") }, { "svenska", N_("Swedish") },
        { "suomi", N_("Finnish") }, { "dansk", N_("Danish") },
        { "norsk", N_("Norwegian") }, { "nederlands", N_("Dutch") },
        { "portugu\352s", N_("Portuguese") }, { "nihongo", N_("Japanese") },
        { "greek", N_("Greek") }, { "ellinika", N_("Greek") }, { "korean", N_("Korean") },
    };
    unsigned i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!amb_stricmp(lang, names[i][0]))
            return _(names[i][1]);
    return "";
}

/* A row of the language list: the language by its own name, which its
 * readers find whatever is shown now, and beside it its name in the
 * language shown, where that differs. */
static void lang_row(struct Page *p, const char *lang, int action, BOOL on)
{
    char own[32];
    const char *here = lang_here(lang);
    lang_name(lang, own);
    action_item(p, own, amb_stricmp(own, here) ? here : "", action,
                on ? MARK_RADIO_ON : MARK_RADIO_OFF);
}

/* Settings > General > Language. */
static void build_lang_settings(struct Page *p)
{
    char t[32];
    int i;
    p->setp = SETP_LANG;
    amb_lang_scan();                   /* a catalog put in meanwhile is here too */
    lang_name(amb_lang_wb(), t);
    action_item(p, _("As Workbench"), t, ACT_LANGWB, !lang_set[0] ? MARK_RADIO_ON : MARK_RADIO_OFF);
    lang_row(p, "english", ACT_LANGEN, !amb_stricmp(lang_set, "english"));
    for (i = 0; i < amb_lang_count() && i < 16; i++)
        lang_row(p, amb_lang_name(i), ACT_LANG0 + i, !amb_stricmp(lang_set, amb_lang_name(i)));
}

/* Settings: General, Display, Video, Sound, Folders, Networking (Plex
 * inside it, as it needs the network). */
static void build_settings(struct Page *p)
{
    p->setp = SETP_ROOT;
    action_item(p, _("General"), "", ACT_GENSET, MARK_ARROW);
    action_item(p, _("Display"), g.wb ? _("Workbench window") : gfx_mode_label(g.mode),
                ACT_SCREENSET, MARK_ARROW);
    action_item(p, _("Video"), caps.zz9000 && !video_cpu ? "ZZ9000" : _("CPU"), ACT_VIDEOSET,
                MARK_ARROW);
    action_item(p, _("Sound"), sound_ahi ? "AHI" : sound_p14 ? _("Paula, 14-bit") : "Paula", ACT_SOUNDSET,
                MARK_ARROW);
    action_item(p, _("Folders"), "", ACT_FOLDERS, MARK_ARROW);
    action_item(p, _("Networking"), !caps.net ? _("no TCP/IP stack") : net_if[0] ? net_if : "",
                ACT_NETSET, MARK_ARROW);
}

/* Settings > Display: the screen mode, the style (its font on a laced
 * screen), double buffering where it is offered, the visualisation, and
 * jump scrolling (how long lists move). */
static const char *const tsize_names[TSIZE_COUNT] = { N_("Small"), N_("Medium"), N_("Large"),
                                                      N_("Extra large") };

/* Is the Window row offered: Workbench can take the window, or it is in one. */
static BOOL window_offered(void)
{
    return g.wb || !gfx_window_problem(&caps);
}

/* Settings > Display's rows, in order: the Window row where offered, then
 * Text size (a window, smooth) or Screen mode (full screen), then Style. */
static int screen_row_window(void) { return window_offered() ? 0 : -1; }
static int screen_row_mode(void) { return window_offered() ? 1 : 0; }  /* (full screen) */
static int screen_row_fonts(void) { return (window_offered() ? 1 : 0) + (g.wb ? 0 : 1); }
/* on Settings > Display > Fonts: the text size's row (a window, smooth),
 * and the style's after it */
static int fonts_row_size(void) { return 0; }
static int fonts_row_style(void) { return g.wb && g.style == STYLE_SMOOTH ? 1 : 0; }

/* What the Fonts row says: the style, and a window's text size */
static const char *fonts_tag(void)
{
    static char tag[64];
    if (gfx_laced(g.mode) || (gfx_mode_is_rtg(g.mode) && g.style != STYLE_SMOOTH))
        amb_strlcpy(tag, gfx_laced_fonts[gfx_laced(g.mode) ? gfx_laced_font : gfx_font_shown(&g)].name,
                    sizeof(tag));
    else
        amb_strlcpy(tag, g.style == STYLE_SMOOTH ? _("Smooth") : _("Pixel"), sizeof(tag));
    if (g.wb && g.style == STYLE_SMOOTH) {
        amb_strlcat(tag, ", ", sizeof(tag));
        amb_strlcat(tag, _(tsize_names[gfx_window_tsize()]), sizeof(tag));
    }
    return tag;
}

/* Settings > Display > Fonts: a window's text size (smooth; the pixel
 * style's size is its font), and the style */
static void build_font_settings(struct Page *p)
{
    struct Item *it;
    p->setp = SETP_FONTS;
    if (g.wb && g.style == STYLE_SMOOTH)
        action_item(p, _("Text size"), _(tsize_names[gfx_window_tsize()]), ACT_TSIZES, MARK_ARROW);
    /* Style: the font on a laced screen; greyed out, showing Pixel, where
     * this screen can only be pixel (other native modes, 8-bit RTG) */
    if (gfx_laced(g.mode)) {
        action_item(p, _("Style"), gfx_laced_fonts[gfx_laced_font].name, ACT_STYLES, MARK_ARROW);
    } else if (gfx_mode_is_rtg(g.mode)) {
        /* RTG: Smooth, or the pixel style's font */
        action_item(p, _("Style"), g.style == STYLE_SMOOTH ? _("Smooth")
                                   : gfx_laced_fonts[gfx_font_shown(&g)].name,
                    ACT_STYLES, MARK_ARROW);
    } else if (gfx_style_possible(&caps, g.mode, STYLE_SMOOTH)) {
        action_item(p, _("Style"), ui_style == STYLE_SMOOTH ? _("Smooth") : _("Pixel"), ACT_STYLES,
                    MARK_ARROW);
    } else if ((it = action_item(p, _("Style"), _("Pixel"), ACT_STYLES, MARK_ARROW))) {
        it->flags |= ITEM_DISABLED;
    }
}

static BOOL depth_offered(void);
static void build_screen_settings(struct Page *p)
{
    char tag[64];
    struct Item *it;
    p->setp = SETP_SCREEN;
    if (window_offered())
        action_item(p, _("Workbench window"), "", ACT_WINDOW,    /* (two ways: a checkbox) */
                    g.wb ? MARK_CHECK_ON : MARK_CHECK_OFF);
    if (!g.wb) {
        amb_strlcpy(tag, gfx_mode_label(g.mode), sizeof(tag));
        action_item(p, _("Screen mode"), tag, ACT_SCREENMODES, MARK_ARROW);
    }
    action_item(p, _("Fonts"), fonts_tag(), ACT_FONTS, MARK_ARROW);
    if (depth_offered()) {
        sprintf(tag, _("%ld-bit"), (long)gfx_bits(&g));
        action_item(p, _("Colour depth"), tag, ACT_DEPTHS, MARK_ARROW);
    }
    if (!g.planar && !g.wb)             /* native screens draw straight on, a window
                                         * always through its buffer: not offered */
        action_item(p, _("Double buffering"), "", ACT_DBUF,
                    gfx_dbuf_wanted ? MARK_CHECK_ON : MARK_CHECK_OFF);
    /* a layout has colours of its own: the scheme is not used while it
     * draws, so the row is greyed out, saying whose they are */
    if ((it = action_item(p, _("Colours"), lv_on() ? lv_label() : _(ui_scheme_names[ui_scheme]),
                          ACT_COLOURS, MARK_ARROW)) && lv_on())
        it->flags |= ITEM_DISABLED;
    if (gfx_mode_is_rtg(g.mode) || lv_native)
        action_item(p, _("Layout"), lv_label(), ACT_LAYOUTS, MARK_ARROW);
    if (!g.wb)
        action_item(p, _("Layouts on native screens (experimental)"), "", ACT_LVNATIVE,
                    lv_native ? MARK_CHECK_ON : MARK_CHECK_OFF);
    action_item(p, _("Visualisation"), _(vis_name(vis_selected())), ACT_VISUAL, MARK_CYCLE);
    fullvis_scan();
    /* AmigaAMP plug-ins to choose from: the row goes round them and Flow;
     * none: Flow is all there is, and the row says how to add some */
    action_item(p, _("Full screen visual (F)"), fullvis[0] ? (const char *)FilePart((STRPTR)fullvis) : _("Flow"),
                fv_n ? ACT_FULLVIS : ACT_FULLVISHELP, fv_n ? MARK_CYCLE : MARK_ARROW);
    action_item(p, _("Jump scrolling (by letter)"), "", ACT_JUMP,
                jump_scroll ? MARK_CHECK_ON : MARK_CHECK_OFF);
    /* a slideshow's way from one picture to the next */
    action_item(p, _("Slideshow transition"), _(trans_names[view_transition]), ACT_TRANS,
                MARK_CYCLE);
}

/* Settings > Video: where IFF ANIMs play, who decodes MPEG video, and
 * whether the CPU's pictures go to the card's overlay. */
static void build_video_settings(struct Page *p)
{
    p->setp = SETP_VIDEO;
    /* videos (IFF ANIMs, from Videos or Pictures): on this screen (a 16-bit
     * RTG one, or the window), or on one of their own */
    {
        struct Item *it = action_item(p, _("Videos on a screen of their own"), "", ACT_ANIMSCR,
                                      !g.truecolor || anim_classic ? MARK_CHECK_ON : MARK_CHECK_OFF);
        if (it && !g.truecolor)
            it->flags |= ITEM_DISABLED;     /* a native screen: always their own */
    }
    /* MPEG video: decoded by the ZZ9000 where there is one (off: by the
     * CPU, mpegv.library, as without one) */
    if (caps.zz9000)
        action_item(p, _("MPEG video on the ZZ9000"), "", ACT_ZZVIDEO,
                    video_cpu ? MARK_CHECK_OFF : MARK_CHECK_ON);
    /* MPEG decoded by the CPU: its pictures on the graphics card's YUV
     * overlay (scaled by the card), else drawn; no card, no overlay */
    {
        struct Item *it = action_item(p, _("Use overlay when available (RTG)"), "", ACT_VOVERLAY,
                                      caps.rtg && !video_no_overlay ? MARK_CHECK_ON : MARK_CHECK_OFF);
        if (it && !caps.rtg)
            it->flags |= ITEM_DISABLED;
    }
}

/* Settings > General. */
static void build_general_settings(struct Page *p)
{
    char t[32];
    struct Item *it;
    p->setp = SETP_GENERAL;
    amb_lang_scan();                   /* a catalog put in meanwhile counts */
    lang_name(amb_lang_now(), t);
    /* only a choice with a catalog besides English: else just what is used */
    if ((it = action_item(p, _("Language"), t, ACT_LANGSET,
                          amb_lang_count() ? MARK_ARROW : MARK_NONE)) && !amb_lang_count())
        it->flags |= ITEM_DISABLED;
    action_item(p, _("Debug (CPU, memory, log)"), "", ACT_DEBUG,
                debug_on ? MARK_CHECK_ON : MARK_CHECK_OFF);
}

/* The interface in use (the stack's list asked again): its name and
 * address into out, as "eth0, 192.168.1.10"; "" not known. */
static void nic_in_use_text(char *out)
{
    char ip[20];
    int i;
    out[0] = 0;
    nnic = net_interfaces(nics, 8, plex.host, &nic_in_use);
    if (!nic_in_use)
        return;
    ip_text(nic_in_use, ip);
    for (i = 0; i < nnic; i++)
        if (nics[i].ip == nic_in_use) {
            sprintf(out, "%.19s, %s", nics[i].name, ip);
            return;
        }
    strcpy(out, ip);
}

/* Settings > Networking: HTTPS, Plex, and the interface connections go out
 * from (its own page, as Screen mode has: the one in use shown here). */
static void build_net_settings(struct Page *p)
{
    struct Item *it;
    char tag[96], use[40];
    p->setp = SETP_NET;
    (void)it;
    if (!caps.net) {
        list_add(&p->list, _("No TCP/IP stack running"), "", "", IT_TEXT);
        return;
    }
    if ((it = action_item(p, _("HTTPS (secure connections)"),
                          caps.amissl ? _("radio stations, plex.tv") : _("needs AmiSSL"), ACT_HTTPS,
                          https_ok() ? MARK_CHECK_ON : MARK_CHECK_OFF)) && !caps.amissl)
        it->flags |= ITEM_DISABLED;     /* secure connections need AmiSSL */
    rkbps_text(rkbps_now(), use);
    sprintf(tag, _("stations up to %s"), use);
    action_item(p, _("Radio"), tag, ACT_RADIOSET, MARK_ARROW);
    action_item(p, "Plex", !plex.on ? _("Off") : plex.name[0] ? plex.name
                           : plex.host[0] ? plex.host : _("On, no server chosen"),
                ACT_PLEXSET, MARK_ARROW);
    action_item(p, "Jellyfin", !jf.on ? _("Off") : !jf.host[0] ? _("On, no server chosen")
                               : !jf.token[0] ? _("Not signed in")
                               : jf.name[0] ? jf.name : jf.host,
                ACT_JFSET, MARK_ARROW);
    action_item(p, _("Stream buffer"), _(buf_names[buf_sel]), ACT_BUFSET, MARK_ARROW);
    nic_in_use_text(use);
    if (net_if[0])
        sprintf(tag, "%.40s", use[0] ? use : net_if);
    else
        sprintf(tag, use[0] ? _("Automatic: %.40s") : _("Automatic"), use);
    action_item(p, _("Network interface"), tag, ACT_NICSET, MARK_ARROW);
}

/* Settings > Networking > Network interface: Automatic (the stack's
 * choice, the one it uses beside it) or one the stack lists. */
static void build_nic_settings(struct Page *p)
{
    struct Item *it;
    char label[64], tag[128], ip[20], use[40];
    int i;
    p->setp = SETP_NIC;
    nic_in_use_text(use);
    action_item(p, _("Automatic (the stack's choice)"), net_if[0] ? "" : use, ACT_NICAUTO,
                net_if[0] ? MARK_RADIO_OFF : MARK_RADIO_ON);
    for (i = 0; i < nnic && i < 8; i++) {
        struct NetIf *f = &nics[i];
        ip_text(f->ip, ip);
        sprintf(label, "%.19s  %s", f->name, f->ip ? ip : _("no address"));
        if (f->dev[0])
            sprintf(tag, _("%s, %.39s unit %ld"), f->up ? _("up") : _("down"), f->dev, (long)f->unit);
        else
            strcpy(tag, f->up ? "up" : "down");
        if ((it = action_item(p, label, tag, ACT_NIC0 + i,
                              !strcmp(net_if, f->name) ? MARK_RADIO_ON : MARK_RADIO_OFF)) &&
            (!f->up || !f->ip))
            it->flags |= ITEM_DISABLED;
        if (!strcmp(net_if, f->name))
            p->sel = p->list.count - 1;
    }
    if (nnic < 0)
        list_add(&p->list, _("This TCP/IP stack does not list its interfaces"), "", "", IT_TEXT);
}

/* Settings > Networking > Radio: the highest bitrate listed, and the
 * server the station lists come from (HTTPS: Settings > Networking). */
static void build_radio_settings(struct Page *p)
{
    char tag[80], k[16], nm[64];
    p->setp = SETP_RADIO;
    rkbps_text(rkbps_now(), k);
    if (rkbps_sel == 0)
        sprintf(tag, _("Automatic: %s"), k);
    else
        strcpy(tag, k);
    action_item(p, _("Highest station bitrate"), tag, ACT_RKBPSSET, MARK_ARROW);
    if (!rsrv[0]) {
        strcpy(tag, _("Automatic"));
    } else if (!strncmp(rsrv, "fastest:", 8)) {
        mirror_short(rsrv + 8, nm, sizeof(nm));
        sprintf(tag, _("Fastest: %s"), nm);
    } else {
        mirror_short(rsrv, tag, sizeof(tag));
    }
    action_item(p, _("Station list server"), tag, ACT_RSRVSET, MARK_ARROW);
}

static void build_rkbps_settings(struct Page *p)
{
    char tag[96], k[16];
    int i;
    p->setp = SETP_RKBPS;
    rkbps_text(rkbps_auto(), k);
    sprintf(tag, _("for this Amiga: %s"), k);
    action_item(p, _("Automatic"), tag, ACT_RKBPS0, rkbps_sel == 0 ? MARK_RADIO_ON : MARK_RADIO_OFF);
    for (i = 1; i < 6; i++) {
        char label[64];
        if (rkbps_choice[i])
            sprintf(label, _("Up to %lukbps"), (unsigned long)rkbps_choice[i]);
        else
            strcpy(label, _("Any"));
        action_item(p, label, "", ACT_RKBPS0 + i,
                    rkbps_sel == i ? MARK_RADIO_ON : MARK_RADIO_OFF);
    }
    p->sel = rkbps_sel;
}

static void build_buf_settings(struct Page *p)
{
    int i;
    p->setp = SETP_BUF;
    for (i = 0; i < 3; i++)
        action_item(p, _(buf_names[i]), _(buf_notes[i]), ACT_BUF0 + i,
                    buf_sel == i ? MARK_RADIO_ON : MARK_RADIO_OFF);
    p->sel = buf_sel;
}

/* The fastest of the servers as timed (-1: none answered). */
static int mirror_fastest(void)
{
    int i, best = -1;
    for (i = 0; i < nmirror; i++)
        if (mirrors[i].ms >= 0 && (best < 0 || mirrors[i].ms < mirrors[best].ms))
            best = i;
    return best;
}

static void build_rsrv_settings(struct Page *p)
{
    char tag[112], nm[64];
    int i, f;
    BOOL fast = !strncmp(rsrv, "fastest:", 8);
    p->setp = SETP_RSRV;
    action_item(p, _("Automatic"), _("one at random, as radio-browser.info asks"), ACT_RSRVAUTO,
                rsrv[0] ? MARK_RADIO_OFF : MARK_RADIO_ON);
    if (nmirror < 0) {
        action_item(p, _("Fastest"), "", ACT_RSRVFAST, fast ? MARK_RADIO_ON : MARK_RADIO_OFF);
        list_add(&p->list, _("radio-browser.info's servers could not be found"), "", "", IT_TEXT);
        return;
    }
    f = mirror_fastest();
    if (f >= 0) {
        mirror_short(mirrors[f].name, nm, sizeof(nm));
        sprintf(tag, _("now %.30s, %ld ms"), nm, (long)mirrors[f].ms);
    } else {
        strcpy(tag, _("none answered"));
    }
    action_item(p, _("Fastest"), tag, ACT_RSRVFAST, fast ? MARK_RADIO_ON : MARK_RADIO_OFF);
    if (!rsrv[0])
        p->sel = 0;
    else if (fast)
        p->sel = 1;
    for (i = 0; i < nmirror; i++) {
        struct Item *it;
        if (mirrors[i].ms >= 0)
            sprintf(tag, "%ld ms", (long)mirrors[i].ms);
        else
            strcpy(tag, _("no answer"));
        mirror_short(mirrors[i].name, nm, sizeof(nm));
        it = action_item(p, nm, tag, ACT_RSRV0 + i,
                         !fast && !amb_stricmp(rsrv, mirrors[i].name) ? MARK_RADIO_ON
                                                                       : MARK_RADIO_OFF);
        if (it && !fast && !amb_stricmp(rsrv, mirrors[i].name))
            p->sel = p->list.count - 1;
    }
}

#define SETTINGS_SCREEN_ROW 1   /* "Display" in build_settings */
static void build_screen_settings(struct Page *p);

/* Settings > Plex: on or off, the server, and how the music is asked for. */
static void build_plex_settings(struct Page *p)
{
    struct Item *it;
    BOOL fast;
    p->setp = SETP_PLEX;
    action_item(p, "Plex", "", ACT_PLEXON, plex.on ? MARK_CHECK_ON : MARK_CHECK_OFF);
    if (!plex.on)
        return;
    if ((it = list_add(&p->list, _("Plex server"),
                       "#gdm", plex.host[0] ? (plex.name[0] ? plex.name : plex.host)
                                            : _("none chosen"), IT_PLEXDIR)))
        it->mark = MARK_ARROW;
    if (plex.account[0]) {
        char tag[48];
        sprintf(tag, "%.40s", plex.user[0] ? plex.user : _("signed in"));
        action_item(p, _("Sign out of plex.tv"), tag, ACT_PLEXOUT, MARK_NONE);
    } else if ((it = action_item(p, _("Sign in at plex.tv"),
                                 https_ok() ? "" : _("needs HTTPS (Settings > Networking)"),
                                 ACT_PLEXLINK, MARK_ARROW)) && !https_ok()) {
        it->flags |= ITEM_DISABLED;     /* plex.tv answers HTTPS alone */
    }
    /* the server's conversion: a choice only where the Amiga is fast enough
     * to decode the files as stored (plex_tier: how fast it really is, not
     * only its CPU); on others always on */
    fast = plex_tier() >= 2 || plex.mhi || plex.mp3_card;  /* MHI or the ZZ9000 decodes MP3 */
    if ((it = action_item(p, _("Request lower quality audio"), fast ? "" : _("needed on this Amiga"),
                          ACT_PLEXCONV, plex.convert || !fast ? MARK_CHECK_ON : MARK_CHECK_OFF)) &&
        !fast)
        it->flags |= ITEM_DISABLED;
}

/* Settings > Sound: what plays the sound and what decodes MP3. Paula and mpega.library are all there is yet;
 * AHI and MHI are shown, greyed, when installed (they come later, and
 * with AHI other rates than Paula's). */
static void head_para(struct Page *p, const char *s);
static void build_sound_settings(struct Page *p)
{
    struct Item *it;
    p->setp = SETP_SOUND;
    /* two choices of one each (where the sound goes, what decodes MP3),
     * so each under its own heading, or their radio buttons read as one */
    heading_item(p, _("Sound output"));
    action_item(p, "Paula (audio.device)", _("4 channels, up to 28kHz, 8-bit"), ACT_SND_PAULA,
                sound_ahi || sound_p14 ? MARK_RADIO_OFF : MARK_RADIO_ON);
    action_item(p, _("Paula, 14-bit: radio, Plex, music files"), _("all 4 channels, cleaner quiet passages"),
                ACT_SND_P14, sound_p14 && !sound_ahi ? MARK_RADIO_ON : MARK_RADIO_OFF);
    if ((it = action_item(p, _("AHI: radio, Plex, music files"),
                          caps.ahi ? _("16-bit, its Preferences' mode") : _("not installed"),
                          ACT_SND_AHI, sound_ahi ? MARK_RADIO_ON : MARK_RADIO_OFF)) && !caps.ahi)
        it->flags |= ITEM_DISABLED;
    heading_item(p, _("MP3 decoder"));
    if ((it = action_item(p, "MP3: ambmpega.library",
                          !caps.mpega_own ? _("not installed (Ambiance's Libs)")
                          : _("Ambiance's own, decoded here"),
                          ACT_SND_OWNMPEGA, !mhi_sel[0] && own_mpega ? MARK_RADIO_ON : MARK_RADIO_OFF)) &&
        !caps.mpega_own)
        it->flags |= ITEM_DISABLED;
    if ((it = action_item(p, "MP3: mpega.library",
                          !caps.mp3_mpega ? _("not installed")
                          : caps.mpega_zz ? _("decoded on the ZZ9000") : _("decoded here"),
                          ACT_SND_MPEGA, !mhi_sel[0] && !own_mpega && caps.mp3_mpega ? MARK_RADIO_ON
                                                                                   : MARK_RADIO_OFF)) &&
        !caps.mp3_mpega)
        it->flags |= ITEM_DISABLED;
    {
        /* MHI: each driver in LIBS:MHI, decoding (and playing) MP3 itself */
        int i;
        if (!nmhi && (it = action_item(p, "MP3: MHI", _("not installed"), ACT_SND_MHI,
                                       MARK_RADIO_OFF)))
            it->flags |= ITEM_DISABLED;
        for (i = 0; i < nmhi; i++) {
            char label[64];
            sprintf(label, "MP3: MHI, %.40s", mhi_names[i]);
            action_item(p, label, _("decodes and plays"), ACT_SND_MHI0 + i,
                        !amb_stricmp(mhi_sel, mhi_names[i]) ? MARK_RADIO_ON : MARK_RADIO_OFF);
        }
    }
    /* MPEG audio's quality where the CPU decodes it (not MHI, not the
     * ZZ9000's mpega.library, which give their best) */
    heading_item(p, _("MP3 quality"));
    {
        /* the heading says what they are: the rows say which (three rows
         * each "MP3 quality: ..." read as three settings); MHI or the
         * ZZ9000 decoding: none to choose, a line saying so */
        static const char *const lab[3] = { N_("Automatic"), N_("High"), N_("Medium") };
        static const char *const sub[3] = { N_("by this Amiga's speed, measured each run"),
                                            N_("the most accurate"), N_("about a tenth less work") };
        BOOL here = !mhi_sel[0] && (own_mpega || !caps.mpega_zz);
        int i;
        if (!here)
            head_para(p, _("MHI and the ZZ9000's mpega.library decode MP3 at their best"));
        else
            for (i = 0; i < 3; i++)
                action_item(p, _(lab[i]), _(sub[i]), ACT_SND_MQ0 + i,
                            mp3_q == i ? MARK_RADIO_ON : MARK_RADIO_OFF);
    }
    /* FLAC has nothing to choose: flacdec.library is used where it is
     * installed, and whether this Amiga is fast enough is measured at
     * each start (plex_tier), so one system image suits a slow Amiga
     * and a fast one. What it found is in About > System information. */
}

/* How many places a kind is found in: its folders, its assign, and for
 * modules and pictures AGS's own drawers that are there. */
static int fs_places(int k)
{
    int n = fs[k].n + (fs_assign_used(k) ? 1 : 0);
    if (k == FS_MODS)
        n += place_there(MODS_ROOT) + place_there(MODS_SID) + place_there(MODS_MENU);
    else if (k == FS_PICS)
        n += place_there(PICS_ROOT);
    else if (k == FS_VIDEOS)
        n += place_there(VIDS_ROOT) + place_there(VIDS_ROOT2);
    return n;
}

/* Settings > Folders: where Music, Mods, Pictures and Videos find their files. */
static const char *ig_tag(void);

static void build_folders_settings(struct Page *p)
{
    int k;
    p->setp = SETP_FOLDERS;
    for (k = 0; k < FS_COUNT; k++) {
        char tag[48];
        int n = fs_places(k);
        if (n)
            sprintf(tag, n == 1 ? _("%ld place") : _("%ld places"), (long)n);
        else
            strcpy(tag, _("none yet"));
        action_item(p, _(fs[k].name), tag, ACT_FSSET0 + k, MARK_ARROW);
    }
    action_item(p, _("iGame"), ig_tag(), ACT_IGSET, MARK_ARROW);
    action_item(p, "WHDLoad", whd_present(NULL, 0) ? _("installed") : _("not installed"),
                ACT_WHDPAGE, MARK_ARROW);
}

/* Settings > Folders > Music (Modules, Pictures): the kind's assign (on or
 * off) and the folders added; Fire twice on a folder takes it out. AGS's
 * own places are not listed: they are used whenever they are there. */
static int fs_rm = -1;              /* the folder Fire was pressed on once */

static void build_folder_settings(struct Page *p)
{
    struct Folders *f = &fs[fs_page];
    char label[48];
    int i;
    p->setp = SETP_FOLDSET;
    sprintf(label, _("Use the %s assign"), f->assign);
    action_item(p, label, place_there(f->assign) ? _("assigned") : _("not assigned now"),
                ACT_FSASSIGN, f->use_assign ? MARK_CHECK_ON : MARK_CHECK_OFF);
    for (i = 0; i < f->n; i++)
        action_item(p, f->dir[i], fs_rm == i ? _("Fire again: remove") : "", ACT_FSDIR0 + i, MARK_NONE);
    action_item(p, _("Add a folder..."), f->n < FS_DIRS ? "" : _("eight at most"), ACT_FSADD, MARK_NONE);
}

/* ------------------------------------------------------------ iGame's list
 * Ambiance/iGame says where iGame's drawer is (its gameslist.csv), or
 * "none" (or "n/a") for no list. Unset, iGame is looked for at each start
 * (igame.c), and the drawer found is saved there: from then on it is the
 * setting, changed or turned off in Settings > Folders > iGame. Its games
 * and demos are listed beside AGS's, or alone where there is no AGS. */
static BOOL ig_off(void)
{
    return !amb_stricmp(ig_setting, "none") || !amb_stricmp(ig_setting, "n/a");
}

/* iGame's list read again as the setting says; unset, and look: iGame
 * looked for, and the drawer found saved as the setting. */
static void igame_start(BOOL look)
{
    char found[256];
    ig_free(&igcat);
    ig_setting[0] = 0;
    if (GetVar((CONST_STRPTR)IG_VAR, (STRPTR)ig_setting, sizeof(ig_setting), GVF_GLOBAL_ONLY) <= 0)
        ig_setting[0] = 0;
    if (ig_off()) {
        amb_log("igame", "set to none: no list");
        return;
    }
    if (!ig_setting[0]) {
        /* the hard disks searched the first time only; the usual places each start */
        char looked[4];
        BOOL deep = GetVar((CONST_STRPTR)IG_LOOKED_VAR, (STRPTR)looked, sizeof(looked),
                           GVF_GLOBAL_ONLY) <= 0;
        if (!look)
            return;
        if (deep)
            save_setting(IG_LOOKED_VAR, "1");
        if (!ig_discover(found, sizeof(found), deep))
            return;
        amb_strlcpy(ig_setting, found, sizeof(ig_setting));
        save_setting(IG_VAR, found);
    }
    ig_load(&igcat, ig_setting);
}

/* Folders' iGame row: what it lists, or why nothing. */
static const char *ig_tag(void)
{
    static char tag[48];
    if (ig_off())
        strcpy(tag, _("none"));
    else if (!ig_setting[0])
        strcpy(tag, _("not found"));
    else if (igcat.n)
        sprintf(tag, igcat.n == 1 ? _("%ld entry") : _("%ld entries"), (long)igcat.n);
    else
        strcpy(tag, _("no list there"));
    return tag;
}

static void build_igame_settings(struct Page *p)
{
    char line[320];
    p->setp = SETP_IGAME;
    if (ig_off())
        strcpy(line, _("No iGame list is shown."));
    else if (!ig_setting[0])
        strcpy(line, _("iGame was not found."));
    else if (igcat.n)
        sprintf(line, _("%.200s: %ld games, %ld demos"), ig_setting, (long)igcat.games,
                (long)igcat.demos);
    else
        sprintf(line, _("%.200s: no gameslist.csv read"), ig_setting);
    heading_item(p, line);
    action_item(p, _("Choose iGame's drawer..."), "", ACT_IGCHOOSE, MARK_NONE);
    action_item(p, _("Find iGame"), "", ACT_IGFIND, MARK_NONE);
    action_item(p, _("None: no iGame list"), "", ACT_IGNONE, ig_off() ? MARK_CHECK_ON : MARK_CHECK_OFF);
}

/* ------------------------------------------------------------ WHDLoad
 * Without AGS (which carries its own) and without WHDLoad in C:, on an
 * Amiga with its network up, Games' and Demos' first pages begin with
 * Install WHDLoad... until it is installed or Not now is chosen; Settings
 * > Folders > WHDLoad always leads to the same page. Download and install
 * fetches the user package from whdload.de into T: (net.c writes it to
 * the file as it comes), unpacks it (unarc.c: xadmaster.library, else
 * C:LhA) and puts WHDLoad in C: (whdinst.c). */
static struct NetJob *whd_job;      /* the package on its way, NULL none */
static void plex_settings_again(BOOL full);
static ULONG whd_shown = ~0UL;      /* bytes last said in the footer */

static BOOL whd_declined(void)
{
    char v[8];
    return GetVar((CONST_STRPTR)WHD_OFFER_VAR, (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0 &&
           !amb_stricmp(v, "no");
}

/* Games and Demos offer it: network up, no AGS, no WHDLoad, not declined. */
static BOOL whd_offer(void)
{
    return caps.net && !ags.found && !whd_present(NULL, 0) && !whd_declined();
}

/* A paragraph as dim rows of a list page, passed over by the cursor. */
static void head_para(struct Page *p, const char *s)
{
    LONG i, from = p->list.count;
    text_para(p, s);
    for (i = from; i < p->list.count; i++)
        p->list.items[i].flags |= ITEM_HEAD;
}

/* Settings > Display > Full screen visual, with no plug-ins to choose:
 * what F does, and how to have more */
static void build_fullvis_page(struct Page *p)
{
    p->setp = SETP_FULLVIS;
    head_para(p, _("F on Now playing shows Flow, Ambiance's own full screen visual."));
    head_para(p, _("AmigaAMP's visual plug-ins can be used instead: put them in Ambiance's Visuals "
                   "drawer (or AmigaAMP:Plugins), then choose one here. Aminet has them in mus/play "
                   "(AmigaAMP-VisualPlugins and others)."));
    head_para(p, _("Most open on Workbench's screen, so they want what it can show: a graphics card's "
                   "screen for most, Warp3D or a PowerPC for some."));
}

static void build_whd_page(struct Page *p)
{
    char where[64], line[160];
    int how = unarc_how();
    p->setp = SETP_WHD;
    if (whd_present(where, sizeof(where))) {
        sprintf(line, _("WHDLoad is installed: %s"), where);
        heading_item(p, line);
        head_para(p, _("Games that need a Kickstart image look for it in Devs:Kickstarts "
                       "(kick34005.A500 for 1.3, kick40068.A1200 for 3.1). Ambiance cannot "
                       "supply those: they come from your own Amiga's ROM, or from Amiga Forever."));
        return;
    }
    head_para(p, _("WHDLoad starts games and demos installed to hard disk, as iGame lists them. "
                   "It is not installed on this Amiga."));
    head_para(p, _("Ambiance can fetch WHDLoad's user package from its author's site, "
                   "whdload.de (free of charge), and put WHDLoad and its tools in C:."));
    if (how == UNARC_NONE)
        head_para(p, _("Unpacking it needs xadmaster.library (Aminet: util/arc/xadmaster000.lha) "
                       "or LhA in C: (Aminet: util/arc/lha.run). Neither is installed."));
    if (!caps.net)
        heading_item(p, _("The network is not up."));
    if (whd_job) {
        if (whd_job->size)
            sprintf(line, _("Downloading: %lu of %lu KB"), (unsigned long)(whd_job->got >> 10),
                    (unsigned long)(whd_job->size >> 10));
        else
            sprintf(line, _("Downloading: %lu KB"), (unsigned long)(whd_job->got >> 10));
        heading_item(p, line);
    } else if (caps.net && how != UNARC_NONE)
        action_item(p, _("Download and install WHDLoad"), WHD_HOST, ACT_WHDGO, MARK_NONE);
    if (!whd_declined())
        action_item(p, _("Not now"), _("Games stops asking"), ACT_WHDNO, MARK_NONE);
}

/* The Games or Demos page's first row: the offer. */
static void whd_offer_row(struct Page *p)
{
    struct Item *it;
    if (!whd_offer())
        return;
    it = list_add_first(&p->list, _("Install WHDLoad..."), _("not installed"), "", IT_ACTION);
    if (it) {
        it->action = ACT_WHDPAGE;
        it->mark = MARK_ARROW;
    }
}

static void whd_go(void)
{
    struct NetJob *j;
    if (whd_job)
        return;
    if (!caps.net) {
        set_msg(_("The network is not up"));
        return;
    }
    if (unarc_how() == UNARC_NONE) {
        set_msg(_("Unpacking needs xadmaster.library or C:LhA"));
        return;
    }
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        set_msg(_("Not enough memory"));
        return;
    }
    whd_cleanup();                          /* a try before, left behind */
    amb_strlcpy(j->host, WHD_HOST, sizeof(j->host));
    amb_strlcpy(j->path, WHD_PATH, sizeof(j->path));
    amb_strlcpy(j->file, WHD_ARCHIVE, sizeof(j->file));
    j->tls = https_ok();                    /* HTTPS where it can be; else HTTP */
    j->wait_s = 30;
    whd_job = j;
    whd_shown = ~0UL;
    amb_log("whdload: fetching", j->tls ? "https://" WHD_HOST WHD_PATH : "http://" WHD_HOST WHD_PATH);
    net_submit(j);
    set_msg(_("Downloading WHDLoad from whdload.de..."));
    plex_settings_again(FALSE);
}

/* Once a second while it comes: how much, in the footer. */
static void whd_tick(void)
{
    char m[80];
    if (!whd_job || whd_job->got == whd_shown)
        return;
    whd_shown = whd_job->got;
    if (whd_job->size)
        sprintf(m, _("Downloading WHDLoad: %lu of %lu KB"), (unsigned long)(whd_shown >> 10),
                (unsigned long)(whd_job->size >> 10));
    else
        sprintf(m, _("Downloading WHDLoad: %lu KB"), (unsigned long)(whd_shown >> 10));
    set_msg(m);
}

/* The package here (or not): unpacked, installed, tidied away. */
static void whd_done(struct NetJob *j)
{
    char why[120], done[120], m[200];
    BOOL ok = FALSE;
    whd_job = NULL;
    if (!j->body || j->err[0]) {
        sprintf(m, _("WHDLoad could not be downloaded: %.100s"),
                j->err[0] ? j->err : _("no answer"));
        amb_log("whdload", m);
        set_msg(m);
    } else {
        sprintf(m, "%lu bytes from %.60s in %lu ms", (unsigned long)j->got, j->server,
                (unsigned long)j->ms);
        amb_log("whdload: downloaded", m);
        busy_begin();
        set_msg(_("Unpacking WHDLoad..."));
        ok = unarc_extract(WHD_ARCHIVE, WHD_DIR, why, sizeof(why)) &&
             whd_install_from(WHD_DIR, why, sizeof(why), done, sizeof(done));
        whd_cleanup();
        busy_end(TRUE);
        if (ok) {
            amb_log("whdload", done);
            set_msg(done);
        } else {
            sprintf(m, _("WHDLoad was not installed: %.120s"), why);
            amb_log("whdload", m);
            set_msg(m);
        }
    }
    if (j->body)
        FreeVec(j->body);
    FreeVec(j);
    plex_settings_again(FALSE);             /* its page: installed, or the button again */
}

static void ig_said(void)
{
    char m[120];
    if (igcat.n)
        sprintf(m, _("iGame's list: %ld games, %ld demos"), (long)igcat.games, (long)igcat.demos);
    else
        strcpy(m, _("iGame's list could not be read"));
    set_msg(m);
}

void draw_footer(void);
void draw_list_rows(struct Page *p);

/* A setting changed: the Settings pages open built again (the selections
 * kept): Settings itself says what is chosen in its rows. */
/* Every Settings page open built again, each keeping its selection. */
static void settings_rebuild(void)
{
    int d;
    for (d = 0; d < depth; d++) {
        struct Page *q = &pages[d];
        LONG qs = q->sel;
        if (q->type != PT_LIST || !q->setp)
            continue;
        list_free(&q->list);
        list_init(&q->list);
        switch (q->setp) {
        case SETP_PLEX: build_plex_settings(q); break;
        case SETP_JF: build_jf_settings(q); break;
        case SETP_SOUND: build_sound_settings(q); break;
        case SETP_NET: build_net_settings(q); break;
        case SETP_GENERAL: build_general_settings(q); break;
        case SETP_SCREEN: build_screen_settings(q); break;
        case SETP_FONTS: build_font_settings(q); break;
        case SETP_FULLVIS: build_fullvis_page(q); break;
        case SETP_VIDEO: build_video_settings(q); break;
        case SETP_NIC: build_nic_settings(q); break;
        case SETP_RADIO: build_radio_settings(q); break;
        case SETP_RKBPS: build_rkbps_settings(q); break;
        case SETP_BUF: build_buf_settings(q); break;
        case SETP_RSRV: build_rsrv_settings(q); break;
        case SETP_FOLDSET: build_folder_settings(q); break;
        case SETP_FOLDERS: build_folders_settings(q); break;
        case SETP_LANG: build_lang_settings(q); break;
        case SETP_IGAME: build_igame_settings(q); break;
        case SETP_WHD: build_whd_page(q); break;
        default: build_settings(q); break;
        }
        if (q->setp == SETP_FOLDSET)
            amb_strlcpy(q->title, _(fs[fs_page].title), sizeof(q->title));
        else if (q->setp < (int)(sizeof(setp_titles) / sizeof(setp_titles[0])))
            amb_strlcpy(q->title, _(setp_titles[q->setp]), sizeof(q->title));
        q->sel = qs < q->list.count ? qs : q->list.count - 1;
        sel_off_heading(q, 1);
    }
}

void draw_list_row(struct Page *p, LONG idx, WORD col);
WORD mark_column(const struct Page *p);

/* What a row shows (its words, value, mark, greying), to tell a row that
 * changed from one that did not. */
static ULONG row_sig(const struct Item *it)
{
    ULONG h = 2166136261UL;
    const char *s;
    for (s = it->label; *s; s++)
        h = (h ^ (UBYTE)*s) * 16777619UL;
    h = (h ^ 0xFF) * 16777619UL;
    if (it->kind == IT_ACTION)
        for (s = it->path; *s; s++)
            h = (h ^ (UBYTE)*s) * 16777619UL;
    return (h ^ ((ULONG)it->mark << 8 | it->flags)) * 16777619UL;
}

static void plex_settings_again(BOOL full)
{
    struct Page *p = cur();
    LONG sel = p ? p->sel : 0, top = p ? p->top : 0, count = 0, i;
    ULONG sig[48];
    int nsig = 0;
    if (p && p->type == PT_LIST && p->list.pool) {
        /* the rows on screen as they are: after the rebuild only those
         * that changed are drawn again (all of them flickered) */
        count = p->list.count;
        for (i = 0; i < list_visible() && i < 48 && top + i < count; i++)
            sig[nsig++] = row_sig(&p->list.items[top + i]);
    }
    settings_rebuild();
    if (!p || p->type != PT_LIST)
        return;
    p->sel = sel < p->list.count ? sel : p->list.count - 1;
    sel_off_heading(p, 1);
    if (full) {
        draw_pane(TRUE);
        draw_footer();
    } else if (p->list.count != count || p->top != top || !nsig) {
        draw_list_rows(p);
    } else {
        WORD col = mark_column(p);
        for (i = 0; i < list_visible() && top + i < p->list.count; i++)
            if (i >= nsig || row_sig(&p->list.items[top + i]) != sig[i] || top + i == sel ||
                top + i == p->sel)
                draw_list_row(p, top + i, col);
        if (p->sel != sel)
            draw_title_count(p);
    }
}

/* Settings > Style: radio buttons, like Screen mode. On a laced screen,
 * the fonts it can use. */
static void build_styles(struct Page *p)
{
    BOOL can_smooth;
    int i;
    if (gfx_laced(g.mode)) {
        p->style_pv = 1;
        for (i = 0; i < LFONT_COUNT; i++)
            action_item(p, gfx_laced_fonts[i].name, "", ACT_LFONT0 + i,
                        i == gfx_laced_font ? MARK_RADIO_ON : MARK_RADIO_OFF);
        p->sel = gfx_laced_font;
        return;
    }
    can_smooth = gfx_style_possible(&caps, g.mode, STYLE_SMOOTH);
    if (gfx_mode_is_rtg(g.mode)) {
        /* RTG: the pixel fonts whose layout fits the screen (Pixelify Sans
         * at its whole factor until one is chosen), and Smooth */
        int shown = gfx_font_shown(&g);
        struct Item *it;
        p->sel = -1;
        for (i = 0; i < LFONT_COUNT; i++) {
            BOOL on = g.style != STYLE_SMOOTH && i == shown;
            if (g.wb ? !gfx_window_font_fits(i) && i : !gfx_font_fits(i, g.h))
                continue;                   /* (a window: those with room on Workbench) */
            action_item(p, gfx_laced_fonts[i].name, "", ACT_RFONT0 + i,
                        on ? MARK_RADIO_ON : MARK_RADIO_OFF);
            if (on)
                p->sel = p->list.count - 1;
        }
        if ((it = action_item(p, _("Smooth"), "", ACT_STYLE_SMOOTH,
                              g.style == STYLE_SMOOTH ? MARK_RADIO_ON : MARK_RADIO_OFF)) &&
            !can_smooth)
            it->flags |= ITEM_DISABLED;     /* (an 8-bit screen) */
        if (g.style == STYLE_SMOOTH || p->sel < 0)
            p->sel = p->list.count - 1;
        return;
    }
    action_item(p, _("Pixel"), "", ACT_STYLE_PIXEL,
                ui_style == STYLE_PIXEL ? MARK_RADIO_ON : MARK_RADIO_OFF);
    action_item(p, _("Smooth"), can_smooth ? "" : _("RTG only"), ACT_STYLE_SMOOTH,
                ui_style == STYLE_SMOOTH ? MARK_RADIO_ON : MARK_RADIO_OFF);
    p->sel = ui_style == STYLE_SMOOTH ? 1 : 0;
}

static void build_screenmodes(struct Page *p)
{
    int m;
    for (m = 0; m < SM_COUNT; m++) {
        char label[64];
        const char *why = gfx_mode_problem(&caps, m);
        if (why && gfx_mode_is_rtg(m) && m != screen_mode)
            continue;           /* RTG sizes are listed only when the card has them */
        if (m == SM_AUTO) {
            amb_strlcpy(label, _("Automatic: "), sizeof(label));
            amb_strlcat(label, gfx_mode_label(gfx_mode_auto(&caps)), sizeof(label));
        } else {
            amb_strlcpy(label, gfx_mode_label(m), sizeof(label));
            if (gfx_mode_is_rtg(m) && !why) {
                char bits[16];
                sprintf(bits, "  %lu-bit", (unsigned long)gfx_mode_depth(m));
                amb_strlcat(label, bits, sizeof(label));
            }
        }
        action_item(p, label, why ? why : "", ACT_MODE0 + m,
                    m == screen_mode ? MARK_RADIO_ON : MARK_RADIO_OFF);
        if (m == screen_mode)
            p->sel = p->list.count - 1;
    }
}

/* Settings > Display > Colour depth: a full RTG screen whose size has a
 * 24 or 32-bit mode as well (a window is as deep as Workbench) */
static BOOL depth_offered(void)
{
    return !g.wb && gfx_mode_is_rtg(g.mode) && caps_rtg_deepest(g.w, g.h) >= 24;
}

static void build_depths(struct Page *p)
{
    action_item(p, _("16-bit: quicker"), "", ACT_DEPTH16,
                caps_rtg_bits < 24 ? MARK_RADIO_ON : MARK_RADIO_OFF);
    action_item(p, _("32-bit: finest pictures and video"), "", ACT_DEPTH32,
                caps_rtg_bits >= 24 ? MARK_RADIO_ON : MARK_RADIO_OFF);
    p->sel = caps_rtg_bits >= 24 ? 1 : 0;
}

/* Settings > Display > Text size (a window, smooth): those with room on
 * this Workbench. */
static void build_tsizes(struct Page *p)
{
    int i;
    for (i = 0; i < TSIZE_COUNT; i++) {
        if (!gfx_window_size_fits(i) && i)
            continue;
        /* (the size the window has: less than the one saved, without room) */
        action_item(p, _(tsize_names[i]), "", ACT_TSIZE0 + i,
                    i == gfx_window_tsize() ? MARK_RADIO_ON : MARK_RADIO_OFF);
        if (i == gfx_window_tsize())
            p->sel = p->list.count - 1;
    }
}

/* Settings > Display > Colours: the schemes, the one in use ticked */
static void build_colours(struct Page *p)
{
    int i;
    for (i = 0; i < UI_SCHEMES; i++) {
        action_item(p, _(ui_scheme_names[i]), "", ACT_COLOUR0 + i,
                    i == ui_scheme ? MARK_RADIO_ON : MARK_RADIO_OFF);
        if (i == ui_scheme)
            p->sel = p->list.count - 1;
    }
}

static void build_about_menu(struct Page *p)
{
    action_item(p, _("Credits"), "Ambiance " AMB_VERSION, ACT_CREDITS, MARK_ARROW);
    action_item(p, _("System information"), "", ACT_SYSINFO, MARK_ARROW);
    if (caps.net) {
        action_item(p, _("Plex in Ambiance"), "", ACT_PLEXHELP, MARK_ARROW);
        action_item(p, _("Jellyfin in Ambiance"), "", ACT_JFHELP, MARK_ARROW);
    }
}

/* AGS's splash screen, shown under the Credits: read from the AGS install
 * itself, never copied into Ambiance. The first of these that exists. */
static const char *const ags_splash[] = { "AGS:Images/logo.iff", NULL };

static void build_credits(struct Page *p)
{
    char line[96];
    int i;
    sprintf(line, "Ambiance %s", AMB_VERSION);
    text_line(p, line);
    text_line(p, _("(c) 2026 Brian King, under the MIT licence"));
    text_line(p, "");
    text_para(p, _("Fonts: Pixelify Sans (c) 2021 The Pixelify Sans Project Authors, Galmuri "
                 "(c) 2019-2025 Lee Minseo, Ark Pixel Font (c) 2021 TakWolf, Rubik (c) 2015 The "
                 "Rubik Project Authors, and Geologica (c) 2020 The Geologisk Project Authors, "
                 "all under the SIL Open Font License 1.1. The amb-pixel and amb-galmuri fonts "
                 "are Modified Versions of Pixelify Sans and Galmuri."));
    text_line(p, "");
    text_para(p, _("Code: libnix C runtime (public domain). The EaglePlayer host follows "
                 "HippoPlayer's (MIT licence, (c) 2017 Kari-Pekka Koljonen)."));
    text_line(p, "");
    text_para(p, _("Spectrum analyser: HippoPlayer's (MIT licence, (c) 2017 Kari-Pekka "
                   "Koljonen), with Tom Roberts' fixed-point FFT (1989)."));
    text_line(p, "");
    text_para(p, _("8-channel modules: mixed the way ProTracker 4.1's mixer does it (MIT "
                   "licence, (c) 2026 Akustikrausch (Andreas Wendorf); ProTracker 4.1: "
                   "github.com/akustikrausch/protracker4)."));
    text_line(p, "");
    text_para(p, _("Used if installed, not included, each under its own licence: Amiga Game "
                 "Selector, WHDLoad, AHI, MHI drivers, mpega.library, datatypes, AmiSSL."));
    text_line(p, "");
    text_para(p, _("FLAC: flacdec.library (c) 2026 Renaud Schweingruber, with dr_flac by David "
                 "Reid (public domain or MIT-0), used if installed, not included. It is free to "
                 "use and to pass on in its own unchanged archive (Aminet: util/libs/flacdec)."));
    text_line(p, "");
    text_para(p, _("Amiga is a trademark of its owner. Games, demos, music and pictures belong "
                 "to their authors. Licence texts are in the Licences drawer."));
    p->note = _("This front-end inspired by the efforts of Paul Vince's Amiga Game Selector");
    for (i = 0; ags_splash[i] && !p->pic[0]; i++) {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR old = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        if ((l = Lock((CONST_STRPTR)ags_splash[i], SHARED_LOCK))) {
            UnLock(l);
            amb_strlcpy(p->pic, ags_splash[i], sizeof(p->pic));
        }
        me->pr_WindowPtr = old;
    }
    if (!p->pic[0]) {                       /* no AGS logo: the thanks with the text */
        text_line(p, "");
        text_para(p, p->note);
        p->note = NULL;
    }
}

static void build_mods_root(void);
static void np_mark_lists(BOOL redraw);
BOOL np_local_art;                  /* the music file playing has a picture */
BOOL np_art_want;                   /* the Plex track playing has a cover */
/* A track playing from a file on this Amiga (Music folders), not a server. */
BOOL np_local(void)
{
    return mp_is_track() && mp_path()[0] && amb_strnicmp(mp_path(), "http", 4);
}
static void build_radio_root(struct Page *p);
static void build_music_root(struct Page *p);
static char plex_now[300];      /* the Plex track playing: its file's key (lists mark it so) */
static char plex_now_in[2][48]; /* ...its album's and artist's keys (their rows marked too) */
/* ...and its file as stored, for Now playing: the rate asked of the server
 * when an album's answer did not have it (plex_dj) */
struct PlexOrig plex_orig;
static struct NetJob *plex_dj;
struct NetJob *plex_sj;      /* an artist's or album's summary asked for (plex_sum_ask) */
static void plex_sum_came(struct NetJob *j);
static void plex_ask_rate(const char *key);
static BOOL lv_art_came;            /* a layout's cover came from the server: drawn again */
struct NetJob *art_nj[ART_KINDS];
static char art_want[ART_KINDS][120];
static ULONG art_asked[ART_KINDS];     /* time_ms each was asked for (log) */
static ULONG art_lv_wanted;
static ULONG lvt_worst;                 /* the longest lv_frame since the load line (log) */             /* time_ms a layout last wanted a server's picture */
BOOL art_fetch(int kind, const char *thumb, WORD w, WORD h, char *file);
static void art_ready(int kind, const char *file);
struct LJob *np_aj;          /* Now playing's cover, made for its square */
BOOL np_art_drawn;
static void np_art_free(void);
static void np_art_start(const struct Item *it);
static char np_album_thumb[160];    /* the playlist's album's cover (its row's), for a track without one */
static const struct Item *lv_opened_from(const struct Page *p);
static const char *lv_plex_field(const struct Item *it, int n, char *out, LONG max);
void np_art_load(const char *file);
WORD plex_art_side(void);
BOOL plex_movable(struct Page *p, LONG idx);
WORD plex_art_side(void);
static void plex_refetch(struct Page *p, const char *key);
static void np_soft_redraw(void);
static void np_seek_track(ULONG ms, ULONG delay);
static void music_root_again(void);
static void pictures_root_again(void);
static BOOL jf_section_row(struct Page *p, char kind, BOOL first);
static void goto_jf_settings(void);

/* A Jellyfin server chosen, Jellyfin on, the network up: its rows shown. */
static BOOL jf_shown(void)
{
    return caps.net && jf.on && jf.host[0];
}

/* A Jellyfin key of Pictures' (its photos' folders): ~Vp, ~Fp... */
static BOOL jf_photo_key(const char *k)
{
    return (k[1] == 'V' || k[1] == 'F') && k[2] == 'p';
}
static int pictures_plex_rows(struct Page *p);
static void radio_collect(void);
static void plex_photo_view(struct Page *p, LONG idx, BOOL slideshow);
static ULONG strtoul_(const char *s)
{
    ULONG v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* Pictures' first page: with one place to look (AGS's Media:Images, the
 * PICTURES: assign or a folder of Settings > Folders > Pictures), its
 * pictures and folders straight away, as before; with more, a row for
 * each. What pics_read_ui gives (-2: being read), or -1 with *src NULL
 * when there is nowhere to look; *src: the one place. */
static LONG pics_root_fill(struct Page *p, const char **src)
{
    const char *at[FS_DIRS + 2];
    int n = 0, i;
    if (place_there(PICS_ROOT))
        at[n++] = PICS_ROOT;
    if (fs_assign_used(FS_PICS))
        at[n++] = "PICTURES:";
    for (i = 0; i < fs[FS_PICS].n; i++)
        at[n++] = fs[FS_PICS].dir[i];
    *src = n == 1 ? at[0] : NULL;
    if (n == 0)
        return -1;
    if (n == 1)
        return pics_read_ui(at[0], &p->list);
    for (i = 0; i < n; i++) {
        char label[180];
        const char *f = (const char *)FilePart((STRPTR)at[i]);
        sprintf(label, _("Pictures: %.100s"), f[0] ? f : at[i]);
        list_add(&p->list, label, at[i], "", IT_DIR);
    }
    return n;
}

/* Videos' DVDs: a row first for each DVD-Video disc in a drive (Fire plays
 * its film: anim_play_dvd). Its path is the disc's VIDEO_TS.IFO, for the
 * preview's size and date; its sortkey "DVD". Their count. */
static ULONG dvd_sig;               /* the CD/DVD volumes when Videos was built (dvd_watch) */

static LONG vids_dvd_rows(struct ItemList *l)
{
    struct DvdVol v[4];
    LONG n, i;
    dvd_sig = dvd_volumes_sig();
    n = dvd_volumes(v, 4);
    for (i = n - 1; i >= 0; i--) {
        char path[64];
        struct Item *it;
        sprintf(path, "%.31s:VIDEO_TS/VIDEO_TS.IFO", v[i].vol);
        if ((it = list_add_first(l, v[i].vol, path, "DVD", IT_ANIM)))
            it->mark = MARK_DISC;           /* its name, a disc beside it */
    }
    return n;
}

/* Two names for one folder (VIDEOS: assigned to Media:Videos, say) */
static BOOL same_place(const char *a, const char *b)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR la, lb;
    BOOL same = FALSE;
    me->pr_WindowPtr = (APTR)-1;
    la = Lock((CONST_STRPTR)a, SHARED_LOCK);
    lb = la ? Lock((CONST_STRPTR)b, SHARED_LOCK) : 0;
    me->pr_WindowPtr = old;
    if (la && lb)
        same = SameLock(la, lb) == LOCK_SAME;
    if (lb)
        UnLock(lb);
    if (la)
        UnLock(la);
    return same;
}

/* A place added to Videos' list, unless it is one there already */
static void vids_place_add(const char **at, int *n, const char *path)
{
    int i;
    for (i = 0; i < *n; i++)
        if (same_place(at[i], path))
            return;
    at[(*n)++] = path;
}

/* Videos' first page, as Pictures': AGS's Media:Animations and Media:Videos, the VIDEOS:
 * assign and the folders of Settings > Folders > Videos; a DVD in a drive
 * first. */
static LONG vids_root_fill(struct Page *p, const char **src)
{
    const char *at[FS_DIRS + 3];
    int n = 0, i;
    LONG r;
    if (place_there(VIDS_ROOT))
        at[n++] = VIDS_ROOT;
    if (place_there(VIDS_ROOT2))
        vids_place_add(at, &n, VIDS_ROOT2);
    if (fs_assign_used(FS_VIDEOS))
        vids_place_add(at, &n, "VIDEOS:");
    for (i = 0; i < fs[FS_VIDEOS].n; i++)
        vids_place_add(at, &n, fs[FS_VIDEOS].dir[i]);
    *src = n == 1 ? at[0] : NULL;
    if (n == 0)
        return vids_dvd_rows(&p->list) ? p->list.count : -1;
    if (n == 1) {
        r = pics_read_ui(at[0], &p->list);
        if (r == -2)
            return r;                       /* (built again when it settles: the DVDs then) */
        if (vids_dvd_rows(&p->list))
            return p->list.count;           /* (the folder's own rows, if any, after) */
        return r;
    }
    for (i = 0; i < n; i++) {
        char label[180];
        const char *f = (const char *)FilePart((STRPTR)at[i]);
        if (!strcmp(at[i], VIDS_ROOT) || !strcmp(at[i], VIDS_ROOT2))
            sprintf(label, "%.100s", f);    /* (AGS's: Animations, Videos) */
        else
            sprintf(label, _("Videos: %.100s"), f[0] ? f : at[i]);
        list_add(&p->list, label, at[i], "", IT_DIR);
    }
    vids_dvd_rows(&p->list);
    return p->list.count;
}

/* A DVD's row: a disc in a drive, or a DVD's image (a .iso, pics.c) */
BOOL is_dvd_row(const struct Item *it)
{
    return it->kind == IT_ANIM && it->sortkey && !strcmp(it->sortkey, "DVD");
}

BOOL is_dvd_image(const struct Item *it)
{
    LONG n = (LONG)strlen(it->path);
    return is_dvd_row(it) && n > 4 && !amb_stricmp(it->path + n - 4, ".iso");
}

/* Pictures' first page with nothing of its own (the server's rows follow). */
static void pics_root_empty(struct Page *p, LONG n, const char *src)
{
    char t[120];
    if (n > 0)
        return;
    if (src)
        sprintf(t, n < 0 ? _("No %s here") : _("%s is empty"), src);
    else
        strcpy(t, _("No pictures folder: Settings > Folders > Pictures"));
    list_add(&p->list, t, "", "", IT_TEXT);
}

static void build_root_pages_now(int mod);
static void np_play_btn(void);

static void build_root_pages(int mod)
{
    build_root_pages_now(mod);
    if (depth == 1)
        plex_shortcut_fix(&pages[0]);       /* Now playing first, where its list has one */
}

static void build_root_pages_now(int mod)
{
    struct Page *p;
    pages_clear();
    switch (mod) {
    case MOD_GAMES:
    case MOD_DEMOS:
        if (ags.found) {
            p = page_push(PT_LIST, _(mod_names[mod]));
            if (p) {
                p->ags = 1;
                build_ags_root(p, mod);
                if (ig_count(&igcat, mod == MOD_DEMOS) > 0) {
                    /* iGame's list too, a folder after AGS's */
                    char path[64];
                    ig_page_path(path, sizeof(path), mod == MOD_DEMOS, "root");
                    list_add(&p->list, "iGame", path, "iGame", IT_DIR);
                }
            }
        } else if (ig_count(&igcat, mod == MOD_DEMOS) > 0 &&
                   (p = page_push(PT_LIST, _(mod_names[mod])))) {
            /* no AGS: iGame's list in its place */
            char path[64];
            BOOL ordered;
            p->ags = 1;
            p->igame = 1;
            ig_page_path(path, sizeof(path), mod == MOD_DEMOS, "root");
            ig_read(&igcat, path, &p->list, &ordered);
            whd_offer_row(p);
        } else if (whd_offer() && (p = page_push(PT_LIST, _(mod_names[mod])))) {
            /* nothing to list yet, and WHDLoad to offer: the offer, then why */
            whd_offer_row(p);
            text_line(p, "");
            no_ags_text(p, mod);
            {
                LONG i;
                for (i = 1; i < p->list.count; i++)
                    p->list.items[i].flags |= ITEM_HEAD;
            }
        } else if ((p = page_push(PT_TEXT, _(mod_names[mod]))))
            no_ags_text(p, mod);
        break;
    case MOD_MODS:
        build_mods_root();
        break;
    case MOD_VIDEOS:
        if ((p = page_push(PT_LIST, _("Videos")))) {
            LONG n;
            const char *src;
            p->pics = 2;                    /* Videos: its folders' .iff files looked inside */
            n = vids_root_fill(p, &src);
            if (n != -2 && jf_section_row(p, 'v', TRUE) && n <= 0)
                n = 1;                      /* Jellyfin's films: the list kept */
            if (n == -2) {
                page_pop();                 /* given up for input: built when it settles */
                page_push(PT_TEXT, _("Videos"));
            } else if (n <= 0) {
                page_pop();
                if ((p = page_push(PT_TEXT, _("Videos")))) {
                    char t[260];
                    if (src)
                        sprintf(t, n < 0 ? _("No animations found: Ambiance looks for them in %s.")
                                         : _("%s has no animations in it."), src);
                    else
                        sprintf(t, _("No videos found. Ambiance looks in VIDEOS:, "
                                     "the folders of Settings > Folders > Videos, and on AGS %s and %s."),
                                VIDS_ROOT, VIDS_ROOT2);
                    text_para(p, t);
                }
            }
        }
        break;
    case MOD_PICTURES:
        if ((p = page_push(PT_LIST, _("Pictures")))) {
            LONG n;
            const char *src;
            p->pics = 1;
            n = pics_root_fill(p, &src);
            if (n == -2) {
                page_pop();                 /* given up for input: built when it settles */
                page_push(PT_TEXT, _("Pictures"));
            } else if (n <= 0 && !(caps.net && plex.on && plex.host[0]) && !jf_shown()) {
                page_pop();
                if ((p = page_push(PT_TEXT, _("Pictures")))) {
                    char t[240];
                    if (src)
                        sprintf(t, n < 0 ? _("No pictures found: Ambiance looks for them in %s.")
                                         : _("%s has no pictures in it."), src);
                    else
                        sprintf(t, _("No pictures found. Ambiance looks for them in PICTURES:, in "
                                     "the folders of Settings > Folders > Pictures, and on AGS in %s."),
                                PICS_ROOT);
                    text_para(p, t);
                }
            } else {
                pics_root_empty(p, n, src);
                jf_section_row(p, 'p', TRUE);
                pictures_plex_rows(p);      /* the server's photo libraries first */
            }
        }
        break;
    case MOD_MUSIC:
        if ((p = page_push(PT_LIST, _("Music"))))
            build_music_root(p);
        break;
    case MOD_RADIO:
        if ((p = page_push(PT_LIST, _("Radio"))))
            build_radio_root(p);
        break;
    case MOD_SETTINGS:
        if ((p = page_push(PT_LIST, _("Settings"))))
            build_settings(p);
        break;
    case MOD_ABOUT:
        if ((p = page_push(PT_LIST, _("About"))))
            build_about_menu(p);
        break;
    case MOD_NOWPLAYING:
        if (page_push(PT_PLAYER, _("Now playing")))
            np_play_btn();                  /* (its Play button chosen) */
        break;
    }
}

/* ------------------------------------------------------------ section memory
 * Each main-menu section remembers where the user was: for every list
 * level, the highlighted entry (by position and by name) and the scroll
 * position. Coming back to the section rebuilds that trail, reopening the
 * folders on the way, so leaving Games on Top 200 and visiting Settings
 * returns to Top 200 (or to the game inside it that was highlighted). An
 * entry that has moved is found again by name; one that is gone leaves
 * the highlight at the nearest position. Kept for this run only. */
struct NavTrail {
    int n;
    struct { LONG sel, top; char label[64]; } lv[MAXDEPTH];
};
static struct NavTrail nav_mem[MOD_COUNT];
static int shown_mod = -1;

/* The trail of the pages shown now (zeroed first: compared byte by byte). */
static void trail_now(struct NavTrail *t)
{
    int d, n = 0;
    memset(t, 0, sizeof(*t));
    for (d = 0; d < depth && pages[d].type == PT_LIST; d++, n++) {
        struct Page *p = &pages[d];
        t->lv[n].sel = p->sel;
        t->lv[n].top = p->top;
        amb_strlcpy(t->lv[n].label, p->list.count ? p->list.items[p->sel].label : "", 64);
    }
    t->n = n;
}

static void save_nav(void)
{
    if (shown_mod < 0 || !depth || shown_mod == MOD_NOWPLAYING)
        return;
    trail_now(&nav_mem[shown_mod]);
}

/* Quitting takes Back after Back (out to the menu, then Esc twice), which
 * would leave nothing of the trail to remember. So the first Back of a
 * run of them notes where the user was; anything else (a move, opening
 * something, a click) forgets it. Quitting at the end of the run
 * remembers that place. */
static struct {
    BOOL on;
    int mod;
    struct NavTrail t;
} nav_snap;

static void nav_back_begin(int f)
{
    if (nav_snap.on || shown_mod < 0 || !depth || f == FOCUS_MENU || shown_mod == MOD_NOWPLAYING)
        return;
    nav_snap.on = TRUE;
    nav_snap.mod = shown_mod;
    trail_now(&nav_snap.t);
}

void nav_moved(void)
{
    nav_snap.on = FALSE;
}

/* Open a folder page without drawing (used while rebuilding the trail). */
static BOOL push_dir_quiet(struct Item *it)
{
    BOOL pics = cur() && cur()->pics, mods = cur() && cur()->mods, songs = cur() && cur()->songs;
    UBYTE pk = cur() ? cur()->pics : 0;     /* 2: Videos' */
    struct Page *p = page_push(PT_LIST, it->label);
    if (!p)
        return FALSE;
    if (songs) {
        p->songs = 1;
        if ((beets_page(it->path) ? beets_read(it->path, &p->list)
                                  : songs_read_dir(it->path, &p->list)) <= 0) {
            page_pop();
            return FALSE;
        }
        np_mark_lists(FALSE);
        return TRUE;
    }
    if (mods) {
        p->mods = 1;
        if (mods_read_dir(it->path, &p->list) <= 0) {
            page_pop();
            return FALSE;
        }
        np_mark_lists(FALSE);
        return TRUE;
    }
    if (pics) {
        p->pics = pk;
        if (pics_read_ui(it->path, &p->list) <= 0) {
            page_pop();
            return FALSE;
        }
        return TRUE;
    }
    if (page_read_ags(p, it, 1) <= 0) {
        page_pop();
        return FALSE;
    }
    return TRUE;                    /* restore_nav reads on to the remembered entry */
}

WORD list_visible(void);

static void restore_nav(int mod)
{
    int lv;
    for (lv = 0; lv < nav_mem[mod].n; lv++) {
        struct Page *p = cur();
        LONG sel = nav_mem[mod].lv[lv].sel, i;
        const char *want = nav_mem[mod].lv[lv].label;
        if (!p || p->type != PT_LIST || !p->list.count)
            return;
        /* a list still loading: read up to the remembered entry, no further */
        ch_wait_until(p, sel + 2 * list_visible(), FALSE);
        ch_wait_item(p, sel < p->list.count ? sel : p->list.count - 1, FALSE);
        if (ch_reading(p) && (sel >= p->list.count || strcmp(p->list.items[sel].label, want) != 0))
            ch_wait_until(p, 0x7FFFFFFF, FALSE);
        if (sel >= p->list.count || strcmp(p->list.items[sel].label, want) != 0) {
            for (i = 0; i < p->list.count; i++)
                if (strcmp(p->list.items[i].label, want) == 0)
                    break;
            sel = i < p->list.count ? i : (sel < p->list.count ? sel : p->list.count - 1);
        }
        p->sel = sel;
        p->top = nav_mem[mod].lv[lv].top;
        if (p->top > p->sel)
            p->top = p->sel;
        if (lv + 1 < nav_mem[mod].n) {
            struct Item *it = &p->list.items[p->sel];
            if (it->kind != IT_DIR || !push_dir_quiet(it))
                return;
        }
    }
}

/* The trail kept across runs too: written when Ambiance quits (and
 * before a game starts, in case it never comes back), read when it
 * starts. A text file, one line each:
 *   Ambiance navigation 1
 *   at <section>\t<menu|list>        where the user was, and in which pane
 *   section <section>\t<levels>      then, for each level of that trail,
 *   <sel>\t<top>\t<label>
 * Sections by name, so a new menu order keeps working. */
#define NAV_FILE "ENVARC:Ambiance/Nav"
#define NAV_MAX  16384

static int mod_by_name(const char *s, LONG n)
{
    int m;
    for (m = 0; m < MOD_COUNT; m++)
        if (mod_names[m] && (LONG)strlen(mod_names[m]) == n && !strncmp(mod_names[m], s, n))
            return m;
    return -1;
}

static void nav_store(BOOL quitting)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    char line[120];
    BPTR fh;
    int m, l, at;
    BOOL in_list;
    if (depth)
        save_nav();                 /* the section shown now */
    at = nmenu ? menu[menu_sel] : -1;
    in_list = !(focus == FOCUS_MENU || pane_pending || at != shown_mod);
    if (quitting && nav_snap.on) {  /* where the run of Backs began */
        nav_mem[nav_snap.mod] = nav_snap.t;
        at = nav_snap.mod;
        in_list = TRUE;
    }
    me->pr_WindowPtr = (APTR)-1;
    ensure_dir("ENVARC:Ambiance");
    fh = Open((CONST_STRPTR)NAV_FILE, MODE_NEWFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return;
    FPuts(fh, (CONST_STRPTR)"Ambiance navigation 1\n");
    if (at >= 0) {
        sprintf(line, "at %s\t%s\n", mod_names[at], in_list ? "list" : "menu");
        FPuts(fh, (CONST_STRPTR)line);
    }
    for (m = 0; m < MOD_COUNT; m++) {
        if (!nav_mem[m].n || !mod_names[m])
            continue;
        sprintf(line, "section %s\t%ld\n", mod_names[m], (long)nav_mem[m].n);
        FPuts(fh, (CONST_STRPTR)line);
        for (l = 0; l < nav_mem[m].n; l++) {
            sprintf(line, "%ld\t%ld\t", (long)nav_mem[m].lv[l].sel, (long)nav_mem[m].lv[l].top);
            FPuts(fh, (CONST_STRPTR)line);
            FPuts(fh, (CONST_STRPTR)nav_mem[m].lv[l].label);
            FPuts(fh, (CONST_STRPTR)"\n");
        }
    }
    Close(fh);
}

static LONG nav_num(const char *s)
{
    LONG v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* Written again a few seconds after the trail last changed (so it is
 * kept even when the machine is switched off rather than quit). */
static ULONG nav_sum_seen, nav_sum_written;
static int nav_quiet;

static ULONG nav_sum(void)
{
    struct NavTrail t;
    const UBYTE *b = (const UBYTE *)&t;
    ULONG h = 2166136261UL, i;
    trail_now(&t);
    for (i = 0; i < sizeof(t); i++)
        h = (h ^ b[i]) * 16777619UL;
    h = (h ^ (ULONG)(nmenu ? menu[menu_sel] : 0)) * 16777619UL;
    return (h ^ (ULONG)(focus == FOCUS_MENU)) * 16777619UL;
}

static void nav_tick_1s(void)
{
    ULONG s;
    if (!depth || pane_pending)
        return;
    s = nav_sum();
    if (s != nav_sum_seen) {
        nav_sum_seen = s;
        nav_quiet = 0;
        return;
    }
    if (s != nav_sum_written && ++nav_quiet >= 3) {
        nav_store(FALSE);
        nav_sum_written = s;
    }
}

/* The next line of buf (from *pos), its end made 0; NULL at the end. */
static char *nav_line(char *buf, LONG len, LONG *pos)
{
    char *s;
    LONG i;
    if (*pos >= len)
        return NULL;
    s = buf + *pos;
    for (i = *pos; i < len && buf[i] != '\n'; i++)
        ;
    buf[i] = 0;
    *pos = i + 1;
    return s;
}

/* The trail of the last run into nav_mem; *at its section (-1 none),
 * *in_list whether the list (not the menu) had the focus. */
static void nav_load(int *at, BOOL *in_list)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    char *buf, *s, *t;
    LONG len, pos = 0;
    BPTR fh;
    int m = -1, left = 0;
    *at = -1;
    *in_list = FALSE;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)NAV_FILE, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return;
    buf = AllocVec(NAV_MAX + 1, MEMF_ANY);
    len = buf ? Read(fh, buf, NAV_MAX) : -1;
    Close(fh);
    if (len <= 0) {
        if (buf)
            FreeVec(buf);
        return;
    }
    buf[len] = 0;
    s = nav_line(buf, len, &pos);
    if (!s || strcmp(s, "Ambiance navigation 1") != 0) {
        FreeVec(buf);
        return;                     /* not one this Ambiance knows */
    }
    while ((s = nav_line(buf, len, &pos))) {
        if (!strncmp(s, "at ", 3) && (t = strchr(s + 3, '\t'))) {
            *at = mod_by_name(s + 3, t - (s + 3));
            *in_list = !strcmp(t + 1, "list");
        } else if (!strncmp(s, "section ", 8) && (t = strchr(s + 8, '\t'))) {
            m = mod_by_name(s + 8, t - (s + 8));
            left = (int)nav_num(t + 1);
            if (m >= 0)
                nav_mem[m].n = 0;
            if (left > MAXDEPTH)
                left = MAXDEPTH;
        } else if (m >= 0 && left > 0 && (t = strchr(s, '\t')) && strchr(t + 1, '\t')) {
            int n = nav_mem[m].n;
            nav_mem[m].lv[n].sel = nav_num(s);
            nav_mem[m].lv[n].top = nav_num(t + 1);
            amb_strlcpy(nav_mem[m].lv[n].label, strchr(t + 1, '\t') + 1, 64);
            nav_mem[m].n = n + 1;
            left--;
        }
    }
    FreeVec(buf);
}

static void build_root(int mod)
{
    save_nav();                 /* the section being left */
    build_root_pages(mod);
    shown_mod = mod;
    restore_nav(mod);
}

/* ------------------------------------------------------------ drawing */
WORD list_visible(void)
{
    return (g.footer_y - 2 - g.list_top) / g.list_row_h;
}

/* Days and months, short (the clock, file dates), translated where used. */
static const char *const day_abbr[] = { N_("Sun"), N_("Mon"), N_("Tue"), N_("Wed"), N_("Thu"),
                                        N_("Fri"), N_("Sat") };
const char *const mon_abbr[] = { N_("Jan"), N_("Feb"), N_("Mar"), N_("Apr"), N_("May"),
                                        N_("Jun"), N_("Jul"), N_("Aug"), N_("Sep"), N_("Oct"),
                                        N_("Nov"), N_("Dec") };

/* Dates the Japanese and Korean way round (year, month, day), the month
 * and day as numbers, each followed by its word: Japanese tsuki and hi,
 * Korean wol and il (cjk.c's three bytes each). */
BOOL date_ymd(void)
{
    const char *iso = amb_lang_iso();
    return !strcmp(iso, "ja") || !strcmp(iso, "ko");
}

static const char *date_mon_word(void)
{
    return !strcmp(amb_lang_iso(), "ko") ? "\230\273\224 " : "\214\274\210";
}

static const char *date_day_word(void)
{
    return !strcmp(amb_lang_iso(), "ko") ? "\230\275\274 " : "\214\267\245";
}

void clock_text(char *buf)
{
    static const UBYTE md[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    struct DateStamp ds;
    LONG days, y = 1978, m = 0;
    DateStamp(&ds);
    days = ds.ds_Days;
    last_minute = ds.ds_Minute;
    if (days < 4383) {          /* before 1990: no battery clock set */
        buf[0] = 0;
        return;
    }
    {
        LONG wday = days % 7;   /* 1 Jan 1978 was a Sunday */
        for (;;) {
            LONG ylen = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
            if (days < ylen)
                break;
            days -= ylen;
            y++;
        }
        for (;;) {
            LONG mlen = md[m] + (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
            if (days < mlen)
                break;
            days -= mlen;
            m++;
        }
        if (date_ymd())         /* "11 gatsu 06 nichi (kin)": the month first */
            sprintf(buf, "%ld%s%02ld%s(%s)  %02ld:%02ld", (long)m + 1, date_mon_word(),
                    (long)days + 1, date_day_word(), _(day_abbr[wday]),
                    (long)ds.ds_Minute / 60, (long)ds.ds_Minute % 60);
        else
            sprintf(buf, "%s %02ld %s  %02ld:%02ld", _(day_abbr[wday]), (long)days + 1,
                    _(mon_abbr[m]), (long)ds.ds_Minute / 60, (long)ds.ds_Minute % 60);
    }
}

/* Left edge of the clock's part of the header (room for the widest date). */
WORD clock_left(void)
{
    /* the widest day and month in this language */
    WORD dw = 0, mw = 0, w;
    int i;
    if (date_ymd()) {
        char wide[48];
        sprintf(wide, "12%s30%s(%s)  00:00", date_mon_word(), date_day_word(), date_mon_word());
        return g.w - U(8) - gfx_textlen(&g, wide, TS_SMALL | TS_TABULAR) - U(4);
    }
    for (i = 0; i < 7; i++)
        if ((w = gfx_textlen(&g, _(day_abbr[i]), TS_SMALL)) > dw)
            dw = w;
    for (i = 0; i < 12; i++)
        if ((w = gfx_textlen(&g, _(mon_abbr[i]), TS_SMALL)) > mw)
            mw = w;
    return g.w - U(8) - dw - mw - gfx_textlen(&g, " 30   00:00", TS_SMALL | TS_TABULAR) - U(4);
}

/* While music plays, its dancing bars in the header, left of the clock:
 * a sign from anywhere that something is playing. */
BOOL hdr_np_on;

/* A layout on: the time, the clock or the Debug line would have been
 * drawn (the classic's own, not drawn): the layout's LC_PLAYING */
BOOL lv_playing_hit;


void draw_clock(void)
{
    ui->clock();
}


/* Debug readout between the logo and the clock: the longest form that fits. */
static LONG cpu_load = -1, app_load = -1;
static ULONG loop_wakes;                /* the main loop's turns (Debug's log) */

/* Debug: every 5 s the loads in the log, with where Ambiance is and how
 * often its loop woke, so a load that follows a page can be traced. */
static void dbg_load_log(void)
{
    static int n;
    static ULONG wakes0;
    static LONG cpu_sum, app_sum;
    char m[760];
    struct Page *p = cur();
    if (cpu_load >= 0)
        cpu_sum += cpu_load;
    if (app_load >= 0)
        app_sum += app_load;
    if (++n < 5)
        return;
    {
        LONG rb = st_busy_sample();
        char rp[480];
        rp[0] = 0;
        if (rb >= 0)
            sprintf(rp, " (its music process %ld%%)", (long)(rb > 100 ? 100 : rb));
        {
            ULONG fr, t;
            vis_timing(&fr, &t);
            if (fr) {                   /* the spectrum's time a frame, and what it heard */
                ULONG pk, none, top;
                vis_input(&pk, &none, &top);
                sprintf(rp + strlen(rp), ", spectrum %ld.%ld ms a frame (sound to it: peak %lu, %lu of %lu frames none, "
                        "loudest step %lu)", (long)(t / fr / 10), (long)(t / fr % 10), (unsigned long)pk,
                        (unsigned long)none, (unsigned long)fr, (unsigned long)top);
            }
        }
        {
            ULONG lms, lw, lp;
            lv_timing(&lms, &lw, &lp);
            if (lms || lw || lp) {      /* a layout's: its time drawing, and how */
                ULONG ss, ps, kpx, gcs;
                sprintf(rp + strlen(rp), ", layout %lu ms (%lu whole, %lu in part, longest %lu ms)",
                        (unsigned long)lms, (unsigned long)lw, (unsigned long)lp, (unsigned long)lvt_worst);
                lvt_worst = 0;
                lk_timing(&ss, &ps, &kpx, &gcs);
                if (ss || ps || gcs) {  /* (8-bit: shown, pictures' colours, pens) */
                    ULONG sh, lf;
                    lk_pens(&sh, &lf);
                    sprintf(rp + strlen(rp), " [8-bit: shown %lu ms, %lu K pixels; pictures %lu ms; %lu pen looks",
                            (unsigned long)ss, (unsigned long)kpx, (unsigned long)ps, (unsigned long)gcs);
                    if (lf < 999)
                        sprintf(rp + strlen(rp), "; fewest pens free %lu, %lu pictures short of them",
                                (unsigned long)lf, (unsigned long)sh);
                    {
                        ULONG bn, bk;
                        lk_blits(&bn, &bk);
                        if (bn)         /* (copies the blitter made, not c2p'd again) */
                            sprintf(rp + strlen(rp), "; blitter %lu copies, %lu K pixels",
                                    (unsigned long)bn, (unsigned long)bk);
                    }
                    strcat(rp, "]");
                }
                {
                    char pf[200];
                    lk_prof(pf);
                    if (pf[0])
                        amb_log("8-bit time", pf);
                }
            }
        }
        sprintf(m, "system %ld%%, Ambiance %ld%%%s, %ld wakes/s; %.20s > %.60s%s%s", (long)(cpu_sum / n),
                (long)(app_sum / n), rp, (long)((loop_wakes - wakes0) / n),
                mod_names[menu[menu_sel]], p ? p->title : "-", focus == FOCUS_MENU ? " (menu)" : "",
                mp_loaded() ? (mp_is_stream() ? ", radio on" : ", music on") : "");
    }
    amb_log("load", m);
    {
        /* which tasks had the CPU (sampled each frame); what the system's
         * load has beyond them was in interrupts (a sound card's driver,
         * the blitter's) or too short to be caught */
        char t[300];
        LONG busy;
        if (dbg_tasks_report(t, sizeof(t) - 60, &busy)) {
            if (cpu_sum / n > busy)
                sprintf(t + strlen(t), " interrupts and the rest about %ld%%", (long)(cpu_sum / n - busy));
            amb_log("tasks", t);
        }
    }
    if (lv_on()) {
        /* the classic's drawing under the layout, by where it was called
         * (from lv_frame: build/Ambiance.dbg and main.o give the place) */
        char t[300];
        gfx_off_report(t, sizeof(t), (const void *)lv_frame);
        if (t[0])
            amb_log("unseen", t);
        lv_who_report();
    }
    n = 0;
    cpu_sum = app_sum = 0;
    wakes0 = loop_wakes;
}

/* Once a second: the screen's state, logged when it changes: which screen
 * is in front, the display's DMA, the UI's colours, the copper lists, and
 * what the screen really shows (pixels read back). Made to find a blank
 * screen after gliding to a long list's end and back (blit_rows wrote past
 * the screen's bitmap); built only with -DAMB_SCREEN_WATCH, should the
 * like come back. */
#ifdef AMB_SCREEN_WATCH
static const char *scr_why;
static void scr_watch(void)
{
    static char last[600];
    char m[600], cols[80], dsp[200];
    struct Screen *front = IntuitionBase->FirstScreen;
    UWORD dma = *(volatile UWORD *)0xDFF002;    /* DMACONR */
    struct Page *p = cur();
    ULONG rgb[3 * 8];
    int i;
    if (!amb_log_path())
        return;                         /* no log: none of this work (pixels read back) */
    cols[0] = 0;
    if (g.scr) {
        GetRGB32(g.scr->ViewPort.ColorMap, 0, 8, rgb);
        for (i = 0; i < 8; i++)
            sprintf(cols + strlen(cols), "%s%02lx%02lx%02lx", i ? " " : "",
                    (unsigned long)(rgb[i * 3] >> 24), (unsigned long)(rgb[i * 3 + 1] >> 24),
                    (unsigned long)(rgb[i * 3 + 2] >> 24));
    }
    /* what the screen really shows: pixels not the background along a line
     * through the header, the first row's text and the middle row's */
    {
        LONG ink[3] = { 0, 0, 0 };
        WORD ys[3], x, k;
        ys[0] = U(12);
        ys[1] = g.list_top + g.list_row_h / 2;
        ys[2] = g.list_top + (list_visible() / 2) * g.list_row_h + g.list_row_h / 2;
        if (g.win && g.rp)
            for (k = 0; k < 3; k++)
                if (ys[k] >= 0 && ys[k] < g.h)
                    for (x = k ? g.pane_x : 0; x < (k ? g.pane_x + g.pane_w / 2 : g.w); x += 2)
                        if (ReadPixel(g.rp, x, ys[k]) != GPEN(PEN_BG))
                            ink[k]++;
        /* the display itself: whose View is loaded, our ViewPort hidden or
         * not, its copper lists there, and chip RAM (copper lists are made
         * in it: without room, a screen can show no bitplanes) */
        {
            struct View *av = GfxBase->ActiView;
            struct ViewPort *vp = g.scr ? &g.scr->ViewPort : NULL;
            sprintf(dsp, "view %s, viewport %s%s, copper %s/%s",
                    av == &IntuitionBase->ViewLord ? "Intuition's" : av ? "ANOTHER" : "NONE",
                    vp && (vp->Modes & VP_HIDE) ? "HIDDEN" : "shown",
                    vp && !vp->DspIns ? ", NO DISPLAY INSTRUCTIONS" : "",
                    av && av->LOFCprList ? "long" : "NO LONG",
                    av && av->SHFCprList ? "short" : "no short");
        }
        sprintf(m, "front %s%.30s, display DMA %s, colours %s, screen at %ld,%ld scrolled %ld,%ld, "
                   "ink: header %s, row 1 %s, middle %s%s",
                front == g.scr ? "ours" : "another: ",
                front == g.scr ? "" : front && front->Title ? (const char *)front->Title : "(untitled)",
                (dma & 0x0300) == 0x0300 ? "on" : "OFF", cols,
                g.scr ? (long)g.scr->LeftEdge : 0L, g.scr ? (long)g.scr->TopEdge : 0L,
                g.scr && g.scr->ViewPort.RasInfo ? (long)g.scr->ViewPort.RasInfo->RxOffset : 0L,
                g.scr && g.scr->ViewPort.RasInfo ? (long)g.scr->ViewPort.RasInfo->RyOffset : 0L,
                ink[0] ? "yes" : "NONE", ink[1] ? "yes" : "NONE", ink[2] ? "yes" : "NONE",
                p && p->type == PT_LIST && p->list.count && p->top >= p->list.count
                    ? ", LIST PAST ITS END" : "");
    }
    amb_strlcat(m, "; ", sizeof(m));
    amb_strlcat(m, dsp, sizeof(m));
    if (strcmp(m, last) || scr_why) {
        amb_strlcpy(last, m, sizeof(last));
        sprintf(m + strlen(m), "; chip free %luK (largest %luK)",
                (unsigned long)(AvailMem(MEMF_CHIP) >> 10),
                (unsigned long)(AvailMem(MEMF_CHIP | MEMF_LARGEST) >> 10));
        if (scr_why)
            sprintf(m + strlen(m), " (on %s)", scr_why);
        scr_why = NULL;
        if (p && p->type == PT_LIST)
            sprintf(m + strlen(m), "; %.30s: sel %ld top %ld of %ld%s%s", p->title, (long)p->sel,
                    (long)p->top, (long)p->list.count, ov.on ? ", letter up" : "",
                    pane_pending ? ", pane pending" : "");
        amb_log("screen", m);
    }
}

static void scr_watch_now(const char *why)
{
    scr_why = why;
    scr_watch();
}
#else
static void scr_watch(void)
{
}

static void scr_watch_now(const char *why)
{
    (void)why;
}
#endif

/* What the readout shows now, so a second that changes nothing redraws
 * nothing. Cleared when the header is drawn afresh. */
char status_shown[96];
static ULONG fps_shown, fps_flips0;     /* flips in the last second; the count then */

/* v in at least w digit cells (FIG_SPACEs before it: TS_TABULAR), then
 * unit */
static void fig_num(char *out, LONG v, int w, const char *unit)
{
    char d[16];
    int n, i = 0;
    sprintf(d, "%ld", (long)v);
    for (n = (int)strlen(d); n < w; n++)
        out[i++] = FIG_SPACE;
    strcpy(out + i, d);
    strcat(out, unit);
}

static char lv_debug[112];          /* a layout's copy of the Debug line ("" off) */
void draw_status(void)
{
    WORD x0 = U(6) + g.logo_w + U(10);
    WORD x1 = clock_left() - g.mark_px - U(8);  /* the playing bars' place kept */
    char cpu[16], app[16], fps[24], ram[16], chp[16], forms[3][112];
    const char *show = "";
    ULONG chip, used;
    int i;
    if (x1 <= x0)
        return;
    if (!debug_on) {
        lv_debug[0] = 0;
        ui->status(NULL);
        return;
    }
    used = dbg_mem_used(&chip);
    /* each number right in cells of its own (tabular, FIG_SPACE before
     * it): one changing moves nothing after it */
    if (cpu_load < 0)
        amb_strlcpy(cpu, "--", sizeof(cpu));
    else
        fig_num(cpu, cpu_load, 3, "%");
    if (app_load < 0)
        amb_strlcpy(app, "--", sizeof(app));
    else
        fig_num(app, app_load, 3, "%");
    fig_num(ram, (LONG)(used >> 10), 6, "K");
    fig_num(chp, (LONG)(chip >> 10), 4, "K");
    /* double buffered: the flips of the last second */
    fps[0] = 0;
    if (gfx_dbuf_active()) {
        fig_num(fps, fps_shown, 3, " FPS  ");
    }
    /* CPU = whole system (idle-task meter); app = Ambiance's own share */
    sprintf(forms[0], "%sCPU %s  app %s   RAM %s  (chip %s)", fps, cpu, app, ram, chp);
    sprintf(forms[1], "%sCPU %s  app %s  RAM %s", fps, cpu, app, ram);
    sprintf(forms[2], "%s%s/%s %s", fps, cpu, app, ram);
    amb_strlcpy(lv_debug, forms[0], sizeof(lv_debug));     /* (a layout shows it whole) */
    for (i = 0; i < 3; i++)
        if (gfx_textlen(&g, forms[i], TS_SMALL | TS_TABULAR) <= x1 - x0 || i == 2) {
            show = forms[i];
            break;
        }
    ui->status(show);                       /* (a layout: its own line, from lv_debug) */
}


static void set_debug(BOOL on)
{
    debug_on = on;
    if (on) {
        dbg_cpu_start();
        amb_log_open(TRUE);
    } else {
        dbg_cpu_stop();
        amb_log_close();
    }
    cpu_load = app_load = -1;
}


ULONG menu_draw_ms = 1000;           /* the last whole menu's drawing (menu_slide) */


BOOL has_aside(struct Page *p);

/* A section that is only a note (none now: Videos plays animations):
 * nothing in it to go to, so the menu keeps the focus. */
BOOL section_inert(void)
{
    return FALSE;
}

const char *hint_text(void)
{
    struct Page *p = cur();
    /* the long hints when the widest of them fits */
    BOOL hi = gfx_textlen(&g, _("Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back"),
                          TS_SMALL) <= g.w - U(12);
    if (focus == FOCUS_MENU && section_inert())
        return hi ? _("Up/Down  choose      Esc twice  quit") : _("Esc twice quit");
    if (focus == FOCUS_MENU)
        return hi ? _("Up/Down  choose      Right/Fire  open      Esc twice  quit")
                  : _("Fire open  Esc twice quit");
    if (focus == FOCUS_PREVIEW && p && p->pics)
        return hi ? _("Up/Down  scroll      Fire/Return  view      Left/Esc  back")
                  : _("Fire view  Esc back");
    if (focus == FOCUS_PREVIEW)
        return hi ? _("Up/Down  scroll      Fire/Return  play      Left/Esc  back")
                  : _("Fire play  Esc back");
    if (p && p->type == PT_LIST && p->pics && p->list.count) {
        /* Fire plays what can be played (a picture, a folder of them);
         * Right goes inside */
        const struct Item *it = &p->list.items[p->sel];
        if (it->kind == IT_PIC)
            return hi ? _("Up/Down  move    Fire/Return  view    Left/Esc  back")
                      : _("Fire view  Esc back");
        if (it->kind == IT_ANIM)
            return hi ? _("Up/Down  move    Fire/Return  play    Left/Esc  back")
                      : _("Fire play  Esc back");
        if (it->mark == MARK_PLAY)
            return hi ? _("Up/Down  move    Fire  slideshow    Right  open    Left/Esc  back")
                      : _("Fire show  Right open");
        return hi ? _("Up/Down  move    Fire/Right  open    Left/Esc  back") : _("Fire open  Esc back");
    }
    if (p && p->type == PT_LIST && p->plex && p->list.count &&
        p->list.items[p->sel].kind == IT_PLEXTRACK)
        return hi ? _("Up/Down  move    Fire/Return  play    Left/Esc  back") : _("Fire play  Esc back");
    if (p && p->type == PT_LIST && p->plex && p->list.count &&
        p->list.items[p->sel].kind == IT_PLEXPHOTO)
        return hi ? _("Up/Down  move    Fire/Return  view    Left/Esc  back") : _("Fire view  Esc back");
    if (p && p->type == PT_LIST && p->plex && p->list.count &&
        p->list.items[p->sel].kind == IT_PLEXSRV)
        return hi ? _("Up/Down  move    Fire  use this server    Left/Esc  back")
                  : _("Fire use it  Esc back");
    if (p && p->type == PT_LIST && p->radio && p->list.count &&
        p->list.items[p->sel].kind == IT_STATION)
        return hi ? _("Up/Down  move    Fire/Return  play    Right  about it    Left/Esc  back")
                  : _("Fire play  Right info");
    if (p && p->type == PT_LIST && p->radio)
        return hi ? _("Up/Down  move    Right/Fire  open    Left/Esc  back") : _("Fire open  Esc back");
    if (p && p->type == PT_LIST && has_aside(p))
        return hi ? _("Up/Down  move    Right  details    Fire/Return  play    Left/Esc  back")
                  : _("Fire play  Esc back");
    if (p && p->type == PT_LIST)
        return hi ? _("Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back")
                  : _("Fire open  Ret play  Esc back");
    if (p && p->type == PT_DETAIL)
        return hi ? _("Fire/Return  play      Up/Down  scroll      Left/Esc  back")
                  : _("Fire play  Esc back");
    if (p && p->type == PT_PLAYER)
        return hi ? _("Left/Right  choose    Fire  press    Up/Down  volume    Space  pause    Esc  back")
                  : _("Fire press  Up/Dn vol  Esc back");
    if (p && p->type == PT_LIST && (p->mods || p->songs) && p->list.count &&
        (p->list.items[p->sel].kind == IT_MOD || p->list.items[p->sel].kind == IT_SONG))
        return hi ? _("Up/Down  move    Fire/Return  play    Left/Esc  back") : _("Fire play  Esc back");
    return hi ? _("Up/Down  scroll      Left/Esc  back") : _("Up/Down scroll  Esc back");
}

const char *hint_shown;      /* the hint in the footer now */

/* In a window, used by the mouse, the footer's tips go once seen
 * (FOOT_SEEN_MS), the content taking its room; keys bring them back. A
 * message meanwhile shows in a footer drawn over the content's foot, and
 * the content is drawn again when it goes. */
#define FOOT_SEEN_MS 4000
BOOL foot_hidden, foot_over, foot_redraw;
ULONG foot_since;            /* shown (with the mouse in use) since */


/* Sizing the window by its grip: pressed there, the pointer followed (by
 * the screen's position, the window moving under it) until let go. */
void set_msg(const char *m)
{
    amb_strlcpy(msg, m ? m : "", sizeof(msg));
    if (!np_full)
        draw_footer();                      /* (Flow on the whole screen: shown as it goes) */
}


void draw_title(struct Page *p);


/* The breadcrumb: each level's title, the earlier ones links back to
 * their level (a click, or hover in mouse mode, as list rows). A long
 * one loses its first levels to "...". crumb[]: where each is drawn. */
struct CrumbAt crumb[MAXDEPTH];
int crumb_first;             /* the first level shown (before it: "...") */
int crumb_hover = -1;        /* the level under the pointer, -1 none */

int np_src = -1;
static char np_trail[MAXDEPTH][64];
static int np_trail_n;
char np_src_list[64];
WORD np_src_x0 = -1, np_src_x1 = -1;
void np_goto_source(void);

BOOL np_away(void)
{
    return np_src >= 0 && menu_sel >= 0 && menu_sel < nmenu && np_src != menu[menu_sel] &&
           mp_loaded();
}



void pane_settle(void);


/* Row markers are icons (gfx_mark) in a column on the left before each
 * label. The column is the same width whatever the markers' state, so
 * ticking a box or picking a radio button never moves the text; lists
 * without markers have no column. */
WORD mark_column(const struct Page *p)
{
    const struct ItemList *l = &p->list;
    LONG i;
    if (p->mods || p->radio || p->songs)
        return U(MARK_SIZE + 5);    /* music lists: room for the playing bars, always */
    for (i = 0; i < l->count; i++)
        if (l->items[i].mark != MARK_NONE || l->items[i].kind == IT_MOD)
            return U(MARK_SIZE + 5);
    return 0;
}

/* Keep the selection inside the visible rows; TRUE if the list scrolled. */
BOOL list_follow(struct Page *p)
{
    WORD vis = list_visible();
    LONG top = p->top;
    if (p->sel < p->top)
        p->top = p->sel;
    if (p->sel >= p->top + vis)
        p->top = p->sel - vis + 1;
    return p->top != top;
}

/* ------------------------------------------------------------ letter overlay
 * While a long content list (mostly titles, not category folders) glides,
 * the first letter of the selected title is shown large: centred vertically
 * in the list, its right edge 90% of the way across. It is the UI font's
 * own glyph blown up in whole pixels, kept as a chip-RAM mask.
 *
 * Beside a list with a column of its own (games, pictures, music: the
 * preview's), it takes that column: rows never touch it. Over a list
 * without one (Radio's, Mods', a lowres screen's) it lies over the rows,
 * outlined in the background's colour so it reads over their text: the
 * rows keep their full width (they once shrank away from a strip of its
 * own while it showed, and the whole list jumped sideways, the tags at the
 * right most of all). The rows it covers are painted again, then it,
 * whenever they change under it: scrolling, the selection, a new letter
 * (ov_rows). It goes half a second after the key is let go. */
/* (state is declared with the other globals, near the top) */

static char ov_letter_of(const char *s)
{
    for (; *s; s++) {
        UBYTE c = (UBYTE)*s;
        if (c >= 0x80 && c <= 0x9F)
            return 0;                       /* Japanese, Chinese, Korean: no letter */
        if (c >= 'a' && c <= 'z')
            return (char)(c - 32);
        if ((c >= 'A' && c <= 'Z'))
            return (char)c;
        if (c >= '0' && c <= '9')
            return '#';
        if (c >= 0xC0 && c != 0xD7 && c != 0xF7) {
            /* Latin-1 letters file under their base letter: É under E */
            static const char base[] = "AAAAAAACEEEEIIIIDNOOOOO.OUUUUYTS";
            if (c == 0xFF)
                return 'Y';                 /* y with diaeresis */
            if (c >= 0xE0)
                c -= 32;
            if (c == 0xDF)
                return 'S';
            return base[c - 0xC0] != '.' ? base[c - 0xC0] : 0;
        }
    }
    return 0;
}

/* A title's letter without its article ("The Dogs": D), 0 if it has none */
static char ov_letter_bare(const char *s)
{
    static const char *const art[] = { "the ", "a ", "an " };
    int k;
    for (k = 0; k < 3; k++) {
        const char *a = art[k], *t = s;
        while (*a && (*t == *a || *t == *a - 32))
            a++, t++;
        if (!*a && *t)
            return ov_letter_of(t);
    }
    return 0;
}

/* The letter of entry i as its list is sorted: a server sorting by title
 * without "The" (Plex's titleSort) files The Dogs under D, a folder's
 * names under T. Which, worked out once a list: the way that keeps the
 * letters in order more often. So the big letter only goes on. */
static const struct Item *ovs_items;
static LONG ovs_count = -1;
static BOOL ovs_bare;
char ov_letter_in(const struct Item *it, LONG n, LONG i)
{
    char raw, bare;
    if (i < 0 || i >= n || !it[i].label)
        return 0;
    if (it != ovs_items || n != ovs_count) {
        LONG k, back_raw = 0, back_bare = 0;
        char pr = 0, pb = 0;
        for (k = 0; k < n; k++) {
            char r = it[k].label ? ov_letter_of(it[k].label) : 0, b;
            if (!r)
                continue;
            b = ov_letter_bare(it[k].label);
            if (!b)
                b = r;
            if (pr && r < pr)
                back_raw++;
            if (pb && b < pb)
                back_bare++;
            pr = r;
            pb = b;
        }
        ovs_items = it;
        ovs_count = n;
        ovs_bare = back_bare < back_raw;
    }
    raw = ov_letter_of(it[i].label);
    if (ovs_bare && (bare = ov_letter_bare(it[i].label)))
        return bare;
    return raw;
}

/* Long and mostly titles: the kind of list the letter helps with. */
static BOOL ov_eligible(const struct Page *p)
{
    LONG i, runs = 0;
    if (p->type != PT_LIST || p->list.count <= list_visible())
        return FALSE;
    if (p->igame == 2)
        return FALSE;                       /* iGame's most played: not by title */
    if (p->radio && !p->plex)
        /* Radio: Countries alone is A to Z (this Amiga's first); stations
         * and genres come by votes, listeners or size, where a letter
         * would say nothing */
        return !strcmp(p->base, RADIO_COUNTRIES);
    for (i = 0; i < p->list.count; i++)
        if (p->list.items[i].kind == IT_RUN || p->list.items[i].kind == IT_PIC ||
            p->list.items[i].kind == IT_ANIM || p->list.items[i].kind == IT_MOD || p->list.items[i].kind == IT_SONG ||
            p->list.items[i].kind == IT_PLEXDIR ||
            (p->list.items[i].kind == IT_DIR && (p->songs || p->mods || p->pics)))
            runs++;                         /* lists in title order (not a track list's);
                                             * folders of music, modules and pictures
                                             * by name too (a library's artists) */
    return runs * 2 >= p->list.count;
}

/* The preview pane: on screens 640 or more wide, a list with games in it
 * takes the left half of the pane, and the right half, with no border,
 * shows the selected game: its screenshot and fact sheet (all of it on
 * screens taller than 256 lines, the key lines on shorter ones). While a
 * long list glides, the same space shows the index letter; rows never
 * touch it, so neither ever covers a title. Lowres screens keep the
 * full-width list, with the letter over its right side. */
WORD aside_w = -1;           /* -1: work out (after a screen opens) */

WORD aside_width(void)
{
    if (aside_w < 0)
        aside_w = !NARROW ? g.pane_w / 2 : 0;
    return aside_w;
}

BOOL has_aside(struct Page *p)
{
    if (!p || p->type != PT_LIST)
        return FALSE;
    if (p->style_pv)
        return aside_width() > 0;
    if (p->plex && p->radio) {
        /* artists and albums: the details pane (worked out each time: the
         * list comes after the page) */
        LONG i;
        if (aside_width() <= 0)
            return FALSE;
        if (p->plex_meta[0])
            return TRUE;                    /* an album's tracks: the album beside them */
        for (i = 0; i < p->list.count; i++)
            if ((p->list.items[i].kind == IT_PLEXDIR && strchr(p->list.items[i].path, '\t')) ||
                p->list.items[i].kind == IT_PLEXPHOTO ||
                (p->list.items[i].kind == IT_ANIM && jf_key(p->list.items[i].path)))
                return TRUE;                /* (a Jellyfin film: its poster) */
        return FALSE;
    }
    if (!p->aside) {
        LONG i;
        p->aside = 1;
        if ((p->pics || (p->songs && !p->plex)) && aside_width() > 0)
            p->aside = 2;           /* pictures and folders (music files) both preview */
        else if (aside_width() > 0)
            for (i = 0; i < p->list.count; i++)
                if (p->list.items[i].kind == IT_RUN) {
                    p->aside = 2;
                    break;
                }
    }
    return p->aside == 2;
}

/* Left edge of the letter column. */
WORD aside_x(void)
{
    return g.pane_x + g.pane_w - aside_width();
}

struct Gutter gut;
BOOL bar_up;                 /* the mouse in use lately: the thumb bright */
ULONG mouse_last;            /* when the mouse last moved or clicked */
#define BAR_HIDE 1500               /* ms the mouse rests before the bar and the
                                     * pointer go */

WORD list_visible(void);




/* ---- the preview of the selected game, drawn once the selection settles:
 * Classic's pane, the view of the entry details service (details.c) */
ULONG dc_time(void);
BOOL hold_active(void);


/* Every job back and freed: before the screen closes or a game runs. */
static void pv_flush(void)
{
    det_flush();
    np_art_free();                          /* Now playing's cover too */
}

/* The details' jobs let go without waiting for them, for the viewer: one
 * still out is orphaned where it holds nothing of the screen's (a
 * colour-table screen: decoded unremapped, no pens) - waiting could take
 * minutes (every preview queued ran first, each a JPEG of seconds) */
static void pv_let_go(void)
{
    det_let_go(!g.truecolor);
}

/* Colours a picture may have (0: as many as the screen gives): fewer for a
 * Plex cover on a slower Amiga, less to reduce and to set. */
int pv_colour_cap;
int plex_colours(void)
{
    return plex_tier() == 0 ? 48 : plex_tier() == 1 ? 64 : 0;
}

BOOL pv_pictures(void)
{
    return g.pens_shared || g.truecolor;
}

/* A Pictures row's lines: what it is and, for a folder, what Fire and
 * Right do there (the picture's size once it is drawn). */


static BOOL pv_spinning(void)
{
    return det.shown && det.has_box && !det.pic_shown && (det.sj || det.tj);
}


/* What the picture place shows now: the file (a folder's: its first
 * picture) and the place, so an entry that shows the same picture in the
 * same place (a folder, then its first picture inside it; and back) keeps
 * it on screen: not cleared, read or drawn again. */
struct OnScreen pv_onscr;



char np_art_file[120];       /* the cover Now playing shows (or will) */
static char np_art_key[170];        /* ...a Plex track's: its thumb as a layout's picture key
                                     * ("\x04" thumb), fetched at the size the layout shows it */


/* ---- Classic's pane as the details' view (details.h): what each step
 * of reading an entry's details draws */
BOOL cv_has_pane(struct Page *p)
{
    return has_aside(p);
}
void cv_job(struct LJob *j)
{
    j->scr = g.scr;
    j->remap = g.truecolor;
    if (j->type == LJ_SHOT && gfx_thumbs(&g)) {
        /* made small in the loader, in colours of its own (thumb.c) */
        j->remap = FALSE;
        j->tw = aside_width() - U(4);
        j->th = pv_box_h;
        j->tall = gfx_mode_tall(g.mode);
        j->maxcol = gfx_thumb_colours(&g);
        if (pv_colour_cap && j->maxcol > pv_colour_cap)
            j->maxcol = pv_colour_cap;
    }
}

/* ---- a layout as the details' view: only told the text came (lv_model
 * reads det.tj); its pictures are its own (lv_picture, at its size) */
static void lvd_text(struct Page *p)
{
    (void)p;
    lv_note_what(LC_DETAILS);
}
/* a Plex list's details: the layout's from the items (lv_model); only
 * what the details service keeps */
static void lvd_plex(struct Page *p, BOOL was_shown)
{
    (void)p;
    (void)was_shown;
    det.has_box = FALSE;
    det.text_shown = TRUE;
}
static const struct DetView layout_view = {
    cv_has_pane, NULL, NULL, cv_job, NULL, NULL, NULL, NULL, NULL, lvd_plex, NULL, NULL, lvd_text, NULL,
    NULL
};

void preview_draw(struct Page *p)
{
    det_begin(p);
}

/* Jobs came back from the loader: letters of long lists, previews. */
static void preview_arrived(void)
{
    struct Page *p = cur();
    int d;
    while (ldr_collect())
        ;
    for (d = 0; d < depth; d++) {
        if (pages[d].ch && pages[d].ch->wjob && pages[d].ch->wjob->done)
            ch_saved(&pages[d]);
        if (pages[d].ch && pages[d].ch->job && pages[d].ch->job->done)
            ch_arrived(&pages[d], &pages[d] == p);
    }
    if (np_aj && np_aj->done && !np_art_drawn)
        ui->np_art();                       /* Now playing's cover is made */
    det_arrived(p);                         /* the entry's details: to the view */
}

/* A job back but not dealt with (collected while waiting for another). */
static BOOL loader_due(void)
{
    int d;
    if (det_due())
        return TRUE;
    for (d = 0; d < depth; d++)
        if (pages[d].ch && ((pages[d].ch->job && pages[d].ch->job->done) ||
                            (pages[d].ch->wjob && pages[d].ch->wjob->done)))
            return TRUE;
    return FALSE;
}

/* Show the preview's text now, waiting for the loader if need be (Right
 * into the pane: its scroll bar must be known). */
static void preview_now(struct Page *p)
{
    preview_draw(p);
    if (det.tj && !det.text_shown) {
        ldr_wait(det.tj);
        preview_arrived();
    }
}

/* The selection changed (or the list was drawn): preview it once it rests. */
/* ---- Settings > Style on a laced screen: beside the list of fonts, the
 * Home menu as it would look in the one selected, at its own size (its
 * row height, margins and icons scaled as the layout would be). */
struct TextFont *spv_font;   /* the font previewed, open */
int spv_idx = -1;

static void style_preview_free(void)
{
    if (spv_font)
        CloseFont(spv_font);
    spv_font = NULL;
    spv_idx = -1;
}


static BOOL plex_art_kept(struct Page *p, LONG idx);

static void ov_free(void)
{
    ov.bl = NULL;
    ov.on = FALSE;
    ov.letter = 0;
}

/* Build the big letter for c, its ink about 40% of the list's height
 * (gfx_bigletter: the font's glyph in whole pixels, or smooth). A layout
 * draws the same glyph (lvh_letter_up); Classic places it (letter_place). */
BOOL ov_build(char ch)
{
    WORD rows_h = list_visible() * g.list_row_h;
    const struct BigLetter *bl = gfx_bigletter(&g, (UBYTE)ch, rows_h * 2 / 5);
    if (!bl)
        return FALSE;
    ov.bl = bl;
    ov.letter = ch;
    ui->letter_place();                     /* (where Classic stamps it) */
    return TRUE;
}


void draw_list_rows(struct Page *p);


/* Stamp the letter: in its column as it is, or over the rows outlined in
 * the background's colour (so the rows' text under it does not run into
 * it). On a native screen only the list's planes are written: its pens
 * (0 and 4) and the rows' under it are below 8 (16 with a Now playing
 * row), and planes above are clear there, in its column too (cleared on
 * all planes before it shows): nine template blits a step on 3 planes,
 * not 8. */


WORD mark_column(const struct Page *p);
void draw_list_row(struct Page *p, LONG idx, WORD col);


static void ov_show(struct Page *p)
{
    char c = ov_letter_in(p->list.items, p->list.count, p->sel);
    ov.page = p;
    if (!c || !ov_build(c))
        return;
    ov.on = TRUE;
    ov.linger = 0;
    if (has_aside(p))
        det.shown = FALSE;                  /* (the letter in the details' place) */
    ui->letter_show(p);
}


/* a layout: its own letter (LPage.letter), told */
static void ly_letter_show(struct Page *p)
{
    (void)p;
    lv_note_what(LC_SELECT);
}

static void ly_letter_relabel(char c)
{
    if (!ov_build(c)) {
        if (!has_aside(ov.page))
            ov.bl = NULL;
        return;
    }
    lv_note_what(LC_SELECT);
}

static void ly_letter_hide(struct Page *p)
{
    (void)p;
    lv_note_what(LC_SELECT);
}

/* Changing letters off screen: the old letter's box and the new one's,
 * cleared and the new letter stamped there, then copied up in one go with
 * the beam (gfx_blit_beam). Cleared and stamped on the screen, the letter
 * flickered: a smooth glide changes it nearly every frame. */
struct BitMap *ov_bm;
WORD ov_bw, ov_bh;

void ov_bm_free(void)
{
    if (ov_bm) {
        WaitBlit();
        FreeBitMap(ov_bm);
        ov_bm = NULL;
    }
}

/* A new letter (A -> D...): the old one's box cleared, the new one
 * stamped. Only the strip changes; the rows are not touched. */
static ULONG ov_ms, ov_n;           /* time in ov_relabel during a glide (log) */
static void ov_relabel_(char c);
void ov_relabel(char c)
{
    ULONG t0 = time_ms();
    char was = ov.letter;
    ov_relabel_(c);
    if (ov.letter != was) {
        ov_ms += time_ms() - t0;
        ov_n++;
    }
}

static void ov_relabel_(char c)
{
    if (!ov.on || !c || c == ov.letter)
        return;
    ui->letter_relabel(c);
}


static void ov_hide(void)
{
    struct Page *p = ov.page;
    BOOL was = ov.on;
    ov.on = FALSE;
    if (!was || !p || p != cur() || p->type != PT_LIST || !g.win)
        return;
    ui->letter_hide(p);
    if (has_aside(p)) {
        det_touch(p);           /* and the preview comes back */
        det.due = time_ms();
    }
}


/* One row of the list in its slot, background and all. Rows are painted
 * one by one over what was there, never after clearing the whole list,
 * so fast scrolling does not flash. */
const char *row_tag(const struct Page *p, const struct Item *it);

/* The Now playing row at a list's top (Mods, Music, Radio, Plex): drawn
 * in the accent's orange, so it does not read as one of the list's. */
BOOL np_row(const struct Item *it)
{
    return it->kind == IT_ACTION && it->action == ACT_NOWPLAYING;
}




/* The bar again as it moves (each step that scrolls the list, the mouse
 * showing, hiding or dragging it): planes 0-2, not the screen's 8. */
void draw_gutter(struct Page *p)
{
    draw_gutter_planes(p, 0x07);
}

void draw_list_row(struct Page *p, LONG idx, WORD col);
void draw_menu_row(int i);
WORD mark_column(const struct Page *p);

BOOL menu_layout(void);



/* The pointer gone (hidden, or the joystick used): the menu column may fold. */
static void hover_none(void)
{
    BOOL was = hover_strip;
    hover_strip = FALSE;
    if (hover_row >= 0 || hover_menu >= 0)
        hover_set(-1, -1);
    else if (was && !pane_pending)
        menu_layout();
}

static void pointer_hide(void)
{
    if (!g.win || ptr_hidden == g.win)
        return;
    if (!ptr_blank && !(ptr_blank = AllocMem(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR)))
        return;
    SetPointer(g.win, ptr_blank, 1, 16, 0, 0);
    ptr_hidden = g.win;
    hover_none();                           /* nothing under a pointer not there */
    if (crumb_hover >= 0) {
        crumb_hover = -1;
        if (cur() && !pane_pending)
            draw_title(cur());
    }
}

void pointer_show(void)
{
    if (g.win && ptr_hidden == g.win)
        ClearPointer(g.win);
    ptr_hidden = NULL;
}

BOOL np_on(void);
void np_update(void);

static void foot_show(BOOL on);

void set_mouse_mode(BOOL on)
{
    struct Page *p = cur();
    if (on == mouse_mode)
        return;
    mouse_mode = on;
    foot_since = on ? time_ms() : 0;
    if (!on && foot_hidden)
        foot_show(TRUE);                    /* keys: their tips again */
    if (p && p->type == PT_LIST && !pane_pending)
        draw_gutter(p);
    if (!on)
        hover_none();                       /* the joystick: no pointer to follow */
    if (np_on())
        np_update();                        /* the yellow frame, or the mouse's light */
    if (lv_on())
        lv_note_what(LC_SELECT);            /* (a layout: Home's chosen tile shown, or not) */
}


/* Copy n visible rows from screen row src to screen row dst, in place on
 * screen. Native screens: a blit inside the screen bitmap, masked to the
 * list's planes (list_planes: every list pen is below 8, or 16 with a Now
 * playing row, and rows are painted with RectFill, so the planes above
 * are zero there). RTG: the same blit, unmasked;
 * the RTG system handles the overlap and accelerates it. */
/* Right edge of what rows top..top+vis-1 have painted outside the bar:
 * the copy need go no further, since everything right of it is background
 * in every row. Measured each move (TextLength is cheap); list entries
 * stay read-only here. */
/* The tag a row shows at its right (a setting's value, a picture's format,
 * a folder's count, a station's bitrate): "" none. */
const char *row_tag(const struct Page *p, const struct Item *it)
{
    if (is_dvd_row(it))
        return "";                          /* (its disc says it) */
    return it->kind == IT_ACTION ? it->path
         : (p->pics || p->radio || it->kind == IT_PLEXDIR) ? it->sortkey : "";
}


/* After the selection moved from old_sel (list top was old_top): repaint
 * just the old and new rows when the list did not scroll; when it scrolled
 * by less than a screenful, move the rows already on screen and paint only
 * what changed; otherwise repaint every row.
 *
 * The highlighted row is never copied: the move is split around it. A
 * copied highlight would sit one row away from the real one until that row
 * was repainted, so the bar flickered across two rows. Instead the row the
 * old selection moves into is painted plain, and the bar's own row is
 * repainted in place (in the same colour, so a highlight pinned at the
 * edge of the list never moves at all). */


/* (Re)build the page's text box below y0, keeping its scroll position. */
void np_draw(BOOL full);

/* A text page with a picture (the credits): the text scrolls in a box at
 * the top; under it, fixed, the note and the picture, as large as fits in
 * up to two fifths of the pane. The box's height (the picture made first,
 * at its size). */
/* Plex in Ambiance's "Works with" Plex's logo, under everything: its height */
WORD plexmark_h(void)
{
    return U(6) + plex_lo_h * g.kint + U(3) + g.font_h;
}

WORD text_box_h(struct Page *p, WORD y0, WORD *note_lines)
{
    WORD avail = g.footer_y - 2 - y0, lh = g.font_h + U(1), note_h = 0, ph;
    *note_lines = 0;
    if (p->type == PT_TEXT && p->plexmark)
        avail -= plexmark_h();
    if (p->type != PT_TEXT || !p->pic[0])
        return avail;
    if (p->note) {
        struct ItemList l;
        if (list_init(&l)) {
            wrap_into(&l, p->note, textbox_w());
            *note_lines = (WORD)l.count;
            list_free(&l);
        }
        note_h = *note_lines * lh + U(4);
    }
    ph = avail * 2 / 5 - note_h;
    if (ph < U(20))
        return avail - note_h - U(8);       /* no room for the picture: the note alone */
    if (!p->pic_bm)
        page_picture(p, textbox_w(), ph);
    return avail - note_h - (p->pic_bm ? p->pic_h : 0) - U(8);
}

/* A layout's measure of a text page (LAC_TEXTFIT): its view's height and
 * the whole text's, in its pixels. The text box (not drawn under a layout)
 * takes the same proportion - its height that part of its text's - so its
 * keys, wheel and scroll bar go the layout's whole way. */
static struct Page *lv_tfit_p;
static ULONG lv_tfit;
static BOOL lv_tfit_apply(struct Page *p)
{
    LONG vis, tot, h, mx;
    if (!p || p != lv_tfit_p || !p->tb.open || !lv_tfit)
        return FALSE;
    vis = (LONG)(lv_tfit >> 16);
    tot = (LONG)(lv_tfit & 0xFFFF);
    h = tot > vis && vis > 0 ? (LONG)((p->tb.content_h * (ULONG)vis) / (ULONG)tot) : p->tb.content_h;
    if (h < p->tb.line_h)
        h = p->tb.line_h;
    if (h > 32767)
        h = 32767;
    if (h == p->tb.h)
        return FALSE;
    p->tb.h = (WORD)h;
    mx = p->tb.content_h - h;
    if (mx < 0)
        mx = 0;
    if (p->tb.pos > mx)
        p->tb.pos = mx;
    if (p->tb.target > mx)
        p->tb.target = mx;
    return TRUE;
}

void open_textbox(struct Page *p, WORD y0)
{
    WORD lines;
    LONG pos = p->tb.pos, target = p->tb.target;
    BOOL was = p->tb.open;
    p->tb.pic = NULL;                       /* the picture is under the box, not in it */
    tb_open(&g, &p->tb, g.pane_x, y0, textbox_w(), text_box_h(p, y0, &lines), &p->list,
            p->type == PT_DETAIL);
    if (!was && p == lv_tfit_p)
        lv_tfit = 0;                        /* (another page in that place: its own measure) */
    if (lv_on() && was && p == lv_tfit_p && lv_tfit && p->tb.open) {
        p->tb.pos = pos;                    /* (where it was, by the layout's measure) */
        p->tb.target = target;
        p->tb.h = 0;
        lv_tfit_apply(p);
    }
}


struct TextBox *cur_tb(void)
{
    struct Page *p = pane_pending ? NULL : cur();   /* a stale section is not shown */
    return (p && p->type != PT_LIST && p->tb.open) ? &p->tb : NULL;
}


void set_focus(int f);

/* ------------------------------------------------------------ music
 * Mods: the folders of modules (mods.c) and the Now Playing page
 * (PT_PLAYER), the music itself on Paula (modplay.c). The music plays on
 * wherever Ambiance goes, until it is stopped or a game starts; at the end
 * of a song the next module in its folder follows.
 *
 * The page: the module's title, what it is, a bar of the time played, the
 * four channels' meters with the sample each is playing, the buttons
 * (previous, back a position, play/pause, stop, forward a position, next,
 * quieter, louder) and the module's sample names, where trackers left their
 * messages. Left/Right choose a button and Fire presses it; Up/Down change
 * the volume; Space plays and pauses; the CD32 pad's shoulders and N/P skip
 * modules. While it is shown it is brought up to date 25 times a second,
 * only what changed, and (double-buffered) without a flip. */
#define VOL_STEP   4

/* (a layout presses them by these numbers: LAC_BUTTON) */
typedef char lnb_as_nb[(int)LNB_PREV == (int)NB_PREV && (int)LNB_PLAY == (int)NB_PLAY &&
                       (int)LNB_LOOP == (int)NB_LOOP && (int)LNB_VUP == (int)NB_VUP &&
                       (int)LNB_COUNT == (int)NB_COUNT ? 1 : -1];

/* Loop: what plays, again at its end (a module from its start, a track or
 * file played again, a video: anim.c), kept (Ambiance/Loop). A station
 * has none. */
BOOL play_loop;
#define LOOP_VAR "Ambiance/Loop"

struct NowPlaying np = { .idx = -1, .btn = NB_PLAY, .flash = -1, .s_flash = -1, .hov = -1, .s_hov = -1,
         .rep = -1, .s_rep = -1 };

#define FLASH_MS   80               /* how long a pressed button stays lit */
#define REP_DELAY  400              /* a button held: it repeats after this, */
#define REP_EVERY  100              /* then this often */

/* The button Fire presses (chosen with Left/Right) has a yellow rounded
 * frame; with the mouse there is none: what it is over lights up instead. */
BOOL np_focus_shown(void)
{
    return focus == FOCUS_CONTENT && !mouse_mode;
}

/* Held down, these press again and again. */
static BOOL np_repeats(int b)
{
    if (b == NB_BACK || b == NB_FWD)
        return mp_can_step(b == NB_FWD ? 1 : -1);
    return b == NB_VDOWN || b == NB_VUP;
}


/* Flow on the whole window or screen (F on Now playing): the page
 * behind it drawn again when it goes (a key, a click, the fire button);
 * np_full itself near the top (set_msg looks at it) */
static BOOL np_full_first;
static ULONG np_full_since, np_full_serial;

BOOL np_on(void)
{
    struct Page *p = pane_pending ? NULL : cur();
    return p && p->type == PT_PLAYER && !np_full;
}

/* The page is up and something on it moves. */
static BOOL np_live(void)
{
    return np_on() && mp_running();
}

/* Muted: the volume it had, given back when unmuted (-1: not muted).
 * Not kept past a quit: Ambiance starts at its volume, never silent. */
static int vol_unmuted = -1;

BOOL np_muted(void)
{
    return vol_unmuted >= 0;
}

static void np_mute(BOOL on)
{
    if (on == np_muted())
        return;
    if (on) {
        vol_unmuted = mp_volume();
        mp_set_volume(0);
    } else {
        int v = vol_unmuted;
        vol_unmuted = -1;
        mp_set_volume(v > 0 ? v : 32);
    }
}

int np_state(void)
{
    return !mp_loaded() ? 0 : mp_playing() ? 1 : mp_paused() ? 2 : 3;
}





/* The meters' rows the page is laid out for: a stream's two always (a
 * mono one's single meter in the first, the second left empty), so it
 * does not change size when the sound turns out mono or stereo. */
int np_meter_rows(void)
{
    if (!mp_loaded())
        return 4;
    if (vis_rows())
        return vis_rows();              /* the visualization's own (the spectrum: 4) */
    return mp_is_stream() ? 2 : mp_meter_count();
}


BOOL np_row(const struct Item *it);

/* The playing module marked in any Mods list open (redraw: and the list
 * on screen drawn again, when the music moved on by itself). */
static void np_mark_lists(BOOL redraw)
{
    int d;
    LONG i;
    const char *now = mp_loaded() ? mp_path() : "";
    for (d = 0; d < depth; d++) {
        struct Page *p = &pages[d];
        if (p->type != PT_LIST || (!p->mods && !p->radio && !p->plex && !p->songs))
            continue;
        for (i = 0; i < p->list.count; i++) {
            struct Item *it = &p->list.items[i];
            if (it->kind == IT_SONG)
                it->mark = now[0] && np_local() && it->path && !amb_stricmp(it->path, now)
                           ? MARK_PLAYING : MARK_NONE;
            else if (it->kind == IT_MOD)
                it->mark = (now[0] && it->path && !amb_stricmp(it->path, now)) ? MARK_PLAYING
                                                                                : MARK_NONE;
            else if (it->kind == IT_STATION) {
                char url[256];
                radio_field(it, 0, url, sizeof(url));
                it->mark = now[0] && mp_is_stream() && !strcmp(url, now) ? MARK_PLAYING
                                                                         : MARK_NONE;
            } else if (it->kind == IT_PLEXTRACK) {
                /* by its file's key: a conversion's URL differs each time */
                char key[300];
                radio_field(it, 0, key, sizeof(key));
                it->mark = now[0] && mp_is_track() && !strcmp(key, plex_now) ? MARK_PLAYING
                                                                             : MARK_NONE;
            } else if (it->kind == IT_PLEXDIR && p->plex &&
                       !strncmp(it->path, "/library/metadata/", 18)) {
                /* an artist or album: the playing bars where the track
                 * playing is of it, its arrow otherwise (other rows, such
                 * as Other libraries, keep theirs) */
                char key[300];
                radio_field(it, 0, key, sizeof(key));
                it->mark = now[0] && mp_is_track() &&
                           ((plex_now_in[0][0] && !strcmp(key, plex_now_in[0])) ||
                            (plex_now_in[1][0] && !strcmp(key, plex_now_in[1])))
                           ? MARK_PLAYING : MARK_ARROW;
            }
        }
    }
    {
        /* each open list's Now playing row, as playback is now */
        BOOL moved = FALSE;
        for (d = 0; d < depth; d++)
            if (plex_shortcut_fix(&pages[d]) && &pages[d] == cur())
                moved = TRUE;
        if (redraw && moved && !pane_pending) {
            draw_pane(TRUE);                /* (its rows all moved down or up) */
            return;
        }
    }
    if (redraw && !pane_pending && cur() && cur()->type == PT_LIST &&
        (cur()->mods || cur()->radio || cur()->plex || cur()->songs))
        draw_list_rows(cur());
}

/* What a Plex track plays as: MP3 when the server converts it, else its
 * own format (MP3, FLAC). */
static const char *plex_tag(const struct Item *it)
{
    static char t[16];
    char *c;
    if (plex_converts(it))
        return "MP3";
    radio_field(it, 1, t, sizeof(t));
    for (c = t; *c; c++)
        if (*c >= 'a' && *c <= 'z')
            *c -= 32;
    return t[0] ? t : "MP3";
}

/* Load playlist entry idx and play it; FALSE (and why, in msg) if not. */
static BOOL np_start_now(LONG idx);
static BOOL np_no_play;             /* np_start_now: loaded only, not played (a resume) */
static ULONG np_resume_ms;          /* loaded at the start, stopped: Play goes on from here */
static ULONG np_resume_total, np_resume_size;  /* (a track's length and file, then) */

static BOOL np_start(LONG idx)
{
    BOOL ok;
    np.seeked = FALSE;                      /* a new track, from its start */
    busy_was_shown = FALSE;
    ok = np_start_now(idx);
    if (!ok && msg[0] && np.list.pool && idx >= 0 && idx < np.list.count) {
        char m[220];
        sprintf(m, "%.100s: %.100s", np.list.items[idx].label, msg);
        amb_log("music: not played", m);
    }
    return ok;
}

/* A track or station started; drawn by the caller, once (the page it is
 * on, or Now Playing opening over it): nothing drawn here in between. */
static BOOL np_start_now(LONG idx)
{
    char err[100];
    struct Item *it;
    if (!np.list.pool || idx < 0 || idx >= np.list.count)
        return FALSE;
    if (!np_no_play)
        np_resume_ms = 0;                   /* something else: no going on from before */
    it = &np.list.items[idx];
    busy_begin();
    if (it->kind == IT_STATION) {
        char url[256];
        radio_field(it, 0, url, sizeof(url));
        if (!mp_load_stream(url, it->label, it->sortkey, plex_decoder_cpu(), err, sizeof(err))) {
            busy_end(FALSE);
            np.idx = idx;
            set_msg(err);
            return FALSE;
        }
        plex_now[0] = 0;                    /* (no Plex track now) */
        plex_now_in[0][0] = plex_now_in[1][0] = 0;
        np_local_art = FALSE;
        np_art_start(NULL);                 /* a station: the last track's cover goes */
    } else if (it->kind == IT_PLEXTRACK) {
        char url[2048], by[200], album[100], ms[16];
        BOOL ok_track;
        if (it->flags & ITEM_DISABLED) {
            busy_end(FALSE);
            np.idx = idx;
            set_msg(plex_why(it));
            return FALSE;
        }
        plex_track_url(it, caps.cpu, 0, url, sizeof(url));
        radio_field(it, 0, plex_now, sizeof(plex_now));
        {
            char rk[48];
            int k;
            for (k = 0; k < 2; k++) {
                radio_field(it, 11 + k, rk, sizeof(rk));
                if (jf_key(rk))
                    amb_strlcpy(plex_now_in[k], rk, sizeof(plex_now_in[k]));  /* (its row's key) */
                else if (rk[0])
                    sprintf(plex_now_in[k], "/library/metadata/%s/children", rk);
                else
                    plex_now_in[k][0] = 0;
            }
        }
        {
            char f[16], key[160];
            radio_field(it, 1, plex_orig.codec, sizeof(plex_orig.codec));
            radio_field(it, 6, f, sizeof(f));
            plex_orig.kbps = f[0] ? strtoul_(f) : 0;
            radio_field(it, 7, f, sizeof(f));
            plex_orig.ch = f[0] ? strtoul_(f) : 0;
            radio_field(it, 8, f, sizeof(f));
            plex_orig.rate = f[0] ? strtoul_(f) : 0;
            plex_orig.conv = plex_converts(it);
            radio_field(it, 5, key, sizeof(key));
            if (!plex_orig.rate && key[0] && !jf_key(key))
                plex_ask_rate(key);         /* (Plex's: Jellyfin's lists say no rates) */
        }
        radio_field(it, 2, ms, sizeof(ms));
        radio_field(it, 3, by, sizeof(by));
        radio_field(it, 4, album, sizeof(album));
        if (album[0]) {
            amb_strlcat(by, by[0] ? "   " : "", sizeof(by));
            amb_strlcat(by, album, sizeof(by));
        }
        {
            ULONG v = 0;
            const char *d = ms;
            while (*d >= '0' && *d <= '9')
                v = v * 10 + (*d++ - '0');
            ok_track = mp_load_track(url, it->label, by, plex_tag(it), v, plex_decoder_cpu(), 0, 0, err,
                                     sizeof(err));
        }
        if (!ok_track) {
            busy_end(FALSE);
            np.idx = idx;
            set_msg(err);
            return FALSE;
        }
        np_art_start(it);                   /* its cover, when it comes */
    } else if (it->kind == IT_SONG) {
        /* a music file on this Amiga: played as a Plex track is, from the
         * file; its tags name it, its picture (folder's or its own) shows */
        static struct SongInfo si;
        char by[220], art[300];
        song_info(it->path, &si, art, sizeof(art), TRUE);
        if (si.artist[0] || si.band[0] || si.album[0]) {
            amb_strlcpy(by, si.artist[0] ? si.artist : si.band, sizeof(by));
            if (si.album[0]) {
                amb_strlcat(by, by[0] ? "   " : "", sizeof(by));
                amb_strlcat(by, si.album, sizeof(by));
            }
        } else {
            amb_strlcpy(by, np.folder, sizeof(by));
        }
        if (!mp_load_track(it->path, si.title[0] ? si.title : it->label, by,
                           song_format(it->path), 0, plex_decoder_cpu(), 0, 0, err,
                           sizeof(err))) {
            busy_end(FALSE);
            np.idx = idx;
            set_msg(err);
            return FALSE;
        }
        plex_now[0] = 0;
        plex_now_in[0][0] = plex_now_in[1][0] = 0;
        np_local_art = FALSE;
        np_art_start(NULL);                 /* the last cover goes */
        if (art[0]) {
            np_local_art = TRUE;
            np_layout();                    /* its square */
            amb_strlcpy(np_art_file, art, sizeof(np_art_file));
            if (np.art_s && !(np_on() && pic_take(art, np.art_x, np.art_y, np.art_s, np.art_h)))
                np_art_load(art);
            else if (np.art_s)
                np_art_drawn = TRUE;        /* the pane's: moved across */
        }
    } else {
        np_art_start(NULL);                 /* a module: no cover (the last track's goes) */
        if (!mp_load(it->path, err, sizeof(err))) {
            busy_end(FALSE);
            np.idx = idx;
            set_msg(err);
            np_mark_lists(FALSE);
            return FALSE;
        }
    }
    busy_end(FALSE);
    np.idx = idx;
    np.ended_seen = FALSE;
    np.seek_due = 0;
    np.fail_due = 0;
    if (np_no_play) {
        np_mark_lists(FALSE);               /* (loaded, stopped: Now playing's row shows it) */
        return TRUE;
    }
    if (!mp_play(err, sizeof(err))) {
        set_msg(err);
        np_mark_lists(FALSE);
        return FALSE;
    }
    np_mark_lists(FALSE);
    return TRUE;
}

/* Another module of the playlist (d: +1 next, -1 previous), passing over
 * any that cannot be played. */
static BOOL np_skip(int d)
{
    LONG i, tries;
    if (!np.list.pool || !np.list.count)
        return FALSE;
    BOOL was_track = mp_is_track();
    for (i = np.idx + d, tries = 0; i >= 0 && i < np.list.count && tries < 8; i += d, tries++)
        if (np_start(i)) {
            if (np_on() && was_track && mp_is_track() && np.w && !busy_was_shown)
                np_soft_redraw();           /* track to track: what changed, over itself */
            else if (np_on()) {
                draw_pane(TRUE);
                draw_footer();
            } else {
                np_mark_lists(TRUE);        /* the list on screen: its playing mark moves */
                if (busy_was_shown)
                    draw_pane(TRUE);        /* where the spinner was */
            }
            return TRUE;
        }
    return FALSE;
}

/* A track's page brought up to date without clearing it: its lines drawn
 * over themselves, the controls, the bar and time; its cover stays until
 * the next one is made (or goes, if it has none). */
static void np_soft_redraw(void)
{
    ui->np_rate();
    np.s_state = -99;                       /* the buttons: all looked at again */
    np_update();
    if (!msg[0] && hint_shown && hint_text() != hint_shown)
        draw_footer();
}

/* A track from ms on (a seek): asked of the server again from there (a
 * conversion from that time, a stored file from that byte); delay > 0
 * waits that long first, as Back and Forward held keep moving the time. */
static void np_track_from(LONG idx, ULONG ms, ULONG total, ULONG size, const char *title_was,
                          const char *by_was);

static void np_seek_track(ULONG ms, ULONG delay)
{
    struct MpStatus st;
    if (!mp_is_track() || !np.list.pool || np.idx < 0 || np.idx >= np.list.count)
        return;
    mp_status(&st);
    if (st.total_ms && ms + 1000 > st.total_ms)
        ms = st.total_ms > 1000 ? st.total_ms - 1000 : 0;
    if (delay) {
        np.seek_to = ms;
        np.seek_due = time_ms() + delay;
        if (!np.seek_due)
            np.seek_due = 1;
        if (np_on())
            np_update();                    /* the bar and time show where to */
        return;
    }
    np.seek_due = 0;
    np.seeked = TRUE;                       /* failing, it stays: not the next track */
    np_track_from(np.idx, ms, st.total_ms, st_size(), mp_stream_name(), mp_track_by());
}

/* Playlist track idx played from ms on, asked of the server (or the file
 * read) from there: total, size, title and by as the track had them
 * (a file's: its tags' names). */
static void np_track_from(LONG idx, ULONG ms, ULONG total, ULONG size, const char *title_was,
                          const char *by_was)
{
    struct Item *it;
    char url[2048], by[200], album[100], err[100];
    if (!np.list.pool || idx < 0 || idx >= np.list.count)
        return;
    it = &np.list.items[idx];
    if (it->kind == IT_SONG) {
        /* a file: FLAC finds the time itself; an MP3 from about its byte */
        ULONG at = 0;
        BOOL mp3 = !strcmp(song_format(it->path), "MP3");  /* FLAC, PCM: by time */
        char title[120];
        if (mp3 && total && size)
            at = (ULONG)(((unsigned long long)size * ms) / total);
        amb_strlcpy(title, title_was, sizeof(title));      /* its tags' names, kept */
        amb_strlcpy(by, by_was, sizeof(by));
        if (!mp_load_track(it->path, title, by, song_format(it->path), total,
                           plex_decoder_cpu(), ms, at, err, sizeof(err)) ||
            !mp_play(err, sizeof(err)))
            set_msg(err);
        return;
    }
    plex_track_url(it, caps.cpu, ms, url, sizeof(url));
    radio_field(it, 3, by, sizeof(by));
    radio_field(it, 4, album, sizeof(album));
    if (album[0]) {
        amb_strlcat(by, by[0] ? "   " : "", sizeof(by));
        amb_strlcat(by, album, sizeof(by));
    }
    if (!mp_load_track(url, it->label, by, plex_tag(it), total, plex_decoder_cpu(), ms,
                       plex_converts(it) ? 0 : plex_track_byte(it, ms), err, sizeof(err)) ||
        !mp_play(err, sizeof(err))) {
        set_msg(err);
        return;
    }
    if (np_on() && np.w)
        np_soft_redraw();
}

/* Settings > Sound changed (Paula or AHI, MHI or mpega) while a track or
 * station plays: it goes on through the new way at once, a track from
 * where it was (paused, it stays so), a station started again. Modules
 * play on Paula either way. TRUE if it was restarted. */
static BOOL sound_restart(void)
{
    struct MpStatus st;
    BOOL paused;
    if (mp_loaded() && !mp_is_stream() && mp_running()) {
        /* a module the mixer plays: on through the new output */
        char err[100];
        err[0] = 0;
        if (mp_output_changed(err, sizeof(err))) {
            amb_log("sound", "Settings > Sound changed: the module on through it");
            return TRUE;
        }
        if (err[0])
            set_msg(err);
        return FALSE;
    }
    if (!mp_loaded() || !mp_is_stream() || !mp_running())
        return FALSE;
    paused = mp_paused();
    if (mp_is_track()) {
        ULONG ms;
        mp_status(&st);
        ms = np.seek_due ? (ULONG)np.seek_to : st.elapsed_ms;
        amb_log("sound", "Settings > Sound changed: the track again from where it was");
        if (np.list.pool && np.idx >= 0 && np.idx < np.list.count) {
            np_seek_track(ms, 0);
        } else {
            /* (not from a list: the same file or address again, from where
             * it was: a file's MP3 from about its byte, else by time) */
            char path[2048], title[120], by[200], tag[24], err[100];
            ULONG at = 0, size = st_size();
            amb_strlcpy(path, mp_path(), sizeof(path));
            amb_strlcpy(title, mp_stream_name(), sizeof(title));
            amb_strlcpy(by, mp_track_by(), sizeof(by));
            amb_strlcpy(tag, mp_tag(), sizeof(tag));
            if (np_local() && !amb_stricmp(tag, "MP3") && st.total_ms && size)
                at = (ULONG)(((unsigned long long)size * ms) / st.total_ms);
            if (!path[0] || !mp_load_track(path, title, by, tag, st.total_ms, plex_decoder_cpu(), ms, at, err,
                                           sizeof(err)) ||
                !mp_play(err, sizeof(err)))
                return FALSE;
        }
    } else {
        if (!np.list.pool || np.idx < 0 || np.idx >= np.list.count)
            return FALSE;
        amb_log("sound", "Settings > Sound changed: the station started again");
        if (!np_start(np.idx))
            return FALSE;
    }
    if (paused && mp_loaded())
        mp_pause(TRUE);
    return TRUE;
}

/* To volume v (the slider's): unmuted, if it was. */
void np_set_volume(int v)
{
    vol_unmuted = -1;
    mp_set_volume(v);
    if (np_on())
        np_update();
}

static void np_volume(int d)
{
    if (np_muted()) {
        int v = vol_unmuted;                /* a change of volume: unmuted, from there */
        vol_unmuted = -1;
        mp_set_volume(v);
    }
    mp_set_volume(mp_volume() + d);
    if (np_on())
        np_update();
}

/* The page drawn again (another song of the module). */
static void np_redraw_all(void)
{
    if (np_on()) {
        draw_pane(TRUE);
        draw_footer();
    }
}

/* ---- what was playing when Ambiance quit, and where: kept, loaded at
 * the next start (stopped), so Now playing's row is there and Play goes on
 * from where it was. Its playlist, its place, where it was started from. */
#define RESUME_FILE "ENVARC:Ambiance/Resume"
#define RESUME_MAX  (512L * 1024)

static void resume_store(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    struct MpStatus st;
    char line[160];
    BPTR fh;
    LONG i;
    me->pr_WindowPtr = (APTR)-1;
    if (!mp_loaded() || !np.list.pool || np.idx < 0 || np.idx >= np.list.count) {
        DeleteFile((CONST_STRPTR)RESUME_FILE);  /* nothing: none next time */
        me->pr_WindowPtr = oldwin;
        return;
    }
    mp_status(&st);
    ensure_dir("ENVARC:Ambiance");
    fh = Open((CONST_STRPTR)RESUME_FILE, MODE_NEWFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return;
    FPuts(fh, (CONST_STRPTR)"Ambiance resume 1\n");
    sprintf(line, "%ld\t%lu\t%lu\t%lu\t%ld\t%ld\t%ld\n", (long)np.idx,
            (unsigned long)(mp_is_stream() && !mp_is_track() ? 0 : st.elapsed_ms),
            (unsigned long)st.total_ms, (unsigned long)(mp_is_track() ? st_size() : 0),
            (long)np_src, (long)np_trail_n, (long)np.list.count);
    FPuts(fh, (CONST_STRPTR)line);
    FPuts(fh, (CONST_STRPTR)np.folder);
    FPuts(fh, (CONST_STRPTR)"\n");
    FPuts(fh, (CONST_STRPTR)np_src_list);
    FPuts(fh, (CONST_STRPTR)"\n");
    for (i = 0; i < np_trail_n; i++) {
        FPuts(fh, (CONST_STRPTR)np_trail[i]);
        FPuts(fh, (CONST_STRPTR)"\n");
    }
    for (i = 0; i < np.list.count; i++) {
        const struct Item *it = &np.list.items[i];
        sprintf(line, "%lu %lu %lu\n", (unsigned long)it->kind, (unsigned long)it->flags,
                (unsigned long)it->action);
        FPuts(fh, (CONST_STRPTR)line);
        FPuts(fh, (CONST_STRPTR)(it->label ? it->label : ""));
        FPuts(fh, (CONST_STRPTR)"\n");
        FPuts(fh, (CONST_STRPTR)(it->path ? it->path : ""));
        FPuts(fh, (CONST_STRPTR)"\n");
        FPuts(fh, (CONST_STRPTR)(it->sortkey ? it->sortkey : ""));
        FPuts(fh, (CONST_STRPTR)"\n");
    }
    Close(fh);
    sprintf(line, "%.80s, at %lu ms: kept for the next start", np.list.items[np.idx].label,
            (unsigned long)st.elapsed_ms);
    amb_log("resume", line);
}

static ULONG resume_num(char **s)
{
    ULONG v = 0;
    while (**s >= '0' && **s <= '9')
        v = v * 10 + (ULONG)(*(*s)++ - '0');
    while (**s == '\t' || **s == ' ')
        (*s)++;
    return v;
}

static void resume_load(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    char *buf, *s, m[160];
    LONG len, pos = 0, i, n, idx, trail;
    ULONG ms, total, size;
    int src;
    BPTR fh;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)RESUME_FILE, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return;
    Seek(fh, 0, OFFSET_END);
    len = Seek(fh, 0, OFFSET_BEGINNING);
    if (len <= 0 || len > RESUME_MAX || !(buf = AllocVec(len + 1, MEMF_ANY))) {
        Close(fh);
        return;
    }
    len = Read(fh, buf, len);
    Close(fh);
    if (len <= 0) {
        FreeVec(buf);
        return;
    }
    buf[len] = 0;
    if (!(s = nav_line(buf, len, &pos)) || strcmp(s, "Ambiance resume 1") != 0 ||
        !(s = nav_line(buf, len, &pos))) {
        FreeVec(buf);
        return;
    }
    idx = (LONG)resume_num(&s);
    ms = resume_num(&s);
    total = resume_num(&s);
    size = resume_num(&s);
    src = *s == '-' ? (s++, -(int)resume_num(&s)) : (int)resume_num(&s);
    trail = (LONG)resume_num(&s);
    n = (LONG)resume_num(&s);
    if (np.list.pool)
        list_free(&np.list);
    list_init(&np.list);
    np_album_thumb[0] = 0;
    if ((s = nav_line(buf, len, &pos)))
        amb_strlcpy(np.folder, s, sizeof(np.folder));
    if ((s = nav_line(buf, len, &pos)))
        amb_strlcpy(np_src_list, s, sizeof(np_src_list));
    np_trail_n = 0;
    for (i = 0; i < trail && (s = nav_line(buf, len, &pos)); i++)
        if (np_trail_n < MAXDEPTH)
            amb_strlcpy(np_trail[np_trail_n++], s, sizeof(np_trail[0]));
    for (i = 0; i < n; i++) {
        char *k, *label, *path, *key;
        ULONG kind, flags, action;
        struct Item *it;
        if (!(k = nav_line(buf, len, &pos)) || !(label = nav_line(buf, len, &pos)) ||
            !(path = nav_line(buf, len, &pos)) || !(key = nav_line(buf, len, &pos)))
            break;
        kind = resume_num(&k);
        flags = resume_num(&k);
        action = resume_num(&k);
        if ((it = list_add(&np.list, label, path, key, (UBYTE)kind))) {
            it->flags = (UBYTE)(flags & ~ITEM_SPLIT);
            it->action = (UBYTE)action;
        }
    }
    FreeVec(buf);
    if (idx < 0 || idx >= np.list.count) {
        list_free(&np.list);
        list_init(&np.list);
        np.idx = -1;
        return;
    }
    np_src = src;
    np_no_play = TRUE;
    if (np_start_now(idx)) {
        /* a track goes on where it was; a module from its start (jumped
         * into, modplug's songs could stay nearly silent until their
         * patterns set their volumes again) */
        np_resume_ms = mp_is_track() ? ms : 0;
        np_resume_total = total;
        np_resume_size = size;
        if (np_resume_ms)
            sprintf(m, "%.80s: loaded, stopped; Play goes on from %lu ms", np.list.items[idx].label,
                    (unsigned long)ms);
        else
            sprintf(m, "%.80s: loaded, stopped; Play starts it from the beginning",
                    np.list.items[idx].label);
    } else {
        sprintf(m, "%.80s: could not be loaded again", np.list.items[idx].label);
        np.idx = -1;
    }
    np_no_play = FALSE;
    msg[0] = 0;
    amb_log("resume", m);
}

/* Play on what the last run left loaded: from where it was. FALSE: played
 * as any is (a station: live, from now). */
static BOOL resume_play(void)
{
    ULONG ms = np_resume_ms;
    char err[100];
    np_resume_ms = 0;
    if (!ms || !mp_loaded())
        return FALSE;
    if (mp_is_track()) {
        np.seeked = TRUE;
        np_track_from(np.idx, ms, np_resume_total, np_resume_size, mp_stream_name(),
                      mp_track_by());
        return TRUE;
    }
    if (mp_is_stream())
        return FALSE;
    if (!mp_play(err, sizeof(err))) {
        set_msg(err);
        return TRUE;
    }
    if (!mp_seek_ms(ms)) {
        /* ProTracker's: the position it was in */
        struct MpStatus st;
        int pos, best = 0;
        mp_status(&st);
        for (pos = 0; pos < st.songlen; pos++) {
            ULONG at = mp_position_ms(pos);
            if (at != 0xFFFFFFFFUL && at <= ms && at >= mp_position_ms(best))
                best = pos;
        }
        if (best)
            mp_seek(best);
    }
    return TRUE;
}

void np_press(int b)
{
    char err[100];
    np.flash = b;                           /* lit for a moment, however pressed */
    np.flash_until = time_ms() + FLASH_MS;
    switch (b) {
    case NB_PREV:
        if (mp_is_stream()) {               /* live: the station before */
            np_skip(-1);
            return;
        }
        /* a few seconds in: back to its start, as players do; else the one before */
        if (mp_loaded()) {
            struct MpStatus st;
            mp_status(&st);
            if (st.elapsed_ms > 4000) {
                mp_rewind();
                break;
            }
            if (mp_subsong_step(-1)) {      /* a module of several songs: the one before */
                np_redraw_all();
                return;
            }
            if (np.idx <= 0) {
                mp_rewind();
                break;
            }
        }
        np_skip(-1);
        return;
    case NB_NEXT:
        if (mp_subsong_step(1)) {           /* its next song first */
            np.ended_seen = FALSE;
            np_redraw_all();
            return;
        }
        if (!np_skip(1))
            set_msg(mp_is_track() ? _("That was the last track")
                    : mp_is_stream() ? _("That was the last station in the list")
                                     : _("That was the last module in the folder"));
        return;
    case NB_BACK:
    case NB_FWD:
        if (!mp_can_step(b == NB_FWD ? 1 : -1)) {
            np.flash = -1;                  /* greyed out: no flash, nothing done */
            if (np.rep == b)
                np.rep = -1;
            break;
        }
        if (mp_is_track()) {
            /* 10 s a press (held: on and on), asked for once it rests */
            struct MpStatus st;
            LONG at;
            mp_status(&st);
            at = np.seek_due ? np.seek_to : (LONG)st.elapsed_ms;
            at += b == NB_FWD ? 10000 : -10000;
            np_seek_track(at < 0 ? 0 : (ULONG)at, 450);
            break;
        }
        if (mp_loaded() && !mp_step(b == NB_FWD ? 1 : -1))
            set_msg(_("This module cannot be moved through"));
        break;
    case NB_PLAY:
        if (!mp_loaded()) {
            if (np.idx >= 0 && np_start(np.idx) && np_on()) {
                draw_pane(TRUE);            /* stopped, started again: the page as it plays */
                draw_footer();
            }
        } else if (mp_playing()) {
            mp_pause(TRUE);
        } else if (mp_paused()) {
            mp_pause(FALSE);
        } else if (resume_play()) {
            ;                               /* (left by the last run: on from where it was) */
        } else if (!mp_play(err, sizeof(err))) {
            set_msg(err);
        }
        break;
    case NB_STOP:
        mp_stop();
        np.fail_due = 0;                    /* a failed track's next not started either */
        break;
    case NB_LOOP:
        if (mp_is_stream()) {
            np.flash = -1;                  /* greyed: nothing done */
            break;
        }
        play_loop = !play_loop;
        save_setting(LOOP_VAR, play_loop ? "1" : "0");
        set_msg(play_loop ? _("Loop on: it plays again at its end")
                          : _("Loop off: it stops at its end"));
        break;
    case NB_MUTE:
        np_mute(!np_muted());
        set_msg(np_muted() ? _("Muted") : _("Sound on"));
        break;
    case NB_VDOWN:
        np_volume(-VOL_STEP);
        return;
    case NB_VUP:
        np_volume(VOL_STEP);
        return;
    }
    if (np_on())
        np_update();
}

static void np_play_btn(void)
{
    np.btn = NB_PLAY;
}

static void np_open_page(void)
{
    struct Page *p;
    if (np_on())
        return;
    if (!(p = page_push(PT_PLAYER, _("Now playing"))))
        return;
    np.btn = NB_PLAY;
    if (focus != FOCUS_CONTENT) {
        focus = FOCUS_CONTENT;              /* drawn below, all of it */
        draw_menu_row(menu_sel);
    }
    draw_pane(TRUE);
    draw_footer();
}

/* Fire on a module in a Mods list: the folder's modules become the
 * playlist and this one plays (or, if it is the one playing, its page). */
static void mods_play_item(struct Page *p)
{
    LONG i;
    struct Item *sel = &p->list.items[p->sel];
    UBYTE kind = sel->kind;                 /* IT_MOD, or IT_STATION (a Radio list) */
    if (mp_loaded() && mp_running() && sel->mark == MARK_PLAYING) {
        np_open_page();
        return;
    }
    if (np.list.pool)
        list_free(&np.list);
    list_init(&np.list);
    np.idx = -1;
    amb_strlcpy(np.folder, p->title, sizeof(np.folder));
    np_album_thumb[0] = 0;
    if (kind == IT_PLEXTRACK && p->plex) {  /* an album's tracks: its row's cover */
        const struct Item *from = lv_opened_from(p);
        if (from && from->kind == IT_PLEXDIR)
            lv_plex_field(from, 2, np_album_thumb, sizeof(np_album_thumb));
    }
    {
        /* where this was started, for Now playing opened from elsewhere */
        int d;
        np_src = menu_sel >= 0 && menu_sel < nmenu ? menu[menu_sel] : -1;
        np_trail_n = 0;
        for (d = 0; d < depth - 1; d++) {
            struct Page *q = &pages[d];
            if (q->type != PT_LIST || q->sel < 0 || q->sel >= q->list.count)
                break;
            amb_strlcpy(np_trail[np_trail_n++], q->list.items[q->sel].label, sizeof(np_trail[0]));
        }
        amb_strlcpy(np_src_list, depth > 1 ? p->title : "", sizeof(np_src_list));
    }
    for (i = 0; i < p->list.count; i++) {
        struct Item *it = &p->list.items[i];
        if (it->kind != kind)
            continue;
        if (i == p->sel)
            np.idx = np.list.count;
        list_copy(&np.list, it, it->label);
    }
    if (np.idx < 0)
        return;
    if (np_start(np.idx)) {
        if (np_on()) {
            draw_pane(TRUE);                /* started from Now Playing's own list */
            draw_footer();
        } else {
            np_open_page();                 /* the one drawing: the list is not again */
        }
    } else {
        draw_pane(TRUE);                    /* its marks, and where a spinner was */
        draw_footer();
    }
}

/* Now playing's link to where the music is from: that section, and down
 * its folders as they were chosen, as far as they open at once (Mods' and
 * Music's own folders; a Plex or Radio list is asked of the network, so
 * the way stops at its section), the playing entry selected. */
static void open_dir_now(struct Item *it);
void draw_all(void);
void menu_select_now(int i, BOOL now);

void np_goto_source(void)
{
    int i, k;
    if (np_src < 0)
        return;
    for (i = 0; i < nmenu && menu[i] != np_src; i++)
        ;
    if (i >= nmenu)
        return;
    busy_begin();
    menu_select_now(i, TRUE);               /* its section's top, built now */
    for (k = 0; k < np_trail_n; k++) {
        struct Page *p = cur();
        LONG j;
        if (!p || p->type != PT_LIST)
            break;
        for (j = 0; j < p->list.count; j++)
            if (p->list.items[j].kind == IT_DIR && !strcmp(p->list.items[j].label, np_trail[k]))
                break;
        if (j >= p->list.count)
            break;                          /* not a folder that opens at once */
        p->sel = j;
        open_dir_now(&p->list.items[j]);
        if (cur() == p)
            break;                          /* it did not open */
    }
    {
        struct Page *p = cur();
        LONG j;
        for (j = 0; p && p->type == PT_LIST && j < p->list.count; j++)
            if (p->list.items[j].mark == MARK_PLAYING) {
                p->sel = j;
                p->top = j - list_visible() / 2 > 0 ? j - list_visible() / 2 : 0;
                break;
            }
    }
    busy_end(TRUE);
    msg[0] = 0;
    focus = FOCUS_CONTENT;
    draw_all();
}

/* A Mods list on screen with the playing module in it. */
BOOL np_list_on(void)
{
    struct Page *p = pane_pending ? NULL : cur();
    return p && p->type == PT_LIST && (p->mods || p->radio || p->plex || p->songs) && mp_loaded();
}


/* The bars' dance: a frame every 120 ms while the music plays. */
static void np_eq_tick(void)
{
    ui->eq_tick();
}

/* a layout: its own marks move (from the spectrum); the classic's dancing
 * marks are neither stepped nor drawn. Started, stopped or paused: the
 * layout's told (what plays changed) */
static void ly_eq_tick(void)
{
    BOOL still = !mp_playing();
    if (still != gfx_eq_still || mp_loaded() != hdr_np_on) {
        gfx_eq_still = still;
        hdr_np_on = mp_loaded();
        lv_note();
    }
}



void np_rep_start(int b, int src)
{
    if (!np_repeats(b))
        return;
    np.rep = b;
    np.rep_src = src;
    np.rep_next = time_ms() + REP_DELAY;
}

void np_rep_stop(void)
{
    if (np.rep < 0)
        return;
    np.rep = -1;
    if (np_on())
        np_update();
}

/* A button held down: pressed again, every REP_EVERY ms (the mouse's
 * only while the pointer is still on it). */
static void np_repeat(void)
{
    if (np.rep < 0)
        return;
    if (!np_on() || !mp_loaded()) {
        np.rep = -1;
        return;
    }
    if ((LONG)(time_ms() - np.rep_next) < 0)
        return;
    np.rep_next = time_ms() + REP_EVERY;
    if (np.rep_src == REP_MOUSE && g.win &&
        np_button_at(g.win->MouseX - g.win->BorderLeft, g.win->MouseY - g.win->BorderTop) != np.rep)
        return;
    np_press(np.rep);
}

/* Once a second: a FLAC track decoded here that keeps running dry (the
 * sound out of data again and again, a few seconds in) - this Amiga is too
 * slow for it. Said once a track on the status line, instead of a silent
 * stutter: a music file has no other way to play; for a Plex track the
 * server's conversion is. */
static void flac_slow_check(void)
{
    static ULONG told = (ULONG)-1;
    struct StInfo in;
    if (!mp_running() || !mp_is_track() || mp_serial() == told)
        return;
    st_info(&in);
    if (!in.flac || in.state != ST_PLAYING || in.underruns < 3 || in.played_ms < 3000)
        return;
    told = mp_serial();
    amb_log("track", "FLAC: dropouts - this Amiga cannot decode it in time");
    set_msg(np_local() ? _("This Amiga is too slow to decode FLAC smoothly")
                       : _("Too slow for FLAC: try Request lower quality audio"));
}

static void np_tick(void)
{
    if (np.seek_due && (LONG)(time_ms() - np.seek_due) >= 0)
        np_seek_track((ULONG)np.seek_to, 0);    /* Back/Forward rested: from there */
    if (np.fail_due && (LONG)(time_ms() - np.fail_due) >= 0) {
        /* a track that failed, its message read: the next in its list,
         * unless something else plays by now (a station, a video, ...) */
        np.fail_due = 0;
        if (mp_running())
            ;
        else if (!np_skip(1)) {
            np_mark_lists(TRUE);
            if (np_on())
                np_redraw_all();
        }
        np.s_rstate = -2;
        return;
    }
    np_repeat();
    if (!np_full)
        np_eq_tick();                       /* (Flow on the whole screen: no marker under it) */
    if (mp_is_track() && mp_running() && np_on() && np.w) {
        /* the average bitrate settling: its line again when it moves (a
         * layout shows no such line) */
        static ULONG shown;
        ULONG k = st_avg_kbps();
        if (k != shown && (k > shown + 1 || k + 1 < shown || !shown || !k)) {
            shown = k;
            ui->np_rate();
        }
    }
    if (mp_is_track() && mp_running() && mp_ended()) {
        /* a track played to its end (or failed): the next in its list */
        BOOL failed = st_state() == ST_FAILED;
        char why[100];
        amb_strlcpy(why, st_error(), sizeof(why));
        if (failed) {
            /* said which, and why: the track's title before the reason */
            char m[200];
            const char *t = np.list.pool && np.idx >= 0 && np.idx < np.list.count
                                ? np.list.items[np.idx].label : "";
            amb_log("plex", why[0] ? why : "the track failed");
            if (!why[0])
                amb_strlcpy(why, _("That track could not be played"), sizeof(why));
            if (t[0])
                sprintf(m, "%.80s: %.100s", t, why);
            else
                amb_strlcpy(m, why, sizeof(m));
            amb_strlcpy(why, m, sizeof(why));
            set_msg(m);
        }
        if (failed && np.seeked) {
            /* a seek the server would not do: the track stays, stopped,
             * rather than the next one starting */
            np.seeked = FALSE;
            mp_stop();
            np_mark_lists(TRUE);
            set_msg(why[0] ? why : _("The track could not be started there"));
            if (np_on())
                np_redraw_all();
            np.s_rstate = -2;
            return;
        }
        if (failed) {
            /* a few seconds to read why before the next one starts (Stop,
             * or anything chosen meanwhile, and it does not) */
            mp_stop();
            np_mark_lists(TRUE);
            if (np_on())
                np_redraw_all();
            np.fail_due = time_ms() + 3000;
            if (!np.fail_due)
                np.fail_due = 1;
            np.s_rstate = -2;
            return;
        }
        if (play_loop && !failed && np.idx >= 0 && np_start(np.idx)) {
            if (np_on())
                np_redraw_all();            /* looping: the same again */
            np.s_rstate = -2;
            return;
        }
        if (!np_skip(1)) {
            mp_stop();
            np_mark_lists(TRUE);
            if (np_on())
                np_redraw_all();
        }
        np.s_rstate = -2;
        return;
    }
    if (mp_is_stream()) {
        int state = mp_running() ? st_state() : -1;
        if (mp_running() && mp_ended()) {
            /* a station gone quiet: said why, not the next one */
            char why[100];
            amb_strlcpy(why, st_error()[0] ? st_error() : _("The station stopped sending"),
                         sizeof(why));
            mp_stop();
            np_mark_lists(TRUE);
            set_msg(why);
            amb_log("radio", why);
            state = -1;
        }
        if (st_title_seq() != np.s_rseq || state != np.s_rstate) {
            np.s_rseq = st_title_seq();
            np.s_rstate = state;
            if (np_on() && (!np.w || !mp_loaded())) {
                np_redraw_all();            /* stopped: "Nothing playing" */
            } else if (np_on()) {
                /* what plays, or its state, changed: its lines over themselves,
                 * and the controls (a full redraw flashed the page each time) */
                ui->np_rate();
                np_update();
                if (!msg[0] && hint_shown && hint_text() != hint_shown)
                    draw_footer();
            }
            return;
        }
    }
    if (mp_running() && mp_ended() && !np.ended_seen) {
        np.ended_seen = TRUE;
        if (play_loop) {                    /* looping: this song from its start */
            mp_rewind();
            np.ended_seen = FALSE;
            if (np_on())
                np_update();
            return;
        }
        if (mp_subsong_step(1)) {           /* a module of several songs: the next */
            np.ended_seen = FALSE;
            np_redraw_all();
            return;
        }
        if (!np_skip(1)) {
            mp_stop();                      /* the folder played through */
            np_mark_lists(TRUE);
        }
        if (np_on())
            np_update();
        return;
    }
    if (np_on())
        np_update();
}

/* Left/Right on the page: the buttons. FALSE: Left past the first (back). */
static BOOL np_lr(int d)
{
    if (!np_on() || focus != FOCUS_CONTENT)
        return FALSE;
    if (d < 0 && np.btn == 0)
        return FALSE;
    if (d > 0 && np.btn == NB_COUNT - 1)
        return TRUE;
    np.btn += d;
    np_update();
    return TRUE;
}



/* To part n of d of the song (a bar clicked: Now playing's, the mini
 * player's). */
void np_seek_part(ULONG n, ULONG d)
{
    struct MpStatus st;
    ULONG want;
    int pos, best = 0;
    mp_status(&st);
    if (!st.total_ms || !d)
        return;                             /* its length is not known */
    want = st.total_ms / 100 * n / d * 100;
    if (mp_is_track()) {
        np_seek_track(want, 0);             /* a track: from there, at once */
        return;
    }
    if (mp_seek_ms(want)) {                 /* mixed modules: to the time itself */
        if (np_on())
            np_update();
        return;
    }
    for (pos = 0; pos < st.songlen; pos++) {
        ULONG at = mp_position_ms(pos);
        if (at != 0xFFFFFFFFUL && at <= want && at >= mp_position_ms(best))
            best = pos;
    }
    mp_seek(best);
    if (np_on())
        np_update();
}


/* Now playing rows at the top of lists: none now (Now playing is the
 * menu's first entry while something plays); 1 brings them back. */
#define NP_ROWS 0

/* "Now playing" at the top of the Mods and Radio pages while something
 * plays (a module or a station), its tag the title or the station. */
static void np_shortcut(struct Page *p)
{
    const char *t;
    char label[120];
    if (!NP_ROWS || !mp_loaded())
        return;
    if (mp_is_stream()) {
        t = mp_stream_name();
    } else {
        t = mp_title();
        while (*t == ' ')
            t++;
        if (!t[0]) {
            mods_label(FilePart((STRPTR)mp_path()), label, sizeof(label));
            t = label;
        }
    }
    action_item(p, _("Now playing"), t, ACT_NOWPLAYING, MARK_PLAYING);
}

/* How long a Plex server may take to start answering a list: a NAS's
 * (a Synology's, say) thinks for longer than 20 seconds over a large
 * library's artists, where a PC's answers at once. Esc gives up sooner. */
#define PLEX_WAIT_S 60

/* A library's long list (Artists, Albums, Recently added), or one of
 * Radio's (stations, genres, countries): a Now playing row first while
 * something plays, as Music's and Radio's own pages have. */
static BOOL plex_lib_list(const struct Page *p)
{
    if (!p || p->type != PT_LIST || !p->radio)
        return FALSE;
    if (!p->plex)
        return p > &pages[0];               /* Radio's lists under its own page */
    return !p->photos && !strncmp(p->base, "/library/sections/", 18);
}

/* A list with a Now playing row at its top while something plays: a Plex
 * library's or Radio's long lists, and every folder of Mods and Music
 * under their first pages (whose own row is made with them). */
static BOOL np_row_list(const struct Page *p)
{
    if (plex_lib_list(p))
        return TRUE;
    if (p && p->type == PT_LIST && p->ags && !p->ch)
        return TRUE;                        /* Games', Demos' menus (a long list held a letter at
                                             * a time keeps its places: none) */
    if (p && p->type == PT_LIST && p->pics && !p->plex && !(vw_active && p == cur()))
        return TRUE;                        /* Pictures', Videos' folders (not while the viewer
                                             * steps through it by its places) */
    return p && p->type == PT_LIST && p > &pages[0] && (p->mods || p->songs);
}

/* Its Now playing row made as playback is now (added, taken out, or its
 * title changed); the selection stays on the entry it was on. TRUE: the
 * list changed. */
static BOOL plex_shortcut_fix(struct Page *p)
{
    struct Item *it;
    const char *t;
    char label[120];
    BOOL has, playing = NP_ROWS && mp_loaded();
    if (!np_row_list(p) || !p->list.pool)
        return FALSE;
    it = p->list.count ? &p->list.items[0] : NULL;
    has = it && it->kind == IT_ACTION && it->action == ACT_NOWPLAYING;
    t = !playing ? "" : mp_is_stream() ? mp_stream_name() : mp_title();
    while (*t == ' ')
        t++;
    if (playing && !t[0] && !mp_is_stream()) {
        mods_label(FilePart((STRPTR)mp_path()), label, sizeof(label));     /* (an untitled module) */
        t = label;
    }
    if (has && playing && !strcmp(it->path, t))
        return FALSE;                       /* as it is */
    if (!has && !playing)
        return FALSE;
    {
        LONG row = p->sel - p->top, e = p->sel - (has ? 1 : 0), ns;
        BOOL added = FALSE;
        if (has)
            list_drop_first(&p->list);
        if (playing && (it = list_add_first(&p->list, _("Now playing"), t, "", IT_ACTION))) {
            it->action = ACT_NOWPLAYING;
            it->mark = MARK_PLAYING;
            added = TRUE;
        }
        ns = e < 0 ? 0 : e + (added ? 1 : 0);   /* the entry it was on, where it is now */
        if (ns >= p->list.count)
            ns = p->list.count ? p->list.count - 1 : 0;
        p->sel = ns;
        p->top = ns - row;
        if (p->top < 0)
            p->top = 0;
        if (p->top > ns)
            p->top = ns;
    }
    return TRUE;
}

/* Where each of the last few Plex lists was left (its key, the entry and
 * its row on screen): opened again, it comes back there. */
#define PLEX_POS_KEPT 8
static struct { char key[120]; char label[96]; LONG sel, row; } plex_pos[PLEX_POS_KEPT];
static int plex_pos_next;

static void plex_pos_save(const struct Page *p)
{
    if (p->type != PT_LIST)
        return;
    int i, k = -1;
    if (!p->plex || !p->radio || !p->base[0] || p->base[0] == '#' || p->sel < 0 ||
        p->sel >= p->list.count)
        return;
    for (i = 0; i < PLEX_POS_KEPT; i++)
        if (!strcmp(plex_pos[i].key, p->base))
            k = i;
    if (k < 0) {
        k = plex_pos_next;
        plex_pos_next = (plex_pos_next + 1) % PLEX_POS_KEPT;
    }
    amb_strlcpy(plex_pos[k].key, p->base, sizeof(plex_pos[k].key));
    amb_strlcpy(plex_pos[k].label, p->list.items[p->sel].label, sizeof(plex_pos[k].label));
    plex_pos[k].sel = p->sel;
    plex_pos[k].row = p->sel - p->top;
}

static void plex_pos_restore(struct Page *p)
{
    int i;
    LONG s = -1, k;
    if (p->sel > 0)
        return;                             /* moved already, while it came */
    for (i = 0; i < PLEX_POS_KEPT; i++)
        if (plex_pos[i].key[0] && !strcmp(plex_pos[i].key, p->base))
            break;
    if (i == PLEX_POS_KEPT)
        return;
    /* by name, from where it was (the list may have changed, or gained a
     * Now playing row) */
    for (k = 0; k < 3 && s < 0; k++) {
        LONG at = plex_pos[i].sel + (k == 0 ? 0 : k == 1 ? 1 : -1);
        if (at >= 0 && at < p->list.count && !strcmp(p->list.items[at].label, plex_pos[i].label))
            s = at;
    }
    for (k = 0; k < p->list.count && s < 0; k++)
        if (!strcmp(p->list.items[k].label, plex_pos[i].label))
            s = k;
    if (s < 0)
        return;
    p->sel = s;
    p->top = s - plex_pos[i].row;
    if (p->top < 0)
        p->top = 0;
    if (p->top > s)
        p->top = s;
}

/* Back on the Mods or Radio page: its Now playing row as playback is now
 * (it may have started, stopped or changed since the page was made). */
static void np_shortcut_sync(void)
{
    struct Page *p = cur();
    if (np_row_list(p)) {
        plex_shortcut_fix(p);
        return;
    }
    struct Item *it;
    char want[120], sel[96];
    BOOL has, playing = NP_ROWS && mp_loaded();
    LONG i;
    if (depth != 1 || !p || p->type != PT_LIST || !(p->mods || p->radio || p->plex || p->songs))
        return;
    it = p->list.count ? &p->list.items[0] : NULL;
    has = it && it->kind == IT_ACTION && it->action == ACT_NOWPLAYING;
    want[0] = 0;
    if (playing)
        amb_strlcpy(want, mp_is_stream() ? mp_stream_name() : mp_title(), sizeof(want));
    if (has == playing && (!has || !strcmp(it->path, want) || !mp_is_stream()))
        return;                             /* as it is: nothing to do */
    sel[0] = 0;
    if (p->sel >= 0 && p->sel < p->list.count)
        amb_strlcpy(sel, p->list.items[p->sel].label, sizeof(sel));
    if (p->radio || p->plex) {
        list_free(&p->list);
        list_init(&p->list);
        if (p->plex)
            build_music_root(p);
        else
            build_radio_root(p);
    } else {
        build_root_pages(MOD_MODS);         /* the root again, as when first opened */
        p = cur();
        if (!p)
            return;
    }
    p->sel = 0;
    for (i = 0; i < p->list.count; i++)
        if (!strcmp(p->list.items[i].label, sel))
            p->sel = i;
    p->top = 0;
}

/* Mods' first page: AGS's modules, C64 music and menu music where they
 * are, then MODS: and the folders of Settings > Folders > Modules. */
static void build_mods_root(void)
{
    struct Page *p;
    BOOL mods = amb_exists(MODS_ROOT), menu_music = amb_exists(MODS_MENU), sid = amb_exists(MODS_SID);
    BOOL asg = fs_assign_used(FS_MODS);
    int i;
    if (!mods && !menu_music && !sid && !asg && !fs[FS_MODS].n && !mp_loaded()) {
        if ((p = page_push(PT_TEXT, _("Mods")))) {
            char t[400];
            sprintf(t, _("No modules found. Ambiance looks for them in MODS:, in the folders of "
                         "Settings > Folders > Modules, and on AGS in %s and %s (its menu music)."),
                    MODS_ROOT, MODS_MENU);
            text_para(p, t);
            text_line(p, "");
            text_para(p, _("It plays ProTracker modules itself, and hundreds of other formats "
                         "(OctaMED, TFMX, Future Composer, SID...) with EaglePlayer's and "
                         "DeliTracker's players, all on the Amiga's own sound."));
            if (!ep_installed()) {
                text_line(p, "");
                text_para(p, _("EaglePlayer was not found: for those formats, install it from "
                             "Aminet (mus/play/Eagleplayer) with its own installer."));
            }
        }
        return;
    }
    if (!(p = page_push(PT_LIST, _("Mods"))))
        return;
    p->mods = 1;
    np_shortcut(p);
    if (mods)
        list_add(&p->list, _("Modules"), MODS_ROOT, "", IT_DIR);
    if (sid)
        list_add(&p->list, _("C64 music"), MODS_SID, "", IT_DIR);
    if (menu_music)
        list_add(&p->list, _("Menu music"), MODS_MENU, "", IT_DIR);
    if (asg)
        list_add(&p->list, _("Modules (MODS:)"), "MODS:", "", IT_DIR);
    for (i = 0; i < fs[FS_MODS].n; i++) {
        char label[180];
        const char *f = (const char *)FilePart((STRPTR)fs[FS_MODS].dir[i]);
        sprintf(label, _("Modules: %.100s"), f[0] ? f : fs[FS_MODS].dir[i]);
        list_add(&p->list, label, fs[FS_MODS].dir[i], "", IT_DIR);
    }
    if (!ep_installed())
        list_add(&p->list, _("More formats: EaglePlayer, from Aminet (mus/play/Eagleplayer)"),
                 "", "", IT_TEXT);
}

void pane_settle(void);


/* ------------------------------------------------------------ actions */
/* Moving through the main menu only moves the highlight and shows the
 * section's name: the section itself (reading AGS folders from disk,
 * drawing the whole pane) is built once the selection settles, PANE_SETTLE
 * ms after the last move with no Up/Down held, or as soon as anything else
 * needs it (pane_settle). While pane_pending, pages[] still hold the
 * section shown before. */
#define PANE_SETTLE 180
static BOOL key_is_down(UWORD rawkey);
static ULONG pane_due;

void pane_settle(void)
{
    if (!pane_pending)
        return;
    pane_pending = FALSE;
    busy_begin();                   /* a big section (Pictures) can take a while */
    build_root(menu[menu_sel]);
    draw_pane(TRUE);
    if (ui_aborted) {
        /* given up for input: built again once that input is dealt with
         * and the menu rests (the read goes on meanwhile) */
        pane_pending = TRUE;
        pane_due = time_ms() + PANE_SETTLE;
    }
    busy_end(FALSE);                /* drawn just now */
}

/* Videos' first page, shown: a DVD put in or taken out shows there within
 * a second (the DOS list's CD/DVD volumes looked at, no disc read unless
 * they changed), the page built again as when the section is chosen. */
static void dvd_watch(void)
{
    ULONG s;
    if (shown_mod != MOD_VIDEOS || depth != 1 || pane_pending || menu[menu_sel] != MOD_VIDEOS)
        return;
    s = dvd_volumes_sig();
    if (s == dvd_sig)
        return;
    dvd_sig = s;                            /* (once a change, built or not) */
    amb_log("dvd", "a CD/DVD volume came or went: Videos built again");
    pane_pending = TRUE;
    pane_settle();
}

/* Up/Down (or keypad 8/2) still held? */
static BOOL menu_key_held(void)
{
    return key_is_down(0x4C) || key_is_down(0x4D) || key_is_down(0x3E) || key_is_down(0x1E);
}

static void pane_tick(void)
{
    ULONG now;
    if (!pane_pending)
        return;
    now = time_ms();
    if (LowLevelBase && menu_key_held())
        pane_due = now + PANE_SETTLE;
    else if ((LONG)(now - pane_due) >= 0)
        pane_settle();
}


/* now: build the section straight away (a click), not once it settles */
void menu_select_now(int i, BOOL now)
{
    int old = menu_sel;
    nav_moved();
    if (i < 0 || i >= nmenu || i == old)
        return;
    menu_sel = i;
    draw_menu_row(old);
    draw_menu_row(i);
    if (now || !time_ms()) {
        pane_pending = TRUE;
        pane_settle();
        return;
    }
    ov.on = FALSE;
    pane_preview(!pane_pending);
    pane_pending = TRUE;
    pane_due = time_ms() + PANE_SETTLE;
}

static void menu_select(int i)
{
    menu_select_now(i, FALSE);
}


/* The menu column is its icons alone while the content has the focus, on
 * every screen: lists, Now playing and the rest get its room. The labels
 * come back with the focus, or the pointer, on the menu; the breadcrumb
 * and the highlighted icon say where you are meanwhile. On the first page
 * it keeps its labels. Opened by the pointer, it stays open (the pointer
 * moving off, or hidden at rest, is no reason to close it) until the
 * content is gone into: Return, Fire or Right (the focus to the content),
 * or a click in it (menu_unpin). TRUE if the layout changed (everything
 * was drawn again). */
static BOOL menu_pinned;            /* opened by the pointer: open till the content is used */

void menu_unpin(void)
{
    menu_pinned = FALSE;
}

BOOL menu_layout(void)
{
    BOOL narrow;
    if (hover_menu >= 0 || hover_strip)
        menu_pinned = TRUE;
    narrow = focus != FOCUS_MENU && !menu_pinned && depth > 0;
    if (!g.win || narrow == gfx_menu_is_narrow())
        return FALSE;
    ui->menu_slide(g.menu_w, gfx_menu_width(&g, narrow));
    if (!gfx_menu_narrow(&g, narrow))
        return FALSE;
    draw_all();
    return TRUE;
}

void set_focus(int f)
{
    pane_settle();
    if (f == FOCUS_CONTENT && !section_inert())
        menu_unpin();                       /* into the content: the menu to its icons */
    if (focus == f || (f == FOCUS_CONTENT && section_inert()))
        return;
    focus = f;
    pv_tb.focused = f == FOCUS_PREVIEW;
    if (menu_layout())
        return;
    if (pv_tb.open) {
        pv_tb.thumb_y = -1;         /* repaint the thumb: brighter while focused */
        tb_refresh_bar(&g, &pv_tb);
    }
    draw_menu_row(menu_sel);
    draw_pane(FALSE);
    draw_footer();
}

static void open_dir_now(struct Item *it);

/* Open a folder: the spinner if it keeps you waiting. */
void open_dir(struct Item *it)
{
    busy_begin();
    open_dir_now(it);
    busy_end(TRUE);
}

static void open_dir_now(struct Item *it)
{
    struct Page *p;
    LONG n;
    BOOL pics = cur() && cur()->pics, mods = cur() && cur()->mods, songs = cur() && cur()->songs;
    UBYTE pk = cur() ? cur()->pics : 0;     /* 2: Videos' */
    if (pics && (it->flags & ITEM_DISABLED)) {
        set_msg(_("No pictures in that folder"));
        return;
    }
    set_msg(_("Loading..."));
    p = page_push(PT_LIST, it->label);
    if (!p) {
        set_msg(_("Too many levels open"));
        return;
    }
    if (pics) {
        p->pics = pk;
        n = pics_read_ui(it->path, &p->list);
        if (n <= 0) {
            page_pop();
            if (n == -2)
                msg[0] = 0;                 /* given up for input */
            else
                set_msg(n < 0 ? _("Could not open that folder") : _("That folder has no pictures"));
            return;
        }
        plex_shortcut_fix(p);               /* Now playing first, while something plays */
        msg[0] = 0;
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    if (songs) {
        p->songs = 1;
        if (beets_page(it->path)) {
            /* a beets library's page (beets.c) */
            n = beets_read(it->path, &p->list);
            if (n <= 0) {
                char m[200];
                page_pop();
                sprintf(m, _("The music library could not be read: %.60s"), beets_error());
                set_msg(m);
                return;
            }
        } else {
            n = songs_read_dir(it->path, &p->list);
        }
        if (n <= 0) {
            page_pop();
            set_msg(n < 0 ? _("Could not open that folder") : _("No MP3 or FLAC files in that folder"));
            return;
        }
        np_mark_lists(FALSE);
        msg[0] = 0;
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    if (mods) {
        p->mods = 1;
        n = mods_read_dir(it->path, &p->list);
        if (n <= 0) {
            page_pop();
            set_msg(n < 0 ? _("Could not open that folder") : _("That folder has no modules"));
            return;
        }
        np_mark_lists(FALSE);
        msg[0] = 0;
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    p->ags = 1;
    if (ig_page(it->path)) {
        /* iGame's list: read from memory (igame.c) */
        BOOL ordered;
        n = ig_read(&igcat, it->path, &p->list, &ordered);
        p->igame = ordered ? 2 : 1;
        if (n <= 0) {
            page_pop();
            set_msg(n < 0 ? _("iGame's list is not read now") : _("That list is empty"));
            return;
        }
        plex_shortcut_fix(p);
        msg[0] = 0;
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    n = page_read_ags(p, it, 2 * list_visible());   /* the rest follows (ch_pump) */
    if (n <= 0) {
        page_pop();
        if (n == -2)
            msg[0] = 0;                     /* given up for input */
        else
            set_msg(n < 0 ? _("Could not open that menu") : _("That menu is empty"));
        return;
    }
    plex_shortcut_fix(p);                   /* (a short menu: Now playing first; not a long list) */
    msg[0] = 0;
    draw_pane(TRUE);
    draw_footer();
}

static void open_detail(struct Item *it)
{
    struct Page *p = page_push(PT_DETAIL, it->label);
    LONG n, i;
    if (!p)
        return;
    item_path(it, p->base, sizeof(p->base));
    if (p > &pages[0] && p[-1].igame) {
        p->igame = 1;                       /* iGame's entry: its fields */
        n = ig_facts(ig_lookup(&igcat, p->base), fact_buf, FACT_LINES);
    } else
        n = ags_read_txt(p->base, fact_buf, FACT_LINES);
    if (!n)
        text_line(p, it->label);
    for (i = 0; i < n; i++)
        text_line(p, fact_buf[i]);
    draw_pane(TRUE);
    draw_footer();
}

/* ------------------------------------------------------------ launching */
/* Libraries Ambiance can do without while a game runs (closed then, so the
 * system can unload them, picture datatypes and all). */
static void open_extra_libs(void)
{
    if (!DiskfontBase)
        DiskfontBase = OpenLibrary((CONST_STRPTR)"diskfont.library", 36);
    if (!DataTypesBase)
        DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
    if (!LowLevelBase)
        LowLevelBase = OpenLibrary((CONST_STRPTR)"lowlevel.library", 40);
    zz_view_hold(TRUE);             /* (zz9k.library, where the viewer's JPEGs go to the card) */
}

static void close_extra_libs(void)
{
    zz_view_hold(FALSE);            /* (zz9k.library: the viewer's, after the loader stopped) */
    if (LowLevelBase)
        CloseLibrary(LowLevelBase);
    if (DataTypesBase)
        CloseLibrary(DataTypesBase);
    if (DiskfontBase)
        CloseLibrary(DiskfontBase);
    if (AslBase)
        CloseLibrary(AslBase);          /* opened again when wanted */
    LowLevelBase = DataTypesBase = DiskfontBase = AslBase = NULL;
}

/* Ask for more memory than there is: the system then unloads everything
 * not in use (libraries, devices, fonts, datatypes) to try to find it. */
static void flush_memory(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        APTR p = AllocMem(0x7FFFFFF0UL, MEMF_PUBLIC);
        if (p)
            FreeMem(p, 0x7FFFFFF0UL);
    }
}

static void log_free(const char *what)
{
    char buf[64];
    sprintf(buf, "chip %luK, fast %luK", (unsigned long)(AvailMem(MEMF_CHIP) >> 10),
            (unsigned long)(AvailMem(MEMF_FAST) >> 10));
    amb_log(what, buf);
}

/* The screen (or window) closed, to be opened again or not: a layout's off
 * first (its canvas and its library's drawing belong to this screen, and
 * nothing of it may draw onto the next one); lv_frame puts it on again
 * where the new screen suits it (lv_sync) */
static void screen_close(void)
{
    lv_hold();
    gfx_close(&g);
}

/* Before a game: everything Ambiance can rebuild goes. Where each section
 * was is kept (nav_mem), so the same entry comes back afterwards. */
static void release_for_launch(void)
{
    lv_release();                   /* (a layout: its memory too) */
    log_free("free before launch");
    if (mp_loaded())
        amb_log("music", "stopped for the game");
    mp_shutdown();                  /* the game gets Paula and the CIA timers */
    np_mark_lists(FALSE);
    save_nav();
    pv_flush();                     /* the loader's jobs back, then the loader gone */
    pics_forget();                  /* every byte for the game */
    ldr_stop();
    pages_clear();                  /* lists, text boxes, the screenshot */
    tb_close(&pv_tb);
    if (pv_list.pool)
        list_free(&pv_list);
    det.shown = det.pending = FALSE;
    ov.on = FALSE;
    ov.bl = NULL;
    ptr_hidden = NULL;              /* a new window comes with its pointer */
    spin_free();
    ov_bm_free();
    vis_stop();                     /* the spectrum's tables */
    screen_close();                 /* screen, fonts, artwork, smooth style, letters */
    close_extra_libs();
    flush_memory();
    log_free("free for the game");
}

/* After the game: reopen, and rebuild the section as it was. */
static BOOL restore_after_launch(void)
{
    open_extra_libs();
    ldr_start();
    if (!gfx_open(&g, &caps, screen_mode, ui_style))
        return FALSE;
    if (focus == FOCUS_PREVIEW)
        focus = FOCUS_CONTENT;
    pane_pending = FALSE;
    /* the frame first, so there is something to see while folders load */
    gfx_fill(&g, 0, 0, g.w - 1, g.h - 1, PEN_BG);
    draw_header();
    draw_menu();
    set_msg(_("Loading..."));
    busy_begin();
    build_root(menu[menu_sel]);
    busy_end(FALSE);                /* all drawn next */
    msg[0] = 0;
    draw_all();
    note_style();
    log_free("free after the game");
    return TRUE;
}

static void launch(const char *base)
{
    static const char *script = "T:Ambiance-run";
    BPTR con;
    if (cur() && cur()->igame) {
        /* iGame's entry: WHDLoad (its icon's tooltypes), or the program */
        const struct IgGame *gm = ig_lookup(&igcat, base);
        char why[100];
        if (!gm) {
            set_msg(_("iGame's list no longer has that entry"));
            return;
        }
        if (!ig_write_launch_script(gm, script, why, sizeof(why))) {
            set_msg(why);
            return;
        }
    } else {
        if (!ags.found)
            return;
        if (!ags_write_launch_script(&ags, base, script)) {
            set_msg(_("Could not write T:Ambiance-run"));
            return;
        }
    }
    /* Give the game every byte we can. */
    amb_log("launch", base);
    nav_store(FALSE);               /* should it not come back */
    radio_stop();                   /* no network process under a game */
    if (debug_on)
        dbg_cpu_stop();
    release_for_launch();
    con = Open((CONST_STRPTR)"CON:0/0/640/200/Ambiance/AUTO/CLOSE", MODE_OLDFILE);
    if (con) {
        SystemTags((CONST_STRPTR)"Execute T:Ambiance-run",
                   SYS_Input, (ULONG)con, SYS_Output, 0, SYS_UserShell, TRUE,
                   NP_StackSize, 16384, TAG_END);
        Close(con);
    }
    DeleteFile((CONST_STRPTR)script);
    if (debug_on)
        dbg_cpu_start();
    cpu_load = -1;
    if (!restore_after_launch())
        running = FALSE;
}

/* ------------------------------------------------------------ picture viewer
 * Full screen on Ambiance's own screen (view.c). Left/Right (or Up/Down,
 * the joystick, a click) step through the pictures of the list, Fire,
 * Return or Space start and pause the slideshow, Esc, Backspace, the right
 * mouse button or the second fire button go back. The info bar shows for
 * a few seconds after each change; in a slideshow only when asked. Keys
 * pressed while a picture loads are taken together, so holding Right
 * skips ahead instead of loading every picture on the way. */
static void hold_stop(void);

static LONG view_step(const struct ItemList *l, LONG i, LONG d)
{
    LONG n = l->count, k;
    for (k = 0; k < n; k++) {
        i = (i + d + n) % n;
        if (l->items[i].kind == IT_PIC)
            return i;
    }
    return -1;
}

static BOOL vw_mouse;               /* the mouse in use: the bar has its buttons */
static BOOL vw_full;                /* from a window, pictures full screen (for this run) */

static void viewer_bar(const struct ItemList *l, LONG i, const struct PicInfo *pi,
                       const char *scale, BOOL slideshow)
{
    char detail[200], desc[64];
    LONG k, pos = 0, n = 0;
    for (k = 0; k < l->count; k++)
        if (l->items[k].kind == IT_PIC) {
            n++;
            if (k == i)
                pos = n;
        }
    pic_describe(pi, desc, sizeof(desc));
    sprintf(detail, _("%ld/%ld   %s   %s%s"), (long)pos, (long)n, desc, scale,
            slideshow ? _("   Slideshow") : "");
    view_bar_buttons(vw_mouse, slideshow, gfx_fs_on() ? 2 : g.wb ? 1 : 0);
    view_bar(&g, l->items[i].label, detail,
             slideshow ? _("Left/Right  previous/next    Fire  pause    Esc  back")
                       : _("Left/Right  previous/next    Fire  slideshow    Esc  back"));
}

/* The next picture, decoded ahead by the loader while this one is looked
 * at, so stepping on (or the slideshow) does not wait for the disk. On a
 * colour-table screen it is decoded unremapped (it takes no pens: the
 * picture on screen has them all); one that would need remapping there is
 * left to view_show. */
static struct LJob *vw_job;

static void vw_drop(void)
{
    if (!vw_job)
        return;
    if (vw_job->busy && !vw_job->scr) {
        ldr_orphan(vw_job);                 /* holds no pens: freed when it ends */
        vw_job = NULL;
        return;
    }
    if (vw_job->busy)
        ldr_wait(vw_job);
    pic_dispose(vw_job->obj);
    thumb_free(vw_job->thumb);
    FreeVec(vw_job);
    vw_job = NULL;
}

/* The loader asked to decode path for the viewer (first: wanted now). */
static void vw_start(const char *path, BOOL first)
{
    if (!(vw_job = AllocVec(sizeof(*vw_job), MEMF_ANY | MEMF_CLEAR)))
        return;
    vw_job->type = LJ_PICTURE;
    amb_strlcpy(vw_job->path, path, sizeof(vw_job->path));
    vw_job->scr = g.truecolor ? g.scr : NULL;
    vw_job->remap = g.truecolor;
    vw_job->want_pic = TRUE;
    vw_job->vw = TRUE;                      /* (its own readers first: view_own_read) */
    vw_job->vw_true = g.truecolor;
    vw_job->vw_w = g.w;
    vw_job->vw_h = g.h;
    vw_job->tall = gfx_mode_tall(g.mode);
    vw_job->maxcol = view_pic_colours(&g);
    if (!g.truecolor) {
        /* one with more colours than the screen: made ready for it */
        vw_job->tw = g.w;
        vw_job->th = g.h;
        vw_job->tall = gfx_mode_tall(g.mode);
        vw_job->viewfit = TRUE;
        vw_job->maxcol = view_pic_colours(&g);
    }
    if (first)
        ldr_submit_first(vw_job);
    else
        ldr_submit(vw_job);
}

static void vw_ahead(const struct ItemList *l, LONG from, LONG dir)
{
    LONG i = view_step(l, from, dir);
    if (i < 0 || i == from)
        return;
    if (vw_job && strcmp(vw_job->path, l->items[i].path) == 0)
        return;                             /* already on its way */
    if (vw_job && vw_job->busy)
        return;                             /* one at a time */
    vw_drop();
    vw_start(l->items[i].path, FALSE);
}

/* The picture read ahead for path, if it is: its object, or what was
 * made of it for the screen, and what it is (now the caller's). */
static APTR vw_take(const char *path, BOOL *remapped, struct Thumb **t, struct PicInfo *info)
{
    APTR o = NULL;
    *t = NULL;
    if (vw_job && strcmp(vw_job->path, path) == 0) {
        if (vw_job->busy) {
            ldr_submit_first(vw_job);       /* wanted now: hurried, or to the front */
            ldr_wait(vw_job);               /* nearly there: sooner than starting over */
        }
        if (vw_job->ran) {
            char m[300];
            o = vw_job->obj;
            *remapped = vw_job->remapped;
            *t = vw_job->thumb;
            *info = vw_job->info;
            vw_job->obj = NULL;
            vw_job->thumb = NULL;
            if (*t) {
                char cols[24];
                if ((*t)->argb)
                    strcpy(cols, "true colour");
                else
                    sprintf(cols, "%lu colours", (unsigned long)(*t)->ncol);
                sprintf(m, "%.80s, %lu x %lu, depth %lu: decoded in %lu ms, made %lu x %lu, "
                           "%s, in %lu ms",
                        FilePart((STRPTR)path), (unsigned long)info->w, (unsigned long)info->h,
                        (unsigned long)info->depth, (unsigned long)vw_job->ms_decode,
                        (unsigned long)(*t)->w, (unsigned long)(*t)->h,
                        cols, (unsigned long)vw_job->ms_thumb);
                amb_log("viewer, read ahead", m);
            }
        }
        vw_drop();
    }
    return o;
}

/* ---- Plex photos in the viewer (vw_remote): l's entries are IT_PIC, path
 * the file the photo is kept in (RAM:, plex_art), sortkey its picture's
 * key on the server. Each is asked of the server when wanted (ART_VW),
 * the next while one is looked at (ART_VWNEXT), made for the screen there
 * (PNG); the files of those left behind are deleted (they are big). */
static BOOL vw_remote, vw_failed;
static const struct ItemList *vw_list;
static LONG vw_cur = -1, vw_dir = 1;
#define VW_KEEP 4
static char vw_kept[VW_KEEP][120];
static int vw_kept_next;

/* The size a photo is asked for: the screen's, in square pixels (hires
 * pixels are twice as tall as wide: twice the lines, halved here). */
static void vw_photo_size(WORD *w, WORD *h)
{
    *w = g.w;
    *h = gfx_mode_tall(g.mode) ? g.h * 2 : g.h;
}

static void vw_keep(const char *file)
{
    int i;
    for (i = 0; i < VW_KEEP; i++)
        if (!strcmp(vw_kept[i], file))
            return;
    if (vw_kept[vw_kept_next][0])
        DeleteFile((CONST_STRPTR)vw_kept[vw_kept_next]);  /* the oldest: well behind */
    amb_strlcpy(vw_kept[vw_kept_next], file, sizeof(vw_kept[0]));
    vw_kept_next = (vw_kept_next + 1) % VW_KEEP;
}

static void vw_forget(void)
{
    int i, k;
    for (k = ART_VW; k <= ART_VWNEXT; k++)
        if (art_nj[k]) {
            net_cancel(art_nj[k]);
            art_nj[k]->orphan = TRUE;       /* freed when it comes back */
            art_nj[k]->seq = 0;
            art_nj[k] = NULL;
        }
    for (i = 0; i < VW_KEEP; i++) {
        if (vw_kept[i][0])
            DeleteFile((CONST_STRPTR)vw_kept[i]);
        vw_kept[i][0] = 0;
    }
    vw_kept_next = 0;
}

/* Photo i's file is here: TRUE; else asked for as kind (unless on its way). */
static BOOL vw_fetch(const struct ItemList *l, LONG i, int kind)
{
    const struct Item *it = &l->items[i];
    char file[120];
    WORD w, h;
    BPTR lk;
    int k;
    if ((lk = Lock((CONST_STRPTR)it->path, SHARED_LOCK))) {
        UnLock(lk);
        vw_keep(it->path);
        return TRUE;
    }
    for (k = ART_VW; k <= ART_VWNEXT; k++)
        if (art_nj[k] && !strcmp(art_want[k], it->path))
            return FALSE;                   /* asked for already */
    vw_photo_size(&w, &h);
    if (art_fetch(kind, it->sortkey, w, h, file)) {
        vw_keep(file);
        return TRUE;
    }
    if (!art_nj[kind] && kind == ART_VW)
        vw_failed = TRUE;                   /* could not even be asked */
    return FALSE;
}

/* Show the pictures of l from entry idx; returns the last one shown. */
static LONG viewer_run(const struct ItemList *l, LONG idx, BOOL slideshow)
{
    struct PicInfo pi = { 0 };
    char scale[24];
    LONG shown = -1, want = idx, dir = 1, asked = -1, fails = 0;
    ULONG bar_until = 0, next_at = 0, jlast = 0, wait_from = 0, spin_tick = 0, ptr_until = 0;
    BOOL by_hand = FALSE;               /* the next one asked for (keys, joystick, mouse): no transition */
    BOOL quit = FALSE, ok = FALSE, bar_due = TRUE, loading_bar = FALSE;

    vw_mouse = FALSE;

    if (idx < 0 || idx >= l->count || l->items[idx].kind != IT_PIC)
        return idx;
    hold_stop();
    ov.on = FALSE;
    tb_close(&pv_tb);
    det.shown = FALSE;
    det.pending = FALSE;
    pv_let_go();                    /* the viewer takes the screen's colours over */
    lv_hold();                      /* (a layout: the classic drawing back) */
    vw_active = TRUE;
    gfx_dbuf_stop(&g);              /* it draws straight onto the screen */
    gfx_window_rmb(&g, 32767);      /* (the right button: Back, not the menus) */
    if (g.wb && vw_full && !gfx_fs_enter(&g))
        vw_full = FALSE;            /* full screen, as chosen before (from a window) */
    if (!view_begin(&g)) {
        gfx_fs_leave(&g);
        gfx_dbuf_start(&g);
        vw_active = FALSE;
        set_msg(_("Not enough memory for the viewer"));
        return idx;
    }
    if (LowLevelBase)
        jlast = gfx_joy(&g);
    while (!quit) {
        struct IntuiMessage *im;
        LONG step = 0;
        BOOL toggle = FALSE, poke = FALSE, moused = FALSE, keyed = FALSE, full = FALSE;
        ULONG now;
        while (ldr_collect())
            ;                               /* the loader's jobs back: marked done */
        if (vw_remote)
            radio_collect();                /* the server's photos, as they come */
        if (want != shown) {
            BOOL here = TRUE;
            if (vw_remote && !(here = vw_fetch(l, want, ART_VW)) && vw_failed) {
                /* not sent: said so, and on to the next */
                LONG nx = view_step(l, want, dir);
                char d[128];
                vw_failed = FALSE;
                asked = -1;
                loading_bar = FALSE;
                view_spin_hide(&g);
                sprintf(d, _("not sent by %.50s"), plex_server_name(cur() ? cur()->base : ""));
                view_bar(&g, l->items[want].label, d, _("Esc  back"));
                bar_until = time_ms() + BAR_MS;
                if (++fails >= 3 || nx < 0 || nx == want || (shown < 0 && nx == idx)) {
                    Delay(50);              /* read, then back to the list */
                    quit = TRUE;
                    break;
                }
                want = nx;
                goto input;
            }
            if (here) {
                /* not read ahead: decoded by the loader now, while the
                 * spinner turns and keys are still taken */
                const char *path = l->items[want].path;
                if (!vw_job || strcmp(vw_job->path, path)) {
                    vw_drop();
                    vw_start(path, TRUE);
                }
                here = !vw_job || !vw_job->busy;   /* no job: view_show reads it */
            }
            if (!here) {
                if (asked != want) {
                    asked = want;
                    wait_from = time_ms();
                    loading_bar = FALSE;
                }
                if (!loading_bar && (shown < 0 || !slideshow)) {
                    /* the bar says what comes; a slideshow's picture stays
                     * in view, the spinner small in its corner */
                    view_spin_hide(&g);
                    view_bar(&g, l->items[want].label,
                             vw_remote ? _("Loading from Plex...") : _("Loading..."), _("Esc  back"));
                    bar_until = time_ms() + 3600000;   /* till it comes */
                    loading_bar = TRUE;
                }
                if (time_ms() - wait_from > 300)
                    view_spin(&g, (UWORD)(spin_tick++ / 4));
                goto input;                 /* keys meanwhile: another one, or back */
            }
        }
        if (want != shown) {
            BOOL remapped = FALSE;
            struct Thumb *pre_t;
            struct PicInfo pre_i;
            APTR pre = vw_take(l->items[want].path, &remapped, &pre_t, &pre_i);
            ULONG t0 = time_ms();
            BOOL keep = vw_mouse && (LONG)(t0 - bar_until) < 0;    /* the mouse's bar stays up */
            int vt = view_transition;
            if (by_hand)
                view_transition = VT_CUT;   /* stepped through by hand: at once */
            ok = view_show(&g, l->items[want].path, l->items[want].label, &pi, scale, sizeof(scale),
                           pre, remapped, pre_t, &pre_i);
            view_transition = vt;
            {
                char m[300];
                sprintf(m, "%.80s, %lu x %lu, depth %lu: %lu ms%s, %s%s", l->items[want].label,
                        (unsigned long)pi.w, (unsigned long)pi.h, (unsigned long)pi.depth,
                        (unsigned long)(time_ms() - t0),
                        pre || pre_t ? " (read ahead)" : "",
                        view_last_own ? (pi.cls[0] ? pi.cls : "ilbm.c") : pi.cls[0] ? pi.cls : "?",
                        view_last_own ? " (own reader)" : !strcmp(pi.cls, "zz9k") ? " (the ZZ9000's session)" : ".datatype");
                amb_log("viewer", m);
            }
            shown = want;
            asked = -1;
            fails = 0;
            loading_bar = FALSE;
            if (vw_remote) {
                /* the next asked for now; decoded ahead once it is here */
                LONG nx = view_step(l, shown, dir);
                vw_cur = shown;
                vw_dir = dir;
                if (nx >= 0 && nx != shown && vw_fetch(l, nx, ART_VWNEXT))
                    vw_ahead(l, shown, dir);
            } else {
                vw_ahead(l, shown, dir);
            }
            next_at = time_ms() + SLIDE_MS;
            if (ok && (bar_due || keep || view_bar_shown())) {
                /* (a bar kept up through the change: the new picture's, its
                 * time to go as it was) */
                BOOL through = !bar_due && !keep;
                viewer_bar(l, shown, &pi, scale, slideshow);
                if (!keep && !through)
                    bar_until = time_ms() + BAR_MS;
            }
            bar_due = !slideshow;
        }
    input:
        while ((im = (struct IntuiMessage *)GetMsg(g.win->UserPort))) {
            ULONG cl = im->Class;
            UWORD code = im->Code;
            WORD mx = im->MouseX - g.win->BorderLeft, my = im->MouseY - g.win->BorderTop;
            ReplyMsg((struct Message *)im);
            if (gfx_window_msg(&g, cl, code)) {
                if (gfx_close_asked || gfx_iconify_asked)
                    quit = TRUE;            /* the close gadget: out, then Ambiance quits;
                                             * iconify: out, then iconified */
            } else if (cl == IDCMP_MENUPICK) {
                /* Amiga-F comes as Project > Full screen: here, the
                 * picture's own (as F) */
                UWORD c = code;
                ULONG what;
                while ((what = gfx_window_menu_pick(&c)))
                    if (what == WM_FULL)
                        full = TRUE;
                    else if (what == WM_ICONIFY) {
                        gfx_iconify_asked = TRUE;   /* out, then iconified */
                        quit = TRUE;
                    }
            } else if (cl == IDCMP_MOUSEMOVE) {
                moused = TRUE;              /* the pointer, and the bar with its buttons */
            } else if (cl == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX)) {
                keyed = TRUE;
                switch (code) {
                case 0x4F: case 0x4C: step--; view_bar_press(&g, VB_PREV); break;   /* left, up */
                case 0x4E: case 0x4D: step++; view_bar_press(&g, VB_NEXT); break;   /* right, down */
                case 0x43: case 0x44:                                               /* enter, return */
                    toggle = !toggle;
                    view_bar_press(&g, VB_PLAY);
                    break;
                case 0x45: case 0x41: quit = TRUE; break;       /* esc, backspace */
                case 0x23: full = TRUE; break;                  /* F, Amiga-F */
                default: poke = TRUE; break;
                }
            } else if (cl == IDCMP_VANILLAKEY) {
                keyed = TRUE;
                if (code == 27 || code == 8)
                    quit = TRUE;
                else if (code == 13 || code == ' ') {
                    toggle = !toggle;
                    view_bar_press(&g, VB_PLAY);
                }
                else if (code == 'f' || code == 'F')
                    full = TRUE;            /* (Amiga-F comes as its raw key) */
                else
                    poke = TRUE;
            } else if (cl == IDCMP_MOUSEBUTTONS) {
                if (code == SELECTDOWN) {
                    /* the bar's buttons; elsewhere in the bar nothing;
                     * on the picture, the next one */
                    int b = view_bar_hit(&g, mx, my);
                    moused = TRUE;
                    if (b >= 0 && b < VB_COUNT)
                        view_bar_press(&g, b);      /* (clicked: shown pressed, as by its key) */
                    if (b == VB_PREV)
                        step--;
                    else if (b == VB_NEXT)
                        step++;
                    else if (b == VB_PLAY)
                        toggle = !toggle;
                    else if (b == VB_FULL)
                        full = TRUE;
                    else if (!view_bar_at(&g, my))
                        step++;
                } else if (code == MENUDOWN)
                    quit = TRUE;
            }
        }
        if (gfx_wb_event() == GFX_WB_CLOSING)
            quit = TRUE;                    /* Workbench closing: back, so the window can go */
        if (LowLevelBase) {
            ULONG st = gfx_joy(&g), pressed = st & ~jlast;
            if ((st & JP_TYPE_MASK) == JP_TYPE_JOYSTK || (st & JP_TYPE_MASK) == JP_TYPE_GAMECTLR) {
                if (pressed & (JPF_JOY_LEFT | JPF_JOY_UP))
                    step--;
                if (pressed & (JPF_JOY_RIGHT | JPF_JOY_DOWN))
                    step++;
                if (pressed & JPF_BUTTON_RED)
                    toggle = !toggle;
                if (pressed & JPF_BUTTON_BLUE)
                    quit = TRUE;
                if (pressed)
                    keyed = TRUE;
            }
            jlast = st;
        }
        if (quit)
            break;
        if (full && (g.wb || gfx_fs_on())) {
            /* from a window: the whole display (a screen in Workbench's
             * mode) or back, the picture shown again at its new size */
            view_bar_hide(&g);
            view_end(&g);
            if (gfx_fs_on())
                gfx_fs_leave(&g);
            else if (!gfx_fs_enter(&g))
                set_msg("");
            vw_full = gfx_fs_on();
            if (!view_begin(&g)) {
                quit = TRUE;
                break;
            }
            shown = -1;                     /* shown again, from its file */
            bar_due = TRUE;
            continue;
        }
        now = time_ms();
        if (keyed && vw_mouse) {
            /* keys again: the keys' hint, the pointer away */
            vw_mouse = FALSE;
            view_pointer(&g, FALSE);
            if (view_bar_has_buttons())
                poke = TRUE;
        } else if (moused) {
            view_pointer(&g, TRUE);
            ptr_until = now + BAR_MS;
            if (ok && !view_bar_has_buttons()) {
                vw_mouse = TRUE;
                poke = TRUE;
            }
            bar_until = now + BAR_MS;
        }
        if (toggle) {
            slideshow = !slideshow;
            next_at = now + SLIDE_MS;
            poke = TRUE;
        }
        if (step) {
            LONG i = shown, d = step > 0 ? 1 : -1, n = step > 0 ? step : -step;
            dir = d;
            while (n-- > 0 && i >= 0)
                i = view_step(l, i, d);
            if (i >= 0)
                want = i;
            bar_due = TRUE;
            by_hand = TRUE;
        } else if (slideshow && (LONG)(now - next_at) >= 0) {
            LONG i = view_step(l, shown, 1);
            dir = 1;
            if (i >= 0)
                want = i;
            by_hand = FALSE;                /* the slideshow's own: its transition */
        }
        if (poke && want == shown && ok) {
            viewer_bar(l, shown, &pi, scale, slideshow);
            bar_until = now + BAR_MS;
        }
        view_bar_tick(&g);                  /* (a button a key pressed: raised again) */
        if (view_bar_shown() && ok && (LONG)(now - bar_until) >= 0) {
            view_bar_hide(&g);
            vw_mouse = FALSE;
        }
        if (view_pointer_shown() && (LONG)(now - ptr_until) >= 0 && !view_bar_has_buttons())
            view_pointer(&g, FALSE);        /* the mouse at rest: hidden again */
        gfx_present(&g);                    /* a window: what changed, copied in */
        dbg_sleep_begin();
        WaitTOF();
        dbg_sleep_end();
    }
    vw_drop();
    view_end(&g);
    gfx_fs_leave(&g);               /* (full screen: the window again) */
    vw_active = FALSE;
    return shown >= 0 ? shown : idx;
}

/* Back from the viewer: the UI again, its colours faded back in. */
static void viewer_done(void)
{
    BOOL hidden = ptr_hidden != NULL;
    msg[0] = 0;
    ptr_hidden = NULL;              /* the viewer put the pointer back */
    if (hidden)
        pointer_hide();             /* hidden it was: hidden it stays */
    /* double buffered again first, then everything drawn into the buffer
     * and shown with a flip: drawn onto the screen and then copied, it
     * came back blank on PrismRTG (the copy made before the card had
     * drawn it, it seems), only what was drawn later showing */
    gfx_dbuf_start(&g);
    plex_shortcut_fix(cur());       /* (its Now playing row, held while it was viewed) */
    if (lv_chosen() && !g.truecolor && g.rp) {
        /* a layout on an 8-bit screen: the screen cleared while it is still
         * black, the UI's colours back at once, then the layout opened (its
         * own colours made from those) - faded in after, they went over
         * the layout's pens */
        SetRast(g.rp, 0);
        view_restore_now(&g);
        lv_resume();
        draw_all();
        lv_frame();
        gfx_dirty();
        gfx_present(&g);
        return;
    }
    lv_resume();                    /* a layout: on again first (the classic one only counted) */
    draw_all();
    lv_frame();
    gfx_dirty();
    gfx_present(&g);
    view_restore(&g);
}

static void layout_let_go(void);
static void window_relaid(WORD ow, WORD oh, ULONG t0);

/* An animation of a Videos (or Pictures) list, played on a screen of its
 * own in front of Ambiance's (anim.c), which comes back as it was. */
static void anim_from_item(const struct Item *it);
static void jf_film(const struct Item *it);
static const struct VProbe *anim_net_probe;    /* anim_from_item's item: a stream, probed so */
static const char *anim_net_seek;               /* ...its start's and seeks' (anim.c: '*' the ms) */

static void anim_from_page(struct Page *p)
{
    const struct Item *it = &p->list.items[p->sel];
    if (jf_key(it->path))
        jf_film(it);                        /* a Jellyfin film: fetched first */
    else
        anim_from_item(it);
}

static void anim_from_item(const struct Item *it)
{
    char err[160], title[120], by[200];
    BOOL dvd = is_dvd_row(it);
    BOOL mpeg = dvd || (it->sortkey && !strcmp(it->sortkey, "MPEG"));
    /* (an MPEG plays through the stream process: what streams stops, sound or not) */
    BOOL sound = mpeg || anim_has_sound(it->path), stream = FALSE, track = FALSE, was_paused = FALSE;
    BOOL module = FALSE;
    LONG idx = np.idx;
    struct MpStatus st;
    ULONG size = 0;
    /* with a sound of its own, what plays waits: a module paused; a station
     * or a track stopped (paused, it would go on being read and decoded,
     * unheard), played again afterwards, a track from where it was */
    if (sound && mp_running()) {
        if (mp_is_stream()) {
            mp_status(&st);
            track = mp_is_track();
            was_paused = mp_paused();
            size = st_size();
            amb_strlcpy(title, mp_stream_name(), sizeof(title));
            amb_strlcpy(by, mp_track_by(), sizeof(by));
            mp_stop();
            stream = TRUE;
            amb_log("anim", track ? "the track stopped while it plays" : "the station stopped while it plays");
        } else {
            if (mp_playing()) {
                mp_pause(TRUE);
                module = TRUE;
            }
            mp_hold(TRUE);                  /* (the mixer's: AHI let go, paused or not) */
        }
    }
    /* on this screen it draws straight onto it: single buffered meanwhile,
     * everything drawn again after (into the back buffer, then a flip) */
    lv_hold();                              /* (a layout: the classic drawing back) */
    gfx_dbuf_stop(&g);
    gfx_window_rmb(&g, 32767);              /* (the right button: Back, not the menus) */
    {
        BOOL loop_was = play_loop;
        WORD aw = g.w, ah = g.h;
        if (dvd && is_dvd_image(it)) {
            if (anim_play_dvd(&g, it->path, TRUE, plex_decoder_cpu(), err, sizeof(err)) < 0 && err[0])
                set_msg(err);               /* (a DVD's image: the same, from its file) */
        } else if (dvd) {
            char vol[40];
            const char *c = strchr(it->path, ':');
            amb_strlcpy(vol, it->path, c ? (LONG)(c - it->path) + 1 : (LONG)sizeof(vol));
            if (anim_play_dvd(&g, vol, FALSE, plex_decoder_cpu(), err, sizeof(err)) < 0 && err[0])
                set_msg(err);               /* (a DVD's film: dvdplay.c, then as an MPEG) */
        } else if (mpeg) {
            if ((anim_net_probe ? anim_play_video_net(&g, it->path, it->label, anim_net_probe,
                                                      anim_net_seek, plex_decoder_cpu(), err,
                                                      sizeof(err))
                                : anim_play_video(&g, it->path, plex_decoder_cpu(), err, sizeof(err))) < 0 &&
                err[0])
                set_msg(err);               /* (MPEG: the ZZ9000's, anim.c and video.c) */
        } else if (anim_play(&g, it->path, sound_ahi, anim_classic, err, sizeof(err)) < 0 || err[0]) {
            amb_log("anim", err);
            set_msg(err);
        }
        if (play_loop != loop_was)          /* (its loop button: the one setting) */
            save_setting(LOOP_VAR, play_loop ? "1" : "0");
        if (g.wb && (g.w != aw || g.h != ah)) {
            layout_let_go();                /* resized while it played: laid out for it */
            window_relaid(aw, ah, time_ms());
        }
    }
    gfx_dbuf_start(&g);
    lv_resume();                            /* a layout: on again first */
    draw_all();
    lv_frame();
    gfx_dirty();
    gfx_present(&g);
    mp_hold(FALSE);
    if (module)
        mp_pause(FALSE);
    if (stream) {
        if (track)
            np_track_from(idx, st.elapsed_ms, st.total_ms, size, title, by);
        else
            np_start(idx);
        if (was_paused)
            mp_pause(TRUE);
    }
    draw_footer();
}

static void view_from_page(struct Page *p)
{
    LONG vis, last;
    struct Item *it = p->sel >= 0 && p->sel < p->list.count ? &p->list.items[p->sel] : NULL;
    const char *f = it ? pics_format(it->label) : NULL;
    if (it && f && !strcmp(f, "ILBM")) {    /* by an IFF name: what it holds */
        const char *k = pics_iff_kind(it->path);
        if (k && !strcmp(k, "ANIM")) {
            it->kind = IT_ANIM;             /* an animation by a picture's name (.iff) */
            it->sortkey = (char *)"ANIM";
            anim_from_page(p);
            return;
        }
        if (k && strcmp(k, "ILBM") && strcmp(k, "PBM ")) {
            char m[80];                     /* not a picture: a sound (8SVX), music (SMUS).. */
            sprintf(m, _("Not a picture: an IFF %s file"), k);
            set_msg(m);
            draw_footer();
            return;
        }
    }
    vis = list_visible();
    last = viewer_run(&p->list, p->sel, FALSE);
    p->sel = last;
    if (p->sel < p->top)
        p->top = p->sel;
    if (p->sel >= p->top + vis)
        p->top = p->sel - vis + 1;
    viewer_done();
}

/* A photo page's "Slideshow of these N photos", counted again (the parts
 * after the first come in while it is shown). */
static void plex_slides_label(struct Page *p)
{
    LONG i, k = 0;
    char label[96], *s;
    struct Item *a = NULL;
    for (i = 0; i < p->list.count; i++) {
        struct Item *it = &p->list.items[i];
        if (it->kind == IT_PLEXPHOTO)
            k++;
        else if (it->kind == IT_ACTION && it->action == ACT_PLEXSLIDES)
            a = it;
    }
    if (!a || k < 2)
        return;
    sprintf(label, _("Slideshow of these %ld photos"), (long)k);
    if (strcmp(a->label, label) && (s = list_strdup(&p->list, label)))
        a->label = s;
}

/* A Plex page's photos in the viewer, from entry idx (-1: the first, as a
 * slideshow); the page's selection follows the last one shown. */
static void plex_photo_view(struct Page *p, LONG idx, BOOL slideshow)
{
    struct ItemList l;
    char thumb[160], file[120], m[100];
    LONG i, first = -1, last, k, vis = list_visible();
    ULONG t0 = time_ms();
    WORD w, h;
    if (!list_init(&l))
        return;
    vw_photo_size(&w, &h);
    busy_begin();                           /* thousands of photos: the spinner meanwhile */
    for (i = 0; i < p->list.count; i++) {
        struct Item *it = &p->list.items[i];
        if (it->kind != IT_PLEXPHOTO)
            continue;
        radio_field(it, 2, thumb, sizeof(thumb));
        plex_art_file(thumb, w, h, file);  /* the request is made when it is fetched */
        if (i == idx || (idx < 0 && first < 0))
            first = l.count;
        list_add(&l, it->label, file, thumb, IT_PIC);
        if (!(l.count & 63)) {
            busy_tick();
            gfx_present(&g);
        }
    }
    if (first < 0) {
        list_free(&l);
        busy_end(TRUE);
        return;
    }
    busy_end(FALSE);                        /* the viewer covers it */
    sprintf(m, "%ld photos, asked for at %ld x %ld, listed in %ld ms", (long)l.count, (long)w,
            (long)h, (long)(time_ms() - t0));
    amb_log("plex: viewer", m);
    vw_remote = TRUE;
    vw_failed = FALSE;
    vw_list = &l;
    vw_cur = -1;
    vw_dir = 1;
    last = viewer_run(&l, first, slideshow);
    vw_forget();
    vw_remote = FALSE;
    vw_list = NULL;
    vw_cur = -1;
    /* the page on the photo last shown */
    for (i = 0, k = 0; i < p->list.count; i++)
        if (p->list.items[i].kind == IT_PLEXPHOTO && k++ == last) {
            p->sel = i;
            break;
        }
    if (p->sel < p->top)
        p->top = p->sel;
    if (p->sel >= p->top + vis)
        p->top = p->sel - vis + 1;
    list_free(&l);
    viewer_done();
}

/* Fire on a folder of pictures: its slideshow, from the first picture. */
static void folder_slideshow(struct Item *it)
{
    struct ItemList l;
    LONG n, first;
    if (!list_init(&l))
        return;
    set_msg(_("Loading..."));
    busy_begin();
    n = pics_read_ui(it->path, &l);
    busy_end(n <= 0);               /* the viewer covers the screen next */
    first = n > 0 ? view_step(&l, l.count - 1, 1) : -1;
    if (n == -2) {
        msg[0] = 0;                 /* given up for input */
        draw_footer();
    } else if (first < 0) {
        set_msg(_("No pictures in that folder"));
    } else {
        viewer_run(&l, first, TRUE);
        viewer_done();
    }
    list_free(&l);
}

void act_move(LONG d)
{
    struct Page *p = cur();
    nav_moved();
    if (focus == FOCUS_MENU) {
        LONG i = menu_sel + d;
        if (i < 0)
            i = 0;
        if (i >= nmenu)
            i = nmenu - 1;
        menu_select(i);
        return;
    }
    if (!p)
        return;
    if (focus == FOCUS_PREVIEW) {
        tb_scroll_by(&pv_tb, d * pv_tb.line_h);     /* the main loop animates it */
        return;
    }
    if (p->type == PT_PLAYER) {
        np_press(d < 0 ? NB_VUP : NB_VDOWN);       /* Up: louder */
        return;
    }
    if (p->type == PT_LIST) {
        LONG s;
        if (d)
            det.dir = d > 0 ? 1 : -1;
        if (d > 0 && ch_reading(p) && p->sel + d >= p->list.count) {
            busy_begin();
            ch_wait_until(p, p->sel + d + 1, TRUE);    /* outran the reading: wait for it */
            busy_end(TRUE);
        }
        s = p->sel + d;
        if (s < 0)
            s = 0;
        if (s >= p->list.count)
            s = p->list.count - 1;
        while (is_heading(p, s) && s + (d < 0 ? -1 : 1) >= 0 && s + (d < 0 ? -1 : 1) < p->list.count)
            s += d < 0 ? -1 : 1;            /* over a heading */
        if (is_heading(p, s))
            s = p->sel;                     /* a heading at the end: stay */
        if (s < 0 || s == p->sel)
            return;
        {
            LONG old = p->sel, old_top = p->top;
            p->sel = s;
            if (ov.on && ov.page == p)
                ov_relabel(ov_letter_in(p->list.items, p->list.count, s));
            draw_title_count(p);    /* only the number changes */
            draw_list_move(p, old, old_top);
        }
    } else {
        tb_scroll_by(&p->tb, d * p->tb.line_h);   /* the main loop animates it */
    }
}


static void open_screenmodes(void)
{
    struct Page *p = page_push(PT_LIST, _("Screen mode"));
    if (!p)
        return;
    build_screenmodes(p);
    draw_pane(TRUE);
    draw_footer();
}

static void open_styles(void)
{
    struct Page *p = page_push(PT_LIST, _("Style"));
    if (!p)
        return;
    build_styles(p);
    draw_pane(TRUE);
    draw_footer();
}

static void open_credits(void)
{
    struct Page *p = page_push(PT_TEXT, _("Credits"));
    if (!p)
        return;
    build_credits(p);
    draw_pane(TRUE);
    draw_footer();
}

static void open_sysinfo(void)
{
    struct Page *p = page_push(PT_TEXT, _("System information"));
    if (!p)
        return;
    build_sysinfo(p);
    draw_pane(TRUE);
    draw_footer();
}

/* Switch to another screen mode now and remember it. The screen is closed
 * and reopened, so every page is rebuilt for the new layout; the user lands
 * back on the Screen mode list with the new mode selected. */
static void apply_screen_mode(int sm)
{
    int old = screen_mode, i;
    const char *why = gfx_mode_problem(&caps, sm);
    BOOL failed = FALSE;
    if (why) {
        char m[128];
        sprintf(m, _("That mode %.60s"), why);
        set_msg(m);
        return;
    }
    if (sm == screen_mode)
        return;
    /* The screen already open is the one asked for (Automatic and the mode
     * it stands for, either way round): the setting changes, the screen
     * is kept, without closing and opening it again (a flash). */
    if (!g.wb && (sm == SM_AUTO ? gfx_mode_auto(&caps) : sm) == g.mode) {
        struct Page *p = cur();
        screen_mode = sm;
        save_screen_mode();
        settings_rebuild();         /* the Display page's row says which */
        if (p && p->type == PT_LIST) {
            for (i = 0; i < p->list.count; i++)
                if (p->list.items[i].action >= ACT_MODE0 && p->list.items[i].action < ACT_MODE0 + SM_COUNT)
                    p->list.items[i].mark = p->list.items[i].action == ACT_MODE0 + sm ? MARK_RADIO_ON
                                                                                      : MARK_RADIO_OFF;
            draw_list_rows(p);      /* the radio marks only: no flash */
        }
        return;
    }
    pages_clear();
    pv_flush();                     /* decoded for this screen */
    ptr_hidden = NULL;              /* a new window comes with its pointer */
    spin_free();
    ov_bm_free();
    screen_close();
    screen_mode = sm;
    if (lv_chosen() && ui_style == STYLE_PIXEL && gfx_style_possible(&caps, sm, STYLE_SMOOTH)) {
        /* a layout chosen, and a mode that can show it: Smooth again (a
         * native mode had made it Pixel) */
        ui_style = STYLE_SMOOTH;
        save_setting(STYLE_VAR, "smooth");
    }
    if (!gfx_open(&g, &caps, sm, ui_style)) {
        failed = TRUE;
        screen_mode = old;
        if (!gfx_open(&g, &caps, old, ui_style)) {
            running = FALSE;
            return;
        }
    } else {
        save_screen_mode();
        /* a screen that can only be pixel: the style goes back to Pixel */
        if (ui_style == STYLE_SMOOTH && !gfx_style_possible(&caps, g.mode, STYLE_SMOOTH)) {
            ui_style = STYLE_PIXEL;
            save_setting(STYLE_VAR, "pixel");
        }
    }
    for (i = 0; i < nmenu; i++)
        if (menu[i] == MOD_SETTINGS)
            menu_sel = i;
    build_root(MOD_SETTINGS);
    if (cur())
        cur()->sel = SETTINGS_SCREEN_ROW;
    {
        /* back where it was chosen: Settings > Display > Screen mode */
        struct Page *p = page_push(PT_LIST, _("Display"));
        if (p) {
            build_screen_settings(p);
            p->sel = screen_row_mode();
        }
        if ((p = page_push(PT_LIST, _("Screen mode"))))
            build_screenmodes(p);
    }
    focus = FOCUS_CONTENT;
    msg[0] = 0;
    lv_resume();                    /* a layout: on again first, if this screen suits it */
    draw_all();
    if (failed)
        set_msg(_("Could not open that screen mode"));
    else
        note_style();
}

static void wb_note_fallback(void);

/* Settings > Display > Window, or Text size: the display closed and opened
 * again as now wanted, every page rebuilt, and back on Settings > Display
 * at that row (in the list sub, if one). */
static BOOL display_colours_row;     /* display_again: back at Colours' row */
static int display_sel_act;          /* display_again: back at the row of this action (0: none) */

static void display_again(BOOL size_row, const char *sub_title, void (*sub)(struct Page *))
{
    struct Page *p;
    BOOL ok;
    int i;
    pages_clear();
    pv_flush();
    ptr_hidden = NULL;              /* a new window comes with its pointer */
    spin_free();
    ov_bm_free();
    screen_close();
    if (!(ok = gfx_open(&g, &caps, screen_mode, ui_style)) && gfx_window_wanted) {
        gfx_window_wanted = FALSE;  /* full screen, as it was */
        ok = gfx_open(&g, &caps, screen_mode, ui_style);
        gfx_window_wanted = TRUE;
    }
    if (!ok && !(ok = gfx_open(&g, &caps, SM_AUTO, ui_style))) {
        running = FALSE;
        return;
    }
    for (i = 0; i < nmenu; i++)
        if (menu[i] == MOD_SETTINGS)
            menu_sel = i;
    build_root(MOD_SETTINGS);
    if (cur())
        cur()->sel = SETTINGS_SCREEN_ROW;
    if ((p = page_push(PT_LIST, _("Display")))) {
        build_screen_settings(p);
        p->sel = size_row ? screen_row_fonts() : screen_row_window();
        if (display_colours_row || display_sel_act) {
            LONG i;
            for (i = 0; i < p->list.count; i++)
                if (p->list.items[i].action == (display_sel_act ? display_sel_act : ACT_COLOURS))
                    p->sel = i;
        }
        if (p->sel < 0)
            p->sel = 0;
    }
    if (size_row && (p = page_push(PT_LIST, _("Fonts")))) {
        build_font_settings(p);     /* (Text size is under Fonts) */
        p->sel = fonts_row_size();
    }
    if (sub && (p = page_push(PT_LIST, sub_title)))
        sub(p);
    focus = FOCUS_CONTENT;
    msg[0] = 0;
    lv_resume();                    /* a layout: on again first, if this screen suits it */
    draw_all();
    wb_note_fallback();
    if (gfx_window_wanted && !g.wb)
        set_msg(_("The window could not open: full screen"));
    else
        note_style();
}

static void toggle_window(void)
{
    gfx_window_wanted = !g.wb;
    save_setting(WINDOW_VAR, gfx_window_wanted ? "1" : "0");
    win_chosen_by = gfx_window_wanted ? "Settings: a window" : "Settings: full screen";
    display_again(FALSE, NULL, NULL);
}

/* Resized: laid out again once it settles (a resize sends one NEWSIZE,
 * an opaque one many). Pages keep what they hold: lists and Now Playing lay
 * themselves out as drawn; what was made for the old size goes (text boxes,
 * a text page's picture, the letter column's choice, the preview's
 * pictures, the spinner, the letter, the spectrum's tables). A text page's
 * lines keep their wrapping until it is opened again. */
#define RESIZE_SETTLE 150

/* What was made for the layout let go before it changes (text boxes, a
 * text page's picture, the letter column's choice). */
static void layout_let_go(void)
{
    int i;
    hold_stop();
    ov.on = FALSE;
    tb_close(&pv_tb);
    det.shown = FALSE;
    for (i = 0; i < depth; i++) {
        struct Page *p = &pages[i];
        tb_close(&p->tb);
        if (p->pic_bm) {
            WaitBlit();
            FreeBitMap(p->pic_bm);
            p->pic_bm = NULL;
            p->pic_y = -1;
        }
        p->aside = 0;
    }
}

/* ...and after: the rest made for it, and each list's selection in view. */
static void layout_again(void)
{
    WORD vis;
    int i;
    spin_free();
    ov_bm_free();
    pv_flush();
    vis_stop();
    vis = list_visible();
    for (i = 0; i < depth; i++) {
        struct Page *p = &pages[i];
        if (p->sel >= p->top + vis)
            p->top = p->sel - vis + 1;
        if (p->top > p->list.count - vis)
            p->top = p->list.count - vis;
        if (p->top < 0)
            p->top = 0;
    }
}

/* The footer shown or hidden (a window used by the mouse): the content's
 * foot moved, everything laid out again. */
static void foot_show(BOOL on)
{
    if (on != foot_hidden || !g.wb)
        return;
    layout_let_go();
    foot_hidden = !on;
    foot_over = FALSE;
    g.footer_y = foot_hidden ? g.h : g.h - U(12);
    layout_again();
    draw_all();
    foot_since = on && mouse_mode ? time_ms() : 0;
}

/* Which display the Workbench uses: Automatic follows it onto the
 * graphics card (gfx_mode_auto), and a native <-> RTG switch on exit can be
 * slow in some emulators and drivers. Read at the start, and again when
 * Workbench comes back (its screen mode changed). */
static void caps_wb_read(void)
{
    struct Screen *wb = LockPubScreen(NULL);
    if (wb) {
        ULONG id = GetVPModeID(&wb->ViewPort);
        char m[80];
        caps.wb_rtg = caps_mode_is_rtg(id);
        caps.wb_w = wb->Width;
        caps.wb_h = wb->Height;
        sprintf(m, "%s, %lu x %lu, %lu-bit", caps.wb_rtg ? "RTG" : "native chipset",
                (unsigned long)caps.wb_w, (unsigned long)caps.wb_h,
                (unsigned long)GetBitMapAttr(wb->RastPort.BitMap, BMA_DEPTH));
        amb_log("Workbench screen", m);
        UnlockPubScreen(NULL, wb);
    }
}

static void window_relaid(WORD ow, WORD oh, ULONG t0);

/* Workbench closing (its screen mode changed, say): the window closed at
 * once, so Intuition can close its screen with no requester; opened again
 * when it is back, on its new screen, or full screen if a window cannot be
 * there now (under 15 bits, or off the graphics card). What is open stays
 * open, laid out for the new size. */
#define WB_BACK_WAIT 60000              /* ms: Workbench not back by then, opened anyway */

static BOOL wb_fell_back;               /* a window wanted, full screen for now */
static BOOL iconified;                  /* (below: iconify) */
static BOOL mini_on;                    /* the mini player up, the window away (below) */
static struct Gfx mg;                   /* ...its window and drawing */
static void mini_drop(void);
static BOOL iconic_screen;              /* ...full screen: its screen behind Workbench's */

/* Full screen where a window was wanted (Workbench could not take one):
 * Workbench's prefs watched for a change that lets it. */
static void wb_note_fallback(void)
{
    wb_fell_back = gfx_window_wanted && !g.wb;
    gfx_wb_want_back(wb_fell_back);
}

static void wb_follow(void)
{
    ULONG t0 = time_ms(), tl;
    WORD ow = g.w, oh = g.h;
    BOOL was_window = g.wb;
    char m[120];
    if (iconified && !g.wb) {
        if (gfx_wb_event())             /* (iconified, its screen behind: left so) */
            gfx_wb_done();
        return;
    }
    if (mini_on && !iconified && gfx_wb_event() == GFX_WB_CLOSING)
        mini_drop();                    /* (the window comes back on the new Workbench) */
    if (gfx_wb_event() != GFX_WB_CLOSING || (!g.wb && !wb_fell_back)) {
        if (gfx_wb_event())
            gfx_wb_done();              /* (nothing of ours on it) */
        return;
    }
    if (was_window) {
        layout_let_go();
        pv_flush();
        spin_free();
        ov_bm_free();
        vis_stop();
        ptr_hidden = NULL;              /* a new window comes with its pointer */
        screen_close();
        amb_log("window", "Workbench is closing: closed until it is back");
    } else {
        amb_log("window", "Workbench's prefs changed: a window again, if it can be one now");
    }
    gfx_wb_gone();
    while (gfx_wb_event() != GFX_WB_BACK && time_ms() - t0 < WB_BACK_WAIT) {
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            running = FALSE;
            break;
        }
        Delay(5);
    }
    gfx_wb_done();
    SetSignal(0, gfx_wb_sigmask());     /* (a prefs file's other notifications meanwhile) */
    sprintf(m, "%s after %lu ms", running && time_ms() - t0 < WB_BACK_WAIT ? "Workbench back" :
            "Workbench not back", (unsigned long)(time_ms() - t0));
    amb_log("window", m);
    caps_wb_read();
    if (!was_window) {
        const char *why = gfx_window_problem(&caps);
        if (why) {
            sprintf(m, "%.80s: full screen still", why);
            amb_log("window", m);
            return;
        }
        layout_let_go();                /* the full screen closed for the window */
        pv_flush();
        spin_free();
        ov_bm_free();
        vis_stop();
        ptr_hidden = NULL;
        screen_close();
    }
    tl = time_ms();
    if (iconified) {
        /* still iconified: the buffer alone on the new Workbench; or, no
         * window there now, full screen behind it */
        if (!gfx_open_hidden(&g, &caps, ui_style)) {
            if (!gfx_open(&g, &caps, screen_mode, ui_style) &&
                !gfx_open(&g, &caps, SM_AUTO, ui_style)) {
                running = FALSE;
                return;
            }
            if (g.wb) {
                gfx_window_hide(&g);    /* (a window after all) */
            } else {
                ScreenToBack(g.scr);
                iconic_screen = TRUE;
                amb_log("iconify", "no window on the new Workbench: full screen, behind it");
            }
        }
        wb_note_fallback();
        window_relaid(ow, oh, tl);
        return;
    }
    if (!gfx_open(&g, &caps, screen_mode, ui_style) && !gfx_open(&g, &caps, SM_AUTO, ui_style)) {
        running = FALSE;
        return;
    }
    wb_note_fallback();
    foot_hidden = FALSE;
    foot_since = mouse_mode && g.wb ? time_ms() : 0;
    window_relaid(ow, oh, tl);
    if (wb_fell_back)
        set_msg(_("The window could not open: full screen"));
}

static void window_relaid(WORD ow, WORD oh, ULONG t0);

static void window_resized(void)
{
    ULONG t0 = time_ms();
    WORD ow = g.w, oh = g.h;
    gfx_resized_at = 0;
    if (!g.wb)
        return;
    layout_let_go();
    if (!gfx_window_resize(&g))
        return;
    window_relaid(ow, oh, t0);
}

/* ...its new size's layout, from ow x oh (a video resizes the window as it
 * plays, and this follows when it ends). */
static void window_relaid(WORD ow, WORD oh, ULONG t0)
{
    WORD vis;
    char m[200];
    int i;
    if (foot_hidden)
        g.footer_y = g.h;                   /* (still hidden) */
    spin_free();
    ov_bm_free();
    pv_flush();
    vis_stop();
    for (i = 0; i < depth; i++) {           /* text pages wrapped again to the new width */
        struct Page *p = &pages[i];
        if (p->type == PT_TEXT || p->type == PT_DETAIL) {
            LONG n0 = p->list.count;
            if (rewrap_list(&p->list, textbox_w()) && n0 && p->list.count != n0)
                p->tb.pos = p->tb.target = p->tb.pos * p->list.count / n0;    /* about there */
        }
    }
    vis = list_visible();
    for (i = 0; i < depth; i++) {           /* the selection still in view */
        struct Page *p = &pages[i];
        if (p->sel >= p->top + vis)
            p->top = p->sel - vis + 1;
        if (p->top > p->list.count - vis)
            p->top = p->list.count - vis;
        if (p->top < 0)
            p->top = 0;
    }
    draw_all();
    gfx_present(&g);
    sprintf(m, "%ld x %ld to %ld x %ld (%ld x %ld design units): laid out again in %lu ms, on %.60s",
            (long)ow, (long)oh, (long)g.w, (long)g.h, (long)g.w * 256 / g.k, (long)g.h * 256 / g.k,
            (unsigned long)(time_ms() - t0), cur() ? cur()->title : "the menu");
    amb_log("window: resized", m);
}

/* A double-click on the window's title bar: full screen. Intuition keeps
 * the clicks on its drag bar to itself (they drag the window), so the left
 * button is watched (the mouse port's, through lowlevel.library or CIA-A)
 * while the window is active and the pointer over its title bar, clear of
 * the gadgets at its ends; two presses Intuition counts as a double-click
 * there. */
static void toggle_window(void);

/* The pointer over the active window's title bar (the clock ticks faster
 * then, for a double-click's presses). */
static BOOL window_title_hover(void)
{
    struct Window *w = g.win;
    WORD y;
    if (!g.wb || !w || gfx_fs_on() || !(w->Flags & WFLG_WINDOWACTIVE))
        return FALSE;
    y = w->WScreen->MouseY - w->TopEdge;
    return y >= 0 && y < w->BorderTop && w->WScreen->MouseX >= w->LeftEdge &&
           w->WScreen->MouseX < w->LeftEdge + w->Width;
}

static void window_title_watch(void)
{
    static BOOL down;
    static ULONG s0, m0;
    static BOOL first;
    struct Window *w = g.win;
    struct Screen *sc;
    WORD x, y, gad;
    BOOL lmb;
    if (!g.wb || !w || gfx_fs_on() || !(w->Flags & WFLG_WINDOWACTIVE)) {
        down = first = FALSE;
        return;
    }
    lmb = LowLevelBase ? (ReadJoyPort(0) & JPF_BUTTON_RED) != 0
                       : !(*(volatile UBYTE *)0xBFE001 & 0x40);
    if (lmb == down)
        return;
    down = lmb;
    if (!lmb)
        return;                             /* (its press counts) */
    sc = w->WScreen;
    x = sc->MouseX - w->LeftEdge;
    y = sc->MouseY - w->TopEdge;
    gad = w->BorderTop + 4;                 /* about a title bar gadget's width */
    if (y < 0 || y >= w->BorderTop || x < gad || x >= w->Width - 2 * gad) {
        first = FALSE;                      /* not the bar: a double-click starts again */
        return;
    }
    {
        ULONG s1, m1;
        CurrentTime(&s1, &m1);
        if (first && DoubleClick(s0, m0, s1, m1)) {
            first = FALSE;
            down = FALSE;
            amb_log("window", "title bar double-clicked: full screen");
            toggle_window();
            return;
        }
        first = TRUE;
        s0 = s1;
        m0 = m1;
    }
}

/* ---- Iconified: the window closed (its buffer kept and drawn into
 * unseen, gfx_window_hide), or full screen, the screen put behind
 * Workbench's; an AppIcon on Workbench (iconify.c) brings it back, as does
 * the screen brought to the front. The music plays on: the main loop's
 * tick does only what keeps it going (iconified_tick). A video stops and
 * the viewer closes first (their loops end on gfx_iconify_asked). */

/* Workbench's own font has no Japanese, Chinese or Korean letters: its
 * labels (menus, the icon's) in English then. */
static BOOL wb_text_english(void)
{
    const char *iso = amb_lang_iso();
    return iso && (!strncmp(iso, "ja", 2) || !strncmp(iso, "zh", 2) || !strncmp(iso, "ko", 2));
}

static void mini_drop_window(void);

static void iconify(void)
{
    struct Screen *wb;
    if (iconified || gfx_fs_on())
        return;
    if (!(wb = LockPubScreen((CONST_STRPTR)"Workbench"))) {
        set_msg(_("Iconify needs Workbench"));
        return;
    }
    UnlockPubScreen(NULL, wb);
    if (!icon_add("Ambiance", wb_text_english() ? "Quit Ambiance" : _("Quit Ambiance"))) {
        set_msg(_("Iconify needs Workbench"));
        return;
    }
    hold_stop();
    ov.on = FALSE;
    if (mini_on) {
        mini_drop_window();             /* the mini player's window, back on the icon */
        amb_log("iconify", "the mini player closed, an AppIcon on Workbench: the music plays on");
    } else if (g.wb) {
        if (!gfx_window_hide(&g)) {
            icon_remove();
            return;
        }
        ptr_hidden = NULL;              /* (a new window comes with its pointer) */
        amb_log("iconify", "the window closed, an AppIcon on Workbench: the music plays on");
    } else {
        ScreenToBack(g.scr);
        iconic_screen = TRUE;
        amb_log("iconify", "the screen behind Workbench's, an AppIcon on it: the music plays on");
    }
    iconified = TRUE;
}

/* The window would not open again (Workbench changed meanwhile): the
 * display made anew, every page kept. */
static void display_reopen(void)
{
    WORD ow = g.w, oh = g.h;
    ULONG t0 = time_ms();
    layout_let_go();
    pv_flush();
    spin_free();
    ov_bm_free();
    vis_stop();
    ptr_hidden = NULL;
    screen_close();
    if (!gfx_open(&g, &caps, screen_mode, ui_style) && !gfx_open(&g, &caps, SM_AUTO, ui_style)) {
        running = FALSE;
        return;
    }
    wb_note_fallback();
    window_relaid(ow, oh, t0);
}

static BOOL mini_window(void);

static void uniconify(const char *why)
{
    if (!iconified)
        return;
    icon_remove();
    iconified = FALSE;
    amb_log("iconify", why);
    if (mini_on) {
        if (mini_window())
            return;                     /* the mini player again */
        mini_on = FALSE;                /* (would not open: the window instead) */
    }
    if (iconic_screen) {
        iconic_screen = FALSE;
        if (g.scr && !g.wb) {
            ScreenToFront(g.scr);
            if (g.win)
                ActivateWindow(g.win);
        }
    } else if (g.wb && !gfx_window_unhide(&g)) {
        display_reopen();
        return;
    }
    mouse_last = time_ms();
    draw_all();
    gfx_present(&g);
}

/* The main loop's tick while iconified: the music kept going (the next
 * track, a stream's title), nothing drawn but what that draws. */
static void iconified_tick(void)
{
    static ULONG next;
    np_tick();
    mp_paula_check();
    if (!next || (LONG)(time_ms() - next) >= 0) {
        next = time_ms() + 1000;
        mp_tempo_check();
        flac_slow_check();
        nav_tick_1s();
    }
    if (iconic_screen && g.scr && IntuitionBase->FirstScreen == g.scr)
        uniconify("its screen brought to the front: no longer iconified");
}


/* ---- The mini player: Now playing, small, in a window of its own, the
 * window away meanwhile (its buffer kept and drawn into unseen, as when
 * iconified). The window's zoom gadget opens it; its own zoom gadget, a
 * click on its cover, Esc or Return bring the window back. In the pixel
 * font at 1x, 2x or 3x by Workbench's height (gfx_mini_size), whatever
 * the window's style: the cover, what plays, the count in its list, the
 * time and its bar, the buttons, mute, the volume, a meter. Music only: a
 * video stops and the viewer closes before (as when iconified). */
enum { MB_PREV, MB_PLAY, MB_STOP, MB_NEXT, MB_MUTE, MB_SLIDER, MB_BAR, MB_ART };
static void window_menu(UWORD code);
static struct {
    WORD k;                                 /* pixels a unit: 1-3 */
    WORD art_x, art_y, art_s, tx, tw, r1, r2, r3, ty, tw_time, bar_x0, bar_x1, bar_y, bar_h;
    WORD by, bh, bw, bx[4], mute_x, mute_w, sl_x0, sl_x1, vu_x0, vu_x1;
    int hov, press, s_hov, s_press, s_state, s_vol, s_muted, s_fill, s_n;
    ULONG press_until;
    LONG s_sec, s_total;
    BOOL sl_drag, drawn;
    char s_l1[48], s_l2[160], s_l3[200];
    struct LJob *aj;                        /* its cover, made for its square */
    char art_file[120];
    BOOL art_drawn;
    UBYTE s_lv[4];
} mn = { .hov = -1, .press = -1 };

#define MK(v) ((WORD)((v) * mn.k))

static void mini_layout(void)
{
    WORD fh = mg.font_h, i;
    mn.k = mg.kint ? mg.kint : 1;
    mn.art_s = MK(88);
    mn.art_x = MK(6);
    mn.art_y = (mg.h - mn.art_s) / 2;
    mn.tx = mn.art_x + mn.art_s + MK(10);
    mn.tw = mg.w - MK(8) - mn.tx;
    mn.r1 = MK(5);
    mn.r2 = mn.r1 + fh + MK(2);
    mn.r3 = mn.r2 + fh + MK(1);
    mn.ty = mn.r3 + fh + MK(5);
    mn.tw_time = TextLength(mg.rp, (CONST_STRPTR) "88:88", 5);
    mn.bar_x0 = mn.tx + mn.tw_time + MK(5);
    mn.bar_x1 = mn.tx + mn.tw - mn.tw_time - MK(5) - 1;
    mn.bar_h = MK(3);
    mn.bar_y = mn.ty + (fh - mn.bar_h) / 2;
    mn.bh = MK(18);
    mn.by = mg.h - MK(5) - mn.bh;
    mn.bw = MK(22);
    for (i = 0; i < 4; i++)
        mn.bx[i] = mn.tx + i * (mn.bw + MK(3));
    mn.mute_x = mn.bx[3] + mn.bw + MK(8);
    mn.mute_w = MK(16);
    mn.sl_x0 = mn.mute_x + mn.mute_w + MK(4);
    mn.sl_x1 = mn.sl_x0 + MK(56);
    mn.vu_x0 = mn.sl_x1 + MK(10);
    mn.vu_x1 = mn.tx + mn.tw - 1;
}

/* s at x, y (the line's top), in maxw at most ("..." where cut short). */
static void mini_text(WORD x, WORD y, WORD maxw, const char *s, UBYTE pen)
{
    ULONG n = strlen(s), f;
    struct TextExtent te;
    if (maxw <= 0 || !n)
        return;
    if (cjk_any(s, n)) {
        cjk_text(&mg, mg.rp, x, y, s, n, pen, PEN_BG, FALSE, 0, maxw);
        return;
    }
    f = TextFit(mg.rp, (CONST_STRPTR)s, n, &te, NULL, 1, maxw, mg.font_h + 1);
    if (f < n) {
        WORD dw = TextLength(mg.rp, (CONST_STRPTR) "...", 3);
        f = maxw > dw ? TextFit(mg.rp, (CONST_STRPTR)s, n, &te, NULL, 1, maxw - dw, mg.font_h + 1)
                      : 0;
    }
    SetAPen(mg.rp, GPEN(pen));
    SetDrMd(mg.rp, JAM1);
    Move(mg.rp, x, y + mg.baseline);
    Text(mg.rp, (CONST_STRPTR)s, f);
    if (f < n)
        Text(mg.rp, (CONST_STRPTR) "...", 3);
}

/* What it shows: the count in its list, the title, the line under it. */
static void mini_lines(char *l1, char *l2, char *l3)
{
    l1[0] = l2[0] = l3[0] = 0;
    if (np.list.pool && np.idx >= 0 && np.list.count > 1 && mp_loaded())
        sprintf(l1, mp_is_track() ? _("Track %ld of %ld") : _("%ld of %ld"), (long)np.idx + 1,
                (long)np.list.count);
    if (!mp_loaded()) {
        strcpy(l2, _("Nothing playing"));
        return;
    }
    if (mp_is_stream()) {
        const char *by = mp_track_by(), *p;
        char *o = l3;
        amb_strlcpy(l2, mp_stream_name()[0] ? mp_stream_name()
                                              : mp_is_track() ? _("(no title)") : _("Radio"), 160);
        if (!mp_is_track()) {
            /* a station: what is on air, or how it is */
            const char *now = st_title();
            int stt = mp_running() ? st_state() : -1;
            if (!mp_running())
                now = _("Stopped");
            else if (stt == ST_CONNECTING)
                now = _("Connecting...");
            else if (stt == ST_BUFFERING)
                now = _("Buffering...");
            amb_strlcpy(l3, now, 200);
            return;
        }
        /* a track: its artist and album ("   " between) as "artist - album" */
        for (p = by; *p && o < l3 + 196;) {
            if (p[0] == ' ' && p[1] == ' ' && p[2] == ' ') {
                strcpy(o, " - ");
                o += 3;
                p += 3;
            } else {
                *o++ = *p++;
            }
        }
        *o = 0;
        return;
    }
    {
        /* a module: its title (or its file's), what it is, by whom */
        const char *t = mp_title();
        char label[120];
        while (*t == ' ')
            t++;
        mods_label(FilePart((STRPTR)mp_path()), label, sizeof(label));
        amb_strlcpy(l2, t[0] ? t : label, 160);
        amb_strlcpy(l3, mp_tag(), 200);
        if (mp_author()[0]) {
            char part[120];
            sprintf(part, _("   by %s"), mp_author());
            amb_strlcat(l3, part, 200);
        }
    }
}

static void mini_button(int i)
{
    BOOL lit = i == mn.press, hov = i == mn.hov;
    UBYTE bg = lit ? PEN_HL : hov ? PEN_SELDIM : PEN_BAR, fg = lit || hov ? PEN_WHITE : PEN_TEXT;
    WORD x = mn.bx[i], y = mn.by, w = mn.bw, h = mn.bh;
    static const UBYTE kind[] = { GL_PREV, GL_PLAY, GL_STOP, GL_NEXT };
    gfx_fill(&mg, x, y, x + w - 1, y + h - 1, PEN_BG);
    gfx_rrect(&mg, x, y, w, h, MK(2), MK(2), bg);
    if (i >= MB_PREV && i <= MB_NEXT)
        glyph_draw(mg.rp, i == MB_PLAY && mp_playing() ? GL_PAUSE : kind[i - MB_PREV], x + w / 2,
                   y + h / 2, (h / 2) | 1, glyph_sx(mg.scr), GPEN(fg), GPEN(fg), 0);
}

/* Mute (the speaker) and the volume slider. */
static void mini_volume(void)
{
    BOOL muted = np_muted(), hot = mn.sl_drag || mn.hov == MB_SLIDER;
    WORD gh = (mn.bh / 2) | 1, cy = mn.by + mn.bh / 2, kw = MK(3), th = MK(2), kh = mn.bh * 6 / 10;
    WORD span = mn.sl_x1 - mn.sl_x0 + 1 - kw;
    WORD kx = mn.sl_x0 + (WORD)((LONG)span * (muted ? vol_unmuted : mp_volume()) / 64);
    UBYTE sp = muted ? PEN_ACCENT : mn.hov == MB_MUTE ? PEN_WHITE : PEN_TEXT;
    gfx_fill(&mg, mn.mute_x, mn.by, mn.sl_x1, mn.by + mn.bh - 1, PEN_BG);
    glyph_draw(mg.rp, GL_MUTE, mn.mute_x + mn.mute_w / 2, cy, gh, glyph_sx(mg.scr), GPEN(sp), GPEN(sp),
               muted ? GLF_ON : 0);
    if (kx > mn.sl_x0)
        gfx_fill(&mg, mn.sl_x0, cy - th / 2, kx - 1, cy - th / 2 + th - 1,
                 muted ? PEN_RULE : hot ? PEN_TEXT : PEN_DIM);
    if (kx + kw <= mn.sl_x1)
        gfx_fill(&mg, kx + kw, cy - th / 2, mn.sl_x1, cy - th / 2 + th - 1, PEN_RULE);
    gfx_fill(&mg, kx, cy - kh / 2, kx + kw - 1, cy - kh / 2 + kh - 1,
             muted ? PEN_DIM : hot ? PEN_WHITE : PEN_TEXT);
}

/* The meter: a thin bar a channel (Paula's four, a stream's two, or one),
 * across, stacked, each filling from the left over a dim track. */
static void mini_meter(const struct MpStatus *st, BOOL full)
{
    int n = mp_loaded() && mp_running() ? mp_meter_count() : 0, i;
    WORD gap = MK(2), h, y, len = mn.vu_x1 - mn.vu_x0 + 1;
    BOOL same = !full && n == mn.s_n;
    if (n > 4)
        n = 4;
    for (i = 0; same && i < n; i++)
        if (st->level[i] != mn.s_lv[i])
            same = FALSE;
    if (same)
        return;
    gfx_fill(&mg, mn.vu_x0, mn.by, mn.vu_x1, mn.by + mn.bh - 1, PEN_BG);
    mn.s_n = n;
    mn.drawn = TRUE;
    if (!n)
        return;
    h = (mn.bh - (n - 1) * gap) / n;
    if (h > MK(3))
        h = MK(3);                          /* thin, whatever their number */
    y = mn.by + (mn.bh - (n * h + (n - 1) * gap)) / 2;
    for (i = 0; i < n; i++, y += h + gap) {
        WORD w = (WORD)((LONG)len * st->level[i] / 64);
        mn.s_lv[i] = st->level[i];
        if (w > 0)
            gfx_fill(&mg, mn.vu_x0, y, mn.vu_x0 + w - 1, y + h - 1, PEN_HL);
        if (w < len)
            gfx_fill(&mg, mn.vu_x0 + w, y, mn.vu_x1, y + h - 1, PEN_RULE);
    }
}

static void mini_art_free(void)
{
    struct LJob *j = mn.aj;
    mn.aj = NULL;
    mn.art_file[0] = 0;
    mn.art_drawn = FALSE;
    if (!j)
        return;
    if (j->busy)
        ldr_wait(j);
    pic_dispose(j->obj);
    thumb_free(j->thumb);
    FreeVec(j);
}

/* No cover: two quavers in its square. */
static void mini_art_none(void)
{
    WORD s = mn.art_s, x = mn.art_x, y = mn.art_y, u = s / 14 > 1 ? s / 14 : 1, c;
    gfx_fill(&mg, x, y, x + s - 1, y + s - 1, PEN_BG);
    gfx_rrect(&mg, x, y, s, s, MK(4), MK(4), PEN_SELDIM);
    x += s / 2 - 7 * u;
    y += s / 2 - 7 * u;
    gfx_dot(&mg, x + u * 9 / 2, y + u * 21 / 2, u * 3 / 2 + 1, PEN_DIM, PEN_SELDIM);
    gfx_dot(&mg, x + u * 19 / 2, y + u * 19 / 2, u * 3 / 2 + 1, PEN_DIM, PEN_SELDIM);
    gfx_fill(&mg, x + u * 11 / 2, y + u * 4, x + u * 11 / 2 + u - 1, y + u * 21 / 2, PEN_DIM);
    gfx_fill(&mg, x + u * 21 / 2, y + u * 3, x + u * 21 / 2 + u - 1, y + u * 19 / 2, PEN_DIM);
    for (c = 0; c <= u * 6; c++) {          /* the beam, rising to the right */
        WORD yt = y + u * 3 + (WORD)((LONG)(u * 6 - c) * u / (u * 6));
        gfx_fill(&mg, x + u * 11 / 2 + c, yt, x + u * 11 / 2 + c, yt + u * 3 / 2, PEN_DIM);
    }
}

/* The cover: Now playing's file, made for this square by the loader. */
static void mini_art(BOOL full)
{
    const char *f = mp_loaded() && mp_is_track() ? np_art_file : "";
    if (strcmp(f, mn.art_file)) {
        BPTR l = 0;
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR old = me->pr_WindowPtr;
        if (f[0]) {
            me->pr_WindowPtr = (APTR)-1;
            l = Lock((CONST_STRPTR)f, SHARED_LOCK);
            me->pr_WindowPtr = old;
        }
        if (f[0] && !l) {
            if (!mn.art_drawn || full) {    /* (not here yet: a server's, coming) */
                mini_art_none();
                mn.art_drawn = TRUE;
                mn.drawn = TRUE;
            }
            return;
        }
        if (l)
            UnLock(l);
        mini_art_free();
        amb_strlcpy(mn.art_file, f, sizeof(mn.art_file));
        /* in the Workbench's own terms: its own colours, uncut, on a
         * true-colour one (a window needs one: 15 bits or more) */
        if (f[0] && mg.truecolor && gfx_thumbs(&g) &&
            (mn.aj = AllocVec(sizeof(*mn.aj), MEMF_ANY | MEMF_CLEAR))) {
            struct LJob *j = mn.aj;
            j->type = LJ_SHOT;
            j->mode = SHOT_FILE;
            amb_strlcpy(j->path, f, sizeof(j->path));
            j->scr = g.scr;
            j->tw = j->th = mn.art_s;
            j->maxcol = THUMB_DEEP;
            ldr_submit_first(j);
        }
        full = TRUE;
    }
    if (full)
        mn.art_drawn = FALSE;
    if (mn.art_drawn || (mn.aj && !mn.aj->done))
        return;
    if (mn.aj && mn.aj->thumb) {
        struct Thumb *t = mn.aj->thumb;
        gfx_fill(&mg, mn.art_x, mn.art_y, mn.art_x + mn.art_s - 1, mn.art_y + mn.art_s - 1, PEN_BG);
        gfx_mini_thumb(&mg, t, mn.art_x + (mn.art_s - t->w) / 2, mn.art_y + (mn.art_s - t->h) / 2);
    } else {
        mini_art_none();
    }
    mn.art_drawn = TRUE;
    mn.drawn = TRUE;
}

/* Drawn (full: everything; else what changed), and shown. */
static void mini_draw(BOOL full)
{
    struct MpStatus st;
    char l1[48], l2[160], l3[200], a[16], b[16];
    ULONG at;
    LONG fill = 0, bw = mn.bar_x1 - mn.bar_x0 + 1;
    int state = np_state(), i;
    WORD right = mn.tx + mn.tw;
    if (!mini_on || !mg.rp)
        return;
    mp_status(&st);
    mn.drawn = FALSE;
    if (full) {
        gfx_fill(&mg, 0, 0, mg.w - 1, mg.h - 1, PEN_BG);
        mn.drawn = TRUE;
    }
    mini_art(full);
    mini_lines(l1, l2, l3);
    if (full || strcmp(l1, mn.s_l1)) {
        WORD w1 = l1[0] ? TextLength(mg.rp, (CONST_STRPTR)l1, strlen(l1)) : 0;
        gfx_fill(&mg, mn.tx, mn.r1, right - 1, mn.r1 + mg.font_h - 1, PEN_BG);
        mini_text(mn.tx, mn.r1, mn.tw - w1 - MK(6), _("Now playing"), PEN_ACCENT);
        if (l1[0])
            mini_text(right - w1, mn.r1, w1, l1, PEN_DIM);
        strcpy(mn.s_l1, l1);
        mn.drawn = TRUE;
    }
    if (full || strcmp(l2, mn.s_l2)) {
        gfx_fill(&mg, mn.tx, mn.r2, right - 1, mn.r2 + mg.font_h - 1, PEN_BG);
        mini_text(mn.tx, mn.r2, mn.tw, l2, mp_loaded() ? PEN_WHITE : PEN_DIM);
        strcpy(mn.s_l2, l2);
        mn.drawn = TRUE;
    }
    if (full || strcmp(l3, mn.s_l3)) {
        gfx_fill(&mg, mn.tx, mn.r3, right - 1, mn.r3 + mg.font_h - 1, PEN_BG);
        mini_text(mn.tx, mn.r3, mn.tw, l3, PEN_DIM);
        amb_strlcpy(mn.s_l3, l3, sizeof(mn.s_l3));
        mn.drawn = TRUE;
    }
    /* the time, and its bar */
    at = np.seek_due ? (ULONG)np.seek_to : st.elapsed_ms;
    if (st.total_ms && bw > 0) {
        fill = (LONG)((ULONG)bw * (at / 100) / ((st.total_ms + 99) / 100));
        if (fill > bw)
            fill = bw;
    }
    if (full || (LONG)(at / 1000) != mn.s_sec || (LONG)st.total_ms != mn.s_total || fill != mn.s_fill) {
        gfx_fill(&mg, mn.tx, mn.ty, right - 1, mn.ty + mg.font_h - 1, PEN_BG);
        if (mp_loaded()) {
            amb_time_text(a, at);
            mini_text(mn.tx, mn.ty, mn.tw_time + MK(4), a, PEN_TEXT);
            if (st.total_ms) {
                amb_time_text(b, st.total_ms);
                mini_text(right - TextLength(mg.rp, (CONST_STRPTR)b, strlen(b)), mn.ty,
                          mn.tw_time + MK(4), b, PEN_DIM);
            }
            if (fill > 0)
                gfx_fill(&mg, mn.bar_x0, mn.bar_y, mn.bar_x0 + fill - 1, mn.bar_y + mn.bar_h - 1,
                         PEN_ACCENT);
            if (fill < bw)
                gfx_fill(&mg, mn.bar_x0 + fill, mn.bar_y, mn.bar_x1, mn.bar_y + mn.bar_h - 1,
                         PEN_RULE);
        }
        mn.s_sec = (LONG)(at / 1000);
        mn.s_total = (LONG)st.total_ms;
        mn.s_fill = fill;
        mn.drawn = TRUE;
    }
    if (mn.press >= 0 && (LONG)(time_ms() - mn.press_until) >= 0)
        mn.press = -1;                      /* its moment is over */
    for (i = 0; i < 4; i++)
        if (full || (i == MB_PLAY && state != mn.s_state) ||
            ((i == mn.hov || i == mn.s_hov) && mn.hov != mn.s_hov) ||
            ((i == mn.press || i == mn.s_press) && mn.press != mn.s_press)) {
            mini_button(i);
            mn.drawn = TRUE;
        }
    if (full || mp_volume() != mn.s_vol || np_muted() != mn.s_muted ||
        ((mn.hov >= MB_MUTE || mn.s_hov >= MB_MUTE) && mn.hov != mn.s_hov)) {
        mini_volume();
        mn.drawn = TRUE;
    }
    mini_meter(&st, full);
    mn.s_state = state;
    mn.s_hov = mn.hov;
    mn.s_press = mn.press;
    mn.s_vol = mp_volume();
    mn.s_muted = np_muted();
    if (mn.drawn)
        gfx_mini_show(&mg, 0, 0, mg.w - 1, mg.h - 1);
}

/* Its window (again: after iconified), drawn. */
static BOOL mini_window(void)
{
    WORD l, t, w, h;
    UWORD k;
    if (!gfx_window_box(&l, &t, &w, &h, &k))
        l = t = -1;
    if (!gfx_mini_open(&g, &mg, l, t))
        return FALSE;
    mini_layout();
    mn.hov = mn.press = -1;
    mn.sl_drag = FALSE;
    mini_draw(TRUE);
    return TRUE;
}

/* The window's zoom gadget: the window away, the mini player up. */
static void mini_enter(void)
{
    if (mini_on || iconified || !g.wb || !g.win || gfx_fs_on())
        return;
    hold_stop();
    ov.on = FALSE;
    if (!gfx_window_hide(&g))
        return;
    ptr_hidden = NULL;
    mini_on = TRUE;
    if (!mini_window()) {
        mini_on = FALSE;
        if (!gfx_window_unhide(&g)) {
            display_reopen();
            return;
        }
        draw_all();
        gfx_present(&g);
        return;
    }
    amb_log("mini player", "the window's zoom gadget: the mini player");
}

/* Its window closed (iconified, or quitting): mini_on stays as it was. */
static void mini_drop_window(void)
{
    mini_art_free();
    if (mg.win || mg.rp)
        gfx_mini_close(&g, &mg);
}

/* ...and the mini player left for good (Workbench closing): the window
 * comes back with it. */
static void mini_drop(void)
{
    mini_drop_window();
    mini_on = FALSE;
}

/* Back to the window, where the mini player was. */
static void mini_leave(const char *why)
{
    if (!mini_on)
        return;
    mini_drop();
    amb_log("mini player", why);
    if (!gfx_window_unhide(&g)) {
        display_reopen();
        return;
    }
    mouse_last = time_ms();
    draw_all();
    gfx_present(&g);
}

static int mini_hit(WORD x, WORD y)
{
    int i;
    if (x >= mn.art_x && x < mn.art_x + mn.art_s && y >= mn.art_y && y < mn.art_y + mn.art_s)
        return MB_ART;
    if (y >= mn.by && y < mn.by + mn.bh) {
        for (i = 0; i < 4; i++)
            if (x >= mn.bx[i] && x < mn.bx[i] + mn.bw)
                return i;
        if (x >= mn.mute_x && x < mn.mute_x + mn.mute_w)
            return MB_MUTE;
        if (x >= mn.sl_x0 - MK(2) && x <= mn.sl_x1 + MK(2))
            return MB_SLIDER;
    }
    if (y >= mn.ty - MK(2) && y < mn.ty + mg.font_h + MK(2) && x >= mn.bar_x0 && x <= mn.bar_x1)
        return MB_BAR;
    return -1;
}

static void mini_slide(WORD x)
{
    WORD kw = MK(3), span = mn.sl_x1 - mn.sl_x0 + 1 - kw;
    LONG v = span > 0 ? ((LONG)(x - mn.sl_x0 - kw / 2) * 64 + span / 2) / span : mp_volume();
    np_set_volume(v < 0 ? 0 : v > 64 ? 64 : (int)v);
}

static void mini_press(int b)
{
    static const int nb[] = { NB_PREV, NB_PLAY, NB_STOP, NB_NEXT };
    mn.press = b;
    mn.press_until = time_ms() + FLASH_MS;
    if (b >= MB_PREV && b <= MB_NEXT)
        np_press(nb[b]);
    else if (b == MB_MUTE)
        np_mute(!np_muted());
}

static void mini_mouse(const struct IntuiMessage *im)
{
    WORD x = im->MouseX - mg.win->BorderLeft, y = im->MouseY - mg.win->BorderTop;
    if (im->Class == IDCMP_MOUSEMOVE) {
        if (mn.sl_drag)
            mini_slide(x);
        else
            mn.hov = mini_hit(x, y);
    } else if (im->Code == SELECTDOWN) {
        int b = mini_hit(x, y);
        if (b == MB_ART) {
            mini_leave("its cover clicked: the window again");
            return;
        }
        if (b == MB_SLIDER) {
            mn.sl_drag = TRUE;
            mini_slide(x);
        } else if (b == MB_BAR) {
            np_seek_part((ULONG)(x - mn.bar_x0), (ULONG)(mn.bar_x1 - mn.bar_x0 + 1));
        } else if (b >= 0) {
            mini_press(b);
        }
    } else if (im->Code == SELECTUP) {
        mn.sl_drag = FALSE;
    }
    mini_draw(FALSE);
}

static void mini_key(const struct IntuiMessage *im)
{
    UWORD c = im->Code;
    BOOL cmd = (im->Qualifier & (IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND)) != 0;
    if (im->Class == IDCMP_VANILLAKEY) {
        if (cmd && (c == 'i' || c == 'I')) {
            gfx_iconify_asked = TRUE;
            return;
        }
        if (cmd && (c == 'f' || c == 'F')) {
            mini_leave("Amiga-F: the window, then full screen");
            if (running && g.wb)
                toggle_window();
            return;
        }
        if (cmd && (c == 'q' || c == 'Q')) {
            running = FALSE;
            return;
        }
        switch (c) {
        case ' ': mini_press(MB_PLAY); break;
        case 's': case 'S': mini_press(MB_STOP); break;
        case 'n': case 'N': case '>': mini_press(MB_NEXT); break;
        case 'p': case 'P': case '<': mini_press(MB_PREV); break;
        case 'm': case 'M': mini_press(MB_MUTE); break;
        case '+': case '=': np_volume(VOL_STEP); break;
        case '-': case '_': np_volume(-VOL_STEP); break;
        case 13: case 27:
            mini_leave(c == 27 ? "Esc: the window again" : "Return: the window again");
            return;
        }
    } else if (!(c & IECODE_UP_PREFIX)) {
        switch (c) {
        case 0x4C: np_volume(VOL_STEP); break;          /* up: louder */
        case 0x4D: np_volume(-VOL_STEP); break;
        case 0x4F: case 0x4E: np_press(c == 0x4E ? NB_FWD : NB_BACK); break;
        }
    }
    mini_draw(FALSE);
}

/* Its window's messages. */
static void mini_input(void)
{
    struct IntuiMessage *im;
    while (mini_on && mg.win && (im = (struct IntuiMessage *)GetMsg(mg.win->UserPort))) {
        struct IntuiMessage c = *im;
        ReplyMsg((struct Message *)im);
        switch (c.Class) {
        case IDCMP_CLOSEWINDOW:
            if (c.Code == 1)
                gfx_iconify_asked = TRUE;   /* (OS 3.2's iconify gadget) */
            else
                running = FALSE;
            return;
        case IDCMP_REFRESHWINDOW:
            gfx_mini_refresh(&mg);
            break;
        case IDCMP_NEWSIZE:
            if (mg.win->Flags & WFLG_ZOOMED) {
                mini_leave("its zoom gadget: the window again");
                return;
            }
            break;
        case IDCMP_MENUPICK:
            window_menu(c.Code);
            if (!mini_on)
                return;
            break;
        case IDCMP_MOUSEBUTTONS:
        case IDCMP_MOUSEMOVE:
            mini_mouse(&c);
            break;
        case IDCMP_RAWKEY:
        case IDCMP_VANILLAKEY:
            mini_key(&c);
            break;
        }
        if (!running)
            return;
    }
}

/* The main loop's tick: the music kept going, the mini player brought up
 * to date. */
static void mini_tick(void)
{
    iconified_tick();
    mini_draw(FALSE);
}

/* The window's Workbench menus. */
static void window_menu(UWORD code)
{
    ULONG what;
    while ((what = gfx_window_menu_pick(&code))) {
        int i, mod = -1;
        if (what == WM_QUIT) {
            running = FALSE;
            return;
        }
        if (mini_on && what != WM_ICONIFY) {
            /* (its menus go with it: this one item, then back) */
            mini_leave("a menu item: the window again");
            if (!running || mini_on)
                return;
            code = MENUNULL;
        }
        if (what == WM_FULL) {
            amb_log("window", "Project > Full screen (the menu, or Right-Amiga-F)");
            toggle_window();                /* full screen, and kept */
            return;
        }
        if (what == WM_ICONIFY) {
            amb_log("iconify", "Project > Iconify (the menu, or Right-Amiga-I)");
            gfx_iconify_asked = TRUE;       /* (the main loop's, once these are read) */
            return;
        }
        mod = what == WM_ABOUT ? MOD_ABOUT : MOD_SETTINGS;
        for (i = 0; i < nmenu && menu[i] != mod; i++)
            ;
        if (i < nmenu) {
            while (depth > 1)
                page_pop();
            focus = FOCUS_MENU;
            menu_select_now(i, TRUE);
            draw_all();
        }
    }
}

/* Settings > Display > Colour depth: saved, and the screen opened again
 * at it (gfx_open: 16-bit after all if a 32-bit one will not open) */
static void apply_depth(UWORD bits)
{
    char m[60];
    if (bits == caps_rtg_bits)
        return;
    caps_rtg_bits = bits;
    save_setting(DEPTH_VAR, bits >= 24 ? "32" : "16");
    display_sel_act = ACT_DEPTHS;
    display_again(FALSE, _("Colour depth"), build_depths);
    display_sel_act = 0;
    sprintf(m, "%ld-bit asked for, %ld-bit open", (long)bits, (long)gfx_bits(&g));
    amb_log("colour depth", m);
    if (bits >= 24 && g.depth < 24)
        set_msg(_("32 bits would not open here: 16-bit instead"));
}

static void apply_tsize(int i)
{
    if (i < 0 || i >= TSIZE_COUNT || (i == gfx_text_size && gfx_window_tsize() == i))
        return;
    gfx_text_size = i;
    save_setting(TSIZE_VAR, i == TSIZE_SMALL ? "small" : i == TSIZE_LARGE ? "large"
                            : i == TSIZE_XLARGE ? "xlarge" : "medium");
    if (g.wb)
        display_again(TRUE, _("Text size"), build_tsizes);
}

/* Settings > Display > Colours: the scheme saved, and the display opened
 * again in it (a screen's palette, a window's Workbench pens, the smooth
 * style's shades: all from ui_palette32 as it opens), back on the list */
static void apply_colours(int k)
{
    char m[60];
    if (k < 0 || k >= UI_SCHEMES || k == ui_scheme)
        return;
    ui_scheme_apply(k);
    save_setting(COLOURS_VAR, ui_scheme_keys[k]);
    sprintf(m, "colours: %s", ui_scheme_names[k]);
    amb_log("window", m);
    display_colours_row = TRUE;
    display_again(FALSE, _("Colours"), build_colours);
    display_colours_row = FALSE;
}

static void toggle_jump(void)
{
    struct Page *p = cur();
    LONG sel = p ? p->sel : 0;
    jump_scroll = !jump_scroll;
    save_setting(JUMP_VAR, jump_scroll ? "1" : "0");
    (void)p;
    (void)sel;
    plex_settings_again(FALSE);
}

/* Settings > Plex: Music's Plex rows, and its asking, on or off. */
static void toggle_plex_on(void)
{
    plex_set_on(!plex.on);
    plex_settings_again(TRUE);              /* rows come and go */
}

/* Settings > Networking > Plex > Request lower quality audio: every track the server's
 * conversion, made for this Amiga (22 kHz mono on a 68030: a fraction of
 * the decoding of a 44.1 kHz stereo file), or MP3s as stored (other
 * formats are converted either way). The next track it applies to. */
static void toggle_plex_convert(void)
{
    if (plex_tier() < 2)
        return;                             /* greyed: always on here */
    plex_set_convert(!plex.convert);
    plex_settings_again(FALSE);
}

static void open_settings_page(const char *title, void (*build)(struct Page *))
{
    struct Page *p = page_push(PT_LIST, title);
    if (!p)
        return;
    build(p);
    sel_off_heading(p, 1);
    draw_pane(TRUE);
    draw_footer();
}

/* Settings > General > Language: one chosen, kept, and everything shown
 * again in it (the menu column fitted to its words). */
static void choose_language(const char *lang)
{
    char t[32];
    amb_strlcpy(lang_set, lang, sizeof(lang_set));
    save_setting(LANG_VAR, lang_set);
    amb_lang_open(lang_set);
    gfx_menu_refit(&g);
    window_menus_make();                    /* (the window's, in it too) */
    gfx_window_menus_again(&g);
    settings_rebuild();
    draw_all();
    lang_name(amb_lang_now(), t);
    set_msg(t);
}

/* Settings > Networking: an interface chosen (i < 0: the stack's choice). */
static void choose_nic(int i)
{
    if (i >= 0 && (i >= nnic || !nics[i].up || !nics[i].ip))
        return;
    amb_strlcpy(net_if, i < 0 ? "" : nics[i].name, sizeof(net_if));
    save_setting(NETIF_VAR, net_if);
    net_bind_to(i < 0 ? 0 : nics[i].ip);
    plex_settings_again(FALSE);
    set_msg(i < 0 ? _("Connections: the stack's choice of interface")
                  : _("Connections go out from this interface"));
}

static void open_sound_settings(void)
{
    struct Page *p = page_push(PT_LIST, _("Sound"));
    if (!p)
        return;
    build_sound_settings(p);
    draw_pane(TRUE);
    draw_footer();
}

static void open_plex_settings(void)
{
    struct Page *p = page_push(PT_LIST, _("Plex"));
    if (!p)
        return;
    build_plex_settings(p);
    draw_pane(TRUE);
    draw_footer();
}

/* A row saying Plex is off or has no server, chosen: Settings >
 * Networking > Plex itself, opened as if gone there by hand (the section
 * left remembered as ever, Back going up through Networking). */
static void goto_plex_settings(void)
{
    struct Page *p;
    LONG i;
    int m;
    for (m = 0; m < nmenu && menu[m] != MOD_SETTINGS; m++)
        ;
    if (m >= nmenu)
        return;
    menu_sel = m;
    build_root(MOD_SETTINGS);
    while (depth > 1)
        page_pop();                         /* (Settings as left before: its root) */
    if (!(p = cur()))
        return;
    for (i = 0; i < p->list.count; i++)
        if (p->list.items[i].kind == IT_ACTION && p->list.items[i].action == ACT_NETSET)
            p->sel = i;
    if ((p = page_push(PT_LIST, _("Networking")))) {
        build_net_settings(p);
        for (i = 0; i < p->list.count; i++)
            if (p->list.items[i].kind == IT_ACTION && p->list.items[i].action == ACT_PLEXSET)
                p->sel = i;
        if ((p = page_push(PT_LIST, _("Plex"))))
            build_plex_settings(p);
    }
    focus = FOCUS_CONTENT;
    amb_log("navigation", "to Settings > Networking > Plex (from a row about Plex)");
    draw_all();
}

/* ---- Settings > Networking > Jellyfin (jellyfin.c): the server, found
 * on the network (its UDP discovery) or typed; signing in (a user name
 * and password typed, sent once: the token kept, never the password);
 * signing out; the films' size. Its jobs (a search, a sign-in, a
 * sign-out, a server's own name asked) come back to jf_job_came. */
static struct NetJob *jf_nj;            /* the one out */
static char jf_found[8][3][80];         /* found: address, id, name */
static int jf_nfound = -1;              /* -1 not looked yet */
static char jf_body[448];          /* (two 199-byte JSON strings at most) */               /* a sign-in's body, while it is out */
static char jf_user_asked[64];

static void build_jf_settings(struct Page *p)
{
    struct Item *it;
    char tag[96];
    int i;
    BOOL listed = FALSE;
    p->setp = SETP_JF;
    action_item(p, "Jellyfin", _("a media server (jellyfin.org)"), ACT_JFON,
                jf.on ? MARK_CHECK_ON : MARK_CHECK_OFF);
    if (!jf.on)
        return;
    heading_item(p, _("Server"));
    for (i = 0; i < jf_nfound && i < 8; i++) {
        char a[80];
        BOOL here;
        amb_strlcpy(a, jf_found[i][0], sizeof(a));
        here = jf.host[0] && strstr(a, jf.host) != NULL;
        listed |= here;
        action_item(p, jf_found[i][2][0] ? jf_found[i][2] : a, a, ACT_JFSRV0 + i,
                    here ? MARK_RADIO_ON : MARK_RADIO_OFF);
    }
    if (jf.host[0] && !listed) {
        sprintf(tag, "%.60s:%lu", jf.host, (unsigned long)jf.port);
        if ((it = action_item(p, jf.name[0] ? jf.name : jf.host, tag, ACT_JFADDR, MARK_RADIO_ON)))
            (void)it;
    }
    action_item(p, _("Look for servers on this network"),
                jf_nj && jf_nj->jfdisc ? _("looking...") : jf_nfound == 0 ? _("none answered") : "",
                ACT_JFFIND, MARK_NONE);
    action_item(p, _("Type a server's address"), jf.host[0] ? "" : "http://host:8096", ACT_JFADDR,
                MARK_NONE);
    heading_item(p, _("User"));
    if (jf.token[0]) {
        sprintf(tag, "%.60s", jf.user[0] ? jf.user : _("signed in"));
        action_item(p, _("Sign out"), tag, ACT_JFOUT, MARK_NONE);
    } else if ((it = action_item(p, _("Sign in"), !jf.host[0] ? _("choose a server first")
                                                  : jf_nj && jf_nj->jf == JFJ_SIGNIN
                                                  ? _("signing in...") : "",
                                 ACT_JFSIGNIN, MARK_ARROW)) && !jf.host[0]) {
        it->flags |= ITEM_DISABLED;
    }
    heading_item(p, _("Films"));
    action_item(p, _("Picture size"),
                jf.vsize == 1 ? _("small: 320 wide, MPEG-1 (the CPU decodes)")
                : jf.vsize == 2 ? _("large: up to 720 x 576, MPEG-2 (the ZZ9000 decodes)")
                                : _("automatic: by what decodes it"),
                ACT_JFVSIZE, MARK_CYCLE);
}

static void goto_jf_settings(void)
{
    struct Page *p;
    LONG i;
    int m;
    for (m = 0; m < nmenu && menu[m] != MOD_SETTINGS; m++)
        ;
    if (m >= nmenu)
        return;
    menu_sel = m;
    build_root(MOD_SETTINGS);
    while (depth > 1)
        page_pop();
    if (!(p = cur()))
        return;
    for (i = 0; i < p->list.count; i++)
        if (p->list.items[i].kind == IT_ACTION && p->list.items[i].action == ACT_NETSET)
            p->sel = i;
    if ((p = page_push(PT_LIST, _("Networking")))) {
        build_net_settings(p);
        for (i = 0; i < p->list.count; i++)
            if (p->list.items[i].kind == IT_ACTION && p->list.items[i].action == ACT_JFSET)
                p->sel = i;
        if ((p = page_push(PT_LIST, "Jellyfin"))) {
            build_jf_settings(p);
            sel_off_heading(p, 1);
        }
    }
    focus = FOCUS_CONTENT;
    amb_log("navigation", "to Settings > Networking > Jellyfin (from a row about Jellyfin)");
    draw_all();
}

/* Music's, Pictures' and Videos' Jellyfin row (kind 'm', 'p', 'v'), at
 * the end of p's rows, or (first) at their top: none unless a server is
 * chosen (Jellyfin is no clutter where it is not used). */
static BOOL jf_section_row(struct Page *p, char kind, BOOL first)
{
    char label[128], key[8];
    struct Item *it;
    if (!jf_shown())
        return FALSE;
    if (!jf.token[0]) {
        sprintf(label, _("Jellyfin: sign in to %.60s (Settings > Networking > Jellyfin)"),
                jf.name[0] ? jf.name : jf.host);
        it = first ? list_add_first(&p->list, label, "", "", IT_ACTION)
                   : list_add(&p->list, label, "", "", IT_ACTION);
        if (it) {
            it->action = ACT_GOJF;
            it->mark = MARK_ARROW;
        }
        return it != NULL;
    }
    sprintf(label, kind == 'p' ? _("Jellyfin photos on %.80s") : _("Jellyfin: %.80s"),
            jf.name[0] ? jf.name : jf.host);
    key[0] = '~';
    key[1] = 'V';
    key[2] = kind;
    key[3] = 0;
    it = first ? list_add_first(&p->list, label, key, "", IT_PLEXDIR)
               : list_add(&p->list, label, key, "", IT_PLEXDIR);
    if (it)
        it->mark = MARK_ARROW;
    return it != NULL;
}

static void jf_roots_again(void)
{
    music_root_again();
    pictures_root_again();
}

static void jf_settings_again(void)
{
    plex_settings_again(FALSE);             /* (every Settings page open, made again) */
}

/* A job of Jellyfin's own out, at most one: the last given up. */
static BOOL jf_submit(struct NetJob *j, UBYTE what)
{
    if (jf_nj) {
        net_cancel(jf_nj);
        jf_nj->orphan = TRUE;
        jf_nj = NULL;
    }
    j->jf = what;
    j->seq = 0;
    jf_nj = j;
    net_submit(j);
    if (j->done && !j->busy)
        radio_done(j);
    return TRUE;
}

/* ---- A line typed on a page of its own (an address, a user name, a
 * password), drawn by the view as any list is: its first row the text
 * (a password as stars), Return or Fire keeps it, Esc or Left gives it
 * up. Letters come to entry_key from handle_key while it is the page. */
static struct {
    struct Page *p;                     /* the page, while it is open */
    char buf[128];
    LONG max;
    BOOL secret;
    void (*done)(const char *text);     /* kept: what it was for (not on Esc) */
} entry;

static void entry_rows(void)
{
    struct Page *p = entry.p;
    char shown[140];
    LONG i, n = (LONG)strlen(entry.buf);
    struct Item *it;
    if (!p)
        return;
    for (i = 0; i < n && i < (LONG)sizeof(shown) - 2; i++)
        shown[i] = entry.secret ? '*' : entry.buf[i];
    shown[i++] = '_';                   /* where the next letter goes */
    shown[i] = 0;
    list_free(&p->list);
    list_init(&p->list);
    if ((it = list_add(&p->list, shown, "", "", IT_ACTION))) {
        it->action = ACT_ENTRY;
        it->mark = MARK_NONE;
    }
    text_line(p, "");
    text_line(p, _("Return: OK   Esc: cancel"));
    p->sel = 0;
    p->top = 0;
}

static void entry_open(const char *title, const char *now, LONG max, BOOL secret,
                       void (*done)(const char *text))
{
    struct Page *p;
    if (!(p = page_push(PT_LIST, title))) {
        set_msg(_("Too many levels open"));
        return;
    }
    memset(entry.buf, 0, sizeof(entry.buf));
    amb_strlcpy(entry.buf, now ? now : "", sizeof(entry.buf));
    entry.max = max < (LONG)sizeof(entry.buf) ? max : (LONG)sizeof(entry.buf);
    entry.secret = secret;
    entry.done = done;
    entry.p = p;
    entry_rows();
    focus = FOCUS_CONTENT;
    draw_all();
}

/* The page let go (kept: done with its text; else given up), the text
 * wiped either way (a password not left in memory). */
static void entry_close(BOOL keep)
{
    char text[128];
    void (*done)(const char *) = entry.done;
    if (!entry.p)
        return;
    amb_strlcpy(text, entry.buf, sizeof(text));
    memset(entry.buf, 0, sizeof(entry.buf));
    if (cur() == entry.p) {
        entry.p = NULL;
        page_pop();
    }
    entry.p = NULL;
    entry.done = NULL;
    draw_all();
    if (keep && done)
        done(text);
    memset(text, 0, sizeof(text));
}

static BOOL entry_on(void)
{
    if (entry.p && cur() != entry.p) {
        entry.p = NULL;                 /* (left some other way: given up) */
        memset(entry.buf, 0, sizeof(entry.buf));
    }
    return entry.p != NULL;
}

/* A typed key on the entry page: TRUE taken. */
static BOOL entry_key(UWORD code)
{
    LONG n = (LONG)strlen(entry.buf);
    if (code == 13) {
        entry_close(TRUE);
        return TRUE;
    }
    if (code == 27) {
        entry_close(FALSE);
        return TRUE;
    }
    if (code == 8 || code == 127) {
        if (n > 0)
            entry.buf[n - 1] = 0;
    } else if (code == 24) {
        entry.buf[0] = 0;               /* Ctrl-X: all of it */
    } else if (code >= 32 && code != 127 && !(code >= 128 && code < 160) && n < entry.max - 1) {
        entry.buf[n] = (char)code;
        entry.buf[n + 1] = 0;
    } else {
        return TRUE;
    }
    entry_rows();
    draw_pane(TRUE);
    draw_footer();
    return TRUE;
}

static void jf_find(void)
{
    struct NetJob *j;
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        set_msg(_("Not enough memory"));
        return;
    }
    j->jfdisc = 1;
    jf_submit(j, JFJ_FIND);
    set_msg(_("Looking for Jellyfin servers..."));
    jf_settings_again();
}

/* The server's own name and id, asked of it (/System/Info/Public: no
 * sign-in needed): a typed address checked, and named. */
static void jf_ask_info(void)
{
    struct NetJob *j;
    if (!jf.host[0] || !net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return;
    amb_strlcpy(j->host, jf.host, sizeof(j->host));
    j->port = jf.port;
    strcpy(j->path, "/System/Info/Public");
    jf_submit(j, JFJ_INFO);
}

static void jf_address_typed(const char *a)
{
    if (!a[0])
        return;
    jf_set_server(a, "", "");
    if (!jf.host[0]) {
        set_msg(_("That is not an address"));
        return;
    }
    amb_log("jellyfin: server typed", jf.host);
    jf_ask_info();
    jf_settings_again();
    jf_roots_again();
}

static void jf_type_address(void)
{
    char a[96];
    if (jf.host[0])
        sprintf(a, "%.60s:%lu", jf.host, (unsigned long)jf.port);
    else
        strcpy(a, "");
    set_msg(_("The server's address (name or numbers, and :port if not 8096)"));
    entry_open(_("Type a server's address"), a, 90, FALSE, jf_address_typed);
}

static void jf_password_typed(const char *pw)
{
    struct NetJob *j;
    char m[120];
    if (!jf.host[0] || !jf_user_asked[0])
        return;
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        set_msg(_("Not enough memory"));
        return;
    }
    jf_signin_job(j, jf_user_asked, pw, jf_body, sizeof(jf_body));
    jf_submit(j, JFJ_SIGNIN);
    sprintf(m, _("Signing in to %.60s..."), jf.name[0] ? jf.name : jf.host);
    set_msg(m);
    jf_settings_again();
}

static void jf_user_typed(const char *user)
{
    if (!user[0])
        return;
    amb_strlcpy(jf_user_asked, user, sizeof(jf_user_asked));
    entry_open(_("Password"), "", 120, TRUE, jf_password_typed);
}

static void jf_sign_in(void)
{
    if (!jf.host[0])
        return;
    entry_open(_("User name"), jf.user, 60, FALSE, jf_user_typed);
}

static void jf_sign_out(void)
{
    struct NetJob *j;
    if (jf.token[0] && net_start() && (j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        jf_signout_job(j);                  /* (its header has the token: made first) */
        jf_submit(j, JFJ_SIGNOUT);
    }
    jf_signed_in("", "", "");
    amb_log("jellyfin", "signed out");
    jf_settings_again();
    jf_roots_again();
}

/* ---- A Jellyfin film (a Videos row "~Y<id>"): the server makes it
 * MPEG as it plays (jf_video_url: MPEG-1 for the CPU, MPEG-2 for the
 * ZZ9000), streamed: nothing kept on this Amiga. Its first bytes are asked
 * for first (a job of their own, JFJ_VIDEO: its size, kind and sound
 * probed from them), then it plays from the server as it comes; a seek
 * asks the server again from that time (startTimeTicks). */
#define JF_PROBE (32UL * 1024)          /* its start, probed: a sequence header, a sound frame */
static struct NetJob *jf_fj;            /* the start asked for */
static char jf_fj_title[120], jf_fj_url[600], jf_fj_seek[64];
static ULONG jf_fj_ms;

static char jf_fj_key[48];
static BOOL jf_fj_zz;                   /* the ZZ9000 decodes it (MPEG-2) */
static int jf_fj_tier;                  /* how fast the CPU is (plex_tier) */
static int jf_fj_level, jf_fj_top, jf_fj_bottom;   /* its size on the ladder, how far it may go */
static ULONG jf_fj_from;                /* where it starts (a new size's: where it was) */
static BOOL jf_fj_tried;                /* its decoding timed before it played (once a film) */

static int jf_fj_snd[16], jf_fj_nsnd, jf_fj_cur;  /* its sound streams, the one asked for */
static char jf_fj_sndt[16][48];                  /* ...their names */
static void jf_film_probe(void);

static void jf_film(const struct Item *it)
{
    char f[48];
    struct NetJob *j;
    if (jf_fj) {
        net_cancel(jf_fj);                  /* (another asked for meanwhile: that one) */
        jf_fj->orphan = TRUE;
        jf_fj->seq = 0;
        jf_fj = NULL;
    }
    if (!jf.token[0]) {
        goto_jf_settings();
        return;
    }
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        set_msg(_("Not enough memory"));
        return;
    }
    radio_field(it, 4, f, sizeof(f));
    jf_fj_ms = strtoul_(f);
    radio_field(it, 0, jf_fj_key, sizeof(jf_fj_key));    /* (its key alone) */
    amb_strlcpy(jf_fj_title, it->label, sizeof(jf_fj_title));
    /* its size: the card decodes only on a screen with its overlay (RTG,
     * not AGA); else by how fast the CPU is (plex_tier: a JIT or Emu68 is
     * 3). Where on the ladder it starts and how far it may go: as last
     * time for this kind of decoder (jf_film_plan) */
    jf_fj_zz = caps.zz9000 && !video_cpu && g.truecolor;
    jf_fj_tier = plex_tier();
    jf_fj_level = jf_film_plan(jf_fj_zz, jf_fj_tier, &jf_fj_top, &jf_fj_bottom);
    if (!g.truecolor && jf_fj_top < 4 && jf_fj_bottom >= 4)
        jf_fj_top = 4;                      /* (AGA: 320 wide is all a HAM8 screen shows) */
    if (jf_fj_level < jf_fj_top)
        jf_fj_level = jf_fj_top;
    jf_fj_from = 0;
    jf_fj_tried = FALSE;
    /* first its sound streams, to choose one (a film may have several
     * languages; the server's own choice is the file's default) */
    amb_strlcpy(j->host, jf.host, sizeof(j->host));
    j->port = jf.port;
    jf_streams_request(jf_fj_key, j->path, sizeof(j->path));
    j->wait_s = 30;
    jf_fj = j;
    {
        char m[200];
        sprintf(m, _("Asking %.40s for %.50s..."), jf.name[0] ? jf.name : jf.host, it->label);
        set_msg(m);
    }
    draw_footer();
    j->jf = JFJ_STREAMS;
    j->seq = 0;
    net_submit(j);
    if (j->done && !j->busy)
        radio_done(j);
}

/* Its sound streams came: one chosen, then its start asked for. */
static void jf_film_streams(struct NetJob *j)
{
    char why[160];
    int kept;
    jf_fj = NULL;
    jf_fj_nsnd = 0;
    jf_fj_cur = -1;
    if (j->body && j->status == 200 && j->len)
        jf_fj_cur = jf_pick_audio((char *)j->body, j->len, why, sizeof(why), jf_fj_snd, jf_fj_sndt, &jf_fj_nsnd);
    else
        sprintf(why, "its streams not listed (%ld %.60s): the server's choice", (long)j->status, j->err);
    if (jf_fj_nsnd > 1 && (kept = jf_sound_kept(jf_fj_key, jf_fj_snd, jf_fj_nsnd)) >= 0) {
        jf_fj_cur = kept;
        sprintf(why, "stream %ld, chosen with A before", (long)jf_fj_snd[kept]);
    }
    amb_log("jellyfin: film's sound", why);
    jf_film_probe();
}

static void jf_film_probe(void)
{
    char m[320];
    const char *path;
    struct NetJob *j;
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        set_msg(_("Not enough memory"));
        return;
    }
    /* (its sound stream not in the URL: the player adds it, as A changes it) */
    jf_video_url(jf_fj_key, jf_fj_level, jf_fj_zz, -1, 0, jf_fj_url, sizeof(jf_fj_url));
    jf_video_seek(jf_fj_seek, sizeof(jf_fj_seek));
    path = strchr(jf_fj_url + 7, '/');
    amb_strlcpy(j->host, jf.host, sizeof(j->host));
    j->port = jf.port;
    amb_strlcpy(j->path, path ? path : "/", sizeof(j->path));
    {
        /* the probe: the start's own session (the player's first, from
         * where it starts) */
        char *c, *o = j->path + strlen(j->path), *e = j->path + sizeof(j->path) - 1, t[16];
        const char *d;
        sprintf(t, "%lu", (unsigned long)jf_fj_from);
        for (c = jf_fj_seek; *c && o < e; c++)
            if (*c == '*')
                for (d = t; *d && o < e; d++)
                    *o++ = *d;
            else
                *o++ = *c;
        *o = 0;
        if (jf_fj_nsnd > 1 && jf_fj_cur >= 0) {
            char a[32];
            sprintf(a, "&audioStreamIndex=%ld", (long)jf_fj_snd[jf_fj_cur]);
            amb_strlcat(j->path, a, sizeof(j->path));
        }
    }
    j->upto = JF_PROBE;
    j->wait_s = 60;                         /* (the server starts converting first) */
    jf_fj = j;
    {
        char lv[120];
        jf_level_text(jf_fj_level, jf_fj_zz, lv);
        sprintf(m, "%.40s: %.90s (step %ld of %ld to %ld), from %lu of %lu ms", jf_fj_title, lv,
                (long)jf_fj_level, (long)jf_fj_top, (long)jf_fj_bottom, (unsigned long)jf_fj_from,
                (unsigned long)jf_fj_ms);
        amb_log("jellyfin: film asked for", m);
    }
    j->jf = JFJ_VIDEO;                      /* (beside the settings' own: not jf_submit's) */
    j->seq = 0;
    net_submit(j);
    if (j->done && !j->busy)
        radio_done(j);
}

/* A play session's conversion stopped on the server (the player's, done
 * with it): a DELETE, its answer not waited for. */
static void jf_stop_session(const char *session)
{
    struct NetJob *j;
    char m[80];
    if (!jf.token[0] || !net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return;
    amb_strlcpy(j->host, jf.host, sizeof(j->host));
    j->port = jf.port;
    jf_stop_request(session, j->path, sizeof(j->path));
    j->post = 2;
    j->wait_s = 10;
    j->orphan = TRUE;                       /* (freed when it comes back) */
    j->seq = 0;
    sprintf(m, "conversion %.40s stopped", session);
    amb_log("jellyfin", m);
    net_submit(j);
}

/* The size it goes to from jf_fj_level (dir -1 smaller, 1 larger): a
 * size it went down from is not gone back to (its new top); kept as this
 * kind of decoder's start. */
static void jf_film_step(int dir, ULONG at)
{
    if (dir < 0)
        jf_fj_top = jf_fj_level + 1 <= jf_fj_bottom ? jf_fj_level + 1 : jf_fj_bottom;
    jf_fj_level += dir < 0 ? 1 : -1;
    if (jf_fj_level < jf_fj_top)
        jf_fj_level = jf_fj_top;
    if (jf_fj_level > jf_fj_bottom)
        jf_fj_level = jf_fj_bottom;
    jf_film_keep(jf_fj_zz, jf_fj_tier, jf_fj_level);
    jf_fj_from = at;
}

/* The sizes either side of where it plays, for the player's note. */
static void jf_film_sides(void)
{
    UWORD dw = 0, dh = 0, uw = 0, uh = 0;
    if (jf_fj_level < jf_fj_bottom)
        jf_level_dims(jf_fj_level + 1, &dw, &dh);
    if (jf_fj_level > jf_fj_top)
        jf_level_dims(jf_fj_level - 1, &uw, &uh);
    anim_net_next_sizes(dw, dh, uw, uh);
}

/* Another size asked for while it plays, the player up (anim.c): the
 * server asked for the stream there and its start probed here and now
 * (net_wait); the player starts again with them. */
static BOOL jf_relevel_now(int dir, ULONG at, char *url, LONG umax, char *seek, LONG smax, struct VProbe *v)
{
    struct NetJob *j;
    const char *path;
    char m[320], lv[120], t[16], *o, *e;
    const char *c, *d;
    BOOL ok = FALSE;
    jf_film_step(dir, at);
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    jf_video_url(jf_fj_key, jf_fj_level, jf_fj_zz, -1, 0, url, umax);
    jf_video_seek(seek, smax);
    path = strchr(url + 7, '/');
    amb_strlcpy(j->host, jf.host, sizeof(j->host));
    j->port = jf.port;
    amb_strlcpy(j->path, path ? path : "/", sizeof(j->path));
    sprintf(t, "%lu", (unsigned long)at);
    for (o = j->path + strlen(j->path), e = j->path + sizeof(j->path) - 1, c = seek; *c && o < e; c++)
        if (*c == '*')
            for (d = t; *d && o < e; d++)
                *o++ = *d;
        else
            *o++ = *c;
    *o = 0;
    if (jf_fj_nsnd > 1 && jf_fj_cur >= 0) {
        sprintf(t, "%ld", (long)jf_fj_snd[anim_net_sound()]);
        amb_strlcat(j->path, "&audioStreamIndex=", sizeof(j->path));
        amb_strlcat(j->path, t, sizeof(j->path));
    }
    j->upto = JF_PROBE;
    j->wait_s = 60;
    j->jf = JFJ_VIDEO;
    j->seq = 0;
    jf_level_text(jf_fj_level, jf_fj_zz, lv);
    sprintf(m, "%.40s: %.90s (step %ld of %ld to %ld), from %lu ms, the player up", jf_fj_title, lv,
            (long)jf_fj_level, (long)jf_fj_top, (long)jf_fj_bottom, (unsigned long)at);
    amb_log("jellyfin: film asked for", m);
    net_submit(j);
    if (net_wait(j, 65000)) {
        if (j->body && j->status == 200 && j->len && vprobe_mem(j->body, (LONG)j->len, v)) {
            v->ps = TRUE;
            v->ms = jf_fj_ms;
            sprintf(m, "its first %lu KB in %lu ms", (unsigned long)(j->len >> 10), (unsigned long)j->ms);
            amb_log("jellyfin: film", m);
            if (j->ms > 5000 && jf_fj_top < jf_fj_level) {
                /* slow to start from a time (a DVD's folder: the server's
                 * ffmpeg decodes from the start of a VOB to get there, 10
                 * to 20 s on Brian's NAS): no larger size for this film,
                 * only a smaller one when it must */
                jf_fj_top = jf_fj_level;
                amb_log("jellyfin: film", "slow to start mid-film: no larger size for it from here");
            }
            anim_net_adapt(at, (jf_fj_level < jf_fj_bottom ? 1 : 0) | (jf_fj_level > jf_fj_top ? 2 : 0));
            jf_film_sides();
            ok = TRUE;
        } else
            amb_log("jellyfin: film", "the new size's start could not be read");
        if (j->body)
            FreeVec(j->body);
        FreeVec(j);
    } else {
        amb_log("jellyfin: film", "no answer for the new size within 65 s");
        j->orphan = TRUE;                   /* (freed when it comes back) */
    }
    return ok;
}

/* Its start here: probed, then played as a stream from the server. */
static void jf_film_came(struct NetJob *j)
{
    char m[200];
    struct VProbe v;
    jf_fj = NULL;
    if (!j->body || j->status != 200 || !j->len) {
        sprintf(m, _("%.50s could not be fetched: %.100s"), jf_fj_title,
                j->err[0] ? j->err : j->status ? "" : _("no answer"));
        if (!j->err[0] && j->status)
            sprintf(m + strlen(m), _("the server said %ld"), (long)j->status);
        amb_log("jellyfin: film", m);
        set_msg(m);
        draw_footer();
        return;
    }
    sprintf(m, "its first %lu KB in %lu ms", (unsigned long)(j->len >> 10), (unsigned long)j->ms);
    amb_log("jellyfin: film", m);
    if (!vprobe_mem(j->body, (LONG)j->len, &v)) {
        amb_log("jellyfin: film", "no MPEG sequence header in its start");
        set_msg(_("Not an MPEG video Ambiance can read"));
        draw_footer();
        return;
    }
    v.ps = TRUE;                            /* (a Program Stream: stream.mpg) */
    v.ms = jf_fj_ms;
    if (!jf_fj_tried && !jf_fj_zz && jf_fj_top < jf_fj_bottom) {
        /* before it plays, the CPU's decoding of the probe's pictures timed:
         * the largest size whose pictures it can decode and show in time,
         * the lot within three quarters of a picture's 33 ms. The trial
         * (alone, the pictures of a still start) ran about 6 times quicker
         * than the same pictures playing in Amiberry (1.71 ms against 0.29
         * at 320 x 212, with the sound, the reading and the overlay beside
         * it), so a picture is counted as 4 times its trial on a graphics
         * card's screen and 8 on AGA's (HAM8 and planes cost more than the
         * decoding: ambbench 1.1 on Brian's A3000). Another size: asked for
         * again before playing; then it adapts as it plays. */
        ULONG n = 0, us;
        UWORD pw, ph;
        int k, want = jf_fj_bottom;
        jf_fj_tried = TRUE;
        us = mpeg_trial_us(j->body, j->len, &n, &pw, &ph);
        if (us && n) {
            ULONG area = (ULONG)pw * ph, f = g.truecolor ? 4 : 8;
            for (k = jf_fj_top; k <= jf_fj_bottom; k++)
                if ((unsigned long long)us * jf_level_area(k) / (area ? area : 1) * f <= 25000ULL) {
                    want = k;
                    break;
                }
            sprintf(m, "%lu pictures %lu x %lu decoded in %lu us each: step %ld (was %ld)", (unsigned long)n,
                    (unsigned long)pw, (unsigned long)ph, (unsigned long)us, (long)want, (long)jf_fj_level);
            amb_log("jellyfin: film's trial", m);
            if (want != jf_fj_level) {
                jf_fj_level = want;
                jf_film_keep(jf_fj_zz, jf_fj_tier, jf_fj_level);
                jf_film_probe();            /* (at that size: then played) */
                return;
            }
        } else
            amb_log("jellyfin: film's trial", "no picture decoded from its start: as it was");
    }
    {
        struct Item it;
        msg[0] = 0;
        memset(&it, 0, sizeof(it));
        it.kind = IT_ANIM;
        it.label = jf_fj_title;
        it.path = jf_fj_url;
        it.sortkey = (char *)"MPEG";
        ULONG at;
        int rl;
        anim_net_probe = &v;
        anim_net_seek = jf_fj_seek;
        anim_net_on_stop(jf_stop_session);
        anim_net_adapt(jf_fj_from, (jf_fj_level < jf_fj_bottom ? 1 : 0) | (jf_fj_level > jf_fj_top ? 2 : 0));
        jf_film_sides();
        anim_net_on_relevel(jf_relevel_now);
        {
            const char *t[16];
            int i;
            for (i = 0; i < 16; i++)
                t[i] = jf_fj_sndt[i];
            anim_net_sounds(jf_fj_snd, t, jf_fj_nsnd, jf_fj_cur);
        }
        anim_from_item(&it);
        anim_net_probe = NULL;
        anim_net_seek = NULL;
        anim_net_on_stop(NULL);
        anim_net_on_relevel(NULL);
        if (jf_fj_nsnd > 1 && anim_net_sound() != jf_fj_cur) {
            jf_fj_cur = anim_net_sound();
            jf_sound_keep(jf_fj_key, jf_fj_snd[jf_fj_cur]);  /* (chosen with A: kept) */
        }
        if ((rl = anim_net_relevel(&at)) != 0) {
            /* it asked for a smaller or larger picture: the film again at
             * that size from where it was; the size kept for the next
             * film; a size it went down from not gone back to */
            jf_film_step(rl, at);           /* (the player could not: started again here) */
            sprintf(m, _("Asking %.40s for %.50s..."), jf.name[0] ? jf.name : jf.host, jf_fj_title);
            set_msg(m);
            draw_footer();
            jf_film_probe();
        }
    }
}

/* A Jellyfin job of the UI's own, back. */
static void jf_job_came(struct NetJob *j)
{
    char m[200];
    BOOL mine = j == jf_nj;
    if (mine)
        jf_nj = NULL;
    if (j->jf == JFJ_STREAMS && j == jf_fj) {
        jf_film_streams(j);
    } else if (j->jf == JFJ_VIDEO && j == jf_fj) {
        jf_film_came(j);
    } else if (mine && j->jf == JFJ_FIND) {
        char *s = (char *)j->body, *e;
        jf_nfound = 0;
        while (s && *s && jf_nfound < 8) {
            char *t1, *t2;
            if ((e = strchr(s, '\n')))
                *e = 0;
            if ((t1 = strchr(s, '\t')) && (t2 = strchr(t1 + 1, '\t'))) {
                *t1 = *t2 = 0;
                amb_strlcpy(jf_found[jf_nfound][0], s, 80);
                amb_strlcpy(jf_found[jf_nfound][1], t1 + 1, 80);
                amb_strlcpy(jf_found[jf_nfound][2], t2 + 1, 80);
                sprintf(m, "%s (%s)", jf_found[jf_nfound][2], jf_found[jf_nfound][0]);
                amb_log("jellyfin: found", m);
                jf_nfound++;
            }
            s = e ? e + 1 : NULL;
        }
        if (!j->body) {
            amb_log("jellyfin: none found", j->err);
            set_msg(j->err[0] ? j->err : _("No Jellyfin server answered on this network"));
        } else if (jf_nfound == 1 && !jf.host[0]) {
            /* the one: chosen */
            jf_set_server(jf_found[0][0], jf_found[0][2], jf_found[0][1]);
            sprintf(m, _("Jellyfin server: %.60s"), jf.name);
            set_msg(m);
            jf_roots_again();
        } else {
            sprintf(m, jf_nfound == 1 ? _("%ld Jellyfin server found") : _("%ld Jellyfin servers found"),
                    (long)jf_nfound);
            set_msg(m);
        }
        jf_settings_again();
    } else if (mine && j->jf == JFJ_SIGNIN) {
        char why[120];
        memset(jf_body, 0, sizeof(jf_body));    /* (the password in it) */
        if (jf_signin_came(j, jf_user_asked, why, sizeof(why))) {
            sprintf(m, _("Signed in to %.60s as %.40s"), jf.name[0] ? jf.name : jf.host, jf.user);
            amb_log("jellyfin", m);         /* (never the token) */
            set_msg(m);
            jf_roots_again();
        } else {
            sprintf(m, "sign-in refused or failed (%ld): %.100s", (long)j->status, why);
            amb_log("jellyfin", m);
            set_msg(why);
        }
        jf_settings_again();
    } else if (mine && j->jf == JFJ_INFO) {
        /* the server's own name and id (a typed address) */
        if (j->body && j->status == 200) {
            char name[64], id[40];
            if (jf_value((const char *)j->body, j->len, "ServerName", name, sizeof(name)) && name[0]) {
                utf8_to_latin1(name);
                jf_value((const char *)j->body, j->len, "Id", id, sizeof(id));
                sprintf(m, "%.60s:%lu", jf.host, (unsigned long)jf.port);
                {
                    /* (the token kept: the same server, now named) */
                    char tok[80], uid[40], user[64];
                    amb_strlcpy(tok, jf.token, sizeof(tok));
                    amb_strlcpy(uid, jf.uid, sizeof(uid));
                    amb_strlcpy(user, jf.user, sizeof(user));
                    jf_set_server(m, name, id);
                    if (tok[0] && !jf.token[0])
                        jf_signed_in(user, uid, tok);
                }
                sprintf(m, _("Jellyfin server: %.60s"), name);
                set_msg(m);
                amb_log("jellyfin: server named", name);
                jf_settings_again();
                jf_roots_again();
            }
        } else {
            sprintf(m, _("No Jellyfin server answered at %.60s"), jf.host);
            set_msg(m);
            amb_log("jellyfin: not answering", j->err);
        }
    }
    if (j->body)
        FreeVec(j->body);
    FreeVec(j);
}

static void toggle_https(void)
{
    struct Page *p = cur();
    LONG sel = p ? p->sel : 0;
    if (!caps.amissl)
        return;
    radio_https = !radio_https;
    save_setting(HTTPS_VAR, radio_https ? "1" : "0");
    https_apply();
    (void)p;
    (void)sel;
    plex_settings_again(FALSE);
}

static void toggle_dbuf(void)
{
    struct Page *p = cur();
    LONG sel = p ? p->sel : 0;
    if (g.planar)
        return;
    gfx_dbuf_wanted = !gfx_dbuf_wanted;
    save_setting(DBUF_VAR, gfx_dbuf_wanted ? "1" : "0");
    if (gfx_dbuf_wanted)
        gfx_dbuf_start(&g);
    else
        gfx_dbuf_stop(&g);
    (void)p;
    (void)sel;
    plex_settings_again(FALSE);
    status_shown[0] = 0;
    lv_redraw();                    /* (a layout: drawn whole onto the other buffer arrangement) */
    draw_all();
}

/* Settings > Display > Layouts on native screens: on, a layout chosen
 * shows on an 8-bit screen too (tried again here); off, such a one goes */
static void toggle_lv_native(void)
{
    struct Page *p = cur();
    LONG i;
    lv_native = !lv_native;
    save_setting(LVNATIVE_VAR, lv_native ? "1" : "0");
    lv_native_changed();
    if (p && p->type == PT_LIST) {
        for (i = 0; i < p->list.count; i++)
            if (p->list.items[i].action == ACT_LVNATIVE)
                p->list.items[i].mark = lv_native ? MARK_CHECK_ON : MARK_CHECK_OFF;
    }
    settings_rebuild();
    draw_all();
}

static void toggle_debug(void)
{
    struct Page *p = cur();
    LONG sel = p ? p->sel : 0;
    set_debug(!debug_on);
    save_setting(DEBUG_VAR, debug_on ? "1" : "0");
    (void)p;
    (void)sel;
    plex_settings_again(FALSE);
    draw_status();
}

/* The same screen, its look made again (no mode switch), and back
 * where we were: Settings > Style, on the new choice. */
static void restyle(void)
{
    struct Page *p;
    int i;
    pages_clear();
    pv_flush();
    spin_free();
    ov_bm_free();
    BOOL restyled;
    if (g.wb) {
        /* a window: opened again, its size following the new scale */
        ptr_hidden = NULL;
        screen_close();
        if (!gfx_open(&g, &caps, screen_mode, ui_style)) {
            running = FALSE;
            return;
        }
        restyled = ui_style != STYLE_SMOOTH || g.style == STYLE_SMOOTH;
    } else {
        lv_hold();                  /* a layout's off first: the smooth font lent it is one
                                     * gfx_restyle frees (then freed again at the end: a
                                     * crash on quitting; meanwhile lists drawn with none) */
        restyled = gfx_restyle(&g, ui_style);
    }
    for (i = 0; i < nmenu; i++)
        if (menu[i] == MOD_SETTINGS)
            menu_sel = i;
    build_root(MOD_SETTINGS);
    if (cur())
        cur()->sel = SETTINGS_SCREEN_ROW;
    if ((p = page_push(PT_LIST, _("Display")))) {
        build_screen_settings(p);
        p->sel = screen_row_fonts();
    }
    if ((p = page_push(PT_LIST, _("Fonts")))) {
        build_font_settings(p);
        p->sel = fonts_row_style();
    }
    if ((p = page_push(PT_LIST, _("Style"))))
        build_styles(p);
    focus = FOCUS_CONTENT;
    msg[0] = 0;
    draw_all();
    note_style();
    if (!restyled)
        set_msg(_("The smooth style's files could not be loaded"));
}

/* Settings > Style. Remembered either way; the screen is reopened only
 * when the look changes (smooth needs an RTG screen). */
static void apply_style(int st)
{
    struct Page *p = cur();
    if (st == ui_style)
        return;
    ui_style = st;
    save_setting(STYLE_VAR, st == STYLE_SMOOTH ? "smooth" : "pixel");
    if ((st == STYLE_SMOOTH) == (g.style == STYLE_SMOOTH) ||
        (st == STYLE_SMOOTH && !gfx_style_possible(&caps, g.mode, STYLE_SMOOTH))) {
        /* the look stays: just move the radio button, and update the
         * Settings page underneath (its Style row shows the choice) */
        if (p && p->type == PT_LIST) {
            list_free(&p->list);
            list_init(&p->list);
            build_styles(p);
            draw_list_rows(p);
        }
        settings_rebuild();         /* Screen's Style row shows the choice */
        if (st == STYLE_SMOOTH && g.style != STYLE_SMOOTH)
            set_msg(_("Smooth: on RTG screens only"));
        return;
    }
    restyle();
}

/* Settings > Style on an RTG screen: a pixel font (and the pixel style,
 * if it was Smooth), and the layout with it. */
static void apply_rtg_font(int i)
{
    if (i == gfx_font_shown(&g) && g.style != STYLE_SMOOTH)
        return;
    gfx_rtg_font = i;
    save_setting(RFONT_VAR, gfx_laced_fonts[i].name);
    if (ui_style != STYLE_PIXEL) {
        ui_style = STYLE_PIXEL;
        save_setting(STYLE_VAR, "pixel");
    }
    restyle();
}

/* Settings > Style on a laced screen: its font, and the layout with it. */
static void apply_laced_font(int i)
{
    if (i == gfx_laced_font)
        return;
    gfx_laced_font = i;
    save_setting(LFONT_VAR, gfx_laced_fonts[i].name);
    restyle();
}

/* ------------------------------------------------------------ radio
 * Radio: lists of stations from radio-browser.info (radio.c, asked by the
 * network process, net.c). The root lists ways in: the stations most
 * voted for, most listened to, trending, two scene favourites, and by
 * genre or country (lists themselves). Opening one pushes its page at
 * once, empty, and asks; the answer fills it when it comes, while the UI
 * goes on (Back gives the request up). One request is out at a time: a
 * new one gives the last up. A station opens a page about it.
 *
 * Only stations an Amiga can play are listed: MP3, working, no HLS, and
 * https: ones only when Settings allows HTTPS (with AmiSSL). */
static struct NetJob *rq;           /* the request out, or NULL */
static ULONG rq_seq;

/* Music: a Plex server's music (plex.c); the server chosen, or found. */
/* Music's own files on its first page: the MUSIC: assign and each music
 * folder, opened as folders of MP3 and FLAC files. */
static void music_file_rows(struct Page *p)
{
    struct Item *it;
    int i, n = 0;
    p->songs = 1;
    if (music_assign && music_assign_there() && beets_there() &&
        (it = list_add(&p->list, _("Music library (beets)"), "beets:", "", IT_DIR))) {
        it->mark = MARK_ARROW;             /* its database in MUSIC: (beets.c) */
        n++;
    }
    if (music_assign && music_assign_there() &&
        (it = list_add(&p->list, _("Music files (MUSIC:)"), "MUSIC:", "", IT_DIR))) {
        it->mark = MARK_ARROW;
        n++;
    }
    for (i = 0; i < nmusic_dirs; i++) {
        char label[180];
        const char *f = (const char *)FilePart((STRPTR)music_dirs[i]);
        sprintf(label, _("Music files: %.100s"), f[0] ? f : music_dirs[i]);
        if ((it = list_add(&p->list, label, music_dirs[i], "", IT_DIR))) {
            it->mark = MARK_ARROW;
            n++;
        }
    }
    if (!n)
        list_add(&p->list, _("Music files: Settings > Folders > Music"), "", "", IT_TEXT);
}

static void build_music_root(struct Page *p)
{
    struct Item *it;
    char label[128];
    p->plex = 1;
    np_shortcut(p);
    if (!caps.net) {
        list_add(&p->list, _("No TCP/IP stack running: Plex needs networking"), "", "", IT_TEXT);
        music_file_rows(p);
        return;
    }
    if (!plex.on) {
        /* (chosen: there, Settings > Networking > Plex) */
        action_item(p, _("Plex is turned off (Settings > Networking > Plex)"), "", ACT_GOPLEX, MARK_ARROW);
        jf_section_row(p, 'm', FALSE);
        music_file_rows(p);
        return;
    }
    if (plex.host[0] && plex.library[0]) {
        /* the library known: its Artists, Albums, Recently added here */
        plex_section_menu(plex.library, &p->list);
        if (plex.libraries > 1) {
            sprintf(label, _("Other libraries on %s"), plex.name[0] ? plex.name : plex.host);
            if ((it = list_add(&p->list, label, "/library/sections", "", IT_PLEXDIR)))
                it->mark = MARK_ARROW;
        }
    } else if (plex.host[0]) {
        sprintf(label, _("Plex: %s"), plex.name[0] ? plex.name : plex.host);
        if ((it = list_add(&p->list, label, "/library/sections", "", IT_PLEXDIR)))
            it->mark = MARK_ARROW;
    } else {
        /* the server is chosen in Settings (About > Plex in Ambiance says more) */
        action_item(p, _("No Plex server chosen: Settings > Networking > Plex"), "", ACT_GOPLEX, MARK_ARROW);
    }
    jf_section_row(p, 'm', FALSE);
    music_file_rows(p);
}

static void build_radio_root(struct Page *p)
{
    static const struct { const char *label, *query; } ways[] = {
        { N_("Top voted"), "order=votes&reverse=true" },
        { N_("Most listened to"), "order=clickcount&reverse=true" },
        { N_("Trending"), "order=clicktrend&reverse=true" },
        { N_("Chiptune"), "tag=chiptune&tagExact=true&order=votes&reverse=true" },
        { N_("Demoscene"), "tag=demoscene&tagExact=true&order=votes&reverse=true" },
        { N_("Genres"), RADIO_TAGS },
        { N_("Countries"), RADIO_COUNTRIES },
    };
    int i;
    p->radio = 1;
    np_shortcut(p);
    for (i = 0; i < (int)(sizeof(ways) / sizeof(ways[0])); i++) {
        struct Item *it = list_add(&p->list, _(ways[i].label), ways[i].query, "", IT_NET);
        if (it)
            it->mark = MARK_ARROW;
    }
}

/* The answer to j into the page that asked, if it is still open. */
/* ---- long Plex lists: the server's first PLEX_PART entries come first;
 * the rest are placeholders (ITEM_GONE, labelled with their first letter
 * once the server says where each letter starts, so typing one jumps
 * there), read part by part: all of it in turn with memory to spare,
 * else only the parts shown or selected, as they are needed. One request
 * at a time (rq_more), not the page's own (rq): opening an entry meanwhile
 * does not give it up. */
#define PLEX_PART  1000
#define PLEX_PARTS 128
static struct NetJob *rq_more;

/* Rows above the server's entries (Now playing, a slideshow). */
static LONG plex_lead(struct Page *p)
{
    LONG i;
    for (i = 0; i < p->list.count && i < 3 && p->list.items[i].kind == IT_ACTION; i++)
        ;
    return i;
}

static BOOL plex_part_read(struct Page *p, LONG k)
{
    return k < 0 || k >= PLEX_PARTS || (p->plex_parts[k >> 5] & (1UL << (k & 31)));
}

/* The part to read next: with memory to spare the first not read; else
 * one the list shows or has selected (none: nothing yet). -1 none. */
static LONG plex_part_wanted(struct Page *p)
{
    LONG lead = plex_lead(p), parts = (p->plex_total + PLEX_PART - 1) / PLEX_PART, k;
    if (parts > PLEX_PARTS)
        parts = PLEX_PARTS;
    if (AvailMem(MEMF_ANY) > 3000000UL) {
        for (k = 0; k < parts; k++)
            if (!plex_part_read(p, k))
                return k;
        return -1;
    }
    {
        LONG idx[3], i;
        idx[0] = p->sel;
        idx[1] = p->top;
        idx[2] = p->top + list_visible();
        for (i = 0; i < 3; i++) {
            k = idx[i] - lead < p->plex_n0 ? 0
                : (idx[i] - lead - p->plex_n0 + PLEX_PART) / PLEX_PART;
            if (idx[i] >= lead && k < parts && !plex_part_read(p, k))
                return k;
        }
    }
    return -1;
}

static void plex_more_ask(struct Page *p)
{
    struct NetJob *j;
    LONG k = -1;
    char key[300], *q;
    if (rq_more || !p->plex_total || AvailMem(MEMF_ANY) < 400000UL)
        return;
    /* the pictures on screen first: a long list's next part (a third of a
     * second or more, the network's one at a time) waits while any are
     * being fetched or still wanted (the first page's covers came one a
     * part, Albums' 8450 in 9 parts) - the tick asks again */
    if (!p->plex_letters && jf_key(p->base))
        p->plex_letters = TRUE;             /* Jellyfin: no letters to ask, its parts at once */
    if (p->plex_letters) {
        int d;
        for (d = 0; d < ART_KINDS; d++)
            if (art_nj[d] && d != ART_AHEAD)
                return;
        if (time_ms() - art_lv_wanted < 500)
            return;
    }
    if (p->plex_letters && (k = plex_part_wanted(p)) < 0)
        return;
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return;
    amb_strlcpy(j->host, plex.host, sizeof(j->host));
    j->port = plex.port;
    if (!p->plex_letters) {
        /* first: where each letter starts (/firstCharacter, the list's type) */
        amb_strlcpy(key, p->base, sizeof(key));
        if ((q = strstr(key, "/all")))
            memmove(q + 15, q + 4, strlen(q + 4) + 1), memcpy(q, "/firstCharacter", 15);
        plex_request(key, j->path, sizeof(j->path));
        j->start = -1;
        p->plex_letters = TRUE;
    } else {
        plex_job(j, p->base, k * PLEX_PART);
        j->parse = 1;
        j->wait_s = PLEX_WAIT_S;          /* a NAS's server may think for a while */
        j->start = k * PLEX_PART + 1;       /* +1: never 0, which is a first part */
        amb_strlcpy(j->pkey, p->base, sizeof(j->pkey));
    }
    j->seq = ++rq_seq;
    p->more_seq = j->seq;
    rq_more = j;
    net_submit(j);
}

/* The server's first characters (/firstCharacter: Directory title, size,
 * in the list's order): the placeholders labelled with them. */
static void plex_letters_came(struct Page *p, struct NetJob *j)
{
    static char letters[40][2];
    LONG lead = plex_lead(p), at = 0, i;
    const char *s = (const char *)j->body, *e = s ? s + j->len : NULL;
    if (!s)
        return;
    while ((s = strstr(s, "<Directory")) && s < e) {
        const char *t = strstr(s, " title=\""), *z = strstr(s, " size=\""), *end = strchr(s, '>');
        LONG n = 0;
        UBYTE c;
        if (!t || !z || !end || t > end || z > end)
            break;
        for (z += 7; *z >= '0' && *z <= '9'; z++)
            n = n * 10 + (*z - '0');
        c = (UBYTE)t[8];
        c = c >= 'a' && c <= 'z' ? c - 32 : c;
        i = c >= 'A' && c <= 'Z' ? c - 'A' + 1 : 0;
        letters[i][0] = i ? (char)c : '#';
        letters[i][1] = 0;
        for (; n > 0 && at < p->plex_total; n--, at++) {
            LONG li = lead + p->plex_n0 + at - PLEX_PART;   /* the server's entry at */
            if (at >= PLEX_PART && li < p->list.count && (p->list.items[li].flags & ITEM_GONE))
                p->list.items[li].label = letters[i];
        }
        s = end;
    }
}

/* Settings > Networking > Plex > Sign in at plex.tv: Plex's own way for a
 * device without a keyboard to speak of. A code is asked for (a POST to
 * /api/v2/pins), shown, and entered by the user at plex.tv/link on a phone
 * or computer; the code is asked after every few seconds until its
 * authToken comes (or it runs out). Then the account's name, and its
 * servers: the one chosen gets its token, or, none chosen, the first is.
 * All over HTTPS (AmiSSL, net.c); no token is logged. Leaving the page
 * gives up. */
enum { PL_IDLE, PL_PIN, PL_WAIT, PL_CHECK, PL_USER, PL_SERVERS, PL_DONE, PL_FAILED };
static struct {
    UBYTE state;
    char id[24], code[16], why[120];
    ULONG next_ms, until_ms;
    struct NetJob *j;
} plink;

#define PLINK_POLL_MS 3000

static ULONG plink_num(const char *t)
{
    ULONG v = 0;
    while (*t >= '0' && *t <= '9')
        v = v * 10 + (ULONG)(*t++ - '0');
    return v;
}

static struct Page *plink_page(void)
{
    int d;
    for (d = 0; d < depth; d++)
        if (pages[d].plexlink && pages[d].type == PT_TEXT)
            return &pages[d];
    return NULL;
}

/* The page as things stand. */
static void plink_show(void)
{
    struct Page *p = plink_page();
    char line[200];
    int i, k = 0;
    if (!p)
        return;
    tb_close(&p->tb);                       /* drawn from the lines below, anew */
    list_free(&p->list);
    if (!list_init(&p->list))
        return;
    switch (plink.state) {
    case PL_PIN:
        text_para(p, _("Asking plex.tv for a code...  Esc to give up"));
        break;
    case PL_WAIT:
    case PL_CHECK:
        text_para(p, _("On a phone or computer, go to plex.tv/link, sign in to Plex if it asks, "
                       "and enter this code:"));
        text_line(p, "");
        strcpy(line, "        ");
        k = strlen(line);
        for (i = 0; plink.code[i] && k < (int)sizeof(line) - 3; i++) {
            line[k++] = plink.code[i];      /* spaced: read across a room */
            line[k++] = ' ';
        }
        line[k] = 0;
        text_line(p, line);
        text_line(p, "");
        text_para(p, _("Ambiance is waiting for it to be entered (the code lasts 15 minutes). "
                       "Esc gives up."));
        break;
    case PL_USER:
    case PL_SERVERS:
        text_para(p, _("Signed in. Asking plex.tv for this account's servers..."));
        break;
    case PL_DONE:
        sprintf(line, _("Signed in to plex.tv as %s."), plex.user[0] ? plex.user : "?");
        text_para(p, line);
        text_line(p, "");
        sprintf(line, _("Server: %s"), plex.host[0] ? (plex.name[0] ? plex.name : plex.host)
                                                    : _("none chosen"));
        text_line(p, line);
        text_line(p, "");
        text_para(p, _("Settings > Networking > Plex > Plex server lists this account's servers "
                       "beside those found on the network."));
        break;
    case PL_FAILED:
        sprintf(line, _("Signing in did not work: %s"), plink.why);
        text_para(p, line);
        break;
    }
    if (p == cur()) {
        draw_pane(TRUE);
        draw_footer();
    }
}

static void plink_fail(const char *why)
{
    amb_strlcpy(plink.why, why, sizeof(plink.why));
    plink.state = PL_FAILED;
    amb_log("plex: signing in", why);
    plink_show();
}

/* The next request of the sign-in, out to plex.tv. */
static void plink_ask(UBYTE state)
{
    struct NetJob *j;
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        plink_fail(_("Not enough memory"));
        return;
    }
    amb_strlcpy(j->host, PLEX_TV, sizeof(j->host));
    j->port = 443;
    j->tls = 1;
    plex_tv_headers(j->extra, sizeof(j->extra), state == PL_USER || state == PL_SERVERS);
    if (state == PL_PIN) {
        j->post = 1;
        strcpy(j->path, "/api/v2/pins?strong=false");      /* a short code, for plex.tv/link */
    } else if (state == PL_CHECK) {
        sprintf(j->path, "/api/v2/pins/%s", plink.id);
    } else if (state == PL_USER) {
        strcpy(j->path, "/api/v2/user");
    } else {
        strcpy(j->path, "/api/resources?includeHttps=1&includeRelay=0");
    }
    plink.state = state;
    plink.j = j;
    net_submit(j);
    if (j->done && !j->busy) {              /* the process could not start */
        plink.j = NULL;
        FreeVec(j);
        plink_fail(_("No TCP/IP stack is running"));
    }
}

/* The request out, if any, given up (its job freed when it comes back). */
static void plink_stop(void)
{
    if (plink.j) {
        net_cancel(plink.j);
        plink.j->orphan = TRUE;
        plink.j->seq = 0;
        plink.j = NULL;
    }
    plink.state = PL_IDLE;
}

/* The account's servers: the chosen one's token, or, none chosen, the
 * first of them chosen (the owner's token where none fits). */
static void plink_servers(UBYTE *body, ULONG len)
{
    static char lines[2048];
    char want[80], m[80], line[240], first[240], *f[4];
    const char *l, *e;
    LONG n = plex_resource_lines(body, len, lines, sizeof(lines));
    BOOL found = FALSE;
    first[0] = 0;
    sprintf(want, "%.60s\t%lu\t", plex.host, (unsigned long)plex.port);
    for (l = lines; *l && (e = strchr(l, '\n')); l = e + 1) {
        LONG c = e - l < (LONG)sizeof(line) - 1 ? e - l : (LONG)sizeof(line) - 1;
        memcpy(line, l, c);
        line[c] = 0;
        if (!first[0])
            strcpy(first, line);
        if (plex.host[0] && !strncmp(line, want, strlen(want))) {
            strcpy(first, line);            /* the one chosen: this one's token */
            found = TRUE;
            break;
        }
    }
    if (first[0] && (found || !plex.host[0])) {
        int k = 1;
        f[0] = first;
        while (k < 4 && (f[k] = strchr(f[k - 1], '\t')))
            *f[k++]++ = 0;
        if (k == 4) {
            if (!found) {
                LONG port = (LONG)plink_num(f[1]);
                utf8_to_latin1(f[2]);
                plex_set_server(f[0], (UWORD)(port > 0 && port < 65536 ? port : 32400), f[2]);
            }
            plex_set_token(f[3]);           /* the server's own token */
            found = TRUE;
        }
    }
    if (!found && !plex.token[0])
        plex_set_token(plex.account);       /* the owner's token opens the owner's servers */
    sprintf(m, "signed in; %ld server%s on the account", (long)n, n == 1 ? "" : "s");
    amb_log("plex: plex.tv", m);
}

/* plex.tv answered. */
static void plink_came(struct NetJob *j)
{
    char v[80];
    UBYTE was = plink.state;
    plink.j = NULL;
    if (was == PL_CHECK && !j->body && j->status != 404) {
        plink.state = PL_WAIT;              /* a hiccup: asked again in a moment */
        plink.next_ms = time_ms() + PLINK_POLL_MS;
        return;
    }
    if (!j->body) {
        plink_fail(was == PL_CHECK ? _("The code ran out before it was entered: try again")
                   : j->err[0] ? j->err : _("The server did not answer"));
        return;
    }
    switch (was) {
    case PL_PIN:
        if (!plex_tv_value(j->body, "id", plink.id, sizeof(plink.id)) ||
            !plex_tv_value(j->body, "code", plink.code, sizeof(plink.code))) {
            plink_fail(_("plex.tv sent no code"));
            return;
        }
        plink.state = PL_WAIT;
        plink.next_ms = time_ms() + PLINK_POLL_MS;
        plink.until_ms = time_ms() + (plex_tv_value(j->body, "expiresIn", v, sizeof(v)) &&
                                      plink_num(v) > 60 ? plink_num(v) : 900UL) * 1000UL;
        amb_log("plex: signing in", "a code to enter at plex.tv/link");
        plink_show();
        break;
    case PL_CHECK:
        {
            char tok[64];
            if (plex_tv_value(j->body, "authToken", tok, sizeof(tok))) {
                plex_set_account(tok, "");
                plink_ask(PL_USER);
                plink_show();
            } else {
                plink.state = PL_WAIT;      /* not entered yet */
                plink.next_ms = time_ms() + PLINK_POLL_MS;
            }
        }
        break;
    case PL_USER:
        if (!plex_tv_value(j->body, "username", v, sizeof(v)))
            plex_tv_value(j->body, "title", v, sizeof(v));
        utf8_to_latin1(v);
        plex_set_account(plex.account, v);
        plink_ask(PL_SERVERS);
        break;
    case PL_SERVERS:
        plink_servers(j->body, j->len);
        plink.state = PL_DONE;
        settings_rebuild();                 /* Settings > Plex under: signed in, its server */
        plink_show();
        break;
    }
}

/* Once a second: the code asked after, until it is entered or runs out;
 * the page gone, given up. */
static void plink_tick(void)
{
    if (plink.state == PL_IDLE)
        return;
    if (!plink_page()) {
        plink_stop();
        return;
    }
    if (plink.state != PL_WAIT)
        return;
    if ((LONG)(time_ms() - plink.until_ms) >= 0) {
        plink_fail(_("The code ran out before it was entered: try again"));
        return;
    }
    if ((LONG)(time_ms() - plink.next_ms) >= 0)
        plink_ask(PL_CHECK);
}

static void plink_start(void)
{
    struct Page *p;
    if (!https_ok()) {
        set_msg(_("needs HTTPS (Settings > Networking)"));
        return;
    }
    plink_stop();
    if (!(p = page_push(PT_TEXT, _("Sign in at plex.tv")))) {
        set_msg(_("Too many levels open"));
        return;
    }
    p->plexlink = 1;
    plink_ask(PL_PIN);
    if (plink.state == PL_PIN)
        plink_show();
}

static void plink_sign_out(void)
{
    plink_stop();
    plex_set_account("", "");
    plex_set_token("");                     /* the server's, which came with it */
    amb_log("plex: plex.tv", "signed out");
    plex_settings_again(TRUE);
    set_msg(_("Signed out of plex.tv"));
}

static void whd_done(struct NetJob *j);
static struct NetJob *whd_job;

void radio_done(struct NetJob *j)
{
    struct Page *p = NULL, *again_p = NULL;
    char again[160];
    int d;
    if (j == whd_job) {
        whd_done(j);                        /* WHDLoad's package */
        return;
    }
    if (j->jf) {
        jf_job_came(j);                     /* Jellyfin's settings' own */
        return;
    }
    for (d = 0; d < ART_KINDS; d++)
        if (j == art_nj[d]) {
            /* a picture of the server's: kept in RAM:, then shown */
            art_nj[d] = NULL;
            if (j->body && j->status == 200 && plex_art_save(art_want[d], j->body, j->len)) {
                char m[220];
                ULONG now = time_ms();
                /* its size, and its time step by step: waiting for the
                 * network process, fetching, then until it was taken here */
                sprintf(m, "%.50s%s: %lu bytes; %lu ms waiting to be fetched, fetched in %lu ms, "
                           "taken %lu ms later%s",
                        FilePart((STRPTR)art_want[d]), d == ART_AHEAD ? " (ahead)" : "",
                        (unsigned long)j->len,
                        (unsigned long)(j->t_start - j->t_sub), (unsigned long)j->ms,
                        (unsigned long)(now - (j->t_start + j->ms)), d == ART_PV ? "" : "");
                if (d == ART_PV) {
                    char t[60];
                    sprintf(t, "; asked %lu ms after the move",
                            (unsigned long)(art_asked[d] - det.since));
                    amb_strlcat(m, t, sizeof(m));
                }
                amb_log("plex: picture", m);
                art_ready(d, art_want[d]);
            } else {
                char m[80];
                sprintf(m, "not sent (%ld)%s%.40s", (long)j->status, j->err[0] ? ": " : "", j->err);
                amb_log("plex: picture", m);
                if (d == ART_VW)
                    vw_failed = TRUE;       /* the viewer goes on without it */
            }
            if (j->body)
                FreeVec(j->body);
            FreeVec(j);
            return;
        }
    if (j == plink.j) {
        plink_came(j);                      /* signing in at plex.tv */
        if (j->body)
            FreeVec(j->body);
        FreeVec(j);
        return;
    }
    if (j == plex_sj) {
        /* an artist's or album's own answer: its summary */
        plex_sum_came(j);
        if (j->body)
            FreeVec(j->body);
        FreeVec(j);
        return;
    }
    if (j == plex_dj || (j->orphan && j->seq == 0)) {
        /* a Plex track's own answer: its sample rate, for Now playing */
        if (j == plex_dj) {
            plex_dj = NULL;
            if (j->body && (plex_orig.rate = plex_stream_rate(j->body, j->len)) && np_on() &&
                mp_is_track())
                ui->np_rate();
        }
        if (j->body)
            FreeVec(j->body);
        FreeVec(j);
        return;
    }
    if (j == rq)
        rq = NULL;
    if (j == rq_more)
        rq_more = NULL;
    for (d = 0; d < depth && !j->orphan; d++)
        if (pages[d].radio && (pages[d].net_seq == j->seq || pages[d].more_seq == j->seq))
            p = &pages[d];
    if (p && p->plex && j->start) {
        /* a long list's letters, or a part of it: its placeholders filled */
        char m[120];
        p->more_seq = 0;
        if (j->start < 0) {
            plex_letters_came(p, j);
        } else if (j->parsed) {
            LONG k = (j->start - 1) / PLEX_PART, i, n = 0;
            LONG at = plex_lead(p) + p->plex_n0 + (j->start - 1 - PLEX_PART);
            for (i = 0; i < j->list.count && at + i < p->list.count; i++)
                if ((p->list.items[at + i].flags & ITEM_GONE) &&
                    list_set(&p->list, at + i, &j->list.items[i]))
                    n++;
            list_free(&j->list);
            j->parsed = FALSE;
            if (k < PLEX_PARTS)
                p->plex_parts[k >> 5] |= 1UL << (k & 31);
            p->plex_more++;
            sprintf(m, "%.40s: part %ld read (%ld entries from %ld), in %ld ms", p->title,
                    (long)k + 1, (long)n, (long)j->start - 1, (long)j->ms);
            amb_log("plex", m);
            np_mark_lists(FALSE);
            if (p->photos)
                plex_slides_label(p);       /* its count: these photos too */
            if (p == cur() && !pane_pending) {
                draw_title_count(p);
                draw_list_rows(p);
            }
        }
        if (j->body)
            FreeVec(j->body);
        FreeVec(j);
        plex_more_ask(p);
        return;
    }
    if (p && p->plex) {
        LONG n = 0, total = 0;
        char m[200];
        BOOL servers = !strcmp(p->base, "#gdm");
        p->net_seq = 0;
        if (j->parsed) {
            /* read already, in the network process: its list is the page's */
            list_free(&p->list);
            p->list = j->list;
            j->parsed = FALSE;
            n = j->n;
            total = j->total;
        } else if (j->body) {
            n = servers ? plex_fill_servers(j->body, j->len, &p->list)
                        : plex_fill(p->base, j->body, j->len, &p->list, &total);
        }
        if (n > 0 && plex_photo_sections(p->base)) {
            /* Pictures' photo libraries: kept, Pictures' rows made of them;
             * just one: its photos and albums here */
            char v[400], line[160], sec[64];
            LONG i;
            v[0] = 0;
            for (i = 0; i < p->list.count && i < 6; i++) {
                struct Item *a = &p->list.items[i];
                if (a->kind != IT_PLEXDIR)
                    continue;
                sprintf(line, "%s%.60s\t%.80s", v[0] ? "\n" : "", a->path, a->label);
                amb_strlcat(v, line, sizeof(v));
            }
            plex_set_photo_libs(v);
            pictures_root_again();
            if (n == 1) {
                amb_strlcpy(sec, p->list.items[0].path, sizeof(sec));
                amb_strlcpy(p->title, p->list.items[0].label, sizeof(p->title));
                list_free(&p->list);
                list_init(&p->list);
                sprintf(m, "photos: one photo library, %s", sec);
                amb_log("plex", m);
                plex_refetch(p, sec);
                if (j->body)
                    FreeVec(j->body);
                FreeVec(j);
                return;
            }
        }
        if (n == 1 && !strcmp(p->base, "/library/sections") && p->list.items[0].path[0] == '@') {
            /* one music library: its menu (Artists, Albums...) on this page,
             * not a "Music" folder inside Music */
            char sec[32];
            radio_field(&p->list.items[0], 0, sec, sizeof(sec));
            list_free(&p->list);
            list_init(&p->list);
            plex_section_menu(sec + 1, &p->list);
            plex_set_library(sec + 1, 1);
            music_root_again();
            amb_strlcpy(p->base, "@", sizeof(p->base));
            sprintf(m, "%s: one music library, %s", p->title, sec + 1);
            amb_log("plex", m);
            if (p == cur() && !pane_pending) {
                msg[0] = 0;
                draw_pane(TRUE);
                draw_footer();
            }
            if (j->body)
                FreeVec(j->body);
            FreeVec(j);
            return;
        }
        if (n == 1 && !servers && p->list.items[0].kind == IT_PLEXDIR && strstr(p->base, "/children")) {
            /* an artist with one album and nothing loose: the album itself */
            char type[16];
            struct Item *a = &p->list.items[0];
            radio_field(a, 1, type, sizeof(type));
            if (!strcmp(type, "album")) {
                char key[300], title[96];
                radio_field(a, 0, key, sizeof(key));
                amb_strlcpy(p->plex_label, a->label, sizeof(p->plex_label));
                amb_strlcpy(p->plex_meta, a->path, sizeof(p->plex_meta));
                sprintf(title, "%.44s - %.44s", p->title, a->label);
                amb_strlcpy(p->title, title, sizeof(p->title));
                list_free(&p->list);
                list_init(&p->list);
                amb_log("plex: one album", key);
                plex_refetch(p, key);
                if (j->body)
                    FreeVec(j->body);
                FreeVec(j);
                return;
            }
        }
        const char *gone = NULL;
        if (!j->body && j->status == 404 && plex.library[0]) {
            /* the library Music remembers is not on the server now (a new
             * server, or the library made again): forgotten, so Music
             * lists the server's libraries again */
            char lib[64];
            sprintf(lib, "/library/sections/%.40s/", plex.library);
            if (!strncmp(p->base, lib, strlen(lib))) {
                sprintf(m, "%s: library %.40s not on the server now: forgotten", p->title,
                        plex.library);
                amb_log("plex", m);
                plex_set_library("", 0);
                music_root_again();
                gone = _("That library is gone from the server: Music lists them again");
            }
        }
        if (!j->body || n <= 0) {
            const char *why = gone ? gone
                              : !j->body ? j->err
                              : n < 0 ? _("The server answered with something unexpected")
                              : !strcmp(p->base, "/library/sections") ? _("No music libraries there")
                              : plex_photo_sections(p->base) ? _("No photo libraries there")
                                                             : _("Nothing here");
            if (!gone && !j->body && strstr(j->err, " 401") && jf_key(p->base)) {
                /* Jellyfin's sign-in gone (signed out elsewhere): again there */
                why = _("The server wants you signed in: Settings > Networking > Jellyfin");
                action_item(p, why, "", ACT_GOJF, MARK_ARROW);
            } else if (!gone && !j->body && strstr(j->err, " 401")) {
                /* signing in is what it wants: the row goes where that is
                 * done (Settings > Networking > Plex: Sign in at plex.tv) */
                why = _("The server wants you signed in: Settings > Networking > Plex");
                action_item(p, why, "", ACT_GOPLEX, MARK_ARROW);
            } else {
                list_add(&p->list, why[0] ? why : _("Could not reach the Plex server"), "", "", IT_TEXT);
            }
            sprintf(m, "%s: %s", p->title, why);
        } else {
            if (servers)
                sprintf(m, "%s: %ld found in %ld ms", p->title, (long)n, (long)j->ms);
            else
                sprintf(m, "%s: %ld listed of %ld, %lu KB from %s in %ld ms, read in %lu ms",
                        p->title, (long)n, (long)total, (unsigned long)((j->len + 512) >> 10),
                        j->server, (long)j->ms, (unsigned long)j->ms_read);
            p->plex_more = 0;
            p->plex_total = 0;
            p->plex_letters = FALSE;
            memset(p->plex_parts, 0, sizeof(p->plex_parts));
            if (!servers && total > PLEX_PART && n > 0) {
                /* the rest: placeholders, the server's letters asked for
                 * now, then its parts (plex_more_ask) */
                struct Item ph;
                LONG i, lead = p->list.count - n;
                if (list_add(&p->list, "", "", "", IT_PLEXDIR)) {
                    p->list.items[p->list.count - 1].flags |= ITEM_GONE;
                    p->list.items[p->list.count - 1].mark = MARK_NONE;
                    ph = p->list.items[p->list.count - 1];
                    for (i = PLEX_PART + 1; i < total && list_append(&p->list, &ph); i++)
                        ;
                }
                p->plex_n0 = n;
                p->plex_total = PLEX_PART + (p->list.count - lead - n);
                p->plex_parts[0] = 1;
                p->plex_more = 1;
                plex_more_ask(p);
            }
            if (p->photos) {
                /* photos here: a slideshow of them, first */
                LONG i, k = 0;
                char label[96];
                struct Item *a;
                for (i = 0; i < p->list.count; i++)
                    if (p->list.items[i].kind == IT_PLEXPHOTO)
                        k++;
                if (k == 1)
                    strcpy(label, _("Show the photo"));
                else
                    sprintf(label, _("Slideshow of these %ld photos"), (long)k);
                if (k && (a = list_add_first(&p->list, label, "", "", IT_ACTION))) {
                    a->action = ACT_PLEXSLIDES;
                    a->mark = MARK_PLAY;
                }
            }
        }
        amb_log("plex", m);
        if (jf_key(p->base) && p->base[1] == 'V' && p->base[2] != 'm' && p->list.count == 1 &&
            p->list.items[0].kind == IT_PLEXDIR) {
            /* Jellyfin's one photo or video library: opened at once (as
             * one music library opens on its menu) */
            radio_field(&p->list.items[0], 0, again, sizeof(again));
            amb_log("jellyfin: its one library, opened", p->list.items[0].label);
            again_p = p;                    /* (asked for once this job is let go) */
        }
        np_mark_lists(FALSE);               /* what plays, marked in the new list too */
        plex_shortcut_fix(p);               /* a library's list: Now playing first */
        plex_pos_restore(p);                /* back where it was left */
        if (p == cur() && !pane_pending) {
            msg[0] = 0;
            draw_pane(TRUE);
            draw_footer();
        }
    } else if (p) {
        LONG n = 0, skipped = 0;
        char m[160];
        p->net_seq = 0;
        if (j->body)
            n = radio_fill(p->base, j->body, j->len, &p->list, https_ok(), &skipped);
        if (!j->body || n <= 0) {
            const char *why = !j->body ? j->err
                              : n < 0 ? "radio-browser.info answered with something unexpected"
                              : p->base[0] == '#' ? _("Nothing listed")
                                                  : _("No stations here that Ambiance can play");
            list_add(&p->list, why[0] ? why : "Could not reach radio-browser.info", "", "",
                     IT_TEXT);
            sprintf(m, "%s: %s", p->title, why);
            amb_log("radio", m);
        } else {
            sprintf(m, "%s: %ld listed, %ld left out, from %s in %ld ms", p->title, (long)n,
                    (long)skipped, j->server, (long)j->ms);
            amb_log("radio", m);
            plex_shortcut_fix(p);           /* Now playing first, while a station plays */
        }
        if (p == cur() && !pane_pending) {
            msg[0] = 0;
            draw_pane(TRUE);
            draw_footer();
        }
    }
    if (j->parsed)
        list_free(&j->list);                /* its page gone meanwhile */
    if (j->body)
        FreeVec(j->body);
    FreeVec(j);
    if (again_p)
        plex_refetch(again_p, again);
}

static void radio_collect(void)
{
    struct NetJob *j;
    while ((j = net_collect()))
        radio_done(j);
}

/* A page closing: its request, if still out, is given up. */
static void radio_page_gone(struct Page *p)
{
    if (p->radio && p->more_seq && rq_more && rq_more->seq == p->more_seq) {
        net_cancel(rq_more);
        rq_more->orphan = TRUE;
        rq_more = NULL;
    }
    if (p->radio && p->net_seq && rq && rq->seq == p->net_seq) {
        net_cancel(rq);
        rq->orphan = TRUE;                  /* freed when it comes back */
        rq = NULL;
        msg[0] = 0;
    }
    p->net_seq = 0;
    p->more_seq = 0;
}

/* Every request given up and the network process stopped (quitting, or a
 * game starting). */
static void radio_stop(void)
{
    int d;
    struct NetJob **held[3 + ART_KINDS];
    held[0] = &plex_dj;
    held[1] = &plex_sj;
    plink_stop();                       /* (signing in: given up) */
    held[2] = &plink.j;
    for (d = 0; d < ART_KINDS; d++)
        held[3 + d] = &art_nj[d];
    for (d = 0; d < 3 + ART_KINDS; d++)     /* the rest out given up too: net_stop frees them */
        if (*held[d]) {
            net_cancel(*held[d]);
            (*held[d])->orphan = TRUE;
            (*held[d])->seq = 0;
            *held[d] = NULL;
        }
    if (rq) {
        net_cancel(rq);
        rq->orphan = TRUE;
        rq = NULL;
    }
    if (rq_more) {
        net_cancel(rq_more);
        rq_more->orphan = TRUE;
        rq_more = NULL;
    }
    for (d = 0; d < depth; d++)
        pages[d].net_seq = pages[d].more_seq = 0;
    net_stop();
}

static void radio_open(struct Item *it)
{
    struct Page *p;
    struct NetJob *j;
    BOOL photos;
    if (it->flags & ITEM_DISABLED)
        return;
    if (it->flags & ITEM_GONE) {
        set_msg(_("Still being read from the server: a moment"));
        return;
    }
    if (it->kind == IT_PLEXDIR && it->path[0] == '@') {
        /* a music library: its own menu, nothing to ask yet; and the one
         * Music opens on from now */
        LONG libs = 0, i;
        struct Page *up = cur();
        for (i = 0; up && i < up->list.count; i++)
            if (up->list.items[i].kind == IT_PLEXDIR && up->list.items[i].path[0] == '@')
                libs++;
        plex_set_library(it->path + 1, (UWORD)(libs ? libs : 1));
        music_root_again();
        if (!(p = page_push(PT_LIST, it->label))) {
            set_msg(_("Too many levels open"));
            return;
        }
        p->plex = 1;
        p->radio = 1;
        plex_section_menu(it->path + 1, &p->list);
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    if (it->kind == IT_PLEXDIR && jf_key(it->path) && jf_is_menu(it->path)) {
        /* a Jellyfin music library: its own menu, made here */
        if (!(p = page_push(PT_LIST, it->label))) {
            set_msg(_("Too many levels open"));
            return;
        }
        p->plex = 1;
        p->radio = 1;
        radio_field(it, 0, p->base, sizeof(p->base));
        jf_menu(p->base, &p->list);
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    if (it->kind == IT_PLEXDIR && it->path[0] != '#' && !plex_server_for(it->path)) {
        if (jf_key(it->path))
            goto_jf_settings();
        else
            goto_plex_settings();           /* (no server: where one is chosen) */
        return;
    }
    if (it->kind == IT_PLEXDIR && jf_key(it->path))
        photos = jf_photo_key(it->path);    /* (the key says: Videos' folders are not photos') */
    else
        photos = cur() && (cur()->photos || cur()->pics);  /* Pictures' Plex pages */
    if (rq) {                               /* one out at a time: the last given up */
        net_cancel(rq);
        rq->orphan = TRUE;
        rq = NULL;
    }
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        set_msg(_("Not enough memory"));
        return;
    }
    if (!(p = page_push(PT_LIST, it->label))) {
        FreeVec(j);
        set_msg(_("Too many levels open"));
        return;
    }
    p->radio = 1;
    amb_strlcpy(p->base, it->path, sizeof(p->base));
    if (it->kind == IT_PLEXDIR) {
        p->plex = 1;
        p->photos = photos;
        radio_field(it, 0, p->base, sizeof(p->base));  /* its key; the rest is its details */
        if (strstr(it->path, "\talbum\t")) {
            /* an album: its details stay beside its tracks */
            amb_strlcpy(p->plex_label, it->label, sizeof(p->plex_label));
            amb_strlcpy(p->plex_meta, it->path, sizeof(p->plex_meta));
        }
        if (!strcmp(p->base, "#gdm")) {
            j->gdm = 1;
            if (plex.account[0] && https_ok()) {
                j->tls = 1;                 /* the account's servers too, from plex.tv */
                plex_tv_headers(j->extra, sizeof(j->extra), TRUE);
            }
            amb_strlcpy(msg, _("Looking for Plex servers...  Esc to give up"), sizeof(msg));
        } else {
            plex_job(j, p->base, 0);
            j->parse = 1;                   /* read in the network process */
            j->wait_s = PLEX_WAIT_S;        /* a NAS's server may think for a while */
            amb_strlcpy(j->pkey, p->base, sizeof(j->pkey));
            sprintf(msg, _("Asking %.40s...  Esc to give up"), plex_server_name(p->base));
        }
    } else {
        radio_set_max_kbps(rkbps_now());
        radio_request(p->base, https_ok(), j->path, sizeof(j->path));
        amb_strlcpy(msg, _("Asking radio-browser.info...  Esc to give up"), sizeof(msg));
    }
    j->seq = ++rq_seq;
    p->net_seq = j->seq;
    rq = j;
    draw_pane(TRUE);
    draw_footer();
    net_submit(j);
    if (j->done && !j->busy)                /* the process could not start: no answer to wait for */
        radio_done(j);
}

/* Music's own list made again (its library now known), where it is. */
static void music_root_again(void)
{
    struct Page *r = &pages[0];
    LONG sel;
    if (depth < 1 || !r->plex || r->radio || r->type != PT_LIST)
        return;
    sel = r->sel;
    list_free(&r->list);
    list_init(&r->list);
    build_music_root(r);
    r->sel = sel < r->list.count ? sel : 0;
    r->top = 0;
    if (r == cur() && !pane_pending) {
        draw_pane(TRUE);
        draw_footer();
    }
}

/* Pictures' Plex rows, put first: each photo library of the server as
 * last listed, or (none listed yet) the server, to list them. */
static int pictures_plex_rows(struct Page *p)
{
    char lines[400], label[160], *l, *e, *t;
    char keys[6][64], titles[6][96];
    struct Item *it;
    int n = 0, i;
    if (!caps.net || !plex.on || !plex.host[0]) {
        amb_log("pictures: no Plex rows", !caps.net ? "no TCP/IP stack" : !plex.on
                                           ? "Plex is turned off" : "no Plex server chosen");
        if (caps.net && (it = list_add_first(&p->list, !plex.on
                                             ? _("Plex photos: Plex is turned off (Settings > Networking > Plex)")
                                             : _("Plex photos: choose a server (Settings > Networking > Plex)"),
                                             "", "", IT_ACTION))) {
            it->action = ACT_GOPLEX;        /* (chosen: there) */
            it->mark = MARK_ARROW;
        }
        return 0;
    }
    amb_strlcpy(lines, plex_photo_libs(), sizeof(lines));
    for (l = lines; *l && n < 6; l = e) {
        e = strchr(l, '\n');
        if (e)
            *e++ = 0;
        else
            e = l + strlen(l);
        if (!(t = strchr(l, '\t')))
            continue;
        *t++ = 0;
        amb_strlcpy(keys[n], l, sizeof(keys[n]));
        amb_strlcpy(titles[n], t, sizeof(titles[n]));
        n++;
    }
    {
        char m[120];
        sprintf(m, "%ld photo librar%s known on %.60s", (long)n, n == 1 ? "y" : "ies",
                plex.name[0] ? plex.name : plex.host);
        amb_log("pictures: Plex rows", m);
    }
    if (!n) {
        sprintf(label, _("Plex photos on %.80s"), plex.name[0] ? plex.name : plex.host);
        if ((it = list_add_first(&p->list, label, "/library/sections?photos=1", "", IT_PLEXDIR)))
            it->mark = MARK_ARROW;
        return 1;
    }
    for (i = n - 1; i >= 0; i--) {
        sprintf(label, _("Plex: %.100s"), titles[i]);
        if ((it = list_add_first(&p->list, label, keys[i], "", IT_PLEXDIR)))
            it->mark = MARK_ARROW;
    }
    return n;
}

/* Pictures' own list made again (the server's photo libraries now known). */
static void pictures_root_again(void)
{
    struct Page *r = &pages[0];
    LONG sel, n;
    if (depth < 1 || shown_mod != MOD_PICTURES || r->type != PT_LIST || !r->pics)
        return;
    sel = r->sel;
    list_free(&r->list);
    list_init(&r->list);
    {
        const char *src;
        n = pics_root_fill(r, &src);
        if (n != -2)
            pics_root_empty(r, n, src);
    }
    jf_section_row(r, 'p', TRUE);
    pictures_plex_rows(r);
    r->sel = sel < r->list.count ? sel : 0;
    r->top = 0;
}

/* A Plex page asked again, for key (an artist's one album, say). */
static void plex_refetch(struct Page *p, const char *key)
{
    struct NetJob *j;
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return;
    amb_strlcpy(p->base, key, sizeof(p->base));
    plex_job(j, key, 0);
    j->parse = 1;
    j->wait_s = PLEX_WAIT_S;
    amb_strlcpy(j->pkey, key, sizeof(j->pkey));
    j->seq = ++rq_seq;
    p->net_seq = j->seq;
    rq = j;
    if (p == cur() && !pane_pending) {
        draw_pane(TRUE);
        draw_footer();
    }
    net_submit(j);
    if (j->done && !j->busy)
        radio_done(j);
}

/* A Plex track's sample rate, asked of the server (its own key): an
 * album's answer has its bitrate and channels, not that. */
static void plex_ask_rate(const char *key)
{
    struct NetJob *j;
    if (plex_dj) {
        net_cancel(plex_dj);
        plex_dj->orphan = TRUE;             /* freed when it comes back */
        plex_dj->seq = 0;
        plex_dj = NULL;
    }
    if (!plex.host[0] || !net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return;
    amb_strlcpy(j->host, plex.host, sizeof(j->host));
    j->port = plex.port;
    plex_request(key, j->path, sizeof(j->path));
    j->seq = 0;
    plex_dj = j;
    net_submit(j);
    if (j->done && !j->busy)
        radio_done(j);
}

/* The server's picture thumb made w x h, for kind: TRUE, and its file in
 * file, when it is kept already; else asked for (whatever kind asked for
 * before given up), and art_ready comes when it is here. */
BOOL art_fetch(int kind, const char *thumb, WORD w, WORD h, char *file)
{
    struct NetJob *j;
    char req[400];
    if (art_nj[kind]) {
        net_cancel(art_nj[kind]);
        art_nj[kind]->orphan = TRUE;        /* freed when it comes back */
        art_nj[kind]->seq = 0;
        art_nj[kind] = NULL;
    }
    file[0] = 0;
    if (!thumb[0] || !plex_server_for(thumb) || w < 8 || h < 8)
        return FALSE;
    if (plex_art(thumb, w, h, req, sizeof(req), file, 120))
        return TRUE;
    if (kind != ART_AHEAD && art_nj[ART_AHEAD]) {
        if (kind == ART_PV && !strcmp(art_want[ART_AHEAD], file)) {
            /* on its way already, fetched ahead: this kind's now */
            art_nj[kind] = art_nj[ART_AHEAD];
            amb_strlcpy(art_want[kind], file, sizeof(art_want[kind]));
            art_asked[kind] = art_asked[ART_AHEAD];
            art_nj[ART_AHEAD] = NULL;
            return FALSE;
        }
        /* one fetched ahead is no reason for this one to wait behind it */
        net_cancel(art_nj[ART_AHEAD]);
        art_nj[ART_AHEAD]->orphan = TRUE;   /* freed when it comes back */
        art_nj[ART_AHEAD]->seq = 0;
        art_nj[ART_AHEAD] = NULL;
    }
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    if (jf_key(thumb)) {
        amb_strlcpy(j->host, jf.host, sizeof(j->host));
        j->port = jf.port;                  /* (its picture's URL has its key) */
    } else {
        amb_strlcpy(j->host, plex.host, sizeof(j->host));
        j->port = plex.port;
    }
    amb_strlcpy(j->path, req, sizeof(j->path));
    j->seq = 0;
    amb_strlcpy(art_want[kind], file, sizeof(art_want[kind]));
    art_asked[kind] = time_ms();
    art_nj[kind] = j;
    net_submit(j);
    if (j->done && !j->busy)
        radio_done(j);
    return FALSE;
}

/* Now playing's cover let go (another track, the page gone, the screen). */
static void np_art_free(void)
{
    struct LJob *j = np_aj;
    np_aj = NULL;
    np_art_drawn = FALSE;
    if (!j)
        return;
    if (j->busy && !g.truecolor) {
        ldr_orphan(j);                      /* holds no pens: freed when it ends */
        return;
    }
    if (j->busy)
        ldr_wait(j);
    pic_dispose(j->obj);
    thumb_free(j->thumb);
    FreeVec(j);
}

/* The size Now Playing's cover is asked for and made: the details pane's
 * where it fits (the same file, moved across as it is), else its square's
 * own (no pane, on lowres: 0 there). */
static WORD np_art_size(void)
{
    WORD s = plex_art_side();
    return s >= 8 && s <= np.art_s ? s : np.art_s;
}

/* The cover's file to the loader, made for the square. */
void np_art_load(const char *file)
{
    if (view_own_pictures())
        return;     /* a layout: the classic page's cover (read into its pens) is
                     * for nothing; the page reads it when it is drawn again (np_draw) */
    struct LJob *j;
    np_art_free();
    if (!np.art_s || !pv_pictures() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return;
    j->type = LJ_SHOT;
    j->mode = SHOT_FILE;
    amb_strlcpy(j->path, file, sizeof(j->path));
    j->scr = g.scr;
    j->remap = g.truecolor;
    j->want_pic = FALSE;
    if (gfx_thumbs(&g)) {
        j->remap = FALSE;
        j->tw = j->th = np_art_size();
        j->tall = gfx_mode_tall(g.mode);
        j->maxcol = gfx_thumb_colours(&g);
        if (plex_colours() && j->maxcol > plex_colours())
            j->maxcol = plex_colours();
    }
    np_aj = j;
    ldr_submit_first(j);
}


/* A Plex track starting: its cover asked for (or loaded, if kept). */
static void np_art_start(const struct Item *it)
{
    static char shown[160];
    char thumb[160], file[120];
    WORD s;
    if (it)
        radio_field(it, 9, thumb, sizeof(thumb));
    else
        thumb[0] = 0;
    if (it && !thumb[0])                    /* (Plex gave the track none: its album's) */
        amb_strlcpy(thumb, np_album_thumb, sizeof(thumb));
    if (!strcmp(thumb, shown) && thumb[0] &&
        (np_aj || art_nj[ART_NP] || (g.pic_w && !strcmp(np_art_file, pv_onscr.key))))
        return;                             /* the same album: its cover stays as it is */
    amb_strlcpy(shown, thumb, sizeof(shown));
    np_art_free();
    np_art_file[0] = 0;
    np_art_key[0] = 0;
    if (thumb[0]) {
        np_art_key[0] = '\x04';            /* (LV_ART_PLEX) */
        amb_strlcpy(np_art_key + 1, thumb, sizeof(np_art_key) - 1);
    }
    np_art_want = thumb[0] != 0;            /* a cover to show: its square kept */
    np_layout();                            /* the square's size */
    ui->np_art_room(!thumb[0]);             /* (none: the last one goes) */
    if (!np.art_s)
        return;
    /* the pane's size where it fits: the same file, and the same picture,
     * which can then go across as it is (pic_take) */
    s = np_art_size();
    if (art_fetch(ART_NP, thumb, s, s, file)) {
        amb_strlcpy(np_art_file, file, sizeof(np_art_file));
        if (np_on() && pic_take(file, np.art_x, np.art_y, np.art_s, np.art_h)) {
            np_art_drawn = TRUE;            /* the one on screen: moved */
            return;
        }
        if (!np_on() && g.pic_w && !strcmp(file, pv_onscr.key))
            return;                         /* in the pane: taken when the page opens */
        np_art_load(file);
    } else {
        amb_strlcpy(np_art_file, file, sizeof(np_art_file));     /* asked for: art_ready */
    }
}


/* What a Plex page's details pane shows for entry idx: the entry (an
 * artist, an album) or, on an album's own page, the album. */
BOOL plex_pv_src(struct Page *p, LONG idx, struct Item *out)
{
    memset(out, 0, sizeof(*out));
    if (idx >= 0 && idx < p->list.count &&
        ((p->list.items[idx].kind == IT_PLEXDIR && strchr(p->list.items[idx].path, '\t')) ||
         p->list.items[idx].kind == IT_PLEXPHOTO)) {
        *out = p->list.items[idx];
        return TRUE;
    }
    if (p->plex_meta[0]) {
        out->label = p->plex_label;
        out->path = p->plex_meta;
        out->kind = IT_PLEXDIR;
        return TRUE;
    }
    return FALSE;
}

/* The cover's square: as much of the pane's top half as fits, less on a
 * slower Amiga (a 68030 decodes and reduces each pixel of a JPEG: 160
 * pixels a side is half the work of 220). */
WORD plex_art_side(void)
{
    WORD w = aside_width() - U(4), h = (g.footer_y - 2 - g.list_top) / 2, s = w < h ? w : h;
    WORD cap = plex_tier() == 0 ? 160 : plex_tier() == 1 ? 224 : 400;
    return s > cap ? cap : s;
}


struct SumKept sum_kept[SUM_KEPT];
static int sum_next;
char sum_want[120];          /* its key */
WORD sum_y;                  /* where its text goes in the pane */

/* The item's own key: /library/metadata/<n> (its list key without
 * /children); FALSE where it has none (not an artist or album). */
BOOL sum_key(const struct Item *it, char *out, LONG max)
{
    char *c;
    radio_field(it, 0, out, max);
    if (strncmp(out, "/library/metadata/", 18))
        return FALSE;
    if ((c = strstr(out, "/children")))
        *c = 0;
    return TRUE;
}


/* Its answer: kept, and written in the pane if it still shows that one. */
static void plex_sum_came(struct NetJob *j)
{
    struct Page *p = cur();
    struct Item src;
    char text[300], k[120];
    int i = sum_next;
    plex_sj = NULL;
    text[0] = 0;
    if (j->body && j->status == 200)
        plex_summary(j->body, j->len, text, sizeof(text));
    else if (!j->body)
        return;                             /* not reached: asked again next time */
    amb_strlcpy(sum_kept[i].key, sum_want, sizeof(sum_kept[i].key));
    amb_strlcpy(sum_kept[i].text, text, sizeof(sum_kept[i].text));
    sum_next = (sum_next + 1) % SUM_KEPT;
    if (!text[0] || !p || !p->plex || p != det.page || p->sel != det.sel || !det.shown ||
        pane_pending || !plex_pv_src(p, p->sel, &src) || !sum_key(&src, k, sizeof(k)) ||
        strcmp(k, sum_want))
        return;
    ui->pane_summary(text);
}


/* The picture on screen (anywhere: Now playing's square) is the one entry
 * idx shows, and fits the pane: plex_pv_draw moves it rather than clear it. */
BOOL plex_movable(struct Page *p, LONG idx)
{
    struct Item src;
    char thumb[160], req[400], file[120];
    WORD s = plex_art_side();
    if (!g.pic_w || !pv_onscr.key[0] || !pv_pictures() || !plex_pv_src(p, idx, &src))
        return FALSE;
    radio_field(&src, 2, thumb, sizeof(thumb));
    if (!thumb[0])
        return FALSE;
    plex_art(thumb, s, s, req, sizeof(req), file, sizeof(file));
    return !strcmp(file, pv_onscr.key) && g.pic_w <= aside_width() - U(4) && g.pic_h <= s;
}


/* Entry idx's picture, at the preview's size, kept in RAM already. */
static BOOL plex_art_kept(struct Page *p, LONG idx)
{
    struct Item src;
    char thumb[160], req[400], file[120];
    WORD s = plex_art_side();
    if (idx < 0 || idx >= p->list.count || !pv_pictures() || !plex_pv_src(p, idx, &src))
        return FALSE;
    radio_field(&src, 2, thumb, sizeof(thumb));
    return thumb[0] && plex_server_for(thumb) && plex_art(thumb, s, s, req, sizeof(req), file, sizeof(file));
}

/* While the selection rests, its shown and nothing else asked of the
 * server: the neighbours' pictures fetched into RAM, one at a time, the
 * way it was moving first (1 and 2 on, 1 back, 3 on, 2 back), so moving
 * to one shows it at once. Anything the user asks for gives it up
 * (art_fetch). */
static void plex_ahead(struct Page *p)
{
    static struct Page *ap;
    static LONG asel = -1;
    static int adir = 1, astep;
    static const signed char off[] = { 1, 2, -1, 3, -2 };
    struct Item src;
    char thumb[160], file[120];
    WORD s;
    if (!p->plex || p->type != PT_LIST || !has_aside(p) || !pv_pictures() || !g.pic_w ||
        hold_active() || ov.on || pane_pending)
        return;
    if (!det.shown || det.page != p || det.sel != p->sel || art_nj[ART_PV] || art_nj[ART_AHEAD] ||
        plex_sj)
        return;                             /* not at rest, or the server busy for it */
    if (view_own_pictures())
        return;                             /* (a layout: it asks for its own, at its sizes) */
    if (AvailMem(MEMF_ANY) < 2UL << 20)
        return;                             /* not ahead with under 2 MB free (RAM:'s
                                             * pictures are trimmed to a budget: plex.c) */
    if (ap != p || asel != p->sel) {
        if (ap == p && asel >= 0 && p->sel != asel)
            adir = p->sel > asel ? 1 : -1;  /* the way it was going */
        ap = p;
        asel = p->sel;
        astep = 0;
    }
    s = plex_art_side();
    while (astep < (int)sizeof(off)) {
        LONG i = p->sel + off[astep] * adir;
        astep++;
        if (i < 0 || i >= p->list.count || !plex_pv_src(p, i, &src))
            continue;
        radio_field(&src, 2, thumb, sizeof(thumb));
        if (!thumb[0] || art_fetch(ART_AHEAD, thumb, s, s, file))
            continue;                       /* none, or kept already */
        return;                             /* asked: the next once it has come */
    }
}


/* A picture of the server's is here (kept in file). */
static void art_ready(int kind, const char *file)
{
    struct Page *p = cur();
    if (kind == ART_AHEAD)
        return;                             /* kept in RAM: shown if it is moved to */
    if (kind == ART_LAYOUT) {
        lv_art_came = TRUE;                 /* the layout asks again, and finds it */
        return;
    }
    if (kind == ART_VW || kind == ART_VWNEXT) {
        vw_keep(file);
        if (kind == ART_VWNEXT && vw_list && vw_cur >= 0)
            vw_ahead(vw_list, vw_cur, vw_dir);  /* decoded ahead now it is here */
        return;
    }
    if (kind == ART_NP) {
        if (mp_is_track())
            np_art_load(file);
        return;
    }
    if (view_own_pictures())
        return;                             /* (the classic pane's picture: a layout draws) */
    if (p && p->plex && p == det.page && p->sel == det.sel && det.shown && !det.sj && det.has_box &&
        !pane_pending) {
        pv_colour_cap = plex_colours();
        det.sj = det_request(LJ_SHOT, SHOT_FILE, file, FALSE, TRUE);
        pv_colour_cap = 0;
        if (det.sj && det.sj->done)
            det_progress(p);
    }
}

/* A server found: kept as the one to use, and its libraries shown. */
static void plex_choose(struct Item *it)
{
    char hp[120], name[96], host[64];
    struct Item tmp;
    char *c;
    UWORD port = 32400;
    radio_field(it, 0, hp, sizeof(hp));
    radio_field(it, 1, name, sizeof(name));
    amb_strlcpy(host, hp, sizeof(host));
    if ((c = strchr(host, ':'))) {
        LONG v = 0;
        const char *d = c + 1;
        *c = 0;
        while (*d >= '0' && *d <= '9')
            v = v * 10 + (*d++ - '0');
        if (v > 0 && v < 65536)
            port = (UWORD)v;
    }
    plex_set_server(host, port, name);
    radio_field(it, 2, name, sizeof(name));
    if (name[0])
        plex_set_token(name);               /* the account's server: its own token */
    amb_log("plex: server chosen", hp);
    if (depth >= 2 && menu_sel >= 0 && menu[menu_sel] == MOD_SETTINGS) {
        /* chosen in Settings > Plex: back there, the row naming it */
        page_pop();
        plex_settings_again(TRUE);
        set_msg(_("Plex server chosen"));
        return;
    }
    if (depth >= 2 && pages[depth - 2].plex && depth == 2) {
        /* Music's own list: its Plex row now names this server */
        struct Page *up = &pages[depth - 2];
        list_free(&up->list);
        list_init(&up->list);
        build_music_root(up);
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.label = name[0] ? name : host;
    tmp.path = (char *)"/library/sections";
    tmp.kind = IT_PLEXDIR;
    radio_open(&tmp);
}

/* About > Plex in Ambiance: what it plays, and how to connect. */
static void plex_help(void)
{
    struct Page *p = page_push(PT_TEXT, _("Plex in Ambiance"));
    char line[200];
    if (!p)
        return;
    p->plexmark = 1;                        /* "Works with" Plex's logo, at the bottom */
    text_para(p, _("Ambiance plays music from a Plex Media Server on your network: its "
                   "music libraries, by artist and album, with their covers. The server makes "
                   "each track an MP3 this Amiga can play: 22kHz mono for a 68020/030, 24kHz "
                   "stereo for a 68040/060, more for a fast one, and its best quality when an "
                   "MHI driver decodes MP3 (Settings > Sound). On a fast Amiga, or with MHI, "
                   "turning off Settings > Networking > Plex > Request lower quality audio "
                   "plays MP3 files as stored, and on a fast Amiga FLAC files too where "
                   "flacdec.library is installed; other formats are converted either way. "
                   "Settings > Networking > "
                   "Plex > Plex turns it all off."));
    text_line(p, "");
    text_para(p, _("Finding a server: Settings > Networking > Plex > Plex server asks the "
                 "network (Plex's GDM); choose one and it is kept. Or set its address from the Shell: "
                 "SetEnv SAVE Ambiance/PlexServer 192.168.1.10:32400"));
    text_line(p, "");
    text_para(p, _("Signing in: with AmiSSL 5 installed and Settings > Networking > HTTPS on, "
                   "Settings > Networking > Plex > Sign in at plex.tv shows a code to enter at "
                   "plex.tv/link on a phone or computer. The account's servers are then listed "
                   "with those found on the network, each with its own token. Without AmiSSL, "
                   "set a server's token from the Shell: find it in Plex Web (an item's Get "
                   "Info > View XML: the X-Plex-Token in the address), then SetEnv SAVE "
                   "Ambiance/PlexToken <token>."));
    text_line(p, "");
    sprintf(line, _("Server: %s"), plex.host[0] ? plex.host : _("none chosen"));
    text_line(p, line);
    text_line(p, plex.token[0] ? _("Token: set") : _("Token: not set"));
    if (plex.account[0]) {
        sprintf(line, _("plex.tv: signed in as %s"), plex.user[0] ? plex.user : "?");
        text_line(p, line);
    } else {
        text_line(p, _("plex.tv: not signed in"));
    }
    {
        /* what plays the tracks here: MP3, FLAC, and what they come out of */
        int i;
        const char *mp3 = NULL;
        for (i = 0; i < nmhi && !mp3; i++)
            if (mhi_sel[0] && !strcmp(mhi_sel, mhi_names[i]))
                mp3 = mhi_names[i];
        if (mp3)
            sprintf(line, _("MP3: decoded by the MHI driver %.40s"), mp3);
        else
            amb_strlcpy(line, own_mpega ? _("MP3: decoded by ambmpega.library, Ambiance's own")
                               : caps.mpega_zz ? _("MP3: decoded on the ZZ9000 (its mpega.library)")
                               : caps.mp3_mpega ? _("MP3: decoded by mpega.library")
                               : _("MP3: no decoder (mpega.library or MHI)"),
                         sizeof(line));
        text_line(p, line);
        text_line(p, !caps.flacdec ? _("FLAC: converted to MP3 (flacdec.library is not installed)")
                     : plex_tier() < 2 ? _("FLAC: converted to MP3 (too slow to decode here)")
                     : plex.convert ? _("FLAC: converted to MP3 (lower quality audio is on)")
                     : _("FLAC: played as stored, by flacdec.library"));
        text_line(p, sound_ahi ? _("Sound: AHI, 16-bit") : sound_p14 ? _("Sound: Paula, 14-bit")
                                                          : _("Sound: Paula, 8-bit"));
    }
    draw_pane(TRUE);
    draw_footer();
}

/* About > Jellyfin in Ambiance: what it plays, and how to connect. */
static void jf_help(void)
{
    struct Page *p = page_push(PT_TEXT, _("Jellyfin in Ambiance"));
    char line[200];
    if (!p)
        return;
    p->plexmark = 2;                        /* "Works with" Jellyfin, at the bottom */
    text_para(p, _("Ambiance plays music, shows photos and plays films from a Jellyfin server "
                   "on your network (jellyfin.org: free software). Its music libraries are listed "
                   "by artist and album with their covers, and the server makes each track an MP3 "
                   "this Amiga can play, as for Plex. Its photos are made the size of the screen "
                   "by the server. Its films are made MPEG for this Amiga: MPEG-1 320 pixels wide "
                   "for the CPU to decode, or MPEG-2 up to 720 x 576 when the ZZ9000 decodes "
                   "(Settings > Networking > Jellyfin > Picture size). Films stream from the "
                   "server as they play: nothing is kept on this Amiga."));
    text_line(p, "");
    text_para(p, _("Ambiance is developed mainly with AI (Claude, by Anthropic), guided and "
                   "tested by its author. It is not an official Jellyfin client."));
    text_line(p, "");
    text_para(p, _("Finding a server: Settings > Networking > Jellyfin > Look for servers on "
                   "this network, or Type a server's address (as 192.168.1.10:8096)."));
    text_line(p, "");
    text_para(p, _("Signing in: Settings > Networking > Jellyfin > Sign in asks for a Jellyfin "
                   "user's name and password. The password is sent once and not kept; the "
                   "server's token for this Amiga is kept, until Sign out. A user of its own for "
                   "the Amiga, in the server's Dashboard > Users, is a good idea."));
    text_line(p, "");
    sprintf(line, _("Server: %s"), jf.host[0] ? (jf.name[0] ? jf.name : jf.host) : _("none chosen"));
    text_line(p, line);
    if (jf.token[0]) {
        sprintf(line, _("Signed in as %s"), jf.user[0] ? jf.user : "?");
        text_line(p, line);
    } else {
        text_line(p, _("Not signed in"));
    }
    draw_pane(TRUE);
    draw_footer();
}

/* A station's page: what radio-browser.info says of it. */
static void radio_station(struct Item *it)
{
    struct Page *p = page_push(PT_TEXT, it->label);
    char f[640], line[600];
    LONG br;
    if (!p)
        return;
    radio_field(it, 6, f, sizeof(f));
    br = 0;
    {
        const char *s = f;
        while (*s >= '0' && *s <= '9')
            br = br * 10 + (*s++ - '0');
    }
    radio_field(it, 5, line, sizeof(line));
    if (br > 0) {
        sprintf(f, "%s, %ldkbps", line, (long)br);
        amb_strlcpy(line, f, sizeof(line));
    }
    sprintf(f, _("Format:   %s"), line);
    text_line(p, f);
    radio_field(it, 1, line, sizeof(line));
    if (line[0]) {
        sprintf(f, _("Country:  %s"), line);
        text_line(p, f);
    }
    radio_field(it, 4, line, sizeof(line));
    sprintf(f, _("Votes:    %s"), line[0] ? line : "0");
    text_line(p, f);
    radio_field(it, 2, line, sizeof(line));
    if (line[0]) {
        sprintf(f, _("Tags:     %s"), line);
        text_para(p, f);
    }
    radio_field(it, 3, line, sizeof(line));
    if (line[0]) {
        sprintf(f, _("Homepage: %s"), line);
        text_para(p, f);
    }
    radio_field(it, 0, line, sizeof(line));
    sprintf(f, _("Stream:   %s"), line);
    text_para(p, f);
    text_line(p, "");
    text_para(p, _("Fire on the station in its list plays it."));
    draw_pane(TRUE);
    draw_footer();
}

/* Settings > Folders > (a kind) > Add a folder: asl.library's requester,
 * for folders only, on Ambiance's screen. */
static void folder_add(int k)
{
    static const char *const req[FS_COUNT] = { N_("A folder of MP3 or FLAC files"),
        N_("A folder of modules"), N_("A folder of pictures"), N_("A folder of animations") };
    static const char *const added[FS_COUNT] = { N_("Folder added: its music is under Music"),
        N_("Folder added: its modules are under Mods"),
        N_("Folder added: its pictures are under Pictures"),
        N_("Folder added: its animations are under Videos") };
    struct Folders *f = &fs[k];
    struct FileRequester *fr;
    char path[300];
    BOOL db, got = FALSE;
    int i;
    if (f->n >= FS_DIRS) {
        set_msg(_("Eight folders at most: take one out first"));
        return;
    }
    if (!AslBase && !(AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 38))) {
        set_msg("asl.library (version 38) is needed to choose a folder");
        return;
    }
    db = gfx_dbuf_active();
    if (db)
        gfx_dbuf_stop(&g);                  /* the requester draws on the screen shown */
    fr = (struct FileRequester *)AllocAslRequestTags(
        ASL_FileRequest, ASLFR_Window, (ULONG)g.win, ASLFR_SleepWindow, TRUE,
        ASLFR_TitleText, (ULONG)_(req[k]), ASLFR_PositiveText, (ULONG)_("Add"),
        ASLFR_DrawersOnly, TRUE, ASLFR_InitialDrawer,
        (ULONG)(f->n ? f->dir[f->n - 1] : "SYS:"), TAG_DONE);
    if (fr && AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
        /* made whole (a name typed may be relative to where Ambiance runs) */
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR old = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        if ((l = Lock((CONST_STRPTR)fr->fr_Drawer, SHARED_LOCK))) {
            got = NameFromLock(l, (STRPTR)path, sizeof(f->dir[0]) - 1) != 0;
            UnLock(l);
        }
        me->pr_WindowPtr = old;
        if (!got)
            set_msg(_("That folder could not be found"));
    }
    if (fr)
        FreeAslRequest(fr);
    if (db)
        gfx_dbuf_start(&g);
    if (got) {
        for (i = 0; i < f->n; i++)
            if (!amb_stricmp(f->dir[i], path))
                break;
        if (i < f->n) {
            set_msg(_("That folder is there already"));
        } else {
            amb_strlcpy(f->dir[f->n++], path, sizeof(f->dir[0]));
            save_folders(k);
            settings_rebuild();
            if (cur() && cur()->setp == SETP_FOLDSET)
                cur()->sel = f->n;          /* on the folder added */
            set_msg(_(added[k]));
        }
    }
    draw_all();                             /* where the requester was */
}

/* Settings > Folders > iGame > Choose iGame's drawer: asl.library's
 * requester; the drawer is kept only if a gameslist.csv is in it. */
static void igame_choose(void)
{
    struct FileRequester *fr;
    char path[256];
    BOOL db, got = FALSE;
    if (!AslBase && !(AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 38))) {
        set_msg("asl.library (version 38) is needed to choose a folder");
        return;
    }
    db = gfx_dbuf_active();
    if (db)
        gfx_dbuf_stop(&g);
    fr = (struct FileRequester *)AllocAslRequestTags(
        ASL_FileRequest, ASLFR_Window, (ULONG)g.win, ASLFR_SleepWindow, TRUE,
        ASLFR_TitleText, (ULONG)_("iGame's drawer (with its gameslist.csv)"),
        ASLFR_PositiveText, (ULONG)_("Use"), ASLFR_DrawersOnly, TRUE, ASLFR_InitialDrawer,
        (ULONG)(ig_setting[0] && !ig_off() ? ig_setting : "SYS:"), TAG_DONE);
    if (fr && AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR old = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        if ((l = Lock((CONST_STRPTR)fr->fr_Drawer, SHARED_LOCK))) {
            got = NameFromLock(l, (STRPTR)path, sizeof(path) - 1) != 0;
            UnLock(l);
        }
        me->pr_WindowPtr = old;
        if (!got)
            set_msg(_("That folder could not be found"));
    }
    if (fr)
        FreeAslRequest(fr);
    if (db)
        gfx_dbuf_start(&g);
    if (got) {
        if (!ig_folder_ok(path))
            set_msg(_("No gameslist.csv there: choose iGame's own drawer, after a scan in iGame"));
        else {
            save_setting(IG_VAR, path);
            igame_start(FALSE);
            settings_rebuild();
            ig_said();
        }
    }
    draw_all();
}

static void act_action(struct Item *it)
{
    if (it->flags & ITEM_DISABLED) {
        if (it->action == ACT_STYLES)
            set_msg(_("Style: Smooth needs an RTG screen"));
        else if (it->action == ACT_PLEXCONV)
            set_msg(_("This Amiga plays Plex's lower quality: the files as stored are too much"));
        else if (it->action >= ACT_NIC0 && it->action <= ACT_NIC_END)
            set_msg(_("That interface is not up"));
        else if (it->action == ACT_SND_AHI)
            set_msg("AHI is not installed (DEVS:ahi.device)");
        else if (it->action == ACT_SND_MHI)
            set_msg(_("MHI is not installed (its drivers in LIBS:MHI)"));
        else if (it->action == ACT_SND_MPEGA)
            set_msg("mpega.library is not installed: MP3s cannot be played");
        else if (it->action == ACT_SND_OWNMPEGA)
            set_msg(_("ambmpega.library is not in Ambiance's Libs"));
        else if (it->action >= ACT_SND_MQ0 && it->action <= ACT_SND_MQ_END)
            set_msg(_("MHI and the ZZ9000's mpega.library decode MP3 at their best"));
        return;
    }
    if (it->action == ACT_SCREENMODES)
        open_screenmodes();
    else if (it->action == ACT_WINDOW)
        toggle_window();
    else if (it->action == ACT_TSIZES)
        open_settings_page(_("Text size"), build_tsizes);
    else if (it->action == ACT_FONTS)
        open_settings_page(_("Fonts"), build_font_settings);
    else if (it->action == ACT_COLOURS)
        open_settings_page(_("Colours"), build_colours);
    else if (it->action == ACT_LAYOUTS)
        open_settings_page(_("Layout"), lv_build_page);
    else if (it->action >= ACT_LAYOUT0 && it->action <= ACT_LAYOUT_END)
        lv_pick(it->action - ACT_LAYOUT0);
    else if (it->action >= ACT_COLOUR0 && it->action <= ACT_COLOUR_END)
        apply_colours(it->action - ACT_COLOUR0);
    else if (it->action >= ACT_TSIZE0 && it->action <= ACT_TSIZE_END)
        apply_tsize(it->action - ACT_TSIZE0);
    else if (it->action == ACT_STYLES)
        open_styles();
    else if (it->action == ACT_SYSINFO)
        open_sysinfo();
    else if (it->action == ACT_CREDITS)
        open_credits();
    else if (it->action == ACT_DEBUG)
        toggle_debug();
    else if (it->action == ACT_JUMP)
        toggle_jump();
    else if (it->action == ACT_VISUAL) {
        next_visual();
        plex_settings_again(FALSE);
    } else if (it->action == ACT_FULLVISHELP) {
        open_settings_page(_("Full screen visual"), build_fullvis_page);
    } else if (it->action == ACT_FULLVIS) {
        fullvis_next();
        plex_settings_again(FALSE);
    } else if (it->action == ACT_TRANS) {
        view_transition = (view_transition + 1) % VT_COUNT;
        save_setting(TRANS_VAR, trans_keys[view_transition]);
        plex_settings_again(FALSE);
    } else if (it->action == ACT_ANIMSCR) {
        anim_classic = !anim_classic;
        save_setting(ANIMSCR_VAR, anim_classic ? "own" : "");
        plex_settings_again(FALSE);
    } else if (it->action == ACT_ZZVIDEO) {
        video_cpu = !video_cpu;
        save_setting(VDEC_VAR, video_cpu ? "CPU" : "");
        plex_settings_again(FALSE);
    } else if (it->action == ACT_VOVERLAY) {
        video_no_overlay = !video_no_overlay;
        save_setting(VOVL_VAR, video_no_overlay ? "off" : "");
        plex_settings_again(FALSE);
    }
    else if (it->action == ACT_LANGSET)
        open_settings_page(_("Language"), build_lang_settings);
    else if (it->action == ACT_LANGWB || it->action == ACT_LANGEN ||
             (it->action >= ACT_LANG0 && it->action < ACT_LANG0 + amb_lang_count()))
        choose_language(it->action == ACT_LANGWB ? "" : it->action == ACT_LANGEN
                        ? "english" : amb_lang_name(it->action - ACT_LANG0));
    else if (it->action == ACT_DBUF)
        toggle_dbuf();
    else if (it->action == ACT_DEPTHS)
        open_settings_page(_("Colour depth"), build_depths);
    else if (it->action == ACT_DEPTH16 || it->action == ACT_DEPTH32)
        apply_depth(it->action == ACT_DEPTH32 ? 32 : 16);
    else if (it->action == ACT_LVNATIVE)
        toggle_lv_native();
    else if (it->action == ACT_HTTPS)
        toggle_https();
    else if (it->action == ACT_PLEXCONV)
        toggle_plex_convert();
    else if (it->action == ACT_PLEXLINK)
        plink_start();
    else if (it->action == ACT_PLEXOUT)
        plink_sign_out();
    else if (it->action == ACT_PLEXON)
        toggle_plex_on();
    else if (it->action == ACT_PLEXSET)
        open_plex_settings();
    else if (it->action == ACT_GOPLEX)
        goto_plex_settings();
    else if (it->action == ACT_JFSET)
        open_settings_page("Jellyfin", build_jf_settings);
    else if (it->action == ACT_JFON) {
        jf_set_on(!jf.on);
        amb_log("jellyfin", jf.on ? "on" : "off");
        jf_settings_again();
        jf_roots_again();
    } else if (it->action == ACT_JFFIND)
        jf_find();
    else if (it->action == ACT_JFADDR)
        jf_type_address();
    else if (it->action == ACT_JFSIGNIN)
        jf_sign_in();
    else if (it->action == ACT_JFOUT)
        jf_sign_out();
    else if (it->action == ACT_GOJF)
        goto_jf_settings();
    else if (it->action == ACT_JFVSIZE) {
        jf_set_vsize((jf.vsize + 1) % 3);
        jf_settings_again();
    } else if (it->action >= ACT_JFSRV0 && it->action <= ACT_JFSRV_END &&
               it->action - ACT_JFSRV0 < jf_nfound) {
        int i = it->action - ACT_JFSRV0;
        jf_set_server(jf_found[i][0], jf_found[i][2], jf_found[i][1]);
        amb_log("jellyfin: server chosen", jf.name);
        jf_settings_again();
        jf_roots_again();
    }
    else if (it->action == ACT_SOUNDSET)
        open_sound_settings();
    else if (it->action == ACT_GENSET)
        open_settings_page(_("General"), build_general_settings);
    else if (it->action == ACT_SCREENSET)
        open_settings_page(_("Display"), build_screen_settings);
    else if (it->action == ACT_VIDEOSET)
        open_settings_page(_("Video"), build_video_settings);
    else if (it->action == ACT_NICSET)
        open_settings_page(_("Network interface"), build_nic_settings);
    else if (it->action == ACT_RADIOSET)
        open_settings_page(_("Radio"), build_radio_settings);
    else if (it->action == ACT_RKBPSSET)
        open_settings_page(_("Highest station bitrate"), build_rkbps_settings);
    else if (it->action == ACT_BUFSET)
        open_settings_page(_("Stream buffer"), build_buf_settings);
    else if (it->action == ACT_RSRVSET) {
        /* the servers asked for and timed now: a few seconds at most */
        set_msg("Asking radio-browser.info for its servers...");
        busy_begin();
        nmirror = net_radio_mirrors(mirrors, 8);
        busy_end(FALSE);
        set_msg(NULL);
        open_settings_page(_("Station list server"), build_rsrv_settings);
    } else if (it->action >= ACT_RKBPS0 && it->action <= ACT_RKBPS_END) {
        char k[16];
        rkbps_sel = it->action - ACT_RKBPS0;
        if (rkbps_sel == 0)
            save_setting(RKBPS_VAR, "");
        else if (rkbps_sel == 5)
            save_setting(RKBPS_VAR, "any");
        else {
            sprintf(k, "%lu", (unsigned long)rkbps_choice[rkbps_sel]);
            save_setting(RKBPS_VAR, k);
        }
        radio_set_max_kbps(rkbps_now());
        plex_settings_again(FALSE);
        set_msg(_("Station lists opened from now on"));
    } else if (it->action >= ACT_BUF0 && it->action <= ACT_BUF_END) {
        buf_sel = it->action - ACT_BUF0;
        save_setting(BUF_VAR, buf_names[buf_sel]);
        st_set_buffer(buf_sel);
        plex_settings_again(FALSE);
        set_msg(_("The next station or track started"));
    } else if (it->action == ACT_RSRVAUTO || it->action == ACT_RSRVFAST ||
               (it->action >= ACT_RSRV0 && it->action <= ACT_RSRV_END)) {
        if (it->action == ACT_RSRVAUTO) {
            rsrv[0] = 0;
        } else if (it->action == ACT_RSRVFAST) {
            int f = mirror_fastest();
            if (f < 0) {
                set_msg(_("None of the servers answered: Automatic kept"));
                return;
            }
            sprintf(rsrv, "fastest:%.60s", mirrors[f].name);
        } else {
            int i = it->action - ACT_RSRV0;
            if (i >= nmirror)
                return;
            amb_strlcpy(rsrv, mirrors[i].name, sizeof(rsrv));
        }
        save_setting(RSRV_VAR, rsrv);
        rsrv_apply();
        plex_settings_again(FALSE);
    }
    else if (it->action == ACT_NETSET)
        open_settings_page(_("Networking"), build_net_settings);
    else if (it->action == ACT_PLEXSLIDES && cur())
        plex_photo_view(cur(), -1, TRUE);
    else if (it->action == ACT_SND_MPEGA || it->action == ACT_SND_OWNMPEGA ||
             (it->action >= ACT_SND_MHI0 && it->action <= ACT_SND_MHI_END)) {
        const char *d = it->action == ACT_SND_MPEGA || it->action == ACT_SND_OWNMPEGA
                        ? "" : mhi_names[it->action - ACT_SND_MHI0];
        BOOL own = it->action == ACT_SND_OWNMPEGA;
        if (amb_stricmp(d, mhi_sel) || own != own_mpega) {
            amb_strlcpy(mhi_sel, d, sizeof(mhi_sel));
            own_mpega = own;
            save_setting(MP3_VAR, d[0] ? d : own ? "ambmpega" : "mpega");
            mhi_apply();
            sound_restart();                /* what plays: through it now */
            plex_settings_again(FALSE);
        }
        set_msg(d[0] ? _("MP3 decoded by MHI: radio, Plex (at its best quality) and files")
                : own ? _("MP3 decoded here by ambmpega.library, Ambiance's own")
                      : _("MP3 decoded here by mpega.library"));
    }
    else if (it->action >= ACT_SND_MQ0 && it->action <= ACT_SND_MQ_END) {
        int q = it->action - ACT_SND_MQ0;
        if (q != mp3_q) {
            mp3_q = q;
            st_mp3_quality(q);
            save_setting(MP3Q_VAR, mp3q_val[q]);
            sound_restart();                /* what plays: at it now */
            plex_settings_again(FALSE);     /* (its radio buttons: the new one on) */
        }
        set_msg(q == 1 ? _("MP3 at high quality, at the rate this Amiga can decode it")
                : q == 2 ? _("MP3 at medium quality: about a tenth less work, 70 dB")
                         : _("MP3 quality by this Amiga's speed, measured at the first MP3 each run"));
    }
    else if (it->action == ACT_SND_PAULA || it->action == ACT_SND_AHI || it->action == ACT_SND_P14) {
        BOOL on = it->action == ACT_SND_AHI, p14 = it->action == ACT_SND_P14;
        if (on != sound_ahi || p14 != sound_p14) {
            sound_ahi = on;
            sound_p14 = p14;
            st_use_ahi(on);
            st_use_paula14(p14);
            save_setting(SOUND_VAR, on ? "ahi" : p14 ? "paula14" : "paula");
            sound_restart();                /* what plays: through it now */
            plex_settings_again(FALSE);
        }
        set_msg(on ? _("AHI: radio, Plex, music files, modules modplug mixes (others: Paula)")
                : p14 ? _("Paula at 14 bits: radio, Plex, music files, modules modplug mixes")
                      : _("Paula plays everything"));
    }
    else if (it->action == ACT_FOLDERS)
        open_settings_page(_("Folders"), build_folders_settings);
    else if (it->action >= ACT_FSSET0 && it->action <= ACT_FSSET_END) {
        fs_page = it->action - ACT_FSSET0;
        fs_rm = -1;
        open_settings_page(_(fs[fs_page].title), build_folder_settings);
    } else if (it->action == ACT_FSASSIGN) {
        struct Folders *f = &fs[fs_page];
        char m[96];
        f->use_assign = !f->use_assign;
        save_setting(f->avar, f->use_assign ? "" : "off");
        fs_rm = -1;
        plex_settings_again(FALSE);
        sprintf(m, f->use_assign ? _("%s used when it is assigned") : _("%s left out"), f->assign);
        set_msg(m);
    } else if (it->action == ACT_FSADD) {
        fs_rm = -1;
        folder_add(fs_page);
    } else if (it->action == ACT_WHDPAGE) {
        open_settings_page("WHDLoad", build_whd_page);
    } else if (it->action == ACT_WHDGO) {
        whd_go();
    } else if (it->action == ACT_WHDNO) {
        save_setting(WHD_OFFER_VAR, "no");
        set_msg(_("Games no longer offers WHDLoad: Settings > Folders > WHDLoad still does"));
        plex_settings_again(FALSE);
    } else if (it->action == ACT_IGSET) {
        open_settings_page(_("iGame"), build_igame_settings);
    } else if (it->action == ACT_IGCHOOSE) {
        igame_choose();
    } else if (it->action == ACT_IGFIND) {
        char found[256];
        busy_begin();
        if (ig_discover(found, sizeof(found), TRUE)) {
            save_setting(IG_VAR, found);
            igame_start(FALSE);
            ig_said();
        } else
            set_msg(_("iGame was not found: choose its drawer"));
        busy_end(TRUE);
        plex_settings_again(FALSE);
    } else if (it->action == ACT_IGNONE) {
        if (ig_off()) {
            char found[256];
            save_setting(IG_VAR, "");           /* unset: looked for, everywhere */
            busy_begin();
            if (ig_discover(found, sizeof(found), TRUE))
                save_setting(IG_VAR, found);
            igame_start(FALSE);
            busy_end(TRUE);
            if (igcat.n)
                ig_said();
            else
                set_msg(_("iGame was not found: choose its drawer"));
        } else {
            save_setting(IG_VAR, "none");
            igame_start(FALSE);
            set_msg(_("No iGame list: Games and Demos show AGS's alone"));
        }
        plex_settings_again(FALSE);
    } else if (it->action >= ACT_FSDIR0 && it->action <= ACT_FSDIR_END) {
        struct Folders *f = &fs[fs_page];
        int i = it->action - ACT_FSDIR0;
        if (i >= f->n)
            return;
        if (fs_rm != i) {
            fs_rm = i;
            plex_settings_again(FALSE);
            set_msg(_("Fire again to take this folder out (its files stay)"));
            return;
        }
        for (; i < f->n - 1; i++)
            strcpy(f->dir[i], f->dir[i + 1]);
        f->n--;
        fs_rm = -1;
        save_folders(fs_page);
        plex_settings_again(FALSE);
        set_msg(_("Folder taken out"));
    }
    else if (it->action == ACT_NICAUTO)
        choose_nic(-1);
    else if (it->action >= ACT_NIC0 && it->action <= ACT_NIC_END)
        choose_nic(it->action - ACT_NIC0);
    else if (it->action == ACT_NOWPLAYING)
        np_open_page();
    else if (it->action == ACT_PLEXHELP)
        plex_help();
    else if (it->action == ACT_JFHELP)
        jf_help();
    else if (it->action == ACT_ENTRY)
        entry_close(TRUE);                  /* Fire on the line: kept */
    else if (it->action == ACT_STYLE_PIXEL)
        apply_style(STYLE_PIXEL);
    else if (it->action == ACT_STYLE_SMOOTH)
        apply_style(STYLE_SMOOTH);
    else if (it->action >= ACT_LFONT0 && it->action <= ACT_LFONT_END)
        apply_laced_font(it->action - ACT_LFONT0);
    else if (it->action >= ACT_RFONT0 && it->action <= ACT_RFONT_END)
        apply_rtg_font(it->action - ACT_RFONT0);
    else if (it->action >= ACT_MODE0 && it->action < ACT_MODE0 + SM_COUNT)
        apply_screen_mode(it->action - ACT_MODE0);
}

/* Holding Up/Down in a list or text page scrolls faster the longer it is
 * held, without skipping anything: lists still move one entry at a time,
 * only more often, and text glides at a rising pixel speed. Ambiance paces
 * this itself, one step per video frame at most, from key-down until
 * key-up (the keyboard's own repeat is fixed at about 10 a second), and
 * eases off near the end so it settles on the last entry.
 *
 * Lists speed up in two stages: to cruising speed over HOLD_RAMP, then,
 * still held, on up to LIST_BOOST times that over HOLD_BOOST, or for a
 * long list as fast as crosses all of it in LIST_SWEEP (All Games' 3,600
 * entries: about 3,600 a second; top to bottom in about 2.4 s), the big letter showing where it is. */
#define HOLD_DELAY  250     /* ms before the glide starts */
#define HOLD_RAMP   500     /* ms to reach cruising speed */
#define HOLD_BOOST  600     /* further ms to reach top speed */
#define LIST_MIN     12     /* entries per second when the glide starts */
#define MENU_MIN     35     /* ...in a short list (a menu: two screens or less), */
#define MENU_MAX     50     /* rising to this over MENU_RAMP (a row a frame); */
#define MENU_RAMP   500     /* never eased */
#define LIST_MAX    100     /* cruising entries per second on a small screen */
#define LIST_SCREENS  6     /* ...or this many screenfuls a second, if more */
#define LIST_BOOST    4     /* top speed, as a multiple of cruising speed, */
#define LIST_SWEEP 1000     /* ...or the whole list in this many ms, if faster */
#define LIST_CROSS_CRUISE 4000  /* cruising never crosses a whole list faster */
#define LIST_CROSS_PEAK   2000  /* nor top speed (both: a row a frame at the least) */
#define FRAME_MS     20     /* assumed frame time if there is no timer */
#define LIST_BRAKE  700     /* ms to brake from full speed to a stop at the end, */
#define LIST_EASE     5     /* ...and the last screens: entries a second per entry left */
/* Let go of a list moving faster than it can be read, and it coasts on,
 * as a flicked list does on a phone: slowing evenly to a stop in COAST_MS
 * from the speed it had, but no faster than COAST_FAST screenfuls a second
 * (past that it is a blur, and the glide let go of near where it was
 * wanted: from full speed it went 700 entries on), so it goes a screenful
 * or two at the most, braking for the end as a held glide does. Slower than COAST_SCREENS
 * screenfuls a second (the start of a glide, a short hold) it stops where
 * it is, as before, and a menu never coasts. Any key, button or click
 * stops a coast at once; Return, Fire or a click only stop it (a second
 * one opens); Up/Down the same way carries on at the coasting speed. */
#define COAST_MS    400     /* ms to coast to a stop from the speed let go at */
#define COAST_FAST    8     /* screenfuls a second at the most to coast from */
#define COAST_SCREENS 3     /* screenfuls a second at the least to coast */
#define COAST_MIN    40     /* ...and entries a second */
#define COAST_STOP    8     /* entries a second: below this it has stopped */

static struct {
    LONG  dir;              /* -1 up, +1 down, 0 not held */
    ULONG held_ms, last_ms; /* time held so far, clock at the last tick */
    LONG  acc;              /* entries (x1000) or pixels (x1000) owed */
    BOOL  joy;              /* held on the joystick rather than the keyboard */
    BOOL  letters;          /* show the letter overlay once the glide starts */
    LONG  lacc;             /* letters (x1000) owed, letter by letter */
    LONG  from;             /* the selection when the key went down (log) */
    ULONG worst;            /* the longest frame meanwhile (log) */
    LONG  slow;             /* frames over 40 ms meanwhile (log) */
    ULONG present_ms, move_ms;  /* time flipping, and moving the list (log) */
    ULONG t_down;           /* the clock at key-down (log: the time it really took) */
    LONG  rate;             /* entries a second at the last step (0: not a list glide) */
    BOOL  coast;            /* let go, still moving (COAST_MS) */
    LONG  cdir, cv, cv0, cdec;  /* its direction, speed, starting speed, slowing per s */
    LONG  cmoved;           /* entries it has gone (log) */
    ULONG cstart;           /* the clock when it was let go (log) */
    struct Page *cpage;     /* the list it coasts in */
} hold;

/* A coast stopped by Up/Down: the speed it had, for the new hold to go on
 * at (taken only straight away, within COAST_CARRY ms) */
#define COAST_CARRY 300
static LONG carry_v, carry_dir;
static ULONG carry_ms;

/* Long titled lists, held past cruising: a whole letter a step. Shorter
 * ones glide on smoothly, the big letter going by: one of 1,000 entries
 * is crossed in about 4 s that way (top speed, LIST_BOOST x cruising),
 * and a letter at a time would skip most of it. */
#define LETTER_MIN_LIST 1000    /* entries a list needs for that */
#define LETTERS_MIN   4     /* letters a second to begin with, */
#define LETTERS_MAX  12     /* rising to this over HOLD_BOOST */

static void hold_carry(LONG dir);
static void hold_start(LONG dir, BOOL joy)
{
    hold.dir = dir;
    hold.held_ms = 0;
    hold.last_ms = time_ms();
    hold.acc = 0;
    hold.lacc = 0;
    hold.joy = joy;
    hold.letters = cur() && ov_eligible(cur());
    hold.from = cur() ? cur()->sel : 0;
    hold.worst = 0;
    hold.slow = 0;
    hold.present_ms = hold.move_ms = 0;
    hold.t_down = time_ms();
    hold.rate = 0;
    hold.coast = FALSE;
    ov_ms = ov_n = 0;
    hold_carry(dir);
}

static void scr_watch_now(const char *why);
static void hold_stop(void)
{
    struct Page *p = cur();
    if (hold.coast) {
        char m[120];
        hold.coast = FALSE;
        sprintf(m, "%ld entries in %ld ms from %ld a second; now %ld of %ld",
                (long)hold.cmoved, (long)(time_ms() - hold.cstart), (long)hold.cv0,
                p && p->type == PT_LIST ? (long)p->sel : -1L,
                p && p->type == PT_LIST ? (long)p->list.count : -1L);
        amb_log("coast", m);
    }
    if (hold.dir && hold.held_ms > HOLD_DELAY && p && p->type == PT_LIST) {
        char m[320];
        LONG moved = p->sel - hold.from;
        sprintf(m, "%ld entries in %ld ms (%ld by the clock), longest frame %ld ms, "
                   "%ld frames over 40 ms; "
                   "moving the list %ld ms, flipping %ld ms, %ld letters in %ld ms; "
                   "now %ld, top %ld of %ld",
                (long)(moved < 0 ? -moved : moved), (long)hold.held_ms,
                (long)(time_ms() - hold.t_down), (long)hold.worst, (long)hold.slow,
                (long)hold.move_ms, (long)hold.present_ms, (long)ov_n,
                (long)ov_ms, (long)p->sel, (long)p->top, (long)p->list.count);
        amb_log(g.wb ? "glide (in a window)" : "glide", m);
        scr_watch_now("the glide's end");
    }
    hold.dir = 0;
}

/* The first entry of the next letter (dir > 0), or of this letter, or the
 * one before if already there (dir < 0); from itself if there is none. */
static LONG letter_step(struct Page *p, LONG from, LONG dir)
{
    const struct Item *it = p->list.items;
    LONG i = from, n = p->list.count;
    char c = ov_letter_in(it, n, from);
    if (dir > 0) {
        while (i < n && ov_letter_in(it, n, i) == c)
            i++;
        return i < n ? i : from;
    }
    if (i > 0 && ov_letter_in(it, n, i - 1) == c) {
        while (i > 0 && ov_letter_in(it, n, i - 1) == c)
            i--;
        return i;
    }
    if (i == 0)
        return from;
    c = ov_letter_in(it, n, --i);
    while (i > 0 && ov_letter_in(it, n, i - 1) == c)
        i--;
    return i;
}

/* The whole square root of n (n < 2^31): braking needs the speed that
 * stops in a distance, sqrt(2 a d). */
static LONG isqrt(ULONG n)
{
    ULONG x = 0, b = 1UL << 30;
    while (b > n)
        b >>= 2;
    while (b) {
        if (n >= x + b) {
            n -= x + b;
            x = (x >> 1) + b;
        } else
            x >>= 1;
        b >>= 2;
    }
    return (LONG)x;
}

/* A held list's speed (entries a second) glide ms after the glide began,
 * before slowing for the end; *top: its cruising speed, *menu: a short
 * list (a steady menu pace, never eased, never coasting). */
static LONG list_rate(struct Page *p, LONG glide, LONG *topp, BOOL *menup)
{
    /* Cruising: two entries a frame, or six screenfuls a second on
     * screens that show more rows; above one per frame the list
     * moves several rows in a single frame, still drawn every frame. */
    LONG top = LIST_MAX, screens = LIST_SCREENS * list_visible(), rate;
    LONG cruise_cap = p->list.count * 1000 / LIST_CROSS_CRUISE;
    BOOL menu = p->list.count <= 2 * list_visible();
    if (screens > top)
        top = screens;
    /* a shorter list (111 artists) is not crossed in a blink: cruising
     * takes LIST_CROSS_CRUISE ms over all of it, but never under a
     * row a frame */
    if (cruise_cap < MENU_MAX)
        cruise_cap = MENU_MAX;
    if (top > cruise_cap)
        top = cruise_cap;
    if (menu)
        /* a short list (Games' own menu): a steady menu pace from
         * the start, as a fast key repeat would give; the slow
         * start and the easing at the end are for long lists, and
         * a short one was all start and end */
        rate = MENU_MIN + (MENU_MAX - MENU_MIN) *
               (glide < MENU_RAMP ? glide : MENU_RAMP) / MENU_RAMP;
    else if (glide < HOLD_RAMP)
        rate = LIST_MIN + (top - LIST_MIN) * glide / HOLD_RAMP;
    else {
        /* top speed: four times cruising, or for a long list (whose
         * length is known from the start when it has an index)
         * fast enough to cross all of it in LIST_SWEEP ms */
        LONG b = glide - HOLD_RAMP, peak = top * LIST_BOOST,
             sweep = p->list.count * 1000 / LIST_SWEEP;
        if (sweep > peak)
            peak = sweep;
        if (p->list.count < LETTER_MIN_LIST &&
            peak > p->list.count * 1000 / LIST_CROSS_PEAK)
            peak = p->list.count * 1000 / LIST_CROSS_PEAK;    /* not too fast, short */
        if (peak < top)
            peak = top;
        if (b > HOLD_BOOST)
            b = HOLD_BOOST;
        rate = top + (peak - top) * b / HOLD_BOOST;
    }
    *topp = top;
    *menup = menu;
    return rate;
}

/* Slowing for the end, by the clock (not by frames: a fast Amiga draws a
 * frame in 20-30 ms, and a rule per frame slowed it only in the last
 * tenth of a second - the list all but hit the end). First braking, as a
 * car does: the speed that stops in what is left, sqrt(2 a d), with a
 * deceleration that stops from speed `from' in LIST_BRAKE ms (All
 * Artists' 2,450 at full speed: from some 800 entries out); then the last
 * screens eased, LIST_EASE entries a second per entry left (20 left: 110
 * a second, 1 left: 15), the end seen coming for about half a second. */
static LONG end_cap(LONG rate, LONG from, LONG remaining)
{
    LONG a = from * 1000 / LIST_BRAKE, stop;
    stop = isqrt((ULONG)(2 * a) * (ULONG)remaining);
    if (stop < 1)
        stop = 1;
    if (rate > stop)
        rate = stop;
    if (rate > 10 + remaining * LIST_EASE)
        rate = 10 + remaining * LIST_EASE;
    return rate;
}

/* Up/Down pressed to stop a coast, the same way: the new hold goes on at
 * the coasting speed, its clock set to where the glide reaches it. */
static void hold_carry(LONG dir)
{
    struct Page *p = cur();
    LONG v = carry_v, glide, top;
    BOOL menu;
    BOOL fresh = carry_dir == dir && (ULONG)(time_ms() - carry_ms) < COAST_CARRY;
    carry_dir = 0;
    if (!fresh || !p || p->type != PT_LIST)
        return;
    for (glide = 0; glide < HOLD_RAMP + HOLD_BOOST; glide += 20)
        if (list_rate(p, glide, &top, &menu) >= v)
            break;
    hold.held_ms = HOLD_DELAY + glide;
}

/* Let go of a held list: a coast, if it was going fast enough. */
static void hold_release(void)
{
    struct Page *p = cur();
    LONG v = hold.rate, dir = hold.dir;
    BOOL go = p && p->type == PT_LIST && focus == FOCUS_CONTENT && hold.held_ms > HOLD_DELAY &&
              v >= COAST_MIN && v >= COAST_SCREENS * list_visible();
    hold_stop();
    if (!go)
        return;
    if (v > COAST_FAST * list_visible())
        v = COAST_FAST * list_visible();
    hold.coast = TRUE;
    hold.cdir = dir;
    hold.cv = hold.cv0 = v;
    hold.cdec = v * 1000 / COAST_MS;
    hold.cmoved = 0;
    hold.cstart = hold.last_ms = time_ms();
    hold.cpage = p;
    hold.acc = 0;
}

/* Something pressed during a coast: it stops where it is. TRUE if one was
 * going (a Return, Fire or click then only stops it). Its speed is kept a
 * moment for Up/Down the same way to go on at (hold_start). */
BOOL coast_interrupt(void)
{
    if (!hold.coast)
        return FALSE;
    carry_v = hold.cv;
    carry_dir = hold.cdir;
    carry_ms = time_ms();
    hold_stop();
    return TRUE;
}

/* Once a frame while coasting. */
static void coast_tick(void)
{
    struct Page *p = cur();
    ULONG now = time_ms(), dt = now - hold.last_ms;
    LONG remaining, rate, n;
    if (dt > 250)
        dt = 250;
    hold.last_ms = now;
    if (focus != FOCUS_CONTENT || !p || p != hold.cpage || p->type != PT_LIST) {
        hold_stop();
        return;
    }
    remaining = hold.cdir > 0 ? p->list.count - 1 - p->sel : p->sel;
    hold.cv -= hold.cdec * (LONG)dt / 1000;
    if (remaining <= 0 || hold.cv < COAST_STOP) {
        hold_stop();
        return;
    }
    ov.linger = 0;                          /* the letter stays while it moves */
    rate = end_cap(hold.cv, hold.cv0, remaining);
    hold.acc += rate * (LONG)dt;
    n = hold.acc / 1000;
    if (n > 1 && n > remaining / 2) {
        n = remaining / 2 > 1 ? remaining / 2 : 1;
        hold.acc = n * 1000;
    }
    if (n > 0) {
        hold.acc -= n * 1000;
        if (n > remaining)
            n = remaining;
        act_move(hold.cdir * n);
        hold.cmoved += n;
    }
}

BOOL hold_active(void)
{
    return hold.dir != 0 || hold.coast;     /* a coast is a glide too: previews wait */
}

/* Is this raw key physically down right now? Asked of the keyboard itself
 * (lowlevel.library QueryKeys), not inferred from messages, which can be
 * late or queued behind repeats. Without lowlevel, assume yes. */
static BOOL key_is_down(UWORD rawkey)
{
    struct KeyQuery kq;
    if (!gfx_input_ours(&g))
        return FALSE;                       /* typed into another window */
    if (!LowLevelBase)
        return TRUE;
    kq.kq_KeyCode = rawkey;
    kq.kq_Pressed = FALSE;
    QueryKeys(&kq, 1);
    return kq.kq_Pressed;
}

/* Once per frame while held. */
static void hold_tick(void)
{
    struct Page *p = cur();
    LONG rate, remaining, ramp, elapsed, glide;
    if (!hold.dir) {
        if (hold.coast)
            coast_tick();
        return;
    }
    if ((focus != FOCUS_CONTENT && focus != FOCUS_PREVIEW) || !p ||
        (focus == FOCUS_PREVIEW && !pv_tb.open)) {
        hold_stop();
        return;
    }
    if (!hold.joy && !key_is_down(hold.dir < 0 ? 0x4C : 0x4D)) {
        hold_release();                     /* let go: stop now (whatever is queued), or coast */
        return;
    }
    {
        /* Speed is set by the clock, not by how often this runs: if drawing
         * overruns a frame, the next step makes up the distance. */
        ULONG now = time_ms(), dt = now ? now - hold.last_ms : FRAME_MS;
        if (dt > hold.worst)
            hold.worst = dt;
        if (dt > 40)
            hold.slow++;
        hold.last_ms = now;
        /* how long it has been held, by the clock: the speeding up the
         * same however slowly the frames come (it was the frames' clipped
         * times added up, slower on a slow screen) */
        hold.held_ms += dt > 2000 ? 2000 : dt;
        if (dt > 600)
            dt = 600;               /* a slow frame is made up for (one of 330 ms lost a
                                     * quarter of its distance at 250), a stall is not */
        if (hold.held_ms < HOLD_DELAY)
            return;
        glide = hold.held_ms - HOLD_DELAY;
        ramp = glide > HOLD_RAMP ? HOLD_RAMP : glide;
        elapsed = dt;
    }
    if (p->type == PT_LIST && focus != FOCUS_PREVIEW) {
        if (hold.letters && !(ov.on && ov.page == p))
            ov_show(p);
        ov.linger = 0;
        remaining = hold.dir > 0 ? p->list.count - 1 - p->sel : p->sel;
        if (remaining <= 0 && hold.dir > 0 && ch_reading(p))
            return;                         /* the end of what is read: it is coming */
        if (remaining <= 0) {
            hold_stop();
            return;
        }
        if (jump_scroll && glide >= HOLD_RAMP && hold.letters &&
            p->list.count >= LETTER_MIN_LIST) {
            /* past cruising in a long titled list: a letter a step (let go,
             * and the selection is on the first entry of the letter shown) */
            LONG b = glide - HOLD_RAMP, lps, to;
            hold.rate = 0;                  /* let go: on the letter's first entry, no coast */
            if (b > HOLD_BOOST)
                b = HOLD_BOOST;
            lps = LETTERS_MIN + (LETTERS_MAX - LETTERS_MIN) * b / HOLD_BOOST;
            hold.lacc += lps * elapsed;
            if (hold.lacc < 1000)
                return;
            hold.lacc -= 1000;
            if (hold.lacc > 1000)
                hold.lacc = 1000;               /* never more than one owed */
            if ((to = letter_step(p, p->sel, hold.dir)) != p->sel) {
                act_move(to - p->sel);
                return;
            }
            /* no letter further: on to the end entry by entry */
        }
        {
            LONG top, n;
            BOOL menu;
            rate = list_rate(p, glide, &top, &menu);
            if (!menu)
                rate = end_cap(rate, rate > top ? rate : top, remaining);
            hold.rate = menu ? 0 : rate;    /* let go at this: a coast, if fast */
            hold.acc += rate * elapsed;     /* entries x 1000 */
            n = hold.acc / 1000;
            if (!menu && n > 1 && n > remaining / 2) {
                /* a frame takes 100-200 ms on a big native screen: by speed
                 * alone the easing was crossed in one, so a step is also at
                 * most half of what is left, and the end is seen coming in
                 * some ten frames, slowing (a quarter took three seconds) */
                n = remaining / 2 > 1 ? remaining / 2 : 1;
                hold.acc = n * 1000;
            }
            if (n > 0) {
                ULONG t0 = time_ms();
                hold.acc -= n * 1000;
                act_move(hold.dir * (n < remaining ? n : remaining));
                hold.move_ms += time_ms() - t0;
            }
        }
    } else if (focus == FOCUS_PREVIEW || p->tb.open) {
        struct TextBox *tb = focus == FOCUS_PREVIEW ? &pv_tb : &p->tb;
        LONG px;
        if (tb->jump) {
            /* a line at a time, 10 to 30 lines a second: each a copy of the box */
            LONG n;
            hold.acc += (10 + 20 * ramp / HOLD_RAMP) * elapsed;
            n = hold.acc / 1000;
            hold.acc -= n * 1000;
            if (n <= 0)
                return;
            if (tb_remaining(tb, hold.dir) <= 0) {
                hold_stop();
                return;
            }
            tb_scroll_by(tb, hold.dir * n * tb->line_h);
            return;
        }
        hold.acc += (100 + 500 * ramp / HOLD_RAMP) * elapsed;  /* 100 to 600 px/s, x1000 */
        px = hold.acc / 1000;
        hold.acc -= px * 1000;
        remaining = tb_remaining(tb, hold.dir);
        if (remaining <= 0)
            return;
        if ((tb->target - tb->pos) * hold.dir > 4 * tb->line_h)
            return;                         /* never run far ahead of the screen */
        if (px > 1 + remaining / 8)
            px = 1 + remaining / 8;
        tb_scroll_by(tb, hold.dir * px);
    }
}

void act_open(BOOL play)
{
    struct Page *p;
    (void)play;                     /* Return and Fire both play a game now */
    pane_settle();
    p = cur();
    if (focus == FOCUS_MENU) {
        if (p && (p->type != PT_LIST || p->list.count))
            set_focus(FOCUS_CONTENT);
        return;
    }
    if (!p)
        return;
    if (lp_open_all())
        return;                     /* (Home's "All ..." tile: the section's whole list) */
    lp_note_open(p);
    if (p->type == PT_LIST && p->list.count) {
        struct Item *it;
        busy_begin();
        ch_wait_item(p, p->sel, TRUE);      /* its letter, if it was let go */
        busy_end(TRUE);
        it = &p->list.items[p->sel];
        if (it->kind == IT_MOD || it->kind == IT_SONG)
            mods_play_item(p);      /* the music, and its page */
        else if (p->pics && it->kind == IT_DIR && it->mark == MARK_PLAY)
            folder_slideshow(it);   /* Fire plays a folder of pictures */
        else if (it->kind == IT_PIC)
            view_from_page(p);
        else if (it->kind == IT_ANIM)
            anim_from_page(p);
        else if (it->kind == IT_DIR)
            open_dir(it);
        else if (it->kind == IT_ACTION)
            act_action(it);
        else if (it->kind == IT_NET || it->kind == IT_PLEXDIR)
            radio_open(it);         /* another list, from radio-browser.info or Plex */
        else if (it->kind == IT_STATION)
            mods_play_item(p);      /* the station, on Now playing */
        else if (it->kind == IT_PLEXTRACK) {
            if (it->flags & ITEM_DISABLED)
                set_msg(plex_why(it));
            else
                mods_play_item(p);  /* the album (or list) its playlist */
        } else if (it->kind == IT_PLEXPHOTO)
            plex_photo_view(p, p->sel, FALSE);  /* full screen, the others a step away */
        else if (it->kind == IT_PLEXSRV)
            plex_choose(it);
        else if (it->kind == IT_RUN) {
            char base[300];
            launch(item_path(it, base, sizeof(base)));  /* Fire, Return, Space, double-click */
        }
    } else if (p->type == PT_DETAIL) {
        launch(p->base);
    } else if (p->type == PT_PLAYER) {
        np_press(np.btn);
    }
}

/* Right: on a game, into the details pane when its fact sheet scrolls
 * (nothing otherwise); lowres, with no pane, opens the game's own page.
 * Anywhere else Right opens, as Fire does. */
static void act_right(void)
{
    struct Page *p;
    pane_settle();
    p = cur();
    if (focus == FOCUS_PREVIEW)
        return;
    if (focus == FOCUS_CONTENT && p && p->type == PT_LIST && p->list.count) {
        busy_begin();
        ch_wait_item(p, p->sel, TRUE);
        busy_end(TRUE);
    }
    if (focus == FOCUS_CONTENT && p && p->type == PT_LIST && p->list.count &&
        p->list.items[p->sel].kind == IT_DIR) {
        open_dir(&p->list.items[p->sel]);   /* Right goes inside, never plays */
        return;
    }
    if (focus == FOCUS_CONTENT && p && p->type == PT_LIST && p->list.count &&
        p->list.items[p->sel].kind == IT_STATION) {
        radio_station(&p->list.items[p->sel]);  /* Right: about it; Fire plays it */
        return;
    }
    if (focus == FOCUS_CONTENT && p && p->type == PT_LIST && p->list.count &&
        (p->list.items[p->sel].kind == IT_RUN || p->list.items[p->sel].kind == IT_PIC)) {
        if (!has_aside(p)) {
            if (p->list.items[p->sel].kind == IT_PIC)
                view_from_page(p);          /* lowres: no pane to look into */
            else
                open_detail(&p->list.items[p->sel]);
            return;
        }
        if (det.pending || !det.shown || !det.text_shown) {    /* not shown yet: show it now */
            ov.on = FALSE;
            preview_now(p);
        }
        if (pv_tb.open && pv_tb.bar_w)
            set_focus(FOCUS_PREVIEW);
        return;
    }
    act_open(FALSE);
}

void act_back(void)
{
    if (lp_back())
        return;                     /* (the section's whole list: back to Home's row) */
    pane_settle();
    nav_back_begin(focus);
    if (focus == FOCUS_PREVIEW) {   /* out of the details pane, back to the list */
        set_focus(FOCUS_CONTENT);
        return;
    }
    if (focus == FOCUS_MENU) {
        if (quit_armed) {
            running = FALSE;
            return;
        }
        quit_armed = TRUE;
        set_msg(_("Press Esc again to quit Ambiance"));
        return;
    }
    if (depth > 1) {
        page_pop();
        np_shortcut_sync();
        draw_pane(TRUE);
        draw_footer();
    } else {
        set_focus(FOCUS_MENU);
    }
}

static void act_letter_now(char c);

/* Typing in a list: letters (and digits) typed within TYPE_GAP_MS of each
 * other make one word, and the selection goes to the first entry from
 * here on that begins with it ("a", "c": AC/DC, after ABBA); a pause
 * starts a new word. Punctuation and spaces in the entries are passed
 * over ("acdc" finds AC/DC too), capitals and accents do not matter (É is
 * E). The same letter again and again steps through that letter's
 * entries, as one letter at a time always did. */
#define TYPE_GAP_MS 1000
#define TYPE_MAX 24
static char typed[TYPE_MAX + 1];
static ULONG typed_ms;
static struct Page *typed_page;     /* the list typed in (another: a new word) */

static void act_letter(char c)
{
    ULONG now = time_ms();
    LONG n = (LONG)strlen(typed);
    if (!n || !now || now - typed_ms > TYPE_GAP_MS || n >= TYPE_MAX || typed_page != cur())
        n = 0;                      /* a new word */
    typed_page = cur();
    typed[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    typed[n] = 0;
    typed_ms = now;
    busy_begin();                   /* a letter not read yet can take a while */
    act_letter_now(c);
    busy_end(TRUE);
    typed_ms = time_ms() ? time_ms() : now; /* (the gap counts from the jump made) */
}

/* A character as typing compares it: a-z, 0-9, accented letters as their
 * base letter in small, anything else 0 (passed over). */
static char type_fold(UBYTE c)
{
    static const char base[] = "aaaaaaaceeeeiiiidnooooo.ouuuuyts";
    if (c >= 'A' && c <= 'Z')
        return (char)(c + 32);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return (char)c;
    if (c >= 0xC0 && c != 0xD7 && c != 0xF7) {
        if (c == 0xFF)
            return 'y';
        if (c >= 0xE0)
            c -= 32;
        if (c == 0xDF)
            return 's';
        return base[c - 0xC0] != '.' ? base[c - 0xC0] : 0;
    }
    return 0;
}

/* Does the entry begin with w (folded, its punctuation passed over)? */
static BOOL type_match(const char *label, const char *w)
{
    const UBYTE *s = (const UBYTE *)label;
    if ((*s >= 0x80 && *s <= 0x9F))
        return FALSE;               /* Japanese, Chinese, Korean: no letters to type */
    for (; *w; w++) {
        char f = 0;
        while (*s && !(f = type_fold(*s)))
            s++;
        if (!*s || f != *w)
            return FALSE;
        s++;
    }
    return TRUE;
}

static void act_letter_now(char c)
{
    struct Page *p;
    LONG i, n, from;
    const char *w = typed;
    char one[2];
    BOOL same = TRUE;
    nav_moved();
    pane_settle();
    p = cur();
    if (focus != FOCUS_CONTENT || !p || p->type != PT_LIST || !p->list.count)
        return;
    for (i = 1; typed[i]; i++)
        if (typed[i] != typed[0])
            same = FALSE;
    if (!typed[0] || same) {
        /* one letter, or the same again: its next entry after this one */
        one[0] = typed[0] ? typed[0] : type_fold((UBYTE)c);
        one[1] = 0;
        w = one;
        from = p->sel + 1;
    } else
        from = p->sel;              /* this one may begin with the longer word too */
    /* still loading: read on until it turns up after the selection (or the
     * list is complete, then wrap round as usual) */
    for (i = from; ch_reading(p) || i < p->list.count; i++) {
        if (i >= p->list.count) {
            if (!ch_wait_one(p, TRUE))
                break;
            i--;
            continue;
        }
        if (type_match(p->list.items[i].label, w)) {
            if (p->sel != i) {
                p->sel = i;
                draw_title_count(p);
                draw_list_rows(p);
            }
            return;
        }
    }
    n = p->list.count;
    for (i = 0; i < n; i++) {
        LONG idx = (from + i) % n;
        if (type_match(p->list.items[idx].label, w)) {
            if (p->sel != idx) {
                p->sel = idx;
                draw_title_count(p);
                draw_list_rows(p);
            }
            return;
        }
    }
}

/* The next letter's first entry (or this letter's, then the one before). */
static void act_letter_step(LONG dir)
{
    struct Page *p;
    LONG to;
    pane_settle();
    p = cur();
    if (focus == FOCUS_CONTENT && np_on()) {
        np_press(dir > 0 ? NB_NEXT : NB_PREV);  /* the music: next / previous module */
        return;
    }
    if (focus != FOCUS_CONTENT || !p || p->type != PT_LIST || !p->list.count)
        return;
    if (dir > 0 && ch_reading(p) && letter_step(p, p->sel, 1) == p->sel) {
        busy_begin();
        ch_wait_until(p, p->list.count + 1, TRUE);  /* the next letter is not read yet */
        busy_end(TRUE);
    }
    if ((to = letter_step(p, p->sel, dir)) != p->sel) {
        det.dir = dir;
        act_move(to - p->sel);
    }
}

void disarm(void)
{
    if (quit_armed || msg[0]) {
        quit_armed = FALSE;
        set_msg(NULL);
    }
}

/* ------------------------------------------------------------ input */

/* A double-click: Intuition's test on the events' times, or (should those
 * times be off: after a slideshow they were) our own clock's, within the
 * same time Intuition allows (its Preferences, read once). */
/* Preferences' double-click time, in ms. */
ULONG dc_time(void)
{
    static ULONG dc_ms;
    if (!dc_ms) {
        struct Preferences pr;
        dc_ms = 500;
        if (GetPrefs(&pr, sizeof(pr)))
            dc_ms = pr.DoubleClick.tv_secs * 1000 + pr.DoubleClick.tv_micro / 1000;
        if (dc_ms < 200 || dc_ms > 3000)
            dc_ms = 500;
    }
    return dc_ms;
}





/* ------------------------------------------------------------ AmigaAMP's plug-ins */
/* Each tick: the attached plug-in fed; one Ambiance started puts
 * Ambiance's screen behind as it comes, and in front again as it goes
 * (Esc in it, the music over) */
static void amp_frame(void)
{
    static BOOL was, was_ours;
    static struct Screen *behind;
    BOOL now;
    amp_tick();
    now = amp_attached();
    if (now && !was) {
        was_ours = amp_ours();
        if (was_ours && (behind = gfx_own_screen(&g))) {
            ScreenToBack(behind);
            WBenchToFront();
        }
    }
    if (now && was_ours && !mp_loaded())
        amp_stop(1500);                     /* the music over: it goes */
    if (!amp_attached() && was) {
        if (was_ours && behind && behind == gfx_own_screen(&g)) {
            ScreenToFront(behind);
            if (g.win)
                ActivateWindow(g.win);
            draw_all();
        }
        behind = NULL;
        was_ours = FALSE;
    }
    was = amp_attached();
}

/* ------------------------------------------------------------ Flow on the whole screen */
/* F on Now playing: Flow over the whole window or screen, what plays
 * written over it for a few seconds (and again as it changes); a key, a
 * click or the fire button and the page is back. The music goes on as
 * ever (np_tick between frames: the next track, a station's titles). */
/* Flow on the whole screen: from a window, Ambiance's own full screen (as
 * the picture viewer's), back to the window as it ends. Its controls, over
 * the bottom, while the mouse moves: previous, play/pause, next, volume
 * down and up, and full screen or the window. */
static BOOL np_full_fs;             /* it opened the full screen (a window before) */
static ULONG np_full_ctl;           /* the controls shown until (0: hidden) */
enum { NF_PREV, NF_PLAY, NF_NEXT, NF_VDOWN, NF_VUP, NF_FULL, NF_COUNT };
static WORD nf_x[NF_COUNT], nf_y, nf_w, nf_h, nf_bx0, nf_bx1, nf_by0;   /* (the controls' strip) */
static WORD nf_band = 0x7FFF;       /* the strip at the bottom kept black (0x7FFF: none) */

/* the pointer over Flow: shown or not, nothing else drawn (pointer_hide's
 * hover and title would draw over the picture) */
static void nf_pointer(BOOL show)
{
    if (!g.win)
        return;
    if (show) {
        if (ptr_hidden == g.win)
            ClearPointer(g.win);
        ptr_hidden = NULL;
    } else if (ptr_hidden != g.win) {
        if (!ptr_blank && !(ptr_blank = AllocMem(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR)))
            return;
        SetPointer(g.win, ptr_blank, 1, 16, 0, 0);
        ptr_hidden = g.win;
    }
}

static void np_full_exit(void)
{
    if (!np_full)
        return;
    np_full = FALSE;
    np_full_ctl = 0;
    nf_band = 0x7FFF;
    vf_clip(0x7FFF);
    nf_pointer(TRUE);
    if (np_full_fs && gfx_fs_on())
        gfx_fs_leave(&g);                   /* the window again */
    np_full_fs = FALSE;
    vf_stop();                              /* (its full-size maps given back) */
    amb_log("visualisation", "Flow: the whole screen left");
    lv_resume();                            /* a layout: on again first */
    draw_all();
    lv_frame();
}

static void np_full_enter(void)
{
    if (np_full)
        return;
    if (!mp_loaded()) {
        set_msg(_("Nothing is playing"));
        return;
    }
    if (fullvis[0]) {
        /* an AmigaAMP plug-in: its own window on Workbench (or the default
         * public screen), Ambiance's screen behind it as it attaches */
        char err[100];
        if (amp_launch(fullvis, err, sizeof(err)))
            set_msg(_("Starting the plug-in (Esc in it to come back)"));
        else
            set_msg(err);
        return;
    }
    if (!vis_available(VIS_FLOW)) {
        set_msg(!vf_possible(&g) ? _("Flow needs a graphics card's screen (15 to 32-bit)")
                                 : _("Flow: this player does not say what it plays"));
        return;
    }
    lv_hold();
    np_full = TRUE;
    np_full_fs = g.wb && gfx_fs_enter(&g);  /* a window: the whole display */
    np_full_first = TRUE;
    np_full_since = time_ms();
    np_full_serial = mp_serial();
    np_full_ctl = 0;
    nf_pointer(FALSE);
    amb_log("visualisation", np_full_fs ? "Flow: full screen (Esc, a click on it or fire: back)"
                                         : "Flow: the whole screen (Esc, a click on it or fire: back)");
}

/* what plays, over the picture for a moment: the title, and who by */
static void np_full_title(void)
{
    const char *t = mp_is_stream() ? (st_title()[0] ? st_title() : mp_stream_name()) : mp_title();
    const char *by = mp_is_stream() ? mp_stream_name() : mp_track_by();
    WORD x = U(16), y = g.h - U(12) - 2 * (g.font_h + U(4));
    if (!t || !t[0])
        return;
    gfx_text(&g, x, y, t, PEN_WHITE, PEN_BG, g.w - 2 * x, TS_BOLD);
    if (by && by[0] && strcmp(by, t))
        gfx_text(&g, x, y + g.font_h + U(4), by, PEN_TEXT, PEN_BG, g.w - 2 * x, TS_SMALL);
}

/* The controls: the same strip as a video's (anim.c's strip_draw), along
 * the foot of the screen: previous, play/pause, next, volume down, the
 * volume, up; the time, how far it has got, the title; full screen or the
 * window at the right end. Drawn whole once a second (or as something on
 * it changes), the bar alone in between: drawn over each frame, its text
 * would flicker. */
static WORD nf_px0, nf_px1, nf_cy, nf_fill;
static ULONG nf_sec;
static int nf_vol;
static BOOL nf_paused, nf_drawn;

static void np_full_layout(void)
{
    nf_h = g.font_h + U(12);
    nf_y = g.h - nf_h;
    nf_w = nf_h + U(6);
    nf_by0 = nf_y;
    nf_bx0 = 0;
    nf_bx1 = g.w - 1;
}

static void nf_bar(WORD fill)
{
    gfx_fill(&g, nf_px0, nf_cy - 1, nf_px1, nf_cy + 1, PEN_RULE);
    if (fill > nf_px0)
        gfx_fill(&g, nf_px0, nf_cy - 1, fill, nf_cy + 1, PEN_WHITE);
    gfx_fill(&g, fill - 2, nf_cy - 4, fill + 2, nf_cy + 4, PEN_WHITE);
    nf_fill = fill;
}

static void np_full_controls(void)
{
    static const UBYTE kind[NF_COUNT] = { GL_PREV, GL_PLAY, GL_NEXT, GL_VDOWN, GL_VUP, GL_FULL };
    struct MpStatus st;
    WORD i, x = U(8), cy, sx = gfx_mode_tall(g.mode) ? 2 : 1;
    BOOL paused = !mp_playing();
    const char *t = mp_is_stream() ? (st_title()[0] ? st_title() : mp_stream_name()) : mp_title();
    char a[16], b[16], line[48];
    np_full_layout();
    cy = nf_y + nf_h / 2;
    memset(&st, 0, sizeof(st));
    mp_status(&st);
    if (nf_drawn && st.elapsed_ms / 1000 == nf_sec && paused == nf_paused && mp_volume() == nf_vol) {
        /* only how far it has got, if it has moved a pixel */
        if (st.total_ms && nf_px1 > nf_px0) {
            WORD fill = nf_px0 + (WORD)((st.elapsed_ms > st.total_ms ? st.total_ms : st.elapsed_ms) *
                                        (ULONG)(nf_px1 - nf_px0) / st.total_ms);
            if (fill != nf_fill) {
                gfx_fill(&g, nf_px0 - 2, nf_cy - 4, nf_px1 + 2, nf_cy + 4, PEN_BG);
                nf_bar(fill);
            }
        }
        return;
    }
    gfx_fill(&g, 0, nf_y, g.w - 1, g.h - 1, PEN_BG);
    for (i = 0; i < NF_COUNT; i++) {
        WORD bx, by, bw, bh, ry, rx;
        int k = kind[i];
        if (i == NF_FULL) {
            x = g.w - U(8) - nf_w;          /* at the right end */
            k = g.wb ? GL_FULL : GL_UNFULL; /* (on its own screen: back to the page) */
        }
        if (i == NF_VUP) {                  /* the volume between - and + */
            WORD vw = gfx_textlen(&g, "100%", TS_BODY | TS_TABULAR) + U(8);
            sprintf(line, "%ld%%", (long)(mp_volume() * 100 / 64));
            gfx_text(&g, x + (vw - gfx_textlen(&g, line, TS_BODY | TS_TABULAR)) / 2, cy - g.font_h / 2, line,
                     PEN_TEXT, PEN_BG, 0, TS_BODY | TS_TABULAR);
            x += vw;
        }
        if (i == NF_PLAY && !paused)
            k = GL_PAUSE;
        nf_x[i] = x;
        bx = x + 1, by = nf_y + 2, bw = nf_w - 2, bh = nf_h - 4;
        ry = bh / 5 > 2 ? bh / 5 : 2;
        if (sx > 1)
            ry = ry / 2 > 1 ? ry / 2 : 1;
        rx = ry * sx;
        gfx_rrect(&g, bx, by, bw, bh, rx, ry, PEN_BAR);
        glyph_draw(g.rp, k, bx + bw / 2, by + bh / 2, glyph_rows(bh), sx, GPEN(PEN_WHITE), GPEN(PEN_DIM), 0);
        if (i < NF_FULL)
            x += nf_w;
    }
    x = nf_x[NF_VUP] + nf_w + U(10);        /* the time, after the buttons on the left */
    amb_time_text(a, st.elapsed_ms);
    if (st.total_ms) {
        amb_time_text(b, st.total_ms);
        sprintf(line, "%s / %s", a, b);
    } else
        strcpy(line, a);
    gfx_text(&g, x, cy - g.font_h / 2, line, PEN_TEXT, PEN_BG, 0, TS_BODY | TS_TABULAR);
    nf_px0 = x + gfx_textlen(&g, line, TS_BODY | TS_TABULAR) + U(8);   /* (the bar kept still) */
    nf_px1 = nf_x[NF_FULL] - 1 - U(8);
    nf_cy = cy;
    nf_fill = -1;
    if (t && t[0] && nf_px1 - nf_px0 > U(40)) {
        WORD nw = gfx_textlen(&g, t, TS_SMALL);
        if (nw > (nf_px1 - nf_px0) / 3)
            nw = (nf_px1 - nf_px0) / 3;
        gfx_text(&g, nf_px1 - nw, cy - g.font_h / 2, t, PEN_DIM, PEN_BG, nw, TS_SMALL);
        nf_px1 -= nw + U(8);
    }
    if (nf_px1 - nf_px0 > U(16))
        nf_bar(nf_px0 + (st.total_ms ? (WORD)((st.elapsed_ms > st.total_ms ? st.total_ms : st.elapsed_ms) *
                                               (ULONG)(nf_px1 - nf_px0) / st.total_ms)
                                     : 0));
    else
        nf_px1 = nf_px0;                    /* (no room: no bar) */
    nf_sec = st.elapsed_ms / 1000;
    nf_paused = paused;
    nf_vol = mp_volume();
    nf_drawn = TRUE;
}

/* full screen from the window, or the window again, Flow on either */
static void np_full_toggle_fs(void)
{
    if (!np_full_fs && !g.wb) {
        np_full_exit();                     /* its own screen already: the page */
        return;
    }
    nf_pointer(TRUE);                       /* (the window it was on: its own pointer back) */
    if (np_full_fs && gfx_fs_on()) {
        gfx_fs_leave(&g);
        np_full_fs = FALSE;
    } else if (g.wb) {
        np_full_fs = gfx_fs_enter(&g);
    }
    np_full_first = TRUE;                   /* (its size another: from black) */
    if (!np_full_ctl)
        nf_pointer(FALSE);
}

/* A message while Flow fills the screen: TRUE if it was Flow's. The mouse
 * moved shows the controls; a click on one of them presses it, elsewhere
 * goes back to the page; Space, N, P and Up/Down play as on Now playing;
 * F the full screen or the window; Esc, Return, Backspace back. */
static BOOL np_full_input(struct IntuiMessage *m)
{
    WORD x = m->MouseX - (g.win ? g.win->BorderLeft : 0), y = m->MouseY - (g.win ? g.win->BorderTop : 0);
    if (m->Class == IDCMP_MOUSEMOVE) {
        if (!np_full_ctl)
            nf_pointer(TRUE);
        np_full_ctl = time_ms() + 3000;
        return TRUE;
    }
    if (m->Class == IDCMP_MOUSEBUTTONS) {
        int i;
        if (m->Code != SELECTDOWN) {
            if (m->Code == MENUDOWN)
                np_full_exit();             /* the right button: back */
            return TRUE;
        }
        if (np_full_ctl && y >= nf_y) {
            np_full_ctl = time_ms() + 3000;
            for (i = 0; i < NF_COUNT; i++)
                if (x >= nf_x[i] && x < nf_x[i] + nf_w) {
                    if (i == NF_FULL)
                        np_full_toggle_fs();
                    else
                        np_press(i == NF_PREV ? NB_PREV : i == NF_PLAY ? NB_PLAY : i == NF_NEXT ? NB_NEXT
                                 : i == NF_VDOWN ? NB_VDOWN : NB_VUP);
                    nf_drawn = FALSE;       /* (its state shown again) */
                    return TRUE;
                }
            if (nf_px1 > nf_px0 && x >= nf_px0 - U(4) && x <= nf_px1 + U(4)) {
                np_seek_part((ULONG)(x < nf_px0 ? 0 : x > nf_px1 ? nf_px1 - nf_px0 : x - nf_px0),
                             (ULONG)(nf_px1 - nf_px0));   /* the bar: there */
                nf_drawn = FALSE;
            }
            return TRUE;                    /* (elsewhere on the strip: nothing) */
        }
        np_full_exit();
        return TRUE;
    }
    if (m->Class == IDCMP_VANILLAKEY) {
        switch (m->Code) {
        case ' ': np_press(NB_PLAY); return TRUE;
        case 'n': case 'N': np_press(NB_NEXT); return TRUE;
        case 'p': case 'P': np_press(NB_PREV); return TRUE;
        case 'f': case 'F': np_full_toggle_fs(); return TRUE;
        case 27: case 13: case 8: np_full_exit(); return TRUE;
        default: return TRUE;
        }
    }
    if (m->Class == IDCMP_RAWKEY && !(m->Code & IECODE_UP_PREFIX)) {
        if (m->Code == 0x4C)
            np_press(NB_VUP);               /* Up */
        else if (m->Code == 0x4D)
            np_press(NB_VDOWN);             /* Down */
        else if (m->Code == 0x4F)
            np_press(NB_PREV);              /* Left */
        else if (m->Code == 0x4E)
            np_press(NB_NEXT);              /* Right */
        return TRUE;
    }
    return m->Class == IDCMP_RAWKEY;        /* (key-ups) */
}

static void np_full_tick(void)
{
    ULONG now = time_ms();
    ULONG joy = gfx_joy(&g);
    static BOOL fire_was;
    BOOL fire = (joy & (JPF_BUTTON_RED | JPF_BUTTON_BLUE)) != 0;
    if (fire && !fire_was && !np_full_first) {
        fire_was = fire;
        np_full_exit();
        return;
    }
    fire_was = fire;
    if (!mp_loaded()) {
        np_full_exit();                     /* the music over: the page */
        return;
    }
    if (np_full_ctl && (LONG)(now - np_full_ctl) >= 0) {
        np_full_ctl = 0;                    /* the mouse still: the controls go */
        nf_pointer(FALSE);
    }
    {
        /* the strip at the bottom for what plays and the controls: black,
         * Flow above it (drawn over, they would flicker on a screen
         * without a buffer) */
        BOOL title = !np_full_ctl && now - np_full_since < 6000;
        WORD band = 0x7FFF;
        np_full_layout();
        if (np_full_ctl)
            band = nf_y;                    /* the controls' strip */
        else if (title)
            band = g.h - U(12) - 2 * (g.font_h + U(4)) - U(8);  /* what plays, for a moment */
        if (band < g.h / 2)
            band = g.h / 2;
        vf_clip(band);
        if (band != nf_band || np_full_first) {
            if (band < (WORD)g.h) {
                SetAPen(g.rp, GPEN(PEN_BG));
                RectFill(g.rp, 0, band, g.w - 1, g.h - 1);
            }
            nf_band = band;
            nf_drawn = FALSE;               /* (what is on it drawn again) */
        }
    }
    if (!vis_draw_full(&g, 0, 0, (WORD)g.w, (WORD)g.h, np_full_first)) {
        np_full_exit();
        set_msg(_("Flow could not be shown here"));
        return;
    }
    np_full_first = FALSE;
    if (mp_serial() != np_full_serial) {
        np_full_serial = mp_serial();       /* another track: said again */
        np_full_since = now;
        nf_band = 0x7FFF;                   /* (its strip cleared next frame) */
    }
    if (np_full_ctl)
        np_full_controls();
    else if (nf_band < (WORD)g.h && !nf_drawn) {
        np_full_title();                    /* once: Flow stays above it */
        nf_drawn = TRUE;
    }
}

static void handle_key(struct IntuiMessage *im)
{
    UWORD code = im->Code;
    if (im->Class == IDCMP_VANILLAKEY) {
        pointer_hide();
        set_mouse_mode(FALSE);              /* the keyboard in use */
    }
    BOOL shift = (im->Qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
    LONG page = list_visible() - 1;

    /* Amiga-F, either Amiga: a window to full screen and back (Right-Amiga
     * comes as Project > Full screen when the menus are there; Left-Amiga,
     * a PC keyboard's Windows key under UAE, as a key) */
    if ((im->Class == IDCMP_VANILLAKEY ? (code == 'f' || code == 'F') : code == 0x23) &&
        (im->Qualifier & (IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND))) {
        amb_log("window", g.wb ? "Amiga-F (a key): full screen" : "Amiga-F (a key): the window");
        if (window_offered())
            toggle_window();
        return;
    }
    /* Amiga-I, either Amiga: iconified (full screen too) */
    if ((im->Class == IDCMP_VANILLAKEY ? (code == 'i' || code == 'I') : code == 0x17) &&
        (im->Qualifier & (IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND))) {
        amb_log("iconify", "Amiga-I (a key)");
        gfx_iconify_asked = TRUE;
        return;
    }
    if (im->Class == IDCMP_VANILLAKEY && entry_on() && entry_key(code))
        return;                             /* a line being typed: its letters */
    if (im->Class == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX) && code != 0x7A && code != 0x7B &&
        (code < 0x60 || code > 0x67) && lv_on()) {
        pointer_hide();                     /* the keyboard in use, for a layout's keys too
                                             * (its Home row's choice shows again) */
        set_mouse_mode(FALSE);
    }
    if (lv_key(im))
        return;                             /* a layout's own (or the wheel, in one) */

    if (im->Class == IDCMP_RAWKEY) {
        if (code & IECODE_UP_PREFIX) {
            UWORD k = code & ~IECODE_UP_PREFIX;
            if ((k == 0x4C || k == 0x4D) && hold.dir && !hold.joy)
                hold_release();
            return;
        }
        if ((code < 0x60 || code > 0x67) &&
            !((im->Qualifier & IEQUALIFIER_REPEAT) && !key_is_down(code)))
            coast_interrupt();              /* a key: a coast stops (not Shift and the like
                                             * alone, nor the let-go key's stale repeats) */
        if (code != 0x7A && code != 0x7B) {
            pointer_hide();                 /* the keyboard in use (the wheel is the mouse) */
            if (code < 0x60 || code > 0x67)
                set_mouse_mode(FALSE);      /* (not Shift, Ctrl, Alt, Amiga alone) */
        }
        switch (code) {
        case 0x4C: case 0x4D: {                                      /* cursor up / down */
            LONG dir = code == 0x4C ? -1 : 1;
            disarm();
            if (shift) {
                act_move(dir * page);
            } else if (im->Qualifier & IEQUALIFIER_REPEAT) {
                /* main menu: the keyboard's own repeat, but not stale ones
                 * still queued after the key was let go */
                if (!hold_active() && key_is_down(code))
                    act_move(dir);
            } else {
                act_move(dir);
                if (focus != FOCUS_MENU)
                    hold_start(dir, FALSE);
            }
            break;
        }
        case 0x4E: disarm(); if (!np_lr(1)) act_right(); break;      /* cursor right */
        case 0x4F: disarm(); if (focus != FOCUS_MENU && !np_lr(-1)) act_back(); break; /* left */
        case 0x7A: disarm(); ui->wheel(-1, im->MouseX - g.win->BorderLeft,
                                       im->MouseY - g.win->BorderTop); break; /* NM_WHEEL_UP */
        case 0x7B: disarm(); ui->wheel(1, im->MouseX - g.win->BorderLeft,
                                       im->MouseY - g.win->BorderTop); break; /* NM_WHEEL_DOWN */
        }
        return;
    }
    /* IDCMP_VANILLAKEY */
    if (coast_interrupt() && code == 13)
        return;                             /* Return during a coast only stops it */
    if (im->Qualifier & IEQUALIFIER_NUMERICPAD) {
        switch (code) {
        case '8': case '2':                          /* keypad: the keyboard's repeat */
            disarm();
            if (!(im->Qualifier & IEQUALIFIER_REPEAT) || key_is_down(code == '8' ? 0x3E : 0x1E))
                act_move(code == '8' ? -1 : 1);
            return;
        case '4': disarm(); if (focus != FOCUS_MENU && !np_lr(-1)) act_back(); return;
        case '6': disarm(); if (!np_lr(1)) act_right(); return;
        case '5': disarm(); act_open(FALSE); return;
        case '9': disarm(); act_move(-page); return;
        case '3': disarm(); act_move(page); return;
        case 13: disarm(); act_open(TRUE); return;
        }
        return;
    }
    if (focus == FOCUS_CONTENT && np_on()) {
        /* the music's own keys */
        switch (code) {
        case ' ': disarm(); np_press(NB_PLAY); return;
        case '+': case '=': disarm(); np_press(NB_VUP); return;
        case '-': case '_': disarm(); np_press(NB_VDOWN); return;
        case 'm': case 'M': disarm(); np_press(NB_MUTE); return;
        case 'n': case 'N': case '>': disarm(); np_press(NB_NEXT); return;
        case 'p': case 'P': case '<': disarm(); np_press(NB_PREV); return;
        case 's': case 'S': disarm(); np_press(NB_STOP); return;
        case 'v': case 'V': disarm(); next_visual(); return;
        case 'f': case 'F': disarm(); np_full_enter(); return;
        case 'g': case 'G': disarm(); if (np_away()) np_goto_source(); return;
        case ',': disarm(); np_press(NB_BACK); return;
        case '.': disarm(); np_press(NB_FWD); return;
        }
        if (code != 27 && code != 8 && code != 13)
            return;                 /* no letters to jump to here */
    }
    switch (code) {
    case 27: scr_watch_now("Esc"); act_back(); return;   /* Esc: Back, twice at the menu quits */
    case 8: disarm(); act_back(); return;        /* Backspace */
    case 13: disarm(); act_open(TRUE); return;   /* Return: play */
    case ' ': disarm(); act_open(FALSE); return;
    }
    if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) {
        disarm();
        act_letter((char)code);
    }
}

/* Joystick in port 2 via lowlevel.library, polled on the 40 ms tick. */
static ULONG joy_last, joy_hold;
static BOOL joy_present;        /* a joystick or pad answered on port 2 */

static void joy_poll(void)
{
    const ULONG dirmask = JPF_JOY_UP | JPF_JOY_DOWN | JPF_JOY_LEFT | JPF_JOY_RIGHT;
    const ULONG btnmask = JPF_BUTTON_RED | JPF_BUTTON_BLUE | JPF_BUTTON_FORWARD |
                          JPF_BUTTON_REVERSE;
    ULONG st, dir, btn, newbtn, act = 0;
    if (!LowLevelBase)
        return;
    st = gfx_joy(&g);
    joy_present = (st & JP_TYPE_MASK) == JP_TYPE_JOYSTK || (st & JP_TYPE_MASK) == JP_TYPE_GAMECTLR;
    if (!joy_present)
        return;
    dir = st & dirmask;
    btn = st & btnmask;
    newbtn = btn & ~(joy_last & btnmask);
    if (dir) {
        if (dir != (joy_last & dirmask)) {
            joy_hold = 0;
            act = dir;
            if (hold.joy && hold.dir)
                hold_stop();
        } else if (++joy_hold >= 8 && (joy_hold - 8) % 3 == 0) {
            act = dir;          /* repeat after ~320 ms, then every ~120 ms */
        }
    } else {
        joy_hold = 0;
        if (hold.joy && hold.dir)
            hold_release();                 /* (not again each poll: a coast goes on) */
    }
    joy_last = dir | btn;
    if ((act || newbtn) && coast_interrupt())
        newbtn &= ~JPF_BUTTON_RED;          /* Fire during a coast only stops it */
    if (act || newbtn) {
        set_mouse_mode(FALSE);              /* the joystick in use: arrows, not bars */
        pointer_hide();
    }
    if (act & (JPF_JOY_UP | JPF_JOY_DOWN)) {
        LONG d = (act & JPF_JOY_UP) ? -1 : 1;
        disarm();
        if (joy_hold == 0) {
            act_move(d);        /* the press itself */
            if (focus != FOCUS_MENU)
                hold_start(d, TRUE);
        } else if (!hold_active()) {
            act_move(d);        /* main menu: plain repeat */
        }
    }
    else if (act & JPF_JOY_RIGHT) { disarm(); if (!np_lr(1)) act_right(); }
    else if (act & JPF_JOY_LEFT) { disarm(); if (focus != FOCUS_MENU && !np_lr(-1)) act_back(); }
    if (newbtn & JPF_BUTTON_RED) {
        disarm();
        act_open(FALSE);
        if (np_on() && focus == FOCUS_CONTENT)
            np_rep_start(np.btn, REP_JOY);  /* held: it repeats */
    }
    if (np.rep >= 0 && np.rep_src == REP_JOY && !(btn & JPF_BUTTON_RED))
        np_rep_stop();
    /* CD32 pad shoulder buttons: the next / previous letter in a list */
    if (newbtn & JPF_BUTTON_FORWARD) { disarm(); act_letter_step(1); }
    if (newbtn & JPF_BUTTON_REVERSE) { disarm(); act_letter_step(-1); }
    if (newbtn & JPF_BUTTON_BLUE) { disarm(); act_back(); }
}

/* ------------------------------------------------------------ main */
static void build_menu(void)
{
    /* Main menu order, top to bottom. */
    static const int order[] = { MOD_GAMES, MOD_DEMOS, MOD_MODS, MOD_MUSIC, MOD_RADIO,
                                 MOD_PICTURES, MOD_VIDEOS, MOD_SETTINGS, MOD_ABOUT };
    int i, m;
    nmenu = 0;
    if (mp_loaded())
        menu[nmenu++] = MOD_NOWPLAYING;     /* (while something plays or is paused) */
    for (i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) {
        m = order[i];
        if (m == MOD_RADIO && !(caps.net && (caps.mp3_mhi || caps.mp3_mpega || caps.mpega_own)))
            continue;           /* needs a network stack and an MP3 path */
        menu[nmenu++] = m;
    }
}

/* The menu's Now playing entry as playback is now: there while something
 * plays or is paused, gone when nothing is. The section chosen stays
 * chosen; Now playing gone from under the selection leaves the first
 * section's chosen (not while its page is open: it goes once that's left,
 * so a Stop on it doesn't throw the page away). */
static void np_menu_sync(void)
{
    BOOL on = mp_loaded(), has = nmenu > 0 && menu[0] == MOD_NOWPLAYING;
    int m, i;
    if (on == has)
        return;
    m = nmenu ? menu[menu_sel] : -1;
    if (!on && m == MOD_NOWPLAYING && focus != FOCUS_MENU)
        return;
    build_menu();
    menu_sel = 0;
    for (i = 0; i < nmenu; i++)
        if (menu[i] == m)
            menu_sel = i;
    if (!on && m == MOD_NOWPLAYING) {
        amb_log("navigation", "Now playing gone from the menu: its first section chosen");
        pane_pending = FALSE;
        build_root(menu[menu_sel]);
        draw_all();
        return;
    }
    amb_log("navigation", on ? "Now playing in the menu" : "Now playing gone from the menu");
    draw_menu();
}

static void say(const char *s)
{
    BPTR out = Output();
    if (out)
        FPuts(out, (CONST_STRPTR)s);
}

static int app_main(void);

/* Picture datatypes decode on the stack of whoever asks, and the viewer
 * asks from this task: WarpJPEG needs far more than the 4 KB a Workbench
 * icon or a plain CLI gives (an overflow crashed with #80000003). So
 * Ambiance runs on a stack of its own, MAIN_STACK bytes, whatever it was
 * started with. Statics, not locals: the stack moves under them. */
#define MAIN_STACK (128UL * 1024)
static struct StackSwapStruct main_sss;
static APTR main_stack;
static int main_rc;

int amb_cpu_ok(void);              /* cpucheck.c: built for the 68000 */

/* One Ambiance at a time: a second started (a double-click, a script)
 * gives way. Two at once fought over Paula's interrupt - the first to
 * quit put back the vector the second had saved, the second then put
 * back the first's, its code gone - and crashed after quitting. */
#define ONE_PORT "Ambiance.running"
static struct MsgPort *one_port;
static int main_stacked(void)
{
    struct Task *me = FindTask(NULL);
    if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower >= MAIN_STACK)
        return app_main();
    if (!(main_stack = AllocVec(MAIN_STACK, MEMF_ANY)))
        return app_main();
    main_sss.stk_Lower = main_stack;
    main_sss.stk_Upper = (ULONG)main_stack + MAIN_STACK;
    main_sss.stk_Pointer = (APTR)main_sss.stk_Upper;
    StackSwap(&main_sss);
    main_rc = app_main();
    StackSwap(&main_sss);
    FreeVec(main_stack);
    return main_rc;
}

int main(void)
{
    int rc;
    if (!amb_cpu_ok())
        return 20;                          /* a 68000/010: said in a requester */
    Forbid();
    if (FindPort((CONST_STRPTR)ONE_PORT)) {
        Permit();
        return 5;                           /* (another runs: it is the one) */
    }
    if ((one_port = CreateMsgPort())) {
        one_port->mp_Node.ln_Name = (char *)ONE_PORT;
        one_port->mp_Node.ln_Pri = -128;
        AddPort(one_port);
    }
    Permit();
    rc = main_stacked();
    if (one_port) {
        RemPort(one_port);
        DeleteMsgPort(one_port);
    }
    return rc;
}

static int app_main(void)
{
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    BOOL timer_open = FALSE;
    ULONG next_sec = 0, tick_us = 40000;
    int rc = 0;

    view_use(&classic_ui);          /* Classic draws (a layout, once one is on) */
    {
        /* the language first: everything said from here on is in it */
        char buf[32];
        if (GetVar((CONST_STRPTR)LANG_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            amb_strlcpy(lang_set, buf, sizeof(lang_set));
        amb_lang_scan();
        amb_lang_open(lang_set);
    }
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!IntuitionBase || !GfxBase) {
        say(_("Ambiance needs Kickstart 3.0 or newer.\n"));
        rc = 20;
        goto out;
    }
    LayersBase = OpenLibrary((CONST_STRPTR)"layers.library", 39);
    if (!LayersBase) {
        say(_("Ambiance needs Kickstart 3.0 or newer.\n"));
        rc = 20;
        goto out;
    }
    amb_card_init();               /* (before any process that reaches the card) */
    load_debug();
    dbg_mem_baseline();
    read_start_args();
    gfx_iconify_gadget = icon_available();  /* (OS 3.2: the window's gadget) */
    window_menus_make();
    load_debug();
    load_dbuf();
    load_https();
    load_jump();
    load_visual();
    amb_log_open(debug_on);
    open_extra_libs();
    {
        /* PROGDIR:Ambiance.noloader: do the reading in the UI (to compare) */
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR oldwin = me->pr_WindowPtr;
        BPTR l;
        dir_tick_task = FindTask(NULL);
        dir_tick = busy_dir_tick;
        me->pr_WindowPtr = (APTR)-1;
        l = Lock((CONST_STRPTR)"PROGDIR:Ambiance.noloader", SHARED_LOCK);
        me->pr_WindowPtr = oldwin;
        if (l) {
            UnLock(l);
            amb_log("loader", "off (Ambiance.noloader)");
        } else if (ldr_start()) {
            extern struct Task *ldr_task, *ldr_task2, *ldr_task3;
            char m[128];
            sprintf(m, "started (tasks: Ambiance %08lx, loader %08lx, pictures %08lx, "
                       "picture now %08lx)",
                    (unsigned long)FindTask(NULL), (unsigned long)ldr_task, (unsigned long)ldr_task2,
                    (unsigned long)ldr_task3);
            amb_log("loader", m);
        } else {
            amb_log("loader", "could not start: reading in the UI");
        }
    }

    caps_detect(&caps);
    jpeg_thumb_init();                  /* before the loader: its note's lock */
    https_apply();                  /* the setting, now that AmiSSL is looked for */
    {
        /* HTTPS's root certificates (tls.c), by their full name: the
         * network and playback processes have no PROGDIR: of their own */
        char cp[256];
        BPTR l;
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR oldwin = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;
        if ((l = Lock((CONST_STRPTR)"PROGDIR:Certs/cacert.pem", SHARED_LOCK))) {
            if (NameFromLock(l, (STRPTR)cp, sizeof(cp)))
                tls_set_certs(cp);
            UnLock(l);
        }
        me->pr_WindowPtr = oldwin;
    }
    plex_load(caps.cpu);            /* after: it needs the CPU */
    jf_load();
    plex.flac = caps.flacdec;
    plex.jpeg_card = pic_on_card("JPEG");   /* the ZZ9000's datatype reads JPEG */
    view_jpeg_card = plex.jpeg_card;        /* ...the viewer's too, not jdec */
    {
        /* the viewer's JPEGs decoded on the card at the size shown, where it
         * hands back pixels the 68k can read (zz9k.c); Ambiance/ViewCard 0:
         * not (to compare) */
        char v[4];
        zz_view_on = caps.zz_cpusurf &&
                     !(GetVar((CONST_STRPTR)"Ambiance/ViewCard", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0 &&
                       v[0] == '0');
        zz_view_hold(TRUE);
        if (caps.zz9000)
            amb_log("viewer", zz_view_on ? "JPEGs decoded on the ZZ9000, at the size shown"
                              : caps.zz_cpusurf ? "ViewCard 0: JPEGs not decoded on the ZZ9000"
                              : "JPEGs not decoded on the ZZ9000: it hands back no pixels the 68k can read");
    }
    {
        char v[4];
        view_own_off = GetVar((CONST_STRPTR)"Ambiance/ViewOwn", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0 &&
                       v[0] == '0';
        if (view_own_off)
            amb_log("viewer", "ViewOwn 0: every picture by the datatypes");
    }
    if (caps.zz9000) {
        char m[160];
        sprintf(m, "ZZ9000 on Zorro %s; mpega.library %s; JPEG %s, PNG %s",
                caps.zz9000 == 3 ? "III" : "II",
                caps.mpega_zz ? "the ZZ9000 SDK's" : caps.mp3_mpega ? "not the card's" : "absent",
                plex.jpeg_card ? "by zz9k-picture.datatype" : "by another datatype",
                pic_on_card("PNG") ? "by zz9k-picture.datatype" : "by another datatype");
        amb_log("zz9000", m);
    }
    load_sound_out();
    load_folders();
    songs_can_play(caps.mp3_mpega || caps.mpega_own, caps.flacdec);   /* what music lists show */
    load_net_if();                  /* after: the interface's address, toward the server */
    load_radio_settings();
    ags_init(&ags);
    igame_start(TRUE);              /* iGame's list (looked for while unset) */
    build_menu();
    load_screen_mode();
    load_style();
    {
        caps_wb_read();
    }

    gfx_menu_labels((const char *const *)mod_names, MOD_COUNT);
    amb_log("window: chosen by", win_chosen_by);
    {
        BOOL opened = gfx_open(&g, &caps, screen_mode, ui_style) ||
                      (screen_mode != SM_AUTO && gfx_open(&g, &caps, SM_AUTO, ui_style));
        if (!opened && caps.wb_rtg) {
            /* Automatic on the card would not open: the chipset's, as before */
            amb_log("screen", "the graphics card's mode would not open: a native one");
            caps.wb_rtg = FALSE;
            opened = gfx_open(&g, &caps, SM_AUTO, ui_style);
        }
        if (!opened) {
            say(_("Ambiance could not open its screen.\n"));
            rc = 20;
            goto out;
        }
    }
    wb_note_fallback();             /* (full screen where a window was wanted: watched) */

    tport = CreateMsgPort();
    treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(*treq)) : NULL;
    if (treq && !OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0)) {
        timer_open = TRUE;
        dbg_set_timer(treq->tr_node.io_Device);
        mouse_last = time_ms();             /* the pointer stays a while at first */
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs = 0;
        treq->tr_time.tv_micro = 40000;
        SendIO((struct IORequest *)treq);
    }
    {
        /* the smooth style's text blended in 16 steps where this Amiga is
         * fast (as measured, so after the timer: a JIT's 68030 too, never a
         * 25 MHz one), on a graphics card's 15-32 bit screen;
         * Ambiance/TextBlend "on" or "off" says otherwise */
        char v[8];
        BOOL on = plex_tier() >= 2;
        if (GetVar((CONST_STRPTR)"Ambiance/TextBlend", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0)
            on = !amb_stricmp(v, "on");
        sm_alpha_allow(on);
    }

    if (debug_on)
        dbg_cpu_start();
    resume_load();                  /* what was playing at the last quit: loaded, stopped */
    lv_resume();                    /* a layout: on before anything is drawn (the classic
                                     * screen showed for a few frames first) */
    {
        /* where the last run left off: its section, its trail, its pane */
        int at, i;
        BOOL in_list;
        nav_load(&at, &in_list);
        menu_sel = 0;
        for (i = 0; i < nmenu; i++)
            if (menu[i] == at)
                menu_sel = i;
        build_root(menu[menu_sel]);
        if (in_list && menu[menu_sel] == at && cur() &&
            (cur()->type != PT_LIST || cur()->list.count) && !section_inert())
            focus = FOCUS_CONTENT;
        if (at >= 0)
            amb_log("navigation", in_list ? "back where the last run left off (in the list)"
                                            : "back where the last run left off (in the menu)");
        nav_sum_seen = nav_sum_written = nav_sum();
    }
    draw_all();
    if (lv_on()) {
        lv_frame();                 /* the layout's first frame, now */
        gfx_present(&g);
    }
    if (screen_mode != SM_AUTO && g.mode != screen_mode && !g.wb) {
        screen_mode = SM_AUTO;      /* not saved: the choice comes back if it works again */
        set_msg(_("Saved screen mode not available here: using Automatic"));
    } else {
        note_style();
    }

    while (running) {
        if (gfx_close_asked)
            break;                  /* the window's close gadget (the viewer saw it, say) */
        np_menu_sync();             /* (the menu's Now playing entry) */
        if (gfx_iconify_asked) {
            gfx_iconify_asked = FALSE;  /* its gadget, Project > Iconify, Amiga-I */
            iconify();
        }
        if (gfx_mini_asked) {
            gfx_mini_asked = FALSE;     /* the window's zoom gadget */
            mini_enter();
        }
        ULONG wsig = g.win ? 1UL << g.win->UserPort->mp_SigBit : 0;
        loop_wakes++;
        ULONG tsig = timer_open ? 1UL << tport->mp_SigBit : 0;
        ULONG lsig = ldr_sigmask();
        ULONG nsig = net_sigmask();
        ULONG bsig = gfx_wb_sigmask();
        ULONG isig = icon_sigmask();
        ULONG msig = mini_on && mg.win ? 1UL << mg.win->UserPort->mp_SigBit : 0;
        ULONG mask = wsig | tsig | lsig | nsig | bsig | isig | msig | SIGBREAKF_CTRL_C;
        struct TextBox *atb = cur_tb();
        /* While a text box glides, poll instead of sleeping; WaitTOF below
         * paces the loop to one step per frame. */
        BOOL busy = !iconified && !mini_on && !np_full &&
                    ((atb && tb_animating(atb)) || hold_active() || pane_pending || det.pending ||
                     tb_animating(&pv_tb));
        ULONG sig;
        if (!np_full) {
            /* (Flow on the whole screen: the page's own drawing waits) */
            if (loader_due())
                preview_arrived();
            menu_layout();              /* the focus moved by another way: the column follows */
        }
        if (busy) {
            sig = SetSignal(0, mask) & mask;
        } else {
            dbg_sleep_begin();
            sig = Wait(mask);
            dbg_sleep_end();
        }
        if (sig & SIGBREAKF_CTRL_C)
            break;
        if (sig & isig) {
            int a = icon_poll();
            if (a == ICON_QUIT) {
                amb_log("iconify", "Workbench's Tools > Quit Ambiance");
                break;
            }
            if (a == ICON_OPEN)
                uniconify("its AppIcon opened: no longer iconified");
        }
        if (sig & msig) {
            mini_input();               /* the mini player's window */
            if (!running)
                break;
        }
        if (sig & bsig)
            gfx_wb_signalled();         /* (watched prefs changed: OS 3.0, 3.1) */
        if (gfx_wb_event()) {
            wb_follow();                /* Workbench closing: our window too, until it is back */
            if (!running)
                break;
            continue;
        }
        if ((sig & lsig) && !np_full)
            preview_arrived();          /* the loader brought something */
        if (sig & nsig)
            radio_collect();            /* radio-browser.info answered */
        if (sig & tsig) {
            if (GetMsg(tport)) {
                if (iconified) {
                    iconified_tick();       /* the music kept going, nothing more */
                    tick_us = 200000;
                    goto tick_again;
                }
                amp_frame();                /* AmigaAMP's plug-in, if one is attached */
                if (np_full) {
                    /* Flow on the whole screen: a frame, the music kept going */
                    static ULONG nf_sec;
                    np_full_tick();
                    np_tick();
                    mp_paula_check();
                    if (!nf_sec || (LONG)(time_ms() - nf_sec) >= 0) {
                        nf_sec = time_ms() + 1000;
                        mp_tempo_check();
                        flac_slow_check();
                        if (debug_on)
                            dbg_load_log();
                    }
                    tick_us = 40000;
                    goto tick_again;
                }
                if (mini_on) {
                    mini_tick();            /* ...and the mini player up to date */
                    tick_us = mp_running() || mn.sl_drag ? 40000 : 200000;
                    goto tick_again;
                }
                joy_poll();
                window_title_watch();       /* (a window: its title bar double-clicked) */
                if (gfx_resized_at && (LONG)(time_ms() - gfx_resized_at) >= RESIZE_SETTLE)
                    window_resized();   /* (a resize's last NEWSIZE, settled) */
                if (g.wb && mouse_mode && !foot_hidden && !msg[0] && foot_since &&
                    time_ms() - foot_since >= FOOT_SEEN_MS)
                    foot_show(FALSE);   /* the mouse's: its tips seen, their room given over */
                if (foot_redraw) {
                    foot_redraw = FALSE;
                    if (foot_hidden)
                        draw_all();     /* a message's footer gone from over the content */
                }
                /* the letter lingers ~0.5 s after the glide ends */
                if (ov.on && !hold_active()) {
                    if (!ov.linger)
                        ov.linger = 13;
                    else if (--ov.linger == 0)
                        ov_hide();
                }
                /* once a second by the clock, not by counting ticks: a tick
                 * comes late when the CPU is busy (a station decoding), and
                 * counted ticks made the "second" two or three long */
                if (g.win && (!next_sec || (LONG)(time_ms() - next_sec) >= 0)) {
                    ULONG now_ms = time_ms();
                    next_sec = (next_sec && (LONG)(now_ms - next_sec) < 1000) ? next_sec + 1000
                                                                              : now_ms + 1000;
                    struct DateStamp ds;
                    DateStamp(&ds);
                    if (debug_on) {
                        fps_shown = gfx_flips - fps_flips0;
                        fps_flips0 = gfx_flips;
                        cpu_load = dbg_cpu_sample();
                        app_load = dbg_app_sample();
                    }
                    ui->readouts(ds.ds_Minute != last_minute, debug_on);   /* the header's */
                    if (debug_on)
                        dbg_load_log();
                    nav_tick_1s();          /* the trail kept, once it rests */
                    scr_watch();            /* a screen gone blank: why (log) */
                    mp_tempo_check();
                    flac_slow_check();      /* a FLAC track running dry: said once */
                    plink_tick();           /* signing in at plex.tv: asked after */
                    whd_tick();             /* WHDLoad coming: how much */
                    dvd_watch();            /* Videos: a DVD put in or taken out */
                }
                mp_paula_check();           /* every tick: a moment's change seen too */
                ui->tick();                 /* the view's spinners: the preview's, a Radio
                                             * list's, if they are waiting */
                if (!rq_more && cur() && cur()->plex && cur()->plex_letters && !pane_pending)
                    plex_more_ask(cur());   /* a long list's part now shown, if not read */
                np_tick();                  /* the music: its page, the next module */
                if (g.win && ptr_hidden != g.win && !gut.drag &&
                    (LONG)(time_ms() - mouse_last) >= BAR_HIDE)
                    pointer_hide();         /* the mouse at rest: the pointer goes */
                if (bar_up && !gut.drag && (LONG)(time_ms() - mouse_last) >= BAR_HIDE) {
                    bar_up = FALSE;         /* the mouse at rest: the bar goes */
                    if (cur() && cur()->type == PT_LIST && !pane_pending)
                        draw_gutter(cur());
                }
                /* 25 a second for a joystick, a fading letter or the
                 * spinner (each step, every 80 ms, then comes on time);
                 * otherwise 5 a second, which still notices a joystick
                 * plugged in. */
                tick_us = (joy_present || ov.on || pv_spinning() || radio_waiting() || np_live() ||
                           amp_attached() ||
                           window_title_hover() ||
                           np.flash >= 0 || np.rep >= 0 ||
                           (np_list_on() && mp_playing())) ? 40000 :
                          mp_playing() ? 120000 : 200000;  /* the header's bars dance */
            tick_again:
                if (running) {
                    treq->tr_node.io_Command = TR_ADDREQUEST;
                    treq->tr_time.tv_secs = 0;
                    treq->tr_time.tv_micro = tick_us;
                    SendIO((struct IORequest *)treq);
                }
            }
        }
        if (g.win && (sig & wsig)) {
            struct IntuiMessage *im;
            while (g.win && (im = (struct IntuiMessage *)GetMsg(g.win->UserPort))) {
                struct IntuiMessage copy = *im;
                ReplyMsg((struct Message *)im);
                if (iconified)
                    continue;               /* (its screen behind Workbench's: let go) */
                if (np_full && np_full_input(&copy))
                    continue;               /* (Flow on the whole screen: its own) */
                if (copy.Class == IDCMP_MOUSEMOVE)
                    gfx_window_rmb(&g, copy.MouseY);    /* title bar: the menus */
                if (gfx_window_msg(&g, copy.Class, copy.Code)) {
                    if (gfx_close_asked)
                        running = FALSE;    /* the close gadget: as Esc on the menu twice */
                } else if (copy.Class == IDCMP_MENUPICK) {
                    window_menu(copy.Code);
                } else if (gfx_window_sizing(&g, copy.Class, copy.Code, copy.MouseX, copy.MouseY)) {
                    ;                       /* the grip's (a window) */
                } else if (copy.Class == IDCMP_INACTIVEWINDOW)
                    hold_stop();    /* the key-up will not come to us */
                else if (copy.Class == IDCMP_MOUSEBUTTONS || copy.Class == IDCMP_MOUSEMOVE) {
                    ui->mouse(&copy);       /* (the view's hit tests: Classic's, input.c) */
                }
                else if (copy.Class == IDCMP_RAWKEY || copy.Class == IDCMP_VANILLAKEY)
                    handle_key(&copy);
                if (!g.win)
                    break;      /* a launch closed and reopened the window */
            }
        }
        lv_frame();                     /* a layout: the screen drawn again, if it changed */
        if (g.win && !np_full && (hold_active() || pane_pending || det.pending || tb_animating(&pv_tb) ||
                                  ((atb = cur_tb()) && tb_animating(atb)))) {
            ULONG tp = time_ms();
            BOOL flipped = gfx_present(&g);
            if (hold.dir && (flipped || g.wb))
                hold.present_ms += time_ms() - tp;  /* (a window's copy waits for no flip) */
            if (!flipped) {             /* a flip paces the frame, or else */
                dbg_sleep_begin();
                WaitTOF();
                dbg_sleep_end();
            }
            pane_tick();
            det_tick();
            if (tb_animating(&pv_tb))
                tb_step(&g, &pv_tb);
            hold_tick();
            if ((atb = cur_tb()) && tb_animating(atb))
                tb_step(&g, atb);
        } else {
            gfx_present(&g);            /* what this wake drew, shown */
        }
    }

out:
    {
        char m[64];
        sprintf(m, "%ld read ahead, %ld waited for", (long)det.ready, (long)det.waited);
        amb_log("previews", m);
    }
    amb_log("quitting", "");
    lv_release();                   /* a layout closed and put out of memory: left open, the
                                     * next Ambiance (a newer one) was handed the old one */
    amp_host_end();                 /* (a plug-in asked to go, its port taken down) */
    resume_store();                 /* what plays now, for the next start */
    vo_end();                       /* (Picasso96API.library, if a video opened it) */
    st_mpv_close();                 /* (mpegv.library, if a video decoded by the CPU did) */
    {
        /* the volume, for the next start (muted: the one before, never silent) */
        char v[16];
        int vol = vol_unmuted >= 0 ? vol_unmuted : mp_volume();
        sprintf(v, "%ld", (long)(vol > 0 ? vol : 32));
        save_setting("Ambiance/Volume", v);
    }
    radio_stop();
    if (depth)
        nav_store(TRUE);            /* where to start next time */
    dbg_cpu_stop();
    ov_free();
    if (timer_open) {
        if (!CheckIO((struct IORequest *)treq))
            AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
        CloseDevice((struct IORequest *)treq);
    }
    if (treq)
        DeleteIORequest((struct IORequest *)treq);
    if (tport)
        DeleteMsgPort(tport);
    amb_log("timer closed", "");
    mini_art_free();                /* (the mini player's cover: before the loader stops) */
    pv_flush();
    pics_forget();
    ldr_stop();
    anim_fact_forget();             /* (after the loader's processes: they read it) */
    mpeg_thumb_forget();
    pages_clear();
    amb_log("pages freed", "");
    plex_art_forget();              /* (RAM:'s covers and photos: memory, not kept) */
    beets_forget();
    mp_shutdown();
    if (np.list.pool)
        list_free(&np.list);
    if (gfx_fs_on())
        gfx_fs_leave(&g);           /* (Flow's full screen, as it quit) */
    ptr_hidden = NULL;              /* a new window comes with its pointer */
    spin_free();
    ov_bm_free();
    if (mini_on)
        mini_drop_window();         /* (the mini player's window, on Workbench) */
    screen_close();
    amb_log("screen closed", "");
    {
        WORD l, t, w, h;
        UWORD k;
        if (gfx_window_box(&l, &t, &w, &h, &k)) {   /* where the window was left */
            char b[48];
            sprintf(b, "%ld %ld %ld %ld %ld", (long)l, (long)t, (long)w, (long)h, (long)k);
            save_setting(WINBOX_VAR, b);
        }
    }
    gfx_wb_watch_end();                 /* (Workbench's hook out: its process calls it) */
    gfx_quit();
    icon_end();                         /* (the AppIcon, if iconified at the end) */
    if (ptr_blank)
        FreeMem(ptr_blank, 6 * sizeof(UWORD));
    ags_free(&ags);
    ig_free(&igcat);
    zz_view_hold(FALSE);            /* (after the loader stopped: it may have been decoding) */
    if (LowLevelBase)
        CloseLibrary(LowLevelBase);
    if (DataTypesBase)
        CloseLibrary(DataTypesBase);
    if (AslBase)
        CloseLibrary(AslBase);
    if (DiskfontBase)
        CloseLibrary(DiskfontBase);
    if (LayersBase)
        CloseLibrary(LayersBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    amb_lang_quit();
    vis_stop();
    /* (no flush_memory first: putting idle libraries out put out ahi.device
     * while the ZZ9000AX's driver stayed, and the next program's sound hung
     * the Amiga as it began; what stays in memory is listed instead) */
    if (debug_on)
        dbg_mem_report();
    amb_log("libraries closed, exiting", "");
    amb_log_close();
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

/* ------------------------------------------------------------ layouts (lhost.h) */
/* A layout chosen in Settings > Display > Layout draws the screen in its
 * own style while it can: a graphics card's 15-32 bit screen in the
 * Smooth style, nothing else over it (the viewer, a video, Flow on the
 * whole screen, the mini player). The program goes on as ever underneath:
 * its drawing is only counted (gfx_off), and each pass of the main loop
 * that counted some, the layout draws the screen again from the models
 * below. Left Shift held as Ambiance starts: the classic layout this
 * time (a layout that fails to draw can always be left). */
#define LAYOUT_VAR "Ambiance/Layout"
#define LV_MAX 8                    /* layouts offered in Settings */

static struct {
    BOOL read, skip, on, full;
    char file[32];                  /* chosen: "showcase.layout", "" classic */
    char name[32];                  /* its name, once known */
    const struct LayoutEngine *eng;
    struct LkOut out;
    struct LayoutHost host;
    struct Screen *scr;             /* the screen it draws on (opened again: again) */
    struct Window *win;
    UWORD w, h;
    struct Screen *tried;           /* tried there and could not: not again */
    UWORD tried_w, tried_h;
    int drawn;                      /* the screen drawn last (LS_*), -1 none */
    ULONG tick_at;
    char clock[24];
    struct LayoutInfo found[LV_MAX];
    int nfound;
    /* what the generic page was drawn with: a move alone is drawn as one
     * (changed(LC_SELECT)), not the whole screen */
    const struct Page *last_p;
    const void *last_items;
    LONG last_sel, last_count;
    int last_depth;
    char last_letter;
    ULONG last_msg;
    LONG last_pos;                  /* a text page's: where it was scrolled to */
    ULONG last_now;                 /* what plays, as drawn (lv_now_sig): its time apart */
    ULONG last_time;                /* ...its time, the clock, the Debug line as drawn */
    int wait_phase;                 /* the waiting spinner's step drawn, -1 none */
    struct Smooth *sm;              /* the smooth font lent to g.sm while it draws, where the
                                     * classic style is Pixel (a layout's text is smooth
                                     * whatever the classic's style): NULL none */
} lv = { .drawn = -1, .wait_phase = -1 };

BOOL lv_on(void)
{
    return lv.on;
}

static BOOL lv_chosen(void)
{
    return lv.file[0] != 0;
}

/* Layouts on native screens turned on or off: tried again on this screen
 * (on), or off it (off: lv_sync finds it no longer suits) */
static void lv_native_changed(void)
{
    lv.tried = NULL;
}

/* The layout is the view driven (the classic's drawing functions return
 * at once, noting it); and the note: it draws again (lv_frame) */
static BOOL lv_dirty;
static ULONG lv_what;                       /* what was noted (LC_*; LV_WHOLE: anything) */
#define LV_WHOLE 0x80000000UL
static void lv_note_what(ULONG what)
{
    lv_dirty = TRUE;
    lv_what |= what;
}
/* Who asked for whole draws (Debug's "wholes" line: by where they were
 * called from, as "unseen"): a whole draw is the slow kind */
#define LV_WHO 6
static const void *lv_who_at[LV_WHO];
static BOOL lv_sp_up;          /* the big letter up by sprites (h_letter; lk_close frees them) */
static ULONG lvp_n[5], lvp_ms[5];      /* drawn in part: what plays, pictures, a move, details;
                                         * the layout's ticks that drew (a glide's steps) */
static ULONG lvw_why[8], lvw_what;     /* whole draws: full, noted, unseen, else (of those:
                                         * the message, what plays, the page, the selection
                                         * the same), and what was noted */
static ULONG lv_who_n[LV_WHO];
static __attribute__((noinline)) void lv_note(void)
{
    const void *at = __builtin_return_address(0);
    int i, k = -1;
    lv_note_what(LV_WHOLE);
    for (i = 0; i < LV_WHO; i++)
        if (lv_who_at[i] == at || (!lv_who_at[i] && k < 0)) {
            if (lv_who_at[i] == at) {
                k = i;
                break;
            }
            k = i;
        }
    if (k >= 0) {
        lv_who_at[k] = at;
        lv_who_n[k]++;
    }
}
static void lv_who_report(void)
{
    char t[360];
    int i;
    t[0] = 0;
    for (i = 0; i < LV_WHO && lv_who_at[i]; i++) {
        LONG d = (LONG)((const UBYTE *)lv_who_at[i] - (const UBYTE *)lv_frame);
        if (strlen(t) < sizeof(t) - 30)
            sprintf(t + strlen(t), "%s%s%ld x%lu", t[0] ? ", " : "", d < 0 ? "-" : "+", (long)(d < 0 ? -d : d),
                    (unsigned long)lv_who_n[i]);
        lv_who_at[i] = NULL;
        lv_who_n[i] = 0;
    }
    if (lvw_why[0] || lvw_why[1] || lvw_why[2] || lvw_why[3]) {
        sprintf(t + strlen(t), "%sasked whole %lu, noted %lu, unseen %lu, else %lu (message %lu, playing %lu, "
                "page %lu, selection kept %lu; noted %lx)", t[0] ? "; " : "", (unsigned long)lvw_why[0],
                (unsigned long)lvw_why[1], (unsigned long)lvw_why[2], (unsigned long)lvw_why[3],
                (unsigned long)lvw_why[4], (unsigned long)lvw_why[5], (unsigned long)lvw_why[6],
                (unsigned long)lvw_why[7], (unsigned long)lvw_what);
        memset(lvw_why, 0, sizeof(lvw_why));
        lvw_what = 0;
    }
    if (t[0])
        amb_log("wholes", t);
    if (lvp_n[0] || lvp_n[1] || lvp_n[2] || lvp_n[3] || lvp_n[4]) {
        sprintf(t, "what plays %lu (%lu ms), pictures %lu (%lu ms), moves %lu (%lu ms), details %lu (%lu ms), "
                "glide steps %lu (%lu ms)",
                (unsigned long)lvp_n[0], (unsigned long)lvp_ms[0], (unsigned long)lvp_n[1], (unsigned long)lvp_ms[1],
                (unsigned long)lvp_n[2], (unsigned long)lvp_ms[2], (unsigned long)lvp_n[3], (unsigned long)lvp_ms[3],
                (unsigned long)lvp_n[4], (unsigned long)lvp_ms[4]);
        amb_log("parts", t);
        memset(lvp_n, 0, sizeof(lvp_n));
        memset(lvp_ms, 0, sizeof(lvp_ms));
    }
}

/* The screen under the layout changed (double buffering on or off, its
 * buffers swapped for others): all of it drawn and shown again, whatever
 * else moved */
static void lv_redraw(void)
{
    lv_note();
    lv.full = TRUE;
}

/* While a slideshow's or video's controls are drawn (lv_controls_draw):
 * their strip, which nothing goes outside (h_clip) */
static struct LvClip lv_ctl;

/* ---- a slideshow's and a video's controls, drawn by the layout in its
 * style (LControls) where it has them and the screen is a graphics card's
 * of 15 to 32 bits (g as it is now: a window, full screen, Ambiance's) */
static BOOL lv_ctl_open;            /* the layout opened for them alone (a viewer, a video: lv_hold
                                     * closed it) */
static BOOL lv_screen_can(void);
static BOOL lv_lend_font(void);
static ULONG *lv_mem;               /* the controls drawn in memory first (an AGA screen) */
static LONG lv_mem_n;
static ULONG lv_ctl_rgb[VW_BAR_PENS];
static WORD lv_ctl_first[VW_BAR_PENS];  /* ...each one's first row in the controls (-1: none) */
#define LV_CTL_ROWS 256
static ULONG lv_ctl_row[LV_CTL_ROWS];   /* ...each row's colour of the background pen */
static WORD lv_ctl_dom, lv_ctl_h;       /* ...which pen that is; the rows */
static int lv_ctl_n = VW_BAR_PENS / 2;  /* ...how many colours the controls have */
static BOOL lv_ctl_pick = TRUE;         /* ...the colours chosen at the next whole draw (once
                                         * a viewer: kept, so the copper's list stays the same) */
static BOOL lv_ctl_8(void)
{
    return !g.truecolor && g.depth == 8 && g.scr && lv_screen_can();
}
BOOL lv_controls_keep(void)
{
    return lv_ctl_8() && lv.file[0] && cop_ok(g.scr);
}
const ULONG *lv_controls_rgb(void)
{
    return lv_ctl_rgb;
}
const WORD *lv_controls_first(void)
{
    return lv_ctl_first;
}
int lv_controls_ncol(void)
{
    return lv_ctl_n;
}
const ULONG *lv_controls_rows(WORD *dom, WORD *n)
{
    *dom = lv_ctl_dom;
    *n = lv_ctl_h;
    return lv_ctl_row;
}
void lv_controls_fresh(void)
{
    lv_ctl_pick = TRUE;
}
BOOL lv_controls_ok(void)
{
    const struct LayoutEngine *e;
    if (!g.rp || !((g.truecolor && !g.planar) || lv_ctl_8()))
        return FALSE;
    if (!g.sm && !lv_ctl_8())
        return FALSE;
    if (lv.on || lv_ctl_open)
        return g.sm && lv.eng && lv.eng->controls && lv.eng->controls_h;
    /* a layout chosen, and opened before on this run (its host set up):
     * opened again, as it was, for the controls alone */
    e = layout_loaded();
    if (!lv.file[0] || !e || !e->controls || !e->controls_h || !lv.host.fill || strcmp(e->name, lv.name))
        return FALSE;
    if (!g.sm && !lv_lend_font())       /* (AGA's Pixel style: the smooth font lent again) */
        return FALSE;
    lv.host.w = g.w;
    lv.host.h = g.h;
    clock_text(lv.clock);
    if (e->open && !e->open(&lv.host))
        return FALSE;
    lv.eng = e;
    lv_ctl_open = TRUE;
    return TRUE;
}

/* The viewer or a video done with them: the layout closed again (lv_resume
 * opens it as ever) */
void lv_controls_end(void)
{
    if (lv_ctl_open && lv.eng && lv.eng->close)
        lv.eng->close();
    lv_ctl_open = FALSE;
}

WORD lv_controls_h(BOOL timed)
{
    return lv_controls_ok() ? lv.eng->controls_h(timed) : 0;
}

/* Drawn into bm (g.w x c->h, a friend of the screen's bitmap), which
 * stands for the screen's rows from c->y; what: 0 all, LC_PLAYING the
 * time. FALSE: not drawn. */
BOOL lv_controls_draw(struct LControls *c, ULONG what, struct BitMap *bm)
{
    struct LkOut keep, o;
    BOOL mem = !g.truecolor, probe = FALSE;
    LONG n = (LONG)g.w * c->h;
    if (!lv_controls_ok() || !bm)
        return FALSE;
    if (mem) {                              /* (AGA: in memory, then the kept registers) */
        if (n * 2 > lv_mem_n) {             /* (and as much again: the probe) */
            FreeVec(lv_mem);
            lv_mem_n = 0;
            if (!(lv_mem = AllocVec((ULONG)n * 2 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR)))
                return FALSE;
            lv_mem_n = n * 2;
        }
        if (!lk_open_mem(&g, &o, lv_mem, c->y, g.w, c->h))
            return FALSE;
    } else if (!lk_open_strip(&g, &o, bm, c->y, g.w, c->h)) {
        return FALSE;
    }
    keep = lv.out;
    lv.out = o;
    lv_ctl.on = TRUE;
    lv_ctl.x0 = c->x;
    lv_ctl.y0 = c->y;
    lv_ctl.x1 = c->x + c->w;
    lv_ctl.y1 = c->y + c->h;
    lk_clip(&lv.out, lv_ctl.x0, lv_ctl.y0, lv_ctl.x1, lv_ctl.y1);
    lv.eng->controls(c, what);
    lk_close(&lv.out);
    /* AGA, the colours to be chosen: drawn again with every button pressed
     * (its glow's yellows), unseen, so they are among them from the start
     * and a key's press needs no new copper list */
    if (mem && what == 0 && lv_ctl_pick && lk_open_mem(&g, &o, lv_mem + n, c->y, g.w, c->h)) {
        UWORD was = c->pressed;
        int b;
        lv.out = o;
        lk_clip(&lv.out, lv_ctl.x0, lv_ctl.y0, lv_ctl.x1, lv_ctl.y1);
        for (b = 0; b < c->n; b++)
            c->pressed |= (UWORD)LNB_BIT(c->btn[b]);
        lv.eng->controls(c, 0);
        c->pressed = was;
        lk_close(&lv.out);
        probe = TRUE;
    }
    lv_ctl.on = FALSE;
    lv_sp_up = FALSE;
    lv.out = keep;
    if (mem) {
        BOOL pick = what == 0 && lv_ctl_pick;
        int ncol = VW_BAR_PENS / 2, k;
        char eb[4];
        if (GetVar((CONST_STRPTR)"Ambiance/BarColours", (STRPTR)eb, sizeof(eb), GVF_GLOBAL_ONLY) > 0) {
            ncol = (eb[0] - '0') * (eb[1] >= '0' && eb[1] <= '9' ? 10 : 1) + (eb[1] >= '0' && eb[1] <= '9' ? eb[1] - '0' : 0);
            if (ncol < 8 || ncol > VW_BAR_PENS)
                ncol = VW_BAR_PENS / 2;
        }
        for (k = 0; k < VW_BAR_PENS; k++)
            lv_ctl_first[k] = -1;           /* (the pens past ncol: none) */
        lv_ctl_n = ncol - 1;                /* (and one more register: the gradient's second) */
        lv_ctl_h = c->h <= LV_CTL_ROWS ? c->h : 0;
        if (!lk_mem_pens(lv_mem, probe ? lv_mem + n : NULL, g.w, c->h, bm, (WORD)(GetBitMapAttr(bm, BMA_HEIGHT) - c->h), VW_BAR_PEN0,
                         ncol - 1, lv_ctl_rgb, pick, lv_ctl_first, lv_ctl_h ? lv_ctl_row : NULL, &lv_ctl_dom))
            return FALSE;                   /* (its rows the bitmap's last: a band's gap over them) */
        if (pick)
            lv_ctl_pick = FALSE;
    }
    return TRUE;
}

const char *lv_label(void)
{
    static char stem[32];
    char *dot;
    if (!lv.file[0])
        return _("Classic");
    if (lv.name[0])
        return lv.name;
    amb_strlcpy(stem, lv.file, sizeof(stem));
    if ((dot = strrchr(stem, '.')))
        *dot = 0;
    return stem;
}

/* ---- the models */
static UBYTE lv_section(int mod)
{
    switch (mod) {
    case MOD_GAMES: return LSEC_GAMES;
    case MOD_DEMOS: return LSEC_DEMOS;
    case MOD_MUSIC: return LSEC_MUSIC;
    case MOD_MODS: return LSEC_MODS;
    case MOD_PICTURES: return LSEC_PICTURES;
    case MOD_VIDEOS: return LSEC_VIDEOS;
    case MOD_RADIO: return LSEC_RADIO;
    case MOD_SETTINGS: return LSEC_SETTINGS;
    case MOD_NOWPLAYING: return LSEC_MUSIC;
    }
    return LSEC_OTHER;
}

static int lv_icon_of(int section)
{
    static const UBYTE icon[] = { ICON_GAMES, ICON_DEMOS, ICON_MUSIC, ICON_MODS, ICON_PICTURES,
                                  ICON_VIDEOS, ICON_RADIO, ICON_SETTINGS, ICON_ABOUT };
    return section >= 0 && section < (int)sizeof(icon) ? icon[section] : ICON_ABOUT;
}

static UBYTE lv_kind(const struct Item *it)
{
    switch (it->kind) {
    case IT_DIR: case IT_NET: case IT_PLEXDIR: return LI_FOLDER;
    case IT_RUN: return LI_GAME;
    case IT_ACTION:
        return it->mark >= MARK_CHECK_OFF && it->mark <= MARK_RADIO_ON ? LI_SETTING
             : it->mark == MARK_CYCLE ? LI_SETTING : LI_ACTION;
    case IT_PIC: case IT_PLEXPHOTO: return LI_PICTURE;
    case IT_MOD: return LI_MODULE;
    case IT_STATION: return LI_STATION;
    case IT_PLEXTRACK: case IT_SONG: return LI_TRACK;
    case IT_ANIM: return is_dvd_row(it) ? LI_DISC : LI_VIDEO;
    }
    return LI_OTHER;
}

static UBYTE lv_mark(const struct Item *it)
{
    switch (it->mark) {
    case MARK_ARROW: return LM_ARROW;
    case MARK_CHECK_OFF: return LM_CHECK_OFF;
    case MARK_CHECK_ON: return LM_CHECK_ON;
    case MARK_RADIO_OFF: return LM_RADIO_OFF;
    case MARK_RADIO_ON: return LM_RADIO_ON;
    case MARK_PLAY: return LM_PLAY;
    case MARK_PLAYING: return LM_PLAYING;
    case MARK_CYCLE: return LM_CYCLE;
    }
    return (it->kind == IT_DIR || it->kind == IT_NET || it->kind == IT_PLEXDIR) ? LM_ARROW : LM_NONE;
}

static const struct Page *lv_lp;    /* the page item() makes rows of */
static struct LItem lv_ring[32];
static char lv_ring_art[32][304];
static UWORD lv_ri;

/* A row's picture, as a key lv_picture() reads: a file, or a first byte
 * saying how the loader finds it (LV_ART_*) and then a path. The finding
 * (a folder's cover, a game's screenshot) is the loader's, off the UI. */
#define LV_ART_SONG   '\x01'       /* a music file's or folder's cover (SHOT_SONG) */
#define LV_ART_AGS    '\x02'       /* an AGS entry's screenshot (SHOT_AGS) */
#define LV_ART_FOLDER '\x03'       /* a Pictures folder's first picture (SHOT_FOLDER) */
#define LV_ART_PLEX   '\x04'       /* the Plex server's picture (its thumb): fetched, then made */

/* A Plex row's details (plex.c: its key, then by tabs: type, thumb, year,
 * by, count, studio, summary): field n (1 type ... 7 summary) into out */
static const char *lv_plex_field(const struct Item *it, int n, char *out, LONG max)
{
    const char *s = it && it->path ? it->path : "";
    LONG k = 0;
    out[0] = 0;
    while (n-- > 0) {
        s = strchr(s, '\t');
        if (!s)
            return out;
        s++;
    }
    while (*s && *s != '\t' && k < max - 1)
        out[k++] = *s++;
    out[k] = 0;
    return out;
}
/* An IFF ANIM (a frame that is not blank) or an MPEG file (an I picture
 * through it, mpgthumb.c) has a picture; a DVD has none */
static BOOL lv_anim_shot(const struct Item *it)
{
    return it->kind == IT_ANIM && !(it->sortkey && !strcmp(it->sortkey, "DVD"));
}

static BOOL lv_pic_failed(const char *file);

static const char *lv_art_of(const struct Page *p, const struct Item *it, char *buf, LONG max)
{
    char path[300];
    if (!it || (it->flags & ITEM_GONE) || max < 8)
        return NULL;
    buf[0] = 0;
    if (it->kind == IT_RUN) {
        buf[0] = LV_ART_AGS;
    } else if (p->songs && !p->plex && (it->kind == IT_SONG || it->kind == IT_DIR) &&
               !beets_page(it->path)) {
        buf[0] = LV_ART_SONG;
    } else if (p->pics && !p->plex && it->kind == IT_DIR) {
        buf[0] = LV_ART_FOLDER;
    } else if ((it->kind == IT_PIC || lv_anim_shot(it)) && !p->plex) {
        item_path(it, buf, max);            /* (an animation: its first frame) */
        return buf[0] ? buf : NULL;
    } else if (p->plex && (it->kind == IT_PLEXDIR || (it->kind == IT_ANIM && jf_key(it->path)))) {
        buf[0] = LV_ART_PLEX;               /* (a Jellyfin film's: its poster) */
        lv_plex_field(it, 2, buf + 1, max - 1);
        return buf[1] ? buf : NULL;
    } else {
        return NULL;
    }
    item_path(it, path, sizeof(path));
    if (!path[0])
        return NULL;
    amb_strlcpy(buf + 1, path, max - 1);
    return buf;
}

static void lv_fill_item_art(struct LItem *li, const struct Page *p, LONG i, char *art, LONG max)
{
    const struct Item *it;
    const char *tag;
    memset(li, 0, sizeof(*li));
    li->label = "";
    if (!p || i < 0 || i >= p->list.count)
        return;
    it = &p->list.items[i];
    li->label = it->label ? it->label : "";
    tag = row_tag(p, it);
    li->tag = tag && tag[0] ? tag : NULL;
    li->kind = lv_kind(it);
    li->mark = lv_mark(it);
    if (li->kind == LI_DISC && !li->tag)
        li->tag = "DVD";                    /* (the classic row's disc glyph says it) */
    if (it->flags & ITEM_DISABLED)
        li->flags |= LIF_DISABLED;
    if (it->flags & ITEM_HEAD)
        li->flags |= LIF_HEAD;
    if (it->mark == MARK_PLAYING)
        li->flags |= LIF_NOW;
    if (art) {
        li->art = lv_art_of(p, it, art, max);
        if (li->art && lv_pic_failed(li->art))
            li->art = NULL;                 /* (none to be had: drawn as one without) */
    }
    if (p->plex && it->kind == IT_PLEXDIR) {
        static char subs[32][104];
        static UWORD si;
        char *sub = subs[si++ & 31], type[16];
        sub[0] = 0;                 /* (a ring: what it held is another row's) */
        lv_plex_field(it, 1, type, sizeof(type));
        if (!strcmp(type, "album")) {
            li->kind = LI_ALBUM;
            lv_plex_field(it, 4, sub, 100);         /* by: the album's artist */
        } else if (!strcmp(type, "artist")) {
            li->kind = LI_ARTIST;
            lv_plex_field(it, 5, sub, 100);         /* its albums' count */
        }
        if (sub[0])
            li->sub = sub;
    } else if (beets_page(it->path) && !strncmp(it->path, "beets:album/", 12)) {
        li->kind = LI_ALBUM;
    }
}

static const struct LItem *lv_item(LONG i)
{
    UWORD k = lv_ri++ & 31;
    struct LItem *li = &lv_ring[k];
    lv_fill_item_art(li, lv_lp, i, lv_ring_art[k], sizeof(lv_ring_art[k]));
    li->letter = lv_lp && lv_lp->type == PT_LIST ? (UBYTE)ov_letter_in(lv_lp->list.items, lv_lp->list.count, i) : 0;
    if (!li->art && lv_lp && lv_lp->type == PT_LIST && i >= 0 && i < lv_lp->list.count &&
        np_row(&lv_lp->list.items[i]) && (np_art_key[0] || np_art_file[0]) && mp_loaded())
        li->art = np_art_key[0] ? np_art_key : np_art_file;     /* the Now playing row: its cover */
    return li;
}

/* The selected row's picture: a picture's own file, a game's screenshot
 * (looked for once a row). */
/* Entry idx's picture for a page's details (a picture file, a game's
 * screenshot): its file, or FALSE */
static BOOL lv_pic_file(struct Page *p, LONG idx, char *file, LONG max)
{
    struct Item *it;
    if (!p || p->type != PT_LIST || idx < 0 || idx >= p->list.count)
        return FALSE;
    it = &p->list.items[idx];
    if (it->flags & ITEM_GONE)
        return FALSE;
    if ((it->kind == IT_PIC || lv_anim_shot(it)) && !p->plex) {
        item_path(it, file, max);
        return TRUE;
    }
    if (it->kind == IT_RUN) {
        char base[300];
        return ags_screenshot_path(item_path(it, base, sizeof(base)), file, max);
    }
    if (p->plex && it->kind == IT_ANIM && jf_key(it->path)) {
        file[0] = LV_ART_PLEX;              /* a Jellyfin film: its poster, from the server */
        lv_plex_field(it, 2, file + 1, max - 1);
        return file[1] != 0;
    }
    return FALSE;
}

static char lv_pof_file[300];               /* lv_picture_of's (lv_picture knows it by it) */
static UWORD lv_pof_w, lv_pof_h;            /* the size the layout asked it at (lv_pics_ahead) */

static const char *lv_picture_of(struct Page *p)
{
    static const struct Page *for_p;
    static const struct Item *for_items;    /* (its list: another page in the same place
                                             * has another, though its row and count agree) */
    static LONG for_sel = -1, for_count = -1;
    static BOOL have;
    if (!p || p->type != PT_LIST || p->sel < 0 || p->sel >= p->list.count)
        return NULL;
    if (p == for_p && p->sel == for_sel && p->list.count == for_count && p->list.items == for_items)
        return have ? lv_pof_file : NULL;
    have = lv_pic_file(p, p->sel, lv_pof_file, sizeof(lv_pof_file));
    if (have || !(p->list.items[p->sel].flags & ITEM_GONE)) {
        for_p = p;                          /* (kept: an entry not held yet - its letter
                                             * being read - asked again until it is) */
        for_items = p->list.items;
        for_sel = p->sel;
        for_count = p->list.count;
    }
    return have ? lv_pof_file : NULL;
}

static const char *lv_lines[256];

/* A list whose rows are asked for over the network (Plex, Radio, Plex's
 * servers) and have not come; the spinner's step, one every SPIN_FRAME */
static BOOL lv_waiting(const struct Page *p)
{
    return p && p->type == PT_LIST && ((p->radio && p->net_seq && !p->list.count) || lv_busy);
}
static int lv_wait_step(void)
{
    return (int)((time_ms() / SPIN_FRAME) & 7);
}

static void lv_page(struct Page *p, struct LPage *m)
{
    int i, mod = shown_mod >= 0 ? shown_mod : menu[menu_sel];
    memset(m, 0, sizeof(*m));
    m->section = lv_section(mod);
    m->crumb[m->ncrumb++] = _(mod_names[mod]);
    for (i = 0; i < depth && m->ncrumb < LCRUMBS; i++)
        if (strcmp(pages[i].title, m->crumb[m->ncrumb - 1]))
            m->crumb[m->ncrumb++] = pages[i].title;     /* (a section's first page is
                                                         * titled as it: once, not twice) */
    m->hint = hint_text();
    m->message = msg[0] ? msg : NULL;
    if (!p)
        return;
    if (p->type == PT_TEXT || p->type == PT_DETAIL) {
        const struct ItemList *l = p->tb.lines ? p->tb.lines : &p->list;
        m->text = 1;
        if (p->tb.line_h > 0) {
            /* where it is now (it glides to tb.target): its first line, and how
             * much of that is scrolled past */
            m->list.top = p->tb.pos / p->tb.line_h;
            m->text_off = (UBYTE)((p->tb.pos % p->tb.line_h) * 256 / p->tb.line_h);
        }
        {
            LONG mx = p->tb.content_h - p->tb.h;    /* (textbox.c's max_pos) */
            m->text_frac = mx <= 0 ? 0 : p->tb.pos >= mx ? 65536UL
                         : (ULONG)(p->tb.pos * 1024L / mx) * 64UL;
        }
        for (i = 0; i < l->count && i < 256; i++)
            lv_lines[i] = l->items[i].label ? l->items[i].label : "";
        m->ndetails = (WORD)i;
        m->details = lv_lines;
        if (p->type == PT_TEXT) {           /* (Credits': AGS's logo, the thanks, Plex's mark) */
            m->picture = p->pic[0] ? p->pic : NULL;
            m->note = p->note;
            m->plexmark = p->plexmark;
            lv_mark_kind = p->plexmark;     /* (which mark host->plexmark gives) */
        }
        return;
    }
    lv_lp = p;
    m->list.count = p->list.count;
    m->list.sel = p->sel;
    m->list.top = p->top;
    m->list.item = lv_item;
    if (det.tj && det.tj->done && det.page == p && det.sel == p->sel) {
        for (i = 0; i < det.tj->nlines && i < LJ_FACTS; i++)
            lv_lines[i] = det.tj->lines[i];
        m->ndetails = (WORD)i;
        m->details = lv_lines;
    }
    if (!m->ndetails && p->plex && p->sel >= 0 && p->sel < p->list.count &&
        p->list.items[p->sel].kind == IT_ANIM && jf_key(p->list.items[p->sel].path)) {
        /* a Jellyfin film: its year, length and rating, then what it is
         * about (two sets, a row's each in turn: the column's key changes) */
        static char fl[2][3][320];
        static UWORD fs;
        const struct Item *it = &p->list.items[p->sel];
        char (*l)[320] = fl[fs++ & 1], v[24];
        int n = 0;
        amb_strlcpy(l[0], it->label, sizeof(l[0]));
        lv_plex_field(it, 3, l[1], sizeof(l[1]));
        lv_plex_field(it, 5, v, sizeof(v));
        if (v[0]) {
            if (l[1][0])
                amb_strlcat(l[1], " \xB7 ", sizeof(l[1]));
            amb_strlcat(l[1], v, sizeof(l[1]));
        }
        lv_lines[n++] = l[0];
        if (l[1][0])
            lv_lines[n++] = l[1];
        lv_plex_field(it, 6, l[2], sizeof(l[2]));
        if (l[2][0])
            lv_lines[n++] = l[2];
        m->ndetails = (WORD)n;
        m->details = lv_lines;
    }
    /* while it glides, or a letter is up, no pictures asked for: the
     * classic pane waits the same way */
    m->picture = hold_active() || ov.on ? NULL : lv_picture_of(p);
    m->aside = has_aside(p);                /* (its details' column from the start) */
    m->letter = ov.on && ov.page == p && ov.bl ? ov.letter : 0;
    m->frames = p->pics ? 1 : 0;
    if (lv_waiting(p)) {
        /* its rows asked for and not come: the layout's small spinner,
         * and "Nothing here" only once the answer says so */
        m->waiting = !lv_busy && !strcmp(p->base, "#gdm") ? _("Searching...") : _("Loading...");
        m->wait_phase = (UBYTE)lv_wait_step();
    }
}

/* ---- the music screens: which a page is, and their models */
/* The row a page was opened from (its parent's selected one), or NULL */
static const struct Item *lv_opened_from(const struct Page *p)
{
    int d;
    for (d = 1; d < depth; d++)
        if (&pages[d] == p) {
            const struct Page *q = &pages[d - 1];
            return q->sel >= 0 && q->sel < q->list.count ? &q->list.items[q->sel] : NULL;
        }
    return NULL;
}

/* How many of p's first rows (up to 40) are Plex rows of type t, and how
 * many are looked at */
static LONG lv_plex_count(const struct Page *p, const char *t, LONG *of)
{
    LONG i, n = 0;
    char type[16];
    *of = 0;
    for (i = 0; i < p->list.count && i < 40; i++) {
        const struct Item *it = &p->list.items[i];
        if (it->flags & ITEM_HEAD)
            continue;
        (*of)++;
        if (it->kind == IT_PLEXDIR && !strcmp(lv_plex_field(it, 1, type, sizeof(type)), t))
            n++;
    }
    return n;
}

/* Which screen a list page is drawn as: the album wall (a list of albums
 * with covers), an album (its tracks), an artist (its albums), or the
 * generic page */
static int lv_music_screen(const struct Page *p)
{
    const struct Item *from;
    LONG i, dirs = 0, songs = 0, of = 0;
    char type[16];
    if (p && p->type == PT_LIST && !p->list.count && p->plex && !p->photos &&
        (strstr(p->base, "type=9") || strstr(p->base, "type=8") || strstr(p->base, "/recentlyAdded")))
        return LS_ALBUMS;               /* (its albums or artists still coming: the wall waits,
                                         * so the screen does not change when they come) */
    if (!p || p->type != PT_LIST || !p->list.count || p->setp)
        return LS_PAGE;
    if (p->pics) {
        /* Pictures and Videos: a wall of their pictures (a folder's: its
         * first), when most rows have one to show */
        for (i = 0; i < p->list.count && i < 200; i++) {
            const struct Item *it = &p->list.items[i];
            if (it->kind == IT_PIC || it->kind == IT_ANIM || it->kind == IT_DIR)
                dirs++;
            else if (!np_row(it))
                songs++;
        }
        return !p->plex && dirs >= 2 && dirs >= songs * 2 ? LS_ALBUMS : LS_PAGE;
    }
    from = lv_opened_from(p);
    if (p->plex && !p->photos) {
        if (from && from->kind == IT_PLEXDIR) {
            lv_plex_field(from, 1, type, sizeof(type));
            if (!strcmp(type, "album"))
                return LS_ALBUM;
            if (!strcmp(type, "artist"))
                return LS_ARTIST;
        }
        if (lv_plex_count(p, "album", &of) * 2 > of && of)
            return LS_ALBUMS;
        if (lv_plex_count(p, "artist", &of) * 2 > of && of)
            return LS_ALBUMS;               /* (artists too: a wall of their pictures) */
        return LS_PAGE;
    }
    if (!p->songs)
        return LS_PAGE;
    if (from && from->path && beets_page(from->path)) {
        if (!strncmp(from->path, "beets:album/", 12))
            return LS_ALBUM;
        if (!strncmp(from->path, "beets:artist/", 13))
            return LS_ARTIST;
        if (!strcmp(from->path, "beets:albums") || !strcmp(from->path, "beets:recent") ||
            !strcmp(from->path, "beets:artists"))
            return LS_ALBUMS;
        return LS_PAGE;
    }
    if (!from)
        return LS_PAGE;                     /* Music's own first page */
    for (i = 0; i < p->list.count && i < 200; i++) {
        UBYTE k = p->list.items[i].kind;
        if (k == IT_DIR)
            dirs++;
        else if (k == IT_SONG)
            songs++;
    }
    if (songs >= 2 && songs >= dirs * 2)
        return LS_ALBUM;                    /* a folder of tracks: an album */
    if (dirs >= 3 && !songs)
        return LS_ALBUMS;                   /* a folder of folders: albums (or artists) */
    return LS_PAGE;
}

/* An album's or artist's page: its picture's key, and what is said of it
 * (a Plex row's details; a music folder's album.nfo or artist.nfo) */
static struct {
    const struct Page *p;           /* worked out for (and its row count) */
    LONG count;
    char title[100], artist[100], line[140], rating[8];
    char art[304];
    char *text;                     /* review or biography (nfo's, AllocVec'd), or Plex's */
    char plextext[310];
    const char *facts[12];
    char fbuf[6][100];
    WORD nfacts;
} lvm;

static void lvm_clear(void)
{
    if (lvm.text && lvm.text != lvm.plextext)
        FreeVec(lvm.text);
    memset(&lvm, 0, sizeof(lvm));
}

static void lvm_fact(const char *label, const char *value)
{
    if (!value || !value[0] || lvm.nfacts >= 6)
        return;
    amb_strlcpy(lvm.fbuf[lvm.nfacts], value, sizeof(lvm.fbuf[0]));
    lvm.facts[2 * lvm.nfacts] = label;
    lvm.facts[2 * lvm.nfacts + 1] = lvm.fbuf[lvm.nfacts];
    lvm.nfacts++;
}

static void lvm_work_out(const struct Page *p, BOOL artist)
{
    const struct Item *from = lv_opened_from(p);
    char path[300], v[100];
    LONG i;
    if (lvm.p == p && lvm.count == p->list.count)
        return;
    lvm_clear();
    lvm.p = p;
    lvm.count = p->list.count;
    amb_strlcpy(lvm.title, p->title, sizeof(lvm.title));
    if (p->plex && from && from->kind == IT_PLEXDIR) {
        char thumb[200];
        amb_strlcpy(lvm.title, from->label, sizeof(lvm.title));
        lv_plex_field(from, 2, thumb, sizeof(thumb));
        if (thumb[0]) {
            lvm.art[0] = LV_ART_PLEX;
            amb_strlcpy(lvm.art + 1, thumb, sizeof(lvm.art) - 1);
        }
        if (!artist)
            lv_plex_field(from, 4, lvm.artist, sizeof(lvm.artist));
        lv_plex_field(from, 3, v, sizeof(v));       /* year */
        amb_strlcpy(lvm.line, v, sizeof(lvm.line));
        lv_plex_field(from, 6, v, sizeof(v));       /* studio: the label */
        if (v[0]) {
            if (lvm.line[0])
                amb_strlcat(lvm.line, " \xB7 ", sizeof(lvm.line));
            amb_strlcat(lvm.line, v, sizeof(lvm.line));
        }
        lv_plex_field(from, 5, v, sizeof(v));       /* tracks or albums */
        lvm_fact(artist ? _("Albums") : _("Tracks"), v);
        lv_plex_field(from, 7, lvm.plextext, sizeof(lvm.plextext));
        if (lvm.plextext[0])
            lvm.text = lvm.plextext;
        return;
    }
    /* a music folder, or a beets album: its folder is its first track's */
    path[0] = 0;
    for (i = 0; i < p->list.count; i++)
        if (p->list.items[i].kind == IT_SONG || p->list.items[i].kind == IT_DIR) {
            item_path(&p->list.items[i], path, sizeof(path));
            break;
        }
    if (path[0] && !beets_page(path)) {
        char dir[300];
        struct Nfo n;
        amb_strlcpy(dir, path, sizeof(dir));
        if (p->list.items[i].kind == IT_SONG)
            *PathPart((STRPTR)dir) = 0;             /* the album's folder (an album folder's
                                                     * own: the artist's is the one above) */
        lvm.art[0] = LV_ART_SONG;
        amb_strlcpy(lvm.art + 1, path, sizeof(lvm.art) - 1);
        memset(&n, 0, sizeof(n));
        if (artist ? nfo_artist_of(dir, p->list.items[i].kind == IT_DIR, &n) : nfo_album_of(dir, &n)) {
            if (n.title[0])
                amb_strlcpy(lvm.title, n.title, sizeof(lvm.title));
            amb_strlcpy(lvm.artist, n.artist, sizeof(lvm.artist));
            amb_strlcpy(lvm.line, artist ? n.type : n.year, sizeof(lvm.line));
            if (!artist && n.label[0]) {
                if (lvm.line[0])
                    amb_strlcat(lvm.line, " \xB7 ", sizeof(lvm.line));
                amb_strlcat(lvm.line, n.label, sizeof(lvm.line));
            }
            amb_strlcpy(lvm.rating, n.rating, sizeof(lvm.rating));
            lvm_fact(_("Genre"), n.genres);
            lvm_fact(_("Style"), n.styles);
            lvm_fact(_("Mood"), n.moods);
            if (artist) {
                lvm_fact(n.born ? _("Born") : _("Formed"), n.formed);
                lvm_fact(n.born ? _("Died") : _("Disbanded"), n.ended);
            }
            lvm.text = n.text;                      /* (ours now) */
            n.text = NULL;
        }
        nfo_free(&n);
        if (!artist && !lvm.artist[0] && from && depth >= 3) {
            const struct Page *up = &pages[depth - 3];
            if (up->sel >= 0 && up->sel < up->list.count)
                amb_strlcpy(lvm.artist, up->list.items[up->sel].label, sizeof(lvm.artist));
        }
    }
}

static BOOL lv_home_list(void);

/* Home's tiles are the section's first level's rows */
/* Home's tiles' pictures, where they have none of their own: the one
 * chosen last in the page each opens (Albums: the album; Artists: the
 * artist), remembered across runs (ENVARC:Ambiance/TileArt) */
#define TILEART_VAR "Ambiance/TileArt"
#define LV_TA 24
static struct { UBYTE sec; char label[40]; char art[160]; } lv_ta[LV_TA];
static int lv_nta, lv_ta_next;
static BOOL lv_ta_loaded, lv_ta_dirty;
static void lv_ta_load(void)
{
    static char buf[LV_TA * 210];
    char *p, *e;
    LONG n;
    lv_ta_loaded = TRUE;
    if ((n = GetVar((CONST_STRPTR)TILEART_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY)) <= 0)
        return;
    for (p = buf; *p && lv_nta < LV_TA; p = e) {
        char *t1, *t2;
        for (e = p; *e && *e != '\n'; e++)
            ;
        if (*e)
            *e++ = 0;
        if (!(t1 = strchr(p, '\t')) || !(t2 = strchr(t1 + 1, '\t')))
            continue;
        *t1++ = 0;
        *t2++ = 0;
        lv_ta[lv_nta].sec = (UBYTE)(p[0] >= '0' && p[0] <= '9' ? (p[1] >= '0' && p[1] <= '9' ? (p[0] - '0') * 10 + p[1] - '0' : p[0] - '0') : 0);
        amb_strlcpy(lv_ta[lv_nta].label, t1, sizeof(lv_ta[0].label));
        amb_strlcpy(lv_ta[lv_nta].art, t2, sizeof(lv_ta[0].art));
        lv_nta++;
    }
    lv_ta_next = lv_nta % LV_TA;
}
static void lv_ta_save(void)
{
    static char buf[LV_TA * 210];
    int i;
    if (!lv_ta_dirty)
        return;
    lv_ta_dirty = FALSE;
    buf[0] = 0;
    for (i = 0; i < lv_nta; i++) {
        char line[210];
        sprintf(line, "%lu\t%s\t%s\n", (unsigned long)lv_ta[i].sec, lv_ta[i].label, lv_ta[i].art);
        amb_strlcat(buf, line, sizeof(buf));
    }
    save_setting(TILEART_VAR, buf);
}
static const char *lv_ta_find(int sec, const char *label)
{
    int i;
    if (!lv_ta_loaded)
        lv_ta_load();
    for (i = 0; i < lv_nta; i++)
        if (lv_ta[i].sec == sec && !strcmp(lv_ta[i].label, label))
            return lv_ta[i].art;
    return NULL;
}
static void lv_ta_note(int sec, const char *label, const char *art)
{
    int i;
    if (!label || !*label || !art || !*art || strlen(art) >= sizeof(lv_ta[0].art) || strchr(art, '\n') ||
        strchr(art, '\t'))
        return;
    if (!lv_ta_loaded)
        lv_ta_load();
    for (i = 0; i < lv_nta; i++)
        if (lv_ta[i].sec == sec && !strcmp(lv_ta[i].label, label))
            break;
    if (i == lv_nta) {
        if (lv_nta < LV_TA)
            i = lv_nta++;
        else
            i = lv_ta_next++ % LV_TA;
    } else if (!strcmp(lv_ta[i].art, art)) {
        return;
    }
    lv_ta[i].sec = (UBYTE)sec;
    amb_strlcpy(lv_ta[i].label, label, sizeof(lv_ta[0].label));
    amb_strlcpy(lv_ta[i].art, art, sizeof(lv_ta[0].art));
    lv_ta_dirty = TRUE;
}
/* The page opened from a tile, its chosen one's picture noted for the tile */
static void lv_ta_watch(void)
{
    static const struct Page *was_p;
    static LONG was_sel = -1;
    struct Page *p1;
    const struct Item *from;
    struct LItem li;
    char art[200];
    if (depth < 2 || !nmenu || pages[0].type != PT_LIST || pages[0].sel < 0 || pages[0].sel >= pages[0].list.count)
        return;
    p1 = &pages[1];
    if (p1->type != PT_LIST || p1->sel < 0 || p1->sel >= p1->list.count)
        return;
    if (p1 == was_p && p1->sel == was_sel)
        return;
    was_p = p1;
    was_sel = p1->sel;
    from = &pages[0].list.items[pages[0].sel];
    lv_fill_item_art(&li, p1, p1->sel, art, sizeof(art));
    if (li.art && from->label && !np_row(from))
        lv_ta_note(menu[menu_sel], from->label, li.art);
}

static const struct LItem *lv_home_item(LONG i)
{
    const struct LItem *li;
    lv_lp = &pages[0];
    li = lv_item(i);
    if (!li->art && nmenu && li->label && *li->label) {
        const char *a = lv_ta_find(menu[menu_sel], li->label);
        if (a && !lv_pic_failed(a))
            ((struct LItem *)li)->art = a;      /* (the ring's own: ours to set) */
    }
    return li;
}

/* ---- Home's row for Now playing (its first level the player, not a list):
 * what is loaded - its cover, its title, who by or the station, or that it
 * is paused - then what plays next. A tile chosen: the player's page. */
#define NPH_NEXT 7
static LONG np_home_count(void)
{
    LONG n;
    if (!mp_loaded())
        return 0;
    n = np.list.count - (np.idx + 1);
    if (n < 0 || np.idx < 0)
        n = 0;
    return 1 + (n > NPH_NEXT ? NPH_NEXT : n);
}

static const struct LItem *np_home_item(LONG i)
{
    static struct LItem li;
    const char *t, *by;
    UBYTE kind = mp_is_track() ? LI_TRACK : mp_is_stream() ? LI_STATION : LI_MODULE;
    memset(&li, 0, sizeof(li));
    li.kind = kind;
    if (i > 0) {                            /* what plays next */
        LONG k = np.idx + i;
        li.label = k >= 0 && k < np.list.count ? np.list.items[k].label : "";
        return &li;
    }
    t = mp_title();
    by = mp_is_track() ? mp_track_by() : mp_is_stream() ? NULL : mp_author();
    if (mp_is_stream() && mp_stream_name() && *mp_stream_name()) {
        if (!t || !*t)
            t = mp_stream_name();
        else if (!by || !*by)
            by = mp_stream_name();          /* (the song as its title, the station under it) */
    }
    li.label = t && *t ? t : _(mod_names[MOD_NOWPLAYING]);
    li.sub = mp_paused() ? _("paused") : !mp_playing() ? _("stopped") : by && *by ? by : NULL;
    li.art = np_art_key[0] && mp_is_track() ? np_art_key : np_art_file[0] ? np_art_file : NULL;
    li.mark = mp_playing() && !mp_paused() ? LM_PLAYING : LM_NONE;
    li.flags = LIF_NOW;
    return &li;
}

/* ---- Home's launchpad. A section whose first level is a long flat list
 * of files (Videos: the films themselves) shows on Home not every file -
 * a row of hundreds, a tile a key, no letters - but its folders and other
 * rows (now playing, a disc), the few files opened last, and "All ..." :
 * the section's whole list as a page, its letters and details as any
 * list's. Esc or Back there: the row again. */
#define LP_LEAVES 6                 /* more files than this at the first level: a launchpad */
#define LP_RECENT 4
#define LP_ALL (-1)
#define LP_MAX 40
#define RECENT_VAR "Ambiance/HomeRecent"
static LONG lp_map[LP_MAX];         /* each tile: its row of the first level, or LP_ALL */
static int lp_n = -1;               /* tiles (-1: no launchpad, the rows themselves) */
static int lp_sel, lp_sec = -1;     /* the tile chosen; the section it was made for */
static LONG lp_files;               /* the files of the first level */
static BOOL lp_all;                 /* the section's whole list shown (its "All" tile opened) */
static const void *lp_items;        /* (made for this list: items, count) */
static LONG lp_count = -1;
static struct { UBYTE sec; char label[48]; } lp_rec[24];
static int lp_nrec;
static BOOL lp_rec_loaded;

static BOOL lp_file(const struct Item *it)
{
    if (np_row(it) || it->mark == MARK_DISC)
        return FALSE;                       /* (now playing, a disc in a drive: their own tiles) */
    switch (it->kind) {
    case IT_PIC: case IT_MOD: case IT_SONG: case IT_ANIM: case IT_RUN: case IT_PLEXTRACK:
    case IT_PLEXPHOTO: case IT_STATION:
        return TRUE;
    }
    return FALSE;
}

static void lp_rec_load(void)
{
    static char buf[24 * 56];
    char *q, *e;
    lp_rec_loaded = TRUE;
    if (GetVar((CONST_STRPTR)RECENT_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) <= 0)
        return;
    for (q = buf; *q && lp_nrec < 24; q = e) {
        char *t;
        for (e = q; *e && *e != '\n'; e++)
            ;
        if (*e)
            *e++ = 0;
        if (!(t = strchr(q, '\t')))
            continue;
        *t++ = 0;
        lp_rec[lp_nrec].sec = (UBYTE)(q[0] >= '0' && q[0] <= '9' ? (q[1] >= '0' && q[1] <= '9' ? (q[0] - '0') * 10 + q[1] - '0' : q[0] - '0') : 0);
        amb_strlcpy(lp_rec[lp_nrec].label, t, sizeof(lp_rec[0].label));
        lp_nrec++;
    }
}

static void lp_rec_save(void)
{
    static char buf[24 * 56];
    int i;
    buf[0] = 0;
    for (i = 0; i < lp_nrec; i++) {
        char line[60];
        sprintf(line, "%lu\t%s\n", (unsigned long)lp_rec[i].sec, lp_rec[i].label);
        amb_strlcat(buf, line, sizeof(buf));
    }
    save_setting(RECENT_VAR, buf);
}

/* A file of a section's first level opened: first of its recent ones */
static void lp_note_open(const struct Page *p)
{
    const struct Item *it;
    int sec, i, j, kept = 0;
    if (!nmenu || depth != 1 || p != &pages[0] || p->type != PT_LIST || p->sel < 0 || p->sel >= p->list.count)
        return;
    it = &p->list.items[p->sel];
    if (!lp_file(it) || !it->label || !*it->label || strchr(it->label, '\n') || strchr(it->label, '\t'))
        return;
    if (!lp_rec_loaded)
        lp_rec_load();
    sec = menu[menu_sel];
    for (i = j = 0; i < lp_nrec; i++)       /* (it out, and the section's beyond its few) */
        if (!(lp_rec[i].sec == sec &&
              (!strcmp(lp_rec[i].label, it->label) || ++kept >= LP_RECENT)))
            lp_rec[j++] = lp_rec[i];
    lp_nrec = j < 23 ? j : 23;
    memmove(&lp_rec[1], &lp_rec[0], (size_t)lp_nrec * sizeof(lp_rec[0]));
    lp_rec[0].sec = (UBYTE)sec;
    amb_strlcpy(lp_rec[0].label, it->label, sizeof(lp_rec[0].label));
    lp_nrec++;
    lp_rec_save();
    lp_items = NULL;                        /* (the row made again) */
}

/* The launchpad's tiles for the section shown (lp_n -1: none) */
static void lp_build(void)
{
    const struct Page *p = &pages[0];
    LONG i, k;
    int sec = nmenu ? menu[menu_sel] : -1;
    if (sec != lp_sec) {
        lp_sec = sec;
        lp_all = FALSE;
        lp_sel = 0;
        lp_items = NULL;
    }
    if (depth < 1 || p->type != PT_LIST || !p->list.count) {
        lp_n = -1;
        return;
    }
    if (lp_items == (const void *)p->list.items && lp_count == p->list.count)
        return;
    lp_items = (const void *)p->list.items;
    lp_count = p->list.count;
    for (i = 0, lp_files = 0; i < p->list.count; i++)
        if (lp_file(&p->list.items[i]))
            lp_files++;
    if (lp_files <= LP_LEAVES) {
        lp_n = -1;
        return;
    }
    lp_n = 0;
    for (i = 0; i < p->list.count && lp_n < LP_MAX - LP_RECENT - 1; i++)
        if (!lp_file(&p->list.items[i]))
            lp_map[lp_n++] = i;             /* (its folders, now playing, a disc) */
    if (!lp_rec_loaded)
        lp_rec_load();
    for (k = 0; k < lp_nrec; k++)           /* (its files opened last, still there) */
        if (lp_rec[k].sec == sec)
            for (i = 0; i < p->list.count; i++)
                if (lp_file(&p->list.items[i]) && p->list.items[i].label &&
                    !strcmp(p->list.items[i].label, lp_rec[k].label)) {
                    lp_map[lp_n++] = i;
                    break;
                }
    lp_map[lp_n++] = LP_ALL;
    if (lp_sel >= lp_n)
        lp_sel = lp_n - 1;
    for (k = 0; k < lp_n; k++)              /* (the row chosen before, if it has a tile) */
        if (lp_map[k] == p->sel) {
            lp_sel = (int)k;
            break;
        }
}

/* Home's row: the launchpad's (its tiles, chosen, and not its whole list) */
static BOOL lp_on(void)
{
    lp_build();
    return lp_n > 0 && !lp_all;
}

static const struct LItem *lp_item(LONG i)
{
    static struct LItem all;
    static char lab[48], sub[24];
    if (i >= 0 && i < lp_n && lp_map[i] >= 0) {
        const struct LItem *it = lv_home_item(lp_map[i]);
        static struct LItem rec;
        static char rsub[64];
        if (!it || !lp_file(&pages[0].list.items[lp_map[i]]))
            return it;
        rec = *it;                          /* (one opened lately: said so under it) */
        if (it->sub && it->sub[0])
            sprintf(rsub, "%.20s \267 %.36s", _("recent"), it->sub);
        else
            sprintf(rsub, "%.20s", _("recent"));
        rec.sub = rsub;
        return &rec;
    }
    memset(&all, 0, sizeof(all));
    sprintf(lab, "%.20s %.24s", _("All"), nmenu ? _(mod_names[menu[menu_sel]]) : "");
    sprintf(sub, "%ld", (long)lp_files);
    all.label = lab;
    all.sub = sub;
    all.kind = LI_FOLDER;
    return &all;
}

/* A tile chosen (LAC_SELECT), opened or played: the launchpad's own */
static BOOL lp_act(int what, LONG arg)
{
    struct Page *p = &pages[0];
    if (!lv_home_list() || !lp_on() || arg < 0 || arg >= lp_n)
        return FALSE;
    lp_sel = (int)arg;
    if (lp_map[arg] >= 0 && lp_map[arg] != p->sel)
        act_move(lp_map[arg] - p->sel);
    lv_note_what(LC_SELECT);
    if (what != LAC_SELECT)
        act_open(what == LAC_PLAY);
    return TRUE;
}

/* The section's whole list shown as a page (its "All" tile opened) */
static BOOL lp_on_list(void)
{
    lp_build();
    return lp_n > 0 && lp_all;
}

static BOOL lp_open_all(void)
{
    if (!lv_on() || !lv_home_list() || !lp_on() || lp_sel < 0 || lp_sel >= lp_n)
        return FALSE;
    if (lp_map[lp_sel] != LP_ALL) {
        if (lp_map[lp_sel] != pages[0].sel)
            act_move(lp_map[lp_sel] - pages[0].sel);    /* (the tile's own row, opened) */
        return FALSE;
    }
    lp_all = TRUE;
    lv_note();
    return TRUE;
}

static BOOL lp_back(void)
{
    if (!lp_all || depth != 1 || focus != FOCUS_CONTENT)
        return FALSE;
    lp_all = FALSE;
    lv_note();
    return TRUE;
}

static const void *lv_model(int screen)
{
    static struct LPage page;
    static struct LHome home;
    static struct LNow now;
    static UBYTE secs[MOD_COUNT];
    static const char *names[MOD_COUNT];
    static struct LRow row;
    static const char *next[5];
    static char year[1];
    int i;
    switch (screen) {
    case LS_ALBUM: case LS_ARTIST: {
        static struct LAlbum al;
        static struct LArtist ar;
        struct Page *p = cur();
        if (!p)
            break;
        lvm_work_out(p, screen == LS_ARTIST);
        if (screen == LS_ALBUM) {
            memset(&al, 0, sizeof(al));
            lv_page(p, &al.page);
            al.title = lvm.title;
            al.artist = lvm.artist[0] ? lvm.artist : NULL;
            al.line = lvm.line[0] ? lvm.line : NULL;
            al.review = lvm.text;
            al.rating = lvm.rating[0] ? lvm.rating : NULL;
            al.art = lvm.art[0] && !hold_active() ? lvm.art : NULL;
            al.facts.n = lvm.nfacts;
            al.facts.facts = lvm.facts;
            return &al;
        }
        memset(&ar, 0, sizeof(ar));
        lv_page(p, &ar.page);
        ar.name = lvm.title;
        ar.line = lvm.line[0] ? lvm.line : NULL;
        ar.bio = lvm.text;
        ar.art = lvm.art[0] && !hold_active() ? lvm.art : NULL;
        ar.facts.n = lvm.nfacts;
        ar.facts.facts = lvm.facts;
        return &ar;
    }
    case LS_HOME:
        memset(&home, 0, sizeof(home));
        for (i = 0; i < nmenu && i < MOD_COUNT; i++) {
            secs[i] = lv_section(menu[i]);
            names[i] = _(mod_names[menu[i]]);
        }
        home.n = (WORD)i;
        home.sel = (WORD)menu_sel;
        home.sections = secs;
        home.names = names;
        home.tile_sel = -1;
        if (!pane_pending && depth >= 1 && pages[0].type == PT_LIST) {
            row.title = _(mod_names[menu[menu_sel]]);
            row.n = pages[0].list.count;
            row.item = lv_home_item;            /* (the section's own list: its tiles) */
            home.nrows = 1;
            home.rows = &row;
            if (lp_on()) {                      /* (a long flat list: the launchpad's) */
                row.n = lp_n;
                row.item = lp_item;
            }
            if (lv_home_list())
                home.tile_sel = lp_on() ? lp_sel : pages[0].sel;
            home.tile_shown = !mouse_mode;  /* (come in with the mouse: nothing chosen
                                             * shows, the hover does) */
        } else if (!pane_pending && depth >= 1 && pages[0].type == PT_PLAYER &&
                   menu[menu_sel] == MOD_NOWPLAYING && mp_loaded()) {
            row.title = _(mod_names[MOD_NOWPLAYING]);
            row.n = np_home_count();        /* (the player's: what is loaded, what is next) */
            row.item = np_home_item;
            home.nrows = 1;
            home.rows = &row;
            home.tile_shown = !mouse_mode;
        }
        return &home;
    case LS_NOW: {
        struct MpStatus st;
        memset(&now, 0, sizeof(now));
        if (!mp_loaded())
            return &now;
        mp_status(&st);
        now.kind = mp_is_track() ? LN_TRACK : mp_is_stream() ? LN_STREAM : LN_MODULE;
        now.paused = st.paused ? 1 : 0;
        now.title = mp_title();
        now.artist = mp_is_track() ? mp_track_by() : mp_author();
        now.year = year;
        now.format = mp_tag();
        now.source = now.kind == LN_STREAM ? mp_stream_name() : np.folder;
        /* a Plex track's cover as the server's picture (made at the size the
         * layout shows it); else the file (the classic's, 327 square: blurred,
         * made larger) */
        now.art = np_art_key[0] && mp_is_track() ? np_art_key : np_art_file[0] ? np_art_file : NULL;
        now.pos_ms = st.elapsed_ms;
        now.len_ms = st.total_ms;
        for (i = 0; i < 5 && np.idx + 1 + i < np.list.count; i++)
            next[i] = np.list.items[np.idx + 1 + i].label;
        now.nnext = (WORD)i;
        now.next = next;
        {
            /* the spectrum, 32 bands (as the classic's bars); the four
             * meters where it cannot be had */
            static UBYTE lev[32];
            if (!st.paused && cur() && cur()->type == PT_PLAYER && vis_spectrum(lev, 32)) {
                now.nlevels = 32;
            } else {
                for (i = 0; i < 4; i++)
                    lev[i] = (UBYTE)(gfx_eq_lev[i] > 11 ? 255 : gfx_eq_lev[i] * 23);
                now.nlevels = 4;
            }
            now.levels = lev;
        }
        /* the buttons, as the classic page's (np_button) */
        now.stopped = !mp_playing() && !mp_paused() ? 1 : 0;
        now.volume = (UBYTE)mp_volume();
        now.btn = np_focus_shown() ? (WORD)np.btn : -1;
        now.off = 0;
        if (!mp_can_step(-1))
            now.off |= LNB_BIT(LNB_BACK);
        if (!mp_can_step(1))
            now.off |= LNB_BIT(LNB_FWD);
        if (mp_is_stream() && !mp_is_track())
            now.off |= LNB_BIT(LNB_LOOP);
        if (mp_volume() == 0)
            now.off |= LNB_BIT(LNB_VDOWN);
        if (mp_volume() >= 64)
            now.off |= LNB_BIT(LNB_VUP);
        now.on = (UWORD)((play_loop ? LNB_BIT(LNB_LOOP) : 0) | (np_muted() ? LNB_BIT(LNB_MUTE) : 0));
        return &now;
    }
    }
    lv_page(cur(), &page);
    return &page;
}

/* ---- what the layout asks */
/* The big letter, as the layout moves through a long list by itself (its
 * wall's keys held, a scroll bar dragged or paged): up, or the letter of
 * where it is now, lingering a second after the last move (as the held
 * Up and Down keys' glide shows it) */
static ULONG lv_sel_ms;
static void lv_letter_touch(struct Page *p)
{
    if (!p || p->type != PT_LIST || !p->list.count || !ov_eligible(p))
        return;
    if (!(ov.on && ov.page == p))
        ov_show(p);
    else
        ov_relabel(ov_letter_in(p->list.items, p->list.count, p->sel));
    ov.linger = 25;
}

static void lv_act(int what, LONG arg)
{
    struct Page *p = cur();
    switch (what) {
    case LAC_SELECT: case LAC_OPEN: case LAC_PLAY:
        if (focus == FOCUS_CONTENT && lp_act(what, arg))
            break;                          /* (Home's launchpad: its own tiles) */
        if (focus == FOCUS_MENU) {
            if (arg >= 0 && arg < nmenu && arg != menu_sel)
                menu_select(arg);
        } else if (p && p->type == PT_LIST && arg >= 0 && arg < p->list.count && arg != p->sel) {
            ULONG now = time_ms();
            BOOL quick = what == LAC_SELECT && now - lv_sel_ms < 400;  /* (a key held: moves in a row) */
            lv_sel_ms = what == LAC_SELECT ? now : 0;
            act_move(arg - p->sel);
            if (quick)
                lv_letter_touch(p);
        }
        if (what != LAC_SELECT)
            act_open(what == LAC_PLAY);
        break;
    case LAC_BACK: act_back(); break;
    case LAC_SECTION:
        if (focus == FOCUS_CONTENT && lv_home_list())
            set_focus(FOCUS_MENU);          /* (a section's tiles in use: the menu clicked) */
        if (focus == FOCUS_MENU && arg >= 0 && arg < nmenu && arg != menu_sel)
            menu_select(arg);
        break;
    case LAC_ENTER:
        if (focus == FOCUS_MENU)
            act_open(FALSE);
        break;
    case LAC_PAUSE: if (mp_loaded()) np_press(NB_PLAY); break;
    case LAC_NEXT: if (mp_loaded()) np_press(NB_NEXT); break;
    case LAC_PREV: if (mp_loaded()) np_press(NB_PREV); break;
    case LAC_NOWPLAYING: if (mp_loaded()) np_open_page(); break;
    case LAC_SCROLLTO:                      /* a scroll bar dragged, or clicked */
        if (p && p->type == PT_LIST && p->list.count > 1 && arg >= 0) {
            LONG to = (LONG)((ULONG)arg * (ULONG)(p->list.count - 1) / 65536UL);
            if (to >= p->list.count)
                to = p->list.count - 1;
            if (focus == FOCUS_MENU) {
                act_open(FALSE);            /* (Home's tiles' bar: into the section first) */
                p = cur();
                if (focus == FOCUS_MENU || !p || p->type != PT_LIST || to >= p->list.count)
                    break;
            }
            if (to != p->sel) {
                act_move(to - p->sel);
                lv_letter_touch(p);
            }
        } else if (p && (p->type == PT_TEXT || p->type == PT_DETAIL) && p->tb.open && arg >= 0) {
            tb_scroll_frac(&p->tb, (ULONG)arg);
        }
        break;
    case LAC_TEXTFIT:                       /* the layout's measure of the text drawn */
        if (p && (p->type == PT_TEXT || p->type == PT_DETAIL) && p->tb.open &&
            (p != lv_tfit_p || (ULONG)arg != lv_tfit)) {
            LONG was = p->tb.pos;
            lv_tfit_p = p;
            lv_tfit = (ULONG)arg;
            if (lv_tfit_apply(p) && was)
                lv_note();                  /* (the same place is a new fraction: drawn again) */
        }
        break;
    case LAC_SEEK:                          /* Now playing's bar, clicked */
        if (mp_loaded() && arg >= 0 && arg <= 65536)
            np_seek_part((ULONG)arg, 65536UL);
        break;
    case LAC_BUTTON:                        /* Now playing's, clicked */
        if (arg >= 0 && arg < NB_COUNT)
            np_press((int)arg);
        break;
    case LAC_HOME:                          /* (the logo clicked) */
        if (focus == FOCUS_MENU)
            break;
        pane_settle();
        while (depth > 1)
            page_pop();                     /* closed at once, none drawn on the way */
        np_shortcut_sync();
        set_focus(FOCUS_MENU);
        break;
    }
}

/* ---- pictures, made for the layout at the size it asks: by the loader's
 * picture process (decoded off the UI's back, as the classic pane's), the
 * layout drawn again when one is ready */
#define LV_PICS 32                  /* (Home's tiles and a wall's covers on screen at once) */
static struct {
    char file[304];                 /* the key asked for (lv_art_of) */
    UWORD w, h;                     /* asked for */
    BOOL failed;
    ULONG used;
    struct LJob *job;               /* out at the loader, or back and not yet taken */
    struct LImage im;
    ULONG *px;
} lv_pic[LV_PICS];
static BOOL lv_pic_made;            /* (kept for the frame's count) */

/* for lvhost.c: the big letter up (the classic's mask), and whether a
 * picture is one the host made for the layout (its own pens) */
static const struct BigLetter *lvh_letter_up(void)
{
    return ov.on ? ov.bl : NULL;
}
static BOOL lvh_own(const struct LImage *im)
{
    return im >= &lv_pic[0].im && im <= &lv_pic[LV_PICS - 1].im;
}
static ULONG lv_pic_clock;


/* slot i emptied: its job given up if still out (it holds no pens) */
static void lv_pic_drop(int i)
{
    struct LJob *j = lv_pic[i].job;
    if (j) {
        if (j->busy) {
            ldr_orphan(j);
        } else {
            pic_dispose(j->obj);
            thumb_free(j->thumb);
            FreeVec(j);
        }
    }
    if (lv_pic[i].px)
        FreeVec(lv_pic[i].px);
    memset(&lv_pic[i], 0, sizeof(lv_pic[i]));
}

/* slot i's job back: its thumbnail as the layout's pixels */
static void lv_pic_take(int i)
{
    struct LJob *j = lv_pic[i].job;
    struct Thumb *t = j ? j->thumb : NULL;
    lv_pic[i].job = NULL;
    lv_pic[i].failed = TRUE;
    if (t && (lv_pic[i].px = AllocVec((ULONG)t->w * t->h * 4, MEMF_ANY))) {
        ULONG *d = lv_pic[i].px;
        UWORD x, y;
        if (t->argb) {
            CopyMem(t->argb, d, (ULONG)t->w * t->h * 4);    /* (true colour, THUMB_DEEP) */
        } else {
            for (y = 0; y < t->h; y++) {
                const UBYTE *src = t->px + (ULONG)y * t->mod;
                for (x = 0; x < t->w; x++)
                    *d++ = t->pal[src[x]] & 0xFFFFFF;
            }
        }
        lv_pic[i].im.w = t->w;
        lv_pic[i].im.h = t->h;
        lv_pic[i].im.mod = t->w;
        lv_pic[i].im.px = lv_pic[i].px;
        lv_pic[i].failed = FALSE;
    } else if (!t) {
        amb_log("layout: a picture could not be read", lv_pic[i].file);
    }
    if (j) {
        pic_dispose(j->obj);
        thumb_free(j->thumb);
        FreeVec(j);
    }
}

/* Each frame: a picture back from the loader: the layout told
 * (changed(LC_IMAGE): it draws what was waiting for it, not all) */
static BOOL lv_img;
static void lv_pics_poll(void)
{
    int i;
    for (i = 0; i < LV_PICS; i++)
        if (lv_pic[i].job && lv_pic[i].job->done) {
            struct LJob *j = lv_pic[i].job;
            if (j->ran && j->thumb) {
                /* (Debug: how each of the layout's pictures was read: Ambiance's
                 * own reader or a datatype, and how big it was and is) */
                char m[420];
                const char *f = strrchr(j->path, '/');
                sprintf(m, "%.120s: %ldx%ld %ld-bit%s -> %ldx%ld, %s%s%s, %ld ms", f ? f + 1 : j->path,
                        (long)j->info.w, (long)j->info.h, (long)j->info.depth,
                        (j->info.mode & HAM_KEY) ? " HAM" : "", (long)j->thumb->w, (long)j->thumb->h,
                        j->own ? "own reader" : "datatype ", j->own ? "" : j->info.fmt,
                        j->own || !j->info.cls[0] ? "" : " (class)", (long)j->ms_decode);
                amb_log("layout picture", m);
            }
            lv_pic_take(i);
            lv_img = TRUE;
        }
}

static void lv_pics_free(void)
{
    int i;
    lvh_free();                     /* (the logo's, Plex's mark's) */
    for (i = 0; i < LV_PICS; i++)
        lv_pic_drop(i);
}

static const struct LImage *lv_picture(const char *file, UWORD w, UWORD h)
{
    static char got[120];
    int i, k = 0;
    struct LJob *j;
    if (!file || !file[0] || !w || !h)
        return NULL;
    if (file == lv_pof_file) {
        lv_pof_w = w;                       /* (the details' size: its neighbours read ahead so) */
        lv_pof_h = h;
    }
    for (i = 0; i < LV_PICS; i++) {
        if (lv_pic[i].w == w && lv_pic[i].h == h && strcmp(lv_pic[i].file, file) == 0) {
            lv_pic[i].used = ++lv_pic_clock;
            if (lv_pic[i].job) {
                if (!lv_pic[i].job->done)
                    return NULL;            /* still being made: drawn when it is */
                lv_pic_take(i);
            }
            return lv_pic[i].failed ? NULL : &lv_pic[i].im;
        }
        if (lv_pic[i].used < lv_pic[k].used)
            k = i;
    }
    if (file[0] == LV_ART_PLEX) {
        /* the server's picture: in RAM: already, or fetched (one at a time)
         * and asked for again when it has come (lv_art_came) */
        char req[400];
        if (!plex_art(file + 1, w, h, req, sizeof(req), got, sizeof(got))) {
            art_lv_wanted = time_ms();      /* (a list's next part waits: plex_more_ask) */
            if (!art_nj[ART_LAYOUT])
                art_fetch(ART_LAYOUT, file + 1, w, h, got);
            return NULL;
        }
        for (i = 0; i < LV_PICS; i++)       /* the file's made already? */
            if (lv_pic[i].w == w && lv_pic[i].h == h && !strcmp(lv_pic[i].file, got)) {
                lv_pic[i].used = ++lv_pic_clock;
                if (lv_pic[i].job) {
                    if (!lv_pic[i].job->done)
                        return NULL;
                    lv_pic_take(i);
                }
                return lv_pic[i].failed ? NULL : &lv_pic[i].im;
            }
        file = got;                         /* (its slot keyed by the file) */
        k = 0;
        for (i = 1; i < LV_PICS; i++)
            if (lv_pic[i].used < lv_pic[k].used)
                k = i;
    }
    lv_pic_drop(k);
    amb_strlcpy(lv_pic[k].file, file, sizeof(lv_pic[k].file));
    lv_pic[k].w = w;
    lv_pic[k].h = h;
    lv_pic[k].used = ++lv_pic_clock;
    lv_pic[k].failed = TRUE;
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    j->type = LJ_SHOT;
    j->mode = file[0] == LV_ART_SONG ? SHOT_SONG : file[0] == LV_ART_AGS ? SHOT_AGS
            : file[0] == LV_ART_FOLDER ? SHOT_FOLDER : SHOT_FILE;
    amb_strlcpy(j->path, j->mode == SHOT_FILE ? file : file + 1, sizeof(j->path));
    j->scr = g.scr;
    j->tw = w;
    j->th = h;
    j->maxcol = THUMB_DEEP;     /* true colour: a layout draws it so (on an 8-bit screen,
                                 * lk_picture cuts its colours down to pens, undithered: cut
                                 * to 32 or 96 here first, it was cut twice, the loader's
                                 * dithered) */
    lv_pic[k].job = j;
    if (file == lv_pof_file)
        ldr_submit_first(j);                /* (the one shown: before those read ahead) */
    else
        ldr_submit(j);
    return NULL;
}

/* A picture the layout asked for that could not be had (none in the
 * folder, a file that cannot be read): its row then offers none, so a
 * layout draws its look without one, not an empty frame for good */
static BOOL lv_pic_failed(const char *file)
{
    int i;
    for (i = 0; i < LV_PICS; i++)
        if (lv_pic[i].file[0] && !lv_pic[i].job && lv_pic[i].failed && !strcmp(lv_pic[i].file, file))
            return TRUE;
    return FALSE;
}

/* A picture of the layout's for file, made (or failed): not still out */
static BOOL lv_pic_done_for(const char *file)
{
    int i;
    for (i = 0; i < LV_PICS; i++)
        if (lv_pic[i].file[0] && !strcmp(lv_pic[i].file, file))
            return !lv_pic[i].job || lv_pic[i].job->done;
    return FALSE;
}

/* A list page's details picture: the entries next to the one shown read
 * ahead at the layout's size, as the classic pane's previews are (the
 * next one the way the list moves, the one after, the one behind), one
 * at a time while the loader has none of the layout's out - so a steady
 * Down finds each game's screenshot made */
static void lv_pics_ahead(void)
{
    static const struct Page *for_p;
    static LONG for_sel = -1;
    static int step;
    static const LONG near[] = { 1, 2, -1 };
    struct Page *p = cur();
    char file[300];
    int i;
    if (!p || p->type != PT_LIST || !lv_pof_w || hold_active() || ov.on || pane_pending || !det_roomy())
        return;
    if (p != for_p || p->sel != for_sel) {
        for_p = p;
        for_sel = p->sel;
        step = 0;
    }
    if (step >= 3)
        return;
    for (i = 0; i < LV_PICS; i++)           /* (one of the layout's out: after it) */
        if (lv_pic[i].job && !lv_pic[i].job->done)
            return;
    if (!lv_picture_of(p) || !lv_pic_done_for(lv_pof_file))
        return;                             /* (the one shown first) */
    if (lv_pic_file(p, p->sel + near[step] * det.dir, file, sizeof(file)))
        lv_picture(file, lv_pof_w, lv_pof_h);   /* (made, or asked for: drawn from there) */
    step++;
}


/* ---- on and off */
static void lv_give_back_font(void);
static void lv_off(BOOL redraw)
{
    if (!lv.on)
        return;
    if (lv.eng && lv.eng->close)
        lv.eng->close();
    lk_close(&lv.out);
    lv_sp_up = FALSE;
    lv_pics_free();
    lv.on = FALSE;
    view_use(&classic_ui);                  /* (Classic draws again: its details pane too) */
    lv.drawn = -1;
    lv_give_back_font();            /* (the font lent it: the classic's Pixel style again) */
    settings_rebuild();         /* Display's Colours row: the scheme's again */
    if (redraw && g.win && g.rp)
        draw_all();             /* the classic layout, all of it */
}

/* Whether a layout can draw on a screen like this one at all (lv_suits,
 * whatever else is up) */
static BOOL lv_screen_can(void)
{
    return (g.truecolor && !g.planar) || (lv_native && !g.truecolor && g.depth == 8 && !g.wb);
}

static BOOL lv_suits(void)
{
    if (iconified || mini_on || np_full || vw_active || !g.win || !g.rp || !g.scr)
        return FALSE;
    if (g.truecolor && !g.planar)
        return TRUE;                    /* (in either style: the Pixel one's lent a smooth font) */
    return lv_native && !g.truecolor && g.depth == 8 && !g.wb;     /* (the experiment) */
}

/* The classic style is Pixel (no smooth font open): the smooth set for
 * this screen's height, or the nearest under it, lent to g.sm while the
 * layout draws (lv_off takes it back). FALSE: none to be had. */
static BOOL lv_lend_font(void)
{
    static const UWORD sizes[] = { 1200, 1080, 1024, 768, 720, 600, 480, 384 };
    int i;
    if (g.sm)
        return TRUE;
    for (i = 0; i < 8 && !lv.sm; i++)
        if (sizes[i] <= g.h || i == 7)
            lv.sm = sm_load(sizes[i]);
    if (!lv.sm)
        return FALSE;
    g.sm = lv.sm;
    amb_log("layout", "the Pixel style is chosen: a smooth font lent to the layout");
    return TRUE;
}

static void lv_give_back_font(void)
{
    if (lv.sm) {
        if (g.sm == lv.sm)
            g.sm = NULL;
        sm_free(lv.sm);
        lv.sm = NULL;
    }
}

static void lv_activate(void)
{
    const struct LayoutEngine *e = layout_loaded();
    char m[120];
    if (lv.tried == g.scr && lv.tried_w == g.w && lv.tried_h == g.h)
        return;                     /* not here: once is enough */
    lv.tried = g.scr;
    lv.tried_w = g.w;
    lv.tried_h = g.h;
    if (!e || !lv.name[0] || strcmp(e->name, lv.name) != 0) {
        layout_unload();
        e = layout_load(lv.file);
    }
    if (!e) {
        set_msg(_("The layout could not be opened: the classic one is used"));
        return;
    }
    amb_strlcpy(lv.name, e->name, sizeof(lv.name));
    if (!lv_lend_font()) {
        amb_log("layout", "no smooth font for this screen (Fonts/amb-smooth): classic");
        return;
    }
    if (!(g.truecolor ? lk_open(&g, &lv.out, TRUE) : lk_open_chunky(&g, &lv.out))) {
        if (!g.truecolor)
            amb_log("layout", "an 8-bit screen: no room for its canvas, or graphics.library before 40");
        lv_give_back_font();
        return;
    }
    if (lv.out.bits < e->min_bits || g.h < e->min_h) {
        sprintf(m, "%s needs %ld-bit and %ld lines; this is %ld-bit, %ld lines: classic",
                e->name, (long)e->min_bits, (long)e->min_h, (long)lv.out.bits, (long)g.h);
        amb_log("layout", m);
        lk_close(&lv.out);
        lv_sp_up = FALSE;
        lv_give_back_font();
        return;
    }
    memset(&lv.host, 0, sizeof(lv.host));
    lv.host.w = g.w;
    lv.host.h = g.h;
    lv.host.bits = (UBYTE)lv.out.bits;
    lv.host.tier = (UBYTE)plex_tier();
    lvh.g = &g;                     /* (the drawing calls: lvhost.c) */
    lvh.out = &lv.out;
    lvh.ctl = &lv_ctl;
    lvh.letter = lvh_letter_up;
    lvh.own = lvh_own;
    lvh.sp_up = &lv_sp_up;
    lvh.icon_of = lv_icon_of;
    lvh_calls(&lv.host);
    lv.host.icon_px = lk_icon_px(&g);
    lv.host.picture = lv_picture;
    lv.host.model = lv_model;
    lv.host.act = lv_act;
    clock_text(lv.clock);
    lv.host.clock = lv.clock;
    lv.host.debug = lv_debug;
    if (e->open && !e->open(&lv.host)) {
        amb_log("layout", "it would not open: classic");
        lk_close(&lv.out);
        lv_sp_up = FALSE;
        lv_give_back_font();
        return;
    }
    lv.eng = e;
    lv.tried = NULL;                /* (it can: tried again whenever it is off) */
    lv.scr = g.scr;
    lv.win = g.win;
    lv.w = g.w;
    lv.h = g.h;
    lv.on = TRUE;
    view_use(&layout_ui);                   /* (the layout draws: everything noted for its frame) */
    lv.full = TRUE;
    lv.drawn = -1;
    gfx_off_hits = 0;
    settings_rebuild();         /* Display's Colours row: greyed, the layout's */
    sprintf(m, "%s on a %ld-bit %ld x %ld screen%s", e->name, (long)(g.truecolor ? lv.out.bits : g.depth), (long)g.w,
            (long)g.h, lk_canvas(&lv.out) ? ", drawn off screen" : "");
    amb_log("layout", m);
}

static void lv_sync(void)
{
    if (!lv.read) {
        char buf[32];
        lv.read = TRUE;
        if (GetVar((CONST_STRPTR)LAYOUT_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            amb_strlcpy(lv.file, buf, sizeof(lv.file));
        if (GetVar((CONST_STRPTR)LVNATIVE_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            lv_native = buf[0] == '1';
        /* (an AGA screen's background by the copper unless Ambiance/LayoutCopper is 0) */
        if (GetVar((CONST_STRPTR)"Ambiance/LayoutCopper", (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            lk_copper_ok = buf[0] != '0';
        /* (the big letter by sprites on AGA unless Ambiance/LayoutSprites is 0) */
        if (GetVar((CONST_STRPTR)"Ambiance/LayoutSprites", (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            lk_sprites_ok = buf[0] != '0';
        if (GetVar((CONST_STRPTR)"Ambiance/LayoutSkip", (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            lk_skip_ok = buf[0] != '0';
        /* (an AGA screen's copies by the blitter unless Ambiance/LayoutBlit is 0) */
        if (GetVar((CONST_STRPTR)"Ambiance/LayoutBlit", (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            lk_blit_ok = buf[0] != '0';
        if (GetVar((CONST_STRPTR)"Ambiance/LayoutBeam", (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
            lk_beam_ok = buf[0] != '0';
        if (lv.file[0] && LowLevelBase && (key_is_down(0x60) || key_is_down(0x61))) {
            lv.skip = TRUE;
            amb_log("layout", "Shift held as Ambiance started: the classic layout this time");
        }
    }
    if (lv.on && (!lv_suits() || g.scr != lv.scr || g.win != lv.win || g.w != lv.w ||
                  g.h != lv.h || !lv.file[0])) {
        /* gone over to something else: it draws itself, and the classic
         * screen is drawn as it comes back; the screen itself changed:
         * drawn again here, in the layout if it still can */
        BOOL away = iconified || mini_on || np_full || vw_active || !g.win;
        lv_off(!away);
    }
    if (!lv.on && lv.file[0] && !lv.skip && lv_suits())
        lv_activate();
}

/* A section's first level in use, a list: Home shows it, as its tiles
 * (Home and that list were two screens of the same rows) */
static BOOL lv_home_list(void)
{
    struct Page *p = cur();
    return focus == FOCUS_CONTENT && depth == 1 && !pane_pending && p == &pages[0] &&
           p->type == PT_LIST && p->list.count > 0;
}

static int lv_screen(void)
{
    struct Page *p = cur();
    if (focus == FOCUS_MENU)
        lp_all = FALSE;                     /* (a launchpad's whole list: left with its section) */
    if ((focus == FOCUS_MENU || (lv_home_list() && !(lp_all && lp_on_list()))) &&
        (lv.eng->screens & LS_BIT(LS_HOME)))
        return LS_HOME;
    if (p && p->type == PT_PLAYER && (lv.eng->screens & LS_BIT(LS_NOW)))
        return LS_NOW;
    {
        int m = lv_music_screen(p);
        if (m != LS_PAGE && (lv.eng->screens & LS_BIT(m)))
            return m;
    }
    return LS_PAGE;
}

/* A busy wait's frame (busy_tick, the main loop not running): the
 * layout drawn as it is now, the first time whole, then its spinner */
static void lv_busy_frame(void)
{
    if (!lv.on || !lv.eng)
        return;
    if (lv.wait_phase < 0)
        lv.full = TRUE;                     /* (no spinner up yet: all of it) */
    lv_frame();
}

/* What plays, all but its time: a change in it is drawn whole; only
 * the time going on (and the clock, the meters) is LC_PLAYING's */
static ULONG lv_sig_s(ULONG h, const char *t)
{
    if (t)
        for (; *t; t++)
            h = h * 33 + (UBYTE)*t;
    return h * 33 + 1;
}
static ULONG lv_now_sig(void)
{
    const struct LNow *n = lv_model(LS_NOW);
    ULONG h = 5381;
    h = lv_sig_s(h, n->title);
    h = lv_sig_s(h, n->artist);
    h = lv_sig_s(h, n->album);
    h = lv_sig_s(h, n->art);
    h = lv_sig_s(h, n->format);
    h = lv_sig_s(h, n->source);
    h = lv_sig_s(h, n->nnext > 0 ? n->next[0] : NULL);
    h = h * 33 + n->kind;
    h = h * 33 + (n->paused ? 1 : 0) + (n->stopped ? 2 : 0);
    h = h * 33 + (ULONG)(n->btn + 1);
    h = h * 33 + n->off;
    h = h * 33 + n->on;
    h = h * 33 + n->volume;
    h = h * 33 + (n->len_ms / 1000);
    return h;
}

/* The layout's time (Debug's load line): ms in lv_frame, whole draws and
 * parts (changed) since lv_timing last asked */
static ULONG lvt_ms, lvt_whole, lvt_parts;
void lv_timing(ULONG *ms, ULONG *whole, ULONG *parts)
{
    *ms = lvt_ms;
    *whole = lvt_whole;
    *parts = lvt_parts;
    lvt_ms = lvt_whole = lvt_parts = 0;
}

static void lv_frame_(void);
void lv_frame(void)
{
    ULONG t0 = time_ms(), d;
    lv_frame_();
    d = time_ms() - t0;
    lvt_ms += d;
    if (d > lvt_worst)
        lvt_worst = d;
}

static void lv_frame_(void)
{
    int scr;
    ULONG now;
    lv_sync();
    if (!lv.on)
        return;
    scr = lv_screen();
    now = time_ms();
    if (lv_sp_up && !(ov.on && ov.page == cur() && ov.bl)) {
        lk_letter_sprites_off(&lv.out);     /* (the letter by sprites down with the letter) */
        lv_sp_up = FALSE;
    }
    lv_pic_made = FALSE;
    lv_pics_poll();
    if (lv_art_came) {
        lv_art_came = FALSE;
        lv_img = TRUE;              /* (the layout asks again, and finds it) */
    }
    if (lv.eng->tick && (LONG)(now - lv.tick_at) >= 40) {
        ULONG t0 = time_ms(), dt;
        lv.tick_at = now;
        lv.eng->tick(now);
        if ((dt = time_ms() - t0) >= 2) {      /* (one that drew: a glide's step) */
            lvp_n[4]++;
            lvp_ms[4] += dt;
        }
    }
    if (scr != lv.drawn)
        lv.full = TRUE;
    if (lv_playing_hit) {
        /* the time, the clock or the Debug line went on, and nothing else
         * was drawn: the layout's own of those alone (what plays the same) */
        lv_playing_hit = FALSE;
        if (!lv.full && !lv_dirty && !gfx_off_hits && lv.drawn == scr && lv.eng->changed) {
            ULONG nsig = lv_now_sig(), tsig;
            const struct LNow *n = lv_model(LS_NOW);
            char ck[40];
            clock_text(ck);
            tsig = lv_sig_s(lv_sig_s((n->pos_ms / 1000) * 7 + n->kind, ck), lv_debug);
            if (nsig == lv.last_now) {
                if (tsig != lv.last_time) {     /* (what it shows changed: a second, a minute) */
                    lv.last_time = tsig;
                    clock_text(lv.clock);
                    lk_clip(&lv.out, 0, 0, g.w, g.h);
                    {
                        ULONG t0 = time_ms();
                        lv.eng->changed(scr, LC_PLAYING);
                        lvp_n[0]++;
                        lvp_ms[0] += time_ms() - t0;
                    }
                    lvt_parts++;
                }
            } else {
                lv.full = TRUE;
            }
        }
    }
    if (lv_img && !lv.full && lv.eng->changed) {
        if (lv_dirty || gfx_off_hits) {
            /* something else to draw too (the details came with it, a
             * move): that first, the picture's own after - a whole draw
             * for the two (each game's details and screenshot, a
             * moment apart: 360 ms each without the JIT) was most of
             * Games' slowness */
        } else {
            lv_img = FALSE;
            lk_clip(&lv.out, 0, 0, g.w, g.h);
            {
                ULONG t0 = time_ms();
                lv.eng->changed(scr, LC_IMAGE);     /* pictures came: what waited for them */
                lvp_n[1]++;
                lvp_ms[1] += time_ms() - t0;
            }
            lvt_parts++;
        }
    } else if (lv_img) {
        lv_img = FALSE;                         /* (a whole draw: it draws them) */
        lv.full = TRUE;
    }
    if (!lv.full && !lv_dirty && !gfx_off_hits && (scr == LS_PAGE || scr == LS_ALBUMS) && lv.eng->changed &&
        lv_waiting(cur()) &&
        lv_wait_step() != lv.wait_phase) {
        lv.wait_phase = lv_wait_step();     /* the spinner turned: it alone drawn again */
        lk_clip(&lv.out, 0, 0, g.w, g.h);
        lv.eng->changed(scr, LC_WAIT);
    }
    if (lv.full || lv_dirty || gfx_off_hits) {
        struct Page *p = cur();
        ULONG mh = 5381;
        const char *c;
        char letter = p && ov.on && ov.page == p && ov.bl ? ov.letter : 0;
        BOOL moved_only, details_only;
        ULONG nsig;
        for (c = msg; *c; c++)
            mh = mh * 33 + (UBYTE)*c;
        nsig = lv_now_sig();
        moved_only = !lv.full && (scr == LS_PAGE || scr == LS_ALBUMS || scr == LS_ALBUM ||
                                  scr == LS_ARTIST || (scr == LS_HOME && focus != FOCUS_MENU)) &&
                     lv.drawn == scr && lv.eng->changed && p &&
                     p == lv.last_p && depth == lv.last_depth && mh == lv.last_msg &&
                     (p->type == PT_LIST
                          ? p->list.count == lv.last_count && (const void *)p->list.items == lv.last_items &&
                            (p->sel != lv.last_sel || letter != lv.last_letter ||
                             /* a row drawn again, the count (a read ahead's preview, the
                              * list's letters): the page's body, not all - each was a
                              * whole draw, 2-3 a second at the start */
                             (lv_what && !(lv_what & ~(ULONG)(LC_SELECT | LC_LIST | LC_DETAILS))))
                          : (p->type == PT_TEXT || p->type == PT_DETAIL) && p->tb.open &&
                            p->tb.pos != lv.last_pos);
        /* only the details came (the selection where it was, nothing else
         * noted): the layout's page body alone */
        details_only = !lv.full && !moved_only && lv_what == LC_DETAILS && scr == LS_PAGE &&
                       lv.drawn == scr && lv.eng->changed && p && p == lv.last_p && depth == lv.last_depth &&
                       mh == lv.last_msg && nsig == lv.last_now && p->type == PT_LIST &&
                       p->list.count == lv.last_count && (const void *)p->list.items == lv.last_items &&
                       p->sel == lv.last_sel && letter == lv.last_letter;
        /* only classic drawing that slipped past the gates came (counted
         * by gfx_off, logged as "unseen"), and nothing the layout shows
         * changed: nothing to draw (it once meant drawing it all) */
        if (!lv.full && !lv_dirty && lv.drawn == scr && p && p == lv.last_p && depth == lv.last_depth &&
            mh == lv.last_msg && nsig == lv.last_now && letter == lv.last_letter &&
            (p->type == PT_LIST ? p->list.count == lv.last_count &&
                                      (const void *)p->list.items == lv.last_items && p->sel == lv.last_sel
                                : !p->tb.open || p->tb.pos == lv.last_pos)) {
            gfx_off_hits = 0;
            goto shown;
        }
        if (!(moved_only && nsig == lv.last_now) && !details_only) {
            /* (why a whole draw: Debug's "wholes" line) */
            if (lv.full)
                lvw_why[0]++;
            else if (lv_what & LV_WHOLE)
                lvw_why[1]++;
            else if (!lv_dirty)
                lvw_why[2]++;
            else {
                lvw_why[3]++;
                lvw_what |= lv_what;
                if (mh != lv.last_msg)
                    lvw_why[4]++;
                if (nsig != lv.last_now)
                    lvw_why[5]++;
                if (p && p != lv.last_p)
                    lvw_why[6]++;
                if (p && p == lv.last_p && p->sel == lv.last_sel)
                    lvw_why[7]++;
            }
        }
        lv.full = FALSE;
        lv_dirty = FALSE;
        lv_what = 0;
        gfx_off_hits = 0;
        lv.drawn = scr;
        clock_text(lv.clock);
        lv_ri = 0;
        lk_clip(&lv.out, 0, 0, g.w, g.h);
        {
            ULONG t0 = time_ms();
            int k = -1;
            if (moved_only && nsig == lv.last_now)
                lv.eng->changed(scr, LC_SELECT), lvt_parts++, k = 2;
            else if (details_only)
                lv.eng->changed(scr, LC_DETAILS), lvt_parts++, k = 3;
            else
                lk_fresh(&lv.out), lv.eng->draw(scr), lvt_whole++;
            if (k >= 0) {
                lvp_n[k]++;
                lvp_ms[k] += time_ms() - t0;
            }
        }
        lv.last_now = nsig;
        lv.wait_phase = lv_waiting(p) ? lv_wait_step() : -1;
        lv.last_p = p;
        lv.last_items = p ? (const void *)p->list.items : NULL;
        if (p && p == lv.last_p && p->sel != lv.last_sel)
            lk_show_up = p->sel < lv.last_sel;  /* (shown from the bottom up when it went up:
                                                 * the old highlight gone before the new one
                                                 * comes, never two a moment) */
        lv.last_sel = p ? p->sel : -1;
        lv.last_count = p ? p->list.count : -1;
        lv.last_depth = depth;
        lv.last_letter = letter;
        lv.last_msg = mh;
        lv.last_pos = p && p->tb.open ? p->tb.pos : -1;
    }
shown:
    lk_show(&lv.out);
    lv_ta_watch();
    if (scr == LS_PAGE) {
        struct Page *p = cur();
        /* a list's details not shown, nor asked for: asked for now (the
         * classic asks as it draws the title or moves; come back to at the
         * start, nothing did, and the pane stayed empty until a move) */
        static ULONG unshown_since;
        static const struct Page *diag;
        /* (Debug: a list's details not given to the layout for 3 s, at rest:
         * why, once a page) */
        if (p && p->type == PT_LIST && p->sel >= 0 && p->sel < p->list.count &&
            !(det.page == p && det.sel == p->sel && det.text_shown)) {
            if (!unshown_since)
                unshown_since = time_ms();
            else if (time_ms() - unshown_since > 3000 && diag != p) {
                char m[200];
                diag = p;
                sprintf(m, "no details yet for row %ld: pending %ld, due in %ld ms, shown %ld (this %ld), "
                        "text %s, pane pending %ld, moving %ld, letter %ld, has pane %ld",
                        (long)p->sel, (long)det.pending, (long)(det.due - time_ms()), (long)det.shown,
                        (long)(det.page == p && det.sel == p->sel),
                        !det.tj ? "none" : det.tj->done ? "back" : "out", (long)pane_pending,
                        (long)hold_active(), (long)ov.on, (long)has_aside(p));
                amb_log("layout", m);
            }
        } else {
            unshown_since = 0;
        }
        if (p && p->type == PT_LIST && p->sel >= 0 && p->sel < p->list.count && !det.pending && !pane_pending &&
            !(det.shown && det.page == p && det.sel == p->sel) && !hold_active() && has_aside(p)) {
            static const struct Page *told;
            if (told != p) {
                char m[80];
                told = p;
                sprintf(m, "details asked for (none were): row %ld%s", (long)p->sel, ov.on ? ", letter up" : "");
                amb_log("layout", m);
            }
            det_touch(p);
        }
        lv_pics_ahead();
    }
}

void lv_hold(void)
{
    lv_off(FALSE);
}

/* Back from the viewer or a video: the layout on again before anything is
 * drawn, so the classic screen never shows for a frame */
void lv_resume(void)
{
    lv_controls_end();                      /* (opened for a viewer's or video's controls: closed) */
    lv_sync();
    if (!lv.on)
        lv_give_back_font();                /* (lent for the controls: the classic's Pixel again) */
}

void lv_release(void)
{
    FreeVec(lv_mem);                        /* (the controls' memory strip) */
    lv_mem = NULL;
    lv_mem_n = 0;
    lv_ta_save();                           /* (Home's tiles' pictures, for the next run) */
    lv_off(FALSE);
    layout_unload();
    lv.tried = NULL;
}

/* The wheel's notches as they come (the log says when several come at
 * once: a driver sending more than one a notch) */
static ULONG wheel_last, wheel_t0, wheel_burst, wheel_said;

/* ---- input */
BOOL lv_key(struct IntuiMessage *im)
{
    int a = -1, scr;
    LONG arg = 0;
    UWORD code = im->Code;
    if (!lv.on)
        return FALSE;
    if (im->Class == IDCMP_RAWKEY) {
        if (code & IECODE_UP_PREFIX)
            return FALSE;
        switch (code) {
        case 0x4C: a = LA_UP; break;
        case 0x4D: a = LA_DOWN; break;
        case 0x4E: a = LA_RIGHT; break;
        case 0x4F: a = LA_LEFT; break;
        case 0x7A: case 0x7B: {
            /* the wheel: a notch one entry on, in the list's own order
             * (a wall's Up and Down are a row, or stop at a column's end:
             * not the wheel's). Home and Now playing: the layout's Up and
             * Down (Home's sections). The classic's finds what is under
             * the pointer by its own places, which are not the layout's. */
            int d = code == 0x7A ? -1 : 1;
            ULONG now = time_ms();
            scr = lv_screen();
            if (now - wheel_last < 25) {
                wheel_burst++;              /* (notches closer than a hand turns: the driver's?) */
            } else {
                if (wheel_burst >= 2 && wheel_said < 5) {
                    char m[80];
                    sprintf(m, "%ld notches within %ld ms", (long)wheel_burst + 1, (long)(wheel_last - wheel_t0));
                    amb_log("wheel", m);
                    wheel_said++;
                }
                wheel_burst = 0;
                wheel_t0 = now;
            }
            wheel_last = now;
            if (scr == LS_PAGE || scr == LS_ALBUMS || scr == LS_ALBUM || scr == LS_ARTIST ||
                !lv.eng->input || lv.eng->input(scr, d < 0 ? LA_UP : LA_DOWN, 0) != LR_DONE) {
                struct Page *wp = cur();
                act_move(focus == FOCUS_CONTENT && wp && wp->type != PT_LIST && wp->type != PT_PLAYER
                         ? d * 3 : d);       /* (a text: three lines a notch, as the classic's) */
            }
            return TRUE;
        }
        }
        if (im->Qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) {
            if (a == LA_UP)
                a = LA_PAGEUP;
            else if (a == LA_DOWN)
                a = LA_PAGEDOWN;
        }
    } else if (im->Class == IDCMP_VANILLAKEY) {
        if (im->Qualifier & IEQUALIFIER_NUMERICPAD)
            return FALSE;
        if (code == 13)
            a = LA_RETURN;
        else if (code == 27 || code == 8)
            a = LA_BACK;
        else if (code == ' ')
            a = LA_SPACE;
        else if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
                 (code >= '0' && code <= '9')) {
            a = LA_LETTER;
            arg = code;
        }
    }
    if (a < 0 || !lv.eng->input)
        return FALSE;
    scr = lv_screen();
    return lv.eng->input(scr, a, arg) == LR_DONE;
}

BOOL lv_mouse(struct IntuiMessage *im)
{
    if (!lv.on)
        return FALSE;
    if (im->Class == IDCMP_MOUSEMOVE) {
        mouse_last = time_ms();
        if (ptr_hidden)
            pointer_show();
        set_mouse_mode(TRUE);               /* (the keys' choice shown no more) */
        /* what the pointer is over: the layout's hover glow */
        if (lv.eng->pointer && g.win) {
            lk_clip(&lv.out, 0, 0, g.w, g.h);
            if (lv.eng->pointer(lv_screen(), im->MouseX - g.win->BorderLeft,
                                im->MouseY - g.win->BorderTop, LP_MOVE) == LR_DONE)
                lk_show(&lv.out);
        }
        return TRUE;
    }
    if (im->Class != IDCMP_MOUSEBUTTONS)
        return TRUE;
    mouse_last = time_ms();
    set_mouse_mode(TRUE);
    if (ptr_hidden)
        pointer_show();
    if (im->Code == MENUDOWN) {             /* the right button: back, whatever the layout */
        disarm();
        act_back();
    } else if (im->Code == SELECTDOWN && lv.eng->pointer) {
        lv.eng->pointer(lv_screen(), im->MouseX - g.win->BorderLeft, im->MouseY - g.win->BorderTop,
                        LP_PRESS);
    } else if (im->Code == SELECTUP && lv.eng->pointer) {
        lv.eng->pointer(lv_screen(), im->MouseX - g.win->BorderLeft, im->MouseY - g.win->BorderTop,
                        LP_RELEASE);                /* (a scroll bar's drag ends) */
    }
    return TRUE;
}

/* ---- Settings > Display > Layout */
void lv_build_page(struct Page *p)
{
    int i;
    action_item(p, _("Classic"), "", ACT_LAYOUT0, lv.file[0] ? MARK_RADIO_OFF : MARK_RADIO_ON);
    lv.nfound = layout_list(lv.found, LV_MAX);
    for (i = 0; i < lv.nfound; i++) {
        BOOL on = strcmp(lv.found[i].file, lv.file) == 0;
        struct Item *it = action_item(p, lv.found[i].name, "", ACT_LAYOUT0 + 1 + i,
                                      on ? MARK_RADIO_ON : MARK_RADIO_OFF);
        if (on) {
            p->sel = p->list.count - 1;
            amb_strlcpy(lv.name, lv.found[i].name, sizeof(lv.name));
        }
        if (it && !lv_screen_can())
            it->flags |= ITEM_DISABLED;     /* (a graphics card's 15-32 bit screen, or an 8-bit
                                             * native one with Layouts on native screens on;
                                             * greyed there too, a layout could be left for the
                                             * classic and never chosen again) */
    }
    if (layout_stale) {
        /* an older copy of a layout is still in memory, open (an Ambiance
         * before 0.9.1.1-28 left it so): Exec hands it out by its name */
        head_para(p, _("An older version of a layout is still in memory: restart the Amiga to see the new one."));
    }
}

void lv_pick(int k)
{
    struct Page *p = cur();
    LONG i;
    const char *file = k > 0 && k <= lv.nfound ? lv.found[k - 1].file : "";
    if (k > lv.nfound || strcmp(file, lv.file) == 0)
        return;
    amb_strlcpy(lv.file, file, sizeof(lv.file));
    lv.name[0] = 0;
    if (k > 0)
        amb_strlcpy(lv.name, lv.found[k - 1].name, sizeof(lv.name));
    lv.skip = FALSE;
    lv.tried = NULL;
    save_setting(LAYOUT_VAR, file);
    amb_log("layout chosen", k ? file : "classic");
    if (p)
        for (i = 0; i < p->list.count; i++)
            if (p->list.items[i].action >= ACT_LAYOUT0 && p->list.items[i].action <= ACT_LAYOUT_END)
                p->list.items[i].mark = p->list.items[i].action == ACT_LAYOUT0 + k ? MARK_RADIO_ON
                                                                                  : MARK_RADIO_OFF;
    settings_rebuild();             /* the Display page's row says which */
    lv_release();                   /* another one, or none: the old one closed and put
                                     * out of memory, on or not (chosen on a screen that
                                     * did not suit it, it was open all the same) */
    if (!k) {
        draw_all();
    } else if (ui_style != STYLE_SMOOTH && g.truecolor && gfx_style_possible(&caps, g.mode, STYLE_SMOOTH)) {
        apply_style(STYLE_SMOOTH);  /* a layout chosen: the Smooth style with it, where it can be */
    } else {
        draw_pane(TRUE);
        if (!lv_suits())
            set_msg(_("Layouts need a graphics card's 15 to 32-bit screen"));
    }
}

/* ---- the views (struct UiView): Classic's draws at once (cl_), a
 * layout's notes what changed for its next frame (ly_) */
static void ly_draw_header(void)
{
    lv_note();
}
static void ly_draw_menu(void)
{
    lv_note();
}
static void ly_draw_menu_row(int i)
{
    (void)i;
    lv_note();
}
static void ly_draw_footer(void)
{
    lv_note_what(LC_MESSAGE);
}
static void ly_draw_title(struct Page *p)
{
    (void)p;
    lv_note();
}
static void ly_draw_title_count(struct Page *p)
{
    (void)p;
    lv_note_what(LC_SELECT);
}
static void ly_draw_list_row(struct Page *p, LONG idx, WORD col)
{
    (void)p;
    (void)idx;
    (void)col;
    lv_note_what(LC_SELECT);
}
static void ly_draw_gutter_planes(struct Page *p, UBYTE planes)
{
    (void)p;
    (void)planes;
    lv_note_what(LC_SELECT);
}
static void ly_draw_list_rows(struct Page *p)
{
    (void)p;
    lv_note_what(LC_SELECT | LC_LIST);
}
static void ly_draw_list_move(struct Page *p, LONG old_sel, LONG old_top)
{
    (void)p;
    (void)old_sel;
    (void)old_top;
    lv_note_what(LC_SELECT);
}
static void ly_draw_text_picture(struct Page *p)
{
    (void)p;
    lv_note();
}
static void ly_draw_pane(BOOL full)
{
    struct Page *p = cur();
    /* a text page's lines in a text box of the classic's geometry (the
     * keys scroll it, and LPage.text_frac comes from it) */
    if (p && p->type == PT_TEXT && (full || !p->tb.open)) {
        open_textbox(p, g.list_top);
    } else if (p && p->type == PT_DETAIL && (full || !p->tb.open)) {
        p->facts_y = g.list_top;
        open_textbox(p, p->facts_y);
    }
    lv_note();
}
static void ly_np_draw(BOOL full)
{
    if (full)
        plex_shown[0] = 0;                  /* (the pane's details: gone under this page) */
    lv_note();
}
static void ly_draw_all(void)
{
    lv_note();
}
static void ly_pane_preview(BOOL first)
{
    (void)first;
    lv_note_what(LC_SELECT);                /* (Home shows the section moved to) */
}

static void ly_tick(void)
{
}
static void ly_header_np(BOOL quiet, BOOL on)
{
    (void)quiet;
    (void)on;
    lv_note();                              /* (playing or not: the layout's, from its model) */
}
static void ly_clock(void)
{
    lv_playing_hit = TRUE;                  /* (the layout's own clock) */
}
static void ly_status(const char *show)
{
    if (show)
        lv_playing_hit = TRUE;              /* (a layout's own line, lv_debug) */
}
static void ly_np_station(void)
{
}
static void ly_np_visual(void)
{
    if (np_on())
        lv_playing_hit = TRUE;
}
static void ly_np_art(void)
{
}
static void ly_page_leave(struct Page *gone, struct Page *under)
{
    (void)gone;
    (void)under;
}
static void ly_np_update(void)
{
    lv_playing_hit = TRUE;                  /* (the layout's own Now playing: its time alone) */
}
static void ly_none(void)
{
}
static void ly_np_art_room(BOOL none)
{
    (void)none;
}
static void ly_menu_slide(UWORD from, UWORD to)
{
    (void)from;
    (void)to;
}
static void ly_mouse(struct IntuiMessage *im)
{
    lv_mouse(im);                           /* (the layout's hit tests; the right button: back) */
}
static void ly_readouts(BOOL clock, BOOL status)
{
    if (clock)
        draw_clock();                       /* (ly_clock, ly_status: lv_playing_hit) */
    if (status)
        draw_status();
}
static void ly_wheel(LONG d, WORD x, WORD y)
{
    (void)d;                                /* (lv_key takes the wheel first) */
    (void)x;
    (void)y;
}
static void ly_pane_summary(const char *text)
{
    (void)text;                             /* (the layout's details: from the items) */
}

const struct UiView layout_ui = {
    ly_draw_header, ly_draw_menu, ly_draw_menu_row, ly_draw_footer, ly_draw_title, ly_draw_title_count,
    ly_draw_list_row, ly_draw_gutter_planes, ly_draw_list_rows, ly_draw_list_move, ly_draw_text_picture,
    ly_draw_pane, ly_np_draw, ly_draw_all, ly_pane_preview,
    ly_tick, ly_busy, ly_header_np, ly_clock, ly_status, ly_letter_show, ly_letter_relabel,
    ly_letter_hide, ly_np_station, ly_eq_tick, ly_np_visual, ly_np_art,
    ly_page_leave, ly_np_update, ly_none, ly_none, ly_np_art_room, ly_menu_slide, ly_pane_summary,
    ly_mouse, ly_wheel, ly_none, ly_readouts,
    "layout", &layout_view
};

/* The view that drives the screen: Classic's or the layout's. Each has
 * its entry details' view; gfx.c's classic drawing calls are done only
 * while Classic drives (under a layout one that still comes is counted,
 * gfx_off_hits, and logged as "unseen": a call not yet routed). */
void view_use(const struct UiView *v)
{
    ui = v;
    det_view(v->details);
    gfx_off = v != &classic_ui;
    if (ov.bl)
        v->letter_place();                  /* (a letter up: where this view stamps it) */
}

void np_update(void)
{
    ui->np_update();
}

/* ---- the program's side of each: its state, then the view driven */
void draw_header(void)
{
    ui->header();
}
void draw_menu(void)
{
    ui->menu();
}
void draw_menu_row(int i)
{
    ui->menu_row(i);
}
void draw_footer(void)
{
    ui->footer();
}
void draw_title(struct Page *p)
{
    ui->title(p);
}
void draw_title_count(struct Page *p)
{
    det_touch(p);                           /* its details, once it rests */
    ch_pump(p);                             /* letters around the selection, ahead */
    ui->title_count(p);
}
void draw_list_row(struct Page *p, LONG idx, WORD col)
{
    ui->list_row(p, idx, col);
}
void draw_gutter_planes(struct Page *p, UBYTE planes)
{
    ui->gutter_planes(p, planes);
}
void draw_list_rows(struct Page *p)
{
    list_follow(p);                         /* where the list is scrolled to */
    ui->list_rows(p);
}
void draw_list_move(struct Page *p, LONG old_sel, LONG old_top)
{
    list_follow(p);
    ui->list_move(p, old_sel, old_top);
}
void draw_pane(BOOL full)
{
    struct Page *p = cur();
    if (!p)
        return;
    if (full) {
        det.shown = FALSE;                  /* the pane anew: its details asked for again */
        tb_close(&pv_tb);
    }
    if (p->type == PT_LIST)
        list_follow(p);
    ui->pane(full);
    if (p->type == PT_LIST && !pane_pending)
        det_touch(p);                       /* (its details: Classic's title asks too) */
}
void np_draw(BOOL full)
{
    ui->np(full);
}
void draw_all(void)
{
    struct Page *p;
    pane_settle();                          /* (the section built, if still to be) */
    ui->all();
    if ((p = cur()) && p->type == PT_LIST && !pane_pending)
        det_touch(p);                       /* its details asked for */
}
void pane_preview(BOOL first)
{
    ui->pane_preview(first);
}

/* ---- app.h: main.c's answers to the services (details.c) */
struct Page *app_cur(void)
{
    return cur();
}
BOOL app_moving(void)
{
    return hold_active() || ov.on;
}
BOOL app_pane_pending(void)
{
    return pane_pending;
}
ULONG app_dc_time(void)
{
    return dc_time();
}
BOOL app_plex_art_kept(struct Page *p, LONG idx)
{
    return plex_art_kept(p, idx);
}
BOOL app_plex_movable(struct Page *p, LONG idx)
{
    return plex_movable(p, idx);
}
void app_plex_ahead(struct Page *p)
{
    plex_ahead(p);
}
