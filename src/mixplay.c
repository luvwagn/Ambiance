/*
 * Ambiance - modules mixed by modplug.library, streamed to Paula
 * Copyright (c) 2026 Brian King
 *
 * For what Paula's four channels and the EaglePlayer players cannot play
 * themselves: FastTracker (XM), Scream Tracker (S3M), Impulse Tracker
 * (IT), 6- and 8-channel MODs, MTM, 669... modplug.library (Thomas Wenzel's
 * 68k port of libmodplug, LIBS:) mixes them in software into a buffer,
 * and Ambiance plays that as Settings > Sound says (as radio and music
 * files): on two of Paula's channels (left, right) at 8 bits, on all four
 * at 14 (each side's high byte at volume 64, its next 6 bits at volume 1),
 * or through AHI at 16. It needs a 68020 or better; how good it sounds
 * depends on the CPU: a 68030 mixes at 22 kHz, a 68040/060 at 28 kHz with
 * interpolation. A change of output while it plays (mx_output): it goes
 * on from where it was through the new one.
 *
 * It mixes 16-bit stereo into one half of a double buffer while the other
 * plays. On Paula, the channel 0 audio interrupt, as Paula starts each
 * half, points it at the other half next and wakes the mixer for the one
 * just played; through AHI, its sound hook does the same with its two
 * sounds.
 *
 * modplug.library comes without a C header here; its calls are taken from
 * the library itself (its vector table): libmodplug's API with a handle
 * from ModPlug_Init that holds the settings.
 */
#include "amb.h"

#include <string.h>
#include <stdio.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/semaphores.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <dos/dostags.h>
#include <devices/ahi.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>

extern struct Library *AHIBase;     /* (stream.c's: one of the two plays at a time) */

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
#define MP_SetMasterVolume(f, v) LP2NR(96, ModPlug_SetMasterVolume, APTR, f, a1, ULONG, v, d1, , ModPlugBase)
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

enum { MXO_PAULA8, MXO_PAULA14, MXO_AHI };

static struct {
    APTR handle, file;
    struct SignalSemaphore lock;    /* around the library's calls on the file */
    UBYTE out;                      /* MXO_: where it plays */
    BYTE *chip;                     /* Paula's: left 0, left 1, right 0, right 1 (14 bits:
                                     * then left's low bits 0, 1, right's 0, 1) */
    ULONG chip_size;
    WORD *mix[2];                   /* each half as mixed: 16-bit stereo, interleaved
                                     * (AHI's two sounds themselves) */
    /* AHI's */
    struct MsgPort *aport;
    struct AHIRequest *aio;
    BOOL aopen, aplaying;
    struct AHIAudioCtrl *actrl;
    struct Hook ahook;
    volatile BYTE aqueued;
    volatile UBYTE afill_due;
    UWORD avol;
    UWORD period;
    ULONG rate;
    struct Process *proc;
    volatile BYTE sig;
    volatile UBYTE cur, want;       /* the half Paula plays; the half to mix */
    volatile BOOL quit, ended, running, paused;
    volatile ULONG mixed;           /* samples mixed (the time played) */
    volatile UBYTE peak_l, peak_r;
    volatile UWORD first_pk;        /* the loudest sample since Play (its first seconds logged) */
    ULONG first_t;                  /* when Play started it (0: logged) */
    volatile ULONG play_t;          /* when the half playing now began (spectrum) */
    struct Interrupt aint;
    struct Interrupt *old_aint;
    BOOL int_set;
    UWORD master;
    char title[64], kind[24];
    char names[NAMES][32];
    int nnames;
    ULONG length_ms;
} mx;

static void paula_volumes(void);

