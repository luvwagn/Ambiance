/*
 * AmigaMCE - modules played on Paula
 * Copyright (c) 2026 Brian King
 *
 * ptcore.c plays the song; this is the Amiga around it. The four audio
 * channels are taken from audio.device (at the top priority, so nothing
 * steals them mid-song), then driven directly, as trackers do. A CIA-B
 * timer interrupt runs the song at its tempo (50 ticks a second at 125
 * BPM), wherever AmigaMCE is and however busy its drawing: the music
 * never waits for the screen.
 *
 * Starting a note on Paula takes three steps a little apart, as the
 * ProTracker play routine does them: the channel's DMA off and its new
 * start written (in the tick), the DMA on once Paula has noticed it was
 * off, then its loop written once Paula has taken the start. The second
 * CIA-B timer, run once each time, times the two gaps; without it (in use
 * by something else) the tick waits them out on the beam instead.
 *
 * The module: header and patterns in any memory, samples in chip RAM.
 *
 * Modules of 5-8 channels ("8CHN", "OCTA", "FLT8"...) on a 68020 or
 * better: the replayer drives eight Paulas in software (ptmix.c, after
 * ProTracker 4.1's mixer by Andreas Wendorf), channel n + 4 mixed with
 * channel n into a ring that Paula's channel n plays, a tick's worth at a
 * time in a software interrupt (Cause) after each tick.
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <devices/audio.h>
#include <hardware/custom.h>
#include <hardware/intbits.h>
#include <hardware/adkbits.h>
#include <hardware/dmabits.h>
#include <hardware/cia.h>
#include <resources/cia.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/cia.h>
#include <devices/timer.h>
#include <proto/timer.h>

#include "ptcore.h"
#include "ptmix.h"
#include "medtime.h"

extern struct ExecBase *SysBase;
#define CU ((volatile struct Custom *)0xDFF000)

/* CIA-B's timer registers (bytes, 256 apart; CIA-A's are at odd addresses) */
#define CIAB_BASE 0xBFD000UL
#define CIAA_BASE 0xBFE001UL
#define R_TALO 0x400
#define R_TAHI 0x500
#define R_TBLO 0x600
#define R_TBHI 0x700
#define R_CRA  0xE00
#define R_CRB  0xF00
#define CIAREG(base, r) (*(volatile UBYTE *)((base) + (r)))

#define GAP_US 550
#define MIX_RING 8192               /* bytes a ring (Paula's length: 4096 words) */
#define MIX_Q    8                  /* ticks queued for mixing */

enum { ENG_NONE, ENG_PT, ENG_EAGLE, ENG_MIX, ENG_RADIO };          /* each of the two gaps in starting a note */

static struct {
    /* the hardware */
    struct MsgPort *port;
    struct IOAudio *aio;
    BOOL audio_open;
    struct Library *cia;            /* ciab.resource (or ciaa.resource) */
    ULONG cia_base;
    struct Interrupt tick_int, gap_int;
    BOOL have_tick, have_gap;
    UBYTE tick_bit;                 /* CIAICRB_TA or _TB: the one the tick has */
    ULONG eclock;
    /* the module */
    struct Pt *pt;
    struct PtTimes *times;
    struct PtLayout lay;
    UBYTE *hdr, *pat;
    BYTE *smp, *silence;
    ULONG smp_size;
    char path[2048];
    char rname[96], rtag[24];       /* a station's name and format ("128k") */
    UWORD rcpu;
    BOOL rtrack;                    /* not a station: a track (a Plex server's), with an end */
    ULONG rtotal_ms;                /*   and its length (0 not known) */
    ULONG rbase_ms, rrange;         /*   played from here (a seek: its time, its byte) */
    char rby[96];                   /*   its artist and album */
    char packer[40];                /* what it was packed with ("" none) */
    UBYTE engine;                   /* ENG_*: what plays it */
    UWORD tick_cnt;                 /* an EaglePlayer player's timer (CIA count) */
    ULONG med_units;                /* a MED song's length (medtime.c: ticks x 16), 0 none */
    UWORD med_cnt0;                 /* its timer at its first tempo (its first tick's) */
    UWORD med_tracks;               /* its tracks (a note player's mixer: four or fewer at full scale) */
    BOOL med_said;                  /* its length logged (once a song) */
    UWORD med_timer;                /* its first tempo's timer, from the module (a player's 0 put right) */
    ULONG ep_t0, ep_played;         /* ...and its time played, by the clock */
    /* playing */
    volatile BOOL running, paused;
    volatile WORD seek_to;          /* -1, or a position to go to on the next tick */
    volatile BYTE fin_left;         /* the song's end reached: its last row's ticks to go, */
    volatile BOOL finished;         /*   then quiet, until played again */
    volatile UBYTE phase;           /* the note start under way: 0 none, 1, 2 */
    volatile UWORD dma_mask, loop_mask;
    BYTE *loop_ptr[4];
    UWORD loop_len[4];
    UBYTE bpm_set;
    UWORD master;                   /* 0-64 */
    ULONG level[4];                 /* the meters (UI side) */
    /* the spectrum's view of Paula: the notes the tick started (ProTracker),
     * then each voice followed from there by its period (UI side) */
    volatile UBYTE vtrig;
    struct { const BYTE *start, *loop; UWORD len, replen; } vnew[4];
    struct { const BYTE *pos, *loop; LONG left, replen; } vf[4];
    ULONG vf_t;
    struct EpVoice epv[4];          /* an EaglePlayer player's voices (mp_status) */
    UBYTE ep_trig;
    BOOL ep_voices;                 /* ...it says where its samples are */
    ULONG serial;                   /* counts what has been loaded (the visualizations) */
    volatile ULONG ticks;           /* the tempo interrupt's count, playing */
    volatile ULONG caught;          /* ticks played to catch up (missed interrupts) */
    ULONG chk_t0, chk_n0, chk_c0, chk_said; /* mp_tempo_check's window */
    BOOL chk_first;                 /* the first check of a song: always logged */
    /* tick_owed's anchor: the E-clock (low 32 bits) and the ticks played
     * since then, at the count cnt */
    BOOL anc_on;
    ULONG anc_e, anc_done, anc_cnt;
    /* 5-8 channels: mixed (ptmix.c). The replayer's registers for its
     * eight channels; each tick's changes queued for the software
     * interrupt that mixes them into Paula's rings */
    BOOL mix8;
    struct PtMix *pm;
    BYTE *rings;                    /* chip: 4 x MIX_RING */
    BYTE *vt;                       /* 65 x 256 */
    struct { const pt_s8 *ptr; UWORD len, per; UBYTE vol; } vreg[PT_CHANS];
    UBYTE voff;                     /* DMA switched off this tick */
    struct PmTick q[MIX_Q];
    volatile UWORD qhead, qtail;
    struct Interrupt mix_int;
    ULONG mix_lost;                 /* ticks the queue had no room for */
} mp;

/* ------------------------------------------------------------ Paula */
static UWORD scaled(UBYTE v)
{
    if (mp.paused)
        return 0;
    return (UWORD)(((UWORD)v * mp.master) >> 6);
}

void pt_hw_period(int ch, pt_u16 p)
{
    if (mp.mix8)
        mp.vreg[ch].per = p;
    else
        CU->aud[ch].ac_per = p < 113 ? 113 : p;
}

void pt_hw_volume(int ch, pt_u8 v)
{
    if (mp.mix8)
        mp.vreg[ch].vol = v;
    else
        CU->aud[ch].ac_vol = scaled(v);
}

void pt_hw_data(int ch, const pt_s8 *ptr)
{
    if (mp.mix8)
        mp.vreg[ch].ptr = ptr;
    else
        CU->aud[ch].ac_ptr = (UWORD *)ptr;
}

void pt_hw_length(int ch, pt_u16 words)
{
    if (mp.mix8)
        mp.vreg[ch].len = words ? words : 1;
    else
        CU->aud[ch].ac_len = words ? words : 1;
}

void pt_hw_dma_off(pt_u16 mask)
{
    if (mp.mix8)
        mp.voff |= (UBYTE)mask;
    else
        CU->dmacon = mask & 0x0F;
}

/* ---- 5-8 channels: the mixing (ptmix.c) */
static UWORD tick_count(UBYTE bpm);
static void wait_lines(int n);

/* The software interrupt: each tick queued, applied and mixed. */
static void mix_code(void)
{
    while (mp.qtail != mp.qhead) {
        struct PmTick *t = &mp.q[mp.qtail];
        if (t->hold) {
            pm_mix_silence(mp.pm, t->samples);     /* paused: the voices held */
        } else {
            pm_apply(mp.pm, t);
            pm_mix(mp.pm, t->samples);
        }
        mp.qtail = (UWORD)((mp.qtail + 1) & (MIX_Q - 1));
    }
}

/* This tick's changes queued (in the tempo interrupt): hold, a paused
 * tick (silence, the voices where they are). */
static void mix_queue(BOOL hold)
{
    struct Pt *pt = mp.pt;
    UWORD next = (UWORD)((mp.qhead + 1) & (MIX_Q - 1));
    struct PmTick *t;
    int c;
    if (next == mp.qtail) {
        mp.mix_lost++;                      /* not mixed for a while: dropped */
        mp.voff = 0;
        return;
    }
    t = &mp.q[mp.qhead];
    t->samples = pm_tick_samples(mp.pm, tick_count(mp.bpm_set));
    t->hold = (pt_u8)hold;
    if (!hold) {
        t->off = mp.voff;
        t->on = (UBYTE)pt->dmacontemp;
        for (c = 0; c < pt->nch; c++) {
            t->start[c] = mp.vreg[c].ptr;
            t->slen[c] = mp.vreg[c].len;
            if (pt->loopmask & (1 << c)) {
                /* the loop, once the start is taken (at once: no Paula to wait for) */
                mp.vreg[c].ptr = pt->loop_ptr[c];
                mp.vreg[c].len = pt->loop_len[c] ? pt->loop_len[c] : 1;
            }
            t->ptr[c] = mp.vreg[c].ptr;
            t->len[c] = mp.vreg[c].len;
            t->per[c] = mp.vreg[c].per;
            t->vol[c] = mp.vreg[c].vol;
        }
    }
    mp.voff = 0;
    mp.qhead = next;
    Cause(&mp.mix_int);
}

