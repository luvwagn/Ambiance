/*
 * Ambiance - what the layouts share (lshare.c), compiled into each one
 * Copyright (c) 2026 Brian King
 *
 * Helpers Showcase and Horizon both use, written once: measuring and
 * drawing text through the host, numbers in fixed cells, times, wrapping
 * and paragraphs, a long text's line heights, the gradient, the playing
 * marks, Now playing's buttons' glyphs and the disc, rounded shapes,
 * click targets, scroll bars' units, glides, a held key's acceleration,
 * and the lists' letters. A new layout starts from these rather than a
 * copy of another layout's.
 *
 * Each layout defines what they draw with: its host (H), its screen and
 * line heights, its colours (ls_top and ls_bottom, the background
 * gradient's ends; ls_acc, what is lit), and its texts' style (what their
 * lines are over, and how tall a blank line is).
 */
#ifndef LSHARE_H
#define LSHARE_H

#include <exec/types.h>
#include "../layout.h"

/* ---- each layout's own */
extern const struct LayoutHost *H;
extern WORD W, HT, FH, TH;      /* the screen; a line of text; a title's (LF_TITLE) */
extern const ULONG ls_top, ls_bottom, ls_acc;
extern const struct LkBg *const ls_lines_bg;   /* what draw_lines' text is over */
extern const BOOL ls_blank_half;               /* a blank line in a text: half a line (else a whole one) */

/* ---- shared state */
/* What a click lands on, noted as it is drawn (what: the layout's HT_*) */
struct Hit {
    WORD x, y, w, h;
    UBYTE what;                 /* HT_* */
    UBYTE kind;                 /* a row's or tile's LI_* */
    LONG arg;                   /* its index (a crumb: its level) */
};
#define MAX_HITS 80
extern struct Hit hits[MAX_HITS];
extern int nhits;

/* Numbers that change as they are shown: the widest digit's width, a face */
extern WORD dcell[4];

/* Paragraphs (a line each, "\n" between) into lines for draw_lines */
extern const char *para[64];
extern char parabuf[2048];

/* The playing mark's bars, percent of its height: low, middle, high (a
 * row's: still; the top right's: moved in the ticks as the sound goes) */
extern const UBYTE pm_still[3];
extern UBYTE pm_lv[3];

/* The scroll bars drawn (four at most): each one's run (at, len) and thumb
 * (t, tl), across (a wall that scrolls sideways) or down, and its list's
 * units (cur: the row chosen, or a text's first line; a page of it; its
 * last place, LAC_SCROLLTO's whole way). A click on the thumb drags it
 * (scroll_to) until the button is let go; above or below it, a page. */
struct SBar { LONG at, len, t, tl; BOOL across; LONG cur, page, last; };
extern struct SBar sbar[4];
extern int nsbar;
extern int sb_drag;             /* the bar being dragged, -1 none */
extern LONG sb_off;             /* where in the thumb it was taken */
/* the next scroll bar's units: set before drawing it (scroll_units) */
extern LONG sb_cur, sb_page, sb_last;

/* the page split worked out last (page_split) */
extern ULONG split_key;

/* A long text as the layout wraps it: each line's height, and the whole;
 * worked out once a text and width (text_measure). The host scrolls its
 * own wrapping of it (the classic's lines and box), so where it is goes
 * by the fraction of the way (LPage.text_frac): its end is the end here. */
#define TL_N 256
extern LONG tl_h[TL_N], tl_n, tl_w, tl_total;
extern const void *tl_of;

/* A glide: from where it was to where it is going in GLIDE_MS, fast at
 * first and settling (however long each frame takes to draw: a slow
 * screen takes fewer steps, not longer). A new goal starts one from
 * where it is. */
#define GLIDE_MS 180
struct Glide { LONG from, to; ULONG t0; };

/* A key held on a wall (its repeats coming close together): after a few
 * moves, several at a time; held longer, a letter at a time (to the first
 * of the next letter, or back to the first of this one, then the last) */
extern ULONG acc_ms;
extern int acc_dir, acc_run;

/* ---- the helpers */
LONG X(LONG v);
LONG Y(LONG v);
int accel(int dir);
void background(void);
BOOL ct_right(int b);
WORD digit_cell(int face);
int disc_angle(LONG dx, LONG dy);
ULONG disc_sector(int a);
const struct LkBg *flat(ULONG c);
LONG glide(struct Glide *g, LONG at, LONG to, ULONG ms);
ULONG grad(LONG y);
void hit(LONG x, LONG y, LONG w, LONG h, int what, int kind, LONG arg);
char initial(const struct LList *l, LONG i);
const char *join(char *b, LONG max, const char *s, const char *sep, const char *t);
LONG letter_jump(const struct LList *l, LONG sel, int dir);
LONG mark(int m, LONG x, LONG cy, ULONG fg, ULONG bg);
char *mmss(ULONG ms, char *b);
void nb_glyph(int b, BOOL playing, LONG cx, LONG cy, LONG s, ULONG c);
void note_centred(const char *t, LONG x, LONG y, LONG w, ULONG c, const struct LkBg *bg);
ULONG page_key(const struct LPage *m);
int page_split(const struct LPage *m);
LONG paras(const char *t);
LONG play_bars(LONG x, LONG y, LONG s, const UBYTE *lv);
BOOL playable(int kind);
LONG rinset16(LONG j, LONG h, LONG r);
void rshape(LONG x, LONG y, LONG w, LONG h, LONG r, ULONG c);
void scopy(char *d, const char *s, LONG max);
void scroll_page(int k, int dir);
void scroll_units(LONG cur, LONG page, LONG last);
ULONG shash(const char *s);
LONG slen(const char *s);
WORD text(LONG x, LONG y, const char *s, int face, ULONG fg, const struct LkBg *bg, LONG maxw);
LONG text_from(const struct LPage *m, LONG vh, LONG *dy);
WORD tlen(const char *s, int face);
LONG tnum(LONG x, LONG y, const char *s, int face, ULONG fg, const struct LkBg *bg);
LONG tnum_len(const char *s, int face);
void tri(LONG x, LONG cy, LONG h, int dir, ULONG c);
LONG wrapped(const char *p, LONG w, int face);
ULONG mix(ULONG a, ULONG b, LONG num, LONG den);
LONG isqrt(LONG v);
BOOL pm_step(const struct LNow *n);
void text_measure(const struct LPage *m, LONG w, LONG lh);
LONG draw_lines(const char *const *s, LONG n, LONG from, LONG x, LONG y, LONG w, LONG bottom, ULONG first,
                ULONG rest);
void disc(LONG x, LONG y, LONG s, ULONG bg);
void scroll_to(LONG p);

#endif
