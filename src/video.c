/*
 * AmigaMCE - MPEG video in Videos, played by the ZZ9000
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * The card decodes an MPEG Program Stream and shows it on its overlay: a
 * Picasso96 PIP window (YUV) over AmigaMCE's display, borderless, where the
 * anim player puts its picture (anim.c: anim_play_video), so a video looks
 * and acts as an ANIM does there: its controls beneath, full screen, the
 * same keys. The radio process feeds the card and paces its pictures
 * against the sound, which comes back from the card and plays as music
 * does (stream.c: video_run). Here: the checks before it plays, and the
 * overlay's window: shown where it is asked for, moved with AmigaMCE's
 * window, and taken down before AmigaMCE's window or screen goes.
 *
 * Needs the ZZ9000's firmware with media sessions (zz9k.c's checks) and
 * AmigaMCE's screen (or Workbench, in a window) on the ZZ9000 itself.
 *
 * MPEG-1 decoded by the CPU (stream.c: swv_run) uses the same overlay where
 * it opens, its pictures written into its memory as packed YUV (vo_lock);
 * where it does not, anim.c draws them as RGB.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/var.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <proto/graphics.h>
#include <graphics/modeid.h>
#include <graphics/view.h>
#include <stdio.h>
#include <string.h>
#include "amce.h"
#include "zz9k.h"
#include "vprobe.h"
#include "video.h"

struct Library *P96Base;

static struct Window *vo_win;           /* the overlay's window, NULL none */
static struct Screen *vo_scr;           /* the screen it is on */
static struct VProbe vo_v;              /* the picture it shows */
static LONG vo_sw;                      /* its width for its height, as meant to be seen */
static LONG vo_x, vo_y, vo_w, vo_h;     /* its place, in AmigaMCE's coordinates */
static LONG vo_wx = -1, vo_wy = -1;     /* AmigaMCE's window, where it was last seen */
static ULONG vo_cs, vo_cm;              /* the last click (a double click) */
static BOOL vo_failed;                  /* it could not be opened for this video */
static BOOL vo_off;                     /* the CPU's pictures not to go on it (Settings) */
static ULONG vo_opens;                  /* times opened (a new one's memory is not the last's) */
static struct BitMap *vo_bm;            /* its picture's memory, locked (vo_lock) */
static LONG vo_lk;
static int vo_logged;                   /* (said once a video: its memory, the first picture) */

static const char *pip_error(LONG e)
{
    switch (e) {
    case 1: return "out of memory";
    case 2: return "could not attach to the screen";
    case 3: return "no PIP on this screen (not on the ZZ9000?)";
    case 4: return "no free pen for its colour key";
    case 5: return "its size or format refused";
    case 6: return "its window could not be opened";
    }
    return "?";
}

/* The picture's width for its height, as it is meant to be seen (its
 * aspect code: MPEG-1's pixel shape, MPEG-2's display shape). */
ULONG video_shown_w(const struct VProbe *v)
{
    static const UWORD pel[15] = { 0, 10000, 6735, 7031, 7615, 8055, 8437, 8935,
                                   9375, 9815, 10255, 10695, 10950, 11575, 12015 };
    if (v->mpeg2) {
        if (v->aspect == 2)
            return (ULONG)v->h * 4 / 3;
        if (v->aspect == 3)
            return (ULONG)v->h * 16 / 9;
        if (v->aspect == 4)
            return (ULONG)v->h * 221 / 100;
        return v->w;
    }
    if (v->aspect == 3 || v->aspect == 6)
        return (ULONG)v->h * 16 / 9;        /* (16:9, 625 and 525 lines) */
    if (v->aspect == 8 || v->aspect == 12)
        return (ULONG)v->h * 4 / 3;         /* (CCIR 601: a 4:3 picture) */
    if (v->aspect >= 1 && v->aspect < 15)
        return (ULONG)v->w * 10000 / pel[v->aspect];
    return v->w;
}