/* ---------------------------------------------------------- mixing */
/* Half half mixed (16-bit stereo), then, on Paula, as its channels want it */
static void mix_half(int half)
{
    WORD *t = mx.mix[half];
    LONG got = 0, i, n;
    UWORD pl = 0, pr = 0;
    if (!mx.ended) {
        ObtainSemaphore(&mx.lock);
        got = MP_Read(mx.file, t, HALF * 4);
        ReleaseSemaphore(&mx.lock);
        if (got < HALF * 4)
            mx.ended = TRUE;
    }
    n = got / 4;
    for (i = 0; i < n; i++) {
        WORD a = t[2 * i], b = t[2 * i + 1];
        UWORD aa = (UWORD)(a < 0 ? -a : a), bb = (UWORD)(b < 0 ? -b : b);
        if (aa > pl)
            pl = aa;
        if (bb > pr)
            pr = bb;
    }
    for (i = n; i < HALF; i++)
        t[2 * i] = t[2 * i + 1] = 0;
    if (mx.out == MXO_PAULA8) {
        BYTE *l = mx.chip + half * HALF, *r = mx.chip + (2 + half) * HALF;
        for (i = 0; i < HALF; i++) {
            l[i] = (BYTE)(t[2 * i] >> 8);
            r[i] = (BYTE)(t[2 * i + 1] >> 8);
        }
    } else if (mx.out == MXO_PAULA14) {
        /* each side's high byte (volume 64) and next 6 bits (volume 1): the
         * volume ours, Paula's are those */
        BYTE *l = mx.chip + half * HALF, *r = mx.chip + (2 + half) * HALF;
        UBYTE *ll = (UBYTE *)mx.chip + (4 + half) * HALF, *rl = (UBYTE *)mx.chip + (6 + half) * HALF;
        LONG vol = mx.master;
        for (i = 0; i < HALF; i++) {
            LONG a = ((LONG)t[2 * i] * vol) >> 6, b = ((LONG)t[2 * i + 1] * vol) >> 6;
            l[i] = (BYTE)(a >> 8);
            r[i] = (BYTE)(b >> 8);
            ll[i] = (UBYTE)((a & 0xff) >> 2);
            rl[i] = (UBYTE)((b & 0xff) >> 2);
        }
    }
    mx.mixed += (ULONG)n;
    if (pl > mx.first_pk)
        mx.first_pk = pl;
    if (pr > mx.first_pk)
        mx.first_pk = pr;
    if ((pl >> 8) > mx.peak_l)
        mx.peak_l = (UBYTE)(pl >> 8);
    if ((pr >> 8) > mx.peak_r)
        mx.peak_r = (UBYTE)(pr >> 8);
}

/* AHI's sound hook (its interrupt): a sound starts; the other is queued
 * after it and the mixer woken to mix it */
static ULONG ahi_hook(register struct Hook *h __asm("a0"), register struct AHISoundMessage *sm __asm("a1"),
                      register struct AHIAudioCtrl *a __asm("a2"))
{
    BYTE k = mx.aqueued < 0 ? 0 : mx.aqueued;
    (void)h;
    (void)sm;
    mx.cur = (UBYTE)k;
    mx.play_t = time_ms();
    mx.aqueued = (BYTE)(k ^ 1);
    AHI_SetSound(0, (UWORD)(k ^ 1), 0, 0, a, AHISF_NONE);
    mx.afill_due |= (UBYTE)(1 << (k ^ 1));
    if (mx.proc && mx.sig >= 0)
        Signal(&mx.proc->pr_Task, 1UL << mx.sig);
    return 0;
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
        if (mx.out == MXO_AHI) {
            UBYTE due;
            int k;
            Disable();
            due = mx.afill_due;
            mx.afill_due = 0;
            Enable();
            for (k = 0; k < 2; k++)
                if (due & (1 << k)) {
                    if (mx.paused)
                        memset(mx.mix[k], 0, HALF * 4);     /* (paused: silence, the song held) */
                    else
                        mix_half(k);
                }
            if (mx.actrl && mx.avol != (mx.paused ? 0 : mx.master)) {
                mx.avol = mx.paused ? 0 : mx.master;
                AHI_SetVol(0, (Fixed)(((ULONG)mx.avol << 16) / 64), 0x8000, mx.actrl, AHISF_IMM);
            }
        } else if (!mx.paused) {
            mix_half(mx.want);
        }
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
    mx.play_t = time_ms();                  /* (ReadEClock: callable here) */
    next = (UBYTE)(mx.cur ^ 1);
    CU->aud[0].ac_ptr = (UWORD *)(mx.chip + next * HALF);
    CU->aud[1].ac_ptr = (UWORD *)(mx.chip + (2 + next) * HALF);
    if (mx.out == MXO_PAULA14) {
        CU->aud[3].ac_ptr = (UWORD *)(mx.chip + (4 + next) * HALF);
        CU->aud[2].ac_ptr = (UWORD *)(mx.chip + (6 + next) * HALF);
    }
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
    s.mBits = 16;                           /* (Paula's 8 bits taken from it) */
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
        amb_strlcpy(err, (SysBase->AttnFlags & AFF_68020)
                              ? "This module needs modplug.library, which is not installed"
                              : _("This module needs a 68020 or better to be mixed"),
                     errmax);
        return FALSE;
    }
    if (!mx.lock.ss_Link.ln_Type) {
        InitSemaphore(&mx.lock);
        mx.lock.ss_Link.ln_Type = NT_SIGNALSEM;
    }
    if (!(mx.handle = MP_Init())) {
        FreeVec(mod);
        amb_strlcpy(err, _("Not enough memory to mix this module"), errmax);
        return FALSE;
    }
    settings();
    mx.file = MP_Load(mx.handle, mod, (LONG)len);
    FreeVec(mod);                           /* libmodplug keeps a copy of its own */
    if (!mx.file) {
        MP_Exit(mx.handle);
        mx.handle = NULL;
        amb_strlcpy(err, _("Not a module Ambiance can play"), errmax);
        return FALSE;
    }
    mx.title[0] = 0;
    if ((name = MP_GetName(mx.file)))
        amb_strlcpy(mx.title, (const char *)name, sizeof(mx.title));
    amb_strlcpy(mx.kind, type_name(MP_GetModuleType(mx.file)), sizeof(mx.kind));
    mx.length_ms = (ULONG)MP_GetLength(mx.file);
    /* louder: libmodplug's own level leaves a 4-channel module's peaks
     * near -10 dB (WeNeedJungle: 10560 of 32767), its quiet passages
     * 30 dB down, and Paula's 8 bits only the top of them. 384 is twice
     * its 128 (6 dB), 256 one and a half (more channels: more to add up) */
    MP_SetMasterVolume(mx.file, MP_NumChannels(mx.file) <= 4 ? 384 : 256);
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
    amb_log("modplug", m);
    return TRUE;
}

