/* spu.c on the host: a subpicture unit made here (run lengths of 1 to 4
 * nibbles, two fields, colours, opacities, an area, start and stop), split
 * across packets as a DVD has it, decoded and blended; a menu's button
 * highlighted; one shown only with subtitles off when forced.
 * MIT, (c) 2026 Brian King */
#include <stdio.h>
#include <string.h>
#include "exec/types.h"
#include "../../src/spu.h"

static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- an encoder: nibbles */
static UBYTE out[65536];
static unsigned nb;                         /* nibbles written */
static void put(unsigned v) { if (nb & 1) out[nb >> 1] |= v; else out[nb >> 1] = v << 4; nb++; }
static void run(unsigned len, unsigned val, int to_end)
{
    unsigned v;
    if (to_end) { put(0); put(0); put(0); put(val); return; }
    v = len << 2 | val;
    if (len < 4) put(v);
    else if (len < 16) { put(v >> 4); put(v & 15); }
    else if (len < 64) { put(v >> 8); put((v >> 4) & 15); put(v & 15); }
    else { put(v >> 12); put((v >> 8) & 15); put((v >> 4) & 15); put(v & 15); }
}
static void line_end(void) { if (nb & 1) put(0); }

/* the picture: W x H, value (x / 8 + y) & 3, but row 3 to its end as 2 */
#define W 150
#define H 6
static UBYTE want(int x, int y) { return y == 3 ? 2 : (UBYTE)(((x / 8) + y) & 3); }

static void encode_rows(int first)
{
    int y, x;
    for (y = first; y < H; y += 2) {
        if (y == 3) { run(0, 2, 1); line_end(); continue; }
        for (x = 0; x < W;) {
            int v = want(x, y), n = 0;
            while (x + n < W && want(x + n, y) == v && n < 255) n++;
            /* runs of every size: split a long one as 1 + 4 + 16 + 64 + ... */
            run(n, v, 0);
            x += n;
        }
        line_end();
    }
}

static ULONG make_unit(UBYTE *u, int forced, int with_stop)
{
    ULONG off1, off2, c, dcsq;
    nb = 4 * 2;                             /* after size and the DCSQ pointer */
    off1 = nb / 2;
    encode_rows(0);
    off2 = nb / 2;
    encode_rows(1);
    dcsq = nb / 2;
    memcpy(u, out, dcsq);
    c = dcsq;
    /* first DCSQ: delay 0, next -> second (or itself) */
    u[c++] = 0; u[c++] = 0;
    {
        ULONG second = dcsq + 4 + 1 + 3 + 3 + 7 + 5 + 1;
        u[c++] = (UBYTE)((with_stop ? second : dcsq) >> 8); u[c++] = (UBYTE)(with_stop ? second : dcsq);
    }
    u[c++] = forced ? 0x00 : 0x01;
    u[c++] = 0x03; u[c++] = 0x98; u[c++] = 0x76;        /* colours: 3->9, 2->8, 1->7, 0->6 */
    u[c++] = 0x04; u[c++] = 0xF8; u[c++] = 0x40;        /* opacities: 3->15, 2->8, 1->4, 0->0 */
    u[c++] = 0x05;                                      /* area: x 100..249, y 200..205 */
    u[c++] = 100 >> 4; u[c++] = (UBYTE)((100 & 15) << 4 | (249 >> 8)); u[c++] = 249 & 255;
    u[c++] = 200 >> 4; u[c++] = (UBYTE)((200 & 15) << 4 | (205 >> 8)); u[c++] = 205 & 255;
    u[c++] = 0x06; u[c++] = (UBYTE)(off1 >> 8); u[c++] = (UBYTE)off1; u[c++] = (UBYTE)(off2 >> 8); u[c++] = (UBYTE)off2;
    u[c++] = 0xFF;
    if (with_stop) {                        /* second: after 100 * 1024 ticks, stop */
        ULONG at = c;
        u[c++] = 0; u[c++] = 100; u[c++] = (UBYTE)(at >> 8); u[c++] = (UBYTE)at;
        u[c++] = 0x02; u[c++] = 0xFF;
    }
    u[0] = (UBYTE)(c >> 8); u[1] = (UBYTE)c;
    u[2] = (UBYTE)(dcsq >> 8); u[3] = (UBYTE)dcsq;
    return c;
}