/* The overlay: a borderless PIP window on scr (pub: Workbench), the
 * picture fitted into ax, ay, aw x ah with its aspect (sw for its height);
 * *pe Picasso96's error if not. */
/* The picture's place in ax, ay, aw x ah: centred, its aspect kept. */
static void pip_fit(const struct VProbe *v, LONG sw, LONG ax, LONG ay, LONG aw, LONG ah, LONG *x,
                    LONG *y, LONG *dw, LONG *dh)
{
    *dw = aw;
    *dh = (LONG)((ULONG)aw * v->h / (ULONG)sw);
    if (*dh > ah) {
        *dh = ah;
        *dw = (LONG)((ULONG)ah * (ULONG)sw / v->h);
    }
    *dw &= ~1;
    *x = ax + (aw - *dw) / 2;
    *y = ay + (ah - *dh) / 2;
}

/* The overlay's window moved to x, y, dw x dh inside (as a drag does: so
 * Picasso96 moves the overlay with it), waited for (half a second at most);
 * FALSE if it did not get there. */
static BOOL pip_move(struct Window *w, LONG x, LONG y, LONG dw, LONG dh)
{
    LONG ow = dw + w->BorderLeft + w->BorderRight, oh = dh + w->BorderTop + w->BorderBottom, t;
    if (w->Width != ow || w->Height != oh || w->LeftEdge != x || w->TopEdge != y)
        ChangeWindowBox(w, x, y, ow, oh);
    for (t = 0; t < 25; t++) {
        if (w->Width == ow && w->Height == oh && w->LeftEdge == x && w->TopEdge == y)
            break;
        Delay(1);
    }
    Delay(1);                               /* (Picasso96 sets the overlay after Intuition) */
    return t < 25;
}

static struct Window *pip_open(struct Screen *scr, BOOL pub, const struct VProbe *v, LONG ax,
                               LONG ay, LONG aw, LONG ah, LONG sw, LONG *pe)
{
    struct Window *w;
    LONG x, y, dw, dh;
    char m[160];
    pip_fit(v, sw, ax, ay, aw, ah, &x, &y, &dw, &dh);
    *pe = 0;
    sprintf(m, "opening its overlay: %ld x %ld YUV, at %ld, %ld, %ld x %ld%s", (long)v->w, (long)v->h,
            (long)x, (long)y, (long)dw, (long)dh, v->sw ? " (the CPU's pictures)" : "");
    amce_log("video", m);
    w = p96PIP_OpenTags(P96PIP_SourceFormat, RGBFB_YUV422CGX, P96PIP_SourceWidth, (ULONG)v->w,
                        P96PIP_SourceHeight, (ULONG)v->h, P96PIP_Type, P96PIPT_MemoryWindow,
                        P96PIP_ErrorCode, (ULONG)pe, pub ? WA_PubScreen : WA_CustomScreen,
                        (ULONG)scr, WA_Left, x, WA_Top, y,
                        WA_InnerWidth, dw, WA_InnerHeight, dh, WA_Borderless, TRUE,
                        WA_Activate, FALSE, WA_RMBTrap, TRUE, WA_IDCMP, IDCMP_MOUSEBUTTONS,
                        TAG_DONE);
    if (w)
        amce_log("video", "its overlay opened: placing it");
    if (w) {
        /* the size and place asked for at opening are not reliably taken
         * (the window stays the picture's size, top left): set again */
        BOOL there = pip_move(w, x, y, dw, dh);
        sprintf(m, "overlay %ld x %ld at %ld, %ld (from %ld x %ld)%s",
                (long)(w->Width - w->BorderLeft - w->BorderRight),
                (long)(w->Height - w->BorderTop - w->BorderBottom), (long)w->LeftEdge,
                (long)w->TopEdge, (long)v->w, (long)v->h, there ? "" : "; not the size asked for");
    } else
        sprintf(m, "its overlay (PIP) could not be opened: %s (%ld)", pip_error(*pe), (long)*pe);
    amce_log("video", m);
    return w;
}


