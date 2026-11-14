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
        } else if (c == 0xB5 && first && (b[i + 4] >> 4) == 1) {
            v->mpeg2 = TRUE;                /* a sequence extension */
        } else if (c >= 0xC0 && c <= 0xDF) {
            v->mp2 = TRUE;                  /* MPEG audio (layer I or II, in practice II) */
        } else if (c >= 0xE0 && c <= 0xEF) {
            unsigned long long t;
            if (pes_pts(b + i, n - i, &t)) {
                if (!*have || t < *lo)
                    *lo = t;
                if (!*have || t > *hi)
                    *hi = t;
                *have = TRUE;
            }
        }
    }
}

BOOL vprobe(const char *path, struct VProbe *v)
{
    UBYTE *buf;
    BPTR fh;
    LONG n, size;
    unsigned long long lo0 = 0, hi0 = 0, lo1 = 0, hi1 = 0;
    BOOL have0 = FALSE, have1 = FALSE;
    memset(v, 0, sizeof(*v));
    if (!(fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
        return FALSE;
    if (!(buf = AllocVec(PROBE_BYTES, MEMF_ANY))) {
        Close(fh);
        return FALSE;
    }
    n = Read(fh, buf, PROBE_BYTES);
    if (n > 0)
        scan(buf, n, v, TRUE, &lo0, &hi0, &have0);
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    v->bytes = size > 0 ? (ULONG)size : 0;
    if (size > (LONG)PROBE_BYTES * 2 && Seek(fh, size - PROBE_BYTES, OFFSET_BEGINNING) >= 0 &&
        (n = Read(fh, buf, PROBE_BYTES)) > 0)
        scan(buf, n, v, FALSE, &lo1, &hi1, &have1);
    Close(fh);
    FreeVec(buf);
    if (have0) {
        unsigned long long end = have1 ? hi1 : hi0;
        if (end < lo0)
            end += 1ULL << 33;              /* (the clock went round) */
        v->ms = (ULONG)((end - lo0) / 90);
        if (v->fps_milli)
            v->ms += 1000000UL / v->fps_milli;  /* (its last picture's time too) */
    }
    return v->w && v->h;
}

const char *vprobe_kind(const struct VProbe *v)
{
    return v->mpeg2 ? "MPEG-2" : "MPEG-1";
}
