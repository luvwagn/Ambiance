/*
 * AmigaMCE - Now Playing's visualizations: plug-ins drawn in the place of
 * the level meters. Each says what it needs of the playing engine (VF_*,
 * from mp_vis_feed) and draws only what changed since its last frame.
 * Copyright (c) 2026 Brian King
 *
 *   VIS_METERS    the level meters: Paula's four channels with their
 *                 samples, or a stream's left and right (any engine)
 *   VIS_SPECTRUM  HippoPlayer's spectrum analyser (MIT licence, (c) 2017
 *                 Kari-Pekka Koljonen; puu016.s and hippo_fft.s, the FFT
 *                 in hfft.s): a 128-point FFT of the sound at about 8 kHz,
 *                 0-4 kHz across. Paula's voices are mixed from their
 *                 samples where they play now, as HippoPlayer mixes them;
 *                 what AmigaMCE decodes itself comes as samples. Not
 *                 under MHI (the driver plays the sound; none comes back):
 *                 the meters are drawn there instead.
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/graphics.h>

void hfft_window(WORD *re, const WORD *sine);         /* hfft.s */
void hfft_fft(WORD *re, WORD *im, const WORD *sine);

#define VU(c, v) GU((c)->g, v)

static int sel = VIS_METERS;    /* the one chosen */
static struct {
    WORD level[4];              /* the meters' bar lengths drawn */
    UBYTE sample[4];            /* the sample names shown */
    int n;                      /* meters drawn (4, 2, 1) */
} mt;
static int drawn = -1;          /* the one drawn last */

static const char *const names[VIS_COUNT] = { N_("Level meters"), N_("Spectrum") };

const char *vis_name(int v)
{
    return v >= 0 && v < VIS_COUNT ? names[v] : names[0];
}

/* Too much for this Amiga: while the music plays, Now playing should be
 * drawn 25 times a second; where it gets fewer than 8 for 4 seconds the
 * CPU is short (a decoder on a slow Amiga), and the visualization stops
 * for the rest of the module, track or station, a note in its place, so
 * the music keeps what CPU there is. V, or choosing one again, tries it
 * again. */
#define LOAD_WINDOW 2000        /* ms */
#define LOAD_MIN_FPS 8
static struct {
    ULONG t0, serial;
    UWORD frames, slow;
    BOOL paused, noted;
} ld;

static void load_reset(void)
{
    ld.t0 = time_ms();
    ld.frames = ld.slow = 0;
    ld.paused = ld.noted = FALSE;
}

BOOL vis_paused(void)
{
    return ld.paused;
}

void vis_select(int v)
{
    sel = v >= 0 && v < VIS_COUNT ? v : VIS_METERS;
    load_reset();
    drawn = -1;
}

int vis_selected(void)
{
    return sel;
}

int vis_active(void)
{
    if (sel == VIS_SPECTRUM && (mp_vis_have() & (VF_VOICES | VF_PCM)))
        return VIS_SPECTRUM;
    return VIS_METERS;
}

int vis_rows(void)
{
    return vis_active() == VIS_SPECTRUM ? 4 : 0;
}

static WORD area_h(const struct VisCtx *c)
{
    return c->rows * c->row_h + (c->rows - 1) * c->gap;
}

