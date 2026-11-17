/*
 * Ambiance tests: how long an ANIM's frames take, decoded (animdec.c) and
 * drawn as 16-bit pixels (animdraw.c), on a 68k (run under vamos).
 *
 *   anim_bench file.anim mode scale%
 *
 * mode 0: decode only; 1: decode, and each frame drawn whole (HAM by its
 * steps) at scale% (100: as it is, 200: doubled), as the player does with
 * no rows to spare; 2: as 1, HAM or not by the colour table alone; 3: the
 * planes made chunky alone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/animdec.h"
#include "../src/animdraw.h"

typedef unsigned char u8;
static unsigned long be32(const u8 *p) { return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }
static unsigned be16(const u8 *p) { return (p[0] << 8) | p[1]; }

int main(int argc, char **argv)
{
    FILE *f;
    long len, pos;
    u8 *file, *buf[2], *out;
    int w = 0, h = 0, depth = 0, masking = 0, comp = 0, frame = 0, mode, scale, ham = 0, k, i, y;
    unsigned long camg = 0, rb = 0, fsz = 0, lut[256], bpr = 0, rows = 0;
    unsigned short l16[256], *xo = NULL, *yo = NULL;
    struct AnimPlanes pl[2];
    if (argc < 4 || !(f = fopen(argv[1], "rb")))
        return 20;
    mode = atoi(argv[2]);
    scale = atoi(argv[3]);
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (!(file = malloc(len)) || fread(file, 1, len, f) != (size_t)len)
        return 20;
    fclose(f);
    ad_init();
    ad_pix_tables(7);                       /* PIXFMT_RGB16PC, as PrismRTG's */
    memset(lut, 0, sizeof(lut));
    for (pos = 12; pos + 12 <= len;) {
        unsigned long fl = be32(file + pos + 4), cp;
        long end = pos + 8 + fl;
        int op = -1, il = 0;
        unsigned long bits = 0;
        const u8 *dlta = NULL, *body = NULL;
        unsigned long dl = 0, bl = 0;
        if (end > len)
            end = len;
        if (memcmp(file + pos, "FORM", 4) || memcmp(file + pos + 8, "ILBM", 4)) {
            pos = end + (fl & 1);
            continue;
        }
        for (cp = pos + 12; cp + 8 <= (unsigned long)end;) {
            const u8 *c = file + cp, *d = c + 8;
            unsigned long cl = be32(c + 4);
            if (cp + 8 + cl > (unsigned long)end)
                cl = end - cp - 8;
            if (!memcmp(c, "BMHD", 4) && frame == 0) {
                w = be16(d);
                h = be16(d + 2);
                depth = d[8];
                masking = d[9];
                comp = d[10];
            } else if (!memcmp(c, "CAMG", 4))
                camg = be32(d);
            else if (!memcmp(c, "CMAP", 4))
                ad_lut(d, cl, camg, depth, lut);
            else if (!memcmp(c, "ANHD", 4)) {
                op = d[0];
                il = d[18];
                bits = be32(d + 20);
            } else if (!memcmp(c, "DLTA", 4)) {
                dlta = d;
                dl = cl;
            } else if (!memcmp(c, "BODY", 4)) {
                body = d;
                bl = cl;
            }
            cp += 8 + cl + (cl & 1);
        }
        if (frame == 0) {
            int dw = w * scale / 100, dh = h * scale / 100;
            rb = ((w + 15) >> 4) << 1;
            fsz = rb * depth * h;
            buf[0] = calloc(1, fsz);
            buf[1] = calloc(1, fsz);
            for (k = 0; k < 2; k++) {
                for (i = 0; i < 8; i++)
                    pl[k].plane[i] = buf[k] + i * rb;
                pl[k].stride = rb * depth;
                pl[k].depth = depth;
                pl[k].rb = rb;
                pl[k].h = h;
            }
            ham = (camg & 0x800) && (depth == 6 || depth == 8);
            xo = malloc((w + 1) * 2);
            yo = malloc((h + 1) * 2);
            for (i = 0; i <= w; i++)
                xo[i] = i * dw / w;
            for (i = 0; i <= h; i++)
                yo[i] = i * dh / h;
            bpr = dw * 2;
            out = malloc(bpr * (dh + 1));
            animdec_body(body, bl, &pl[0], comp, masking);
            memcpy(buf[1], buf[0], fsz);
            k = 0;
        } else {
            k = frame & 1;
            if (il == 1)
                memcpy(buf[k], buf[!k], fsz);
            if (body && op == 0)
                animdec_body(body, bl, &pl[k], comp, masking);
            else if (dlta)
                animdec_delta(dlta, dl, &pl[k], op, bits);
        }
        if (mode == 3)                      /* the planes made chunky alone */
            for (y = 0; y < h; y++)
                ad_p2c_row(buf[k] + y * rb * depth, rb, depth, w, out);
        if (mode == 1 || mode == 2) {
            if (mode == 2)
                ham = 0;                    /* its colours by the table alone */
            int fx = (w * scale / 100) % w ? 0 : w * scale / 100 / w;
            ad_lut16(lut, l16);
            if (ham)
                ad_ham(l16, depth);
            for (y = 0; y < h; y++) {
                u8 *o = out + (unsigned long)yo[y] * bpr;
                ad_row_put(buf[k] + y * rb * depth, rb, depth, w, 0, rb, fx, xo, ham, lut, l16, o);
                for (i = 1; i < yo[y + 1] - yo[y]; i++)
                    memcpy(o + i * bpr, o, bpr);
                rows++;
            }
        }
        frame++;
        pos = end + (fl & 1);
    }
    printf("%dx%dx%d camg %05lx: %d frames, mode %d, scale %d%%, %lu rows\n", w, h, depth, camg,
           frame, mode, scale, rows);
    return 0;
}
