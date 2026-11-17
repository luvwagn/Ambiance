/*
 * Ambiance - AmigaAMP's visual plug-ins, hosted: Ambiance answers on the
 * port they look for, "AmigaAMP plugin port", as AmigaAMP 2.7 and later
 * do (its Plugin Development Kit 1.4, Thomas Wenzel), so the plug-ins
 * written for it show what Ambiance plays. No code of the kit's is used:
 * only its messages' layout, which is what the plug-ins expect.
 * Copyright (c) 2026 Brian King
 *
 * A plug-in is a program of its own. It sends a PluginMessage to the
 * port, its task and its signals, and where to put the pointers to the
 * data; the host fills in the pointers, says whether it is accepted (one
 * at a time), and replies. From then on, each frame, the host signals the
 * plug-in's PluginMask: the data are new.
 *   SpecRawL[512], SpecRawR[512]  each channel's spectrum, lowest first,
 *                                 logarithmic: 0 below -96 dB, 65535 0 dB
 *   SampleRaw[1024]               the sound: 512 frames, 16-bit, left and
 *                                 right interleaved
 *   TrackInfo                     what plays (its InfoMask when it changes)
 * It says goodbye with the same message, its mask 0, and waits for the
 * reply. A plug-in is asked to go with Ctrl-C.
 */
#include "amce.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define AMP_PORT "AmigaAMP plugin port"
#define NFFT     1024           /* points: 512 stereo frames as one complex FFT */

struct AmpTrackInfo {           /* TrackInfo, as the kit has it */
    UBYTE private_[4];
    ULONG StreamSize, HeaderSize, Length, Frequency;
    UBYTE Layer, Version, CRCs, Emphasis, Private, Copyright, Original, Mode;
    UWORD Bitrate, Channels;
    char *TrackInfoText, *ID3title, *ID3artist, *ID3album, *ID3year, *ID3comment, *ID3genre;
    ULONG Position;
    UWORD TrackNumber, DriveMode, Hardware;
};

struct AmpPluginMessage {       /* PluginMessage */
    struct Message msg;
    ULONG PluginMask;
    struct Task *PluginTask;
    UWORD **SpecRawL, **SpecRawR;
    WORD Accepted, reserved0;
    /* 1.2 */
    ULONG InfoMask;
    struct AmpTrackInfo **tinfo;
    struct MsgPort *PluginWP;
    /* 1.3 */
    WORD **SampleRaw;
};
#define V12_LEN (offsetof(struct AmpPluginMessage, InfoMask) + 12)
#define V13_LEN (sizeof(struct AmpPluginMessage))

static struct {
    struct MsgPort *port;       /* ours, public (NULL: not ours: AmigaAMP itself is running) */
    struct Task *task;          /* the plug-in attached (NULL: none) */
    ULONG mask, info_mask;
    BOOL ours;                  /* started by Ambiance (amp_launch) */
    ULONG launched;             /* ...when (it attaches within 15 s, or it is not ours) */
    ULONG serial;               /* what plays, as last told */
    ULONG frames;
    UWORD spec_l[512], spec_r[512];
    WORD samples[1024];
    struct AmpTrackInfo ti;
    char text[200], title[120], artist[120], album[120];
    LONG re[NFFT], im[NFFT];
    WORD sine[1024];
    BOOL tables;
    char name[64];              /* the plug-in's (the log) */
} ah_;

/* ------------------------------------------------------------ the spectrum */

/* re/im (NFFT points) transformed in place, each stage halved (the
 * result is the transform over NFFT) */
static void fft(void)
{
    LONG *re = ah_.re, *im = ah_.im;
    int i, j, k, m, len;
    for (i = 1, j = 0; i < NFFT; i++) {
        int bit = NFFT >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            LONG t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (len = 2; len <= NFFT; len <<= 1) {
        int half = len >> 1, step = 1024 / len;
        for (i = 0; i < NFFT; i += len) {
            for (k = 0, m = 0; k < half; k++, m += step) {
                LONG wr = ah_.sine[(m + 256) & 1023], wi = -ah_.sine[m];   /* e^(-i 2 pi k / len), 16383 = 1 */
                LONG *ar = re + i + k, *ai = im + i + k, *br = ar + half, *bi = ai + half;
                LONG tr = (*br * wr - *bi * wi) >> 14, ti = (*br * wi + *bi * wr) >> 14;
                *br = (*ar - tr) >> 1;
                *bi = (*ai - ti) >> 1;
                *ar = (*ar + tr) >> 1;
                *ai = (*ai + ti) >> 1;
            }
        }
    }
}

/* a squared size as the kit's scale: 0 at -96 dB, 65535 at 0 dB (a full
 * scale sine's bin) */
static UWORD db_scale(ULONG m)
{
    static const UBYTE frac[16] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15 };
    int b = 31;
    LONG l16, v;
    if (!m)
        return 0;
    while (!(m & 0x80000000UL)) {
        m <<= 1;
        b--;
    }
    l16 = b * 16 + frac[(m >> 27) & 15];        /* log2, sixteenths */
    /* 10 log10 = 3.0103 log2; a full scale sine's bin here is about 2^24
     * (its amplitude, halved by the window and again as a real sine's
     * two bins, 512 frames, the transform over 1024) */
    v = (l16 - 24 * 16) * 30103 / 160000 + 96;  /* dB above -96 */
    if (v <= 0)
        return 0;
    if (v >= 96)
        return 65535;
    return (UWORD)(v * 65535 / 96);
}