/* An EaglePlayer note player's tick (eagle.c fills it, in the tempo
 * interrupt), for the mixer: a slot, NULL if the queue is full; then
 * queued and mixed. */
BOOL mp_mixing(void)
{
    return mp.mix8;
}

static UWORD mix_next;

struct PmTick *mp_mix_slot(void)
{
    struct PmTick *t;
    if (!mp.mix8)
        return NULL;
    mix_next = (UWORD)((mp.qhead + 1) & (MIX_Q - 1));
    if (mix_next == mp.qtail) {
        mp.mix_lost++;
        return NULL;
    }
    t = &mp.q[mp.qhead];
    t->samples = pm_tick_samples(mp.pm, mp.tick_cnt ? mp.tick_cnt : 14187);
    t->hold = 0;
    return t;
}

void mp_mix_push(void)
{
    mp.qhead = mix_next;
    Cause(&mp.mix_int);
}

/* Paula's four channels on the rings, from their start, the next tick
 * written a lead ahead (some 120 ms: more than the slowest tempo's tick). */
static void mix_rings_start(void)
{
    int i;
    Disable();
    CU->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    mp.qhead = mp.qtail = 0;
    pm_ring_start(mp.pm, mp.pm->rate * 12 / 100);
    Enable();
    wait_lines(3);
    CU->adkcon = 0x00FF;                    /* no channel modulating another */
    for (i = 0; i < 4; i++) {
        CU->aud[i].ac_ptr = (UWORD *)(mp.rings + i * MIX_RING);
        CU->aud[i].ac_len = MIX_RING / 2;
        CU->aud[i].ac_per = mp.pm->period;
        CU->aud[i].ac_vol = scaled(64);
    }
    CU->dmacon = DMAF_SETCLR | DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
}

/* The rings' volume: the master's (0 paused). */
static void mix_volume(void)
{
    int i;
    for (i = 0; i < 4; i++)
        CU->aud[i].ac_vol = scaled(64);
}

static void mix_free(void)
{
    Disable();
    mp.mix8 = FALSE;
    mp.qhead = mp.qtail = 0;
    Enable();
    if (mp.rings)
        FreeMem(mp.rings, 4 * MIX_RING);
    if (mp.vt)
        FreeVec(mp.vt);
    if (mp.pm)
        FreeVec(mp.pm);
    mp.rings = NULL;
    mp.vt = NULL;
    mp.pm = NULL;
}

/* Set up for a module of nch (5-8) channels: FALSE without the memory. */
static BOOL mix_alloc(int nch)
{
    BYTE *r[4];
    int i;
    /* 28.6 kHz on a 68030 or better, 22 kHz on a 68020 (as ProTracker 4.1) */
    UWORD period = (SysBase->AttnFlags & (AFF_68030 | AFF_68040)) ? 124 : 161;
    mp.rings = AllocMem(4 * MIX_RING, MEMF_CHIP | MEMF_CLEAR);
    mp.vt = AllocVec(65 * 256, MEMF_ANY);
    mp.pm = AllocVec(sizeof(struct PtMix), MEMF_ANY | MEMF_CLEAR);
    if (!mp.rings || !mp.vt || !mp.pm) {
        mix_free();
        return FALSE;
    }
    pm_voltab((pt_s8 *)mp.vt);
    if (nch <= 4) {
        /* (a note player of four: nothing paired, so each voice full scale) */
        int v, x;
        for (v = 0; v <= 64; v++)
            for (x = 0; x < 256; x++)
                mp.vt[v * 256 + x] = (BYTE)(((int)(BYTE)x * v) >> 6);
    }
    for (i = 0; i < 4; i++)
        r[i] = mp.rings + i * MIX_RING;
    pm_init(mp.pm, nch, period, (pt_s8 *)mp.vt, (pt_s8 *const *)r, MIX_RING);
    mp.mix_int.is_Node.ln_Type = NT_INTERRUPT;
    mp.mix_int.is_Node.ln_Pri = 0;
    mp.mix_int.is_Node.ln_Name = (char *)"AmigaMCE music (8 channels)";
    mp.mix_int.is_Data = NULL;
    mp.mix_int.is_Code = (void (*)())mix_code;
    mp.mix8 = TRUE;
    return TRUE;
}

static void set_loops(void)
{
    int i;
    for (i = 0; i < 4; i++)
        if (mp.loop_mask & (1 << i)) {
            CU->aud[i].ac_ptr = (UWORD *)mp.loop_ptr[i];
            CU->aud[i].ac_len = mp.loop_len[i] ? mp.loop_len[i] : 1;
        }
    mp.loop_mask = 0;
}

/* Wait about n raster lines (64 us each) on the beam. */
static void wait_lines(int n)
{
    volatile UBYTE *vpos = (volatile UBYTE *)0xDFF006;
    while (n-- > 0) {
        UBYTE v = *vpos;
        int guard = 2000;
        while (*vpos == v && --guard)
            ;
    }
}

static BOOL vol_set;                /* a volume chosen (0 is one) */

/* ------------------------------------------------------------ timers */
static UWORD tick_count(UBYTE bpm)
{
    ULONG c = (mp.eclock * 5) / ((ULONG)bpm * 2);     /* 2.5 s / bpm */
    return (UWORD)(c > 65535 ? 65535 : c);
}

static void tick_latch(UWORD c)
{
    if (mp.tick_bit == CIAICRB_TA) {
        CIAREG(mp.cia_base, R_TALO) = (UBYTE)c;
        CIAREG(mp.cia_base, R_TAHI) = (UBYTE)(c >> 8);
    } else {
        CIAREG(mp.cia_base, R_TBLO) = (UBYTE)c;
        CIAREG(mp.cia_base, R_TBHI) = (UBYTE)(c >> 8);
    }
}

/* The gap timer (always timer B when the tick has A), once. */
static void gap_start(void)
{
    UWORD c = (UWORD)(mp.eclock * GAP_US / 1000000UL);
    CIAREG(mp.cia_base, R_CRB) = (UBYTE)(CIAREG(mp.cia_base, R_CRB) & 0x80);
    CIAREG(mp.cia_base, R_TBLO) = (UBYTE)c;
    CIAREG(mp.cia_base, R_TBHI) = (UBYTE)(c >> 8);
    CIAREG(mp.cia_base, R_CRB) =
        (UBYTE)((CIAREG(mp.cia_base, R_CRB) & 0x80) | CIACRBF_LOAD | CIACRBF_RUNMODE | CIACRBF_START);
}

/* ------------------------------------------------------------ interrupts */
static void gap_code(void)
{
    if (mp.phase == 1) {
        CU->dmacon = DMAF_SETCLR | (mp.dma_mask & 0x0F);
        mp.phase = 2;
        gap_start();
    } else if (mp.phase == 2) {
        set_loops();
        mp.phase = 0;
    }
}

/* How many ticks this interrupt should play: by the E-clock, the ticks
 * due since the anchor less those played. Normally 1; more when
 * interrupts were missed or came late (RTG screens under some emulators
 * lose CIA-B's), so the tempo stays right whatever happens to them. */
static UWORD tick_owed(void)
{
    struct EClockVal ev;
    ULONG cnt = mp.engine == ENG_EAGLE ? mp.tick_cnt : tick_count(mp.bpm_set), el, due;
    UWORD n = 1;
    if (!TimerBase || !cnt || mp.paused || mp.finished) {
        mp.anc_on = FALSE;
        return 1;
    }
    ReadEClock(&ev);
    if (!mp.anc_on || cnt != mp.anc_cnt) {
        mp.anc_on = TRUE;                   /* from here (a new tempo, a resume...) */
        mp.anc_e = ev.ev_lo;
        mp.anc_done = 0;
        mp.anc_cnt = cnt;
        return 1;
    }
    el = ev.ev_lo - mp.anc_e;
    due = el / cnt;
    mp.anc_done++;                          /* this one */
    if (due > mp.anc_done) {
        n = (UWORD)(due - mp.anc_done + 1);
        if (n > 4) {
            n = 4;                          /* a long stall: not all at once */
            mp.anc_on = FALSE;
        }
        mp.anc_done += n - 1;
        mp.caught += n - 1;
    } else if (mp.anc_done > due + 2 || el > 0x40000000UL) {
        mp.anc_on = FALSE;                  /* drifted, or a long while: anew */
    }
    return n;
}

static void tick_one(void);
static ULONG med_ms(ULONG units16);

static void tick_code(void)
{
    UWORD n;
    if (!mp.running)
        return;
    if (!mp.paused)
        mp.ticks++;
    n = tick_owed();
    while (n--)
        tick_one();
}

