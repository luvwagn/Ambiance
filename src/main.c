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

enum { PT_LIST, PT_TEXT, PT_DETAIL };
enum { FOCUS_MENU, FOCUS_CONTENT,
       FOCUS_PREVIEW };     /* the preview pane's fact sheet: Up/Down scroll it */
#define MAXDEPTH 12
#define FACT_LINES 24

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
    struct AgsLoad *ld;         /* still being read (long AGS lists, see load_tick) */
    UBYTE title_wide;           /* title drawn with room for a growing count */
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
static int ui_style = STYLE_PIXEL;  /* Settings > Style (smooth only on deep RTG screens) */
static BOOL pane_pending;           /* menu moved, its section not built yet (see pane_settle) */
static struct TextBox pv_tb;        /* the preview pane's fact sheet (scrolls) */
static struct Page *pv_page;        /* what the preview shows: page and entry */
static LONG pv_sel;
static BOOL pv_shown;               /* FALSE once the pane is cleared or left */
static struct ItemList pv_list;     /* its wrapped lines */
static struct LJob *pv_wait;        /* the loader job the pane waits for */

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
       ACT_STYLE_PIXEL, ACT_STYLE_SMOOTH,
       ACT_MODE0 = 16 /* + SM_* */ };

/* ------------------------------------------------------------ pages */
static struct Page *cur(void) { return depth ? &pages[depth - 1] : NULL; }

