/*
 * AmigaMCE - DVD-Video discs: found, and their main film played (dvdplay.c)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Through amcedvd.library (src/dvdlib), opened only while a disc is
 * played. Its sectors are read from the drive (dvddisc.c), not through the
 * volume's filesystem, which only says a DVD is there.
 */
#ifndef AMCE_DVDPLAY_H
#define AMCE_DVDPLAY_H

#include <exec/types.h>

struct VProbe;

/* ---- DVDs in drives */
struct DvdVol {
    char vol[32];               /* its volume's name, without the colon */
    char dev[48];               /* its drive: device and unit, from its DOS device */
    LONG unit;
};

/* The DVD-Video discs mounted now: volumes of CD/DVD filesystems (by their
 * handler's name or DOS type: UDF, CDFileSystem, ODFileSystem...) with a
 * VIDEO_TS/VIDEO_TS.IFO. Their count, up to max. */
LONG dvd_volumes(struct DvdVol *v, LONG max);
/* The CD/DVD volumes mounted now, as a number that changes when one comes
 * or goes (no disc is read: cheap enough to ask every second). */
ULONG dvd_volumes_sig(void);

BOOL dvd_lib_there(void);       /* amcedvd.library in PROGDIR:Libs or LIBS: */

/* ---- the main film of one */
struct DvdPlay;

/* The disc in volume vol (its name, with or without the colon): its
 * longest title chosen and started, v filled for the player (an MPEG-2
 * Program Stream: size, aspect, length, sound). NULL, with why in err. */
struct DvdPlay *dvdplay_open(const char *vol, struct VProbe *v, char *err, LONG errlen);
/* The same, from an image file of a disc (a .iso) */
struct DvdPlay *dvdplay_open_image(const char *path, struct VProbe *v, char *err, LONG errlen);
const char *dvdplay_name(struct DvdPlay *dp);   /* "MOVIE: title 3" */
/* The film from where it is, for the card: whole 2048-byte packs into
 * dst, up to max bytes. The count; 0 at its end; -1 if the disc could not
 * be read. Called by the player's process. */
LONG dvdplay_read(struct DvdPlay *dp, volatile UBYTE *dst, LONG max);
/* To ms into it (0: its start), or to the next or previous chapter
 * (dir 1, -1); *at: where it is now (ms). FALSE if it could not. Only
 * while dvdplay_read is not being called. */
BOOL dvdplay_seek(struct DvdPlay *dp, ULONG ms, ULONG *at);
BOOL dvdplay_chapter(struct DvdPlay *dp, int dir, ULONG *at);
BOOL dvdplay_can_chapter(struct DvdPlay *dp, int dir);  /* a chapter there (asks nothing of the disc) */
void dvdplay_close(struct DvdPlay *dp);

#endif