static void tick_one(void)
{
    struct Pt *pt = mp.pt;
    int i;
    if (mp.engine == ENG_EAGLE) {
        if (!mp.paused) {
            ep_tick();                      /* the player's own routine */
            if (!mp.med_cnt0)
                mp.med_cnt0 = mp.tick_cnt;  /* (the song's first tempo, as it set it) */
        }
        return;
    }
    if (!pt)
        return;
    if (mp.phase) {
        /* the last note start not finished (a very fast tempo): finish it */
        if (mp.phase == 1)
            CU->dmacon = DMAF_SETCLR | (mp.dma_mask & 0x0F);
        set_loops();
        mp.phase = 0;
    }
    if (mp.seek_to >= 0) {
        WORD to = mp.seek_to;
        mp.seek_to = -1;
        if (mp.mix8) {
            mp.voff = 0xFF;                 /* every channel stopped (the rings play on) */
        } else {
            CU->dmacon = 0x0F;
            for (i = 0; i < 4; i++)
                CU->aud[i].ac_vol = 0;
        }
        if (mp.times && mp.times->at_ms[to] != 0xFFFFFFFFUL) {
            pt_restart(pt, to, mp.times->speed[to], mp.times->bpm[to]);
            pt->elapsed_us = mp.times->at_ms[to] * 1000;
        } else {
            pt_restart(pt, to, pt->speed, pt->bpm);
        }
        mp.fin_left = -1;
        mp.finished = FALSE;
    }
    if (mp.paused || mp.finished) {
        if (mp.mix8)
            mix_queue(TRUE);                /* the rings fed silence, the voices held */
        return;
    }
    if (mp.fin_left >= 0) {
        /* the song is over once its last row has played out */
        if (mp.fin_left == 0) {
            mp.finished = TRUE;
            if (mp.mix8) {
                mp.voff = 0xFF;
                mix_queue(FALSE);
                return;
            }
            CU->dmacon = 0x0F;
            for (i = 0; i < 4; i++)
                CU->aud[i].ac_vol = 0;
            return;
        }
        mp.fin_left--;
    }
    pt_tick(pt);
    if (pt->ended && mp.fin_left < 0)
        mp.fin_left = (BYTE)(pt->ended == 2 ? 0 : pt->speed - 1);
    if (pt->bpm != mp.bpm_set) {
        mp.bpm_set = pt->bpm;
        tick_latch(tick_count(pt->bpm));    /* from the next tick */
    }
    if (mp.mix8) {
        /* 5-8 channels: no Paula to wait for, the tick's changes mixed */
        for (i = 0; i < 4; i++)
            if (pt->dmacontemp & (1 << i)) {
                mp.vnew[i].start = (const BYTE *)pt->ch[i].n_start;
                mp.vnew[i].len = pt->ch[i].n_length;
                mp.vnew[i].loop = (const BYTE *)pt->ch[i].n_loopstart;
                mp.vnew[i].replen = pt->ch[i].n_replen;
            }
        mp.vtrig |= (UBYTE)(pt->dmacontemp & 0x0F);
        mix_queue(FALSE);
        return;
    }
    if (pt->loopmask) {
        for (i = 0; i < 4; i++) {
            mp.loop_ptr[i] = pt->loop_ptr[i];
            mp.loop_len[i] = pt->loop_len[i];
        }
        mp.loop_mask = pt->loopmask;
    }
    mp.dma_mask = pt->dmacontemp;
    if (mp.dma_mask) {
        for (i = 0; i < 4; i++)
            if (mp.dma_mask & (1 << i)) {
                /* a note starts: for the spectrum, where its sample is */
                mp.vnew[i].start = (const BYTE *)pt->ch[i].n_start;
                mp.vnew[i].len = pt->ch[i].n_length;
                mp.vnew[i].loop = (const BYTE *)pt->ch[i].n_loopstart;
                mp.vnew[i].replen = pt->ch[i].n_replen;
            }
        mp.vtrig |= (UBYTE)mp.dma_mask;
        if (mp.have_gap) {
            mp.phase = 1;
            gap_start();
        } else {
            wait_lines(9);
            CU->dmacon = DMAF_SETCLR | (mp.dma_mask & 0x0F);
            wait_lines(9);
            set_loops();
        }
    } else if (mp.loop_mask) {
        set_loops();                        /* nothing starting: at once */
    }
}

/* ------------------------------------------------------------ setup */
static BOOL take_timer(UBYTE bit, struct Interrupt *in, void (*code)(void), const char *name)
{
    in->is_Node.ln_Type = NT_INTERRUPT;
    in->is_Node.ln_Pri = 0;
    in->is_Node.ln_Name = (char *)name;
    in->is_Data = NULL;
    in->is_Code = (void (*)())code;
    return AddICRVector(mp.cia, bit, in) == NULL;
}

static void timers_stop(void)
{
    if (!mp.cia)
        return;
    CIAREG(mp.cia_base, R_CRA) = (UBYTE)(CIAREG(mp.cia_base, R_CRA) & 0xC0);
    if (mp.have_gap || mp.tick_bit == CIAICRB_TB)
        CIAREG(mp.cia_base, R_CRB) = (UBYTE)(CIAREG(mp.cia_base, R_CRB) & 0x80);
}

/* The CIA timers: B's resource first (players' custom), then A's. */
static BOOL timers_open(void)
{
    static const char *const res[2] = { CIABNAME, CIAANAME };
    static const ULONG base[2] = { CIAB_BASE, CIAA_BASE };
    int k;
    mp.eclock = SysBase->ex_EClockFrequency ? SysBase->ex_EClockFrequency : 709379;
    for (k = 0; k < 2; k++) {
        mp.cia = (struct Library *)OpenResource((CONST_STRPTR)res[k]);
        if (!mp.cia)
            continue;
        mp.cia_base = base[k];
        Disable();
        if (take_timer(CIAICRB_TA, &mp.tick_int, tick_code, "AmigaMCE music")) {
            mp.have_tick = TRUE;
            mp.tick_bit = CIAICRB_TA;
            CIAREG(mp.cia_base, R_CRA) = (UBYTE)(CIAREG(mp.cia_base, R_CRA) & 0xC0);
            if (take_timer(CIAICRB_TB, &mp.gap_int, gap_code, "AmigaMCE music (notes)")) {
                mp.have_gap = TRUE;
                CIAREG(mp.cia_base, R_CRB) = (UBYTE)(CIAREG(mp.cia_base, R_CRB) & 0x80);
            }
        } else if (take_timer(CIAICRB_TB, &mp.tick_int, tick_code, "AmigaMCE music")) {
            mp.have_tick = TRUE;
            mp.tick_bit = CIAICRB_TB;
            CIAREG(mp.cia_base, R_CRB) = (UBYTE)(CIAREG(mp.cia_base, R_CRB) & 0x80);
        }
        Enable();
        if (mp.have_tick) {
            char m[96];
            sprintf(m, "CIA-%s timer %s for the tempo%s", k == 0 ? "B" : "A",
                    mp.tick_bit == CIAICRB_TA ? "A" : "B",
                    mp.have_gap ? ", timer B for note starts" : ", note starts timed on the beam");
            amce_log("music", m);
            return TRUE;
        }
    }
    mp.cia = NULL;
    return FALSE;
}

static void timers_close(void)
{
    if (!mp.cia)
        return;
    timers_stop();
    if (mp.have_gap)
        RemICRVector(mp.cia, CIAICRB_TB, &mp.gap_int);
    if (mp.have_tick)
        RemICRVector(mp.cia, mp.tick_bit, &mp.tick_int);
    mp.have_gap = mp.have_tick = FALSE;
    mp.cia = NULL;
}

static void tick_run(BOOL on)
{
    if (!mp.have_tick)
        return;
    if (mp.tick_bit == CIAICRB_TA) {
        CIAREG(mp.cia_base, R_CRA) = (UBYTE)(CIAREG(mp.cia_base, R_CRA) & 0xC0);
        if (on) {
            tick_latch(mp.engine == ENG_EAGLE ? mp.tick_cnt : tick_count(mp.bpm_set));
            CIAREG(mp.cia_base, R_CRA) =
                (UBYTE)((CIAREG(mp.cia_base, R_CRA) & 0xC0) | CIACRAF_LOAD | CIACRAF_START);
        }
    } else {
        CIAREG(mp.cia_base, R_CRB) = (UBYTE)(CIAREG(mp.cia_base, R_CRB) & 0x80);
        if (on) {
            tick_latch(mp.engine == ENG_EAGLE ? mp.tick_cnt : tick_count(mp.bpm_set));
            CIAREG(mp.cia_base, R_CRB) =
                (UBYTE)((CIAREG(mp.cia_base, R_CRB) & 0x80) | CIACRBF_LOAD | CIACRBF_START);
        }
    }
}

/* Once a second or so: whether the tempo interrupt comes as often as it
 * should (by the E-clock, the same clock its timer counts). Said in the
 * log when it is more than 10% off, at most twice a minute. */
void mp_tempo_check(void)
{
    ULONG now = time_ms(), n = mp.ticks, el, cnt, hz10, want, got, caught;
    if (!mp.running || mp.paused || mp.engine == ENG_MIX || mp.engine == ENG_RADIO ||
        !mp.have_tick) {
        mp.chk_t0 = 0;
        return;
    }
    if (!mp.chk_t0) {
        mp.chk_t0 = now ? now : 1;
        mp.chk_n0 = n;
        mp.chk_c0 = mp.caught;
        return;
    }
    el = now - mp.chk_t0;
    if (el < 5000)
        return;
    cnt = mp.engine == ENG_EAGLE ? mp.tick_cnt : tick_count(mp.bpm_set);
    got = n - mp.chk_n0;
    caught = mp.caught - mp.chk_c0;
    mp.chk_t0 = now ? now : 1;
    mp.chk_n0 = n;
    mp.chk_c0 = mp.caught;
    if (!cnt)
        return;
    hz10 = mp.eclock * 10 / cnt;            /* ticks a second, x10 */
    want = el * hz10 / 10000;
    if (!want)
        return;
    if (!mp.chk_first && got * 10 >= want * 9 && got * 10 <= want * 11)
        return;
    if (!mp.chk_first && mp.chk_said && now - mp.chk_said < 30000)
        return;
    mp.chk_first = FALSE;
    mp.chk_said = now ? now : 1;
    {
        char m[160];
        sprintf(m, "the tempo interrupt came %ld times in %ld ms, of %ld (%ld%%); %ld ticks caught up",
                (long)got, (long)el, (long)want, (long)(got * 100 / want), (long)caught);
        amce_log("music", m);
        if (mp.engine == ENG_EAGLE) {
            ep_note_log();                  /* (a note player's first notes, once) */

        }
    }
}

/* For EaglePlayer players: their timer (callable from the interrupt). */
void mp_tick_count(UWORD c)
{
    /* 0 is no tempo (a CIA timer of 0 underflows at once, again and
     * again): MED_note sets it for some songs (Pure Aggression); the
     * song's own first tempo's timer then, else 50 Hz's */
    if (!c)
        c = mp.med_timer ? mp.med_timer : 14187;
    mp.tick_cnt = c;
    if (mp.have_tick && mp.running)
        tick_latch(c);
}

