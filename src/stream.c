/*
 * AmigaMCE - internet radio: an MP3 stream decoded and played on Paula
 * Copyright (c) 2026 Brian King
 *
 * A station's stream is an endless MP3 file sent over HTTP (Icecast and
 * SHOUTcast servers; SHOUTcast answers "ICY 200 OK"). A process of its own
 * ("AmigaMCE playback") connects, asks for the stream with its titles
 * ("Icy-MetaData: 1": every icy-metaint bytes a little block saying what
 * plays), and gives the MP3 to mpega.library through a bitstream hook:
 * the decoder asks for bytes, the hook reads them off the socket and takes
 * the title blocks out. What it decodes, 16-bit, goes into a FIFO as 8-bit
 * samples; Paula plays them from a double buffer in chip memory, as for
 * the mixed modules (mixplay.c): channel 0's audio interrupt, at the start
 * of each half, points Paula at the other one and wakes the process to
 * fill the half just played from the FIFO.
 *
 * Decoding MP3 is a lot of work for a 68030: the decoder is told to make
 * less of it on slower CPUs (mono, a lower rate, its low quality), and
 * never more than Paula plays (28 kHz). Playing starts once a second or
 * so is in the FIFO, so the network's hiccups go unheard.
 *
 * A track (a Plex server's MP3 file, st_start's finite) is the same, with
 * an end: the connection closing after its last byte is that end, not a
 * failure, and what is in the FIFO plays out. Its ID3v2 tag (up to a
 * picture of the cover, often hundreds of KB) is read past first.
 *
 * The process has its own bsdsocket.library (each process opens its own);
 * net.c's SocketBase is the network process's, so this file names its own
 * base for the library's inline calls. An https: stream (Settings >
 * Networking > HTTPS on, AmiSSL installed) goes through AmiSSL (tls.c),
 * which the process likewise opens for itself, the first time one plays.
 */
#include "amce.h"
#include "radiocsv.h"
#include "zz9k.h"
#include "tspack.h"
#include <stddef.h>
#include <libraries/mpegv.h>
#include <proto/mpegv.h>
#include "vprobe.h"
#include "dvdplay.h"
#include "spu.h"
#include "video.h"
#include <libraries/ac3.h>
#include <proto/ac3.h>

#include <string.h>
#include <stdio.h>
#include <exec/interrupts.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <utility/hooks.h>
#include <devices/ahi.h>
#include <libraries/mhi.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <devices/timer.h>
#include <proto/mhi.h>
#include <proto/ahi.h>
#define SocketBase st_SocketBase
#include <proto/bsdsocket.h>
#include <sys/filio.h>

struct Library *st_SocketBase;      /* the playback process's */
static struct TlsLib st_tls;        /* AmiSSL, the playback process's (tls.c) */
static APTR st_ssl;                 /* the stream's TLS, NULL plain HTTP */
static volatile BOOL https_on;      /* Settings > Networking > HTTPS (with AmiSSL) */
static volatile BOOL secure_now;    /* the stream's own connection is TLS (st_secure) */

void st_use_https(BOOL on)
{
    https_on = on;
}

/* Time the playback process spends asleep, all told (Debug's load line says
 * how busy it is: st_busy_sample) */
static volatile ULONG slept_ms;

static ULONG st_wait(ULONG mask)
{
    ULONG t = time_ms(), r = Wait(mask);
    slept_ms += time_ms() - t;
    return r;
}

static void st_delay(LONG ticks)
{
    ULONG t = time_ms();
    Delay(ticks);
    slept_ms += time_ms() - t;
}

static struct Library *MPEGABase;

#define CU ((volatile struct Custom *)0xDFF000)
#define PAULA_CLOCK 3546895UL       /* PAL */
#define HALF        2048            /* samples in each half of Paula's buffer */
/* The FIFO: samples of each channel decoded ahead (a power of 2), and how
 * long in it before Paula starts: Settings > Networking > Stream buffer
 * (short, normal, long; st_set_buffer). At 22050 Hz the FIFO holds 1.5,
 * 3 or 6 seconds. */
static const ULONG fifo_sizes[3] = { 32768UL, 65536UL, 131072UL };
static const ULONG prebuf_ms[3] = { 600, 1200, 3000 };
static int buf_level = 1;
#define FIFO_N      st.fifo_n
#define NETBUF      8192
#define CONNECT_S   10
#define READ_S      15

/* ---------------------------------------------------------- mpega.library
 * Stephane Tavenard's MPEG audio decoder; no C header here, so its
 * structures (word-aligned, as the 68k compilers of its day laid them out)
 * and calls, from its documentation. */
typedef struct { WORD freq_div, quality; LONG freq_max; } MPEGA_OUTPUT;
typedef struct { WORD force_mono; MPEGA_OUTPUT mono, stereo; } MPEGA_LAYER;
typedef struct {
    struct Hook *bs_access;
    MPEGA_LAYER layer_1_2, layer_3;
    WORD check_mpeg;
    LONG stream_buffer_size;
} MPEGA_CTRL;
typedef struct {
    WORD norm, layer, mode, bitrate;
    LONG frequency;
    WORD channels;
    ULONG ms_duration;
    WORD private_bit, copyright, original;
    WORD dec_channels, dec_quality;
    LONG dec_frequency;
    APTR handle;
} MPEGA_STREAM;
typedef struct {
    LONG func;
    union {
        struct { char *stream_name; LONG buffer_size, stream_size; } open;
        struct { APTR buffer; LONG num_bytes; } read;
        struct { LONG abs_byte_seek_pos; } seek;
    } data;
} MPEGA_ACCESS;
#define MPEGA_BSFUNC_OPEN  0
#define MPEGA_BSFUNC_CLOSE 1
#define MPEGA_BSFUNC_READ  2
#define MPEGA_BSFUNC_SEEK  3
#define MPEGA_PCM_SIZE     1152

#define MPEGA_open_(n, c) \
    LP2(30, MPEGA_STREAM *, MPEGA_open, char *, n, a0, MPEGA_CTRL *, c, a1, , MPEGABase)
#define MPEGA_close_(s) LP1NR(36, MPEGA_close, MPEGA_STREAM *, s, a0, , MPEGABase)
#define MPEGA_decode_frame_(s, p) \
    LP2(42, LONG, MPEGA_decode_frame, MPEGA_STREAM *, s, a0, WORD **, p, a1, , MPEGABase)
/* the ZZ9000 SDK's mpega.library streams to the card: each call under the
 * card lock (amce_card_lock: one task at a time) */
#define MPEGA_open(n, c) \
    ({ MPEGA_STREAM *r_; amce_card_lock(); r_ = MPEGA_open_(n, c); amce_card_unlock(); r_; })
#define MPEGA_close(s) \
    do { amce_card_lock(); MPEGA_close_(s); amce_card_unlock(); } while (0)
#define MPEGA_decode_frame(s, p) \
    ({ LONG r_; amce_card_lock(); r_ = MPEGA_decode_frame_(s, p); amce_card_unlock(); r_; })

typedef char ctrl_size_check[sizeof(MPEGA_CTRL) == 46 ? 1 : -1];

/* ---------------------------------------------------------- flacdec.library
 * Renaud Schweingruber's FLAC decoder (dr_flac by David Reid, its loops in
 * 68020 assembler), on Aminet as util/libs/flacdec. Its archive goes
 * about unchanged only, so its interface is declared here from its
 * documentation: decoders read through a hook of ours (A0 the hook, A2 the
 * decoder, A1 the message), and give 16-bit frames, channels interleaved. */
static struct Library *FLACDecBase;
struct FLACDecInfo {
    ULONG fdi_SampleRate;
    UWORD fdi_Channels, fdi_BitsPerSample;
    ULONG fdi_TotalFramesHi, fdi_TotalFramesLo;
    ULONG fdi_Reserved[4];
};
struct FLACDecIO {
    ULONG fio_Method;
    APTR fio_Buffer;
    LONG fio_Length, fio_Mode;
};
#define FDIO_READ 1
#define FDIO_SEEK 2
#define FDIO_TELL 3
#define FLACDecOpen(h, i) \
    LP2(30, APTR, FLACDecOpen, struct Hook *, h, a0, struct FLACDecInfo *, i, a1, , FLACDecBase)
#define FLACDecClose(d) LP1NR(42, FLACDecClose, APTR, d, a0, , FLACDecBase)
#define FLACDecRead(d, b, n) \
    LP3(48, LONG, FLACDecRead, APTR, d, a0, WORD *, b, a1, LONG, n, d0, , FLACDecBase)
#define FLACDecSeek(d, f) LP2(54, BOOL, FLACDecSeek, APTR, d, a0, ULONG, f, d0, , FLACDecBase)
typedef char access_size_check[sizeof(MPEGA_ACCESS) == 16 ? 1 : -1];

/* ---------------------------------------------------------- the state */
static struct {
    /* set before the process starts, then its */
    char url[2048];                 /* a Plex conversion's is long */
    struct Process *proc;
    volatile BOOL quit;
    volatile BYTE sig;              /* the process's: Paula wants a half */
    /* the connection (the process's) */
    LONG sock;
    UBYTE *nb;                      /* bytes read, not yet given to the decoder */
    LONG nb_pos, nb_len;
    LONG metaint, meta_left;        /* title blocks: every metaint bytes (0 none) */
    /* what is known, for the UI (strings written whole, then their seq bumped) */
    volatile UBYTE state;
    char err[96];
    char name[64], genre[48], title[128];
    volatile ULONG title_seq;
    LONG bitrate, freq, channels, dec_freq, dec_channels;
    /* the FIFO: decoded samples, 16-bit, left and right (mono: left only) */
    WORD *fl, *fr;
    ULONG fifo_n;                   /* their size (FIFO_N) */
    volatile ULONG f_in, f_out;     /* written, played (they run on) */
    volatile ULONG underruns;
    /* Paula */
    BYTE *chip;                     /* left 0, left 1, right 0, right 1 */
    UWORD period;
    volatile UBYTE cur, want;
    volatile BOOL started, paused;
    struct Interrupt aint;
    struct Interrupt *old_aint;
    BOOL int_set;
    UWORD master;
    volatile ULONG played;          /* samples Paula has been given */
    volatile ULONG play_t;          /* when the buffer playing now began (spectrum) */
    volatile UBYTE ahi_play;        /*   AHI: which of its two that is */
    volatile UBYTE peak_l, peak_r;
    UWORD cpu;                      /* 20, 30, 40, 60: how much decoding it can do */
    BOOL finite;                    /* a track: its end is the end, not a failure */
    ULONG mp3_bytes;                /* MP3 given to the decoder: its average bitrate */
    ULONG range_from;               /* a track from this byte on (a seek; 0 all of it) */
    ULONG clen;                     /* a track's whole length in bytes, 0 not known */
    BPTR fh;                        /* a music file on this Amiga (local): read, not received */
    BOOL local;
    ULONG len_ms;                   /* a track's length as the stream says it, 0 not known */
    ULONG fpos;                     /* FLAC: where in the file the decoder has read to */
    ULONG skip_ms;                  /* a conversion from this time on (a seek): asked of
                                     * the server (its offset), else frames passed over */
    BOOL skipping;                  /* the server would not: passed over here */
    volatile BOOL eof;              /* a track's last byte read */
    BOOL ahi;                       /* played through AHI (16-bit), not Paula */
    BOOL mhi;                       /* MP3 decoded and played by an MHI driver */
    BOOL flac;                      /* decoded here by flacdec.library */
    BOOL icy_br;                    /* the station said its bitrate (icy-br) */
    /* a video (MPEG on the ZZ9000's overlay: video_run); its sound as music's */
    BOOL video, v_mpeg2, v_mp2, v_es;   /* (v_mp2: its sound the card's, MPEG audio or AC3) */
    BOOL v_sw;                      /* decoded by the CPU (mpegv.library), not the card */
    ULONG v_hz;                     /* its sound's rate (the probe's) */
    BOOL v_ac3;                     /* ...AC3 */
    BOOL v_ac3cpu;                  /* its AC3 decoded by the CPU (ac3.library), not the card */
    /* for its information (st_video_stats; the "i" key) */
    volatile ULONG v_bytes;         /* the file's (disc's) bytes read so far */
    volatile LONG v_drift;          /* the last picture's time less the sound's (ms) */
    volatile ULONG v_bskipped;      /* B pictures passed over (the CPU's) */
    volatile ULONG v_akbps;         /* its sound's bit rate (kbit/s), 0 not known */
    volatile UBYTE v_adec;          /* who decodes it: VSD_ */
    volatile UBYTE v_achans, v_alfe;    /* its own channels (AC3's 5.1: 5 and 1) */
    ULONG v_probe_kbps;             /* (the probe's, for MPEG audio) */
    ULONG v_bps;                    /* its bytes a second (the probe's; 0 not known) */
    UBYTE v_ts;                     /* a transport stream (its packet size): packed as it goes */
    UWORD v_vpid, v_apid;
    ULONG v_tsfirst;
    volatile BOOL v_hidden;         /* its overlay down: pictures let go on time, unseen */
    volatile ULONG v_ms;            /* where it is: the last picture's time from the first's */
    UWORD v_w, v_h;
    volatile ULONG v_shown, v_dropped;
    struct DvdPlay *v_dvd;          /* a DVD's film (dvdplay.c), read from there, not a file */
} st;

static struct {                     /* st_start_video's */
    struct DvdPlay *dvd;
    BOOL on, mpeg2, mp2, es, ac3, sw, ac3cpu;
    UBYTE ts, achans;
    ULONG hz, akbps, bps;
    UWORD w, h, vpid, apid;
    ULONG tsfirst;
} vid_next;

static BOOL use_ahi;                /* Settings > Sound: AHI (st_use_ahi) */
static char mhi_driver[160];        /* Settings > Sound: MP3 by this MHI driver ("" mpega) */
static BOOL card_mpega;             /* mpega.library is the ZZ9000 SDK's: it decodes on the card */
struct Library *MHIBase;            /* the MHI driver's (the playback process's, while it plays) */

/* ---------------------------------------------------------- Paula */
/* A half of Paula's buffer from the FIFO (the process, when woken). */
static void fill_half(int half)
{
    BYTE *l = st.chip + half * HALF, *r = st.chip + (2 + half) * HALF;
    ULONG have = st.f_in - st.f_out, n = have < HALF ? have : HALF, i, o = st.f_out;
    UBYTE pl = 0, pr = 0;
    BOOL stereo = st.dec_channels == 2;
    for (i = 0; i < n; i++, o++) {
        BYTE a = (BYTE)(st.fl[o & (FIFO_N - 1)] >> 8);
        BYTE b = stereo ? (BYTE)(st.fr[o & (FIFO_N - 1)] >> 8) : a;
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
    if (n < HALF && st.state == ST_PLAYING)
        st.underruns++;
    st.f_out += n;
    st.played += n;
    if (pl > st.peak_l)
        st.peak_l = pl;
    if (pr > st.peak_r)
        st.peak_r = pr;
}

/* Channel 0's audio interrupt: Paula has taken a half and plays it. */
static void audio_code(void)
{
    UBYTE next;
    CU->intreq = INTF_AUD0;
    st.cur ^= 1;
    st.play_t = time_ms();                  /* (ReadEClock: callable here) */
    next = (UBYTE)(st.cur ^ 1);
    CU->aud[0].ac_ptr = (UWORD *)(st.chip + next * HALF);
    CU->aud[1].ac_ptr = (UWORD *)(st.chip + (2 + next) * HALF);
    st.want = next;
    if (st.proc && st.sig >= 0)
        Signal(&st.proc->pr_Task, 1UL << st.sig);
}

static void paula_start(void)
{
    int i;
    ULONG rate = st.dec_freq ? (ULONG)st.dec_freq : 22050;
    st.period = (UWORD)((PAULA_CLOCK + rate / 2) / rate);
    if (st.period < 124)
        st.period = 124;
    fill_half(0);
    st.cur = 1;                             /* the first interrupt: half 0 starts */
    st.want = 1;
    st.aint.is_Node.ln_Type = NT_INTERRUPT;
    st.aint.is_Node.ln_Pri = 0;
    st.aint.is_Node.ln_Name = (char *)"AmigaMCE playback";
    st.aint.is_Data = NULL;
    st.aint.is_Code = (void (*)())audio_code;
    CU->intena = INTF_AUD0;
    CU->intreq = INTF_AUD0;
    st.old_aint = SetIntVector(INTB_AUD0, &st.aint);
    st.int_set = TRUE;
    for (i = 0; i < 2; i++) {
        CU->aud[i].ac_ptr = (UWORD *)(st.chip + (i ? 2 : 0) * HALF);
        CU->aud[i].ac_len = HALF / 2;
        CU->aud[i].ac_per = st.period;
        CU->aud[i].ac_vol = st.paused ? 0 : st.master;
    }
    CU->aud[2].ac_vol = 0;
    CU->aud[3].ac_vol = 0;
    st.started = TRUE;
    CU->intena = INTF_SETCLR | INTF_AUD0;
    CU->dmacon = DMAF_SETCLR | DMAF_AUD0 | DMAF_AUD1;
}

static void paula_stop(void)
{
    CU->intena = INTF_AUD0;
    CU->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    CU->intreq = INTF_AUD0;
    CU->aud[0].ac_vol = 0;
    CU->aud[1].ac_vol = 0;
    if (st.int_set) {
        SetIntVector(INTB_AUD0, st.old_aint);
        st.int_set = FALSE;
    }
    st.started = FALSE;
}

/* ---------------------------------------------------------- AHI
 * Settings > Sound > AHI: AHI's own default, its Music unit's mode and
 * mixing rate as AHI's Preferences set them (as players have it: the
 * library's interface, ahi.device opened on no unit; ahi.device's unit 0
 * is often left on a Paula mode at a low rate). Fed 16-bit stereo from two
 * buffers, each one dynamic sample: as one starts (its sound hook) the
 * other is queued after it and the process woken to fill it. */
#define AHI_FRAMES 4096
struct Library *AHIBase;
static struct {
    struct MsgPort *port;
    struct AHIRequest *io;
    struct AHIAudioCtrl *ctrl;
    struct Hook hook;
    struct Task *task;
    WORD *buf[2];
    BYTE sig, old_sig, queued;      /* queued: the sound queued after this one, -1 none yet */
    BOOL open, playing;
    volatile UBYTE fill_due;        /* bit k: buffer k to be filled (it is queued next) */
    UWORD vol;
} ah;

/* Buffer k filled from the FIFO (silence when paused or short). */
static void ahi_fill(int k)
{
    WORD *b = ah.buf[k];
    ULONG have = st.paused ? 0 : st.f_in - st.f_out, n = have < AHI_FRAMES ? have : AHI_FRAMES;
    ULONG i, o = st.f_out;
    UWORD pl = 0, pr = 0;
    BOOL stereo = st.dec_channels == 2;
    for (i = 0; i < n; i++, o++) {
        WORD a = st.fl[o & (FIFO_N - 1)], c = stereo ? st.fr[o & (FIFO_N - 1)] : a;
        UWORD aa = (UWORD)(a < 0 ? -a : a), cc = (UWORD)(c < 0 ? -c : c);
        b[2 * i] = a;
        b[2 * i + 1] = c;
        if (aa > pl)
            pl = aa;
        if (cc > pr)
            pr = cc;
    }
    for (; i < AHI_FRAMES; i++)
        b[2 * i] = b[2 * i + 1] = 0;
    if (n < AHI_FRAMES && !st.paused && st.state == ST_PLAYING)
        st.underruns++;
    st.f_out += n;
    st.played += n;
    if ((pl >> 8) > st.peak_l)
        st.peak_l = (UBYTE)(pl >> 8);
    if ((pr >> 8) > st.peak_r)
        st.peak_r = (UBYTE)(pr >> 8);
}

/* AHI's sound hook (its interrupt): a buffer starts playing; the other,
 * just played, is queued after it and the process woken to fill it. */
static ULONG ahi_soundfunc(register struct Hook *h __asm("a0"),
                           register struct AHISoundMessage *sm __asm("a1"),
                           register struct AHIAudioCtrl *a __asm("a2"))
{
    BYTE k = ah.queued < 0 ? 0 : ah.queued;     /* the one starting now */
    (void)h;
    (void)sm;
    st.ahi_play = (UBYTE)k;
    ah.queued = (BYTE)(k ^ 1);
    AHI_SetSound(0, (UWORD)(k ^ 1), 0, 0, a, AHISF_NONE);
    ah.fill_due |= (UBYTE)(1 << (k ^ 1));
    Signal(ah.task, 1UL << ah.sig);
    return 0;
}

/* The process woken: the buffer queued next filled; the volume as set. */
static void ahi_serve(void)
{
    UBYTE due;
    int k;
    Disable();
    due = ah.fill_due;
    ah.fill_due = 0;
    Enable();
    if (due)
        st.play_t = time_ms();              /* (one began about now: the spectrum's clock) */
    for (k = 0; k < 2; k++)
        if (st.started && (due & (1 << k)))
            ahi_fill(k);
    if (ah.ctrl && ah.vol != st.master) {
        ah.vol = st.master;
        AHI_SetVol(0, (Fixed)(((ULONG)st.master << 16) / 64), 0x8000, ah.ctrl, AHISF_IMM);
    }
}

/* How AHI's mode mixes us (logged once): stereo and panning, its
 * centred channel half each side, as loud as mpega to Paula. */
static void ahi_level_log(void)
{
    static BOOL said;
    struct TagItem t[5];
    ULONG stereo = 0, pan = 0, hifi = 0, maxch = 0;
    char m[120];
    if (said || !ah.ctrl)
        return;
    said = TRUE;
    t[0].ti_Tag = AHIDB_Stereo;
    t[0].ti_Data = (ULONG)&stereo;
    t[1].ti_Tag = AHIDB_Panning;
    t[1].ti_Data = (ULONG)&pan;
    t[2].ti_Tag = AHIDB_HiFi;
    t[2].ti_Data = (ULONG)&hifi;
    t[3].ti_Tag = AHIDB_MaxChannels;
    t[3].ti_Data = (ULONG)&maxch;
    t[4].ti_Tag = TAG_DONE;
    AHI_GetAudioAttrsA(AHI_INVALID_ID, ah.ctrl, t);
    sprintf(m, "AHI's mode: %s%s%s, %lu channels at most", stereo ? "stereo" : "mono",
            pan ? ", panning" : "", hifi ? ", HiFi" : "", (unsigned long)maxch);
    amce_log("sound", m);
}

static BOOL ahi_start_(void)
{
    struct TagItem t[8];
    struct AHISampleInfo si;
    char m[160], name[64];
    ULONG mix = 0;
    int k;
    memset(&ah, 0, sizeof(ah));
    ah.sig = -1;
    ah.queued = -1;
    if (!(ah.port = CreateMsgPort()) ||
        !(ah.io = (struct AHIRequest *)CreateIORequest(ah.port, sizeof(struct AHIRequest))))
        return FALSE;
    ah.buf[0] = AllocVec(AHI_FRAMES * 4, MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR);
    ah.buf[1] = AllocVec(AHI_FRAMES * 4, MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR);
    if (!ah.buf[0] || !ah.buf[1] || (ah.sig = AllocSignal(-1)) < 0)
        return FALSE;
    ah.io->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)ah.io, 0))
        return FALSE;
    ah.open = TRUE;
    AHIBase = (struct Library *)ah.io->ahir_Std.io_Device;
    ah.task = FindTask(NULL);
    ah.hook.h_Entry = (HOOKFUNC)(APTR)ahi_soundfunc;
    t[0].ti_Tag = AHIA_AudioID;
    t[0].ti_Data = AHI_DEFAULT_ID;          /* the Music unit's mode */
    t[1].ti_Tag = AHIA_MixFreq;
    t[1].ti_Data = AHI_DEFAULT_FREQ;        /* ...and its rate */
    t[2].ti_Tag = AHIA_Channels;
    t[2].ti_Data = 1;
    t[3].ti_Tag = AHIA_Sounds;
    t[3].ti_Data = 2;
    t[4].ti_Tag = AHIA_SoundFunc;
    t[4].ti_Data = (ULONG)&ah.hook;
    t[5].ti_Tag = TAG_DONE;
    if (!(ah.ctrl = AHI_AllocAudioA(t)))
        return FALSE;
    for (k = 0; k < 2; k++) {
        si.ahisi_Type = AHIST_S16S;
        si.ahisi_Address = ah.buf[k];
        si.ahisi_Length = AHI_FRAMES;
        if (AHI_LoadSound((UWORD)k, AHIST_DYNAMICSAMPLE, &si, ah.ctrl) != AHIE_OK)
            return FALSE;
    }
    ah.old_sig = st.sig;
    st.sig = ah.sig;                        /* its hook wakes the process */
    st.started = TRUE;
    ahi_fill(0);                            /* the first; the second asked for as it starts */
    t[0].ti_Tag = AHIC_Play;
    t[0].ti_Data = TRUE;
    t[1].ti_Tag = TAG_DONE;
    if (AHI_ControlAudioA(ah.ctrl, t) != AHIE_OK)
        return FALSE;
    ah.playing = TRUE;
    ah.vol = st.master;
    t[0].ti_Tag = AHIP_BeginChannel;
    t[0].ti_Data = 0;
    t[1].ti_Tag = AHIP_Freq;
    t[1].ti_Data = st.dec_freq ? (ULONG)st.dec_freq : 22050;
    t[2].ti_Tag = AHIP_Vol;
    t[2].ti_Data = (((ULONG)st.master << 16) / 64);
    t[3].ti_Tag = AHIP_Pan;
    t[3].ti_Data = 0x8000;
    t[4].ti_Tag = AHIP_Sound;
    t[4].ti_Data = 0;
    t[5].ti_Tag = AHIP_EndChannel;
    t[5].ti_Data = 0;
    t[6].ti_Tag = TAG_DONE;
    AHI_PlayA(ah.ctrl, t);
    st.play_t = time_ms();
    /* what it plays on: the mode allocated, its mixing rate */
    name[0] = 0;
    t[0].ti_Tag = AHIDB_Name;
    t[0].ti_Data = (ULONG)name;
    t[1].ti_Tag = AHIDB_BufferLen;
    t[1].ti_Data = sizeof(name);
    t[2].ti_Tag = TAG_DONE;
    if (!AHI_GetAudioAttrsA(AHI_INVALID_ID, ah.ctrl, t))
        name[0] = 0;
    t[0].ti_Tag = AHIC_MixFreq_Query;
    t[0].ti_Data = (ULONG)&mix;
    t[1].ti_Tag = TAG_DONE;
    AHI_ControlAudioA(ah.ctrl, t);
    sprintf(m, "AHI's default mode (its Music unit's): %.50s, mixing at %lu Hz; given %ld Hz "
               "16-bit %s",
            name[0] ? name : "?", (unsigned long)mix, (long)(st.dec_freq ? st.dec_freq : 22050),
            st.dec_channels == 1 ? "mono" : "stereo");
    amce_log("sound", m);
    ahi_level_log();
    return TRUE;
}

