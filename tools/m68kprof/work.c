/*
 * work.c - the jdec + shrink work m68kprof measures, as zzprobe's "own"
 * column and Ambiance's jpgthumb.c do it: the JPEG fitted in a box (its
 * shape kept, never made larger), jdec at the smallest of 1/1..1/8 still at
 * least that size, its rows made small as they come (shrink.c).
 *
 * Built twice: for the host (the reference output) and for the 68k (run
 * in m68kprof under Musashi, counted). Copyright (c) 2026 Brian King, MIT
 */
#include <stdint.h>
#include <string.h>
#include "../../src/jdec/jdec.h"
#include "../../src/shrink.h"
#include "work.h"

struct Src {
    const uint8_t *p;
    uint32_t n, pos;
    struct Shrink *sk;
};

static int32_t w_read(void *ctx, uint8_t *buf, int32_t n)
{
    struct Src *s = ctx;
    if ((uint32_t)n > s->n - s->pos)
        n = (int32_t)(s->n - s->pos);
    memcpy(buf, s->p + s->pos, (size_t)n);
    s->pos += (uint32_t)n;
    return n;
}

static void *w_alloc(void *ctx, uint32_t n)
{
    (void)ctx;
    return work_alloc(n);
}

static void w_free(void *ctx, void *p)
{
    (void)ctx;
    work_free(p);
}

static int w_rows(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride)
{
    shrink_rows(((struct Src *)ctx)->sk, rgb, y, n, stride);
    return 0;
}

static void fit(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h, uint32_t *dw, uint32_t *dh)
{
    if (sw <= w && sh <= h) {
        *dw = sw;
        *dh = sh;
        return;
    }
    *dw = w;
    *dh = sh * w / sw;
    if (*dh > h) {
        *dh = h;
        *dw = sw * h / sh;
    }
    if (!*dw)
        *dw = 1;
    if (!*dh)
        *dh = 1;
}

int work_run(struct Work *w)
{
    struct Src s;
    JDecIO io;
    JDecInfo info;
    JDec *d = NULL;
    int32_t ow, oh;
    int r;
    memset(&s, 0, sizeof(s));
    s.p = w->in;
    s.n = w->in_len;
    io.ctx = &s;
    io.read = w_read;
    io.alloc = w_alloc;
    io.free = w_free;
    io.rows = w_rows;
    work_mark(WORK_OPEN);
    if ((r = jd_open(&d, &io, &info)))
        return r;
    fit((uint32_t)info.width, (uint32_t)info.height, w->box_w, w->box_h, &w->dw, &w->dh);
    w->scale = jd_scale_for(&info, (int32_t)w->dw, (int32_t)w->dh);
    jd_out_size(&info, w->scale, &ow, &oh);
    w->bw = (uint32_t)ow;
    w->bh = (uint32_t)oh;
    if (w->dw > w->bw || w->dh > w->bh || !(s.sk = shrink_new(w->bw, w->bh, w->dw, w->dh))) {
        jd_close(d);
        return -100;
    }
    work_mark(WORK_DECODE);
    r = jd_decode(d, w->scale);
    jd_close(d);
    work_mark(WORK_DONE);
    w->out = shrink_done(s.sk);
    return r;
}