const char *video_check(const char *path, struct VProbe *v)
{
    static char why[120];
    const char *w;
    char m[200];
    if (!vprobe(path, v, 0)) {
        if (v->h264) {
            sprintf(why, _("%s video: the ZZ9000 plays MPEG-1 and MPEG-2"), vprobe_kind(v));
            sprintf(m, "%s: %s", (const char *)FilePart((STRPTR)path), why);
            amce_log("video", m);
            return why;
        }
        return _("Not an MPEG video AmigaMCE can read");
    }
    sprintf(m, "%s: %s %ld x %ld, aspect %ld, %ld.%03ld fps, %s, %lu ms, %lu bytes%s",
            (const char *)FilePart((STRPTR)path), vprobe_kind(v), (long)v->w, (long)v->h,
            (long)v->aspect, (long)(v->fps_milli / 1000), (long)(v->fps_milli % 1000),
            v->ac3 ? "AC3 sound" : v->mp2 ? "MPEG audio" : "no sound", (unsigned long)v->ms,
            (unsigned long)v->bytes,
            v->ts ? " (a transport stream)" : v->ps ? "" : " (a bare video stream)");
    amce_log("video", m);
    if (v->ts) {
        sprintf(m, "its transport stream: %ld-byte packets from byte %lu; video PID %ld (type "
                   "%02lx), sound PID %ld (type %02lx)%s%s",
                (long)v->ts, (unsigned long)v->ts_first, (long)v->ts_vpid,
                (unsigned long)v->ts_vtype, (long)(v->ts_apid == 0x1FFF ? -1 : v->ts_apid),
                (unsigned long)v->ts_atype, v->a_other ? "; also " : "",
                v->a_other ? v->a_other : "");
        amce_log("video", m);
    }
    if (v->a_other && !v->mp2 && !v->ac3) {
        sprintf(m, "its sound (%s) is not the card's: played without it", v->a_other);
        amce_log("video", m);
    }
    if (v->audio_id) {
        sprintf(m, "its sound: stream %02lx, %s", (unsigned long)v->audio_id,
                v->a_layer ? "" : "no frame header found at its start");
        if (v->a_layer)
            sprintf(m + strlen(m), "MPEG-%s layer %s, %lu Hz, %lu kbit/s, %s",
                    v->a_version == 1 ? "1" : v->a_version == 2 ? "2" : "2.5",
                    v->a_layer == 1 ? "I" : v->a_layer == 2 ? "II" : "III", (unsigned long)v->a_hz,
                    (unsigned long)v->a_kbps, v->a_chans == 1 ? "mono" : "stereo");
        amce_log("video", m);
    }
    if (!v->ps && !v->ts && v->mpeg2)
        return _("Only MPEG Program Streams play (this one has no packs)");
    v->sw = FALSE;
    {
        /* the card if it can (unless ENV:AmigaMCE/VideoDecoder says CPU),
         * else the CPU: MPEG-1 only (mpegv.library) */
        char ev[16];
        BOOL cpu = GetVar((CONST_STRPTR)"AmigaMCE/VideoDecoder", (STRPTR)ev, sizeof(ev),
                          GVF_GLOBAL_ONLY) > 0 && (ev[0] == 'c' || ev[0] == 'C');
        w = NULL;
        if (cpu)
            w = "Settings > Display: MPEG video on the ZZ9000 off";
        else if (!zz_open())
            w = "no zz9k.library";
        else {
            w = zz_video_check(v->mpeg2, v->mp2 && !v->ac3 && (v->ps || v->ts));
            zz_close();
        }
        if (w) {
            if (v->mpeg2 || v->ts) {
                amce_log("video", w);
                if (!cpu && !strcmp(w, "no zz9k.library"))
                    return _("MPEG-2 video plays on a ZZ9000 (zz9k.library not found)");
                amce_strlcpy(why, cpu ? "MPEG-2 video plays on a ZZ9000 (the CPU decodes MPEG-1)" : w,
                             sizeof(why));
                return why;
            }
            sprintf(m, "decoded by the CPU (%s)", w);
            amce_log("video", m);
            if (!st_mpv_open()) {
                amce_log("video", "no mpegv.library (AmigaMCE's Libs, or LIBS:)");
                return _("MPEG-1 by the CPU needs mpegv.library (AmigaMCE's Libs)");
            }
            v->sw = TRUE;
            /* its pictures on the card's overlay if this screen has one
             * (Picasso96's PIP: it scales them), else drawn */
            vo_failed = FALSE;
            if (GetVar((CONST_STRPTR)"AmigaMCE/VideoOverlay", (STRPTR)ev, sizeof(ev),
                       GVF_GLOBAL_ONLY) > 0 && !strcmp(ev, "off")) {
                amce_log("video", "Settings > Display: the overlay off: its pictures drawn");
                vo_off = TRUE;
            } else {
                vo_off = FALSE;
                if (!P96Base)
                    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
            }
            return NULL;
        }
    }
    if (!P96Base && !(P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2)))
        return _("MPEG video needs Picasso96 (Picasso96API.library)");
    vo_failed = FALSE;
    vo_off = FALSE;                         /* (the card's: its overlay is its only way) */                      /* a new video: its overlay tried again */
    return NULL;
}

