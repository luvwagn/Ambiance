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
};

/* TRUE when a sequence header was found (its size known). */
BOOL vprobe(const char *path, struct VProbe *v);
const char *vprobe_kind(const struct VProbe *v);

#endif
