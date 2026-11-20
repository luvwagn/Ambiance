/*
 * Ambiance - the classic layout: Ambiance's own list view, built in
 * Copyright (c) 2026 Brian King
 *
 * Moved out of main.c (step 3b of claude/classic-as-layout.md), as it was:
 * the header (logo, Debug line, playing mark, clock), the menu and its
 * slide, the title and its trail, the footer, the list's rows, its bar and
 * moves, the big letter, the entry details' pane (pictures, text, the Plex
 * details, the font previews), the spinners, and Now playing's page. The
 * program drives it through classic_ui and classic_view (classic.h).
 */
#include "classic.h"

static int mod_icon(int m);
static void np_rrect(WORD x, WORD y, WORD w, WORD h, WORD rx, WORD ry, UBYTE pen);
static void np_draw_cycle(void);
static void cl_np_update(void);
static void cl_menu_slide(UWORD from, UWORD to);
static void pv_wrap(WORD x, WORD *y, WORD w, WORD max_y, const char *s, UBYTE pen);
static void spin_metrics(WORD *ring, WORD *ringy, WORD *r, WORD *ry);
static UWORD spin_phase(ULONG since);
static void spin_draw(WORD cx, WORD cy, UWORD phase, BOOL first);
static void radio_spin(void);
static void cl_busy(ULONG now);
static void hdr_np_draw(BOOL quiet);
static void cl_header_np(BOOL quiet, BOOL on);
static void cl_clock(void);
static void cl_status(const char *show);
static void cl_draw_header(void);
static void cl_draw_menu_row(int i);
static void cl_draw_menu(void);
static void cl_draw_footer(void);
static void title_band_rest(WORD x0, WORD x1, WORD right_from);
static void cl_draw_title_count(struct Page *p);
static void cl_draw_title(struct Page *p);
static void date_text(LONG days, char *out);
static void pv_line(const char *s, WORD w);
static void pv_pics_lines(struct Item *it, struct LJob *j, BOOL drawn, WORD w);
static void pv_spinner(BOOL first);
static void pv_spin_tick(void);
static void pv_draw_text(struct Page *p);
static void fill_around_pic(WORD x0, WORD y0, WORD x1, WORD y1);
static void pv_shot_key(UBYTE shot, const char *path, char *key);
static BOOL pv_keeps(struct Page *p, LONG idx);
static void pv_draw_picture(void);
static BOOL cv_shots(void);
static void cv_clear_picture(struct Page *p);
static void cv_clear_all(struct Page *p);
static void cv_start(struct Page *p);
static void cv_empty(struct Page *p);
static void cv_begin(struct Page *p, BOOL keep);
static void cv_no_picture(struct Page *p);
static void cv_text(struct Page *p);
static void cv_picture(struct Page *p);
static void cv_tick(struct Page *p);
static const char *spv_label(int mod);
static void style_preview(struct Page *p);
static BOOL ov_over(struct Page *p);
static void ov_stamp(void);
static void ov_rows(struct Page *p, WORD y0, WORD y1);
static void ov_rows_under(struct Page *p);
static void cl_letter_show(struct Page *p);
static void cl_letter_relabel(char c);
static void cl_letter_hide(struct Page *p);
static UBYTE list_planes(const struct Page *p);
static void cl_draw_list_row(struct Page *p, LONG idx, WORD col);
static void cl_draw_gutter_planes(struct Page *p, UBYTE planes);
static void cl_draw_list_rows(struct Page *p);
static WORD rows_right(struct Page *p, LONG top, WORD col);
static void blit_rows(LONG src, LONG dst, LONG n);
static void cl_draw_list_move(struct Page *p, LONG old_sel, LONG old_top);
static void draw_list_move2(struct Page *p, LONG old_sel, LONG old_top);
static void draw_plex_logo(WORD x, WORD y);
static void draw_plexmark(void);
static void cl_draw_text_picture(struct Page *p);
static void cl_draw_pane(BOOL full);
static void np_button(int i);
static void np_volume_text(void);
static WORD np_knob_x(int v);
static BOOL np_slider_hot(void);
static void np_slider(void);
static void np_draw_progress(BOOL full, const struct MpStatus *st);
static void np_draw_progress_(BOOL full, const struct MpStatus *st);
static void np_cycle_layout(void);
static void np_visual_changed(void);
static void np_draw_meters(BOOL full, const struct MpStatus *st);
static void np_line(WORD y, const char *s, UBYTE fg, int ts);
static void np_pair(WORD y, const char *label, const char *value);
static void np_audio_line(WORD y, const char *label, const char *codec, ULONG kbps, ULONG rate, ULONG ch);
static void np_playback_line(WORD y, const struct StInfo *in);
static void np_draw_station(void);
static void cl_np_draw(BOOL full);
static void np_eq_redraw(void);
static void cl_eq_tick(void);
static void cl_draw_all(void);
static void cl_pane_preview(BOOL first);
static void np_art_draw(void);
static BOOL plex_keeps(struct Page *p, LONG idx);
static void plex_sum_find(const struct Item *it, char *out, LONG max);
static void plex_sum_ask(const struct Item *it, WORD y);
static BOOL plex_pv_key(struct Page *p, char *key);
static BOOL plex_text_kept(struct Page *p);
static BOOL plex_take(struct Page *p);
static void pv_photo_keep(const char *file);
static void plex_pv_draw(struct Page *p, BOOL was_shown);
static void cl_tick(void);
static void draw_text_picture(struct Page *p);

/* A section's menu icon (the icons are the sections', in their order;
 * Now playing has Music's) */
static int mod_icon(int m)
{
    return m == MOD_NOWPLAYING ? ICON_MUSIC : m;
}


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
        WORD tw = gfx_textlen(&g, _("Loading..."), TS_SMALL);
        gfx_text(&g, cx - tw / 2, cy + ringy + ry + U(6), _("Loading..."), PEN_DIM, PEN_BG, 0,
                 TS_SMALL);
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

