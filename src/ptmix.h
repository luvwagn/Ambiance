/*
 * Ambiance - eight channels on Paula's four (ptmix.c)
 * Copyright (c) 2026 Brian King
 * The design is ProTracker 4.1's 8-channel mixer, (c) 2026 Akustikrausch
 * (Andreas Wendorf), MIT licence: https://github.com/akustikrausch/protracker4
 */
#ifndef PTMIX_H
#define PTMIX_H

#include "ptcore.h"

/* A Paula in software: registers as the replayer writes them, and what it
 * is playing (taken from the registers at DMA on and at each block end). */
struct PtVoice {
    const pt_s8 *pos, *end;         /* playing: where, and the block's end */
    pt_u32 frac;                    /* 16-bit fraction of a byte */
    pt_u32 step;                    /* bytes per output sample, 16.16 */
    pt_u16 step_per;                /* the period step is for */
    const pt_s8 *reg_ptr;           /* the registers: start and length (words) */
    pt_u32 reg_len;                 /* (32 bits: a note player's samples run past 128K) */
    pt_u16 per;
    pt_u8 vol, on;
};

/* One tick's changes, as the replayer made them (built in its interrupt,
 * applied where the mixing is done). */
struct PmTick {
    pt_u8 off, on;                  /* DMA switched off, then on (bit a channel) */
    const pt_s8 *start[PT_CHANS];   /* on: the start and length taken */
    pt_u32 slen[PT_CHANS];          /* words, 0 Paula's 65536 (a note player's: longer) */
    const pt_s8 *ptr[PT_CHANS];     /* the registers after the tick (the loop) */
    pt_u32 len[PT_CHANS];
    pt_u16 per[PT_CHANS];
    pt_u8 vol[PT_CHANS];
    pt_u8 hold;                     /* paused: silence, the voices where they are */
    pt_u32 samples;                 /* output samples this tick */
};

struct PtMix {
    int nch;                        /* 5-8 */
    pt_u16 period;                  /* Paula's for the rings */
    pt_u32 rate;                    /* the rings' rate, Hz (rounded down) */
    pt_s8 *vt;                      /* 65 x 256: (sample * volume) >> 7 */
    pt_s8 *ring[4];                 /* Paula's four rings */
    pt_u32 ring_len, wpos;          /* bytes each; where the next tick goes */
    pt_u32 spt_rest;                /* pm_tick_samples' fraction */
    struct PtVoice v[PT_CHANS];
};

void pm_voltab(pt_s8 *vt);          /* fill a 65 x 256 volume table */
void pm_init(struct PtMix *m, int nch, pt_u16 period, pt_s8 *vt, pt_s8 *const ring[4],
             pt_u32 ring_len);
/* The samples a tick of cia_count E-clocks is (the fraction kept). */
pt_u32 pm_tick_samples(struct PtMix *m, pt_u32 cia_count);
void pm_apply(struct PtMix *m, const struct PmTick *t);
void pm_mix(struct PtMix *m, pt_u32 n);     /* n samples into each ring, at wpos */
void pm_mix_silence(struct PtMix *m, pt_u32 n);     /* n of silence (paused) */
/* The rings cleared and the next tick written lead samples on from their
 * start, where Paula begins. */
void pm_ring_start(struct PtMix *m, pt_u32 lead);

#endif
