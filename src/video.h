/*
 * AmigaMCE - MPEG video, played by the ZZ9000 (video.c)
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef AMCE_VIDEO_H
#define AMCE_VIDEO_H

#include <exec/types.h>

struct Gfx;
struct VProbe;

/* Whether path plays here (v: what its headers say): NULL if it can, else
 * why not (for the footer). */
const char *video_check(const char *path, struct VProbe *v);
const char *video_check_dvd(const struct VProbe *v);   /* a DVD's film (dvdplay.c's v) */
ULONG video_shown_w(const struct VProbe *v);    /* its width for its height, as meant to be seen */
/* The overlay at x, y, w x h of AmigaMCE's display (the picture fitted in
 * it): opened, or moved there. FALSE if it cannot be shown (logged; not
 * tried again for this video). */
BOOL vo_show(struct Gfx *g, const struct VProbe *v, LONG x, LONG y, LONG w, LONG h);
BOOL vo_failed_here(void);
void vo_retry(void);                    /* tried again (another screen) */
void vo_hide(void);                     /* taken down (before AmigaMCE's window goes, too) */
BOOL vo_shown(void);
ULONG vo_opened(void);                  /* times opened: a new one's picture is blank */
/* Its picture's memory (YUV422CGX: Y0 Cb Y1 Cr) and bytes a row, locked
 * until vo_unlock (briefly: the screen's drawing waits); NULL if not. */
UBYTE *vo_lock(ULONG *bpr);
void vo_unlock(void);
void vo_end(void);                      /* at quitting */
ULONG vo_sigmask(void);
enum { VO_NONE, VO_CLICK, VO_DCLICK, VO_RIGHT };
int vo_poll(struct Gfx *g);             /* the overlay clicked (the keys given back) */

#endif