/* the classic's: in the middle of the list's half */
static void cl_busy(ULONG now)
{
    struct Page *p = cur();
    if (!busy.on) {
        WORD ring, ringy, r, ry, x1;
        if ((LONG)(now - busy.since) < SPIN_AFTER)
            return;
        spin_metrics(&ring, &ringy, &r, &ry);
        x1 = p && has_aside(p) ? aside_x() - U(6) : g.pane_x + g.pane_w;
        busy.cx = (g.pane_x + x1) / 2;
        busy.cy = (g.list_top + g.footer_y - 2) / 2 - U(8);
        {
            WORD tw = gfx_textlen(&g, _("Loading..."), TS_SMALL), hw = ring + r + U(4);
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

static void hdr_np_draw(BOOL quiet)
{
    BOOL on = mp_loaded();
    if (!on && !hdr_np_on)
        return;
    ui->header_np(quiet, on);               /* (a layout: from its model, told) */
    hdr_np_on = on;
}

static void cl_header_np(BOOL quiet, BOOL on)
{
    WORD n = g.mark_px, x = clock_left() - n - U(2), y = (U(24) - n) / 2;
    BOOL was = FALSE;
    if (quiet)
        was = gfx_quiet_begin();
    if (on)
        gfx_mark(&g, MARK_PLAYING, x, y, PEN_TEXT, PEN_BAR);
    else
        gfx_fill(&g, x, y, x + n - 1, y + n - 1, PEN_BAR);
    if (quiet)
        gfx_quiet_end(&g, was, x, y, x + n - 1, y + n - 1);
}

static void cl_clock(void)
{
    char buf[40];
    WORD tw;
    clock_text(buf);
    gfx_fill(&g, clock_left(), 0, g.w - 1, U(24) - 1, PEN_BAR);
    if (!buf[0])
        return;
    tw = gfx_textlen(&g, buf, TS_SMALL | TS_TABULAR);   /* (its left edge still as the minutes go) */
    gfx_text(&g, g.w - U(8) - tw, (U(24) - g.font_h) / 2, buf, PEN_TEXT, PEN_BAR, 0, TS_SMALL | TS_TABULAR);
}

static void cl_status(const char *show)
{
    WORD x0 = U(6) + g.logo_w + U(10);
    WORD x1 = clock_left() - g.mark_px - U(8);
    if (!show) {
        if (status_shown[0])
            gfx_fill(&g, x0, 0, x1, U(24) - 1, PEN_BAR);
        status_shown[0] = 0;
        return;
    }
    if (status_shown[0] && strcmp(show, status_shown) == 0)
        return;                             /* unchanged: leave it be */
    gfx_fill(&g, x0, 0, x1, U(24) - 1, PEN_BAR);
    gfx_text(&g, x0, (U(24) - g.font_h) / 2, show, PEN_ACCENT, PEN_BAR, x1 - x0, TS_SMALL | TS_TABULAR);
    amb_strlcpy(status_shown, show, sizeof(status_shown));
}

static void cl_draw_header(void)
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

static void cl_draw_menu_row(int i)
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
    gfx_icon(&g, mod_icon(menu[i]), U(3), y + (g.row_h - g.icon_px) / 2, fg, bg);
    if (g.menu_w > U(30))                   /* not icons alone (menu_layout) */
        gfx_text(&g, U(23), y + (g.row_h - g.font_h) / 2, _(mod_names[menu[i]]), fg, bg,
                 g.menu_w - U(24), TS_BOLD);
    if (g.planar)
        SetWriteMask(g.rp, 0xFF);
}

static void cl_draw_menu(void)
{
    ULONG t0 = time_ms();
    int i;
    gfx_fill(&g, 0, U(25), g.menu_w + U(2), g.footer_y - 2, PEN_BG);
    for (i = 0; i < nmenu; i++)
        draw_menu_row(i);
    gfx_fill(&g, g.menu_w + U(2), U(25), g.menu_w + U(3) - 1, g.footer_y - 2, PEN_RULE);
    if (t0)
        menu_draw_ms = time_ms() - t0;
}

static void cl_draw_footer(void)
{
    WORD fy = foot_hidden ? g.h - U(12) : g.footer_y;
    hint_shown = msg[0] ? NULL : hint_text();
    if (foot_hidden && !msg[0]) {
        if (foot_over) {                    /* a message's footer gone: what it covered */
            foot_over = FALSE;
            foot_redraw = TRUE;
        }
        return;
    }
    if (foot_hidden)
        foot_over = TRUE;
    /* Redrawn as the hints follow the selection: on a native screen planes
     * 0-3 only (its pens are below 16, and nothing else draws in its
     * place, cleared on all planes with the whole screen), half an AGA
     * screen's. (Over the content, a window's: all of them.) */
    if (g.planar && !foot_hidden)
        SetWriteMask(g.rp, 0x0F);
    gfx_fill(&g, 0, fy - U(1), g.w - 1, fy - 1, PEN_RULE);
    gfx_fill(&g, 0, fy, g.w - 1, g.h - 1, PEN_BAR);
    gfx_text(&g, U(6), fy + (g.h - fy - g.font_h) / 2, msg[0] ? msg : hint_text(),
             msg[0] ? PEN_ACCENT : PEN_DIM, PEN_BAR, g.w - U(12) - gfx_window_grip(&g), TS_SMALL);
    if (g.planar)
        SetWriteMask(g.rp, 0xFF);
}

/* The list's "n/total" at the right of the title row. Only this changes
 * while a list scrolls, so it is redrawn alone: drawn with its background
 * in one pass (JAM2) over the old digits, never cleared first, and only
 * the sliver a shorter number leaves behind is cleared. Clearing the whole
 * title row every step flickered on 8-plane screens. */
static WORD count_left = -1;    /* left edge of the count drawn last */

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

static void cl_draw_title_count(struct Page *p)
{
    char count[24];
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

static void cl_draw_title(struct Page *p)
{
    char widest[24];
    int i;
    WORD cw = 0, maxw, x, sep, dots;
    sep = gfx_textlen(&g, " > ", TS_BOLD);
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
    np_src_x0 = np_src_x1 = -1;
    if (p->type == PT_PLAYER && np_away()) {
        /* Now playing from another section: itself, then where it is from
         * (a link there), not this section's path, which it is not part of */
        char from[140];
        BOOL hov = crumb_hover == CRUMB_SRC && mouse_mode;
        WORD w, room;
        for (i = 0; i < depth; i++)
            crumb[i].x0 = crumb[i].x1 = -1;
        crumb_first = 0;
        w = gfx_textlen(&g, p->title, TS_BOLD);
        gfx_text(&g, x, U(28), p->title, PEN_ACCENT, PEN_BG, maxw, TS_BOLD | TS_OPAQUE);
        x += w < maxw ? w : maxw;
        amb_strlcpy(from, _(mod_names[np_src]), sizeof(from));
        if (np_src_list[0] && strcmp(np_src_list, from)) {
            amb_strlcat(from, " > ", sizeof(from));
            amb_strlcat(from, np_src_list, sizeof(from));
        }
        room = g.pane_x + maxw - x - sep;
        if (room > 0) {
            gfx_text(&g, x, U(28), "  \267  ", PEN_DIM, PEN_BG, 0, TS_BOLD | TS_OPAQUE);
            x += gfx_textlen(&g, "  \267  ", TS_BOLD);
            room = g.pane_x + maxw - x;
            w = gfx_textlen(&g, from, TS_BOLD);
            if (w > room)
                w = room;
            if (w > 0) {
                if (hov)
                    gfx_fill(&g, x - U(2), U(27), x + w + U(1), U(28) + g.font_h, PEN_RULE);
                gfx_text(&g, x, U(28), from, PEN_TEXT, hov ? PEN_RULE : PEN_BG, room,
                         TS_BOLD | TS_OPAQUE);
                np_src_x0 = x;
                np_src_x1 = x + w - 1;
                x += w;
            }
        }
        title_band_rest(g.pane_x, x, g.w);
        draw_title_count(p);
        gfx_fill(&g, g.pane_x, U(41), g.pane_x + g.pane_w - 1, U(42) - 1, PEN_RULE);
        return;
    }
    if (p->type == PT_PLAYER && depth > 2) {
        /* Now playing: its section, "..." (the list it came from: what is
         * playing is on the page below, not spelt out twice), and itself */
        static const char *const dots_s = "...";
        int lv[3];
        const char *txt[3];
        int k;
        lv[0] = 0;
        txt[0] = pages[0].title;
        lv[1] = depth - 2;
        txt[1] = dots_s;
        lv[2] = depth - 1;
        txt[2] = pages[depth - 1].title;
        for (i = 0; i < depth; i++)
            crumb[i].x0 = crumb[i].x1 = -1;
        for (k = 0; k < 3; k++) {
            BOOL last = k == 2, hov = !last && lv[k] == crumb_hover && mouse_mode;
            WORD w = gfx_textlen(&g, txt[k], TS_BOLD), room = g.pane_x + maxw - x;
            if (room <= 0)
                break;
            if (w > room)
                w = room;
            if (hov)
                gfx_fill(&g, x - U(2), U(27), x + w + U(1), U(28) + g.font_h, PEN_RULE);
            gfx_text(&g, x, U(28), txt[k], last ? PEN_ACCENT : PEN_TEXT, hov ? PEN_RULE : PEN_BG,
                     room, TS_BOLD | TS_OPAQUE);     /* (the last: Now playing, in its orange) */
            crumb[lv[k]].x0 = x;
            crumb[lv[k]].x1 = x + w - 1;
            x += w;
            if (!last) {
                gfx_text(&g, x, U(28), " > ", PEN_DIM, PEN_BG, 0, TS_BOLD | TS_OPAQUE);
                x += sep;
            }
        }
        crumb_first = 0;
        title_band_rest(g.pane_x, x, g.w);
        draw_title_count(p);
        gfx_fill(&g, g.pane_x, U(41), g.pane_x + g.pane_w - 1, U(42) - 1, PEN_RULE);
        return;
    }
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
            gfx_text(&g, x, U(28), pages[i].title,
                     pages[i].type == PT_PLAYER ? PEN_ACCENT : last ? PEN_WHITE : PEN_TEXT, bg, room,
                     TS_BOLD | TS_OPAQUE);          /* (Now playing: in its orange) */
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

/* "12-Mar-1994" from a DateStamp's days ("1994-03-12" in Japanese). */
static void date_text(LONG days, char *out)
{
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
    if (date_ymd())
        sprintf(out, "%ld-%02ld-%02ld", (long)y, (long)m + 1, (long)days + 1);
    else
        sprintf(out, "%ld-%s-%ld", (long)days + 1, _(mon_abbr[m]), (long)y);
}

static void pv_line(const char *s, WORD w)
{
    if (!s[0])
        list_add(&pv_list, "", "", "", IT_TEXT);
    else
        wrap_into(&pv_list, s, w);
}

#define PV_SPIN_AFTER SPIN_AFTER    /* ms without its picture before the spinner */
WORD pv_box_y, pv_box_h;     /* where the picture goes */
static BOOL pv_spin_on;

static UWORD pv_spin_phase;

static ULONG pv_spin_since;         /* when it began turning */

static void pv_pics_lines(struct Item *it, struct LJob *j, BOOL drawn, WORD w)
{
    char line[120];
    pv_line(it->label, w);
    if (is_dvd_image(it)) {
        /* a DVD's image: what its first sectors say, and its size (some
         * 12 KB read once an image, the last 4 kept) */
        static struct { char path[256]; LONG kind; ULONG sectors; } ic[4];
        static int ic_next;
        int q;
        for (q = 0; q < 4; q++)
            if (ic[q].path[0] && !strcmp(ic[q].path, it->path))
                break;
        if (q == 4) {
            char err[160];
            q = ic_next;
            ic_next = (ic_next + 1) & 3;
            amb_strlcpy(ic[q].path, it->path, sizeof(ic[q].path));
            if ((ic[q].kind = dvdd_image_kind(it->path, &ic[q].sectors, err, sizeof(err))) < 0)
                amb_log("dvd", err);
        }
        pv_line(ic[q].kind == DVDI_VIDEO ? _("A DVD-Video disc image")
                : ic[q].kind == DVDI_UDF ? _("A disc image (UDF)")
                : ic[q].kind >= 0 ? _("Not a DVD-Video disc image")
                : _("This image could not be read (the log says why)"), w);
        if (ic[q].sectors >= 524288)
            sprintf(line, "%lu.%02lu GB", (unsigned long)(ic[q].sectors / 524288),
                    (unsigned long)(ic[q].sectors % 524288 * 100 / 524288));
        else
            sprintf(line, "%lu MB", (unsigned long)(ic[q].sectors / 512));
        if (ic[q].sectors)
            pv_line(line, w);
        if (ic[q].kind == DVDI_VIDEO || ic[q].kind == DVDI_UDF) {
            if (!dvd_lib_there())
                pv_line(_("DVDs need dvdnav.library (in Ambiance's Libs)"), w);
            else {
                pv_line(_("Fire plays its film, or its programmes one after another, on the ZZ9000"), w);
                pv_line(_("N and P: the next and previous chapter or programme"), w);
                pv_line(_("Decoded by the CPU: its menus, M to bring them back"), w);
            }
        }
        if (j && j->days > 0) {             /* (j->size: -1 for 2 GB and more) */
            char date[32];
            pv_line("", w);
            date_text(j->days, date);
            pv_line(date, w);
        }
        return;
    }
    if (is_dvd_row(it)) {
        /* a DVD: what Fire does (the disc itself read only then) */
        pv_line(_("A DVD-Video disc"), w);
        if (!dvd_lib_there()) {
            pv_line(_("DVDs need dvdnav.library (in Ambiance's Libs)"), w);
            return;
        }
        pv_line(_("Fire plays its film, or its programmes one after another, on the ZZ9000"), w);
        pv_line(_("N and P: the next and previous chapter or programme"), w);
        pv_line(_("Decoded by the CPU: its menus, M to bring them back"), w);
        if (j && j->size >= 0) {
            char date[32];
            pv_line("", w);
            date_text(j->days, date);
            pv_line(date, w);
        }
        return;
    }
    if (it->kind == IT_ANIM && it->sortkey && !strcmp(it->sortkey, "MPEG")) {
        /* an MPEG file: what its headers say (64 KB of its start and end
         * read once a file, the last 8 kept: a list moved through is not
         * read again) */
        static struct { char path[256]; struct VProbe vp; BOOL ok; } pc[8];
        static int pc_next;
        int q;
        BOOL ok;
        struct VProbe vp;
        for (q = 0; q < 8; q++)
            if (pc[q].path[0] && !strcmp(pc[q].path, it->path))
                break;
        if (q == 8) {
            q = pc_next;
            pc_next = (pc_next + 1) & 7;
            amb_strlcpy(pc[q].path, it->path, sizeof(pc[q].path));
            pc[q].ok = vprobe(it->path, &pc[q].vp, 64UL * 1024);
        }
        ok = pc[q].ok;
        vp = pc[q].vp;
        if (ok) {
            char t[16];
            sprintf(line, "%s, %ld x %ld", vprobe_kind(&vp), (long)vp.w, (long)vp.h);
            if (vp.fps_milli)
                sprintf(line + strlen(line), ", %ld.%02ld fps", (long)(vp.fps_milli / 1000),
                        (long)(vp.fps_milli % 1000 / 10));
            pv_line(line, w);
            if (vp.ms) {
                amb_time_text(t, vp.ms);
                pv_line(t, w);
            }
            pv_line(vp.mp2 || vp.ac3 ? _("With sound") : _("No sound"), w);
            if (vp.ts)
                pv_line(_("A transport stream"), w);
            else if (!vp.ps)
                pv_line(_("A bare video stream"), w);
        } else if (vp.h264) {
            sprintf(line, _("%s video: the ZZ9000 plays MPEG-1 and MPEG-2"), vprobe_kind(&vp));
            pv_line(line, w);
        } else {
            pv_line(_("Not an MPEG video Ambiance can read"), w);
        }
    }
    if (it->kind == IT_PIC || it->kind == IT_ANIM) {
        char date[32];
        if (drawn) {
            pic_describe(&g.pic, line, sizeof(line));
            pv_line(line, w);
        }
        if (it->kind == IT_ANIM && drawn && g.pic.frames) {
            ULONG ds = (g.pic.ms + 50) / 100;
            sprintf(line, _("%ld frames, %ld.%ld s"), (long)g.pic.frames, (long)(ds / 10),
                    (long)(ds % 10));
            pv_line(line, w);
        }
        sprintf(line, "%s", drawn && g.pic.fmt[0] ? g.pic.fmt : it->sortkey);
        if (j->size >= 0) {
            char kb[24];
            sprintf(kb, "  -  %ldKB", (long)((j->size + 1023) / 1024));
            amb_strlcat(line, kb, sizeof(line));
        }
        pv_line(line, w);
        pv_line("", w);
        {
            const char *e = strrchr(it->path, '/');
            if (!e)
                e = strchr(it->path, ':');
            amb_strlcpy(line, it->path, e ? (LONG)(e - it->path) + 1 + (*e == ':') : (LONG)sizeof(line));
            if (line[0])
                pv_line(line, w);
        }
        if (j->size >= 0) {
            date_text(j->days, date);
            sprintf(line, _("Changed %s"), date);
            pv_line(line, w);
        }
    } else if (cur() && cur()->pics == 2) {     /* Videos: its animations, not its pictures */
        LONG na = 0;
        const char *c = it->sortkey;
        while (c && *c >= '0' && *c <= '9')
            na = na * 10 + (*c++ - '0');    /* its count, as its row's tag */
        line[0] = 0;
        if (j->nd > 0)
            sprintf(line, j->nd == 1 ? _("%ld folder") : _("%ld folders"), (long)j->nd);
        if (na > 0) {
            if (line[0])
                strcat(line, ", ");
            sprintf(line + strlen(line), na == 1 ? _("%ld animation") : _("%ld animations"), (long)na);
        }
        if (line[0])
            pv_line(line, w);
        pv_line("", w);
        pv_line(_("Fire or Right: open"), w);
    } else if ((it->flags & ITEM_DISABLED) || (j->np <= 0 && j->nd == 0)) {
        pv_line(_("No pictures in this folder."), w);
    } else if (j->nd == 0) {
        sprintf(line, j->np == 1 ? _("%ld picture") : _("%ld pictures"), (long)j->np);
        pv_line(line, w);
        pv_line("", w);
        pv_line(_("Fire: slideshow"), w);
        pv_line(_("Right: open the folder"), w);
    } else {
        sprintf(line, j->nd == 1 ? _("%ld folder") : _("%ld folders"), (long)j->nd);
        if (j->np > 0) {
            strcat(line, ", ");
            sprintf(line + strlen(line), j->np == 1 ? _("%ld picture") : _("%ld pictures"),
                    (long)j->np);
        }
        pv_line(line, w);
        pv_line("", w);
        pv_line(_("Fire or Right: open"), w);
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
    if (!det.shown || !p || p != det.page || p->sel != det.sel || !has_aside(p) || det.pic_shown ||
        !det.has_box || hold_active() || ov.on || pane_pending)
        return;
    if (!pv_spin_on) {
        if ((LONG)(time_ms() - det.since) < PV_SPIN_AFTER)
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

/* The text, under the picture's place (or at the top with no picture). */
static void pv_draw_text(struct Page *p)
{
    WORD x0, w, max_y;
    x0 = aside_x();
    w = aside_width() - U(4);
    max_y = g.footer_y - 2;
    WORD y = det.has_box ? pv_box_y + pv_box_h + U(6) : g.list_top;
    struct LJob *j = det.tj;
    LONG i;
    tb_close(&pv_tb);
    gfx_fill_bg(&g, x0, det.has_box ? pv_box_y + pv_box_h : g.list_top, g.w - 1, max_y);
    if (pv_list.pool)
        list_free(&pv_list);
    if (!list_init(&pv_list))
        return;
    if (p->pics)
        pv_pics_lines(&p->list.items[p->sel], j, det.pic_shown && g.pic_w, w - U(9));
    else if (p->igame) {
        /* iGame's entry: its fields (igame.c), in place of a fact sheet */
        LONG n = ig_facts(ig_lookup(&igcat, p->list.items[p->sel].path), fact_buf, FACT_LINES);
        for (i = 0; i < n; i++)
            pv_line(fact_buf[i], w - U(9));
    } else
        /* the whole fact sheet, in a text box with a scroll bar when it is
         * longer than the room left (wheel or bar; Up/Down stay with the list) */
        for (i = 0; i < j->nlines; i++)
            pv_line(j->lines[i], w - U(9));
    if (!p->pics && j->text) {
        /* the album's review or the artist's biography (.nfo), a paragraph
         * a line, a blank line between */
        char *s = j->text, *e, c;
        while (*s) {
            for (e = s; *e && *e != '\n'; e++)
                ;
            c = *e;
            *e = 0;                         /* the paragraph alone, then put back */
            pv_line("", w - U(9));
            pv_line(s, w - U(9));
            *e = c;
            s = c ? e + 1 : e;
        }
    }
    if (pv_list.count && max_y - y >= g.font_h) {
        pv_tb.pos = pv_tb.target = 0;
        pv_tb.focused = focus == FOCUS_PREVIEW;
        if (tb_open(&g, &pv_tb, x0, y, w - U(9), max_y - y + 1, &pv_list, TRUE))
            tb_draw(&g, &pv_tb);
    }
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

/* The picture file a preview of path (shot: SHOT_*) shows, into key ("":
 * not known yet, a folder not scanned). */
static void pv_shot_key(UBYTE shot, const char *path, char *key)
{
    key[0] = 0;
    if (shot == SHOT_FILE) {
        amb_strlcpy(key, path, 300);
    } else if (shot == SHOT_AGS) {
        key[0] = 1;                         /* an entry's screenshot: never a file's key */
        amb_strlcpy(key + 1, path, 299);
    } else if (shot == SHOT_FOLDER || shot == SHOT_SONG) {
        det_first(shot, path, key);
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
        !det_kind(p, idx, path, sizeof(path), &shot) || !shot)
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
    WORD x0, w, dh = 0;
    struct LJob *j = det.sj;
    BOOL taken = FALSE, ok = FALSE;
    x0 = aside_x();
    w = aside_width() - U(4);
    if (j && j->thumb) {
        char m[400];
        ULONG t0 = time_ms(), now;
        char by[64];
        ok = gfx_thumb(&g, j->thumb, &j->info, x0, pv_box_y, w, pv_box_h, &dh);
        now = time_ms();
        /* who read it: Ambiance's own readers (ILBM, JPEG by jdec), or a
         * datatype (its class named), and the format */
        if (!strcmp(j->info.cls, "jdec"))
            strcpy(by, "JPEG read by Ambiance (jdec)");
        else if (j->own)
            sprintf(by, "%.8s read by Ambiance", j->info.fmt[0] ? j->info.fmt : "ILBM");
        else if (j->info.cls[0])
            sprintf(by, "decoded by %.40s.datatype", j->info.cls);
        else
            strcpy(by, "decoded by a datatype");
        /* where the time went: waiting for the loader, reading, making,
         * drawing; and in all since the selection came to rest */
        sprintf(m, "%.60s, %lu x %lu, depth %lu: %s%lu ms to start, %s in %lu ms, "
                   "made %lu x %lu, %lu colours, in %lu ms, drawn in %lu ms "
                   "(colours %lu, pixels %lu, copy up %lu); "
                   "shown %lu ms after the move%s%s%s",
                FilePart((STRPTR)j->path), (unsigned long)j->info.w, (unsigned long)j->info.h,
                (unsigned long)j->info.depth, j->urgent ? "" : "read ahead, ",
                (unsigned long)(j->t_begun - j->t_sent), by,
                (unsigned long)j->ms_decode, (unsigned long)j->thumb->w,
                (unsigned long)j->thumb->h, (unsigned long)j->thumb->ncol,
                (unsigned long)j->ms_thumb, (unsigned long)(now - t0),
                (unsigned long)gfx_draw_ms[0], (unsigned long)gfx_draw_ms[1],
                (unsigned long)gfx_draw_ms[2],
                (unsigned long)(now - det.since), "", "", "");
        amb_log("preview", m);
        if (j->ms_parts[0] + j->ms_parts[1] + j->ms_parts[2] + j->ms_parts[3]) {
            sprintf(m, "made in: reading the picture %lu ms, counting its colours %lu, "
                       "choosing %lu, mapping %lu",
                    (unsigned long)j->ms_parts[0], (unsigned long)j->ms_parts[1],
                    (unsigned long)j->ms_parts[2], (unsigned long)j->ms_parts[3]);
            amb_log("preview", m);
        }
    } else if (j && j->obj) {
        ok = gfx_picture_obj(&g, j->obj, j->remapped, x0, pv_box_y, w, pv_box_h, TRUE, &dh, &taken);
    }
    pv_onscr.key[0] = 0;
    if (ok && j) {
        if (j->mode == SHOT_FOLDER || j->mode == SHOT_SONG)
            amb_strlcpy(pv_onscr.key, j->first, sizeof(pv_onscr.key));
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

static BOOL cv_shots(void)
{
    return pv_pictures();
}

static void cv_clear_picture(struct Page *p)
{
    (void)p;
    gfx_picture_clear(&g, aside_x(), g.list_top, g.w - 1, g.footer_y - 2);
}

static void cv_clear_all(struct Page *p)
{
    (void)p;
    tb_close(&pv_tb);
    gfx_fill_bg(&g, aside_x(), g.list_top, g.w - 1, g.footer_y - 2);
}

static void cv_start(struct Page *p)
{
    (void)p;
    pv_spin_on = FALSE;
    tb_close(&pv_tb);
    pv_box_y = g.list_top;
    pv_box_h = (g.footer_y - 2 - g.list_top) / 2;
}

static void cv_empty(struct Page *p)
{
    WORD x0 = aside_x(), max_y = g.footer_y - 2;
    (void)p;
    tb_close(&pv_tb);
    gfx_picture_clear(&g, x0, g.list_top, g.w - 1, max_y);
    gfx_fill_bg(&g, x0, g.list_top, g.w - 1, max_y);
}

static void cv_begin(struct Page *p, BOOL keep)
{
    WORD x0 = aside_x(), max_y = g.footer_y - 2;
    (void)p;
    if (keep) {
        if (!det.tj->done)                  /* the picture stays; the text place cleared */
            gfx_fill_bg(&g, x0, pv_box_y + pv_box_h, g.w - 1, max_y);
    } else if (!det.tj->done || (det.sj && !det.sj->done) || !det.sj) {
        /* not all here: nothing of the entry before stays up meanwhile */
        gfx_picture_clear(&g, x0, g.list_top, g.w - 1, max_y);
        gfx_fill_bg(&g, x0, g.list_top, g.w - 1, det.tj->done ? g.list_top + pv_box_h - 1 : max_y);
    }
}

static void cv_no_picture(struct Page *p)
{
    (void)p;
    gfx_picture_clear(&g, aside_x(), g.list_top, g.w - 1, g.footer_y - 2);
}

static void cv_text(struct Page *p)
{
    pv_draw_text(p);
}

static void cv_picture(struct Page *p)
{
    pv_draw_picture();
    pv_spin_on = FALSE;
    if (p->pics)
        pv_draw_text(p);                    /* now with the picture's size */
}

static void cv_tick(struct Page *p)
{
    (void)p;
    pv_spin_tick();                         /* waited 0.25 s already: the spinner now */
}

const struct DetView classic_view = {
    cv_has_pane, cv_start, cv_shots, cv_job, pv_keeps, cv_clear_picture, cv_clear_all, style_preview,
    cv_begin, plex_pv_draw, cv_empty, cv_no_picture, cv_text, cv_picture, cv_tick
};

/* A menu label for the preview: the font shown has no Far East
 * characters, so a Japanese label is shown in English there. */
static const char *spv_label(int mod)
{
    const char *s = _(mod_names[mod]);
    return cjk_any(s, strlen(s)) ? mod_names[mod] : s;
}

static void style_preview(struct Page *p)
{
    WORD x0 = aside_x(), x1 = g.w - 1, y0 = g.list_top, y1 = g.footer_y - 2;
    const struct LacedFont *lf;
    struct TextFont *f;
    struct TextExtent te;
    LONG k, longest = 0, w, rh, mw, ipx, y, i, n;
#define SU(v) ((WORD)(((LONG)(v) * k + 128) >> 8))
    det.page = p;
    det.sel = p->sel;
    det.shown = TRUE;
    det.pending = FALSE;
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
        gfx_text(&g, x0, y0, _("Not in the Fonts drawer"), PEN_DIM, PEN_BG, x1 - x0, TS_BODY);
        return;
    }
    k = ((LONG)lf->ysize << 8) / 12;
    rh = SU(18);
    ipx = 16 * lf->kint;
    SetFont(g.rp, f);
    for (i = 0; i < nmenu; i++)
        if ((w = TextLength(g.rp, (CONST_STRPTR)spv_label(menu[i]),
                            strlen(spv_label(menu[i])))) > longest)
            longest = w;
    mw = SU(23) + longest + SU(6);
    if (mw > x1 - x0 - SU(3))
        mw = x1 - x0 - SU(3);
    for (i = 0, y = y0; i < nmenu && y + rh - 1 <= y1; i++, y += rh) {
        UBYTE bg = i == menu_sel ? PEN_HL : PEN_BG;
        const char *s = spv_label(menu[i]);
        gfx_fill(&g, x0, y, x0 + mw - 1, y + rh - 1, bg);
        gfx_icon_sized(&g, mod_icon(menu[i]), x0 + SU(3), y + (rh - ipx) / 2, PEN_WHITE, ipx);
        SetFont(g.rp, f);
        n = TextFit(g.rp, (CONST_STRPTR)s, strlen(s), &te, NULL, 1, mw - SU(24),
                    f->tf_YSize + 1);
        SetAPen(g.rp, GPEN(PEN_WHITE));
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

/* The letter over the rows (a list with no column beside it). */
static BOOL ov_over(struct Page *p)
{
    return ov.on && ov.bl && ov.page == p && !has_aside(p);
}

static void ov_stamp(void)
{
    UBYTE was;
    if (!ov.bl)
        return;
    was = g.rp->Mask;
    if (g.planar)
        SetWriteMask(g.rp, list_planes(ov.page));
    if (!has_aside(ov.page)) {
        static const BYTE d[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 },
                                      { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
        WORD o = U(1) > 0 ? U(1) : 1;
        int i;
        for (i = 0; i < 8; i++)
            gfx_bigletter_draw(&g, ov.bl, ov.x + d[i][0] * o, ov.y + d[i][1] * o, PEN_BG);
    }
    gfx_bigletter_draw(&g, ov.bl, ov.x, ov.y, PEN_DIM);
    if (g.planar)
        SetWriteMask(g.rp, was);
}

/* While the rows are painted in a batch, the letter waits to be stamped
 * once at its end (draw_list_row stamps it after a row of its own). */
static BOOL ov_hold;

/* The rows between y0 and y1 (screen lines) painted again, then the letter
 * over them, if it shows. */
static void ov_rows(struct Page *p, WORD y0, WORD y1)
{
    WORD vis = list_visible(), r, col = mark_column(p);
    BOOL was = ov_hold;
    ov_hold = TRUE;
    for (r = 0; r < vis && p->top + r < p->list.count; r++) {
        WORD ry = g.list_top + r * g.list_row_h;
        if (ry + g.list_row_h - 1 >= y0 && ry <= y1)
            draw_list_row(p, p->top + r, col);
    }
    ov_hold = was;
    if (!ov_hold && ov_over(p))
        ov_stamp();
}

/* The rows under the letter, outline and all, again (then the letter). */
static void ov_rows_under(struct Page *p)
{
    WORD o = U(1) > 0 ? U(1) : 1;
    if (ov.bl)
        ov_rows(p, ov.y - o, ov.y + ov.bl->h - 1 + o);
}

static void cl_letter_show(struct Page *p)
{
    if (has_aside(p)) {
        gfx_picture_clear(&g, 0, g.list_top, g.w - 1, g.footer_y - 2);  /* the letter now */
        tb_close(&pv_tb);
        gfx_fill_bg(&g, ov.strip_x, g.list_top, g.w - 1, g.footer_y - 2);  /* its column */
    }
    ov_stamp();                             /* (over the rows: they are as they were) */
}

static void cl_letter_relabel(char c)
{
    const struct BigLetter *old = ov.bl;
    WORD ox = ov.x, oy = ov.y, x0, y0, x1, y1, w, h;
    if (!has_aside(ov.page)) {
        /* over the rows: those under the old letter and the new painted
         * again, the new one stamped over them */
        WORD o = U(1) > 0 ? U(1) : 1, top = oy, bot = old ? oy + old->h - 1 : oy;
        if (!ov_build(c)) {
            ov.bl = NULL;
            if (old)
                ov_rows(ov.page, top - o, bot + o);
            return;
        }
        if (!old || ov.y < top)
            top = ov.y;
        if (ov.y + ov.bl->h - 1 > bot)
            bot = ov.y + ov.bl->h - 1;
        ov_rows(ov.page, top - o, bot + o);
        return;
    }
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

static void cl_letter_hide(struct Page *p)
{
    if (has_aside(p)) {
        if (ov.bl)                  /* the column is the letter's alone: clear it */
            gfx_fill(&g, ov.x, ov.y, ov.x + ov.bl->w - 1, ov.y + ov.bl->h - 1, PEN_BG);
    } else if (ov.bl) {
        ov_rows_under(p);           /* the rows it lay over, without it */
    }
}

/* The planes a list's rows are drawn and moved on (a native screen): its
 * pens are below 8, so 0-2 (3/8 of the blitter's work), except where it
 * has a Now playing row, whose orange (pen 8) is on plane 3 too: 0-3,
 * still half an AGA screen's (no list pen is 16 or more). */
static UBYTE list_planes(const struct Page *p)
{
    return p && p->list.count && np_row(&p->list.items[0]) ? 0x0F : 0x07;
}

static void cl_draw_list_row(struct Page *p, LONG idx, WORD col)
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
    BOOL np = np_row(it);
    UBYTE fg = off || (it->flags & ITEM_HEAD) ? PEN_DIM : np ? PEN_ACCENT : sel ? PEN_WHITE : PEN_TEXT;
    UBYTE bg = sel ? (focus == FOCUS_CONTENT ? PEN_HL : PEN_SELDIM) :
               mouse_mode && idx - p->top == hover_row && p == cur() && !(it->flags & ITEM_HEAD)
               ? PEN_RULE : PEN_BG;
    /* Every list pen is below 8 (16 with a Now playing row) and the planes
     * above are clear in the list (the pane is cleared on all planes when
     * the page is drawn), so on native screens only planes 0-2 (0-3) are
     * written: 3/8 of the blitter work (half). */
    if (g.planar)
        SetWriteMask(g.rp, list_planes(p));
    gfx_fill(&g, g.pane_x - U(2), y, xr, y + g.list_row_h - 1, bg);
    if (it->flags & ITEM_GONE) {            /* its letter is being read again */
        if (g.planar)
            SetWriteMask(g.rp, 0xFF);
        return;
    }
    if (it->mark == MARK_CYCLE)
        gfx_cycle_glyph(&g, g.pane_x, y + (g.list_row_h - g.mark_px) / 2, g.mark_px,
                        sel ? PEN_WHITE : PEN_TEXT);
    else if (it->mark == MARK_DISC)
        gfx_disc_glyph(&g, g.pane_x, y + (g.list_row_h - g.mark_px) / 2, g.mark_px, PEN_TEXT,
                       PEN_DIM, PEN_WHITE);
    else if (it->mark != MARK_NONE)
        gfx_mark(&g, it->mark, g.pane_x, y + (g.list_row_h - g.mark_px) / 2,
                 off ? PEN_RULE : sel ? PEN_WHITE : it->mark == MARK_ARROW ? PEN_DIM : PEN_TEXT, bg);
    {
        /* the tag at the right: a setting's value; a picture's format or
         * a playable folder's picture count (pics.c keeps it in sortkey) */
        const char *tag = row_tag(p, it);
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
    if (!ov_hold && ov_over(p) && y + g.list_row_h > ov.y - U(1) - 1 &&
        y <= ov.y + ov.bl->h + U(1))
        ov_stamp();                         /* a row under the letter: the letter again */
}

/* planes: what a native screen writes (draw_gutter: 0-2, its pens being
 * below 8 and the column's planes above clear once the list is drawn;
 * draw_list_rows: all, as it draws the column the first time) */
static void cl_draw_gutter_planes(struct Page *p, UBYTE planes)
{
    WORD bx0, bx1, ty, th, y0 = g.list_top, y1 = g.footer_y - 2;
    LONG vis = list_visible();
    if (!list_overflows(p))
        return;                             /* no column */
    gut_geom(p, &bx0, &bx1, &ty, &th);
    (void)vis;
    if (g.planar)
        SetWriteMask(g.rp, planes);
    gfx_fill(&g, list_edge(p) - GUT_W + 1, y0, list_edge(p), y1, PEN_BG);
    if (ty > y0)
        gfx_fill(&g, bx0, y0, bx1, ty - 1, PEN_RULE);           /* the track, above */
    if (ty + th <= y1)
        gfx_fill(&g, bx0, ty + th, bx1, y1, PEN_RULE);          /* and below */
    gfx_fill(&g, bx0, ty, bx1, ty + th - 1,
             gut.drag ? PEN_WHITE : mouse_mode && bar_up ? PEN_TEXT : PEN_DIM);
    if (g.planar)
        SetWriteMask(g.rp, 0xFF);
}

static void cl_draw_list_rows(struct Page *p)
{
    WORD vis, r, col;
    vis = list_visible();
    col = mark_column(p);
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
        gfx_text(&g, g.pane_x, g.list_top + U(2), _("(empty)"), PEN_DIM, PEN_BG, 0, TS_BODY);
        return;
    }
    {
        BOOL was = ov_hold;
        ov_hold = TRUE;                     /* the letter once, after them all */
        for (r = 0; r < vis && p->top + r < p->list.count; r++)
            draw_list_row(p, p->top + r, col);
        ov_hold = was;
    }
    gfx_fill(&g, g.pane_x - U(2), g.list_top + r * g.list_row_h, list_xr(p), g.footer_y - 2, PEN_BG);
    draw_gutter_planes(p, 0xFF);
    if (!ov_hold && ov_over(p))
        ov_stamp();
}

static WORD rows_right(struct Page *p, LONG top, WORD col)
{
    WORD vis = list_visible(), r, right = g.pane_x;
    for (r = 0; r < vis && top + r < p->list.count; r++) {
        struct Item *it = &p->list.items[top + r];
        WORD e;
        if (row_tag(p, it)[0]) {
            right = g.w - 1;                /* a tag at the right (a count, a value):
                                             * the whole row, or it stays behind */
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
    LONG vis = list_visible();
    if (ov.on && has_aside(cur()) && x1 > ov.strip_x - 1)
        x1 = ov.strip_x - 1;                /* never into the letter's column */
    struct BitMap *bm = g.rp->BitMap;
    /* The copy goes straight to the screen's bitmap, which clips nothing:
     * never outside the list's rows. Rows copied past them once went past
     * the bitmap's end, into chip RAM after it (the display's copper list:
     * a blank screen, after gliding to a list's end and back). */
    if (src < 0 || dst < 0)
        return;
    if (src + n > vis)
        n = vis - src;
    if (dst + n > vis)
        n = vis - dst;
    if (n <= 0 || src == dst || x1 < x0)
        return;
    gfx_dirty();
    LockLayerRom(g.rp->Layer);             /* the window's, or a double buffer's */
    BltBitMap(bm, x0, g.list_top + src * g.list_row_h, bm, x0, g.list_top + dst * g.list_row_h,
              x1 - x0 + 1, n * g.list_row_h, 0xC0, g.planar ? list_planes(cur()) : 0xFF, NULL);
    UnlockLayerRom(g.rp->Layer);
}

static void cl_draw_list_move(struct Page *p, LONG old_sel, LONG old_top)
{
    if (ov_over(p)) {
        /* the letter over the rows: they move without it stamped each
         * time, then those under it are painted again (what was copied
         * there carries the letter, moved), and it over them */
        BOOL was = ov_hold;
        LONG k;
        ov_hold = TRUE;
        draw_list_move2(p, old_sel, old_top);   /* (it moves p->top to follow) */
        ov_hold = was;
        k = p->top - old_top;
        if (k) {
            /* the copy took the letter's pixels k rows along with the rows
             * (up for k > 0, down for k < 0): those rows too, or a sliver
             * of it is left there and carried further each move */
            WORD o = U(1) > 0 ? U(1) : 1, y0 = ov.y - o, y1 = ov.y + ov.bl->h - 1 + o;
            if (k > 0)
                y0 -= (WORD)(k * g.list_row_h);
            else
                y1 += (WORD)(-k * g.list_row_h);
            ov_rows(p, y0, y1);
        } else {
            ov_stamp();
        }
    } else {
        draw_list_move2(p, old_sel, old_top);
    }
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
            /* above it: only as many as land inside the list (the bar on
             * the bottom row, at a list's end: r_old rows would land m - 1
             * rows past it) */
            blit_rows(0, m, r_old < vis - m ? r_old : vis - m);
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

/* The note and the picture under a text page's box. */
/* Plex's logo (art.c's, in its pens), k times larger, top left at x, y:
 * a row's runs of one pen each a fill */
static void draw_plex_logo(WORD x, WORD y)
{
    WORD k = g.kint, i, j, r;
    for (j = 0; j < (WORD)plex_lo_h; j++)
        for (i = 0; i < (WORD)plex_lo_w; i = r) {
            UBYTE pen = plex_lo[j * plex_lo_w + i];
            for (r = i + 1; r < (WORD)plex_lo_w && plex_lo[j * plex_lo_w + r] == pen; r++)
                ;
            if (pen != 255)
                gfx_fill(&g, x + i * k, y + j * k, x + r * k - 1, y + j * k + k - 1, pen);
        }
}

/* Plex in Ambiance: "Works with" Plex's logo, as Plex's guidelines allow (to show
 * that Ambiance works with Plex), and their attribution under it */
static void draw_plexmark(void)
{
    WORD y = g.footer_y - 2 - plexmark_h() + U(6), x = g.pane_x, w;
    const char *with = _("Works with:");
    gfx_text(&g, x, y + (plex_lo_h * g.kint - g.font_h) / 2, with, PEN_DIM, PEN_BG, textbox_w(), TS_BODY);
    x += gfx_textlen(&g, with, TS_BODY) + U(6);
    draw_plex_logo(x, y);
    w = plex_lo_w * g.kint;
    gfx_text(&g, x + w + U(1), y, "*", PEN_DIM, PEN_BG, U(10), TS_BODY);
    gfx_text(&g, g.pane_x, y + plex_lo_h * g.kint + U(3), "* used under license from Plex", PEN_FAINT,
             PEN_BG, textbox_w(), g.sm ? SM_TINY : TS_BODY);   /* (fine print where it can be) */
}

static void draw_text_picture(struct Page *p);  /* the program's: state, then the view (ui) */
static void cl_draw_text_picture(struct Page *p)
{
    WORD lines, lh = g.font_h + U(1), y, i;
    WORD box_h = text_box_h(p, g.list_top, &lines);
    if (p->plexmark)
        draw_plexmark();
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
            for (i = 0; i < l.count; i++) {     /* centred, over the picture */
                WORD tw = gfx_textlen(&g, l.items[i].label, TS_BODY);
                gfx_text(&g, g.pane_x + (tw < textbox_w() ? (textbox_w() - tw) / 2 : 0), y + i * lh,
                         l.items[i].label, PEN_DIM, PEN_BG, textbox_w(), TS_BODY);
            }
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

static void cl_draw_pane(BOOL full)
{
    struct Page *p = cur();
    BOOL took = FALSE;                      /* a cover moved into the pane: its text now too */
    if (!p)
        return;
    if (full) {
        det.shown = FALSE;           /* the pane is cleared: preview again */
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
            WORD ph = NARROW ? U(128) : (g.k == 256 && g.h >= 400) ? 170 : U(85), dh = 0;
            p->facts_y = g.list_top;
            if ((g.pens_shared || g.truecolor) && ags_screenshot_path(p->base, shot, sizeof(shot))) {
                set_msg(_("Loading screenshot..."));
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

static void np_button(int i)
{
    BOOL lit = i == np.flash || i == np.rep;    /* just pressed, or held */
    BOOL ring = np_focus_shown() && i == np.btn;
    BOOL hov = mouse_mode && i == np.hov;       /* the mouse over it */
    UBYTE bg = lit ? PEN_HL : hov ? PEN_SELDIM : PEN_BAR, fg = lit || hov ? PEN_WHITE : PEN_TEXT;
    WORD x = np.bx[i], y = np.by, w = np.bw, h = np.bh;
    BOOL tall = gfx_mode_tall(g.mode);
    WORD sx = tall ? 2 : 1;                     /* pixels across for one down, to look square */
    WORD gh, cx, cy, rx, ry;
    if (i == NB_VDOWN || i == NB_VUP) {
        if ((i == NB_VDOWN && mp_volume() == 0) || (i == NB_VUP && mp_volume() >= 64))
            fg = lit ? PEN_TEXT : PEN_DIM;
    }
    if (i == NB_MUTE && np_muted() && !lit && !hov)
        fg = PEN_ACCENT;                    /* muted: the speaker in orange */
    if ((i == NB_BACK || i == NB_FWD) && !mp_can_step(i == NB_FWD ? 1 : -1)) {
        bg = PEN_BAR;                       /* not for this module: greyed, never lit */
        fg = PEN_RULE;
    }
    if (i == NB_LOOP) {
        if (mp_is_stream()) {
            bg = PEN_BAR;                   /* a station: nothing to loop */
            fg = PEN_RULE;
        }
    }
    /* the glyph (glyph.c): an odd number of rows, about three fifths of
     * the button, so each triangle has a one-row tip */
    gh = glyph_rows(h);
    cx = x + w / 2;
    cy = y + h / 2;
    /* the button: rounded by about a fifth of its height; the focus ring
     * (yellow) is its outer t rows, the face inside it */
    ry = h / 5 > 2 ? h / 5 : 2;
    if (h <= 16)
        ry = 1;                             /* small: its corners' one pixel off, not steps */
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
    {
        static const UBYTE kind[] = { GL_PREV, GL_BACK, GL_PLAY, GL_STOP, GL_FWD, GL_NEXT, GL_LOOP,
                                      GL_MUTE, GL_VDOWN, GL_VUP };
        int k = i == NB_PLAY && mp_playing() ? GL_PAUSE : i <= NB_VUP ? kind[i] : -1;
        UWORD f = (i == NB_LOOP && play_loop) || (i == NB_MUTE && np_muted()) ? GLF_ON : 0;
        if (k >= 0)
            glyph_draw(g.rp, k, cx, cy, gh, sx, GPEN(fg), GPEN(fg == PEN_RULE ? PEN_RULE : PEN_DIM), f);
    }
}

static void np_volume_text(void)
{
    char m[64];
    WORD x = np.vol_x, y = np.by + (np.bh - g.font_h) / 2, end = np.x0 + np.w - 1;
    if (x < 0)
        return;
    if (np.art_s && np.art_y <= np.by + np.bh)
        end = np.art_x - U(8) - 1;          /* the cover beside it */
    if (np_muted())
        amb_strlcpy(m, _("Muted"), sizeof(m));
    else
        sprintf(m, _("Volume %ld%%"), (long)(mp_volume() * 100 / 64));
    gfx_text(&g, x, y, m, PEN_DIM, PEN_BG, 0, TS_SMALL | TS_OPAQUE | TS_TABULAR);
    x += gfx_textlen(&g, m, TS_SMALL | TS_TABULAR);
    if (x <= end)
        gfx_fill(&g, x, y, end, y + g.font_h - 1, PEN_BG);
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

/* The time, the bar and the position: only what changed. Several times a
 * second for a module (its row): on a native screen planes 0-3 only, its
 * pens being below 16 (the bar's orange is 8) and these lines never
 * reaching the cover, half an AGA screen's work. */
static void np_draw_progress(BOOL full, const struct MpStatus *st)
{
    if (g.planar)
        SetWriteMask(g.rp, 0x0F);
    np_draw_progress_(full, st);
    if (g.planar)
        SetWriteMask(g.rp, 0xFF);
}

static void np_draw_progress_(BOOL full, const struct MpStatus *st)
{
    char t[160], a[64], b[24], pos[160];
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
    amb_time_text(a, at);
    amb_time_text(b, st->total_ms);
    if (st->total_ms)
        sprintf(t, "%s / %s", a, b);
    else
        strcpy(t, a);                       /* its length is not known */
    if (st->songlen && st->pattern >= 0)
        sprintf(pos, _("Position %ld of %ld   Pattern %ld   Row %ld"), (long)st->order + 1,
                (long)st->songlen, (long)st->pattern, (long)st->row);
    else if (st->songlen)
        sprintf(pos, _("Position %ld of %ld"), (long)st->order + 1, (long)st->songlen);
    else if (st->pattern >= 0 && mp_running())
        sprintf(pos, _("Position %ld   Pattern %ld   Row %ld"), (long)st->order + 1,
                (long)st->pattern, (long)st->row);     /* mixed: its length in positions unknown */
    else
        pos[0] = 0;
    if (mp_is_stream() && mp_running()) {
        struct StInfo in;
        st_info(&in);
        if (in.state == ST_PLAYING) {
            sprintf(pos, _("Buffered %ld.%lds"), (long)(in.buffer_ms / 1000),
                    (long)(in.buffer_ms % 1000 / 100));
            if (in.underruns) {
                sprintf(a, _("   dropouts %ld"), (long)in.underruns);
                amb_strlcat(pos, a, sizeof(pos));
            }
        } else {
            amb_strlcpy(pos, in.state == ST_CONNECTING ? _("Connecting...") :
                              in.state == ST_BUFFERING ? _("Buffering...") : "", sizeof(pos));
        }
    }
    /* the time right of the bar, right-aligned */
    lw = gfx_textlen(&g, t, TS_BODY | TS_TABULAR);  /* (its left edge still as digits change) */
    if (x + bw + U(4) <= end - lw)
        gfx_fill(&g, x + bw, y, end - lw, y + g.font_h - 1, PEN_BG);
    gfx_text(&g, end - lw + 1, y, t, np.seek_due ? PEN_WHITE : PEN_TEXT, PEN_BG, 0,
             TS_BODY | TS_OPAQUE | TS_TABULAR);
    /* the position (or the buffer) on its own line under the buttons; left
     * of the cover where it comes down beside the controls */
    if (np.art_s && np.art_y <= np.pos_y)
        end = np.art_x - U(8) - 1;
    rw = gfx_textlen(&g, pos, TS_SMALL | TS_TABULAR);     /* (Pattern and Row stay put) */
    if (rw > end - x + 1)
        rw = end - x + 1;
    if (rw)
        gfx_text(&g, x, np.pos_y, pos, PEN_DIM, PEN_BG, rw, TS_SMALL | TS_OPAQUE | TS_TABULAR);
    if (x + rw <= end)
        gfx_fill(&g, x + rw, np.pos_y, end, np.pos_y + g.font_h - 1, PEN_BG);
}

/* The visualization's cycle gadget, as Intuition's: its glyph, and the one
 * chosen, at the top right of the meters' place (the end of the line
 * above it), drawn as the buttons are. A click, or V, goes to the next.
 * On lowres the glyph alone. */
static void np_cycle_layout(void)
{
    WORD box = g.font_h, right = np.x0 + np.w - 1, w, nw = 0;
    int i;
    np.cyc_label = g.wb ? DW > 320 : g.w > 320;
    for (i = 0; np.cyc_label && i < VIS_COUNT; i++) {
        WORD t = gfx_textlen(&g, _(vis_name(i)), TS_SMALL);
        if (t > nw)
            nw = t;
    }
    w = U(3) + box + U(3) + (np.cyc_label ? U(3) + 1 + U(4) + nw + U(4) : 0);
    np.cyc_x1 = right;
    np.cyc_x0 = right - w + 1;
    np.cyc_y0 = np.info_y - U(1);
    np.cyc_y1 = np.info_y + g.font_h;
}

/* Another visualization chosen: the same place, the other one in it; that
 * and the gadget drawn, not the page. */
static void np_visual_changed(void)
{
    if (np_on() && np.w && np.cyc_x1 > np.cyc_x0) {
        BOOL was = gfx_quiet_begin();
        np_draw_cycle();
        gfx_quiet_end(&g, was, np.cyc_x0, np.cyc_y0, np.cyc_x1, np.cyc_y1);
        cl_np_update();
    } else if (np_on()) {
        np_draw(TRUE);
    }
}

/* The meters' place: the visualization chosen (vis.c), the level meters
 * where the engine cannot give what it needs. */
static void np_draw_meters(BOOL full, const struct MpStatus *st)
{
    struct VisCtx c;
    c.g = &g;
    c.x = np.x0;
    c.y = np.meter_y;
    c.w = np.w;
    c.row_h = np.meter_h;
    c.rows = np.rows;
    c.gap = U(2);
    c.label_w = np.name_w;
    vis_draw(&c, st, full);
}

/* A station's page: its name; what plays now; what it is and how it is
 * played (in the place of a module's samples). */
/* A line of the page drawn over what was there (opaque, the rest of the
 * line filled): drawn again unchanged, nothing on screen changes. */
static void np_line(WORD y, const char *s, UBYTE fg, int ts)
{
    WORD x1 = np.x0 + np.w, w = gfx_textlen(&g, s, ts);
    if (y == np.info_y && np.cyc_x0 > np.x0)
        x1 = np.cyc_x0 - U(6);              /* the cycle gadget at its end */
    if (w > x1 - np.x0)
        w = x1 - np.x0;
    gfx_text(&g, np.x0, y, s, fg, PEN_BG, x1 - np.x0, ts | TS_OPAQUE);
    if (np.x0 + w < x1)
        gfx_fill(&g, np.x0 + w, y, x1 - 1, y + g.font_h - 1, PEN_BG);
}

/* label, then value at the column after the longest label (Playback). */
static void np_pair(WORD y, const char *label, const char *value)
{
    WORD col = gfx_textlen(&g, _("Playback"), TS_SMALL) + U(8), w;
    WORD tw = np.art_s ? np.w - np.art_s - U(8) : np.w, x1 = np.x0 + tw;
    gfx_text(&g, np.x0, y, label, PEN_DIM, PEN_BG, col, TS_SMALL | TS_OPAQUE);
    w = gfx_textlen(&g, label, TS_SMALL);
    if (w < col)
        gfx_fill(&g, np.x0 + w, y, np.x0 + col - 1, y + g.font_h - 1, PEN_BG);
    w = gfx_textlen(&g, value, TS_SMALL | TS_TABULAR);    /* (a bit rate that changes: nothing moves) */
    if (w > tw - col)
        w = tw - col;
    gfx_text(&g, np.x0 + col, y, value, PEN_TEXT, PEN_BG, tw - col, TS_SMALL | TS_OPAQUE | TS_TABULAR);
    /* cleared to the cover's edge, or the line's end where the cover is
     * not beside it: the line drawn before the cover came (a new album's,
     * the page not laid out again) went that far, and its end stayed in
     * the gap before the cover */
    if (np.art_s && y < np.art_y + np.art_h && y + g.font_h > np.art_y)
        x1 = np.art_x;
    else
        x1 = np.x0 + np.w;
    if (np.x0 + col + w < x1)
        gfx_fill(&g, np.x0 + col + w, y, x1 - 1, y + g.font_h - 1, PEN_BG);
}

/* "MP3 320kbps, 44100Hz stereo", what of it is known. */
static void np_audio_line(WORD y, const char *label, const char *codec, ULONG kbps, ULONG rate,
                          ULONG ch)
{
    char v[96], part[32], up[16];
    int i;
    {
        /* Plex's codec names (audioCodec) as people know them */
        static const char *const names[][2] = {
            { "wmalossless", "WMA Lossless" }, { "wmapro", "WMA Pro" }, { "wmav2", "WMA" },
            { "wma", "WMA" }, { "alac", "Apple Lossless" }, { "aac", "AAC" },
            { "vorbis", "Ogg Vorbis" }, { "opus", "Opus" }, { "ac3", "AC-3" },
            { "eac3", "E-AC-3" }, { "dca", "DTS" }, { "pcm", "PCM" }, { "aiff", "AIFF" },
            { "ape", "Monkey's Audio" }, { "wavpack", "WavPack" }, { NULL, NULL } };
        up[0] = 0;
        for (i = 0; names[i][0]; i++)
            if (!amb_stricmp(codec, names[i][0]))
                amb_strlcpy(up, names[i][1], sizeof(up));
        if (!up[0]) {
            for (i = 0; codec[i] && i < 15; i++)
                up[i] = codec[i] >= 'a' && codec[i] <= 'z' ? codec[i] - 32 : codec[i];
            up[i] = 0;
        }
    }
    amb_strlcpy(v, up, sizeof(v));
    if (kbps) {
        sprintf(part, "%s%lukbps", v[0] ? " " : "", (unsigned long)kbps);
        amb_strlcat(v, part, sizeof(v));
    }
    if (rate) {
        char k[16];
        sprintf(part, "%s%s", v[0] ? ", " : "", fmt_khz(rate, k));
        amb_strlcat(v, part, sizeof(v));
    }
    if (ch) {
        sprintf(part, " %s", ch == 1 ? "mono" : ch == 2 ? "stereo" : "multichannel");
        amb_strlcat(v, part, sizeof(v));
    }
    np_pair(y, label, v);
}

/* How it plays: through Paula (8-bit), AHI (16-bit) or MHI; its rate and
 * channels only where they are not the source's. */
static void np_playback_line(WORD y, const struct StInfo *in)
{
    char v[64];
    amb_strlcpy(v, in->mhi ? _("MHI (decoded and played by it)") : in->ahi ? "AHI 16-bit"
                                                                          : "Paula 8-bit",
                 sizeof(v));
    if (in->dec_freq && (in->dec_freq != in->freq || in->dec_channels != in->channels)) {
        char k[16];
        sprintf(v + strlen(v), ", %s %s", fmt_khz(in->dec_freq, k),
                in->dec_channels == 1 ? "mono" : "stereo");
    }
    np_pair(y, _("Playback"), v);
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
        np_line(np.title_y, mp_stream_name()[0] ? mp_stream_name() : _("(no title)"), PEN_WHITE,
                TS_BOLD);
        amb_strlcpy(info, mp_track_by(), sizeof(info));
        if (np.list.pool && np.idx >= 0 && np.list.count > 1) {
            sprintf(part, _("   Track %ld of %ld"), (long)np.idx + 1, (long)np.list.count);
            amb_strlcat(info, part, sizeof(info));
        }
        np_line(np.info_y, info, PEN_DIM, TS_SMALL);
        if (y + lh * 3 > bottom)
            return;
        np_pair(y, _("File"), FilePart((STRPTR)mp_path()));
        y += lh;
        if (!kbps && len >= 1000 && st_size())
            kbps = st_size() / (len / 8);   /* FLAC: bytes / (ms / 8) = kbit/s */
        if (!mp_running())
            np_pair(y, _("Format"), st_state() == ST_FAILED && st_error()[0] ? st_error() : _("Stopped"));
        else if (!in.freq)
            np_pair(y, _("Format"), _("Reading..."));
        else
            np_audio_line(y, _("Format"), mp_tag(), kbps, in.freq, in.channels);
        y += lh;
        if (mp_running() && in.dec_freq)
            np_playback_line(y, &in);
        else
            np_pair(y, _("Playback"), "");
        y += lh;
        if (y <= bottom)                    /* the picture's square left alone */
            gfx_fill(&g, np.x0, y, np.x0 + (np.art_s ? np.w - np.art_s - U(8) : np.w) - 1, bottom,
                     PEN_BG);
        return;
    }
    if (mp_is_track()) {
        /* a Plex track: its title, artist and album, where in the list, how it plays */
        np_line(np.title_y, mp_stream_name()[0] ? mp_stream_name() : _("(no title)"), PEN_WHITE,
                TS_BOLD);
        amb_strlcpy(info, mp_track_by(), sizeof(info));
        if (np.list.pool && np.idx >= 0 && np.list.count > 1) {
            /* the album is named already: only where in it */
            sprintf(part, _("   Track %ld of %ld"), (long)np.idx + 1, (long)np.list.count);
            amb_strlcat(info, part, sizeof(info));
        }
        np_line(np.info_y, info, PEN_DIM, TS_SMALL);
        if (y + lh * 3 > bottom)
            return;
        /* the file as stored, as the server sends it, as the Amiga plays it */
        np_audio_line(y, _("Original"), plex_orig.codec, plex_orig.kbps, plex_orig.rate,
                      plex_orig.ch);
        y += lh;
        if (!mp_running())
            np_pair(y, "Plex", st_state() == ST_FAILED && st_error()[0] ? st_error() : _("Stopped"));
        else if (in.state == ST_CONNECTING)
            np_pair(y, "Plex", _("Connecting..."));
        else if (in.state == ST_BUFFERING)
            np_pair(y, "Plex", _("Buffering..."));
        else if (!plex_orig.conv)
            np_pair(y, "Plex", _("sends the file as stored"));
        else
            np_audio_line(y, "Plex", "MP3", in.avg_kbps, in.freq, in.channels);
        y += lh;
        if (mp_running() && in.dec_freq)
            np_playback_line(y, &in);
        else
            np_pair(y, _("Playback"), "");
        y += lh;
        if (y <= bottom)                    /* the cover's square left alone */
            gfx_fill(&g, np.x0, y, np.x0 + (np.art_s ? np.w - np.art_s - U(8) : np.w) - 1, bottom,
                     PEN_BG);
        return;
    }
    np_line(np.title_y, mp_stream_name()[0] ? mp_stream_name() : _("Radio"), PEN_WHITE, TS_BOLD);
    sprintf(info, _("Radio   %s"), mp_tag());
    if (st_secure()) {
        /* the stream's connection, whatever the address asked for said */
        if (info[strlen(info) - 1] != ' ')
            amb_strlcat(info, ",", sizeof(info));
        amb_strlcat(info, " HTTPS", sizeof(info));
    }
    if (np.list.pool && np.idx >= 0 && np.list.count > 1) {
        sprintf(part, _("   %ld of %ld in "), (long)np.idx + 1, (long)np.list.count);
        amb_strlcat(info, part, sizeof(info));
        amb_strlcat(info, np.folder, sizeof(info));
    }
    np_line(np.info_y, info, PEN_DIM, TS_SMALL);
    if (y + lh * 5 > bottom)
        return;
    np_line(y, _("On air"), PEN_DIM, TS_BOLD);
    y += lh;
    if (!mp_running())
        now = st_state() == ST_FAILED && st_error()[0] ? st_error() : _("Stopped");
    else if (in.state == ST_CONNECTING)
        now = _("Connecting...");
    else if (in.state == ST_BUFFERING)
        now = _("Buffering...");
    else if (!now[0])
        now = _("(the station does not say)");
    np_line(y, now, PEN_TEXT, TS_SMALL);
    gfx_fill(&g, np.x0, y + g.font_h, np.x0 + np.w - 1, y + lh * 2 - 1, PEN_BG);
    y += lh * 2;
    if (in.name[0] || in.genre[0]) {
        sprintf(part, "%s%s%s", in.name, in.name[0] && in.genre[0] ? "   " : "", in.genre);
        np_line(y, part, PEN_DIM, TS_SMALL);
        y += lh;
    }
    if (in.dec_freq && y + lh <= bottom) {
        char k[16];
        sprintf(part, _("MP3 %ldkbps, %s %s"), (long)in.bitrate, fmt_khz(in.freq, k),
                in.channels == 2 ? "stereo" : "mono");
        if (in.dec_freq != in.freq || in.dec_channels != in.channels)
            sprintf(part + strlen(part), _("; played at %s %s"), fmt_khz(in.dec_freq, k),
                    in.dec_channels == 2 ? "stereo" : "mono");
        amb_strlcat(part, in.mhi ? _(", by MHI") : in.ahi ? ", AHI" : "", sizeof(part));
        np_line(y, part, PEN_DIM, TS_SMALL);
        y += lh;
    }
    if (y <= bottom)
        gfx_fill(&g, np.x0, y, np.x0 + np.w - 1, bottom, PEN_BG);   /* lines there before */
}

static void cl_np_draw(BOOL full)
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
        np_cycle_layout();
        plex_shown[0] = 0;                  /* the pane's details: gone under this page */
        if (mp_is_track() && np.art_s && pic_take(np_art_file, np.art_x, np.art_y, np.art_s,
                                                  np.art_h)) {
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
            gfx_text(&g, np.x0, np.title_y, _("Nothing playing"), PEN_DIM, PEN_BG, np.w, TS_BOLD);
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
            char part[160];
            mp_subsongs(&sub, &subs);
            sprintf(info, "%s   %s", FilePart((STRPTR)mp_path()), mp_tag());
            if (mp_author()[0]) {
                sprintf(part, _("   by %s"), mp_author());
                amb_strlcat(info, part, sizeof(info));
            }
            if (subs > 1) {
                sprintf(part, _("   subsong %ld of %ld"), (long)sub, (long)subs);
                amb_strlcat(info, part, sizeof(info));
            }
            if (np.list.pool && np.idx >= 0 && np.list.count > 1) {
                sprintf(part, _("   %ld of %ld in "), (long)np.idx + 1, (long)np.list.count);
                amb_strlcat(info, part, sizeof(info));
                amb_strlcat(info, np.folder, sizeof(info));
            }
        }
        gfx_text(&g, np.x0, np.info_y, info, PEN_DIM, PEN_BG,
                 np.cyc_x0 > np.x0 ? np.cyc_x0 - U(6) - np.x0 : np.w, TS_SMALL);
        /* the sample names, in two columns where they fit */
        {
            WORD lh = g.font_h + U(1), cols = np.w >= U(300) ? 2 : 1, cw = np.w / cols;
            WORD rows = (g.footer_y - 2 - np.samples_y - lh) / lh, n = 0, k;
            if (rows > 0 && mp_samples() > 0) {
                gfx_text(&g, np.x0, np.samples_y, _("Samples"), PEN_DIM, PEN_BG, 0, TS_BOLD);
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
    if (full)
        np_draw_cycle();
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
                ((i == NB_VDOWN || i == NB_VUP || i == NB_MUTE) && mp_volume() != np.s_vol) ||
                ((i == NB_BACK || i == NB_FWD) && steps != np.s_steps) ||
                (i == NB_LOOP && play_loop != np.s_loop))
                np_button(i);
        if (full || mp_volume() != np.s_vol || hot != np.s_slhot)
            np_slider();
        if (full || mp_volume() != np.s_vol)
            np_volume_text();
        np.s_state = state;
        np.s_steps = steps;
        np.s_loop = play_loop;
        np.s_btn = np.btn;
        np.s_hov = np.hov;
        np.s_flash = np.flash;
        np.s_rep = np.rep;
        np.s_focus = foc;
        np.s_slhot = hot;
        np.s_vol = mp_volume();
    }
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
        gfx_mark(&g, MARK_PLAYING, g.pane_x, y,
                 sel ? PEN_WHITE : PEN_TEXT, bg);
        gfx_quiet_end(&g, was, g.pane_x, y, g.pane_x + g.mark_px - 1, y + g.mark_px - 1);
    }
}

static void cl_eq_tick(void)
{
    static ULONG last;
    BOOL still = !mp_playing();
    ULONG now = time_ms();
    if (still != gfx_eq_still) {
        gfx_eq_still = still;
        np_eq_redraw();
        hdr_np_draw(TRUE);
    } else if (!still && now - last >= 120) {
        int k;
        last = now;
        gfx_eq_frame++;
        /* as the visualization shown moves (not while it is paused for
         * the CPU's sake: the dance then) */
        k = vis_paused() ? -1 : vis_marker(gfx_eq_lev);
        gfx_eq_kind = (UBYTE)(k == VIS_SPECTRUM ? 1 : k == VIS_METERS ? 2 : 0);
        np_eq_redraw();
        hdr_np_draw(TRUE);
    } else if (mp_loaded() != hdr_np_on) {
        hdr_np_draw(TRUE);                  /* started or stopped */
    }
}

static void cl_draw_all(void)
{
    if (foot_hidden && (!g.wb || g.footer_y != g.h)) {
        foot_hidden = foot_over = FALSE;    /* opened again (its metrics anew): shown */
        foot_since = mouse_mode ? time_ms() : 0;
    }
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

/* The pane while the selection is moving: blank, with the section's name. */
static void cl_pane_preview(BOOL first)
{
    const char *t = _(mod_names[menu[menu_sel]]);
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

/* The cover in its square, when it is made. */
static void np_art_draw(void)
{
    WORD dh = 0;
    BOOL taken = FALSE;
    if (!np_aj || !np_aj->done || !np_on() || !mp_is_track() || !np.art_s || np_art_drawn)
        return;
    if (np_aj->thumb &&
        gfx_thumb(&g, np_aj->thumb, &np_aj->info, np.art_x, np.art_y, np.art_s, np.art_h, &dh)) {
        /* the square around a cover not square (the one before may show there) */
        if (g.pic_x > np.art_x)
            gfx_fill_bg(&g, np.art_x, np.art_y, g.pic_x - 1, np.art_y + np.art_h - 1);
        if (g.pic_x + g.pic_w < np.art_x + np.art_s)
            gfx_fill_bg(&g, g.pic_x + g.pic_w, np.art_y, np.art_x + np.art_s - 1,
                        np.art_y + np.art_h - 1);
        if (dh < np.art_h)
            gfx_fill_bg(&g, g.pic_x, np.art_y + dh, g.pic_x + g.pic_w - 1, np.art_y + np.art_h - 1);
    }
    else if (np_aj->obj && gfx_picture_obj(&g, np_aj->obj, np_aj->remapped, np.art_x, np.art_y,
                                           np.art_s, np.art_h, TRUE, &dh, &taken) && taken)
        np_aj->obj = NULL;                  /* the screen has it (its pens) */
    if (g.pic_w) {
        /* what is on screen, and where: the album's pane takes it back */
        amb_strlcpy(pv_onscr.key, np_aj->path, sizeof(pv_onscr.key));
        pv_onscr.x = np.art_x;
        pv_onscr.y = np.art_y;
        pv_onscr.w = np.art_s;
        pv_onscr.h = np.art_h;
    }
    np_art_drawn = TRUE;
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

static void plex_sum_find(const struct Item *it, char *out, LONG max)
{
    char k[120];
    int i;
    if (!sum_key(it, k, sizeof(k)))
        return;
    for (i = 0; i < SUM_KEPT; i++)
        if (sum_kept[i].key[0] && !strcmp(sum_kept[i].key, k)) {
            amb_strlcpy(out, sum_kept[i].text, max);
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
    amb_strlcpy(j->host, plex.host, sizeof(j->host));
    j->port = plex.port;
    plex_request(k, j->path, sizeof(j->path));
    amb_strlcpy(sum_want, k, sizeof(sum_want));
    sum_y = y;
    plex_sj = j;
    net_submit(j);
    if (j->done && !j->busy)
        radio_done(j);
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
    amb_strlcpy(pv_photo_file[pv_photo_next], file, sizeof(pv_photo_file[0]));
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
    det.has_box = FALSE;
    det.text_shown = TRUE;
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
        det.has_box = TRUE;
        det.pic_shown = TRUE;
        pv_box_y = g.list_top;
        pv_box_h = s;
        if (!strcmp(key, last))
            return;                         /* the same album's: nothing changes */
        gfx_fill_bg(&g, x0, g.list_top + s, g.w - 1, max_y);
    } else {
        gfx_picture_clear(&g, x0, g.list_top, g.w - 1, max_y);
        gfx_fill_bg(&g, x0, g.list_top, g.w - 1, max_y);
    }
    amb_strlcpy(last, key, sizeof(plex_shown));
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
        det.has_box = TRUE;
        y = pv_box_y + pv_box_h + U(4);
        if (!keep && art_fetch(ART_PV, thumb, s, s, file)) {
            pv_colour_cap = plex_colours();
            det.sj = det_request(LJ_SHOT, SHOT_FILE, file, FALSE, TRUE);
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
        amb_strlcat(line, year, sizeof(line));
    if (count[0]) {
        amb_strlcat(line, line[0] ? "   " : "", sizeof(line));
        amb_strlcat(line, count, sizeof(line));
    }
    if (studio[0]) {
        amb_strlcat(line, line[0] ? "   " : "", sizeof(line));
        amb_strlcat(line, studio, sizeof(line));
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
    if (det.sj && det.sj->done)
        det_progress(p);
}

/* the classic's turn of the loop: the preview's spinner, a Radio list's */
static void cl_tick(void)
{
    pv_spin_tick();
    radio_spin();
}

static void draw_text_picture(struct Page *p)
{
    ui->text_picture(p);
}

/* ---- the classic as the view driven (struct UiView) */
/* A rounded frame t thick just inside x,y w x h (or, solid, the corners
 * the rounding takes off the button). */
static void np_rrect(WORD x, WORD y, WORD w, WORD h, WORD rx, WORD ry, UBYTE pen)
{
    gfx_rrect(&g, x, y, w, h, rx, ry, pen);
}

void np_layout(void)
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
    np.rows = np_meter_rows();
    np.time_y = np.meter_y + np.rows * (np.meter_h + U(2)) + U(4);
    np.prog_h = U(4) > 2 ? U(4) : 2;
    np.prog_y = np.time_y + (g.font_h - np.prog_h) / 2;
    np.bar_w = np.w - gfx_textlen(&g, "88:88 / 88:88", TS_BODY) - U(10);
    np.by = np.time_y + row + U(4);
    np.bh = g.font_h + U(8);
    vt = gfx_textlen(&g, _("Volume 100%"), TS_SMALL | TS_TABULAR) + U(6);
    avail = np.w - vt;                      /* buttons, gaps, and two more before volume */
    np.bw = (avail - (NB_COUNT + 1) * gap) / NB_COUNT;
    np.vol_x = 0;
    if (np.bw < U(14)) {                    /* narrow: the buttons take the row */
        np.bw = (np.w - (NB_COUNT + 1) * gap) / NB_COUNT;
        np.vol_x = -1;
    }
    if (np.bw > U(26))
        np.bw = U(26);
    /* no taller than wide (lowres's narrow buttons; hires's pixels are
     * twice as tall: half the rows), but the icon's room */
    if (!gfx_mode_tall(g.mode) && np.bh > np.bw)
        np.bh = np.bw > g.font_h + U(3) ? np.bw : g.font_h + U(3);
    else if (gfx_mode_tall(g.mode) && np.bh * 2 > np.bw)
        np.bh = np.bw / 2 > g.font_h + U(2) ? np.bw / 2 : g.font_h + U(2);
    for (i = 0; i <= NB_VDOWN; i++)
        np.bx[i] = np.x0 + i * (np.bw + gap) + (i >= NB_MUTE ? gap * 2 : 0);
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
    np.art_s = np.art_h = 0;
    if (mp_is_track() && (np_local() ? np_local_art : np_art_want) && pv_pictures()) {
        /* the cover, a square to the eye at the right: below the controls,
         * beside the track's lines; or, where the controls leave more room
         * at their right, from their row down. Its width s, its rows s
         * (half on hires, whose pixels are twice as tall) */
        WORD per = gfx_mode_tall(g.mode) ? 2 : 1;
        WORD cap = plex_tier() == 0 ? 160 : plex_tier() == 1 ? 224 : 400;
        /* (as if the tips bar showed: a window's hides and comes back,
         * and the cover would grow and shrink, and move, with it) */
        WORD fy = foot_hidden ? g.h - U(12) : g.footer_y;
        WORD sa = (fy - 2 - np.samples_y - U(2)) * per, sb, right, y = np.samples_y;
        if (sa > np.w * 2 / 5)
            sa = np.w * 2 / 5;
        right = (np.vol_x >= 0 ? np.vol_x + vt : np.bx[NB_VUP] + np.bw) + U(10);
        sb = np.x0 + np.w - right;
        if (sb > (fy - 2 - np.by - U(2)) * per)
            sb = (fy - 2 - np.by - U(2)) * per;
        if (sb > sa) {
            sa = sb;
            y = np.by;
        }
        if (sa > cap)
            sa = cap;                       /* less to decode on a slower Amiga */
        if (sa >= U(40)) {
            np.art_s = sa;
            np.art_h = sa / per;
            np.art_x = np.x0 + np.w - sa;
            np.art_y = y;
        }
    }
}

static void np_draw_cycle(void)
{
    WORD x0 = np.cyc_x0, x1 = np.cyc_x1, y0 = np.cyc_y0, y1 = np.cyc_y1, box = g.font_h;
    WORD ry = gfx_mode_tall(g.mode) ? 1 : 2;
    UBYTE fg, bg;
    BOOL hov;
    if (x1 - x0 < box)
        return;
    /* greyed where this music allows one only (the spectrum under MHI):
     * its name then the one drawn */
    np.cyc_off = !vis_available(VIS_SPECTRUM);
    hov = mouse_mode && np.hov == NB_CYCLE && !np.cyc_off;
    fg = np.cyc_off ? PEN_RULE : hov ? PEN_WHITE : PEN_TEXT;
    bg = hov ? PEN_SELDIM : PEN_BAR;        /* the mouse over it, as over a button */
    /* a button's face (np_button): rounded, the bar's colour, its glyph
     * and words in the buttons' */
    gfx_fill(&g, x0, y0, x1, y1, PEN_BG);
    np_rrect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, 2, ry, bg);
    gfx_cycle_glyph(&g, x0 + U(3), y0 + (y1 - y0 + 1 - box) / 2, box, fg);
    if (np.cyc_label) {
        WORD sx = x0 + U(3) + box + U(3);
        gfx_fill(&g, sx, y0 + U(2), sx, y1 - U(2), PEN_RULE);
        gfx_text(&g, sx + 1 + U(4), np.info_y,
                 _(vis_name(np.cyc_off ? vis_active() : vis_selected())), fg, bg,
                 x1 - U(3) - (sx + 1 + U(4)), TS_SMALL);
    }
}

/* Brought up to date (the timer tick): drawn into the hidden buffer and
 * copied up, no flip. */
static void cl_np_update(void)
{
    BOOL was;
    if (np.w && mp_loaded() && (np_meter_rows() != np.rows ||
                                (np.cyc_x1 > np.cyc_x0 && np.cyc_off == vis_available(VIS_SPECTRUM)))) {
        /* the sound has more or fewer channels than the page was laid out
         * for (a station found to be mono): laid out again, as tall as needed */
        np_draw(TRUE);
        return;
    }
    was = gfx_quiet_begin();
    np_draw(FALSE);
    if (np.art_s && np.art_y <= np.pos_y + g.font_h) {
        /* the cover beside the controls: what is above it, then left of it */
        gfx_quiet_end(&g, was, g.pane_x - U(2), np.meter_y, g.w - 1, np.art_y - 1);
        gfx_quiet_end(&g, was, g.pane_x - U(2), np.art_y, np.art_x - 1, np.pos_y + g.font_h);
    } else {
        gfx_quiet_end(&g, was, g.pane_x - U(2), np.meter_y, g.w - 1, np.pos_y + g.font_h);
    }
}

/* The menu column sliding between its two widths, where drawing it is
 * quick (a whole menu in MENU_SLIDE_MAX ms or less: a fast Amiga, or a
 * modest screen): MENU_SLIDE ms, easing out, a frame at a time. The
 * content beside it moves with it (ScrollRaster: the card's blitter on
 * RTG), so opening pushes the page right and closing pulls it back; then
 * the caller lays everything out at the new width as before. Double
 * buffered, each frame is shown with a flip; else at the vertical blank.
 * On a native (planar) screen moving the page is too much for the blitter
 * (320 x 256 in 256 colours: 80 ms a frame), so there the column slides
 * over the page, or back off it, which then moves once at the end.
 * A frame that takes over MENU_SLIDE_SLOW ms ends the slide there. */
#define MENU_SLIDE      160
#define MENU_SLIDE_MAX   16
#define MENU_SLIDE_SLOW  40
static void cl_menu_slide(UWORD from, UWORD to)
{
    ULONG t0 = time_ms(), t, tf;
    UWORD keep = g.menu_w, prev = from;
    WORD y0 = U(25), y1 = g.footer_y - 2;
    int frames = 0;
    if (!t0 || menu_draw_ms > MENU_SLIDE_MAX || from == to)
        return;
    SetBPen(g.rp, GPEN(PEN_BG));
    for (;;) {
        LONG f;
        UWORD w;
        t = time_ms() - t0;
        if (t >= MENU_SLIDE)
            break;
        f = 1024 - (LONG)(t * 1024 / MENU_SLIDE);
        f = 1024 - f * f / 1024;            /* ease out: fast first, settling */
        w = (UWORD)((LONG)from + ((LONG)to - (LONG)from) * f / 1024);
        if (w == prev) {
            WaitTOF();
            continue;
        }
        tf = time_ms();
        SetBPen(g.rp, GPEN(PEN_BG));
        if (g.planar) {
            if (w < prev)                   /* the column's old edge, now page margin */
                gfx_fill(&g, w + U(3), y0, prev + U(3), y1, PEN_BG);
        } else if (w > prev)                /* opening: the page pushed right */
            ScrollRaster(g.rp, -(WORD)(w - prev), 0, prev + U(3), y0, g.w - 1, y1);
        else                                /* closing: pulled back left */
            ScrollRaster(g.rp, (WORD)(prev - w), 0, w + U(3), y0, g.w - 1, y1);
        g.menu_w = w;                       /* the column alone: the layout keeps its own */
        draw_menu();
        gfx_dirty();
        prev = w;
        frames++;
        if (!gfx_present(&g))
            WaitTOF();
        if (time_ms() - tf > MENU_SLIDE_SLOW)
            break;                          /* too slow here: straight to the end */
    }
    g.menu_w = keep;
    {
        char m[80];
        sprintf(m, "%s in %ld frames, %ld ms (menu drawn in %ld ms)", to > from ? "opened" : "closed",
                (long)frames, (long)(time_ms() - t0), (long)menu_draw_ms);
        amb_log("menu slide", m);
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

/* The picture on screen is file's: moved into the box x, y, w x h (centred
 * across, at its top), as it would be drawn there, and counted as there.
 * A cover going from an album's pane to Now playing and back: copied
 * across, not asked for, decoded and remapped again. */
BOOL pic_take(const char *file, WORD x, WORD y, WORD w, WORD h)
{
    if (!file[0] || !g.pic_w || strcmp(file, pv_onscr.key) || g.pic_w > w || g.pic_h > h)
        return FALSE;
    if (!gfx_picture_move(&g, x + (w - g.pic_w) / 2, y))
        return FALSE;
    if (pv_onscr.x != x || pv_onscr.y != y)
        amb_log("preview", "the cover on screen moved across, not read again");
    pv_onscr.x = x;
    pv_onscr.y = y;
    pv_onscr.w = w;
    pv_onscr.h = h;
    return TRUE;
}

/* ---- step 3c: what program logic drew itself until now */

/* A page left: a picture page's picture taken off behind the beam (the
 * logo gone while its pens are its own: left, it would show in the next
 * page's), and the pane's picture cleared, unless the page under shows it
 * again (its folder's): draw_pane or the preview clears it if not. */
static void cl_page_leave(struct Page *p, struct Page *under)
{
    if (p->pic_bm && p->pic_y >= 0) {
        WaitBlit();
        gfx_clear_beam(&g, p->pic_x, p->pic_y, p->pic_x + p->pic_w - 1, p->pic_y + p->pic_h - 1);
    }
    if (!(under && g.pic_w && !p->pic_bm && has_aside(under) && g.pic_x >= aside_x()))
        gfx_picture_clear(&g, 0, 0, g.w - 1, g.h - 1);
}

/* The average bitrate moved: the station's lines again. */
static void cl_np_rate(void)
{
    BOOL was = gfx_quiet_begin();
    np_draw_station();
    gfx_quiet_end(&g, was, np.x0, np.title_y, np.x0 + np.w - 1, g.footer_y - 2);
}

/* The mouse came onto the cycle gadget or left it: above the part
 * np_update draws, so on its own. */
static void cl_np_cycle(void)
{
    BOOL was = gfx_quiet_begin();
    np_draw_cycle();
    gfx_quiet_end(&g, was, np.cyc_x0, np.cyc_y0, np.cyc_x1, np.cyc_y1);
}

/* Now playing's cover square laid out again (np_layout) for a new track:
 * none, the last one goes; the gap before it cleared (lines drawn wider
 * before). */
static void cl_np_art_room(BOOL none)
{
    if (!np_on() || !np.art_s)
        return;
    if (none && g.pic_w)
        gfx_picture_clear(&g, np.art_x, np.art_y, np.art_x + np.art_s - 1, np.art_y + np.art_h - 1);
    gfx_fill(&g, np.art_x - U(8), np.art_y, np.art_x - 1, np.art_y + np.art_h - 1, PEN_BG);
}

/* A Plex artist's or album's summary came, for the entry shown. */
static void cl_pane_summary(const char *text)
{
    WORD y = sum_y + U(3), w = aside_width() - U(4);
    pv_wrap(aside_x(), &y, w, g.footer_y - 2, text, PEN_DIM);
}

const struct UiView classic_ui = {
    cl_draw_header, cl_draw_menu, cl_draw_menu_row, cl_draw_footer, cl_draw_title, cl_draw_title_count,
    cl_draw_list_row, cl_draw_gutter_planes, cl_draw_list_rows, cl_draw_list_move, cl_draw_text_picture,
    cl_draw_pane, cl_np_draw, cl_draw_all, cl_pane_preview,
    cl_tick, cl_busy, cl_header_np, cl_clock, cl_status, cl_letter_show, cl_letter_relabel,
    cl_letter_hide, np_draw_station, cl_eq_tick, np_visual_changed, np_art_draw,
    cl_page_leave, cl_np_update, cl_np_rate, cl_np_cycle, cl_np_art_room, cl_menu_slide, cl_pane_summary,
    "classic", &classic_view
};
