/*
 * Ambiance - an MPEG transport stream (.ts, .m2ts) packed as a Program
 * Stream as it is read (tspack.c), for the ZZ9000's media session
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef AMB_TSPACK_H
#define AMB_TSPACK_H

#include <exec/types.h>

/* Reads up to n bytes of the file into b: the bytes read, 0 at its end,
 * -1 if it could not be read. */
typedef LONG (*TspRead)(void *ctx, UBYTE *b, LONG n);

/* A new stream (or the file from another place: a seek): packets of size
 * (188, or 192 with a 4-byte time before each), the video's PID (MPEG-1 or
 * MPEG-2), the sound's (0x1FFF none), AC3 or MPEG audio. */
void tsp_begin(UBYTE size, UWORD vpid, UWORD apid, BOOL ac3, BOOL mpeg2);
/* Up to max bytes of the Program Stream into out: how many (0 at its end,
 * its end code given before); -1 if the file could not be read. */
LONG tsp_pack(TspRead rd, void *ctx, UBYTE *out, ULONG max);
/* The packets lost (out of step), PES packets not read, and the video's
 * bytes passed over before its sound's first, so far. */
void tsp_counts(ULONG *resyncs, ULONG *packets, ULONG *before_sound);

/* The same with one of the caller's own (the three above use one of
 * tspack.c's, the playback process's): its memory tsp_size() bytes, any
 * kind, begun with tsp_begin_in. For a second stream read at the same
 * time (a video's picture made while another plays). */
struct Tsp;
ULONG tsp_size(void);
void tsp_begin_in(struct Tsp *t, UBYTE size, UWORD vpid, UWORD apid, BOOL ac3, BOOL mpeg2);
LONG tsp_pack_in(struct Tsp *t, TspRead rd, void *ctx, UBYTE *out, ULONG max);
void tsp_counts_in(struct Tsp *t, ULONG *resyncs, ULONG *packets, ULONG *before_sound);
/* After a seek: the file read from elsewhere, its video from its next PES
 * packet on (its decoder has a sequence header already: from its start) */
void tsp_seek_in(struct Tsp *t);

#endif
