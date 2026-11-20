/*
 * Ambiance - an MPEG transport stream (.ts, .m2ts) packed as a Program
 * Stream as it is read, for the ZZ9000's media session (which takes
 * Program Streams and WebM): tspack.h
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Written from ISO 13818-1 (transport packets, PES packets, pack and system
 * headers) and the DVD's way with AC3 (private stream 1, a substream number
 * and a 4-byte header before its frames). Only the video's PID and the
 * sound's are kept; each of their PES packets (a transport stream's are
 * often a whole picture, length 0: unbounded) is cut into Program Stream
 * packets of at most TSP_CHUNK bytes, each after a pack header, the first
 * with the PES packet's time stamps (PTS, DTS), the rest with none. The
 * data is copied, not decoded: a few memory copies a byte on the 68k.
 * The pack headers' clock is the bytes so far at a nominal rate: the
 * card's demultiplexer goes by the time stamps.
 */
#include <exec/types.h>
#include <string.h>
#include "tspack.h"

#define TSP_CHUNK   2016        /* a Program Stream packet's payload at most */
#define TSP_IN      (32L * 1024)
#define TSP_RATE    25200UL     /* the packs' mux rate, 50-byte units (10 Mbit/s) */
#define TSP_PRIME   (8UL * 1024 * 1024)     /* passed over at most, waiting */
#define TSP_HOLD    (128L * 1024)
enum { P_SEQ, P_SOUND, P_ON };

struct Tsp {
    UBYTE size, lead;           /* packet size, the bytes before its 0x47 */
    UWORD pid[2];               /* 0 video, 1 sound */
    UBYTE sid[2];               /* their Program Stream ids */
    BOOL ac3, mpeg2;
    struct {
        BOOL on;                /* a PES packet begun (its start seen) */
        BOOL first;             /* the next chunk is its first: its time stamps */
        UBYTE ts[10];           /* its PTS (and DTS) */
        UBYTE flags;            /* PTS_DTS_flags (0x80, 0xC0, 0) */
        UBYTE align;            /* data_alignment_indicator */
        UWORD n;
        UBYTE buf[TSP_CHUNK];
    } s[2];
    UBYTE in[TSP_IN];
    LONG ipos, ilen;
    BOOL eof, ended, sys;
    int prime;                  /* its start: P_SEQ, P_SOUND, P_ON (see tsp_pack) */
    ULONG primed;               /* the bytes passed over meanwhile */
    UBYTE hold[TSP_HOLD];       /* the video's first packs, while its sound's first is awaited */
    ULONG hlen, hoff;
    ULONG bytes;                /* written so far (the packs' clock) */
    ULONG resyncs, cut;
};

static struct Tsp tsp_one;           /* (tsp_begin's, tsp_pack's: the playback process's) */

void tsp_begin_in(struct Tsp *T, UBYTE size, UWORD vpid, UWORD apid, BOOL ac3, BOOL mpeg2)
{
    memset(T, 0, sizeof(*T));
    T->size = size;
    T->lead = size == 192 ? 4 : 0;
    T->pid[0] = vpid;
    T->pid[1] = apid;
    T->sid[0] = 0xE0;
    T->sid[1] = ac3 ? 0xBD : 0xC0;
    T->ac3 = ac3;
    T->mpeg2 = mpeg2;
    T->prime = P_SEQ;
}

void tsp_counts_in(struct Tsp *T, ULONG *resyncs, ULONG *packets, ULONG *before_sound)
{
    *resyncs = T->resyncs;
    *packets = T->cut;
    *before_sound = T->primed;
}

