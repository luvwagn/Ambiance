/*
 * AmigaMCE - MPEG video, played by the ZZ9000 (video.c)
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef AMCE_VIDEO_H
#define AMCE_VIDEO_H

#include <exec/types.h>

struct Gfx;

/* path played on g's display till its end or a key; vol(d): the volume
 * up or down by d (0: mute or not). 0 played, -1 not (err says why). */
int video_play(struct Gfx *g, const char *path, UWORD cpu, BOOL (*vol)(int d), char *err,
               LONG errmax);

#endif
