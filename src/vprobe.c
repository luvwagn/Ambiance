/*
 * AmigaMCE - an MPEG file's headers: what the ZZ9000's media session is
 * begun with, and what the preview pane says (vprobe.h)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Written from the MPEG-1 and MPEG-2 systems and video layers (ISO 11172,
 * 13818): a Program Stream's pack headers (MPEG-1's '0010', MPEG-2's '01'),
 * its video's sequence header (size, aspect, frame rate) and sequence
 * extension (MPEG-2), its MPEG audio streams (0xC0-0xDF), and its length
 * from the first and last video time stamps (the file's start and end read).
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "amce.h"
#include "vprobe.h"

#define PROBE_BYTES (192UL * 1024)
#define TS_LOOK     16          /* a transport stream: spans looked through for its first sequence header */

static const ULONG fps_milli[16] = { 0, 23976, 24000, 25000, 29970, 30000, 50000, 59940, 60000 };

/* A PES header's PTS (90 kHz, 33 bits), FALSE if it has none. b at the
 * packet's start code, n bytes there. */
static BOOL pes_pts(const UBYTE *b, LONG n, unsigned long long *pts)
{
    LONG i = 6;
    const UBYTE *p;
    if (n < 14)
        return FALSE;
    if ((b[6] & 0xC0) == 0x80) {            /* MPEG-2's PES header */
        if (!(b[7] & 0x80))
            return FALSE;
        p = b + 9;
    } else {                                /* MPEG-1's: stuffing, the STD buffer, then */
        while (i < n && i < 6 + 16 && b[i] == 0xFF)
            i++;
        if (i + 2 < n && (b[i] & 0xC0) == 0x40)
            i += 2;
        if (i + 5 > n || ((b[i] & 0xF0) != 0x20 && (b[i] & 0xF0) != 0x30))
            return FALSE;
        p = b + i;
    }
    *pts = ((unsigned long long)((p[0] >> 1) & 7) << 30) | ((ULONG)p[1] << 22) |
           ((ULONG)(p[2] >> 1) << 15) | ((ULONG)p[3] << 7) | (p[4] >> 1);
    return TRUE;
}

/* A PES packet's payload: its offset from the start code, -1 if not here. */
static LONG pes_payload(const UBYTE *b, LONG n)
{
    LONG i = 6;
    if (n < 9)
        return -1;
    if ((b[6] & 0xC0) == 0x80)              /* MPEG-2's PES header */
        return 9 + b[8];
    while (i < n && i < 6 + 16 && b[i] == 0xFF)
        i++;
    if (i + 2 < n && (b[i] & 0xC0) == 0x40)
        i += 2;
    if (i >= n)
        return -1;
    if ((b[i] & 0xF0) == 0x20)
        return i + 5;
    if ((b[i] & 0xF0) == 0x30)
        return i + 10;
    return b[i] == 0x0F ? i + 1 : -1;
}

/* The MPEG audio frame header in a's n bytes: what kind it is. */
static void audio_head(const UBYTE *a, LONG n, struct VProbe *v)
{
    static const UWORD hz1[3] = { 44100, 48000, 32000 };
    static const UWORD kb[3][16] = {
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 } };
    LONG i;
    for (i = 0; i + 3 < n; i++) {
        UBYTE ver, lay, rate;
        if (a[i] != 0xFF || (a[i + 1] & 0xE0) != 0xE0)
            continue;
        ver = (UBYTE)((a[i + 1] >> 3) & 3);
        lay = (UBYTE)((a[i + 1] >> 1) & 3);
        rate = (UBYTE)((a[i + 2] >> 2) & 3);
        if (ver == 1 || lay == 0 || rate == 3 || (a[i + 2] >> 4) == 15)
            continue;
        v->a_layer = (UBYTE)(4 - lay);
        v->a_version = ver == 3 ? 1 : ver == 2 ? 2 : 25;
        v->a_hz = hz1[rate] / (ver == 3 ? 1 : ver == 2 ? 2 : 4);
        v->a_kbps = ver == 3 ? kb[v->a_layer - 1][a[i + 2] >> 4] : 0;
        v->a_chans = (UBYTE)((a[i + 3] >> 6) == 3 ? 1 : 2);
        return;
    }
}