/* ---------------------------------------------------------- playing */
static void ahi_end(void)
{
    amb_card_lock();
    if (mx.actrl) {
        if (mx.aplaying) {
            struct TagItem t[2];
            t[0].ti_Tag = AHIC_Play;
            t[0].ti_Data = FALSE;
            t[1].ti_Tag = TAG_DONE;
            AHI_ControlAudioA(mx.actrl, t);
        }
        AHI_FreeAudio(mx.actrl);
    }
    if (mx.aopen)
        CloseDevice((struct IORequest *)mx.aio);
    if (mx.aopen || mx.actrl)
        AHIBase = NULL;
    amb_card_unlock();
    if (mx.aio)
        DeleteIORequest((struct IORequest *)mx.aio);
    if (mx.aport)
        DeleteMsgPort(mx.aport);
    mx.actrl = NULL;
    mx.aio = NULL;
    mx.aport = NULL;
    mx.aopen = mx.aplaying = FALSE;
}

void mx_stop(void)
{
    int k;
    if (!mx.running && !mx.proc && !mx.int_set && !mx.actrl && !mx.aopen)
        return;
    if (mx.out != MXO_AHI || mx.int_set) {
        CU->intena = INTF_AUD0;
        CU->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
        for (k = 0; k < 4; k++)
            CU->aud[k].ac_vol = 0;
        CU->intreq = INTF_AUD0;
    }
    if (mx.int_set) {
        SetIntVector(INTB_AUD0, mx.old_aint);
        mx.int_set = FALSE;
    }
    if (mx.actrl && mx.aplaying) {
        /* AHI stopped first: its hook no longer wakes the mixer */
        struct TagItem t[2];
        t[0].ti_Tag = AHIC_Play;
        t[0].ti_Data = FALSE;
        t[1].ti_Tag = TAG_DONE;
        amb_card_lock();
        AHI_ControlAudioA(mx.actrl, t);
        amb_card_unlock();
        mx.aplaying = FALSE;
    }
    if (mx.proc) {
        mx.quit = TRUE;
        Signal(&mx.proc->pr_Task, SIGBREAKF_CTRL_C);
        while (mx.proc)
            Delay(1);
    }
    if (mx.actrl || mx.aopen)
        ahi_end();                          /* (the mixer gone: nothing calls AHI now) */
    Delay(1);                               /* Paula's last fetches from the buffer done */
    mx.running = FALSE;
    if (mx.chip) {
        FreeMem(mx.chip, mx.chip_size);
        mx.chip = NULL;
    }
    for (k = 0; k < 2; k++) {
        if (mx.mix[k])
            FreeVec(mx.mix[k]);
        mx.mix[k] = NULL;
    }
}