void mp_tick_run(BOOL on)
{
    tick_run(on);
}

/* Stopped: DMA off, volumes 0, and each channel pointed at a word of
 * silence of our own, its length one. A channel left pointing at a buffer
 * that has been freed (a stream's, the mixer's) would play whatever chip
 * memory comes to be there, pictures and all, should its DMA come on. */
static UWORD *quiet_word;           /* chip: a few words of 0 */

static void paula_quiet(void)
{
    int i;
    CU->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    for (i = 0; i < 4; i++)
        CU->aud[i].ac_vol = 0;
    if (!quiet_word)
        quiet_word = AllocMem(8, MEMF_CHIP | MEMF_CLEAR);
    wait_lines(3);                          /* the DMA's last fetches done */
    for (i = 0; i < 4; i++) {
        if (quiet_word)
            CU->aud[i].ac_ptr = quiet_word;
        CU->aud[i].ac_len = 1;
        CU->aud[i].ac_dat = 0;
    }
}

/* Once a second, nothing playing: what Paula is doing, said in the log
 * when it changes (audio DMA, audio interrupts enabled and waiting,
 * modulation), so a sound heard after Stop can be put to something. The
 * channels are quietened only while they are ours (audio.device); given
 * back, they are another program's to use. */
void mp_paula_check(void)
{
    static ULONG last = 0xFFFFFFFFUL;
    UWORD dma = CU->dmaconr & 0x0F, ien = (CU->intenar >> 7) & 0x0F;
    UWORD irq = (CU->intreqr >> 7) & 0x0F, adk = CU->adkconr & 0xFF;
    ULONG now = ((ULONG)dma << 24) | ((ULONG)ien << 16) | ((ULONG)irq << 8) | adk;
    if (mp.running) {
        last = 0xFFFFFFFFUL;                /* playing: said again after the next stop */
        return;
    }
    if (now != last) {
        char m[160];
        sprintf(m, "stopped; Paula: audio DMA %lx, audio interrupts on %lx, waiting %lx, "
                   "modulation %lx; channels %s",
                (unsigned long)dma, (unsigned long)ien, (unsigned long)irq, (unsigned long)adk,
                mp.audio_open ? "ours" : "given back");
        amce_log("music", m);
        last = now;
    }
    if (dma && mp.audio_open)
        paula_quiet();
}

/* Paula left as a quiet machine for whoever comes next: DMA off, the
 * channels finished with their last word, volumes and data zero, no
 * modulation, no audio interrupts waiting. Nothing points at our memory
 * that matters: a channel whose DMA is off reads none. */
static void paula_reset(void)
{
    int i;
    CU->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    for (i = 0; i < 4; i++)
        CU->aud[i].ac_vol = 0;
    wait_lines(3);                          /* the DMA's last fetches done */
    for (i = 0; i < 4; i++) {
        CU->aud[i].ac_dat = 0;              /* the output: 0, not its last sample */
        CU->aud[i].ac_len = 1;
        CU->aud[i].ac_per = 428;
        CU->aud[i].ac_vol = 0;
    }
    CU->adkcon = 0x00FF;                    /* no channel modulates another */
    CU->intreq = INTF_AUD0 | INTF_AUD1 | INTF_AUD2 | INTF_AUD3;
}

static BOOL audio_open(void)
{
    static UBYTE all[] = { 0x0F };
    if (mp.audio_open)
        return TRUE;
    if (!(mp.port = CreateMsgPort()))
        return FALSE;
    if (!(mp.aio = (struct IOAudio *)CreateIORequest(mp.port, sizeof(struct IOAudio)))) {
        DeleteMsgPort(mp.port);
        mp.port = NULL;
        return FALSE;
    }
    mp.aio->ioa_Request.io_Message.mn_Node.ln_Pri = 127;    /* none can take them from us */
    mp.aio->ioa_Request.io_Flags = ADIOF_NOWAIT;
    mp.aio->ioa_AllocKey = 0;
    mp.aio->ioa_Data = all;
    mp.aio->ioa_Length = sizeof(all);
    if (OpenDevice((CONST_STRPTR)AUDIONAME, 0, (struct IORequest *)mp.aio, 0)) {
        DeleteIORequest((struct IORequest *)mp.aio);
        DeleteMsgPort(mp.port);
        mp.aio = NULL;
        mp.port = NULL;
        return FALSE;
    }
    mp.audio_open = TRUE;
    {
        static BOOL said;
        if (!said)
            amce_log("music", "Paula's 4 channels, from audio.device (not AHI); given back on Stop");
        said = TRUE;
    }
    return TRUE;
}

static void audio_close(void)
{
    if (!mp.audio_open)
        return;
    paula_reset();
    CloseDevice((struct IORequest *)mp.aio);
    DeleteIORequest((struct IORequest *)mp.aio);
    DeleteMsgPort(mp.port);
    mp.aio = NULL;
    mp.port = NULL;
    mp.audio_open = FALSE;
}

/* ------------------------------------------------------------ the module */
static void module_free(void)
{
    if (mp.engine == ENG_EAGLE)
        ep_unload();
    if (mp.engine == ENG_MIX)
        mx_unload();
    if (mp.engine == ENG_RADIO)
        st_stop();
    mix_free();
    mp.med_units = 0;
    mp.med_tracks = 0;
    mp.med_timer = 0;
    mp.engine = ENG_NONE;
    mp.serial++;
    mp.vtrig = mp.ep_trig = 0;
    mp.ep_voices = FALSE;
    memset(mp.vf, 0, sizeof(mp.vf));
    memset(mp.epv, 0, sizeof(mp.epv));
    if (mp.pt) {
        FreeVec(mp.pt);
        mp.pt = NULL;
    }
    if (mp.times) {
        FreeVec(mp.times);
        mp.times = NULL;
    }
    if (mp.hdr) {
        FreeVec(mp.hdr);
        mp.hdr = NULL;
    }
    if (mp.pat) {
        FreeVec(mp.pat);
        mp.pat = NULL;
    }
    if (mp.smp) {
        FreeMem(mp.smp, mp.smp_size);
        mp.smp = NULL;
    }
    mp.path[0] = 0;
}

static const char *layout_problem(int rc, int channels)
{
    static char m[128];
    switch (rc) {
    case PT_ERR_SHORT:
        return _("The module file is cut short");
    case PT_ERR_CHANNELS:
        sprintf(m, _("%ld-channel modules need more than Paula's four"), (long)channels);
        return m;
    default:
        return NULL;
    }
}

/* A format of the PC's trackers: mixed (mixplay.c) rather than tried on
 * the EaglePlayer players first. */
static BOOL pc_format(const UBYTE *h, ULONG n)
{
    if (n >= 17 && !memcmp(h, "Extended Module: ", 17))
        return TRUE;                        /* XM */
    if (n >= 48 && !memcmp(h + 44, "SCRM", 4))
        return TRUE;                        /* S3M */
    if (n >= 4 && !memcmp(h, "IMPM", 4))
        return TRUE;                        /* IT */
    if (n >= 3 && !memcmp(h, "MTM", 3))
        return TRUE;
    if (n >= 2 && (!memcmp(h, "if", 2) || !memcmp(h, "JN", 2)))
        return TRUE;                        /* 669 */
    if (n >= 14 && !memcmp(h, "MAS_UTrack_V00", 14))
        return TRUE;                        /* ULT */
    if (n >= 28 && !memcmp(h + 20, "!Scream!", 8))
        return TRUE;                        /* STM */
    if (n >= 4 && !memcmp(h, "FAR\xFE", 4))
        return TRUE;
    return FALSE;
}

/* What a file that is not a module seems to be, for the message. */
static const char *packed_kind(const UBYTE *h, LONG n)
{
    if (n >= 4 && !memcmp(h, "PP20", 4))
        return _("packed with PowerPacker");
    if (n >= 4 && !memcmp(h, "XPKF", 4))
        return _("packed with XPK");
    if (n >= 4 && (!memcmp(h, "ICE!", 4) || !memcmp(h, "Ice!", 4)))
        return _("packed with Pack-Ice");
    if (n >= 4 && (!memcmp(h, "MMD0", 4) || !memcmp(h, "MMD1", 4) || !memcmp(h, "MMD2", 4) ||
                   !memcmp(h, "MMD3", 4)))
        return _("a MED/OctaMED module");
    if (n >= 17 && !memcmp(h, "Extended Module: ", 17))
        return _("a FastTracker 2 module (XM)");
    if (n >= 48 && !memcmp(h + 44, "SCRM", 4))
        return _("a Scream Tracker 3 module (S3M)");
    if (n >= 4 && !memcmp(h, "IMPM", 4))
        return _("an Impulse Tracker module (IT)");
    if (n >= 4 && !memcmp(h, "THX", 3))
        return _("an AHX module");
    return NULL;
}

/* Read n bytes at off, from the file (fh) or from memory (mem, mlen):
 * what is not there stays as it was (zero). */
static void src_read(BPTR fh, const UBYTE *mem, ULONG mlen, ULONG off, APTR dst, ULONG n)
{
    if (fh) {
        Seek(fh, off, OFFSET_BEGINNING);
        Read(fh, dst, n);
    } else if (off < mlen) {
        CopyMem((APTR)(mem + off), dst, off + n <= mlen ? n : mlen - off);
    }
}