/* Through n bytes at b: what they say (first: the start of the file). */
static void scan(const UBYTE *b, LONG n, struct VProbe *v, BOOL first,
                 unsigned long long *lo, unsigned long long *hi, BOOL *have)
{
    LONG i;
    for (i = 0; i + 12 < n; i++) {
        UBYTE c;
        if (b[i] || b[i + 1] || b[i + 2] != 1)
            continue;
        c = b[i + 3];
        if (c == 0xBA) {                    /* a pack header */
            v->ps = TRUE;
            if ((b[i + 4] & 0xC0) == 0x40)
                v->mpeg2 = TRUE;
        } else if (c == 0xB3 && first && !v->w) {
            v->w = (UWORD)((b[i + 4] << 4) | (b[i + 5] >> 4));
            v->h = (UWORD)(((b[i + 5] & 0x0F) << 8) | b[i + 6]);
            v->aspect = (UBYTE)(b[i + 7] >> 4);
            v->fps_milli = fps_milli[b[i + 7] & 0x0F];
            v->kbps = (((ULONG)b[i + 8] << 10) | ((ULONG)b[i + 9] << 2) | (b[i + 10] >> 6)) * 2 / 5;
        } else if (c == 0xB5 && first && (b[i + 4] >> 4) == 1) {
            v->mpeg2 = TRUE;                /* a sequence extension */
        } else if (c >= 0xC0 && c <= 0xDF) {
            v->mp2 = TRUE;                  /* MPEG audio (layer I or II, in practice II) */
            if (first && (!v->audio_id || (c == v->audio_id && !v->a_layer))) {
                LONG len = ((LONG)b[i + 4] << 8) | b[i + 5], o = pes_payload(b + i, n - i);
                v->audio_id = c;
                if (o > 0 && o < len + 6) {
                    LONG end = i + 6 + len;
                    if (end > n)
                        end = n;
                    if (i + o < end)
                        audio_head(b + i + o, end - i - o, v);
                }
            }
        } else if (c >= 0xE0 && c <= 0xEF) {
            unsigned long long t;
            if (pes_pts(b + i, n - i, &t)) {
                if (!*have || t < *lo)
                    *lo = t;
                if (!*have || t > *hi)
                    *hi = t;
                *have = TRUE;
            }
        } else if (c == 0xBD && i + 9 < n) {
            LONG o = pes_payload(b + i, n - i);
            if (o > 0 && i + o < n && b[i + o] >= 0x80 && b[i + o] <= 0x87)
                v->ac3 = TRUE;              /* a DVD's AC3 (private stream 1, 0x80-0x87) */
        } else if (first && !v->w && (c & 0x80) == 0 && (c & 0x1F) == 7 && c != 0x07) {
            v->h264 = TRUE;                 /* an H.264 sequence parameter set (no MPEG
                                             * sequence header come before it) */
        }
    }
}

/* ---- a transport stream: its packets' PIDs from its PAT and PMT, the
 * video's and the sound's payloads put together and read as above */
static BOOL ts_size(const UBYTE *b, LONG n, ULONG *at, UBYTE *size)
{
    LONG i;
    for (i = 0; i < 200 && i + 3 * 192 < n; i++) {
        if (b[i] == 0x47 && b[i + 188] == 0x47 && b[i + 376] == 0x47 && b[i + 564] == 0x47) {
            *at = (ULONG)i;
            *size = 188;
            return TRUE;
        }
        if (i >= 4 && b[i] == 0x47 && b[i + 192] == 0x47 && b[i + 384] == 0x47) {
            *at = (ULONG)(i - 4);           /* (an .m2ts: a 4-byte time before each) */
            *size = 192;
            return TRUE;
        }
    }
    return FALSE;
}

