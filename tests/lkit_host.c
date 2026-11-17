/* Host test for lkcore.c, the layouts' drawing kit: drawn into a picture
 * in memory that keeps only the bits a 15, 16 or 24-bit screen keeps, then
 * checked; a scene in each depth written as build/lkit-<bits>.ppm to look
 * at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/lkcore.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define W 800
#define H 600

struct Screen {
    int bits;
    unsigned long px[W * H];    /* as the screen keeps it, widened back to 8 bits */
    long fills, rows;           /* calls made */
};

/* what a screen of s->bits keeps of a colour */
static unsigned long keep(int bits, unsigned long c)
{
    unsigned long r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (bits >= 24)
        return c & 0xFFFFFF;
    r = (r >> 3) << 3 | r >> 5;
    b = (b >> 3) << 3 | b >> 5;
    g = bits == 16 ? ((g >> 2) << 2 | g >> 6) : ((g >> 3) << 3 | g >> 5);
    return r << 16 | g << 8 | b;
}

static void s_fill(void *ctx, long x, long y, long w, long h, unsigned long rgb)
{
    struct Screen *s = ctx;
    long i, j;
    s->fills++;
    if (x < 0 || y < 0 || x + w > W || y + h > H)
        printf("FAIL fill outside the screen: %ld,%ld %ldx%ld\n", x, y, w, h), fails++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            s->px[(y + j) * W + x + i] = keep(s->bits, rgb);
}

static void s_rows(void *ctx, long x, long y, long w, long h, const unsigned long *p, long mod)
{
    struct Screen *s = ctx;
    long i, j;
    s->rows++;
    if (x < 0 || y < 0 || x + w > W || y + h > H)
        printf("FAIL rows outside the screen: %ld,%ld %ldx%ld\n", x, y, w, h), fails++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            s->px[(y + j) * W + x + i] = keep(s->bits, p[j * mod + i]);
}

static unsigned long scratch[W * 16];

static void out_for(struct LkOut *o, struct Screen *s, int bits)
{
    memset(s, 0, sizeof(*s));
    s->bits = bits;
    o->ctx = s;
    o->fill = s_fill;
    o->rows = s_rows;
    o->bits = bits;
    o->buf = scratch;
    o->bufn = W * 16;
    lk_clip(o, 0, 0, W, H);
}

static int ch(unsigned long c, int sh) { return (int)((c >> sh) & 255); }
static int near(unsigned long a, unsigned long b, int tol)
{
    int sh;
    for (sh = 0; sh <= 16; sh += 8)
        if (abs(ch(a, sh) - ch(b, sh)) > tol)
            return 0;
    return 1;
}

static void write_ppm(const struct Screen *s, const char *name)
{
    FILE *f = fopen(name, "wb");
    long i;
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        fputc(ch(s->px[i], 16), f);
        fputc(ch(s->px[i], 8), f);
        fputc(ch(s->px[i], 0), f);
    }
    fclose(f);
}

/* a stand-in for text: coverage blobs the shape of letters' strokes */
static void fake_text(struct LkOut *o, long x, long y, int n, unsigned long fg, const struct LkBg *bg)
{
    static unsigned char cov[20 * 400];
    long w = n * 12, h = 20, i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            long cx = i % 12;
            int v = 0;
            if (j >= 4 && j < 18 && (cx == 2 || cx == 8))
                v = 15;                         /* stems */
            else if (j >= 4 && j < 18 && (cx == 1 || cx == 3 || cx == 7 || cx == 9))
                v = 6;                          /* their edges */
            else if ((j == 10 || j == 4) && cx > 2 && cx < 8)
                v = 12;                         /* bars */
            cov[j * w + i] = (unsigned char)v;
        }
    lk_cover(o, x, y, cov, w, h, w, fg, bg);
}

