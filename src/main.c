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
    "Games", "Mods", "Demos", "Videos", "Pictures", "Music", "Internet Radio", "Settings", "About"
};

enum { PT_LIST, PT_TEXT, PT_DETAIL };
enum { FOCUS_MENU, FOCUS_CONTENT };
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
};

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
static BOOL debug_on;               /* Settings > Debug: header CPU/memory readout and log */

/* IT_ACTION codes (struct Item.action) */
enum { ACT_SCREENMODES = 1, ACT_SYSINFO, ACT_CREDITS, ACT_DEBUG, ACT_MODE0 = 16 /* + SM_* */ };

/* ------------------------------------------------------------ pages */
static struct Page *cur(void) { return depth ? &pages[depth - 1] : NULL; }

static struct Page *page_push(UBYTE type, const char *title)
{
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
    if (!depth)
        return;
    gfx_picture_free(&g);
    tb_close(&pages[depth - 1].tb);
    list_free(&pages[--depth].list);
}

static void pages_clear(void)
{
    while (depth)
        page_pop();
}

static void text_line(struct Page *p, const char *s)
{
    list_add(&p->list, s, "", "", IT_TEXT);
}

/* Text box width: the pane less room for the scroll bar (gap + bar + gap). */
static WORD textbox_w(void)
{
    return g.pane_w - 7 * g.sx;
}

/* Word-wrap a paragraph to the text box width. */
static void text_para(struct Page *p, const char *s)
{
    char line[160], word[96];
    WORD maxw = textbox_w();
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
        if (line[0] && gfx_textlen(&g, tryline) > maxw) {
            text_line(p, line);
            amce_strlcpy(line, word, sizeof(line));
        } else {
            amce_strlcpy(line, tryline, sizeof(line));
        }
    }
    if (line[0])
        text_line(p, line);
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
            amce_strlcpy(out, "All A-Z", max);
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
            if (classify(it->label, label, sizeof(label), &prio) == mod && prio == want)
                list_add(&p->list, label, it->path, it->sortkey, IT_DIR);
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
        sprintf(line, "Artwork: %s", g.layout == LAYOUT_HIRES ? "hires (tall pixels)"
                                                              : "lowres, 1:1 (square pixels)");
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
    action_item(p, "Screen mode", tag, ACT_SCREENMODES, MARK_ARROW);
    action_item(p, "Debug (CPU, memory, log)", "", ACT_DEBUG,
                debug_on ? MARK_CHECK_ON : MARK_CHECK_OFF);
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
    text_line(p, "Created by Brian King");
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

static void build_root(int mod)
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
        if ((p = page_push(PT_TEXT, "Pictures"))) {
            text_para(p, "The picture viewer arrives in a later version.");
            text_line(p, "");
            found_line(p, "Pictures", "Media:Images");
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
        if ((p = page_push(PT_TEXT, "Internet Radio")))
            text_para(p, "Internet Radio arrives in a later version.");
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
    return g.w - 8 * g.sx - gfx_textlen(&g, "Wed 30 Sep  00:00") - 4 * g.sx;
}

static void draw_clock(void)
{
    char buf[40];
    WORD tw;
    clock_text(buf);
    gfx_fill(&g, clock_left(), 0, g.w - 1, 23, PEN_BAR);
    if (!buf[0])
        return;
    tw = gfx_textlen(&g, buf);
    gfx_text(&g, g.w - 8 * g.sx - tw, (24 - g.font_h) / 2, buf, PEN_TEXT, 0);
}

/* Debug readout between the logo and the clock: the longest form that fits. */
static LONG cpu_load = -1;

