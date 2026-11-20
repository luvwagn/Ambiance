/*
 * Ambiance - the classic layout's input: what the pointer is over
 * Copyright (c) 2026 Brian King
 *
 * Moved out of main.c (step 4 of claude/classic-as-layout.md): the mouse
 * and its wheel on Classic's screen, read against Classic's geometry, as a library
 * layout reads its own (its pointer()). The menu's and the list's rows,
 * the breadcrumb's links, the list's column (its thumb dragged), the
 * details pane, the text boxes' bars, and Now playing's buttons, slider
 * and bar; hover's soft highlight; single and double clicks. What a click
 * does is the program's (act_open, act_back, menu_select_now, open_dir,
 * the player's). The program hands Classic each mouse event through
 * classic_ui (cl_mouse); a layout's go to lv_mouse.
 */
#include "classic.h"

static ULONG last_secs, last_mics;
static ULONG last_ms;                       /* our own clock at the last click */
static int crumb_at(WORD x, WORD y);
static void crumb_go(int level);
static void hover_at(WORD x, WORD y);
static int np_slider_vol(WORD x);
static void np_hover(WORD x, WORD y);
static void np_mouse_up(void);
static BOOL np_click(WORD x, WORD y);
static BOOL dbl_click(const struct IntuiMessage *im);
static void gut_drag_to(WORD my);
static BOOL gut_click(WORD x, WORD y);

/* The breadcrumb level at (x, y), -1 if none (the current level is not a link). */
static int crumb_at(WORD x, WORD y)
{
    int i;
    if (y < U(26) || y >= U(41))
        return -1;
    if (np_src_x0 >= 0 && x >= np_src_x0 - U(2) && x <= np_src_x1 + U(2))
        return CRUMB_SRC;
    for (i = crumb_first; i < depth - 1; i++)
        if (crumb[i].x0 >= 0 && x >= crumb[i].x0 - U(2) && x <= crumb[i].x1 + U(2))
            return i;
    return -1;
}