/* AHI set up (ZZ9000AX: on the card) under the card lock */
static BOOL ahi_start(void)
{
    BOOL r;
    amce_card_lock();
    r = ahi_start_();
    amce_card_unlock();
    return r;
}

static void ahi_stop_(void);

static void ahi_stop(void)
{
    amce_card_lock();
    ahi_stop_();
    amce_card_unlock();
}

static void ahi_stop_(void)
{
    st.started = FALSE;
    if (ah.ctrl) {
        if (ah.playing) {
            struct TagItem t[2];
            t[0].ti_Tag = AHIC_Play;
            t[0].ti_Data = FALSE;
            t[1].ti_Tag = TAG_DONE;
            AHI_ControlAudioA(ah.ctrl, t);
        }
        AHI_FreeAudio(ah.ctrl);             /* (its sounds unloaded with it) */
    }
    if (ah.open)
        CloseDevice((struct IORequest *)ah.io);
    AHIBase = NULL;
    if (ah.io)
        DeleteIORequest((struct IORequest *)ah.io);
    if (ah.port)
        DeleteMsgPort(ah.port);
    if (ah.sig >= 0) {
        if (st.sig == ah.sig)
            st.sig = ah.old_sig;
        FreeSignal(ah.sig);
    }
    if (ah.buf[0])
        FreeVec(ah.buf[0]);
    if (ah.buf[1])
        FreeVec(ah.buf[1]);
    memset(&ah, 0, sizeof(ah));
    ah.sig = -1;
}

/* The output: Paula's or AHI's, started once the prebuffer is in. */
static void out_start(void)
{
    if (st.ahi) {
        if (!ahi_start()) {
            ahi_stop();
            if (!st.err[0])
                amce_strlcpy(st.err, _("AHI could not be opened (its Preferences: a mode?)"),
                             sizeof(st.err));
            st.state = ST_FAILED;
            st.quit = TRUE;
        }
        return;
    }
    paula_start();
}

static void out_stop(void)
{
    if (st.ahi)
        ahi_stop();
    else if (st.started)
        paula_stop();
}

/* The output wants more: Paula a half, AHI a request back. */
static void out_take(void)
{
    if (st.ahi)
        ahi_serve();
    else
        fill_half(st.want);
}

/* ---------------------------------------------------------- the network */
static void fail(const char *why)
{
    if (!st.err[0]) {
        amce_strlcpy(st.err, why, sizeof(st.err));
        if (st.local)
            amce_log("track: not played", why);     /* a file here: why, for the log */
    }
    st.state = ST_FAILED;
}

/* Paula wants a half: given it now (from wherever the process waits). */
static void mhi_serve(void);
static void titles_due(void);

static void serve(void)
{
    titles_due();                           /* a title whose audio has come round */
    if (st.mhi) {
        mhi_serve();                        /* MHI's buffers back, a stall restarted */
        return;
    }
    if (st.started && (SetSignal(0, 1UL << st.sig) & (1UL << st.sig)))
        out_take();
}

/* Until s can be read or written (1), timed out (0), stopping (-1); Paula
 * served meanwhile. */
static int wait_sock(LONG s, BOOL wr, LONG secs)
{
    ULONG deadline = time_ms() + (ULONG)secs * 1000;
    for (;;) {
        fd_set fs;
        struct timeval tv;
        ULONG sigs = (st.mhi ? 0 : 1UL << st.sig) | SIGBREAKF_CTRL_C;
        LONG r;
        if (st.quit)
            return -1;
        FD_ZERO(&fs);
        FD_SET(s, &fs);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        {
            ULONG t = time_ms();
            r = WaitSelect(s + 1, wr ? NULL : &fs, wr ? &fs : NULL, NULL, &tv, &sigs);
            slept_ms += time_ms() - t;
        }
        if (sigs & (1UL << st.sig))
            SetSignal(1UL << st.sig, 1UL << st.sig);    /* serve() takes it */
        serve();
        if (st.quit || (sigs & SIGBREAKF_CTRL_C))
            return -1;
        if (r > 0)
            return 1;
        if (r < 0 || (LONG)(time_ms() - deadline) >= 0)
            return 0;
    }
}

static BOOL split_url(const char *url, char *host, LONG hmax, UWORD *port, char *path, LONG pmax)
{
    const char *h, *slash, *colon;
    LONG n;
    BOOL secure = !amce_strnicmp(url, "https://", 8);
    if (amce_strnicmp(url, "http://", 7) && !secure)
        return FALSE;
    h = url + (secure ? 8 : 7);
    slash = strchr(h, '/');
    n = slash ? slash - h : (LONG)strlen(h);
    colon = memchr(h, ':', n);
    *port = secure ? 443 : 80;
    if (colon) {
        LONG v = 0;
        const char *c = colon + 1;
        while (c < h + n && *c >= '0' && *c <= '9')
            v = v * 10 + (*c++ - '0');
        *port = (UWORD)v;
        n = colon - h;
    }
    if (n <= 0 || n >= hmax)
        return FALSE;
    memcpy(host, h, n);
    host[n] = 0;
    amce_strlcpy(path, slash ? slash : "/", pmax);
    return TRUE;
}

static LONG connect_to(const char *host, UWORD port)
{
    struct hostent *h;
    struct sockaddr_in sa;
    LONG s, one = 1, e = 0, el = sizeof(e);
    ULONG a = inet_addr((STRPTR)host);
    if (a == 0xFFFFFFFFUL) {
        if (!(h = gethostbyname((STRPTR)host)) || h->h_length != 4) {
            fail(_("The station's server could not be found (DNS)"));
            return -1;
        }
        memcpy(&a, h->h_addr_list[0], 4);
    }
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fail(_("No socket (is the TCP/IP stack running?)"));
        return -1;
    }
    IoctlSocket(s, FIONBIO, (char *)&one);
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = port;
    memcpy(&sa.sin_addr, &a, 4);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 && Errno() != EINPROGRESS) {
        CloseSocket(s);
        fail(_("The station refused the connection"));
        return -1;
    }
    if (wait_sock(s, TRUE, CONNECT_S) <= 0 ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
        CloseSocket(s);
        fail(st.quit ? _("Stopped") : _("The station did not answer"));
        return -1;
    }
    return s;
}

/* The connection closed (its TLS first). */
static void sock_close(void)
{
    if (st_ssl)
        tls_free(&st_tls, st_ssl);
    st_ssl = NULL;
    secure_now = FALSE;
    if (st.sock >= 0)
        CloseSocket(st.sock);
    st.sock = -1;
}

/* Connected to host (TLS's handshake too, secure): st.sock, FALSE failed. */
static BOOL sock_open(const char *host, UWORD port, BOOL secure)
{
    char m[200];
    sock_close();
    if ((st.sock = connect_to(host, port)) < 0)
        return FALSE;
    if (!secure)
        return TRUE;
    if (!tls_open(&st_tls, st_SocketBase)) {
        amce_log("radio: https", st_tls.why);
        sock_close();
        fail(_("HTTPS needs AmiSSL 5"));
        return FALSE;
    }
    if (!(st_ssl = tls_new(&st_tls, st.sock, host, FALSE))) {
        sock_close();
        fail(_("Not enough memory"));
        return FALSE;
    }
    for (;;) {
        int r = tls_handshake(&st_tls, st_ssl), w;
        if (r == 1) {
            secure_now = TRUE;
            return TRUE;
        }
        if (r != TLS_WANT_READ && r != TLS_WANT_WRITE) {
            sprintf(m, "%.60s: %.120s", host, st_tls.why);
            amce_log("radio: https", m);
            sock_close();
            fail(_("The secure connection failed"));
            return FALSE;
        }
        if ((w = wait_sock(st.sock, r == TLS_WANT_WRITE, CONNECT_S)) <= 0) {
            sock_close();
            fail(w < 0 ? _("Stopped") : _("The station did not answer"));
            return FALSE;
        }
    }
}

/* The request sent whole: FALSE failed. */
static BOOL sock_send(const char *req)
{
    LONG left = strlen(req), off = 0;
    while (left > 0) {
        LONG w;
        if (wait_sock(st.sock, TRUE, READ_S) <= 0)
            return FALSE;
        if (st_ssl) {
            w = tls_write(&st_tls, st_ssl, req + off, left);
            if (w == TLS_WANT_READ || w == TLS_WANT_WRITE)
                continue;
        } else {
            w = send(st.sock, (APTR)(req + off), left, 0);
        }
        if (w <= 0)
            return FALSE;
        off += w;
        left -= w;
    }
    return TRUE;
}

/* More bytes into the network buffer (after what is there): FALSE at the
 * stream's end or on a failure. */
static BOOL net_more(void)
{
    LONG r;
    if (st.nb_pos > 0 && st.nb_pos == st.nb_len)
        st.nb_pos = st.nb_len = 0;
    if (st.nb_pos > NETBUF / 2) {
        memmove(st.nb, st.nb + st.nb_pos, st.nb_len - st.nb_pos);
        st.nb_len -= st.nb_pos;
        st.nb_pos = 0;
    }
    if (st.fh) {
        r = Read(st.fh, st.nb + st.nb_len, NETBUF - st.nb_len);
        if (r <= 0) {
            if (r == 0)
                st.eof = TRUE;              /* the file's end */
            else
                fail(_("The file could not be read"));
            return FALSE;
        }
        st.nb_len += r;
        return TRUE;
    }
    for (;;) {
        int w = st_ssl && tls_pending(&st_tls, st_ssl) > 0 ? 1 : wait_sock(st.sock, FALSE, READ_S);
        if (w <= 0) {
            fail(w < 0 ? _("Stopped") : _("The station stopped sending"));
            return FALSE;
        }
        if (st_ssl) {
            r = tls_read(&st_tls, st_ssl, st.nb + st.nb_len, NETBUF - st.nb_len);
            if (r == TLS_WANT_READ || r == TLS_WANT_WRITE)
                continue;
        } else {
            r = recv(st.sock, st.nb + st.nb_len, NETBUF - st.nb_len, 0);
            if (r < 0 && Errno() == EWOULDBLOCK)
                continue;
        }
        if (r <= 0) {
            if (st.finite && r == 0)
                st.eof = TRUE;              /* a track: all of it read */
            else
                fail(st.finite ? _("The server stopped sending") : _("The station closed the stream"));
            return FALSE;
        }
        st.nb_len += r;
        return TRUE;
    }
}

/* The response's status and headers (into hdr): the status, 0 on failure. */
static LONG read_headers(char *hdr, LONG max)
{
    char *end;
    LONG status = 0;
    st.nb_pos = st.nb_len = 0;
    for (;;) {
        st.nb[st.nb_len] = 0;
        if ((end = strstr((char *)st.nb, "\r\n\r\n")) || (end = strstr((char *)st.nb, "\n\n")))
            break;
        if (st.nb_len >= NETBUF - 1 || !net_more())
            return 0;
    }
    {
        LONG n = end - (char *)st.nb;
        LONG skip = end[0] == '\r' ? 4 : 2;
        if (n >= max)
            n = max - 1;
        memcpy(hdr, st.nb, n);
        hdr[n] = 0;
        st.nb_pos = (end - (char *)st.nb) + skip;  /* the stream's first bytes after */
    }
    {
        char *sp = strchr(hdr, ' ');
        if (sp && sp[1] >= '0' && sp[1] <= '9')
            status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    }
    return status;
}

/* A header's value (case ignored), into out: FALSE if there is none. */
static BOOL header(const char *hdr, const char *name, char *out, LONG max)
{
    LONG n = strlen(name);
    const char *p = hdr;
    while (p && *p) {
        if (!amce_strnicmp(p, name, n) && p[n] == ':') {
            const char *v = p + n + 1, *e;
            while (*v == ' ')
                v++;
            for (e = v; *e && *e != '\r' && *e != '\n'; e++)
                ;
            if (e - v >= max)
                e = v + max - 1;
            memcpy(out, v, e - v);
            out[e - v] = 0;
            return TRUE;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return FALSE;
}

/* A seek's "&offset=" taken out of url: FALSE if it had none. */
static BOOL no_offset;              /* this server refused one: not asked again */
static BOOL drop_offset(char *url)
{
    char *o = strstr(url, "&offset="), *e;
    if (!o)
        return FALSE;
    e = strchr(o + 1, '&');
    if (e)
        memmove(o, e, strlen(e) + 1);
    else
        *o = 0;
    return TRUE;
}

/* What a refusal says (its first line or so of text), for the log. */
static void log_refusal(LONG status)
{
    char m[200];
    LONG n = st.nb_len - st.nb_pos, i, k = 0;
    while (n < 120 && st.nb_len < NETBUF - 1 && net_more())
        n = st.nb_len - st.nb_pos;
    for (i = 0; i < n && k < (LONG)sizeof(m) - 1; i++) {
        char c = (char)st.nb[st.nb_pos + i];
        if (c == '<') {                     /* tags out */
            while (i < n && st.nb[st.nb_pos + i] != '>')
                i++;
            if (k && m[k - 1] != ' ')
                m[k++] = ' ';
            continue;
        }
        m[k++] = c < ' ' ? ' ' : c;
    }
    m[k] = 0;
    squeeze_spaces(m);
    {
        char t[240];
        sprintf(t, "%ld: %.200s", (long)status, m[0] ? m : "(nothing said)");
        amce_log("track: refused", t);
    }
}

/* Asked first, before the stream (st_set_stop_url): Plex told to stop the
 * conversion played before, or it refuses another of that track (400). */
static char stop_url[1024];

void st_set_stop_url(const char *url)
{
    amce_strlcpy(stop_url, url ? url : "", sizeof(stop_url));
}

/* A GET whose answer is not wanted, only that it was asked: quietly, a
 * failure not the stream's. */
static void ping_url(const char *url)
{
    static char path[1024], req[1300];
    char host[96], hdr[512], m[80];
    UWORD port;
    LONG status = 0;
    if (!split_url(url, host, sizeof(host), &port, path, sizeof(path)))
        return;
    if (sock_open(host, port, !amce_strnicmp(url, "https:", 6))) {
        sprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: AmigaMCE/" AMCE_VERSION
                     "\r\nConnection: close\r\n\r\n", path, host);
        if (sock_send(req))
            status = read_headers(hdr, sizeof(hdr));
        sock_close();
    }
    sprintf(m, "the conversion before stopped: %ld", (long)status);
    amce_log("plex", m);
    st.err[0] = 0;                          /* its failure, if it failed, not the stream's */
    st.state = ST_CONNECTING;
    st.nb_pos = st.nb_len = 0;
}

/* Connected to the stream (redirects and playlists followed). */
static BOOL open_stream(void)
{
    static char url[2048], path[2048], req[2600];
    char host[96], hdr[2048], v[160], range[40];
    UWORD port;
    int hop;
    amce_strlcpy(url, st.url, sizeof(url));
    if (st.v_dvd) {
        st.clen = 0;                        /* a DVD: read by dvdplay.c, no file */
        st.nb_pos = st.nb_len = 0;
        return TRUE;
    }
    if (st.local) {
        /* a music file on this Amiga: opened, from range_from on */
        LONG size;
        if (st.fh)
            Close(st.fh);
        if (!(st.fh = Open((CONST_STRPTR)url, MODE_OLDFILE))) {
            fail(_("The file could not be opened"));
            return FALSE;
        }
        Seek(st.fh, 0, OFFSET_END);
        size = Seek(st.fh, st.range_from, OFFSET_BEGINNING);
        st.clen = size > 0 ? (ULONG)size : 0;
        st.metaint = st.meta_left = 0;
        st.nb_pos = st.nb_len = 0;
        return TRUE;
    }
    if (st.skip_ms && no_offset && drop_offset(url))
        st.skipping = TRUE;
    if (stop_url[0]) {
        ping_url(stop_url);
        stop_url[0] = 0;
    }
    for (hop = 0; hop < 4; hop++) {
        LONG status;
        BOOL secure = !amce_strnicmp(url, "https:", 6);
        if (secure && !https_on) {
            fail(_("This station only streams over HTTPS: turn on Settings > Networking > HTTPS"));
            return FALSE;
        }
        if (!split_url(url, host, sizeof(host), &port, path, sizeof(path))) {
            fail(_("The station's address is not one AmigaMCE can follow"));
            return FALSE;
        }
        st.state = ST_CONNECTING;
        if (!sock_open(host, port, secure))
            return FALSE;
        if (st.range_from)
            sprintf(range, "Range: bytes=%lu-\r\n", (unsigned long)st.range_from);
        else
            range[0] = 0;
        sprintf(req,
                "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: AmigaMCE/" AMCE_VERSION
                "\r\nAccept: */*\r\n%s%sConnection: close\r\n\r\n",
                path, host, st.finite ? "" : "Icy-MetaData: 1\r\n", range);
        if (!sock_send(req)) {
            fail(st.quit ? "Stopped" : "The request could not be sent");
            return FALSE;
        }
        if (!(status = read_headers(hdr, sizeof(hdr)))) {
            fail(st.err[0] ? st.err : st.finite ? "The server sent no answer we understand"
                                    : "The station sent no answer we understand");
            return FALSE;
        }
        if (status >= 300 && status < 400 && header(hdr, "Location", v, sizeof(v))) {
            sock_close();
            amce_strlcpy(url, v, sizeof(url));
            continue;
        }
        if (status >= 400 && st.finite)
            log_refusal(status);
        if (status == 400 && st.skip_ms && !st.skipping && drop_offset(url)) {
            /* the server would not start its conversion part way in: from
             * the start, the frames before the time passed over here */
            sock_close();
            st.skipping = no_offset = TRUE;
            amce_log("track", "the server would not start part way in: seeking by reading");
            hop--;
            continue;
        }
        if (status != 200 && !(status == 206 && st.range_from)) {
            sprintf(v, _("%s answered %ld"), st.finite ? _("The server") : _("The station"), (long)status);
            fail(v);
            return FALSE;
        }
        if (header(hdr, "icy-name", v, sizeof(v))) {
            utf8_to_latin1(v);
            squeeze_spaces(v);
            amce_strlcpy(st.name, v, sizeof(st.name));
        }
        if (header(hdr, "icy-genre", v, sizeof(v))) {
            utf8_to_latin1(v);
            squeeze_spaces(v);
            amce_strlcpy(st.genre, v, sizeof(st.genre));
        }
        if (header(hdr, "icy-br", v, sizeof(v))) {
            st.bitrate = amce_num(v);
            st.icy_br = TRUE;
        }
        st.clen = header(hdr, "Content-Length", v, sizeof(v)) ? amce_num(v) + st.range_from : 0;
        st.metaint = header(hdr, "icy-metaint", v, sizeof(v)) ? amce_num(v) : 0;
        st.meta_left = st.metaint;
        if (header(hdr, "Content-Type", v, sizeof(v))) {
            if (strstr(v, "mpegurl") || strstr(v, "scpls") || strstr(v, "x-mpegURL")) {
                /* a playlist: the first http address in it */
                char *p, *e;
                while (st.nb_len < NETBUF - 1 && st.nb_len - st.nb_pos < 1024 && net_more())
                    ;
                st.err[0] = 0;
                st.nb[st.nb_len] = 0;
                {
                    char *q = strstr((char *)st.nb + st.nb_pos, "https://");
                    p = strstr((char *)st.nb + st.nb_pos, "http://");
                    if (q && https_on && (!p || q < p))
                        p = q;              /* the first address, secure too */
                }
                if (!p) {
                    fail(_("The station's playlist has no stream AmigaMCE can play"));
                    return FALSE;
                }
                for (e = p; *e && *e != '\r' && *e != '\n'; e++)
                    ;
                *e = 0;
                amce_strlcpy(url, p, sizeof(url));
                sock_close();
                continue;
            }
            if (!strstr(v, "mpeg") && !strstr(v, "mp3") && !strstr(v, "octet") &&
                !(st.finite && strstr(v, "flac"))) {
                sprintf(req, _("%s sends %s, not MP3"), st.finite ? _("The server") : _("The station"), v);
                fail(req);
                return FALSE;
            }
        }
        return TRUE;
    }
    fail(_("The station sent us on too many times"));
    return FALSE;
}

/* ---- titles, in time with what is heard
 * A title block comes with the stream's bytes as they are read, a few
 * seconds before they are heard (the FIFO's, or MHI's buffers, between):
 * shown then, the title changed before the song did. So each is kept with
 * how much audio had been read when it came (titles_in_ms) and shown once
 * that much has been played (titles_out_ms), by the playback process, which
 * also logs it. */
#define TITLES 4
static struct { ULONG at; char t[160]; } tq[TITLES];
static UWORD tq_head, tq_tail;          /* the playback process's alone */
static ULONG mh_queued_ms(void);
static ULONG mh_ms(void);

static ULONG titles_in_ms(void)
{
    ULONG r = st.dec_freq ? (ULONG)st.dec_freq : 0;
    if (st.mhi)
        return mh_queued_ms();
    return r ? (st.f_in / r) * 1000 + (st.f_in % r) * 1000 / r : 0;
}

static ULONG titles_out_ms(void)
{
    ULONG r = st.dec_freq ? (ULONG)st.dec_freq : 0;
    if (!st.started)
        return 0;
    if (st.mhi)
        return mh_ms();
    return r ? (st.f_out / r) * 1000 + (st.f_out % r) * 1000 / r : 0;
}

static void title_show(const char *t)
{
    if (!strcmp(t, st.title))
        return;
    amce_strlcpy(st.title, t, sizeof(st.title));
    st.title_seq++;
    if (!st.finite && t[0]) {
        char m[200];
        sprintf(m, "%.60s: %.120s", st.name[0] ? st.name : "a station", t);
        amce_log("radio: on air", m);
    }
}

static void titles_due(void)
{
    ULONG out;
    if (tq_head == tq_tail)
        return;
    out = titles_out_ms();
    while (tq_head != tq_tail && (st.started && (LONG)(out - tq[tq_head].at) >= 0)) {
        title_show(tq[tq_head].t);
        tq_head = (UWORD)((tq_head + 1) % TITLES);
    }
}

/* A title block: StreamTitle='...'; kept till its audio is heard. */
static void take_meta(const char *m)
{
    const char *p = strstr(m, "StreamTitle='"), *e;
    char t[sizeof(st.title)];
    LONG n;
    if (!p)
        return;
    p += 13;
    e = strstr(p, "';");
    if (!e)
        e = p + strlen(p);
    n = e - p;
    if (n >= (LONG)sizeof(t))
        n = sizeof(t) - 1;
    memcpy(t, p, n);
    t[n] = 0;
    utf8_to_latin1(t);
    squeeze_spaces(t);
    {
        UWORD next = (UWORD)((tq_tail + 1) % TITLES);
        if (next == tq_head) {              /* full (titles coming fast): the oldest now */
            title_show(tq[tq_head].t);
            tq_head = (UWORD)((tq_head + 1) % TITLES);
        }
        tq[tq_tail].at = titles_in_ms();
        amce_strlcpy(tq[tq_tail].t, t, sizeof(tq[0].t));
        tq_tail = next;
    }
    titles_due();
}

/* Up to n bytes of MP3 into buf (title blocks taken out): the count, 0 at
 * the end. */
static LONG stream_read(UBYTE *buf, LONG n)
{
    LONG got = 0;
    while (got < n) {
        LONG avail, take;
        if (st.nb_pos >= st.nb_len && !net_more())
            break;
        if (st.metaint && st.meta_left == 0) {
            /* a title block: its length (x16) first */
            LONG len = st.nb[st.nb_pos++] * 16;
            char m[4096 + 1];
            LONG have = 0;
            while (have < len) {
                LONG k;
                if (st.nb_pos >= st.nb_len && !net_more()) {
                    st.mp3_bytes += got;
                    return got;
                }
                k = st.nb_len - st.nb_pos;
                if (k > len - have)
                    k = len - have;
                memcpy(m + have, st.nb + st.nb_pos, k);
                have += k;
                st.nb_pos += k;
            }
            m[have] = 0;
            if (have)
                take_meta(m);
            st.meta_left = st.metaint;
            continue;
        }
        avail = st.nb_len - st.nb_pos;
        take = n - got;
        if (take > avail)
            take = avail;
        if (st.metaint && take > st.meta_left)
            take = st.meta_left;
        CopyMem(st.nb + st.nb_pos, buf + got, take);
        st.nb_pos += take;
        got += take;
        if (st.metaint)
            st.meta_left -= take;
    }
    st.mp3_bytes += got;
    return got;
}

static BOOL sw_on;                      /* (a video's sound: swv_run) */
static LONG sw_aread(UBYTE *buf, LONG n);

/* mpega.library's bitstream hook: the stream, read as it comes. */
static ULONG hook_func(register struct Hook *h __asm("a0"), register MPEGA_ACCESS *a __asm("a1"),
                       register APTR handle __asm("a2"))
{
    (void)h;
    (void)handle;
    switch (a->func) {
    case MPEGA_BSFUNC_OPEN:
        a->data.open.stream_size = 0;       /* endless */
        return 1;
    case MPEGA_BSFUNC_CLOSE:
        return 0;
    case MPEGA_BSFUNC_READ:
        if (sw_on)                          /* (a video's sound: its demuxed packets) */
            return (ULONG)sw_aread(a->data.read.buffer, a->data.read.num_bytes);
        return (ULONG)stream_read(a->data.read.buffer, a->data.read.num_bytes);
    case MPEGA_BSFUNC_SEEK:
        return a->data.seek.abs_byte_seek_pos == 0 ? 0 : (ULONG)-1;
    }
    return (ULONG)-1;
}

/* A track's ID3v2 tag (its header says how long) read past. */
static BOOL skip_id3(void)
{
    ULONG left;
    while (st.nb_len - st.nb_pos < 10)
        if (!net_more())
            return FALSE;
    {
        const UBYTE *b = st.nb + st.nb_pos;
        if (b[0] != 'I' || b[1] != 'D' || b[2] != '3')
            return TRUE;
        left = 10 + (((ULONG)b[6] & 0x7F) << 21 | ((ULONG)b[7] & 0x7F) << 14 |
                     ((ULONG)b[8] & 0x7F) << 7 | ((ULONG)b[9] & 0x7F)) + ((b[5] & 0x10) ? 10 : 0);
    }
    while (left) {
        ULONG k = st.nb_len - st.nb_pos;
        if (!k) {
            if (!net_more())
                return FALSE;
            continue;
        }
        if (k > left)
            k = left;
        st.nb_pos += k;
        left -= k;
    }
    return TRUE;
}

/* MP3 frames read past, without decoding, to st.skip_ms in (a seek the
 * server would not start at): their headers say their length and time. */
static BOOL skip_to(void)
{
    static const UBYTE kb[2][3][15] = {
        { /* MPEG 1: layers I, II, III (kbit/s; I's halved, 255 the top ones) */
          { 0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224 },
          { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 255, 255, 255 },
          { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 255, 255 } },
        { /* MPEG 2 and 2.5 */
          { 0, 16, 24, 28, 32, 40, 48, 56, 64, 72, 80, 88, 96, 112, 128 },
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } },
    };
    static const LONG rates[4][3] = {
        { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };
    ULONG done = 0, want = 0;
    LONG freq = 0;
    while (!st.quit) {
        const UBYTE *b;
        UBYTE ver, layer, bri, sri, m2;
        ULONG kbps, len, samples;
        while (st.nb_len - st.nb_pos < 4)
            if (!net_more())
                return FALSE;
        b = st.nb + st.nb_pos;
        ver = (b[1] >> 3) & 3;
        layer = (b[1] >> 1) & 3;            /* 3 I, 2 II, 1 III */
        bri = b[2] >> 4;
        sri = (b[2] >> 2) & 3;
        if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0 || ver == 1 || !layer || bri == 15 || !bri ||
            sri == 3) {
            st.nb_pos++;                    /* not a frame's start: on a byte */
            continue;
        }
        m2 = ver != 3;
        kbps = kb[m2][3 - layer][bri];
        if (kbps == 255)
            kbps = layer == 2 ? (bri == 12 ? 256 : bri == 13 ? 320 : 384)
                              : (bri == 13 ? 256 : 320);
        if (layer == 3)
            kbps *= 2;
        if (!freq) {
            freq = rates[ver][sri];
            want = (st.skip_ms / 1000) * (ULONG)freq + (st.skip_ms % 1000) * (ULONG)freq / 1000;
        }
        if (layer == 3) {
            len = (12 * kbps * 1000 / rates[ver][sri] + ((b[2] >> 1) & 1)) * 4;
            samples = 384;
        } else {
            ULONG k = layer == 1 && m2 ? 72 : 144;
            len = k * kbps * 1000 / rates[ver][sri] + ((b[2] >> 1) & 1);
            samples = layer == 1 && m2 ? 576 : 1152;
        }
        if (done + samples > want)
            return TRUE;                    /* this frame holds the time: from here */
        done += samples;
        while (len) {
            ULONG k = st.nb_len - st.nb_pos;
            if (!k) {
                if (!net_more())
                    return FALSE;
                continue;
            }
            if (k > len)
                k = len;
            st.nb_pos += k;
            len -= k;
        }
    }
    return FALSE;
}