static struct Page *page_push(UBYTE type, const char *title)
{
    ov.on = FALSE;
    tb_close(&pv_tb);
    pv_shown = FALSE;
    pv_wait = NULL;
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

static void page_pop(void)
{
    ov.on = FALSE;
    tb_close(&pv_tb);
    pv_shown = FALSE;
    pv_wait = NULL;
    if (focus == FOCUS_PREVIEW)
        focus = FOCUS_CONTENT;
    if (!depth)
        return;
    gfx_picture_free(&g);
    tb_close(&pages[depth - 1].tb);
    ags_load_free(pages[depth - 1].ld);
    pages[depth - 1].ld = NULL;
    list_free(&pages[--depth].list);
}

static void pages_clear(void)
{
    while (depth)
        page_pop();
}

/* ---- long AGS lists, read in steps (ags_load_*): the first screens
 * before the list is shown, the rest while AmigaMCE waits for input, and
 * at once whatever a move, a page or a letter jump reaches for. */
static void set_msg(const char *m);

static BOOL page_load_some(struct Page *p, LONG budget)
{
    if (!p->ld)
        return FALSE;
    if (!ags_load_step(p->ld, &p->list, budget)) {
        BOOL cut = ags_load_cut(p->ld);
        ags_load_free(p->ld);
        p->ld = NULL;
        if (cut) {
            char m[80];
            sprintf(m, "Low on memory: list stops at %ld entries", (long)p->list.count);
            set_msg(m);
        }
        return FALSE;
    }
    return TRUE;
}

static void page_load_until(struct Page *p, LONG count)
{
    while (p->ld && p->list.count < count)
        page_load_some(p, 64);
}

static void page_load_all(struct Page *p)
{
    while (p->ld)
        page_load_some(p, 256);
}

/* Open an AGS folder as a list page: in steps when it is a long flat one. */
static LONG page_read_ags(struct Page *p, const struct Item *it, LONG first)
{
    char path[300];
    LONG n = ags_load_start(item_path(it, path, sizeof(path)), &p->list, ags.adult[0] == '1',
                            (it->flags & ITEM_FLAT) != 0, &p->ld);
    if (n < 0)
        return n;
    page_load_until(p, first);
    return p->list.count;
}

static void text_line(struct Page *p, const char *s)
{
    list_add(&p->list, s, "", "", IT_TEXT);
}

/* Text box width: the pane less room for the scroll bar (gap + bar + gap). */
static WORD textbox_w(void)
{
    return g.pane_w - U(7);
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
    }
    sprintf(line, "Graphics card (RTG): %s", caps.rtg ? "yes" : "no");
    text_line(p, line);
    sprintf(line, "Free memory: %lu KB chip, %lu KB fast",
            (unsigned long)(caps.chipfree >> 10), (unsigned long)(caps.fastfree >> 10));
    text_line(p, line);
    sprintf(line, "Font: %s", g.font_is_ours ? "amce-pixel (bundled)" : "topaz 8 (fallback)");
    text_line(p, line);
    sprintf(line, "Datatypes: %s", DataTypesBase ? "available" : "missing");
    text_line(p, line);
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
    action_item(p, "Smooth", can_smooth ? "" : "RTG 16-bit+ only", ACT_STYLE_SMOOTH,
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

static void build_credits(struct Page *p)
{
    char line[96];
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
        if ((p = page_push(PT_TEXT, "Mods"))) {
            text_para(p, "Module playback arrives in version 0.2.");
            text_line(p, "");
            found_line(p, "Modules", "ST-00:Modules");
            found_line(p, "Menu music", "AGS:Mods");
        }
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
            n = pics_read_dir(PICS_ROOT, &p->list);
            if (n <= 0) {
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
    BOOL pics = cur() && cur()->pics;
    struct Page *p = page_push(PT_LIST, it->label);
    if (!p)
        return FALSE;
    if (pics) {
        p->pics = 1;
        if (pics_read_dir(it->path, &p->list) <= 0) {
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
        page_load_until(p, sel + 2 * list_visible());
        if (p->ld && (sel >= p->list.count || strcmp(p->list.items[sel].label, want) != 0))
            page_load_all(p);
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
    UBYTE bg = sel ? (focus == FOCUS_MENU ? PEN_HL : PEN_SELDIM) : PEN_BG;
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
    if (!msg[0] && hint_shown && hint_text() != hint_shown)
        draw_footer();              /* the hints follow the selection */
    if (p->title_wide && !p->ld) {
        draw_title(p);              /* fully read: the breadcrumb gets its room back */
        return;
    }
    WORD right = g.pane_x + g.pane_w, tw, x;
    if (p->type != PT_LIST || !p->list.count)
        return;
    sprintf(count, "%ld/%ld", (long)p->sel + 1, (long)p->list.count);
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

static void draw_title(struct Page *p)
{
    char t[160], widest[24];
    int i;
    WORD cw = 0;
    t[0] = 0;
    for (i = 0; i < depth; i++) {
        if (i)
            amce_strlcat(t, " > ", sizeof(t));
        amce_strlcat(t, pages[i].title, sizeof(t));
    }
    if (p->type == PT_LIST && p->list.count) {
        /* room for the widest count this list can show, so a growing
         * number never runs into the breadcrumb */
        p->title_wide = p->ld != NULL;
        if (p->ld)
            strcpy(widest, "88888/88888");      /* still growing */
        else
            sprintf(widest, "%ld/%ld", (long)p->list.count, (long)p->list.count);
        cw = gfx_textlen(&g, widest, TS_SMALL);
    }
    gfx_fill(&g, g.pane_x - U(2), U(26), g.w - 1, U(42) - 1, PEN_BG);
    count_left = -1;
    /* show the end of a long breadcrumb */
    {
        const char *s = t;
        WORD maxw = g.pane_w - cw - U(8);
        while (*s && gfx_textlen(&g, s, TS_BOLD) > maxw)
            s++;
        gfx_text(&g, g.pane_x, U(28), s, PEN_WHITE, PEN_BG, maxw, TS_BOLD);
    }
    draw_title_count(p);
    gfx_fill(&g, g.pane_x, U(41), g.pane_x + g.pane_w - 1, U(42) - 1, PEN_RULE);
}

/* Row markers are icons (gfx_mark) in a column on the left before each
 * label. The column is the same width whatever the markers' state, so
 * ticking a box or picking a radio button never moves the text; lists
 * without markers have no column. */
static WORD mark_column(const struct ItemList *l)
{
    LONG i;
    for (i = 0; i < l->count; i++)
        if (l->items[i].mark != MARK_NONE)
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

/* Last x a list row may paint. */
static WORD list_xr(struct Page *p)
{
    if (has_aside(p))
        return aside_x() - U(6) - 1;
    if (ov.on && ov.page == p)
        return ov.strip_x - 1;              /* lowres: the letter's strip is off limits */
    return g.w - 1;
}

/* ---- the preview of the selected game, drawn once the selection settles */
#define PREVIEW_SETTLE 250
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

/* ---- preview jobs: read by the loader (loader.c), the one shown and those
 * next to it, so a steady pace of Down finds each entry ready. With memory
 * to spare (PV_ROOMY), speed comes first: two entries ahead and one behind
 * are read, and entries shown stay decoded for going back, up to PV_SLOTS.
 * Short of memory, only the one shown and the next one ahead are kept. */
#define PV_SLOTS 12
#define PV_TIGHT 3
static struct LJob *pv_slot[PV_SLOTS];
static ULONG pv_clock;              /* cache order */

static BOOL pv_roomy(void)
{
    return AvailMem(MEMF_ANY) >= 3UL * 1024 * 1024 && AvailMem(MEMF_CHIP) >= 768UL * 1024;
}
static LONG pv_dir = 1;             /* the way the list last moved */

/* What the pane shows for entry idx: a job type and its path, 0 if none. */
static int pv_kind(struct Page *p, LONG idx, char *path, LONG max)
{
    struct Item *it;
    if (idx < 0 || idx >= p->list.count)
        return 0;
    it = &p->list.items[idx];
    if (p->pics) {
        amce_strlcpy(path, it->path, max);
        if (it->kind == IT_PIC)
            return LJ_PICTURE;
        if (it->kind == IT_DIR)
            return LJ_PICFOLDER;
        return 0;
    }
    if (it->kind != IT_RUN)
        return 0;
    item_path(it, path, max);
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
    if (pv_wait == j)
        pv_wait = NULL;
    FreeVec(j);
    pv_slot[i] = NULL;
}

/* Every job back and freed: before the screen closes or a game runs. */
static void pv_flush(void)
{
    int i;
    for (i = 0; i < PV_SLOTS; i++)
        pv_free_slot(i);
    pv_wait = NULL;
}

/* The job for (type, path): one already out or back, else a new one. NULL
 * if every slot is busy (a read-ahead is then skipped). */
static struct LJob *pv_request(int type, const char *path, BOOL ahead)
{
    int i, free_i = -1;
    struct LJob *j;
    int limit = pv_roomy() ? PV_SLOTS : PV_TIGHT, used = 0;
    for (i = 0; i < PV_SLOTS; i++) {
        j = pv_slot[i];
        if (j && j->type == type && strcmp(j->path, path) == 0) {
            if (!j->busy && (j->spent || !j->ran)) {
                pv_free_slot(i);        /* its picture went to the screen, or it was
                                         * skipped: read it again */
                break;
            }
            j->cancel = FALSE;
            j->stamp = ++pv_clock;
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
            if (pv_slot[i] && !pv_slot[i]->busy && pv_slot[i] != pv_wait &&
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
         * asked for goes past the limit, and reads ahead not yet started
         * are skipped so it comes sooner (never waited for here) */
        if (ahead)
            return NULL;
        for (i = 0; i < PV_SLOTS; i++)
            if (pv_slot[i] && pv_slot[i] != pv_wait)
                pv_slot[i]->cancel = TRUE;
        for (i = 0; i < PV_SLOTS && free_i < 0; i++)
            if (!pv_slot[i])
                free_i = i;
        if (free_i < 0)
            return NULL;
    }
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    j->type = type;
    amce_strlcpy(j->path, path, sizeof(j->path));
    j->scr = g.scr;
    j->remap = g.truecolor;
    j->want_pic = g.pens_shared || g.truecolor;
    j->stamp = ++pv_clock;
    pv_slot[free_i] = j;
    ldr_submit(j);
    return j;
}

static void pv_release(struct LJob *j)
{
    int i;
    for (i = 0; i < PV_SLOTS; i++)
        if (pv_slot[i] == j)
            pv_free_slot(i);
}

/* Read the entries around this one ahead: the next one the way the list
 * moves; with memory to spare, the one after it and the one behind too. */
static void pv_read_ahead(struct Page *p)
{
    static const LONG near[] = { 1, 2, -1 };
    char path[300];
    int i, n = pv_roomy() ? 3 : 1;
    for (i = 0; i < n; i++) {
        int k = pv_kind(p, p->sel + near[i] * pv_dir, path, sizeof(path));
        if (k && !pv_request(k, path, TRUE))
            break;
    }
}

/* The picture of a job at the top of the pane; the y below it. */
static WORD pv_picture(struct LJob *j, WORD x0, WORD w, WORD max_y, BOOL *drawn)
{
    WORD y = g.list_top, dh = 0;
    BOOL taken = FALSE;
    *drawn = FALSE;
    if (j->obj && gfx_picture_obj(&g, j->obj, j->remapped, x0, y, w, (max_y - y) / 2, TRUE, &dh,
                                  &taken)) {
        /* drawn straight over the old one (which keeps its colours until
         * then); only what it does not cover is cleared afterwards */
        gfx_fill_bg(&g, x0, y, g.pic_x - 1, y + dh - 1);
        gfx_fill_bg(&g, g.pic_x + g.pic_w, y, g.w - 1, y + dh - 1);
        y += dh;
        *drawn = TRUE;
    } else {
        gfx_picture_free(&g);
    }
    if (taken) {
        j->obj = NULL;                      /* the screen has it now (its pens) */
        j->spent = TRUE;
    }
    gfx_fill_bg(&g, x0, y, g.w - 1, max_y);
    return dh ? y + U(6) : y;
}

/* A Pictures row: the picture (or a folder's first picture), then what it
 * is and, for a folder, what Fire and Right do there. */
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

/* Draw the pane from a job that is back, then read the next one ahead. */
static void preview_show(struct Page *p, struct LJob *j)
{
    WORD x0 = aside_x(), w = aside_width() - U(4), max_y = g.footer_y - 2, y;
    BOOL drawn;
    LONG i;
    tb_close(&pv_tb);
    y = pv_picture(j, x0, w, max_y, &drawn);
    if (pv_list.pool)
        list_free(&pv_list);
    if (list_init(&pv_list)) {
        if (p->pics) {
            pv_pics_lines(&p->list.items[p->sel], j, drawn, w - U(7));
        } else {
            /* the whole fact sheet, in a text box with a scroll bar when it
             * is longer than the room left (wheel or bar; Up/Down stay
             * with the list) */
            for (i = 0; i < j->nlines; i++)
                pv_line(j->lines[i], w - U(7));
        }
        if (pv_list.count && max_y - y >= g.font_h) {
            pv_tb.pos = pv_tb.target = 0;
            pv_tb.focused = focus == FOCUS_PREVIEW;
            if (tb_open(&g, &pv_tb, x0, y, w - U(7), max_y - y + 1, &pv_list, TRUE))
                tb_draw(&g, &pv_tb);
        }
    }
    if (!pv_roomy())
        pv_release(j);                      /* short of memory: keep only what is ahead */
    pv_read_ahead(p);
}

/* Preview the selected entry: at once if it was read ahead, else when the
 * loader brings it (preview_arrived); the old one stays up meanwhile. */
static void preview_draw(struct Page *p)
{
    char path[300];
    int k;
    struct LJob *j;
    pv_pending = FALSE;
    pv_page = p;
    pv_sel = p->sel;
    pv_shown = TRUE;
    pv_wait = NULL;
    k = p->list.count ? pv_kind(p, p->sel, path, sizeof(path)) : 0;
    if (!k || !(j = pv_request(k, path, FALSE))) {
        tb_close(&pv_tb);
        gfx_picture_free(&g);
        gfx_fill_bg(&g, aside_x(), g.list_top, g.w - 1, g.footer_y - 2);
        return;
    }
    if (j->done)
        preview_show(p, j);
    else
        pv_wait = j;
}

/* Jobs came back from the loader. */
static void preview_arrived(void)
{
    struct Page *p = cur();
    struct LJob *j;
    while (ldr_collect())
        ;
    if (!(j = pv_wait) || !j->done)
        return;
    pv_wait = NULL;
    if (!j->ran) {                          /* skipped (cancelled): ask again */
        pv_release(j);
        if (p && p == pv_page && p->sel == pv_sel && has_aside(p))
            preview_draw(p);
        return;
    }
    if (p && p == pv_page && p->sel == pv_sel && has_aside(p) && !pane_pending &&
        !hold_active() && !ov.on)
        preview_show(p, j);
    else if (p) {
        pv_shown = FALSE;                   /* moved on: preview again when it rests */
        preview_touch(p);
    }
}

/* Show the preview now, waiting for the loader if need be (Right into it). */
static void preview_now(struct Page *p)
{
    preview_draw(p);
    if (pv_wait) {
        ldr_wait(pv_wait);
        preview_arrived();
    }
}

/* The selection changed (or the list was drawn): preview it once it rests. */
static void preview_touch(struct Page *p)
{
    if (!has_aside(p) || (pv_shown && pv_page == p && pv_sel == p->sel))
        return;                             /* already showing this one */
    pv_pending = TRUE;
    pv_due = time_ms() + PREVIEW_SETTLE;
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
        gfx_picture_free(&g);       /* the preview gives way to the letter */
        tb_close(&pv_tb);
        pv_shown = FALSE;
    }
    /* hand the strip to the letter: clear it once, then stamp */
    gfx_fill_bg(&g, ov.strip_x, g.list_top, g.w - 1, g.footer_y - 2);
    ov_stamp();
}

/* A new letter (A -> D...): clear the old one's box, stamp the new one.
 * Only the strip changes; the rows are not touched. */
static void ov_relabel(char c)
{
    if (!ov.on || !c || c == ov.letter)
        return;
    if (ov.bl)
        gfx_fill(&g, ov.x, ov.y, ov.x + ov.bl->w - 1, ov.y + ov.bl->h - 1, PEN_BG);
    if (ov_build(c))
        ov_stamp();
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
    UBYTE bg = !sel ? PEN_BG : focus == FOCUS_CONTENT ? PEN_HL : PEN_SELDIM;
    /* Every list pen is below 8 and planes 3+ are clear in the list (the
     * pane is cleared on all planes when the page is drawn), so on native
     * screens only planes 0-2 are written: 3/8 of the blitter work. */
    if (g.planar)
        SetWriteMask(g.rp, 0x07);
    gfx_fill(&g, g.pane_x - U(2), y, xr, y + g.list_row_h - 1, bg);
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

static void draw_list_rows(struct Page *p)
{
    WORD vis = list_visible(), r, col = mark_column(&p->list);
    list_follow(p);
    if (!p->list.count) {
        gfx_fill(&g, g.pane_x - U(2), g.list_top, g.w - 1, g.footer_y - 2, PEN_BG);
        gfx_text(&g, g.pane_x, g.list_top + U(2), "(empty)", PEN_DIM, PEN_BG, 0, TS_BODY);
        return;
    }
    for (r = 0; r < vis && p->top + r < p->list.count; r++)
        draw_list_row(p, p->top + r, col);
    gfx_fill(&g, g.pane_x - U(2), g.list_top + r * g.list_row_h, list_xr(p), g.footer_y - 2, PEN_BG);
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
    LockLayerRom(g.win->WLayer);
    BltBitMap(bm, x0, g.list_top + src * g.list_row_h, bm, x0, g.list_top + dst * g.list_row_h,
              x1 - x0 + 1, n * g.list_row_h, 0xC0, g.planar ? 0x07 : 0xFF, NULL);
    UnlockLayerRom(g.win->WLayer);
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
static void draw_list_move(struct Page *p, LONG old_sel, LONG old_top)
{
    WORD vis = list_visible(), col;
    LONG k, i, first, last, r_old, n_old;
    list_follow(p);
    k = p->top - old_top;
    col = mark_column(&p->list);
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
        } else {
            gfx_fill_bg(&g, px, U(25), g.w - 1, pb);
        }
    }
    draw_title(p);
    switch (p->type) {
    case PT_LIST:
        draw_list_rows(p);
        break;
    case PT_TEXT:
        if (!p->tb.open)
            open_textbox(p, g.list_top);
        if (full || !p->tb.drawn)
            tb_draw(&g, &p->tb);    /* a focus change alone leaves the box alone */
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
    build_root(menu[menu_sel]);
    draw_pane(TRUE);
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

static void open_dir(struct Item *it)
{
    struct Page *p;
    LONG n;
    BOOL pics = cur() && cur()->pics;
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
        n = pics_read_dir(it->path, &p->list);
        if (n <= 0) {
            page_pop();
            set_msg(n < 0 ? "Could not open that folder" : "That folder has no pictures");
            return;
        }
        msg[0] = 0;
        draw_pane(TRUE);
        draw_footer();
        return;
    }
    n = page_read_ags(p, it, 2 * list_visible());   /* the rest follows (load_tick) */
    if (n <= 0) {
        page_pop();
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
    save_nav();
    pv_flush();                     /* the loader's jobs back, then the loader gone */
    ldr_stop();
    pages_clear();                  /* lists, text boxes, the screenshot */
    tb_close(&pv_tb);
    if (pv_list.pool)
        list_free(&pv_list);
    pv_shown = pv_pending = FALSE;
    ov.on = FALSE;
    ov.bl = NULL;
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
    build_root(menu[menu_sel]);
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
    ldr_submit(vw_job);
}

/* The picture read ahead for path, if it is: its object (now the caller's). */
static APTR vw_take(const char *path, BOOL *remapped)
{
    APTR o = NULL;
    if (vw_job && strcmp(vw_job->path, path) == 0) {
        if (vw_job->busy)
            ldr_wait(vw_job);               /* nearly there: sooner than starting over */
        if (vw_job->ran) {
            o = vw_job->obj;
            *remapped = vw_job->remapped;
            vw_job->obj = NULL;
        }
        vw_drop();
    }
    return o;
}

/* Show the pictures of l from entry idx; returns the last one shown. */
static LONG viewer_run(const struct ItemList *l, LONG idx, BOOL slideshow)
{
    struct PicInfo pi;
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
    if (!view_begin(&g)) {
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
            APTR pre = vw_take(l->items[want].path, &remapped);
            ok = view_show(&g, l->items[want].path, l->items[want].label, &pi, scale, sizeof(scale),
                           pre, remapped);
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
    msg[0] = 0;
    draw_all();
    view_restore(&g);
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
    n = pics_read_dir(it->path, &l);
    first = n > 0 ? view_step(&l, l.count - 1, 1) : -1;
    if (first < 0) {
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
    if (p->type == PT_LIST) {
        LONG s;
        if (d)
            pv_dir = d > 0 ? 1 : -1;
        if (p->ld && d > 0)
            page_load_until(p, p->sel + d + 2 * list_visible());
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
    act_move((focus == FOCUS_CONTENT && p && p->type != PT_LIST) ? d * 3 : d);
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
 * when the look changes (smooth needs a 16-bit or deeper RTG screen). */
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
            set_msg("Smooth: on RTG screens of 16 bits or more");
        return;
    }
    pages_clear();
    pv_flush();                     /* decoded for this screen */
    gfx_close(&g);
    if (!gfx_open(&g, &caps, screen_mode, ui_style) &&
        !gfx_open(&g, &caps, screen_mode, STYLE_PIXEL) &&
        !gfx_open(&g, &caps, SM_AUTO, STYLE_PIXEL)) {
        running = FALSE;
        return;
    }
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
}

static void act_action(struct Item *it)
{
    if (it->flags & ITEM_DISABLED) {
        if (it->action == ACT_STYLES)
            set_msg("Style: Smooth needs an RTG screen of 16 bits or more");
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
 * still held, on up to LIST_BOOST times that over HOLD_BOOST, for very
 * long lists (the big letter shows where the list is meanwhile). */
#define HOLD_DELAY  250     /* ms before the glide starts */
#define HOLD_RAMP  1200     /* ms to reach cruising speed */
#define HOLD_BOOST 3000     /* further ms to reach top speed */
#define LIST_MIN     12     /* entries per second when the glide starts */
#define LIST_MAX    100     /* cruising entries per second on a small screen */
#define LIST_SCREENS  6     /* ...or this many screenfuls a second, if more */
#define LIST_BOOST    4     /* top speed, as a multiple of cruising speed */
#define FRAME_MS     20     /* assumed frame time if there is no timer */

static struct {
    LONG  dir;              /* -1 up, +1 down, 0 not held */
    ULONG held_ms, last_ms; /* time held so far, clock at the last tick */
    LONG  acc;              /* entries (x1000) or pixels (x1000) owed */
    BOOL  joy;              /* held on the joystick rather than the keyboard */
    BOOL  letters;          /* show the letter overlay once the glide starts */
} hold;

static void hold_start(LONG dir, BOOL joy)
{
    hold.dir = dir;
    hold.held_ms = 0;
    hold.last_ms = time_ms();
    hold.acc = 0;
    hold.joy = joy;
    hold.letters = cur() && ov_eligible(cur());
}

static void hold_stop(void)
{
    hold.dir = 0;
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
        if (dt > 100)
            dt = 100;
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
        if (hold.dir > 0 && p->ld)
            page_load_until(p, p->sel + 4 * list_visible() + 32);
        remaining = hold.dir > 0 ? p->list.count - 1 - p->sel : p->sel;
        if (remaining <= 0) {
            hold_stop();
            return;
        }
        {
            /* Cruising: two entries a frame, or six screenfuls a second on
             * screens that show more rows; above one per frame the list
             * moves several rows in a single frame, still drawn every frame. */
            LONG top = LIST_MAX, screens = LIST_SCREENS * list_visible(), n;
            if (screens > top)
                top = screens;
            if (glide < HOLD_RAMP)
                rate = LIST_MIN + (top - LIST_MIN) * ramp / HOLD_RAMP;
            else {
                LONG b = glide - HOLD_RAMP;
                if (b > HOLD_BOOST)
                    b = HOLD_BOOST;
                rate = top + top * (LIST_BOOST - 1) * b / HOLD_BOOST;
            }
            if (rate > 6 + remaining * 4)   /* ease in: 406/s at 100 left ... 10/s at 1 */
                rate = 6 + remaining * 4;
            hold.acc += rate * elapsed;     /* entries x 1000 */
            n = hold.acc / 1000;
            if (n > 0) {
                hold.acc -= n * 1000;
                act_move(hold.dir * (n < remaining ? n : remaining));
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
        struct Item *it = &p->list.items[p->sel];
        if (p->pics && it->kind == IT_DIR && it->mark == MARK_PLAY)
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
        if (pv_pending || !pv_shown || pv_wait) {   /* not shown yet: show it now */
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

static void act_letter(char c)
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
    for (i = p->sel + 1; p->ld || i < p->list.count; i++) {
        char first[2];
        if (i >= p->list.count) {
            page_load_some(p, 64);
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

static void disarm(void)
{
    if (quit_armed || msg[0]) {
        quit_armed = FALSE;
        set_msg(NULL);
    }
}

/* ------------------------------------------------------------ input */
static ULONG last_secs, last_mics;
static LONG last_click = -1;

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
    if (im->Class == IDCMP_MOUSEMOVE) {
        if (tb)
            tb_mouse_move(tb, y);
        return;
    }
    if (im->Code == SELECTUP) {
        if (tb && tb->dragging) {
            tb_mouse_up(tb);
            ReportMouse(FALSE, g.win);
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
    if (tb && tb_mouse_down(tb, x, y)) {
        set_focus(tb == &pv_tb ? FOCUS_PREVIEW : FOCUS_CONTENT);
        if (tb->dragging) {
            ReportMouse(TRUE, g.win);
            tb_refresh_bar(&g, tb);
        }
        return;
    }
    if (x < g.menu_w && y >= g.top && y < g.top + nmenu * g.row_h) {
        idx = (y - g.top) / g.row_h;
        dbl = idx + 1000 == last_click && DoubleClick(last_secs, last_mics, im->Seconds, im->Micros);
        last_click = idx + 1000;
        if (focus != FOCUS_MENU)
            set_focus(FOCUS_MENU);
        menu_select_now(idx, TRUE);
        if (dbl)
            act_open(FALSE);
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
            if (dbl) {
                struct Item *it = &p->list.items[idx];
                if (it->kind == IT_DIR)
                    open_dir(it);           /* a double-click goes inside a folder */
                else
                    act_open(FALSE);
            }
        } else {
            set_focus(FOCUS_CONTENT);
        }
    }
    last_secs = im->Seconds;
    last_mics = im->Micros;
}

static void handle_key(struct IntuiMessage *im)
{
    UWORD code = im->Code;
    BOOL shift = (im->Qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
    LONG page = list_visible() - 1;

    if (im->Class == IDCMP_RAWKEY) {
        if (code & IECODE_UP_PREFIX) {
            UWORD k = code & ~IECODE_UP_PREFIX;
            if ((k == 0x4C || k == 0x4D) && hold.dir && !hold.joy)
                hold_stop();
            return;
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
        case 0x4E: disarm(); act_right(); break;                      /* cursor right */
        case 0x4F: disarm(); if (focus != FOCUS_MENU) act_back(); break; /* cursor left */
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
        case '4': disarm(); if (focus != FOCUS_MENU) act_back(); return;
        case '6': disarm(); act_right(); return;
        case '5': disarm(); act_open(FALSE); return;
        case '9': disarm(); act_move(-page); return;
        case '3': disarm(); act_move(page); return;
        case 13: disarm(); act_open(TRUE); return;
        }
        return;
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
    const ULONG btnmask = JPF_BUTTON_RED | JPF_BUTTON_BLUE;
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
    else if (act & JPF_JOY_RIGHT) { disarm(); act_right(); }
    else if (act & JPF_JOY_LEFT) { disarm(); if (focus != FOCUS_MENU) act_back(); }
    if (newbtn & JPF_BUTTON_RED) { disarm(); act_open(FALSE); }
    if (newbtn & JPF_BUTTON_BLUE) { disarm(); act_back(); }
}

/* Read more of a long list while nothing else is happening: ~20 ms at a
 * time between looks at the input (a few ms a frame while a list glides),
 * so the list stays usable while it grows. */
static void load_tick(void)
{
    struct Page *p = cur();
    LONG before, vis;
    ULONG t0;
    if (!p || !p->ld || pane_pending)
        return;
    before = p->list.count;
    t0 = time_ms();
    while (page_load_some(p, 16) && (LONG)(time_ms() - t0) < (hold_active() ? 4 : 20))
        ;
    vis = list_visible();
    if (p->list.count == before && p->ld)
        return;
    if (!p->ld && p->title_wide)
        draw_title(p);              /* done: also the full count */
    else
        draw_title_count(p);
    if (before < p->top + vis || !p->ld)
        draw_list_rows(p);          /* rows that were empty, or a final re-sort */
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

int main(void)
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
    amce_log_open(debug_on);
    open_extra_libs();
    if (!ldr_start())
        amce_log("loader", "could not start: reading in the UI");

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
                    tb_animating(&pv_tb) || (cur() && cur()->ld);
        ULONG sig;
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
                    if (ds.ds_Minute != last_minute)
                        draw_clock();
                    if (debug_on) {
                        cpu_load = dbg_cpu_sample();
                        app_load = dbg_app_sample();
                        draw_status();
                    }
                }
                /* 25 a second for a joystick or a fading letter; otherwise
                 * 5 a second, which still notices a joystick plugged in. */
                tick_us = (joy_present || ov.on) ? 40000 : 200000;
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
        if (g.win && cur() && cur()->ld) {
            load_tick();
            /* the loader runs below us: let it have the rest of the frame */
            if (ldr_out() && !(hold_active() || pane_pending || pv_pending || tb_animating(&pv_tb) ||
                               ((atb = cur_tb()) && tb_animating(atb)))) {
                dbg_sleep_begin();
                WaitTOF();
                dbg_sleep_end();
            }
        }
        if (g.win && (hold_active() || pane_pending || pv_pending || tb_animating(&pv_tb) ||
                      ((atb = cur_tb()) && tb_animating(atb)))) {
            dbg_sleep_begin();
            WaitTOF();
            dbg_sleep_end();
            pane_tick();
            preview_tick();
            if (tb_animating(&pv_tb))
                tb_step(&g, &pv_tb);
            hold_tick();
            if ((atb = cur_tb()) && tb_animating(atb))
                tb_step(&g, atb);
        }
    }

out:
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
    ldr_stop();
    pages_clear();
    amce_log("pages freed", "");
    gfx_close(&g);
    amce_log("screen closed", "");
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
