/*
 * AmigaMCE - main program: Home Screen, navigation, input and launching
 * Copyright (c) 2026 Brian King
 */
#include "amce.h"

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

static const char version_tag[] __attribute__((used)) =
    "$VER: AmigaMCE " AMCE_VERSION " (" AMCE_DATE ")";

struct IntuitionBase *IntuitionBase;
struct Library *AslBase;            /* opened for Settings > Music folders' requester */
struct GfxBase *GfxBase;
struct Library *DiskfontBase, *DataTypesBase, *LowLevelBase, *LayersBase;

/* ------------------------------------------------------------ modules */
enum { MOD_GAMES, MOD_MODS, MOD_DEMOS, MOD_VIDEOS, MOD_PICTURES, MOD_MUSIC,
       MOD_RADIO, MOD_SETTINGS, MOD_ABOUT, MOD_COUNT };
static const char *mod_names[MOD_COUNT] = {
    "Games", "Mods", "Demos", "Videos", "Pictures", "Music", "Radio", "Settings", "About"
};

enum { PT_LIST, PT_TEXT, PT_DETAIL, PT_PLAYER };
enum { FOCUS_MENU, FOCUS_CONTENT,
       FOCUS_PREVIEW };     /* the preview pane's fact sheet: Up/Down scroll it */
#define MAXDEPTH 12
#define FACT_LINES 24

struct Chunks;
static void ch_free(struct Chunks *ch);

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
    char plex_label[96];        /* a Plex album's page: the album (its details pane) */
    UBYTE setp;                 /* a Settings page: SETP_* */
    UBYTE songs;                /* Music's own files: IT_DIR and IT_SONG rows (mods.c) */
    UBYTE photos;               /* a Plex photo library's (Pictures): albums, IT_PLEXPHOTO */
    char plex_meta[720];
    ULONG net_seq;              /*   its request out (base: the query), 0 none */
    struct Chunks *ch;          /* a long list held a letter at a time (see Chunk) */
    UBYTE title_wide;           /* title drawn with room for a growing count */
    char pic[160];              /* PT_TEXT: a picture under the text box ("" none), */
    struct BitMap *pic_bm;      /*   made for the screen when the page is first drawn, */
    WORD pic_w, pic_h;          /*   as large as fits there, */
    WORD pic_x, pic_y;          /*   drawn there (pic_y < 0: not drawn) */
    const char *note;           /*   and a line or two above it (fixed, not scrolled) */
};

#define PICS_ROOT "Media:Images"
#define SLIDE_MS  5000          /* slideshow: each picture this long */
#define BAR_MS    3000          /* the viewer's info bar, after a change */

static struct Gfx g;
static struct Caps caps;
static struct Ags ags;
static struct Page pages[MAXDEPTH];
static int depth;
static int menu[MOD_COUNT], nmenu, menu_sel;
static int focus = FOCUS_MENU;
static BOOL running = TRUE, quit_armed;
static char msg[80];
static char fact_buf[FACT_LINES][96];
static LONG last_minute = -1;
static int screen_mode = SM_AUTO;   /* as chosen in Settings, may be SM_AUTO */
static int ui_style = STYLE_PIXEL;  /* Settings > Style (smooth only on RTG screens) */
static BOOL pane_pending;           /* menu moved, its section not built yet (see pane_settle) */
static struct TextBox pv_tb;        /* the preview pane's fact sheet (scrolls) */
static struct Page *pv_page;        /* what the preview shows: page and entry */
static LONG pv_sel;
static BOOL pv_shown;               /* FALSE once the pane is cleared or left */
static struct ItemList pv_list;     /* its wrapped lines */
static struct LJob *pv_tj, *pv_sj;  /* the text and picture the pane shows or waits for */
static BOOL mouse_mode;             /* the mouse was used last */
/* Mouse mode: the row under the pointer, highlighted softly (PEN_RULE;
 * the selection stays where it is until a click). Kept as a place on
 * screen, so a list scrolling under a still pointer stays right. */
static WORD hover_row = -1;         /* content list: screen row, -1 none */
static WORD hover_menu = -1;        /* main menu: entry, -1 none */
/* The keyboard or joystick moving: the pointer hides (an empty sprite)
 * and so does its highlight, until the mouse moves. */
static struct Window *ptr_hidden;   /* the window whose pointer is hidden, or NULL */
static UWORD *ptr_blank;            /* the empty pointer (chip RAM) */

/* Design units (a 256-line layout) to pixels on this screen. */
#define U(v) GU(&g, v)

/* Big-letter overlay on long content lists while they glide (see below). */
static struct {
    BOOL  on;
    struct Page *page;
    char  letter;
    const struct BigLetter *bl;     /* gfx.c owns it */
    WORD  x, y;
    WORD  strip_x;          /* left edge of the letter's strip; rows stop before it */
    UWORD linger;           /* timer ticks left before hiding, 0 = not counting */
} ov;
static BOOL debug_on;               /* Settings > Debug: header CPU/memory readout and log */

/* IT_ACTION codes (struct Item.action) */
enum { ACT_SCREENMODES = 1, ACT_SYSINFO, ACT_CREDITS, ACT_DEBUG, ACT_STYLES,
       ACT_STYLE_PIXEL, ACT_STYLE_SMOOTH, ACT_LFONT0, ACT_LFONT_END = ACT_LFONT0 + LFONT_COUNT - 1, ACT_JUMP, ACT_DBUF, ACT_NOWPLAYING, ACT_HTTPS, ACT_PLEXHELP, ACT_PLEXCONV, ACT_PLEXON, ACT_PLEXSET, ACT_SOUNDSET, ACT_SND_PAULA, ACT_SND_AHI, ACT_SND_MPEGA, ACT_SND_MHI,
       ACT_SND_FLAC, ACT_GENSET, ACT_NETSET, ACT_SCREENSET, ACT_NICSET,
       ACT_RADIOSET, ACT_RKBPSSET, ACT_RSRVSET, ACT_BUFSET, ACT_RKBPS0, ACT_RKBPS_END = ACT_RKBPS0 + 5,
       ACT_BUF0, ACT_BUF_END = ACT_BUF0 + 2, ACT_RSRVAUTO, ACT_RSRVFAST, ACT_RSRV0,
       ACT_RSRV_END = ACT_RSRV0 + 7, ACT_NICAUTO, ACT_NIC0, ACT_NIC_END = ACT_NIC0 + 7,
       ACT_MUSICSET, ACT_MUSICASSIGN, ACT_MUSICADD, ACT_MUSICDIR0, ACT_MUSICDIR_END = ACT_MUSICDIR0 + 7,
       ACT_PLEXSLIDES,
       ACT_COUNT,
       /* + SM_*: well past the rest (at 16 it had come to share numbers with
        * Now playing, HTTPS and the Plex rows, as actions were added) */
       ACT_MODE0 = 128 };
typedef char act_mode0_clear[ACT_COUNT <= ACT_MODE0 && ACT_MODE0 + SM_COUNT <= 255 ? 1 : -1];

/* ------------------------------------------------------------ pages */
static struct Page *cur(void) { return depth ? &pages[depth - 1] : NULL; }

static void nav_moved(void);

static struct Page *page_push(UBYTE type, const char *title)
{
    nav_moved();
    ov.on = FALSE;
    tb_close(&pv_tb);
    pv_shown = FALSE;
    pv_tj = pv_sj = NULL;
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
    amce_strlcpy(p->title, title, sizeof(p->title));
    depth++;
    return p;
}

static WORD textbox_w(void);
static BOOL has_aside(struct Page *p);
static WORD aside_x(void);

/* A text page's picture (Page.pic), made for this screen: as wide as the
 * text and no taller than the box, in pens of its own (gfx_thumb_bitmap). */
static void page_picture(struct Page *p, WORD w, WORD h)
{
    struct PicInfo pi;
    struct Thumb *t;
    BOOL tall = gfx_mode_tall(g.mode);
    if (!gfx_thumbs(&g)) {
        amce_log("page picture", "this screen shows no pictures");
        return;
    }
    if (!(t = ilbm_thumb(p->pic, w, h, tall, FALSE, gfx_thumb_colours(&g), &pi))) {
        BOOL remapped = FALSE;
        APTR o = pic_decode(p->pic, NULL, FALSE, &remapped, &pi);
        if (o) {
            ULONG dw, dh;
            pic_fit(pi.w, pi.h, tall, w, h, FALSE, &dw, &dh);    /* smaller or larger: to fit */
            t = thumb_make(o, dw, dh, gfx_thumb_colours(&g));
            pic_dispose(o);
        }
    }
    if (!t) {
        amce_log("page picture: could not be read", p->pic);
        return;
    }
    if ((p->pic_bm = gfx_thumb_bitmap(&g, t))) {
        char m[200];
        p->pic_w = t->w;
        p->pic_h = t->h;
        sprintf(m, "%s, %ld x %ld", p->pic, (long)t->w, (long)t->h);
        amce_log("page picture", m);
    } else {
        amce_log("page picture: no memory for its bitmap", p->pic);
    }
    thumb_free(t);
}

static void radio_page_gone(struct Page *p);
static void style_preview_free(void);
static BOOL has_aside(struct Page *p);
static WORD aside_x(void);
static void radio_stop(void);

static void plex_pos_save(const struct Page *p);

