/*
 * AmigaMCE - the ZZ9000's zz9k.library, as AmigaMCE uses it (zz9k.c)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Only zz9k.c includes the ZZ9000 SDK's headers (third_party/zz9k-sdk,
 * GPL-3.0-or-later: see its README before distributing a build); the rest
 * of AmigaMCE sees these types of its own.
 */
#ifndef AMCE_ZZ9K_H
#define AMCE_ZZ9K_H

#include <exec/types.h>

typedef unsigned long long ZzU64;

#define ZZ_OK   0
#define ZZ_BUSY 2               /* (a write the card has no room for yet) */
#define ZZ_NO_PTS 0xFFFFFFFFFFFFFFFFULL

/* A media session's result flags (the SDK's ZZ9K_MEDIA_SESSION_RESULT_*) */
#define ZZ_R_HEADER_READY  (1UL << 0)
#define ZZ_R_NEED_INPUT    (1UL << 1)
#define ZZ_R_FRAME_HELD    (1UL << 2)
#define ZZ_R_DONE          (1UL << 3)
#define ZZ_R_REBASED       (1UL << 6)
#define ZZ_R_AUDIO_READY   (1UL << 7)
#define ZZ_R_BACKPRESSURE  (1UL << 8)

/* DECODE's flags */
#define ZZ_SKIP_TO_KEYFRAME  (1UL << 0)     /* MPEG: B pictures skipped */
#define ZZ_SKIP_TO_I_PICTURE (1UL << 1)     /* MPEG: on to the next I picture */

enum { ZZ_DECODE, ZZ_PRESENT, ZZ_DISCARD, ZZ_CLOSE };

struct ZzBuf {
    ULONG handle;
    volatile UBYTE *data;       /* the 68k's view of it */
    ULONG length;
};

struct ZzMain {
    ULONG session, state, width, height, rate_num, rate_den, frame;
    ZzU64 pts;                  /* the held picture's, 90 kHz */
    ULONG accepted, written, flags;
};

struct ZzAudio {
    ULONG session, state, rate, channels, format;
    ZzU64 produced, acknowledged, pts;
    ULONG flags;
};

BOOL zz_open(void);             /* zz9k.library (counted: each open closed) */
void zz_close(void);
const char *zz_name(int status);
/* MPEG Program Stream video on the card's overlay: NULL if it can, else
 * why not (logged once). */
const char *zz_video_check(BOOL mpeg2, BOOL mp2);
int zz_alloc(ULONG length, struct ZzBuf *b);    /* host-visible, shared */
void zz_free(struct ZzBuf *b);
enum { ZZ_AUDIO_NONE = 0, ZZ_AUDIO_MP2 = 1, ZZ_AUDIO_AC3 = 3 };   /* (the SDK's numbers) */
int zz_begin(ULONG w, ULONG h, BOOL mpeg2, int audio, const struct ZzBuf *pcm,
             struct ZzMain *r);
int zz_write(ULONG session, const struct ZzBuf *in, ULONG off, ULONG len, BOOL eof,
             struct ZzMain *r);
int zz_cmd(int op, ULONG session, ULONG flags, struct ZzMain *r);
int zz_audio_read(ULONG session, ZzU64 acknowledged, struct ZzAudio *r);
int zz_audio_origin(ULONG session, ZzU64 *pts);     /* the audio's first PTS */

#endif
