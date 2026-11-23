/*
 * work_mpga.c - m68kprof's ambmpega work: the stream (the input file) read
 * through a hook from memory, box_w frames decoded by MPGA_decode_to (as
 * Ambiance's stream.c does) into 16-bit rings large enough to keep them
 * all, so a change can be checked sample for sample; box_h's bits: 1
 * medium quality, 2 half rate, 4 mono. mpga.c built as for the host (no
 * exec: calloc, the hook called directly), libmad and the assembler as
 * the library has them. Copyright (c) 2026 Brian King, MIT
 */
#include <exec/types.h>
#include <utility/hooks.h>
#include <libraries/mpega.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "work.h"

MPEGA_STREAM *MPEGA_open(char *, MPEGA_CTRL *);
void MPEGA_close(MPEGA_STREAM *);
LONG MPGA_decode_to(MPEGA_STREAM *, struct MpgaOut *);

static const uint8_t *data;
static int32_t data_len, data_pos;

static ULONG bs_func(struct Hook *h, APTR handle, MPEGA_ACCESS *a)
{
    (void)h;
    (void)handle;
    switch (a->func) {
    case MPEGA_BSFUNC_OPEN:
        data_pos = 0;
        a->data.open.stream_size = data_len;
        return 1;
    case MPEGA_BSFUNC_CLOSE:
        return 0;
    case MPEGA_BSFUNC_READ: {
        int32_t n = a->data.read.num_bytes;
        if (n > data_len - data_pos)
            n = data_len - data_pos;
        if (n > 0) {
            memcpy(a->data.read.buffer, data + data_pos, (size_t)n);
            data_pos += n;
        }
        return n < 0 ? 0 : (ULONG)n;
    }
    case MPEGA_BSFUNC_SEEK:
        if (a->data.seek.abs_byte_seek_pos < 0 || a->data.seek.abs_byte_seek_pos > data_len)
            return 1;
        data_pos = a->data.seek.abs_byte_seek_pos;
        return 0;
    }
    return 0;
}

/* mpga.c's host path names stdio's file calls (for MPEGA_open by name,
 * which this never does) */
FILE *fopen(const char *n, const char *m) { (void)n; (void)m; return NULL; }
int fclose(FILE *f) { (void)f; return 0; }
size_t fread(void *p, size_t s, size_t n, FILE *f) { (void)p; (void)s; (void)n; (void)f; return 0; }
int fseek(FILE *f, long o, int w) { (void)f; (void)o; (void)w; return -1; }
long ftell(FILE *f) { (void)f; return -1; }
void *malloc(size_t n) { return work_alloc((uint32_t)n); }

int work_run(struct Work *w)
{
    static struct Hook hook;
    MPEGA_CTRL c;
    MPEGA_STREAM *s;
    struct MpgaOut o;
    uint32_t ring = 1, nf = 0, total = 0;
    WORD q = (w->box_h & 1) ? MPEGA_QUALITY_MEDIUM : MPEGA_QUALITY_HIGH;
    WORD div = (w->box_h & 2) ? 2 : 1, mono = (w->box_h & 4) != 0;
    int16_t *l, *r;
    while (ring < w->box_w * 1152)
        ring <<= 1;
    l = work_alloc(ring * 2);
    r = work_alloc(ring * 2);
    if (!l || !r)
        return -2;
    data = w->in;
    data_len = (int32_t)w->in_len;
    hook.h_Entry = (ULONG (*)())bs_func;
    memset(&c, 0, sizeof(c));
    c.bs_access = &hook;
    c.layer_1_2.mono.freq_div = c.layer_1_2.stereo.freq_div = div;
    c.layer_3.mono.freq_div = c.layer_3.stereo.freq_div = div;
    c.layer_1_2.mono.quality = c.layer_1_2.stereo.quality = q;
    c.layer_3.mono.quality = c.layer_3.stereo.quality = q;
    c.layer_1_2.force_mono = c.layer_3.force_mono = mono;
    c.check_mpeg = 1;
    work_mark(WORK_OPEN);
    if (!(s = MPEGA_open(NULL, &c)))
        return -1;
    memset(&o, 0, sizeof(o));
    o.mo_Format = MPGA_FMT_S16;
    o.mo_Buffer[0] = l;
    o.mo_Buffer[1] = r;
    o.mo_Mask = ring - 1;
    work_mark(WORK_DECODE);
    while (nf < w->box_w) {
        LONG n = MPGA_decode_to(s, &o);
        if (n <= 0)
            break;
        total += (uint32_t)n;
        nf++;
    }
    work_mark(WORK_DONE);
    MPEGA_close(s);
    /* the samples, left then right: their bytes */
    memcpy(l + total, r, total * 2);
    w->out = (uint8_t *)l;
    w->out_len = total * 4;
    w->dw = total;
    w->scale = (int32_t)nf;
    return 0;
}

/* what libmad (built for the library) calls for memory, and timer.c's
 * stubbed sprintf: the library's own are in mpga.c's AmigaOS part */
void *mpga_malloc(size_t n) { return work_alloc((uint32_t)n); }
void *calloc(size_t n, size_t m);
void *mpga_calloc(size_t n, size_t m) { return calloc(n, m); }
void mpga_free(void *p) { work_free(p); }
int mpga_nosprintf(char *s, const char *f, ...) { (void)f; *s = 0; return 0; }