static void page_pop(void)
{
    if (depth) {
        radio_page_gone(&pages[depth - 1]);     /* its request, if out, given up */
        plex_pos_save(&pages[depth - 1]);       /* a Plex list: where it was left */
    }
    ov.on = FALSE;
    tb_close(&pv_tb);
    pv_shown = FALSE;
    pv_tj = pv_sj = NULL;
    if (focus == FOCUS_PREVIEW)
        focus = FOCUS_CONTENT;
    if (!depth)
        return;
    if (pages[depth - 1].style_pv)
        style_preview_free();
    if (pages[depth - 1].pic_bm && pages[depth - 1].pic_y >= 0) {
        struct Page *p = &pages[depth - 1];
        WaitBlit();                             /* the logo gone while its pens are its own: */
        gfx_clear_beam(&g, p->pic_x, p->pic_y, p->pic_x + p->pic_w - 1,
                       p->pic_y + p->pic_h - 1);    /* left, it would show in the next
                                                     * page's; all its planes together,
                                                     * behind the beam, not garbled */
        p->pic_y = -1;
    }
    /* the preview's picture stays for the page under, which may show it
     * again (its folder's); draw_pane or the preview clears it if not */
    if (!(depth >= 2 && g.pic_w && !pages[depth - 1].pic_bm && has_aside(&pages[depth - 2]) &&
          g.pic_x >= aside_x()))
        gfx_picture_clear(&g, 0, 0, g.w - 1, g.h - 1);
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

static void set_msg(const char *m);
static WORD list_visible(void);
static void draw_list_rows(struct Page *p);
static void draw_title(struct Page *p);
static void draw_title_count(struct Page *p);
static void preview_touch(struct Page *p);
static BOOL pv_roomy(void);
static LONG pv_dir;

/* ---- the spinner: eight round dots on a circle, the bright one going
 * round with a fading tail, and "Loading..." under it. In the details pane
 * while its picture comes, and over the content pane for any navigation
 * that keeps you waiting (busy_*), after SPIN_AFTER ms. */
#define SPIN_AFTER 200
#define SPIN_FRAME  80              /* ms a step: a turn in 0.64 s */

static BOOL has_aside(struct Page *p);
static WORD aside_x(void);
static void draw_pane(BOOL full);

/* The spinner's size around its centre: dots ring (rx, ry) and dot radius. */
static void spin_metrics(WORD *ring, WORD *ringy, WORD *r, WORD *ry)
{
    WORD sy = gfx_mode_tall(g.mode) ? 2 : 1;    /* tall pixels: half as many down */
    *r = U(3);
    *ring = U(12);
    if (*r < 2)
        *r = 2;
    if (sy == 2 && *r < 4) {
        *r = 4;                                 /* rows enough to look round, */
        *ring = *ring * 4 / 3;                  /* and room for them on the ring */
    }
    *ry = (*r + sy - 1) / sy;
    *ringy = *ring / sy;
}

/* Each step is drawn off screen, then copied up in one go with the beam
 * (gfx_blit_beam): cleared and drawn dot by dot on the screen, it was
 * seen half done, a flicker. */
static struct BitMap *spin_bm;
static WORD spin_bw, spin_bh;

static void spin_free(void)
{
    if (spin_bm) {
        WaitBlit();
        FreeBitMap(spin_bm);
        spin_bm = NULL;
    }
}

/* The step the spinner is at, since: one every SPIN_FRAME ms, so it
 * turns evenly however often it is asked. */
static UWORD spin_phase(ULONG since)
{
    return (UWORD)(((time_ms() - since) / SPIN_FRAME) & 7);
}

/* The spinner centred on (cx, cy) at a step; first: with its label (the
 * caller has cleared the area). */
static void spin_draw(WORD cx, WORD cy, UWORD phase, BOOL first)
{
    static const UBYTE pens[8] = { PEN_WHITE, PEN_TEXT, PEN_DIM, PEN_DIM,
                                   PEN_RULE, PEN_RULE, PEN_RULE, PEN_RULE };
    /* the dots' places on a circle of radius 12 design pixels */
    static const BYTE dx[8] = { 0, 8, 12, 8, 0, -8, -12, -8 };
    static const BYTE dy[8] = { -12, -8, 0, 8, 12, 8, 0, -8 };
    WORD ring, ringy, r, ry, i, sy = gfx_mode_tall(g.mode) ? 2 : 1;
    spin_metrics(&ring, &ringy, &r, &ry);
    if (first) {
        WORD tw = gfx_textlen(&g, "Loading...", TS_SMALL);
        gfx_text(&g, cx - tw / 2, cy + ringy + ry + U(6), "Loading...", PEN_DIM, PEN_BG, 0, TS_SMALL);
    }
    {
        WORD x0 = cx - ring - r, y0 = cy - ringy - ry;
        WORD w = 2 * (ring + r) + 1, h = 2 * (ringy + ry) + 1;
        struct RastPort orp, *screen_rp = g.rp;
        if (!spin_bm || spin_bw != w || spin_bh != h) {
            spin_free();
            spin_bm = AllocBitMap(w, h, g.depth, BMF_CLEAR, g.rp->BitMap);
            spin_bw = w;
            spin_bh = h;
        }
        if (spin_bm) {
            InitRastPort(&orp);
            orp.BitMap = spin_bm;
            g.rp = &orp;                    /* drawn off screen */
            cx -= x0;
            cy -= y0;
        }
        gfx_fill(&g, cx - ring - r, cy - ringy - ry, cx + ring + r, cy + ringy + ry, PEN_BG);
        for (i = 0; i < 8; i++)
            gfx_dot(&g, cx + U(dx[i]) * ring / U(12), cy + U(dy[i]) * ring / U(12) / sy, r,
                    pens[(phase - i) & 7], PEN_BG);   /* the tail behind */
        g.rp = screen_rp;
        if (spin_bm)
            gfx_blit_beam(&g, spin_bm, x0, y0, w, h);
    }
}

/* A Radio list waiting for radio-browser.info: the spinner turns in the
 * middle of its (empty) pane until the answer comes; Back gives it up.
 * rsp.on: drawn since the pane was (draw_pane starts it anew). */
static struct { BOOL on; ULONG since; UWORD phase; WORD cx, cy; } rsp;

static BOOL radio_waiting(void)
{
    struct Page *p = cur();
    return p && p->type == PT_LIST && p->radio && p->net_seq && !pane_pending &&
           focus != FOCUS_MENU;
}

static void radio_spin(void)
{
    if (!radio_waiting() || !g.win) {
        rsp.on = FALSE;
        return;
    }
    if (!rsp.on) {
        WORD ring, ringy, r, ry;
        spin_metrics(&ring, &ringy, &r, &ry);
        rsp.cx = g.pane_x + g.pane_w / 2;
        if (has_aside(cur()))               /* the list's half: never over the pane's picture */
            rsp.cx = g.pane_x + (aside_x() - g.pane_x) / 2;
        rsp.cy = (g.list_top + g.footer_y - 2) / 2 - U(8);
        rsp.on = TRUE;
        rsp.since = time_ms();
        rsp.phase = 0;
        spin_draw(rsp.cx, rsp.cy, 0, TRUE);    /* the pane under it is clear */
        return;
    }
    if (spin_phase(rsp.since) != rsp.phase) {
        rsp.phase = spin_phase(rsp.since);
        spin_draw(rsp.cx, rsp.cy, rsp.phase, FALSE);
    }
}

/* Navigation that may keep you waiting: busy_begin ... busy_end around it,
 * waits inside it through ui_wait. After SPIN_AFTER ms the spinner turns
 * in the middle of the content pane; at the end the pane is drawn again
 * where it was (redraw), unless what comes next covers the screen. */
static struct { int depth; BOOL on; ULONG since, last; UWORD phase; WORD cx, cy; } busy;
static BOOL ui_aborted;             /* a wait in busy gave way to input */
static ULONG ui_joy0;               /* the joystick when the busy began */

static void busy_begin(void)
{
    if (busy.depth++ == 0) {
        busy.on = FALSE;
        busy.since = time_ms();
        ui_aborted = FALSE;
        ui_joy0 = LowLevelBase ? ReadJoyPort(1) : 0;
    }
}

static void busy_tick(void)
{
    struct Page *p = cur();
    ULONG now = time_ms();
    if (!busy.depth || !g.win)
        return;
    if (!busy.on) {
        WORD ring, ringy, r, ry, x1;
        if ((LONG)(now - busy.since) < SPIN_AFTER)
            return;
        spin_metrics(&ring, &ringy, &r, &ry);
        x1 = p && has_aside(p) ? aside_x() - U(6) : g.pane_x + g.pane_w;
        busy.cx = (g.pane_x + x1) / 2;
        busy.cy = (g.list_top + g.footer_y - 2) / 2 - U(8);
        {
            WORD tw = gfx_textlen(&g, "Loading...", TS_SMALL), hw = ring + r + U(4);
            if (tw / 2 + U(4) > hw)
                hw = tw / 2 + U(4);
            gfx_fill(&g, busy.cx - hw, busy.cy - ringy - ry - U(4), busy.cx + hw,
                     busy.cy + ringy + ry + U(8) + g.font_h, PEN_BG);
        }
        busy.on = TRUE;
        busy.phase = 0;
        busy.last = now;
        spin_draw(busy.cx, busy.cy, 0, TRUE);
        return;
    }
    if (spin_phase(busy.last) != busy.phase) {
        busy.phase = spin_phase(busy.last);
        spin_draw(busy.cx, busy.cy, busy.phase, FALSE);
    }
}

/* The Plex details on screen under the cover (their entry's name and
 * cover), or "" when that part of the pane was cleared. */
static char plex_shown[260];

static void busy_end(BOOL redraw)
{
    if (busy.depth && --busy.depth == 0 && busy.on) {
        busy.on = FALSE;
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
                         im->Class == IDCMP_VANILLAKEY ||
                         (im->Class == IDCMP_MOUSEBUTTONS &&
                          (im->Code == SELECTDOWN || im->Code == MENUDOWN)));
            Permit();
            if (!im)
                break;
            if (nav)
                return TRUE;
            if ((im = (struct IntuiMessage *)GetMsg(mp)))
                ReplyMsg((struct Message *)im);
        }
    if (LowLevelBase) {
        ULONG st = ReadJoyPort(1), mask = JPF_JOY_UP | JPF_JOY_DOWN | JPF_JOY_LEFT | JPF_JOY_RIGHT |
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
    struct DateStamp ds;
    BOOL dated = idx_date(dir, &ds);
    struct LJob *j;
    LONG n;
    int k, slot = 0;
    char m[80];
    for (k = 0; k < PICS_KEEP; k++)
        if (pkeep[k].list.pool && dated && !amce_stricmp(pkeep[k].path, dir) &&
            !CompareDates(&ds, &pkeep[k].date)) {
            list_free(out);
            list_init(out);
            pics_copy(&pkeep[k].list, out);
            pkeep[k].stamp = ++pkeep_clock;
            return out->count;
        }
    if (pics_job && strcmp(pics_job->path, dir) != 0) {
        ldr_orphan(pics_job);               /* another folder's: not wanted now */
        pics_job = NULL;
    }
    if (!pics_job) {
        if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
            return pics_read_dir(dir, out);
        j->type = LJ_PICDIR;
        amce_strlcpy(j->path, dir, sizeof(j->path));
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
    amce_log("pictures read", m);
    if (n >= 0 && j->list.pool) {
        list_free(out);
        list_init(out);
        pics_copy(&j->list, out);
        if (dated && pv_roomy()) {          /* kept for the next visit */
            for (k = 1; k < PICS_KEEP; k++)
                if (!pkeep[k].list.pool ||
                    (pkeep[slot].list.pool && pkeep[k].stamp < pkeep[slot].stamp))
                    slot = k;
            if (pkeep[slot].list.pool)
                list_free(&pkeep[slot].list);
            amce_strlcpy(pkeep[slot].path, dir, sizeof(pkeep[slot].path));
            pkeep[slot].date = ds;
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
static BOOL ch_reading(const struct Page *p)
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
    if (!ch || ch->frozen || pv_roomy())
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
            if (!(a->flags & ITEM_GONE) && amce_stricmp(a->sortkey, b->sortkey) > 0)
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
    amce_strlcpy(j->path, ch->c[k].path, sizeof(j->path));
    if (ch->c[k].cached) {                  /* from the index file: one read */
        amce_strlcpy(j->cache, ch->idx, sizeof(j->cache));
        j->off = ch->c[k].coff;
        j->len = ch->c[k].clen;
    }
    ch->job = j;
    ch->job_k = k;
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
    roomy = pv_roomy();
    for (k = 0; k < ch->n; k++) {
        struct Chunk *c = &ch->c[k];
        if (c->first >= 0 && c->count && !c->held && c->first < p->top + vis &&
            c->first + c->count > p->top) {
            ch_send(p, k);
            return;
        }
    }
    if ((at = ch_of(ch, p->sel)) >= 0) {
        LONG d = pv_dir >= 0 ? 1 : -1, s;
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
            char m[80];
            ch_trim(p);
            if (AvailMem(MEMF_ANY) < CH_FLOOR) {
                ch->cut = TRUE;
                sprintf(m, "Low on memory: list stops at %ld entries", (long)p->list.count);
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
    amce_strlcpy(j->path, ch->idx, sizeof(j->path));
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
    amce_log("index cache", j->ran && j->np > 0 ? "saved" : "could not be saved");
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
        amce_log("list read", m);
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
        if (p->sel >= c->first && p->sel < c->first + c->count)
            preview_touch(p);
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
static void ch_wait_item(struct Page *p, LONG idx, BOOL shown)
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
        if (amce_stricmp(ix.l[k].name, (const char *)FilePart((STRPTR)ch->c[k].path)) != 0) {
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
    amce_log("list from index", m);
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
static WORD textbox_w(void)
{
    return g.pane_w - U(9);
}

/* Word-wrap a paragraph into l, maxw pixels wide. */
static void wrap_into(struct ItemList *l, const char *s, WORD maxw)
{
    char line[160], word[96];
    line[0] = 0;
    while (*s) {
        LONG n = 0;
        char tryline[256];
        while (*s == ' ')
            s++;
        while (*s && *s != ' ' && n < (LONG)sizeof(word) - 1)
            word[n++] = *s++;
        word[n] = 0;
        if (!n)
            break;
        amce_strlcpy(tryline, line, sizeof(tryline));
        if (tryline[0])
            amce_strlcat(tryline, " ", sizeof(tryline));
        amce_strlcat(tryline, word, sizeof(tryline));
        if (line[0] && gfx_textlen(&g, tryline, TS_BODY) > maxw) {
            list_add(l, line, "", "", IT_TEXT);
            amce_strlcpy(line, word, sizeof(line));
        } else {
            amce_strlcpy(line, tryline, sizeof(line));
        }
    }
    if (line[0])
        list_add(l, line, "", "", IT_TEXT);
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
    amce_strlcpy(tmp, s, n + 1);
    if (amce_stricmp(tmp, pre) != 0)
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
        if (amce_stricmp(label, skip[i]) == 0)
            return -1;
    amce_strlcpy(out, label, max);
    if (starts_nocase(label, "Games - ", &rest) || starts_nocase(label, "Demos - ", &rest)) {
        int mod = (label[0] == 'G' || label[0] == 'g') ? MOD_GAMES : MOD_DEMOS;
        if (amce_stricmp(rest, "0-Z") == 0) {
            /* one list of every title (ITEM_FLAT), not a folder per letter */
            amce_strlcpy(out, mod == MOD_GAMES ? "All Games" : "All Demos", max);
            *prio = 1;
        } else {
            amce_strlcpy(out, rest, max);
            *prio = 2;
        }
        return mod;
    }
    if (amce_stricmp(label, "Disk Magazines") == 0) {
        *prio = 2;
        return MOD_DEMOS;
    }
    if (amce_stricmp(label, "Favourites") == 0) {
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

static void no_ags_text(struct Page *p)
{
    text_line(p, "Amiga Game Selector was not found.");
    text_line(p, "");
    text_para(p, "AmigaMCE looks for the AGS: assign, then for an AGS2 drawer next to its own "
                 "drawer and in WHDLoad:. Install AmigaMCE on an AGS 3 system, or start AGS once "
                 "so its assigns exist.");
}

static void found_line(struct Page *p, const char *what, const char *path)
{
    char line[160];
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR l;
    me->pr_WindowPtr = (APTR)-1;
    l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = old;
    if (l)
        UnLock(l);
    sprintf(line, "%s: %s (%s)", what, path, l ? "found" : "not found");
    text_line(p, line);
}

static void dt_line(void *ctx, const char *s)
{
    text_line((struct Page *)ctx, s);
}

static void build_sysinfo(struct Page *p)
{
    char line[160];
    text_line(p, "Found when AmigaMCE started:");
    text_line(p, "");
    sprintf(line, "Machine type: %s", caps.emulator ? "emulator (UAE family)" : "real Amiga");
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
    sprintf(line, "Chipset: %s%s, %s", caps.aga ? "AGA" : caps.ecs ? "ECS" : "OCS",
            caps.ehb ? " with EHB" : "", caps.pal ? "PAL" : "NTSC");
    text_line(p, line);
    {
        /* Read the mode back from the open screen, not from what we asked for. */
        struct NameInfo ni;
        char mode[DISPLAYNAMELEN + 1];
        ULONG id = GetVPModeID(&g.scr->ViewPort);
        amce_strlcpy(mode, "unnamed mode", sizeof(mode));
        if (id != (ULONG)INVALID_ID &&
            GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, id) > 0)
            amce_strlcpy(mode, (char *)ni.Name, sizeof(mode));
        sprintf(line, "Screen: %s", mode);
        text_line(p, line);
        {
            /* Bits per pixel from the bitmap itself: an RTG bitmap's Depth
             * field is not reliable, GetBitMapAttr is. */
            ULONG d = GetBitMapAttr(g.scr->RastPort.BitMap, BMA_DEPTH);
            char colours[24];
            if (d > 8)
                sprintf(colours, "%lu-bit colour", (unsigned long)d);
            else
                sprintf(colours, "%lu colours", (unsigned long)(1UL << d));
            sprintf(line, "  %lu x %lu, %s, mode ID $%08lx",
                    (unsigned long)g.scr->Width, (unsigned long)g.scr->Height, colours,
                    (unsigned long)id);
        }
        text_line(p, line);
        if (g.style == STYLE_SMOOTH)
            sprintf(line, "Style: smooth, %lu.%02lux", (unsigned long)(g.k >> 8),
                    (unsigned long)(((g.k & 255) * 100 + 128) >> 8));
        else
            sprintf(line, "Style: pixel, %lux%s", (unsigned long)g.kint,
                    gfx_mode_tall(g.mode) ? " (tall pixels: dense)" : "");
        text_line(p, line);
        if (g.sm && !g.truecolor) {
            sprintf(line, "  its shades and logo: %lu pens so far",
                    (unsigned long)sm_pens_used(g.sm));
            text_line(p, line);
        }
    }
    sprintf(line, "Graphics card (RTG): %s", caps.rtg ? "yes" : "no");
    text_line(p, line);
    sprintf(line, "Free memory: %lu KB chip, %lu KB fast",
            (unsigned long)(caps.chipfree >> 10), (unsigned long)(caps.fastfree >> 10));
    text_line(p, line);
    sprintf(line, "Font: %s", g.font_is_ours ? "amce-pixel (bundled)" : "topaz 8 (fallback)");
    text_line(p, line);
    text_line(p, "");
    text_line(p, "Datatypes (which one reads each picture format):");
    pic_datatypes(dt_line, p);
    text_line(p, "");
    sprintf(line, "AGS: %s", ags.found ? "found" : "not found");
    text_line(p, line);
    if (ags.added[0]) {
        /* S:AGS-Stuff did not run at boot; AmigaMCE filled the gaps. */
        text_line(p, "Assigns AmigaMCE added (AGS-Stuff did not run):");
        text_para(p, ags.added);
    }
    sprintf(line, "AGS machine setting (HW): %s", ags.hw[0] ? ags.hw : "not set");
    text_line(p, line);
    sprintf(line, "Adult entries: %s", ags.adult[0] == '1' ? "shown" : "hidden");
    text_line(p, line);
    sprintf(line, "Network stack: %s", caps.net ? "running" : "none");
    text_line(p, line);
    sprintf(line, "MP3: %s", caps.mp3_mhi ? "MHI driver" : caps.mp3_mpega ? "mpega.library" : "none");
    text_line(p, line);
    text_line(p, "");
    sprintf(line, "Debug log: %s", amce_log_path() ? amce_log_path() : "off");
    text_line(p, line);
}

/* ------------------------------------------------------------ settings */
#define MODE_VAR "AmigaMCE/ScreenMode"

static void ensure_dir(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!l)
        l = CreateDir((CONST_STRPTR)path);
    if (l)
        UnLock(l);
}

/* Remembered in ENV: and ENVARC: (AmigaMCE/ScreenMode), like AGS's own
 * settings. Delete ENVARC:AmigaMCE/ScreenMode to go back to Automatic. */
static void load_screen_mode(void)
{
    char buf[32];
    int m;
    if (GetVar((CONST_STRPTR)MODE_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
        (m = gfx_mode_from_key(buf)) >= 0)
        screen_mode = m;
}

static void save_setting(const char *var, const char *value)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    ensure_dir("ENV:AmigaMCE");
    ensure_dir("ENVARC:AmigaMCE");
    SetVar((CONST_STRPTR)var, (CONST_STRPTR)value, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    me->pr_WindowPtr = oldwin;
}

static void save_screen_mode(void)
{
    save_setting(MODE_VAR, gfx_mode_key(screen_mode));
}

#define DEBUG_VAR "AmigaMCE/Debug"
#define STYLE_VAR "AmigaMCE/Style"
#define LFONT_VAR "AmigaMCE/LacedFont"
#define JUMP_VAR  "AmigaMCE/JumpScroll"
#define DBUF_VAR  "AmigaMCE/DoubleBuffer"
#define HTTPS_VAR "AmigaMCE/RadioHTTPS"
#define NETIF_VAR "AmigaMCE/NetInterface"

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
        amce_log("network interface", m);
    }
}

static ULONG strtoul_(const char *s);

/* ---- Settings > Networking > Radio, and the stream buffer */
#define RKBPS_VAR "AmigaMCE/RadioMaxKbps"   /* "" automatic, a number, "any" */
#define RSRV_VAR  "AmigaMCE/RadioServer"    /* "" automatic, "fastest:<name>", "<name>" */
#define BUF_VAR   "AmigaMCE/StreamBuffer"   /* short, normal, long */

static const UWORD rkbps_choice[6] = { 0xFFFF, 64, 96, 128, 192, 0 }; /* automatic .. any */
static int rkbps_sel;               /* 0 automatic */
static int buf_sel = 1;             /* normal */
static char rsrv[80];               /* as kept */
static struct NetMirror mirrors[8];
static int nmirror = -2;            /* -2 not asked yet, -1 could not be */
static const char *const buf_names[3] = { "Short", "Normal", "Long" };
static const char *const buf_notes[3] = { "starts in 0.6 s", "1.2 s",
                                          "3 s, more held against dropouts" };

/* Automatic: by how fast this Amiga is (plex_tier). Paula plays no more
 * than 28 kHz, and the decoder is told to make what it can play, so a
 * 68030 copes with 128k stations; above that is work for nothing. */
static UWORD rkbps_auto(void)
{
    int t = plex_tier();
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
    amce_strlcpy(out, name, max);
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
            amce_strlcpy(iso, t[i].iso, sizeof(iso));
    return iso;
}

static void load_radio_settings(void)
{
    char buf[80], m[160];
    int i;
    rkbps_sel = 0;
    if (GetVar((CONST_STRPTR)RKBPS_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        if (!amce_stricmp(buf, "any"))
            rkbps_sel = 5;
        for (i = 1; i < 5; i++)
            if ((UWORD)strtoul_(buf) == rkbps_choice[i])
                rkbps_sel = i;
    }
    buf_sel = 1;
    if (GetVar((CONST_STRPTR)BUF_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        for (i = 0; i < 3; i++)
            if (!amce_stricmp(buf, buf_names[i]))
                buf_sel = i;
    st_set_buffer(buf_sel);
    rsrv[0] = 0;
    /* the bitrate is set as each list is asked for (radio_list_kbps): its
     * Automatic times this Amiga (plex_tier), which needs the timer, not
     * open yet here */
    if (GetVar((CONST_STRPTR)RSRV_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        amce_strlcpy(rsrv, buf, sizeof(rsrv));
    rsrv_apply();
    radio_set_home(home_country());
    if (rkbps_sel)
        rkbps_text(rkbps_choice[rkbps_sel], buf);
    else
        strcpy(buf, "automatic");
    sprintf(m, "stations up to %.9s, buffer %s, list server %.60s, country %s", buf,
            buf_names[buf_sel], rsrv[0] ? rsrv : "automatic",
            home_country()[0] ? home_country() : "not known");
    amce_log("radio", m);
}

/* ---- Music's own files: the folders they are in (Settings > Sound >
 * Music folders), and the MUSIC: assign, used when it is there unless
 * turned off */
#define MUSICDIRS_VAR   "AmigaMCE/MusicFolders"     /* one a line */
#define MUSICASSIGN_VAR "AmigaMCE/MusicAssign"      /* "off": MUSIC: not used */
#define MUSIC_DIRS 8
static char music_dirs[MUSIC_DIRS][160];
static int nmusic_dirs;
static BOOL music_assign = TRUE;

static void load_music_dirs(void)
{
    static char buf[MUSIC_DIRS * 162];
    char *p, *e;
    nmusic_dirs = 0;
    music_assign = !(GetVar((CONST_STRPTR)MUSICASSIGN_VAR, (STRPTR)buf, 8, GVF_GLOBAL_ONLY) > 0 &&
                     !amce_stricmp(buf, "off"));
    if (GetVar((CONST_STRPTR)MUSICDIRS_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) <= 0)
        return;
    for (p = buf; *p && nmusic_dirs < MUSIC_DIRS; p = e) {
        LONG n;
        e = strchr(p, '\n');
        n = e ? e - p : (LONG)strlen(p);
        if (n > 0 && n < (LONG)sizeof(music_dirs[0])) {
            memcpy(music_dirs[nmusic_dirs], p, n);
            music_dirs[nmusic_dirs++][n] = 0;
        }
        e = e ? e + 1 : p + n;
    }
}

static void save_music_dirs(void)
{
    static char buf[MUSIC_DIRS * 162];
    int i;
    buf[0] = 0;
    for (i = 0; i < nmusic_dirs; i++) {
        if (i)
            amce_strlcat(buf, "\n", sizeof(buf));
        amce_strlcat(buf, music_dirs[i], sizeof(buf));
    }
    save_setting(MUSICDIRS_VAR, buf);
}

/* MUSIC: is assigned (no requester asking for it if not). */
static BOOL music_assign_there(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR l;
    me->pr_WindowPtr = (APTR)-1;
    l = Lock((CONST_STRPTR)"MUSIC:", SHARED_LOCK);
    me->pr_WindowPtr = old;
    if (!l)
        return FALSE;
    UnLock(l);
    return TRUE;
}

static void load_net_if(void)
{
    char buf[20];
    net_if[0] = 0;
    if (GetVar((CONST_STRPTR)NETIF_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        amce_strlcpy(net_if, buf, sizeof(net_if));
    if (net_if[0] && caps.net)
        net_if_apply();
}

/* Settings > Allow HTTPS streams: Radio lists https: stations too (off by
 * default; only with AmiSSL installed, which plays them). */
static BOOL radio_https;

/* Settings > Jump scrolling: held Up/Down in a very long list goes a
 * letter at a time once past cruising speed (off: it glides on). */
static BOOL jump_scroll;

static void set_msg(const char *m);

/* Smooth chosen, the screen could show it, but its file would not load
 * (missing, or from another AmigaMCE version): say so, rather than
 * quietly drawing the pixel style. TRUE when it said something. */
static BOOL note_style(void)
{
    if (ui_style == STYLE_SMOOTH && g.style != STYLE_SMOOTH &&
        gfx_style_possible(&caps, g.mode, STYLE_SMOOTH)) {
        set_msg("Smooth: Fonts/amce-smooth does not match this AmigaMCE");
        return TRUE;
    }
    return FALSE;
}

static void load_style(void)
{
    char buf[32];
    int i;
    if (GetVar((CONST_STRPTR)STYLE_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        ui_style = amce_stricmp(buf, "smooth") == 0 ? STYLE_SMOOTH : STYLE_PIXEL;
    if (GetVar((CONST_STRPTR)LFONT_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        for (i = 0; i < LFONT_COUNT; i++)
            if (!amce_stricmp(buf, gfx_laced_fonts[i].name))
                gfx_laced_font = i;
}

static void load_jump(void)
{
    char buf[8];
    jump_scroll = GetVar((CONST_STRPTR)JUMP_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
                  buf[0] == '1';
}

/* Settings > Double buffering (RTG screens): on, by the setting; without
 * one, the old way (PROGDIR:AmigaMCE.dbuf there). */
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
        l = Lock((CONST_STRPTR)"PROGDIR:AmigaMCE.dbuf", SHARED_LOCK);
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
}

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

enum { SETP_ROOT = 1, SETP_GENERAL, SETP_SCREEN, SETP_SOUND, SETP_NET, SETP_PLEX, SETP_NIC,
       SETP_RADIO, SETP_RKBPS, SETP_BUF, SETP_RSRV, SETP_MUSIC };
static void build_radio_settings(struct Page *p);
static void build_rkbps_settings(struct Page *p);
static void build_buf_settings(struct Page *p);
static void build_rsrv_settings(struct Page *p);
static void build_music_settings(struct Page *p);
static void build_general_settings(struct Page *p);
static void build_net_settings(struct Page *p);
static void build_nic_settings(struct Page *p);

/* Settings: General, Screen, Sound, Networking (Plex inside it, as it
 * needs the network). */
static void build_settings(struct Page *p)
{
    p->setp = SETP_ROOT;
    action_item(p, "General", "", ACT_GENSET, MARK_ARROW);
    action_item(p, "Screen", gfx_mode_label(g.mode), ACT_SCREENSET, MARK_ARROW);
    action_item(p, "Sound", caps.ahi ? "Paula (AHI later)" : "Paula", ACT_SOUNDSET, MARK_ARROW);
    action_item(p, "Networking", !caps.net ? "no TCP/IP stack" : net_if[0] ? net_if : "",
                ACT_NETSET, MARK_ARROW);
}

/* Settings > Screen: the mode, the style (its font on a laced screen), and
 * double buffering where it is offered. */
static void build_screen_settings(struct Page *p)
{
    char tag[64];
    struct Item *it;
    p->setp = SETP_SCREEN;
    amce_strlcpy(tag, gfx_mode_label(g.mode), sizeof(tag));
    action_item(p, "Screen mode", tag, ACT_SCREENMODES, MARK_ARROW);
    /* Style: the font on a laced screen; greyed out, showing Pixel, where
     * this screen can only be pixel (other native modes, 8-bit RTG) */
    if (gfx_laced(g.mode)) {
        action_item(p, "Style", gfx_laced_fonts[gfx_laced_font].name, ACT_STYLES, MARK_ARROW);
    } else if (gfx_style_possible(&caps, g.mode, STYLE_SMOOTH)) {
        action_item(p, "Style", ui_style == STYLE_SMOOTH ? "Smooth" : "Pixel", ACT_STYLES,
                    MARK_ARROW);
    } else if ((it = action_item(p, "Style", "Pixel", ACT_STYLES, MARK_ARROW))) {
        it->flags |= ITEM_DISABLED;
    }
    if (!g.planar)                      /* native screens draw straight on: not offered */
        action_item(p, "Double buffering", "", ACT_DBUF,
                    gfx_dbuf_wanted ? MARK_CHECK_ON : MARK_CHECK_OFF);
}

/* Settings > General. */
static void build_general_settings(struct Page *p)
{
    p->setp = SETP_GENERAL;
    action_item(p, "Jump scrolling (by letter)", "", ACT_JUMP,
                jump_scroll ? MARK_CHECK_ON : MARK_CHECK_OFF);
    action_item(p, "Debug (CPU, memory, log)", "", ACT_DEBUG,
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
    char tag[64], use[40];
    p->setp = SETP_NET;
    (void)it;
    if (!caps.net) {
        list_add(&p->list, "No TCP/IP stack running", "", "", IT_TEXT);
        return;
    }
    rkbps_text(rkbps_now(), use);
    sprintf(tag, "stations up to %s", use);
    action_item(p, "Radio", tag, ACT_RADIOSET, MARK_ARROW);
    action_item(p, "Plex", !plex.on ? "Off" : plex.name[0] ? plex.name
                           : plex.host[0] ? plex.host : "On, no server chosen",
                ACT_PLEXSET, MARK_ARROW);
    action_item(p, "Stream buffer", buf_names[buf_sel], ACT_BUFSET, MARK_ARROW);
    nic_in_use_text(use);
    if (net_if[0])
        sprintf(tag, "%.40s", use[0] ? use : net_if);
    else
        sprintf(tag, use[0] ? "Automatic: %.40s" : "Automatic", use);
    action_item(p, "Network interface", tag, ACT_NICSET, MARK_ARROW);
}

/* Settings > Networking > Network interface: Automatic (the stack's
 * choice, the one it uses beside it) or one the stack lists. */
static void build_nic_settings(struct Page *p)
{
    struct Item *it;
    char label[64], tag[64], ip[20], use[40];
    int i;
    p->setp = SETP_NIC;
    nic_in_use_text(use);
    action_item(p, "Automatic (the stack's choice)", net_if[0] ? "" : use, ACT_NICAUTO,
                net_if[0] ? MARK_RADIO_OFF : MARK_RADIO_ON);
    for (i = 0; i < nnic && i < 8; i++) {
        struct NetIf *f = &nics[i];
        ip_text(f->ip, ip);
        sprintf(label, "%.19s  %s", f->name, f->ip ? ip : "no address");
        if (f->dev[0])
            sprintf(tag, "%s, %.39s unit %ld", f->up ? "up" : "down", f->dev, (long)f->unit);
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
        list_add(&p->list, "This TCP/IP stack does not list its interfaces", "", "", IT_TEXT);
}

/* Settings > Networking > Radio: HTTPS, the highest bitrate listed, and
 * the server the station lists come from. */
static void build_radio_settings(struct Page *p)
{
    struct Item *it;
    char tag[80], k[16], nm[64];
    p->setp = SETP_RADIO;
    if ((it = action_item(p, "Allow HTTPS streams", caps.amissl ? "" : "needs AmiSSL", ACT_HTTPS,
                          radio_https && caps.amissl ? MARK_CHECK_ON : MARK_CHECK_OFF)) &&
        !caps.amissl)
        it->flags |= ITEM_DISABLED;     /* secure streams need AmiSSL */
    rkbps_text(rkbps_now(), k);
    if (rkbps_sel == 0)
        sprintf(tag, "Automatic: %s", k);
    else
        strcpy(tag, k);
    action_item(p, "Highest station bitrate", tag, ACT_RKBPSSET, MARK_ARROW);
    if (!rsrv[0]) {
        strcpy(tag, "Automatic");
    } else if (!strncmp(rsrv, "fastest:", 8)) {
        mirror_short(rsrv + 8, nm, sizeof(nm));
        sprintf(tag, "Fastest: %s", nm);
    } else {
        mirror_short(rsrv, tag, sizeof(tag));
    }
    action_item(p, "Station list server", tag, ACT_RSRVSET, MARK_ARROW);
}

static void build_rkbps_settings(struct Page *p)
{
    char tag[40], k[16];
    int i;
    p->setp = SETP_RKBPS;
    rkbps_text(rkbps_auto(), k);
    sprintf(tag, "for this Amiga: %s", k);
    action_item(p, "Automatic", tag, ACT_RKBPS0, rkbps_sel == 0 ? MARK_RADIO_ON : MARK_RADIO_OFF);
    for (i = 1; i < 6; i++) {
        char label[24];
        if (rkbps_choice[i])
            sprintf(label, "Up to %lu kbit/s", (unsigned long)rkbps_choice[i]);
        else
            strcpy(label, "Any");
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
        action_item(p, buf_names[i], buf_notes[i], ACT_BUF0 + i,
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
    char tag[64], nm[64];
    int i, f;
    BOOL fast = !strncmp(rsrv, "fastest:", 8);
    p->setp = SETP_RSRV;
    action_item(p, "Automatic", "one at random, as radio-browser.info asks", ACT_RSRVAUTO,
                rsrv[0] ? MARK_RADIO_OFF : MARK_RADIO_ON);
    if (nmirror < 0) {
        action_item(p, "Fastest", "", ACT_RSRVFAST, fast ? MARK_RADIO_ON : MARK_RADIO_OFF);
        list_add(&p->list, "radio-browser.info's servers could not be found", "", "", IT_TEXT);
        return;
    }
    f = mirror_fastest();
    if (f >= 0) {
        mirror_short(mirrors[f].name, nm, sizeof(nm));
        sprintf(tag, "now %.30s, %ld ms", nm, (long)mirrors[f].ms);
    } else {
        strcpy(tag, "none answered");
    }
    action_item(p, "Fastest", tag, ACT_RSRVFAST, fast ? MARK_RADIO_ON : MARK_RADIO_OFF);
    if (!rsrv[0])
        p->sel = 0;
    else if (fast)
        p->sel = 1;
    for (i = 0; i < nmirror; i++) {
        struct Item *it;
        if (mirrors[i].ms >= 0)
            sprintf(tag, "%ld ms", (long)mirrors[i].ms);
        else
            strcpy(tag, "no answer");
        mirror_short(mirrors[i].name, nm, sizeof(nm));
        it = action_item(p, nm, tag, ACT_RSRV0 + i,
                         !fast && !amce_stricmp(rsrv, mirrors[i].name) ? MARK_RADIO_ON
                                                                       : MARK_RADIO_OFF);
        if (it && !fast && !amce_stricmp(rsrv, mirrors[i].name))
            p->sel = p->list.count - 1;
    }
}

#define SETTINGS_SCREEN_ROW 1   /* "Screen" in build_settings */
#define SCREEN_STYLE_ROW 1      /* "Style" in build_screen_settings */
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
    if ((it = list_add(&p->list, "Plex server",
                       "#gdm", plex.host[0] ? (plex.name[0] ? plex.name : plex.host)
                                            : "none chosen", IT_PLEXDIR)))
        it->mark = MARK_ARROW;
    /* the server's conversion: a choice only where the Amiga is fast enough
     * to decode the files as stored (plex_tier: how fast it really is, not
     * only its CPU); on others always on */
    fast = plex_tier() >= 2;
    if ((it = action_item(p, "Request lower quality audio", fast ? "" : "needed on this Amiga",
                          ACT_PLEXCONV, plex.convert || !fast ? MARK_CHECK_ON : MARK_CHECK_OFF)) &&
        !fast)
        it->flags |= ITEM_DISABLED;
}

/* Settings > Sound: what plays the sound and what decodes MP3. Paula and mpega.library are all there is yet;
 * AHI and MHI are shown, greyed, when installed (they come later, and
 * with AHI other rates than Paula's). */
static void build_sound_settings(struct Page *p)
{
    struct Item *it;
    p->setp = SETP_SOUND;
    action_item(p, "Paula (audio.device)", "4 channels, up to 28 kHz", ACT_SND_PAULA,
                MARK_RADIO_ON);
    if ((it = action_item(p, "AHI", caps.ahi ? "installed: comes later" : "not installed",
                          ACT_SND_AHI, MARK_RADIO_OFF)))
        it->flags |= ITEM_DISABLED;
    if ((it = action_item(p, "MP3: mpega.library", caps.mp3_mpega ? "" : "not installed",
                          ACT_SND_MPEGA, caps.mp3_mpega ? MARK_RADIO_ON : MARK_RADIO_OFF)) &&
        !caps.mp3_mpega)
        it->flags |= ITEM_DISABLED;
    if ((it = action_item(p, "MP3: MHI", caps.mp3_mhi ? "installed: comes later" : "not installed",
                          ACT_SND_MHI, MARK_RADIO_OFF)))
        it->flags |= ITEM_DISABLED;
    /* FLAC: Plex's tracks as stored, where the Amiga is fast enough */
    if ((it = action_item(p, "FLAC: flacdec.library",
                          caps.flacdec ? "installed" : "not installed (Aminet: util/libs/flacdec)",
                          ACT_SND_FLAC, caps.flacdec ? MARK_CHECK_ON : MARK_CHECK_OFF)))
        it->flags |= ITEM_DISABLED;
    {
        /* where Music finds the MP3 and FLAC files on this Amiga */
        char tag[48];
        int n = nmusic_dirs + (music_assign && music_assign_there() ? 1 : 0);
        if (n)
            sprintf(tag, "%ld place%s", (long)n, n == 1 ? "" : "s");
        else
            strcpy(tag, "none yet");
        if ((it = action_item(p, "Music folders", tag, ACT_MUSICSET, MARK_NONE)))
            it->mark = MARK_ARROW;
    }
}

/* Settings > Sound > Music folders: the MUSIC: assign (on or off) and the
 * folders added; Fire twice on a folder takes it out. */
static int music_rm = -1;           /* the folder Fire was pressed on once */

static void build_music_settings(struct Page *p)
{
    int i;
    p->setp = SETP_MUSIC;
    action_item(p, "Use the MUSIC: assign", music_assign_there() ? "assigned" : "not assigned now",
                ACT_MUSICASSIGN, music_assign ? MARK_CHECK_ON : MARK_CHECK_OFF);
    for (i = 0; i < nmusic_dirs; i++)
        action_item(p, music_dirs[i], music_rm == i ? "Fire again: remove" : "",
                    ACT_MUSICDIR0 + i, MARK_NONE);
    action_item(p, "Add a folder...", nmusic_dirs < MUSIC_DIRS ? "" : "eight at most",
                ACT_MUSICADD, MARK_NONE);
}

static void draw_footer(void);
static void draw_list_rows(struct Page *p);

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
        case SETP_SOUND: build_sound_settings(q); break;
        case SETP_NET: build_net_settings(q); break;
        case SETP_GENERAL: build_general_settings(q); break;
        case SETP_SCREEN: build_screen_settings(q); break;
        case SETP_NIC: build_nic_settings(q); break;
        case SETP_RADIO: build_radio_settings(q); break;
        case SETP_RKBPS: build_rkbps_settings(q); break;
        case SETP_BUF: build_buf_settings(q); break;
        case SETP_RSRV: build_rsrv_settings(q); break;
        case SETP_MUSIC: build_music_settings(q); break;
        default: build_settings(q); break;
        }
        q->sel = qs < q->list.count ? qs : q->list.count - 1;
    }
}

static void plex_settings_again(BOOL full)
{
    struct Page *p = cur();
    LONG sel = p ? p->sel : 0;
    settings_rebuild();
    if (!p || p->type != PT_LIST)
        return;
    p->sel = sel < p->list.count ? sel : p->list.count - 1;
    if (full) {
        draw_pane(TRUE);
        draw_footer();
    } else {
        draw_list_rows(p);
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
    action_item(p, "Pixel", "", ACT_STYLE_PIXEL,
                ui_style == STYLE_PIXEL ? MARK_RADIO_ON : MARK_RADIO_OFF);
    action_item(p, "Smooth", can_smooth ? "" : "RTG only", ACT_STYLE_SMOOTH,
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
            amce_strlcpy(label, "Automatic: ", sizeof(label));
            amce_strlcat(label, gfx_mode_label(gfx_mode_auto(&caps)), sizeof(label));
        } else {
            amce_strlcpy(label, gfx_mode_label(m), sizeof(label));
            if (gfx_mode_is_rtg(m) && !why) {
                char bits[16];
                sprintf(bits, "  %lu-bit", (unsigned long)gfx_mode_depth(m));
                amce_strlcat(label, bits, sizeof(label));
            }
        }
        action_item(p, label, why ? why : "", ACT_MODE0 + m,
                    m == screen_mode ? MARK_RADIO_ON : MARK_RADIO_OFF);
        if (m == screen_mode)
            p->sel = p->list.count - 1;
    }
}

static void build_about_menu(struct Page *p)
{
    action_item(p, "Credits", "AmigaMCE " AMCE_VERSION, ACT_CREDITS, MARK_ARROW);
    action_item(p, "System information", "", ACT_SYSINFO, MARK_ARROW);
    if (caps.net)
        action_item(p, "Plex in AmigaMCE", "", ACT_PLEXHELP, MARK_ARROW);
}

/* AGS's splash screen, shown under the Credits: read from the AGS install
 * itself, never copied into AmigaMCE. The first of these that exists. */
static const char *const ags_splash[] = { "AGS:Images/logo.iff", NULL };

static void build_credits(struct Page *p)
{
    char line[96];
    int i;
    sprintf(line, "AmigaMCE %s", AMCE_VERSION);
    text_line(p, line);
    text_line(p, "(c) 2026 Brian King, under the MIT licence");
    text_line(p, "");
    text_para(p, "Fonts: Pixelify Sans (c) 2021 The Pixelify Sans Project Authors, Galmuri "
                 "(c) 2019-2025 Lee Minseo, and Rubik (c) 2015 The Rubik Project Authors, all "
                 "under the SIL Open Font License 1.1. The amce-pixel and amce-galmuri fonts are "
                 "Modified Versions of Pixelify Sans and Galmuri.");
    text_line(p, "");
    text_para(p, "Code: libnix C runtime (public domain). The EaglePlayer host follows "
                 "HippoPlayer's (MIT licence, (c) 2017 Kari-Pekka Koljonen).");
    text_line(p, "");
    text_para(p, "Used if installed, not included, each under its own licence: Amiga Game "
                 "Selector, WHDLoad, AHI, MHI drivers, mpega.library, datatypes.");
    text_line(p, "");
    text_para(p, "FLAC: flacdec.library (c) 2026 Renaud Schweingruber, with dr_flac by David "
                 "Reid (public domain or MIT-0), used if installed, not included. It is free to "
                 "use and to pass on in its own unchanged archive (Aminet: util/libs/flacdec).");
    text_line(p, "");
    text_para(p, "Amiga is a trademark of its owner. Games, demos, music and pictures belong "
                 "to their authors. Licence texts are in the Licences drawer.");
    p->note = "This front-end would not be possible without the immense efforts of Paul Vince "
              "and Amiga Game Selector.";
    for (i = 0; ags_splash[i] && !p->pic[0]; i++) {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR old = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        if ((l = Lock((CONST_STRPTR)ags_splash[i], SHARED_LOCK))) {
            UnLock(l);
            amce_strlcpy(p->pic, ags_splash[i], sizeof(p->pic));
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
static BOOL np_local_art;           /* the music file playing has a picture */
/* A track playing from a file on this Amiga (Music folders), not a server. */
static BOOL np_local(void)
{
    return mp_is_track() && mp_path()[0] && amce_strnicmp(mp_path(), "http", 4);
}
static void build_radio_root(struct Page *p);
static void build_music_root(struct Page *p);
static char plex_now[300];      /* the Plex track playing: its file's key (lists mark it so) */
static char plex_now_in[2][48]; /* ...its album's and artist's keys (their rows marked too) */
/* ...and its file as stored, for Now playing: the rate asked of the server
 * when an album's answer did not have it (plex_dj) */
static struct { char codec[16]; ULONG kbps, ch, rate; BOOL conv; } plex_orig;
static struct NetJob *plex_dj;
static struct NetJob *plex_sj;      /* an artist's or album's summary asked for (plex_sum_ask) */
static void plex_sum_came(struct NetJob *j);
static void plex_ask_rate(const char *key);
/* Pictures of the server's (an album's cover): asked for (art_fetch), kept
 * in RAM: (plex_art), then made for their place by the loader */
enum { ART_NP, ART_PV, ART_VW, ART_VWNEXT, ART_KINDS };   /* VW: the viewer's photo, and its next */
static struct NetJob *art_nj[ART_KINDS];
static char art_want[ART_KINDS][120];
static BOOL art_fetch(int kind, const char *thumb, WORD w, WORD h, char *file);
static void art_ready(int kind, const char *file);
static struct LJob *np_aj;          /* Now playing's cover, made for its square */
static BOOL np_art_drawn;
static void np_art_draw(void);
static void np_art_free(void);
static void np_art_start(const struct Item *it);
static void np_art_load(const char *file);
static WORD plex_art_side(void);
static void plex_pv_draw(struct Page *p, BOOL was_shown);
static BOOL plex_text_kept(struct Page *p);
static BOOL plex_keeps(struct Page *p, LONG idx);
static BOOL plex_movable(struct Page *p, LONG idx);
static BOOL plex_take(struct Page *p);
static WORD plex_art_side(void);
static void plex_refetch(struct Page *p, const char *key);
static void np_soft_redraw(void);
static void np_seek_track(ULONG ms, ULONG delay);
static void music_root_again(void);
static void pictures_root_again(void);
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

static void build_root_pages(int mod)
{
    struct Page *p;
    pages_clear();
    switch (mod) {
    case MOD_GAMES:
    case MOD_DEMOS:
        if (ags.found) {
            p = page_push(PT_LIST, mod_names[mod]);
            if (p)
                build_ags_root(p, mod);
        } else if ((p = page_push(PT_TEXT, mod_names[mod])))
            no_ags_text(p);
        break;
    case MOD_MODS:
        build_mods_root();
        break;
    case MOD_VIDEOS:
        if ((p = page_push(PT_TEXT, "Videos"))) {
            text_para(p, "Video playback arrives in a later version.");
            text_line(p, "");
            found_line(p, "Animations", "Media:Animations");
        }
        break;
    case MOD_PICTURES:
        if ((p = page_push(PT_LIST, "Pictures"))) {
            LONG n;
            p->pics = 1;
            n = pics_read_ui(PICS_ROOT, &p->list);
            if (n == -2) {
                page_pop();                 /* given up for input: built when it settles */
                page_push(PT_TEXT, "Pictures");
            } else if (n <= 0 && !(caps.net && plex.on && plex.host[0])) {
                page_pop();
                if ((p = page_push(PT_TEXT, "Pictures")))
                    text_para(p, n < 0 ? "No pictures found: AmigaMCE looks for them in "
                                         PICS_ROOT "." : PICS_ROOT " has no pictures in it.");
            } else {
                if (n <= 0)
                    list_add(&p->list, n < 0 ? "No " PICS_ROOT " here" : PICS_ROOT " is empty",
                             "", "", IT_TEXT);
                pictures_plex_rows(p);      /* the server's photo libraries first */
            }
        }
        break;
    case MOD_MUSIC:
        if ((p = page_push(PT_LIST, "Music")))
            build_music_root(p);
        break;
    case MOD_RADIO:
        if ((p = page_push(PT_LIST, "Radio")))
            build_radio_root(p);
        break;
    case MOD_SETTINGS:
        if ((p = page_push(PT_LIST, "Settings")))
            build_settings(p);
        break;
    case MOD_ABOUT:
        if ((p = page_push(PT_LIST, "About")))
            build_about_menu(p);
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
        amce_strlcpy(t->lv[n].label, p->list.count ? p->list.items[p->sel].label : "", 64);
    }
    t->n = n;
}

static void save_nav(void)
{
    if (shown_mod < 0 || !depth)
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
    if (nav_snap.on || shown_mod < 0 || !depth || f == FOCUS_MENU)
        return;
    nav_snap.on = TRUE;
    nav_snap.mod = shown_mod;
    trail_now(&nav_snap.t);
}

static void nav_moved(void)
{
    nav_snap.on = FALSE;
}

/* Open a folder page without drawing (used while rebuilding the trail). */
static BOOL push_dir_quiet(struct Item *it)
{
    BOOL pics = cur() && cur()->pics, mods = cur() && cur()->mods, songs = cur() && cur()->songs;
    struct Page *p = page_push(PT_LIST, it->label);
    if (!p)
        return FALSE;
    if (songs) {
        p->songs = 1;
        if (songs_read_dir(it->path, &p->list) <= 0) {
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
        p->pics = 1;
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

static WORD list_visible(void);

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

/* The trail kept across runs too: written when AmigaMCE quits (and
 * before a game starts, in case it never comes back), read when it
 * starts. A text file, one line each:
 *   AmigaMCE navigation 1
 *   at <section>\t<menu|list>        where the user was, and in which pane
 *   section <section>\t<levels>      then, for each level of that trail,
 *   <sel>\t<top>\t<label>
 * Sections by name, so a new menu order keeps working. */
#define NAV_FILE "ENVARC:AmigaMCE/Nav"
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
    ensure_dir("ENVARC:AmigaMCE");
    fh = Open((CONST_STRPTR)NAV_FILE, MODE_NEWFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return;
    FPuts(fh, (CONST_STRPTR)"AmigaMCE navigation 1\n");
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
    if (!s || strcmp(s, "AmigaMCE navigation 1") != 0) {
        FreeVec(buf);
        return;                     /* not one this AmigaMCE knows */
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
            amce_strlcpy(nav_mem[m].lv[n].label, strchr(t + 1, '\t') + 1, 64);
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
static WORD list_visible(void)
{
    return (g.footer_y - 2 - g.list_top) / g.list_row_h;
}

static void clock_text(char *buf)
{
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
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
        sprintf(buf, "%s %02ld %s  %02ld:%02ld", wd[wday], (long)days + 1, mn[m],
                (long)ds.ds_Minute / 60, (long)ds.ds_Minute % 60);
    }
}

/* Left edge of the clock's part of the header (room for the widest date). */
static WORD clock_left(void)
{
    return g.w - U(8) - gfx_textlen(&g, "Wed 30 Sep  00:00", TS_SMALL) - U(4);
}

/* While music plays, its dancing bars in the header, left of the clock:
 * a sign from anywhere that something is playing. */
static BOOL hdr_np_on;

static void hdr_np_draw(BOOL quiet)
{
    WORD n = g.mark_px, x = clock_left() - n - U(2), y = (U(24) - n) / 2;
    BOOL on = mp_loaded(), was = FALSE;
    if (!on && !hdr_np_on)
        return;
    if (quiet)
        was = gfx_quiet_begin();
    if (on)
        gfx_mark(&g, MARK_PLAYING, x, y, PEN_TEXT, PEN_BAR);
    else
        gfx_fill(&g, x, y, x + n - 1, y + n - 1, PEN_BAR);
    if (quiet)
        gfx_quiet_end(&g, was, x, y, x + n - 1, y + n - 1);
    hdr_np_on = on;
}

static void draw_clock(void)
{
    char buf[40];
    WORD tw;
    clock_text(buf);
    gfx_fill(&g, clock_left(), 0, g.w - 1, U(24) - 1, PEN_BAR);
    if (!buf[0])
        return;
    tw = gfx_textlen(&g, buf, TS_SMALL);
    gfx_text(&g, g.w - U(8) - tw, (U(24) - g.font_h) / 2, buf, PEN_TEXT, PEN_BAR, 0, TS_SMALL);
}

/* Debug readout between the logo and the clock: the longest form that fits. */
static LONG cpu_load = -1, app_load = -1;
static ULONG loop_wakes;                /* the main loop's turns (Debug's log) */

/* Debug: every 5 s the loads in the log, with where AmigaMCE is and how
 * often its loop woke, so a load that follows a page can be traced. */
static void dbg_load_log(void)
{
    static int n;
    static ULONG wakes0;
    static LONG cpu_sum, app_sum;
    char m[200];
    struct Page *p = cur();
    if (cpu_load >= 0)
        cpu_sum += cpu_load;
    if (app_load >= 0)
        app_sum += app_load;
    if (++n < 5)
        return;
    sprintf(m, "system %ld%%, AmigaMCE %ld%%, %ld wakes/s; %s > %s%s%s", (long)(cpu_sum / n),
            (long)(app_sum / n), (long)((loop_wakes - wakes0) / n), mod_names[menu[menu_sel]],
            p ? p->title : "-", focus == FOCUS_MENU ? " (menu)" : "",
            mp_loaded() ? (mp_is_stream() ? ", radio on" : ", music on") : "");
    amce_log("load", m);
    n = 0;
    cpu_sum = app_sum = 0;
    wakes0 = loop_wakes;
}

/* What the readout shows now, so a second that changes nothing redraws
 * nothing. Cleared when the header is drawn afresh. */
static char status_shown[96];
static ULONG fps_shown, fps_flips0;     /* flips in the last second; the count then */

static void draw_status(void)
{
    WORD x0 = U(6) + g.logo_w + U(10);
    WORD x1 = clock_left() - g.mark_px - U(8);  /* the playing bars' place kept */
    char cpu[16], app[16], fps[16], forms[3][96];
    const char *show = "";
    ULONG chip, used;
    int i;
    if (x1 <= x0)
        return;
    if (!debug_on) {
        if (status_shown[0])
            gfx_fill(&g, x0, 0, x1, U(24) - 1, PEN_BAR);
        status_shown[0] = 0;
        return;
    }
    used = dbg_mem_used(&chip);
    if (cpu_load < 0)
        amce_strlcpy(cpu, "--", sizeof(cpu));
    else
        sprintf(cpu, "%ld%%", (long)cpu_load);
    if (app_load < 0)
        amce_strlcpy(app, "--", sizeof(app));
    else
        sprintf(app, "%ld%%", (long)app_load);
    /* double buffered: the flips of the last second */
    fps[0] = 0;
    if (gfx_dbuf_active())
        sprintf(fps, "%ld FPS  ", (long)fps_shown);
    /* CPU = whole system (idle-task meter); app = AmigaMCE's own share */
    sprintf(forms[0], "%sCPU %s  app %s   RAM %luK  (chip %luK)", fps, cpu, app,
            (unsigned long)(used >> 10), (unsigned long)(chip >> 10));
    sprintf(forms[1], "%sCPU %s  app %s  RAM %luK", fps, cpu, app, (unsigned long)(used >> 10));
    sprintf(forms[2], "%s%s/%s %luK", fps, cpu, app, (unsigned long)(used >> 10));
    for (i = 0; i < 3; i++)
        if (gfx_textlen(&g, forms[i], TS_SMALL) <= x1 - x0 || i == 2) {
            show = forms[i];
            break;
        }
    if (status_shown[0] && strcmp(show, status_shown) == 0)
        return;                             /* unchanged: leave it be */
    gfx_fill(&g, x0, 0, x1, U(24) - 1, PEN_BAR);
    gfx_text(&g, x0, (U(24) - g.font_h) / 2, show, PEN_ACCENT, PEN_BAR, x1 - x0, TS_SMALL);
    amce_strlcpy(status_shown, show, sizeof(status_shown));
}

static void set_debug(BOOL on)
{
    debug_on = on;
    if (on) {
        dbg_cpu_start();
        amce_log_open(TRUE);
    } else {
        dbg_cpu_stop();
        amce_log_close();
    }
    cpu_load = app_load = -1;
}

static void draw_header(void)
{
    gfx_fill(&g, 0, 0, g.w - 1, U(24) - 1, PEN_BAR);
    gfx_fill(&g, 0, U(24), g.w - 1, U(25) - 1, PEN_RULE);
    status_shown[0] = 0;                    /* the readout was just cleared */
    gfx_logo(&g, U(6), (U(24) - g.logo_h) / 2);
    hdr_np_on = FALSE;                      /* the header was just cleared */
    hdr_np_draw(FALSE);
    draw_status();
    draw_clock();
}

static void draw_menu_row(int i)
{
    WORD y = g.top + i * g.row_h;
    BOOL sel = i == menu_sel;
    UBYTE bg = sel ? (focus == FOCUS_MENU ? PEN_HL : PEN_SELDIM) :
               i == hover_menu && mouse_mode ? PEN_RULE : PEN_BG;
    UBYTE fg = PEN_WHITE;       /* the menu stands out like the list headers */
    /* the menu's pens are all below 8 and its planes 3+ are clear, so on
     * native screens only planes 0-2 are written: 3/8 of the work */
    if (g.planar)
        SetWriteMask(g.rp, 0x07);
    gfx_fill(&g, 0, y, g.menu_w - 1, y + g.row_h - 1, bg);
    gfx_icon(&g, menu[i], U(3), y + (g.row_h - g.icon_px) / 2, fg, bg);
    gfx_text(&g, U(23), y + (g.row_h - g.font_h) / 2, mod_names[menu[i]], fg, bg,
             g.menu_w - U(24), TS_BOLD);
    if (g.planar)
        SetWriteMask(g.rp, 0xFF);
}

static void draw_menu(void)
{
    int i;
    gfx_fill(&g, 0, U(25), g.menu_w + U(2), g.footer_y - 2, PEN_BG);
    for (i = 0; i < nmenu; i++)
        draw_menu_row(i);
    gfx_fill(&g, g.menu_w + U(2), U(25), g.menu_w + U(3) - 1, g.footer_y - 2, PEN_RULE);
}

static BOOL has_aside(struct Page *p);

/* A section that is only a note for now (Videos): nothing in it
 * to go to, so the menu keeps the focus. */
static BOOL section_inert(void)
{
    int m = menu_sel >= 0 && menu_sel < nmenu ? menu[menu_sel] : -1;
    return depth <= 1 && m == MOD_VIDEOS;
}

static const char *hint_text(void)
{
    struct Page *p = cur();
    /* the long hints when the widest of them fits */
    BOOL hi = gfx_textlen(&g, "Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back",
                          TS_SMALL) <= g.w - U(12);
    if (focus == FOCUS_MENU && section_inert())
        return hi ? "Up/Down  choose      Esc twice  quit" : "Esc twice quit";
    if (focus == FOCUS_MENU)
        return hi ? "Up/Down  choose      Right/Fire  open      Esc twice  quit"
                  : "Fire open  Esc twice quit";
    if (focus == FOCUS_PREVIEW && p && p->pics)
        return hi ? "Up/Down  scroll      Fire/Return  view      Left/Esc  back"
                  : "Fire view  Esc back";
    if (focus == FOCUS_PREVIEW)
        return hi ? "Up/Down  scroll      Fire/Return  play      Left/Esc  back"
                  : "Fire play  Esc back";
    if (p && p->type == PT_LIST && p->pics && p->list.count) {
        /* Fire plays what can be played (a picture, a folder of them);
         * Right goes inside */
        const struct Item *it = &p->list.items[p->sel];
        if (it->kind == IT_PIC)
            return hi ? "Up/Down  move    Fire/Return  view    Left/Esc  back"
                      : "Fire view  Esc back";
        if (it->mark == MARK_PLAY)
            return hi ? "Up/Down  move    Fire  slideshow    Right  open    Left/Esc  back"
                      : "Fire show  Right open";
        return hi ? "Up/Down  move    Fire/Right  open    Left/Esc  back" : "Fire open  Esc back";
    }
    if (p && p->type == PT_LIST && p->plex && p->list.count &&
        p->list.items[p->sel].kind == IT_PLEXTRACK)
        return hi ? "Up/Down  move    Fire/Return  play    Left/Esc  back" : "Fire play  Esc back";
    if (p && p->type == PT_LIST && p->plex && p->list.count &&
        p->list.items[p->sel].kind == IT_PLEXPHOTO)
        return hi ? "Up/Down  move    Fire/Return  view    Left/Esc  back" : "Fire view  Esc back";
    if (p && p->type == PT_LIST && p->plex && p->list.count &&
        p->list.items[p->sel].kind == IT_PLEXSRV)
        return hi ? "Up/Down  move    Fire  use this server    Left/Esc  back"
                  : "Fire use it  Esc back";
    if (p && p->type == PT_LIST && p->radio && p->list.count &&
        p->list.items[p->sel].kind == IT_STATION)
        return hi ? "Up/Down  move    Fire/Return  play    Right  about it    Left/Esc  back"
                  : "Fire play  Right info";
    if (p && p->type == PT_LIST && p->radio)
        return hi ? "Up/Down  move    Right/Fire  open    Left/Esc  back" : "Fire open  Esc back";
    if (p && p->type == PT_LIST && has_aside(p))
        return hi ? "Up/Down  move    Right  details    Fire/Return  play    Left/Esc  back"
                  : "Fire play  Esc back";
    if (p && p->type == PT_LIST)
        return hi ? "Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back"
                  : "Fire open  Ret play  Esc back";
    if (p && p->type == PT_DETAIL)
        return hi ? "Fire/Return  play      Up/Down  scroll      Left/Esc  back"
                  : "Fire play  Esc back";
    if (p && p->type == PT_PLAYER)
        return hi ? "Left/Right  choose    Fire  press    Up/Down  volume    Space  pause    Esc  back"
                  : "Fire press  Up/Dn vol  Esc back";
    if (p && p->type == PT_LIST && (p->mods || p->songs) && p->list.count &&
        (p->list.items[p->sel].kind == IT_MOD || p->list.items[p->sel].kind == IT_SONG))
        return hi ? "Up/Down  move    Fire/Return  play    Left/Esc  back" : "Fire play  Esc back";
    return hi ? "Up/Down  scroll      Left/Esc  back" : "Up/Down scroll  Esc back";
}

static const char *hint_shown;      /* the hint in the footer now */

static void draw_footer(void)
{
    hint_shown = msg[0] ? NULL : hint_text();
    gfx_fill(&g, 0, g.footer_y - U(1), g.w - 1, g.footer_y - 1, PEN_RULE);
    gfx_fill(&g, 0, g.footer_y, g.w - 1, g.h - 1, PEN_BAR);
    gfx_text(&g, U(6), g.footer_y + (g.h - g.footer_y - g.font_h) / 2, msg[0] ? msg : hint_text(),
             msg[0] ? PEN_ACCENT : PEN_DIM, PEN_BAR, g.w - U(12), TS_SMALL);
}

static void set_msg(const char *m)
{
    amce_strlcpy(msg, m ? m : "", sizeof(msg));
    draw_footer();
}

/* The list's "n/total" at the right of the title row. Only this changes
 * while a list scrolls, so it is redrawn alone: drawn with its background
 * in one pass (JAM2) over the old digits, never cleared first, and only
 * the sliver a shorter number leaves behind is cleared. Clearing the whole
 * title row every step flickered on 8-plane screens. */
static WORD count_left = -1;    /* left edge of the count drawn last */

static void preview_touch(struct Page *p);

static void draw_title(struct Page *p);

/* The title band (U(26) to the rule at U(41)) is never cleared whole
 * before it is drawn: the text goes on opaque, over what was there, and
 * only the rows above and below it and the ends of its line are filled.
 * Drawn again unchanged, nothing on screen changes; a clear and then the
 * text flashed the title on every redraw (a section built after the menu
 * settled drew it twice). Text runs from x0 to x1 (exclusive); the line's
 * ends either side are filled. */
static void title_band_rest(WORD x0, WORD x1, WORD right_from)
{
    WORD ty0 = U(28), ty1 = U(28) + g.font_h - 1, l = g.pane_x - U(2);
    if (ty0 > U(26))
        gfx_fill(&g, l, U(26), g.w - 1, ty0 - 1, PEN_BG);
    if (ty1 + 1 <= U(41) - 1)
        gfx_fill(&g, l, ty1 + 1, g.w - 1, U(41) - 1, PEN_BG);
    if (x0 > l)
        gfx_fill(&g, l, ty0, x0 - 1, ty1, PEN_BG);
    if (x1 < right_from)
        gfx_fill(&g, x1, ty0, right_from - 1, ty1, PEN_BG);
}

static void draw_title_count(struct Page *p)
{
    char count[24];
    preview_touch(p);
    ch_pump(p);                             /* letters around the selection, ahead */
    if (!msg[0] && hint_shown && hint_text() != hint_shown)
        draw_footer();              /* the hints follow the selection */
    if (p->title_wide && !ch_reading(p)) {
        draw_title(p);              /* fully read: the breadcrumb gets its room back */
        return;
    }
    WORD right = g.pane_x + g.pane_w, tw, x;
    if (p->type != PT_LIST || !p->list.count)
        return;
    sprintf(count, ch_reading(p) ? "%ld/%ld..." : "%ld/%ld", (long)p->sel + 1,
            (long)p->list.count);   /* "...": still counting (no index yet) */
    tw = gfx_textlen(&g, count, TS_SMALL);
    x = right - tw;
    if (count_left >= 0 && count_left < x)
        gfx_fill(&g, count_left, U(28), x - 1, U(28) + g.font_h - 1, PEN_BG);
    if (g.planar)
        SetWriteMask(g.rp, 0x07);           /* pens 0 and 4 only */
    gfx_text(&g, x, U(28), count, PEN_DIM, PEN_BG, 0, TS_SMALL | TS_OPAQUE);
    if (g.planar)
        SetWriteMask(g.rp, 0xFF);
    count_left = x;
}

/* The breadcrumb: each level's title, the earlier ones links back to
 * their level (a click, or hover in mouse mode, as list rows). A long
 * one loses its first levels to "...". crumb[]: where each is drawn. */
static struct { WORD x0, x1; } crumb[MAXDEPTH];
static int crumb_first;             /* the first level shown (before it: "...") */
static int crumb_hover = -1;        /* the level under the pointer, -1 none */

static void draw_title(struct Page *p)
{
    char widest[24];
    int i;
    WORD cw = 0, maxw, x, sep = gfx_textlen(&g, " > ", TS_BOLD), dots;
    if (p->type == PT_LIST && p->list.count) {
        /* room for the widest count this list can show, so a growing
         * number never runs into the breadcrumb */
        p->title_wide = ch_reading(p);
        if (p->title_wide)
            strcpy(widest, "88888/88888...");   /* still growing */
        else
            sprintf(widest, "%ld/%ld", (long)p->list.count, (long)p->list.count);
        cw = gfx_textlen(&g, widest, TS_SMALL);
    }
    count_left = -1;
    maxw = g.pane_w - cw - U(8);
    dots = gfx_textlen(&g, "...", TS_BOLD) + sep;
    /* the levels that fit, the last ones first */
    {
        WORD total = 0;
        crumb_first = depth - 1;
        total = gfx_textlen(&g, pages[depth - 1].title, TS_BOLD);
        while (crumb_first > 0) {
            WORD w = gfx_textlen(&g, pages[crumb_first - 1].title, TS_BOLD) + sep;
            if (total + w + (crumb_first > 1 ? dots : 0) > maxw)
                break;
            total += w;
            crumb_first--;
        }
    }
    x = g.pane_x;
    if (crumb_first > 0) {
        gfx_text(&g, x, U(28), "...", PEN_DIM, PEN_BG, 0, TS_BOLD | TS_OPAQUE);
        x += gfx_textlen(&g, "...", TS_BOLD);
        gfx_text(&g, x, U(28), " > ", PEN_DIM, PEN_BG, 0, TS_BOLD | TS_OPAQUE);
        x += sep;
    }
    for (i = 0; i < depth; i++) {
        crumb[i].x0 = crumb[i].x1 = -1;
        if (i < crumb_first)
            continue;
        {
            BOOL last = i == depth - 1, hov = !last && i == crumb_hover && mouse_mode;
            WORD w = gfx_textlen(&g, pages[i].title, TS_BOLD), room = g.pane_x + maxw - x;
            UBYTE bg = hov ? PEN_RULE : PEN_BG;
            if (room <= 0)
                break;
            if (w > room)
                w = room;
            if (hov)
                gfx_fill(&g, x - U(2), U(27), x + w + U(1), U(28) + g.font_h, PEN_RULE);
            gfx_text(&g, x, U(28), pages[i].title, last ? PEN_WHITE : PEN_TEXT, bg, room,
                     TS_BOLD | TS_OPAQUE);
            crumb[i].x0 = x;
            crumb[i].x1 = x + w - 1;
            x += w;
            if (!last) {
                gfx_text(&g, x, U(28), " > ", PEN_DIM, PEN_BG, 0, TS_BOLD | TS_OPAQUE);
                x += sep;
            }
        }
    }
    {
        /* the rest of the band; the count (draw_title_count) keeps its place */
        WORD right = g.w;
        if (p->type == PT_LIST && p->list.count) {
            char count[24];
            sprintf(count, ch_reading(p) ? "%ld/%ld..." : "%ld/%ld", (long)p->sel + 1,
                    (long)p->list.count);
            right = g.pane_x + g.pane_w - gfx_textlen(&g, count, TS_SMALL);
            count_left = right;
            if (g.pane_x + g.pane_w < g.w)
                gfx_fill(&g, g.pane_x + g.pane_w, U(28), g.w - 1, U(28) + g.font_h - 1,
                         PEN_BG);
        }
        title_band_rest(g.pane_x, x, right);
    }
    draw_title_count(p);
    gfx_fill(&g, g.pane_x, U(41), g.pane_x + g.pane_w - 1, U(42) - 1, PEN_RULE);
}

/* The breadcrumb level at (x, y), -1 if none (the current level is not a link). */
static int crumb_at(WORD x, WORD y)
{
    int i;
    if (y < U(26) || y >= U(41))
        return -1;
    for (i = crumb_first; i < depth - 1; i++)
        if (crumb[i].x0 >= 0 && x >= crumb[i].x0 - U(2) && x <= crumb[i].x1 + U(2))
            return i;
    return -1;
}

static void pane_settle(void);

/* Back to level i of the breadcrumb (0: the section's top). */
static void crumb_go(int level)
{
    if (level < 0 || level >= depth - 1)
        return;
    pane_settle();
    while (depth > level + 1)
        page_pop();
    crumb_hover = -1;
    if (focus != FOCUS_CONTENT)
        focus = FOCUS_CONTENT;
    draw_menu();
    draw_pane(TRUE);
    draw_footer();
}

/* Row markers are icons (gfx_mark) in a column on the left before each
 * label. The column is the same width whatever the markers' state, so
 * ticking a box or picking a radio button never moves the text; lists
 * without markers have no column. */
static WORD mark_column(const struct Page *p)
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
static BOOL list_follow(struct Page *p)
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
 * It is a layer of its own: while it shows, the right of the list from
 * ov.strip_x on belongs to it alone. Rows are painted and scrolled only
 * left of the strip (the highlight and very long titles stop at its edge),
 * so scrolling never copies or repaints the letter; it is stamped once,
 * and again only when it changes. When it goes, the rows are repainted at
 * full width. It goes half a second after the key is let go. */
/* (state is declared with the other globals, near the top) */

static char ov_letter_of(const char *s)
{
    for (; *s; s++) {
        UBYTE c = (UBYTE)*s;
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

/* Long and mostly titles: the kind of list the letter helps with. */
static BOOL ov_eligible(const struct Page *p)
{
    LONG i, runs = 0;
    if (p->type != PT_LIST || p->list.count <= list_visible())
        return FALSE;
    for (i = 0; i < p->list.count; i++)
        if (p->list.items[i].kind == IT_RUN || p->list.items[i].kind == IT_PIC ||
            p->list.items[i].kind == IT_MOD || p->list.items[i].kind == IT_SONG ||
            p->list.items[i].kind == IT_PLEXDIR)
            runs++;                         /* lists in title order (not a track list's) */
    return runs * 2 >= p->list.count;
}

/* The preview pane: on screens 640 or more wide, a list with games in it
 * takes the left half of the pane, and the right half, with no border,
 * shows the selected game: its screenshot and fact sheet (all of it on
 * screens taller than 256 lines, the key lines on shorter ones). While a
 * long list glides, the same space shows the index letter; rows never
 * touch it, so neither ever covers a title. Lowres screens keep the
 * full-width list, with the letter over its right side. */
static WORD aside_w = -1;           /* -1: work out (after a screen opens) */

static WORD aside_width(void)
{
    if (aside_w < 0)
        aside_w = g.w >= 640 ? g.pane_w / 2 : 0;
    return aside_w;
}

static BOOL has_aside(struct Page *p)
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
                p->list.items[i].kind == IT_PLEXPHOTO)
                return TRUE;
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
static WORD aside_x(void)
{
    return g.pane_x + g.pane_w - aside_width();
}

/* ---- a long list's column at its right edge: with the mouse in use, a
 * scroll bar (click above or below the thumb to page, drag the thumb);
 * otherwise (joystick, pad), arrows where there is more above or below.
 * The mouse moving or clicking switches to the bar, the joystick to the
 * arrows; the keyboard leaves it as it is. */
#define GUT_W U(9)                  /* the column: gap, bar, gap */
static struct { BOOL drag; WORD off; } gut;
static BOOL bar_up;                 /* mouse mode: the bar is showing (the mouse moved lately) */
static ULONG mouse_last;            /* when the mouse last moved or clicked */
#define BAR_HIDE 1500               /* ms the mouse rests before the bar and the
                                     * pointer go */

static WORD list_visible(void);

static BOOL list_overflows(struct Page *p)
{
    return p && p->type == PT_LIST && p->list.count > list_visible();
}

/* The right edge of a list's area, its column included. */
static WORD list_edge(struct Page *p)
{
    return has_aside(p) ? aside_x() - U(6) - 1 : g.w - 1;
}

/* Last x a list row may paint. */
static WORD list_xr(struct Page *p)
{
    WORD e = list_edge(p);
    if (list_overflows(p))
        e -= GUT_W;                         /* the column */
    if (!has_aside(p) && ov.on && ov.page == p && e > ov.strip_x - 1)
        e = ov.strip_x - 1;                 /* lowres: the letter's strip is off limits */
    return e;
}

/* ---- the preview of the selected game, drawn once the selection settles */
#define PREVIEW_SETTLE 100     /* ms the selection rests before its preview is read */
#define PLEX_SETTLE    350     /* ...a Plex entry's: its picture asked for, then decoded */
#define PREVIEW_READY   30     /* ...or drawn, if it was read ahead already */
static BOOL hold_active(void);
static BOOL pv_pending;
static ULONG pv_due;

/* "12-Mar-1994" from a DateStamp's days. */
static void date_text(LONG days, char *out)
{
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const UBYTE mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    LONG y = 1978, m = 0;
    for (;;) {
        LONG yd = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        if (days < yd)
            break;
        days -= yd;
        y++;
    }
    for (m = 0; m < 11; m++) {
        LONG md = mdays[m] + (m == 1 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        if (days < md)
            break;
        days -= md;
    }
    sprintf(out, "%ld-%s-%ld", (long)days + 1, mon[m], (long)y);
}

static void pv_line(const char *s, WORD w)
{
    if (!s[0])
        list_add(&pv_list, "", "", "", IT_TEXT);
    else
        wrap_into(&pv_list, s, w);
}

/* ---- preview jobs: read by the loader (loader.c). Each entry is two jobs,
 * its text (fact sheet, file details) and its picture, so the text is up
 * at once and the picture follows; what the pane waits for goes ahead of
 * the loader's queue. The entries next to the one shown are read ahead,
 * so a steady pace of Down finds each ready. With memory to spare
 * (pv_roomy), speed comes first: two entries ahead and one behind are
 * read, and entries shown stay decoded for going back, up to PV_SLOTS
 * jobs. Short of memory, only the one shown and the next one ahead. */
#define PV_SLOTS 12
#define PV_TIGHT 4
#define PV_SPIN_AFTER SPIN_AFTER    /* ms without its picture before the spinner */
static struct LJob *pv_slot[PV_SLOTS];
static ULONG pv_clock;              /* cache order */
static BOOL pv_text_drawn, pv_pic_drawn, pv_has_box, pv_spin_on;
static WORD pv_box_y, pv_box_h;     /* where the picture goes */
static ULONG pv_since;              /* when the selection moved to what the pane shows */
static UWORD pv_spin_phase;
static ULONG pv_spin_since;         /* when it began turning */

static BOOL pv_roomy(void)
{
    return AvailMem(MEMF_ANY) >= 3UL * 1024 * 1024 && AvailMem(MEMF_CHIP) >= 768UL * 1024;
}
static LONG pv_dir = 1;             /* the way the list last moved */
static LONG pv_ready, pv_waited;    /* previews found read ahead, or waited for (log) */

/* What the pane shows for entry idx: its text job's type and path (0 if
 * none), and how its picture is found (*shot SHOT_*, 0 if it has none). */
static int pv_kind(struct Page *p, LONG idx, char *path, LONG max, UBYTE *shot)
{
    struct Item *it;
    *shot = 0;
    if (idx < 0 || idx >= p->list.count)
        return 0;
    it = &p->list.items[idx];
    if (it->flags & ITEM_GONE)
        return 0;                           /* not held now: previewed once read again */
    if (p->songs && !p->plex) {
        /* a music file or folder: its details, and its picture */
        if (it->kind != IT_SONG && it->kind != IT_DIR)
            return 0;
        amce_strlcpy(path, it->path, max);
        *shot = SHOT_SONG;
        return LJ_SONG;
    }
    if (p->pics) {
        amce_strlcpy(path, it->path, max);
        if (it->kind == IT_PIC) {
            *shot = SHOT_FILE;
            return LJ_PICTURE;
        }
        if (it->kind == IT_DIR) {
            if (!(it->flags & ITEM_DISABLED))
                *shot = SHOT_FOLDER;
            return LJ_PICFOLDER;
        }
        return 0;
    }
    if (it->kind != IT_RUN)
        return 0;
    item_path(it, path, max);
    *shot = SHOT_AGS;
    return LJ_ENTRY;
}

static void pv_free_slot(int i)
{
    struct LJob *j = pv_slot[i];
    if (!j)
        return;
    if (j->busy)
        ldr_wait(j);
    pic_dispose(j->obj);
    thumb_free(j->thumb);
    if (pv_tj == j)
        pv_tj = NULL;
    if (pv_sj == j)
        pv_sj = NULL;
    FreeVec(j);
    pv_slot[i] = NULL;
}

/* Every job back and freed: before the screen closes or a game runs. */
static void pv_flush(void)
{
    int i;
    for (i = 0; i < PV_SLOTS; i++)
        pv_free_slot(i);
    pv_tj = pv_sj = NULL;
    np_art_free();                          /* Now playing's cover too */
}

/* The pane's jobs let go without waiting for them, for the viewer: one
 * still out is orphaned (one not begun never runs, one begun is freed
 * when it ends), where it holds nothing of the screen's (a colour-table
 * screen: decoded unremapped, no pens). Waiting, as pv_flush does, could
 * take minutes: every preview queued ran first, each a JPEG of seconds,
 * and a station playing takes half the CPU. */
static void pv_let_go(void)
{
    int i;
    for (i = 0; i < PV_SLOTS; i++) {
        struct LJob *j = pv_slot[i];
        if (j && j->busy && !g.truecolor) {
            ldr_orphan(j);
            pv_slot[i] = NULL;
        } else {
            pv_free_slot(i);
        }
    }
    pv_tj = pv_sj = NULL;
}

static BOOL pv_current(const struct LJob *j)
{
    return j == pv_tj || j == pv_sj;
}

/* The job for (type, mode, path): one already out or back, else a new one.
 * NULL if every slot is busy (a read-ahead is then skipped). first: the
 * pane waits for it, so it goes ahead of the loader's queue. */
/* Colours a picture may have (0: as many as the screen gives): fewer for a
 * Plex cover on a slower Amiga, less to reduce and to set. */
static int pv_colour_cap;
static int plex_colours(void)
{
    return plex_tier() == 0 ? 48 : plex_tier() == 1 ? 64 : 0;
}

static struct LJob *pv_request(int type, UBYTE mode, const char *path, BOOL ahead, BOOL first)
{
    int i, free_i = -1;
    struct LJob *j;
    int limit = pv_roomy() ? PV_SLOTS : PV_TIGHT, used = 0;
    for (i = 0; i < PV_SLOTS; i++) {
        j = pv_slot[i];
        if (j && j->type == type && j->mode == mode && strcmp(j->path, path) == 0) {
            if (!j->busy && (j->spent || !j->ran)) {
                pv_free_slot(i);        /* its picture went to the screen, or it was
                                         * skipped: read it again */
                break;
            }
            j->cancel = FALSE;
            j->stamp = ++pv_clock;
            if (first && j->busy)
                ldr_submit_first(j);    /* not started yet: to the front */
            return j;
        }
    }
    for (i = 0; i < PV_SLOTS; i++)
        if (pv_slot[i])
            used++;
    for (i = 0; i < PV_SLOTS && free_i < 0 && used < limit; i++)
        if (!pv_slot[i])
            free_i = i;
    while (free_i < 0) {
        /* the longest unused one that is back */
        int old = -1;
        for (i = 0; i < PV_SLOTS; i++)
            if (pv_slot[i] && !pv_slot[i]->busy && !pv_current(pv_slot[i]) &&
                (old < 0 || pv_slot[i]->stamp < pv_slot[old]->stamp))
                old = i;
        if (old < 0)
            break;
        pv_free_slot(old);
        if (--used < limit)
            free_i = old;
    }
    if (free_i < 0) {
        /* every one is still out: a read-ahead waits its turn; the one
         * asked for goes past the limit (never waited for here) */
        if (ahead)
            return NULL;
        for (i = 0; i < PV_SLOTS && free_i < 0; i++)
            if (!pv_slot[i])
                free_i = i;
        if (free_i < 0)
            return NULL;
    }
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    j->type = type;
    j->mode = mode;
    amce_strlcpy(j->path, path, sizeof(j->path));
    j->scr = g.scr;
    j->remap = g.truecolor;
    j->want_pic = FALSE;                    /* pictures are LJ_SHOT jobs of their own */
    if (type == LJ_SHOT && gfx_thumbs(&g)) {
        /* made small in the loader, in colours of its own (thumb.c) */
        j->remap = FALSE;
        j->tw = aside_width() - U(4);
        j->th = pv_box_h;
        j->tall = gfx_mode_tall(g.mode);
        j->maxcol = gfx_thumb_colours(&g);
        if (pv_colour_cap && j->maxcol > pv_colour_cap)
            j->maxcol = pv_colour_cap;
    }
    j->stamp = ++pv_clock;
    pv_slot[free_i] = j;
    if (first)
        ldr_submit_first(j);
    else
        ldr_submit(j);
    return j;
}

static void pv_release(struct LJob *j)
{
    int i;
    for (i = 0; i < PV_SLOTS; i++)
        if (j && pv_slot[i] == j)
            pv_free_slot(i);
}

static BOOL pv_pictures(void)
{
    return g.pens_shared || g.truecolor;
}

/* Read the entries around this one ahead: the next one the way the list
 * moves; with memory to spare, the one after it and the one behind too. */
static void pv_read_ahead(struct Page *p)
{
    static const LONG near[] = { 1, 2, -1 };
    char path[300];
    int i, n = pv_roomy() ? 3 : 1;
    UBYTE shot;
    for (i = 0; i < n; i++) {
        int k = pv_kind(p, p->sel + near[i] * pv_dir, path, sizeof(path), &shot);
        if (!k || !pv_request(k, 0, path, TRUE, FALSE))
            break;
        if (shot && pv_pictures() && !pv_request(LJ_SHOT, shot, path, TRUE, FALSE))
            break;
    }
}

/* A Pictures row's lines: what it is and, for a folder, what Fire and
 * Right do there (the picture's size once it is drawn). */
static void pv_pics_lines(struct Item *it, struct LJob *j, BOOL drawn, WORD w)
{
    char line[120];
    pv_line(it->label, w);
    if (it->kind == IT_PIC) {
        char date[24];
        if (drawn) {
            pic_describe(&g.pic, line, sizeof(line));
            pv_line(line, w);
        }
        sprintf(line, "%s", drawn && g.pic.fmt[0] ? g.pic.fmt : it->sortkey);
        if (j->size >= 0) {
            char kb[24];
            sprintf(kb, "  -  %ld KB", (long)((j->size + 1023) / 1024));
            amce_strlcat(line, kb, sizeof(line));
        }
        pv_line(line, w);
        pv_line("", w);
        {
            const char *e = strrchr(it->path, '/');
            if (!e)
                e = strchr(it->path, ':');
            amce_strlcpy(line, it->path, e ? (LONG)(e - it->path) + 1 + (*e == ':') : (LONG)sizeof(line));
            if (line[0])
                pv_line(line, w);
        }
        if (j->size >= 0) {
            date_text(j->days, date);
            sprintf(line, "Changed %s", date);
            pv_line(line, w);
        }
    } else if ((it->flags & ITEM_DISABLED) || (j->np <= 0 && j->nd == 0)) {
        pv_line("No pictures in this folder.", w);
    } else if (j->nd == 0) {
        sprintf(line, j->np == 1 ? "%ld picture" : "%ld pictures", (long)j->np);
        pv_line(line, w);
        pv_line("", w);
        pv_line("Fire: slideshow", w);
        pv_line("Right: open the folder", w);
    } else {
        if (j->np > 0)
            sprintf(line, "%ld folder%s, %ld picture%s", (long)j->nd, j->nd == 1 ? "" : "s",
                    (long)j->np, j->np == 1 ? "" : "s");
        else
            sprintf(line, j->nd == 1 ? "%ld folder" : "%ld folders", (long)j->nd);
        pv_line(line, w);
        pv_line("", w);
        pv_line("Fire or Right: open", w);
    }
}

/* The spinner in the picture's place (its place cleared the first time). */
static void pv_spinner(BOOL first)
{
    WORD x0 = aside_x(), w = aside_width() - U(4);
    if (first)
        gfx_fill_bg(&g, x0, pv_box_y, g.w - 1, pv_box_y + pv_box_h - 1);
    spin_draw(x0 + w / 2, pv_box_y + pv_box_h / 2 - U(8), pv_spin_phase, first);
}

/* The timer: turn the spinner while the picture is not there yet. */
static void pv_spin_tick(void)
{
    struct Page *p = cur();
    if (!pv_shown || !p || p != pv_page || p->sel != pv_sel || !has_aside(p) || pv_pic_drawn ||
        !pv_has_box || hold_active() || ov.on || pane_pending)
        return;
    if (!pv_spin_on) {
        if ((LONG)(time_ms() - pv_since) < PV_SPIN_AFTER)
            return;
        pv_spin_on = TRUE;
        pv_spin_phase = 0;
        pv_spin_since = time_ms();
        pv_spinner(TRUE);
        return;
    }
    if (spin_phase(pv_spin_since) != pv_spin_phase) {
        pv_spin_phase = spin_phase(pv_spin_since);
        pv_spinner(FALSE);
    }
}

static BOOL pv_spinning(void)
{
    return pv_shown && pv_has_box && !pv_pic_drawn && (pv_sj || pv_tj);
}

/* The text, under the picture's place (or at the top with no picture). */
static void pv_draw_text(struct Page *p)
{
    WORD x0 = aside_x(), w = aside_width() - U(4), max_y = g.footer_y - 2;
    WORD y = pv_has_box ? pv_box_y + pv_box_h + U(6) : g.list_top;
    struct LJob *j = pv_tj;
    LONG i;
    tb_close(&pv_tb);
    gfx_fill_bg(&g, x0, pv_has_box ? pv_box_y + pv_box_h : g.list_top, g.w - 1, max_y);
    if (pv_list.pool)
        list_free(&pv_list);
    if (!list_init(&pv_list))
        return;
    if (p->pics)
        pv_pics_lines(&p->list.items[p->sel], j, pv_pic_drawn && g.pic_w, w - U(9));
    else
        /* the whole fact sheet, in a text box with a scroll bar when it is
         * longer than the room left (wheel or bar; Up/Down stay with the list) */
        for (i = 0; i < j->nlines; i++)
            pv_line(j->lines[i], w - U(9));
    if (pv_list.count && max_y - y >= g.font_h) {
        pv_tb.pos = pv_tb.target = 0;
        pv_tb.focused = focus == FOCUS_PREVIEW;
        if (tb_open(&g, &pv_tb, x0, y, w - U(9), max_y - y + 1, &pv_list, TRUE))
            tb_draw(&g, &pv_tb);
    }
}

/* What the picture place shows now: the file (a folder's: its first
 * picture) and the place, so an entry that shows the same picture in the
 * same place (a folder, then its first picture inside it; and back) keeps
 * it on screen: not cleared, read or drawn again. */
static struct { char key[300]; WORD x, y, w, h; } pv_onscr;

/* The picture on screen is file's: moved into the box x, y, w x h (centred
 * across, at its top), as it would be drawn there, and counted as there.
 * A cover going from an album's pane to Now playing and back: copied
 * across, not asked for, decoded and remapped again. */
static BOOL pic_take(const char *file, WORD x, WORD y, WORD w, WORD h)
{
    if (!file[0] || !g.pic_w || strcmp(file, pv_onscr.key) || g.pic_w > w || g.pic_h > h)
        return FALSE;
    if (!gfx_picture_move(&g, x + (w - g.pic_w) / 2, y))
        return FALSE;
    if (pv_onscr.x != x || pv_onscr.y != y)
        amce_log("preview", "the cover on screen moved across, not read again");
    pv_onscr.x = x;
    pv_onscr.y = y;
    pv_onscr.w = w;
    pv_onscr.h = h;
    return TRUE;
}

/* x0,y0..x1,y1 cleared but for the picture on screen. */
static void fill_around_pic(WORD x0, WORD y0, WORD x1, WORD y1)
{
    WORD px1 = g.pic_x + g.pic_w - 1, py1 = g.pic_y + g.pic_h - 1;
    if (g.pic_y > y0)
        gfx_fill_bg(&g, x0, y0, x1, g.pic_y - 1);
    if (g.pic_x > x0)
        gfx_fill_bg(&g, x0, g.pic_y, g.pic_x - 1, py1);
    if (px1 < x1)
        gfx_fill_bg(&g, px1 + 1, g.pic_y, x1, py1);
    if (py1 < y1)
        gfx_fill_bg(&g, x0, py1 + 1, x1, y1);
}

static char np_art_file[120];       /* the cover Now playing shows (or will) */

/* The picture file a preview of path (shot: SHOT_*) shows, into key ("":
 * not known yet, a folder not scanned). */
static void pv_shot_key(UBYTE shot, const char *path, char *key)
{
    int i;
    key[0] = 0;
    if (shot == SHOT_FILE) {
        amce_strlcpy(key, path, 300);
    } else if (shot == SHOT_AGS) {
        key[0] = 1;                         /* an entry's screenshot: never a file's key */
        amce_strlcpy(key + 1, path, 299);
    } else if (shot == SHOT_FOLDER || shot == SHOT_SONG) {
        for (i = 0; i < PV_SLOTS; i++) {
            struct LJob *j = pv_slot[i];
            if (j && j->done && j->first[0] && !strcmp(j->path, path) &&
                (j->type == LJ_PICFOLDER || j->type == LJ_SONG ||
                 (j->type == LJ_SHOT && j->mode == shot))) {
                amce_strlcpy(key, j->first, 300);
                return;
            }
        }
    }
}

/* Entry idx of p would show the picture on screen now, where it is. */
static BOOL pv_keeps(struct Page *p, LONG idx)
{
    char path[300], key[300];
    UBYTE shot;
    WORD max_y = g.footer_y - 2;
    if (p->plex)
        return has_aside(p) && plex_keeps(p, idx);
    if (!g.pic_w || !pv_onscr.key[0] || !has_aside(p) || !pv_pictures() ||
        !pv_kind(p, idx, path, sizeof(path), &shot) || !shot)
        return FALSE;
    pv_shot_key(shot, path, key);
    return key[0] && !strcmp(key, pv_onscr.key) && pv_onscr.x == aside_x() &&
           pv_onscr.y == g.list_top && pv_onscr.w == aside_width() - U(4) &&
           pv_onscr.h == (max_y - g.list_top) / 2 && g.pic_x >= aside_x() &&
           g.pic_y >= g.list_top && g.pic_y + g.pic_h <= g.list_top + pv_onscr.h;
}

/* The picture in its place, the rest of the place cleared. */
static void pv_draw_picture(void)
{
    WORD x0 = aside_x(), w = aside_width() - U(4), dh = 0;
    struct LJob *j = pv_sj;
    BOOL taken = FALSE, ok = FALSE;
    if (j && j->thumb) {
        char m[400];
        ULONG t0 = time_ms(), now;
        ok = gfx_thumb(&g, j->thumb, &j->info, x0, pv_box_y, w, pv_box_h, &dh);
        now = time_ms();
        /* where the time went: waiting for the loader, reading, making,
         * drawing; and in all since the selection came to rest */
        sprintf(m, "%.60s, %lu x %lu, depth %lu: %s%lu ms to start, %s in %lu ms, "
                   "made %lu x %lu, %lu colours, in %lu ms, drawn in %lu ms "
                   "(colours %lu, pixels %lu, copy up %lu); "
                   "shown %lu ms after the move%s%s%s",
                FilePart((STRPTR)j->path), (unsigned long)j->info.w, (unsigned long)j->info.h,
                (unsigned long)j->info.depth, j->urgent ? "" : "read ahead, ",
                (unsigned long)(j->t_begun - j->t_sent), j->own ? "read by AmigaMCE" : "decoded",
                (unsigned long)j->ms_decode, (unsigned long)j->thumb->w,
                (unsigned long)j->thumb->h, (unsigned long)j->thumb->ncol,
                (unsigned long)j->ms_thumb, (unsigned long)(now - t0),
                (unsigned long)gfx_draw_ms[0], (unsigned long)gfx_draw_ms[1],
                (unsigned long)gfx_draw_ms[2],
                (unsigned long)(now - pv_since), j->info.cls[0] ? " (" : "",
                j->info.cls, j->info.cls[0] ? ".datatype)" : "");
        amce_log("preview", m);
        if (j->ms_parts[0] + j->ms_parts[1] + j->ms_parts[2] + j->ms_parts[3]) {
            sprintf(m, "made in: reading the picture %lu ms, counting its colours %lu, "
                       "choosing %lu, mapping %lu",
                    (unsigned long)j->ms_parts[0], (unsigned long)j->ms_parts[1],
                    (unsigned long)j->ms_parts[2], (unsigned long)j->ms_parts[3]);
            amce_log("preview", m);
        }
    } else if (j && j->obj) {
        ok = gfx_picture_obj(&g, j->obj, j->remapped, x0, pv_box_y, w, pv_box_h, TRUE, &dh, &taken);
    }
    pv_onscr.key[0] = 0;
    if (ok && j) {
        if (j->mode == SHOT_FOLDER || j->mode == SHOT_SONG)
            amce_strlcpy(pv_onscr.key, j->first, sizeof(pv_onscr.key));
        else
            pv_shot_key(j->mode, j->path, pv_onscr.key);
        pv_onscr.x = x0;
        pv_onscr.y = pv_box_y;
        pv_onscr.w = w;
        pv_onscr.h = pv_box_h;
    }
    if (ok) {
        gfx_fill_bg(&g, x0, pv_box_y, g.pic_x - 1, pv_box_y + dh - 1);
        gfx_fill_bg(&g, g.pic_x + g.pic_w, pv_box_y, g.w - 1, pv_box_y + dh - 1);
        if (dh < pv_box_h)
            gfx_fill_bg(&g, x0, pv_box_y + dh, g.w - 1, pv_box_y + pv_box_h - 1);
    } else {
        gfx_picture_clear(&g, x0, pv_box_y, g.w - 1, pv_box_y + pv_box_h - 1);
        gfx_fill_bg(&g, x0, pv_box_y, g.w - 1, pv_box_y + pv_box_h - 1);
    }
    if (taken) {
        j->obj = NULL;                      /* the screen has it now (its pens) */
        j->spent = TRUE;
    }
}

/* Draw what has come back for the pane: the text as soon as it is here,
 * then the picture; when both are up, read the neighbours ahead. */
static void pv_progress(struct Page *p)
{
    char path[300];
    UBYTE shot;
    int k;
    if (pv_tj && pv_tj->done && !pv_tj->ran) {      /* skipped: ask again */
        k = pv_kind(p, p->sel, path, sizeof(path), &shot);
        pv_release(pv_tj);
        pv_tj = k ? pv_request(k, 0, path, FALSE, TRUE) : NULL;
    }
    if (pv_sj && pv_sj->done && !pv_sj->ran) {
        k = pv_kind(p, p->sel, path, sizeof(path), &shot);
        pv_release(pv_sj);
        pv_sj = shot ? pv_request(LJ_SHOT, shot, path, FALSE, TRUE) : NULL;
    }
    if (pv_tj && pv_tj->done && !pv_text_drawn) {
        /* an entry without a screenshot, an empty folder: no picture place */
        if (pv_has_box && pv_tj->type != LJ_PICTURE && pv_tj->np <= 0) {
            pv_has_box = FALSE;
            if (pv_sj)
                pv_sj->cancel = TRUE;
            pv_sj = NULL;
            gfx_picture_clear(&g, aside_x(), g.list_top, g.w - 1, g.footer_y - 2);
        }
        pv_draw_text(p);
        pv_text_drawn = TRUE;
    }
    if (pv_text_drawn && pv_sj && pv_sj->done && !pv_pic_drawn) {
        pv_draw_picture();
        pv_pic_drawn = TRUE;
        pv_spin_on = FALSE;
        if (p->pics)
            pv_draw_text(p);                /* now with the picture's size */
    }
    if (pv_text_drawn && (pv_pic_drawn || !pv_has_box)) {
        if (!pv_roomy()) {
            struct LJob *t = pv_tj, *s2 = pv_sj;
            pv_tj = pv_sj = NULL;           /* short of memory: keep only what is ahead */
            pv_release(t);
            pv_release(s2);
        }
        pv_read_ahead(p);
    }
}

/* Preview the selected entry: its text and picture at once if they were
 * read ahead, else as the loader brings them (text first); the old ones
 * go at once, and a spinner shows if the picture takes a while. */
static void preview_draw(struct Page *p)
{
    char path[300];
    UBYTE shot;
    int k;
    BOOL keep;
    WORD x0 = aside_x(), max_y = g.footer_y - 2;
    BOOL was_shown = pv_shown && pv_page == p;
    pv_pending = FALSE;
    pv_page = p;
    pv_sel = p->sel;
    pv_shown = TRUE;
    pv_tj = pv_sj = NULL;
    pv_text_drawn = pv_pic_drawn = pv_spin_on = FALSE;
    tb_close(&pv_tb);
    if (p->plex) {
        plex_pv_draw(p, was_shown);         /* the server's details (plex_pv_draw) */
        return;
    }
    k = p->list.count ? pv_kind(p, p->sel, path, sizeof(path), &shot) : 0;
    if (!pv_pictures())
        shot = 0;
    pv_box_y = g.list_top;
    pv_box_h = (max_y - g.list_top) / 2;
    pv_has_box = shot != 0;
    keep = shot && pv_keeps(p, p->sel);     /* the picture on screen is this one's */
    if (!pv_since)
        pv_since = time_ms();
    if (k) {
        if (shot && !keep)
            pv_sj = pv_request(LJ_SHOT, shot, path, FALSE, TRUE);
        pv_tj = pv_request(k, 0, path, FALSE, TRUE);   /* last in: first out */
    }
    if (keep && pv_tj)
        pv_pic_drawn = TRUE;
    if (!pv_tj) {
        tb_close(&pv_tb);
        gfx_picture_clear(&g, x0, g.list_top, g.w - 1, max_y);
        gfx_fill_bg(&g, x0, g.list_top, g.w - 1, max_y);
        return;
    }
    if (pv_tj->done && (!pv_sj || pv_sj->done))
        pv_ready++;
    else
        pv_waited++;
    if (keep) {
        if (!pv_tj->done)                   /* the picture stays; the text place cleared */
            gfx_fill_bg(&g, x0, pv_box_y + pv_box_h, g.w - 1, max_y);
    } else if (!pv_tj->done || (pv_sj && !pv_sj->done) || !pv_sj) {
        /* not all here: nothing of the entry before stays up meanwhile */
        gfx_picture_clear(&g, x0, g.list_top, g.w - 1, max_y);
        gfx_fill_bg(&g, x0, g.list_top, g.w - 1, pv_tj->done ? g.list_top + pv_box_h - 1 : max_y);
    }
    pv_progress(p);
    pv_spin_tick();                         /* waited 0.25 s already: the spinner now */
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
        np_art_draw();                      /* Now playing's cover is made */
    if (!pv_tj && !pv_sj)
        return;
    if (!((pv_tj && pv_tj->done && !pv_text_drawn) || (pv_sj && pv_sj->done && !pv_pic_drawn) ||
          (pv_tj && pv_tj->done && !pv_tj->ran)))
        return;
    if (p && p == pv_page && p->sel == pv_sel && has_aside(p) && !pane_pending &&
        !hold_active() && !ov.on)
        pv_progress(p);
    else if (p && (p != pv_page || p->sel != pv_sel)) {
        pv_shown = FALSE;                   /* moved on: preview again when it rests */
        pv_tj = pv_sj = NULL;
        preview_touch(p);
    }
}

/* A job back but not dealt with (collected while waiting for another). */
static BOOL loader_due(void)
{
    int d;
    if ((pv_tj && pv_tj->done && !pv_text_drawn) || (pv_sj && pv_sj->done && !pv_pic_drawn))
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
    if (pv_tj && !pv_text_drawn) {
        ldr_wait(pv_tj);
        preview_arrived();
    }
}

/* The selection changed (or the list was drawn): preview it once it rests. */
/* ---- Settings > Style on a laced screen: beside the list of fonts, the
 * Home menu as it would look in the one selected, at its own size (its
 * row height, margins and icons scaled as the layout would be). */
static struct TextFont *spv_font;   /* the font previewed, open */
static int spv_idx = -1;

static void style_preview_free(void)
{
    if (spv_font)
        CloseFont(spv_font);
    spv_font = NULL;
    spv_idx = -1;
}

static void style_preview(struct Page *p)
{
    WORD x0 = aside_x(), x1 = g.w - 1, y0 = g.list_top, y1 = g.footer_y - 2;
    const struct LacedFont *lf;
    struct TextFont *f;
    struct TextExtent te;
    LONG k, longest = 0, w, rh, mw, ipx, y, i, n;
#define SU(v) ((WORD)(((LONG)(v) * k + 128) >> 8))
    pv_page = p;
    pv_sel = p->sel;
    pv_shown = TRUE;
    pv_pending = FALSE;
    gfx_fill_bg(&g, x0, y0, x1, y1);
    if (p->sel < 0 || p->sel >= LFONT_COUNT)
        return;
    lf = &gfx_laced_fonts[p->sel];
    if (spv_idx != p->sel) {
        if (spv_font)
            CloseFont(spv_font);
        spv_font = gfx_open_laced_font(p->sel);
        spv_idx = p->sel;
    }
    if (!(f = spv_font)) {
        gfx_text(&g, x0, y0, "Not in the Fonts drawer", PEN_DIM, PEN_BG, x1 - x0, TS_BODY);
        return;
    }
    k = ((LONG)lf->ysize << 8) / 12;
    rh = SU(18);
    ipx = 16 * lf->kint;
    SetFont(g.rp, f);
    for (i = 0; i < nmenu; i++)
        if ((w = TextLength(g.rp, (CONST_STRPTR)mod_names[menu[i]],
                            strlen(mod_names[menu[i]]))) > longest)
            longest = w;
    mw = SU(23) + longest + SU(6);
    if (mw > x1 - x0 - SU(3))
        mw = x1 - x0 - SU(3);
    for (i = 0, y = y0; i < nmenu && y + rh - 1 <= y1; i++, y += rh) {
        UBYTE bg = i == menu_sel ? PEN_HL : PEN_BG;
        const char *s = mod_names[menu[i]];
        gfx_fill(&g, x0, y, x0 + mw - 1, y + rh - 1, bg);
        gfx_icon_sized(&g, menu[i], x0 + SU(3), y + (rh - ipx) / 2, PEN_WHITE, ipx);
        SetFont(g.rp, f);
        n = TextFit(g.rp, (CONST_STRPTR)s, strlen(s), &te, NULL, 1, mw - SU(24),
                    f->tf_YSize + 1);
        SetAPen(g.rp, PEN_WHITE);
        SetDrMd(g.rp, JAM1);
        Move(g.rp, x0 + SU(23), y + (rh - f->tf_YSize) / 2 + f->tf_Baseline);
        if (n > 0)
            Text(g.rp, (CONST_STRPTR)s, n);
    }
    if (y > y0)
        gfx_fill(&g, x0 + mw + SU(2), y0, x0 + mw + SU(3) - 1, y - 1, PEN_RULE);
    SetFont(g.rp, g.font);
#undef SU
}

static void preview_touch(struct Page *p)
{
    char path[300];
    UBYTE shot;
    int k, i, found = 0, want;
    ULONG settle = PREVIEW_SETTLE;
    if (!has_aside(p) || (pv_shown && pv_page == p && pv_sel == p->sel))
        return;                             /* already showing this one */
    if (p->style_pv) {
        style_preview(p);                   /* no reading: drawn at once */
        return;
    }
    if (p->plex)
        settle = PLEX_SETTLE;               /* its picture is asked of the server: rest first */
    /* read ahead and back already: drawing it is only a copy, so sooner */
    if ((k = pv_kind(p, p->sel, path, sizeof(path), &shot))) {
        want = shot && pv_pictures() ? 2 : 1;
        for (i = 0; i < PV_SLOTS; i++) {
            struct LJob *j = pv_slot[i];
            if (j && j->done && j->ran && !j->spent && strcmp(j->path, path) == 0 &&
                ((j->type == k && !j->mode) || (j->type == LJ_SHOT && j->mode == shot)))
                found++;
        }
        if (found >= want)
            settle = PREVIEW_READY;
    }
    if (!pv_shown && p->plex && plex_movable(p, p->sel))
        settle = PREVIEW_READY;             /* its cover is on screen: only moved */
    else if (!pv_shown && g.pic_w && !pv_keeps(p, p->sel))
        /* a page just shown (opened, or back to it): a picture left from
         * the page before goes now, unless it is this entry's too */
        gfx_picture_clear(&g, aside_x(), g.list_top, g.w - 1, g.footer_y - 2);
    if (pv_shown && pv_page == p && !hold_active() && !ov.on && !pv_keeps(p, p->sel)) {
        /* the entry before's picture goes now, not when this one's comes:
         * it never stands next to this one's text */
        gfx_picture_clear(&g, aside_x(), g.list_top, g.w - 1, g.footer_y - 2);
        if (settle != PREVIEW_READY) {
            /* not read yet: its text goes now too */
            tb_close(&pv_tb);
            gfx_fill_bg(&g, aside_x(), g.list_top, g.w - 1, g.footer_y - 2);
            pv_shown = FALSE;
            pv_tj = pv_sj = NULL;
        }
    }
    pv_pending = TRUE;
    pv_since = time_ms();
    pv_due = pv_since + settle;
}

static void preview_tick(void)
{
    struct Page *p = cur();
    if (!pv_pending)
        return;
    if (!p || !has_aside(p) || pane_pending) {
        pv_pending = FALSE;
        return;
    }
    if (hold_active() || ov.on || (LONG)(time_ms() - pv_due) < 0)
        return;                             /* still moving, or the letter is up */
    preview_draw(p);
}

static void ov_free(void)
{
    ov.bl = NULL;
    ov.on = FALSE;
    ov.letter = 0;
}

/* Build the big letter for c, its ink about 40% of the list's height
 * (gfx_bigletter: the font's glyph in whole pixels, or smooth). */
static BOOL ov_build(char ch)
{
    WORD rows_h = list_visible() * g.list_row_h;
    const struct BigLetter *bl = gfx_bigletter(&g, (UBYTE)ch, rows_h * 2 / 5);
    if (!bl)
        return FALSE;
    ov.bl = bl;
    ov.letter = ch;
    ov.y = g.list_top + (rows_h - bl->h) / 2;
    if (has_aside(ov.page)) {
        /* centred in the letter column */
        ov.strip_x = aside_x();
        ov.x = ov.strip_x + (aside_width() - bl->w) / 2;
        return TRUE;
    }
    ov.x = g.pane_x + g.pane_w * 9 / 10 - bl->w;
    /* the strip fits the widest letter, so it does not move as letters change */
    ov.strip_x = g.pane_x + g.pane_w * 9 / 10 - bl->cell_w - U(6);
    if (ov.strip_x < g.pane_x + g.pane_w / 2)
        ov.strip_x = g.pane_x + g.pane_w / 2;
    return TRUE;
}


static void draw_list_rows(struct Page *p);

/* Stamp the letter into its strip (nothing else is drawn there). */
static void ov_stamp(void)
{
    gfx_bigletter_draw(&g, ov.bl, ov.x, ov.y, PEN_DIM);
}

static void ov_show(struct Page *p)
{
    char c = ov_letter_of(p->list.items[p->sel].label);
    ov.page = p;
    if (!c || !ov_build(c))
        return;
    ov.on = TRUE;
    ov.linger = 0;
    if (has_aside(p)) {
        gfx_picture_clear(&g, 0, g.list_top, g.w - 1, g.footer_y - 2);  /* the letter now */
        tb_close(&pv_tb);
        pv_shown = FALSE;
    }
    /* hand the strip to the letter: clear it once, then stamp */
    gfx_fill_bg(&g, ov.strip_x, g.list_top, g.w - 1, g.footer_y - 2);
    ov_stamp();
}

/* Changing letters off screen: the old letter's box and the new one's,
 * cleared and the new letter stamped there, then copied up in one go with
 * the beam (gfx_blit_beam). Cleared and stamped on the screen, the letter
 * flickered: a smooth glide changes it nearly every frame. */
static struct BitMap *ov_bm;
static WORD ov_bw, ov_bh;

static void ov_bm_free(void)
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
static void ov_relabel(char c)
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
    const struct BigLetter *old = ov.bl;
    WORD ox = ov.x, oy = ov.y, x0, y0, x1, y1, w, h;
    if (!ov.on || !c || c == ov.letter)
        return;
    if (!ov_build(c)) {
        if (old)
            gfx_fill(&g, ox, oy, ox + old->w - 1, oy + old->h - 1, PEN_BG);
        return;
    }
    x0 = ov.x;
    y0 = ov.y;
    x1 = ov.x + ov.bl->w - 1;
    y1 = ov.y + ov.bl->h - 1;
    if (old) {
        if (ox < x0) x0 = ox;
        if (oy < y0) y0 = oy;
        if (ox + old->w - 1 > x1) x1 = ox + old->w - 1;
        if (oy + old->h - 1 > y1) y1 = oy + old->h - 1;
    }
    w = x1 - x0 + 1;
    h = y1 - y0 + 1;
    if (!ov_bm || ov_bw < w || ov_bh < h) {
        WORD nw = ov_bm && ov_bw > w ? ov_bw : w, nh = ov_bm && ov_bh > h ? ov_bh : h;
        ov_bm_free();
        ov_bm = AllocBitMap(nw, nh, g.depth, BMF_CLEAR, g.rp->BitMap);
        ov_bw = nw;
        ov_bh = nh;
    }
    if (ov_bm) {
        struct RastPort orp, *screen_rp = g.rp;
        InitRastPort(&orp);
        orp.BitMap = ov_bm;
        g.rp = &orp;                        /* drawn off screen */
        gfx_fill(&g, 0, 0, w - 1, h - 1, PEN_BG);
        gfx_bigletter_draw(&g, ov.bl, ov.x - x0, ov.y - y0, PEN_DIM);
        g.rp = screen_rp;
        gfx_blit_beam(&g, ov_bm, x0, y0, w, h);
    } else {
        if (old)
            gfx_fill(&g, ox, oy, ox + old->w - 1, oy + old->h - 1, PEN_BG);
        ov_stamp();
    }
}

static void ov_hide(void)
{
    struct Page *p = ov.page;
    BOOL was = ov.on;
    ov.on = FALSE;
    if (!was || !p || p != cur() || p->type != PT_LIST || !g.win)
        return;
    if (has_aside(p)) {
        if (ov.bl)                  /* the column is the letter's alone: clear it */
            gfx_fill(&g, ov.x, ov.y, ov.x + ov.bl->w - 1, ov.y + ov.bl->h - 1, PEN_BG);
        preview_touch(p);           /* and the preview comes back */
        pv_due = time_ms();
    } else {
        draw_list_rows(p);          /* rows at full width again, strip cleared */
    }
}

/* One row of the list in its slot, background and all. Rows are painted
 * one by one over what was there, never after clearing the whole list,
 * so fast scrolling does not flash. */
static void draw_list_row(struct Page *p, LONG idx, WORD col)
{
    struct Item *it = &p->list.items[idx];
    WORD y = g.list_top + (idx - p->top) * g.list_row_h;
    WORD ty = y + (g.list_row_h - g.font_h) / 2, right = g.pane_x + g.pane_w - U(2);
    WORD lx = g.pane_x + col;
    WORD xr = list_xr(p);                   /* never into the letter's space */
    if (it->kind == IT_TEXT && p->setp)
        lx = g.pane_x;                      /* a Settings note: does nothing, so a heading */
    if (right > xr - U(2))
        right = xr - U(2);
    BOOL sel = idx == p->sel;
    BOOL off = (it->flags & ITEM_DISABLED) != 0;    /* greyed out */
    UBYTE fg = off ? PEN_DIM : sel ? PEN_WHITE : PEN_TEXT;
    UBYTE bg = sel ? (focus == FOCUS_CONTENT ? PEN_HL : PEN_SELDIM) :
               mouse_mode && idx - p->top == hover_row && p == cur() ? PEN_RULE : PEN_BG;
    /* Every list pen is below 8 and planes 3+ are clear in the list (the
     * pane is cleared on all planes when the page is drawn), so on native
     * screens only planes 0-2 are written: 3/8 of the blitter work. */
    if (g.planar)
        SetWriteMask(g.rp, 0x07);
    gfx_fill(&g, g.pane_x - U(2), y, xr, y + g.list_row_h - 1, bg);
    if (it->flags & ITEM_GONE) {            /* its letter is being read again */
        if (g.planar)
            SetWriteMask(g.rp, 0xFF);
        return;
    }
    if (it->mark != MARK_NONE)
        gfx_mark(&g, it->mark, g.pane_x, y + (g.list_row_h - g.mark_px) / 2,
                 off ? PEN_RULE : sel ? PEN_WHITE : it->mark == MARK_ARROW ? PEN_DIM : PEN_TEXT, bg);
    {
        /* the tag at the right: a setting's value; a picture's format or
         * a playable folder's picture count (pics.c keeps it in sortkey) */
        const char *tag = it->kind == IT_ACTION ? it->path : (p->pics || p->radio || it->kind == IT_PLEXDIR) ? it->sortkey : "";
        if (tag[0]) {
            WORD tagw = gfx_textlen(&g, tag, TS_SMALL);
            gfx_text(&g, right - tagw, ty, tag, off ? PEN_RULE : sel ? PEN_WHITE : PEN_DIM, bg, 0,
                     TS_SMALL);
            right -= tagw + U(8);
        }
    }
    gfx_text(&g, lx, ty, it->label, fg, bg, right - lx, TS_BODY);
    if (g.planar)
        SetWriteMask(g.rp, 0xFF);
}

/* The bar's place and its thumb's. */
static void gut_geom(struct Page *p, WORD *bx0, WORD *bx1, WORD *ty, WORD *th)
{
    LONG vis = list_visible(), n = p->list.count, h = g.footer_y - 2 - g.list_top + 1, t;
    *bx0 = list_edge(p) - GUT_W + 1 + U(2);
    *bx1 = *bx0 + U(5) - 1;
    t = h * vis / n;
    if (t < U(10))
        t = U(10);
    if (t > h)
        t = h;
    *th = (WORD)t;
    *ty = g.list_top + (WORD)(n > vis ? (h - t) * p->top / (n - vis) : 0);
}

static void draw_gutter(struct Page *p)
{
    WORD bx0, bx1, ty, th, y0 = g.list_top, y1 = g.footer_y - 2;
    LONG vis = list_visible();
    if (!list_overflows(p) || (!has_aside(p) && ov.on && ov.page == p))
        return;                             /* no column, or under the letter's strip */
    gut_geom(p, &bx0, &bx1, &ty, &th);
    gfx_fill(&g, list_edge(p) - GUT_W + 1, y0, list_edge(p), y1, PEN_BG);
    if (mouse_mode) {
        if (bar_up || gut.drag) {
            gfx_fill(&g, bx0, y0, bx1, y1, PEN_RULE);           /* the track */
            gfx_fill(&g, bx0, ty, bx1, ty + th - 1, gut.drag ? PEN_WHITE : PEN_TEXT);
        }
        /* the mouse at rest: no bar (the count at the top right says where) */
    } else {
        /* arrows: more above, more below */
        WORD ax = (bx0 + bx1) / 2, ah = U(3), i;
        for (i = 0; i < ah; i++) {
            WORD half = (i * U(3)) / ah;
            if (p->top > 0)
                gfx_fill(&g, ax - half, y0 + U(1) + i, ax + half, y0 + U(1) + i, PEN_DIM);
            if (p->top + vis < p->list.count)
                gfx_fill(&g, ax - half, y1 - U(1) - i, ax + half, y1 - U(1) - i, PEN_DIM);
        }
    }
}

static void draw_list_row(struct Page *p, LONG idx, WORD col);
static void draw_menu_row(int i);
static WORD mark_column(const struct Page *p);

/* Move the soft highlight: to screen row `row` of the list, or to main
 * menu entry `menu_i` (-1: neither). */
static void hover_set(WORD row, WORD menu_i)
{
    struct Page *p = cur();
    WORD old_row = hover_row, old_menu = hover_menu;
    if (row == hover_row && menu_i == hover_menu)
        return;
    hover_row = row;
    hover_menu = menu_i;
    if (pane_pending)
        return;
    if (p && p->type == PT_LIST) {
        WORD col = mark_column(p);
        if (old_row >= 0 && p->top + old_row < p->list.count)
            draw_list_row(p, p->top + old_row, col);
        if (row >= 0 && p->top + row < p->list.count)
            draw_list_row(p, p->top + row, col);
    }
    if (old_menu >= 0 && old_menu < nmenu)
        draw_menu_row(old_menu);
    if (menu_i >= 0 && menu_i < nmenu)
        draw_menu_row(menu_i);
}

/* The pointer at (x, y): which row, if any, is under it. */
static void hover_at(WORD x, WORD y)
{
    struct Page *p = cur();
    WORD row = -1, mi = -1;
    int c = p && !pane_pending ? crumb_at(x, y) : -1;
    if (c != crumb_hover) {
        crumb_hover = c;
        if (p && !pane_pending)
            draw_title(p);                  /* the link under the pointer, lit */
    }
    if (x < g.menu_w && y >= g.top && y < g.top + nmenu * g.row_h)
        mi = (y - g.top) / g.row_h;
    else if (p && p->type == PT_LIST && x >= g.pane_x - U(2) && x <= list_xr(p) &&
             y >= g.list_top && y < g.list_top + list_visible() * g.list_row_h) {
        row = (y - g.list_top) / g.list_row_h;
        if (p->top + row >= p->list.count)
            row = -1;
    }
    hover_set(row, mi);
}

static void pointer_hide(void)
{
    if (!g.win || ptr_hidden == g.win)
        return;
    if (!ptr_blank && !(ptr_blank = AllocMem(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR)))
        return;
    SetPointer(g.win, ptr_blank, 1, 16, 0, 0);
    ptr_hidden = g.win;
    hover_set(-1, -1);                      /* nothing under a pointer not there */
    if (crumb_hover >= 0) {
        crumb_hover = -1;
        if (cur() && !pane_pending)
            draw_title(cur());
    }
}

static void pointer_show(void)
{
    if (g.win && ptr_hidden == g.win)
        ClearPointer(g.win);
    ptr_hidden = NULL;
}

static BOOL np_on(void);
static void np_update(void);

static void set_mouse_mode(BOOL on)
{
    struct Page *p = cur();
    if (on == mouse_mode)
        return;
    mouse_mode = on;
    if (p && p->type == PT_LIST && !pane_pending)
        draw_gutter(p);
    if (!on)
        hover_set(-1, -1);                  /* the joystick: no pointer to follow */
    if (np_on())
        np_update();                        /* the yellow frame, or the mouse's light */
}

static void draw_list_rows(struct Page *p)
{
    WORD vis = list_visible(), r, col = mark_column(p);
    list_follow(p);
    if (!p->list.count) {
        /* the list's own place: beside it a pane (an album's cover, kept)
         * is the preview's to clear; filled over, the cover flashed in the
         * UI's colours and was gone while still counted as shown */
        gfx_fill(&g, g.pane_x - U(2), g.list_top, has_aside(p) ? aside_x() - 1 : g.w - 1,
                 g.footer_y - 2, PEN_BG);
        if (p->radio && p->net_seq) {
            rsp.on = FALSE;
            radio_spin();           /* still asking: the spinner, not "(empty)" */
            return;
        }
        gfx_text(&g, g.pane_x, g.list_top + U(2), "(empty)", PEN_DIM, PEN_BG, 0, TS_BODY);
        return;
    }
    for (r = 0; r < vis && p->top + r < p->list.count; r++)
        draw_list_row(p, p->top + r, col);
    gfx_fill(&g, g.pane_x - U(2), g.list_top + r * g.list_row_h, list_xr(p), g.footer_y - 2, PEN_BG);
    draw_gutter(p);
}

/* Copy n visible rows from screen row src to screen row dst, in place on
 * screen. Native screens: a blit inside the screen bitmap, masked to
 * planes 0-2 (every list pen is below 8 and rows are painted with
 * RectFill, so planes 3+ are zero there). RTG: the same blit, unmasked;
 * the RTG system handles the overlap and accelerates it. */
/* Right edge of what rows top..top+vis-1 have painted outside the bar:
 * the copy need go no further, since everything right of it is background
 * in every row. Measured each move (TextLength is cheap); list entries
 * stay read-only here. */
static WORD rows_right(struct Page *p, LONG top, WORD col)
{
    WORD vis = list_visible(), r, right = g.pane_x;
    for (r = 0; r < vis && top + r < p->list.count; r++) {
        struct Item *it = &p->list.items[top + r];
        WORD e;
        if (it->kind == IT_ACTION && it->path[0]) {
            right = g.w - 1;                /* right-aligned values: the whole row */
            break;
        }
        e = g.pane_x + col + gfx_textlen(&g, it->label, TS_BODY) + U(1);
        if (e > right)
            right = e;
    }
    if (right > list_xr(p))
        right = list_xr(p);                 /* the letter's space is never copied */
    return right;
}

static WORD copy_right = 0;     /* right edge for blit_rows, set per move */

static void blit_rows(LONG src, LONG dst, LONG n)
{
    WORD x0 = g.pane_x - U(2), x1 = copy_right > x0 ? copy_right : list_xr(cur());
    if (ov.on && x1 > ov.strip_x - 1)
        x1 = ov.strip_x - 1;                /* never into the letter's strip */
    struct BitMap *bm = g.rp->BitMap;
    if (n <= 0 || src == dst)
        return;
    gfx_dirty();
    LockLayerRom(g.rp->Layer);             /* the window's, or a double buffer's */
    BltBitMap(bm, x0, g.list_top + src * g.list_row_h, bm, x0, g.list_top + dst * g.list_row_h,
              x1 - x0 + 1, n * g.list_row_h, 0xC0, g.planar ? 0x07 : 0xFF, NULL);
    UnlockLayerRom(g.rp->Layer);
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
static void draw_list_move2(struct Page *p, LONG old_sel, LONG old_top);

static void draw_list_move(struct Page *p, LONG old_sel, LONG old_top)
{
    draw_list_move2(p, old_sel, old_top);
    if (p->top != old_top) {
        draw_gutter(p);                     /* the thumb or the arrows */
        if (hover_row >= 0 && p == cur()) {
            /* the copy took the soft highlight along with its row: back
             * under the pointer, gone from where it went */
            WORD vis = list_visible(), col = mark_column(p), moved = hover_row - (WORD)(p->top - old_top);
            if (moved >= 0 && moved < vis && p->top + moved < p->list.count)
                draw_list_row(p, p->top + moved, col);
            if (p->top + hover_row < p->list.count)
                draw_list_row(p, p->top + hover_row, col);
        }
    }
}

static void draw_list_move2(struct Page *p, LONG old_sel, LONG old_top)
{
    WORD vis = list_visible(), col;
    LONG k, i, first, last, r_old, n_old;
    list_follow(p);
    k = p->top - old_top;
    col = mark_column(p);
    if (!k || p->list.count <= vis) {
        if (k)
            draw_list_rows(p);
        else {
            if (old_sel >= p->top && old_sel < p->top + vis && old_sel < p->list.count)
                draw_list_row(p, old_sel, col);
            draw_list_row(p, p->sel, col);
        }
        return;
    }
    if (k >= vis || -k >= vis) {
        draw_list_rows(p);
        return;
    }
    copy_right = rows_right(p, old_top, col);   /* text extent of the rows being moved */
    r_old = old_sel - old_top;              /* the bar's screen row before the move */
    if (r_old < 0 || r_old >= vis)
        r_old = -1;
    if (k > 0) {                            /* rows move up: dst row d <- src row d + k */
        if (r_old < 0) {
            blit_rows(k, 0, vis - k);
        } else {
            blit_rows(k, 0, r_old - k);                     /* rows above the bar */
            blit_rows(r_old + 1, r_old + 1 - k, vis - 1 - r_old);   /* rows below it */
        }
        first = p->top + vis - k;
        last = p->top + vis - 1;
    } else {                                /* rows move down: dst row d <- src row d - |k| */
        LONG m = -k;
        if (r_old < 0) {
            blit_rows(0, m, vis - m);
        } else {
            blit_rows(r_old + 1, r_old + 1 + m, vis - 1 - r_old - m);   /* below the bar */
            blit_rows(0, m, r_old);                                     /* above it */
        }
        first = p->top;
        last = p->top + m - 1;
    }
    /* Order matters: plain rows first, the new highlight last, so the old
     * bar is gone before the new one appears (never two at once). */
    n_old = old_sel - p->top;               /* the row the old selection moved into */
    if (r_old >= 0 && n_old >= 0 && n_old < vis && old_sel < p->list.count && old_sel != p->sel)
        draw_list_row(p, old_sel, col);
    if (r_old >= 0 && p->top + r_old < p->list.count && p->top + r_old != p->sel)
        draw_list_row(p, p->top + r_old, col);     /* the row under the old bar */
    for (i = first; i <= last && i < p->list.count; i++)   /* rows scrolled into view */
        if (i != p->sel && i != p->top + r_old)
            draw_list_row(p, i, col);
    draw_list_row(p, p->sel, col);
}

/* (Re)build the page's text box below y0, keeping its scroll position. */
static void np_draw(BOOL full);

/* A text page with a picture (the credits): the text scrolls in a box at
 * the top; under it, fixed, the note and the picture, as large as fits in
 * up to two fifths of the pane. The box's height (the picture made first,
 * at its size). */
static WORD text_box_h(struct Page *p, WORD y0, WORD *note_lines)
{
    WORD avail = g.footer_y - 2 - y0, lh = g.font_h + U(1), note_h = 0, ph;
    *note_lines = 0;
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

static void open_textbox(struct Page *p, WORD y0)
{
    WORD lines;
    p->tb.pic = NULL;                       /* the picture is under the box, not in it */
    tb_open(&g, &p->tb, g.pane_x, y0, textbox_w(), text_box_h(p, y0, &lines), &p->list,
            p->type == PT_DETAIL);
}

/* The note and the picture under a text page's box. */
static void draw_text_picture(struct Page *p)
{
    WORD lines, lh = g.font_h + U(1), y, i;
    WORD box_h = text_box_h(p, g.list_top, &lines);
    if (!p->pic[0])
        return;
    /* a rule under the text, as the title has one over it */
    gfx_fill(&g, g.pane_x, g.list_top + box_h + U(3), g.pane_x + g.pane_w - 1,
             g.list_top + box_h + U(4) - 1, PEN_RULE);
    y = g.list_top + box_h + U(8);
    if (p->note && lines) {
        struct ItemList l;
        if (list_init(&l)) {
            wrap_into(&l, p->note, textbox_w());
            for (i = 0; i < l.count; i++)
                gfx_text(&g, g.pane_x, y + i * lh, l.items[i].label, PEN_DIM, PEN_BG, textbox_w(),
                         TS_BODY);
            list_free(&l);
        }
        y += lines * lh + U(4);
    }
    if (p->pic_bm && y + p->pic_h <= g.footer_y - 2) {
        p->pic_x = g.pane_x + (textbox_w() - p->pic_w) / 2;
        p->pic_y = y;
        gfx_blit_beam(&g, p->pic_bm, p->pic_x, p->pic_y, p->pic_w, p->pic_h);
    }
}

static struct TextBox *cur_tb(void)
{
    struct Page *p = pane_pending ? NULL : cur();   /* a stale section is not shown */
    return (p && p->type != PT_LIST && p->tb.open) ? &p->tb : NULL;
}

static void draw_pane(BOOL full)
{
    struct Page *p = cur();
    BOOL took = FALSE;                      /* a cover moved into the pane: its text now too */
    if (!p)
        return;
    if (full) {
        pv_shown = FALSE;           /* the pane is cleared: preview again */
        tb_close(&pv_tb);
        WORD px = g.menu_w + U(3), pb = g.footer_y - 2;
        if (p->type == PT_TEXT) {
            /* Clear around the text box, not under it: tb_draw copies the
             * new text straight over the old, so there is no blank frame. */
            struct TextBox *tb = &p->tb;
            WORD bx1;
            open_textbox(p, g.list_top);
            bx1 = tb->bar_x + U(3) - 1;
            gfx_fill_bg(&g, px, U(25), g.w - 1, U(26) - 1);   /* the title band: draw_title */
            gfx_fill_bg(&g, px, U(26), g.pane_x - U(2) - 1, U(42) - 1);
            gfx_fill_bg(&g, px, U(42), g.w - 1, tb->y - 1);
            gfx_fill_bg(&g, px, tb->y, tb->x - 1, tb->y + tb->h - 1);
            gfx_fill_bg(&g, bx1 + 1, tb->y, g.w - 1, tb->y + tb->h - 1);
            gfx_fill_bg(&g, px, tb->y + tb->h, g.w - 1, pb);
        } else if (p->type != PT_PLAYER) {          /* the player clears its own */
            gfx_fill_bg(&g, px, U(25), g.w - 1, U(26) - 1);   /* the title band: draw_title */
            gfx_fill_bg(&g, px, U(26), g.pane_x - U(2) - 1, U(42) - 1);
            if (g.pic_w && p->type == PT_LIST && p->plex && has_aside(p) && !plex_keeps(p, p->sel))
                took = plex_take(p);    /* back from Now playing: its cover into the pane, now */
            if (g.pic_w && p->type == PT_LIST && has_aside(p) && g.pic_x >= aside_x() &&
                g.pic_y >= U(42)) {
                /* a picture in the preview's place: left there, for the
                 * preview to keep (the same one) or clear; filled over, it
                 * flashed in the UI's colours plane by plane */
                WORD ax = aside_x(), py1 = g.pic_y + g.pic_h - 1;
                BOOL text = plex_text_kept(p);  /* an album's details: the same, left */
                gfx_fill_bg(&g, px, U(42), ax - 1, pb);
                gfx_fill_bg(&g, ax, U(42), g.w - 1, g.pic_y - 1);
                gfx_fill_bg(&g, ax, g.pic_y, g.pic_x - 1, py1);
                gfx_fill_bg(&g, g.pic_x + g.pic_w, g.pic_y, g.w - 1, py1);
                if (!text) {
                    gfx_fill_bg(&g, ax, py1 + 1, g.w - 1, pb);
                    plex_shown[0] = 0;
                }
            } else {
                if (g.pic_w)                /* cleared first, then its pens go */
                    gfx_picture_clear(&g, px, U(42), g.w - 1, pb);
                gfx_fill_bg(&g, px, U(42), g.w - 1, pb);
            }
        }
    }
    draw_title(p);
    switch (p->type) {
    case PT_LIST:
        draw_list_rows(p);          /* (a Radio list still asking: its spinner) */
        if (took)
            preview_draw(p);        /* its details with it, not when the pane settles */
        break;
    case PT_TEXT:
        if (!p->tb.open)
            open_textbox(p, g.list_top);
        if (full || !p->tb.drawn)
            tb_draw(&g, &p->tb);    /* a focus change alone leaves the box alone */
        if (full)
            draw_text_picture(p);   /* the credits' AGS logo, under the box */
        break;
    case PT_PLAYER:
        np_draw(full);
        break;
    case PT_DETAIL:
        if (full) {
            char shot[300];
            /* screenshot box: AGS's are 320 x 128 lowres, so 128 design
             * lines shows them at their own size where the pane is wide
             * enough (lowres); 85 elsewhere, twice that on laced screens */
            WORD ph = g.w < 640 ? U(128) : (g.k == 256 && g.h >= 400) ? 170 : U(85), dh = 0;
            p->facts_y = g.list_top;
            if ((g.pens_shared || g.truecolor) && ags_screenshot_path(p->base, shot, sizeof(shot))) {
                set_msg("Loading screenshot...");
                if (gfx_picture(&g, shot, g.pane_x, g.list_top, g.pane_w, ph, TRUE, &dh))
                    p->facts_y = g.list_top + dh + U(6);
                set_msg(NULL);
            }
            open_textbox(p, p->facts_y);
        }
        if (full || !p->tb.drawn)
            tb_draw(&g, &p->tb);
        break;
    }
}

static void set_focus(int f);

/* ------------------------------------------------------------ music
 * Mods: the folders of modules (mods.c) and the Now Playing page
 * (PT_PLAYER), the music itself on Paula (modplay.c). The music plays on
 * wherever AmigaMCE goes, until it is stopped or a game starts; at the end
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
#define MODS_ROOT  "ST-00:Modules"
#define MODS_MENU  "AGS:Mods"
#define MODS_SID   "ST-00:C64Music"
#define VOL_STEP   4

enum { NB_PREV, NB_BACK, NB_PLAY, NB_STOP, NB_FWD, NB_NEXT, NB_VDOWN, NB_VUP, NB_COUNT };

static struct {
    struct ItemList list;       /* the playlist: the modules of the folder played from */
    LONG idx;                   /* the one loaded */
    char folder[64];
    int btn;                    /* the button chosen with the keyboard or joystick */
    BOOL ended_seen;            /* the song's end acted on */
    /* where things are on the page */
    WORD x0, w, title_y, info_y, prog_y, prog_h, time_y, meter_y, meter_h, name_w;
    WORD bx[NB_COUNT], by, bw, bh, vol_x, samples_y;
    WORD art_x, art_y, art_s;               /* a Plex track's cover: its square (0 none) */
    WORD bar_w, pos_y;                      /* the bar's width (the time right of it); the
                                             * position line, under the buttons */
    LONG seek_to;                           /* a track's seek waiting (ms, -1 none) */
    ULONG seek_due;
    /* what is drawn there now: only changes are drawn again */
    LONG s_ms, s_total, s_order, s_row, s_fill;
    WORD s_level[4];
    UBYTE s_sample[4];
    int s_state, s_vol, s_btn, s_flash, s_steps;
    BOOL s_focus;
    int flash;                  /* the button just pressed (-1 none): lit briefly */
    ULONG flash_until;
    int hov, s_hov;             /* what the mouse is over: a button, NB_SLIDER, -1 */
    int rep, rep_src, s_rep;    /* the button held down, repeating (-1 none) */
    ULONG rep_next;
    BOOL sl_on, sl_drag, s_slhot; /* the volume slider (when there is room) */
    WORD sl_x0, sl_x1;
    int s_meters;               /* level meters drawn (4, 2, 1) */
    int rows;                   /* ...and laid out for: the page as tall as they need */
    ULONG s_rseq;               /* a station: its title and state drawn */
    int s_rstate;
} np = { .idx = -1, .btn = NB_PLAY, .flash = -1, .s_flash = -1, .hov = -1, .s_hov = -1,
         .rep = -1, .s_rep = -1 };

#define NB_SLIDER  NB_COUNT         /* np.hov: the slider */
#define FLASH_MS   80               /* how long a pressed button stays lit */
#define REP_DELAY  400              /* a button held: it repeats after this, */
#define REP_EVERY  100              /* then this often */
enum { REP_MOUSE, REP_JOY };

/* The button Fire presses (chosen with Left/Right) has a yellow rounded
 * frame; with the mouse there is none: what it is over lights up instead. */
static BOOL np_focus_shown(void)
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

/* A rounded frame t thick just inside x,y w x h (or, solid, the corners
 * the rounding takes off the button). */
/* A rounded rectangle filled: its corners a quarter ellipse of rx pixels
 * across and ry rows down (on a screen of tall pixels ry is half rx, so
 * they look round). */
static WORD isqrt_round(LONG v)
{
    LONG r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return (WORD)(v - r * r > r ? r + 1 : r);  /* to the nearer */
}

static void np_rrect(WORD x, WORD y, WORD w, WORD h, WORD rx, WORD ry, UBYTE pen)
{
    WORD k;
    if (w <= 0 || h <= 0)
        return;
    if (ry * 2 > h)
        ry = h / 2;
    for (k = 0; k < ry; k++) {
        /* row k from the top (and from the bottom): its inset */
        LONG dy = (LONG)(ry - k) * rx;      /* in x units: rows scaled to rx */
        WORD in = rx - isqrt_round((LONG)rx * rx - (dy / ry) * (dy / ry));
        if (in < 0)
            in = 0;
        if (in * 2 >= w)
            in = (w - 1) / 2;
        gfx_fill(&g, x + in, y + k, x + w - 1 - in, y + k, pen);
        gfx_fill(&g, x + in, y + h - 1 - k, x + w - 1 - in, y + h - 1 - k, pen);
    }
    if (h - 2 * ry > 0)
        gfx_fill(&g, x, y + ry, x + w - 1, y + h - 1 - ry, pen);
}

static BOOL np_on(void)
{
    struct Page *p = pane_pending ? NULL : cur();
    return p && p->type == PT_PLAYER;
}

/* The page is up and something on it moves. */
static BOOL np_live(void)
{
    return np_on() && mp_running();
}

static void time_text(ULONG ms, char *out)
{
    ULONG s = ms / 1000;
    sprintf(out, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* A triangle w x h pointing right (dir 1) or left (-1), row by row. */
/* A triangle pointing right (dir > 0) or left, h rows (odd: one row for
 * its tip), each row sx pixels wider than the last to the middle: clean
 * 45-degree edges in pixels, as wide as it is tall on screen (sx 2 where
 * pixels are tall). Its width: (h + 1) / 2 * sx. */
static void np_tri(WORD x, WORD y, WORD h, WORD sx, int dir, UBYTE pen)
{
    WORD r, w = (h + 1) / 2 * sx;
    for (r = 0; r < h; r++) {
        WORD d = r < h - 1 - r ? r : h - 1 - r, len = (d + 1) * sx;
        if (dir > 0)
            gfx_fill(&g, x, y + r, x + len - 1, y + r, pen);
        else
            gfx_fill(&g, x + w - len, y + r, x + w - 1, y + r, pen);
    }
}

static int np_state(void)
{
    return !mp_loaded() ? 0 : mp_playing() ? 1 : mp_paused() ? 2 : 3;
}

static void np_button(int i)
{
    BOOL lit = i == np.flash || i == np.rep;    /* just pressed, or held */
    BOOL ring = np_focus_shown() && i == np.btn;
    BOOL hov = mouse_mode && i == np.hov;       /* the mouse over it */
    UBYTE bg = lit ? PEN_HL : hov ? PEN_SELDIM : PEN_BAR, fg = lit || hov ? PEN_WHITE : PEN_TEXT;
    WORD x = np.bx[i], y = np.by, w = np.bw, h = np.bh;
    BOOL tall = gfx_mode_tall(g.mode);
    WORD sx = tall ? 2 : 1;                     /* pixels across for one down, to look square */
    WORD gh, tw, cx, cy, gy, t, rx, ry;
    if (i == NB_VDOWN || i == NB_VUP) {
        if ((i == NB_VDOWN && mp_volume() == 0) || (i == NB_VUP && mp_volume() >= 64))
            fg = lit ? PEN_TEXT : PEN_DIM;
    }
    if ((i == NB_BACK || i == NB_FWD) && !mp_can_step(i == NB_FWD ? 1 : -1)) {
        bg = PEN_BAR;                       /* not for this module: greyed, never lit */
        fg = PEN_RULE;
    }
    /* the glyph: an odd number of rows, about half the button, so each
     * triangle has a one-row tip; everything else measured from it */
    gh = (h * 3 / 5) | 1;
    if (gh < 7)
        gh = 7;
    tw = (gh + 1) / 2 * sx;                 /* a triangle's width */
    t = gh / 5 > 2 ? gh / 5 : 2;            /* a bar's thickness, in rows */
    cx = x + w / 2;
    cy = y + h / 2;
    gy = cy - gh / 2;
    /* the button: rounded by about a fifth of its height; the focus ring
     * (yellow) is its outer t rows, the face inside it */
    ry = h / 5 > 2 ? h / 5 : 2;
    if (tall)
        ry = ry / 2 > 1 ? ry / 2 : 1;
    rx = tall ? ry * 2 : ry;
    gfx_fill(&g, x, y, x + w - 1, y + h - 1, PEN_BG);
    if (ring) {
        WORD ft = U(2) > 1 ? U(2) : 1, fx = ft * sx;
        np_rrect(x, y, w, h, rx, ry, PEN_YELLOW);
        np_rrect(x + fx, y + ft, w - 2 * fx, h - 2 * ft, rx > fx ? rx - fx : 1,
                 ry > ft ? ry - ft : 1, bg);
    } else {
        np_rrect(x, y, w, h, rx, ry, bg);
    }
    switch (i) {
    case NB_PREV: {
        WORD bw = t * sx, x0 = cx - (bw + tw) / 2;
        gfx_fill(&g, x0, gy, x0 + bw - 1, gy + gh - 1, fg);
        np_tri(x0 + bw, gy, gh, sx, -1, fg);
        break;
    }
    case NB_NEXT: {
        WORD bw = t * sx, x0 = cx - (bw + tw) / 2;
        np_tri(x0, gy, gh, sx, 1, fg);
        gfx_fill(&g, x0 + tw, gy, x0 + tw + bw - 1, gy + gh - 1, fg);
        break;
    }
    case NB_BACK:
        np_tri(cx - tw, gy, gh, sx, -1, fg);
        np_tri(cx, gy, gh, sx, -1, fg);
        break;
    case NB_FWD:
        np_tri(cx - tw, gy, gh, sx, 1, fg);
        np_tri(cx, gy, gh, sx, 1, fg);
        break;
    case NB_PLAY:
        if (mp_playing()) {
            /* pause: two bars, a bar's width apart, as wide in all as a
             * triangle is */
            WORD bw = gh / 3 * sx, x0 = cx - (bw * 3) / 2;
            gfx_fill(&g, x0, gy, x0 + bw - 1, gy + gh - 1, fg);
            gfx_fill(&g, x0 + bw * 2, gy, x0 + bw * 3 - 1, gy + gh - 1, fg);
        } else {
            np_tri(cx - tw / 2, gy, gh, sx, 1, fg);
        }
        break;
    case NB_STOP: {
        WORD sh = gh - 2, sw = sh * sx;     /* a square, a little under the triangles */
        gfx_fill(&g, cx - sw / 2, cy - sh / 2, cx - sw / 2 + sw - 1, cy - sh / 2 + sh - 1, fg);
        break;
    }
    case NB_VDOWN:
    case NB_VUP: {
        WORD len = gh, lw = len * sx, th = t > 2 ? t - 1 : 2, tx = th * sx;
        gfx_fill(&g, cx - lw / 2, cy - th / 2, cx - lw / 2 + lw - 1, cy - th / 2 + th - 1, fg);
        if (i == NB_VUP)
            gfx_fill(&g, cx - tx / 2, cy - len / 2, cx - tx / 2 + tx - 1, cy - len / 2 + len - 1, fg);
        break;
    }
    }
}

static void np_volume_text(void)
{
    char m[24];
    WORD x = np.vol_x, y = np.by + (np.bh - g.font_h) / 2, end = np.x0 + np.w - 1;
    if (x < 0)
        return;
    sprintf(m, "Volume %ld%%", (long)(mp_volume() * 100 / 64));
    gfx_text(&g, x, y, m, PEN_DIM, PEN_BG, 0, TS_SMALL | TS_OPAQUE);
    x += gfx_textlen(&g, m, TS_SMALL);
    if (x <= end)
        gfx_fill(&g, x, y, end, y + g.font_h - 1, PEN_BG);
}

/* The volume slider: -----*----- between - and +. */
static WORD np_knob_w(void)
{
    WORD k = U(3) > 2 ? U(3) : 2;
    return gfx_mode_tall(g.mode) ? k * 2 : k;
}

static WORD np_knob_x(int v)
{
    return np.sl_x0 + (WORD)((LONG)(np.sl_x1 - np.sl_x0 + 1 - np_knob_w()) * v / 64);
}

static BOOL np_slider_hot(void)
{
    return np.sl_drag || (mouse_mode && np.hov == NB_SLIDER);
}

static void np_slider(void)
{
    WORD y = np.by, h = np.bh, kw = np_knob_w(), kx, th, ty, kh;
    BOOL hot = np_slider_hot();
    if (!np.sl_on)
        return;
    kx = np_knob_x(mp_volume());
    th = U(2) > 1 ? U(2) : 1;
    ty = y + (h - th) / 2;
    kh = h * 6 / 10;
    gfx_fill(&g, np.sl_x0, y, np.sl_x1, y + h - 1, PEN_BG);
    if (kx > np.sl_x0)                      /* the part up to the volume */
        gfx_fill(&g, np.sl_x0, ty, kx - 1, ty + th - 1, hot ? PEN_TEXT : PEN_DIM);
    if (kx + kw <= np.sl_x1)                /* the rest */
        gfx_fill(&g, kx + kw, ty, np.sl_x1, ty + th - 1, PEN_RULE);
    gfx_fill(&g, kx, y + (h - kh) / 2, kx + kw - 1, y + (h - kh) / 2 + kh - 1,
             hot ? PEN_WHITE : PEN_TEXT);
}

/* The volume for a point on the slider. */
static int np_slider_vol(WORD x)
{
    WORD span = np.sl_x1 - np.sl_x0 + 1 - np_knob_w();
    LONG v;
    if (span < 1)
        return mp_volume();
    v = ((LONG)(x - np.sl_x0 - np_knob_w() / 2) * 64 + span / 2) / span;
    return v < 0 ? 0 : v > 64 ? 64 : (int)v;
}

static void np_layout(void)
{
    WORD row = g.font_h + U(3), gap = U(3), vt, i, avail;
    np.x0 = g.pane_x;
    np.w = g.pane_w - U(2);
    np.title_y = g.list_top + U(2);
    np.info_y = np.title_y + row + U(1);
    /* the meters, then the bar with the time right of it, directly above
     * the buttons (back and forward next to where they move to), then
     * the position line */
    np.meter_y = np.info_y + row + U(6);
    np.meter_h = g.font_h + U(2);
    np.name_w = np.w * 2 / 5;
    np.rows = mp_loaded() ? mp_meter_count() : 4;
    np.time_y = np.meter_y + np.rows * (np.meter_h + U(2)) + U(4);
    np.prog_h = U(4) > 2 ? U(4) : 2;
    np.prog_y = np.time_y + (g.font_h - np.prog_h) / 2;
    np.bar_w = np.w - gfx_textlen(&g, "88:88 / 88:88", TS_BODY) - U(10);
    np.by = np.time_y + row + U(4);
    np.bh = g.font_h + U(8);
    vt = gfx_textlen(&g, "Volume 100%", TS_SMALL) + U(6);
    avail = np.w - vt;                      /* buttons, gaps, and two more before volume */
    np.bw = (avail - (NB_COUNT + 1) * gap) / NB_COUNT;
    np.vol_x = 0;
    if (np.bw < U(14)) {                    /* narrow: the buttons take the row */
        np.bw = (np.w - (NB_COUNT + 1) * gap) / NB_COUNT;
        np.vol_x = -1;
    }
    if (np.bw > U(26))
        np.bw = U(26);
    for (i = 0; i <= NB_VDOWN; i++)
        np.bx[i] = np.x0 + i * (np.bw + gap) + (i >= NB_VDOWN ? gap * 2 : 0);
    {
        /* room enough: a slider between - and + */
        WORD at = np.bx[NB_VDOWN] + np.bw + gap;
        WORD room = np.x0 + np.w - at - np.bw - (np.vol_x >= 0 ? vt : 0) - gap;
        np.sl_on = room >= U(40);
        if (np.sl_on) {
            if (room > U(120))
                room = U(120);
            np.sl_x0 = at + U(2);
            np.sl_x1 = at + room - 1 - U(2);
            np.bx[NB_VUP] = at + room + gap;
        } else {
            np.bx[NB_VUP] = at;
        }
        np.sl_drag = FALSE;
    }
    if (np.vol_x >= 0)
        np.vol_x = np.bx[NB_VUP] + np.bw + U(6);
    np.pos_y = np.by + np.bh + U(5);
    np.samples_y = np.pos_y + row + U(2);
    np.art_s = 0;
    if (mp_is_track() && (!np_local() || np_local_art)) {
        /* the cover: a square at the right of the track's lines */
        WORD s = g.footer_y - 2 - np.samples_y - U(2);
        WORD cap = plex_tier() == 0 ? 160 : plex_tier() == 1 ? 224 : 400;
        if (s > np.w * 2 / 5)
            s = np.w * 2 / 5;
        if (s > cap)
            s = cap;                        /* less to decode on a slower Amiga */
        if (s >= U(40)) {
            np.art_s = s;
            np.art_x = np.x0 + np.w - s;
            np.art_y = np.samples_y;
        }
    }
}

/* The time, the bar and the position: only what changed. */
static void np_draw_progress(BOOL full, const struct MpStatus *st)
{
    char t[64], a[24], b[24], pos[96];
    WORD x = np.x0, y = np.time_y, end = np.x0 + np.w - 1, bw = np.bar_w, lw, rw;
    ULONG at = np.seek_due ? (ULONG)np.seek_to : st->elapsed_ms;   /* a seek: where to */
    LONG fill = st->total_ms ? (LONG)((ULONG)bw * (at / 100) / ((st->total_ms + 99) / 100)) : 0;
    if (fill > bw)
        fill = bw;
    if (full || fill != np.s_fill) {
        if (fill > 0)
            gfx_fill(&g, x, np.prog_y, x + fill - 1, np.prog_y + np.prog_h - 1, PEN_ACCENT);
        if (fill < bw)
            gfx_fill(&g, x + fill, np.prog_y, x + bw - 1, np.prog_y + np.prog_h - 1, PEN_RULE);
        np.s_fill = fill;
    }
    if (!full && at / 1000 == (ULONG)np.s_ms / 1000 && st->order == np.s_order &&
        st->row == np.s_row && (LONG)st->total_ms == np.s_total)
        return;
    np.s_ms = at;
    np.s_total = st->total_ms;
    np.s_order = st->order;
    np.s_row = st->row;
    time_text(at, a);
    time_text(st->total_ms, b);
    if (st->total_ms)
        sprintf(t, "%s / %s", a, b);
    else
        strcpy(t, a);                       /* its length is not known */
    if (st->songlen && st->pattern >= 0)
        sprintf(pos, "Position %ld of %ld   Pattern %ld   Row %ld", (long)st->order + 1,
                (long)st->songlen, (long)st->pattern, (long)st->row);
    else if (st->songlen)
        sprintf(pos, "Position %ld of %ld", (long)st->order + 1, (long)st->songlen);
    else if (st->pattern >= 0 && mp_running())
        sprintf(pos, "Position %ld   Pattern %ld   Row %ld", (long)st->order + 1,
                (long)st->pattern, (long)st->row);     /* mixed: its length in positions unknown */
    else
        pos[0] = 0;
    if (mp_is_stream() && mp_running()) {
        struct StInfo in;
        st_info(&in);
        if (in.state == ST_PLAYING) {
            sprintf(pos, "Buffered %ld.%ld s", (long)(in.buffer_ms / 1000),
                    (long)(in.buffer_ms % 1000 / 100));
            if (in.underruns) {
                sprintf(a, "   dropouts %ld", (long)in.underruns);
                amce_strlcat(pos, a, sizeof(pos));
            }
        } else {
            amce_strlcpy(pos, in.state == ST_CONNECTING ? "Connecting..." :
                              in.state == ST_BUFFERING ? "Buffering..." : "", sizeof(pos));
        }
    }
    /* the time right of the bar, right-aligned */
    lw = gfx_textlen(&g, t, TS_BODY);
    if (x + bw + U(4) <= end - lw)
        gfx_fill(&g, x + bw, y, end - lw, y + g.font_h - 1, PEN_BG);
    gfx_text(&g, end - lw + 1, y, t, np.seek_due ? PEN_WHITE : PEN_TEXT, PEN_BG, 0,
             TS_BODY | TS_OPAQUE);
    /* the position (or the buffer) on its own line under the buttons */
    rw = gfx_textlen(&g, pos, TS_SMALL);
    if (rw > np.w)
        rw = np.w;
    if (rw)
        gfx_text(&g, x, np.pos_y, pos, PEN_DIM, PEN_BG, np.w, TS_SMALL | TS_OPAQUE);
    if (x + rw <= end)
        gfx_fill(&g, x + rw, np.pos_y, end, np.pos_y + g.font_h - 1, PEN_BG);
}

static void np_draw_meters(BOOL full, const struct MpStatus *st)
{
    int i, n = mp_meter_count();
    WORD bx = np.x0 + np.name_w + U(4), bw = np.x0 + np.w - bx, bh = np.meter_h - U(4);
    if (bh < 2)
        bh = 2;
    if (n != np.s_meters) {
        /* fewer meters than last time (stereo, mono): the rest cleared */
        gfx_fill(&g, np.x0, np.meter_y, np.x0 + np.w - 1,
                 np.meter_y + np.rows * (np.meter_h + U(2)) - 1, PEN_BG);
        np.s_meters = n;
        full = TRUE;
    }
    if (n > np.rows)
        n = np.rows;                        /* laid out for fewer: np_update lays it out again */
    for (i = 0; i < n; i++) {
        WORD y = np.meter_y + i * (np.meter_h + U(2)), by = y + (np.meter_h - bh) / 2;
        WORD lit = (WORD)((LONG)bw * st->level[i] / 64);
        if (n < 4) {
            /* a channel of the sound, not of Paula: Left and Right, or Mono */
            if (full) {
                gfx_fill(&g, np.x0, y, np.x0 + np.name_w + U(3), y + np.meter_h - 1, PEN_BG);
                gfx_text(&g, np.x0, y + (np.meter_h - g.font_h) / 2,
                         n == 1 ? "Mono" : i ? "Right" : "Left", PEN_DIM, PEN_BG, np.name_w,
                         TS_SMALL);
            }
        } else if (full || st->sample[i] != np.s_sample[i]) {
            char name[40];
            BOOL used;
            const char *sn = st->sample[i] ? mp_sample_name(st->sample[i] - 1, &used) : "";
            if (st->sample[i])
                sprintf(name, "%ld  %s", (long)st->sample[i], sn);
            else
                name[0] = 0;
            gfx_fill(&g, np.x0, y, np.x0 + np.name_w + U(3), y + np.meter_h - 1, PEN_BG);
            if (name[0])
                gfx_text(&g, np.x0, y + (np.meter_h - g.font_h) / 2, name, PEN_DIM, PEN_BG,
                         np.name_w, TS_SMALL);
            np.s_sample[i] = st->sample[i];
        }
        if (full || lit != np.s_level[i]) {
            if (lit > 0)
                gfx_fill(&g, bx, by, bx + lit - 1, by + bh - 1, i == 0 || i == 3 ? PEN_ACCENT : PEN_HL);
            if (lit < bw)
                gfx_fill(&g, bx + lit, by, bx + bw - 1, by + bh - 1, PEN_RULE);
            np.s_level[i] = lit;
        }
    }
}

/* A station's page: its name; what plays now; what it is and how it is
 * played (in the place of a module's samples). */
/* A line of the page drawn over what was there (opaque, the rest of the
 * line filled): drawn again unchanged, nothing on screen changes. */
static void np_line(WORD y, const char *s, UBYTE fg, int ts)
{
    WORD x1 = np.x0 + np.w, w = gfx_textlen(&g, s, ts);
    if (w > np.w)
        w = np.w;
    gfx_text(&g, np.x0, y, s, fg, PEN_BG, np.w, ts | TS_OPAQUE);
    if (np.x0 + w < x1)
        gfx_fill(&g, np.x0 + w, y, x1 - 1, y + g.font_h - 1, PEN_BG);
}

/* label, then value at the column after the longest label (Playback). */
static void np_pair(WORD y, const char *label, const char *value)
{
    WORD col = gfx_textlen(&g, "Playback", TS_SMALL) + U(8), w;
    WORD tw = np.art_s ? np.w - np.art_s - U(8) : np.w, x1 = np.x0 + tw;
    gfx_text(&g, np.x0, y, label, PEN_DIM, PEN_BG, col, TS_SMALL | TS_OPAQUE);
    w = gfx_textlen(&g, label, TS_SMALL);
    if (w < col)
        gfx_fill(&g, np.x0 + w, y, np.x0 + col - 1, y + g.font_h - 1, PEN_BG);
    w = gfx_textlen(&g, value, TS_SMALL);
    if (w > tw - col)
        w = tw - col;
    gfx_text(&g, np.x0 + col, y, value, PEN_TEXT, PEN_BG, tw - col, TS_SMALL | TS_OPAQUE);
    if (np.x0 + col + w < x1)
        gfx_fill(&g, np.x0 + col + w, y, x1 - 1, y + g.font_h - 1, PEN_BG);
}

/* "MP3 320 kbit/s, 44100 Hz stereo", what of it is known. */
static void np_audio_line(WORD y, const char *label, const char *codec, ULONG kbps, ULONG rate,
                          ULONG ch)
{
    char v[96], part[32], up[16];
    int i;
    for (i = 0; codec[i] && i < 15; i++)
        up[i] = codec[i] >= 'a' && codec[i] <= 'z' ? codec[i] - 32 : codec[i];
    up[i] = 0;
    amce_strlcpy(v, up, sizeof(v));
    if (kbps) {
        sprintf(part, "%s%lu kbit/s", v[0] ? " " : "", (unsigned long)kbps);
        amce_strlcat(v, part, sizeof(v));
    }
    if (rate) {
        sprintf(part, "%s%lu Hz", v[0] ? ", " : "", (unsigned long)rate);
        amce_strlcat(v, part, sizeof(v));
    }
    if (ch) {
        sprintf(part, " %s", ch == 1 ? "mono" : ch == 2 ? "stereo" : "multichannel");
        amce_strlcat(v, part, sizeof(v));
    }
    np_pair(y, label, v);
}

/* A station's part of the page: what it is, and what is on air. Its
 * state and title change while it plays (connecting, buffering, a new
 * song), so it is drawn line by line over itself, never cleared first. */
static void np_draw_station(void)
{
    struct StInfo in;
    char info[320], part[160];
    const char *now = st_title();
    WORD lh = g.font_h + U(1), y = np.samples_y, bottom = g.footer_y - 2;
    st_info(&in);
    if (np_local()) {
        /* a music file: its name, folder, where in it, what it is, how it plays */
        ULONG len = st_length_ms(), kbps = in.bitrate;
        np_line(np.title_y, mp_stream_name()[0] ? mp_stream_name() : "(no title)", PEN_WHITE,
                TS_BOLD);
        amce_strlcpy(info, mp_track_by(), sizeof(info));
        if (np.list.pool && np.idx >= 0 && np.list.count > 1) {
            sprintf(part, "   Track %ld of %ld", (long)np.idx + 1, (long)np.list.count);
            amce_strlcat(info, part, sizeof(info));
        }
        np_line(np.info_y, info, PEN_DIM, TS_SMALL);
        if (y + lh * 3 > bottom)
            return;
        np_pair(y, "File", FilePart((STRPTR)mp_path()));
        y += lh;
        if (!kbps && len >= 1000 && st_size())
            kbps = st_size() / (len / 8);   /* FLAC: bytes / (ms / 8) = kbit/s */
        if (!mp_running())
            np_pair(y, "Format", st_state() == ST_FAILED && st_error()[0] ? st_error() : "Stopped");
        else if (!in.freq)
            np_pair(y, "Format", "Reading...");
        else
            np_audio_line(y, "Format", mp_tag(), kbps, in.freq, in.channels);
        y += lh;
        if (mp_running() && in.dec_freq)
            np_audio_line(y, "Playback", "", 0, in.dec_freq, in.dec_channels);
        else
            np_pair(y, "Playback", "");
        y += lh;
        if (y <= bottom)                    /* the picture's square left alone */
            gfx_fill(&g, np.x0, y, np.x0 + (np.art_s ? np.w - np.art_s - U(8) : np.w) - 1, bottom,
                     PEN_BG);
        return;
    }
    if (mp_is_track()) {
        /* a Plex track: its title, artist and album, where in the list, how it plays */
        np_line(np.title_y, mp_stream_name()[0] ? mp_stream_name() : "(no title)", PEN_WHITE,
                TS_BOLD);
        amce_strlcpy(info, mp_track_by(), sizeof(info));
        if (np.list.pool && np.idx >= 0 && np.list.count > 1) {
            /* the album is named already: only where in it */
            sprintf(part, "   Track %ld of %ld", (long)np.idx + 1, (long)np.list.count);
            amce_strlcat(info, part, sizeof(info));
        }
        np_line(np.info_y, info, PEN_DIM, TS_SMALL);
        if (y + lh * 3 > bottom)
            return;
        /* the file as stored, as the server sends it, as the Amiga plays it */
        np_audio_line(y, "Original", plex_orig.codec, plex_orig.kbps, plex_orig.rate,
                      plex_orig.ch);
        y += lh;
        if (!mp_running())
            np_pair(y, "Plex", st_state() == ST_FAILED && st_error()[0] ? st_error() : "Stopped");
        else if (in.state == ST_CONNECTING)
            np_pair(y, "Plex", "Connecting...");
        else if (in.state == ST_BUFFERING)
            np_pair(y, "Plex", "Buffering...");
        else if (!plex_orig.conv)
            np_pair(y, "Plex", "sends the file as stored");
        else
            np_audio_line(y, "Plex", "MP3", in.avg_kbps, in.freq, in.channels);
        y += lh;
        if (mp_running() && in.dec_freq)
            np_audio_line(y, "Playback", "", 0, in.dec_freq, in.dec_channels);
        else
            np_pair(y, "Playback", "");
        y += lh;
        if (y <= bottom)                    /* the cover's square left alone */
            gfx_fill(&g, np.x0, y, np.x0 + (np.art_s ? np.w - np.art_s - U(8) : np.w) - 1, bottom,
                     PEN_BG);
        return;
    }
    np_line(np.title_y, mp_stream_name()[0] ? mp_stream_name() : "Radio", PEN_WHITE, TS_BOLD);
    sprintf(info, "Radio   %s", mp_tag());
    if (np.list.pool && np.idx >= 0 && np.list.count > 1) {
        sprintf(part, "   %ld of %ld in ", (long)np.idx + 1, (long)np.list.count);
        amce_strlcat(info, part, sizeof(info));
        amce_strlcat(info, np.folder, sizeof(info));
    }
    np_line(np.info_y, info, PEN_DIM, TS_SMALL);
    if (y + lh * 5 > bottom)
        return;
    np_line(y, "On air", PEN_DIM, TS_BOLD);
    y += lh;
    if (!mp_running())
        now = st_state() == ST_FAILED && st_error()[0] ? st_error() : "Stopped";
    else if (in.state == ST_CONNECTING)
        now = "Connecting...";
    else if (in.state == ST_BUFFERING)
        now = "Buffering...";
    else if (!now[0])
        now = "(the station does not say)";
    np_line(y, now, PEN_TEXT, TS_SMALL);
    gfx_fill(&g, np.x0, y + g.font_h, np.x0 + np.w - 1, y + lh * 2 - 1, PEN_BG);
    y += lh * 2;
    if (in.name[0] || in.genre[0]) {
        sprintf(part, "%s%s%s", in.name, in.name[0] && in.genre[0] ? "   " : "", in.genre);
        np_line(y, part, PEN_DIM, TS_SMALL);
        y += lh;
    }
    if (in.dec_freq && y + lh <= bottom) {
        sprintf(part, "MP3 %ld kbit/s, %ld Hz %s; played at %ld Hz %s", (long)in.bitrate,
                (long)in.freq, in.channels == 2 ? "stereo" : "mono", (long)in.dec_freq,
                in.dec_channels == 2 ? "stereo" : "mono");
        np_line(y, part, PEN_DIM, TS_SMALL);
        y += lh;
    }
    if (y <= bottom)
        gfx_fill(&g, np.x0, y, np.x0 + np.w - 1, bottom, PEN_BG);   /* lines there before */
}

static void np_draw(BOOL full)
{
    struct MpStatus st;
    char info[320];
    int i;
    mp_status(&st);
    if (!np.w)
        full = TRUE;                        /* not laid out yet */
    if (full) {
        const char *t = mp_title();
        char label[120];
        np_layout();
        plex_shown[0] = 0;                  /* the pane's details: gone under this page */
        if (mp_is_track() && np.art_s && pic_take(np_art_file, np.art_x, np.art_y, np.art_s,
                                                  np.art_s)) {
            /* the cover on screen already (the album's pane): moved into
             * its square, the rest cleared around it */
            fill_around_pic(g.menu_w + U(3), U(42), g.w - 1, g.footer_y - 2);
            np_art_drawn = TRUE;
        } else {
            if (g.pic_w)                    /* a cover there: cleared, then its pens go */
                gfx_picture_clear(&g, g.menu_w + U(3), U(42), g.w - 1, g.footer_y - 2);
            np_art_drawn = FALSE;
            gfx_fill_bg(&g, g.menu_w + U(3), U(42), g.w - 1, g.footer_y - 2);
            if (mp_is_track() && np.art_s && !np_aj && np_art_file[0] && !art_nj[ART_NP])
                np_art_load(np_art_file);   /* left to the pane, and gone from it since */
        }
        if (!mp_loaded()) {
            gfx_text(&g, np.x0, np.title_y, "Nothing playing", PEN_DIM, PEN_BG, np.w, TS_BOLD);
            return;
        }
        if (mp_is_stream()) {
            np_draw_station();
            np_art_draw();
            goto controls;
        }
        mods_label(FilePart((STRPTR)mp_path()), label, sizeof(label));
        while (*t == ' ')
            t++;
        gfx_text(&g, np.x0, np.title_y, t[0] ? t : label, PEN_WHITE, PEN_BG, np.w, TS_BOLD);
        {
            /* the file, what plays it (or its kind), the subsong, where in the folder */
            int sub, subs;
            char part[80];
            mp_subsongs(&sub, &subs);
            sprintf(info, "%s   %s", FilePart((STRPTR)mp_path()), mp_tag());
            if (mp_author()[0]) {
                sprintf(part, "   by %s", mp_author());
                amce_strlcat(info, part, sizeof(info));
            }
            if (subs > 1) {
                sprintf(part, "   subsong %ld of %ld", (long)sub, (long)subs);
                amce_strlcat(info, part, sizeof(info));
            }
            if (np.list.pool && np.idx >= 0 && np.list.count > 1) {
                sprintf(part, "   %ld of %ld in ", (long)np.idx + 1, (long)np.list.count);
                amce_strlcat(info, part, sizeof(info));
                amce_strlcat(info, np.folder, sizeof(info));
            }
        }
        gfx_text(&g, np.x0, np.info_y, info, PEN_DIM, PEN_BG, np.w, TS_SMALL);
        /* the sample names, in two columns where they fit */
        {
            WORD lh = g.font_h + U(1), cols = np.w >= U(300) ? 2 : 1, cw = np.w / cols;
            WORD rows = (g.footer_y - 2 - np.samples_y - lh) / lh, n = 0, k;
            if (rows > 0 && mp_samples() > 0) {
                gfx_text(&g, np.x0, np.samples_y, "Samples", PEN_DIM, PEN_BG, 0, TS_BOLD);
                for (k = 0; k < mp_samples() && n < rows * cols; k++) {
                    BOOL used;
                    const char *sn = mp_sample_name(k, &used);
                    char line[40];
                    if (!sn[0] && !used)
                        continue;
                    sprintf(line, "%2ld  %s", (long)k + 1, sn);
                    gfx_text(&g, np.x0 + (n / rows) * cw, np.samples_y + lh + (n % rows) * lh, line,
                             used ? PEN_TEXT : PEN_DIM, PEN_BG, cw - U(6), TS_SMALL);
                    n++;
                }
            }
        }
    }
controls:
    if (!mp_loaded())
        return;
    np_draw_progress(full, &st);
    np_draw_meters(full, &st);
    {
        int state = np_state();
        int steps = (mp_can_step(-1) ? 1 : 0) | (mp_can_step(1) ? 2 : 0);
        BOOL foc = np_focus_shown(), hot = np_slider_hot();
        if (np.flash >= 0 && (LONG)(time_ms() - np.flash_until) >= 0)
            np.flash = -1;                  /* its moment is over */
        for (i = 0; i < NB_COUNT; i++)
            if (full || foc != np.s_focus || (i == NB_PLAY && state != np.s_state) ||
                ((i == np.btn || i == np.s_btn) && np.btn != np.s_btn) ||
                ((i == np.hov || i == np.s_hov) && np.hov != np.s_hov) ||
                ((i == np.flash || i == np.s_flash) && np.flash != np.s_flash) ||
                ((i == np.rep || i == np.s_rep) && np.rep != np.s_rep) ||
                ((i == NB_VDOWN || i == NB_VUP) && mp_volume() != np.s_vol) ||
                ((i == NB_BACK || i == NB_FWD) && steps != np.s_steps))
                np_button(i);
        if (full || mp_volume() != np.s_vol || hot != np.s_slhot)
            np_slider();
        if (full || mp_volume() != np.s_vol)
            np_volume_text();
        np.s_state = state;
        np.s_steps = steps;
        np.s_btn = np.btn;
        np.s_hov = np.hov;
        np.s_flash = np.flash;
        np.s_rep = np.rep;
        np.s_focus = foc;
        np.s_slhot = hot;
        np.s_vol = mp_volume();
    }
}

/* Brought up to date (the timer tick): drawn into the hidden buffer and
 * copied up, no flip. */
static void np_update(void)
{
    BOOL was;
    if (np.w && mp_loaded() && mp_meter_count() != np.rows) {
        /* the sound has more or fewer channels than the page was laid out
         * for (a station found to be mono): laid out again, as tall as needed */
        np_draw(TRUE);
        return;
    }
    was = gfx_quiet_begin();
    np_draw(FALSE);
    gfx_quiet_end(&g, was, g.pane_x - U(2), np.meter_y, g.w - 1, np.pos_y + g.font_h);
}

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
                it->mark = now[0] && np_local() && it->path && !amce_stricmp(it->path, now)
                           ? MARK_PLAYING : MARK_NONE;
            else if (it->kind == IT_MOD)
                it->mark = (now[0] && it->path && !amce_stricmp(it->path, now)) ? MARK_PLAYING
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
static BOOL np_start(LONG idx)
{
    char err[100];
    struct Item *it;
    if (!np.list.pool || idx < 0 || idx >= np.list.count)
        return FALSE;
    it = &np.list.items[idx];
    busy_begin();
    if (it->kind == IT_STATION) {
        char url[256];
        radio_field(it, 0, url, sizeof(url));
        if (!mp_load_stream(url, it->label, it->sortkey, caps.cpu, err, sizeof(err))) {
            busy_end(TRUE);
            np.idx = idx;
            set_msg(err);
            return FALSE;
        }
    } else if (it->kind == IT_PLEXTRACK) {
        char url[2048], by[200], album[100], ms[16];
        BOOL ok_track;
        if (it->flags & ITEM_DISABLED) {
            busy_end(TRUE);
            np.idx = idx;
            set_msg(plex_why(it));
            return FALSE;
        }
        plex_track_url(it, caps.cpu, 0, url, sizeof(url));
        radio_field(it, 0, plex_now, sizeof(plex_now));
        {
            char rk[16];
            int k;
            for (k = 0; k < 2; k++) {
                radio_field(it, 11 + k, rk, sizeof(rk));
                if (rk[0])
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
            if (!plex_orig.rate && key[0])
                plex_ask_rate(key);
        }
        radio_field(it, 2, ms, sizeof(ms));
        radio_field(it, 3, by, sizeof(by));
        radio_field(it, 4, album, sizeof(album));
        if (album[0]) {
            amce_strlcat(by, by[0] ? "   " : "", sizeof(by));
            amce_strlcat(by, album, sizeof(by));
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
            busy_end(TRUE);
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
            amce_strlcpy(by, si.artist[0] ? si.artist : si.band, sizeof(by));
            if (si.album[0]) {
                amce_strlcat(by, by[0] ? "   " : "", sizeof(by));
                amce_strlcat(by, si.album, sizeof(by));
            }
        } else {
            amce_strlcpy(by, np.folder, sizeof(by));
        }
        if (!mp_load_track(it->path, si.title[0] ? si.title : it->label, by,
                           song_format(it->path), 0, plex_decoder_cpu(), 0, 0, err,
                           sizeof(err))) {
            busy_end(TRUE);
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
            amce_strlcpy(np_art_file, art, sizeof(np_art_file));
            if (np.art_s && !(np_on() && pic_take(art, np.art_x, np.art_y, np.art_s, np.art_s)))
                np_art_load(art);
            else if (np.art_s)
                np_art_drawn = TRUE;        /* the pane's: moved across */
        }
    } else if (!mp_load(it->path, err, sizeof(err))) {
        busy_end(TRUE);
        np.idx = idx;
        set_msg(err);
        np_mark_lists(TRUE);
        return FALSE;
    }
    busy_end(TRUE);
    np.idx = idx;
    np.ended_seen = FALSE;
    np.seek_due = 0;
    if (!mp_play(err, sizeof(err))) {
        set_msg(err);
        np_mark_lists(TRUE);
        return FALSE;
    }
    np_mark_lists(TRUE);
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
            if (np_on() && was_track && mp_is_track() && np.w)
                np_soft_redraw();           /* track to track: what changed, over itself */
            else if (np_on()) {
                draw_pane(TRUE);
                draw_footer();
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
    BOOL was = gfx_quiet_begin();
    np_draw_station();
    gfx_quiet_end(&g, was, np.x0, np.title_y, np.x0 + np.w - 1, g.footer_y - 2);
    np.s_state = -99;                       /* the buttons: all looked at again */
    np_update();
    if (!msg[0] && hint_shown && hint_text() != hint_shown)
        draw_footer();
}

/* A track from ms on (a seek): asked of the server again from there (a
 * conversion from that time, a stored file from that byte); delay > 0
 * waits that long first, as Back and Forward held keep moving the time. */
static void np_seek_track(ULONG ms, ULONG delay)
{
    struct MpStatus st;
    struct Item *it;
    char url[2048], by[200], album[100], err[100];
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
    it = &np.list.items[np.idx];
    if (it->kind == IT_SONG) {
        /* a file: FLAC finds the time itself; an MP3 from about its byte */
        ULONG at = 0, size = st_size();
        BOOL mp3 = !strcmp(song_format(it->path), "MP3");  /* FLAC, PCM: by time */
        if (mp3 && st.total_ms && size)
            at = (ULONG)(((unsigned long long)size * ms) / st.total_ms);
        char title[120];
        amce_strlcpy(title, mp_stream_name(), sizeof(title));  /* its tags' names, kept */
        amce_strlcpy(by, mp_track_by(), sizeof(by));
        if (!mp_load_track(it->path, title, by, song_format(it->path), st.total_ms,
                           plex_decoder_cpu(), ms, at, err, sizeof(err)) ||
            !mp_play(err, sizeof(err)))
            set_msg(err);
        return;
    }
    plex_track_url(it, caps.cpu, ms, url, sizeof(url));
    radio_field(it, 3, by, sizeof(by));
    radio_field(it, 4, album, sizeof(album));
    if (album[0]) {
        amce_strlcat(by, by[0] ? "   " : "", sizeof(by));
        amce_strlcat(by, album, sizeof(by));
    }
    if (!mp_load_track(url, it->label, by, plex_tag(it), st.total_ms, plex_decoder_cpu(), ms,
                       plex_converts(it) ? 0 : plex_track_byte(it, ms), err, sizeof(err)) ||
        !mp_play(err, sizeof(err))) {
        set_msg(err);
        return;
    }
    if (np_on() && np.w)
        np_soft_redraw();
}

static void np_volume(int d)
{
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

static void np_press(int b)
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
            set_msg(mp_is_track() ? "That was the last track"
                    : mp_is_stream() ? "That was the last station in the list"
                                     : "That was the last module in the folder");
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
            set_msg("This module cannot be moved through");
        break;
    case NB_PLAY:
        if (!mp_loaded()) {
            if (np.idx >= 0)
                np_start(np.idx);
        } else if (mp_playing()) {
            mp_pause(TRUE);
        } else if (mp_paused()) {
            mp_pause(FALSE);
        } else if (!mp_play(err, sizeof(err))) {
            set_msg(err);
        }
        break;
    case NB_STOP:
        mp_stop();
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

static void np_open_page(void)
{
    struct Page *p;
    if (np_on())
        return;
    if (!(p = page_push(PT_PLAYER, "Now playing")))
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
    amce_strlcpy(np.folder, p->title, sizeof(np.folder));
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
    if (np_start(np.idx))
        np_open_page();
    else {
        draw_list_rows(p);
        draw_footer();
    }
}

/* A Mods list on screen with the playing module in it. */
static BOOL np_list_on(void)
{
    struct Page *p = pane_pending ? NULL : cur();
    return p && p->type == PT_LIST && (p->mods || p->radio || p->plex || p->songs) && mp_loaded();
}

/* The bars by the playing module (and Now playing) drawn again: the next
 * frame of their dance, or still. Just the markers, without a flip. */
static void np_eq_redraw(void)
{
    struct Page *p = cur();
    LONG i, end;
    if (!np_list_on())
        return;
    end = p->top + list_visible();
    if (end > p->list.count)
        end = p->list.count;
    for (i = p->top; i < end; i++) {
        struct Item *it = &p->list.items[i];
        WORD y;
        BOOL sel, was;
        UBYTE bg;
        if (it->mark != MARK_PLAYING)
            continue;
        y = g.list_top + (i - p->top) * g.list_row_h + (g.list_row_h - g.mark_px) / 2;
        sel = i == p->sel;
        bg = sel ? (focus == FOCUS_CONTENT ? PEN_HL : PEN_SELDIM)
                 : mouse_mode && i - p->top == hover_row ? PEN_RULE : PEN_BG;
        was = gfx_quiet_begin();
        gfx_mark(&g, MARK_PLAYING, g.pane_x, y, sel ? PEN_WHITE : PEN_TEXT, bg);
        gfx_quiet_end(&g, was, g.pane_x, y, g.pane_x + g.mark_px - 1, y + g.mark_px - 1);
    }
}

/* The bars' dance: a frame every 120 ms while the music plays. */
static void np_eq_tick(void)
{
    static ULONG last;
    BOOL still = !mp_playing();
    ULONG now = time_ms();
    if (still != gfx_eq_still) {
        gfx_eq_still = still;
        np_eq_redraw();
        hdr_np_draw(TRUE);
    } else if (!still && now - last >= 120) {
        last = now;
        gfx_eq_frame++;
        np_eq_redraw();
        hdr_np_draw(TRUE);
    } else if (mp_loaded() != hdr_np_on) {
        hdr_np_draw(TRUE);                  /* started or stopped */
    }
}

/* The timer tick: the next module at a song's end, the page kept up. */
static int np_button_at(WORD x, WORD y)
{
    int i;
    if (y < np.by || y >= np.by + np.bh)
        return -1;
    for (i = 0; i < NB_COUNT; i++)
        if (x >= np.bx[i] && x < np.bx[i] + np.bw)
            return i;
    if (np.sl_on && x >= np.sl_x0 - U(2) && x <= np.sl_x1 + U(2))
        return NB_SLIDER;
    return -1;
}

static void np_rep_start(int b, int src)
{
    if (!np_repeats(b))
        return;
    np.rep = b;
    np.rep_src = src;
    np.rep_next = time_ms() + REP_DELAY;
}

static void np_rep_stop(void)
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
    if (np.rep_src == REP_MOUSE && g.win && np_button_at(g.win->MouseX, g.win->MouseY) != np.rep)
        return;
    np_press(np.rep);
}

static void np_tick(void)
{
    if (np.seek_due && (LONG)(time_ms() - np.seek_due) >= 0)
        np_seek_track((ULONG)np.seek_to, 0);    /* Back/Forward rested: from there */
    np_repeat();
    np_eq_tick();
    if (mp_is_track() && mp_running() && np_on() && np.w) {
        /* the average bitrate settling: its line again when it moves */
        static ULONG shown;
        ULONG k = st_avg_kbps();
        if (k != shown && (k > shown + 1 || k + 1 < shown || !shown || !k)) {
            BOOL was = gfx_quiet_begin();
            shown = k;
            np_draw_station();
            gfx_quiet_end(&g, was, np.x0, np.title_y, np.x0 + np.w - 1, g.footer_y - 2);
        }
    }
    if (mp_is_track() && mp_running() && mp_ended()) {
        /* a track played to its end (or failed): the next in its list */
        BOOL failed = st_state() == ST_FAILED;
        char why[100];
        amce_strlcpy(why, st_error(), sizeof(why));
        if (failed) {
            amce_log("plex", why[0] ? why : "the track failed");
            set_msg(why[0] ? why : "That track could not be played");
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
            amce_strlcpy(why, st_error()[0] ? st_error() : "The station stopped sending",
                         sizeof(why));
            mp_stop();
            np_mark_lists(TRUE);
            set_msg(why);
            amce_log("radio", why);
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
                BOOL was = gfx_quiet_begin();
                np_draw_station();
                gfx_quiet_end(&g, was, np.x0, np.title_y, np.x0 + np.w - 1, g.footer_y - 2);
                np_update();
                if (!msg[0] && hint_shown && hint_text() != hint_shown)
                    draw_footer();
            }
            return;
        }
    }
    if (mp_running() && mp_ended() && !np.ended_seen) {
        np.ended_seen = TRUE;
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

/* The mouse over a button or the slider: it lights up (dragging the
 * slider: the volume follows). */
static void np_hover(WORD x, WORD y)
{
    int h;
    if (!np_on() || !mp_loaded())
        return;
    if (np.sl_drag) {
        int v = np_slider_vol(x);
        if (v != mp_volume())
            np_volume(v - mp_volume());
        return;
    }
    h = np_button_at(x, y);
    if (h != np.hov) {
        np.hov = h;
        np_update();
    }
}

/* The mouse button let go: no more repeating or dragging. */
static void np_mouse_up(void)
{
    if (np.rep >= 0 && np.rep_src == REP_MOUSE)
        np_rep_stop();
    if (np.sl_drag) {
        np.sl_drag = FALSE;
        if (np_on())
            np_update();
    }
}

static BOOL np_click(WORD x, WORD y)
{
    int i;
    if (!np_on() || !mp_loaded())
        return FALSE;
    i = np_button_at(x, y);
    if (i == NB_SLIDER) {                   /* to that volume, and drag from there */
        np.sl_drag = TRUE;
        np.hov = NB_SLIDER;
        np_volume(np_slider_vol(x) - mp_volume());
        return TRUE;
    }
    if (i >= 0) {
        np.btn = i;                         /* the keys carry on from here */
        np_press(i);
        np_rep_start(i, REP_MOUSE);
        return TRUE;
    }
    /* the bar (and the line under it): to that part of the song */
    if (y >= np.time_y - U(2) && y < np.time_y + g.font_h + U(2) && x >= np.x0 &&
        x < np.x0 + np.bar_w) {
        struct MpStatus st;
        ULONG want;
        int pos, best = 0;
        mp_status(&st);
        if (!st.total_ms)
            return TRUE;                    /* its length is not known */
        want = st.total_ms / 100 * (ULONG)(x - np.x0) / (ULONG)np.bar_w * 100;
        if (mp_is_track()) {
            np_seek_track(want, 0);         /* a track: from there, at once */
            return TRUE;
        }
        if (mp_seek_ms(want)) {             /* mixed modules: to the time itself */
            np_update();
            return TRUE;
        }
        for (pos = 0; pos < st.songlen; pos++) {
            ULONG at = mp_position_ms(pos);
            if (at != 0xFFFFFFFFUL && at <= want && at >= mp_position_ms(best))
                best = pos;
        }
        mp_seek(best);
        np_update();
        return TRUE;
    }
    return FALSE;
}

/* The Mods section: what is playing, then the folders of modules. */
static BOOL dir_exists(const char *path)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR l;
    me->pr_WindowPtr = (APTR)-1;
    l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = old;
    if (l)
        UnLock(l);
    return l != 0;
}

/* "Now playing" at the top of the Mods and Radio pages while something
 * plays (a module or a station), its tag the title or the station. */
static void np_shortcut(struct Page *p)
{
    const char *t;
    char label[120];
    if (!mp_loaded())
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
    action_item(p, "Now playing", t, ACT_NOWPLAYING, MARK_PLAYING);
}

/* A library's long list (Artists, Albums, Recently added): a Now playing
 * row first while something plays, as Music's own page has. */
static BOOL plex_lib_list(const struct Page *p)
{
    return p && p->type == PT_LIST && p->plex && p->radio && !p->photos &&
           !strncmp(p->base, "/library/sections/", 18);
}

/* Its Now playing row made as playback is now (added, taken out, or its
 * title changed); the selection stays on the entry it was on. */
static void plex_shortcut_fix(struct Page *p)
{
    struct Item *it;
    const char *t;
    BOOL has, playing = mp_loaded();
    if (!plex_lib_list(p) || !p->list.pool)
        return;
    it = p->list.count ? &p->list.items[0] : NULL;
    has = it && it->kind == IT_ACTION && it->action == ACT_NOWPLAYING;
    t = !playing ? "" : mp_is_stream() ? mp_stream_name() : mp_title();
    if (has && playing && !strcmp(it->path, t))
        return;                             /* as it is */
    {
        LONG row = p->sel - p->top, e = p->sel - (has ? 1 : 0), ns;
        BOOL added = FALSE;
        if (has)
            list_drop_first(&p->list);
        if (playing && (it = list_add_first(&p->list, "Now playing", t, "", IT_ACTION))) {
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
    amce_strlcpy(plex_pos[k].key, p->base, sizeof(plex_pos[k].key));
    amce_strlcpy(plex_pos[k].label, p->list.items[p->sel].label, sizeof(plex_pos[k].label));
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
    if (plex_lib_list(p)) {
        plex_shortcut_fix(p);
        return;
    }
    struct Item *it;
    char want[120], sel[96];
    BOOL has, playing = mp_loaded();
    LONG i;
    if (depth != 1 || !p || p->type != PT_LIST || !(p->mods || p->radio || p->plex || p->songs))
        return;
    it = p->list.count ? &p->list.items[0] : NULL;
    has = it && it->kind == IT_ACTION && it->action == ACT_NOWPLAYING;
    want[0] = 0;
    if (playing)
        amce_strlcpy(want, mp_is_stream() ? mp_stream_name() : mp_title(), sizeof(want));
    if (has == playing && (!has || !strcmp(it->path, want) || !mp_is_stream()))
        return;                             /* as it is: nothing to do */
    sel[0] = 0;
    if (p->sel >= 0 && p->sel < p->list.count)
        amce_strlcpy(sel, p->list.items[p->sel].label, sizeof(sel));
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

static void build_mods_root(void)
{
    struct Page *p;
    BOOL mods = dir_exists(MODS_ROOT), menu_music = dir_exists(MODS_MENU), sid = dir_exists(MODS_SID);
    if (!mods && !menu_music && !mp_loaded()) {
        if ((p = page_push(PT_TEXT, "Mods"))) {
            text_para(p, "No modules found. AmigaMCE looks for them in " MODS_ROOT
                         " and in " MODS_MENU " (AGS's menu music).");
            text_line(p, "");
            text_para(p, "It plays ProTracker modules itself, and hundreds of other formats "
                         "(OctaMED, TFMX, Future Composer, SID...) with EaglePlayer's and "
                         "DeliTracker's players, all on the Amiga's own sound.");
        }
        return;
    }
    if (!(p = page_push(PT_LIST, "Mods")))
        return;
    p->mods = 1;
    np_shortcut(p);
    if (mods)
        list_add(&p->list, "Modules", MODS_ROOT, "", IT_DIR);
    if (sid)
        list_add(&p->list, "C64 music", MODS_SID, "", IT_DIR);
    if (menu_music)
        list_add(&p->list, "Menu music", MODS_MENU, "", IT_DIR);
}

static void pane_settle(void);

static void draw_all(void)
{
    aside_w = -1;               /* the screen may be new: work the column out again */
    pane_settle();
    gfx_fill(&g, 0, 0, g.w - 1, g.h - 1, PEN_BG);
    if (g.pic_w)
        gfx_picture_free(&g);   /* filled over just now: its pens go too */
    draw_header();
    draw_menu();
    draw_pane(TRUE);
    draw_footer();
}

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

static void pane_settle(void)
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

/* The pane while the selection is moving: blank, with the section's name. */
static void pane_preview(BOOL first)
{
    const char *t = mod_names[menu[menu_sel]];
    if (first) {
        /* all but the title band (title_band_rest); a picture cleared
         * first, then its pens let go (filled over, it flashed) */
        if (g.pic_w)
            gfx_picture_clear(&g, g.menu_w + U(3), U(25), g.w - 1, g.footer_y - 2);
        gfx_fill_bg(&g, g.menu_w + U(3), U(25), g.w - 1, U(26) - 1);
        gfx_fill_bg(&g, g.menu_w + U(3), U(26), g.pane_x - U(2) - 1, U(42) - 1);
        gfx_fill_bg(&g, g.menu_w + U(3), U(42), g.w - 1, g.footer_y - 2);
    }
    gfx_text(&g, g.pane_x, U(28), t, PEN_WHITE, PEN_BG, g.pane_w, TS_BOLD | TS_OPAQUE);
    title_band_rest(g.pane_x, g.pane_x + gfx_textlen(&g, t, TS_BOLD), g.w);
    gfx_fill(&g, g.pane_x, U(41), g.pane_x + g.pane_w - 1, U(42) - 1, PEN_RULE);
}

/* now: build the section straight away (a click), not once it settles */
static void menu_select_now(int i, BOOL now)
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

static void set_focus(int f)
{
    pane_settle();
    if (focus == f || (f == FOCUS_CONTENT && section_inert()))
        return;
    focus = f;
    pv_tb.focused = f == FOCUS_PREVIEW;
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
static void open_dir(struct Item *it)
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
    if (pics && (it->flags & ITEM_DISABLED)) {
        set_msg("No pictures in that folder");
        return;
    }
    set_msg("Loading...");
    p = page_push(PT_LIST, it->label);
    if (!p) {
        set_msg("Too many levels open");
        return;
    }
    if (pics) {
        p->pics = 1;
        n = pics_read_ui(it->path, &p->list);
        if (n <= 0) {
            page_pop();
            if (n == -2)
                msg[0] = 0;                 /* given up for input */
            else
                set_msg(n < 0 ? "Could not open that folder" : "That folder has no pictures");
            return;
        }
        msg[0] = 0;
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    if (songs) {
        p->songs = 1;
        n = songs_read_dir(it->path, &p->list);
        if (n <= 0) {
            page_pop();
            set_msg(n < 0 ? "Could not open that folder" : "No MP3 or FLAC files in that folder");
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
            set_msg(n < 0 ? "Could not open that folder" : "That folder has no modules");
            return;
        }
        np_mark_lists(FALSE);
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
            set_msg(n < 0 ? "Could not open that menu" : "That menu is empty");
        return;
    }
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
    n = ags_read_txt(p->base, fact_buf, FACT_LINES);
    if (!n)
        text_line(p, it->label);
    for (i = 0; i < n; i++)
        text_line(p, fact_buf[i]);
    draw_pane(TRUE);
    draw_footer();
}

/* ------------------------------------------------------------ launching */
/* Libraries AmigaMCE can do without while a game runs (closed then, so the
 * system can unload them, picture datatypes and all). */
static void open_extra_libs(void)
{
    if (!DiskfontBase)
        DiskfontBase = OpenLibrary((CONST_STRPTR)"diskfont.library", 36);
    if (!DataTypesBase)
        DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
    if (!LowLevelBase)
        LowLevelBase = OpenLibrary((CONST_STRPTR)"lowlevel.library", 40);
}

static void close_extra_libs(void)
{
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
    amce_log(what, buf);
}

/* Before a game: everything AmigaMCE can rebuild goes. Where each section
 * was is kept (nav_mem), so the same entry comes back afterwards. */
static void release_for_launch(void)
{
    log_free("free before launch");
    if (mp_loaded())
        amce_log("music", "stopped for the game");
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
    pv_shown = pv_pending = FALSE;
    ov.on = FALSE;
    ov.bl = NULL;
    ptr_hidden = NULL;              /* a new window comes with its pointer */
    spin_free();
    ov_bm_free();
    gfx_close(&g);                  /* screen, fonts, artwork, smooth style, letters */
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
    set_msg("Loading...");
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
    static const char *script = "T:AmigaMCE-run";
    BPTR con;
    if (!ags.found)
        return;
    if (!ags_write_launch_script(&ags, base, script)) {
        set_msg("Could not write T:AmigaMCE-run");
        return;
    }
    /* Give the game every byte we can. */
    amce_log("launch", base);
    nav_store(FALSE);               /* should it not come back */
    radio_stop();                   /* no network process under a game */
    if (debug_on)
        dbg_cpu_stop();
    release_for_launch();
    con = Open((CONST_STRPTR)"CON:0/0/640/200/AmigaMCE/AUTO/CLOSE", MODE_OLDFILE);
    if (con) {
        SystemTags((CONST_STRPTR)"Execute T:AmigaMCE-run",
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
 * Full screen on AmigaMCE's own screen (view.c). Left/Right (or Up/Down,
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

static void viewer_bar(const struct ItemList *l, LONG i, const struct PicInfo *pi,
                       const char *scale, BOOL slideshow)
{
    char detail[120], desc[64];
    LONG k, pos = 0, n = 0;
    for (k = 0; k < l->count; k++)
        if (l->items[k].kind == IT_PIC) {
            n++;
            if (k == i)
                pos = n;
        }
    pic_describe(pi, desc, sizeof(desc));
    sprintf(detail, "%ld/%ld   %s   %s%s", (long)pos, (long)n, desc, scale,
            slideshow ? "   Slideshow" : "");
    view_bar(&g, l->items[i].label, detail,
             slideshow ? "Left/Right  previous/next    Fire  pause    Esc  back"
                       : "Left/Right  previous/next    Fire  slideshow    Esc  back");
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
    if (!(vw_job = AllocVec(sizeof(*vw_job), MEMF_ANY | MEMF_CLEAR)))
        return;
    vw_job->type = LJ_PICTURE;
    amce_strlcpy(vw_job->path, l->items[i].path, sizeof(vw_job->path));
    vw_job->scr = g.truecolor ? g.scr : NULL;
    vw_job->remap = g.truecolor;
    vw_job->want_pic = TRUE;
    if (!g.truecolor) {
        /* one with more colours than the screen: made ready for it */
        vw_job->tw = g.w;
        vw_job->th = g.h;
        vw_job->tall = gfx_mode_tall(g.mode);
        vw_job->viewfit = TRUE;
        vw_job->maxcol = (1 << (g.depth > 8 ? 8 : g.depth)) - 1;
    }
    ldr_submit(vw_job);
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
                sprintf(m, "%.80s, %lu x %lu, depth %lu: decoded in %lu ms, made %lu x %lu, "
                           "%lu colours, in %lu ms",
                        FilePart((STRPTR)path), (unsigned long)info->w, (unsigned long)info->h,
                        (unsigned long)info->depth, (unsigned long)vw_job->ms_decode,
                        (unsigned long)(*t)->w, (unsigned long)(*t)->h,
                        (unsigned long)(*t)->ncol, (unsigned long)vw_job->ms_thumb);
                amce_log("viewer, read ahead", m);
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
    amce_strlcpy(vw_kept[vw_kept_next], file, sizeof(vw_kept[0]));
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
    ULONG bar_until = 0, next_at = 0, jlast = 0, wait_from = 0;
    BOOL quit = FALSE, ok = FALSE, bar_due = TRUE;

    if (idx < 0 || idx >= l->count || l->items[idx].kind != IT_PIC)
        return idx;
    hold_stop();
    ov.on = FALSE;
    tb_close(&pv_tb);
    pv_shown = FALSE;
    pv_pending = FALSE;
    pv_let_go();                    /* the viewer takes the screen's colours over */
    gfx_dbuf_stop(&g);              /* it draws straight onto the screen */
    if (!view_begin(&g)) {
        gfx_dbuf_start(&g);
        set_msg("Not enough memory for the viewer");
        return idx;
    }
    if (LowLevelBase)
        jlast = ReadJoyPort(1);
    while (!quit) {
        struct IntuiMessage *im;
        LONG step = 0;
        BOOL toggle = FALSE, poke = FALSE;
        ULONG now;
        if (vw_remote) {
            radio_collect();                /* the server's photos, as they come */
            if (want != shown && !vw_fetch(l, want, ART_VW)) {
                if (vw_failed) {
                    /* not sent: said so, and on to the next */
                    LONG nx = view_step(l, want, dir);
                    char d[80];
                    vw_failed = FALSE;
                    asked = -1;
                    sprintf(d, "not sent by %.50s", plex.name[0] ? plex.name : plex.host);
                    view_bar(&g, l->items[want].label, d, "Esc  back");
                    bar_until = time_ms() + BAR_MS;
                    if (++fails >= 3 || nx < 0 || nx == want || (shown < 0 && nx == idx)) {
                        Delay(50);          /* read, then back to the list */
                        quit = TRUE;
                        break;
                    }
                    want = nx;
                } else if (asked != want) {
                    asked = want;
                    wait_from = time_ms();
                    if (shown < 0 || !slideshow) {
                        view_bar(&g, l->items[want].label, "Loading from Plex...", "Esc  back");
                        bar_until = time_ms() + 3600000;   /* till it comes */
                    }
                } else if (slideshow && shown >= 0 && !view_bar_shown() &&
                           time_ms() - wait_from > 1500) {
                    view_bar(&g, l->items[want].label, "Loading from Plex...", "Esc  back");
                    bar_until = time_ms() + 3600000;
                }
                goto input;                 /* keys meanwhile: another one, or back */
            }
        }
        if (want != shown) {
            BOOL remapped = FALSE;
            struct Thumb *pre_t;
            struct PicInfo pre_i;
            APTR pre = vw_take(l->items[want].path, &remapped, &pre_t, &pre_i);
            ULONG t0 = time_ms();
            ok = view_show(&g, l->items[want].path, l->items[want].label, &pi, scale, sizeof(scale),
                           pre, remapped, pre_t, &pre_i);
            {
                char m[300];
                sprintf(m, "%.80s, %lu x %lu, depth %lu: %lu ms%s, %s.datatype", l->items[want].label,
                        (unsigned long)pi.w, (unsigned long)pi.h, (unsigned long)pi.depth,
                        (unsigned long)(time_ms() - t0),
                        pre || pre_t ? " (read ahead)" : "", pi.cls[0] ? pi.cls : "?");
                amce_log("viewer", m);
            }
            shown = want;
            asked = -1;
            fails = 0;
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
            if (ok && bar_due) {
                viewer_bar(l, shown, &pi, scale, slideshow);
                bar_until = time_ms() + BAR_MS;
            }
            bar_due = !slideshow;
        }
    input:
        while ((im = (struct IntuiMessage *)GetMsg(g.win->UserPort))) {
            ULONG cl = im->Class;
            UWORD code = im->Code;
            ReplyMsg((struct Message *)im);
            if (cl == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX)) {
                switch (code) {
                case 0x4F: case 0x4C: step--; break;            /* left, up */
                case 0x4E: case 0x4D: step++; break;            /* right, down */
                case 0x43: case 0x44: toggle = !toggle; break;  /* enter, return */
                case 0x45: case 0x41: quit = TRUE; break;       /* esc, backspace */
                default: poke = TRUE; break;
                }
            } else if (cl == IDCMP_VANILLAKEY) {
                if (code == 27 || code == 8)
                    quit = TRUE;
                else if (code == 13 || code == ' ')
                    toggle = !toggle;
                else
                    poke = TRUE;
            } else if (cl == IDCMP_MOUSEBUTTONS) {
                if (code == SELECTDOWN)
                    step++;
                else if (code == MENUDOWN)
                    quit = TRUE;
            }
        }
        if (LowLevelBase) {
            ULONG st = ReadJoyPort(1), pressed = st & ~jlast;
            if ((st & JP_TYPE_MASK) == JP_TYPE_JOYSTK || (st & JP_TYPE_MASK) == JP_TYPE_GAMECTLR) {
                if (pressed & (JPF_JOY_LEFT | JPF_JOY_UP))
                    step--;
                if (pressed & (JPF_JOY_RIGHT | JPF_JOY_DOWN))
                    step++;
                if (pressed & JPF_BUTTON_RED)
                    toggle = !toggle;
                if (pressed & JPF_BUTTON_BLUE)
                    quit = TRUE;
            }
            jlast = st;
        }
        if (quit)
            break;
        now = time_ms();
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
        } else if (slideshow && (LONG)(now - next_at) >= 0) {
            LONG i = view_step(l, shown, 1);
            dir = 1;
            if (i >= 0)
                want = i;
        }
        if (poke && want == shown && ok) {
            viewer_bar(l, shown, &pi, scale, slideshow);
            bar_until = now + BAR_MS;
        }
        if (view_bar_shown() && ok && (LONG)(now - bar_until) >= 0)
            view_bar_hide(&g);
        dbg_sleep_begin();
        WaitTOF();
        dbg_sleep_end();
    }
    vw_drop();
    view_end(&g);
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
    draw_all();
    view_restore(&g);
    gfx_dbuf_start(&g);
}

static void view_from_page(struct Page *p)
{
    LONG vis = list_visible(), last = viewer_run(&p->list, p->sel, FALSE);
    p->sel = last;
    if (p->sel < p->top)
        p->top = p->sel;
    if (p->sel >= p->top + vis)
        p->top = p->sel - vis + 1;
    viewer_done();
}

/* A Plex page's photos in the viewer, from entry idx (-1: the first, as a
 * slideshow); the page's selection follows the last one shown. */
static void plex_photo_view(struct Page *p, LONG idx, BOOL slideshow)
{
    struct ItemList l;
    char thumb[160], req[400], file[120], m[80];
    LONG i, first = -1, last, k, vis = list_visible();
    WORD w, h;
    if (!plex.host[0] || !list_init(&l))
        return;
    vw_photo_size(&w, &h);
    for (i = 0; i < p->list.count; i++) {
        struct Item *it = &p->list.items[i];
        if (it->kind != IT_PLEXPHOTO)
            continue;
        radio_field(it, 2, thumb, sizeof(thumb));
        plex_art(thumb, w, h, req, sizeof(req), file, sizeof(file));
        if (i == idx || (idx < 0 && first < 0))
            first = l.count;
        list_add(&l, it->label, file, thumb, IT_PIC);
    }
    if (first < 0) {
        list_free(&l);
        return;
    }
    sprintf(m, "%ld photos, asked for at %ld x %ld", (long)l.count, (long)w, (long)h);
    amce_log("plex: viewer", m);
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
    set_msg("Loading...");
    busy_begin();
    n = pics_read_ui(it->path, &l);
    busy_end(n <= 0);               /* the viewer covers the screen next */
    first = n > 0 ? view_step(&l, l.count - 1, 1) : -1;
    if (n == -2) {
        msg[0] = 0;                 /* given up for input */
        draw_footer();
    } else if (first < 0) {
        set_msg("No pictures in that folder");
    } else {
        viewer_run(&l, first, TRUE);
        viewer_done();
    }
    list_free(&l);
}

static void act_move(LONG d)
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
            pv_dir = d > 0 ? 1 : -1;
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
        if (s < 0 || s == p->sel)
            return;
        {
            LONG old = p->sel, old_top = p->top;
            p->sel = s;
            if (ov.on && ov.page == p)
                ov_relabel(ov_letter_of(p->list.items[s].label));
            draw_title_count(p);    /* only the number changes */
            draw_list_move(p, old, old_top);
        }
    } else {
        tb_scroll_by(&p->tb, d * p->tb.line_h);   /* the main loop animates it */
    }
}

/* One wheel notch scrolls whatever is under the pointer, as on other
 * desktops: one entry in the menu and in lists, three lines of text.
 * Scrolling the content pane also gives it the keyboard focus. Over the
 * header or footer the notch goes to whatever has the focus. */
static void act_wheel(LONG d, WORD x, WORD y)
{
    set_mouse_mode(TRUE);
    mouse_last = time_ms();
    bar_up = TRUE;                  /* the wheel shows the bar too (drawn with the move) */
    struct Page *p = cur();
    BOOL over_menu = x < g.menu_w && y >= g.top && y < g.footer_y - 1;
    BOOL over_pane = x >= g.pane_x - U(2) && y >= g.top && y < g.footer_y - 1;
    if (over_pane && pv_tb.open && p && has_aside(p) && x >= aside_x()) {
        tb_scroll_by(&pv_tb, d * 3 * pv_tb.line_h);     /* the preview's fact sheet */
        return;
    }
    if (over_menu && focus != FOCUS_MENU)
        set_focus(FOCUS_MENU);
    else if (over_pane && section_inert())
        return;                     /* nothing there to scroll */
    else if (over_pane && p && (p->type != PT_LIST || p->list.count) && focus != FOCUS_CONTENT)
        set_focus(FOCUS_CONTENT);
    act_move((focus == FOCUS_CONTENT && p && p->type != PT_LIST && p->type != PT_PLAYER) ? d * 3 : d);
}

static void open_screenmodes(void)
{
    struct Page *p = page_push(PT_LIST, "Screen mode");
    if (!p)
        return;
    build_screenmodes(p);
    draw_pane(TRUE);
    draw_footer();
}

static void open_styles(void)
{
    struct Page *p = page_push(PT_LIST, "Style");
    if (!p)
        return;
    build_styles(p);
    draw_pane(TRUE);
    draw_footer();
}

static void open_credits(void)
{
    struct Page *p = page_push(PT_TEXT, "Credits");
    if (!p)
        return;
    build_credits(p);
    draw_pane(TRUE);
    draw_footer();
}

static void open_sysinfo(void)
{
    struct Page *p = page_push(PT_TEXT, "System information");
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
        char m[80];
        amce_strlcpy(m, "That mode ", sizeof(m));
        amce_strlcat(m, why, sizeof(m));
        set_msg(m);
        return;
    }
    if (sm == screen_mode)
        return;
    pages_clear();
    pv_flush();                     /* decoded for this screen */
    ptr_hidden = NULL;              /* a new window comes with its pointer */
    spin_free();
    ov_bm_free();
    gfx_close(&g);
    screen_mode = sm;
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
        /* back where it was chosen: Settings > Screen > Screen mode */
        struct Page *p = page_push(PT_LIST, "Screen");
        if (p) {
            build_screen_settings(p);
            p->sel = 0;
        }
        if ((p = page_push(PT_LIST, "Screen mode")))
            build_screenmodes(p);
    }
    focus = FOCUS_CONTENT;
    msg[0] = 0;
    draw_all();
    if (failed)
        set_msg("Could not open that screen mode");
    else
        note_style();
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
    draw_pane(TRUE);
    draw_footer();
}

/* Settings > Networking: an interface chosen (i < 0: the stack's choice). */
static void choose_nic(int i)
{
    if (i >= 0 && (i >= nnic || !nics[i].up || !nics[i].ip))
        return;
    amce_strlcpy(net_if, i < 0 ? "" : nics[i].name, sizeof(net_if));
    save_setting(NETIF_VAR, net_if);
    net_bind_to(i < 0 ? 0 : nics[i].ip);
    plex_settings_again(FALSE);
    set_msg(i < 0 ? "Connections: the stack's choice of interface"
                  : "Connections go out from this interface");
}

static void open_sound_settings(void)
{
    struct Page *p = page_push(PT_LIST, "Sound");
    if (!p)
        return;
    build_sound_settings(p);
    draw_pane(TRUE);
    draw_footer();
}

static void open_plex_settings(void)
{
    struct Page *p = page_push(PT_LIST, "Plex");
    if (!p)
        return;
    build_plex_settings(p);
    draw_pane(TRUE);
    draw_footer();
}

static void toggle_https(void)
{
    struct Page *p = cur();
    LONG sel = p ? p->sel : 0;
    if (!caps.amissl)
        return;
    radio_https = !radio_https;
    save_setting(HTTPS_VAR, radio_https ? "1" : "0");
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
    BOOL restyled = gfx_restyle(&g, ui_style);
    for (i = 0; i < nmenu; i++)
        if (menu[i] == MOD_SETTINGS)
            menu_sel = i;
    build_root(MOD_SETTINGS);
    if (cur())
        cur()->sel = SETTINGS_SCREEN_ROW;
    if ((p = page_push(PT_LIST, "Screen"))) {
        build_screen_settings(p);
        p->sel = SCREEN_STYLE_ROW;
    }
    if ((p = page_push(PT_LIST, "Style")))
        build_styles(p);
    focus = FOCUS_CONTENT;
    msg[0] = 0;
    draw_all();
    note_style();
    if (!restyled)
        set_msg("The smooth style's files could not be loaded");
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
            set_msg("Smooth: on RTG screens only");
        return;
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
    if (music_assign && music_assign_there() &&
        (it = list_add(&p->list, "Music files (MUSIC:)", "MUSIC:", "", IT_DIR))) {
        it->mark = MARK_ARROW;
        n++;
    }
    for (i = 0; i < nmusic_dirs; i++) {
        char label[120];
        const char *f = (const char *)FilePart((STRPTR)music_dirs[i]);
        sprintf(label, "Music files: %.100s", f[0] ? f : music_dirs[i]);
        if ((it = list_add(&p->list, label, music_dirs[i], "", IT_DIR))) {
            it->mark = MARK_ARROW;
            n++;
        }
    }
    if (!n)
        list_add(&p->list, "Music files: Settings > Sound > Music folders", "", "", IT_TEXT);
}

static void build_music_root(struct Page *p)
{
    struct Item *it;
    char label[96];
    p->plex = 1;
    np_shortcut(p);
    if (!caps.net) {
        list_add(&p->list, "No TCP/IP stack running: Plex needs networking", "", "", IT_TEXT);
        music_file_rows(p);
        return;
    }
    if (!plex.on) {
        list_add(&p->list, "Plex is turned off (Settings > Networking > Plex)", "", "", IT_TEXT);
        music_file_rows(p);
        return;
    }
    if (plex.host[0] && plex.library[0]) {
        /* the library known: its Artists, Albums, Recently added here */
        plex_section_menu(plex.library, &p->list);
        if (plex.libraries > 1) {
            sprintf(label, "Other libraries on %s", plex.name[0] ? plex.name : plex.host);
            if ((it = list_add(&p->list, label, "/library/sections", "", IT_PLEXDIR)))
                it->mark = MARK_ARROW;
        }
    } else if (plex.host[0]) {
        sprintf(label, "Plex: %s", plex.name[0] ? plex.name : plex.host);
        if ((it = list_add(&p->list, label, "/library/sections", "", IT_PLEXDIR)))
            it->mark = MARK_ARROW;
    } else {
        /* the server is chosen in Settings (About > Plex in AmigaMCE says more) */
        list_add(&p->list, "No Plex server chosen: Settings > Networking > Plex", "", "", IT_TEXT);
    }
    music_file_rows(p);
}

static void build_radio_root(struct Page *p)
{
    static const struct { const char *label, *query; } ways[] = {
        { "Top voted", "order=votes&reverse=true" },
        { "Most listened to", "order=clickcount&reverse=true" },
        { "Trending", "order=clicktrend&reverse=true" },
        { "Chiptune", "tag=chiptune&tagExact=true&order=votes&reverse=true" },
        { "Demoscene", "tag=demoscene&tagExact=true&order=votes&reverse=true" },
        { "Genres", RADIO_TAGS },
        { "Countries", RADIO_COUNTRIES },
    };
    int i;
    p->radio = 1;
    np_shortcut(p);
    for (i = 0; i < (int)(sizeof(ways) / sizeof(ways[0])); i++) {
        struct Item *it = list_add(&p->list, ways[i].label, ways[i].query, "", IT_NET);
        if (it)
            it->mark = MARK_ARROW;
    }
}

/* The answer to j into the page that asked, if it is still open. */
static void radio_done(struct NetJob *j)
{
    struct Page *p = NULL;
    int d;
    for (d = 0; d < ART_KINDS; d++)
        if (j == art_nj[d]) {
            /* a picture of the server's: kept in RAM:, then shown */
            art_nj[d] = NULL;
            if (j->body && j->status == 200 && plex_art_save(art_want[d], j->body, j->len)) {
                art_ready(d, art_want[d]);
            } else {
                char m[80];
                sprintf(m, "not sent (%ld)%s%.40s", (long)j->status, j->err[0] ? ": " : "", j->err);
                amce_log("plex: picture", m);
                if (d == ART_VW)
                    vw_failed = TRUE;       /* the viewer goes on without it */
            }
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
                mp_is_track()) {
                BOOL was = gfx_quiet_begin();
                np_draw_station();
                gfx_quiet_end(&g, was, np.x0, np.title_y, np.x0 + np.w - 1, g.footer_y - 2);
            }
        }
        if (j->body)
            FreeVec(j->body);
        FreeVec(j);
        return;
    }
    if (j == rq)
        rq = NULL;
    for (d = 0; d < depth && !j->orphan; d++)
        if (pages[d].radio && pages[d].net_seq == j->seq)
            p = &pages[d];
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
                amce_strlcat(v, line, sizeof(v));
            }
            plex_set_photo_libs(v);
            pictures_root_again();
            if (n == 1) {
                amce_strlcpy(sec, p->list.items[0].path, sizeof(sec));
                amce_strlcpy(p->title, p->list.items[0].label, sizeof(p->title));
                list_free(&p->list);
                list_init(&p->list);
                sprintf(m, "photos: one photo library, %s", sec);
                amce_log("plex", m);
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
            amce_strlcpy(p->base, "@", sizeof(p->base));
            sprintf(m, "%s: one music library, %s", p->title, sec + 1);
            amce_log("plex", m);
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
                amce_strlcpy(p->plex_label, a->label, sizeof(p->plex_label));
                amce_strlcpy(p->plex_meta, a->path, sizeof(p->plex_meta));
                sprintf(title, "%.44s - %.44s", p->title, a->label);
                amce_strlcpy(p->title, title, sizeof(p->title));
                list_free(&p->list);
                list_init(&p->list);
                amce_log("plex: one album", key);
                plex_refetch(p, key);
                if (j->body)
                    FreeVec(j->body);
                FreeVec(j);
                return;
            }
        }
        if (!j->body || n <= 0) {
            const char *why = !j->body && strstr(j->err, " 401")
                              ? "The server wants a sign-in token: see About > Plex in AmigaMCE"
                              : !j->body ? j->err
                              : n < 0 ? "The server answered with something unexpected"
                              : !strcmp(p->base, "/library/sections") ? "No music libraries there"
                              : plex_photo_sections(p->base) ? "No photo libraries there"
                                                             : "Nothing here";
            list_add(&p->list, why[0] ? why : "Could not reach the Plex server", "", "", IT_TEXT);
            sprintf(m, "%s: %s", p->title, why);
        } else {
            if (servers)
                sprintf(m, "%s: %ld found in %ld ms", p->title, (long)n, (long)j->ms);
            else
                sprintf(m, "%s: %ld listed of %ld, %lu KB from %s in %ld ms, read in %lu ms",
                        p->title, (long)n, (long)total, (unsigned long)((j->len + 512) >> 10),
                        j->server, (long)j->ms, (unsigned long)j->ms_read);
            if (!servers && total > n)
                list_add(&p->list, "(more than AmigaMCE shows at once: the first listed)", "", "",
                         IT_TEXT);
            if (p->photos) {
                /* photos here: a slideshow of them, first */
                LONG i, k = 0;
                char label[48];
                struct Item *a;
                for (i = 0; i < p->list.count; i++)
                    if (p->list.items[i].kind == IT_PLEXPHOTO)
                        k++;
                if (k == 1)
                    strcpy(label, "Show the photo");
                else
                    sprintf(label, "Slideshow of these %ld photos", (long)k);
                if (k && (a = list_add_first(&p->list, label, "", "", IT_ACTION))) {
                    a->action = ACT_PLEXSLIDES;
                    a->mark = MARK_PLAY;
                }
            }
        }
        amce_log("plex", m);
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
            n = radio_fill(p->base, j->body, j->len, &p->list, radio_https, &skipped);
        if (!j->body || n <= 0) {
            const char *why = !j->body ? j->err
                              : n < 0 ? "radio-browser.info answered with something unexpected"
                              : p->base[0] == '#' ? "Nothing listed"
                                                  : "No stations here that AmigaMCE can play";
            list_add(&p->list, why[0] ? why : "Could not reach radio-browser.info", "", "",
                     IT_TEXT);
            sprintf(m, "%s: %s", p->title, why);
            amce_log("radio", m);
        } else {
            sprintf(m, "%s: %ld listed, %ld left out, from %s in %ld ms", p->title, (long)n,
                    (long)skipped, j->server, (long)j->ms);
            amce_log("radio", m);
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
    if (p->radio && p->net_seq && rq && rq->seq == p->net_seq) {
        net_cancel(rq);
        rq->orphan = TRUE;                  /* freed when it comes back */
        rq = NULL;
        msg[0] = 0;
    }
    p->net_seq = 0;
}

/* Every request given up and the network process stopped (quitting, or a
 * game starting). */
static void radio_stop(void)
{
    int d;
    if (rq) {
        net_cancel(rq);
        rq->orphan = TRUE;
        rq = NULL;
    }
    for (d = 0; d < depth; d++)
        pages[d].net_seq = 0;
    net_stop();
}

static void radio_open(struct Item *it)
{
    struct Page *p;
    struct NetJob *j;
    BOOL photos;
    if (it->flags & ITEM_DISABLED)
        return;
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
            set_msg("Too many levels open");
            return;
        }
        p->plex = 1;
        p->radio = 1;
        plex_section_menu(it->path + 1, &p->list);
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    if (it->kind == IT_PLEXDIR && it->path[0] != '#' && !plex.host[0]) {
        set_msg("No Plex server chosen: Settings > Networking > Plex");
        return;
    }
    photos = cur() && (cur()->photos || cur()->pics);  /* Pictures' Plex pages */
    if (rq) {                               /* one out at a time: the last given up */
        net_cancel(rq);
        rq->orphan = TRUE;
        rq = NULL;
    }
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR))) {
        set_msg("Not enough memory");
        return;
    }
    if (!(p = page_push(PT_LIST, it->label))) {
        FreeVec(j);
        set_msg("Too many levels open");
        return;
    }
    p->radio = 1;
    amce_strlcpy(p->base, it->path, sizeof(p->base));
    if (it->kind == IT_PLEXDIR) {
        p->plex = 1;
        p->photos = photos;
        radio_field(it, 0, p->base, sizeof(p->base));  /* its key; the rest is its details */
        if (strstr(it->path, "\talbum\t")) {
            /* an album: its details stay beside its tracks */
            amce_strlcpy(p->plex_label, it->label, sizeof(p->plex_label));
            amce_strlcpy(p->plex_meta, it->path, sizeof(p->plex_meta));
        }
        if (!strcmp(p->base, "#gdm")) {
            j->gdm = 1;
            amce_strlcpy(msg, "Looking for Plex servers...  Esc to give up", sizeof(msg));
        } else {
            amce_strlcpy(j->host, plex.host, sizeof(j->host));
            j->port = plex.port;
            plex_request(p->base, j->path, sizeof(j->path));
            j->parse = 1;                   /* read in the network process */
            amce_strlcpy(j->pkey, p->base, sizeof(j->pkey));
            sprintf(msg, "Asking %.40s...  Esc to give up", plex.name[0] ? plex.name : plex.host);
        }
    } else {
        radio_set_max_kbps(rkbps_now());
        radio_request(p->base, radio_https && caps.amissl, j->path, sizeof(j->path));
        amce_strlcpy(msg, "Asking radio-browser.info...  Esc to give up", sizeof(msg));
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
    char lines[400], label[120], *l, *e, *t;
    char keys[6][64], titles[6][96];
    struct Item *it;
    int n = 0, i;
    if (!caps.net || !plex.on || !plex.host[0])
        return 0;
    amce_strlcpy(lines, plex_photo_libs(), sizeof(lines));
    for (l = lines; *l && n < 6; l = e) {
        e = strchr(l, '\n');
        if (e)
            *e++ = 0;
        else
            e = l + strlen(l);
        if (!(t = strchr(l, '\t')))
            continue;
        *t++ = 0;
        amce_strlcpy(keys[n], l, sizeof(keys[n]));
        amce_strlcpy(titles[n], t, sizeof(titles[n]));
        n++;
    }
    if (!n) {
        sprintf(label, "Plex photos on %.80s", plex.name[0] ? plex.name : plex.host);
        if ((it = list_add_first(&p->list, label, "/library/sections?photos=1", "", IT_PLEXDIR)))
            it->mark = MARK_ARROW;
        return 1;
    }
    for (i = n - 1; i >= 0; i--) {
        sprintf(label, "Plex: %.100s", titles[i]);
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
    n = pics_read_ui(PICS_ROOT, &r->list);
    if (n <= 0 && n != -2)
        list_add(&r->list, n < 0 ? "No " PICS_ROOT " here" : PICS_ROOT " is empty", "", "",
                 IT_TEXT);
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
    amce_strlcpy(p->base, key, sizeof(p->base));
    amce_strlcpy(j->host, plex.host, sizeof(j->host));
    j->port = plex.port;
    plex_request(key, j->path, sizeof(j->path));
    j->parse = 1;
    amce_strlcpy(j->pkey, key, sizeof(j->pkey));
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
    amce_strlcpy(j->host, plex.host, sizeof(j->host));
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
static BOOL art_fetch(int kind, const char *thumb, WORD w, WORD h, char *file)
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
    if (!thumb[0] || !plex.host[0] || w < 8 || h < 8)
        return FALSE;
    if (plex_art(thumb, w, h, req, sizeof(req), file, 120))
        return TRUE;
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    amce_strlcpy(j->host, plex.host, sizeof(j->host));
    j->port = plex.port;
    amce_strlcpy(j->path, req, sizeof(j->path));
    j->seq = 0;
    amce_strlcpy(art_want[kind], file, sizeof(art_want[kind]));
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

/* The cover's file to the loader, made for the square. */
static void np_art_load(const char *file)
{
    struct LJob *j;
    np_art_free();
    if (!np.art_s || !pv_pictures() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return;
    j->type = LJ_SHOT;
    j->mode = SHOT_FILE;
    amce_strlcpy(j->path, file, sizeof(j->path));
    j->scr = g.scr;
    j->remap = g.truecolor;
    j->want_pic = FALSE;
    if (gfx_thumbs(&g)) {
        j->remap = FALSE;
        j->tw = j->th = plex_art_side() <= np.art_s ? plex_art_side() : np.art_s;
        j->tall = gfx_mode_tall(g.mode);
        j->maxcol = gfx_thumb_colours(&g);
        if (plex_colours() && j->maxcol > plex_colours())
            j->maxcol = plex_colours();
    }
    np_aj = j;
    ldr_submit_first(j);
}

/* The cover in its square, when it is made. */
static void np_art_draw(void)
{
    WORD dh = 0;
    BOOL taken = FALSE;
    if (!np_aj || !np_aj->done || !np_on() || !mp_is_track() || !np.art_s || np_art_drawn)
        return;
    if (np_aj->thumb &&
        gfx_thumb(&g, np_aj->thumb, &np_aj->info, np.art_x, np.art_y, np.art_s, np.art_s, &dh)) {
        /* the square around a cover not square (the one before may show there) */
        if (g.pic_x > np.art_x)
            gfx_fill_bg(&g, np.art_x, np.art_y, g.pic_x - 1, np.art_y + np.art_s - 1);
        if (g.pic_x + g.pic_w < np.art_x + np.art_s)
            gfx_fill_bg(&g, g.pic_x + g.pic_w, np.art_y, np.art_x + np.art_s - 1,
                        np.art_y + np.art_s - 1);
        if (dh < np.art_s)
            gfx_fill_bg(&g, g.pic_x, np.art_y + dh, g.pic_x + g.pic_w - 1, np.art_y + np.art_s - 1);
    }
    else if (np_aj->obj && gfx_picture_obj(&g, np_aj->obj, np_aj->remapped, np.art_x, np.art_y,
                                           np.art_s, np.art_s, TRUE, &dh, &taken) && taken)
        np_aj->obj = NULL;                  /* the screen has it (its pens) */
    if (g.pic_w) {
        /* what is on screen, and where: the album's pane takes it back */
        amce_strlcpy(pv_onscr.key, np_aj->path, sizeof(pv_onscr.key));
        pv_onscr.x = np.art_x;
        pv_onscr.y = np.art_y;
        pv_onscr.w = pv_onscr.h = np.art_s;
    }
    np_art_drawn = TRUE;
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
    if (!strcmp(thumb, shown) && thumb[0] &&
        (np_aj || art_nj[ART_NP] || (g.pic_w && !strcmp(np_art_file, pv_onscr.key))))
        return;                             /* the same album: its cover stays as it is */
    amce_strlcpy(shown, thumb, sizeof(shown));
    np_art_free();
    np_art_file[0] = 0;
    np_layout();                            /* the square's size */
    if (!thumb[0] && np_on() && np.art_s && g.pic_w)   /* none: the last one goes */
        gfx_picture_clear(&g, np.art_x, np.art_y, np.art_x + np.art_s - 1,
                          np.art_y + np.art_s - 1);
    if (!np.art_s)
        return;
    /* the pane's size where it fits: the same file, and the same picture,
     * which can then go across as it is (pic_take) */
    s = plex_art_side() <= np.art_s ? plex_art_side() : np.art_s;
    if (art_fetch(ART_NP, thumb, s, s, file)) {
        amce_strlcpy(np_art_file, file, sizeof(np_art_file));
        if (np_on() && pic_take(file, np.art_x, np.art_y, np.art_s, np.art_s)) {
            np_art_drawn = TRUE;            /* the one on screen: moved */
            return;
        }
        if (!np_on() && g.pic_w && !strcmp(file, pv_onscr.key))
            return;                         /* in the pane: taken when the page opens */
        np_art_load(file);
    } else {
        amce_strlcpy(np_art_file, file, sizeof(np_art_file));     /* asked for: art_ready */
    }
}

/* ---- the details pane of a Plex list (artists, albums): the picture
 * above (the server's, made for the place), the details below */
static void pv_wrap(WORD x, WORD *y, WORD w, WORD max_y, const char *s, UBYTE pen)
{
    char line[160];
    WORD lh = g.font_h + U(1);
    while (*s && *y + g.font_h - 1 <= max_y) {
        LONG n = 0, fit = 0;
        while (s[n] && n < (LONG)sizeof(line) - 1) {
            LONG e = n;
            while (s[e] && s[e] != ' ' && e < (LONG)sizeof(line) - 1)
                e++;
            memcpy(line, s, e);
            line[e] = 0;
            if (gfx_textlen(&g, line, TS_SMALL) > w && fit)
                break;
            fit = e;
            n = e;
            if (s[n] == ' ')
                n++;
            if (gfx_textlen(&g, line, TS_SMALL) > w)
                break;                      /* one word longer than the line: cut */
        }
        if (!fit)
            fit = n ? n : 1;
        memcpy(line, s, fit);
        line[fit] = 0;
        gfx_text(&g, x, *y, line, pen, PEN_BG, w, TS_SMALL);
        *y += lh;
        s += fit;
        while (*s == ' ')
            s++;
    }
}

/* What a Plex page's details pane shows for entry idx: the entry (an
 * artist, an album) or, on an album's own page, the album. */
static BOOL plex_pv_src(struct Page *p, LONG idx, struct Item *out)
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
static WORD plex_art_side(void)
{
    WORD w = aside_width() - U(4), h = (g.footer_y - 2 - g.list_top) / 2, s = w < h ? w : h;
    WORD cap = plex_tier() == 0 ? 160 : plex_tier() == 1 ? 224 : 400;
    return s > cap ? cap : s;
}

/* Entry idx shows the picture on screen now, where it is (an album's
 * tracks all show its cover: kept, not drawn again). */
static BOOL plex_keeps(struct Page *p, LONG idx)
{
    struct Item src;
    char thumb[160], req[400], file[120];
    WORD s = plex_art_side();
    if (!g.pic_w || !pv_onscr.key[0] || !plex_pv_src(p, idx, &src))
        return FALSE;
    radio_field(&src, 2, thumb, sizeof(thumb));
    if (!thumb[0])
        return FALSE;
    plex_art(thumb, s, s, req, sizeof(req), file, sizeof(file));
    return !strcmp(file, pv_onscr.key) && pv_onscr.x == aside_x() && pv_onscr.y == g.list_top &&
           pv_onscr.h == s;
}

/* ---- an artist's biography, an album's review: left out of a library's
 * long lists (excludeFields), so asked for alone when the pane shows one,
 * and kept for the last few shown */
#define SUM_KEPT 8
static struct { char key[120]; char text[300]; } sum_kept[SUM_KEPT];
static int sum_next;
static char sum_want[120];          /* its key */
static WORD sum_y;                  /* where its text goes in the pane */

/* The item's own key: /library/metadata/<n> (its list key without
 * /children); FALSE where it has none (not an artist or album). */
static BOOL sum_key(const struct Item *it, char *out, LONG max)
{
    char *c;
    radio_field(it, 0, out, max);
    if (strncmp(out, "/library/metadata/", 18))
        return FALSE;
    if ((c = strstr(out, "/children")))
        *c = 0;
    return TRUE;
}

static void plex_sum_find(const struct Item *it, char *out, LONG max)
{
    char k[120];
    int i;
    if (!sum_key(it, k, sizeof(k)))
        return;
    for (i = 0; i < SUM_KEPT; i++)
        if (sum_kept[i].key[0] && !strcmp(sum_kept[i].key, k)) {
            amce_strlcpy(out, sum_kept[i].text, max);
            return;
        }
}

static void plex_sum_ask(const struct Item *it, WORD y)
{
    struct NetJob *j;
    char k[120];
    int i;
    if (!sum_key(it, k, sizeof(k)) || !plex.host[0])
        return;
    for (i = 0; i < SUM_KEPT; i++)
        if (sum_kept[i].key[0] && !strcmp(sum_kept[i].key, k))
            return;                         /* asked before: it has none */
    if (plex_sj) {
        if (!strcmp(sum_want, k)) {
            sum_y = y;
            return;                         /* on its way */
        }
        net_cancel(plex_sj);
        plex_sj->orphan = TRUE;             /* freed when it comes back */
        plex_sj->seq = 0;
        plex_sj = NULL;
    }
    if (!net_start() || !(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return;
    amce_strlcpy(j->host, plex.host, sizeof(j->host));
    j->port = plex.port;
    plex_request(k, j->path, sizeof(j->path));
    amce_strlcpy(sum_want, k, sizeof(sum_want));
    sum_y = y;
    plex_sj = j;
    net_submit(j);
    if (j->done && !j->busy)
        radio_done(j);
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
    amce_strlcpy(sum_kept[i].key, sum_want, sizeof(sum_kept[i].key));
    amce_strlcpy(sum_kept[i].text, text, sizeof(sum_kept[i].text));
    sum_next = (sum_next + 1) % SUM_KEPT;
    if (!text[0] || !p || !p->plex || p != pv_page || p->sel != pv_sel || !pv_shown ||
        pane_pending || !plex_pv_src(p, p->sel, &src) || !sum_key(&src, k, sizeof(k)) ||
        strcmp(k, sum_want))
        return;
    {
        WORD y = sum_y + U(3), w = aside_width() - U(4);
        pv_wrap(aside_x(), &y, w, g.footer_y - 2, text, PEN_DIM);
    }
}

static BOOL plex_pv_key(struct Page *p, char *key)
{
    struct Item src;
    char thumb[160];
    if (!plex_pv_src(p, p->sel, &src))
        return FALSE;
    radio_field(&src, 2, thumb, sizeof(thumb));
    sprintf(key, "%.90s\t%.160s", src.label, thumb);
    return TRUE;
}

/* The details on screen are this page's too (into an album from its
 * entry, or back): left as they are, cover and text. */
static BOOL plex_text_kept(struct Page *p)
{
    char key[260];
    return p->plex && plex_shown[0] && plex_keeps(p, p->sel) && plex_pv_key(p, key) &&
           !strcmp(key, plex_shown);
}

/* The picture on screen (anywhere: Now playing's square) is the one entry
 * idx shows, and fits the pane: plex_pv_draw moves it rather than clear it. */
static BOOL plex_movable(struct Page *p, LONG idx)
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

/* The cover on screen moved into the pane (plex_movable), not drawn again:
 * TRUE if it was. */
static BOOL plex_take(struct Page *p)
{
    struct Item src;
    char thumb[160], req[400], file[120];
    WORD s = plex_art_side();
    if (!plex_movable(p, p->sel) || !plex_pv_src(p, p->sel, &src))
        return FALSE;
    radio_field(&src, 2, thumb, sizeof(thumb));
    plex_art(thumb, s, s, req, sizeof(req), file, sizeof(file));
    if (!pic_take(file, aside_x(), g.list_top, aside_width() - U(4), s))
        return FALSE;
    plex_shown[0] = 0;          /* what was under it is another page's: the text written again */
    return TRUE;
}

/* Photos' pictures for the pane, kept in RAM: for the last few only (a
 * library of thousands would fill it); albums' covers stay, as Music's. */
#define PV_PHOTOS_KEPT 12
static char pv_photo_file[PV_PHOTOS_KEPT][120];
static int pv_photo_next;

static void pv_photo_keep(const char *file)
{
    int i;
    for (i = 0; i < PV_PHOTOS_KEPT; i++)
        if (!strcmp(pv_photo_file[i], file))
            return;
    if (pv_photo_file[pv_photo_next][0])
        DeleteFile((CONST_STRPTR)pv_photo_file[pv_photo_next]);
    amce_strlcpy(pv_photo_file[pv_photo_next], file, sizeof(pv_photo_file[0]));
    pv_photo_next = (pv_photo_next + 1) % PV_PHOTOS_KEPT;
}

static void plex_pv_draw(struct Page *p, BOOL was_shown)
{
    char *last = plex_shown;
    WORD x0 = aside_x(), w = aside_width() - U(4), max_y = g.footer_y - 2, y, s = plex_art_side();
    struct Item src;
    char thumb[160], year[8], by[100], count[24], studio[60], summary[300], line[200], file[120];
    char key[260];
    BOOL keep = plex_keeps(p, p->sel);
    file[0] = 0;
    pv_has_box = FALSE;
    pv_text_drawn = TRUE;
    if (!plex_pv_src(p, p->sel, &src)) {
        gfx_picture_clear(&g, x0, g.list_top, g.w - 1, max_y);
        gfx_fill_bg(&g, x0, g.list_top, g.w - 1, max_y);
        last[0] = 0;
        return;
    }
    (void)was_shown;
    radio_field(&src, 2, thumb, sizeof(thumb));
    sprintf(key, "%.90s\t%.160s", src.label, thumb);
    if (!keep && thumb[0] && pv_pictures() && g.pic_w) {
        /* the cover on screen elsewhere (back from Now playing): moved
         * here as it is, the pane cleared around it */
        char req[400];
        plex_art(thumb, s, s, req, sizeof(req), file, sizeof(file));
        if (pic_take(file, x0, g.list_top, w, s)) {
            fill_around_pic(x0, g.list_top, g.w - 1, max_y);
            last[0] = 0;                    /* the text below it: written again */
            keep = TRUE;
        }
    }
    if (keep) {
        pv_has_box = TRUE;
        pv_pic_drawn = TRUE;
        pv_box_y = g.list_top;
        pv_box_h = s;
        if (!strcmp(key, last))
            return;                         /* the same album's: nothing changes */
        gfx_fill_bg(&g, x0, g.list_top + s, g.w - 1, max_y);
    } else {
        gfx_picture_clear(&g, x0, g.list_top, g.w - 1, max_y);
        gfx_fill_bg(&g, x0, g.list_top, g.w - 1, max_y);
    }
    amce_strlcpy(last, key, sizeof(plex_shown));
    radio_field(&src, 3, year, sizeof(year));
    radio_field(&src, 4, by, sizeof(by));
    radio_field(&src, 5, count, sizeof(count));
    radio_field(&src, 6, studio, sizeof(studio));
    radio_field(&src, 7, summary, sizeof(summary));
    if (!summary[0])
        plex_sum_find(&src, summary, sizeof(summary));  /* left out of a library's list */
    pv_box_y = g.list_top;
    pv_box_h = s;
    y = g.list_top;
    if (thumb[0] && pv_pictures()) {
        pv_has_box = TRUE;
        y = pv_box_y + pv_box_h + U(4);
        if (!keep && art_fetch(ART_PV, thumb, s, s, file)) {
            pv_colour_cap = plex_colours();
            pv_sj = pv_request(LJ_SHOT, SHOT_FILE, file, FALSE, TRUE);
            pv_colour_cap = 0;
        }
        if (src.kind == IT_PLEXPHOTO && file[0])
            pv_photo_keep(file);            /* deleted when a dozen more have come */
    }
    gfx_text(&g, x0, y, src.label, PEN_WHITE, PEN_BG, w, TS_BOLD);
    y += g.font_h + U(2);
    if (by[0]) {
        gfx_text(&g, x0, y, by, PEN_TEXT, PEN_BG, w, TS_SMALL);
        y += g.font_h + U(1);
    }
    line[0] = 0;
    if (year[0])
        amce_strlcat(line, year, sizeof(line));
    if (count[0]) {
        amce_strlcat(line, line[0] ? "   " : "", sizeof(line));
        amce_strlcat(line, count, sizeof(line));
    }
    if (studio[0]) {
        amce_strlcat(line, line[0] ? "   " : "", sizeof(line));
        amce_strlcat(line, studio, sizeof(line));
    }
    if (line[0]) {
        gfx_text(&g, x0, y, line, PEN_DIM, PEN_BG, w, TS_SMALL);
        y += g.font_h + U(1);
    }
    if (summary[0]) {
        y += U(3);
        pv_wrap(x0, &y, w, max_y, summary, PEN_DIM);
    } else {
        plex_sum_ask(&src, y);              /* asked for: written here when it comes */
    }
    if (pv_sj && pv_sj->done)
        pv_progress(p);
}

/* A picture of the server's is here (kept in file). */
static void art_ready(int kind, const char *file)
{
    struct Page *p = cur();
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
    if (p && p->plex && p == pv_page && p->sel == pv_sel && pv_shown && !pv_sj && pv_has_box &&
        !pane_pending) {
        pv_colour_cap = plex_colours();
        pv_sj = pv_request(LJ_SHOT, SHOT_FILE, file, FALSE, TRUE);
        pv_colour_cap = 0;
        if (pv_sj && pv_sj->done)
            pv_progress(p);
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
    amce_strlcpy(host, hp, sizeof(host));
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
    amce_log("plex: server chosen", hp);
    if (depth >= 2 && menu_sel >= 0 && menu[menu_sel] == MOD_SETTINGS) {
        /* chosen in Settings > Plex: back there, the row naming it */
        page_pop();
        plex_settings_again(TRUE);
        set_msg("Plex server chosen");
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

/* About > Plex in AmigaMCE: what it plays, and how to connect. */
static void plex_help(void)
{
    struct Page *p = page_push(PT_TEXT, "Plex in AmigaMCE");
    char line[200];
    if (!p)
        return;
    text_para(p, "AmigaMCE plays music from a Plex Media Server on your network: its "
                 "music libraries, by artist and album, with their covers, played on the "
                 "Amiga's own sound (mpega.library). The server makes each track for this "
                 "Amiga: a 22 kHz mono MP3 for a 68020/030, 24 kHz stereo for a 68040/060, "
                 "and 24 kHz stereo at a higher bitrate for a fast one. On a fast Amiga, "
                 "turning off Settings > Networking > Plex > Request lower quality audio "
                 "plays MP3 files as stored (other formats are converted either way); "
                 "Settings > Networking > Plex > Plex turns it all off.");
    text_line(p, "");
    text_para(p, "Finding a server: Settings > Networking > Plex > Plex server asks the "
                 "network (Plex's GDM); choose one and it is kept. Or set its address from the Shell: "
                 "SetEnv SAVE AmigaMCE/PlexServer 192.168.1.10:32400");
    text_line(p, "");
    text_para(p, "Signing in: a server asks for its owner's token. Find it in Plex Web (an "
                 "item's Get Info > View XML: the X-Plex-Token in the address) and set it from "
                 "the Shell: SetEnv SAVE AmigaMCE/PlexToken <token>. Signing in at plex.tv "
                 "needs HTTPS (AmiSSL) and comes later.");
    text_line(p, "");
    sprintf(line, "Server: %s", plex.host[0] ? plex.host : "none chosen");
    text_line(p, line);
    text_line(p, plex.token[0] ? "Token: set" : "Token: not set");
    text_line(p, caps.mp3_mpega ? "MP3 decoding: mpega.library found"
                                : "MP3 decoding: mpega.library not found");
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
        sprintf(f, "%s, %ld kbit/s", line, (long)br);
        amce_strlcpy(line, f, sizeof(line));
    }
    sprintf(f, "Format:   %s", line);
    text_line(p, f);
    radio_field(it, 1, line, sizeof(line));
    if (line[0]) {
        sprintf(f, "Country:  %s", line);
        text_line(p, f);
    }
    radio_field(it, 4, line, sizeof(line));
    sprintf(f, "Votes:    %s", line[0] ? line : "0");
    text_line(p, f);
    radio_field(it, 2, line, sizeof(line));
    if (line[0]) {
        sprintf(f, "Tags:     %s", line);
        text_para(p, f);
    }
    radio_field(it, 3, line, sizeof(line));
    if (line[0]) {
        sprintf(f, "Homepage: %s", line);
        text_para(p, f);
    }
    radio_field(it, 0, line, sizeof(line));
    sprintf(f, "Stream:   %s", line);
    text_para(p, f);
    text_line(p, "");
    text_para(p, "Fire on the station in its list plays it.");
    draw_pane(TRUE);
    draw_footer();
}

/* Settings > Music folders > Add a folder: asl.library's requester, for
 * folders only, on AmigaMCE's screen. */
static void music_add_folder(void)
{
    struct FileRequester *fr;
    char path[300];
    BOOL db, got = FALSE;
    int i;
    if (nmusic_dirs >= MUSIC_DIRS) {
        set_msg("Eight music folders at most: take one out first");
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
        ASLFR_TitleText, (ULONG)"A folder of MP3 or FLAC files", ASLFR_PositiveText, (ULONG)"Add",
        ASLFR_DrawersOnly, TRUE, ASLFR_InitialDrawer,
        (ULONG)(nmusic_dirs ? music_dirs[nmusic_dirs - 1] : "SYS:"), TAG_DONE);
    if (fr && AslRequest(fr, NULL) && fr->fr_Drawer && fr->fr_Drawer[0]) {
        /* made whole (a name typed may be relative to where AmigaMCE runs) */
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR old = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        if ((l = Lock((CONST_STRPTR)fr->fr_Drawer, SHARED_LOCK))) {
            got = NameFromLock(l, (STRPTR)path, sizeof(music_dirs[0]) - 1) != 0;
            UnLock(l);
        }
        me->pr_WindowPtr = old;
        if (!got)
            set_msg("That folder could not be found");
    }
    if (fr)
        FreeAslRequest(fr);
    if (db)
        gfx_dbuf_start(&g);
    if (got) {
        for (i = 0; i < nmusic_dirs; i++)
            if (!amce_stricmp(music_dirs[i], path))
                break;
        if (i < nmusic_dirs) {
            set_msg("That folder is in Music already");
        } else {
            amce_strlcpy(music_dirs[nmusic_dirs++], path, sizeof(music_dirs[0]));
            save_music_dirs();
            settings_rebuild();
            if (cur() && cur()->setp == SETP_MUSIC)
                cur()->sel = nmusic_dirs;   /* on the folder added */
            set_msg("Folder added: its music is under Music");
        }
    }
    draw_all();                             /* where the requester was */
}

static void act_action(struct Item *it)
{
    if (it->flags & ITEM_DISABLED) {
        if (it->action == ACT_STYLES)
            set_msg("Style: Smooth needs an RTG screen");
        else if (it->action == ACT_PLEXCONV)
            set_msg("This Amiga plays Plex's lower quality: the files as stored are too much");
        else if (it->action >= ACT_NIC0 && it->action <= ACT_NIC_END)
            set_msg("That interface is not up");
        else if (it->action == ACT_SND_AHI)
            set_msg(caps.ahi ? "AHI: coming in a later version" : "AHI is not installed");
        else if (it->action == ACT_SND_MHI)
            set_msg(caps.mp3_mhi ? "MHI: coming in a later version" : "MHI is not installed");
        else if (it->action == ACT_SND_FLAC)
            set_msg(caps.flacdec ? "FLAC: music files, and Plex's tracks as stored on a fast Amiga"
                                 : "Install flacdec.library (Aminet: util/libs/flacdec) for FLAC");
        else if (it->action == ACT_SND_MPEGA)
            set_msg("mpega.library is not installed: MP3s cannot be played");
        return;
    }
    if (it->action == ACT_SCREENMODES)
        open_screenmodes();
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
    else if (it->action == ACT_DBUF)
        toggle_dbuf();
    else if (it->action == ACT_HTTPS)
        toggle_https();
    else if (it->action == ACT_PLEXCONV)
        toggle_plex_convert();
    else if (it->action == ACT_PLEXON)
        toggle_plex_on();
    else if (it->action == ACT_PLEXSET)
        open_plex_settings();
    else if (it->action == ACT_SOUNDSET)
        open_sound_settings();
    else if (it->action == ACT_GENSET)
        open_settings_page("General", build_general_settings);
    else if (it->action == ACT_SCREENSET)
        open_settings_page("Screen", build_screen_settings);
    else if (it->action == ACT_NICSET)
        open_settings_page("Network interface", build_nic_settings);
    else if (it->action == ACT_RADIOSET)
        open_settings_page("Radio", build_radio_settings);
    else if (it->action == ACT_RKBPSSET)
        open_settings_page("Highest station bitrate", build_rkbps_settings);
    else if (it->action == ACT_BUFSET)
        open_settings_page("Stream buffer", build_buf_settings);
    else if (it->action == ACT_RSRVSET) {
        /* the servers asked for and timed now: a few seconds at most */
        set_msg("Asking radio-browser.info for its servers...");
        busy_begin();
        nmirror = net_radio_mirrors(mirrors, 8);
        busy_end(FALSE);
        set_msg(NULL);
        open_settings_page("Station list server", build_rsrv_settings);
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
        set_msg("Station lists opened from now on");
    } else if (it->action >= ACT_BUF0 && it->action <= ACT_BUF_END) {
        buf_sel = it->action - ACT_BUF0;
        save_setting(BUF_VAR, buf_names[buf_sel]);
        st_set_buffer(buf_sel);
        plex_settings_again(FALSE);
        set_msg("The next station or track started");
    } else if (it->action == ACT_RSRVAUTO || it->action == ACT_RSRVFAST ||
               (it->action >= ACT_RSRV0 && it->action <= ACT_RSRV_END)) {
        if (it->action == ACT_RSRVAUTO) {
            rsrv[0] = 0;
        } else if (it->action == ACT_RSRVFAST) {
            int f = mirror_fastest();
            if (f < 0) {
                set_msg("None of the servers answered: Automatic kept");
                return;
            }
            sprintf(rsrv, "fastest:%.60s", mirrors[f].name);
        } else {
            int i = it->action - ACT_RSRV0;
            if (i >= nmirror)
                return;
            amce_strlcpy(rsrv, mirrors[i].name, sizeof(rsrv));
        }
        save_setting(RSRV_VAR, rsrv);
        rsrv_apply();
        plex_settings_again(FALSE);
    }
    else if (it->action == ACT_NETSET)
        open_settings_page("Networking", build_net_settings);
    else if (it->action == ACT_PLEXSLIDES && cur())
        plex_photo_view(cur(), -1, TRUE);
    else if (it->action == ACT_MUSICSET) {
        music_rm = -1;
        open_settings_page("Music folders", build_music_settings);
    } else if (it->action == ACT_MUSICASSIGN) {
        music_assign = !music_assign;
        save_setting(MUSICASSIGN_VAR, music_assign ? "" : "off");
        music_rm = -1;
        plex_settings_again(FALSE);
        set_msg(music_assign ? "Music lists MUSIC: when it is assigned" : "Music leaves MUSIC: out");
    } else if (it->action == ACT_MUSICADD) {
        music_rm = -1;
        music_add_folder();
    } else if (it->action >= ACT_MUSICDIR0 && it->action <= ACT_MUSICDIR_END) {
        int i = it->action - ACT_MUSICDIR0;
        if (i >= nmusic_dirs)
            return;
        if (music_rm != i) {
            music_rm = i;
            plex_settings_again(FALSE);
            set_msg("Fire again to take this folder out of Music (its files stay)");
            return;
        }
        for (; i < nmusic_dirs - 1; i++)
            strcpy(music_dirs[i], music_dirs[i + 1]);
        nmusic_dirs--;
        music_rm = -1;
        save_music_dirs();
        plex_settings_again(FALSE);
        set_msg("Folder taken out of Music");
    }
    else if (it->action == ACT_NICAUTO)
        choose_nic(-1);
    else if (it->action >= ACT_NIC0 && it->action <= ACT_NIC_END)
        choose_nic(it->action - ACT_NIC0);
    else if (it->action == ACT_NOWPLAYING)
        np_open_page();
    else if (it->action == ACT_PLEXHELP)
        plex_help();
    else if (it->action == ACT_STYLE_PIXEL)
        apply_style(STYLE_PIXEL);
    else if (it->action == ACT_STYLE_SMOOTH)
        apply_style(STYLE_SMOOTH);
    else if (it->action >= ACT_LFONT0 && it->action <= ACT_LFONT_END)
        apply_laced_font(it->action - ACT_LFONT0);
    else if (it->action >= ACT_MODE0 && it->action < ACT_MODE0 + SM_COUNT)
        apply_screen_mode(it->action - ACT_MODE0);
}

/* Holding Up/Down in a list or text page scrolls faster the longer it is
 * held, without skipping anything: lists still move one entry at a time,
 * only more often, and text glides at a rising pixel speed. AmigaMCE paces
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
} hold;

/* Long titled lists, held past cruising: a whole letter a step. Shorter
 * ones glide on smoothly, the big letter going by: one of 1,000 entries
 * is crossed in about 4 s that way (top speed, LIST_BOOST x cruising),
 * and a letter at a time would skip most of it. */
#define LETTER_MIN_LIST 1000    /* entries a list needs for that */
#define LETTERS_MIN   4     /* letters a second to begin with, */
#define LETTERS_MAX  12     /* rising to this over HOLD_BOOST */

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
    ov_ms = ov_n = 0;
}

static void hold_stop(void)
{
    struct Page *p = cur();
    if (hold.dir && hold.held_ms > HOLD_DELAY && p && p->type == PT_LIST) {
        char m[200];
        LONG moved = p->sel - hold.from;
        sprintf(m, "%ld entries in %ld ms (%ld by the clock), longest frame %ld ms, "
                   "%ld frames over 40 ms; "
                   "moving the list %ld ms, flipping %ld ms, %ld letters in %ld ms",
                (long)(moved < 0 ? -moved : moved), (long)hold.held_ms,
                (long)(time_ms() - hold.t_down), (long)hold.worst, (long)hold.slow,
                (long)hold.move_ms, (long)hold.present_ms, (long)ov_n,
                (long)ov_ms);
        amce_log("glide", m);
    }
    hold.dir = 0;
}

/* The first entry of the next letter (dir > 0), or of this letter, or the
 * one before if already there (dir < 0); from itself if there is none. */
static LONG letter_step(struct Page *p, LONG from, LONG dir)
{
    const struct Item *it = p->list.items;
    LONG i = from, n = p->list.count;
    char c = ov_letter_of(it[from].label);
    if (dir > 0) {
        while (i < n && ov_letter_of(it[i].label) == c)
            i++;
        return i < n ? i : from;
    }
    if (i > 0 && ov_letter_of(it[i - 1].label) == c) {
        while (i > 0 && ov_letter_of(it[i - 1].label) == c)
            i--;
        return i;
    }
    if (i == 0)
        return from;
    c = ov_letter_of(it[--i].label);
    while (i > 0 && ov_letter_of(it[i - 1].label) == c)
        i--;
    return i;
}

static BOOL hold_active(void)
{
    return hold.dir != 0;
}

/* Is this raw key physically down right now? Asked of the keyboard itself
 * (lowlevel.library QueryKeys), not inferred from messages, which can be
 * late or queued behind repeats. Without lowlevel, assume yes. */
static BOOL key_is_down(UWORD rawkey)
{
    struct KeyQuery kq;
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
    if (!hold.dir)
        return;
    if ((focus != FOCUS_CONTENT && focus != FOCUS_PREVIEW) || !p ||
        (focus == FOCUS_PREVIEW && !pv_tb.open) ||
        (!hold.joy && !key_is_down(hold.dir < 0 ? 0x4C : 0x4D))) {
        hold_stop();                        /* let go: stop now, whatever is queued */
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
        if (dt > 250)
            dt = 250;               /* a slow frame is made up for, a stall is not */
        hold.last_ms = now;
        hold.held_ms += dt;
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
            /* Cruising: two entries a frame, or six screenfuls a second on
             * screens that show more rows; above one per frame the list
             * moves several rows in a single frame, still drawn every frame. */
            LONG top = LIST_MAX, screens = LIST_SCREENS * list_visible(), n;
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
                rate = LIST_MIN + (top - LIST_MIN) * ramp / HOLD_RAMP;
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
            if (!menu && rate > 10 + remaining * 10) /* ease in: 1010/s at 100 left ... 20/s at 1 */
                rate = 10 + remaining * 10;
            hold.acc += rate * elapsed;     /* entries x 1000 */
            n = hold.acc / 1000;
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

static void act_open(BOOL play)
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
        if (pv_pending || !pv_shown || !pv_text_drawn) {    /* not shown yet: show it now */
            ov.on = FALSE;
            preview_now(p);
        }
        if (pv_tb.open && pv_tb.bar_w)
            set_focus(FOCUS_PREVIEW);
        return;
    }
    act_open(FALSE);
}

static void act_back(void)
{
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
        set_msg("Press Esc again to quit AmigaMCE");
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

static void act_letter(char c)
{
    busy_begin();                   /* a letter not read yet can take a while */
    act_letter_now(c);
    busy_end(TRUE);
}

static void act_letter_now(char c)
{
    struct Page *p;
    nav_moved();
    pane_settle();
    p = cur();
    LONG i, n;
    char k[2];
    if (focus != FOCUS_CONTENT || !p || p->type != PT_LIST || !p->list.count)
        return;
    k[0] = c;
    k[1] = 0;
    /* still loading: read on until the letter turns up after the
     * selection (or the list is complete, then wrap round as usual) */
    for (i = p->sel + 1; ch_reading(p) || i < p->list.count; i++) {
        char first[2];
        if (i >= p->list.count) {
            if (!ch_wait_one(p, TRUE))
                break;
            i--;
            continue;
        }
        first[0] = p->list.items[i].label[0];
        first[1] = 0;
        if (amce_stricmp(first, k) == 0) {
            p->sel = i;
            draw_title_count(p);
            draw_list_rows(p);
            return;
        }
    }
    n = p->list.count;
    for (i = 1; i <= n; i++) {
        LONG idx = (p->sel + i) % n;
        char first[2];
        first[0] = p->list.items[idx].label[0];
        first[1] = 0;
        if (amce_stricmp(first, k) == 0) {
            p->sel = idx;
            draw_title_count(p);
            draw_list_rows(p);
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
        pv_dir = dir;
        act_move(to - p->sel);
    }
}

static void disarm(void)
{
    if (quit_armed || msg[0]) {
        quit_armed = FALSE;
        set_msg(NULL);
    }
}

/* ------------------------------------------------------------ input */
static ULONG last_secs, last_mics;
static BOOL click_opened;           /* the last click opened something (one click) */
static LONG last_click = -1;

/* Dragging a list's thumb: the list follows, the selection kept in view. */
static void gut_drag_to(WORD my)
{
    struct Page *p = cur();
    WORD bx0, bx1, ty, th;
    LONG vis = list_visible(), span, pos, top;
    if (!list_overflows(p)) {
        gut.drag = FALSE;
        return;
    }
    gut_geom(p, &bx0, &bx1, &ty, &th);
    span = (g.footer_y - 2 - g.list_top + 1) - th;
    if (span <= 0)
        return;
    pos = my - gut.off - g.list_top;
    if (pos < 0)
        pos = 0;
    if (pos > span)
        pos = span;
    top = pos * (p->list.count - vis) / span;
    if (top == p->top)
        return;
    p->top = top;
    if (p->sel < top)
        p->sel = top;
    if (p->sel > top + vis - 1)
        p->sel = top + vis - 1;
    if (ov.on && ov.page == p)
        ov_relabel(ov_letter_of(p->list.items[p->sel].label));
    draw_title_count(p);
    draw_list_rows(p);
}

/* A click in a long list's column: on the thumb, drag it; above or below
 * it, a page up or down. FALSE if the click was not there. */
static BOOL gut_click(WORD x, WORD y)
{
    struct Page *p = cur();
    WORD bx0, bx1, ty, th;
    LONG vis = list_visible();
    if (!list_overflows(p) || y < g.list_top || y > g.footer_y - 2 ||
        x <= list_edge(p) - GUT_W || x > list_edge(p) ||
        (!has_aside(p) && ov.on && ov.page == p))
        return FALSE;
    gut_geom(p, &bx0, &bx1, &ty, &th);
    if (focus != FOCUS_CONTENT)
        set_focus(FOCUS_CONTENT);
    if (y >= ty && y < ty + th) {
        gut.drag = TRUE;
        gut.off = y - ty;
        draw_gutter(p);
    } else {
        act_move(y < ty ? -(vis - 1) : vis - 1);
    }
    return TRUE;
}

static void handle_mouse(struct IntuiMessage *im)
{
    WORD x = im->MouseX, y = im->MouseY;
    BOOL dbl;
    LONG idx = -1;
    struct TextBox *tb;
    if (im->Class == IDCMP_MOUSEBUTTONS && !(x < g.menu_w && y >= g.top))
        pane_settle();              /* a click outside the menu acts on the section */
    tb = cur_tb();
    if (!tb && pv_tb.open)
        tb = &pv_tb;                /* the preview pane's fact sheet */
    set_mouse_mode(TRUE);                   /* the mouse is in use: scroll bars */
    pointer_show();
    mouse_last = time_ms();
    if (!bar_up) {
        bar_up = TRUE;
        if (cur() && cur()->type == PT_LIST && !pane_pending)
            draw_gutter(cur());
    }
    if (im->Class == IDCMP_MOUSEMOVE) {
        if (gut.drag)
            gut_drag_to(y);
        else if (tb && tb->dragging)
            tb_mouse_move(tb, y);
        else {
            hover_at(x, y);
            np_hover(x, y);
        }
        return;
    }
    if (im->Code == SELECTUP) {
        np_mouse_up();
        if (gut.drag) {
            gut.drag = FALSE;
            if (cur())
                draw_gutter(cur());
        }
        if (tb && tb->dragging) {
            tb_mouse_up(tb);
            tb_refresh_bar(&g, tb);
        }
        return;
    }
    if (im->Code == MENUDOWN) {
        disarm();
        act_back();
        return;
    }
    if (im->Code != SELECTDOWN)
        return;
    nav_moved();
    disarm();
    if (click_opened) {
        /* a single click opened something: the second click of a habitual
         * double-click is not a click on what opened */
        click_opened = FALSE;
        if (DoubleClick(last_secs, last_mics, im->Seconds, im->Micros)) {
            last_secs = im->Seconds;
            last_mics = im->Micros;
            last_click = -1;
            return;
        }
    }
    if (gut_click(x, y))
        return;
    {
        int c = crumb_at(x, y);
        if (c >= 0) {
            crumb_go(c);                    /* a breadcrumb link: back to that level */
            click_opened = TRUE;
            return;
        }
    }
    if (tb && tb_mouse_down(tb, x, y)) {
        set_focus(tb == &pv_tb ? FOCUS_PREVIEW : FOCUS_CONTENT);
        if (tb->dragging)
            tb_refresh_bar(&g, tb);
        return;
    }
    if (x < g.menu_w && y >= g.top && y < g.top + nmenu * g.row_h) {
        idx = (y - g.top) / g.row_h;
        dbl = idx + 1000 == last_click && DoubleClick(last_secs, last_mics, im->Seconds, im->Micros);
        last_click = idx + 1000;
        if (focus != FOCUS_MENU)
            set_focus(FOCUS_MENU);
        menu_select_now(idx, TRUE);
        (void)dbl;
        if (focus == FOCUS_MENU && cur() && (cur()->type != PT_LIST || cur()->list.count)) {
            act_open(FALSE);                /* a section is not played: one click goes in */
            click_opened = TRUE;
        }
    } else if (x >= g.pane_x - U(2) && y >= g.list_top && y < g.footer_y - 1) {
        struct Page *p = cur();
        if (!p)
            return;
        if (p->type == PT_LIST) {
            if (x > list_xr(p)) {           /* the details pane */
                if (pv_tb.open && pv_tb.bar_w && x >= aside_x())
                    set_focus(FOCUS_PREVIEW);
                return;
            }
            idx = p->top + (y - g.list_top) / g.list_row_h;
            if (idx >= p->list.count)
                return;
            dbl = idx == last_click && DoubleClick(last_secs, last_mics, im->Seconds, im->Micros);
            last_click = idx;
            if (focus != FOCUS_CONTENT)
                set_focus(FOCUS_CONTENT);
            if (p->sel != idx) {
                p->sel = idx;
                draw_title_count(p);
                draw_list_rows(p);
            }
            {
                /* one click opens what cannot be played (a folder, a
                 * setting); what can (a game, a picture, a folder that
                 * plays as a slideshow) is selected, and a double-click
                 * plays it (a folder: goes inside) */
                struct Item *it = &p->list.items[idx];
                BOOL playable = it->kind == IT_RUN || it->kind == IT_PIC || it->kind == IT_MOD ||
                                it->kind == IT_SONG || it->kind == IT_PLEXPHOTO ||
                                (it->kind == IT_DIR && it->mark == MARK_PLAY);
                if (dbl || !playable) {
                    ch_wait_item(p, idx, TRUE);
                    it = &p->list.items[idx];
                    if (!playable)
                        click_opened = TRUE;
                    if (it->kind == IT_DIR)
                        open_dir(it);
                    else
                        act_open(FALSE);
                }
            }
        } else {
            set_focus(FOCUS_CONTENT);
            np_click(x, y);                 /* the player's buttons and bar */
        }
    }
    last_secs = im->Seconds;
    last_mics = im->Micros;
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

    if (im->Class == IDCMP_RAWKEY) {
        if (code & IECODE_UP_PREFIX) {
            UWORD k = code & ~IECODE_UP_PREFIX;
            if ((k == 0x4C || k == 0x4D) && hold.dir && !hold.joy)
                hold_stop();
            return;
        }
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
        case 0x7A: disarm(); act_wheel(-1, im->MouseX, im->MouseY); break; /* NM_WHEEL_UP */
        case 0x7B: disarm(); act_wheel(1, im->MouseX, im->MouseY); break;  /* NM_WHEEL_DOWN */
        }
        return;
    }
    /* IDCMP_VANILLAKEY */
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
        case 'n': case 'N': case '>': disarm(); np_press(NB_NEXT); return;
        case 'p': case 'P': case '<': disarm(); np_press(NB_PREV); return;
        case 's': case 'S': disarm(); np_press(NB_STOP); return;
        case ',': disarm(); np_press(NB_BACK); return;
        case '.': disarm(); np_press(NB_FWD); return;
        }
        if (code != 27 && code != 8 && code != 13)
            return;                 /* no letters to jump to here */
    }
    switch (code) {
    case 27: act_back(); return;                 /* Esc: Back, twice at the menu quits */
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
    st = ReadJoyPort(1);
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
            if (hold.joy)
                hold_stop();
        } else if (++joy_hold >= 8 && (joy_hold - 8) % 3 == 0) {
            act = dir;          /* repeat after ~320 ms, then every ~120 ms */
        }
    } else {
        joy_hold = 0;
        if (hold.joy)
            hold_stop();
    }
    joy_last = dir | btn;
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
    for (i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) {
        m = order[i];
        if (m == MOD_RADIO && !(caps.net && (caps.mp3_mhi || caps.mp3_mpega)))
            continue;           /* needs a network stack and an MP3 path */
        menu[nmenu++] = m;
    }
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
 * AmigaMCE runs on a stack of its own, MAIN_STACK bytes, whatever it was
 * started with. Statics, not locals: the stack moves under them. */
#define MAIN_STACK (128UL * 1024)
static struct StackSwapStruct main_sss;
static APTR main_stack;
static int main_rc;

int amce_cpu_ok(void);              /* cpucheck.c: built for the 68000 */

int main(void)
{
    struct Task *me;
    if (!amce_cpu_ok())
        return 20;                          /* a 68000/010: said in a requester */
    me = FindTask(NULL);
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

static int app_main(void)
{
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    BOOL timer_open = FALSE;
    ULONG next_sec = 0, tick_us = 40000;
    int rc = 0;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    if (!IntuitionBase || !GfxBase) {
        say("AmigaMCE needs Kickstart 3.0 or newer.\n");
        rc = 20;
        goto out;
    }
    LayersBase = OpenLibrary((CONST_STRPTR)"layers.library", 39);
    if (!LayersBase) {
        say("AmigaMCE needs Kickstart 3.0 or newer.\n");
        rc = 20;
        goto out;
    }
    dbg_mem_baseline();
    load_debug();
    load_dbuf();
    load_https();
    load_jump();
    amce_log_open(debug_on);
    open_extra_libs();
    {
        /* PROGDIR:AmigaMCE.noloader: do the reading in the UI (to compare) */
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR oldwin = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        l = Lock((CONST_STRPTR)"PROGDIR:AmigaMCE.noloader", SHARED_LOCK);
        me->pr_WindowPtr = oldwin;
        if (l) {
            UnLock(l);
            amce_log("loader", "off (AmigaMCE.noloader)");
        } else if (ldr_start()) {
            extern struct Task *ldr_task, *ldr_task2, *ldr_task3;
            char m[128];
            sprintf(m, "started (tasks: AmigaMCE %08lx, loader %08lx, pictures %08lx, "
                       "picture now %08lx)",
                    (unsigned long)FindTask(NULL), (unsigned long)ldr_task, (unsigned long)ldr_task2,
                    (unsigned long)ldr_task3);
            amce_log("loader", m);
        } else {
            amce_log("loader", "could not start: reading in the UI");
        }
    }

    caps_detect(&caps);
    plex_load(caps.cpu);            /* after: it needs the CPU */
    plex.flac = caps.flacdec;
    load_music_dirs();
    songs_can_play(caps.mp3_mpega, caps.flacdec);   /* what music lists show */
    load_net_if();                  /* after: the interface's address, toward the server */
    load_radio_settings();
    ags_init(&ags);
    build_menu();
    load_screen_mode();
    load_style();
    {
        /* Which display the Workbench uses: a native <-> RTG switch on exit
         * can be slow in some emulators and drivers. */
        struct Screen *wb = LockPubScreen(NULL);
        if (wb) {
            ULONG id = GetVPModeID(&wb->ViewPort);
            amce_log("Workbench screen", caps_mode_is_rtg(id) ? "RTG" : "native chipset");
            UnlockPubScreen(NULL, wb);
        }
    }

    gfx_menu_labels((const char *const *)mod_names, MOD_COUNT);
    if (!gfx_open(&g, &caps, screen_mode, ui_style) &&
        (screen_mode == SM_AUTO || !gfx_open(&g, &caps, SM_AUTO, ui_style))) {
        say("AmigaMCE could not open its screen.\n");
        rc = 20;
        goto out;
    }

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

    if (debug_on)
        dbg_cpu_start();
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
            amce_log("navigation", in_list ? "back where the last run left off (in the list)"
                                            : "back where the last run left off (in the menu)");
        nav_sum_seen = nav_sum_written = nav_sum();
    }
    draw_all();
    if (screen_mode != SM_AUTO && g.mode != screen_mode) {
        screen_mode = SM_AUTO;      /* not saved: the choice comes back if it works again */
        set_msg("Saved screen mode not available here: using Automatic");
    } else {
        note_style();
    }

    while (running) {
        ULONG wsig = g.win ? 1UL << g.win->UserPort->mp_SigBit : 0;
        loop_wakes++;
        ULONG tsig = timer_open ? 1UL << tport->mp_SigBit : 0;
        ULONG lsig = ldr_sigmask();
        ULONG nsig = net_sigmask();
        ULONG mask = wsig | tsig | lsig | nsig | SIGBREAKF_CTRL_C;
        struct TextBox *atb = cur_tb();
        /* While a text box glides, poll instead of sleeping; WaitTOF below
         * paces the loop to one step per frame. */
        BOOL busy = (atb && tb_animating(atb)) || hold_active() || pane_pending || pv_pending ||
                    tb_animating(&pv_tb);
        ULONG sig;
        if (loader_due())
            preview_arrived();
        if (busy) {
            sig = SetSignal(0, mask) & mask;
        } else {
            dbg_sleep_begin();
            sig = Wait(mask);
            dbg_sleep_end();
        }
        if (sig & SIGBREAKF_CTRL_C)
            break;
        if (sig & lsig)
            preview_arrived();          /* the loader brought something */
        if (sig & nsig)
            radio_collect();            /* radio-browser.info answered */
        if (sig & tsig) {
            if (GetMsg(tport)) {
                joy_poll();
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
                    /* the header, shown without a flip (gfx_quiet_*) */
                    BOOL was = gfx_quiet_begin();
                    if (ds.ds_Minute != last_minute)
                        draw_clock();
                    if (debug_on) {
                        fps_shown = gfx_flips - fps_flips0;
                        fps_flips0 = gfx_flips;
                        cpu_load = dbg_cpu_sample();
                        app_load = dbg_app_sample();
                        draw_status();
                        dbg_load_log();
                    }
                    gfx_quiet_end(&g, was, 0, 0, g.w - 1, U(24) - 1);
                    nav_tick_1s();          /* the trail kept, once it rests */
                    mp_tempo_check();
                }
                mp_paula_check();           /* every tick: a moment's change seen too */
                pv_spin_tick();             /* the preview's spinner, if it is waiting */
                radio_spin();               /* a Radio list's, likewise */
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
                           np.flash >= 0 || np.rep >= 0 ||
                           (np_list_on() && mp_playing())) ? 40000 :
                          mp_playing() ? 120000 : 200000;  /* the header's bars dance */
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
                if (copy.Class == IDCMP_INACTIVEWINDOW)
                    hold_stop();    /* the key-up will not come to us */
                else if (copy.Class == IDCMP_MOUSEBUTTONS || copy.Class == IDCMP_MOUSEMOVE)
                    handle_mouse(&copy);
                else if (copy.Class == IDCMP_RAWKEY || copy.Class == IDCMP_VANILLAKEY)
                    handle_key(&copy);
                if (!g.win)
                    break;      /* a launch closed and reopened the window */
            }
        }
        if (g.win && (hold_active() || pane_pending || pv_pending || tb_animating(&pv_tb) ||
                      ((atb = cur_tb()) && tb_animating(atb)))) {
            ULONG tp = time_ms();
            BOOL flipped = gfx_present(&g);
            if (hold.dir && flipped)
                hold.present_ms += time_ms() - tp;
            if (!flipped) {             /* a flip paces the frame, or else */
                dbg_sleep_begin();
                WaitTOF();
                dbg_sleep_end();
            }
            pane_tick();
            preview_tick();
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
        sprintf(m, "%ld read ahead, %ld waited for", (long)pv_ready, (long)pv_waited);
        amce_log("previews", m);
    }
    amce_log("quitting", "");
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
    amce_log("timer closed", "");
    pv_flush();
    pics_forget();
    ldr_stop();
    pages_clear();
    amce_log("pages freed", "");
    mp_shutdown();
    if (np.list.pool)
        list_free(&np.list);
    ptr_hidden = NULL;              /* a new window comes with its pointer */
    spin_free();
    ov_bm_free();
    gfx_close(&g);
    amce_log("screen closed", "");
    if (ptr_blank)
        FreeMem(ptr_blank, 6 * sizeof(UWORD));
    ags_free(&ags);
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
    amce_log("libraries closed, exiting", "");
    amce_log_close();
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