static void draw_status(void)
{
    WORD x0 = 6 * g.sx + (g.layout == LAYOUT_HIRES ? logo_hi_w : logo_lo_w) + 10 * g.sx;
    WORD x1 = clock_left() - 6 * g.sx;
    char cpu[16], forms[3][64];
    ULONG chip, used;
    int i;
    if (x1 <= x0)
        return;
    gfx_fill(&g, x0, 0, x1, 23, PEN_BAR);
    if (!debug_on)
        return;
    used = dbg_mem_used(&chip);
    if (cpu_load < 0)
        amce_strlcpy(cpu, "--", sizeof(cpu));
    else
        sprintf(cpu, "%ld%%", (long)cpu_load);
    sprintf(forms[0], "CPU %s   RAM %luK  (chip %luK)", cpu,
            (unsigned long)(used >> 10), (unsigned long)(chip >> 10));
    sprintf(forms[1], "CPU %s  RAM %luK", cpu, (unsigned long)(used >> 10));
    sprintf(forms[2], "%s %luK", cpu, (unsigned long)(used >> 10));
    for (i = 0; i < 3; i++)
        if (gfx_textlen(&g, forms[i]) <= x1 - x0 || i == 2) {
            gfx_text(&g, x0, (24 - g.font_h) / 2, forms[i], PEN_ACCENT, x1 - x0);
            break;
        }
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
    cpu_load = -1;
}

static void draw_header(void)
{
    gfx_fill(&g, 0, 0, g.w - 1, 23, PEN_BAR);
    gfx_fill(&g, 0, 24, g.w - 1, 24, PEN_RULE);
    gfx_logo(&g, 6 * g.sx, 4);
    draw_status();
    draw_clock();
}

static void draw_menu_row(int i)
{
    WORD y = g.top + i * g.row_h;
    BOOL sel = i == menu_sel;
    UBYTE bg = sel ? (focus == FOCUS_MENU ? PEN_HL : PEN_SELDIM) : PEN_BG;
    UBYTE fg = sel && focus == FOCUS_MENU ? PEN_WHITE : PEN_TEXT;
    gfx_fill(&g, 0, y, g.menu_w - 1, y + g.row_h - 1, bg);
    gfx_icon(&g, menu[i], 3 * g.sx, y + 1, fg);
    gfx_text(&g, 23 * g.sx, y + (g.row_h - g.font_h) / 2, mod_names[menu[i]], fg,
             g.menu_w - 24 * g.sx);
}

static void draw_menu(void)
{
    int i;
    gfx_fill(&g, 0, 25, g.menu_w + 2 * g.sx, g.footer_y - 2, PEN_BG);
    for (i = 0; i < nmenu; i++)
        draw_menu_row(i);
    gfx_fill(&g, g.menu_w + 2 * g.sx, 25, g.menu_w + 2 * g.sx, g.footer_y - 2, PEN_RULE);
}

static const char *hint_text(void)
{
    struct Page *p = cur();
    BOOL hi = g.w >= 640;      /* room for the long hints */
    if (focus == FOCUS_MENU)
        return hi ? "Up/Down  choose      Right/Fire  open      Esc twice  quit"
                  : "Fire open  Esc twice quit";
    if (p && p->type == PT_LIST)
        return hi ? "Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back"
                  : "Fire open  Ret play  Esc back";
    if (p && p->type == PT_DETAIL)
        return hi ? "Fire/Return  play      Up/Down  scroll      Left/Esc  back"
                  : "Fire play  Esc back";
    return hi ? "Up/Down  scroll      Left/Esc  back" : "Up/Down scroll  Esc back";
}

static void draw_footer(void)
{
    gfx_fill(&g, 0, g.footer_y - 1, g.w - 1, g.footer_y - 1, PEN_RULE);
    gfx_fill(&g, 0, g.footer_y, g.w - 1, g.h - 1, PEN_BAR);
    gfx_text(&g, 6 * g.sx, g.footer_y + (12 - g.font_h) / 2, msg[0] ? msg : hint_text(),
             msg[0] ? PEN_ACCENT : PEN_DIM, g.w - 12 * g.sx);
}

static void set_msg(const char *m)
{
    amce_strlcpy(msg, m ? m : "", sizeof(msg));
    draw_footer();
}