/* A pack header (MPEG-2), and before the first packet the system header. */
static ULONG pack_header(struct Tsp *T, UBYTE *o)
{
    ULONG k = 0, scr = (ULONG)(((unsigned long long)T->bytes * 90000ULL) / (TSP_RATE * 50UL));
    o[k++] = 0; o[k++] = 0; o[k++] = 1; o[k++] = 0xBA;
    o[k++] = (UBYTE)(0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 0x03));
    o[k++] = (UBYTE)(scr >> 20);
    o[k++] = (UBYTE)((((scr >> 15) & 0x1F) << 3) | 0x04 | ((scr >> 13) & 0x03));
    o[k++] = (UBYTE)(scr >> 5);
    o[k++] = (UBYTE)(((scr & 0x1F) << 3) | 0x04);
    o[k++] = 0x01;                          /* (no SCR extension) */
    o[k++] = (UBYTE)(TSP_RATE >> 14);
    o[k++] = (UBYTE)(TSP_RATE >> 6);
    o[k++] = (UBYTE)(((TSP_RATE & 0x3F) << 2) | 0x03);
    o[k++] = 0xF8;                          /* (no stuffing) */
    if (!T->sys) {
        BOOL snd = T->pid[1] != 0x1FFF;
        T->sys = TRUE;
        o[k++] = 0; o[k++] = 0; o[k++] = 1; o[k++] = 0xBB;
        o[k++] = 0; o[k++] = (UBYTE)(snd ? 12 : 9);
        o[k++] = (UBYTE)(0x80 | (TSP_RATE >> 15));
        o[k++] = (UBYTE)(TSP_RATE >> 7);
        o[k++] = (UBYTE)(((TSP_RATE & 0x7F) << 1) | 1);
        o[k++] = (UBYTE)(snd ? 0x04 : 0x00);    /* audio bound, not fixed, not CSPS */
        o[k++] = 0xE1;                      /* locks, a marker, one video stream */
        o[k++] = 0xFF;
        o[k++] = 0xE0; o[k++] = 0xE0; o[k++] = 232;    /* video: 232 KB */
        if (snd) {
            o[k++] = T->sid[1];
            if (T->ac3) {
                o[k++] = 0xE0; o[k++] = 58;             /* AC3: 58 KB */
            } else {
                o[k++] = 0xC0; o[k++] = 32;             /* MPEG audio: 4 KB */
            }
        }
    }
    return k;
}

/* Stream i's chunk out as a Program Stream packet (after a pack header). */
static ULONG put_chunk(struct Tsp *T, int i, UBYTE *o)
{
    ULONG k = pack_header(T, o), len, hl, j;
    UWORD n = T->s[i].n;
    BOOL first = T->s[i].first;
    hl = first ? (T->s[i].flags == 0xC0 ? 10 : T->s[i].flags == 0x80 ? 5 : 0) : 0;
    len = 3 + hl + (T->ac3 && i ? 4 : 0) + n;
    o[k++] = 0; o[k++] = 0; o[k++] = 1; o[k++] = T->sid[i];
    o[k++] = (UBYTE)(len >> 8);
    o[k++] = (UBYTE)len;
    o[k++] = (UBYTE)(0x80 | (first ? T->s[i].align : 0));
    o[k++] = (UBYTE)(hl ? T->s[i].flags : 0);
    o[k++] = (UBYTE)hl;
    for (j = 0; j < hl; j++)
        o[k++] = T->s[i].ts[j];
    if (T->ac3 && i) {
        /* a DVD's AC3: substream 0x80, the frames that begin here, and
         * where the first does (from the byte after these four, plus 1) */
        ULONG f = 0, at = 0;
        const UBYTE *b = T->s[i].buf;
        for (j = 0; j + 1 < n; j++)
            if (b[j] == 0x0B && b[j + 1] == 0x77) {
                if (!f)
                    at = j + 1;
                f++;
            }
        o[k++] = 0x80;
        o[k++] = (UBYTE)(f > 255 ? 255 : f);
        o[k++] = (UBYTE)(at >> 8);
        o[k++] = (UBYTE)at;
    }
    memcpy(o + k, T->s[i].buf, n);
    k += n;
    T->s[i].n = 0;
    T->s[i].first = FALSE;
    T->bytes += k;
    return k;
}

/* The video's chunk into the hold (its sound's first awaited); full, the
 * sound given up: what is held goes out as it is. 0 (nothing out yet). */
