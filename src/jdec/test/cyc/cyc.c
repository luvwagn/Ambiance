/*
 * cyc - jdec on a bare 68020 (Musashi, by cyc.py) to count its cycles
 * Copyright (c) 2026 Brian King, MIT
 *
 * No OS: the JPEG is in memory, allocations come from a bump heap, and
 * rows go to an output buffer (or nowhere, when only timing).
 */
#include "../../jdec.h"

#define PARAMS ((volatile uint32_t *)0x00F000)   /* set by cyc.py */
/* PARAMS[0] JPEG address, [1] its length, [2] scale, [3] output buffer
 * (0: rows not kept), [4] heap start; out: [8] result, [9] width,
 * [10] height, [11] heap used */

static uint8_t *in, *heap;
static uint32_t inlen, inpos, used;

void *memset(void *d, int c, unsigned long n)
{
    uint8_t *p = d;
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    if (!(((uint32_t)p | (uint32_t)q | n) & 3)) {
        uint32_t *pl = d;
        const uint32_t *ql = s;
        n >>= 2;
        while (n--)
            *pl++ = *ql++;
        return d;
    }
    while (n--)
        *p++ = *q++;
    return d;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

static int32_t rd(void *ctx, uint8_t *buf, int32_t n)
{
    int32_t i;
    (void)ctx;
    if (n > (int32_t)(inlen - inpos))
        n = (int32_t)(inlen - inpos);
    for (i = 0; i < n; i++)
        buf[i] = in[inpos + i];
    inpos += n;
    return n;
}

static void *al(void *ctx, uint32_t n)
{
    void *p = heap + used;
    (void)ctx;
    used += (n + 3) & ~3UL;
    return p;
}

static void fr(void *ctx, void *p)
{
    (void)ctx;
    (void)p;
}

static int rows(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride)
{
    (void)ctx;
    if (PARAMS[3])
        memcpy((uint8_t *)PARAMS[3] + y * stride, rgb, (unsigned long)(n * stride));
    return 0;
}

__attribute__((section(".text.start"))) uint32_t start(void)
{
    JDecIO io = { 0, rd, al, fr, rows };
    JDecInfo info;
    JDec *d;
    int32_t w, h;
    int r;
    in = (uint8_t *)PARAMS[0];
    inlen = PARAMS[1];
    heap = (uint8_t *)PARAMS[4];
    r = jd_open(&d, &io, &info);
    if (!r) {
        jd_out_size(&info, (int)PARAMS[2], &w, &h);
        PARAMS[9] = (uint32_t)w;
        PARAMS[10] = (uint32_t)h;
        r = jd_decode(d, (int)PARAMS[2]);
    }
    PARAMS[8] = (uint32_t)r;
    PARAMS[11] = used;
    return (uint32_t)r;
}