/* The sample rate of the stream's first MP3 frame (bytes read ahead for
 * it, as far as the first title block), 0 if none is found. */
static LONG first_frame_freq(void)
{
    static const LONG rates[4][3] = {
        { 11025, 12000, 8000 },             /* MPEG 2.5 */
        { 0, 0, 0 },
        { 22050, 24000, 16000 },            /* MPEG 2 */
        { 44100, 48000, 32000 },            /* MPEG 1 */
    };
    LONG i, lim;
    while (st.nb_len - st.nb_pos < 2048 && st.nb_len < NETBUF &&
           (!st.metaint || st.nb_len - st.nb_pos < st.meta_left))
        if (!net_more())
            return 0;
    lim = st.nb_len - st.nb_pos;
    if (st.metaint && lim > st.meta_left)
        lim = st.meta_left;
    for (i = 0; i + 3 < lim; i++) {
        const UBYTE *b = st.nb + st.nb_pos + i;
        UBYTE ver = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3, bri = b[2] >> 4, sri = (b[2] >> 2) & 3;
        if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0 && ver != 1 && layer != 0 && bri != 15 &&
            bri != 0 && sri != 3)
            return rates[ver][sri];
    }
    return 0;
}

/* A frame header's frame length in bytes (0: not a header). */
static LONG mp3_frame_len(const UBYTE *b)
{
    static const UWORD br[2][3][15] = {
        {   /* MPEG 1: layers I, II, III */
            { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
            { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
            { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
        {   /* MPEG 2 and 2.5 */
            { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
            { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
            { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } } };
    static const LONG rates[4][3] = {
        { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };
    UBYTE ver = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3, bri = b[2] >> 4, sri = (b[2] >> 2) & 3,
          pad = (b[2] >> 1) & 1;
    LONG kbps, sr;
    int l;
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0 || ver == 1 || layer == 0 || bri == 15 || bri == 0 ||
        sri == 3)
        return 0;
    l = 3 - layer;                          /* 0: I, 1: II, 2: III */
    kbps = br[ver == 3 ? 0 : 1][l][bri];
    sr = rates[ver][sri];
    if (l == 0)
        return (12000L * kbps / sr + pad) * 4;
    if (l == 2 && ver != 3)
        return 72000L * kbps / sr + pad;    /* MPEG 2, 2.5's layer III: half */
    return 144000L * kbps / sr + pad;
}

/* A track started part way (a seek: a byte reckoned from the time): the
 * bytes before its next real frame let go, one whose frame is followed by
 * two more like it, never bytes that only look like a header (a seek into
 * the ID3 tag's picture, at the file's start). FALSE: none found. */
static BOOL mp3_resync(void)
{
    ULONG skipped = 0;
    for (;;) {
        LONG lim, i;
        while (st.nb_len - st.nb_pos < 4096 && st.nb_len < NETBUF)
            if (!net_more())
                return FALSE;
        lim = st.nb_len - st.nb_pos;
        for (i = 0; i + 4 <= lim; i++) {
            const UBYTE *b = st.nb + st.nb_pos + i;
            LONG f1 = mp3_frame_len(b), f2;
            if (!f1)
                continue;
            if (i + f1 + 4 > lim)
                break;                      /* (more wanted to tell) */
            if (!(f2 = mp3_frame_len(b + f1)) || ((b[f1 + 1] ^ b[1]) & 0xFE) ||
                ((b[f1 + 2] ^ b[2]) & 0x0C))
                continue;                   /* not followed by one like it */
            if (i + f1 + f2 + 4 > lim)
                break;
            if (!mp3_frame_len(b + f1 + f2) || ((b[f1 + f2 + 1] ^ b[1]) & 0xFE))
                continue;
            st.nb_pos += i;
            skipped += (ULONG)i;
            if (skipped) {
                char m[80];
                sprintf(m, "%lu bytes before the first whole frame let go", (unsigned long)skipped);
                amce_log("track: seek", m);
            }
            return TRUE;
        }
        if (i == 0)
            i = 1;
        st.nb_pos += i;                     /* none there: on through the rest */
        skipped += (ULONG)i;
        if (skipped > 2UL << 20)
            return FALSE;                   /* 2 MB and nothing: not MP3 */
        if (st.nb_pos > 0) {                /* (room for more: what is left to the front) */
            memmove(st.nb, st.nb + st.nb_pos, st.nb_len - st.nb_pos);
            st.nb_len -= st.nb_pos;
            st.nb_pos = 0;
        }
        if (!net_more())
            return FALSE;
    }
}

/* What a track is called in the log: the last part of its URL or path,
 * without the query (a Plex track's carries the sign-in token, which
 * never goes into the log). */
static const char *log_name(void)
{
    static char u[120];
    char *q;
    amce_strlcpy(u, (const char *)FilePart((STRPTR)st.url), sizeof(u));
    if ((q = strchr(u, '?')))
        *q = 0;
    return u;
}

/* ---------------------------------------------------------- decoding */
static ULONG dec_full, dec_samples;     /* mpega's samples at full scale, of all it gave */

/* Frames decoded into the FIFO while there is room; Paula served between. */
static void decode_loop(MPEGA_STREAM *ms)
{
    static WORD pcm0[MPEGA_PCM_SIZE], pcm1[MPEGA_PCM_SIZE];
    WORD *pcm[2];
    ULONG prebuf = (ULONG)st.dec_freq * prebuf_ms[buf_level] / 1000;
    pcm[0] = pcm0;
    pcm[1] = pcm1;
    if (prebuf > FIFO_N / 2)
        prebuf = FIFO_N / 2;
    while (!st.quit) {
        LONG n, i;
        ULONG o;
        serve();
        if (st.f_in - st.f_out > FIFO_N - MPEGA_PCM_SIZE) {
            /* full: wait for Paula to take some */
            ULONG got = st_wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
            if (got & (1UL << st.sig))
                out_take();
            continue;
        }
        n = MPEGA_decode_frame(ms, pcm);
        if (n < 0) {
            if (st.eof) {
                if (!st.started && st.f_in != st.f_out) {
                    st.state = ST_PLAYING;  /* shorter than the prebuffer: it plays now */
                    out_start();
                }
                break;                      /* a track's end: what is left plays out */
            }
            if (n == -2)                    /* a bad frame: the next one */
                continue;
            fail(st.err[0] ? st.err : _("The stream could not be decoded"));
            break;
        }
        o = st.f_in;
        for (i = 0; i < n; i++, o++) {
            WORD a = pcm0[i], b = st.dec_channels == 2 ? pcm1[i] : 0;
            st.fl[o & (FIFO_N - 1)] = a;
            if (st.dec_channels == 2)
                st.fr[o & (FIFO_N - 1)] = b;
            /* (the decoder's own clipping: a sample at full scale) */
            if (a >= 32767 || a <= -32767 || b >= 32767 || b <= -32767)
                dec_full++;
        }
        dec_samples += (ULONG)n;
        st.f_in = o;
        if (!st.started && st.f_in - st.f_out >= prebuf) {
            st.state = ST_PLAYING;
            out_start();
        }
    }
    /* what is left in the FIFO played out, unless stopping */
    while (!st.quit && st.started && st.f_in != st.f_out) {
        ULONG got = st_wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C)
            break;
        out_take();
    }
}

/* ---------------------------------------------------------- FLAC
 * A track stored as FLAC (Plex, played as stored on a fast Amiga): read
 * through flacdec's hook from the connection, decoded to 16 bits, then
 * brought to what Paula plays: every div frames averaged into one (a 44.1
 * kHz file at 22050), mixed to mono where the CPU wants that, 8 bits. */

/* The stream starts with FLAC's own mark. */
static BOOL is_flac(void)
{
    while (st.nb_len - st.nb_pos < 4)
        if (!net_more())
            return FALSE;
    return !memcmp(st.nb + st.nb_pos, "fLaC", 4);
}

/* Bytes passed over (a seek forward a little way, or flacdec skipping
 * metadata): read and dropped. */
static BOOL flac_skip(ULONG n)
{
    while (n) {
        ULONG k = st.nb_len - st.nb_pos;
        if (!k) {
            if (!net_more())
                return FALSE;
            continue;
        }
        if (k > n)
            k = n;
        st.nb_pos += k;
        st.fpos += k;
        n -= k;
    }
    return TRUE;
}

/* flacdec's I/O hook: the file as it comes, seeks forward read past, a
 * seek anywhere else asked of the server again from there (Range). */
static ULONG flac_io(register struct Hook *h __asm("a0"), register struct FLACDecIO *m __asm("a1"),
                     register APTR dec __asm("a2"))
{
    (void)h;
    (void)dec;
    switch (m->fio_Method) {
    case FDIO_READ: {
        LONG got = stream_read(m->fio_Buffer, m->fio_Length);
        st.fpos += got;
        if (!got && st.err[0] && !st.eof)
            return (ULONG)-1;
        return (ULONG)got;
    }
    case FDIO_TELL:
        return st.fpos;
    case FDIO_SEEK: {
        LONG to = m->fio_Length;
        if (m->fio_Mode == OFFSET_CURRENT)
            to += (LONG)st.fpos;
        else if (m->fio_Mode == OFFSET_END) {
            if (!st.clen)
                return FALSE;               /* its length not known */
            to += (LONG)st.clen;
        }
        if (to < 0 || (st.clen && (ULONG)to > st.clen))
            return FALSE;
        if ((ULONG)to == st.fpos)
            return TRUE;
        if ((ULONG)to > st.fpos && (ULONG)to - st.fpos <= 65536UL) {
            st.eof = FALSE;
            return flac_skip((ULONG)to - st.fpos);
        }
        if (st.fh) {
            /* a file here: moved in it */
            if (Seek(st.fh, to, OFFSET_BEGINNING) < 0)
                return FALSE;
            st.nb_pos = st.nb_len = 0;
            st.eof = FALSE;
            st.fpos = (ULONG)to;
            return TRUE;
        }
        /* far, or back: the file asked for again from there */
        {
            UBYTE was = st.state;
            sock_close();
            st.nb_pos = st.nb_len = 0;
            st.eof = FALSE;
            st.err[0] = 0;
            st.range_from = (ULONG)to;
            if (!open_stream())
                return FALSE;
            st.state = was;                 /* still playing, for the UI */
            st.fpos = (ULONG)to;
            return TRUE;
        }
    }
    }
    return (ULONG)-1;
}

/* Frames (of ch 16-bit samples) from a decoder: FLAC's, or a PCM file's. */
typedef LONG (*FrameFn)(APTR ctx, WORD *buf, LONG frames);

static LONG flac_frames(APTR dec, WORD *buf, LONG frames)
{
    return FLACDecRead(dec, buf, frames);
}

/* Decoded frames into the FIFO, Paula started once the prebuffer is in,
 * till the end (or a stop). */
static void pcm_loop(FrameFn rd, APTR dec, UWORD ch, UWORD div, BOOL mono)
{
    static WORD buf[1024 * 8];              /* frames of up to 8 channels */
    LONG frames = 8192 / (ch ? ch : 1), acc_l = 0, acc_r = 0;
    UWORD acc_n = 0;
    ULONG prebuf = (ULONG)st.dec_freq * prebuf_ms[buf_level] / 1000;
    if (frames > 1024)
        frames = 1024;
    if (prebuf > FIFO_N / 2)
        prebuf = FIFO_N / 2;
    while (!st.quit) {
        LONG n, i;
        ULONG o;
        serve();
        if (st.f_in - st.f_out > FIFO_N - (ULONG)frames) {
            ULONG got = st_wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
            if (got & (1UL << st.sig))
                out_take();
            continue;
        }
        n = rd(dec, buf, frames);
        if (n <= 0) {
            if (st.err[0] && !st.eof) {
                fail(st.err);
                break;
            }
            if (!st.started && st.f_in != st.f_out) {
                st.state = ST_PLAYING;      /* shorter than the prebuffer: it plays now */
                out_start();
            }
            break;                          /* the end: what is left plays out */
        }
        o = st.f_in;
        for (i = 0; i < n; i++) {
            const WORD *f = buf + i * ch;
            acc_l += f[0];
            acc_r += ch > 1 ? f[1] : f[0];
            if (++acc_n < div)
                continue;
            if (mono) {
                st.fl[o & (FIFO_N - 1)] = (WORD)((acc_l + acc_r) / (2 * (LONG)div));
            } else {
                st.fl[o & (FIFO_N - 1)] = (WORD)(acc_l / (LONG)div);
                st.fr[o & (FIFO_N - 1)] = (WORD)(acc_r / (LONG)div);
            }
            o++;
            acc_l = acc_r = 0;
            acc_n = 0;
        }
        st.f_in = o;
        if (!st.started && st.f_in - st.f_out >= prebuf) {
            st.state = ST_PLAYING;
            out_start();
        }
    }
    while (!st.quit && st.started && st.f_in != st.f_out) {
        ULONG got = st_wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C)
            break;
        out_take();
    }
}

/* A FLAC track, start to end (from st.skip_ms in, for a seek). */
static void flac_play(void)
{
    static struct Hook fh;
    struct FLACDecInfo info;
    APTR dec;
    UWORD div = 1;
    BOOL mono;
    LONG fmax = st.cpu >= 40 ? (st.ahi ? 48000 : 28000) : st.cpu >= 30 ? 24000 : 12000;
    if (!(FLACDecBase = OpenLibrary((CONST_STRPTR)"flacdec.library", 1))) {
        fail("flacdec.library is needed to play FLAC (Aminet: util/libs/flacdec)");
        return;
    }
    memset(&fh, 0, sizeof(fh));
    fh.h_Entry = (HOOKFUNC)(APTR)flac_io;
    memset(&info, 0, sizeof(info));
    st.fpos = st.range_from;
    if (!(dec = FLACDecOpen(&fh, &info))) {
        fail(st.err[0] ? st.err : "The track is not FLAC that flacdec.library can decode");
        goto out;
    }
    while (div < 8 && (LONG)(info.fdi_SampleRate / div) > fmax)
        div *= 2;                           /* 44.1 kHz: 22050, 96 kHz: 24000 */
    mono = st.cpu < 40 || info.fdi_Channels == 1;
    st.freq = info.fdi_SampleRate;
    st.channels = info.fdi_Channels;
    if (info.fdi_SampleRate && !info.fdi_TotalFramesHi)
        st.len_ms = (info.fdi_TotalFramesLo / info.fdi_SampleRate) * 1000 +
                    (info.fdi_TotalFramesLo % info.fdi_SampleRate) * 1000 / info.fdi_SampleRate;
    st.dec_freq = info.fdi_SampleRate / div;
    st.dec_channels = mono ? 1 : 2;
    st.flac = TRUE;
    if (st.skip_ms) {
        ULONG r = info.fdi_SampleRate;
        ULONG frame = (st.skip_ms / 1000) * r + (st.skip_ms % 1000) * r / 1000;
        if (!FLACDecSeek(dec, frame))
            amce_log("track", "FLAC: could not seek there; from the start");
    }
    {
        char m[360], u[200], *q;
        amce_strlcpy(u, st.url, sizeof(u));
        if ((q = strchr(u, '?')))
            *q = 0;
        sprintf(m, "%s: FLAC %ld Hz %ld-bit, %ld channel%s, decoded at %ld Hz %s",
                u, (long)info.fdi_SampleRate, (long)info.fdi_BitsPerSample,
                (long)info.fdi_Channels, info.fdi_Channels == 1 ? "" : "s", (long)st.dec_freq,
                mono ? "mono" : "stereo");
        amce_log("track", m);
    }
    st.state = ST_BUFFERING;
    pcm_loop(flac_frames, dec, info.fdi_Channels, div, mono);
out:
    if (dec)
        FLACDecClose(dec);
    CloseLibrary(FLACDecBase);
    FLACDecBase = NULL;
}

/* ---- WAV, AIFF and 8SVX (music files here): their samples as they are
 * stored, made 16-bit frames for pcm_loop */
static struct PcmFmt pf;
static ULONG pcm_left;                      /* bytes of samples still to come */

static LONG pcm_frames(APTR ctx, WORD *buf, LONG frames)
{
    static UBYTE raw[1024 * 8 * 3];
    LONG want = frames * pf.align, got, i, n, k;
    (void)ctx;
    if (want > (LONG)sizeof(raw))
        want = sizeof(raw) - sizeof(raw) % pf.align;
    if ((ULONG)want > pcm_left)
        want = (LONG)(pcm_left - pcm_left % pf.align);
    if (want <= 0)
        return 0;
    got = stream_read(raw, want);
    got -= got % pf.align;
    pcm_left -= got;
    n = got / (pf.bits / 8);                /* samples */
    for (i = 0, k = 0; i < n; i++) {
        if (pf.bits == 8) {
            buf[i] = (WORD)((pf.signed8 ? (BYTE)raw[i] : (BYTE)(raw[i] ^ 0x80)) << 8);
        } else if (pf.bits == 16) {
            buf[i] = pf.big ? (WORD)(raw[k] << 8 | raw[k + 1]) : (WORD)(raw[k + 1] << 8 | raw[k]);
            k += 2;
        } else {
            buf[i] = pf.big ? (WORD)(raw[k] << 8 | raw[k + 1]) : (WORD)(raw[k + 2] << 8 | raw[k + 1]);
            k += 3;                         /* 24-bit: its top 16 */
        }
    }
    return got / pf.align;
}

static BOOL is_pcm(void)
{
    while (st.nb_len - st.nb_pos < 12)
        if (!net_more())
            return FALSE;
    return (!memcmp(st.nb + st.nb_pos, "RIFF", 4) && !memcmp(st.nb + st.nb_pos + 8, "WAVE", 4)) ||
           (!memcmp(st.nb + st.nb_pos, "FORM", 4) &&
            (!memcmp(st.nb + st.nb_pos + 8, "AIFF", 4) ||
             !memcmp(st.nb + st.nb_pos + 8, "AIFC", 4) ||
             !memcmp(st.nb + st.nb_pos + 8, "8SVX", 4)));
}

/* A WAV, AIFF or 8SVX file here, start to end (from st.skip_ms in). */
static void pcm_play(void)
{
    static UBYTE hdr[8192];
    LONG n = stream_read(hdr, sizeof(hdr));
    ULONG start;
    UWORD div = 1;
    BOOL mono;
    LONG fmax = st.cpu >= 40 ? (st.ahi ? 48000 : 28000) : st.cpu >= 30 ? 24000 : 16000;
    if (!st.fh) {
        fail(_("Only music files on this Amiga play as WAV, AIFF or 8SVX"));
        return;
    }
    if (!pcm_header(hdr, n, &pf)) {
        fail(_("Not a kind of WAV, AIFF or 8SVX that AmigaMCE plays (compressed?)"));
        return;
    }
    if (st.clen && pf.off + pf.bytes > st.clen) {
        pf.bytes = st.clen > pf.off ? st.clen - pf.off : 0;
        pf.frames = pf.bytes / pf.align;    /* a WAV written as it streamed: to its end */
    }
    while (div < 8 && (LONG)(pf.rate / div) > fmax)
        div *= 2;                           /* 44.1 kHz: 22050 */
    mono = pf.channels == 1 || st.cpu < 30;
    st.freq = pf.rate;
    st.channels = pf.channels;
    st.dec_freq = pf.rate / div;
    st.dec_channels = mono ? 1 : 2;
    st.len_ms = (pf.frames / pf.rate) * 1000 + (pf.frames % pf.rate) * 1000 / pf.rate;
    start = pf.off;
    pcm_left = pf.bytes;
    if (st.skip_ms) {
        ULONG fr = (st.skip_ms / 1000) * pf.rate + (st.skip_ms % 1000) * pf.rate / 1000;
        ULONG by = fr * pf.align;
        if (by < pcm_left) {
            start += by;
            pcm_left -= by;
        }
    }
    /* from its first sample (or the time asked for), not the header's end */
    if (Seek(st.fh, start, OFFSET_BEGINNING) < 0) {
        fail(_("The file could not be read"));
        return;
    }
    st.nb_pos = st.nb_len = 0;
    st.eof = FALSE;
    {
        char m[200];
        sprintf(m, "%s: %s %ld Hz %ld-bit, %ld channel%s, played at %ld Hz %s",
                log_name(), pf.kind == PCM_WAV ? "WAV" : pf.kind == PCM_AIFF ? "AIFF"
                : "8SVX", (long)pf.rate, (long)pf.bits, (long)pf.channels,
                pf.channels == 1 ? "" : "s", (long)st.dec_freq, mono ? "mono" : "stereo");
        amce_log("track", m);
    }
    st.state = ST_BUFFERING;
    pcm_loop(pcm_frames, NULL, pf.channels, div, mono);
}

/* ---------------------------------------------------------- video
 * MPEG on the ZZ9000 (zz9k.c): the file written to the card's media
 * session in chunks; each picture it holds shown on its overlay (the UI's
 * PIP window), or let go if late, against the sound's clock; its MP2
 * decoded on the card, read back from a ring and played as music is (the
 * FIFO, Paula or AHI, the volume, the meters). Without sound, the clock is
 * the time since the first picture. */
#define V_IN_BYTES   (32UL * 1024)
#define V_SCAN      (64UL * 1024)   /* a seek: looked through for where to start */
#define V_PCM_BYTES  (64UL * 1024)

static struct MsgPort *v_tport;
static struct timerequest *v_treq;
static BOOL v_topen;

/* Waited us microseconds (the output served, a stop seen). */
static void v_wait(ULONG us)
{
    ULONG tsig, got;
    if (!v_topen) {
        st_delay(1);
        return;
    }
    tsig = 1UL << v_tport->mp_SigBit;
    v_treq->tr_node.io_Command = TR_ADDREQUEST;
    v_treq->tr_time.tv_secs = us / 1000000;
    v_treq->tr_time.tv_micro = us % 1000000;
    SendIO((struct IORequest *)v_treq);
    for (;;) {
        got = Wait(tsig | (st.sig >= 0 ? 1UL << st.sig : 0) | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C)
            st.quit = TRUE;
        if (st.sig >= 0 && (got & (1UL << st.sig)))
            SetSignal(1UL << st.sig, 1UL << st.sig);    /* (serve() takes it) */
        serve();
        if (CheckIO((struct IORequest *)v_treq) || st.quit)
            break;
    }
    if (!CheckIO((struct IORequest *)v_treq))
        AbortIO((struct IORequest *)v_treq);
    WaitIO((struct IORequest *)v_treq);
    SetSignal(0, tsig);
}

/* The sound's clock, 90 kHz: what the output plays now (the FIFO's samples
 * given it, less what it still holds, plus what it has played of that). */
static ZzU64 v_clock(ZzU64 origin, ULONG f0, ULONG rate, ULONG div)
{
    ULONG lat = st.ahi ? AHI_FRAMES : HALF, pos = st.f_out - f0, el;
    if (!rate)
        return origin;                      /* (no sound's rate yet) */
    el = (time_ms() - st.play_t) * (ULONG)st.dec_freq / 1000;
    if (el > lat)
        el = lat;
    pos = pos > lat ? pos - lat + el : el;
    return origin + (ZzU64)pos * div * 90000ULL / rate;
}

/* The sound's clock (v_clock's, m), carried on by the time of day while
 * its FIFO is empty (its sound over, or not decoded yet): a picture due
 * after the sound's end (a DVD menu's moving part longer than its sound)
 * shown, and never a picture waiting for a clock that waits for sound
 * behind that picture's own (the reading held while mpegv holds enough
 * video). The sound back: its own clock again (the pictures waiting for
 * it to catch up). */
static ZzU64 v_clock_on(ZzU64 m, ULONG *dry_t, ZzU64 *dry_m)
{
    ZzU64 w;
    if (st.f_in != st.f_out) {
        *dry_t = 0;
        return m;
    }
    if (!*dry_t) {
        *dry_t = time_ms() ? time_ms() : 1;
        *dry_m = m;
    }
    w = *dry_m + (ZzU64)(time_ms() - *dry_t) * 90;
    return w > m ? w : m;
}

/* A bare MPEG-1 video stream (no packs: RiVA's, a .m1v by a .mpg name)
 * made a Program Stream as it is given to the card, which takes no other:
 * a pack header (its clock reckoned at 1.5 Mbit/s), a system header with
 * the first, and each 2000 bytes a video packet, the first with a time
 * stamp (0: the card goes on from it). The end: an end code. Up to max
 * bytes at out; 0 at the end. */
#define ES_CHUNK 2000
static ULONG es_bytes;                      /* the stream's bytes packed so far */
static BOOL es_ended;

/* A Program Stream's first system header made to say what it carries: a
 * VCD's first says video only (its audio's own comes next), and a
 * decoder that reads only the first (pl_mpeg's, Amiberry's emulated
 * ZZ9000) then plays no sound. Its bounds set to at least one stream of
 * each the file has; the card's own firmware ignores system headers.
 * TRUE once the first was found (in the first 4 KB of the file). */
static BOOL sys_fix(volatile UBYTE *b, ULONG n)
{
    ULONG i;
    if (n > 4096)
        n = 4096;
    for (i = 0; i + 12 <= n; i++) {
        UBYTE a, v;
        char m[100];
        if (b[i] || b[i + 1] || b[i + 2] != 1 || b[i + 3] != 0xBB)
            continue;
        a = b[i + 9];
        v = b[i + 10];
        if (st.v_mp2 && !(a >> 2))
            b[i + 9] = (UBYTE)((1 << 2) | (a & 3));
        if (!(v & 0x1F))
            b[i + 10] = (UBYTE)((v & 0xE0) | 1);
        if (b[i + 9] != a || b[i + 10] != v) {
            sprintf(m, "its first system header said %lu audio, %lu video streams: %lu and %lu given",
                    (unsigned long)(a >> 2), (unsigned long)(v & 0x1F),
                    (unsigned long)(b[i + 9] >> 2), (unsigned long)(b[i + 10] & 0x1F));
            amce_log("video", m);
        }
        return TRUE;
    }
    return n == 4096;                       /* (none at its start: none looked for) */
}

static ULONG es_pack(volatile UBYTE *out, ULONG max)
{
    static UBYTE b[ES_CHUNK + 64];
    ULONG o = 0;
    while (!es_ended && o + sizeof(b) <= max) {
        LONG n = Read(st.fh, b + 64, ES_CHUNK), k = 0, i;
        ULONG scr = (ULONG)((unsigned long long)es_bytes * 90000ULL / 187500ULL), mr = 3750;
        UBYTE h[64];
        if (n < 0) {
            fail(_("The file could not be read"));
            return o;
        }
        if (n == 0) {
            h[0] = h[1] = 0;
            h[2] = 1;
            h[3] = 0xB9;                    /* the program's end */
            for (i = 0; i < 4; i++)
                out[o++] = h[i];
            es_ended = TRUE;
            break;
        }
        /* the pack header (MPEG-1): its clock, its rate (50 bytes a second units) */
        h[k++] = 0; h[k++] = 0; h[k++] = 1; h[k++] = 0xBA;
        h[k++] = (UBYTE)(0x21 | ((scr >> 29) & 0x0E));
        h[k++] = (UBYTE)(scr >> 22);
        h[k++] = (UBYTE)(((scr >> 14) & 0xFE) | 1);
        h[k++] = (UBYTE)(scr >> 7);
        h[k++] = (UBYTE)(((scr << 1) & 0xFE) | 1);
        h[k++] = (UBYTE)(0x80 | (mr >> 15));
        h[k++] = (UBYTE)(mr >> 7);
        h[k++] = (UBYTE)(((mr & 0x7F) << 1) | 1);
        if (!es_bytes) {
            /* the system header: one video stream, its buffer 46 KB */
            h[k++] = 0; h[k++] = 0; h[k++] = 1; h[k++] = 0xBB;
            h[k++] = 0; h[k++] = 9;
            h[k++] = (UBYTE)(0x80 | (mr >> 15));
            h[k++] = (UBYTE)(mr >> 7);
            h[k++] = (UBYTE)(((mr & 0x7F) << 1) | 1);
            h[k++] = 0x00;                  /* no audio, not fixed, not CSPS */
            h[k++] = 0xE1;                  /* locks, a marker, one video stream */
            h[k++] = 0xFF;
            h[k++] = 0xE0; h[k++] = 0xE0; h[k++] = 46;
        }
        /* the video packet: a time stamp on the first */
        h[k++] = 0; h[k++] = 0; h[k++] = 1; h[k++] = 0xE0;
        h[k++] = (UBYTE)((n + (es_bytes ? 1 : 5)) >> 8);
        h[k++] = (UBYTE)(n + (es_bytes ? 1 : 5));
        if (!es_bytes) {
            h[k++] = 0x21; h[k++] = 0x00; h[k++] = 0x01; h[k++] = 0x00; h[k++] = 0x01;  /* PTS 0 */
        } else {
            h[k++] = 0x0F;                  /* no time stamp */
        }
        for (i = 0; i < k; i++)
            out[o++] = h[i];
        for (i = 0; i < n; i++)
            out[o++] = b[64 + i];
        es_bytes += (ULONG)n;
    }
    return o;
}

/* A transport stream's bytes, for tspack.c. */
static LONG ts_read(void *ctx, UBYTE *b, LONG n)
{
    (void)ctx;
    return Read(st.fh, b, n);
}

/* A seek's start, close before its place: the file's offset of the pack
 * (a bare stream: the header itself) holding the last sequence header, or
 * with gop the last picture group's header (an I picture follows either),
 * at most a second's bytes before at (32 KB to 1 MB); -1 if none is. A
 * picture shown from just before the place, not the next I picture's
 * wait, nor the blocks of pictures built on ones not decoded. */
static LONG v_back(ULONG at, BOOL gop)
{
    ULONG win = st.v_bps < 32768 ? 32768 : st.v_bps > 1048576 ? 1048576 : st.v_bps;
    ULONG from = at > win ? at - win : 0, n = at - from;
    UBYTE *b;
    LONG i, g = -1, r = -1, got;
    if (!n || !(b = AllocVec(n + 4, MEMF_ANY)))
        return -1;
    if (Seek(st.fh, (LONG)from, OFFSET_BEGINNING) >= 0 && (got = Read(st.fh, b, n + 3)) > 4) {
        for (i = got - 4; i >= 0 && g < 0; i--)
            if (!b[i] && !b[i + 1] && b[i + 2] == 1 && (b[i + 3] == 0xB3 || (gop && b[i + 3] == 0xB8)) &&
                from + (ULONG)i <= at)
                g = i;
        if (g >= 0 && st.v_es)
            r = (LONG)from + g;
        else
            for (i = g; i >= 0 && r < 0; i--)
                if (!b[i] && !b[i + 1] && b[i + 2] == 1 && b[i + 3] == 0xBA)
                    r = (LONG)from + i;
    }
    FreeVec(b);
    return r;
}

static BOOL v_from_back;                    /* the last seek began just before its place */

/* A seek (st.range_from, the file there already): for a Program Stream
 * its first headers (pack and system headers, up to its first audio or
 * video packet: a decoder that reads its streams from them, pl_mpeg, needs
 * them first) put in the card's buffer to go first, then the file from the
 * first pack at or after where it is; a bare stream from its next
 * sequence header. FALSE if the file could not be read. */
static BOOL v_seek(struct ZzBuf *in, ULONG *pend)
{
    UBYTE *b = AllocVec(V_SCAN, MEMF_ANY);
    LONG n, i, at = -1;
    UBYTE want = st.v_es ? 0xB3 : 0xBA;
    char m[120];
    BOOL ok = FALSE;
    if (st.v_ts) {
        /* a transport stream: from the packet there (its PES packets
         * taken up from the next that begins) */
        ULONG at = st.range_from > st.v_tsfirst
                       ? st.v_tsfirst + (st.range_from - st.v_tsfirst) / st.v_ts * st.v_ts
                       : st.v_tsfirst;
        FreeVec(b);
        *pend = 0;
        if (Seek(st.fh, (LONG)at, OFFSET_BEGINNING) < 0) {
            fail(_("The file could not be read"));
            return FALSE;
        }
        tsp_begin(st.v_ts, st.v_vpid, st.v_apid, st.v_ac3, st.v_mpeg2);
        sprintf(m, "from byte %lu (a transport stream's packet)", (unsigned long)at);
        amce_log("video", m);
        return TRUE;
    }
    if (!b)
        return FALSE;
    v_from_back = FALSE;
    {
        LONG back = v_back(st.range_from, !st.v_es);
        if (back >= 0) {
            at = back - (LONG)st.range_from;    /* (before it: negative) */
            v_from_back = TRUE;
        }
    }
    if (Seek(st.fh, (LONG)st.range_from, OFFSET_BEGINNING) < 0)
        n = -1;
    else
        n = Read(st.fh, b, V_SCAN);
    for (i = 0; at < 0 && !v_from_back && i + 4 <= n; i++)
        if (!b[i] && !b[i + 1] && b[i + 2] == 1 && b[i + 3] == want) {
            at = i;
            break;
        }
    if (!v_from_back && (n < 0 || at < 0)) {
        sprintf(m, "a seek to byte %lu: no %s after it", (unsigned long)st.range_from,
                st.v_es ? "sequence header" : "pack");
        amce_log("video", m);
        fail(_("The file could not be read"));
        goto done;
    }
    *pend = 0;
    if (!st.v_es && Seek(st.fh, 0, OFFSET_BEGINNING) >= 0 && (n = Read(st.fh, b, V_SCAN)) > 0) {
        LONG p = -1;
        for (i = 0; i + 4 <= n; i++)
            if (!b[i] && !b[i + 1] && b[i + 2] == 1 && b[i + 3] >= 0xC0 && b[i + 3] <= 0xEF) {
                p = i;
                break;
            }
        if (p > 0 && (ULONG)p <= in->length) {
            for (i = 0; i < p; i++)
                in->data[i] = b[i];
            *pend = (ULONG)p;               /* (sent first: its headers) */
        }
    }
    if (Seek(st.fh, (LONG)st.range_from + at, OFFSET_BEGINNING) < 0) {
        fail(_("The file could not be read"));
        goto done;
    }
    sprintf(m, "from byte %lu (asked %lu%s), its first %lu bytes of headers first",
            (unsigned long)(st.range_from + at), (unsigned long)st.range_from,
            v_from_back ? ": the picture group before it" : "", (unsigned long)*pend);
    amce_log("video", m);
    ok = TRUE;
done:
    FreeVec(b);
    return ok;
}

/* ---------------------------------------------------------- AC3 by the CPU
 * Where the card will not decode a video's AC3 (Amiberry's emulated
 * ZZ9000 cannot), the card is given it without sound and shows its
 * pictures, and the CPU decodes the AC3 instead (ac3fpu.library on a
 * 68040/68060 with its FPU, else ac3.library: liba52, GPL, opened at run
 * time from AmigaMCE's Libs or LIBS:), the pictures timed by it as by the
 * card's sound. Its bytes come out of the Program Stream as it is given to
 * the card (a file's, a DVD's, a transport stream's as packed): private
 * stream 1, its first AC3 substream (80-87), whole packets passed over,
 * not read, so not a byte at a time. A 68040 or better: a 68020 or 68030
 * cannot decode AC3 as fast as it plays. */
#define A3Q     (256UL * 1024)              /* its AC3 bytes ahead, a ring */
static BOOL ac3_by_cpu;                     /* the card would not: the CPU's from now on */
struct Library *Ac3Base;
enum { A3_SCAN, A3_ID, A3_PACK1, A3_PACK2, A3_LEN, A3_PES, A3_PESH, A3_SUB, A3_SKIP, A3_TAKE };
static struct {
    APTR dec;
    UBYTE *q;
    ULONG qin, qout, lost;
    UBYTE state, id, sub, hn, want;
    UBYTE h[20];
    ULONG sc, rem;
    ZzU64 pts0;                             /* its first packet's time (ZZ_NO_PTS: none yet) */
    ULONG frames, bad;
    WORD pcm[AC3_FRAME_SAMPLES * 2];
    UBYTE frame[AC3_MAX_FRAME];
} a3;

void st_ac3_by_cpu(BOOL on)
{
    ac3_by_cpu = on;
}

BOOL st_ac3_by_cpu_on(void)
{
    return ac3_by_cpu;
}

/* Whether the CPU can decode AC3 here: NULL if so, else why not. */
const char *st_ac3_cpu_why(void)
{
    static const char *const at[] = { "PROGDIR:Libs/" AC3FPUNAME, "LIBS:" AC3FPUNAME,
                                      "PROGDIR:Libs/" AC3NAME, "LIBS:" AC3NAME };
    int i;
    if (!(SysBase->AttnFlags & (AFF_68040 | AFF_68060)))
        return "AC3 by the CPU needs a 68040 or 68060";
    for (i = 0; i < 4; i++)
        if (amce_exists(at[i]))
            return NULL;
    return "no ac3.library (AmigaMCE's Libs or LIBS:)";
}

/* Which AC3 library is the faster here, found once a run: with an FPU,
 * both are timed on the same four frames (5.1 at 448 kbit/s, as a DVD's)
 * and the faster kept. An FPU's build is not always the faster: ac3fpu 1.0
 * took 14-35 ms a frame under Amiberry's JIT against ac3.library's 1.5
 * (its float-to-integer conversions switched the FPCR's rounding twice a
 * sample, which UAE does slowly; 1.1 does not), and an emulator's FPU may
 * be slow anyway. */
#include "ac3probe.h"
static const char *a3_lib;                  /* the one to use (NULL: not timed yet) */

/* name's microseconds a frame on the probe (its first frame not counted:
 * its tables made), 0 if it could not decode them */
static ULONG a3_time(const char *name)
{
    struct Ac3Info ai;
    APTR dec;
    ULONG hz = 0, t = 0, n = 0, i, at = 0;
    if (!(Ac3Base = amce_open_own_lib(name, AC3VERSION)))
        return 0;
    if ((dec = Ac3Open(AC3_STEREO, 0))) {
        for (i = 0; at + AC3_HEADER <= sizeof(ac3probe); i++) {
            LONG off = Ac3Find((UBYTE *)ac3probe + at, (LONG)(sizeof(ac3probe) - at), &ai);
            ULONG tk;
            if (off < 0 || at + off + ai.ai_FrameBytes > sizeof(ac3probe))
                break;
            at += (ULONG)off;
            CopyMem((APTR)(ac3probe + at), a3.frame, ai.ai_FrameBytes);
            tk = time_ticks(&hz);
            if (Ac3Decode(dec, a3.frame, a3.pcm, &ai) != AC3_FRAME_SAMPLES)
                break;
            if (i) {
                t += time_ticks(&hz) - tk;
                n++;
            }
            at += ai.ai_FrameBytes;
        }
        Ac3Close(dec);
    }
    amce_close_own_lib(&Ac3Base);
    return n && hz ? (ULONG)((unsigned long long)t * 1000000ULL / hz / n) : 0;
}

static void a3_choose(void)
{
    ULONG f, i;
    char m[160];
    if (a3_lib)
        return;
    a3_lib = AC3NAME;
    if (!(SysBase->AttnFlags & AFF_FPU40))
        return;                             /* (no FPU: the integer build) */
    f = a3_time(AC3FPUNAME);
    i = a3_time(AC3NAME);
    if (f && (!i || f <= i))
        a3_lib = AC3FPUNAME;
    sprintf(m, "AC3, a 5.1 frame: %s %lu.%02lu ms, %s %lu.%02lu ms: %s used", AC3FPUNAME,
            (unsigned long)(f / 1000), (unsigned long)(f % 1000 / 10), AC3NAME,
            (unsigned long)(i / 1000), (unsigned long)(i % 1000 / 10), a3_lib);
    amce_log("video", m);
}

/* The library and a decoder (stereo): FALSE if not. */
static BOOL a3_open(char *m)
{
    memset(&a3, 0, offsetof(__typeof__(a3), pcm));
    a3.pts0 = ZZ_NO_PTS;
    a3_choose();
    if (!Ac3Base)
        Ac3Base = amce_open_own_lib(a3_lib, AC3VERSION);
    if (!Ac3Base && strcmp(a3_lib, AC3NAME))
        Ac3Base = amce_open_own_lib(AC3NAME, AC3VERSION);
    if (!Ac3Base) {
        strcpy(m, "no ac3.library: played without its sound");
        return FALSE;
    }
    if (!(a3.q = AllocVec(A3Q, MEMF_ANY)) || !(a3.dec = Ac3Open(AC3_STEREO, 0))) {
        strcpy(m, "not enough memory for its AC3: played without its sound");
        return FALSE;
    }
    sprintf(m, "its AC3 decoded by the CPU: %s %s", (const char *)Ac3Base->lib_Node.ln_Name,
            (const char *)Ac3Version());
    return TRUE;
}

static void a3_close(void)
{
    if (a3.dec)
        Ac3Close(a3.dec);
    if (a3.q)
        FreeVec(a3.q);
    a3.dec = NULL;
    a3.q = NULL;
    amce_close_own_lib(&Ac3Base);
}

static void a3_put(const volatile UBYTE *b, ULONG n)
{
    ULONG i;
    if (a3.qin - a3.qout + n > A3Q) {
        ULONG over = a3.qin - a3.qout + n - A3Q;
        a3.qout += over;                    /* (full: the oldest let go) */
        a3.lost += over;
    }
    for (i = 0; i < n; i++)
        a3.q[(a3.qin + i) & (A3Q - 1)] = b[i];
    a3.qin += n;
}

/* The bytes given to the card, n at b (the card's memory), looked through
 * for the AC3: the Program Stream's packets followed, whatever is not its
 * AC3 passed over whole. */
static void a3_tap(const volatile UBYTE *b, ULONG n)
{
    ULONG i = 0;
    while (i < n) {
        UBYTE c;
        switch (a3.state) {
        case A3_SKIP:
        case A3_TAKE: {
            ULONG k = n - i < a3.rem ? n - i : a3.rem;
            if (a3.state == A3_TAKE)
                a3_put(b + i, k);
            i += k;
            a3.rem -= k;
            if (!a3.rem) {
                a3.state = A3_SCAN;
                a3.sc = 0xFFFFFFFF;
            }
            continue;
        }
        case A3_SCAN:
            c = b[i++];
            a3.sc = (a3.sc << 8) | c;
            if ((a3.sc & 0xFFFFFF00UL) == 0x00000100UL) {
                a3.id = c;
                if (c == 0xBA) {
                    a3.state = A3_PACK1;
                } else if (c >= 0xBB) {
                    a3.state = A3_LEN;
                    a3.hn = 0;
                }
                a3.sc = 0xFFFFFFFF;
            }
            continue;
        default:
            break;
        }
        /* a header's bytes, one at a time (they are few) */
        c = b[i++];
        switch (a3.state) {
        case A3_PACK1:                      /* MPEG-2's pack: 9 more and its stuffing; MPEG-1's: 7 */
            if ((c & 0xC0) == 0x40) {
                a3.state = A3_PACK2;
                a3.hn = 0;
            } else {
                a3.state = A3_SKIP;
                a3.rem = 7;
            }
            break;
        case A3_PACK2:
            if (++a3.hn == 9) {
                a3.state = (c & 7) ? A3_SKIP : A3_SCAN;
                a3.rem = c & 7;
                a3.sc = 0xFFFFFFFF;
            }
            break;
        case A3_LEN:
            a3.h[a3.hn++] = c;
            if (a3.hn == 2) {
                a3.rem = ((ULONG)a3.h[0] << 8) | a3.h[1];
                a3.state = a3.id == 0xBD && a3.rem > 8 ? A3_PES : A3_SKIP;
                a3.hn = 0;
                if (!a3.rem)
                    a3.state = A3_SCAN;
            }
            break;
        case A3_PES:                        /* MPEG-2's PES header: flags, its length */
            a3.h[a3.hn++] = c;
            a3.rem--;
            if (a3.hn == 3) {
                if ((a3.h[0] & 0xC0) != 0x80 || a3.h[2] + 4UL > a3.rem) {
                    a3.state = A3_SKIP;     /* (MPEG-1's private stream: not a DVD's) */
                } else {
                    a3.want = a3.h[2];
                    a3.state = a3.want ? A3_PESH : A3_SUB;
                    a3.hn = 0;
                }
            }
            break;
        case A3_PESH:                       /* its time, if there (h[3..7]) */
            if (a3.hn < 5)
                a3.h[3 + a3.hn] = c;
            a3.hn++;
            a3.rem--;
            if (a3.hn == a3.want) {
                a3.state = A3_SUB;
                a3.hn = 0;
            }
            break;
        case A3_SUB:                        /* its substream, frames, first frame's place */
            a3.h[10 + a3.hn++] = c;
            a3.rem--;
            if (a3.hn == 4) {
                UBYTE sub = a3.h[10];
                if (!a3.sub && sub >= 0x80 && sub <= 0x87)
                    a3.sub = sub;           /* (its first AC3 track) */
                if (sub == a3.sub) {
                    if (a3.pts0 == ZZ_NO_PTS && (a3.h[1] & 0x80) && a3.want >= 5) {
                        const UBYTE *p = a3.h + 3;
                        a3.pts0 = ((ZzU64)((p[0] >> 1) & 7) << 30) | ((ZzU64)p[1] << 22) |
                                  ((ZzU64)(p[2] >> 1) << 15) | ((ZzU64)p[3] << 7) | (p[4] >> 1);
                    }
                    a3.state = a3.pts0 != ZZ_NO_PTS && a3.rem ? A3_TAKE : a3.rem ? A3_SKIP : A3_SCAN;
                } else
                    a3.state = a3.rem ? A3_SKIP : A3_SCAN;
                a3.sc = 0xFFFFFFFF;
            }
            break;
        default:
            a3.state = A3_SCAN;
            break;
        }
    }
}

/* One frame decoded from the ring into pcm (stereo): its samples a
 * channel, 0 if a whole frame is not there yet, -1 if bytes were passed
 * over (not a frame's start). */
static LONG a3_frame(struct Ac3Info *ai)
{
    ULONG avail = a3.qin - a3.qout, n, i;
    LONG off, len;
    if (avail < AC3_HEADER)
        return 0;
    n = avail < AC3_MAX_FRAME ? avail : AC3_MAX_FRAME;
    for (i = 0; i < n; i++)
        a3.frame[i] = a3.q[(a3.qout + i) & (A3Q - 1)];
    off = Ac3Find(a3.frame, (LONG)n, ai);
    if (off < 0) {
        a3.qout += n - (AC3_HEADER - 1);
        return n > AC3_HEADER - 1 ? -1 : 0;
    }
    if (off > 0) {
        a3.qout += (ULONG)off;
        return -1;
    }
    len = (LONG)ai->ai_FrameBytes;
    if ((ULONG)len > avail)
        return 0;
    a3.qout += (ULONG)len;
    if (Ac3Decode(a3.dec, a3.frame, a3.pcm, ai) != AC3_FRAME_SAMPLES) {
        a3.bad++;                           /* (its samples silence: played as such) */
        memset(a3.pcm, 0, sizeof(a3.pcm));
    }
    a3.frames++;
    return AC3_FRAME_SAMPLES;
}

/* -------------------------------------------------- MPEG-1 and MPEG-2 by the CPU
 * Where there is no ZZ9000 (or it is not to be used): mpegv.library 2
 * (extras/mpegv: its own demultiplexer and libmpeg2, opened at run time)
 * reads the file through a hook (a transport stream packed as a Program
 * Stream as it is read, tspack.c; a DVD's film from the disc, dvdplay.c)
 * and hands its sound's packets to another: MPEG audio queued for
 * mpega.library, decoded as an MP3 file's is (through the same bitstream
 * hook), AC3 (a DVD's private stream 1) for ac3.library as for the card. Each picture's planes (Y, Cb, Cr: 4:2:0) copied into one of
 * SWV_SLOTS buffers, held until its time by the sound (or the clock
 * without), then given to the UI (anim.c), which takes the newest it is
 * given and makes it what it shows: the card's YUV overlay's (st_swv_yuyv)
 * or RGB to draw (st_swv_rgb); one too late is let go, not copied.
 * Pictures are decoded as the sound is: on a slow Amiga the sound keeps
 * time and the pictures fall behind and are let go. */
#define SWV_SLOTS   3
#define SW_AQ       (128UL * 1024)          /* its sound's bytes, demuxed, for mpega */
#define SW_IN       16384
#define SW_HELD_MAX (6UL * 1024 * 1024)     /* video held by mpegv while its sound is sought */
enum { SW_FREE, SW_READY, SW_SHOW, SW_ON };
static struct {
    UBYTE *buf;
    ULONG size;
    volatile UBYTE state;
    UBYTE direct;                           /* to go straight onto the overlay when due */
    ULONG seq;
    ZzU64 pts;
} swv[SWV_SLOTS];
static ULONG swv_w, swv_h, swv_seq;
static ULONG swv_yw, swv_yh;              /* its planes' (whole macroblocks) */
static int swv_on = -1;                    /* the slot the UI has (SW_ON) */
static APTR sw_mpv;                         /* its decoder (mpegv.library) */
static UBYTE *sw_aq;
static ULONG sw_aq_in, sw_aq_out;
static LONG sw_apts;                        /* the first audio packet's time (90 kHz), -1 none */
static BOOL sw_sound;                       /* MPEG audio, by mpega.library */
static BOOL sw_ac3;                         /* AC3 (a DVD's, a transport stream's), by ac3.library */
/* The UI's overlay: the pictures written straight onto it from the
 * decoder's own planes, when they are due (no copy of each: a DVD
 * picture's 0.6 MB), else into a slot for the UI as before. */
static volatile BOOL sw_direct;             /* set by the UI (st_video_direct) */
static volatile BOOL sw_redraw;             /* the UI's new overlay wants the picture again */
static struct MpvPicture *sw_f;             /* the decoder's newest picture... */
static BOOL sw_f_ok;                        /* ...still valid (no MpvDecode since) */
static ULONG sw_ndirect;                    /* (pictures written straight to the overlay) */
static unsigned long long sw_tdirect;
/* A DVD's menus (dvdplay.c's own navigation): its subpictures (buttons,
 * subtitles) decoded from their packets and blended into each picture
 * they show over, the highlighted button with them, at the picture's own
 * size before it is shown (on the overlay or drawn alike). The last
 * picture is kept as it was (sw_clean) while the disc stands still, so a
 * button moved to is drawn again on it. */
static BOOL sw_nav;                         /* the disc's own navigation */
static struct Spu *sw_spu;
static UBYTE *sw_clean;
static BOOL sw_clean_ok;
static ULONG sw_clean_pts;
static ULONG sw_hl_drawn, sw_clut_drawn;    /* (what the last composed picture had) */
static const struct SpuPic *sw_spu_drawn;
static ULONG sw_ncomposed;

/* The CPU's pictures straight onto the UI's overlay (TRUE: it shows one
 * now), or not (drawn by the UI). From the UI. */
void st_video_direct(BOOL on)
{
    sw_direct = on;
}

/* The overlay was opened again (a new one is blank): the picture again. */
void st_video_redraw(void)
{
    sw_redraw = TRUE;
}

/* The decoder's picture written onto the overlay (FALSE: no overlay now). */
static BOOL sw_to_overlay(const struct MpvPicture *f)
{
    ULONG bpr, hz = 0, tk = time_ticks(&hz);
    UBYTE *mem = vo_lock(&bpr);
    if (!mem)
        return FALSE;
    MpvToYUV422((struct MpvPicture *)f, mem, bpr, MPVY_Y0UY1V);
    vo_unlock();
    sw_tdirect += time_ticks(&hz) - tk;
    sw_ndirect++;
    return TRUE;
}

/* mpegv.library (AmigaMCE's Libs, else LIBS:): opened by the UI when a
 * video is to be decoded by the CPU (video_check: its PROGDIR: is
 * AmigaMCE's), used by the playback process and the UI alike, let go after
 * each video (st_mpv_close: anim_play_video, once the playback process has
 * stopped), and at quitting. NULL if it is not there. */
struct Library *MpegvBase;

struct Library *st_mpv_open(void)
{
    if (!MpegvBase)
        MpegvBase = amce_open_own_lib(MPEGVNAME, MPEGVVERSION);
    return MpegvBase;
}

void st_mpv_close(void)
{
    amce_close_own_lib(&MpegvBase);
}

/* The newest picture given to show, if one is (FALSE: none new since the
 * last), and its size: the UI's (st_swv_rgb, st_swv_yuyv) until the next,
 * the one before it let go. For the UI. */
BOOL st_swv_take(ULONG *w, ULONG *h)
{
    int i, best = -1;
    ULONG bs = 0;
    Forbid();
    for (i = 0; i < SWV_SLOTS; i++)
        if (swv[i].state == SW_SHOW && (best < 0 || swv[i].seq > bs)) {
            best = i;
            bs = swv[i].seq;
        }
    if (best >= 0) {
        for (i = 0; i < SWV_SLOTS; i++)
            if (i != best && (swv[i].state == SW_ON || swv[i].state == SW_SHOW))
                swv[i].state = SW_FREE;
        swv[best].state = SW_ON;
        swv_on = best;
    }
    Permit();
    *w = swv_w;
    *h = swv_h;
    return best >= 0;
}

/* The UI's picture, as mpegv.library's helpers take one (FALSE: it has
 * none, or there is no library). */
static BOOL swv_pic(struct MpvPicture *p)
{
    int s = swv_on;
    UBYTE *b;
    if (s < 0 || !swv[s].buf || swv[s].state != SW_ON || !MpegvBase)
        return FALSE;
    b = swv[s].buf;
    memset(p, 0, sizeof(*p));
    p->mpic_Y = b;
    p->mpic_Cb = b + swv_yw * swv_yh;
    p->mpic_Cr = p->mpic_Cb + swv_yw * swv_yh / 4;
    p->mpic_Width = (UWORD)swv_w;
    p->mpic_Height = (UWORD)swv_h;
    p->mpic_YModulo = (UWORD)swv_yw;
    p->mpic_CModulo = (UWORD)(swv_yw / 2);
    p->mpic_YRows = (UWORD)swv_yh;
    return TRUE;
}

/* The UI's picture's planes, for drawing it (FALSE: none). */
BOOL st_swv_planes(struct MpvPicture *p)
{
    return swv_pic(p);
}

/* The UI's picture as 24-bit RGB into dst (bpr bytes a row). */
BOOL st_swv_rgb(UBYTE *dst, ULONG bpr)
{
    struct MpvPicture p;
    if (!swv_pic(&p))
        return FALSE;
    MpvToRGB(&p, dst, bpr, MPVR_RGB24);
    return TRUE;
}

/* The UI's picture as packed YUV 4:2:2 into dst (bpr bytes a row): the
 * ZZ9000's YUV422CGX overlay, Y0 Cb Y1 Cr (each chroma row twice), a
 * longword at a time (the card's memory, across the Zorro bus). */
BOOL st_swv_yuyv(UBYTE *dst, ULONG bpr)
{
    struct MpvPicture p;
    if (!swv_pic(&p))
        return FALSE;
    MpvToYUV422(&p, dst, bpr, MPVY_Y0UY1V);
    return TRUE;
}

/* Its pictures' buffers freed: by the UI, once it has stopped it. */
void st_swv_release(void)
{
    int i;
    for (i = 0; i < SWV_SLOTS; i++) {
        if (swv[i].buf)
            FreeVec(swv[i].buf);
        swv[i].buf = NULL;
        swv[i].size = 0;
        swv[i].state = SW_FREE;
    }
    swv_on = -1;
}

static void sw_aq_put(const UBYTE *d, ULONG n)
{
    while (n--) {
        if (sw_aq_in - sw_aq_out >= SW_AQ)
            sw_aq_out++;                    /* (full: the oldest let go) */
        sw_aq[sw_aq_in++ % SW_AQ] = *d++;
    }
}

/* A seek's prefix: the file's first headers, which the decoder needs
 * before anything it can decode (a Program Stream's first pack and system
 * headers, and its sequence header in a video packet of its own; a bare
 * stream's sequence header), given before the file from where it is
 * started. Many files have their sequence header only at their start. */
#define SW_PRE 1024
static UBYTE sw_pre[SW_PRE];
static ULONG sw_pre_len, sw_pre_pos;

/* Looked for in b (n bytes) from i: 00 00 01 id (id < 0: any); its offset
 * or -1. */
static LONG sw_find(const UBYTE *b, LONG n, LONG i, int id)
{
    for (; i + 4 <= n; i++)
        if (!b[i] && !b[i + 1] && b[i + 2] == 1 && (id < 0 || b[i + 3] == (UBYTE)id))
            return i;
    return -1;
}

/* For a start at st.range_from: the prefix made from the file's first
 * 64 KB, and the file from the first pack (a bare stream: sequence header,
 * else picture group) at or after it. FALSE if it cannot start there. */
static BOOL sw_seek_prep(void)
{
    UBYTE *b = AllocVec(V_SCAN, MEMF_ANY);
    LONG n, sq, se, i, at = -1, k = 0;
    char m[120];
    BOOL ok = FALSE;
    sw_pre_len = sw_pre_pos = 0;
    if (!b)
        return FALSE;
    if (st.v_ts) {
        /* a transport stream: its start packed, to find its first headers in */
        LONG got;
        tsp_begin(st.v_ts, st.v_vpid, st.v_apid, st.v_ac3, st.v_mpeg2);
        if (Seek(st.fh, (LONG)st.v_tsfirst, OFFSET_BEGINNING) < 0)
            goto done;
        for (n = 0; n < (LONG)V_SCAN && (got = tsp_pack(ts_read, NULL, b + n, V_SCAN - n)) > 0; n += got)
            ;
        if (n <= 0)
            goto done;
    } else if (Seek(st.fh, 0, OFFSET_BEGINNING) < 0 || (n = Read(st.fh, b, V_SCAN)) <= 0)
        goto done;
    /* the sequence header: to the next start code but its extensions' */
    if ((sq = sw_find(b, n, 0, 0xB3)) < 0)
        goto done;
    for (se = sq + 4; (se = sw_find(b, n, se, -1)) >= 0 && b[se + 3] == 0xB5; se += 4)
        ;
    if (se < 0 || se - sq > SW_PRE - 64)
        goto done;
    if (!st.v_es) {
        LONG pk = sw_find(b, n, 0, 0xBA), vp = -1, plen;
        BOOL mpeg2;
        if (pk < 0 || pk + 14 > n)
            goto done;
        mpeg2 = (b[pk + 4] & 0xC0) == 0x40;
        plen = mpeg2 ? 14 + (b[pk + 13] & 7) : 12;
        for (i = 0; i < plen; i++)
            sw_pre[k++] = b[pk + i];
        if (pk + plen + 6 <= n && sw_find(b, n, pk + plen, -1) == pk + plen && b[pk + plen + 3] == 0xBB) {
            LONG sl = 6 + ((b[pk + plen + 4] << 8) | b[pk + plen + 5]);
            if (sl < 256)
                for (i = 0; i < sl; i++)
                    sw_pre[k++] = b[pk + plen + i];
        }
        /* the video packet the sequence header is in: its stream's id */
        for (i = sq; i >= 0 && vp < 0; i--)
            if (!b[i] && !b[i + 1] && b[i + 2] == 1 && b[i + 3] >= 0xE0 && b[i + 3] <= 0xEF)
                vp = i;
        sw_pre[k++] = 0;
        sw_pre[k++] = 0;
        sw_pre[k++] = 1;
        sw_pre[k++] = vp >= 0 ? b[vp + 3] : 0xE0;
        sw_pre[k++] = (UBYTE)((se - sq + (mpeg2 ? 3 : 1)) >> 8);
        sw_pre[k++] = (UBYTE)(se - sq + (mpeg2 ? 3 : 1));
        if (mpeg2) {
            sw_pre[k++] = 0x80;             /* no time stamp */
            sw_pre[k++] = 0x00;
            sw_pre[k++] = 0x00;
        } else
            sw_pre[k++] = 0x0F;
    }
    for (i = sq; i < se; i++)
        sw_pre[k++] = b[i];
    if (st.v_ts) {
        /* a transport stream: from the packet there, packed afresh (the
         * pictures before its first I picture not shown: want_i) */
        ULONG pk = st.range_from > st.v_tsfirst
                       ? st.v_tsfirst + (st.range_from - st.v_tsfirst) / st.v_ts * st.v_ts
                       : st.v_tsfirst;
        if (Seek(st.fh, (LONG)pk, OFFSET_BEGINNING) < 0)
            goto done;
        tsp_begin(st.v_ts, st.v_vpid, st.v_apid, st.v_ac3, st.v_mpeg2);
        sw_pre_len = (ULONG)k;
        v_from_back = FALSE;
        sprintf(m, "from byte %lu (a transport stream's packet), its first %lu bytes of headers first",
                (unsigned long)pk, (unsigned long)k);
        amce_log("video: by the CPU", m);
        ok = TRUE;
        goto done;
    }
    /* the file from there: the picture group just before it if there is
     * one within a second, else the first pack (sequence header, picture
     * group) after it */
    {
        LONG back = v_back(st.range_from, TRUE);
        v_from_back = back >= 0;
        if (v_from_back)
            at = back - (LONG)st.range_from;
    }
    if (!v_from_back) {
        if (Seek(st.fh, (LONG)st.range_from, OFFSET_BEGINNING) < 0 || (n = Read(st.fh, b, V_SCAN)) <= 0)
            goto done;
        if (st.v_es) {
            LONG a = sw_find(b, n, 0, 0xB3), g = sw_find(b, n, 0, 0xB8);
            at = a >= 0 && (g < 0 || a < g) ? a : g;
        } else
            at = sw_find(b, n, 0, 0xBA);
        if (at < 0)
            goto done;
    }
    if (Seek(st.fh, (LONG)st.range_from + at, OFFSET_BEGINNING) < 0)
        goto done;
    sw_pre_len = (ULONG)k;
    sprintf(m, "from byte %lu (asked %lu%s), its first %lu bytes of headers first",
            (unsigned long)(st.range_from + at), (unsigned long)st.range_from,
            v_from_back ? ": the picture group before it" : "", (unsigned long)k);
    amce_log("video: by the CPU", m);
    ok = TRUE;
done:
    FreeVec(b);
    if (!ok) {
        sprintf(m, "a seek to byte %lu: no sequence header or pack found", (unsigned long)st.range_from);
        amce_log("video: by the CPU", m);
    }
    return ok;
}

/* mpegv.library's read hook: a seek's prefix, then the file's next bytes
 * (0 at its end, as an error was taken before) */
static ULONG sw_read(register struct Hook *h __asm("a0"), register APTR mpv __asm("a2"),
                     register struct MpvRead *r __asm("a1"))
{
    LONG n;
    (void)h;
    (void)mpv;
    if (sw_pre_pos < sw_pre_len) {
        ULONG k = sw_pre_len - sw_pre_pos;
        if (k > (ULONG)r->mr_Length)
            k = (ULONG)r->mr_Length;
        CopyMem(sw_pre + sw_pre_pos, r->mr_Buffer, k);
        sw_pre_pos += k;
        return k;
    }
    if (st.v_dvd)
        n = dvdplay_read(st.v_dvd, r->mr_Buffer, r->mr_Length);    /* (a DVD's film) */
    else if (st.v_ts)
        n = tsp_pack(ts_read, NULL, r->mr_Buffer, (ULONG)r->mr_Length);  /* (packed as read) */
    else
        n = Read(st.fh, r->mr_Buffer, r->mr_Length);
    if (n > 0)
        st.v_bytes += (ULONG)n;             /* (its information's data rate) */
    return n > 0 ? (ULONG)n : 0;
}

/* mpegv.library's packet hook: the file's first audio stream to the
 * sound's queue, its first time kept */
static ULONG sw_packet(register struct Hook *h __asm("a0"), register APTR mpv __asm("a2"),
                       register struct MpvPacket *p __asm("a1"))
{
    (void)h;
    (void)mpv;
    if (p->mp_Stream == 0xC0 && sw_sound) {
        if (sw_apts < 0 && (p->mp_Flags & MPVPF_PTS))
            sw_apts = (LONG)p->mp_PTS;
        sw_aq_put(p->mp_Data, p->mp_Length);
    } else if (p->mp_Stream == 0xBD && sw_spu && p->mp_Length > 1 && (p->mp_SubStream & 0xE0) == 0x20) {
        /* a DVD's subpicture (its menu's buttons, a subtitle): the stream
         * the disc says, after the substream's byte */
        BOOL forced;
        if ((LONG)(p->mp_SubStream & 31) == dvdplay_spu(st.v_dvd, &forced))
            spu_put(sw_spu, p->mp_Data + 1, p->mp_Length - 1, (p->mp_Flags & MPVPF_PTS) != 0,
                    (ULONG)p->mp_PTS);
    } else if (p->mp_Stream == 0xBD && sw_ac3 && p->mp_Length > 4 && p->mp_SubStream >= 0x80 &&
               p->mp_SubStream <= 0x87) {
        /* a DVD's AC3 (private stream 1): its first track's, after the
         * substream's 4 bytes (number, frames, first frame's place) */
        if (!a3.sub)
            a3.sub = (UBYTE)p->mp_SubStream;
        if (p->mp_SubStream == a3.sub) {
            if (a3.pts0 == ZZ_NO_PTS && (p->mp_Flags & MPVPF_PTS))
                a3.pts0 = p->mp_PTS;
            if (a3.pts0 != ZZ_NO_PTS)
                a3_put(p->mp_Data + 4, p->mp_Length - 4);
        }
    }
    return 0;
}

/* mpega's bytes: from the sound's queue (the file demuxed further as
 * needed). */
static LONG sw_aread(UBYTE *buf, LONG n)
{
    LONG got = 0;
    while (got < n) {
        if (sw_aq_in == sw_aq_out && !MpvPump(sw_mpv))
            break;
        while (got < n && sw_aq_out != sw_aq_in)
            buf[got++] = sw_aq[sw_aq_out++ % SW_AQ];
    }
    return got;
}

/* A measured step's time, for the log: "what 1.23 ms (n)" (each, on
 * average, to the hundredth of a millisecond). */
static void sw_stat(char *out, const char *what, unsigned long long ticks, ULONG n, ULONG hz)
{
    ULONG c = n && hz ? (ULONG)(ticks * 100000ULL / hz / n) : 0;
    sprintf(out + strlen(out), "%s%s %lu.%02lu ms (%lu)", out[0] ? ", " : "", what,
            (unsigned long)(c / 100), (unsigned long)(c % 100), (unsigned long)n);
}

static int sw_slot(void)
{
    int i;
    for (i = 0; i < SWV_SLOTS; i++)
        if (swv[i].state == SW_FREE)
            return i;
    return -1;
}

/* Slot s's planes, as mpegv.library's helpers take a picture */
static void sw_slot_pic(int s, struct MpvPicture *p)
{
    UBYTE *b = swv[s].buf;
    memset(p, 0, sizeof(*p));
    p->mpic_Y = b;
    p->mpic_Cb = b + swv_yw * swv_yh;
    p->mpic_Cr = p->mpic_Cb + swv_yw * swv_yh / 4;
    p->mpic_Width = (UWORD)swv_w;
    p->mpic_Height = (UWORD)swv_h;
    p->mpic_YModulo = (UWORD)swv_yw;
    p->mpic_CModulo = (UWORD)(swv_yw / 2);
    p->mpic_YRows = (UWORD)swv_yh;
}

/* The subpicture to show over a picture at pts now (NULL none), and the
 * highlighted button (hl->on) */
static const struct SpuPic *sw_spu_now(ULONG pts, struct SpuHl *hl)
{
    BOOL forced = FALSE;
    LONG k = dvdplay_spu(st.v_dvd, &forced);
    sw_hl_drawn = dvdplay_highlight(st.v_dvd, hl);
    dvdplay_clut(st.v_dvd, &sw_clut_drawn);
    return k >= 0 ? spu_at(sw_spu, pts, forced) : NULL;
}

/* Slot s (its planes the picture as it was: sw_clean's) with the
 * subpicture and highlight blended in, shown: onto the overlay, or given
 * to the UI. */
static void sw_compose_out(int s, const struct SpuPic *sp, const struct SpuHl *hl)
{
    ULONG ny = swv_yw * swv_yh, seq;
    UBYTE *b = swv[s].buf;
    const ULONG *clut = dvdplay_clut(st.v_dvd, &seq);
    spu_blend420(sp, hl, clut, b, b + ny, b + ny + ny / 4, swv_yw, swv_yw / 2, swv_w, swv_h);
    sw_spu_drawn = sp;
    sw_ncomposed++;
    if (sw_direct && !st.v_hidden) {
        ULONG bpr;
        UBYTE *mem = vo_lock(&bpr);
        if (mem) {
            struct MpvPicture p;
            sw_slot_pic(s, &p);
            MpvToYUV422(&p, mem, bpr, MPVY_Y0UY1V);
            vo_unlock();
            swv[s].state = SW_FREE;
            return;
        }
    }
    swv[s].seq = ++swv_seq;
    swv[s].state = st.v_hidden ? SW_FREE : SW_SHOW;
}

/* The last picture again, from sw_clean, with what shows over it now (a
 * button moved to while the disc stands still): FALSE if it cannot be. */
static BOOL sw_recompose(ULONG pts)
{
    struct SpuHl hl;
    const struct SpuPic *sp;
    int s = sw_slot();
    ULONG ny = swv_yw * swv_yh;
    if (!sw_clean_ok || s < 0)
        return FALSE;
    sp = sw_spu_now(pts, &hl);
    memcpy(swv[s].buf, sw_clean, ny * 3 / 2);
    sw_compose_out(s, sp, &hl);
    return TRUE;
}

static void swv_run(void)
{
    struct Hook hook;
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *ms = NULL;
    struct Hook rhook, phook;
    struct MpvInfo mi;
    static WORD pcm0[MPEGA_PCM_SIZE], pcm1[MPEGA_PCM_SIZE];
    WORD *pcm[2];
    ULONG f0 = 0, t0 = 0, rate = 0, period = 3600, hold, drop, run = 0, pre, adiv = 1, pcm_peak = 0;
    ZzU64 origin = 0, pts0 = 0, dry_m = 0;
    ULONG dry_t = 0;                        /* (its sound run dry since: v_clock_on) */
    ULONG shown_t = 0, n_stall = 0;         /* (the last picture shown: when) */
    int held = -1, i;
    BOOL on = FALSE, done = FALSE, out_paused = FALSE, a_done = FALSE;
    /* behind: B pictures passed over (mpegv.library), until enough are on time
     * again to try them (twice as many each time it fell behind again) */
    BOOL skip_b = FALSE;
    ULONG b_ontime = 0, b_need = 30, b_turns = 0, b_late = 0, b_okrun = 0;
    /* a seek: nothing shown until an I picture (those before it are built
     * on pictures from before the seek, never decoded: blocks of others) */
    BOOL want_i = st.range_from != 0;
    ULONG i_skipped = 0;
    /* a DVD's menus: standing still (since still_t0, still_s seconds), the
     * output played out before going on (since drain_t0), a part's start
     * (seg_t0) and how many parts */
    BOOL still_on = FALSE;
    ULONG still_t0 = 0, drain_t0 = 0, seg_t0 = time_ms(), n_seg = 1, n_still = 0, n_hop = 0;
    LONG still_s = 0;
    char m[200];
    /* (measured: each step's E-clock ticks, and how many) */
    unsigned long long t_dec = 0, t_conv = 0, t_aud = 0;
    ULONG n_dec = 0, n_conv = 0, n_aud = 0, hz = 0, tk;
    pcm[0] = pcm0;
    pcm[1] = pcm1;
    st.v_shown = st.v_dropped = 0;
    st.v_ms = 0;
    sw_aq_in = sw_aq_out = 0;
    sw_apts = -1;
    sw_sound = st.v_mp2 && !st.v_es && !st.v_ac3;
    sw_ac3 = st.v_ac3 && !st.v_es;
    sw_mpv = NULL;
    sw_f = NULL;
    sw_f_ok = FALSE;
    sw_redraw = FALSE;
    sw_ndirect = 0;
    sw_tdirect = 0;
    if ((v_tport = CreateMsgPort()) &&
        (v_treq = (struct timerequest *)CreateIORequest(v_tport, sizeof(struct timerequest))) &&
        !OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)v_treq, 0))
        v_topen = TRUE;
    if (!st_mpv_open()) {
        fail(_("MPEG video by the CPU needs mpegv.library"));
        goto out;
    }
    if (sw_ac3) {
        /* its AC3 by ac3.library (a 68040 or better), as for the card */
        const char *why = st_ac3_cpu_why();
        if (why) {
            sprintf(m, "%s: played without its sound", why);
            sw_ac3 = FALSE;
        } else if (!a3_open(m)) {
            a3_close();                     /* (whatever it did open) */
            sw_ac3 = FALSE;
        }
        amce_log("video", m);
    }
    if (st.v_ts && !st.range_from)
        tsp_begin(st.v_ts, st.v_vpid, st.v_apid, st.v_ac3, st.v_mpeg2);    /* (packed as read) */
    if (sw_sound && !(sw_aq = AllocVec(SW_AQ, MEMF_ANY))) {
        fail(_("Not enough memory"));
        goto out;
    }
    /* its pictures' buffers (kept from the last if large enough) */
    Forbid();
    swv_on = -1;                            /* (the UI's let go: these are new) */
    Permit();
    swv_w = st.v_w;
    swv_h = st.v_h;
    swv_yw = (swv_w + 15) & ~15UL;
    swv_yh = (swv_h + 15) & ~15UL;
    for (i = 0; i < SWV_SLOTS; i++) {
        ULONG need = swv_yw * swv_yh * 3 / 2;
        if (swv[i].size < need) {
            if (swv[i].buf)
                FreeVec(swv[i].buf);
            swv[i].size = 0;
            if (!(swv[i].buf = AllocVec(need, MEMF_ANY))) {
                fail(_("Not enough memory"));
                goto out;
            }
            swv[i].size = need;
        }
        swv[i].state = SW_FREE;
    }
    /* a DVD's menus: its subpictures, and the last picture as it was */
    sw_nav = st.v_dvd && dvdplay_is_nav(st.v_dvd);
    sw_spu = NULL;
    sw_clean_ok = FALSE;
    sw_spu_drawn = NULL;
    sw_ncomposed = 0;
    if (sw_nav && (!(sw_spu = spu_new()) || !(sw_clean = AllocVec(swv_yw * swv_yh * 3 / 2, MEMF_ANY)))) {
        fail(_("Not enough memory"));
        goto out;
    }
    /* its decoder: the file through the read hook (16 KB at a time), its
     * audio packets to the queue (a Program Stream's first are read now) */
    memset(&rhook, 0, sizeof(rhook));
    memset(&phook, 0, sizeof(phook));
    rhook.h_Entry = (HOOKFUNC)(APTR)sw_read;
    phook.h_Entry = (HOOKFUNC)(APTR)sw_packet;
    sw_pre_len = sw_pre_pos = 0;
    if (st.range_from && !sw_seek_prep()) {
        fail(_("The file could not be read there"));
        goto out;
    }
    if (!(sw_mpv = MpvOpenTags(MPVA_ReadHook, (ULONG)&rhook, MPVA_PacketHook, (ULONG)&phook,
                               MPVA_Stream, st.v_es ? MPVS_VIDEO : MPVS_PROGRAM,
                               MPVA_ReadSize, SW_IN, TAG_DONE))) {
        fail(_("Not enough memory"));
        goto out;
    }
    if (sw_sound) {
        /* its sound by mpega.library, as an MP3 file's: the same quality
         * and rate for the CPU (stream.c's mpega_run) */
        WORD q = st.cpu >= 60 ? 2 : st.cpu >= 40 ? 1 : 0, mono = st.cpu < 40, div = 1;
        LONG fmax = st.cpu >= 60 && st.ahi ? 48000 : st.cpu >= 40 ? 28000 : st.cpu >= 30 ? 24000 : 12000;
        LONG f = st.v_hz ? (LONG)st.v_hz : 44100;
        MPEGA_LAYER *ls[2];
        int k;
        if (!MPEGABase && !(MPEGABase = OpenLibrary((CONST_STRPTR)"mpega.library", 2))) {
            amce_log("video", "no mpega.library: its sound left out");
            sw_sound = FALSE;
        } else {
            while (div < 4 && f / div > fmax)
                div *= 2;
            memset(&hook, 0, sizeof(hook));
            hook.h_Entry = (HOOKFUNC)(APTR)hook_func;
            memset(&ctrl, 0, sizeof(ctrl));
            ctrl.bs_access = &hook;
            ls[0] = &ctrl.layer_1_2;
            ls[1] = &ctrl.layer_3;
            for (k = 0; k < 2; k++) {
                ls[k]->force_mono = mono;
                ls[k]->mono.freq_div = div;
                ls[k]->mono.quality = q;
                ls[k]->mono.freq_max = fmax;
                ls[k]->stereo = ls[k]->mono;
            }
            sw_on = TRUE;                   /* (the hook: from the queue) */
            if (!(ms = MPEGA_open((char *)"AmigaMCE video", &ctrl))) {
                amce_log("video", "mpega.library would not take its sound: left out");
                sw_sound = FALSE;
            } else {
                st.freq = ms->frequency;
                st.channels = ms->channels;
                st.dec_freq = ms->dec_frequency;
                st.dec_channels = ms->dec_channels;
                if (!st.ahi && (st.dec_freq > 28867 || st.dec_freq < 4000))
                    st.dec_freq = st.dec_freq > 28867 ? st.dec_freq / 2 : 22050;
                rate = (ULONG)st.dec_freq;
                st.v_adec = VSD_MPEGA;
                st.v_akbps = st.v_probe_kbps;
                st.v_achans = (UBYTE)ms->channels;
            }
        }
    }
    sprintf(m, "%s: %ld x %ld MPEG-%ld by the CPU (%s)%s; %s", log_name(), (long)st.v_w,
            (long)st.v_h, st.v_mpeg2 ? 2L : 1L, (const char *)MpvVersion(),
            st.v_dvd ? ", a DVD's" : st.v_ts ? ", a transport stream packed as it is read"
                                           : st.v_es ? ", a bare video stream" : "",
            sw_sound ? "its sound by mpega.library" : sw_ac3 ? "its sound (AC3) by the CPU" : "no sound");
    amce_log("video", m);
    if (sw_sound) {
        sprintf(m, "its sound: %ld Hz %s, decoded at %ld Hz %s through %s", (long)st.freq,
                st.channels == 2 ? "stereo" : "mono", (long)st.dec_freq,
                st.dec_channels == 2 ? "stereo" : "mono", st.ahi ? "AHI" : "Paula");
        amce_log("video", m);
    }
    st.state = ST_BUFFERING;
    pre = (ULONG)st.dec_freq * 300 / 1000;
    hold = period / 2;
    drop = period > 3600 ? period : 3600;
    /* a DVD's jump (a menu's button, its menu, the disc's own): what was
     * decoded and its sound let go, the next part started as the first was
     * (its first picture, its sound with it, timed afresh) */
#define SEG_FLUSH()                                                                  \
    do {                                                                             \
        MpvControl(sw_mpv, MPVC_FLUSH, 0);                                           \
        sw_f_ok = FALSE;                                                             \
        if (held >= 0) {                                                             \
            swv[held].state = SW_FREE;                                               \
            held = -1;                                                               \
        }                                                                            \
        done = a_done = on = FALSE;                                                  \
        if (st.started)                                                              \
            out_stop();                                                              \
        st.f_out = st.f_in;                                                          \
        if (sw_ac3) {                                                                \
            a3.qout = a3.qin;                                                        \
            a3.sub = 0;                                                              \
            a3.pts0 = ZZ_NO_PTS;                                                     \
        }                                                                            \
        sw_aq_out = sw_aq_in;                                                        \
        sw_apts = -1;                                                                \
        spu_reset(sw_spu);                                                           \
        sw_spu_drawn = NULL;                                                         \
        if (skip_b) {                                                                \
            skip_b = FALSE;                                                          \
            MpvControl(sw_mpv, MPVC_SKIPB, FALSE);                                   \
        }                                                                            \
        run = b_late = b_okrun = b_ontime = 0;                                       \
        seg_t0 = time_ms();                                                          \
        shown_t = seg_t0;                                                            \
        drain_t0 = 0;                                                                \
        n_seg++;                                                                     \
    } while (0)
    while (!st.quit) {
        BOOL busy = FALSE;
        serve();
        if (sw_redraw) {
            /* a new overlay (blank): the newest picture on it again (paused,
             * or until the next is due) */
            sw_redraw = FALSE;
            if (sw_nav && sw_clean_ok)
                sw_recompose(sw_clean_pts);         /* (with what shows over it) */
            else if (sw_direct && sw_f_ok)
                sw_to_overlay(sw_f);
        }
        if (st.paused) {
            if (on && st.started && !out_paused) {
                out_stop();
                out_paused = TRUE;
            }
            v_wait(20000);
            continue;
        }
        if (out_paused) {
            out_start();
            out_paused = FALSE;
        }
        /* a DVD's menus: what the viewer asked done; a jump they asked for at
         * once; standing still till its time is up or a button is pressed */
        if (sw_nav) {
            BOOL user = FALSE;
            int pe;
            dvdplay_service(st.v_dvd);
            if (still_on)
                dvdplay_poll(st.v_dvd);     /* (the disc's events while it stands) */
            pe = dvdplay_pending(st.v_dvd, &still_s, &user);
            if (pe == DVDP_HOP && (user || still_on)) {
                n_hop++;
                still_on = FALSE;
                SEG_FLUSH();
                dvdplay_go(st.v_dvd);
                continue;
            }
        }
        /* its sound decoded while there is room (a second and a half ahead) */
        while (sw_sound && !a_done && st.f_in - st.f_out < (ULONG)st.dec_freq * 3 / 2 &&
               st.f_in - st.f_out < FIFO_N - MPEGA_PCM_SIZE) {
            LONG n, k;
            ULONG o;
            tk = time_ticks(&hz);
            n = MPEGA_decode_frame(ms, pcm);
            t_aud += time_ticks(&hz) - tk;
            n_aud++;
            if (n == -2)
                continue;                   /* (a bad frame: the next) */
            if (n < 0) {
                a_done = TRUE;
                break;
            }
            o = st.f_in;
            for (k = 0; k < n; k++, o++) {
                st.fl[o & (FIFO_N - 1)] = pcm0[k];
                st.fr[o & (FIFO_N - 1)] = st.dec_channels == 2 ? pcm1[k] : pcm0[k];
            }
            st.f_in = o;
            busy = TRUE;
            if (!on)
                break;                      /* (before the first picture: a frame a turn) */
        }
        /* its AC3, decoded here into the FIFO (a second ahead): before the
         * first picture as fast as it comes, then a frame or two a turn
         * (more while the sound runs low), the stream demultiplexed further
         * while a whole frame is not there yet */
        if (sw_ac3 && !a_done) {
            struct Ac3Info ai;
            int k, most = !on ? 12 : rate && st.f_in - st.f_out < (ULONG)st.dec_freq / 4 ? 6 : 2;
            for (k = 0; k < most; k++) {
                ULONG room = FIFO_N - (st.f_in - st.f_out), j, o;
                LONG got;
                if (rate && (st.f_in - st.f_out >= (ULONG)st.dec_freq ||
                             room < AC3_FRAME_SAMPLES / adiv + 64))
                    break;
                tk = time_ticks(&hz);
                while (!(got = a3_frame(&ai)) && (ULONG)MpvControl(sw_mpv, MPVC_HELD, 0) < SW_HELD_MAX &&
                       MpvPump(sw_mpv))
                    ;
                t_aud += time_ticks(&hz) - tk;
                if (!got) {
                    if (MpvControl(sw_mpv, MPVC_ENDED, 0))
                        a_done = TRUE;      /* (its end: no more of its sound) */
                    break;
                }
                if (got < 0)
                    continue;
                n_aud++;
                if (!rate) {
                    rate = ai.ai_Rate;
                    while ((!st.ahi && rate / adiv > 28867) || rate / adiv > 48000)
                        adiv *= 2;
                    st.freq = (LONG)rate;
                    st.channels = 2;
                    st.dec_freq = (LONG)(rate / adiv);
                    st.dec_channels = 2;
                    pre = (ULONG)st.dec_freq * 300 / 1000;
                    st.v_adec = VSD_CPU_AC3;
                    st.v_akbps = ai.ai_BitRate / 1000;
                    st.v_achans = (UBYTE)ai.ai_Channels;
                    st.v_alfe = (UBYTE)ai.ai_LFE;
                    sprintf(m, "its sound: AC3 %lu Hz, %lu kbit/s, %lu.%lu channels, mixed to stereo, "
                               "played at %lu Hz through %s; it begins at %lu ms",
                            (unsigned long)rate, (unsigned long)(ai.ai_BitRate / 1000),
                            (unsigned long)ai.ai_Channels, (unsigned long)ai.ai_LFE,
                            (unsigned long)(rate / adiv), st.ahi ? "AHI" : "Paula",
                            (unsigned long)(a3.pts0 / 90));
                    amce_log("video", m);
                }
                o = st.f_in;
                for (j = 0; j + adiv <= (ULONG)got; j += adiv) {
                    LONG al = 0, ar = 0;
                    ULONG q;
                    for (q = 0; q < adiv; q++) {
                        WORD l = a3.pcm[(j + q) * 2];
                        ULONG al_ = (ULONG)(l < 0 ? -l : l);
                        if (al_ > pcm_peak)
                            pcm_peak = al_;
                        al += l;
                        ar += a3.pcm[(j + q) * 2 + 1];
                    }
                    st.fl[o & (FIFO_N - 1)] = (WORD)(al / (LONG)adiv);
                    st.fr[o & (FIFO_N - 1)] = (WORD)(ar / (LONG)adiv);
                    o++;
                }
                st.f_in = o;
                busy = TRUE;
            }
        }
        /* a DVD's still: what shows over its picture drawn again as it
         * changes, till its time is up (its sound decoded on meanwhile) */
        if (sw_nav) {
            if (still_on) {
                int pe = dvdplay_pending(st.v_dvd, &still_s, NULL);
                if (pe != DVDP_STILL) {
                    still_on = FALSE;       /* (the end, or on by itself) */
                    if (pe == DVDP_END)
                        break;
                    SEG_FLUSH();
                    continue;
                }
                {
                    /* what shows over it now (a button moved to, a
                     * subpicture come after the picture) drawn again */
                    struct SpuHl hl;
                    ULONG cq, pts = sw_clean_pts + (time_ms() - still_t0) * 90;
                    BOOL forced = FALSE;
                    LONG k = dvdplay_spu(st.v_dvd, &forced);
                    const struct SpuPic *sp = k >= 0 ? spu_at(sw_spu, pts, forced) : NULL;
                    dvdplay_clut(st.v_dvd, &cq);
                    if (dvdplay_highlight(st.v_dvd, &hl) != sw_hl_drawn || cq != sw_clut_drawn ||
                        sp != sw_spu_drawn)
                        sw_recompose(pts);
                }
                if (still_s != 255 /* (till a button) */ && time_ms() - still_t0 >= (ULONG)still_s * 1000) {
                    still_on = FALSE;       /* its time up: on */
                    dvdplay_go(st.v_dvd);
                    SEG_FLUSH();
                    continue;
                }
                v_wait(20000);
                continue;
            }
        }
        /* the next picture, into a free buffer */
        if (held < 0 && !done) {
            struct MpvPicture *f;
            LONG e = MPVE_OK;
            int s = sw_slot();
            if (s < 0) {
                v_wait(2000);               /* (the UI has them all) */
                continue;
            }
            tk = time_ticks(&hz);
            sw_f_ok = FALSE;                /* (the last one's planes: the decoder's again) */
            f = MpvDecode(sw_mpv, &e);
            if (f) {
                t_dec += time_ticks(&hz) - tk;
                n_dec++;
                sw_f = f;
                sw_f_ok = TRUE;
            }
            if (!f) {
                if (e != MPVE_MORE)
                    done = TRUE;            /* (its end, or it could not be read) */
            } else if (want_i && f->mpic_Type != MPVT_I) {
                i_skipped++;                /* (a seek's: built on pictures not decoded) */
                busy = TRUE;
            } else {
                if (want_i) {
                    want_i = FALSE;
                    sprintf(m, "after the seek: %lu pictures before its first I picture not shown",
                            (unsigned long)i_skipped);
                    amce_log("video: by the CPU", m);
                }
                swv[s].pts = (f->mpic_Flags & MPVPF_PTS) ? f->mpic_PTS : f->mpic_Time;
                swv[s].direct = 0;
                if ((!period || st.v_shown + st.v_dropped == 0) && MpvGetInfo(sw_mpv, &mi) &&
                    mi.mi_Period) {
                    period = mi.mi_Period;
                    hold = period / 2;
                    drop = period > 3600 ? period : 3600;
                }
                held = s;
                swv[s].state = SW_READY;
                busy = TRUE;
                /* converted now unless already late (then let go below) */
                {
                    ZzU64 now = !on ? 0 : (sw_sound || sw_ac3) && st.started
                                              ? v_clock_on(v_clock(origin, f0, rate, adiv), &dry_t,
                                                           &dry_m)
                                              : pts0 + (ZzU64)(time_ms() - t0) * 90;
                    if (on && (long long)(swv[s].pts - now) < -(long long)drop && run < 2)
                        swv[s].pts |= 1ULL << 62;   /* (marked: not converted) */
                    else if (sw_direct && f->mpic_YModulo == swv_yw && f->mpic_YRows == swv_yh) {
                        swv[s].direct = 1;          /* (straight to the overlay when due) */
                    } else {
                        tk = time_ticks(&hz);
                        ULONG ny = swv_yw * swv_yh;
                        if (f->mpic_YModulo != swv_yw || f->mpic_YRows != swv_yh) {
                            swv[s].pts |= 1ULL << 62;   /* (not the size it said: not shown) */
                        } else {
                            memcpy(swv[s].buf, f->mpic_Y, ny);
                            memcpy(swv[s].buf + ny, f->mpic_Cb, ny / 4);
                            memcpy(swv[s].buf + ny + ny / 4, f->mpic_Cr, ny / 4);
                        }
                        t_conv += time_ticks(&hz) - tk;
                        n_conv++;
                    }
                }
            }
        }
        /* started: the first picture, the sound with it */
        if (!on && held >= 0) {
            if ((sw_sound || sw_ac3) && !a_done && (!rate || st.f_in - st.f_out < pre) &&
                !MpvControl(sw_mpv, MPVC_ENDED, 0) && !(sw_nav && time_ms() - seg_t0 > 800)) {
                if (!busy)
                    v_wait(2000);
                continue;
            }
            f0 = st.f_out;
            if ((sw_sound || sw_ac3) && rate && st.f_in != st.f_out)
                out_start();
            t0 = time_ms();
            pts0 = swv[held].pts;
            origin = sw_sound && sw_apts >= 0 ? (ZzU64)(ULONG)sw_apts
                     : sw_ac3 && a3.pts0 != ZZ_NO_PTS ? a3.pts0 : pts0;
            on = TRUE;
            st.state = ST_PLAYING;
            if (n_seg <= 1 || n_seg % 50 == 0) {   /* (a DVD's menus: not every part's) */
                sprintf(m, "playing%s: the first picture at %lu ms, its sound's at %lu ms, %s",
                        n_seg > 1 ? " again" : "", (unsigned long)(pts0 / 90),
                        (unsigned long)(origin / 90), st.started ? "timed by its sound" : "timed by the clock");
                amce_log("video", m);
            }
        }
        /* a DVD's menu: its sound come after its first picture, from here */
        if (sw_nav && on && !st.started && !out_paused && sw_ac3 && rate && a3.pts0 != ZZ_NO_PTS &&
            st.f_in - st.f_out >= pre) {
            f0 = st.f_out;
            origin = a3.pts0;
            out_start();
        }
        /* the held picture: given to show on time, let go if late */
        if (on && held >= 0) {
            BOOL raw = (swv[held].pts >> 62) & 1;
            ZzU64 pts = swv[held].pts & ~(1ULL << 62);
            ZzU64 master = (sw_sound || sw_ac3) && st.started
                               ? v_clock_on(v_clock(origin, f0, rate, adiv), &dry_t, &dry_m)
                               : pts0 + (ZzU64)(time_ms() - t0) * 90;
            long long drift = (long long)(pts - master);
            st.v_drift = (LONG)(drift / 90);
            st.v_bskipped = (ULONG)MpvControl(sw_mpv, MPVC_SKIPPEDB, 0);
            if (!raw && drift > (long long)hold) {
                ULONG us = (ULONG)((drift - (long long)hold) * 1000 / 90);
                v_wait(us > 10000 ? 10000 : us < 1000 ? 1000 : us);
                continue;                   /* early: soon */
            }
            if (raw || (drift < -(long long)drop && run < 2)) {
                swv[held].state = SW_FREE;  /* late: let go */
                st.v_dropped++;
                run++;
                b_ontime = 0;
                b_okrun = 0;
                /* behind, not a start's stumble: three late without thirty
                 * on time between, after its first second */
                if (!skip_b && ++b_late >= 3 && time_ms() - t0 > 1000) {
                    /* behind: its B pictures not decoded at all */
                    skip_b = TRUE;
                    MpvControl(sw_mpv, MPVC_SKIPB, TRUE);
                    if (b_turns++)
                        b_need = b_need < 960 ? b_need * 2 : b_need;
                    if (b_turns <= 3) {
                        sprintf(m, "behind at %lu ms: B pictures passed over", (unsigned long)(pts / 90));
                        amce_log("video: by the CPU", m);
                    }
                }
            } else {
                BOOL direct = swv[held].direct;
                struct SpuHl hl;
                const struct SpuPic *sp = NULL;
                struct DvdNow now;
                ULONG ny = swv_yw * swv_yh;
                swv[held].direct = 0;
                swv[held].pts = pts;
                swv[held].seq = ++swv_seq;
                if (sw_nav) {
                    sp = sw_spu_now((ULONG)pts, &hl);
                    dvdplay_now(st.v_dvd, &now);
                }
                if (sw_nav && (sp || now.menu) && (!direct || sw_f_ok)) {
                    /* a menu's, or under a subpicture: kept as it is, then
                     * what shows over it blended in */
                    if (direct) {
                        memcpy(sw_clean, sw_f->mpic_Y, ny);
                        memcpy(sw_clean + ny, sw_f->mpic_Cb, ny / 4);
                        memcpy(sw_clean + ny + ny / 4, sw_f->mpic_Cr, ny / 4);
                        memcpy(swv[held].buf, sw_clean, ny * 3 / 2);
                    } else
                        memcpy(sw_clean, swv[held].buf, ny * 3 / 2);
                    sw_clean_ok = TRUE;
                    sw_clean_pts = (ULONG)pts;
                    sw_compose_out(held, sp, &hl);
                } else if (direct && !st.v_hidden && sw_f_ok && sw_direct && sw_to_overlay(sw_f))
                    swv[held].state = SW_FREE;      /* (on the overlay already) */
                else {
                    if (direct && sw_f_ok) {
                        /* no overlay now after all: a copy for the UI */
                        memcpy(swv[held].buf, sw_f->mpic_Y, ny);
                        memcpy(swv[held].buf + ny, sw_f->mpic_Cb, ny / 4);
                        memcpy(swv[held].buf + ny + ny / 4, sw_f->mpic_Cr, ny / 4);
                    }
                    swv[held].state = st.v_hidden || (direct && !sw_f_ok) ? SW_FREE : SW_SHOW;
                }
                st.v_shown++;
                shown_t = time_ms();
                run = 0;
                if (!skip_b && ++b_okrun >= 30)
                    b_late = 0;
                if (skip_b && drift >= 0 && ++b_ontime >= b_need) {
                    skip_b = FALSE;         /* caught up: its B pictures tried again */
                    MpvControl(sw_mpv, MPVC_SKIPB, FALSE);
                    b_ontime = 0;
                    b_late = b_okrun = 0;
                }
            }
            if (pts >= pts0)
                st.v_ms = (ULONG)((pts - pts0) / 90);
            held = -1;
            busy = TRUE;
        }
        if (done && held < 0) {
            /* a DVD's menus: the disc stopped giving for a reason of its own */
            int pe = sw_nav ? dvdplay_pending(st.v_dvd, &still_s, NULL) : DVDP_NONE;
            if (pe == DVDP_WAIT) {
                /* all it gave shown: on (the decoder taking more again) */
                dvdplay_go(st.v_dvd);
                MpvControl(sw_mpv, MPVC_FLUSH, 0);
                sw_f_ok = FALSE;
                done = FALSE;
                continue;
            }
            if (pe == DVDP_HOP) {
                /* its own jump (a menu going round, a title's end): what
                 * came before it played out, then on */
                if (st.started && st.f_in != st.f_out && !drain_t0)
                    drain_t0 = time_ms() ? time_ms() : 1;
                if (drain_t0 && st.f_in != st.f_out && time_ms() - drain_t0 < 1500) {
                    v_wait(10000);
                    continue;
                }
                n_hop++;
                SEG_FLUSH();
                dvdplay_go(st.v_dvd);
                continue;
            }
            if (pe == DVDP_STILL) {
                /* its last picture stands (its buttons over it) */
                still_on = TRUE;
                still_t0 = time_ms();
                n_still++;
                sw_hl_drawn = ~0UL;         /* (drawn again with what shows now) */
                continue;
            }
            break;
        }
        /* nothing shown for a while, not standing still or paused: how it
         * stands, for the log (a few times) */
        if (!shown_t)
            shown_t = time_ms() ? time_ms() : 1;
        if (time_ms() - shown_t > 4000 && n_stall < 4) {
            n_stall++;
            shown_t = time_ms();
            sprintf(m, "4 s with nothing shown: on %ld, held %ld, done %ld, sound over %ld, out %ld, "
                       "%lu frames queued, %lu bytes held, read all %ld, DVD stop %ld, drift %ld ms",
                    (long)on, (long)held, (long)done, (long)a_done, (long)st.started,
                    (unsigned long)(st.f_in - st.f_out), (unsigned long)MpvControl(sw_mpv, MPVC_HELD, 0),
                    (long)MpvControl(sw_mpv, MPVC_ENDED, 0),
                    (long)(sw_nav ? dvdplay_pending(st.v_dvd, NULL, NULL) : -1), (long)st.v_drift);
            amce_log("video: by the CPU", m);
        }
        if (!busy)
            v_wait(3000);
    }
#undef SEG_FLUSH
    /* the end: its sound plays out */
    while (!st.quit && st.started && st.f_in != st.f_out) {
        ULONG got = st_wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C)
            break;
        out_take();
    }
    sprintf(m, "%s: %lu pictures shown, %lu let go (late), %lu B pictures passed over (behind %lu "
               "time%s); %lu dropouts",
            log_name(), (unsigned long)st.v_shown, (unsigned long)st.v_dropped,
            (unsigned long)MpvControl(sw_mpv, MPVC_SKIPPEDB, 0), (unsigned long)b_turns, b_turns == 1 ? "" : "s",
            (unsigned long)st.underruns);
    amce_log("video", m);
    m[0] = 0;
    sw_stat(m, "a picture decoded", t_dec, n_dec, hz);
    sw_stat(m, "copied", t_conv, n_conv, hz);
    sw_stat(m, sw_ac3 ? "an AC3 frame decoded" : "a sound frame decoded", t_aud, n_aud, hz);
    if (sw_ndirect)
        sw_stat(m, "written straight to the overlay", sw_tdirect, sw_ndirect, hz);
    sprintf(m + strlen(m), "; a picture every %lu.%02lu ms; a 680%ld",
            (unsigned long)(period / 90), (unsigned long)(period % 90 * 100 / 90), (long)st.cpu);
    amce_log("video: by the CPU", m);
    if (sw_ac3) {
        sprintf(m, "its AC3 by the CPU: %lu frames decoded, %lu of them bad, %lu bytes lost (too far "
                   "ahead), %lu left; peak %lu of 32767",
                (unsigned long)a3.frames, (unsigned long)a3.bad, (unsigned long)a3.lost,
                (unsigned long)(a3.qin - a3.qout), (unsigned long)pcm_peak);
        amce_log("video", m);
    }
    if (sw_nav) {
        ULONG dec = 0, bad = 0;
        spu_counts(sw_spu, &dec, &bad);
        sprintf(m, "a DVD's menus: %lu parts (%lu jumps), %lu stills; %lu subpictures decoded, %lu not; "
                   "%lu pictures with something over them",
                (unsigned long)n_seg, (unsigned long)n_hop, (unsigned long)n_still, (unsigned long)dec,
                (unsigned long)bad, (unsigned long)sw_ncomposed);
        amce_log("video", m);
    }
    if (st.v_ts) {
        ULONG rs, cut, pr;
        tsp_counts(&rs, &cut, &pr);
        sprintf(m, "its transport stream: %lu times out of step, %lu PES packets not read",
                (unsigned long)rs, (unsigned long)cut);
        amce_log("video", m);
    }
out:
    sw_on = FALSE;
    sw_f_ok = FALSE;
    spu_free(sw_spu);
    sw_spu = NULL;
    if (sw_clean)
        FreeVec(sw_clean);
    sw_clean = NULL;
    sw_clean_ok = FALSE;
    sw_nav = FALSE;
    if (sw_ac3)
        a3_close();
    sw_ac3 = FALSE;
    if (ms)
        MPEGA_close(ms);
    if (sw_mpv)
        MpvClose(sw_mpv);
    sw_mpv = NULL;
    if (sw_aq)
        FreeVec(sw_aq);
    sw_aq = NULL;
    for (i = 0; i < SWV_SLOTS; i++)
        if (swv[i].state == SW_READY)
            swv[i].state = SW_FREE;
    if (v_topen) {
        CloseDevice((struct IORequest *)v_treq);
        v_topen = FALSE;
    }
    if (v_treq)
        DeleteIORequest((struct IORequest *)v_treq);
    if (v_tport)
        DeleteMsgPort(v_tport);
    v_treq = NULL;
    v_tport = NULL;
}

static void video_run(void)
{
    struct ZzBuf in, pcm;
    struct ZzMain r, held;
    struct ZzAudio au;
    ULONG session = 0, pend_off = 0, pend_len = 0, acc = 0, rate = 0, chans = 0, div = 1;
    ULONG f0 = st.f_out, t0 = 0, held_at = 0, run = 0, skip = 0, period = 3600, hold, drop, starved_at = 0;
    ULONG pcm_peak = 0, pcm_frames = 0;    /* (the log: what its sound held) */
    ZzU64 ack = 0, origin = ZZ_NO_PTS, pts0 = 0;
    BOOL held_now = FALSE, in_eof = FALSE, eof_sent = FALSE, done = FALSE, on = FALSE;
    BOOL out_paused = FALSE, sound = st.v_mp2, said_audio = FALSE, sys_seen = FALSE;
    BOOL csound = FALSE;                    /* its AC3 decoded by the CPU (the card given none) */
    int s;
    char m[200];
    memset(&in, 0, sizeof(in));
    memset(&pcm, 0, sizeof(pcm));
    memset(&held, 0, sizeof(held));
    memset(&au, 0, sizeof(au));
    st.v_shown = st.v_dropped = 0;
    es_bytes = 0;
    es_ended = FALSE;
    if (st.v_ts)
        tsp_begin(st.v_ts, st.v_vpid, st.v_apid, st.v_ac3, st.v_mpeg2);
    if ((v_tport = CreateMsgPort()) &&
        (v_treq = (struct timerequest *)CreateIORequest(v_tport, sizeof(struct timerequest))) &&
        !OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)v_treq, 0))
        v_topen = TRUE;
    if (!zz_open()) {
        fail("zz9k.library 2 is needed to play video");
        goto out;
    }
    if ((s = zz_alloc(V_IN_BYTES, &in)) != ZZ_OK) {
        sprintf(m, "No room on the ZZ9000 for the video (%s)", zz_name(s));
        fail(m);
        goto out;
    }
    if (sound && st.v_ac3 && st.v_ac3cpu) {
        /* the card would not decode its AC3: the CPU does, the card none */
        csound = a3_open(m);
        amce_log("video", m);
        if (!csound)
            a3_close();
        sound = FALSE;
    }
    if (sound && zz_alloc(V_PCM_BYTES, &pcm) != ZZ_OK && zz_alloc(V_PCM_BYTES / 2, &pcm) != ZZ_OK) {
        amce_log("video", "no room on the card for its sound: played without it");
        sound = FALSE;
    }
    {
        int k;                              /* (a session closing just before: busy a while) */
        for (k = 0; k < 25; k++) {
            if (k)
                Delay(2);
            if ((s = zz_begin(st.v_w, st.v_h, st.v_mpeg2,
                              !sound ? ZZ_AUDIO_NONE : st.v_ac3 ? ZZ_AUDIO_AC3 : ZZ_AUDIO_MP2,
                              sound ? &pcm : NULL, &r)) != ZZ_BUSY)
                break;
        }
    }
    if (s != ZZ_OK) {
        sprintf(m, "The ZZ9000 would not play it (%s)", zz_name(s));
        fail(m);
        goto out;
    }
    session = r.session;
    sprintf(m, "%s: %ld x %ld %s on the ZZ9000, %s%s; session %lu", log_name(), (long)st.v_w,
            (long)st.v_h, st.v_mpeg2 ? "MPEG-2" : "MPEG-1",
            csound ? "its sound (AC3) by the CPU"
            : !sound ? "no sound" : st.v_ac3 ? "its sound (AC3) to the card's ring"
                                            : "its sound to the card's ring",
            st.v_es ? ", a bare video stream packed as it goes"
                    : st.v_ts ? ", a transport stream packed as it goes" : "",
            (unsigned long)session);
    amce_log("video", m);
    st.state = ST_BUFFERING;
    st.v_ms = 0;
    if (st.range_from && !v_seek(&in, &pend_len))
        goto out;
    if (st.range_from && !v_from_back)
        skip = ZZ_SKIP_TO_I_PICTURE;        /* (a seek: from an I picture, not blocks) */
    hold = period / 2;
    drop = period > 3600 ? period : 3600;
    while (!st.quit) {
        BOOL busy = FALSE;
        serve();
        /* paused: nothing shown, the output stopped (its FIFO kept) */
        if (st.paused) {
            if (on && st.started && !out_paused) {
                out_stop();
                out_paused = TRUE;
            }
            v_wait(20000);
            continue;
        }
        if (out_paused) {
            out_start();
            out_paused = FALSE;
        }
        /* the file to the card, while it takes it */
        {
            int k;
            for (k = 0; k < 4 && !eof_sent; k++) {
                if (!pend_len && !in_eof && csound && a3.qin - a3.qout > A3Q * 3 / 4)
                    break;                  /* (its AC3 well ahead: the card waits for it) */
                if (!pend_len && !in_eof) {
                    LONG n = st.v_dvd  ? dvdplay_read(st.v_dvd, in.data, (LONG)in.length)
                             : st.v_ts ? tsp_pack(ts_read, NULL, (UBYTE *)in.data, in.length)
                             : st.v_es ? (LONG)es_pack(in.data, in.length)
                                     : Read(st.fh, (APTR)in.data, in.length);
                    if (st.v_es && st.state == ST_FAILED)
                        goto out;
                    if (n < 0) {
                        fail(_("The file could not be read"));
                        goto out;
                    }
                    if (n == 0)
                        in_eof = TRUE;
                    if (n > 0 && !st.v_es && !st.v_ts && !sys_seen)
                        sys_seen = sys_fix(in.data, (ULONG)n);
                    if (n > 0 && csound)
                        a3_tap(in.data, (ULONG)n);
                    if (n > 0)
                        st.v_bytes += (ULONG)n;
                    pend_off = 0;
                    pend_len = (ULONG)n;
                }
                if (pend_len) {
                    ULONG took;
                    s = zz_write(session, &in, pend_off, pend_len, FALSE, &r);
                    if (s == ZZ_BUSY)
                        break;
                    if (s != ZZ_OK) {
                        sprintf(m, "The ZZ9000 took no more of it (%s)", zz_name(s));
                        fail(m);
                        goto out;
                    }
                    took = r.accepted >= acc ? r.accepted - acc : r.written;
                    acc = r.accepted;
                    if (took > pend_len)
                        took = pend_len;
                    pend_off += took;
                    pend_len -= took;
                    busy = TRUE;
                    if (!took)
                        break;              /* (full: on later) */
                } else if (in_eof) {
                    s = zz_write(session, &in, 0, 0, TRUE, &r);
                    if (s == ZZ_OK)
                        eof_sent = TRUE;
                    else if (s != ZZ_BUSY) {
                        sprintf(m, "the end of the file not taken (%s)", zz_name(s));
                        amce_log("video", m);
                        eof_sent = TRUE;
                    }
                    break;
                }
            }
        }
        /* its sound, from the card's ring into the FIFO */
        if (sound) {
            s = zz_audio_read(session, ack, &au);
            if (s != ZZ_OK) {
                sprintf(m, "its sound could not be read (%s): on without it", zz_name(s));
                amce_log("video", m);
                sound = FALSE;
            } else {
                if (au.rate && !rate) {
                    if (au.format != 2 || (au.channels != 1 && au.channels != 2)) {
                        amce_log("video", "its sound is in a form AmigaMCE does not play: on without it");
                        sound = FALSE;
                    } else {
                        rate = au.rate;
                        chans = au.channels;
                        div = 1;
                        while ((!st.ahi && rate / div > 28867) || rate / div > 48000)
                            div *= 2;       /* Paula's limit: 44.1 kHz at 22050 */
                        st.freq = (LONG)rate;
                        st.channels = (LONG)chans;
                        st.dec_freq = (LONG)(rate / div);
                        st.dec_channels = (LONG)chans;
                        st.v_adec = VSD_CARD;
                        st.v_akbps = st.v_ac3 ? 0 : st.v_probe_kbps;
                        if (!st.v_ac3)
                            st.v_achans = (UBYTE)chans;
                        sprintf(m, "its sound: MP2 %lu Hz %s, played at %lu Hz through %s",
                                (unsigned long)rate, chans == 2 ? "stereo" : "mono",
                                (unsigned long)(rate / div), st.ahi ? "AHI" : "Paula");
                        amce_log("video", m);
                    }
                }
                if (sound && rate && au.produced > ack) {
                    ULONG fb = chans * 2, cap = pcm.length, room, want, i;
                    ZzU64 avail = au.produced - ack;
                    room = FIFO_N - (st.f_in - st.f_out);
                    room = room > 64 ? room - 64 : 0;
                    want = (ULONG)(avail / fb);
                    want -= want % div;
                    if (want / div > room)
                        want = room * div;
                    if (want) {
                        ULONG o = st.f_in, at = (ULONG)(ack % cap);
                        LONG al = 0, ar = 0;
                        for (i = 0; i < want; i++) {
                            /* (S16BE: the 68k's own order, a word a read across the bus) */
                            const volatile WORD *p = (const volatile WORD *)(pcm.data + at);
                            WORD l = p[0];
                            WORD rr = chans == 2 ? p[1] : l;
                            ULONG al_ = (ULONG)(l < 0 ? -l : l);
                            if (al_ > pcm_peak)
                                pcm_peak = al_;
                            at += fb;
                            if (at >= cap)
                                at -= cap;
                            al += l;
                            ar += rr;
                            if ((i + 1) % div)
                                continue;
                            st.fl[o & (FIFO_N - 1)] = (WORD)(al / (LONG)div);
                            st.fr[o & (FIFO_N - 1)] = (WORD)(ar / (LONG)div);
                            o++;
                            al = ar = 0;
                        }
                        st.f_in = o;
                        ack += (ZzU64)want * fb;
                        pcm_frames += want;
                        busy = TRUE;
                    }
                }
                if (sound && origin == ZZ_NO_PTS && au.produced)
                    zz_audio_origin(session, &origin);
                if (sound && !said_audio && origin != ZZ_NO_PTS) {
                    said_audio = TRUE;
                    sprintf(m, "its sound begins at %lu ms", (unsigned long)(origin / 90));
                    amce_log("video", m);
                }
            }
        }
        /* its AC3, decoded here into the FIFO (a second ahead): before the
         * first picture as fast as it comes, then a frame or two a turn,
         * so the pictures are shown on time between them (a dozen at once
         * made the first second's late: they were let go, a stutter);
         * more while the sound runs low */
        if (csound) {
            struct Ac3Info ai;
            int k, most = !on ? 12 : rate && st.f_in - st.f_out < (ULONG)st.dec_freq / 4 ? 6 : 2;
            for (k = 0; k < most; k++) {
                ULONG room = FIFO_N - (st.f_in - st.f_out), i, o;
                LONG got;
                if (rate && (st.f_in - st.f_out >= (ULONG)st.dec_freq ||
                             room < AC3_FRAME_SAMPLES / div + 64))
                    break;
                got = a3_frame(&ai);
                if (!got)
                    break;
                if (got < 0)
                    continue;
                if (!rate) {
                    rate = ai.ai_Rate;
                    chans = 2;
                    div = 1;
                    while ((!st.ahi && rate / div > 28867) || rate / div > 48000)
                        div *= 2;
                    st.freq = (LONG)rate;
                    st.channels = 2;
                    st.dec_freq = (LONG)(rate / div);
                    st.dec_channels = 2;
                    origin = a3.pts0;
                    st.v_adec = VSD_CPU_AC3;
                    st.v_akbps = ai.ai_BitRate / 1000;
                    st.v_achans = (UBYTE)ai.ai_Channels;
                    st.v_alfe = (UBYTE)ai.ai_LFE;
                    sprintf(m, "its sound: AC3 %lu Hz, %lu kbit/s, %lu.%lu channels, mixed to stereo, "
                               "played at %lu Hz through %s; it begins at %lu ms",
                            (unsigned long)rate, (unsigned long)(ai.ai_BitRate / 1000),
                            (unsigned long)ai.ai_Channels, (unsigned long)ai.ai_LFE,
                            (unsigned long)(rate / div), st.ahi ? "AHI" : "Paula",
                            (unsigned long)(origin / 90));
                    amce_log("video", m);
                    said_audio = TRUE;
                }
                o = st.f_in;
                for (i = 0; i + div <= (ULONG)got; i += div) {
                    LONG al = 0, ar = 0;
                    ULONG j;
                    for (j = 0; j < div; j++) {
                        WORD l = a3.pcm[(i + j) * 2];
                        ULONG al_ = (ULONG)(l < 0 ? -l : l);
                        if (al_ > pcm_peak)
                            pcm_peak = al_;
                        al += l;
                        ar += a3.pcm[(i + j) * 2 + 1];
                    }
                    st.fl[o & (FIFO_N - 1)] = (WORD)(al / (LONG)div);
                    st.fr[o & (FIFO_N - 1)] = (WORD)(ar / (LONG)div);
                    o++;
                }
                st.f_in = o;
                pcm_frames += (ULONG)got;
                busy = TRUE;
            }
        }
        /* the next picture */
        if (!held_now && !done) {
            s = zz_cmd(ZZ_DECODE, session, skip, &r);
            if (s == ZZ_BUSY) {
                v_wait(2000);
                continue;
            }
            if (s != ZZ_OK) {
                sprintf(m, "The ZZ9000 could not decode it (%s)", zz_name(s));
                fail(m);
                goto out;
            }
            skip = 0;
            if (r.flags & ZZ_R_FRAME_HELD) {
                held = r;
                held_now = TRUE;
                held_at = time_ms();
                busy = TRUE;
                if (r.rate_num && r.rate_den) {
                    period = (ULONG)((90000ULL * r.rate_den + r.rate_num / 2) / r.rate_num);
                    hold = period / 2;
                    drop = period > 3600 ? period : 3600;
                }
            } else if (r.flags & ZZ_R_DONE) {
                done = TRUE;
            } else if (eof_sent && (r.flags & ZZ_R_NEED_INPUT)) {
                /* all of it given, more wanted: the end, if it says nothing more */
                if (!starved_at)
                    starved_at = time_ms() | 1;
                else if (time_ms() - starved_at > 3000) {
                    amce_log("video", "the card wanted more after the end: taken as the end");
                    done = TRUE;
                }
            }
        }
        /* started: the first picture, the sound with it */
        if (!on && held_now) {
            ULONG pre = (ULONG)st.dec_freq * 300 / 1000;
            BOOL ready = !(sound || csound) || (rate && st.f_in - st.f_out >= pre) || eof_sent ||
                         time_ms() - held_at > 500 || (au.flags & ZZ_R_BACKPRESSURE);
            if (!ready) {
                if (!busy)
                    v_wait(5000);
                continue;
            }
            if ((sound || csound) && rate && st.f_in != st.f_out)
                out_start();                /* (none yet: started when it comes) */
            t0 = time_ms();
            pts0 = held.pts;
            if (origin == ZZ_NO_PTS)
                origin = held.pts;
            on = TRUE;
            st.state = ST_PLAYING;
            sprintf(m, "playing: the first picture at %lu ms, %s; volume %ld", (unsigned long)(held.pts / 90),
                    st.started ? "timed by its sound" : sound || csound ? "by the clock till its sound comes"
                                                          : "timed by the clock", (long)st.master);
            amce_log("video", m);
        }
        if (on && (sound || csound) && rate && !st.started && !out_paused &&
            st.f_in - st.f_out >= (ULONG)st.dec_freq / 5) {
            out_start();                    /* its sound, come late: on from here */
            sprintf(m, "its sound started %lu ms after the first picture",
                    (unsigned long)(time_ms() - t0));
            amce_log("video", m);
        }
        /* the held picture: shown on time, let go if late */
        if (on && held_now) {
            ZzU64 master = (sound || csound) && st.started ? v_clock(origin, f0, rate, div)
                                               : pts0 + (ZzU64)(time_ms() - t0) * 90;
            long long drift = (long long)(held.pts - master);
            int op = st.v_hidden ? ZZ_DISCARD : ZZ_PRESENT;   /* (unseen: on time, let go) */
            st.v_drift = (LONG)(drift / 90);
            if (drift > (long long)hold) {
                BOOL starved = (sound || csound) && st.f_in - st.f_out < (ULONG)st.dec_freq / 10;
                if (!starved) {
                    ULONG us = (ULONG)((drift - (long long)hold) * 1000 / 90);
                    v_wait(us > 10000 ? 10000 : us < 1000 ? 1000 : us);
                    continue;               /* early: soon */
                }
            } else if (drift < -(long long)drop && run < 2) {
                op = ZZ_DISCARD;            /* late: let go */
                skip = drift < -45000 ? ZZ_SKIP_TO_I_PICTURE : ZZ_SKIP_TO_KEYFRAME;
            }
            s = zz_cmd(op, session, 0, &r);
            if (s == ZZ_BUSY) {
                v_wait(2000);
                continue;
            }
            if (s != ZZ_OK) {
                sprintf(m, "The ZZ9000 could not show it (%s)", zz_name(s));
                fail(m);
                goto out;
            }
            if (op == ZZ_DISCARD && !st.v_hidden) {
                st.v_dropped++;
                run++;
            } else {
                st.v_shown++;
                run = 0;
            }
            if (held.pts >= pts0)
                st.v_ms = (ULONG)((held.pts - pts0) / 90);
            held_now = FALSE;
            busy = TRUE;
        }
        if (done && !held_now)
            break;
        if (!busy)
            v_wait(3000);
    }
    /* the end: its sound plays out */
    while (!st.quit && st.started && st.f_in != st.f_out) {
        ULONG got = st_wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C)
            break;
        out_take();
    }
    sprintf(m, "%s: %lu pictures shown, %lu let go (late); %lu dropouts; its sound: %lu frames "
               "from %s, peak %lu of 32767", log_name(),
            (unsigned long)st.v_shown, (unsigned long)st.v_dropped, (unsigned long)st.underruns,
            (unsigned long)pcm_frames, csound ? "the CPU's AC3" : "the card", (unsigned long)pcm_peak);
    amce_log("video", m);
    if (csound) {
        sprintf(m, "its AC3 by the CPU: %lu frames decoded, %lu of them bad, %lu bytes lost (too far "
                   "ahead), %lu left",
                (unsigned long)a3.frames, (unsigned long)a3.bad, (unsigned long)a3.lost,
                (unsigned long)(a3.qin - a3.qout));
        amce_log("video", m);
    }
    if (st.v_ts) {
        ULONG rs, cut, pre;
        tsp_counts(&rs, &cut, &pre);
        sprintf(m, "its transport stream: %lu times out of step, %lu PES packets not read, "
                   "%lu bytes of video before its sound's first left out",
                (unsigned long)rs, (unsigned long)cut, (unsigned long)pre);
        amce_log("video", m);
    }
    if (st.v_mp2 && !csound) {
        sprintf(m, "the card's sound at the end: state %lu, flags %08lx, %lu Hz, %lu channels, "
                   "format %lu, %lu bytes made, %lu taken",
                (unsigned long)au.state, (unsigned long)au.flags, (unsigned long)au.rate,
                (unsigned long)au.channels, (unsigned long)au.format, (unsigned long)au.produced,
                (unsigned long)au.acknowledged);
        amce_log("video", m);
    }
