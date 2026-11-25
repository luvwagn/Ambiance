/*
 * ham8.c on the host: a picture encoded as HAM8 (once and twice across),
 * decoded again as AGA would show it, and measured against the picture
 * (PSNR, its RGB as the same YCbCr formula gives it).
 *
 *   ham8_host                       a made picture: bars, ramps, edges
 *   ham8_host file.yuv W H [n]      n 4:2:0 pictures of a raw file (ffmpeg
 *                                   -pix_fmt yuv420p -f rawvideo), each
 */
#define AMB_HOST 1
#include "../src/ham8.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Ham8 h;

static int c255(long v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }

/* the picture's own RGB (8 bits): BT.601, studio range */
static void ref_rgb(const UBYTE *Y, const UBYTE *U, const UBYTE *V, int w, int x, int *rgb)
{
    double y = 1.164 * (Y[x] - 16), u = U[x >> 1] - 128.0, v = V[x >> 1] - 128.0;
    (void)w;
    rgb[0] = c255(lround(y + 1.596 * v));
    rgb[1] = c255(lround(y - 0.392 * u - 0.813 * v));
    rgb[2] = c255(lround(y + 2.017 * u));
}

/* a row of HAM8 bytes as shown: each pixel's 8-bit RGB (the top 6 bits
 * set, the low 2 kept from the pixel before or the register's) */
static int decode(const UBYTE *b, int n, int *rgb)
{
    int c[3], i, bad = 0;
    ULONG r, g, bl;
    ham8_rgb(&h, 0, &r, &g, &bl);
    c[0] = (int)(r & 255);
    c[1] = (int)(g & 255);
    c[2] = (int)(bl & 255);
    for (i = 0; i < n; i++) {
        int ctl = b[i] >> 6, d = b[i] & 63;
        if (ctl == 0) {
            if (d >= 17 && d <= 19)
                bad++;                      /* (the pointer's registers: never used) */
            ham8_rgb(&h, d, &r, &g, &bl);
            c[0] = (int)(r & 255);
            c[1] = (int)(g & 255);
            c[2] = (int)(bl & 255);
        } else {
            int ch = ctl == 2 ? 0 : ctl == 3 ? 1 : 2;
            c[ch] = (d << 2) | (c[ch] & 3);
        }
        rgb[i * 3] = c[0];
        rgb[i * 3 + 1] = c[1];
        rgb[i * 3 + 2] = c[2];
    }
    return bad;
}

/* PSNR of one picture, once (lores) or twice across (hires: the pair's
 * average for the source pixel, as the eye sees it) */
static double measure(const UBYTE *Y, const UBYTE *U, const UBYTE *V, int w, int ht, int twice, int *bad)
{
    UBYTE *row = malloc((size_t)w * 2);
    int *rgb = malloc(sizeof(int) * (size_t)w * 6), x, y, k;
    double se = 0;
    for (y = 0; y < ht; y++) {
        const UBYTE *yr = Y + y * w, *ur = U + (y >> 1) * (w / 2), *vr = V + (y >> 1) * (w / 2);
        ham8_row(&h, yr, ur, vr, (ULONG)w, row, twice);
        *bad += decode(row, twice ? 2 * w : w, rgb);
        for (x = 0; x < w; x++) {
            int ref[3];
            ref_rgb(yr, ur, vr, w, x, ref);
            for (k = 0; k < 3; k++) {
                double v = twice ? (rgb[(2 * x) * 3 + k] + rgb[(2 * x + 1) * 3 + k]) / 2.0 : rgb[x * 3 + k];
                se += (v - ref[k]) * (v - ref[k]);
            }
        }
    }
    free(row);
    free(rgb);
    se /= (double)w * ht * 3;
    return se > 0 ? 10 * log10(255.0 * 255.0 / se) : 99;
}

/* bars, a grey ramp, a colour ramp, soft gradients and hard edges */
static void made(UBYTE *Y, UBYTE *U, UBYTE *V, int w, int ht)
{
    int x, y;
    for (y = 0; y < ht; y++)
        for (x = 0; x < w; x++) {
            int yy, band = y * 4 / ht;
            if (band == 0)
                yy = 16 + x * 219 / (w - 1);                    /* a grey ramp */
            else if (band == 1)
                yy = 60 + ((x / 40) & 1) * 120;                 /* edges */
            else
                yy = 40 + (x + y) % 180;
            Y[y * w + x] = (UBYTE)yy;
        }
    for (y = 0; y < ht / 2; y++)
        for (x = 0; x < w / 2; x++) {
            int band = y * 8 / ht, u, v;
            if (band == 0) {
                u = v = 128;
            } else if (band == 1) {
                u = 128 + ((x / 20) % 3 - 1) * 90;              /* bars */
                v = 128 + ((x / 30) % 3 - 1) * 90;
            } else if (band == 2) {
                u = 16 + x * 224 / (w / 2);                     /* a colour ramp */
                v = 240 - x * 224 / (w / 2);
            } else {
                u = 100 + y % 50;                               /* skin and sky */
                v = 150 - x % 40;
            }
            U[y * (w / 2) + x] = (UBYTE)u;
            V[y * (w / 2) + x] = (UBYTE)v;
        }
}

int main(int argc, char **argv)
{
    int w = 320, ht = 240, n = 1, i, bad = 0, fail = 0;
    FILE *f = NULL;
    UBYTE *buf;
    ham8_init(&h);
    if (argc >= 4) {
        w = atoi(argv[2]);
        ht = atoi(argv[3]);
        n = argc > 4 ? atoi(argv[4]) : 1;
        if (!(f = fopen(argv[1], "rb"))) {
            perror(argv[1]);
            return 2;
        }
    }
    buf = malloc((size_t)w * ht * 3 / 2);
    for (i = 0; i < n; i++) {
        double lo, hi;
        UBYTE *Y = buf, *U = buf + w * ht, *V = U + (w / 2) * (ht / 2);
        if (f) {
            if (fread(buf, 1, (size_t)w * ht * 3 / 2, f) != (size_t)w * ht * 3 / 2)
                break;
        } else
            made(Y, U, V, w, ht);
        lo = measure(Y, U, V, w, ht, 0, &bad);
        hi = measure(Y, U, V, w, ht, 1, &bad);
        printf("picture %d: HAM8 once across %.1f dB, twice across %.1f dB\n", i, lo, hi);
        if (!f && (lo < 27.0 || hi < 31.0 || hi < lo))
            fail = 1;
    }
    if (bad) {
        printf("%d pixels used the pointer's registers (17-19)\n", bad);
        fail = 1;
    }
    free(buf);
    if (f)
        fclose(f);
    return fail;
}