/* A DVD's film (v from dvdplay_open): MPEG-2, so the card's always, and
 * its overlay made ready as video_check does a file's. NULL if it plays,
 * else why not. */
const char *video_check_dvd(const struct VProbe *v)
{
    static char why[120];
    const char *w;
    if (!zz_open())
        return _("MPEG-2 video plays on a ZZ9000 (zz9k.library not found)");
    w = zz_video_check(TRUE, v->mp2 && !v->ac3);
    zz_close();
    if (w) {
        amce_log("video", w);
        amce_strlcpy(why, w, sizeof(why));
        return why;
    }
    if (!P96Base && !(P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2)))
        return _("MPEG video needs Picasso96 (Picasso96API.library)");
    vo_failed = FALSE;                      /* a new video: its overlay tried again */
    return NULL;
}

/* Where AmigaMCE's coordinates are on the screen. */
static void vo_origin(struct Gfx *g, LONG *ox, LONG *oy)
{
    *ox = g->win ? g->win->LeftEdge + g->win->BorderLeft : 0;
    *oy = g->win ? g->win->TopEdge + g->win->BorderTop : 0;
}

BOOL vo_show(struct Gfx *g, const struct VProbe *v, LONG x, LONG y, LONG w, LONG h)
{
    LONG ox, oy, px, py, dw, dh, pe = 0;
    char m[160];
    if (!g->scr || !g->win || w < 16 || h < 16)
        return FALSE;
    if (vo_win && (vo_scr != g->scr || v->w != vo_v.w || v->h != vo_v.h))
        vo_hide();                          /* another screen, or another picture's size */
    vo_origin(g, &ox, &oy);
    if (vo_win) {
        if (x == vo_x && y == vo_y && w == vo_w && h == vo_h && ox == vo_wx && oy == vo_wy)
            return TRUE;
        vo_x = x;
        vo_y = y;
        vo_w = w;
        vo_h = h;
        vo_wx = ox;
        vo_wy = oy;
        pip_fit(&vo_v, vo_sw, ox + x, oy + y, w, h, &px, &py, &dw, &dh);
        WindowToFront(vo_win);
        pip_move(vo_win, px, py, dw, dh);
        return TRUE;
    }
    if (vo_failed || !P96Base || (v->sw && vo_off))
        return FALSE;
    vo_v = *v;
    vo_sw = (LONG)video_shown_w(v);
    if (vo_sw <= 0)
        vo_sw = v->w;
    if (g->wb && g->scr->ViewPort.ColorMap && g->scr->ViewPort.ColorMap->PalExtra) {
        sprintf(m, "its overlay in AmigaMCE's window (Workbench's free pens: %ld)",
                (long)g->scr->ViewPort.ColorMap->PalExtra->pe_NFree);
        amce_log("video", m);
    }
    vo_win = pip_open(g->scr, g->wb, &vo_v, ox + x, oy + y, w, h, vo_sw, &pe);
    if (!vo_win) {
        vo_failed = TRUE;                   /* (said once: not tried at every tick) */
        return FALSE;
    }
    vo_scr = g->scr;
    vo_opens++;
    vo_logged = 0;
    vo_x = x;
    vo_y = y;
    vo_w = w;
    vo_h = h;
    vo_wx = ox;
    vo_wy = oy;
    if (g->win)
        ActivateWindow(g->win);             /* the keys stay AmigaMCE's */
    return TRUE;
}

