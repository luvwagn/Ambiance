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

#include "ptcore.h"

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

enum { ENG_NONE, ENG_PT, ENG_EAGLE, ENG_MIX };          /* each of the two gaps in starting a note */

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
    char path[256];
    char packer[40];                /* what it was packed with ("" none) */
    UBYTE engine;                   /* ENG_*: what plays it */
    UWORD tick_cnt;                 /* an EaglePlayer player's timer (CIA count) */
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
    volatile ULONG ticks;           /* the tempo interrupt's count, playing */
    ULONG chk_t0, chk_n0, chk_said; /* mp_tempo_check's window */
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
    CU->aud[ch].ac_per = p < 113 ? 113 : p;
}

void pt_hw_volume(int ch, pt_u8 v)
{
    CU->aud[ch].ac_vol = scaled(v);
}

void pt_hw_data(int ch, const pt_s8 *ptr)
{
    CU->aud[ch].ac_ptr = (UWORD *)ptr;
}

void pt_hw_length(int ch, pt_u16 words)
{
    CU->aud[ch].ac_len = words ? words : 1;
}

void pt_hw_dma_off(pt_u16 mask)
{
    CU->dmacon = mask & 0x0F;
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

static void tick_code(void)
{
    struct Pt *pt = mp.pt;
    int i;
    if (!mp.running)
        return;
    if (!mp.paused)
        mp.ticks++;
    if (mp.engine == ENG_EAGLE) {
        if (!mp.paused)
            ep_tick();                      /* the player's own routine */
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
        CU->dmacon = 0x0F;
        for (i = 0; i < 4; i++)
            CU->aud[i].ac_vol = 0;
        if (mp.times && mp.times->at_ms[to] != 0xFFFFFFFFUL) {
            pt_restart(pt, to, mp.times->speed[to], mp.times->bpm[to]);
            pt->elapsed_us = mp.times->at_ms[to] * 1000;
        } else {
            pt_restart(pt, to, pt->speed, pt->bpm);
        }
        mp.fin_left = -1;
        mp.finished = FALSE;
    }
    if (mp.paused || mp.finished)
        return;
    if (mp.fin_left >= 0) {
        /* the song is over once its last row has played out */
        if (mp.fin_left == 0) {
            mp.finished = TRUE;
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
    if (pt->loopmask) {
        for (i = 0; i < 4; i++) {
            mp.loop_ptr[i] = pt->loop_ptr[i];
            mp.loop_len[i] = pt->loop_len[i];
        }
        mp.loop_mask = pt->loopmask;
    }
    mp.dma_mask = pt->dmacontemp;
    if (mp.dma_mask) {
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
    ULONG now = time_ms(), n = mp.ticks, el, cnt, hz10, want, got;
    if (!mp.running || mp.paused || mp.engine == ENG_MIX || !mp.have_tick) {
        mp.chk_t0 = 0;
        return;
    }
    if (!mp.chk_t0) {
        mp.chk_t0 = now ? now : 1;
        mp.chk_n0 = n;
        return;
    }
    el = now - mp.chk_t0;
    if (el < 5000)
        return;
    cnt = mp.engine == ENG_EAGLE ? mp.tick_cnt : tick_count(mp.bpm_set);
    got = n - mp.chk_n0;
    mp.chk_t0 = now ? now : 1;
    mp.chk_n0 = n;
    if (!cnt)
        return;
    hz10 = mp.eclock * 10 / cnt;            /* ticks a second, x10 */
    want = el * hz10 / 10000;
    if (!want || (got * 10 >= want * 9 && got * 10 <= want * 11))
        return;
    if (mp.chk_said && now - mp.chk_said < 30000)
        return;
    mp.chk_said = now ? now : 1;
    {
        char m[120];
        sprintf(m, "the tempo interrupt came %ld times in %ld ms, not %ld (%ld%%)", (long)got,
                (long)el, (long)want, (long)(got * 100 / want));
        amce_log("music", m);
    }
}

/* For EaglePlayer players: their timer (callable from the interrupt). */
void mp_tick_count(UWORD c)
{
    mp.tick_cnt = c;
    if (mp.have_tick && mp.running)
        tick_latch(c);
}

void mp_tick_run(BOOL on)
{
    tick_run(on);
}

static void paula_quiet(void)
{
    int i;
    CU->dmacon = 0x0F;
    for (i = 0; i < 4; i++)
        CU->aud[i].ac_vol = 0;
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
    amce_log("music", "Paula's 4 channels, from audio.device (not AHI)");
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
    mp.engine = ENG_NONE;
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
    static char m[64];
    switch (rc) {
    case PT_ERR_SHORT:
        return "The module file is cut short";
    case PT_ERR_CHANNELS:
        sprintf(m, "%ld-channel modules need more than Paula's four", (long)channels);
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
        return "packed with PowerPacker";
    if (n >= 4 && !memcmp(h, "XPKF", 4))
        return "packed with XPK";
    if (n >= 4 && (!memcmp(h, "ICE!", 4) || !memcmp(h, "Ice!", 4)))
        return "packed with Pack-Ice";
    if (n >= 4 && (!memcmp(h, "MMD0", 4) || !memcmp(h, "MMD1", 4) || !memcmp(h, "MMD2", 4) ||
                   !memcmp(h, "MMD3", 4)))
        return "a MED/OctaMED module";
    if (n >= 17 && !memcmp(h, "Extended Module: ", 17))
        return "a FastTracker 2 module (XM)";
    if (n >= 48 && !memcmp(h + 44, "SCRM", 4))
        return "a Scream Tracker 3 module (S3M)";
    if (n >= 4 && !memcmp(h, "IMPM", 4))
        return "an Impulse Tracker module (IT)";
    if (n >= 4 && !memcmp(h, "THX", 3))
        return "an AHX module";
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
        amce_strlcpy(err, "Could not open the module file", errmax);
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
            sprintf(m, "Packed with %s, but %s", packer[0] ? packer : "a packer", uwhy);
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
            if (ep_load(path, mem, mlen, eerr, sizeof(eerr))) {
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
            sprintf(m, "This file is %s, which AmigaMCE cannot play", pk);
        else if (eerr[0])
            amce_strlcpy(m, eerr, sizeof(m));
        else
            amce_strlcpy(m, "Not a module AmigaMCE can play", sizeof(m));
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
            sprintf(m, "Not enough chip memory for its samples (%lu KB)",
                    (unsigned long)((mp.lay.smp_len + 1023) >> 10));
            amce_strlcpy(err, m, errmax);
        } else {
            amce_strlcpy(err, "Not enough memory for that module", errmax);
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
        amce_strlcpy(err, "No CIA timer free to play music with", errmax);
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
        amce_strlcpy(err, "No module loaded", errmax);
        return FALSE;
    }
    if (!vol_set)
        mp.master = 64;
    if (!audio_open()) {
        amce_strlcpy(err, "The audio channels are in use by another program", errmax);
        return FALSE;
    }
    if (mp.engine == ENG_EAGLE)
        return play_eagle(err, errmax);
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
        amce_strlcpy(err, "No CIA timer free to play music with", errmax);
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
    paula_quiet();
    mp.fin_left = -1;
    mp.finished = FALSE;
    mp.ep_played = 0;
    if (mp.pt) {
        pt_restart(mp.pt, 0, 6, 125);
        mp.pt->elapsed_us = 0;
    }
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
        return;
    }
    if (mp.engine == ENG_MIX) {
        mx_pause(on);
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
    return mp.pt != NULL;
}

BOOL mp_step(int d)
{
    if (mp.engine == ENG_EAGLE)
        return mp.running && ep_step_position(d);
    if (mp.engine == ENG_MIX)
        return mx_step(d);
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

void mp_set_volume(int v)
{
    int i;
    vol_set = TRUE;
    if (v < 0)
        v = 0;
    if (v > 64)
        v = 64;
    mp.master = (UWORD)v;
    if (mp.engine == ENG_EAGLE) {
        ep_set_volume(v);
        return;
    }
    if (mp.engine == ENG_MIX) {
        mx_volume(v);
        return;
    }
    if (mp.running && mp.pt)
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
        s->sub = in.sub;
        s->subs = in.subs;
        ep_levels(&mask, vol);
        for (i = 0; i < 4; i++)
            hit[i] = (mask >> i) & 1;
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
        hit[i] = pt->hit[i] != 0;
        pt->hit[i] = 0;
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

/* To a time in the song (modules mixed): FALSE if this module cannot. */
BOOL mp_seek_ms(ULONG ms)
{
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
