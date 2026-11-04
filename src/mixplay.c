/*
 * AmigaMCE - modules mixed by modplug.library, streamed to Paula
 * Copyright (c) 2026 Brian King
 *
 * For what Paula's four channels and the EaglePlayer players cannot play
 * themselves: FastTracker (XM), Scream Tracker (S3M), Impulse Tracker
 * (IT), 6- and 8-channel MODs, MTM, 669... modplug.library (Thomas Wenzel's
 * 68k port of libmodplug, LIBS:) mixes them in software into a buffer,
 * and AmigaMCE plays that on two channels (left, right) of 8-bit samples.
 * It needs a 68020 or better; how good it sounds depends on the CPU: a
 * 68030 mixes at 22 kHz, a 68040/060 at 28 kHz with interpolation.
 *
 * Paula plays one half of a double buffer while a process of AmigaMCE's
 * mixes the next into the other: the channel 0 audio interrupt, as Paula
 * starts each half, points it at the other half next and wakes the
 * mixer for the one just played.
 *
 * modplug.library comes without a C header here; its calls are taken from
 * the library itself (its vector table): libmodplug's API with a handle
 * from ModPlug_Init that holds the settings.
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/semaphores.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <dos/dostags.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/dos.h>

extern struct ExecBase *SysBase;
#define CU ((volatile struct Custom *)0xDFF000)

struct MPSettings {             /* ModPlug_Settings (libmodplug 0.8.9) */
    LONG mFlags, mChannels, mBits, mFrequency, mResamplingMode, mStereoSeparation,
         mMaxMixChannels, mReverbDepth, mReverbDelay, mBassAmount, mBassRange,
         mSurroundDepth, mSurroundDelay, mLoopCount;
};

static struct Library *ModPlugBase;
#define MP_Init() LP0(30, APTR, ModPlug_Init, , ModPlugBase)
#define MP_Exit(h) LP1NR(36, ModPlug_Exit, APTR, h, a1, , ModPlugBase)
#define MP_Load(h, d, n) LP3(42, APTR, ModPlug_Load, APTR, h, a1, APTR, d, a2, LONG, n, d1, , ModPlugBase)
#define MP_Unload(f) LP1NR(48, ModPlug_Unload, APTR, f, a1, , ModPlugBase)
#define MP_Read(f, b, n) LP3(54, LONG, ModPlug_Read, APTR, f, a1, APTR, b, a2, LONG, n, d1, , ModPlugBase)
#define MP_GetName(f) LP1(60, STRPTR, ModPlug_GetName, APTR, f, a1, , ModPlugBase)
#define MP_GetLength(f) LP1(66, LONG, ModPlug_GetLength, APTR, f, a1, , ModPlugBase)
#define MP_Seek(f, ms) LP2NR(72, ModPlug_Seek, APTR, f, a1, LONG, ms, d1, , ModPlugBase)
#define MP_GetSettings(h, s) LP2NR(78, ModPlug_GetSettings, APTR, h, a1, APTR, s, a2, , ModPlugBase)
#define MP_SetSettings(h, s) LP2NR(84, ModPlug_SetSettings, APTR, h, a1, APTR, s, a2, , ModPlugBase)
#define MP_GetSpeed(f) LP1(102, LONG, ModPlug_GetCurrentSpeed, APTR, f, a1, , ModPlugBase)
#define MP_GetTempo(f) LP1(108, LONG, ModPlug_GetCurrentTempo, APTR, f, a1, , ModPlugBase)
#define MP_GetOrder(f) LP1(114, LONG, ModPlug_GetCurrentOrder, APTR, f, a1, , ModPlugBase)
#define MP_GetPattern(f) LP1(120, LONG, ModPlug_GetCurrentPattern, APTR, f, a1, , ModPlugBase)
#define MP_GetRow(f) LP1(126, LONG, ModPlug_GetCurrentRow, APTR, f, a1, , ModPlugBase)
#define MP_SeekOrder(f, o) LP2NR(138, ModPlug_SeekOrder, APTR, f, a1, LONG, o, d1, , ModPlugBase)
#define MP_GetModuleType(f) LP1(144, LONG, ModPlug_GetModuleType, APTR, f, a1, , ModPlugBase)
#define MP_NumInstruments(f) LP1(156, ULONG, ModPlug_NumInstruments, APTR, f, a1, , ModPlugBase)
#define MP_NumSamples(f) LP1(162, ULONG, ModPlug_NumSamples, APTR, f, a1, , ModPlugBase)
#define MP_NumChannels(f) LP1(174, ULONG, ModPlug_NumChannels, APTR, f, a1, , ModPlugBase)
#define MP_SampleName(f, i, b) LP3(180, ULONG, ModPlug_SampleName, APTR, f, a1, ULONG, i, d1, APTR, b, a2, , ModPlugBase)
#define MP_InstrumentName(f, i, b) LP3(186, ULONG, ModPlug_InstrumentName, APTR, f, a1, ULONG, i, d1, APTR, b, a2, , ModPlugBase)

