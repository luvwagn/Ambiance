/*
 * AmigaMCE - an MPEG transport stream (.ts, .m2ts) packed as a Program
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

static struct {
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
} tsp;

void tsp_begin(UBYTE size, UWORD vpid, UWORD apid, BOOL ac3, BOOL mpeg2)
{
    memset(&tsp, 0, sizeof(tsp));
    tsp.size = size;
    tsp.lead = size == 192 ? 4 : 0;
    tsp.pid[0] = vpid;
    tsp.pid[1] = apid;
    tsp.sid[0] = 0xE0;
    tsp.sid[1] = ac3 ? 0xBD : 0xC0;
    tsp.ac3 = ac3;
    tsp.mpeg2 = mpeg2;
    tsp.prime = P_SEQ;
}

void tsp_counts(ULONG *resyncs, ULONG *packets, ULONG *before_sound)
{
    *resyncs = tsp.resyncs;
    *packets = tsp.cut;
    *before_sound = tsp.primed;
}

/* A pack header (MPEG-2), and before the first packet the system header. */
static ULONG pack_header(UBYTE *o)
{
    ULONG k = 0, scr = (ULONG)(((unsigned long long)tsp.bytes * 90000ULL) / (TSP_RATE * 50UL));
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
    if (!tsp.sys) {
        BOOL snd = tsp.pid[1] != 0x1FFF;
        tsp.sys = TRUE;
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
            o[k++] = tsp.sid[1];
            if (tsp.ac3) {
                o[k++] = 0xE0; o[k++] = 58;             /* AC3: 58 KB */
            } else {
                o[k++] = 0xC0; o[k++] = 32;             /* MPEG audio: 4 KB */
            }
        }
    }
    return k;
}