/* AHI's default mode opened and its two sounds, the first mixed and
 * playing: FALSE if it could not be (the caller stops) */
static BOOL ahi_begin(char *err, LONG errmax)
{
    struct TagItem t[8];
    struct AHISampleInfo si;
    int k;
    BOOL ok = FALSE;
    if (!(mx.aport = CreateMsgPort()) ||
        !(mx.aio = (struct AHIRequest *)CreateIORequest(mx.aport, sizeof(struct AHIRequest))))
        goto out;
    mx.aio->ahir_Version = 4;
    amb_card_lock();
    if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)mx.aio, 0)) {
        amb_card_unlock();
        goto out;
    }
    mx.aopen = TRUE;
    AHIBase = (struct Library *)mx.aio->ahir_Std.io_Device;
    mx.ahook.h_Entry = (HOOKFUNC)(APTR)ahi_hook;
    mx.aqueued = -1;
    mx.afill_due = 0;
    t[0].ti_Tag = AHIA_AudioID;
    t[0].ti_Data = AHI_DEFAULT_ID;
    t[1].ti_Tag = AHIA_MixFreq;
    t[1].ti_Data = AHI_DEFAULT_FREQ;
    t[2].ti_Tag = AHIA_Channels;
    t[2].ti_Data = 1;
    t[3].ti_Tag = AHIA_Sounds;
    t[3].ti_Data = 2;
    t[4].ti_Tag = AHIA_SoundFunc;
    t[4].ti_Data = (ULONG)&mx.ahook;
    t[5].ti_Tag = TAG_DONE;
    if (!(mx.actrl = AHI_AllocAudioA(t))) {
        amb_card_unlock();
        goto out;
    }
    for (k = 0; k < 2; k++) {
        si.ahisi_Type = AHIST_S16S;
        si.ahisi_Address = mx.mix[k];
        si.ahisi_Length = HALF;
        if (AHI_LoadSound((UWORD)k, AHIST_DYNAMICSAMPLE, &si, mx.actrl) != AHIE_OK) {
            amb_card_unlock();
            goto out;
        }
    }
    mix_half(0);                            /* the first; the second asked for as it starts */
    t[0].ti_Tag = AHIC_Play;
    t[0].ti_Data = TRUE;
    t[1].ti_Tag = TAG_DONE;
    if (AHI_ControlAudioA(mx.actrl, t) != AHIE_OK) {
        amb_card_unlock();
        goto out;
    }
    mx.aplaying = TRUE;
    mx.avol = mx.paused ? 0 : mx.master;
    t[0].ti_Tag = AHIP_BeginChannel;
    t[0].ti_Data = 0;
    t[1].ti_Tag = AHIP_Freq;
    t[1].ti_Data = mx.rate;
    t[2].ti_Tag = AHIP_Vol;
    t[2].ti_Data = ((ULONG)mx.avol << 16) / 64;
    t[3].ti_Tag = AHIP_Pan;
    t[3].ti_Data = 0x8000;
    t[4].ti_Tag = AHIP_Sound;
    t[4].ti_Data = 0;
    t[5].ti_Tag = AHIP_EndChannel;
    t[5].ti_Data = 0;
    t[6].ti_Tag = TAG_DONE;
    AHI_PlayA(mx.actrl, t);
    amb_card_unlock();
    mx.play_t = time_ms();
    ok = TRUE;
out:
    if (!ok)
        amb_strlcpy(err, _("AHI could not be opened (its Preferences: a mode?)"), errmax);
    return ok;
}

