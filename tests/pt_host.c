/*
 * AmigaMCE - ptcore.c on the build machine: a module played through a
 * model of Paula into a WAV file, and checked.
 *
 *   cc -O2 -o pt_host tests/pt_host.c src/ptcore.c
 *   pt_host <module> [out.wav] [seconds]
 *
 * Prints the module's layout, its length (pt_measure), and per-second
 * loudness of what was played, so a silent or runaway replayer shows.
 * Paula is modelled as the hardware behaves: a channel takes its start
 * and length when its DMA is switched on, and again each time it reaches
 * the end of what it was playing (the loop).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ptcore.h"

#define RATE  44100
#define CLOCK 3546895.0         /* PAL */

static struct {
    int on;
    const pt_s8 *reg_ptr, *ptr;
    unsigned reg_len, len;      /* words */
    double pos;                 /* in bytes, within the block playing */
    unsigned per, vol;
} paula[PT_CHANS];         /* 5-8: a Paula of their own here (mixed on the Amiga) */

void pt_hw_period(int ch, pt_u16 p) { paula[ch].per = p; }
void pt_hw_volume(int ch, pt_u8 v) { paula[ch].vol = v > 64 ? 64 : v; }
void pt_hw_data(int ch, const pt_s8 *p) { paula[ch].reg_ptr = p; }
void pt_hw_length(int ch, pt_u16 w) { paula[ch].reg_len = w; }
void pt_hw_dma_off(pt_u16 mask)
{
    int i;
    for (i = 0; i < PT_CHANS; i++)
        if (mask & (1 << i))
            paula[i].on = 0;
}

static void dma_on(pt_u16 mask)
{
    int i;
    for (i = 0; i < PT_CHANS; i++)
        if ((mask & (1 << i)) && !paula[i].on) {
            paula[i].on = 1;
            paula[i].ptr = paula[i].reg_ptr;
            paula[i].len = paula[i].reg_len ? paula[i].reg_len : 65536;
            paula[i].pos = 0;
        }
}

static void put32(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); }

int main(int argc, char **argv)
{
    FILE *f, *w = NULL;
    unsigned char *buf;
    long n;
    struct PtLayout L;
    struct Pt *pt;
    struct PtTimes *tm;
    int chans, rc, i, secs;
    pt_s8 *smp, silence[4] = { 0, 0, 0, 0 };
    unsigned char *pat;
    long frames = 0, total, sec_frames = 0;
    double acc = 0, sec_sum = 0, tick_left = 0;
    long loud_secs = 0, silent_secs = 0;
    int chan_heard[PT_CHANS] = { 0 };

    if (argc < 2) {
        fprintf(stderr, "usage: pt_host <module> [out.wav] [seconds]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = calloc(1, n + 2048);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); return 2; }
    fclose(f);

    rc = pt_layout(buf, n < 1084 ? n : 1084, n, &L, &chans);
    if (rc != PT_OK) {
        printf("%s: not played (error %d, %d channels)\n", argv[1], rc, chans);
        return 1;
    }
    /* patterns and samples copied out, as modplay.c loads them */
    pat = calloc(1, L.pat_len);
    memcpy(pat, buf + L.hdr_len, L.pat_len);
    smp = calloc(1, L.smp_len + 4);
    {
        long have = n - (long)(L.hdr_len + L.pat_len);
        if (have > (long)L.smp_len) have = L.smp_len;
        if (have > 0) memcpy(smp, buf + L.hdr_len + L.pat_len, have);
        if (have < (long)L.smp_len) printf("short by %ld sample bytes\n", (long)L.smp_len - have);
    }
    pt = calloc(1, sizeof(*pt));
    tm = calloc(1, sizeof(*tm));
    pt_init(pt, &L, buf, pat, smp, silence);
    pt_measure(pt, tm);
    printf("%s: \"%s\" %s, %d samples, %d positions, %d patterns, length %lu:%02lu.%03lu\n",
           argv[1], pt->title, L.tag, L.nsmp, L.songlen, L.npat,
           (unsigned long)(tm->total_ms / 60000), (unsigned long)(tm->total_ms / 1000 % 60),
           (unsigned long)(tm->total_ms % 1000));

    secs = argc > 3 ? atoi(argv[3]) : (int)(tm->total_ms / 1000 + 1);
    total = (long)secs * RATE;
    if (argc > 2 && strcmp(argv[2], "-")) {
        w = fopen(argv[2], "wb");
        fwrite("RIFF", 1, 4, w); put32(w, 36 + total * 4); fwrite("WAVEfmt ", 1, 8, w);
        put32(w, 16); put16(w, 1); put16(w, 2); put32(w, RATE); put32(w, RATE * 4);
        put16(w, 4); put16(w, 16); fwrite("data", 1, 4, w); put32(w, total * 4);
    }
    while (frames < total) {
        if (tick_left <= 0) {
            pt_tick(pt);
            dma_on(pt->dmacontemp);
            for (i = 0; i < pt->nch; i++)
                if (pt->loopmask & (1 << i)) {
                    paula[i].reg_ptr = pt->loop_ptr[i];
                    paula[i].reg_len = pt->loop_len[i];
                }
            tick_left += pt->tick_us * (RATE / 1e6);
        }
        {
            int l = 0, r = 0;
            for (i = 0; i < pt->nch; i++) {
                int s = 0;
                if (paula[i].on && paula[i].per) {
                    s = paula[i].ptr[(long)paula[i].pos] * (int)paula[i].vol;
                    paula[i].pos += CLOCK / paula[i].per / RATE;
                    while (paula[i].pos >= paula[i].len * 2.0) {
                        paula[i].pos -= paula[i].len * 2.0;
                        paula[i].ptr = paula[i].reg_ptr;
                        paula[i].len = paula[i].reg_len ? paula[i].reg_len : 65536;
                    }
                    if (s)
                        chan_heard[i] = 1;
                }
                if ((i & 3) == 0 || (i & 3) == 3) l += s; else r += s;  /* Paula: 0,3 left */
            }
            if (w) { put16(w, (unsigned)(short)(l * 3)); put16(w, (unsigned)(short)(r * 3)); }
            acc = (double)l * l + (double)r * r;
            sec_sum += acc;
        }
        tick_left -= 1;
        frames++;
        if (++sec_frames == RATE) {
            double rms = sec_sum / RATE;
            if (rms > 1e4) loud_secs++; else silent_secs++;
            sec_frames = 0;
            sec_sum = 0;
        }
    }
    if (w) fclose(w);
    printf("  played %d s: %ld with sound, %ld near silent; channels heard ", secs, loud_secs,
           silent_secs);
    for (i = 0; i < pt->nch; i++)
        printf("%d", chan_heard[i]);
    printf("; ended %d at position %d row %d\n", pt->ended, pt->order, pt->row);
    return 0;
}
