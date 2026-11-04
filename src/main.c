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

static const char version_tag[] __attribute__((used)) =
    "$VER: AmigaMCE " AMCE_VERSION " (" AMCE_DATE ")";

struct IntuitionBase *IntuitionBase;
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
    UBYTE pics;                 /* a Pictures folder (pics.c): IT_DIR and IT_PIC rows */
    UBYTE mods;                 /* a Mods folder (mods.c): IT_DIR and IT_MOD rows */
    struct Chunks *ch;          /* a long list held a letter at a time (see Chunk) */
    UBYTE title_wide;           /* title drawn with room for a growing count */
    char pic[160];              /* PT_TEXT: a picture at the end of the text ("" none), */
    struct BitMap *pic_bm;      /*   made for the screen when the page is first drawn */
    WORD pic_w, pic_h;
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
       ACT_STYLE_PIXEL, ACT_STYLE_SMOOTH, ACT_JUMP, ACT_NOWPLAYING,
       ACT_MODE0 = 16 /* + SM_* */ };

/* ------------------------------------------------------------ pages */
static struct Page *cur(void) { return depth ? &pages[depth - 1] : NULL; }

static struct Page *page_push(UBYTE type, const char *title)
{
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
    if (!list_init(&p->list))
        return NULL;
    p->type = type;
    amce_strlcpy(p->title, title, sizeof(p->title));
    depth++;
    return p;
}

static WORD textbox_w(void);

/* A text page's picture (Page.pic), made for this screen: as wide as the
 * text and no taller than the box, in pens of its own (gfx_thumb_bitmap). */
static void page_picture(struct Page *p)
{
    WORD w = textbox_w(), h = g.footer_y - 2 - g.list_top;
    struct PicInfo pi;
    struct Thumb *t;
    BOOL tall = gfx_mode_tall(g.mode);
    if (!gfx_thumbs(&g))
        return;
    if (!(t = ilbm_thumb(p->pic, w, h, tall, TRUE, gfx_thumb_colours(&g), &pi))) {
        BOOL remapped = FALSE;
        APTR o = pic_decode(p->pic, NULL, FALSE, &remapped, &pi);
        if (o) {
            ULONG dw, dh;
            pic_fit(pi.w, pi.h, tall, w, h, TRUE, &dw, &dh);
            t = thumb_make(o, dw, dh, gfx_thumb_colours(&g));
            pic_dispose(o);
        }
    }
    if (!t)
        return;
    if ((p->pic_bm = gfx_thumb_bitmap(&g, t))) {
        p->pic_w = t->w;
        p->pic_h = t->h;
    }
    thumb_free(t);
}