out:
    if (session) {
        /* its held picture let go, then closed: busy until the card has
         * handed its last picture to the overlay (a tick between tries);
         * left open, the next video's Begin is refused (busy) */
        int k;
        if (held_now)
            zz_cmd(ZZ_DISCARD, session, 0, &r);
        s = ZZ_BUSY;
        for (k = 0; k < 25 && s == ZZ_BUSY; k++) {
            if (k)
                Delay(1);
            s = zz_cmd(ZZ_CLOSE, session, 0, &r);
        }
        if (s != ZZ_OK) {
            sprintf(m, "its session %lu could not be closed (%s)", (unsigned long)session,
                    zz_name(s));
            amce_log("video", m);
        }
    }
    if (csound)
        a3_close();
    zz_free(&pcm);
    zz_free(&in);
    zz_close();
    if (v_topen)
        CloseDevice((struct IORequest *)v_treq);
    v_topen = FALSE;
    if (v_treq)
        DeleteIORequest((struct IORequest *)v_treq);
    if (v_tport)
        DeleteMsgPort(v_tport);
    v_treq = NULL;
    v_tport = NULL;
}

/* ---------------------------------------------------------- MHI
 * MP3 given as it comes to an MHI driver (Settings > Sound), which
 * decodes and plays it itself (in hardware, or the emulator's): buffers
 * queued, taken back when played (its signal), queued again. They are
 * taken back, and the driver started again if it ran dry, while the
 * process waits for the network too (serve, mhi_serve): it waited in a
 * read of a whole buffer with MHI stopped (a second or so at 128 kbit/s),
 * which was heard as choppy sound on a station whose data came in fits.
 * Small buffers, many: one comes back, and is filled again, sooner. */