static void ts_pmt(const UBYTE *p, LONG n, struct VProbe *v)
{
    LONG sl, j, end, pil;
    if (n < 12)
        return;
    sl = ((p[1] & 0x0F) << 8) | p[2];
    end = 3 + sl - 4;
    if (end > n)
        end = n;
    pil = ((p[10] & 0x0F) << 8) | p[11];
    for (j = 12 + pil; j + 5 <= end;) {
        UBYTE t = p[j];
        UWORD pid = (UWORD)(((p[j + 1] & 0x1F) << 8) | p[j + 2]);
        LONG el = ((p[j + 3] & 0x0F) << 8) | p[j + 4], d;
        BOOL ac3 = t == 0x81;
        for (d = j + 5; t == 0x06 && d + 2 <= j + 5 + el && d + 2 <= end; d += 2 + p[d + 1])
            if (p[d] == 0x6A || p[d] == 0x81)
                ac3 = TRUE;                 /* DVB's (or ATSC's) AC3 descriptor */
        if (!v->ts_vtype && (t == 0x01 || t == 0x02 || t == 0x1B || t == 0x24 || t == 0x10)) {
            v->ts_vtype = t;
            v->ts_vpid = pid;
        } else if (v->ts_apid == 0x1FFF && (t == 0x03 || t == 0x04 || ac3)) {
            v->ts_atype = ac3 ? 0x81 : t;
            v->ts_apid = pid;
        } else if (!v->a_other && (t == 0x0F || t == 0x11))
            v->a_other = "AAC";
        j += 5 + el;
    }
}

/* n bytes of packets from at: the video's payloads to vb, the sound's to
 * ab (their sizes back), the PAT and PMT read on the way. */
static void ts_split(const UBYTE *b, LONG n, ULONG at, struct VProbe *v, UBYTE *vb, LONG *vn,
                     LONG vmax, UBYTE *ab, LONG *an, LONG amax)
{
    UWORD pmt = 0x1FFF;
    LONG i, lead = v->ts == 192 ? 4 : 0;
    *vn = *an = 0;
    for (i = (LONG)at; i + v->ts <= n; i += v->ts) {
        const UBYTE *p = b + i + lead;
        UWORD pid;
        LONG k = 4;
        if (p[0] != 0x47) {
            LONG j;                         /* (lost: found again) */
            for (j = i + 1; j + v->ts <= n && b[j + lead] != 0x47; j++)
                ;
            i = j - v->ts;
            continue;
        }
        pid = (UWORD)(((p[1] & 0x1F) << 8) | p[2]);
        if (p[3] & 0x20)
            k += 1 + p[4];                  /* its adaptation field */
        if (!(p[3] & 0x10) || k >= 188)
            continue;                       /* (no payload) */
        if (pid == 0 && (p[1] & 0x40) && pmt == 0x1FFF) {
            const UBYTE *t = p + k + 1 + p[k];
            LONG sl = ((t[1] & 0x0F) << 8) | t[2], j;
            for (j = 8; j + 4 <= 3 + sl - 4 && t + j + 4 <= p + 188; j += 4)
                if ((t[j] << 8 | t[j + 1]) != 0) {
                    pmt = (UWORD)(((t[j + 2] & 0x1F) << 8) | t[j + 3]);
                    break;
                }
        } else if (pid == pmt && (p[1] & 0x40) && !v->ts_vtype) {
            const UBYTE *t = p + k + 1 + p[k];
            if (t < p + 188)
                ts_pmt(t, (LONG)(p + 188 - t), v);
        } else if (pid == v->ts_vpid && v->ts_vtype && *vn + 188 <= vmax) {
            CopyMem((APTR)(p + k), vb + *vn, 188 - k);
            *vn += 188 - k;
        } else if (pid == v->ts_apid && v->ts_apid != 0x1FFF && *an + 188 <= amax) {
            CopyMem((APTR)(p + k), ab + *an, 188 - k);
            *an += 188 - k;
        }
    }
}