/* From the start, or from where Seek put it (the channels are Ambiance's). */
BOOL mx_start(UWORD master, char *err, LONG errmax)
{
    int i, k;
    static const char *const outs[] = { "Paula, 8 bits", "Paula, 14 bits (four channels)", "AHI, 16 bits" };
    if (!mx.file)
        return FALSE;
    mx_stop();
    mx.out = st_ahi() ? MXO_AHI : st_paula14_wanted() ? MXO_PAULA14 : MXO_PAULA8;
    mx.chip_size = mx.out == MXO_AHI ? 0 : HALF * (mx.out == MXO_PAULA14 ? 8 : 4);
    if (mx.chip_size)
        mx.chip = AllocMem(mx.chip_size, MEMF_CHIP | MEMF_CLEAR);
    for (k = 0; k < 2; k++)
        mx.mix[k] = AllocVec(HALF * 4, MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR);
    if ((mx.chip_size && !mx.chip) || !mx.mix[0] || !mx.mix[1]) {
        mx_stop();
        amb_strlcpy(err, _("Not enough memory to mix this module"), errmax);
        return FALSE;
    }
    mx.quit = mx.ended = mx.paused = FALSE;
    mx.sig = -1;
    mx.first_pk = 0;
    mx.first_t = time_ms();
    mx.master = master;
    mx.proc = CreateNewProcTags(NP_Entry, (ULONG)mixer_main, NP_Name, (ULONG)"Ambiance mixer",
                                NP_Priority, 5, NP_StackSize, 32768, TAG_DONE);
    if (!mx.proc) {
        mx_stop();
        amb_strlcpy(err, _("Could not start the mixer"), errmax);
        return FALSE;
    }
    for (i = 0; i < 50 && mx.sig < 0; i++)
        Delay(1);                           /* its signal allocated */
    {
        static UBYTE said = 0xFF;
        if (said != mx.out) {
            char m[100];
            said = mx.out;
            sprintf(m, "mixed at %lu Hz, played through %s", (unsigned long)mx.rate, outs[mx.out]);
            amb_log("modplug", m);
        }
    }
    if (mx.out == MXO_AHI) {
        mx.running = TRUE;
        if (!ahi_begin(err, errmax)) {
            mx_stop();
            return FALSE;
        }
        return TRUE;
    }
    mix_half(0);                            /* the first half, before Paula starts */
    mx.cur = 1;                             /* the first interrupt: half 0 starts */
    mx.want = 1;
    mx.aint.is_Node.ln_Type = NT_INTERRUPT;
    mx.aint.is_Node.ln_Pri = 0;
    mx.aint.is_Node.ln_Name = (char *)"Ambiance mixer";
    mx.aint.is_Data = NULL;
    mx.aint.is_Code = (void (*)())audio_code;
    CU->intena = INTF_AUD0;
    CU->intreq = INTF_AUD0;
    mx.old_aint = SetIntVector(INTB_AUD0, &mx.aint);
    mx.int_set = TRUE;
    for (i = 0; i < 4; i++) {
        /* 0 left, 1 right; at 14 bits 3 left's low bits, 2 right's */
        int part = i == 0 ? 0 : i == 1 ? 2 : i == 3 ? 4 : 6;
        CU->aud[i].ac_ptr = (UWORD *)(mx.chip + (i < 2 || mx.out == MXO_PAULA14 ? part : 0) * HALF);
        CU->aud[i].ac_len = HALF / 2;
        CU->aud[i].ac_per = mx.period;
    }
    paula_volumes();
    mx.running = TRUE;
    CU->intena = INTF_SETCLR | INTF_AUD0;
    CU->dmacon = DMAF_SETCLR | DMAF_AUD0 | DMAF_AUD1 | (mx.out == MXO_PAULA14 ? DMAF_AUD2 | DMAF_AUD3 : 0);
    return TRUE;
}

/* Through what Settings > Sound says now: on from where it was (FALSE: it
 * could not; stopped) */
BOOL mx_output(void (*between)(void), char *err, LONG errmax)
{
    BOOL paused = mx.paused;
    UBYTE want = st_ahi() ? MXO_AHI : st_paula14_wanted() ? MXO_PAULA14 : MXO_PAULA8;
    if (!mx.running || want == mx.out)
        return TRUE;
    /* the old way stopped first (Paula's interrupt given back to
     * audio.device before it is closed), then between: audio.device
     * closed on the way to AHI. Closed while the mixer still had its
     * interrupt, Ambiance hung */
    mx_stop();
    if (between)
        between();
    if (!mx_start(mx.master, err, errmax))
        return FALSE;
    if (paused)
        mx_pause(TRUE);
    return TRUE;
}