#define MHI_BUFS  16
#define MHI_BUFSZ 8192

/* The MHI decoder and its buffers (the playback process's, while it plays). */
static struct {
    APTR h;
    UBYTE *buf[MHI_BUFS];
    LONG len[MHI_BUFS];
    BOOL queued[MHI_BUFS], playing, paused;
    int nq;
    ULONG played_bytes;
} mhs;

/* An MP3 frame's bitrate and channels, from the first one in the buffer. */
static void mp3_first_frame(LONG *kbps, LONG *ch)
{
    static const UWORD br[2][16] = {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } };
    LONG i, lim = st.nb_len - st.nb_pos;
    *kbps = *ch = 0;
    for (i = 0; i + 3 < lim; i++) {
        const UBYTE *b = st.nb + st.nb_pos + i;
        UBYTE ver = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3, bri = b[2] >> 4, sri = (b[2] >> 2) & 3;
        if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0 && ver != 1 && layer == 1 && bri != 15 &&
            bri && sri != 3) {
            *kbps = br[ver == 3 ? 0 : 1][bri];
            *ch = (b[3] >> 6) == 3 ? 1 : 2;
            return;
        }
    }
}

/* MHI's meters: no samples come back from an MHI driver, so the levels
 * are read off the MP3 itself as it is queued: each Layer III granule's
 * global gain (how loud it is, near enough), kept by frame, and shown
 * when that frame plays by the clock since playing began. */