/* the samples' spectra, left as the real part and right as the
 * imaginary, one transform, then each channel's taken out of it */
static void spectra(void)
{
    int n, k;
    if (!ah_.tables) {
        vis_tables(ah_.sine);
        ah_.tables = TRUE;
    }
    for (n = 0; n < NFFT / 2; n++) {
        /* Hann: 0.5 - 0.5 cos, 16383ths; NFFT/2 frames, so twice round
         * the table in NFFT/2 steps */
        LONG w = (16383 - ah_.sine[(n * 2 + 256) & 1023]) >> 1;
        ah_.re[n] = (LONG)ah_.samples[2 * n] * w >> 14;
        ah_.im[n] = (LONG)ah_.samples[2 * n + 1] * w >> 14;
    }
    for (; n < NFFT; n++)
        ah_.re[n] = ah_.im[n] = 0;                  /* (padded: 512 frames in 1024 points) */
    fft();
    for (k = 0; k < 512; k++) {
        int j = k ? NFFT - k : 0;
        LONG a = ah_.re[k], b = ah_.im[k], c = ah_.re[j], d = ah_.im[j];
        LONG lr = (a + c) >> 1, li = (b - d) >> 1, rr = (b + d) >> 1, ri = (c - a) >> 1;
        ah_.spec_l[k] = db_scale((ULONG)(lr * lr) + (ULONG)(li * li));
        ah_.spec_r[k] = db_scale((ULONG)(rr * rr) + (ULONG)(ri * ri));
    }
}

/* ------------------------------------------------------------ what plays */

static void track_info(void)
{
    struct StInfo in;
    const char *t = mp_is_stream() ? (st_title()[0] ? st_title() : mp_stream_name()) : mp_title();
    const char *by = mp_is_stream() ? mp_stream_name() : mp_track_by();
    memset(&ah_.ti, 0, sizeof(ah_.ti));
    amce_strlcpy(ah_.title, t ? t : "", sizeof(ah_.title));
    amce_strlcpy(ah_.artist, by && (!t || strcmp(by, t)) ? by : "", sizeof(ah_.artist));
    ah_.album[0] = 0;
    if (ah_.artist[0])
        sprintf(ah_.text, "%.90s - %.90s", ah_.artist, ah_.title);
    else
        amce_strlcpy(ah_.text, ah_.title, sizeof(ah_.text));
    ah_.ti.TrackInfoText = ah_.text;
    ah_.ti.ID3title = ah_.title;
    ah_.ti.ID3artist = ah_.artist;
    ah_.ti.ID3album = ah_.album;
    ah_.ti.ID3year = ah_.ti.ID3comment = ah_.ti.ID3genre = ah_.album;
    if (mp_is_stream()) {
        st_info(&in);
        ah_.ti.Frequency = (ULONG)in.freq;
        ah_.ti.Bitrate = (UWORD)in.bitrate;
        ah_.ti.Channels = (UWORD)in.channels;
        ah_.ti.Layer = 3;
        ah_.ti.Version = 1;
    } else {
        ah_.ti.Channels = 2;
    }
    ah_.ti.Length = 0;
    ah_.ti.DriveMode = !mp_running() ? 0 : mp_paused() ? 3 : 1;
    ah_.serial = mp_serial();
}

/* ------------------------------------------------------------ the port */

BOOL amp_host_start(void)
{
    if (ah_.port)
        return TRUE;
    Forbid();
    if (FindPort((CONST_STRPTR)AMP_PORT)) {
        Permit();
        amce_log("visualisation", "AmigaAMP's plug-in port is AmigaAMP's own here: not hosted");
        return FALSE;
    }
    if ((ah_.port = CreateMsgPort())) {
        ah_.port->mp_Node.ln_Name = (char *)AMP_PORT;
        ah_.port->mp_Node.ln_Pri = 0;
        AddPort(ah_.port);
    }
    Permit();
    return ah_.port != NULL;
}

static void detach(const char *why)
{
    char m[120];
    sprintf(m, "AmigaAMP plug-in %.40s gone (%s), %lu frames", ah_.name[0] ? ah_.name : "", why,
            (unsigned long)ah_.frames);
    amce_log("visualisation", m);
    ah_.task = NULL;
    ah_.mask = ah_.info_mask = 0;
    ah_.ours = FALSE;
}

