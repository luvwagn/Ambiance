/*
 * Ambiance - DVD-Video discs: found, and their main film played (dvdplay.c)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Through dvdnav.library (extras/dvdnav), opened only while a disc is
 * played. Its sectors are read from the drive (dvddisc.c), not through the
 * volume's filesystem, which only says a DVD is there; an image's from its
 * file.
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

BOOL dvd_lib_there(void);       /* dvdnav.library in PROGDIR:Libs or LIBS: */

/* ---- the main film of one */
struct DvdPlay;

/* The disc in volume vol (its name, with or without the colon): its film,
 * or the first of its programmes, started; v filled for the player (an
 * MPEG-2 Program Stream: size, aspect, length, sound). NULL, with why in
 * err. dvdplay_name: "VOLUME", or "VOLUME: 3 / 14" (changing as it goes). */
struct DvdPlay *dvdplay_open(const char *vol, struct VProbe *v, char *err, LONG errlen);
/* The same, from an image file of a disc (a .iso; named as its file is,
 * without the .iso) */
struct DvdPlay *dvdplay_open_image(const char *path, struct VProbe *v, char *err, LONG errlen);
const char *dvdplay_name(struct DvdPlay *dp);   /* "MOVIE: title 3" */
/* The film from where it is, for the card: whole 2048-byte packs into
 * dst, up to max bytes. The count; 0 at its end; -1 if the disc could not
 * be read. Called by the player's process. */
LONG dvdplay_read(struct DvdPlay *dp, volatile UBYTE *dst, LONG max);
/* To ms into the title (0: its start), or to the next or previous chapter
 * or programme (dir 1, -1; see dvdplay.c); *at: where it is now (ms). FALSE if it could not. Only
 * while dvdplay_read is not being called. */
BOOL dvdplay_seek(struct DvdPlay *dp, ULONG ms, ULONG *at);
BOOL dvdplay_chapter(struct DvdPlay *dp, int dir, ULONG *at);
/* Where it is, for its information: the title, which programme of how
 * many, the chapter of how many, its sound track (0 none) of how many,
 * its channels, AC-3 or not, its language. */
struct DvdWhere {
    LONG title, programme, programmes, chapter, chapters, a_track, a_tracks;
    UBYTE a_ch;
    BOOL a_ac3;
    char a_lang[4];
};
void dvdplay_where(struct DvdPlay *dp, struct DvdWhere *w);
BOOL dvdplay_can_chapter(struct DvdPlay *dp, int dir);  /* somewhere to go (asks nothing of the disc) */
BOOL dvdplay_next_title(struct DvdPlay *dp);    /* another programme after this one */
void dvdplay_probe(struct DvdPlay *dp, struct VProbe *v);  /* v: the title now (size, length) */
void dvdplay_close(struct DvdPlay *dp);
/* ---- the disc's own menus: from its first play, as a DVD player starts
 * one, for the CPU's decoding (Ambiance draws their buttons). The menus
 * it has (dvdnav.h's DVDMB_ bits; 0: none, it only plays). */
ULONG dvdplay_menus(struct DvdPlay *dp);
/* From the disc's start, its own navigation from now on: FALSE if not. */
BOOL dvdplay_nav(struct DvdPlay *dp);
BOOL dvdplay_is_nav(struct DvdPlay *dp);

/* What dvdplay_read stopped for, the disc's navigation (it gives nothing
 * more until it is dealt with): a jump (flush what is decoded; user: the
 * viewer's, at once, else once what came before it is shown), a still
 * (*still_s seconds, 255 till a button), a wait (what was given shown
 * first), the end. dvdplay_go on after it (a still's or wait's end told
 * to the disc). For the playback process. */
enum { DVDP_NONE, DVDP_HOP, DVDP_STILL, DVDP_WAIT, DVDP_END };
int dvdplay_pending(struct DvdPlay *dp, LONG *still_s, BOOL *user);
void dvdplay_go(struct DvdPlay *dp);
/* During a still: what the UI asked carried out, the disc's events read
 * (a button changed, a jump: then pending). For the playback process. */
void dvdplay_poll(struct DvdPlay *dp);
/* What the UI asked (dvdplay_ask) carried out: by the playback process,
 * each time round. */
void dvdplay_service(struct DvdPlay *dp);

/* Asked by the UI, done by the playback process: a menu's button moved to
 * or pressed, a place in the picture pointed at or clicked (a, b: x, y in
 * the video's pixels), its menu (from a title; from a menu, back), a seek
 * (a: ms), a chapter (a: 1 next, -1 previous). */
enum { DVDQ_UP = 1, DVDQ_DOWN, DVDQ_LEFT, DVDQ_RIGHT, DVDQ_ACTIVATE, DVDQ_POINT, DVDQ_CLICK,
       DVDQ_MENU, DVDQ_SEEK, DVDQ_CHAPTER };
void dvdplay_ask(struct DvdPlay *dp, int what, LONG a, LONG b);

/* Where it is, for the UI: in a menu or not, a menu's buttons there now,
 * the title's length and the time in it (ms), what the disc forbids now
 * (DVDU_ bits). */
struct DvdNow {
    BOOL menu, buttons;
    ULONG title_ms, at_ms, forbidden;
    LONG title;
};
void dvdplay_now(struct DvdPlay *dp, struct DvdNow *n);

/* For drawing its subpictures: the stream to decode now (0-31; -1 none)
 * and whether only its forced ones show; the colour table and when it
 * last changed; the highlighted button and when it last changed. */
struct SpuHl;
LONG dvdplay_spu(struct DvdPlay *dp, BOOL *forced_only);
const ULONG *dvdplay_clut(struct DvdPlay *dp, ULONG *seq);
ULONG dvdplay_highlight(struct DvdPlay *dp, struct SpuHl *hl);

/* (what plays, from its titles' lengths: dvdplay.c; for the tests) */
LONG dvdplay_choose(const ULONG *lens, LONG nt, UBYTE *list, ULONG *out);
struct DvdStreamInfo;
LONG dvdplay_pick_audio(const struct DvdStreamInfo *si, LONG n, LONG deflt);

#endif