BOOL mp_load(const char *path, char *err, LONG errmax)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    UBYTE head[1084];
    LONG got, size;
    BPTR fh;
    int rc, chans;
    const char *why = NULL;
    char m[300], packer[40];
    UBYTE *mem = NULL;              /* the file unpacked, when it was packed */
    ULONG mlen = 0;
    ULONG t0 = time_ms();

    err[0] = 0;
    packer[0] = 0;
    mp_stop();
    module_free();
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh) {
        amce_strlcpy(err, _("Could not open the module file"), errmax);
        return FALSE;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    got = Read(fh, head, sizeof(head));
    if (got < 0)
        got = 0;
    if (got < (LONG)sizeof(head))
        memset(head + got, 0, sizeof(head) - got);
    rc = pt_layout(head, (ULONG)got, (ULONG)size, &mp.lay, &chans);
    if (rc != PT_OK && rc != PT_ERR_CHANNELS) {
        /* packed? unpacked whole (xfdmaster), then looked at again */
        const char *uwhy = NULL;
        Close(fh);
        fh = 0;
        if ((mem = unpack_read_whole(path, &mlen)))
            mem = unpack_buffer(mem, &mlen, packer, sizeof(packer), &uwhy);
        if (mem && packer[0]) {
            got = mlen < sizeof(head) ? (LONG)mlen : (LONG)sizeof(head);
            memset(head, 0, sizeof(head));
            CopyMem(mem, head, got);
            size = (LONG)mlen;
            rc = pt_layout(head, (ULONG)got, mlen, &mp.lay, &chans);
        }
        if (uwhy) {
            sprintf(m, _("Packed with %s, but %s"), packer[0] ? packer : _("a packer"), uwhy);
            amce_strlcpy(err, m, errmax);
            FreeVec(mem);
            return FALSE;
        }
    }
    if (rc != PT_OK) {
        /* not ProTracker: EaglePlayer's players (eagle.c), given the file */
        char eerr[100];
        const char *pk = packer[0] ? NULL : packed_kind(head, got);
        if (fh)
            Close(fh);
        fh = 0;
        if (!mem && (mem = unpack_read_whole(path, &mlen))) {
            const char *uwhy = NULL;
            mem = unpack_buffer(mem, &mlen, packer, sizeof(packer), &uwhy);
        }
        eerr[0] = 0;
        if (mem && (rc == PT_ERR_CHANNELS || pc_format(mem, mlen))) {
            /* a format of the PC's trackers (or more than 4 channels): mixed */
            UBYTE *copy = AllocVec(mlen + 16, MEMF_ANY);
            if (copy)
                CopyMem(mem, copy, mlen);
            if (copy && mx_load(path, copy, mlen, eerr, sizeof(eerr))) {
                FreeVec(mem);
                mp.engine = ENG_MIX;
                amce_strlcpy(mp.path, path, sizeof(mp.path));
                amce_strlcpy(mp.packer, packer, sizeof(mp.packer));
                return TRUE;
            }
        }
        if (mem) {
            UBYTE *copy = mx_available() ? AllocVec(mlen + 16, MEMF_ANY) : NULL;
            if (copy)
                CopyMem(mem, copy, mlen);   /* for the mixer, if no player knows it */
            {
                struct MedTime mt;          /* (a MED song's length, before it is relocated) */
                mp.med_units = med_measure(mem, mlen, &mt);
                mp.med_tracks = (UWORD)mt.tracks;
                mp.med_timer = (UWORD)(mt.timer < 65536UL ? mt.timer : 0);
                if (mlen >= 4 && mem[0] == 'M' && mem[1] == 'M' && mem[2] == 'D') {
                    char kind[2];
                    kind[0] = (char)mem[3];
                    kind[1] = 0;
                    sprintf(m, "%s: MMD%s, %ld positions, %ld tracks, first tempo %lu: %lu ticks",
                            FilePart((STRPTR)path), kind, (long)mt.positions, (long)mt.tracks,
                            (unsigned long)mt.tempo, (unsigned long)(mp.med_units / 16));
                    amce_log("music", m);
                }
            }
            /* a MED song the mixer plays: only MED's players asked first */
            if (ep_load(path, mem, mlen,
                        copy && mlen >= 4 && mem[0] == 'M' && mem[1] == 'M' && mem[2] == 'D' &&
                            mem[3] >= '0' && mem[3] <= '3',
                        eerr, sizeof(eerr))) {
                FreeVec(copy);
                mp.engine = ENG_EAGLE;
                amce_strlcpy(mp.path, path, sizeof(mp.path));
                amce_strlcpy(mp.packer, packer, sizeof(mp.packer));
                sprintf(m, "%s: by an EaglePlayer player%s%s (found in %ld ms)",
                        FilePart((STRPTR)path), packer[0] ? ", unpacked from " : "", packer,
                        (long)(time_ms() - t0));
                amce_log("music", m);
                return TRUE;
            }
            mem = NULL;                     /* ep_load has freed it */
            if (copy) {
                char xerr[100];
                if (mx_load(path, copy, mlen, xerr, sizeof(xerr))) {
                    mp.engine = ENG_MIX;
                    amce_strlcpy(mp.path, path, sizeof(mp.path));
                    amce_strlcpy(mp.packer, packer, sizeof(mp.packer));
                    return TRUE;
                }
            }
        }
        if ((why = layout_problem(rc, chans)))
            amce_strlcpy(m, why, sizeof(m));
        else if (pk && !eerr[0])
            sprintf(m, _("This file is %s, which AmigaMCE cannot play"), pk);
        else if (eerr[0])
            amce_strlcpy(m, eerr, sizeof(m));
        else
            amce_strlcpy(m, _("Not a module AmigaMCE can play"), sizeof(m));
        amce_strlcpy(err, m, errmax);
        return FALSE;
    }
    mp.hdr = AllocVec(mp.lay.hdr_len, MEMF_ANY);
    mp.pat = AllocVec(mp.lay.pat_len ? mp.lay.pat_len : 4, MEMF_ANY | MEMF_CLEAR);
    mp.smp_size = mp.lay.smp_len + 4;
    mp.smp = AllocMem(mp.smp_size, MEMF_CHIP | MEMF_CLEAR);
    mp.pt = AllocVec(sizeof(struct Pt), MEMF_ANY | MEMF_CLEAR);
    mp.times = AllocVec(sizeof(struct PtTimes), MEMF_ANY | MEMF_CLEAR);
    if (!mp.silence)
        mp.silence = AllocMem(8, MEMF_CHIP | MEMF_CLEAR);
    if (!mp.hdr || !mp.pat || !mp.smp || !mp.pt || !mp.times || !mp.silence) {
        if (fh)
            Close(fh);
        FreeVec(mem);
        if (!mp.smp && mp.hdr && mp.pat && mp.pt) {
            sprintf(m, _("Not enough chip memory for its samples (%lu KB)"),
                    (unsigned long)((mp.lay.smp_len + 1023) >> 10));
            amce_strlcpy(err, m, errmax);
        } else {
            amce_strlcpy(err, _("Not enough memory for that module"), errmax);
        }
        if (!mp.smp)
            mp.smp_size = 0;
        module_free();
        return FALSE;
    }
    CopyMem(head, mp.hdr, mp.lay.hdr_len);
    /* short: the rest stays empty (patterns) or silent (samples) */
    src_read(fh, mem, mlen, mp.lay.hdr_len, mp.pat, mp.lay.pat_len);
    if (mp.lay.smp_len)
        src_read(fh, mem, mlen, mp.lay.hdr_len + mp.lay.pat_len, mp.smp, mp.lay.smp_len);
    if (fh)
        Close(fh);
    FreeVec(mem);
    CacheClearU();
    pt_init(mp.pt, &mp.lay, mp.hdr, mp.pat, (pt_s8 *)mp.smp, (pt_s8 *)mp.silence);
    pt_measure(mp.pt, mp.times);
    if (mp.pt->nch > 4) {
        /* 5-8 channels: mixed onto Paula's four (ProTracker 4.1's way) */
        if (!mix_alloc(mp.pt->nch)) {
            amce_strlcpy(err, _("Not enough memory for that module"), errmax);
            module_free();
            return FALSE;
        }
        memset(mp.vreg, 0, sizeof(mp.vreg));
        mp.voff = 0;
        mp.mix_lost = 0;
        sprintf(m, "%s: %ld channels, mixed in pairs onto Paula's four at %lu Hz "
                   "(the mixer of ProTracker 4.1, by Andreas Wendorf)",
                FilePart((STRPTR)path), (long)mp.pt->nch, (unsigned long)mp.pm->rate);
        amce_log("music", m);
    }
    mp.engine = ENG_PT;
    amce_strlcpy(mp.path, path, sizeof(mp.path));
    amce_strlcpy(mp.packer, packer, sizeof(mp.packer));
    sprintf(m, "%s: \"%s\" %s%s%s, %lu positions, %lu patterns, %lu KB of samples, %lu:%02lu long "
               "(read and measured in %ld ms)",
            FilePart((STRPTR)path), mp.pt->title, mp.lay.tag, packer[0] ? ", unpacked from " : "",
            packer, (unsigned long)mp.lay.songlen,
            (unsigned long)mp.lay.npat, (unsigned long)(mp.lay.smp_len >> 10),
            (unsigned long)(mp.times->total_ms / 60000), (unsigned long)(mp.times->total_ms / 1000 % 60),
            (long)(time_ms() - t0));
    amce_log("music", m);
    return TRUE;
}

/* ------------------------------------------------------------ playing */
static BOOL play_eagle(char *err, LONG errmax)
{
    UWORD cnt = 0;
    BOOL tick;
    if (ep_own_timer()) {
        timers_close();                     /* it takes a CIA timer of its own */
    } else if (!mp.cia && !timers_open()) {
        amce_strlcpy(err, _("No CIA timer free to play music with"), errmax);
        return FALSE;
    }
    Disable();
    mp.running = FALSE;
    Enable();
    tick_run(FALSE);
    paula_quiet();
    mp.paused = FALSE;
    mp.finished = FALSE;
    mp.fin_left = -1;
    tick = ep_start(&cnt);
    mp.ep_t0 = time_ms();
    mp.ep_played = 0;
    mp.med_cnt0 = 0;
    mp.med_said = FALSE;
    {
        /* a note player of 5-8 channels: mixed in pairs onto Paula's four,
         * as ProTracker's 8-channel modules are */
        int nn = ep_note_channels(), nv = nn > PT_CHANS ? PT_CHANS : nn;
        if (nn > 4 && mp.med_tracks && mp.med_tracks <= 4)
            nv = 4;                         /* (MED_note's slots for four tracks: one a channel, full scale) */
        if (mp.mix8 && mp.pm && mp.pm->nch != nv)
            mix_free();                     /* (another module's mixer) */
        if (nn > 0 && tick && (SysBase->AttnFlags & AFF_68020) &&
            (mp.mix8 || mix_alloc(nv))) {
            char m[140];
            memset(mp.pm->v, 0, sizeof(mp.pm->v));
            mp.mix_lost = 0;
            mp.tick_cnt = cnt;
            mix_rings_start();
            sprintf(m, "a note player's %ld channels, mixed onto Paula's four at %lu Hz "
                       "(samples of any length, as the player gives them)",
                    (long)nn, (unsigned long)mp.pm->rate);
            amce_log("music", m);
        }
    }
    mp.running = TRUE;
    if (tick) {
        mp.tick_cnt = cnt;
        tick_run(TRUE);
    }
    return TRUE;
}