static ULONG hold_chunk(struct Tsp *T)
{
    if (T->hlen + TSP_CHUNK + 80 > TSP_HOLD)
        T->prime = P_ON;                   /* (no sound soon enough) */
    T->hlen += put_chunk(T, 0, T->hold + T->hlen);
    return 0;
}

/* A PES packet's start in a transport packet's payload: its header kept,
 * the bytes after it (its data) at *data. FALSE if it is not one. */
static BOOL pes_start(struct Tsp *T, int i, const UBYTE *p, LONG n, LONG *data)
{
    LONG hl;
    if (n < 9 || p[0] || p[1] || p[2] != 1)
        return FALSE;
    if ((p[6] & 0xC0) != 0x80)
        return FALSE;                       /* (not an MPEG-2 PES header) */
    hl = p[8];
    if (9 + hl > n)
        return FALSE;                       /* (its header cut: not here) */
    T->s[i].flags = (UBYTE)(p[7] & 0xC0);
    if (T->s[i].flags == 0x40)
        T->s[i].flags = 0;                 /* (forbidden) */
    if ((T->s[i].flags == 0x80 && hl < 5) || (T->s[i].flags == 0xC0 && hl < 10))
        T->s[i].flags = 0;
    memcpy(T->s[i].ts, p + 9, T->s[i].flags == 0xC0 ? 10 : T->s[i].flags ? 5 : 0);
    T->s[i].align = (UBYTE)(p[6] & 0x04);
    *data = 9 + hl;
    return TRUE;
}

/* A sequence header (00 00 01 B3) in n bytes at p. */
static BOOL has_seq(const UBYTE *p, LONG n)
{
    LONG j;
    for (j = 0; j + 4 <= n; j++)
        if (!p[j] && !p[j + 1] && p[j + 2] == 1 && p[j + 3] == 0xB3)
            return TRUE;
    return FALSE;
}

/* Its start (and after a seek): nothing until a video PES packet that
 * holds a sequence header (a decoder starts there), then, with sound, its
 * packs held (TSP_HOLD at most) until the sound's next PES packet begins;
 * that packet's first bytes go out first, then what was held. So the two
 * begin together and both show in the first few KB (Amiberry's emulated
 * card looks only at its first 4 KB to find its streams). */
