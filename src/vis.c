/*
 * Ambiance - Now Playing's visualizations: plug-ins drawn in the place of
 * the level meters. Each says what it needs of the playing engine (VF_*,
 * from mp_vis_feed) and draws only what changed since its last frame.
 * Copyright (c) 2026 Brian King
 *
 *   VIS_METERS    the level meters: Paula's four channels with their
 *                 samples, or a stream's left and right (any engine)
 *   VIS_FLOW      Flow (visflow.c): a picture carried on and on, the
 *                 sound's wave drawn in, on graphics cards (else the
 *                 spectrum); F on Now playing shows it on the whole
 *                 screen
 *   VIS_SPECTRUM  HippoPlayer's spectrum analyser (MIT licence, (c) 2017
 *                 Kari-Pekka Koljonen; puu016.s and hippo_fft.s, the FFT
 *                 in hfft.s): a 128-point FFT of the sound at about 8 kHz,
 *                 0-4 kHz across. Paula's voices are mixed from their
 *                 samples where they play now, as HippoPlayer mixes them;
 *                 what Ambiance decodes itself comes as samples. Not
 *                 under MHI (the driver plays the sound; none comes back):
 *                 the meters are drawn there instead.
 */
#include "amb.h"

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
    UBYTE last[4];              /* the levels last drawn (0-64), for the marker */
    ULONG last_t;
} mt;
static int drawn = -1;          /* the one drawn last */

static const char *const names[VIS_COUNT] = { N_("Level meters"), N_("Spectrum"), N_("Flow") };
static struct Gfx *vis_g;       /* the display (Flow: a graphics card's?) */

void vis_set_gfx(struct Gfx *g)
{
    vis_g = g;
}

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
    if (sel == VIS_FLOW && vis_available(VIS_FLOW))
        return VIS_FLOW;
    if ((sel == VIS_SPECTRUM || sel == VIS_FLOW) && (mp_vis_have() & (VF_VOICES | VF_PCM)))
        return VIS_SPECTRUM;
    return VIS_METERS;
}

/* The playing engine can give what v needs (Flow: on a graphics card). */
BOOL vis_available(int v)
{
    if (v == VIS_SPECTRUM)
        return (mp_vis_have() & (VF_VOICES | VF_PCM)) != 0;
    if (v == VIS_FLOW)
        return (mp_vis_have() & (VF_VOICES | VF_PCM)) != 0 && vf_possible(vis_g);
    return v == VIS_METERS;
}

/* Every visualization takes the meters' place as it is (4 rows for
 * Paula's voices, 2 for a stream's left and right): switching between
 * them draws that place only, never the page. */