#define PAULA_CLOCK 3546895UL       /* PAL */
#define HALF        4096            /* samples in each half of the buffer */
#define NAMES       64

static struct {
    APTR handle, file;
    struct SignalSemaphore lock;    /* around the library's calls on the file */
    BYTE *chip;                     /* left 0, left 1, right 0, right 1 */
    UBYTE *tmp;                     /* what it mixes: 8-bit stereo, interleaved */
    UWORD period;
    ULONG rate;
    struct Process *proc;
    volatile BYTE sig;
    volatile UBYTE cur, want;       /* the half Paula plays; the half to mix */
    volatile BOOL quit, ended, running, paused;
    volatile ULONG mixed;           /* samples mixed (the time played) */
    volatile UBYTE peak_l, peak_r;
    struct Interrupt aint;
    struct Interrupt *old_aint;
    BOOL int_set;
    UWORD master;
    char title[64], kind[24];
    char names[NAMES][32];
    int nnames;
    ULONG length_ms;
} mx;

/* ---------------------------------------------------------- mixing */
static void mix_half(int half)
{
    BYTE *l = mx.chip + half * HALF, *r = mx.chip + (2 + half) * HALF;
    const UBYTE *t = mx.tmp;
    LONG got = 0, i, n;
    UBYTE pl = 0, pr = 0;
    if (!mx.ended) {
        ObtainSemaphore(&mx.lock);
        got = MP_Read(mx.file, mx.tmp, HALF * 2);
        ReleaseSemaphore(&mx.lock);
        if (got < HALF * 2)
            mx.ended = TRUE;
    }
    n = got / 2;
    for (i = 0; i < n; i++, t += 2) {
        BYTE a = (BYTE)(t[0] ^ 0x80), b = (BYTE)(t[1] ^ 0x80);
        UBYTE aa = (UBYTE)(a < 0 ? -a : a), bb = (UBYTE)(b < 0 ? -b : b);
        l[i] = a;
        r[i] = b;
        if (aa > pl)
            pl = aa;
        if (bb > pr)
            pr = bb;
    }
    for (; i < HALF; i++)
        l[i] = r[i] = 0;
    mx.mixed += (ULONG)n;
    if (pl > mx.peak_l)
        mx.peak_l = pl;
    if (pr > mx.peak_r)
        mx.peak_r = pr;
}

/* The mixer process: a half each time it is woken. */
static void mixer_main(void)
{
    BYTE s = AllocSignal(-1);
    mx.sig = s;
    while (!mx.quit) {
        ULONG got = Wait((s >= 0 ? 1UL << s : 0) | SIGBREAKF_CTRL_C);
        if (mx.quit || (got & SIGBREAKF_CTRL_C))
            break;
        if (!mx.paused)
            mix_half(mx.want);
    }
    if (s >= 0)
        FreeSignal(s);
    mx.sig = -1;
    Forbid();                               /* gone before mx_stop goes on */
    mx.proc = NULL;
}