/* FALSE: draw as usual. TRUE: paused, its note drawn in the place. */
static BOOL load_check(const struct VisCtx *c, BOOL full)
{
    ULONG now = time_ms();
    if (mp_serial() != ld.serial) {
        ld.serial = mp_serial();    /* another piece: tried again */
        load_reset();
    }
    if (!ld.paused) {
        if (!mp_running() || mp_paused()) {
            ld.t0 = now;            /* not playing: nothing to measure */
            ld.frames = 0;
            return FALSE;
        }
        ld.frames++;
        if (now - ld.t0 >= LOAD_WINDOW) {
            ULONG fps = (ULONG)ld.frames * 1000 / (now - ld.t0);
            ld.slow = fps < LOAD_MIN_FPS ? ld.slow + 1 : 0;
            if (ld.slow >= 2) {
                char m[64];
                sprintf(m, "paused: %lu frames a second while playing", (unsigned long)fps);
                amce_log("visualisation", m);
                ld.paused = TRUE;
                full = TRUE;
            }
            ld.t0 = now;
            ld.frames = 0;
        }
        if (!ld.paused)
            return FALSE;
    }
    if (full || !ld.noted) {
        WORD h = area_h(c);
        const char *t = _("Paused: the music needs the CPU");
        gfx_fill(c->g, c->x, c->y, c->x + c->w - 1, c->y + h - 1, PEN_BG);
        gfx_text(c->g, c->x + (c->w - gfx_textlen(c->g, t, TS_SMALL)) / 2,
                 c->y + (h - c->g->font_h) / 2, t, PEN_DIM, PEN_BG, c->w, TS_SMALL);
        ld.noted = TRUE;
        mt.n = 0;
        drawn = -1;                 /* drawn whole when it runs again */
    }
    return TRUE;
}

/* ------------------------------------------------------------ the level meters */

static void meters_draw(const struct VisCtx *c, const struct MpStatus *st, BOOL full)
{
    struct Gfx *g = c->g;
    int i, n = mp_meter_count();
    WORD bx = c->x + c->label_w + VU(c, 4), bw = c->x + c->w - bx, bh = c->row_h - VU(c, 4);
    if (bh < 2)
        bh = 2;
    if (n != mt.n) {
        /* fewer meters than last time (stereo, mono): the rest cleared */
        gfx_fill(g, c->x, c->y, c->x + c->w - 1, c->y + c->rows * (c->row_h + c->gap) - 1, PEN_BG);
        mt.n = n;
        full = TRUE;
    }
    if (n > c->rows)
        n = c->rows;                        /* laid out for fewer: the page lays it out again */
    for (i = 0; i < n; i++) {
        WORD y = c->y + i * (c->row_h + c->gap), by = y + (c->row_h - bh) / 2;
        WORD lit = (WORD)((LONG)bw * st->level[i] / 64);
        if (n < 4) {
            /* a channel of the sound, not of Paula: Left and Right, or Mono */
            if (full) {
                gfx_fill(g, c->x, y, c->x + c->label_w + VU(c, 3), y + c->row_h - 1, PEN_BG);
                gfx_text(g, c->x, y + (c->row_h - g->font_h) / 2,
                         n == 1 ? _("Mono") : i ? _("Right") : _("Left"), PEN_DIM, PEN_BG,
                         c->label_w, TS_SMALL);
            }
        } else if (full || st->sample[i] != mt.sample[i]) {
            char name[40];
            BOOL used;
            const char *sn = st->sample[i] ? mp_sample_name(st->sample[i] - 1, &used) : "";
            if (st->sample[i])
                sprintf(name, "%ld  %.32s", (long)st->sample[i], sn);
            else
                name[0] = 0;
            gfx_fill(g, c->x, y, c->x + c->label_w + VU(c, 3), y + c->row_h - 1, PEN_BG);
            if (name[0])
                gfx_text(g, c->x, y + (c->row_h - g->font_h) / 2, name, PEN_DIM, PEN_BG,
                         c->label_w, TS_SMALL);
            mt.sample[i] = st->sample[i];
        }
        if (full) {
            if (lit > 0)
                gfx_fill(g, bx, by, bx + lit - 1, by + bh - 1, i == 0 || i == 3 ? PEN_ACCENT : PEN_HL);
            if (lit < bw)
                gfx_fill(g, bx + lit, by, bx + bw - 1, by + bh - 1, PEN_RULE);
            mt.level[i] = lit;
        } else if (lit != mt.level[i]) {
            /* only the change: 25 times a second a whole bar, each one
             * across most of the screen, was most of Now playing's time */
            WORD old = mt.level[i];
            if (old < 0 || old > bw)
                old = lit > 0 ? 0 : bw;
            if (lit > old)
                gfx_fill(g, bx + old, by, bx + lit - 1, by + bh - 1,
                         i == 0 || i == 3 ? PEN_ACCENT : PEN_HL);
            else
                gfx_fill(g, bx + lit, by, bx + old - 1, by + bh - 1, PEN_RULE);
            mt.level[i] = lit;
        }
    }
}