int vis_rows(void)
{
    return 0;
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
                amb_log("visualisation", m);
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
    memcpy(mt.last, st->level, 4);
    mt.last_t = time_ms();
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
/* What of HippoPlayer's 64 loudness steps (1.4 dB each, 89 dB in all) the
 * bars show. All 64, as HippoPlayer does, made a quiet lead-in nearly as
 * tall as the tune: a voice at volume 8 of 64 reached half height, at
 * volume 1 a third. Measured through this FFT (a sine at one bin): the
 * four voices at full volume come to step 55, one voice 46, volume 16 38,
 * 8 34, 4 29, 1 21 - each halving about 4 steps. The bars now span steps
 * SP_FLOOR to SP_TOP, some 33 dB: the full tune at the top, one voice at
 * full volume three quarters, volume 8 a quarter, volume 2 nothing, as
 * the level meters would have it. */
#define SP_FLOOR 28
#define SP_TOP   52

static struct {
    APTR mem;
    WORD *sine;                 /* 1024: a sine of 32767, shifted right once */
    WORD *re, *im;              /* FFT_N each */
    WORD wave[FFT_N];           /* the sound analysed, as it was (Flow draws it) */
    ULONG *loud;                /* LEVELS thresholds, quietest first */
    BYTE *ch;                   /* CH_BYTES a voice */
    struct VisFeed feed;
    WORD shown[BINS];           /* bar heights drawn, in pixels */
    ULONG frames, ms;           /* timing (Debug's load line) */
    UBYTE lev[BINS];            /* the last analysis's loudness, 0-64 */
    ULONG lev_t;                /* ...when */
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

/* The sound now, analysed: each bin's loudness in sp.lev. */
/* What the spectrum was given, since vis_input last asked (the log's load
 * line): its loudest sample, and the frames that had no sound to see */
static UWORD sp_peak;
static ULONG sp_none;
static UBYTE sp_top;            /* (its loudest bin's step: the bars show SP_FLOOR to SP_TOP) */

static BOOL sp_analyse(void)
{
    ULONG t0 = time_ms();
    int k;
    if (!sp_start())
        return FALSE;
    mp_vis_feed(&sp.feed, VF_VOICES | VF_PCM);
    if (sp.feed.have & VF_VOICES) {
        mix_voices(&sp.feed);
    } else if (sp.feed.pcm_n == FFT_N) {
        memcpy(sp.re, sp.feed.pcm, FFT_N * 2);
    } else {
        memset(sp.re, 0, FFT_N * 2);        /* paused, or nothing yet */
        sp_none++;
    }
    for (k = 0; k < FFT_N; k++) {
        UWORD a = (UWORD)(sp.re[k] < 0 ? -sp.re[k] : sp.re[k]);
        if (a > sp_peak)
            sp_peak = a;
    }
    memcpy(sp.wave, sp.re, FFT_N * 2);
    hfft_window(sp.re, sp.sine);
    hfft_fft(sp.re, sp.im, sp.sine);
    loudness(sp.lev);
    for (k = 1; k < BINS; k++)
        if (sp.lev[k] > sp_top)
            sp_top = sp.lev[k];
    sp.lev_t = time_ms();
    sp.ms += sp.lev_t - t0;
    sp.frames++;
    return TRUE;
}

static void spectrum_draw(const struct VisCtx *c, BOOL full)
{
    struct Gfx *g = c->g;
    const UBYTE *lev = sp.lev;
    /* inset as the meters' bars are in their rows: the same height drawn */
    WORD h = area_h(c) - 2 * VU(c, 2), bottom = c->y + VU(c, 2) + h - 1, nb, per, bw, gap, x0, fall;
    int i, j;
    if (!sp_analyse())
        return;
    if (full) {
        gfx_fill(g, c->x, c->y, c->x + c->w - 1, c->y + area_h(c) - 1, PEN_BG);
        memset(sp.shown, 0, sizeof(sp.shown));
    }
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
        want = l <= SP_FLOOR ? 0 : l >= SP_TOP ? h
             : (WORD)((LONG)(l - SP_FLOOR) * h / (SP_TOP - SP_FLOOR));
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

/* ------------------------------------------------------------ stereo, for plug-ins */
#define LR_HZ    22050          /* Paula's voices mixed at this rate, for AmigaAMP's plug-ins */
#define LR_BYTES 4096           /* a voice's sample bytes copied (512 frames at the highest note) */

/* len bytes of a voice from where it plays, round its loop */
static void copy_voice_n(const struct VisVoice *v, BYTE *d, LONG len)
{
    const BYTE *p = v->pos;
    LONG left = v->left, n = 0;
    while (n < len) {
        LONG k = left < len - n ? left : len - n;
        if (k > 0) {
            memcpy(d + n, p, k);
            n += k;
        }
        if (n >= len)
            break;
        if (!v->loop || v->replen <= 2) {
            memset(d + n, 0, len - n);
            break;
        }
        p = v->loop;
        left = v->replen;
    }
}

/* The last n frames playing as 16-bit stereo, interleaved: the engine's
 * own samples (a stream, the mixer), or Paula's four voices mixed at LR_HZ
 * (0 and 3 left, 1 and 2 right). 0: none (MHI, nothing playing). */
static BYTE *lr_ch;             /* (a voice's bytes, copied) */

UWORD vis_pcm_lr(WORD *lr, UWORD n, ULONG *rate)
{
    static struct VisFeed f;
    BYTE *ch;
    UWORD got = mp_pcm_lr(lr, n, rate);
    int c;
    ULONG k;
    if (got)
        return got;
    if (!(mp_vis_have() & VF_VOICES))
        return 0;
    if (!lr_ch && !(lr_ch = AllocVec(LR_BYTES, MEMF_ANY)))
        return 0;
    ch = lr_ch;
    mp_vis_feed(&f, VF_VOICES);
    memset(lr, 0, (ULONG)n * 4);
    for (c = 0; c < 4; c++) {
        const struct VisVoice *v = &f.voice[c];
        ULONG step, at = 0;
        WORD vol = v->vol * mp_volume() / 64;
        WORD *o = lr + (c == 0 || c == 3 ? 0 : 1);
        if (!v->pos || !vol || v->period < 100 || v->left <= 0)
            continue;
        step = ((3546895UL / v->period) << 8) / LR_HZ;     /* 8.8 */
        copy_voice_n(v, ch, LR_BYTES);
        for (k = 0; k < n; k++, o += 2) {
            ULONG i = at >> 8;
            if (i >= LR_BYTES)
                break;
            *o = (WORD)(*o + ch[i] * vol * 2);
            at += step;
        }
    }
    *rate = LR_HZ;
    return n;
}

/* The spectrum for a layout's Now playing: n bands, 0-255 as the classic
 * bars' heights (SP_FLOOR to SP_TOP), analysed again when 40 ms old.
 * FALSE: nothing to analyse (no memory).
 *
 * The bins are 62.5 Hz each, 0-4 kHz. Music's loudness falls with
 * frequency (about 3 dB an octave, more above 2 kHz), so even bands of
 * two bins each left the upper half under SP_FLOOR. Two changes, as
 * analysers for music have them:
 *   - the bands widen as they go up (one bin each low down, then
 *     logarithmic: the last few cover 1.7-4 kHz between them);
 *   - each bin is lifted by about 4 dB an octave above 250 Hz (three
 *     loudness steps of 1.4 dB an octave: 11 steps at 4 kHz). */
static UBYTE sp_edge[BINS + 1];     /* band i: bins sp_edge[i] to sp_edge[i + 1] - 1 */
static int sp_edge_n;
static UBYTE sp_tilt[BINS];

static void sp_bands(int n)
{
    int i, k;
    sp_edge_n = n;
    for (k = 0; k < BINS; k++) {    /* steps of 2^(1/3): (4 * 2^(j/3))^3 = 64 << j */
        ULONG c = (ULONG)k * k * k;
        int j = 0;
        while (j < 15 && c >= (64UL << (j + 1)))
            j++;
        sp_tilt[k] = (UBYTE)(k > 4 ? j : 0);
    }
    /* edges at BINS^(i/n) (bin 1 to BINS), each band a bin at least; i/n
     * in 1/4096ths, 2^x by a doubling and a cubic for the fraction */
    sp_edge[0] = 1;
    for (i = 1; i <= n; i++) {
        ULONG x = (ULONG)i * 6 * 4096 / n;      /* log2(64) = 6 */
        ULONG f = x & 4095, e;
        /* 2^f, f in [0,1): 1 + 0.6957f + 0.2262f^2 + 0.0781f^3 (to 0.1%) */
        ULONG p = 4096 + (2850 * f + ((926 * f / 4096) * f) + (((320 * f / 4096) * f / 4096) * f)) / 4096;
        e = (p << (x >> 12)) / 4096;
        if (e < (ULONG)sp_edge[i - 1] + 1)
            e = sp_edge[i - 1] + 1;
        if (e > (ULONG)(BINS - (n - i)))
            e = BINS - (n - i);
        sp_edge[i] = (UBYTE)e;
    }
    sp_edge[n] = BINS;
}

BOOL vis_spectrum(UBYTE *out, int n)
{
    int i, b;
    if (n <= 0 || n > BINS - 1)
        return FALSE;
    if (time_ms() - sp.lev_t >= 40 && !sp_analyse())
        return FALSE;
    if (n != sp_edge_n)
        sp_bands(n);
    for (i = 0; i < n; i++) {
        int m = 0;
        for (b = sp_edge[i]; b < sp_edge[i + 1]; b++) {
            int v = sp.lev[b] ? sp.lev[b] + sp_tilt[b] : 0;
            if (v > m)
                m = v;
        }
        out[i] = (UBYTE)(m <= SP_FLOOR ? 0 : m >= SP_TOP ? 255 : (m - SP_FLOOR) * 255 / (SP_TOP - SP_FLOOR));
    }
    return TRUE;
}

/* ------------------------------------------------------------ the playing marker */
/* MARK_PLAYING (the header's, a list's playing row) as the visualization
 * shown: four of the spectrum's bands, or the meters' left and right.
 * From what Now playing drew last if that is fresh; otherwise measured
 * here (the marker moves every 120 ms). lev: 0-11 each. */
int vis_marker(UBYTE *lev)
{
    ULONG now = time_ms();
    int a = vis_active(), i;
    if (a == VIS_SPECTRUM || a == VIS_FLOW) {
        static const UBYTE band[5] = { 1, 3, 7, 17, BINS };  /* low to high, wider as they go */
        if (now - sp.lev_t > 200 && !sp_analyse())
            return -1;
        for (i = 0; i < 4; i++) {
            UBYTE m = 0;
            int b;
            for (b = band[i]; b < band[i + 1]; b++)
                if (sp.lev[b] > m)
                    m = sp.lev[b];
            lev[i] = (UBYTE)(m <= SP_FLOOR ? 0 : m >= SP_TOP ? 11
                                 : (m - SP_FLOOR) * 11 / (SP_TOP - SP_FLOOR));
        }
        return VIS_SPECTRUM;
    }
    if (now - mt.last_t > 200) {
        struct MpStatus st;
        mp_status(&st);                     /* Now playing not shown: measured here */
        memcpy(mt.last, st.level, 4);
        mt.last_t = now;
    }
    {
        int n = mp_meter_count();
        /* Paula's left is voices 0 and 3, its right 1 and 2 */
        UBYTE l = n >= 4 ? (mt.last[0] > mt.last[3] ? mt.last[0] : mt.last[3]) : mt.last[0];
        UBYTE r = n >= 4 ? (mt.last[1] > mt.last[2] ? mt.last[1] : mt.last[2])
                         : n == 2 ? mt.last[1] : mt.last[0];
        lev[0] = (UBYTE)(l * 11 / 64);
        lev[1] = (UBYTE)(r * 11 / 64);
    }
    return VIS_METERS;
}

/* ------------------------------------------------------------ the plug-ins' host */
void vis_draw(const struct VisCtx *c, const struct MpStatus *st, BOOL full)
{
    if (gfx_off)                /* a layout has its own levels */
        return;
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
    if (a == VIS_FLOW && sp_analyse() &&
        vf_draw(c->g, c->x, c->y, c->w, area_h(c), sp.wave, FFT_N, sp.lev, BINS, full))
        return;
    if (a == VIS_SPECTRUM || a == VIS_FLOW)
        spectrum_draw(c, full);
    else
        meters_draw(c, st, full);
}

/* Flow on the whole of w x h at x, y (F on Now playing): FALSE if it
 * cannot be (not a graphics card's screen, no memory). */
BOOL vis_draw_full(struct Gfx *g, WORD x, WORD y, WORD w, WORD h, BOOL first)
{
    if (!vf_possible(g) || !sp_analyse())
        return FALSE;
    return vf_draw(g, x, y, w, h, sp.wave, FFT_N, sp.lev, BINS, first);
}

void vis_stop(void)
{
    vf_stop();
    if (lr_ch)
        FreeVec(lr_ch);
    lr_ch = NULL;
    if (sp.mem)
        FreeVec(sp.mem);
    sp.mem = NULL;
    drawn = -1;
}

void vis_input(ULONG *peak, ULONG *none, ULONG *top)
{
    *peak = sp_peak;
    *none = sp_none;
    *top = sp_top;
    sp_top = 0;
    sp_peak = 0;
    sp_none = 0;
}

void vis_timing(ULONG *frames, ULONG *ms100)
{
    ULONG ff, fm;
    vf_timing(&ff, &fm);
    /* (Flow analyses the sound too: its frames are the spectrum's, its
     * time theirs and its own) */
    *frames = sp.frames;
    *ms100 = (sp.ms + fm) * 10;
    sp.frames = sp.ms = 0;
}