#define MH_RING 4096
static struct {
    UBYTE l[MH_RING], r[MH_RING];
    volatile ULONG frames;          /* frames read */
    UBYTE hdr[48];
    int hlen;
    LONG skip;                      /* the rest of the frame (its main data) */
    ULONG spf, rate;
    volatile ULONG t0, paused_ms, pause_at;     /* t0: playing since (0 not yet) */
    volatile ULONG bytes_in;        /* MP3 queued, all told */
    ULONG gtop;                     /* the loudest gain lately, x 256, falling slowly */
} mh;

static ULONG mh_bits(const UBYTE *b, ULONG *pos, int n)
{
    ULONG v = 0;
    while (n--) {
        v = v << 1 | ((b[*pos >> 3] >> (7 - (*pos & 7))) & 1);
        (*pos)++;
    }
    return v;
}

/* A granule's gain as a meter's level (0-128): 0 for silence. Each 6 dB
 * down is about 4 less (measured, LAME); where full scale lies depends on
 * the encoder and its bitrate, so it is the loudest gain lately (falling a
 * step in some two seconds): that near the top, 40 steps (some 60 dB)
 * below it at the bottom. It starts at, and never falls below, a loud
 * track's (pink noise at -6 dB is some 167, at -40 dB 140, LAME 160kbps):
 * a quiet lead-in shows as quiet, not as the loudest yet. */