/* Back to level i of the breadcrumb (0: the section's top). */
static void crumb_go(int level)
{
    if (level == CRUMB_SRC) {
        crumb_hover = -1;
        np_goto_source();                   /* Now playing's: where it is from */
        return;
    }
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

BOOL list_overflows(struct Page *p)
{
    return p && p->type == PT_LIST && p->list.count > list_visible();
}

/* The right edge of a list's area, its column included. */
WORD list_edge(struct Page *p)
{
    return has_aside(p) ? aside_x() - U(6) - 1 : g.w - 1;
}

/* Last x a list row may paint. */
WORD list_xr(struct Page *p)
{
    WORD e = list_edge(p);
    if (list_overflows(p))
        e -= GUT_W;                         /* the column */
    return e;
}

/* The bar's place and its thumb's. */
void gut_geom(struct Page *p, WORD *bx0, WORD *bx1, WORD *ty, WORD *th)
{
    LONG vis = list_visible(), n = p->list.count, h = g.footer_y - 2 - g.list_top + 1, t;
    *bx0 = list_edge(p) - GUT_W + 1 + GUT_GAP;
    *bx1 = *bx0 + GUT_BAR - 1;
    t = h * vis / n;
    if (t < U(10))
        t = U(10);
    if (t > h)
        t = h;
    *th = (WORD)t;
    *ty = g.list_top + (WORD)(n > vis ? (h - t) * p->top / (n - vis) : 0);
}

/* Move the soft highlight: to screen row `row` of the list, or to main
 * menu entry `menu_i` (-1: neither). */
void hover_set(WORD row, WORD menu_i)
{
    struct Page *p = cur();
    WORD old_row = hover_row, old_menu = hover_menu;
    if (row == hover_row && menu_i == hover_menu)
        return;
    hover_row = row;
    hover_menu = menu_i;
    if (pane_pending || menu_layout())
        return;
    /* drawn and shown as a small update, no flip: a flip waits for the
     * vertical blank, and the mouse's moves came quicker than that (the
     * highlight followed it in jerks, Ambiance busy at 40%) */
    if (p && p->type == PT_LIST) {
        WORD col = mark_column(p), x0 = g.pane_x - U(4), x1 = list_xr(p) + U(2);
        WORD r[2];
        int i;
        r[0] = old_row;
        r[1] = row;
        if (x0 < 0)
            x0 = 0;
        if (x1 > g.w - 1)
            x1 = g.w - 1;
        for (i = 0; i < 2; i++)
            if (r[i] >= 0 && p->top + r[i] < p->list.count) {
                WORD y0 = g.list_top + r[i] * g.list_row_h;
                BOOL was = gfx_quiet_begin();
                draw_list_row(p, p->top + r[i], col);
                gfx_quiet_end(&g, was, x0, y0, x1, y0 + g.list_row_h - 1);
            }
    }
    {
        WORD m[2];
        int i;
        m[0] = old_menu;
        m[1] = menu_i;
        for (i = 0; i < 2; i++)
            if (m[i] >= 0 && m[i] < nmenu) {
                WORD y0 = g.top + m[i] * g.row_h;
                BOOL was = gfx_quiet_begin();
                draw_menu_row(m[i]);
                gfx_quiet_end(&g, was, 0, y0, g.menu_w + U(3), y0 + g.row_h - 1);
            }
    }
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
    if ((x < g.menu_w + U(3) && y >= U(25) && y < g.footer_y) != hover_strip) {
        hover_strip = !hover_strip;         /* the column stays open under the pointer */
        if (row == hover_row && mi == hover_menu) {
            if (!pane_pending)
                menu_layout();
            return;
        }
    }
    hover_set(row, mi);
}

/* The volume slider: -----*----- between - and +. */
WORD np_knob_w(void)
{
    WORD k = U(3) > 2 ? U(3) : 2;
    return gfx_mode_tall(g.mode) ? k * 2 : k;
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

/* Now playing's button (NB_*) under the pointer at (x, y): NB_SLIDER on
 * the volume slider, -1 none. */
int np_button_at(WORD x, WORD y)
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

/* The mouse over a button or the slider: it lights up (dragging the
 * slider: the volume follows). */
static void np_hover(WORD x, WORD y)
{
    int h;
    if (!np_on() || !mp_loaded())
        return;
    if (np.sl_drag) {
        int v = np_slider_vol(x);
        if (v != mp_volume() || np_muted())
            np_set_volume(v);
        return;
    }
    h = np_button_at(x, y);
    if (np.cyc_x1 > np.cyc_x0 && x >= np.cyc_x0 && x <= np.cyc_x1 && y >= np.cyc_y0 &&
        y <= np.cyc_y1)
        h = NB_CYCLE;
    if (h != np.hov) {
        BOOL cyc = h == NB_CYCLE || np.hov == NB_CYCLE;
        np.hov = h;
        if (cyc)
            ui->np_cycle();
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
    if (x >= np.cyc_x0 && x <= np.cyc_x1 && y >= np.cyc_y0 && y <= np.cyc_y1 && np.cyc_x1 > np.cyc_x0) {
        next_visual();                      /* the cycle gadget */
        return TRUE;
    }
    i = np_button_at(x, y);
    if (i == NB_SLIDER) {                   /* to that volume, and drag from there */
        np.sl_drag = TRUE;
        np.hov = NB_SLIDER;
        np_set_volume(np_slider_vol(x));
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
        np_seek_part((ULONG)(x - np.x0), (ULONG)np.bar_w);
        return TRUE;
    }
    return FALSE;
}

static BOOL dbl_click(const struct IntuiMessage *im)
{
    if (DoubleClick(last_secs, last_mics, im->Seconds, im->Micros))
        return TRUE;
    return last_ms && time_ms() - last_ms <= dc_time();
}

static BOOL click_opened;           /* the last click opened something (one click) */

static LONG last_click = -1;

static WORD last_x = -100, last_y = -100;   /* where the last click was */

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
        ov_relabel(ov_letter_in(p->list.items, p->list.count, p->sel));
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
        x <= list_edge(p) - GUT_W || x > list_edge(p))
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

void cl_mouse(struct IntuiMessage *im)
{
    /* inside the window's borders (none on our own screen) */
    WORD x = im->MouseX - g.win->BorderLeft, y = im->MouseY - g.win->BorderTop;
    BOOL dbl;
    LONG idx = -1;
    struct TextBox *tb;
    if (im->Class == IDCMP_MOUSEBUTTONS && (im->Code == SELECTDOWN || im->Code == MENUDOWN) &&
        coast_interrupt()) {
        if (debug_on)
            amb_log("click", "it stopped a coast, and did nothing else");
        return;                             /* a click during a coast only stops it */
    }
    if (im->Class == IDCMP_MOUSEBUTTONS && !(x < g.menu_w && y >= g.top)) {
        pane_settle();              /* a click outside the menu acts on the section */
        if (im->Code == SELECTDOWN && x >= g.menu_w && y >= g.top && y < g.footer_y)
            menu_unpin();           /* the content clicked: the menu to its icons */
    }
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
    if (debug_on) {
        char m[320];
        struct Page *dp = cur();
        sprintf(m, "at %ld,%ld; page %.30s, type %ld, %ld rows from y %ld, %ld high, list to x %ld; "
                   "focus %ld; opened %ld; last %ld; time %lu.%06lu (last %lu.%06lu, %lu ms ago)",
                (long)x, (long)y, dp ? dp->title : "-", dp ? (long)dp->type : -1L,
                dp ? (long)dp->list.count : 0L, (long)g.list_top, (long)g.list_row_h,
                dp && dp->type == PT_LIST ? (long)list_xr(dp) : -1L, (long)focus,
                (long)click_opened, (long)last_click, (unsigned long)im->Seconds,
                (unsigned long)im->Micros, (unsigned long)last_secs, (unsigned long)last_mics,
                (unsigned long)(time_ms() - last_ms));
        amb_log("click", m);
    }
    nav_moved();
    disarm();
    if (click_opened) {
        /* a single click opened something: the second click of a habitual
         * double-click is not a click on what opened */
        click_opened = FALSE;
        if (dbl_click(im) &&
            x - last_x <= U(6) && last_x - x <= U(6) && y - last_y <= U(6) &&
            last_y - y <= U(6)) {           /* on the same spot: the habit's second half */
            last_secs = im->Seconds;
    last_ms = time_ms();
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
        dbl = idx + 1000 == last_click && dbl_click(im);
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
                BOOL on_bar = pv_tb.open && pv_tb.bar_w && x >= pv_tb.bar_x - 2;
                if (pv_tb.open && pv_tb.bar_w && x >= aside_x())
                    set_focus(FOCUS_PREVIEW);
                /* a double-click on the pane plays (opens) what it shows:
                 * the entry selected, as a double-click on its row would */
                dbl = !on_bar && p->sel + 100000 == last_click &&
                      dbl_click(im);
                last_click = p->sel + 100000;
                last_secs = im->Seconds;
    last_ms = time_ms();
                last_mics = im->Micros;
                last_x = x;
                last_y = y;
                if (dbl && p->list.count && p->sel >= 0 && p->sel < p->list.count &&
                    has_aside(p)) {
                    struct Item *it = &p->list.items[p->sel];
                    if (it->kind == IT_SONG || it->kind == IT_MOD || it->kind == IT_PIC ||
                        it->kind == IT_ANIM || it->kind == IT_RUN || it->kind == IT_PLEXTRACK || it->kind == IT_PLEXPHOTO) {
                        if (debug_on)
                            amb_log("click: double on the pane", it->label);
                        set_focus(FOCUS_CONTENT);
                        act_open(FALSE);
                    }
                }
                return;
            }
            idx = p->top + (y - g.list_top) / g.list_row_h;
            if (idx >= p->list.count)
                return;
            dbl = idx == last_click && dbl_click(im);
            last_click = idx;
            if (is_heading(p, idx))
                return;                     /* a heading: nothing to choose */
            if (focus != FOCUS_CONTENT)
                set_focus(FOCUS_CONTENT);
            if (p->sel != idx) {
                p->sel = idx;
                det.clicked = TRUE;          /* its preview waits for a second click */
                draw_title_count(p);
                draw_list_rows(p);
                det.clicked = FALSE;
            }
            {
                /* one click opens what cannot be played (a folder, a
                 * setting); what can (a game, a picture, a folder that
                 * plays as a slideshow) is selected, and a double-click
                 * plays it (a folder: goes inside) */
                struct Item *it = &p->list.items[idx];
                BOOL playable = it->kind == IT_RUN || it->kind == IT_PIC || it->kind == IT_ANIM ||
                                it->kind == IT_MOD ||
                                it->kind == IT_SONG || it->kind == IT_PLEXPHOTO ||
                                (it->kind == IT_DIR && it->mark == MARK_PLAY);
                if (debug_on) {
                    char m[160];
                    sprintf(m, "row %ld: %.80s (kind %ld), %s, %s", (long)idx, it->label,
                            (long)it->kind, playable ? "playable" : "opens", dbl ? "double" : "single");
                    amb_log("click", m);
                }
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
    last_ms = time_ms();
    last_mics = im->Micros;
    last_x = x;
    last_y = y;
}

/* One wheel notch scrolls whatever is under the pointer, as on other
 * desktops: one entry in the menu and in lists, three lines of text.
 * Scrolling the content pane also gives it the keyboard focus. Over the
 * header or footer the notch goes to whatever has the focus. */
void cl_wheel(LONG d, WORD x, WORD y)
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
