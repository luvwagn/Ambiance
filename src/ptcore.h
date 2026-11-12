/*
 * AmigaMCE - a ProTracker replayer: the song, without the hardware
 * Copyright (c) 2026 Brian King
 *
 * Plain C with no AmigaOS headers, so the same code plays on Paula
 * (modplay.c) and is tested on the build machine (tests/pt_host.c). The
 * hardware is reached through the pt_hw_* functions the user of this
 * file provides; they are called from pt_tick, which on the Amiga runs in
 * a CIA timer interrupt.
 */
#ifndef PTCORE_H
#define PTCORE_H

typedef unsigned char  pt_u8;
typedef signed char    pt_s8;
typedef unsigned short pt_u16;
typedef signed short   pt_s16;
typedef unsigned long  pt_u32;
typedef signed long    pt_s32;

#define PT_CHANS   8        /* at most: 4 on Paula, 5-8 mixed (modplay.c, ptmix.c) */
#define PT_SAMPLES 31

/* What a module file is, from its first 1084 bytes (pt_layout). */
struct PtLayout {
    pt_u8  nsmp;            /* 31, or 15 (an old Soundtracker module) */
    pt_u8  songlen;         /* positions in the song, 1-128 */
    pt_u8  npat;            /* patterns stored (FLT8: its 4-channel halves) */
    pt_u8  chans;           /* 4, or 5-8 ("6CHN", "8CHN", "OCTA", "FLT8"...) */
    pt_u8  flt8;            /* Startrekker's FLT8: each pattern two of 4 channels */
    pt_u32 hdr_len;         /* header bytes: 1084, or 600 for 15 samples */
    pt_u32 pat_len;         /* pattern bytes after the header (npat * 256 * chans) */
    pt_u32 smp_len;         /* sample bytes after the patterns, all samples */
    char   tag[5];          /* "M.K.", "FLT4"..., or "ST15" */
};

/* pt_layout's answers: PT_ERR_CHANNELS for more than 8 channels (or
 * fewer than 4), *channels saying how many. */
enum { PT_OK, PT_ERR_SHORT, PT_ERR_NOTMOD, PT_ERR_CHANNELS };
int pt_layout(const pt_u8 *hdr, pt_u32 hlen, pt_u32 filelen, struct PtLayout *L,
              int *channels);

struct PtSample {
    pt_s8 *data;            /* in chip RAM on the Amiga */
    pt_u16 len;             /* in words */
    pt_u16 rep, replen;     /* loop start and length, in words */
    pt_u8  fine, vol;
    char   name[23];
};

struct PtChan {
    pt_u16 n_note, n_cmd;           /* the row's four bytes, as ProTracker keeps them */
    pt_s8 *n_start, *n_loopstart, *n_wavestart;
    pt_u16 n_length, n_replen;
    pt_s16 n_period, n_wantedperiod;
    pt_u8  n_finetune;
    pt_s8  n_volume;
    pt_u8  n_toneportdirec, n_toneportspeed, n_vibratocmd, n_vibratopos;
    pt_u8  n_tremolocmd, n_tremolopos, n_wavecontrol, n_glissfunk;
    pt_u8  n_sampleoffset, n_pattpos, n_loopcount, n_funkoffset;
    pt_u8  n_sample;                /* the last sample number given (shown) */
    pt_u16 n_dmabit;
};

struct Pt {
    /* the song */
    const pt_u8 *pat;               /* npat * 256 * nch bytes */
    pt_u8  orders[128];
    pt_u8  songlen, npat, nsmp;
    pt_u8  nch, flt8;               /* channels (4-8), and FLT8's pattern halves */
    struct PtSample smp[PT_SAMPLES];
    pt_s8 *silence;                 /* 4 zero bytes (chip RAM): an empty sample */
    char   title[21];
    /* playing it */
    struct PtChan ch[PT_CHANS];
    pt_u8  speed, counter, bpm;
    pt_u8  pattdeltime, pattdeltime2, pbreakpos, pbreakflag, posjumpflag, lowmask;
    pt_s16 order, row;
    pt_u16 dmacontemp;              /* channels to start after this tick (pt_hw_dma_on) */
    pt_u16 loopmask;                /* channels whose loop to set once started */
    pt_s8 *loop_ptr[PT_CHANS];      /* ...and what to set */
    pt_u16 loop_len[PT_CHANS];
    pt_u8  hwvol[PT_CHANS];         /* the volume each channel was last given */
    pt_u16 hwper[PT_CHANS];         /* and the period */
    pt_u8  hit[PT_CHANS];           /* a note started: its volume (the UI's meters) */
    pt_u8  quiet;                   /* no hardware (measuring the song) */
    pt_u8  ended;                   /* 1: the song came round to where it had been; 2: F00 */
    pt_u8  visited[128 * 8];        /* rows played, a bit each (64 a position) */
    pt_u32 elapsed_us;              /* time played, from the ticks */
    pt_u32 tick_us;                 /* this tick's length (2.5 s / bpm) */
};

/* Set up from a module: hdr is its first L->hdr_len bytes, pat its
 * patterns (L->pat_len bytes), smp its sample data (L->smp_len bytes, in
 * chip RAM). The first word of each sample that does not loop is cleared,
 * as ProTracker does. */
void pt_init(struct Pt *pt, const struct PtLayout *L, const pt_u8 *hdr, const pt_u8 *pat,
             pt_s8 *smp, pt_s8 *silence);
/* Back to the start (position 0), or to position pos with the speed and
 * tempo the song has there (from pt_measure). */
void pt_restart(struct Pt *pt, int pos, pt_u8 speed, pt_u8 bpm);
/* One tick: 50 a second at 125 BPM. */
void pt_tick(struct Pt *pt);

/* The song's length, playing it through without sound: the time each
 * position is first reached and the speed and tempo there. */
struct PtTimes {
    pt_u32 total_ms;
    pt_u32 at_ms[128];              /* 0xFFFFFFFF: never reached */
    pt_u8  speed[128], bpm[128];
};
void pt_measure(struct Pt *pt, struct PtTimes *t);

/* Provided by the user of ptcore.c. */
void pt_hw_period(int ch, pt_u16 period);
void pt_hw_volume(int ch, pt_u8 vol);
void pt_hw_data(int ch, const pt_s8 *ptr);
void pt_hw_length(int ch, pt_u16 words);
void pt_hw_dma_off(pt_u16 mask);
/* After pt_tick: start the channels in dmacontemp (if any), then, once
 * Paula has taken their start, give the channels in loopmask their loop. */

#endif