#define MH_GTOP_START 168
#define MH_GTOP_FLOOR 160
static UBYTE mh_level(ULONG gain, ULONG big)
{
    LONG v;
    if (!big)
        return 0;
    if (gain * 256 > mh.gtop)
        mh.gtop += (gain * 256 - mh.gtop) / 16;     /* risen to over some frames: not
                                                     * by one odd one (the encoder's first) */
    v = 120 + ((LONG)gain - (LONG)(mh.gtop >> 8)) * 128 / 40;
    return (UBYTE)(v < 0 ? 0 : v > 128 ? 128 : v);
}

static void mh_scan(const UBYTE *b, LONG n)
{
    static const UWORD br[2][16] = {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } };
    static const UWORD sr[3] = { 44100, 48000, 32000 };
    LONG i = 0;
    while (i < n) {
        UBYTE *h = mh.hdr, ver, bri, sri, mono;
        LONG need, flen;
        ULONG rate, kbps, pos;
        UBYTE lv[2] = { 0, 0 };
        int gr, ch, ngr, nch;
        if (mh.skip > 0) {
            LONG k = mh.skip < n - i ? mh.skip : n - i;
            i += k;
            mh.skip -= k;
            continue;
        }
        h[mh.hlen++] = b[i++];
        if (mh.hlen < 4)
            continue;
        ver = (h[1] >> 3) & 3;
        bri = h[2] >> 4;
        sri = (h[2] >> 2) & 3;
        if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0 || ver == 1 || ((h[1] >> 1) & 3) != 1 ||
            !bri || bri == 15 || sri == 3) {
            memmove(h, h + 1, --mh.hlen);   /* not a frame's start: on a byte */
            continue;
        }
        mono = (h[3] >> 6) == 3;
        need = 4 + ((h[1] & 1) ? 0 : 2) + (ver == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17));
        if (mh.hlen < need)
            continue;
        rate = sr[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
        kbps = br[ver == 3 ? 0 : 1][bri];
        flen = (LONG)((ver == 3 ? 144000UL : 72000UL) * kbps / rate) + ((h[2] >> 1) & 1);
        /* the side information: each granule's and channel's gain */
        pos = (ULONG)(4 + ((h[1] & 1) ? 0 : 2)) * 8;
        nch = mono ? 1 : 2;
        ngr = ver == 3 ? 2 : 1;
        if (ver == 3)
            pos += 9 + (mono ? 5 : 3) + 4 * nch;
        else
            pos += 8 + (mono ? 1 : 2);
        for (gr = 0; gr < ngr; gr++)
            for (ch = 0; ch < nch; ch++) {
                ULONG p23 = mh_bits(h, &pos, 12), big = mh_bits(h, &pos, 9);
                ULONG gain = mh_bits(h, &pos, 8);
                UBYTE l = mh_level(gain, big | p23);
                pos += ver == 3 ? 30 : 34;
                if (l > lv[ch])
                    lv[ch] = l;
            }
        if ((h[3] >> 6) == 1 && (h[3] & 0x20)) {
            /* mid and side, not left and right: both meters the louder */
            if (lv[1] > lv[0])
                lv[0] = lv[1];
            lv[1] = lv[0];
        }
        if (mh.gtop > MH_GTOP_FLOOR * 256)
            mh.gtop -= 3;                   /* about a step each 2 s (38 frames a second) */
        mh.l[mh.frames & (MH_RING - 1)] = lv[0];
        mh.r[mh.frames & (MH_RING - 1)] = mono ? lv[0] : lv[1];
        mh.frames++;
        mh.rate = rate;
        mh.spf = ver == 3 ? 1152 : 576;
        mh.skip = flen > need ? flen - need : 0;
        mh.hlen = 0;
    }
}

/* How long the frames queued play (their count, not their bytes: the
 * first frame's bitrate, an Info frame's, can be far from the rest's). */
static ULONG mh_queued_ms(void)
{
    if (!mh.rate || !mh.spf)
        return 0;
    return (ULONG)((unsigned long long)mh.frames * mh.spf * 1000 / mh.rate);
}

/* How long MHI has played, by the clock (its pauses not counted). */
static ULONG mh_ms(void)
{
    if (!mh.t0)
        return 0;
    return (mh.pause_at ? mh.pause_at : time_ms()) - mh.t0 - mh.paused_ms;
}

/* The levels of the frame playing now (the UI's, for its meters). */
static void mh_peaks(UBYTE *l, UBYTE *r)
{
    ULONG now = time_ms(), ms, f, k;
    UBYTE a = 0, c = 0;
    if (!mh.t0 || !mh.frames || !mh.rate || !mh.spf) {
        *l = *r = 0;
        return;
    }
    ms = (mh.pause_at ? mh.pause_at : now) - mh.t0 - mh.paused_ms;
    f = (ms / 1000) * mh.rate / mh.spf + (ms % 1000) * mh.rate / (1000 * mh.spf);
    if (f >= mh.frames)
        f = mh.frames - 1;
    for (k = 0; k < 3 && k <= f; k++) {     /* the last few: a meter, not a flicker */
        UBYTE x = mh.l[(f - k) & (MH_RING - 1)], y = mh.r[(f - k) & (MH_RING - 1)];
        if (x > a)
            a = x;
        if (y > c)
            c = y;
    }
    *l = a;
    *r = c;
}

/* The buffers MHI has played, taken back; the driver started again if it
 * ran dry with some queued (a dropout). From mhi_play's loop and from the
 * network's waits (serve). */
static void mhi_serve(void)
{
    APTR e;
    int i;
    if (!mhs.h)
        return;
    while ((e = MHIGetEmpty(mhs.h)))
        for (i = 0; i < MHI_BUFS; i++)
            if (mhs.queued[i] && mhs.buf[i] == e) {
                mhs.queued[i] = FALSE;
                mhs.played_bytes += mhs.len[i];
                mhs.nq--;
            }
    if (mhs.playing && !mhs.paused && mhs.nq && MHIGetStatus(mhs.h) == MHIF_OUT_OF_DATA) {
        st.underruns++;
        MHIPlay(mhs.h);                     /* more came: on again */
    }
}