static void draw_title(struct Page *p)
{
    char t[160], count[24];
    int i;
    WORD cw = 0;
    t[0] = 0;
    for (i = 0; i < depth; i++) {
        if (i)
            amce_strlcat(t, " > ", sizeof(t));
        amce_strlcat(t, pages[i].title, sizeof(t));
    }
    count[0] = 0;
    if (p->type == PT_LIST && p->list.count) {
        sprintf(count, "%ld/%ld", (long)p->sel + 1, (long)p->list.count);
        cw = gfx_textlen(&g, count);
    }
    gfx_fill(&g, g.pane_x - 2 * g.sx, 26, g.w - 1, 41, PEN_BG);
    /* show the end of a long breadcrumb */
    {
        const char *s = t;
        WORD maxw = g.pane_w - cw - 8 * g.sx;
        while (*s && gfx_textlen(&g, s) > maxw)
            s++;
        gfx_text(&g, g.pane_x, 28, s, PEN_WHITE, maxw);
    }
    if (cw)
        gfx_text(&g, g.pane_x + g.pane_w - cw, 28, count, PEN_DIM, 0);
    gfx_fill(&g, g.pane_x, 41, g.pane_x + g.pane_w - 1, 41, PEN_RULE);
}

/* Row markers, drawn in a column on the left before each label. */
static const char *mark_text[] = { "", ">", "[ ]", "[x]", "( )", "(*)" };

/* Width of the marker column for this list: wide enough for its widest
 * marker, so every label in the list starts at the same x; 0 if none. */
static WORD mark_column(const struct ItemList *l)
{
    UBYTE seen[sizeof(mark_text) / sizeof(mark_text[0])];
    WORD w = 0, mw;
    LONG i;
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < l->count; i++)      /* cheap scan; text widths measured once each */
        seen[l->items[i].mark] = 1;
    for (i = 1; i < (LONG)sizeof(seen); i++)
        if (seen[i] && (mw = gfx_textlen(&g, mark_text[i])) > w)
            w = mw;
    return w ? w + 5 * g.sx : 0;
}

static void draw_list_rows(struct Page *p)
{
    WORD vis = list_visible(), r, col = mark_column(&p->list);
    if (p->sel < p->top)
        p->top = p->sel;
    if (p->sel >= p->top + vis)
        p->top = p->sel - vis + 1;
    gfx_fill(&g, g.pane_x - 2 * g.sx, g.list_top, g.w - 1, g.footer_y - 2, PEN_BG);
    if (!p->list.count) {
        gfx_text(&g, g.pane_x, g.list_top + 2, "(empty)", PEN_DIM, 0);
        return;
    }
    for (r = 0; r < vis && p->top + r < p->list.count; r++) {
        LONG idx = p->top + r;
        struct Item *it = &p->list.items[idx];
        WORD y = g.list_top + r * g.list_row_h;
        BOOL sel = idx == p->sel;
        UBYTE fg = PEN_TEXT;
        if (sel) {
            gfx_fill(&g, g.pane_x - 2 * g.sx, y, g.pane_x + g.pane_w - 1, y + g.list_row_h - 1,
                     focus == FOCUS_CONTENT ? PEN_HL : PEN_SELDIM);
            fg = PEN_WHITE;
        }
        {
            WORD ty = y + (g.list_row_h - g.font_h) / 2, right = g.pane_x + g.pane_w - 2 * g.sx;
            WORD lx = g.pane_x + col;
            if (it->mark != MARK_NONE)
                gfx_text(&g, g.pane_x, ty, mark_text[it->mark],
                         sel ? PEN_WHITE : it->mark == MARK_ARROW ? PEN_DIM : PEN_TEXT, 0);
            if (it->kind == IT_ACTION && it->path[0]) {
                WORD tagw = gfx_textlen(&g, it->path);
                gfx_text(&g, right - tagw, ty, it->path, sel ? PEN_WHITE : PEN_DIM, 0);
                right -= tagw + 8 * g.sx;
            }
            gfx_text(&g, lx, ty, it->label, fg, right - lx);
        }
    }
}

/* (Re)build the page's text box below y0, keeping its scroll position. */
static void open_textbox(struct Page *p, WORD y0)
{
    tb_open(&g, &p->tb, g.pane_x, y0, textbox_w(), g.footer_y - 2 - y0, &p->list,
            p->type == PT_DETAIL);
}

static struct TextBox *cur_tb(void)
{
    struct Page *p = cur();
    return (p && p->type != PT_LIST && p->tb.open) ? &p->tb : NULL;
}