static void page_pop(void)
{
    ov.on = FALSE;
    tb_close(&pv_tb);
    pv_shown = FALSE;
    pv_tj = pv_sj = NULL;
    if (focus == FOCUS_PREVIEW)
        focus = FOCUS_CONTENT;
    if (!depth)
        return;
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

static void busy_end(BOOL redraw)
{
    if (busy.depth && --busy.depth == 0 && busy.on) {
        busy.on = FALSE;
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
#define JUMP_VAR  "AmigaMCE/JumpScroll"

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
    char buf[16];
    if (GetVar((CONST_STRPTR)STYLE_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0)
        ui_style = amce_stricmp(buf, "smooth") == 0 ? STYLE_SMOOTH : STYLE_PIXEL;
}

static void load_jump(void)
{
    char buf[8];
    jump_scroll = GetVar((CONST_STRPTR)JUMP_VAR, (STRPTR)buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0 &&
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

static void build_settings(struct Page *p)
{
    char tag[64];
    amce_strlcpy(tag, gfx_mode_label(g.mode), sizeof(tag));
    struct Item *it;
    action_item(p, "Screen mode", tag, ACT_SCREENMODES, MARK_ARROW);
    /* Style: greyed out, showing Pixel, where this screen can only be pixel
     * (native modes, 8-bit RTG) */
    if (gfx_style_possible(&caps, g.mode, STYLE_SMOOTH)) {
        action_item(p, "Style", ui_style == STYLE_SMOOTH ? "Smooth" : "Pixel", ACT_STYLES,
                    MARK_ARROW);
    } else if ((it = action_item(p, "Style", "Pixel", ACT_STYLES, MARK_ARROW))) {
        it->flags |= ITEM_DISABLED;
    }
    action_item(p, "Jump scrolling (by letter)", "", ACT_JUMP,
                jump_scroll ? MARK_CHECK_ON : MARK_CHECK_OFF);
    action_item(p, "Debug (CPU, memory, log)", "", ACT_DEBUG,
                debug_on ? MARK_CHECK_ON : MARK_CHECK_OFF);
}

#define SETTINGS_STYLE_ROW 1    /* "Style" in build_settings */

/* Settings > Style: radio buttons, like Screen mode. */
static void build_styles(struct Page *p)
{
    BOOL can_smooth = gfx_style_possible(&caps, g.mode, STYLE_SMOOTH);
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
    text_line(p, "(c) 2026 Brian King");
    text_line(p, "");
    text_para(p, "Fonts: Pixelify Sans (c) 2021 The Pixelify Sans Project Authors, and Rubik "
                 "(c) 2015 The Rubik Project Authors, both under the SIL Open Font License 1.1. "
                 "The amce-pixel fonts are Modified Versions of Pixelify Sans.");
    text_line(p, "");
    text_para(p, "Code: libnix C runtime (public domain).");
    text_line(p, "");
    text_para(p, "Used if installed, not included, each under its own licence: Amiga Game "
                 "Selector, WHDLoad, AHI, MHI drivers, mpega.library, datatypes.");
    text_line(p, "");
    text_para(p, "Amiga is a trademark of its owner. Games, demos, music and pictures belong "
                 "to their authors. Licence texts are in the Licences drawer.");
    text_line(p, "");
    text_para(p, "This front-end would not be possible without the immense efforts of Paul Vince "
                 "and Amiga Game Selector.");
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
}

static void build_mods_root(void);
static void np_mark_lists(BOOL redraw);

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
            } else if (n <= 0) {
                page_pop();
                if ((p = page_push(PT_TEXT, "Pictures")))
                    text_para(p, n < 0 ? "No pictures found: AmigaMCE looks for them in "
                                         PICS_ROOT "." : PICS_ROOT " has no pictures in it.");
            }
        }
        break;
    case MOD_MUSIC:
        if ((p = page_push(PT_TEXT, "Music"))) {
            text_para(p, "Music playback arrives in a later version.");
            text_line(p, "");
            text_line(p, caps.mp3_mhi ? "MP3 decoding: MHI driver found" :
                         caps.mp3_mpega ? "MP3 decoding: mpega.library found" : "MP3 decoding: none found");
        }
        break;
    case MOD_RADIO:
        if ((p = page_push(PT_TEXT, "Radio")))
            text_para(p, "Radio arrives in a later version.");
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
static struct {
    int n;
    struct { LONG sel, top; char label[64]; } lv[MAXDEPTH];
} nav_mem[MOD_COUNT];
static int shown_mod = -1;

static void save_nav(void)
{
    int d, n = 0;
    if (shown_mod < 0 || !depth)
        return;
    for (d = 0; d < depth && pages[d].type == PT_LIST; d++, n++) {
        struct Page *p = &pages[d];
        nav_mem[shown_mod].lv[n].sel = p->sel;
        nav_mem[shown_mod].lv[n].top = p->top;
        amce_strlcpy(nav_mem[shown_mod].lv[n].label,
                     p->list.count ? p->list.items[p->sel].label : "", 64);
    }
    nav_mem[shown_mod].n = n;
}

/* Open a folder page without drawing (used while rebuilding the trail). */
static BOOL push_dir_quiet(struct Item *it)
{
    BOOL pics = cur() && cur()->pics, mods = cur() && cur()->mods;
    struct Page *p = page_push(PT_LIST, it->label);
    if (!p)
        return FALSE;
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

/* What the readout shows now, so a second that changes nothing redraws
 * nothing. Cleared when the header is drawn afresh. */
static char status_shown[96];

static void draw_status(void)
{
    WORD x0 = U(6) + g.logo_w + U(10);
    WORD x1 = clock_left() - U(6);
    char cpu[16], app[16], forms[3][96];
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
    /* CPU = whole system (idle-task meter); app = AmigaMCE's own share */
    sprintf(forms[0], "CPU %s  app %s   RAM %luK  (chip %luK)", cpu, app,
            (unsigned long)(used >> 10), (unsigned long)(chip >> 10));
    sprintf(forms[1], "CPU %s  app %s  RAM %luK", cpu, app, (unsigned long)(used >> 10));
    sprintf(forms[2], "%s/%s %luK", cpu, app, (unsigned long)(used >> 10));
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

static const char *hint_text(void)
{
    struct Page *p = cur();
    /* the long hints when the widest of them fits */
    BOOL hi = gfx_textlen(&g, "Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back",
                          TS_SMALL) <= g.w - U(12);
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
    if (p && p->type == PT_LIST && p->mods && p->list.count &&
        p->list.items[p->sel].kind == IT_MOD)
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
    gfx_fill(&g, g.pane_x - U(2), U(26), g.w - 1, U(42) - 1, PEN_BG);
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
        gfx_text(&g, x, U(28), "...", PEN_DIM, PEN_BG, 0, TS_BOLD);
        x += gfx_textlen(&g, "...", TS_BOLD);
        gfx_text(&g, x, U(28), " > ", PEN_DIM, PEN_BG, 0, TS_BOLD);
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
            gfx_text(&g, x, U(28), pages[i].title, last ? PEN_WHITE : PEN_TEXT, bg, room, TS_BOLD);
            crumb[i].x0 = x;
            crumb[i].x1 = x + w - 1;
            x += w;
            if (!last) {
                gfx_text(&g, x, U(28), " > ", PEN_DIM, PEN_BG, 0, TS_BOLD);
                x += sep;
            }
        }
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
    if (p->mods)
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
        if (p->list.items[i].kind == IT_RUN || p->list.items[i].kind == IT_PIC)
            runs++;
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
    if (!p->aside) {
        LONG i;
        p->aside = 1;
        if (p->pics && aside_width() > 0)
            p->aside = 2;           /* pictures and folders both preview */
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
}

static BOOL pv_current(const struct LJob *j)
{
    return j == pv_tj || j == pv_sj;
}

/* The job for (type, mode, path): one already out or back, else a new one.
 * NULL if every slot is busy (a read-ahead is then skipped). first: the
 * pane waits for it, so it goes ahead of the loader's queue. */
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
    } else if (j && j->obj) {
        ok = gfx_picture_obj(&g, j->obj, j->remapped, x0, pv_box_y, w, pv_box_h, TRUE, &dh, &taken);
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
    WORD x0 = aside_x(), max_y = g.footer_y - 2;
    pv_pending = FALSE;
    pv_page = p;
    pv_sel = p->sel;
    pv_shown = TRUE;
    pv_tj = pv_sj = NULL;
    pv_text_drawn = pv_pic_drawn = pv_spin_on = FALSE;
    tb_close(&pv_tb);
    k = p->list.count ? pv_kind(p, p->sel, path, sizeof(path), &shot) : 0;
    if (!pv_pictures())
        shot = 0;
    pv_box_y = g.list_top;
    pv_box_h = (max_y - g.list_top) / 2;
    pv_has_box = shot != 0;
    if (!pv_since)
        pv_since = time_ms();
    if (k) {
        if (shot)
            pv_sj = pv_request(LJ_SHOT, shot, path, FALSE, TRUE);
        pv_tj = pv_request(k, 0, path, FALSE, TRUE);   /* last in: first out */
    }
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
    if (!pv_tj->done || (pv_sj && !pv_sj->done) || !pv_sj) {
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
static void preview_touch(struct Page *p)
{
    char path[300];
    UBYTE shot;
    int k, i, found = 0, want;
    ULONG settle = PREVIEW_SETTLE;
    if (!has_aside(p) || (pv_shown && pv_page == p && pv_sel == p->sel))
        return;                             /* already showing this one */
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
    if (pv_shown && pv_page == p && !hold_active() && !ov.on) {
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
        const char *tag = it->kind == IT_ACTION ? it->path : p->pics ? it->sortkey : "";
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
        gfx_fill(&g, g.pane_x - U(2), g.list_top, g.w - 1, g.footer_y - 2, PEN_BG);
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

static void open_textbox(struct Page *p, WORD y0)
{
    tb_open(&g, &p->tb, g.pane_x, y0, textbox_w(), g.footer_y - 2 - y0, &p->list,
            p->type == PT_DETAIL);
}

static struct TextBox *cur_tb(void)
{
    struct Page *p = pane_pending ? NULL : cur();   /* a stale section is not shown */
    return (p && p->type != PT_LIST && p->tb.open) ? &p->tb : NULL;
}

static void draw_pane(BOOL full)
{
    struct Page *p = cur();
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
            gfx_fill_bg(&g, px, U(25), g.w - 1, tb->y - 1);
            gfx_fill_bg(&g, px, tb->y, tb->x - 1, tb->y + tb->h - 1);
            gfx_fill_bg(&g, bx1 + 1, tb->y, g.w - 1, tb->y + tb->h - 1);
            gfx_fill_bg(&g, px, tb->y + tb->h, g.w - 1, pb);
        } else if (p->type != PT_PLAYER) {
            gfx_fill_bg(&g, px, U(25), g.w - 1, pb);  /* the player clears its own */
        }
    }
    draw_title(p);
    switch (p->type) {
    case PT_LIST:
        draw_list_rows(p);
        break;
    case PT_TEXT:
        if (!p->tb.open) {
            if (p->pic[0] && !p->pic_bm)
                page_picture(p);
            p->tb.pic = p->pic_bm;          /* scrolls with the text, after it */
            p->tb.pic_w = p->pic_w;
            p->tb.pic_h = p->pic_h;
            open_textbox(p, g.list_top);
        }
        if (full || !p->tb.drawn)
            tb_draw(&g, &p->tb);    /* a focus change alone leaves the box alone */
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
static void np_round(WORD x, WORD y, WORD w, WORD h, WORD t, UBYTE pen, BOOL frame)
{
    WORD r = t * 2;                         /* the corners' size */
    if (!frame) {
        gfx_fill(&g, x, y, x + t - 1, y + t - 1, pen);
        gfx_fill(&g, x + w - t, y, x + w - 1, y + t - 1, pen);
        gfx_fill(&g, x, y + h - t, x + t - 1, y + h - 1, pen);
        gfx_fill(&g, x + w - t, y + h - t, x + w - 1, y + h - 1, pen);
        return;
    }
    /* four edges; the corners (t x t) are left out: slightly rounded */
    (void)r;
    gfx_fill(&g, x + t, y, x + w - 1 - t, y + t - 1, pen);             /* top */
    gfx_fill(&g, x + t, y + h - t, x + w - 1 - t, y + h - 1, pen);     /* bottom */
    gfx_fill(&g, x, y + t, x + t - 1, y + h - 1 - t, pen);             /* left */
    gfx_fill(&g, x + w - t, y + t, x + w - 1, y + h - 1 - t, pen);     /* right */
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
static void np_tri(WORD x, WORD y, WORD w, WORD h, int dir, UBYTE pen)
{
    WORD r, half = (h - 1) / 2;
    for (r = 0; r < h; r++) {
        WORD d = r <= half ? r : h - 1 - r;
        WORD len = (WORD)(((LONG)w * (d + 1) + half) / (half + 1));
        if (len > w)
            len = w;
        if (len < 1)
            len = 1;
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
    WORD gh = h * 5 / 10, gw, cx, gy, t;
    if (i == NB_VDOWN || i == NB_VUP) {
        if ((i == NB_VDOWN && mp_volume() == 0) || (i == NB_VUP && mp_volume() >= 64))
            fg = lit ? PEN_TEXT : PEN_DIM;
    }
    if ((i == NB_BACK || i == NB_FWD) && !mp_can_step(i == NB_FWD ? 1 : -1)) {
        bg = PEN_BAR;                       /* not for this module: greyed, never lit */
        fg = PEN_RULE;
    }
    if (gh < 5)
        gh = 5;
    gw = gh * 3 / 4;                        /* a triangle's width */
    if (gfx_mode_tall(g.mode))
        gw = gw * 2;                        /* tall pixels: as wide on screen */
    cx = x + w / 2;
    gy = y + (h - gh) / 2;
    t = U(2) > 1 ? U(2) : 1;                /* a bar's thickness */
    gfx_fill(&g, x, y, x + w - 1, y + h - 1, bg);
    np_round(x, y, w, h, t, PEN_BG, FALSE);   /* rounded */
    switch (i) {
    case NB_PREV:
        gfx_fill(&g, cx - gw / 2 - t, gy, cx - gw / 2 - 1, gy + gh - 1, fg);
        np_tri(cx - gw / 2, gy, gw, gh, -1, fg);
        break;
    case NB_NEXT:
        np_tri(cx - gw / 2, gy, gw, gh, 1, fg);
        gfx_fill(&g, cx - gw / 2 + gw, gy, cx - gw / 2 + gw + t - 1, gy + gh - 1, fg);
        break;
    case NB_BACK:
        np_tri(cx - gw, gy, gw, gh, -1, fg);
        np_tri(cx, gy, gw, gh, -1, fg);
        break;
    case NB_FWD:
        np_tri(cx - gw, gy, gw, gh, 1, fg);
        np_tri(cx, gy, gw, gh, 1, fg);
        break;
    case NB_PLAY:
        if (mp_playing()) {                 /* pause */
            WORD bw = gw / 3 > 1 ? gw / 3 : 2;
            gfx_fill(&g, cx - bw - bw / 2, gy, cx - bw / 2 - 1, gy + gh - 1, fg);
            gfx_fill(&g, cx + bw / 2 + (bw & 1), gy, cx + bw + bw / 2 + (bw & 1) - 1, gy + gh - 1, fg);
        } else {
            np_tri(cx - gw / 2, gy, gw, gh, 1, fg);
        }
        break;
    case NB_STOP: {
        WORD sw = gh * 4 / 5, sh = sw;
        if (gfx_mode_tall(g.mode))
            sw *= 2;
        gfx_fill(&g, cx - sw / 2, y + (h - sh) / 2, cx - sw / 2 + sw - 1, y + (h - sh) / 2 + sh - 1, fg);
        break;
    }
    case NB_VDOWN:
    case NB_VUP: {
        WORD lw = gw * 4 / 3, ly = y + h / 2 - t / 2;
        gfx_fill(&g, cx - lw / 2, ly, cx - lw / 2 + lw - 1, ly + t - 1, fg);
        if (i == NB_VUP) {
            WORD lh = gh * 4 / 5;
            if (gfx_mode_tall(g.mode))
                lh = lh * 2 / 3;
            gfx_fill(&g, cx - t / 2, y + (h - lh) / 2, cx - t / 2 + t - 1, y + (h - lh) / 2 + lh - 1, fg);
        }
        break;
    }
    }
    if (ring)
        np_round(x, y, w, h, t, PEN_YELLOW, TRUE);  /* what Fire presses */
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
    np.prog_y = np.info_y + row + U(5);
    np.prog_h = U(4) > 2 ? U(4) : 2;
    np.time_y = np.prog_y + np.prog_h + U(4);
    np.meter_y = np.time_y + row + U(6);
    np.meter_h = g.font_h + U(2);
    np.name_w = np.w * 2 / 5;
    np.by = np.meter_y + 4 * (np.meter_h + U(2)) + U(6);
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
    np.samples_y = np.by + np.bh + U(8);
}

/* The time, the bar and the position: only what changed. */
static void np_draw_progress(BOOL full, const struct MpStatus *st)
{
    char t[64], a[24], b[24], pos[96];
    WORD x = np.x0, y = np.time_y, end = np.x0 + np.w - 1, lw, rw;
    LONG fill = st->total_ms ? (LONG)((ULONG)np.w * (st->elapsed_ms / 100) /
                                      ((st->total_ms + 99) / 100)) : 0;
    if (fill > np.w)
        fill = np.w;
    if (full || fill != np.s_fill) {
        if (fill > 0)
            gfx_fill(&g, x, np.prog_y, x + fill - 1, np.prog_y + np.prog_h - 1, PEN_ACCENT);
        if (fill < np.w)
            gfx_fill(&g, x + fill, np.prog_y, end, np.prog_y + np.prog_h - 1, PEN_RULE);
        np.s_fill = fill;
    }
    if (!full && st->elapsed_ms / 1000 == (ULONG)np.s_ms / 1000 && st->order == np.s_order &&
        st->row == np.s_row && (LONG)st->total_ms == np.s_total)
        return;
    np.s_ms = st->elapsed_ms;
    np.s_total = st->total_ms;
    np.s_order = st->order;
    np.s_row = st->row;
    time_text(st->elapsed_ms, a);
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
    lw = gfx_textlen(&g, t, TS_BODY);
    rw = gfx_textlen(&g, pos, TS_SMALL);
    if (lw + U(8) + rw > np.w) {            /* narrow: the short form */
        if (st->songlen)
            sprintf(pos, "%ld/%ld  %02ld", (long)st->order + 1, (long)st->songlen, (long)st->row);
        else
            sprintf(pos, "%ld  %02ld", (long)st->order + 1, (long)st->row);
        rw = gfx_textlen(&g, pos, TS_SMALL);
        if (lw + U(8) + rw > np.w)
            pos[0] = 0, rw = 0;
    }
    gfx_text(&g, x, y, t, PEN_TEXT, PEN_BG, 0, TS_BODY | TS_OPAQUE);
    if (x + lw <= end - rw)
        gfx_fill(&g, x + lw, y, end - rw, y + g.font_h - 1, PEN_BG);
    if (rw)
        gfx_text(&g, end - rw + 1, y, pos, PEN_DIM, PEN_BG, 0, TS_SMALL | TS_OPAQUE);
}

static void np_draw_meters(BOOL full, const struct MpStatus *st)
{
    int i;
    WORD bx = np.x0 + np.name_w + U(4), bw = np.x0 + np.w - bx, bh = np.meter_h - U(4);
    if (bh < 2)
        bh = 2;
    for (i = 0; i < 4; i++) {
        WORD y = np.meter_y + i * (np.meter_h + U(2)), by = y + (np.meter_h - bh) / 2;
        WORD lit = (WORD)((LONG)bw * st->level[i] / 64);
        if (full || st->sample[i] != np.s_sample[i]) {
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
        gfx_fill_bg(&g, g.menu_w + U(3), U(42), g.w - 1, g.footer_y - 2);
        if (!mp_loaded()) {
            gfx_text(&g, np.x0, np.title_y, "Nothing playing", PEN_DIM, PEN_BG, np.w, TS_BOLD);
            return;
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
    BOOL was = gfx_quiet_begin();
    np_draw(FALSE);
    gfx_quiet_end(&g, was, g.pane_x - U(2), np.prog_y, g.w - 1, np.by + np.bh);
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
        if (p->type != PT_LIST || !p->mods)
            continue;
        for (i = 0; i < p->list.count; i++) {
            struct Item *it = &p->list.items[i];
            if (it->kind == IT_MOD)
                it->mark = (now[0] && it->path && !amce_stricmp(it->path, now)) ? MARK_PLAYING
                                                                                : MARK_NONE;
        }
    }
    if (redraw && !pane_pending && cur() && cur()->type == PT_LIST && cur()->mods)
        draw_list_rows(cur());
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
    if (!mp_load(it->path, err, sizeof(err))) {
        busy_end(TRUE);
        np.idx = idx;
        set_msg(err);
        np_mark_lists(TRUE);
        return FALSE;
    }
    busy_end(TRUE);
    np.idx = idx;
    np.ended_seen = FALSE;
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
    for (i = np.idx + d, tries = 0; i >= 0 && i < np.list.count && tries < 8; i += d, tries++)
        if (np_start(i)) {
            if (np_on()) {
                draw_pane(TRUE);
                draw_footer();
            }
            return TRUE;
        }
    return FALSE;
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
            set_msg("That was the last module in the folder");
        return;
    case NB_BACK:
    case NB_FWD:
        if (!mp_can_step(b == NB_FWD ? 1 : -1)) {
            np.flash = -1;                  /* greyed out: no flash, nothing done */
            if (np.rep == b)
                np.rep = -1;
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
    if (mp_loaded() && mp_running() && !amce_stricmp(mp_path(), sel->path)) {
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
        if (it->kind != IT_MOD)
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
    return p && p->type == PT_LIST && p->mods && mp_loaded();
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
    } else if (!still && now - last >= 120) {
        last = now;
        gfx_eq_frame++;
        np_eq_redraw();
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
    np_repeat();
    np_eq_tick();
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
    if (y >= np.prog_y - U(3) && y < np.time_y && x >= np.x0 && x < np.x0 + np.w) {
        struct MpStatus st;
        ULONG want;
        int pos, best = 0;
        mp_status(&st);
        if (!st.total_ms)
            return TRUE;                    /* its length is not known */
        want = st.total_ms / 100 * (ULONG)(x - np.x0) / (ULONG)np.w * 100;
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
    if (mp_loaded()) {
        const char *t = mp_title();
        char label[120];
        while (*t == ' ')
            t++;
        if (!t[0]) {
            mods_label(FilePart((STRPTR)mp_path()), label, sizeof(label));
            t = label;
        }
        action_item(p, "Now playing", t, ACT_NOWPLAYING, MARK_PLAYING);
    }
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
    if (first)
        gfx_fill_bg(&g, g.menu_w + U(3), U(25), g.w - 1, g.footer_y - 2);
    else
        gfx_fill(&g, g.pane_x - U(2), U(26), g.w - 1, U(42) - 1, PEN_BG);
    gfx_text(&g, g.pane_x, U(28), mod_names[menu[menu_sel]], PEN_WHITE, PEN_BG, g.pane_w, TS_BOLD);
    gfx_fill(&g, g.pane_x, U(41), g.pane_x + g.pane_w - 1, U(42) - 1, PEN_RULE);
}

/* now: build the section straight away (a click), not once it settles */
static void menu_select_now(int i, BOOL now)
{
    int old = menu_sel;
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
    if (focus == f)
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
    BOOL pics = cur() && cur()->pics, mods = cur() && cur()->mods;
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
    LowLevelBase = DataTypesBase = DiskfontBase = NULL;
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
        if (vw_job->busy)
            ldr_wait(vw_job);               /* nearly there: sooner than starting over */
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

/* Show the pictures of l from entry idx; returns the last one shown. */
static LONG viewer_run(const struct ItemList *l, LONG idx, BOOL slideshow)
{
    struct PicInfo pi = { 0 };
    char scale[24];
    LONG shown = -1, want = idx, dir = 1;
    ULONG bar_until = 0, next_at = 0, jlast = 0;
    BOOL quit = FALSE, ok = FALSE, bar_due = TRUE;

    if (idx < 0 || idx >= l->count || l->items[idx].kind != IT_PIC)
        return idx;
    hold_stop();
    ov.on = FALSE;
    tb_close(&pv_tb);
    pv_shown = FALSE;
    pv_pending = FALSE;
    pv_flush();                     /* the viewer takes the screen's colours over */
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
            vw_ahead(l, shown, dir);
            next_at = time_ms() + SLIDE_MS;
            if (ok && bar_due) {
                viewer_bar(l, shown, &pi, scale, slideshow);
                bar_until = time_ms() + BAR_MS;
            }
            bar_due = !slideshow;
        }
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
        cur()->sel = 0;
    {
        struct Page *p = page_push(PT_LIST, "Screen mode");
        if (p)
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
    if (p && p->type == PT_LIST) {
        list_free(&p->list);
        list_init(&p->list);
        build_settings(p);
        p->sel = sel;
        draw_list_rows(p);
    }
}

static void toggle_debug(void)
{
    struct Page *p = cur();
    LONG sel = p ? p->sel : 0;
    set_debug(!debug_on);
    save_setting(DEBUG_VAR, debug_on ? "1" : "0");
    if (p && p->type == PT_LIST) {
        list_free(&p->list);
        list_init(&p->list);
        build_settings(p);
        p->sel = sel;
        draw_list_rows(p);
    }
    draw_status();
}

/* Settings > Style. Remembered either way; the screen is reopened only
 * when the look changes (smooth needs an RTG screen). */
static void apply_style(int st)
{
    struct Page *p = cur();
    int i;
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
        if (depth >= 2) {
            struct Page *up = &pages[depth - 2];
            list_free(&up->list);
            list_init(&up->list);
            build_settings(up);
            up->sel = SETTINGS_STYLE_ROW;
        }
        if (st == STYLE_SMOOTH && g.style != STYLE_SMOOTH)
            set_msg("Smooth: on RTG screens only");
        return;
    }
    /* the same screen, its look made again (no mode switch) */
    pages_clear();
    pv_flush();
    spin_free();
    ov_bm_free();
    BOOL restyled = gfx_restyle(&g, ui_style);
    /* back where we were: Settings > Style, on the new choice */
    for (i = 0; i < nmenu; i++)
        if (menu[i] == MOD_SETTINGS)
            menu_sel = i;
    build_root(MOD_SETTINGS);
    if (cur())
        cur()->sel = SETTINGS_STYLE_ROW;
    if ((p = page_push(PT_LIST, "Style")))
        build_styles(p);
    focus = FOCUS_CONTENT;
    msg[0] = 0;
    draw_all();
    note_style();
    if (!restyled)
        set_msg("The smooth style's files could not be loaded");
}

static void act_action(struct Item *it)
{
    if (it->flags & ITEM_DISABLED) {
        if (it->action == ACT_STYLES)
            set_msg("Style: Smooth needs an RTG screen");
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
    else if (it->action == ACT_NOWPLAYING)
        np_open_page();
    else if (it->action == ACT_STYLE_PIXEL)
        apply_style(STYLE_PIXEL);
    else if (it->action == ACT_STYLE_SMOOTH)
        apply_style(STYLE_SMOOTH);
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
            BOOL menu = p->list.count <= 2 * list_visible();
            if (screens > top)
                top = screens;
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
        if (it->kind == IT_MOD)
            mods_play_item(p);      /* the music, and its page */
        else if (p->pics && it->kind == IT_DIR && it->mark == MARK_PLAY)
            folder_slideshow(it);   /* Fire plays a folder of pictures */
        else if (it->kind == IT_PIC)
            view_from_page(p);
        else if (it->kind == IT_DIR)
            open_dir(it);
        else if (it->kind == IT_ACTION)
            act_action(it);
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

int main(void)
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

static int app_main(void)
{
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    BOOL timer_open = FALSE;
    ULONG ms_acc = 0, tick_us = 40000;
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
    load_jump();
    amce_log_open(debug_on);
    open_extra_libs();
    {
        /* PROGDIR:AmigaMCE.noloader: do the reading in the UI (to compare) */
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR oldwin = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        /* PROGDIR:AmigaMCE.dbuf: double buffering on RTG screens (prototype) */
        if ((l = Lock((CONST_STRPTR)"PROGDIR:AmigaMCE.dbuf", SHARED_LOCK))) {
            UnLock(l);
            gfx_dbuf_wanted = TRUE;
        }
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
    menu_sel = 0;
    build_root(menu[0]);
    draw_all();
    if (screen_mode != SM_AUTO && g.mode != screen_mode) {
        screen_mode = SM_AUTO;      /* not saved: the choice comes back if it works again */
        set_msg("Saved screen mode not available here: using Automatic");
    } else {
        note_style();
    }

    while (running) {
        ULONG wsig = g.win ? 1UL << g.win->UserPort->mp_SigBit : 0;
        ULONG tsig = timer_open ? 1UL << tport->mp_SigBit : 0;
        ULONG lsig = ldr_sigmask();
        ULONG mask = wsig | tsig | lsig | SIGBREAKF_CTRL_C;
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
        if (sig & tsig) {
            if (GetMsg(tport)) {
                ms_acc += tick_us / 1000;
                joy_poll();
                /* the letter lingers ~0.5 s after the glide ends */
                if (ov.on && !hold_active()) {
                    if (!ov.linger)
                        ov.linger = 13;
                    else if (--ov.linger == 0)
                        ov_hide();
                }
                if (ms_acc >= 1000 && g.win) {
                    struct DateStamp ds;
                    ms_acc -= 1000;
                    DateStamp(&ds);
                    /* the header, shown without a flip (gfx_quiet_*) */
                    BOOL was = gfx_quiet_begin();
                    if (ds.ds_Minute != last_minute)
                        draw_clock();
                    if (debug_on) {
                        cpu_load = dbg_cpu_sample();
                        app_load = dbg_app_sample();
                        draw_status();
                    }
                    gfx_quiet_end(&g, was, 0, 0, g.w - 1, U(24) - 1);
                }
                pv_spin_tick();             /* the preview's spinner, if it is waiting */
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
                tick_us = (joy_present || ov.on || pv_spinning() || np_live() || np.flash >= 0 || np.rep >= 0 ||
                           (np_list_on() && mp_playing())) ? 40000 : 200000;
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
