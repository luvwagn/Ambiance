/*
 * AmigaMCE - MPEG video in Videos, played by the ZZ9000
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * The card decodes an MPEG Program Stream and shows it on its overlay: a
 * Picasso96 PIP window (YUV) over AmigaMCE's own display, fitted to it
 * with the picture's aspect, borderless. The radio process feeds the card
 * and paces its pictures against the sound, which comes back from the card
 * and plays as music does (stream.c: video_run). Here: the checks, the
 * window, and the keys while it plays (Esc, Backspace, the right button
 * or Q stop; Space pauses; +/- or Up/Down the volume; M mutes).
 *
 * Needs the ZZ9000's firmware with media sessions (zz9k.c's checks) and
 * AmigaMCE's screen (or Workbench, in a window) on the ZZ9000 itself.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <proto/graphics.h>
#include <graphics/modeid.h>
#include <graphics/view.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <stdio.h>
#include <string.h>
#include "amce.h"
#include "zz9k.h"
#include "vprobe.h"
#include "video.h"

struct Library *P96Base;

#define VOL_STEP 4                      /* (Now playing's) */

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
static ULONG shown_w(const struct VProbe *v)
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
    w = p96PIP_OpenTags(P96PIP_SourceFormat, RGBFB_YUV422CGX, P96PIP_SourceWidth, (ULONG)v->w,
                        P96PIP_SourceHeight, (ULONG)v->h, P96PIP_Type, P96PIPT_MemoryWindow,
                        P96PIP_ErrorCode, (ULONG)pe, pub ? WA_PubScreen : WA_CustomScreen,
                        (ULONG)scr, WA_Left, x, WA_Top, y,
                        WA_InnerWidth, dw, WA_InnerHeight, dh, WA_Borderless, TRUE,
                        WA_Activate, TRUE, WA_RMBTrap, TRUE,
                        WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS, TAG_DONE);
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

/* A screen of its own for it, in mode's own size and depth (on the
 * ZZ9000: Workbench's, or AmigaMCE's), its pens left free (the overlay's
 * colour key needs one), black. */
static UWORD vs_pens[] = { (UWORD)~0 };
static ULONG vs_black[] = { 1UL << 16, 0, 0, 0, 0 };

static struct Screen *own_screen(ULONG mode, ULONG depth)
{
    return p96OpenScreenTags(P96SA_DisplayID, mode, P96SA_Depth, depth, P96SA_Title,
                             (ULONG) "AmigaMCE", P96SA_ShowTitle, FALSE, P96SA_Quiet, TRUE,
                             P96SA_AutoScroll, FALSE, P96SA_SharePens, TRUE, P96SA_Pens,
                             (ULONG)vs_pens, P96SA_Colors32, (ULONG)vs_black, TAG_DONE);
}

