/*
 * Ambiance - eight channels on Paula's four: channels 5-8 mixed in
 * Copyright (c) 2026 Brian King
 *
 * Adapted from ProTracker 4.1's 8-channel mixer (src/engine/mixer.asm),
 * copyright (c) 2026 Akustikrausch (Andreas Wendorf), used under the MIT
 * licence by his leave; ProTracker 4.1: https://github.com/akustikrausch/
 * protracker4. Its design is kept: channel n and channel n + 4 share
 * Paula's channel n, both mixed into a ring of 8-bit samples that Paula
 * plays round and round at a fixed rate (22 or 28.6 kHz), each at half
 * scale through a volume table, a tick's worth at a time, a few ticks
 * ahead of Paula, in a software interrupt after the replayer's tick.
 *
 * Here the channels are Ambiance's own replayer's (ptcore.c), so what is
 * mixed is eight Paulas in software: each takes its start and length when
 * its DMA is switched on and again each time it reaches the end of what it
 * was playing, as the hardware does, so the replayer drives them exactly
 * as it drives Paula (loops, sample offsets, retriggers and all).
 *
 * Plain C with no AmigaOS headers (tested on the build machine by
 * tests/ptmix_host.c); modplay.c is the Amiga around it.
 */
#include "ptmix.h"

#define PAULA_CLOCK 3546895UL       /* PAL; NTSC's is in the same ratio to its E-clock */

static const pt_s8 nothing[1] = { 0 };

void pm_voltab(pt_s8 *vt)
{
    int v, s;
    for (v = 0; v <= 64; v++)
        for (s = 0; s < 256; s++)
            vt[v * 256 + s] = (pt_s8)(((int)(pt_s8)s * v) >> 7);    /* half scale: two fit */
}

void pm_init(struct PtMix *m, int nch, pt_u16 period, pt_s8 *vt, pt_s8 *const ring[4],
             pt_u32 ring_len)
{
    int i;
    pt_u8 *z = (pt_u8 *)m;
    for (i = 0; i < (int)sizeof(*m); i++)
        z[i] = 0;
    m->nch = nch;
    m->period = period;
    m->rate = PAULA_CLOCK / period;
    m->vt = vt;
    for (i = 0; i < 4; i++)
        m->ring[i] = ring[i];
    m->ring_len = ring_len;
}

pt_u32 pm_tick_samples(struct PtMix *m, pt_u32 cia_count)
{
    /* Paula's clock is five times the E-clock (PAL and NTSC), so a tick of
     * cia_count E-clocks is 5 * cia_count / period samples: kept exactly,
     * the fraction carried, or the ring would drift from the replayer */
    pt_u32 num = 5 * cia_count + m->spt_rest;
    pt_u32 n = num / m->period;
    m->spt_rest = num - n * m->period;
    return n;
}

static void set_step(struct PtVoice *v, pt_u32 rate)
{
    pt_u16 p = v->per < 113 ? 113 : v->per;
    if (p == v->step_per)
        return;
    v->step_per = p;
    v->step = ((PAULA_CLOCK / p) << 16) / rate;
}

void pm_apply(struct PtMix *m, const struct PmTick *t)
{
    int i;
    for (i = 0; i < m->nch; i++) {
        struct PtVoice *v = &m->v[i];
        if (t->off & (1 << i))
            v->on = 0;
        if (t->on & (1 << i)) {
            pt_u32 bytes = t->slen[i] ? (pt_u32)t->slen[i] * 2 : 131072UL;
            v->pos = t->start[i] ? t->start[i] : nothing;
            v->end = v->pos + bytes;
            v->frac = 0;
            v->on = t->start[i] != 0;
        }
        v->reg_ptr = t->ptr[i];
        v->reg_len = t->len[i];
        v->per = t->per[i];
        v->vol = t->vol[i] > 64 ? 64 : t->vol[i];
        set_step(v, m->rate);
    }
}

/* Output samples voice v gives before its window or its block ends (huge
 * if silent): a window keeps its position, offset << 16 | fraction, in 32
 * bits. */
#define WINDOW 32768UL
static pt_u32 avail(const struct PtVoice *v)
{
    pt_u32 bytes, s;
    if (!v->on || !v->step)
        return 0x7FFFFFFFUL;
    bytes = (pt_u32)(v->end - v->pos);
    if (!bytes)
        return 1;                       /* at the end: wrap() moves it on */
    if (bytes > WINDOW)
        bytes = WINDOW;
    s = ((bytes << 16) - v->frac + v->step - 1) / v->step;
    return s ? s : 1;
}

/* Past its block's end: on with what the registers say (Paula's loop). */
static void wrap(struct PtVoice *v)
{
    pt_u32 len, over;
    if (!v->on || v->pos < v->end)
        return;
    if (!v->reg_ptr) {
        v->on = 0;
        return;
    }
    len = v->reg_len ? (pt_u32)v->reg_len * 2 : 131072UL;
    over = (pt_u32)(v->pos - v->end) % len;
    v->pos = v->reg_ptr + over;
    v->end = v->reg_ptr + len;
}

/* n samples of voices a and b (b may be NULL) mixed into out. */
static void mix_pair(struct PtMix *m, struct PtVoice *a, struct PtVoice *b, pt_s8 *out, pt_u32 n)
{
    while (n) {
        pt_u32 k = n, x;
        const pt_s8 *pa, *pb, *ta, *tb;
        pt_u32 fa, fb, sa, sb;
        if ((x = avail(a)) < k)
            k = x;
        if (b && (x = avail(b)) < k)
            k = x;
        if (a->on && a->step) {
            pa = a->pos, fa = a->frac, sa = a->step, ta = m->vt + a->vol * 256;
        } else {
            pa = nothing, fa = 0, sa = 0, ta = m->vt;
        }
        if (b && b->on && b->step) {
            pb = b->pos, fb = b->frac, sb = b->step, tb = m->vt + b->vol * 256;
        } else {
            pb = nothing, fb = 0, sb = 0, tb = m->vt;
        }
        n -= k;
        do {
            *out++ = (pt_s8)(ta[(pt_u8)pa[fa >> 16]] + tb[(pt_u8)pb[fb >> 16]]);
            fa += sa;
            fb += sb;
        } while (--k);
        if (sa) {
            a->pos += fa >> 16;
            a->frac = fa & 0xFFFF;
            wrap(a);
        }
        if (sb) {
            b->pos += fb >> 16;
            b->frac = fb & 0xFFFF;
            wrap(b);
        }
    }
}

void pm_mix(struct PtMix *m, pt_u32 n)
{
    while (n) {
        pt_u32 k = m->ring_len - m->wpos, p;
        if (k > n)
            k = n;
        for (p = 0; p < 4; p++)
            mix_pair(m, &m->v[p], (int)p + 4 < m->nch ? &m->v[p + 4] : 0, m->ring[p] + m->wpos, k);
        m->wpos += k;
        if (m->wpos >= m->ring_len)
            m->wpos = 0;
        n -= k;
    }
}

void pm_mix_silence(struct PtMix *m, pt_u32 n)
{
    while (n--) {
        int p;
        for (p = 0; p < 4; p++)
            m->ring[p][m->wpos] = 0;
        if (++m->wpos >= m->ring_len)
            m->wpos = 0;
    }
}

void pm_ring_start(struct PtMix *m, pt_u32 lead)
{
    pt_u32 i, p;
    for (p = 0; p < 4; p++)
        for (i = 0; i < m->ring_len; i++)
            m->ring[p][i] = 0;
    m->wpos = lead % m->ring_len;
}
