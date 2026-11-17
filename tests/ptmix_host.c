/*
 * Ambiance - ptmix.c on the build machine: an 8-channel module played by
 * ptcore.c through the mixer, checked sample for sample against Paula
 * modelled plainly at the mixer's rate.
 *
 *   ptmix_host <module> [out.wav]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ptcore.h"
#include "../src/ptmix.h"

#define ECLOCK 709379UL
#define RING   8192

/* the registers as the replayer writes them (modplay.c keeps the same) */
static struct { const pt_s8 *ptr; pt_u16 len, per; pt_u8 vol; } reg[PT_CHANS];
static pt_u8 offmask;

void pt_hw_period(int ch, pt_u16 p) { reg[ch].per = p; }
void pt_hw_volume(int ch, pt_u8 v) { reg[ch].vol = v; }
void pt_hw_data(int ch, const pt_s8 *p) { reg[ch].ptr = p; }
void pt_hw_length(int ch, pt_u16 w) { reg[ch].len = w; }
void pt_hw_dma_off(pt_u16 mask) { offmask |= (pt_u8)mask; }

/* the plain model: one Paula per channel, a sample at a time, its position
 * in the block in 16.16 with ptmix.c's step (the step's rounding is not
 * what is tested: the windows, block ends and loops are) */
static struct { int on; const pt_s8 *ptr, *rp; unsigned len, rl; unsigned long long pos; unsigned per, vol; } P[PT_CHANS];

int main(int argc, char **argv)
{
    FILE *f, *w = NULL;
    unsigned char *buf;
    long n, total = 0, diff = 0, big = 0, i, loud = 0;
    struct PtLayout L;
    struct Pt *pt;
    struct PtMix *m;
    struct PmTick t;
    int chans, rc, c, heard[PT_CHANS] = { 0 };
    pt_s8 *smp, silence[4] = { 0 }, *vt, *ring[4];
    unsigned char *pat;
    pt_u16 period = 124;                /* 28.6 kHz */

    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = calloc(1, n + 2048);
    if (fread(buf, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    rc = pt_layout(buf, 1084, n, &L, &chans);
    if (rc != PT_OK || chans <= 4) { printf("not an 8-channel module (%d, %d)\n", rc, chans); return 1; }
    pat = calloc(1, L.pat_len); memcpy(pat, buf + L.hdr_len, L.pat_len);
    smp = calloc(1, L.smp_len + 4); memcpy(smp, buf + L.hdr_len + L.pat_len, L.smp_len);
    pt = calloc(1, sizeof(*pt)); m = calloc(1, sizeof(*m));
    pt_init(pt, &L, buf, pat, smp, silence);
    vt = malloc(65 * 256); pm_voltab(vt);
    for (c = 0; c < 4; c++) ring[c] = calloc(1, RING);
    pm_init(m, pt->nch, period, vt, ring, RING);
    if (argc > 2) w = fopen(argv[2], "wb");
    while (!pt->ended) {
        pt_u32 cnt = (ECLOCK * 5) / ((pt_u32)pt->bpm * 2), ns, s0;
        memset(&t, 0, sizeof(t));
        offmask = 0;
        pt_tick(pt);
        t.off = offmask;
        t.on = (pt_u8)pt->dmacontemp;
        for (c = 0; c < pt->nch; c++) {
            t.start[c] = reg[c].ptr;
            t.slen[c] = reg[c].len;
            if (pt->loopmask & (1 << c)) {
                reg[c].ptr = pt->loop_ptr[c];
                reg[c].len = pt->loop_len[c];
            }
            t.ptr[c] = reg[c].ptr; t.len[c] = reg[c].len;
            t.per[c] = reg[c].per; t.vol[c] = reg[c].vol;
            /* the plain model, the same changes */
            if (t.off & (1 << c)) P[c].on = 0;
            if (t.on & (1 << c)) { P[c].on = 1; P[c].ptr = t.start[c]; P[c].len = t.slen[c]; P[c].pos = 0; }
            P[c].rp = t.ptr[c]; P[c].rl = t.len[c]; P[c].per = t.per[c] < 113 ? 113 : t.per[c];
            P[c].vol = t.vol[c] > 64 ? 64 : t.vol[c];
        }
        ns = pm_tick_samples(m, cnt);
        pm_apply(m, &t);
        s0 = m->wpos;
        pm_mix(m, ns);
        for (i = 0; i < (long)ns; i++) {
            int p, want[4] = { 0 };
            for (c = 0; c < pt->nch; c++) {
                int s = 0;
                if (P[c].on && P[c].per) {
                    unsigned long step = ((3546895UL / P[c].per) << 16) / m->rate;
                    s = ((int)P[c].ptr[P[c].pos >> 16] * (int)P[c].vol) >> 7;
                    P[c].pos += step;
                    while ((P[c].pos >> 16) >= P[c].len * 2ULL) {
                        P[c].pos -= (unsigned long long)P[c].len * 2 << 16;
                        P[c].ptr = P[c].rp; P[c].len = P[c].rl;
                    }
                    if (s) heard[c] = 1;
                }
                want[c & 3] += s;
            }
            for (p = 0; p < 4; p++) {
                int got = ring[p][(s0 + i) % RING], d = got - want[p];
                if (d) diff++;
                if (d > 2 || d < -2) big++;
                if (got) loud++;
            }
            if (w) {
                short l = (short)((ring[0][(s0 + i) % RING] + ring[3][(s0 + i) % RING]) * 128);
                short r = (short)((ring[1][(s0 + i) % RING] + ring[2][(s0 + i) % RING]) * 128);
                fwrite(&l, 2, 1, w); fwrite(&r, 2, 1, w);
            }
            total++;
        }
    }
    if (w) fclose(w);
    printf("%s: %d channels mixed at %lu Hz, %ld samples a ring; differing from the plain model: "
           "%ld (%ld by more than 2); with sound %ld; channels heard ",
           argv[1], pt->nch, (unsigned long)m->rate, total, diff, big, loud);
    for (c = 0; c < pt->nch; c++) printf("%d", heard[c]);
    printf("\n");
    return diff ? 1 : 0;
}