BOOL mp_play(char *err, LONG errmax)
{
    if (!mp_loaded()) {
        amce_strlcpy(err, _("No module loaded"), errmax);
        return FALSE;
    }
    mp.anc_on = FALSE;                      /* the tempo anchored anew */
    mp.chk_t0 = 0;                          /* its check from the start, */
    mp.chk_first = TRUE;                    /* logged once whatever it finds */
    if (!vol_set)
        mp.master = 64;
    if (mp.engine == ENG_RADIO &&
        (st_ahi() || (st_mhi() && (!mp.rtrack || !amce_stricmp(mp.rtag, "MP3"))))) {
        /* through AHI, or MP3 by MHI: Paula's channels are theirs to take */
        audio_close();
    } else if (!audio_open()) {
        amce_strlcpy(err, _("The audio channels are in use by another program"), errmax);
        return FALSE;
    }
    if (mp.engine == ENG_EAGLE)
        return play_eagle(err, errmax);
    if (mp.engine == ENG_RADIO) {
        mp.finished = FALSE;
        mp.paused = FALSE;
        if (!st_start(mp.path, mp.master, mp.rcpu, mp.rtrack, mp.rrange,
                      mp.rtrack && !mp.rrange ? mp.rbase_ms : 0, err, errmax))
            return FALSE;
        mp.running = TRUE;
        return TRUE;
    }
    if (mp.engine == ENG_MIX) {
        if (mp.finished)
            mx_rewind();
        mp.finished = FALSE;
        mp.paused = FALSE;
        if (!mx_start(mp.master, err, errmax))
            return FALSE;
        mp.running = TRUE;
        return TRUE;
    }
    if (!mp.cia && !timers_open()) {
        amce_strlcpy(err, _("No CIA timer free to play music with"), errmax);
        audio_close();
        return FALSE;
    }
    Disable();
    mp.running = FALSE;
    mp.phase = 0;
    mp.seek_to = -1;
    Enable();
    tick_run(FALSE);
    paula_quiet();
    if (mp.finished || mp.pt->ended) {
        pt_restart(mp.pt, 0, 6, 125);       /* over: from the start */
        mp.pt->elapsed_us = 0;
    }
    mp.fin_left = -1;
    mp.finished = FALSE;
    mp.bpm_set = mp.pt->bpm;
    mp.paused = FALSE;
    if (mp.mix8) {
        /* every channel silent, then Paula on the rings */
        memset(mp.vreg, 0, sizeof(mp.vreg));
        memset(mp.pm->v, 0, sizeof(mp.pm->v));
        mp.voff = 0;
        mix_rings_start();
    }
    mp.running = TRUE;
    tick_run(TRUE);
    return TRUE;
}

/* From the start of the song again (stopped, or playing on). */
void mp_rewind(void)
{
    if (mp.engine == ENG_EAGLE) {
        struct EpInfo in;
        ep_info(&in);
        if (mp.running) {
            ep_subsong(in.sub_raw);         /* the subsong again, from its start */
            mp.ep_t0 = time_ms();
            mp.ep_played = 0;
        }
        return;
    }
    if (mp.engine == ENG_MIX) {
        mx_rewind();
        return;
    }
    mp_seek(0);
}

void mp_stop(void)
{
    if (!mp.running && !mp.audio_open)
        return;
    Disable();
    mp.running = FALSE;
    mp.phase = 0;
    Enable();
    tick_run(FALSE);
    if (mp.engine == ENG_EAGLE)
        ep_stop();
    if (mp.engine == ENG_MIX) {
        mx_stop();
        mx_rewind();
    }
    if (mp.engine == ENG_RADIO)
        st_stop();
    paula_quiet();
    mp.fin_left = -1;
    mp.finished = FALSE;
    mp.ep_played = 0;
    if (mp.pt) {
        pt_restart(mp.pt, 0, 6, 125);
        mp.pt->elapsed_us = 0;
    }
    /* stopped: the channels and the CIA timer given back, for other
     * programs (the Amiga multitasks); Play takes them again */
    timers_close();
    audio_close();
}

/* Let go of the hardware and the module (a game is starting, or AmigaMCE
 * is quitting). */
void mp_shutdown(void)
{
    BOOL had = mp.audio_open || mp.cia;
    mp_stop();
    timers_close();                         /* no interrupt left to run... */
    if (mp.audio_open)
        paula_reset();                      /* ...nor a channel playing... */
    module_free();                          /* ...when the samples go */
    mx_close_library();
    audio_close();
    if (had)
        amce_log("music", "Paula and the CIA timers let go: DMA off, volumes 0");
    if (mp.silence) {
        FreeMem(mp.silence, 8);
        mp.silence = NULL;
    }
    if (quiet_word) {
        FreeMem(quiet_word, 8);
        quiet_word = NULL;
    }
}

void mp_pause(BOOL on)
{
    int i;
    if (!mp.running || on == mp.paused)
        return;
    mp.paused = on;
    if (mp.engine == ENG_EAGLE) {
        if (on)
            mp.ep_played += time_ms() - mp.ep_t0;
        else
            mp.ep_t0 = time_ms();
        ep_pause(on);
        if (mp.mix8 && !on)
            mix_rings_start();              /* the rings again, in step */
        return;
    }
    if (mp.engine == ENG_MIX) {
        mx_pause(on);
        return;
    }
    if (mp.engine == ENG_RADIO) {
        st_pause(on);                       /* live: it plays on, unheard */
        return;
    }
    if (mp.mix8) {
        mix_volume();
        return;
    }
    for (i = 0; i < 4; i++)
        CU->aud[i].ac_vol = scaled(mp.pt->hwvol[i]);
}

void mp_seek(int pos)
{
    if (!mp.pt)
        return;
    if (pos < 0)
        pos = 0;
    if (pos >= mp.pt->songlen)
        pos = mp.pt->songlen - 1;
    if (mp.running) {
        mp.seek_to = (WORD)pos;             /* the tick does it (and clears the end) */
        return;
    }
    mp.fin_left = -1;
    mp.finished = FALSE;
    if (mp.times->at_ms[pos] != 0xFFFFFFFFUL) {
        pt_restart(mp.pt, pos, mp.times->speed[pos], mp.times->bpm[pos]);
        mp.pt->elapsed_us = mp.times->at_ms[pos] * 1000;
    } else {
        pt_restart(mp.pt, pos, mp.pt->speed, mp.pt->bpm);
    }
}

/* Back or on a position (d -1, +1): FALSE if this module cannot. */
/* Whether mp_step(d) can move through this module now. */
BOOL mp_can_step(int d)
{
    if (mp.engine == ENG_EAGLE)
        return mp.running && ep_can_step(d);
    if (mp.engine == ENG_MIX)
        return mx_can_step();
    if (mp.engine == ENG_RADIO)
        return mp.rtrack && (mp.rtotal_ms || st_length_ms());  /* a track; live, no */
    return mp.pt != NULL;
}

BOOL mp_step(int d)
{
    if (mp.engine == ENG_EAGLE)
        return mp.running && ep_step_position(d);
    if (mp.engine == ENG_MIX)
        return mx_step(d);
    if (mp.engine == ENG_RADIO)
        return FALSE;
    if (!mp.pt)
        return FALSE;
    mp_seek(mp.pt->order + d);
    return TRUE;
}

/* Subsongs (players that have them): *cur 1.., *n. */
void mp_subsongs(int *cur, int *n)
{
    struct EpInfo in;
    *cur = *n = 1;
    if (mp.engine != ENG_EAGLE)
        return;
    ep_info(&in);
    *cur = in.sub;
    *n = in.subs;
}

/* The next or previous subsong (d +1, -1): FALSE if there is none. */
BOOL mp_subsong_step(int d)
{
    struct EpInfo in;
    if (mp.engine != ENG_EAGLE || !mp.running)
        return FALSE;
    ep_info(&in);
    if (!ep_subsong(in.sub_raw + d))
        return FALSE;
    mp.ep_t0 = time_ms();
    mp.ep_played = 0;
    mp.paused = FALSE;
    return TRUE;
}

/* ---- Paula lent (an animation's sound, anim.c): what plays is paused,
 * and channels 2 (right) and 3 (left) are the borrower's, quiet, until
 * given back; then what played goes on. A module's replayer stops while
 * paused; a station or a track mixed to 0 and 1 plays on there unheard
 * (stream.c, mixplay.c), so 2 and 3 are free in every case. The volume
 * meanwhile is only noted (the module's would set all four channels'). */
static struct { BOOL on, opened, resume; } lend;

static void lent_quiet(void)
{
    CU->dmacon = DMAF_AUD2 | DMAF_AUD3;
    CU->aud[2].ac_vol = 0;
    CU->aud[3].ac_vol = 0;
    wait_lines(3);
    CU->aud[2].ac_dat = 0;
    CU->aud[3].ac_dat = 0;
}