int main(void)
{
    static UBYTE u[65536];
    ULONG n = make_unit(u, 0, 1), i;
    struct SpuPic p;
    struct Spu *s = spu_new();
    const struct SpuPic *q;
    int x, y, wrong = 0;
    /* decoded whole */
    CHECK(spu_decode(u, n, 9000, &p), "decode");
    CHECK(p.x == 100 && p.y == 200 && p.w == W && p.h == H, "area %d %d %d %d", p.x, p.y, p.w, p.h);
    CHECK(p.col[3] == 9 && p.col[0] == 6 && p.alpha[3] == 15 && p.alpha[2] == 8 && p.alpha[1] == 4 &&
          p.alpha[0] == 0, "colours");
    CHECK(p.start == 9000 && p.has_stop && p.stop == 9000 + 100 * 1024, "times %lu %lu",
          (unsigned long)p.start, (unsigned long)p.stop);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (p.pix[y * W + x] != want(x, y))
                wrong++;
    CHECK(!wrong, "%d pixels wrong", wrong);
    free(p.pix);
    /* in three packets, the first with its time */
    spu_put(s, u, 3, 1, 9000);
    spu_put(s, u + 3, 100, 0, 0);
    spu_put(s, u + 103, n - 103, 0, 0);
    CHECK(!spu_at(s, 8999, 0), "shown before its time");
    q = spu_at(s, 9000, 0);
    CHECK(q && q->w == W && q->pix[3 * W] == 2, "not decoded from packets");
    CHECK(!spu_at(s, 9000, 1), "shown as forced");
    CHECK(!spu_at(s, 9000 + 100 * 1024, 0), "shown after its stop");
    /* a forced one, no stop: shown with subtitles off, till the next */
    n = make_unit(u, 1, 0);
    spu_put(s, u, n, 1, 200000);
    q = spu_at(s, 300000, 1);
    CHECK(q && q->forced && !q->has_stop, "forced");
    /* blended: 4:2:0 planes 320 x 240, grey; the clut's 9 = Y 200 Cr 100 Cb 50 */
    {
        static UBYTE Y[320 * 240], Cb[160 * 120], Cr[160 * 120];
        ULONG clut[16];
        struct SpuHl hl;
        memset(Y, 128, sizeof(Y)); memset(Cb, 128, sizeof(Cb)); memset(Cr, 128, sizeof(Cr));
        for (i = 0; i < 16; i++) clut[i] = (i * 10UL) << 16 | 0x8080;
        clut[9] = 200UL << 16 | 100 << 8 | 50;
        clut[12] = 30UL << 16 | 0x8080;
        spu_blend420(q, NULL, clut, Y, Cb, Cr, 320, 160, 320, 240);
        /* (x 100+24, row 200: value (3 + 0) & 3 = 3: solid 9) */
        CHECK(Y[200 * 320 + 124] == 200 && Cr[100 * 160 + 62] == 100 && Cb[100 * 160 + 62] == 50,
              "solid: %d %d %d", Y[200 * 320 + 124], Cr[100 * 160 + 62], Cb[100 * 160 + 62]);
        CHECK(Y[200 * 320 + 100] == 128, "clear changed: %d", Y[200 * 320 + 100]);
        /* (value 2, opacity 8 of 15, colour 8 = Y 80: 128 + (80-128)*8/15) */
        CHECK(Y[201 * 320 + 100 + 8] >= 102 && Y[201 * 320 + 100 + 8] <= 103, "half: %d", Y[201 * 320 + 108]);
        /* a highlight over its first 16 columns: value 3 -> colour 12, solid */
        memset(Y, 128, sizeof(Y));
        memset(&hl, 0, sizeof(hl));
        hl.on = 1; hl.x0 = 100; hl.x1 = 115; hl.y0 = 200; hl.y1 = 205;
        hl.col[3] = 12; hl.alpha[3] = 15; hl.col[2] = 12; hl.alpha[2] = 15;
        spu_blend420(q, &hl, clut, Y, Cb, Cr, 320, 160, 320, 240);
        CHECK(Y[203 * 320 + 110] == 30 && Y[203 * 320 + 120] != 30, "highlight: %d %d",
              Y[203 * 320 + 110], Y[203 * 320 + 120]);
        /* only the rows' spans of values other than 0 blended (value 0
         * clear): the same as all of it */
        {
            static UBYTE Y2[320 * 240], Cb2[160 * 120], Cr2[160 * 120];
            struct SpuPic dense = *q;
            ULONG x0, y0, x1, y1;
            dense.span = NULL;
            memset(Y, 77, sizeof(Y)); memset(Cb, 66, sizeof(Cb)); memset(Cr, 55, sizeof(Cr));
            memcpy(Y2, Y, sizeof(Y)); memcpy(Cb2, Cb, sizeof(Cb)); memcpy(Cr2, Cr, sizeof(Cr));
            spu_blend420(q, &hl, clut, Y, Cb, Cr, 320, 160, 320, 240);
            spu_blend420(&dense, &hl, clut, Y2, Cb2, Cr2, 320, 160, 320, 240);
            CHECK(!memcmp(Y, Y2, sizeof(Y)) && !memcmp(Cb, Cb2, sizeof(Cb)) && !memcmp(Cr, Cr2, sizeof(Cr)),
                  "spans blended unlike the whole");
            CHECK(q->nx1 > q->nx0 && q->ny1 > q->ny0, "no box of its values");
            CHECK(spu_extent(q, &hl, &x0, &y0, &x1, &y1) && x0 == (ULONG)q->x + q->nx0 &&
                  y1 == (ULONG)q->y + q->ny1, "extent %lu %lu %lu %lu", (unsigned long)x0, (unsigned long)y0,
                  (unsigned long)x1, (unsigned long)y1);
            for (y = 0; y < 240; y++)
                for (x = 0; x < 320; x++)
                    if ((x < (int)x0 || x >= (int)x1 || y < (int)y0 || y >= (int)y1) && Y[y * 320 + x] != 77)
                        wrong++;
            CHECK(!wrong, "%d pixels changed outside its extent", wrong);
            hl.alpha[0] = 5;                /* (the highlight's value 0 shows: its rectangle too) */
            CHECK(spu_extent(q, &hl, &x0, &y0, &x1, &y1) && x0 <= 100 && y0 <= 200, "extent with hl 0");
        }
    }
    /* a unit that is not one */
    memset(u, 0xEE, 64); u[0] = 0; u[1] = 64; u[2] = 0; u[3] = 8;
    CHECK(!spu_decode(u, 64, 0, &p), "nonsense decoded");
    {
        ULONG d, b;
        spu_counts(s, &d, &b);
        CHECK(d == 2 && b == 0, "counts %lu %lu", (unsigned long)d, (unsigned long)b);
    }
    spu_free(s);
    printf("%s\n", bad ? "spu tests FAILED" : "spu tests passed");
    return bad != 0;
}