static void mhi_play(LONG freq)
{
    LONG kbps = 0, ch = 0;
    BOOL ended = FALSE;
    UWORD vol = 0xFFFF;
    ULONG low_ms = 0xFFFFFFFFUL;
    int i;
    memset(&mhs, 0, sizeof(mhs));
    if (!(MHIBase = OpenLibrary((CONST_STRPTR)mhi_driver, 0))) {
        fail(_("The MHI driver could not be opened (Settings > Sound)"));
        return;
    }
    amce_card_lock();                       /* (mhizz9000: the card's) */
    mhs.h = MHIAllocDecoder(FindTask(NULL), 1UL << st.sig);
    amce_card_unlock();
    if (!mhs.h) {
        mhs.h = NULL;
        fail(_("The MHI driver's decoder is in use, or not there"));
        goto done;
    }
    for (i = 0; i < MHI_BUFS; i++)
        if (!(mhs.buf[i] = AllocVec(MHI_BUFSZ, MEMF_PUBLIC))) {
            fail(_("Not enough memory for MHI's buffers"));
            goto done;
        }
    mp3_first_frame(&kbps, &ch);
    if (!st.bitrate)
        st.bitrate = kbps;
    st.freq = st.dec_freq = freq;
    st.channels = st.dec_channels = ch ? ch : 2;
    if (st.local && st.clen && st.bitrate > 0)
        st.len_ms = (st.clen / (ULONG)st.bitrate) * 8;
    {
        char m[200];
        sprintf(m, "%.60s: MP3 %ld kbit/s %ld Hz %s, decoded and played by %.60s",
                st.finite ? log_name() : st.name[0] ? st.name : "a station",
                (long)st.bitrate, (long)freq, ch == 1 ? "mono" : "stereo",
                (const char *)FilePart((STRPTR)mhi_driver));
        amce_log(st.finite ? "track" : "radio", m);
    }
    memset(&mh, 0, sizeof(mh));
    mh.gtop = MH_GTOP_START * 256;
    st.state = ST_BUFFERING;
    while (!st.quit) {
        UBYTE status;
        mhi_serve();                        /* the buffers played, back */
        titles_due();                       /* a title whose audio has come round */
        if (mhs.playing && !mhs.paused && mh_ms() > 5000) {
            /* the least held, for the log: how near it came to running dry */
            ULONG q = mh_queued_ms(), p = mh_ms();
            q = q > p ? q - p : 0;
            if (q < low_ms)
                low_ms = q;
        }
        if (st.bitrate > 0) {
            ULONG ms = mhs.played_bytes / ((ULONG)st.bitrate) * 8;  /* bytes / (kbit/s / 8) */
            st.played = (ms / 1000) * (ULONG)freq + (ms % 1000) * (ULONG)freq / 1000;
        }
        /* and filled again, from the stream */
        for (i = 0; i < MHI_BUFS && !ended && !st.quit; i++) {
            LONG n;
            if (mhs.queued[i])
                continue;
            n = stream_read(mhs.buf[i], MHI_BUFSZ);     /* (MHI served while it waits) */
            if (n <= 0) {
                ended = TRUE;
                break;
            }
            mhs.len[i] = n;
            mh_scan(mhs.buf[i], n);         /* its levels, for the meters */
            mh.bytes_in += n;
            if (mh.frames >= 40 && !st.icy_br) {
                /* its bitrate from what it is, not its first frame */
                ULONG ms = mh_queued_ms();
                if (ms >= 1000) {
                    st.bitrate = (LONG)((mh.bytes_in / ms) * 8 + (mh.bytes_in % ms) * 8 / ms);
                    if (st.local && st.clen && st.bitrate > 0)
                        st.len_ms = (st.clen / (ULONG)st.bitrate) * 8;
                }
            }
            MHIQueueBuffer(mhs.h, mhs.buf[i], n);
            mhs.queued[i] = TRUE;
            mhs.nq++;
        }
        if (ended && st.err[0] && !st.eof && !st.finite) {
            fail(st.err);
            break;
        }
        if (st.master != vol) {
            /* 90 of MHI's 100 at full volume (by ear: 50 was too quiet), near
             * mpega's level through Paula or AHI */
            vol = st.master;
            MHISetParam(mhs.h, MHIP_VOLUME, (ULONG)vol * 90 / 64);
        }
        if (!mhs.playing && mhs.nq && (mhs.nq == MHI_BUFS || ended)) {
            MHIPlay(mhs.h);
            mh.t0 = time_ms();
            mhs.playing = TRUE;
            st.started = TRUE;
            st.state = ST_PLAYING;
        }
        if (mhs.playing && st.paused != mhs.paused) {
            mhs.paused = st.paused;
            if (mhs.paused) {
                MHIPause(mhs.h);
                mh.pause_at = time_ms();
            } else {
                MHIPlay(mhs.h);
                if (mh.pause_at)
                    mh.paused_ms += time_ms() - mh.pause_at;
                mh.pause_at = 0;
            }
        }
        status = MHIGetStatus(mhs.h);
        if (mhs.playing && status == MHIF_OUT_OF_DATA && ended && !mhs.nq)
            break;                          /* all of it played */
        if (ended && !mhs.nq)
            break;
        if (mhs.nq == MHI_BUFS || ended) {
            /* all queued: till one is played (its signal), looked at a
             * tenth of a second on in any case (pause, volume, its status) */
            ULONG got = SetSignal(0, (1UL << st.sig) | SIGBREAKF_CTRL_C);
            if (got & SIGBREAKF_CTRL_C)
                break;
            if (!(got & (1UL << st.sig)))
                st_delay(5);
        }
    }
done:
    if (mhs.playing) {
        char m[160];
        sprintf(m, "%.60s: %lu s played, %lu dropout%s, at least %lu.%lu s held",
                st.finite ? log_name() : st.name[0] ? st.name : "a station",
                (unsigned long)(mh_ms() / 1000), (unsigned long)st.underruns,
                st.underruns == 1 ? "" : "s", (unsigned long)(low_ms == 0xFFFFFFFFUL ? 0 : low_ms / 1000),
                (unsigned long)(low_ms == 0xFFFFFFFFUL ? 0 : low_ms % 1000 / 100));
        amce_log(st.finite ? "track" : "radio", m);
    }
    if (mhs.h) {
        APTR hh = mhs.h;
        mhs.h = NULL;                           /* serve no more */
        amce_card_lock();
        MHIStop(hh);
        MHIFreeDecoder(hh);
        amce_card_unlock();
    }
    st.started = FALSE;
    for (i = 0; i < MHI_BUFS; i++)
        if (mhs.buf[i])
            FreeVec(mhs.buf[i]);
    CloseLibrary(MHIBase);
    MHIBase = NULL;
}

static void playback_main(void)
{
    MPEGA_STREAM *ms = NULL;
    static struct Hook hook;
    MPEGA_CTRL ctrl;
    BYTE s = AllocSignal(-1);
    st.sig = s;
    st.sock = -1;
    if (s < 0) {
        fail(_("No signal free"));
        goto out;
    }
    if (!st.local && !(st_SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        fail("No TCP/IP stack is running (bsdsocket.library)");
        goto out;
    }
    if (!open_stream())
        goto out;
    if (st.video) {
        if (st.v_sw)
            swv_run();                      /* MPEG-1 or MPEG-2 by the CPU */
        else
            video_run();                    /* MPEG on the ZZ9000 */
        goto out;
    }
    if (st.finite && !skip_id3()) {
        fail(st.err[0] ? st.err : _("The track ended before its music began"));
        goto out;
    }
    if (st.finite && is_flac()) {
        flac_play();                        /* a FLAC file, as stored */
        goto out;
    }
    if (st.local && is_pcm()) {
        pcm_play();                         /* WAV, AIFF, 8SVX: as stored */
        goto out;
    }
    if (mhi_driver[0]) {
        /* MP3 by the MHI driver: from the first frame (or the time asked) */
        LONG f;
        st.mhi = TRUE;
        if (st.skipping && !skip_to()) {
            fail(st.err[0] ? st.err : _("The track ended before that time"));
            goto out;
        }
        if (st.range_from && !st.metaint && !mp3_resync()) {
            fail(st.err[0] ? st.err : _("The stream does not look like MP3"));
            goto out;
        }
        if (!(f = first_frame_freq())) {
            fail(st.err[0] ? st.err : _("The stream does not look like MP3"));
            goto out;
        }
        mhi_play(f);
        goto out;
    }
    if (!(MPEGABase = OpenLibrary((CONST_STRPTR)"mpega.library", 2))) {
        fail("mpega.library is needed to play MP3");
        goto out;
    }
    if (st.skipping && !skip_to()) {
        fail(st.err[0] ? st.err : _("The track ended before that time"));
        goto out;
    }
    if (st.range_from && !st.metaint && !mp3_resync()) {
        fail(st.err[0] ? st.err : _("The stream does not look like MP3"));
        goto out;
    }
    st.state = ST_BUFFERING;
    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (HOOKFUNC)(APTR)hook_func;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bs_access = &hook;
    {
        /* what the CPU can decode in time, and never more than Paula plays
         * (with AHI, a 68060 class Amiga: the whole rate). The ZZ9000
         * SDK's mpega.library decodes on the card, whatever the CPU: the
         * best it offers, stereo, the whole rate on AHI. */
        /* quality: 2, mpega's best, where the CPU is fast (a 68060 class
         * Amiga, or one measured so): its medium (1) reaches full scale on
         * loud passes (Plex's conversions), and clips */
        WORD q = card_mpega || st.cpu >= 60 ? 2 : st.cpu >= 40 ? 1 : 0, mono = !card_mpega && st.cpu < 40,
             div = 1;
        LONG fmax = card_mpega ? (st.ahi ? 48000 : 28000)
                  : st.cpu >= 60 && st.ahi ? 48000 : st.cpu >= 40 ? 28000 : st.cpu >= 30 ? 24000 : 12000;
        LONG f = first_frame_freq();
        MPEGA_LAYER *ls[2];
        int k;
        if (!f) {
            fail(st.err[0] ? st.err : _("The stream does not look like MP3"));
            goto out;
        }
        while (div < 4 && f / div > fmax)
            div *= 2;                       /* 44.1 kHz: 22050 on a 68030 */
        ls[0] = &ctrl.layer_1_2;
        ls[1] = &ctrl.layer_3;
        for (k = 0; k < 2; k++) {
            ls[k]->force_mono = mono;
            ls[k]->mono.freq_div = div;
            ls[k]->mono.quality = q;
            ls[k]->mono.freq_max = fmax;
            ls[k]->stereo = ls[k]->mono;
        }
    }
    ctrl.check_mpeg = 0;
    ctrl.stream_buffer_size = 0;
    if (!(ms = MPEGA_open((char *)"AmigaMCE playback", &ctrl))) {
        fail(st.err[0] ? st.err : "The stream is not MP3 that mpega.library can decode");
        goto out;
    }
    st.freq = ms->frequency;
    st.channels = ms->channels;
    st.dec_freq = ms->dec_frequency;
    st.dec_channels = ms->dec_channels;
    if (!st.bitrate)
        st.bitrate = ms->bitrate;
    if (st.local && st.clen && st.bitrate > 0)
        st.len_ms = (st.clen / (ULONG)st.bitrate) * 8;  /* bytes / (kbit/s / 8) = ms */
    if (!st.ahi && (st.dec_freq > 28867 || st.dec_freq < 4000))
        st.dec_freq = st.dec_freq > 28867 ? st.dec_freq / 2 : 22050;  /* Paula's limit */
    {
        /* named by its station, else its URL without the query: a Plex
         * track's carries the sign-in token, which stays out of the log */
        char m[360], u[200], *q;
        amce_strlcpy(u, st.url, sizeof(u));
        if ((q = strchr(u, '?')))
            *q = 0;
        sprintf(m, "%s: MP3 %ld kbit/s %ld Hz %s, decoded at %ld Hz %s (quality %ld)%s",
                st.name[0] && !st.finite ? st.name : u, (long)st.bitrate, (long)st.freq,
                st.channels == 2 ? "stereo" : "mono", (long)st.dec_freq,
                st.dec_channels == 2 ? "stereo" : "mono", (long)ms->dec_quality,
                card_mpega ? " on the ZZ9000" : "");
        amce_log(st.finite ? "track" : "radio", m);
    }
    dec_full = dec_samples = 0;
    decode_loop(ms);
    if (dec_samples) {
        /* how it went: what was heard, and how much the decoder clipped
         * (full scale: a loud master, or mpega's own) */
        char m[200];
        ULONG per = dec_samples / 10000 ? dec_full / (dec_samples / 10000) : 0;
        sprintf(m, "%.60s: %lu s decoded, %lu dropout%s; %lu samples at full scale (%lu in 10000), "
                   "to %s",
                st.finite ? log_name() : st.name[0] ? st.name : "a station",
                (unsigned long)(dec_samples / (st.dec_freq ? st.dec_freq : 22050)),
                (unsigned long)st.underruns, st.underruns == 1 ? "" : "s",
                (unsigned long)dec_full, (unsigned long)per, st.ahi ? "AHI" : "Paula");
        amce_log(st.finite ? "track" : "radio", m);
    }
out:
    out_stop();
    if (ms)
        MPEGA_close(ms);
    sock_close();
    tls_close(&st_tls);                     /* before the socket library it uses */
    if (st.fh) {
        Close(st.fh);
        st.fh = 0;
    }
    if (MPEGABase)
        CloseLibrary(MPEGABase);
    MPEGABase = NULL;
    if (st_SocketBase)
        CloseLibrary(st_SocketBase);
    st_SocketBase = NULL;
    if (st.state != ST_FAILED)
        st.state = ST_ENDED;
    if (s >= 0)
        FreeSignal(s);
    st.sig = -1;
    Forbid();                               /* gone before st_stop goes on */
    st.proc = NULL;
}

/* ---------------------------------------------------------- the UI's */
/* Settings > Networking > Stream buffer: 0 short, 1 normal, 2 long (the
 * next stream started). */
void st_set_buffer(int level)
{
    buf_level = level < 0 ? 0 : level > 2 ? 2 : level;
}

BOOL st_start(const char *url, UWORD master, UWORD cpu, BOOL finite, ULONG range_from,
              ULONG skip_ms, char *err, LONG errmax)
{
    st_stop();
    st.finite = finite;
    st.range_from = range_from;
    st.skip_ms = skip_ms;
    st.local = finite && amce_strnicmp(url, "http://", 7) && amce_strnicmp(url, "https://", 8);
    st.fh = 0;
    st.len_ms = 0;
    st.skipping = FALSE;
    st.eof = FALSE;
    memset(&st.err, 0, sizeof(st.err));
    st.name[0] = st.genre[0] = st.title[0] = 0;
    st.title_seq++;
    tq_head = tq_tail = 0;                  /* (the playback process is stopped here) */
    st.bitrate = st.freq = st.channels = st.dec_freq = st.dec_channels = 0;
    st.f_in = st.f_out = st.underruns = st.played = 0;
    st.flac = FALSE;
    st.mp3_bytes = 0;
    st.peak_l = st.peak_r = 0;
    st.quit = st.started = st.paused = FALSE;
    st.master = master;
    st.sig = -1;
    st.ahi = use_ahi;
    st.icy_br = FALSE;
    st.mhi = FALSE;                         /* set when it is MP3, and MHI is chosen */
    st.video = vid_next.on;                 /* (st_start_video's) */
    st.v_mpeg2 = vid_next.mpeg2;
    st.v_mp2 = vid_next.mp2;
    st.v_es = vid_next.es;
    st.v_ac3 = vid_next.ac3;
    st.v_ac3cpu = vid_next.ac3cpu;
    st.v_probe_kbps = vid_next.akbps;
    st.v_bps = vid_next.bps;
    st.v_bytes = st.v_bskipped = st.v_akbps = 0;
    st.v_drift = 0;
    st.v_adec = VSD_NONE;
    st.v_achans = vid_next.achans;
    st.v_alfe = 0;
    st.v_sw = vid_next.sw;
    st.v_hz = vid_next.hz;
    st.v_ts = vid_next.ts;
    st.v_vpid = vid_next.vpid;
    st.v_apid = vid_next.apid;
    st.v_tsfirst = vid_next.tsfirst;
    st.v_w = vid_next.w;
    st.v_h = vid_next.h;
    st.v_dvd = vid_next.dvd;
    st.v_shown = st.v_dropped = 0;
    st.v_ms = 0;
    memset(&vid_next, 0, sizeof(vid_next));
    amce_strlcpy(st.url, url, sizeof(st.url));
    st.fifo_n = fifo_sizes[buf_level];
    st.nb = AllocVec(NETBUF + 1, MEMF_ANY);
    st.fl = AllocVec(FIFO_N * 2, MEMF_ANY | MEMF_CLEAR);
    st.fr = AllocVec(FIFO_N * 2, MEMF_ANY | MEMF_CLEAR);
    st.chip = AllocMem(HALF * 4, MEMF_CHIP | MEMF_CLEAR);
    if (!st.nb || !st.fl || !st.fr || !st.chip) {
        st_stop();
        amce_strlcpy(err, _("Not enough memory to play a station"), errmax);
        return FALSE;
    }
    st.state = ST_CONNECTING;
    st.cpu = cpu;
    st.proc = CreateNewProcTags(NP_Entry, (ULONG)playback_main, NP_Name, (ULONG)"AmigaMCE playback",
                                NP_Priority, 3, NP_StackSize, 65536, NP_WindowPtr, (ULONG)-1,
                                TAG_DONE);
    if (!st.proc) {
        st_stop();
        amce_strlcpy(err, _("Could not start the background playback process"), errmax);
        return FALSE;
    }
    return TRUE;
}

/* An MPEG file played by the ZZ9000 (v: what vprobe found): the radio
 * process runs it (video_run), its sound as music's. A Program Stream goes
 * to the card as it is; a bare video stream or a transport stream packed
 * as one as it goes. */
BOOL st_start_video(const char *path, UWORD master, UWORD cpu, const struct VProbe *v,
                    ULONG range_from, char *err, LONG errmax)
{
    BOOL ok;
    vid_next.on = TRUE;
    vid_next.ts = v->ts;
    vid_next.es = !v->ps && !v->ts;
    vid_next.mpeg2 = v->mpeg2;
    vid_next.ac3 = v->ac3 && (v->ps || (v->ts && v->ts_apid != 0x1FFF));
    vid_next.mp2 = vid_next.ac3 || (v->mp2 && (v->ps || (v->ts && v->ts_apid != 0x1FFF)));
    vid_next.vpid = v->ts_vpid;
    vid_next.apid = vid_next.mp2 ? v->ts_apid : 0x1FFF;
    vid_next.tsfirst = v->ts_first;
    vid_next.w = v->w;
    vid_next.h = v->h;
    vid_next.sw = v->sw;
    vid_next.hz = v->a_hz;
    vid_next.akbps = v->a_kbps;
    vid_next.achans = v->a_chans;
    vid_next.bps = v->ms ? (ULONG)((unsigned long long)v->bytes * 1000ULL / v->ms) : 0;
    vid_next.ac3cpu = vid_next.ac3 && ac3_by_cpu;
    ok = st_start(path, master, cpu, TRUE, range_from, 0, err, errmax);
    memset(&vid_next, 0, sizeof(vid_next));
    if (ok)
        st.len_ms = v->ms;
    return ok;
}

/* A DVD's film (dvdplay.c: opened, and where to start chosen already),
 * played as an MPEG file is: v from dvdplay_open. */
BOOL st_start_dvd(struct DvdPlay *dp, UWORD master, UWORD cpu, const struct VProbe *v, char *err,
                  LONG errmax)
{
    BOOL ok;
    vid_next.dvd = dp;
    vid_next.on = TRUE;
    vid_next.mpeg2 = TRUE;
    vid_next.ac3 = v->ac3;
    vid_next.mp2 = v->ac3 || v->mp2;
    vid_next.ac3cpu = v->ac3 && ac3_by_cpu;
    vid_next.sw = v->sw;                    /* (by the CPU: mpegv.library) */
    vid_next.hz = v->a_hz;
    vid_next.apid = 0x1FFF;
    vid_next.w = v->w;
    vid_next.h = v->h;
    ok = st_start(dvdplay_name(dp), master, cpu, TRUE, 0, 0, err, errmax);
    memset(&vid_next, 0, sizeof(vid_next));
    if (ok)
        st.len_ms = v->ms;
    return ok;
}

/* The overlay up (shown) or down: its pictures shown or let go unseen. */
void st_video_show(BOOL shown)
{
    st.v_hidden = !shown;
}

/* What a video's playing is like now, for its information (anim.c). */
void st_video_stats(struct StVStats *v)
{
    ULONG held = st.f_in - st.f_out;
    memset(v, 0, sizeof(*v));
    v->shown = st.v_shown;
    v->dropped = st.v_dropped;
    v->bskipped = st.v_bskipped;
    v->underruns = st.underruns;
    v->bytes = st.v_bytes;
    v->drift_ms = st.v_drift;
    v->a_dec = st.v_adec;
    v->a_kbps = st.v_akbps;
    v->a_chans = st.v_achans;
    v->a_lfe = st.v_alfe;
    v->a_hz = st.v_adec != VSD_NONE ? (ULONG)st.freq : 0;
    v->out_hz = st.v_adec != VSD_NONE ? (ULONG)st.dec_freq : 0;
    v->fifo_ms = st.dec_freq > 0 ? (ULONG)((unsigned long long)held * 1000 / (ULONG)st.dec_freq) : 0;
    v->ahi = st.ahi;
    v->started = st.started;
}

/* Pictures shown and let go (a video). */
void st_video_counts(ULONG *shown, ULONG *dropped)
{
    *shown = st.v_shown;
    *dropped = st.v_dropped;
}

void st_stop(void)
{
    if (st.proc) {
        st.quit = TRUE;
        Signal(&st.proc->pr_Task, SIGBREAKF_CTRL_C);
        while (st.proc)
            Delay(1);                       /* it stops within a fifth of a second */
    }
    if (st.started && !st.ahi)
        paula_stop();                       /* AHI's: the process's own, stopped there */
    if (st.chip) {
        Delay(1);                           /* Paula's last fetches from it done */
        FreeMem(st.chip, HALF * 4);
        st.chip = NULL;
    }
    if (st.nb) {
        FreeVec(st.nb);
        st.nb = NULL;
    }
    if (st.fl) {
        FreeVec(st.fl);
        st.fl = NULL;
    }
    if (st.fr) {
        FreeVec(st.fr);
        st.fr = NULL;
    }
}

/* Settings > Sound: AHI or Paula, for streams started from now on. */
void st_use_ahi(BOOL on)
{
    use_ahi = on;
}

/* What plays came over HTTPS (redirects followed: the connection it is
 * on, not the address asked for). */
BOOL st_secure(void)
{
    return secure_now;
}

BOOL st_ahi(void)
{
    return use_ahi;
}

/* Settings > Sound: MP3 decoded by this MHI driver (a library's path; ""
 * mpega.library), for streams started from now on. */
void st_use_mhi(const char *driver)
{
    amce_strlcpy(mhi_driver, driver ? driver : "", sizeof(mhi_driver));
}

/* Settings > Sound found mpega.library to be the ZZ9000 SDK's (caps.mpega_zz):
 * MP3 is decoded on the card, at the best quality, whatever the CPU. */
void st_use_card_mpega(BOOL on)
{
    card_mpega = on;
}

BOOL st_mhi(void)
{
    return mhi_driver[0] != 0;
}

BOOL st_mhi_now(void)
{
    return st.mhi;
}

/* The spectrum's: the last n samples played, mono, every dec'th (about
 * 8 kHz, as HippoPlayer's spectrum mixes modules), from the buffer Paula
 * or AHI plays now, where it has got to by the clock. Scaled as
 * HippoPlayer scales a sample player's bytes. 0: none to give. */
UWORD st_pcm(WORD *out, UWORD n, ULONG *rate)
{
    ULONG r = st.dec_freq ? (ULONG)st.dec_freq : 22050, dec = (r + 4000) / 8000, at, k, len;
    if (!st.started || st.mhi || st.paused || !st.chip)
        return 0;
    if (!dec)
        dec = 1;
    at = time_ms() - st.play_t;
    if (at > 1000)
        at = 1000;
    at = at * r / 1000;
    len = st.ahi ? AHI_FRAMES : HALF;
    if (at > len)
        at = len;
    if (at < (ULONG)n * dec)
        at = (ULONG)n * dec;
    if (st.ahi) {
        const WORD *b = ah.buf[st.ahi_play & 1];
        if (!b)
            return 0;
        for (k = 0; k < n; k++) {
            ULONG i = at - (n - k) * dec;
            out[k] = (WORD)(((LONG)b[2 * i] + b[2 * i + 1]) >> 4);
        }
    } else {
        const BYTE *l = st.chip + st.cur * HALF, *rr = st.chip + (2 + st.cur) * HALF;
        for (k = 0; k < n; k++) {
            ULONG i = at - (n - k) * dec;
            out[k] = (WORD)(((WORD)l[i] + rr[i]) << 4);
        }
    }
    *rate = r / dec;
    return n;
}

void st_pause(BOOL on)
{
    st.paused = on;
    if (st.started && !st.ahi) {
        CU->aud[0].ac_vol = on ? 0 : st.master;
        CU->aud[1].ac_vol = on ? 0 : st.master;
    }
}

void st_volume(int v)
{
    st.master = (UWORD)v;                   /* AHI: its next requests */
    if (st.started && !st.paused && !st.ahi) {
        CU->aud[0].ac_vol = st.master;
        CU->aud[1].ac_vol = st.master;
    }
}

/* A track's length as its stream says (FLAC's frames, a file's size at
 * its bitrate), 0 not known; its size in bytes. */
ULONG st_length_ms(void) { return st.len_ms; }
ULONG st_size(void) { return st.clen; }

/* Channels decoded (1 mono, 2 stereo), 0 not known yet. */
int st_channels(void) { return st.dec_channels; }

int st_state(void) { return st.proc || st.state == ST_FAILED ? st.state : ST_ENDED; }
const char *st_error(void) { return st.err; }
const char *st_title(void) { return st.title; }
ULONG st_title_seq(void) { return st.title_seq; }

/* The average bitrate (a VBR file's first frame, often all there is in
 * its header, says little): bits a ms of what is decoded; 0 before 3 s. */
ULONG st_avg_kbps(void)
{
    ULONG rate = st.dec_freq ? (ULONG)st.dec_freq : 1, f = st.f_in, b = st.mp3_bytes;
    ULONG ms = (f / rate) * 1000 + (f % rate) * 1000 / rate;
    return ms >= 3000 ? (b / ms) * 8 + ((b % ms) * 8) / ms : 0;
}

void st_info(struct StInfo *i)
{
    ULONG rate = st.dec_freq ? (ULONG)st.dec_freq : 1;
    ULONG have = st.f_in - st.f_out;
    i->state = st_state();
    i->name = st.name;
    i->genre = st.genre;
    i->bitrate = st.bitrate;
    i->freq = st.freq;
    i->channels = st.channels;
    i->dec_freq = st.dec_freq;
    i->dec_channels = st.dec_channels;
    i->buffer_ms = (ULONG)(have * 10 / rate) * 100;
    i->played_ms = st.video ? st.v_ms : (st.played / rate) * 1000 + (st.played % rate) * 1000 / rate;
    i->underruns = st.underruns;
    i->avg_kbps = st_avg_kbps();
    i->ahi = st.ahi;
    i->mhi = st.mhi;
    i->flac = st.flac;
    if (st.mhi && st.started) {
        /* MHI: played by the clock, held: what is queued beyond that */
        ULONG queued = mh_queued_ms(), played = mh_ms();
        if (played > queued)
            played = queued;
        i->buffer_ms = queued - played;
        i->played_ms = played;
    }
    if (st.mhi && st.started) {
        UBYTE l, r;
        mh_peaks(&l, &r);                   /* read off the MP3: none come back */
        st.peak_l = l;
        st.peak_r = r;
    }
    i->peak_l = st.peak_l;
    i->peak_r = st.peak_r;
    st.peak_l = st.peak_l * 3 / 4;
    st.peak_r = st.peak_r * 3 / 4;
}

/* How busy the playback process was since the last call, in percent of the
 * time (-1 not running): for Debug's load line. */
LONG st_busy_sample(void)
{
    static ULONG t0, s0;
    ULONG now = time_ms(), sl = slept_ms, el = now - t0, z = sl - s0;
    LONG r = -1;
    if (t0 && el && st.started)
        r = z >= el ? 0 : (LONG)((el - z) * 100 / el);
    t0 = now;
    s0 = sl;
    return r;
}