BOOL mp_lend(BOOL on)
{
    if (on) {
        if (lend.on)
            return TRUE;
        lend.resume = mp_playing();
        if (lend.resume)
            mp_pause(TRUE);
        lend.opened = !mp.audio_open;
        if (!audio_open()) {
            if (lend.resume)
                mp_pause(FALSE);
            return FALSE;                   /* another has Paula (AHI's driver): no sound */
        }
        lend.on = TRUE;
        CU->intena = INTF_AUD2 | INTF_AUD3;
        CU->adkcon = 0x00EE;                /* nothing modulates 2 or 3, nor 1 them */
        lent_quiet();
        return TRUE;
    }
    if (!lend.on)
        return FALSE;
    lent_quiet();
    lend.on = FALSE;
    if (lend.opened && !mp.running)
        audio_close();
    if (mp.mix8 && mp.running)
        mix_rings_start();                  /* all four rings again, in step */
    if (lend.resume)
        mp_pause(FALSE);
    mp_set_volume(mp.master);               /* a change meanwhile, applied */
    return TRUE;
}

void mp_set_volume(int v)
{
    int i;
    vol_set = TRUE;
    if (v < 0)
        v = 0;
    if (v > 64)
        v = 64;
    mp.master = (UWORD)v;
    if (lend.on)
        return;                             /* noted: applied when given back */
    if (mp.engine == ENG_EAGLE) {
        ep_set_volume(v);
        if (mp.running && mp.mix8)
            mix_volume();                   /* (a mixed note player's: the rings') */
        return;
    }
    if (mp.engine == ENG_MIX) {
        mx_volume(v);
        return;
    }
    if (mp.engine == ENG_RADIO) {
        st_volume(v);
        return;
    }
    if (mp.running && mp.mix8)
        mix_volume();
    else if (mp.running && mp.pt)
        for (i = 0; i < 4; i++)
            CU->aud[i].ac_vol = scaled(mp.pt->hwvol[i]);
}

int mp_volume(void) { return vol_set ? mp.master : 64; }
BOOL mp_loaded(void) { return mp.engine != ENG_NONE; }
BOOL mp_playing(void) { return mp.running && !mp.paused; }
BOOL mp_paused(void) { return mp.running && mp.paused; }
BOOL mp_running(void) { return mp.running; }
BOOL mp_ended(void)
{
    if (mp.engine == ENG_EAGLE)
        return mp.running && ep_song_ended();
    if (mp.engine == ENG_MIX) {
        if (mp.running && mx_ended())
            mp.finished = TRUE;
        return mp.finished;
    }
    if (mp.engine == ENG_RADIO)
        return mp.running && (st_state() == ST_ENDED || st_state() == ST_FAILED);
    return mp.pt && mp.finished;
}
const char *mp_path(void) { return mp.path; }

static void meters(struct MpStatus *s, const UBYTE *hit, const UBYTE *vol)
{
    int i;
    for (i = 0; i < 4; i++) {
        /* each note's start lifts its meter; it falls away between */
        ULONG h = hit[i] ? vol[i] : 0;
        if (h && mp.running && !mp.paused) {
            if (h > 64)
                h = 64;
            h = h * mp_volume() / 64;
            if (h * 256 > mp.level[i])
                mp.level[i] = h * 256;
        }
        s->level[i] = (UBYTE)(mp.level[i] >> 8);
        mp.level[i] = mp.level[i] * 13 / 16;
    }
}

void mp_status(struct MpStatus *s)
{
    struct Pt *pt = mp.pt;
    UBYTE hit[4], vol[4];
    int i;
    memset(s, 0, sizeof(*s));
    s->running = mp.running;
    s->paused = mp.paused;
    if (mp.engine == ENG_EAGLE) {
        struct EpInfo in;
        UBYTE mask;
        ep_info(&in);
        s->order = in.pos >= 0 ? in.pos : 0;
        s->row = in.row >= 0 ? in.row : 0;
        s->songlen = in.pos >= 0 ? in.poslen : 0;
        s->pattern = -1;
        s->elapsed_ms = mp.ep_played + (mp.running && !mp.paused ? time_ms() - mp.ep_t0 : 0);
        s->total_ms = med_ms(mp.med_units);
        if (s->total_ms && s->elapsed_ms > s->total_ms)
            s->elapsed_ms = s->total_ms;
        if (mp.med_units && !mp.med_said && s->elapsed_ms > 1500) {
            char m[140];
            mp.med_said = TRUE;
            sprintf(m, "its length: %lu ms (%lu ticks; a tick %lu E-clocks at its first tempo, "
                       "%lu now; the E-clock %lu Hz)",
                    (unsigned long)s->total_ms, (unsigned long)(mp.med_units / 16),
                    (unsigned long)mp.med_cnt0, (unsigned long)mp.tick_cnt,
                    (unsigned long)mp.eclock);
            amce_log("music", m);
        }
        s->sub = in.sub;
        s->subs = in.subs;
        {
            UBYTE t = 0;
            mp.ep_voices = ep_levels(&mask, vol, mp.epv, &t);
            mp.ep_trig |= t;
        }
        for (i = 0; i < 4; i++)
            hit[i] = (mask >> i) & 1;
        if (mp.mix8 && mp.pm && mp.running && !mp.paused) {
            /* mixed: each of Paula's four as loud as it really is, from its
             * ring about where Paula reads it (the mixing is a lead ahead),
             * so a long sample started once still shows (a bass line) */
            ULONG rl = mp.pm->ring_len, lead = mp.pm->rate * 12 / 100, win = mp.pm->rate / 25;
            ULONG rp = (mp.pm->wpos + rl - lead % rl) % rl, k;
            for (i = 0; i < 4; i++) {
                const BYTE *r = (const BYTE *)mp.pm->ring[i];
                UBYTE pk = 0;
                for (k = 1; k <= win; k += 2) {
                    BYTE v = r[(rp + rl - k) % rl];
                    UBYTE a = (UBYTE)(v < 0 ? -v : v);
                    if (a > pk)
                        pk = a;
                }
                hit[i] = pk != 0;
                vol[i] = (UBYTE)(mp.pm->nch > 4 ? (pk > 64 ? 64 : pk) : pk / 2);
            }
        }
        meters(s, hit, vol);
        return;
    }
    if (mp.engine == ENG_RADIO) {
        struct StInfo in;
        st_info(&in);
        s->elapsed_ms = mp.running ? in.played_ms + mp.rbase_ms : mp.rbase_ms;
        s->total_ms = !mp.rtrack ? 0 : mp.rtotal_ms ? mp.rtotal_ms : st_length_ms();
        if (s->total_ms && s->elapsed_ms > s->total_ms)
            s->elapsed_ms = s->total_ms;
        s->sub = s->subs = 1;
        hit[0] = hit[3] = in.peak_l != 0;
        hit[1] = hit[2] = in.peak_r != 0;
        vol[0] = vol[3] = (UBYTE)(in.peak_l / 2);
        vol[1] = vol[2] = (UBYTE)(in.peak_r / 2);
        meters(s, hit, vol);
        return;
    }
    if (mp.engine == ENG_MIX) {
        UBYTE l, r;
        mx_status(s, &l, &r);
        s->sub = s->subs = 1;
        hit[0] = hit[3] = l != 0;
        hit[1] = hit[2] = r != 0;
        vol[0] = vol[3] = (UBYTE)(l / 2);
        vol[1] = vol[2] = (UBYTE)(r / 2);
        meters(s, hit, vol);
        return;
    }
    if (!pt)
        return;
    s->order = pt->order;
    s->row = pt->row;
    s->songlen = pt->songlen;
    s->pattern = pt->orders[pt->order & 0x7F];
    s->speed = pt->speed;
    s->bpm = pt->bpm;
    s->elapsed_ms = pt->elapsed_us / 1000;
    s->total_ms = mp.times ? mp.times->total_ms : 0;
    s->sub = s->subs = 1;
    if (s->elapsed_ms > s->total_ms && s->total_ms)
        s->elapsed_ms = s->total_ms;        /* playing on past the end (looped) */
    for (i = 0; i < 4; i++) {
        vol[i] = pt->hit[i];
        if (i + 4 < pt->nch && pt->hit[i + 4] > vol[i])
            vol[i] = pt->hit[i + 4];        /* mixed: the pair's meter, the louder */
        hit[i] = vol[i] != 0;
        pt->hit[i] = 0;
        if (i + 4 < pt->nch)
            pt->hit[i + 4] = 0;
        s->sample[i] = pt->ch[i].n_sample;
    }
    meters(s, hit, vol);
}

const char *mp_title(void)
{
    if (mp.engine == ENG_EAGLE) {
        struct EpInfo in;
        ep_info(&in);
        return in.song ? in.song : "";
    }
    if (mp.engine == ENG_MIX)
        return mx_title();
    if (mp.engine == ENG_RADIO)
        return mp.rname;
    return mp.pt ? mp.pt->title : "";
}

/* What it is: the ProTracker tag ("M.K."), or the player's name. */
const char *mp_tag(void)
{
    if (mp.engine == ENG_EAGLE) {
        struct EpInfo in;
        ep_info(&in);
        return in.player ? in.player : "";
    }
    if (mp.engine == ENG_MIX)
        return mx_kind();
    if (mp.engine == ENG_RADIO)
        return mp.rtag;
    return mp.pt ? mp.lay.tag : "";
}

const char *mp_author(void)
{
    if (mp.engine == ENG_EAGLE) {
        struct EpInfo in;
        ep_info(&in);
        return in.author ? in.author : "";
    }
    return "";
}

int mp_samples(void)
{
    if (mp.engine == ENG_MIX)
        return mx_names();
    return mp.pt ? mp.pt->nsmp : 0;
}

/* Sample i's name (and whether it has sound). */
const char *mp_sample_name(int i, BOOL *used)
{
    if (mp.engine == ENG_MIX) {
        const char *n = mx_name(i);
        *used = n[0] != 0;
        return n;
    }
    if (!mp.pt || i < 0 || i >= mp.pt->nsmp) {
        *used = FALSE;
        return "";
    }
    *used = mp.pt->smp[i].len > 1;
    return mp.pt->smp[i].name;
}

/* A station: nothing to load (its stream is opened when it plays);
 * file: a track may be a music file on this Amiga, not an address. */