LONG tsp_pack_in(struct Tsp *T, TspRead rd, void *ctx, UBYTE *out, ULONG max)
{
    ULONG o = 0;
    if (T->ended)
        return 0;
    while (o + 2 * (TSP_CHUNK + 80) <= max) {
        if (T->hoff < T->hlen) {
            /* what was held, out now */
            ULONG c = T->hlen - T->hoff, room = max - o - 2 * (TSP_CHUNK + 80);
            if (c > room)
                c = room;
            memcpy(out + o, T->hold + T->hoff, c);
            o += c;
            T->hoff += c;
            if (T->hoff < T->hlen)
                break;
            T->hoff = T->hlen = 0;
            continue;
        }
        const UBYTE *p;
        LONG k, n, data = 0;
        UWORD pid;
        BOOL pusi;
        int i;
        if (T->ilen - T->ipos < T->size && !T->eof) {
            LONG got;
            if (T->ipos)
                memmove(T->in, T->in + T->ipos, T->ilen - T->ipos);
            T->ilen -= T->ipos;
            T->ipos = 0;
            got = rd(ctx, T->in + T->ilen, TSP_IN - T->ilen);
            if (got < 0)
                return -1;
            if (got == 0)
                T->eof = TRUE;
            T->ilen += got;
        }
        if (T->ilen - T->ipos < T->size) {
            /* its end: what is held put out, then the end code */
            if (T->hlen) {
                T->prime = P_ON;           /* (its sound never came) */
                continue;
            }
            for (i = 0; i < 2; i++)
                if (T->s[i].on && T->s[i].n)
                    o += put_chunk(T, i, out + o);
            out[o++] = 0; out[o++] = 0; out[o++] = 1; out[o++] = 0xB9;
            T->ended = TRUE;
            break;
        }
        p = T->in + T->ipos + T->lead;
        if (p[0] != 0x47) {
            T->ipos++;                     /* out of step: found again */
            T->resyncs++;
            continue;
        }
        T->ipos += T->size;
        pid = (UWORD)(((p[1] & 0x1F) << 8) | p[2]);
        i = pid == T->pid[0] ? 0 : pid == T->pid[1] && pid != 0x1FFF ? 1 : -1;
        if (i < 0 || (p[1] & 0x80))
            continue;                       /* (another stream's, or marked in error) */
        k = 4;
        if (p[3] & 0x20)
            k += 1 + p[4];                  /* its adaptation field */
        if (!(p[3] & 0x10) || k >= 188)
            continue;
        n = 188 - k;
        pusi = (p[1] & 0x40) != 0;
        p += k;
        if (T->prime == P_SEQ) {
            /* waiting for a video PES packet with a sequence header */
            if (i == 1 || !pusi || !pes_start(T, i, p, n, &data) || !has_seq(p + data, n - data)) {
                T->primed += T->size;
                if (T->primed < TSP_PRIME)
                    continue;
            }
            T->prime = T->pid[1] != 0x1FFF ? P_SOUND : P_ON;
        } else if (T->prime == P_SOUND && i == 1 && !pusi)
            continue;                       /* (its sound from its next PES packet) */
        if (pusi) {
            /* a PES packet begins: what was held of the one before put out */
            if (T->s[i].on && T->s[i].n)
                o += i == 0 && T->prime == P_SOUND ? hold_chunk(T) : put_chunk(T, i, out + o);
            if (!pes_start(T, i, p, n, &data)) {
                T->s[i].on = FALSE;        /* (not one we can read: to the next) */
                T->cut++;
                continue;
            }
            T->s[i].on = TRUE;
            T->s[i].first = TRUE;
            T->s[i].n = 0;
        } else if (!T->s[i].on)
            continue;                       /* (its start not seen: after a seek) */
        while (data < n) {
            LONG c = n - data, room = TSP_CHUNK - T->s[i].n;
            if (c > room)
                c = room;
            memcpy(T->s[i].buf + T->s[i].n, p + data, c);
            T->s[i].n += (UWORD)c;
            data += c;
            if (T->s[i].n == TSP_CHUNK)
                o += i == 0 && T->prime == P_SOUND ? hold_chunk(T) : put_chunk(T, i, out + o);
        }
        if (T->prime == P_SOUND && i == 1 && T->s[1].n) {
            o += put_chunk(T, 1, out + o);     /* the sound's first, now: then what was held */
            T->prime = P_ON;
        }
    }
    return (LONG)o;
}

/* After a seek (the file read from elsewhere): what was half read let go,
 * its streams and their PIDs kept, and its video taken from its next PES
 * packet on, not waited for until a sequence header (its decoder has the
 * one from its start: a file may have no other). With sound, still held
 * until the sound's next PES packet. */
void tsp_seek_in(struct Tsp *T)
{
    memset(T->s, 0, sizeof(T->s));
    T->ipos = T->ilen = 0;
    T->eof = T->ended = FALSE;
    T->hlen = T->hoff = 0;
    T->primed = 0;
    T->prime = T->pid[1] != 0x1FFF ? P_SOUND : P_ON;
}

/* ---- the one the playback process has (tspack.h's first three) */
void tsp_begin(UBYTE size, UWORD vpid, UWORD apid, BOOL ac3, BOOL mpeg2)
{
    tsp_begin_in(&tsp_one, size, vpid, apid, ac3, mpeg2);
}

LONG tsp_pack(TspRead rd, void *ctx, UBYTE *out, ULONG max)
{
    return tsp_pack_in(&tsp_one, rd, ctx, out, max);
}

void tsp_counts(ULONG *resyncs, ULONG *packets, ULONG *before_sound)
{
    tsp_counts_in(&tsp_one, resyncs, packets, before_sound);
}

/* ---- one of its own (a picture's maker, beside what plays): its
 * memory the caller's, tsp_size() bytes (about 180 KB) */
ULONG tsp_size(void)
{
    return (ULONG)sizeof(struct Tsp);
}
