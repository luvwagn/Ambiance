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
#define FreeVec(p) free(p)
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
    ULONG *sum;                 /* dw x (r, g, b, n): the output row being made */
    UBYTE *out;                 /* dw x dh RGB */
    ULONG next, oy, y1;         /* the next source row; the output row, its span's end */
    ULONG last_n, rec;
};

void shrink_free(struct Shrink *k)
{
    if (!k)
        return;
    FreeVec(k->xb);
    FreeVec(k->sum);
    FreeVec(k->out);
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
    k->xb = AllocVec((dw + 1) * sizeof(ULONG), MEMF_ANY);
    k->sum = AllocVec(dw * 4 * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
    k->out = AllocVec(dw * dh * 3, MEMF_ANY | MEMF_CLEAR);
    if (!k->xb || !k->sum || !k->out) {
        shrink_free(k);
        return NULL;
    }
    for (x = 0; x <= dw; x++)
        k->xb[x] = x * sw / dw;
    k->y1 = sh / dh;                        /* output row 0's span: 0 .. y1 */
    if (!k->y1)
        k->y1 = 1;
    return k;
}

/* the output row made so far (its sums) out, the sums cleared */
static void shrink_emit(struct Shrink *k)
{
    UBYTE *d = k->out + k->oy * k->dw * 3;
    ULONG x;
    for (x = 0; x < k->dw; x++) {
        ULONG *s = k->sum + x * 4, n = s[3] ? s[3] : 1, h = n >> 1, r, g, b;
        if (n != k->last_n) {
            k->last_n = n;
            k->rec = 65536UL / n + 1;
        }
        r = ((s[0] + h) * k->rec) >> 16;
        g = ((s[1] + h) * k->rec) >> 16;
        b = ((s[2] + h) * k->rec) >> 16;
        *d++ = (UBYTE)(r > 255 ? 255 : r);
        *d++ = (UBYTE)(g > 255 ? 255 : g);
        *d++ = (UBYTE)(b > 255 ? 255 : b);
    }
    memset(k->sum, 0, k->dw * 4 * sizeof(ULONG));
    k->oy++;
    k->y1 = (k->oy + 1) * k->sh / k->dh;
}

void shrink_rows(struct Shrink *k, const UBYTE *rgb, LONG y, LONG n, LONG stride)
{
    LONG i;
    for (i = 0; i < n; i++) {
        const UBYTE *p = rgb + i * stride;
        ULONG x, j = (ULONG)(y + i);
        if (j != k->next || j >= k->sh || k->oy >= k->dh)
            continue;                       /* (out of order or past the end: not ours) */
        for (x = 0; x < k->dw; x++) {
            /* a span summed in registers, then added (as deep_thumb) */
            ULONG c = k->xb[x], e = k->xb[x + 1], *s = k->sum + x * 4, a = 0, g = 0, b = 0;
            const UBYTE *q = p + c * 3;
            if (e <= c)
                e = c + 1;
            s[3] += e - c;
            for (; c < e; c++, q += 3) {
                a += q[0];
                g += q[1];
                b += q[2];
            }
            s[0] += a;
            s[1] += g;
            s[2] += b;
        }
        k->next++;
        if (k->next >= k->y1)
            shrink_emit(k);
    }
}

/* the dw x dh RGB (the caller's to FreeVec), the rest freed; rows never
 * come (a damaged file's) stay black */
UBYTE *shrink_done(struct Shrink *k)
{
    UBYTE *o;
    if (!k)
        return NULL;
    if (k->oy < k->dh && k->sum[3])
        shrink_emit(k);                     /* (a partial last row) */
    o = k->out;
    k->out = NULL;
    shrink_free(k);
    return o;
}