BOOL vo_failed_here(void)
{
    return vo_failed;
}

void vo_retry(void)
{
    vo_failed = FALSE;
}

void vo_hide(void)
{
    vo_unlock();
    if (vo_win) {
        p96PIP_Close(vo_win);
        amce_log("video", "its overlay taken down");
    }
    vo_win = NULL;
    vo_scr = NULL;
    vo_wx = vo_wy = -1;
}

BOOL vo_shown(void)
{
    return vo_win != NULL;
}

ULONG vo_opened(void)
{
    return vo_opens;
}

UBYTE *vo_lock(ULONG *bpr)
{
    struct RenderInfo ri;
    struct BitMap *bm = NULL;
    if (!vo_win || vo_bm)
        return NULL;
    p96PIP_GetTags(vo_win, P96PIP_SourceBitMap, (ULONG)&bm, TAG_DONE);
    if (!bm)
        return NULL;
    memset(&ri, 0, sizeof(ri));
    vo_lk = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));
    if (vo_logged < 1) {
        char m[120];
        sprintf(m, "its overlay's memory: %08lx, %ld bytes a row, format %ld", (unsigned long)ri.Memory,
                (long)ri.BytesPerRow, (long)ri.RGBFormat);
        amce_log("video", m);
        vo_logged = 1;
    }
    if (!ri.Memory || ri.BytesPerRow <= 0) {
        p96UnlockBitMap(bm, vo_lk);
        return NULL;
    }
    vo_bm = bm;
    *bpr = (ULONG)ri.BytesPerRow;
    return (UBYTE *)ri.Memory;
}

void vo_unlock(void)
{
    if (vo_bm)
        p96UnlockBitMap(vo_bm, vo_lk);
    if (vo_bm && vo_logged < 2) {
        amce_log("video", "its first picture written to the overlay");
        vo_logged = 2;
    }
    vo_bm = NULL;
}

void vo_end(void)
{
    vo_hide();
    if (P96Base)
        CloseLibrary(P96Base);
    P96Base = NULL;
}

ULONG vo_sigmask(void)
{
    return vo_win ? 1UL << vo_win->UserPort->mp_SigBit : 0;
}

int vo_poll(struct Gfx *g)
{
    struct IntuiMessage *im;
    int r = VO_NONE;
    if (!vo_win)
        return r;
    while ((im = (struct IntuiMessage *)GetMsg(vo_win->UserPort))) {
        ULONG cl = im->Class, s = im->Seconds, mi = im->Micros;
        UWORD code = im->Code;
        ReplyMsg((struct Message *)im);
        if (cl == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
            r = DoubleClick(vo_cs, vo_cm, s, mi) ? VO_DCLICK : VO_CLICK;
            vo_cs = r == VO_DCLICK ? 0 : s;
            vo_cm = mi;
        } else if (cl == IDCMP_MOUSEBUTTONS && code == MENUDOWN) {
            r = VO_RIGHT;
        }
    }
    if (r != VO_NONE && g->win)
        ActivateWindow(g->win);             /* (clicked, it took the keys: given back) */
    return r;
}
