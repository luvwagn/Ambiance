/*
 * jtest - jdec on the host (and on a 68020 under QEMU): a JPEG to a PPM
 * Copyright (c) 2026 Brian King, MIT
 *
 *   jtest in.jpg out.ppm [scale]      scale 0-3 (1/1 - 1/8)
 *   jtest -i in.jpg                   the header only
 *
 * Exit: 0 decoded, 2 decoded but damaged, 3 not decoded (jdec's error on
 * stderr), 1 usage or I/O. Also used by the fuzz test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../jdec.h"

static FILE *in;
static FILE *out;
static long allocated, peak;

static int32_t rd(void *ctx, uint8_t *buf, int32_t n)
{
    (void)ctx;
    return (int32_t)fread(buf, 1, (size_t)n, in);
}

static void *al(void *ctx, uint32_t n)
{
    uint32_t *p = malloc(n + 8);
    (void)ctx;
    if (!p)
        return NULL;
    p[0] = n;
    allocated += n;
    if (allocated > peak)
        peak = allocated;
    return p + 2;
}

static void fr(void *ctx, void *p)
{
    uint32_t *q = (uint32_t *)p - 2;
    (void)ctx;
    allocated -= q[0];
    free(q);
}

static int32_t next_y;

static int rows(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride)
{
    int32_t i;
    (void)ctx;
    if (y != next_y) {
        fprintf(stderr, "rows out of order: %ld after %ld\n", (long)y, (long)next_y);
        exit(1);
    }
    next_y = y + n;
    if (out)
        for (i = 0; i < n; i++)
            fwrite(rgb + i * stride, 1, (size_t)stride, out);
    return 0;
}

int main(int argc, char **argv)
{
    JDecIO io = { NULL, rd, al, fr, rows };
    JDecInfo info;
    JDec *d;
    int r, scale = 0, header = 0;
    int32_t w, h;
    if (argc >= 3 && !strcmp(argv[1], "-i")) {
        header = 1;
        argv++;
        argc--;
    }
    if (argc < (header ? 2 : 3)) {
        fprintf(stderr, "usage: jtest in.jpg out.ppm [scale] | jtest -i in.jpg\n");
        return 1;
    }
    if (!(in = fopen(argv[1], "rb")))
        return 1;
    r = jd_open(&d, &io, &info);
    if (header || r) {
        printf("%ldx%ld comps %d progressive %d rgb %d sampling %d%d %d%d %d%d restart %d: %d\n",
               (long)info.width, (long)info.height, info.ncomp, info.progressive, info.rgb,
               info.h[0], info.v[0], info.h[1], info.v[1], info.h[2], info.v[2], info.restart, r);
        if (!r)
            jd_close(d);
        return r ? 3 : 0;
    }
    if (argc > 3)
        scale = atoi(argv[3]);
    jd_out_size(&info, scale, &w, &h);
    if (strcmp(argv[2], "-")) {
        if (!(out = fopen(argv[2], "wb")))
            return 1;
        fprintf(out, "P6\n%ld %ld\n255\n", (long)w, (long)h);
    }
    r = jd_decode(d, scale);
    if (r) {
        fprintf(stderr, "jd_decode: %d\n", r);
        jd_close(d);
        return 3;
    }
    if (next_y != h) {
        fprintf(stderr, "rows: %ld of %ld\n", (long)next_y, (long)h);
        return 1;
    }
    r = jd_damaged(d);
    jd_close(d);
    if (allocated) {
        fprintf(stderr, "leaked %ld bytes\n", allocated);
        return 1;
    }
    if (getenv("JTEST_PEAK"))
        fprintf(stderr, "peak %ld bytes\n", peak);
    if (out)
        fclose(out);
    fclose(in);
    return r ? 2 : 0;
}