static BOOL load_stream(const char *url, const char *name, const char *tag, UWORD cpu,
                        BOOL file, char *err, LONG errmax)
{
    err[0] = 0;
    mp_stop();
    module_free();
    if (!file && amce_strnicmp(url, "http", 4)) {
        amce_strlcpy(err, _("Not a station's address"), errmax);
        return FALSE;
    }
    mp.engine = ENG_RADIO;
    mp.rcpu = cpu;
    mp.rtrack = FALSE;
    mp.rtotal_ms = 0;
    mp.rbase_ms = mp.rrange = 0;
    mp.rby[0] = 0;
    amce_strlcpy(mp.path, url, sizeof(mp.path));
    amce_strlcpy(mp.rname, name, sizeof(mp.rname));
    amce_strlcpy(mp.rtag, tag, sizeof(mp.rtag));
    {
        /* the list's HTTPS mark off: the format alone ("MP3" is compared),
         * Now Playing saying HTTPS by the connection itself (st_secure) */
        LONG n = strlen(mp.rtag);
        if (n >= 6 && !strcmp(mp.rtag + n - 6, " HTTPS"))
            mp.rtag[n - 6] = 0;
    }
    return TRUE;
}

BOOL mp_load_stream(const char *url, const char *name, const char *tag, UWORD cpu, char *err,
                    LONG errmax)
{
    return load_stream(url, name, tag, cpu, FALSE, err, errmax);
}

/* A track on a server (Plex): an MP3 file over HTTP, played as a station
 * is, but with an end and a length; by: its artist and album. */
BOOL mp_load_track(const char *url, const char *title, const char *by, const char *tag,
                   ULONG total_ms, UWORD cpu, ULONG from_ms, ULONG range_from, char *err,
                   LONG errmax)
{
    if (!load_stream(url, title, tag, cpu, TRUE, err, errmax))
        return FALSE;
    mp.rtrack = TRUE;
    mp.rtotal_ms = total_ms;
    mp.rbase_ms = from_ms;
    mp.rrange = range_from;
    amce_strlcpy(mp.rby, by, sizeof(mp.rby));
    return TRUE;
}

BOOL mp_is_stream(void) { return mp.engine == ENG_RADIO; }
BOOL mp_is_track(void) { return mp.engine == ENG_RADIO && mp.rtrack; }
const char *mp_track_by(void) { return mp.rby; }

/* The level meters Now playing shows: a module on Paula's four channels
 * has one each; what is mixed or decoded, one for each of its channels. */
/* ------------------------------------------------------------ the visualizations */
#define PAULA_HZ 3546895UL          /* PAL: a period's rate is this / period */
ULONG mp_serial(void)
{
    return mp.serial;
}

UWORD mp_vis_have(void)
{
    if (!mp_loaded())
        return 0;
    switch (mp.engine) {
    case ENG_PT:
        return VF_LEVELS | VF_VOICES;
    case ENG_EAGLE:
        return VF_LEVELS | (mp.ep_voices ? VF_VOICES : 0);
    case ENG_MIX:
        return VF_LEVELS | VF_PCM;
    case ENG_RADIO:
        return VF_LEVELS | (st_mhi_now() ? 0 : VF_PCM);
    }
    return 0;
}

/* Paula's voices where they play now, as HippoPlayer's scopes follow them
 * (puu016.s: quadScopeSampleFollow): from each note's start, on by the
 * bytes its period plays in the time gone by, then round its loop. */
static void voices_follow(struct VisFeed *f)
{
    ULONG now = time_ms(), dt = now - mp.vf_t;
    UBYTE trig;
    int i;
    if (dt > 1000)
        dt = 1000;
    mp.vf_t = now;
    Disable();
    if (mp.engine == ENG_PT) {
        trig = mp.vtrig;
        mp.vtrig = 0;
        for (i = 0; i < 4; i++)
            if (trig & (1 << i)) {
                mp.vf[i].pos = mp.vnew[i].start;
                mp.vf[i].left = (LONG)mp.vnew[i].len * 2;
                mp.vf[i].loop = mp.vnew[i].loop;
                mp.vf[i].replen = (LONG)mp.vnew[i].replen * 2;
                trig &= (UBYTE)~(1 << i);
            }
    } else {
        trig = mp.ep_trig;
        mp.ep_trig = 0;
        for (i = 0; i < 4; i++) {
            if (trig & (1 << i)) {
                mp.vf[i].pos = mp.epv[i].adr;
                mp.vf[i].left = (LONG)mp.epv[i].len * 2;
            }
            mp.vf[i].loop = mp.epv[i].loop;
            mp.vf[i].replen = (LONG)mp.epv[i].replen * 2;
        }
        trig = 0;
    }
    Enable();
    for (i = 0; i < 4; i++) {
        struct VisVoice *v = &f->voice[i];
        UWORD per = mp.engine == ENG_PT ? mp.pt->hwper[i] : mp.epv[i].per;
        UBYTE vol = mp.engine == ENG_PT ? mp.pt->hwvol[i] : mp.epv[i].vol;
        if (mp.vf[i].pos && per >= 100 && mp.running && !mp.paused) {
            LONG adv = (LONG)((PAULA_HZ / per) * dt / 1000);
            if (adv < mp.vf[i].left) {
                mp.vf[i].pos += adv;
                mp.vf[i].left -= adv;
            } else if (mp.vf[i].replen > 2 && mp.vf[i].loop) {
                LONG over = (adv - mp.vf[i].left) % mp.vf[i].replen;
                mp.vf[i].pos = mp.vf[i].loop + over;
                mp.vf[i].left = mp.vf[i].replen - over;
            } else {
                mp.vf[i].pos = NULL;            /* played out: silent */
            }
        }
        v->pos = mp.vf[i].pos;
        v->left = mp.vf[i].left;
        v->loop = mp.vf[i].replen > 2 ? mp.vf[i].loop : NULL;
        v->replen = mp.vf[i].replen;
        v->period = per;
        v->vol = (UBYTE)(mp.paused || !mp.running ? 0 : vol > 64 ? 64 : vol);
    }
}

void mp_vis_feed(struct VisFeed *f, UWORD want)
{
    f->have = mp_vis_have();
    f->pcm_n = 0;
    if ((want & f->have & VF_VOICES))
        voices_follow(f);
    if ((want & f->have & VF_PCM))
        f->pcm_n = mp.engine == ENG_MIX ? mx_pcm(f->pcm, VIS_PCM, &f->rate)
                                        : st_pcm(f->pcm, VIS_PCM, &f->rate);
}

int mp_meter_count(void)
{
    if (mp.engine == ENG_MIX)
        return 2;
    if (mp.engine == ENG_RADIO) {
        int c = st_channels();
        if (!c)                             /* not decoding yet: what it will be */
            c = mp.rcpu < 40 && !(st_mhi() && (!mp.rtrack || !amce_stricmp(mp.rtag, "MP3")))
                ? 1 : 2;                    /*   (a 68030's decoder makes mono; MHI not) */
        return c == 1 ? 1 : 2;
    }
    return 4;
}
const char *mp_stream_name(void) { return mp.rname; }

/* To a time in the song (modules mixed): FALSE if this module cannot. */
/* A MED song's ticks (x 16) as ms, by its timer at its first tempo: 0
 * not known (not MED, or before its first tick). */
static ULONG med_ms(ULONG units16)
{
    ULONG us_tick;
    if (!units16 || !mp.med_cnt0 || !mp.eclock)
        return 0;
    us_tick = (ULONG)mp.med_cnt0 * 1000 / (mp.eclock / 1000);  /* a tick, in us */
    return (units16 / 16) * us_tick / 1000;
}

/* An EaglePlayer song to ms in (a MED song, its length known): its player
 * run on silently from where it is (or from the start, for a time before),
 * its ticks counted by its own timer, the mixer's voices quiet after. */
#define EAGLE_SEEK_MS 4000              /* as long as a seek may take */

static BOOL eagle_seek_ms(ULONG ms)
{
    struct EpInfo in;
    ULONG now, at_us, want_us = ms * 1000, t0 = time_ms(), n = 0;
    BOOL was_paused = mp.paused;
    char m[120];
    if (!mp.med_units || !mp.running || ep_own_timer())
        return FALSE;                       /* (its first tick not needed: the timer as it is) */
    now = mp.ep_played + (!mp.paused ? time_ms() - mp.ep_t0 : 0);
    tick_run(FALSE);
    if (ms < now) {
        ep_info(&in);
        ep_subsong(in.sub_raw);             /* from its start */
        tick_run(FALSE);
        at_us = 0;
    } else {
        at_us = now * 1000;
    }
    while (at_us < want_us && time_ms() - t0 < EAGLE_SEEK_MS) {
        ep_ff_tick();
        at_us += (ULONG)mp.tick_cnt * 1000 / (mp.eclock / 1000);
        n++;
    }
    if (mp.mix8 && mp.pm) {
        Disable();
        memset(mp.pm->v, 0, sizeof(mp.pm->v));  /* what sounded before: quiet */
        Enable();
        mix_rings_start();
    } else {
        paula_quiet();
    }
    mp.ep_played = at_us / 1000;
    mp.ep_t0 = time_ms();
    mp.finished = FALSE;
    mp.anc_on = FALSE;
    if (!was_paused)
        tick_run(TRUE);
    sprintf(m, "seek to %lu ms: %lu ticks run on in %lu ms, at %lu ms", (unsigned long)ms,
            (unsigned long)n, (unsigned long)(time_ms() - t0), (unsigned long)(at_us / 1000));
    amce_log("eagleplayer", m);
    return TRUE;
}

BOOL mp_seek_ms(ULONG ms)
{
    if (mp.engine == ENG_EAGLE)
        return eagle_seek_ms(ms);
    if (mp.engine != ENG_MIX)
        return FALSE;
    mx_seek_ms(ms);
    mp.finished = FALSE;
    return TRUE;
}

/* Where each position starts in the song, in ms (0xFFFFFFFF: never). */
ULONG mp_position_ms(int pos)
{
    if (!mp.times || pos < 0 || pos >= 128)
        return 0xFFFFFFFFUL;
    return mp.times->at_ms[pos];
}