/* Stream i's chunk out as a Program Stream packet (after a pack header). */
static ULONG put_chunk(int i, UBYTE *o)
{
    ULONG k = pack_header(o), len, hl, j;
    UWORD n = tsp.s[i].n;
    BOOL first = tsp.s[i].first;
    hl = first ? (tsp.s[i].flags == 0xC0 ? 10 : tsp.s[i].flags == 0x80 ? 5 : 0) : 0;
    len = 3 + hl + (tsp.ac3 && i ? 4 : 0) + n;
    o[k++] = 0; o[k++] = 0; o[k++] = 1; o[k++] = tsp.sid[i];
    o[k++] = (UBYTE)(len >> 8);
    o[k++] = (UBYTE)len;
    o[k++] = (UBYTE)(0x80 | (first ? tsp.s[i].align : 0));
    o[k++] = (UBYTE)(hl ? tsp.s[i].flags : 0);
    o[k++] = (UBYTE)hl;
    for (j = 0; j < hl; j++)
        o[k++] = tsp.s[i].ts[j];
    if (tsp.ac3 && i) {
        /* a DVD's AC3: substream 0x80, the frames that begin here, and
         * where the first does (from the byte after these four, plus 1) */
        ULONG f = 0, at = 0;
        const UBYTE *b = tsp.s[i].buf;
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
    memcpy(o + k, tsp.s[i].buf, n);
    k += n;
    tsp.s[i].n = 0;
    tsp.s[i].first = FALSE;
    tsp.bytes += k;
    return k;
}

/* The video's chunk into the hold (its sound's first awaited); full, the
 * sound given up: what is held goes out as it is. 0 (nothing out yet). */
static ULONG hold_chunk(void)
{
    if (tsp.hlen + TSP_CHUNK + 80 > TSP_HOLD)
        tsp.prime = P_ON;                   /* (no sound soon enough) */
    tsp.hlen += put_chunk(0, tsp.hold + tsp.hlen);
    return 0;
}

/* A PES packet's start in a transport packet's payload: its header kept,
 * the bytes after it (its data) at *data. FALSE if it is not one. */
static BOOL pes_start(int i, const UBYTE *p, LONG n, LONG *data)
{
    LONG hl;
    if (n < 9 || p[0] || p[1] || p[2] != 1)
        return FALSE;
    if ((p[6] & 0xC0) != 0x80)
        return FALSE;                       /* (not an MPEG-2 PES header) */
    hl = p[8];
    if (9 + hl > n)
        return FALSE;                       /* (its header cut: not here) */
    tsp.s[i].flags = (UBYTE)(p[7] & 0xC0);
    if (tsp.s[i].flags == 0x40)
        tsp.s[i].flags = 0;                 /* (forbidden) */
    if ((tsp.s[i].flags == 0x80 && hl < 5) || (tsp.s[i].flags == 0xC0 && hl < 10))
        tsp.s[i].flags = 0;
    memcpy(tsp.s[i].ts, p + 9, tsp.s[i].flags == 0xC0 ? 10 : tsp.s[i].flags ? 5 : 0);
    tsp.s[i].align = (UBYTE)(p[6] & 0x04);
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
LONG tsp_pack(TspRead rd, void *ctx, UBYTE *out, ULONG max)
{
    ULONG o = 0;
    if (tsp.ended)
        return 0;
    while (o + 2 * (TSP_CHUNK + 80) <= max) {
        if (tsp.hoff < tsp.hlen) {
            /* what was held, out now */
            ULONG c = tsp.hlen - tsp.hoff, room = max - o - 2 * (TSP_CHUNK + 80);
            if (c > room)
                c = room;
            memcpy(out + o, tsp.hold + tsp.hoff, c);
            o += c;
            tsp.hoff += c;
            if (tsp.hoff < tsp.hlen)
                break;
            tsp.hoff = tsp.hlen = 0;
            continue;
        }
        const UBYTE *p;
        LONG k, n, data = 0;
        UWORD pid;
        BOOL pusi;
        int i;
        if (tsp.ilen - tsp.ipos < tsp.size && !tsp.eof) {
            LONG got;
            if (tsp.ipos)
                memmove(tsp.in, tsp.in + tsp.ipos, tsp.ilen - tsp.ipos);
            tsp.ilen -= tsp.ipos;
            tsp.ipos = 0;
            got = rd(ctx, tsp.in + tsp.ilen, TSP_IN - tsp.ilen);
            if (got < 0)
                return -1;
            if (got == 0)
                tsp.eof = TRUE;
            tsp.ilen += got;
        }
        if (tsp.ilen - tsp.ipos < tsp.size) {
            /* its end: what is held put out, then the end code */
            if (tsp.hlen) {
                tsp.prime = P_ON;           /* (its sound never came) */
                continue;
            }
            for (i = 0; i < 2; i++)
                if (tsp.s[i].on && tsp.s[i].n)
                    o += put_chunk(i, out + o);
            out[o++] = 0; out[o++] = 0; out[o++] = 1; out[o++] = 0xB9;
            tsp.ended = TRUE;
            break;
        }
        p = tsp.in + tsp.ipos + tsp.lead;
        if (p[0] != 0x47) {
            tsp.ipos++;                     /* out of step: found again */
            tsp.resyncs++;
            continue;
        }
        tsp.ipos += tsp.size;
        pid = (UWORD)(((p[1] & 0x1F) << 8) | p[2]);
        i = pid == tsp.pid[0] ? 0 : pid == tsp.pid[1] && pid != 0x1FFF ? 1 : -1;
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
        if (tsp.prime == P_SEQ) {
            /* waiting for a video PES packet with a sequence header */
            if (i == 1 || !pusi || !pes_start(i, p, n, &data) || !has_seq(p + data, n - data)) {
                tsp.primed += tsp.size;
                if (tsp.primed < TSP_PRIME)
                    continue;
            }
            tsp.prime = tsp.pid[1] != 0x1FFF ? P_SOUND : P_ON;
        } else if (tsp.prime == P_SOUND && i == 1 && !pusi)
            continue;                       /* (its sound from its next PES packet) */
        if (pusi) {
            /* a PES packet begins: what was held of the one before put out */
            if (tsp.s[i].on && tsp.s[i].n)
                o += i == 0 && tsp.prime == P_SOUND ? hold_chunk() : put_chunk(i, out + o);
            if (!pes_start(i, p, n, &data)) {
                tsp.s[i].on = FALSE;        /* (not one we can read: to the next) */
                tsp.cut++;
                continue;
            }
            tsp.s[i].on = TRUE;
            tsp.s[i].first = TRUE;
            tsp.s[i].n = 0;
        } else if (!tsp.s[i].on)
            continue;                       /* (its start not seen: after a seek) */
        while (data < n) {
            LONG c = n - data, room = TSP_CHUNK - tsp.s[i].n;
            if (c > room)
                c = room;
            memcpy(tsp.s[i].buf + tsp.s[i].n, p + data, c);
            tsp.s[i].n += (UWORD)c;
            data += c;
            if (tsp.s[i].n == TSP_CHUNK)
                o += i == 0 && tsp.prime == P_SOUND ? hold_chunk() : put_chunk(i, out + o);
        }
        if (tsp.prime == P_SOUND && i == 1 && tsp.s[1].n) {
            o += put_chunk(1, out + o);     /* the sound's first, now: then what was held */
            tsp.prime = P_ON;
        }
    }
    return (LONG)o;
}
