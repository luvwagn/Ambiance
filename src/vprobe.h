/*
 * AmigaMCE - an MPEG file's headers (vprobe.c)
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef AMCE_VPROBE_H
#define AMCE_VPROBE_H

#include <exec/types.h>

struct VProbe {
    UWORD w, h;                 /* its pictures' size */
    UBYTE aspect;               /* the sequence header's aspect code */
    ULONG fps_milli;            /* frames a second x 1000, 0 not known */
    BOOL ps;                    /* a Program Stream (pack headers): the card plays these */
    BOOL mpeg2;
    BOOL mp2;                   /* it has MPEG audio */
    ULONG ms;                   /* its length, 0 not known */
    ULONG bytes;
    ULONG kbps;                 /* the sequence header's bitrate (kbit/s) */
    UBYTE audio_id;             /* its first MPEG audio stream (0xC0-0xDF), 0 none */
    UBYTE a_layer;              /* that stream's first frame: layer 1-3, 0 not found */
    UBYTE a_chans;
    UBYTE a_version;            /* 1 MPEG-1, 2 MPEG-2, 25 MPEG-2.5 */
    ULONG a_hz, a_kbps;
};

/* TRUE when a sequence header was found (its size known). */
BOOL vprobe(const char *path, struct VProbe *v);
const char *vprobe_kind(const struct VProbe *v);

#endif