/* a Showcase screen's parts, as the layout will draw them */
static void scene(struct LkOut *o)
{
    struct LkBg bg, panel;
    lk_vgrad(o, 0, 0, W, H, 0x0F1D2C, 0x08111A);
    bg.top = 0x0F1D2C; bg.bottom = 0x08111A; bg.y = 0; bg.h = H;
    lk_fill(o, 0, 0, 204, H, 0x0E1A27);                          /* the menu's column */
    lk_rrect(o, 18, 230, 172, 48, 4, 0x1E3A55, 0x0E1A27);         /* its selection */
    lk_fill(o, 18, 230, 3, 48, 0x7FD3FF);
    lk_bg_flat(&panel, 0x1E3A55);
    fake_text(o, 66, 244, 5, 0xFFFFFF, &panel);
    fake_text(o, 228, 70, 14, 0xB9C7D3, &bg);                    /* a heading on the gradient */
    {
        long i;
        for (i = 0; i < 5; i++) {
            unsigned long c = i == 0 ? 0xFF6B35 : 0x3FA0FF - (unsigned long)i * 0x101010;
            lk_hgrad(o, 228 + i * 108, 96, 96, 96, c, 0xC2185B);  /* covers */
        }
        lk_frame(o, 223, 91, 106, 106, 3, 0x7FD3FF);             /* the focus */
    }
    lk_rrect(o, 228, 404, 100, 56, 4, 0x132131, LK_NOBG);         /* a source chip, hard corners */
    lk_rrect(o, 262, 330, 514, 60, 6, 0x132131, 0x0B1520);
    lk_rrect(o, 228, 480, 540, 8, 4, 0x22364B, 0x0B1520);         /* a progress line */
    lk_rrect(o, 228, 480, 300, 8, 4, 0x7FD3FF, 0x22364B);
}

