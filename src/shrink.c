/*
 * shrink - a picture made small as its rows come (Ambiance)
 * Copyright (c) 2026 Brian King, MIT
 *
 * Built for the Amiga with Ambiance, and on the host (SHRINK_HOST) for
 * tests/shrink_host.c, which checks it against deep_thumb's arithmetic.
 */
#ifdef SHRINK_HOST
#include <stdlib.h>
#define AllocVec(n, f) calloc(1, (n))
#define FreeVec(p) free((void *)(p))
#define CopyMem(s, d, n) memcpy((d), (s), (n))
typedef void *APTR;
#define MEMF_ANY 0
#define MEMF_CLEAR 0
#else
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#endif
#include <string.h>
#include "shrink.h"

/* A picture made small as its rows come (jdec's, a band at a time), so
 * the whole decoded picture is never held: a 12 MP photo at half size was
 * 9 MB of RGB, written and then read again by deep_thumb. The same box
 * averages as deep_thumb's (its spans, its rounding), into dw x dh RGB,
 * which thumb_from_rgb then takes 1:1. Only for making smaller (dw <= sw,
 * dh <= sh: each source row in one output row's span); rows in order. */
struct Shrink {
    ULONG sw, sh, dw, dh;
    ULONG *xb;                  /* dw + 1 column spans */
    UBYTE *out;                 /* dw x dh RGB */
    ULONG next, oy;             /* the next source row; the next output row */
    ULONG rmax;                 /* source rows an output row takes, at most */
    UBYTE *carry[2];            /* rows kept from one band for the next (rmax each), */
    ULONG cy, cn;               /* the first of them and how many (in carry[0]) */
    const UBYTE **row;          /* rmax row pointers: the output row's source rows */
    ULONG last_n, rec;
};

/* 1.27 of zzprobe / this file's second form: each output pixel averaged
 * straight from its source rows, its sums in registers. The first form
 * added each source row into a row of sums (16 bytes an output pixel, 16 KB
 * at 1024 wide): on a real 68060, whose data cache is 8 KB, that was 1-4.6
 * s of a photo. Rows of a span that a band ends in the middle of are kept
 * (copied) for the next band. */

void shrink_free(struct Shrink *k)
{
    if (!k)
        return;
    FreeVec(k->xb);
    FreeVec(k->out);
    FreeVec(k->carry[0]);
    FreeVec(k->carry[1]);
    FreeVec((APTR)k->row);
    FreeVec(k);
}

struct Shrink *shrink_new(ULONG sw, ULONG sh, ULONG dw, ULONG dh)
{
    struct Shrink *k;
    ULONG x;
    if (!dw || !dh || dw > sw || dh > sh || !(k = AllocVec(sizeof(*k), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    k->sw = sw;
    k->sh = sh;
    k->dw = dw;
    k->dh = dh;
    k->rmax = (sh + dh - 1) / dh + 1;
    k->xb = AllocVec((dw + 1) * sizeof(ULONG), MEMF_ANY);
    k->out = AllocVec(dw * dh * 3, MEMF_ANY | MEMF_CLEAR);
    k->carry[0] = AllocVec(k->rmax * sw * 3, MEMF_ANY);
    k->carry[1] = AllocVec(k->rmax * sw * 3, MEMF_ANY);
    k->row = AllocVec(k->rmax * sizeof(*k->row), MEMF_ANY);
    if (!k->xb || !k->out || !k->carry[0] || !k->carry[1] || !k->row) {
        shrink_free(k);
        return NULL;
    }
    for (x = 0; x <= dw; x++)
        k->xb[x] = x * sw / dw;
    return k;
}

/* output row oy from its source rows (k->row[0 .. nr-1]) */
static void shrink_out(struct Shrink *k, ULONG oy, ULONG nr)
{
    UBYTE *d = k->out + oy * k->dw * 3;
    const ULONG *xb = k->xb;
    ULONG x;
    for (x = 0; x < k->dw; x++) {
        ULONG c0 = xb[x], c1 = xb[x + 1], a = 0, g = 0, b = 0, n, h, j;
        if (c1 <= c0)
            c1 = c0 + 1;
        for (j = 0; j < nr; j++) {
            const UBYTE *q = k->row[j] + c0 * 3, *e = k->row[j] + c1 * 3;
            while (q < e) {
                a += q[0];
                g += q[1];
                b += q[2];
                q += 3;
            }
        }
        n = (c1 - c0) * nr;
        if (n != k->last_n) {
            k->last_n = n;
            k->rec = 65536UL / n + 1;
        }
        h = n >> 1;
        a = ((a + h) * k->rec) >> 16;
        g = ((g + h) * k->rec) >> 16;
        b = ((b + h) * k->rec) >> 16;
        *d++ = (UBYTE)(a > 255 ? 255 : a);
        *d++ = (UBYTE)(g > 255 ? 255 : g);
        *d++ = (UBYTE)(b > 255 ? 255 : b);
    }
}

/* source row j: kept from an earlier band, or in this one */
static const UBYTE *shrink_src(struct Shrink *k, ULONG j, const UBYTE *rgb, LONG y, LONG stride)
{
    if (j < (ULONG)y)
        return k->carry[0] + (j - k->cy) * k->sw * 3;
    return rgb + (j - (ULONG)y) * stride;
}

void shrink_rows(struct Shrink *k, const UBYTE *rgb, LONG y, LONG n, LONG stride)
{
    ULONG end, y0, j, sw3 = k->sw * 3;
    if (n <= 0 || (ULONG)y != k->next || k->oy >= k->dh)
        return;                             /* (out of order or past the end: not ours) */
    end = (ULONG)y + (ULONG)n;
    if (end > k->sh)
        end = k->sh;
    k->next = end;
    /* every output row whose source rows are all here (kept or in this band) */
    for (;;) {
        ULONG y1;
        if (k->oy >= k->dh)
            return;
        y0 = k->oy * k->sh / k->dh;
        y1 = (k->oy + 1) * k->sh / k->dh;
        if (y1 <= y0)
            y1 = y0 + 1;
        if (y1 - y0 > k->rmax)
            y1 = y0 + k->rmax;
        if (y1 > end)
            break;
        for (j = y0; j < y1; j++)
            k->row[j - y0] = shrink_src(k, j, rgb, y, stride);
        shrink_out(k, k->oy, y1 - y0);
        k->oy++;
    }
    /* the next output row's rows that are here: kept for the next band */
    {
        UBYTE *nc = k->carry[1];
        ULONG c = 0;
        for (j = y0; j < end; j++, c++)
            CopyMem((APTR)shrink_src(k, j, rgb, y, stride), nc + c * sw3, sw3);
        k->carry[1] = k->carry[0];
        k->carry[0] = nc;
        k->cy = y0;
        k->cn = c;
    }
}

/* the dw x dh RGB (the caller's to FreeVec), the rest freed; rows never
 * come (a damaged file's) stay black, a last row half come is made from
 * what came */
UBYTE *shrink_done(struct Shrink *k)
{
    UBYTE *o;
    if (!k)
        return NULL;
    if (k->oy < k->dh && k->cn) {
        ULONG j;
        for (j = 0; j < k->cn && j < k->rmax; j++)
            k->row[j] = k->carry[0] + j * k->sw * 3;
        shrink_out(k, k->oy, j);
    }
    o = k->out;
    k->out = NULL;
    shrink_free(k);
    return o;
}
