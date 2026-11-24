/*
 * work_ham8.c - m68kprof's AGA video work: a 4:2:0 frame (the input file,
 * box_w x box_h, raw yuv420p) made into a row of 8-bit bytes per picture
 * row, n times: -DPENS anim.c's pens as today (va_pens68k, pens68k.s, its
 * tables made here: the timing is the tables', not their values), else
 * ham8.c's HAM8 (-DTWICE: twice across, a hires screen's).
 * Copyright (c) 2026 Brian King, MIT
 */
#include <stdint.h>
#include "work.h"
#ifndef PENS
#ifndef ASM
#define AMB_HOST 1
#endif
#include "../../src/ham8.h"
static struct Ham8 ham;
#else
void va_pens68k(const uint8_t *y, const uint16_t *cl, uint8_t *dst, uint32_t quads, const uint16_t *yt4,
                const uint8_t *lut);
#endif

int work_run(struct Work *w)
{
    uint32_t W = w->box_w, H = w->box_h, n = 4, y;
#ifdef PENS
    uint32_t x;
#endif
    const uint8_t *Y = w->in, *U = Y + W * H, *V = U + (W / 2) * (H / 2);
    uint8_t *row = work_alloc(W * 2 + 64);
#ifdef PENS
    uint16_t *yt4 = work_alloc(4 * 1024 * 2), *cl = work_alloc(W * 2);
    uint8_t *lut = work_alloc(8192);
    for (x = 0; x < 4096; x++)
        yt4[x] = (uint16_t)(((x >> 2) >> 3) << 8);
    for (x = 0; x < 8192; x++)
        lut[x] = (uint8_t)(x * 37);
#else
    ham8_init(&ham);
#endif
    work_mark(WORK_DECODE);
    while (n--)
        for (y = 0; y < H; y++) {
            const uint8_t *yr = Y + y * W, *ur = U + (y >> 1) * (W / 2), *vr = V + (y >> 1) * (W / 2);
#ifdef PENS
            if (!(y & 1))
                for (x = 0; x < W / 2; x++)
                    cl[x] = (uint16_t)(((ur[x] >> 4) << 4) | (vr[x] >> 4));
            va_pens68k(yr, cl, row, W / 4, yt4 + ((y & 3) << 10), lut);
#else
#ifdef TWICE
            ham8_row(&ham, yr, ur, vr, W, row, 1);
#else
            ham8_row(&ham, yr, ur, vr, W, row, 0);
#endif
#endif
        }
    work_mark(WORK_DONE);
    w->out = row;
    w->out_len = W;
    w->dw = W;
    w->dh = 1;
    return 0;
}