int main(void)
{
    static struct Screen s;
    struct LkOut o;
    int bits[3] = { 15, 16, 24 }, b;

    /* mixing */
    CHECK(lk_mix(0x000000, 0xFFFFFF, 0, 10) == 0x000000);
    CHECK(lk_mix(0x000000, 0xFFFFFF, 10, 10) == 0xFFFFFF);
    CHECK(lk_mix(0x102030, 0x203040, 1, 2) == 0x182838);
    CHECK(lk_mix(0x123456, 0x654321, 5, 0) == 0x123456);

    /* fills clip */
    out_for(&o, &s, 24);
    lk_clip(&o, 10, 10, 20, 20);
    lk_fill(&o, 0, 0, 100, 100, 0xFFFFFF);
    CHECK(s.px[9 * W + 9] == 0 && s.px[10 * W + 10] == 0xFFFFFF && s.px[19 * W + 19] == 0xFFFFFF && s.px[20 * W + 20] == 0);
    lk_fill(&o, 30, 30, 5, 5, 0xFFFFFF);
    CHECK(s.fills == 1);                                          /* outside: no call */

    /* a 24-bit vertical gradient: its ends exact, monotonic, rows merged */
    out_for(&o, &s, 24);
    lk_vgrad(&o, 0, 0, 10, 256, 0x000000, 0x0000FF);
    CHECK(s.px[0] == 0x000000 && s.px[255 * W] == 0x0000FF && s.px[128 * W] == 0x000080);
    CHECK(s.fills == 256 && s.rows == 0);
    out_for(&o, &s, 24);
    lk_vgrad(&o, 0, 0, 10, 600, 0x000000, 0x00000F);             /* 16 colours over 600 rows */
    CHECK(s.fills == 16);

    /* 16-bit: dithered, each 4 x 4 block averaging its row's colour */
    out_for(&o, &s, 16);
    lk_vgrad(&o, 0, 0, 64, 64, 0x102030, 0x405060);
    {
        long r, ok = 1;
        for (r = 0; r < 60; r += 4) {
            long i, j, sum[3] = { 0, 0, 0 };
            unsigned long want = lk_vgrad_at(0x102030, 0x405060, 64, r + 1);
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    unsigned long p = s.px[(r + j) * W + i];
                    sum[0] += ch(p, 16); sum[1] += ch(p, 8); sum[2] += ch(p, 0);
                }
            if (abs((int)(sum[0] / 16) - ch(want, 16)) > 4 || abs((int)(sum[1] / 16) - ch(want, 8)) > 3 ||
                abs((int)(sum[2] / 16) - ch(want, 0)) > 4)
                ok = 0;
        }
        CHECK(ok);
        CHECK(s.rows > 0);
    }

    /* horizontal gradient: its ends */
    out_for(&o, &s, 24);
    lk_hgrad(&o, 100, 10, 100, 30, 0xFF0000, 0x0000FF);
    CHECK(s.px[10 * W + 100] == 0xFF0000 && s.px[39 * W + 199] == 0x0000FF);
    CHECK(s.px[20 * W + 99] == 0 && s.px[20 * W + 200] == 0);

    /* rounded box: the middle full, the corner its background, symmetric */
    out_for(&o, &s, 24);
    lk_fill(&o, 0, 0, W, H, 0x000000);
    lk_rrect(&o, 100, 100, 60, 40, 8, 0xFFFFFF, 0x000000);
    CHECK(s.px[120 * W + 130] == 0xFFFFFF);
    CHECK(s.px[100 * W + 100] == 0x000000);
    CHECK(s.px[100 * W + 108] == 0xFFFFFF);
    CHECK(s.px[101 * W + 101] == s.px[101 * W + 158] && s.px[101 * W + 101] == s.px[138 * W + 101]);
    CHECK(s.px[101 * W + 101] != 0 || s.px[102 * W + 102] != 0);   /* blended, not cut */
    {
        unsigned long edge = s.px[100 * W + 104];                   /* part covered */
        CHECK(edge != 0xFFFFFF && edge != 0x000000);
    }
    /* hard corners: nothing outside them drawn */
    lk_fill(&o, 0, 0, W, H, 0x123456);
    lk_rrect(&o, 100, 100, 60, 40, 8, 0xFFFFFF, LK_NOBG);
    CHECK(s.px[100 * W + 100] == 0x123456 && s.px[120 * W + 100] == 0xFFFFFF);
    /* a radius too big for the box: a pill */
    lk_rrect(&o, 300, 100, 40, 10, 99, 0xFFFFFF, 0x123456);
    CHECK(s.px[105 * W + 320] == 0xFFFFFF);

    /* frame */
    out_for(&o, &s, 24);
    lk_frame(&o, 10, 10, 20, 20, 2, 0xFFFFFF);
    CHECK(s.px[10 * W + 10] == 0xFFFFFF && s.px[11 * W + 11] == 0xFFFFFF && s.px[12 * W + 12] == 0 && s.px[29 * W + 29] == 0xFFFFFF);

    /* text over a gradient: full coverage the ink, none each row's background */
    out_for(&o, &s, 24);
    {
        struct LkBg bg = { 0x000000, 0x0000FF, 0, 256 };
        static unsigned char cov[16 * 256];
        long j;
        for (j = 0; j < 256; j++) {
            memset(cov + j * 16, 0, 16);
            cov[j * 16 + 8] = 15;
            cov[j * 16 + 9] = 8;
        }
        lk_cover(&o, 0, 0, cov, 16, 256, 16, 0xFFFFFF, &bg);
        CHECK(s.px[0] == 0x000000 && s.px[255 * W] == 0x0000FF && s.px[100 * W] == lk_bg_at(&bg, 100));
        CHECK(s.px[100 * W + 8] == 0xFFFFFF);
        CHECK(near(s.px[100 * W + 9], lk_mix(lk_bg_at(&bg, 100), 0xFFFFFF, 8, 15), 0));
    }

    /* off the screen's edge: clipped, never written outside */
    out_for(&o, &s, 16);
    lk_vgrad(&o, -50, -50, W + 100, 100, 0x000000, 0xFFFFFF);
    lk_hgrad(&o, W - 30, H - 30, 100, 100, 0x000000, 0xFFFFFF);
    lk_rrect(&o, -10, H - 20, 60, 60, 12, 0xFFFFFF, 0);
    CHECK(s.px[0] != 0 || s.px[1] != 0);

    /* the scene, in each depth */
    for (b = 0; b < 3; b++) {
        char name[64];
        out_for(&o, &s, bits[b]);
        scene(&o);
        snprintf(name, sizeof(name), "build/lkit-%d.ppm", bits[b]);
        write_ppm(&s, name);
        printf("scene at %d bits: %ld fills, %ld blocks of pixels\n", bits[b], s.fills, s.rows);
    }

    printf("%d failed\n", fails);
    return fails != 0;
}