/* ------------------------------------------------------------ the spectrum */
#define FFT_N    128            /* hfft.s's FFT_LENGTH */
#define BINS     (FFT_N / 2)
#define CH_BYTES 256            /* a voice's sample bytes copied (SAMPLE_LENGTH) */
#define MIX_HZ   8000           /* the voices mixed at this rate (calcSampleStep) */
#define LEVELS   64

static struct {
    APTR mem;
    WORD *sine;                 /* 1024: a sine of 32767, shifted right once */
    WORD *re, *im;              /* FFT_N each */
    ULONG *loud;                /* LEVELS thresholds, quietest first */
    BYTE *ch;                   /* CH_BYTES a voice */
    struct VisFeed feed;
    WORD shown[BINS];           /* bar heights drawn, in pixels */
    ULONG frames, ms;           /* timing (Debug's load line) */
} sp;

static BOOL sp_start(void)
{
    UBYTE *m;
    if (sp.mem)
        return TRUE;
    sp.mem = AllocVec(1024 * 2 + FFT_N * 4 + LEVELS * 4 + 4 * CH_BYTES, MEMF_ANY | MEMF_CLEAR);
    if (!sp.mem)
        return FALSE;
    m = (UBYTE *)sp.mem;
    sp.sine = (WORD *)m;
    m += 1024 * 2;
    sp.re = (WORD *)m;
    m += FFT_N * 2;
    sp.im = (WORD *)m;
    m += FFT_N * 2;
    sp.loud = (ULONG *)m;
    m += LEVELS * 4;
    sp.ch = (BYTE *)m;
    vis_tables(sp.sine);
    vis_loud_table(sp.loud);
    return TRUE;
}

/* CH_BYTES of a voice from where it plays, round its loop (as
 * spectrumCopySamples). */
static void copy_voice(const struct VisVoice *v, BYTE *d)
{
    const BYTE *p = v->pos;
    LONG left = v->left, n = 0;
    while (n < CH_BYTES) {
        LONG k = left < CH_BYTES - n ? left : CH_BYTES - n;
        if (k > 0) {
            memcpy(d + n, p, k);
            n += k;
        }
        if (n >= CH_BYTES)
            break;
        if (!v->loop || v->replen <= 2) {
            memset(d + n, 0, CH_BYTES - n);
            break;
        }
        p = v->loop;
        left = v->replen;
    }
}

/* Paula's four voices mixed at MIX_HZ into re (spectrumMixSamplesNew):
 * each resampled by its period, by its volume. */
static void mix_voices(const struct VisFeed *f)
{
    int c, k;
    memset(sp.re, 0, FFT_N * 2);
    for (c = 0; c < 4; c++) {
        const struct VisVoice *v = &f->voice[c];
        ULONG step, at = 0;
        WORD vol = v->vol * mp_volume() / 64;
        if (!v->pos || !vol || v->period < 100 || v->left <= 0)
            continue;
        copy_voice(v, sp.ch);
        step = ((3546895UL / v->period) << 8) / MIX_HZ;    /* 8.8 */
        for (k = 0; k < FFT_N; k++) {
            sp.re[k] = (WORD)(sp.re[k] + sp.ch[(at >> 8) & (CH_BYTES - 1)] * vol);
            at += step;
        }
    }
}

/* Each bin's loudness, 0-64, from its squared magnitude (loudnessFFT). */
static void loudness(UBYTE *lev)
{
    int i, k;
    for (i = 0; i < BINS; i++) {
        ULONG m = (ULONG)((LONG)sp.re[i] * sp.re[i]) + (ULONG)((LONG)sp.im[i] * sp.im[i]);
        int lo = 0, hi = LEVELS;        /* the steps below m: halved 6 times, not 64 looks */
        while (lo < hi) {
            k = (lo + hi) >> 1;
            if (m > sp.loud[k])
                lo = k + 1;
            else
                hi = k;
        }
        lev[i] = (UBYTE)lo;
    }
}

