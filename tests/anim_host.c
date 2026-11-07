/*
 * AmigaMCE tests: IFF ANIM decoding (src/animdec.c) on the host.
 *
 *   anim_host file.anim [out-prefix [frame...]]
 *
 * Plays the file through two buffers as the player does (a delta changes
 * the frame two back, or with interleave 1 the last), counts the frames
 * and their time, and writes the frames asked for as PPM pictures in the
 * file's colours (extra halfbrite and HAM too).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/animdec.h"

typedef unsigned char u8;
static unsigned long be32(const u8 *p) { return ((unsigned long)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static unsigned be16(const u8 *p) { return (p[0] << 8) | p[1]; }

static int w, h, depth, rb, masking, comp;
static unsigned long camg;
static unsigned long pal[256];
static u8 *buf[2];
static struct AnimPlanes pl[2];

static void planes_for(int k)
{
    int i;
    pl[k].stride = (long)rb * 1;            /* separate planes, as a non-interleaved bitmap */
    pl[k].depth = depth;
    pl[k].rb = rb;
    pl[k].h = h;
    for (i = 0; i < 8; i++)
        pl[k].plane[i] = buf[k] + (long)i * rb * h;
}

static void cmap(const u8 *d, unsigned long n)
{
    unsigned long i, lo = 0;
    if (n / 3 > 256)
        n = 768;
    for (i = 0; i < n; i++)
        lo |= d[i] & 15;
    for (i = 0; i < n / 3; i++) {
        unsigned long r = d[i * 3], g = d[i * 3 + 1], b = d[i * 3 + 2];
        if (!lo) {
            r |= r >> 4;
            g |= g >> 4;
            b |= b >> 4;
        }
        pal[i] = (r << 16) | (g << 8) | b;
    }
}

static void write_ppm(const char *name, int k)
{
    FILE *f = fopen(name, "wb");
    int x, y, p;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        unsigned long rgb = pal[0];
        for (x = 0; x < w; x++) {
            unsigned c = 0;
            u8 o[3];
            for (p = 0; p < depth; p++)
                if (pl[k].plane[p][y * pl[k].stride + (x >> 3)] & (0x80 >> (x & 7)))
                    c |= 1 << p;
            if (camg & 0x800) {                 /* HAM: 6 planes HAM6, 8 HAM8 */
                int bits = depth == 8 ? 6 : 4, ctl = c >> bits;
                unsigned v = c & ((1 << bits) - 1);
                v = bits == 4 ? v * 17 : (v << 2) | (v >> 4);
                if (ctl == 0)
                    rgb = pal[c & ((1 << bits) - 1)];
                else if (ctl == 1)
                    rgb = (rgb & 0xFFFF00) | v;
                else if (ctl == 2)
                    rgb = (rgb & 0x00FFFF) | ((unsigned long)v << 16);
                else
                    rgb = (rgb & 0xFF00FF) | ((unsigned long)v << 8);
            } else if ((camg & 0x80) && depth == 6 && c >= 32)
                rgb = (pal[c - 32] >> 1) & 0x7F7F7F;
            else
                rgb = pal[c];
            o[0] = rgb >> 16;
            o[1] = rgb >> 8;
            o[2] = rgb;
            fwrite(o, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    long len, pos, n = 0;
    u8 *file;
    int bad = 0, frame = 0, unknown = 0;
    unsigned long jiffies = 0;
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc(len);
    if (fread(file, 1, len, f) != (size_t)len)
        return 1;
    fclose(f);
    if (memcmp(file, "FORM", 4) || memcmp(file + 8, "ANIM", 4))
        return printf("not an ANIM\n"), 1;
    for (pos = 12; pos + 12 <= len;) {
        unsigned long fl = be32(file + pos + 4), cp;
        long end = pos + 8 + fl;
        int op = -1, il = 0;
        unsigned long bits = 0, rel = 0;
        const u8 *dlta = NULL, *body = NULL;
        unsigned long dl = 0, bl = 0;
        if (end > len)
            end = len;                          /* cut short: as far as it goes */
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
                cmap(d, cl);
            else if (!memcmp(c, "ANHD", 4)) {
                op = d[0];
                rel = be32(d + 14);
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
            rb = ((w + 15) >> 4) << 1;
            buf[0] = calloc(8, (size_t)rb * h);
            buf[1] = calloc(8, (size_t)rb * h);
            planes_for(0);
            planes_for(1);
            if (!body || animdec_body(body, bl, &pl[0], comp, masking) < 0)
                bad++;
            memcpy(buf[1], buf[0], (size_t)8 * rb * h);
        } else {
            int k = frame & 1, r = 0;
            if (il == 1)                        /* from the last frame, not two back */
                memcpy(buf[k], buf[!k], (size_t)8 * rb * h);
            if (body && op == 0)
                r = animdec_body(body, bl, &pl[k], comp, masking);
            else if (dlta)
                r = animdec_delta(dlta, dl, &pl[k], op, bits);
            if (r == -2)
                unknown++;
            else if (r < 0)
                bad++;
        }
        jiffies += rel;
        if (argc > 3) {
            int i;
            for (i = 3; i < argc; i++)
                if (atoi(argv[i]) == frame) {
                    char name[300];
                    sprintf(name, "%s-%04d.ppm", argv[2], frame);
                    write_ppm(name, frame & 1);
                }
        }
        frame++;
        n++;
        pos = end + (fl & 1);
    }
    printf("%dx%dx%d camg %05lx: %d frames, %lu jiffies, %d bad, %d unknown op\n", w, h, depth,
           camg, frame, jiffies, bad, unknown);
    return bad || unknown;
}