int video_play(struct Gfx *g, const char *path, UWORD cpu, BOOL (*vol)(int d),
               char *err, LONG errmax)
{
    struct Screen *vscr = NULL;
    struct VProbe v;
    struct Window *pip = NULL;
    const char *why;
    LONG pe = 0, ax, ay, aw, ah, sw;
    BOOL paused = FALSE, stop = FALSE;
    ULONG t0, shown = 0, dropped = 0;
    LONG wx = 0, wy = 0, ww = 0, wh = 0;     /* AmigaMCE's window, as last seen */
    int r = -1;
    char m[200];
    err[0] = 0;
    if (!vprobe(path, &v, 0)) {
        amce_strlcpy(err, _("Not an MPEG video AmigaMCE can read"), errmax);
        return -1;
    }
    sprintf(m, "%s: %s %ld x %ld, aspect %ld, %ld.%03ld fps, %s, %lu ms, %lu bytes%s",
            (const char *)FilePart((STRPTR)path), vprobe_kind(&v), (long)v.w, (long)v.h,
            (long)v.aspect, (long)(v.fps_milli / 1000), (long)(v.fps_milli % 1000),
            v.mp2 ? "MPEG audio" : "no sound", (unsigned long)v.ms, (unsigned long)v.bytes,
            v.ps ? "" : " (a bare video stream)");
    amce_log("video", m);
    if (v.audio_id) {
        sprintf(m, "its sound: stream %02lx, %s", (unsigned long)v.audio_id,
                v.a_layer ? "" : "no frame header found at its start");
        if (v.a_layer)
            sprintf(m + strlen(m), "MPEG-%s layer %s, %lu Hz, %lu kbit/s, %s",
                    v.a_version == 1 ? "1" : v.a_version == 2 ? "2" : "2.5",
                    v.a_layer == 1 ? "I" : v.a_layer == 2 ? "II" : "III", (unsigned long)v.a_hz,
                    (unsigned long)v.a_kbps, v.a_chans == 1 ? "mono" : "stereo");
        amce_log("video", m);
    }
    if (!v.ps && v.mpeg2) {
        amce_strlcpy(err, _("Only MPEG Program Streams play (this one has no packs)"), errmax);
        return -1;
    }                                       /* (bare MPEG-1 video: packed as it plays) */
    if (!zz_open()) {
        amce_strlcpy(err, _("MPEG video plays on a ZZ9000 (zz9k.library not found)"), errmax);
        return -1;
    }
    if ((why = zz_video_check(v.mpeg2, v.mp2))) {
        amce_log("video", why);
        amce_strlcpy(err, why, errmax);
        zz_close();
        return -1;
    }
    if (!(P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2))) {
        amce_strlcpy(err, _("MPEG video needs Picasso96 (Picasso96API.library)"), errmax);
        zz_close();
        return -1;
    }
    /* where it goes: AmigaMCE's screen, or its window's inside */
    if (g->wb && g->win) {
        ax = g->win->LeftEdge + g->win->BorderLeft;
        ay = g->win->TopEdge + g->win->BorderTop;
    } else {
        ax = ay = 0;
    }
    aw = g->w;
    ah = g->h;
    sw = (LONG)shown_w(&v);
    if (sw <= 0)
        sw = v.w;
    gfx_fill(g, 0, 0, g->w - 1, g->h - 1, PEN_BG);     /* (around it) */
    gfx_present(g);
    if (g->wb && g->scr->ViewPort.ColorMap && g->scr->ViewPort.ColorMap->PalExtra) {
        sprintf(m, "its overlay over AmigaMCE's window (Workbench's free pens: %ld)",
                (long)g->scr->ViewPort.ColorMap->PalExtra->pe_NFree);
        amce_log("video", m);
    } else
        amce_log("video", g->wb ? "its overlay over AmigaMCE's window" : "its overlay on AmigaMCE's screen");
    pip = pip_open(g->scr, g->wb, &v, ax, ay, aw, ah, sw, &pe);
    if (!pip) {
        /* (all the screen's pens taken, say: Workbench's, by AmigaMCE's window
         * among others) on a screen of its own, in the same mode */
        ULONG mode = GetVPModeID(&g->scr->ViewPort);
        ULONG depth = GetBitMapAttr(g->scr->RastPort.BitMap, BMA_DEPTH);
        if (depth < 15)
            depth = 16;
        if (mode != (ULONG)INVALID_ID && (vscr = own_screen(mode, depth))) {
            sprintf(m, "on a screen of its own: mode %08lx, %ld x %ld, %ld-bit",
                    (unsigned long)mode, (long)vscr->Width, (long)vscr->Height, (long)depth);
            amce_log("video", m);
            pip = pip_open(vscr, FALSE, &v, 0, 0, vscr->Width, vscr->Height, sw, &pe);
        } else {
            sprintf(m, "a screen of its own could not be opened (mode %08lx)", (unsigned long)mode);
            amce_log("video", m);
        }
    }
    if (!pip) {
        amce_strlcpy(err, _("The ZZ9000's overlay could not be opened on this screen"), errmax);
        goto out;
    }
    if (!st_start_video(path, (UWORD)mp_volume(), cpu, v.w, v.h, v.mpeg2, v.mp2 && v.ps, !v.ps,
                        v.ms, err, errmax))
        goto out;
    t0 = time_ms();
    if (!vscr && g->wb && g->win) {
        wx = g->win->LeftEdge;
        wy = g->win->TopEdge;
        ww = g->win->Width;
        wh = g->win->Height;
    }
    while (!stop) {
        struct IntuiMessage *im;
        int s = st_state();
        if (!vscr && g->wb && g->win &&
            (g->win->LeftEdge != wx || g->win->TopEdge != wy || g->win->Width != ww ||
             g->win->Height != wh)) {
            /* AmigaMCE's window moved (or sized): the overlay after it */
            LONG x, y, dw, dh;
            wx = g->win->LeftEdge;
            wy = g->win->TopEdge;
            ww = g->win->Width;
            wh = g->win->Height;
            pip_fit(&v, sw, wx + g->win->BorderLeft, wy + g->win->BorderTop,
                    ww - g->win->BorderLeft - g->win->BorderRight,
                    wh - g->win->BorderTop - g->win->BorderBottom, &x, &y, &dw, &dh);
            WindowToFront(pip);
            pip_move(pip, x, y, dw, dh);
        }
        if (!vscr && g->wb && g->win && g->win->WLayer &&
            (g->win->WLayer->Flags & LAYERREFRESH)) {
            /* AmigaMCE's window uncovered (moved, or the overlay moved off
             * it): the border around the picture drawn again */
            gfx_window_msg(g, IDCMP_REFRESHWINDOW, 0);
        }
        if (s == ST_ENDED || s == ST_FAILED) {
            if (s == ST_FAILED && st_error()[0])
                amce_strlcpy(err, st_error(), errmax);
            break;
        }
        while ((im = (struct IntuiMessage *)GetMsg(pip->UserPort))) {
            ULONG cl = im->Class;
            UWORD code = im->Code;
            ReplyMsg((struct Message *)im);
            if (cl == IDCMP_MOUSEBUTTONS && code == MENUDOWN) {
                stop = TRUE;
            } else if (cl == IDCMP_VANILLAKEY) {
                switch (code) {
                case 27: case 8: case 'q': case 'Q': stop = TRUE; break;
                case ' ': case 'p': case 'P':
                    paused = !paused;
                    st_pause(paused);
                    amce_log("video", paused ? "paused" : "on again");
                    break;
                case '+': case '=': vol(VOL_STEP); break;
                case '-': case '_': vol(-VOL_STEP); break;
                case 'm': case 'M': vol(0); break;
                }
            } else if (cl == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX)) {
                if (code == 0x4C)
                    vol(VOL_STEP);
                else if (code == 0x4D)
                    vol(-VOL_STEP);
                else if (code == 0x45)
                    stop = TRUE;
            }
        }
        Delay(2);
    }
    if (stop)
        amce_log("video", "stopped");
    st_video_counts(&shown, &dropped);
    st_stop();
    sprintf(m, "%lu pictures shown, %lu let go, in %lu ms", (unsigned long)shown,
            (unsigned long)dropped, (unsigned long)(time_ms() - t0));
    amce_log("video", m);
    r = err[0] ? -1 : 0;
out:
    if (pip)
        p96PIP_Close(pip);
    if (vscr)
        p96CloseScreen(vscr);
    if (g->wb && g->win)
        ActivateWindow(g->win);
    CloseLibrary(P96Base);
    P96Base = NULL;
    zz_close();
    return r;
}
