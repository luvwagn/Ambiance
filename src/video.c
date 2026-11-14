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

int video_play(struct Gfx *g, const char *path, UWORD cpu, BOOL (*vol)(int d),
               char *err, LONG errmax)
{
    struct VProbe v;
    struct Window *pip = NULL;
    const char *why;
    LONG pe = 0, ax, ay, aw, ah, dw, dh, sw;
    BOOL paused = FALSE, stop = FALSE;
    ULONG t0, shown = 0, dropped = 0;
    int r = -1;
    char m[200];
    err[0] = 0;
    if (!vprobe(path, &v)) {
        amce_strlcpy(err, _("Not an MPEG video AmigaMCE can read"), errmax);
        return -1;
    }
    sprintf(m, "%s: %s %ld x %ld, aspect %ld, %ld.%03ld fps, %s, %lu ms, %lu bytes%s",
            (const char *)FilePart((STRPTR)path), vprobe_kind(&v), (long)v.w, (long)v.h,
            (long)v.aspect, (long)(v.fps_milli / 1000), (long)(v.fps_milli % 1000),
            v.mp2 ? "MPEG audio" : "no sound", (unsigned long)v.ms, (unsigned long)v.bytes,
            v.ps ? "" : " (not a Program Stream)");
    amce_log("video", m);
    if (!v.ps) {
        amce_strlcpy(err, _("Only MPEG Program Streams play (this one has no packs)"), errmax);
        return -1;
    }
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
    dw = aw;
    dh = (LONG)((ULONG)aw * v.h / (ULONG)sw);
    if (dh > ah) {
        dh = ah;
        dw = (LONG)((ULONG)ah * (ULONG)sw / v.h);
    }
    dw &= ~1;
    gfx_fill(g, 0, 0, g->w - 1, g->h - 1, PEN_BG);     /* (around it) */
    gfx_present(g);
    pip = p96PIP_OpenTags(P96PIP_SourceFormat, RGBFB_YUV422CGX, P96PIP_SourceWidth, (ULONG)v.w,
                          P96PIP_SourceHeight, (ULONG)v.h, P96PIP_Type, P96PIPT_MemoryWindow,
                          P96PIP_ErrorCode, (ULONG)&pe,
                          g->wb ? WA_PubScreen : WA_CustomScreen, (ULONG)g->scr,
                          WA_Left, ax + (aw - dw) / 2, WA_Top, ay + (ah - dh) / 2,
                          WA_InnerWidth, dw, WA_InnerHeight, dh, WA_Borderless, TRUE,
                          WA_Activate, TRUE, WA_RMBTrap, TRUE,
                          WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS,
                          TAG_DONE);
    if (!pip) {
        sprintf(m, "its overlay (PIP) could not be opened: %s (%ld)", pip_error(pe), (long)pe);
        amce_log("video", m);
        amce_strlcpy(err, _("The ZZ9000's overlay could not be opened on this screen"), errmax);
        goto out;
    }
    sprintf(m, "overlay %ld x %ld at %ld, %ld (from %ld x %ld) on %s", (long)dw, (long)dh,
            (long)(ax + (aw - dw) / 2), (long)(ay + (ah - dh) / 2), (long)v.w, (long)v.h,
            g->wb ? "Workbench" : "AmigaMCE's screen");
    amce_log("video", m);
    if (!st_start_video(path, (UWORD)mp_volume(), cpu, v.w, v.h, v.mpeg2, v.mp2, v.ms, err,
                        errmax))
        goto out;
    t0 = time_ms();
    while (!stop) {
        struct IntuiMessage *im;
        int s = st_state();
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
    if (g->wb && g->win)
        ActivateWindow(g->win);
    CloseLibrary(P96Base);
    P96Base = NULL;
    zz_close();
    return r;
}
