/*
 * AmigaMCE - a MED/OctaMED song's length (MMD0-MMD3), by walking it
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * EaglePlayer's players (MED_note) say nothing of how long a song is, so
 * it is worked out here from the module as it is in its file (offsets
 * from its start, before a player relocates it): its order (MMD0/1's
 * playseq; MMD2/3's sections, each a play sequence), each block's lines,
 * the ticks a line takes (the secondary tempo, command 09), and its tempo
 * changes (0F 01-F0), pattern breaks (0F 00, 1D), position jumps (0B),
 * line delays (1E) and the song's stop (0F FE). The length comes in ticks
 * at the song's first tempo (x 16): the player's own timer then says how
 * long a tick is. A tempo's ticks are as much faster as it is higher, in
 * both of MED's modes (SPD: its timer is 474326 / tempo E-clocks; BPM).
 *
 * Plain C, no AmigaOS headers (tests/medtime_host.c tries it on the build
 * machine).
 */
#include "medtime.h"

#define MAXSEQ 1024             /* positions followed, at most */
#define MAXLINES 200000UL       /* lines walked, at most (a song that never ends) */

static unsigned long u32(const unsigned char *m, unsigned long len, unsigned long o)
{
    if (o + 4 > len)
        return 0;
    return ((unsigned long)m[o] << 24) | ((unsigned long)m[o + 1] << 16) |
           ((unsigned long)m[o + 2] << 8) | m[o + 3];
}

static unsigned u16(const unsigned char *m, unsigned long len, unsigned long o)
{
    if (o + 2 > len)
        return 0;
    return ((unsigned)m[o] << 8) | m[o + 1];
}

unsigned long med_measure(const unsigned char *m, unsigned long len, struct MedTime *out)
{
    unsigned long song, blkarr, units = 0, lines = 0;
    unsigned short seq[MAXSEQ], first[MAXSEQ];      /* block per position; its section's start */
    unsigned char seen[MAXSEQ];
    int nseq = 0, ver, pos, line, numblocks, i;
    unsigned tempo, tempo0, tpl;
    int tracks = 0;
    if (out) {
        out->units16 = 0;
        out->positions = 0;
        out->tempo = 0;
        out->tracks = 0;
    }
    if (len < 52 || m[0] != 'M' || m[1] != 'M' || m[2] != 'D' || m[3] < '0' || m[3] > '3')
        return 0;
    ver = m[3] - '0';
    song = u32(m, len, 8);
    blkarr = u32(m, len, 16);
    if (!song || song + 788 > len || !blkarr || blkarr >= len)
        return 0;
    numblocks = (int)u16(m, len, song + 504);
    tempo0 = u16(m, len, song + 764);
    tpl = m[song + 769];
    if (!tempo0)
        tempo0 = 6;
    if (!tpl)
        tpl = 6;
    /* the order: blocks, by position */
    if (ver < 2) {
        int n = (int)u16(m, len, song + 506);
        for (i = 0; i < n && i < 256 && nseq < MAXSEQ; i++) {
            first[nseq] = 0;
            seq[nseq++] = m[song + 508 + i];
        }
    } else {
        int nsec = (int)u16(m, len, song + 506), s;
        unsigned long pstab = u32(m, len, song + 508), sectab = u32(m, len, song + 512);
        unsigned npseq = u16(m, len, song + 522);
        for (s = 0; s < nsec && nseq < MAXSEQ; s++) {
            unsigned ps = u16(m, len, sectab + 2 * s);
            unsigned long pq;
            unsigned n, k;
            if (!sectab || ps >= npseq || !(pq = u32(m, len, pstab + 4 * ps)))
                continue;
            n = u16(m, len, pq + 40);
            for (k = 0; k < n && nseq < MAXSEQ; k++) {
                unsigned b = u16(m, len, pq + 42 + 2 * k);
                if (b >= 0x8000)
                    continue;                       /* (a play sequence's command) */
                first[nseq] = (unsigned short)(nseq - k);
                seq[nseq++] = (unsigned short)b;
            }
        }
    }
    for (i = 0; i < numblocks; i++) {
        unsigned long blk = u32(m, len, blkarr + 4 * i);
        int nt = !blk || blk >= len ? 0 : ver == 0 ? m[blk] : (int)u16(m, len, blk);
        if (nt > tracks)
            tracks = nt;
    }
    if (!nseq)
        return 0;
    for (i = 0; i < nseq; i++)
        seen[i] = 0;
    tempo = tempo0;
    pos = 0;
    line = 0;
    while (pos >= 0 && pos < nseq && lines < MAXLINES) {
        unsigned long blk;
        unsigned ntr, nl, b = seq[pos], hdr, bytes;
        int next = pos + 1, nline = 0, jumped = 0, stop = 0, l;
        if (line == 0) {
            if (seen[pos])
                break;                              /* round again: its end */
            seen[pos] = 1;
        }
        if (b >= (unsigned)numblocks || !(blk = u32(m, len, blkarr + 4 * b)) || blk >= len) {
            pos = next;
            line = 0;
            continue;
        }
        if (ver == 0) {
            ntr = m[blk];
            nl = (unsigned)m[blk + 1] + 1;
            hdr = 2;
            bytes = 3;
        } else {
            ntr = u16(m, len, blk);
            nl = u16(m, len, blk + 2) + 1;
            hdr = 8;
            bytes = 4;
        }
        for (l = line; l < (int)nl && !jumped && !stop; l++) {
            unsigned t, delay = 0;
            unsigned long o = blk + hdr + (unsigned long)l * ntr * bytes;
            lines++;
            for (t = 0; t < ntr; t++, o += bytes) {
                unsigned cmd, dat;
                if (o + bytes > len)
                    break;
                if (ver == 0) {
                    cmd = m[o + 1] & 0x0F;
                    dat = m[o + 2];
                } else {
                    cmd = m[o + 2];
                    dat = m[o + 3];
                }
                if (cmd == 0x09 && dat >= 1 && dat <= 0x20) {
                    tpl = dat;                      /* ticks a line */
                } else if (cmd == 0x0F) {
                    if (dat == 0) {
                        jumped = 1;                 /* to the next position */
                    } else if (dat <= 0xF0) {
                        tempo = dat;
                    } else if (dat == 0xFE) {
                        stop = 1;                   /* the song stops */
                    }
                } else if (cmd == 0x0B) {
                    next = first[pos] + (int)dat;   /* (MMD2/3: in its section) */
                    jumped = 1;
                } else if (cmd == 0x1D) {
                    nline = (int)dat;               /* the next position, from that line */
                    jumped = 1;
                } else if (cmd == 0x1E) {
                    delay = dat;                    /* the line again, so many times */
                }
            }
            /* this line's ticks, as ticks at the first tempo (x 16) */
            units += (unsigned long)tpl * (1 + delay) * 16UL * tempo0 / tempo;
        }
        if (stop)
            break;
        pos = next;
        line = jumped ? nline : 0;
        if (line >= (int)nl)
            line = 0;
    }
    if (out) {
        out->units16 = units;
        out->positions = nseq;
        out->tempo = tempo0;
        out->tracks = tracks;
    }
    return units;
}