/* the plug-ins' messages: hellos and goodbyes */
static void messages(void)
{
    struct AmpPluginMessage *pm;
    while (ah_.port && (pm = (struct AmpPluginMessage *)GetMsg(ah_.port))) {
        if (pm->msg.mn_Length < offsetof(struct AmpPluginMessage, InfoMask)) {
            ReplyMsg(&pm->msg);             /* (not one) */
            continue;
        }
        if (!pm->PluginMask || !pm->PluginTask) {
            if (pm->PluginTask == NULL && ah_.task)
                detach("it said goodbye");
            ReplyMsg(&pm->msg);
            continue;
        }
        if (ah_.task && ah_.task != pm->PluginTask) {
            pm->Accepted = FALSE;           /* one at a time */
            ReplyMsg(&pm->msg);
            continue;
        }
        ah_.task = pm->PluginTask;
        ah_.mask = pm->PluginMask;
        ah_.ours = ah_.launched && time_ms() - ah_.launched < 15000;
        ah_.launched = 0;
        ah_.frames = 0;
        if (pm->SpecRawL)
            *pm->SpecRawL = ah_.spec_l;
        if (pm->SpecRawR)
            *pm->SpecRawR = ah_.spec_r;
        ah_.info_mask = 0;
        track_info();
        if (pm->msg.mn_Length >= V12_LEN) {
            ah_.info_mask = pm->InfoMask;
            if (pm->tinfo)
                *pm->tinfo = &ah_.ti;
        }
        if (pm->msg.mn_Length >= V13_LEN && pm->SampleRaw)
            *pm->SampleRaw = ah_.samples;
        pm->Accepted = TRUE;
        amce_strlcpy(ah_.name, ah_.task->tc_Node.ln_Name ? ah_.task->tc_Node.ln_Name : "", sizeof(ah_.name));
        {
            char m[120];
            sprintf(m, "AmigaAMP plug-in %.40s attached (its message %lu bytes: kit 1.%ld)", ah_.name,
                    (unsigned long)pm->msg.mn_Length,
                    (long)(pm->msg.mn_Length >= V13_LEN ? 3 : pm->msg.mn_Length >= V12_LEN ? 2 : 0));
            amce_log("visualisation", m);
        }
        ReplyMsg(&pm->msg);
        if (ah_.info_mask)
            Signal(ah_.task, ah_.info_mask);
    }
}

BOOL amp_attached(void)
{
    return ah_.task != NULL;
}

BOOL amp_ours(void)
{
    return ah_.task != NULL && ah_.ours;
}

/* Each frame (25 a second while one is attached): its messages, then the
 * sound now, its spectra, and the plug-in told. */
void amp_tick(void)
{
    ULONG rate = 0;
    messages();
    if (!ah_.task)
        return;
    if (!vis_pcm_lr(ah_.samples, 512, &rate))
        memset(ah_.samples, 0, sizeof(ah_.samples));    /* (nothing, or MHI: silence) */
    spectra();
    if (mp_serial() != ah_.serial || ah_.ti.DriveMode != (UWORD)(!mp_running() ? 0 : mp_paused() ? 3 : 1)) {
        track_info();
        if (ah_.info_mask)
            Signal(ah_.task, ah_.info_mask);
    }
    Signal(ah_.task, ah_.mask);
    ah_.frames++;
}

/* The plug-in asked to go (Ctrl-C), and waited for, up to wait_ms. */
void amp_stop(ULONG wait_ms)
{
    ULONG t0 = time_ms();
    if (!ah_.task)
        return;
    Signal(ah_.task, SIGBREAKF_CTRL_C);
    while (ah_.task && time_ms() - t0 < wait_ms) {
        messages();
        if (ah_.task)
            Delay(2);
    }
    if (ah_.task)
        detach("asked to go; no goodbye");
}

void amp_host_end(void)
{
    amp_stop(3000);
    if (ah_.port) {
        struct Message *m;
        RemPort(ah_.port);
        while ((m = GetMsg(ah_.port)))
            ReplyMsg(m);                    /* (a hello now: not accepted) */
        DeleteMsgPort(ah_.port);
        ah_.port = NULL;
    }
}

/* path run as a program of its own (its window on the default public
 * screen, as AmigaAMP's would be): FALSE if it could not be */
BOOL amp_launch(const char *path, char *err, LONG errmax)
{
    char cmd[300];
    BPTR in, out;
    LONG r;
    if (!amp_host_start()) {
        amce_strlcpy(err, _("AmigaAMP is running: its plug-ins are its own"), errmax);
        return FALSE;
    }
    if (ah_.task)
        amp_stop(2000);
    sprintf(cmd, "\"%.280s\"", path);
    in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!in || !out) {
        if (in)
            Close(in);
        if (out)
            Close(out);
        amce_strlcpy(err, _("The plug-in could not be started"), errmax);
        return FALSE;
    }
    r = SystemTags((CONST_STRPTR)cmd, SYS_Input, (ULONG)in, SYS_Output, (ULONG)out, SYS_Asynch, TRUE,
                   NP_StackSize, 32768, NP_Name, (ULONG)FilePart((STRPTR)path), TAG_DONE);
    if (r == -1) {
        Close(in);
        Close(out);
        amce_strlcpy(err, _("The plug-in could not be started"), errmax);
        return FALSE;
    }
    ah_.launched = time_ms();
    if (!ah_.launched)
        ah_.launched = 1;
    {
        char m[200];
        sprintf(m, "AmigaAMP plug-in started: %.160s", path);
        amce_log("visualisation", m);
    }
    return TRUE;
}