/* Channel 0's audio interrupt: Paula has taken a half and plays it. */
static void audio_code(void)
{
    UBYTE next;
    CU->intreq = INTF_AUD0;
    mx.cur ^= 1;
    next = (UBYTE)(mx.cur ^ 1);
    CU->aud[0].ac_ptr = (UWORD *)(mx.chip + next * HALF);
    CU->aud[1].ac_ptr = (UWORD *)(mx.chip + (2 + next) * HALF);
    mx.want = next;
    if (mx.proc && mx.sig >= 0)
        Signal(&mx.proc->pr_Task, 1UL << mx.sig);
}

/* ---------------------------------------------------------- the library */
BOOL mx_available(void)
{
    if (!(SysBase->AttnFlags & AFF_68020))
        return FALSE;                       /* the library is built for 68020+ */
    if (!ModPlugBase)
        ModPlugBase = OpenLibrary((CONST_STRPTR)"modplug.library", 1);
    return ModPlugBase != NULL;
}

static void settings(void)
{
    struct MPSettings s;
    BOOL fast = (SysBase->AttnFlags & (AFF_68040 | AFF_68060)) != 0;
    MP_GetSettings(mx.handle, &s);
    mx.period = fast ? 127 : 161;           /* 27.9 / 22 kHz */
    mx.rate = PAULA_CLOCK / mx.period;
    s.mFlags = 0;                           /* no oversampling, noise reduction... */
    s.mChannels = 2;
    s.mBits = 8;
    s.mFrequency = (LONG)mx.rate;
    s.mResamplingMode = fast ? 1 : 0;       /* linear, or nearest on slower CPUs */
    s.mStereoSeparation = 128;
    s.mMaxMixChannels = 32;
    s.mReverbDepth = s.mReverbDelay = s.mBassAmount = s.mBassRange = 0;
    s.mSurroundDepth = s.mSurroundDelay = 0;
    s.mLoopCount = 0;
    MP_SetSettings(mx.handle, &s);
}

static const char *type_name(LONG t)
{
    /* libmodplug's MOD_TYPE_* bits */
    static const struct { LONG bit; const char *name; } types[] = {
        { 0x01, "MOD" }, { 0x02, "S3M" }, { 0x04, "XM" }, { 0x08, "MED" }, { 0x10, "MTM" },
        { 0x20, "IT" }, { 0x40, "669" }, { 0x80, "ULT" }, { 0x100, "STM" }, { 0x200, "FAR" },
        { 0x800, "AMF" }, { 0x1000, "AMS" }, { 0x2000, "DSM" }, { 0x4000, "MDL" },
        { 0x8000, "OKT" }, { 0x10000, "MID" }, { 0x20000, "DMF" }, { 0x40000, "PTM" },
        { 0x80000, "DBM" }, { 0x100000, "MT2" }, { 0x200000, "AMF" }, { 0x400000, "PSM" },
        { 0x800000, "J2B" }, { 0x1000000, "ABC" }, { 0x2000000, "PAT" }, { 0, NULL }
    };
    int i;
    for (i = 0; types[i].name; i++)
        if (t & types[i].bit)
            return types[i].name;
    return "module";
}

void mx_unload(void)
{
    mx_stop();
    if (mx.file)
        MP_Unload(mx.file);
    mx.file = NULL;
    if (mx.handle)
        MP_Exit(mx.handle);
    mx.handle = NULL;
}

