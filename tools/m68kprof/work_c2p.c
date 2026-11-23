/*
 * work_c2p.c - m68kprof's c2p work: a frame of pens (the input file,
 * box_w x box_h) made into 8 planes as anim.c does it, a row at a time
 * (va_c2p68k, c2p68k.s), the planes at C2P_CHIP (run with PROF_CHIP so
 * they are chip RAM: not cached, written through the store buffer).
 * Copyright (c) 2026 Brian King, MIT
 */
#include <stdint.h>
#include "work.h"

#define C2P_CHIP 0x05000000u
void va_c2p68k(const uint8_t *src, uint32_t groups, uint8_t **planes);
void va_c2p68k_sb(const uint8_t *src, uint32_t groups, uint8_t **planes);
#ifdef C2P_SB
#define C2P va_c2p68k_sb                /* (-DC2P_SB: the store buffer's) */
#else
#define C2P va_c2p68k
#endif

int work_run(struct Work *w)
{
    uint32_t bpr = w->box_w / 8, y, p, n = 1;
    uint8_t *pl[8];
    if (w->box_h > 1000) {             /* box_h above 1000: that many frames of 256 rows */
        n = w->box_h / 1000;
        w->box_h = 256;
    }
    work_mark(WORK_DECODE);
    while (n--)
        for (y = 0; y < w->box_h; y++) {
            for (p = 0; p < 8; p++)
                pl[p] = (uint8_t *)(C2P_CHIP + p * bpr * w->box_h + y * bpr);
            C2P(w->in + (y * w->box_w) % w->in_len, w->box_w / 32, pl);
        }
    work_mark(WORK_DONE);
    w->out = (uint8_t *)C2P_CHIP;
    w->out_len = bpr * w->box_h * 8;
    w->dw = w->box_w;
    w->dh = w->box_h;
    return 0;
}