static void draw_pane(BOOL full)
{
    struct Page *p = cur();
    if (!p)
        return;
    if (full) {
        WORD px = g.menu_w + 3 * g.sx, pb = g.footer_y - 2;
        if (p->type == PT_TEXT) {
            /* Clear around the text box, not under it: tb_draw copies the
             * new text straight over the old, so there is no blank frame. */
            struct TextBox *tb = &p->tb;
            WORD bx1;
            open_textbox(p, g.list_top);
            bx1 = tb->bar_x + 3 * g.sx - 1;
            gfx_fill_bg(&g, px, 25, g.w - 1, tb->y - 1);
            gfx_fill_bg(&g, px, tb->y, tb->x - 1, tb->y + tb->h - 1);
            gfx_fill_bg(&g, bx1 + 1, tb->y, g.w - 1, tb->y + tb->h - 1);
            gfx_fill_bg(&g, px, tb->y + tb->h, g.w - 1, pb);
        } else {
            gfx_fill_bg(&g, px, 25, g.w - 1, pb);
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
            WORD ph = g.h >= 400 ? 170 : 85;
            p->facts_y = g.list_top;
            if ((g.pens_shared || g.truecolor) && ags_screenshot_path(p->base, shot, sizeof(shot))) {
                set_msg("Loading screenshot...");
                if (gfx_picture(&g, shot, g.pane_x, g.list_top, g.pane_w, ph))
                    p->facts_y = g.list_top + ph + 6;
                set_msg(NULL);
            }
            open_textbox(p, p->facts_y);
        }
        if (full || !p->tb.drawn)
            tb_draw(&g, &p->tb);
        break;
    }
}

static void draw_all(void)
{
    gfx_fill(&g, 0, 0, g.w - 1, g.h - 1, PEN_BG);
    draw_header();
    draw_menu();
    draw_pane(TRUE);
    draw_footer();
}

/* ------------------------------------------------------------ actions */
static void menu_select(int i)
{
    int old = menu_sel;
    if (i < 0 || i >= nmenu || i == old)
        return;
    menu_sel = i;
    draw_menu_row(old);
    draw_menu_row(i);
    build_root(menu[i]);
    draw_pane(TRUE);
}

static void set_focus(int f)
{
    if (focus == f)
        return;
    focus = f;
    draw_menu_row(menu_sel);
    draw_pane(FALSE);
    draw_footer();
}

