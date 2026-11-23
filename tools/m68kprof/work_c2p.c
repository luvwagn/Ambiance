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
#elif defined(C2P_GFX)
#define C2P c_gfx                       /* (-DC2P_GFX: gfx.c's C, gfx_chunky's loop) */
#elif defined(C2P_LKIT)
#define C2P c_lkit                      /* (-DC2P_LKIT: lkit.c's C, ck_show's loop) */
#else
#define C2P va_c2p68k
#endif

/* gfx.c's c2p32 and gfx_chunky's row (as they are) */
#define MERGE(lo, hi, sh, m) \
    do { uint32_t t_ = (((hi) >> (sh)) ^ (lo)) & (m); (lo) ^= t_; (hi) ^= t_ << (sh); } while (0)
static void c2p32(const uint32_t *c, uint32_t *out)
{
    uint32_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3], a4 = c[4], a5 = c[5], a6 = c[6], a7 = c[7];
    MERGE(a0, a4, 16, 0x0000FFFFUL);
    MERGE(a1, a5, 16, 0x0000FFFFUL);
    MERGE(a2, a6, 16, 0x0000FFFFUL);
    MERGE(a3, a7, 16, 0x0000FFFFUL);
    MERGE(a0, a2, 8, 0x00FF00FFUL);
    MERGE(a1, a3, 8, 0x00FF00FFUL);
    MERGE(a4, a6, 8, 0x00FF00FFUL);
    MERGE(a5, a7, 8, 0x00FF00FFUL);
    MERGE(a0, a1, 4, 0x0F0F0F0FUL);
    MERGE(a2, a3, 4, 0x0F0F0F0FUL);
    MERGE(a4, a5, 4, 0x0F0F0F0FUL);
    MERGE(a6, a7, 4, 0x0F0F0F0FUL);
    MERGE(a0, a4, 2, 0x33333333UL);
    MERGE(a1, a5, 2, 0x33333333UL);
    MERGE(a2, a6, 2, 0x33333333UL);
    MERGE(a3, a7, 2, 0x33333333UL);
    MERGE(a0, a2, 1, 0x55555555UL);
    MERGE(a1, a3, 1, 0x55555555UL);
    MERGE(a4, a6, 1, 0x55555555UL);
    MERGE(a5, a7, 1, 0x55555555UL);
    out[0] = a7; out[1] = a5; out[2] = a3; out[3] = a1;
    out[4] = a6; out[5] = a4; out[6] = a2; out[7] = a0;
}
static void c_gfx(const uint8_t *src, uint32_t nl, uint8_t **planes)
{
    uint32_t x, k, pl[8];
    uint16_t *dst[8];
    const uint32_t *c = (const uint32_t *)src;
    for (k = 0; k < 8; k++)
        dst[k] = (uint16_t *)planes[k];
    for (x = 0; x < nl; x++, c += 8) {
        c2p32(c, pl);
        for (k = 0; k < 8; k++) {
            *(uint32_t *)dst[k] = pl[k];
            dst[k] += 2;
        }
    }
}

/* lkit.c's ck_c2p32 and ck_show's loop (the planes from the bitmap each
 * group, as there) */
#define CK_MERGE(x, y, s, m) { uint32_t t_ = (((x) >> (s)) ^ (y)) & (m); (y) ^= t_; (x) ^= t_ << (s); }
static void ck_c2p32(const uint8_t *px, uint32_t *out)
{
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    const uint32_t *q = (const uint32_t *)px;
    a0 = q[7]; a1 = q[6]; a2 = q[5]; a3 = q[4];
    a4 = q[3]; a5 = q[2]; a6 = q[1]; a7 = q[0];
    CK_MERGE(a0, a4, 16, 0x0000FFFFUL); CK_MERGE(a1, a5, 16, 0x0000FFFFUL);
    CK_MERGE(a2, a6, 16, 0x0000FFFFUL); CK_MERGE(a3, a7, 16, 0x0000FFFFUL);
    CK_MERGE(a0, a2, 8, 0x00FF00FFUL); CK_MERGE(a1, a3, 8, 0x00FF00FFUL);
    CK_MERGE(a4, a6, 8, 0x00FF00FFUL); CK_MERGE(a5, a7, 8, 0x00FF00FFUL);
    CK_MERGE(a0, a1, 4, 0x0F0F0F0FUL); CK_MERGE(a2, a3, 4, 0x0F0F0F0FUL);
    CK_MERGE(a4, a5, 4, 0x0F0F0F0FUL); CK_MERGE(a6, a7, 4, 0x0F0F0F0FUL);
    CK_MERGE(a0, a4, 2, 0x33333333UL); CK_MERGE(a1, a5, 2, 0x33333333UL);
    CK_MERGE(a2, a6, 2, 0x33333333UL); CK_MERGE(a3, a7, 2, 0x33333333UL);
    CK_MERGE(a0, a2, 1, 0x55555555UL); CK_MERGE(a1, a3, 1, 0x55555555UL);
    CK_MERGE(a4, a6, 1, 0x55555555UL); CK_MERGE(a5, a7, 1, 0x55555555UL);
    out[0] = a0; out[2] = a4; out[4] = a1; out[6] = a5;
    out[1] = a2; out[3] = a6; out[5] = a3; out[7] = a7;
}
static uint8_t **ck_planes;
static void c_lkit(const uint8_t *src, uint32_t nl, uint8_t **planes)
{
    uint32_t x, p, out[8];
    ck_planes = planes;
    for (x = 0; x < nl; x++) {
        ck_c2p32(src + x * 32, out);
        for (p = 0; p < 8; p++)
            *(uint32_t *)(ck_planes[p] + x * 4) = out[p];
    }
}

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