BOOL vprobe(const char *path, struct VProbe *v, ULONG span)
{
    UBYTE *buf, *tb = NULL;
    BPTR fh;
    LONG n, size;
    unsigned long long lo0 = 0, hi0 = 0, lo1 = 0, hi1 = 0;
    BOOL have0 = FALSE, have1 = FALSE;
    memset(v, 0, sizeof(*v));
    if (!span || span > PROBE_BYTES)
        span = PROBE_BYTES;
    if (!(fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
        return FALSE;
    if (!(buf = AllocVec(span, MEMF_ANY))) {
        Close(fh);
        return FALSE;
    }
    v->ts_apid = v->ts_vpid = 0x1FFF;
    n = Read(fh, buf, (LONG)span);
    if (n > 0 && ts_size(buf, n, &v->ts_first, &v->ts))
        tb = AllocVec(span + 4096, MEMF_ANY);
    if (n > 0 && v->ts && tb) {
        /* a transport stream: its video's and sound's payloads, read as a
         * Program Stream's would be */
        LONG vn, an;
        ts_split(buf, n, v->ts_first, v, tb, &vn, (LONG)span, tb + span, &an, 4096);
        if (v->ts_vtype == 0x1B || v->ts_vtype == 0x24 || v->ts_vtype == 0x10)
            v->h264 = TRUE;                 /* H.264, HEVC, MPEG-4: not the card's */
        else if (v->ts_vtype) {
            scan(tb, vn, v, TRUE, &lo0, &hi0, &have0);
            if (v->ts_vtype == 0x02)
                v->mpeg2 = TRUE;
        }
        if (an > 0)
            scan(tb + span, an, v, TRUE, &lo0, &hi0, &have0);    /* (its sound's kind) */
        {
            /* its first sequence header further on (a broadcast's packets
             * are many of them padding, its first picture not a whole one):
             * up to TS_LOOK spans more looked through */
            ULONG more = 0, pos = v->ts_first + ((ULONG)n - v->ts_first) / v->ts * v->ts;
            LONG chunk = (LONG)span - (LONG)span % v->ts;
            if (v->ts_vtype && !v->h264 && !v->w && Seek(fh, (LONG)pos, OFFSET_BEGINNING) >= 0)
                while (!v->w && more < span * TS_LOOK && (n = Read(fh, buf, chunk)) > v->ts) {
                    more += (ULONG)n;
                    ts_split(buf, n, 0, v, tb, &vn, (LONG)span, tb + span, &an, 4096);
                    scan(tb, vn, v, TRUE, &lo0, &hi0, &have0);
                }
        }
        if (v->ts_atype == 0x81) {
            v->ac3 = TRUE;
            v->mp2 = FALSE;
        }
    } else if (n > 0)
        scan(buf, n, v, TRUE, &lo0, &hi0, &have0);
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    v->bytes = size > 0 ? (ULONG)size : 0;
    if (size > (LONG)span * 2 && Seek(fh, size - (LONG)span, OFFSET_BEGINNING) >= 0 &&
        (n = Read(fh, buf, (LONG)span)) > 0) {
        if (v->ts && tb) {
            ULONG at;
            UBYTE sz;
            LONG vn, an;
            if (!v->h264 && ts_size(buf, n, &at, &sz) && sz == v->ts) {
                ts_split(buf, n, at, v, tb, &vn, (LONG)span, tb + span, &an, 4096);
                scan(tb, vn, v, FALSE, &lo1, &hi1, &have1);
            }
        } else if (!v->ts)
            scan(buf, n, v, FALSE, &lo1, &hi1, &have1);
    }
    Close(fh);
    FreeVec(buf);
    FreeVec(tb);
    if (have0) {
        unsigned long long end = have1 ? hi1 : hi0;
        if (end < lo0)
            end += 1ULL << 33;              /* (the clock went round) */
        v->ms = (ULONG)((end - lo0) / 90);
        if (v->fps_milli)
            v->ms += 1000000UL / v->fps_milli;  /* (its last picture's time too) */
    }
    if (!v->ms && v->kbps && v->kbps < 104857 && v->bytes && !v->ts)
        v->ms = v->bytes / v->kbps * 8;     /* (a bare stream: by its bitrate) */
    if (v->ts)
        v->ps = FALSE;                      /* (sent as one, packed as it goes) */
    if (v->w && v->h)
        v->h264 = FALSE;                    /* (an MPEG sequence header: MPEG after all) */
    return v->w && v->h;
}

const char *vprobe_kind(const struct VProbe *v)
{
    if (v->h264)
        return v->ts_vtype == 0x24 ? "HEVC" : v->ts_vtype == 0x10 ? "MPEG-4" : "H.264";
    return v->mpeg2 ? "MPEG-2" : "MPEG-1";
}
