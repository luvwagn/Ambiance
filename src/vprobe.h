/*
 * Ambiance - an MPEG file's headers (vprobe.c)
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef AMB_VPROBE_H
#define AMB_VPROBE_H

#include <exec/types.h>

struct VProbe {
    UWORD w, h;                 /* its pictures' size */
    UBYTE aspect;               /* the sequence header's aspect code */
    ULONG fps_milli;            /* frames a second x 1000, 0 not known */
    BOOL ps;                    /* a Program Stream (pack headers): the card plays these
                                 * (a transport stream, ts, sent to it as one) */
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
    BOOL ac3;                   /* AC3 sound (a DVD's private stream 1, or a .ts's) */
    BOOL h264;                  /* H.264 (or other) video, not MPEG: not the card's */
    UBYTE ts;                   /* a transport stream: its packets' size (188, 192), 0 not */
    UBYTE ts_vtype, ts_atype;   /* its video's and sound's stream types (PMT) */
    UWORD ts_vpid, ts_apid;     /* their packets' PIDs (0x1FFF none) */
    UWORD ts_ppid;              /* a private stream (type 06): its sound told by its packets */
    ULONG ts_first;             /* its first packet's offset in the file */
    const char *a_other;        /* sound there the card does not play ("AAC"), NULL none */
    BOOL sw;                    /* to be decoded by the CPU (mpegv.library): video_check's choice */
};

/* TRUE when a sequence header was found (its size known). span: the
 * bytes read at its start and again at its end (0: 192 KB). */
BOOL vprobe(const char *path, struct VProbe *v, ULONG span);
const char *vprobe_kind(const struct VProbe *v);

#endif