static void spectrum_draw(const struct VisCtx *c, BOOL full)
{
    struct Gfx *g = c->g;
    UBYTE lev[BINS];
    /* inset as the meters' bars are in their rows: the same height drawn */
    WORD h = area_h(c) - 2 * VU(c, 2), bottom = c->y + VU(c, 2) + h - 1, nb, per, bw, gap, x0, fall;
    ULONG t0 = time_ms();
    int i, j;
    if (!sp_start())
        return;
    if (full) {
        gfx_fill(g, c->x, c->y, c->x + c->w - 1, c->y + area_h(c) - 1, PEN_BG);
        memset(sp.shown, 0, sizeof(sp.shown));
    }
    mp_vis_feed(&sp.feed, VF_VOICES | VF_PCM);
    if (sp.feed.have & VF_VOICES) {
        mix_voices(&sp.feed);
    } else if (sp.feed.pcm_n == FFT_N) {
        memcpy(sp.re, sp.feed.pcm, FFT_N * 2);
    } else {
        memset(sp.re, 0, FFT_N * 2);        /* paused, or nothing yet */
    }
    hfft_window(sp.re, sp.sine);
    hfft_fft(sp.re, sp.im, sp.sine);
    loudness(lev);
    sp.ms += time_ms() - t0;
    sp.frames++;
    /* 32 bars, or 16 where narrow (a gap between them from 4 pixels on),
     * each the loudest of its bins: each bar a fill a frame, so not 64 */
    nb = c->w >= BINS / 2 * 4 ? BINS / 2 : BINS / 4;
    per = BINS / nb;
    bw = c->w / nb;
    gap = bw >= 4 ? 1 : 0;
    x0 = c->x + (c->w - nb * bw) / 2;
    fall = h / 16 > 1 ? h / 16 : 1;         /* the bars fall away, a little a frame */
    if (bw - gap < 1)
        return;
    if (g->planar)
        SetWriteMask(g->rp, 0x0F);      /* pens 0 and 8 only: half an AGA screen's planes */
    for (i = 0; i < nb; i++) {
        UBYTE l = 0;
        WORD want, old = sp.shown[i], x = x0 + i * bw;
        for (j = 0; j < per; j++)
            if (lev[i * per + j] > l)
                l = lev[i * per + j];
        want = (WORD)((LONG)l * h / LEVELS);
        if (want < old - fall)
            want = old - fall;
        if (want > old)
            gfx_fill(g, x, bottom - want + 1, x + bw - gap - 1, bottom - old, PEN_ACCENT);
        else if (want < old)
            gfx_fill(g, x, bottom - old + 1, x + bw - gap - 1, bottom - want, PEN_BG);
        sp.shown[i] = want;
    }
    if (g->planar)
        SetWriteMask(g->rp, 0xFF);
}

/* ------------------------------------------------------------ the plug-ins' host */
void vis_draw(const struct VisCtx *c, const struct MpStatus *st, BOOL full)
{
    int a = vis_active();
    if (load_check(c, full))
        return;
    if (a != drawn) {
        /* another one from now: its place cleared, drawn whole */
        gfx_fill(c->g, c->x, c->y, c->x + c->w - 1, c->y + area_h(c) - 1, PEN_BG);
        mt.n = 0;
        drawn = a;
        full = TRUE;
    }
    if (a == VIS_SPECTRUM)
        spectrum_draw(c, full);
    else
        meters_draw(c, st, full);
}

void vis_stop(void)
{
    if (sp.mem)
        FreeVec(sp.mem);
    sp.mem = NULL;
    drawn = -1;
}

void vis_timing(ULONG *frames, ULONG *ms100)
{
    *frames = sp.frames;
    *ms100 = sp.ms * 10;
    sp.frames = sp.ms = 0;
}