/* The module (len bytes, AllocVec, given to this) loaded, ready to play. */
BOOL mx_load(const char *path, UBYTE *mod, ULONG len, char *err, LONG errmax)
{
    STRPTR name;
    ULONG i, n;
    char m[160];
    (void)path;
    mx_unload();
    if (!mx_available()) {
        FreeVec(mod);
        amce_strlcpy(err, (SysBase->AttnFlags & AFF_68020)
                              ? "This module needs modplug.library, which is not installed"
                              : "This module needs a 68020 or better to be mixed",
                     errmax);
        return FALSE;
    }
    if (!mx.lock.ss_Link.ln_Type) {
        InitSemaphore(&mx.lock);
        mx.lock.ss_Link.ln_Type = NT_SIGNALSEM;
    }
    if (!(mx.handle = MP_Init())) {
        FreeVec(mod);
        amce_strlcpy(err, "Not enough memory to mix this module", errmax);
        return FALSE;
    }
    settings();
    mx.file = MP_Load(mx.handle, mod, (LONG)len);
    FreeVec(mod);                           /* libmodplug keeps a copy of its own */
    if (!mx.file) {
        MP_Exit(mx.handle);
        mx.handle = NULL;
        amce_strlcpy(err, "Not a module AmigaMCE can play", errmax);
        return FALSE;
    }
    mx.title[0] = 0;
    if ((name = MP_GetName(mx.file)))
        amce_strlcpy(mx.title, (const char *)name, sizeof(mx.title));
    amce_strlcpy(mx.kind, type_name(MP_GetModuleType(mx.file)), sizeof(mx.kind));
    mx.length_ms = (ULONG)MP_GetLength(mx.file);
    /* the sample names (instruments' where it has them) */
    mx.nnames = 0;
    n = MP_NumInstruments(mx.file);
    if (n) {
        for (i = 1; i <= n && mx.nnames < NAMES; i++) {
            mx.names[mx.nnames][0] = 0;
            MP_InstrumentName(mx.file, i, mx.names[mx.nnames]);
            mx.names[mx.nnames][31] = 0;
            mx.nnames++;
        }
    } else {
        n = MP_NumSamples(mx.file);
        for (i = 1; i <= n && mx.nnames < NAMES; i++) {
            mx.names[mx.nnames][0] = 0;
            MP_SampleName(mx.file, i, mx.names[mx.nnames]);
            mx.names[mx.nnames][31] = 0;
            mx.nnames++;
        }
    }
    sprintf(m, "%s, %lu channels, %lu:%02lu, mixed at %lu Hz", mx.kind,
            (unsigned long)MP_NumChannels(mx.file), (unsigned long)(mx.length_ms / 60000),
            (unsigned long)(mx.length_ms / 1000 % 60), (unsigned long)mx.rate);
    amce_log("modplug", m);
    return TRUE;
}

/* ---------------------------------------------------------- playing */
void mx_stop(void)
{
    if (!mx.running && !mx.proc && !mx.int_set)
        return;
    CU->intena = INTF_AUD0;
    CU->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    CU->intreq = INTF_AUD0;
    if (mx.int_set) {
        SetIntVector(INTB_AUD0, mx.old_aint);
        mx.int_set = FALSE;
    }
    if (mx.proc) {
        mx.quit = TRUE;
        Signal(&mx.proc->pr_Task, SIGBREAKF_CTRL_C);
        while (mx.proc)
            Delay(1);
    }
    mx.running = FALSE;
    if (mx.chip) {
        FreeMem(mx.chip, HALF * 4);
        mx.chip = NULL;
    }
    if (mx.tmp) {
        FreeVec(mx.tmp);
        mx.tmp = NULL;
    }
}

/* From the start, or from where Seek put it (the channels are AmigaMCE's). */
BOOL mx_start(UWORD master, char *err, LONG errmax)
{
    int i;
    if (!mx.file)
        return FALSE;
    mx_stop();
    mx.chip = AllocMem(HALF * 4, MEMF_CHIP | MEMF_CLEAR);
    mx.tmp = AllocVec(HALF * 2, MEMF_ANY);
    if (!mx.chip || !mx.tmp) {
        mx_stop();
        amce_strlcpy(err, "Not enough memory to mix this module", errmax);
        return FALSE;
    }
    mx.quit = mx.ended = mx.paused = FALSE;
    mx.sig = -1;
    mx.master = master;
    mx.proc = CreateNewProcTags(NP_Entry, (ULONG)mixer_main, NP_Name, (ULONG)"AmigaMCE mixer",
                                NP_Priority, 5, NP_StackSize, 32768, TAG_DONE);
    if (!mx.proc) {
        mx_stop();
        amce_strlcpy(err, "Could not start the mixer", errmax);
        return FALSE;
    }
    for (i = 0; i < 50 && mx.sig < 0; i++)
        Delay(1);                           /* its signal allocated */
    mix_half(0);                            /* the first half, before Paula starts */
    mx.cur = 1;                             /* the first interrupt: half 0 starts */
    mx.want = 1;
    mx.aint.is_Node.ln_Type = NT_INTERRUPT;
    mx.aint.is_Node.ln_Pri = 0;
    mx.aint.is_Node.ln_Name = (char *)"AmigaMCE mixer";
    mx.aint.is_Data = NULL;
    mx.aint.is_Code = (void (*)())audio_code;
    CU->intena = INTF_AUD0;
    CU->intreq = INTF_AUD0;
    mx.old_aint = SetIntVector(INTB_AUD0, &mx.aint);
    mx.int_set = TRUE;
    for (i = 0; i < 2; i++) {
        CU->aud[i].ac_ptr = (UWORD *)(mx.chip + (i ? 2 : 0) * HALF);
        CU->aud[i].ac_len = HALF / 2;
        CU->aud[i].ac_per = mx.period;
        CU->aud[i].ac_vol = master;
    }
    CU->aud[2].ac_vol = 0;
    CU->aud[3].ac_vol = 0;
    mx.running = TRUE;
    CU->intena = INTF_SETCLR | INTF_AUD0;
    CU->dmacon = DMAF_SETCLR | DMAF_AUD0 | DMAF_AUD1;
    return TRUE;
}