static void open_dir(struct Item *it)
{
    struct Page *p;
    LONG n;
    set_msg("Loading...");
    p = page_push(PT_LIST, it->label);
    if (!p) {
        set_msg("Too many levels open");
        return;
    }
    n = ags_read_dir(it->path, &p->list, ags.adult[0] == '1');
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
    amce_strlcpy(p->base, it->path, sizeof(p->base));
    n = ags_read_txt(it->path, fact_buf, FACT_LINES);
    if (!n)
        text_line(p, it->label);
    for (i = 0; i < n; i++)
        text_line(p, fact_buf[i]);
    draw_pane(TRUE);
    draw_footer();
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
    /* Give the game every byte of chip RAM we can. */
    amce_log("launch", base);
    if (debug_on)
        dbg_cpu_stop();
    gfx_close(&g);
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
    if (!gfx_open(&g, &caps, screen_mode)) {
        running = FALSE;
        return;
    }
    draw_all();
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
    if (p->type == PT_LIST) {
        LONG s = p->sel + d;
        if (s < 0)
            s = 0;
        if (s >= p->list.count)
            s = p->list.count - 1;
        if (s < 0 || s == p->sel)
            return;
        p->sel = s;
        draw_title(p);
        draw_list_rows(p);
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
    BOOL over_pane = x >= g.pane_x - 2 * g.sx && y >= g.top && y < g.footer_y - 1;
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
    gfx_close(&g);
    screen_mode = sm;
    if (!gfx_open(&g, &caps, sm)) {
        failed = TRUE;
        screen_mode = old;
        if (!gfx_open(&g, &caps, old)) {
            running = FALSE;
            return;
        }
    } else {
        save_screen_mode();
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

static void act_action(struct Item *it)
{
    if (it->action == ACT_SCREENMODES)
        open_screenmodes();
    else if (it->action == ACT_SYSINFO)
        open_sysinfo();
    else if (it->action == ACT_CREDITS)
        open_credits();
    else if (it->action == ACT_DEBUG)
        toggle_debug();
    else if (it->action >= ACT_MODE0 && it->action < ACT_MODE0 + SM_COUNT)
        apply_screen_mode(it->action - ACT_MODE0);
}

static void act_open(BOOL play)
{
    struct Page *p = cur();
    if (focus == FOCUS_MENU) {
        if (p && (p->type != PT_LIST || p->list.count))
            set_focus(FOCUS_CONTENT);
        return;
    }
    if (!p)
        return;
    if (p->type == PT_LIST && p->list.count) {
        struct Item *it = &p->list.items[p->sel];
        if (it->kind == IT_DIR)
            open_dir(it);
        else if (it->kind == IT_ACTION)
            act_action(it);
        else if (it->kind == IT_RUN) {
            if (play)
                launch(it->path);
            else
                open_detail(it);
        }
    } else if (p->type == PT_DETAIL) {
        launch(p->base);
    }
}

static void act_back(void)
{
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
    struct Page *p = cur();
    LONG i, n;
    char k[2];
    if (focus != FOCUS_CONTENT || !p || p->type != PT_LIST || !p->list.count)
        return;
    k[0] = c;
    k[1] = 0;
    n = p->list.count;
    for (i = 1; i <= n; i++) {
        LONG idx = (p->sel + i) % n;
        char first[2];
        first[0] = p->list.items[idx].label[0];
        first[1] = 0;
        if (amce_stricmp(first, k) == 0) {
            p->sel = idx;
            draw_title(p);
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
    struct TextBox *tb = cur_tb();
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
        if (focus != FOCUS_CONTENT)
            set_focus(FOCUS_CONTENT);
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
        menu_select(idx);
        if (dbl)
            act_open(FALSE);
    } else if (x >= g.pane_x - 2 * g.sx && y >= g.list_top && y < g.footer_y - 1) {
        struct Page *p = cur();
        if (!p)
            return;
        if (p->type == PT_LIST) {
            idx = p->top + (y - g.list_top) / g.list_row_h;
            if (idx >= p->list.count)
                return;
            dbl = idx == last_click && DoubleClick(last_secs, last_mics, im->Seconds, im->Micros);
            last_click = idx;
            if (focus != FOCUS_CONTENT)
                set_focus(FOCUS_CONTENT);
            if (p->sel != idx) {
                p->sel = idx;
                draw_title(p);
                draw_list_rows(p);
            }
            if (dbl)
                act_open(FALSE);
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
        if (code & IECODE_UP_PREFIX)
            return;
        switch (code) {
        case 0x4C: disarm(); act_move(shift ? -page : -1); break;     /* cursor up */
        case 0x4D: disarm(); act_move(shift ? page : 1); break;       /* cursor down */
        case 0x4E: disarm(); act_open(FALSE); break;                  /* cursor right */
        case 0x4F: disarm(); if (focus == FOCUS_CONTENT) act_back(); break; /* cursor left */
        case 0x7A: disarm(); act_wheel(-1, im->MouseX, im->MouseY); break; /* NM_WHEEL_UP */
        case 0x7B: disarm(); act_wheel(1, im->MouseX, im->MouseY); break;  /* NM_WHEEL_DOWN */
        }
        return;
    }
    /* IDCMP_VANILLAKEY */
    if (im->Qualifier & IEQUALIFIER_NUMERICPAD) {
        switch (code) {
        case '8': disarm(); act_move(-1); return;
        case '2': disarm(); act_move(1); return;
        case '4': disarm(); if (focus == FOCUS_CONTENT) act_back(); return;
        case '6': case '5': disarm(); act_open(FALSE); return;
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

static void joy_poll(void)
{
    const ULONG dirmask = JPF_JOY_UP | JPF_JOY_DOWN | JPF_JOY_LEFT | JPF_JOY_RIGHT;
    const ULONG btnmask = JPF_BUTTON_RED | JPF_BUTTON_BLUE;
    ULONG st, dir, btn, newbtn, act = 0;
    if (!LowLevelBase)
        return;
    st = ReadJoyPort(1);
    if ((st & JP_TYPE_MASK) == JP_TYPE_NOTAVAIL || (st & JP_TYPE_MASK) == JP_TYPE_MOUSE)
        return;
    dir = st & dirmask;
    btn = st & btnmask;
    newbtn = btn & ~(joy_last & btnmask);
    if (dir) {
        if (dir != (joy_last & dirmask)) {
            joy_hold = 0;
            act = dir;
        } else if (++joy_hold >= 8 && (joy_hold - 8) % 3 == 0) {
            act = dir;          /* repeat after ~320 ms, then every ~120 ms */
        }
    } else {
        joy_hold = 0;
    }
    joy_last = dir | btn;
    if (act & JPF_JOY_UP) { disarm(); act_move(-1); }
    else if (act & JPF_JOY_DOWN) { disarm(); act_move(1); }
    else if (act & JPF_JOY_RIGHT) { disarm(); act_open(FALSE); }
    else if (act & JPF_JOY_LEFT) { disarm(); if (focus == FOCUS_CONTENT) act_back(); }
    if (newbtn & JPF_BUTTON_RED) { disarm(); act_open(FALSE); }
    if (newbtn & JPF_BUTTON_BLUE) { disarm(); act_back(); }
}

/* ------------------------------------------------------------ main */
static void build_menu(void)
{
    /* Main menu order, top to bottom. */
    static const int order[] = { MOD_GAMES, MOD_MODS, MOD_DEMOS, MOD_MUSIC, MOD_RADIO,
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
    ULONG ticks = 0;
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
    DiskfontBase = OpenLibrary((CONST_STRPTR)"diskfont.library", 36);
    DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
    LowLevelBase = OpenLibrary((CONST_STRPTR)"lowlevel.library", 40);

    caps_detect(&caps);
    ags_init(&ags);
    build_menu();
    load_screen_mode();
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

    if (!gfx_open(&g, &caps, screen_mode) &&
        (screen_mode == SM_AUTO || !gfx_open(&g, &caps, SM_AUTO))) {
        say("AmigaMCE could not open its screen.\n");
        rc = 20;
        goto out;
    }

    tport = CreateMsgPort();
    treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(*treq)) : NULL;
    if (treq && !OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0)) {
        timer_open = TRUE;
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
    }

    while (running) {
        ULONG wsig = g.win ? 1UL << g.win->UserPort->mp_SigBit : 0;
        ULONG tsig = timer_open ? 1UL << tport->mp_SigBit : 0;
        ULONG mask = wsig | tsig | SIGBREAKF_CTRL_C;
        struct TextBox *atb = cur_tb();
        /* While a text box glides, poll instead of sleeping; WaitTOF below
         * paces the loop to one step per frame. */
        ULONG sig = (atb && tb_animating(atb)) ? SetSignal(0, mask) & mask : Wait(mask);
        if (sig & SIGBREAKF_CTRL_C)
            break;
        if (sig & tsig) {
            if (GetMsg(tport)) {
                ticks++;
                joy_poll();
                if (ticks % 25 == 0 && g.win) {
                    struct DateStamp ds;
                    DateStamp(&ds);
                    if (ds.ds_Minute != last_minute)
                        draw_clock();
                    if (debug_on) {
                        cpu_load = dbg_cpu_sample();
                        draw_status();
                    }
                }
                if (running) {
                    treq->tr_node.io_Command = TR_ADDREQUEST;
                    treq->tr_time.tv_secs = 0;
                    treq->tr_time.tv_micro = 40000;
                    SendIO((struct IORequest *)treq);
                }
            }
        }
        if (g.win && (sig & wsig)) {
            struct IntuiMessage *im;
            while (g.win && (im = (struct IntuiMessage *)GetMsg(g.win->UserPort))) {
                struct IntuiMessage copy = *im;
                ReplyMsg((struct Message *)im);
                if (copy.Class == IDCMP_MOUSEBUTTONS || copy.Class == IDCMP_MOUSEMOVE)
                    handle_mouse(&copy);
                else if (copy.Class == IDCMP_RAWKEY || copy.Class == IDCMP_VANILLAKEY)
                    handle_key(&copy);
                if (!g.win)
                    break;      /* a launch closed and reopened the window */
            }
        }
        if (g.win && (atb = cur_tb()) && tb_animating(atb)) {
            WaitTOF();
            tb_step(&g, atb);
        }
    }

out:
    amce_log("quitting", "");
    dbg_cpu_stop();
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
