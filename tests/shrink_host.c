/*
 * shrink_host - src/shrink.c (jdec's rows made small as they come) against
 * the box averages of deep_thumb (src/thumb.c), worked out here the same
 * way from the whole picture: spans, sums, (sum + n/2) * (65536/n + 1) >> 16.
 * Random pictures and sizes, rows given in random bands as jdec gives them.
 * Copyright (c) 2026 Brian King, MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/shrink.h"

static unsigned long rnd_s = 12345;
static unsigned long rnd(unsigned long n)
{
    rnd_s = rnd_s * 1103515245UL + 12345UL;
    return ((rnd_s >> 8) & 0xFFFFFF) % n;
}

/* deep_thumb's, from the whole picture */
static void ref(const unsigned char *src, unsigned long sw, unsigned long sh, unsigned char *dst,
                unsigned long dw, unsigned long dh)
{
    unsigned long x, y, rmax = (sh + dh - 1) / dh + 1, *xb = calloc(dw + 1, sizeof(*xb));
    for (x = 0; x <= dw; x++)
        xb[x] = x * sw / dw;
    for (y = 0; y < dh; y++) {
        unsigned long sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh, r;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        if (sy1 > sh)
            sy1 = sh;
        if (sy1 - sy0 > rmax)
            sy1 = sy0 + rmax;
        for (x = 0; x < dw; x++) {
            unsigned long k = xb[x], e = xb[x + 1], s0 = 0, s1 = 0, s2 = 0, n = 0, h, rec, v[3];
            int c;
            if (e <= k)
                e = k + 1;
            for (r = sy0; r < sy1; r++) {
                const unsigned char *q = src + (r * sw + k) * 3;
                unsigned long i;
                n += e - k;
                for (i = k; i < e; i++, q += 3) {
                    s0 += q[0];
                    s1 += q[1];
                    s2 += q[2];
                }
            }
            if (!n)
                n = 1;
            h = n >> 1;
            rec = 65536UL / n + 1;
            v[0] = ((s0 + h) * rec) >> 16;
            v[1] = ((s1 + h) * rec) >> 16;
            v[2] = ((s2 + h) * rec) >> 16;
            for (c = 0; c < 3; c++)
                dst[(y * dw + x) * 3 + c] = (unsigned char)(v[c] > 255 ? 255 : v[c]);
        }
    }
    free(xb);
}

int main(void)
{
    int t, failed = 0, runs = 0;
    for (t = 0; t < 3000; t++) {
        unsigned long sw = 1 + rnd(t < 2000 ? 64 : 700), sh = 1 + rnd(t < 2000 ? 64 : 500);
        unsigned long dw = 1 + rnd(sw), dh = 1 + rnd(sh), i, y, worst = 0;
        unsigned char *src = malloc(sw * sh * 3), *want = malloc(dw * dh * 3), *got;
        struct Shrink *k;
        int smooth = rnd(2);
        for (i = 0; i < sw * sh * 3; i++)
            src[i] = smooth ? (unsigned char)((i / 3 % sw) * 255 / sw + rnd(8)) : (unsigned char)rnd(256);
        ref(src, sw, sh, want, dw, dh);
        if (!(k = shrink_new(sw, sh, dw, dh))) {
            printf("shrink_new %lux%lu -> %lux%lu: NULL\n", sw, sh, dw, dh);
            failed++;
            continue;
        }
        for (y = 0; y < sh;) {              /* bands, as jdec's: 1 to 16 rows, stride a little wider */
            unsigned long n = 1 + rnd(16), stride = sw * 3 + rnd(3) * 4, r;
            unsigned char *band;
            if (n > sh - y)
                n = sh - y;
            band = malloc(n * stride);
            for (r = 0; r < n; r++)
                memcpy(band + r * stride, src + (y + r) * sw * 3, sw * 3);
            shrink_rows(k, band, (long)y, (long)n, (long)stride);
            free(band);
            y += n;
        }
        got = shrink_done(k);
        for (i = 0; got && i < dw * dh * 3; i++) {
            unsigned long d = got[i] > want[i] ? got[i] - want[i] : want[i] - got[i];
            if (d > worst)
                worst = d;
        }
        runs++;
        if (!got || worst) {
            printf("%lux%lu -> %lux%lu: %s (worst %lu)\n", sw, sh, dw, dh, got ? "differs" : "NULL", worst);
            failed++;
        }
        free(got);
        free(src);
        free(want);
    }
    /* enlarging is not its: refused */
    if (shrink_new(10, 10, 11, 10) || shrink_new(10, 10, 10, 11)) {
        printf("enlarging was not refused\n");
        failed++;
    }
    printf("%d pictures, %d failed\n", runs, failed);
    return failed ? 1 : 0;
}