void mx_pause(BOOL on)
{
    if (!mx.running || on == mx.paused)
        return;
    mx.paused = on;
    CU->aud[0].ac_vol = on ? 0 : mx.master;
    CU->aud[1].ac_vol = on ? 0 : mx.master;
}

void mx_volume(int v)
{
    mx.master = (UWORD)v;
    if (mx.running && !mx.paused) {
        CU->aud[0].ac_vol = mx.master;
        CU->aud[1].ac_vol = mx.master;
    }
}

BOOL mx_ended(void)
{
    return mx.running && mx.ended;
}

void mx_seek_ms(ULONG ms)
{
    if (!mx.file)
        return;
    ObtainSemaphore(&mx.lock);
    MP_Seek(mx.file, (LONG)ms);
    mx.mixed = (ULONG)((ms / 10) * (mx.rate / 100));
    mx.ended = FALSE;
    ReleaseSemaphore(&mx.lock);
}

BOOL mx_step(int d)
{
    LONG o;
    if (!mx.file)
        return FALSE;
    ObtainSemaphore(&mx.lock);
    o = MP_GetOrder(mx.file) + d;
    if (o < 0)
        o = 0;
    MP_SeekOrder(mx.file, o);
    mx.ended = FALSE;
    ReleaseSemaphore(&mx.lock);
    return TRUE;
}

void mx_rewind(void)
{
    mx_seek_ms(0);
    mx.mixed = 0;
}

void mx_status(struct MpStatus *s, UBYTE *lev_l, UBYTE *lev_r)
{
    if (!mx.file)
        return;
    s->order = (WORD)MP_GetOrder(mx.file);
    s->row = (WORD)MP_GetRow(mx.file);
    s->pattern = (WORD)MP_GetPattern(mx.file);
    s->speed = (WORD)MP_GetSpeed(mx.file);
    s->bpm = (WORD)MP_GetTempo(mx.file);
    s->songlen = 0;
    s->elapsed_ms = mx.rate ? mx.mixed / mx.rate * 1000 + (mx.mixed % mx.rate) * 1000 / mx.rate : 0;
    s->total_ms = mx.length_ms;
    if (s->elapsed_ms > s->total_ms && s->total_ms)
        s->elapsed_ms = s->total_ms;
    *lev_l = mx.peak_l;
    *lev_r = mx.peak_r;
    mx.peak_l = mx.peak_r = 0;
}

BOOL mx_loaded(void) { return mx.file != NULL; }
const char *mx_title(void) { return mx.title; }
const char *mx_kind(void) { return mx.kind; }
int mx_names(void) { return mx.nnames; }
const char *mx_name(int i) { return i >= 0 && i < mx.nnames ? mx.names[i] : ""; }

void mx_close_library(void)
{
    mx_unload();
    if (ModPlugBase)
        CloseLibrary(ModPlugBase);
    ModPlugBase = NULL;
}