/* Paula's volumes for the output and the state: 8 bits the master's on
 * 0 and 1; 14 bits 64 and 1 (the volume in the samples); paused 0 */
static void paula_volumes(void)
{
    UWORD hi = mx.paused ? 0 : mx.out == MXO_PAULA14 ? 64 : mx.master;
    UWORD lo = !mx.paused && mx.out == MXO_PAULA14 ? 1 : 0;
    CU->aud[0].ac_vol = hi;
    CU->aud[1].ac_vol = hi;
    CU->aud[2].ac_vol = lo;
    CU->aud[3].ac_vol = lo;
}

void mx_pause(BOOL on)
{
    if (!mx.running || on == mx.paused)
        return;
    mx.paused = on;
    if (mx.out == MXO_AHI) {
        if (mx.proc && mx.sig >= 0)
            Signal(&mx.proc->pr_Task, 1UL << mx.sig);   /* (the volume set by the mixer) */
    } else {
        paula_volumes();
    }
}

void mx_volume(int v)
{
    mx.master = (UWORD)v;
    if (mx.running && !mx.paused && mx.out == MXO_PAULA8)
        paula_volumes();                    /* (14 bits: in the samples; AHI: the mixer sets it) */
    else if (mx.running && mx.out == MXO_AHI && mx.proc && mx.sig >= 0)
        Signal(&mx.proc->pr_Task, 1UL << mx.sig);
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

BOOL mx_can_step(void)
{
    return mx.file != NULL;
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
    if (mx.first_t && mx.running && !mx.paused && time_ms() - mx.first_t >= 5000) {
        /* how loud its first seconds were mixed, and the volume they had:
         * a Play heard quieter than another comes out here */
        static const char *const outs[] = { "Paula 8-bit", "Paula 14-bit", "AHI" };
        char m[140];
        sprintf(m, "from %lu ms, 5 s: loudest sample %lu of 32767, %s at volume %lu of 64",
                (unsigned long)(s->elapsed_ms > 5000 ? s->elapsed_ms - 5000 : 0),
                (unsigned long)mx.first_pk, outs[mx.out],
                (unsigned long)(mx.out == MXO_AHI ? mx.avol : mx.master));
        amb_log("modplug", m);
        mx.first_t = 0;
    }
}

BOOL mx_loaded(void) { return mx.file != NULL; }

/* The spectrum's: the last n samples played, mono, about 8 kHz, from the
 * half Paula plays now (as stream.c's st_pcm). 0: none. */
UWORD mx_pcm(WORD *out, UWORD n, ULONG *rate)
{
    ULONG dec = (mx.rate + 4000) / 8000, at, k;
    const WORD *t;
    if (!mx.file || !mx.running || mx.paused || !mx.mix[0] || !mx.rate)
        return 0;
    if (!dec)
        dec = 1;
    at = time_ms() - mx.play_t;
    if (at > 1000)
        at = 1000;
    at = at * mx.rate / 1000;
    if (at > HALF)
        at = HALF;
    if (at < (ULONG)n * dec)
        at = (ULONG)n * dec;
    t = mx.mix[mx.cur & 1];
    for (k = 0; k < n; k++) {
        ULONG i = at - (n - k) * dec;
        out[k] = (WORD)(((LONG)t[2 * i] + t[2 * i + 1]) >> 1);     /* (full scale, as the voices mix) */
    }
    *rate = mx.rate / dec;
    return n;
}

/* The last n sample frames mixed, left and right interleaved, 16-bit (amphost.c) */
UWORD mx_pcm_lr(WORD *lr, UWORD n, ULONG *rate)
{
    ULONG at, k;
    const WORD *t;
    if (!mx.file || !mx.running || mx.paused || !mx.mix[0] || !mx.rate)
        return 0;
    if (n > HALF)
        n = HALF;
    at = time_ms() - mx.play_t;
    if (at > 1000)
        at = 1000;
    at = at * mx.rate / 1000;
    if (at > HALF)
        at = HALF;
    if (at < n)
        at = n;
    t = mx.mix[mx.cur & 1];
    for (k = 0; k < n; k++) {
        ULONG i = at - n + k;
        lr[2 * k] = t[2 * i];
        lr[2 * k + 1] = t[2 * i + 1];
    }
    *rate = mx.rate;
    return n;
}
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
