/*
 * AmigaMCE - internet radio: an MP3 stream decoded and played on Paula
 * Copyright (c) 2026 Brian King
 *
 * A station's stream is an endless MP3 file sent over HTTP (Icecast and
 * SHOUTcast servers; SHOUTcast answers "ICY 200 OK"). A process of its own
 * ("AmigaMCE radio") connects, asks for the stream with its titles
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
 * base for the library's inline calls.
 */
#include "amce.h"
#include "radiocsv.h"

#include <string.h>
#include <stdio.h>
#include <exec/interrupts.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <utility/hooks.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/dos.h>
#define SocketBase st_SocketBase
#include <proto/bsdsocket.h>
#include <sys/filio.h>

struct Library *st_SocketBase;      /* the radio process's */
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

#define MPEGA_open(n, c) \
    LP2(30, MPEGA_STREAM *, MPEGA_open, char *, n, a0, MPEGA_CTRL *, c, a1, , MPEGABase)
#define MPEGA_close(s) LP1NR(36, MPEGA_close, MPEGA_STREAM *, s, a0, , MPEGABase)
#define MPEGA_decode_frame(s, p) \
    LP2(42, LONG, MPEGA_decode_frame, MPEGA_STREAM *, s, a0, WORD **, p, a1, , MPEGABase)

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
    /* the FIFO: decoded samples, 8-bit, left and right (mono: left only) */
    BYTE *fl, *fr;
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
    volatile UBYTE peak_l, peak_r;
    UWORD cpu;                      /* 20, 30, 40, 60: how much decoding it can do */
    BOOL finite;                    /* a track: its end is the end, not a failure */
    ULONG mp3_bytes;                /* MP3 given to the decoder: its average bitrate */
    ULONG range_from;               /* a track from this byte on (a seek; 0 all of it) */
    ULONG clen;                     /* a track's whole length in bytes, 0 not known */
    ULONG fpos;                     /* FLAC: where in the file the decoder has read to */
    ULONG skip_ms;                  /* a conversion from this time on (a seek): asked of
                                     * the server (its offset), else frames passed over */
    BOOL skipping;                  /* the server would not: passed over here */
    volatile BOOL eof;              /* a track's last byte read */
} st;

/* ---------------------------------------------------------- Paula */
/* A half of Paula's buffer from the FIFO (the process, when woken). */
static void fill_half(int half)
{
    BYTE *l = st.chip + half * HALF, *r = st.chip + (2 + half) * HALF;
    ULONG have = st.f_in - st.f_out, n = have < HALF ? have : HALF, i, o = st.f_out;
    UBYTE pl = 0, pr = 0;
    BOOL stereo = st.dec_channels == 2;
    for (i = 0; i < n; i++, o++) {
        BYTE a = st.fl[o & (FIFO_N - 1)], b = stereo ? st.fr[o & (FIFO_N - 1)] : a;
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
    st.aint.is_Node.ln_Name = (char *)"AmigaMCE radio";
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

/* ---------------------------------------------------------- the network */
static void fail(const char *why)
{
    if (!st.err[0])
        amce_strlcpy(st.err, why, sizeof(st.err));
    st.state = ST_FAILED;
}

/* Paula wants a half: given it now (from wherever the process waits). */
static void serve(void)
{
    if (st.started && (SetSignal(0, 1UL << st.sig) & (1UL << st.sig)))
        fill_half(st.want);
}

/* Until s can be read or written (1), timed out (0), stopping (-1); Paula
 * served meanwhile. */
static int wait_sock(LONG s, BOOL wr, LONG secs)
{
    ULONG deadline = time_ms() + (ULONG)secs * 1000;
    for (;;) {
        fd_set fs;
        struct timeval tv;
        ULONG sigs = (1UL << st.sig) | SIGBREAKF_CTRL_C;
        LONG r;
        if (st.quit)
            return -1;
        FD_ZERO(&fs);
        FD_SET(s, &fs);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        r = WaitSelect(s + 1, wr ? NULL : &fs, wr ? &fs : NULL, NULL, &tv, &sigs);
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
    if (amce_strnicmp(url, "http://", 7))
        return FALSE;
    h = url + 7;
    slash = strchr(h, '/');
    n = slash ? slash - h : (LONG)strlen(h);
    colon = memchr(h, ':', n);
    *port = 80;
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
            fail("The station's server could not be found (DNS)");
            return -1;
        }
        memcpy(&a, h->h_addr_list[0], 4);
    }
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fail("No socket (is the TCP/IP stack running?)");
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
        fail("The station refused the connection");
        return -1;
    }
    if (wait_sock(s, TRUE, CONNECT_S) <= 0 ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
        CloseSocket(s);
        fail(st.quit ? "Stopped" : "The station did not answer");
        return -1;
    }
    return s;
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
    for (;;) {
        int w = wait_sock(st.sock, FALSE, READ_S);
        if (w <= 0) {
            fail(w < 0 ? "Stopped" : "The station stopped sending");
            return FALSE;
        }
        r = recv(st.sock, st.nb + st.nb_len, NETBUF - st.nb_len, 0);
        if (r < 0 && Errno() == EWOULDBLOCK)
            continue;
        if (r <= 0) {
            if (st.finite && r == 0)
                st.eof = TRUE;              /* a track: all of it read */
            else
                fail(st.finite ? "The server stopped sending" : "The station closed the stream");
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

static LONG num(const char *s)
{
    LONG v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
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

/* Connected to the stream (redirects and playlists followed). */
static BOOL open_stream(void)
{
    static char url[2048], path[2048], req[2600];
    char host[96], hdr[2048], v[160], range[40];
    UWORD port;
    int hop;
    amce_strlcpy(url, st.url, sizeof(url));
    if (st.skip_ms && no_offset && drop_offset(url))
        st.skipping = TRUE;
    for (hop = 0; hop < 4; hop++) {
        LONG status;
        if (!amce_strnicmp(url, "https:", 6)) {
            fail("This station only streams over HTTPS, which AmigaMCE cannot play yet");
            return FALSE;
        }
        if (!split_url(url, host, sizeof(host), &port, path, sizeof(path))) {
            fail("The station's address is not one AmigaMCE can follow");
            return FALSE;
        }
        st.state = ST_CONNECTING;
        if ((st.sock = connect_to(host, port)) < 0)
            return FALSE;
        if (st.range_from)
            sprintf(range, "Range: bytes=%lu-\r\n", (unsigned long)st.range_from);
        else
            range[0] = 0;
        sprintf(req,
                "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: AmigaMCE/" AMCE_VERSION
                "\r\nAccept: */*\r\n%s%sConnection: close\r\n\r\n",
                path, host, st.finite ? "" : "Icy-MetaData: 1\r\n", range);
        {
            LONG left = strlen(req), off = 0;
            while (left > 0) {
                LONG w;
                if (wait_sock(st.sock, TRUE, READ_S) <= 0 ||
                    (w = send(st.sock, (APTR)(req + off), left, 0)) <= 0) {
                    fail(st.quit ? "Stopped" : "The request could not be sent");
                    return FALSE;
                }
                off += w;
                left -= w;
            }
        }
        if (!(status = read_headers(hdr, sizeof(hdr)))) {
            fail(st.err[0] ? st.err : st.finite ? "The server sent no answer we understand"
                                    : "The station sent no answer we understand");
            return FALSE;
        }
        if (status >= 300 && status < 400 && header(hdr, "Location", v, sizeof(v))) {
            CloseSocket(st.sock);
            st.sock = -1;
            amce_strlcpy(url, v, sizeof(url));
            continue;
        }
        if (status == 400 && st.skip_ms && !st.skipping && drop_offset(url)) {
            /* the server would not start its conversion part way in: from
             * the start, the frames before the time passed over here */
            CloseSocket(st.sock);
            st.sock = -1;
            st.skipping = no_offset = TRUE;
            amce_log("track", "the server would not start part way in: seeking by reading");
            hop--;
            continue;
        }
        if (status != 200 && !(status == 206 && st.range_from)) {
            sprintf(v, "%s answered %ld", st.finite ? "The server" : "The station", (long)status);
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
        if (header(hdr, "icy-br", v, sizeof(v)))
            st.bitrate = num(v);
        st.clen = header(hdr, "Content-Length", v, sizeof(v)) ? num(v) + st.range_from : 0;
        st.metaint = header(hdr, "icy-metaint", v, sizeof(v)) ? num(v) : 0;
        st.meta_left = st.metaint;
        if (header(hdr, "Content-Type", v, sizeof(v))) {
            if (strstr(v, "mpegurl") || strstr(v, "scpls") || strstr(v, "x-mpegURL")) {
                /* a playlist: the first http address in it */
                char *p, *e;
                while (st.nb_len < NETBUF - 1 && st.nb_len - st.nb_pos < 1024 && net_more())
                    ;
                st.err[0] = 0;
                st.nb[st.nb_len] = 0;
                if (!(p = strstr((char *)st.nb + st.nb_pos, "http://"))) {
                    fail("The station's playlist has no stream AmigaMCE can play");
                    return FALSE;
                }
                for (e = p; *e && *e != '\r' && *e != '\n'; e++)
                    ;
                *e = 0;
                amce_strlcpy(url, p, sizeof(url));
                CloseSocket(st.sock);
                st.sock = -1;
                continue;
            }
            if (!strstr(v, "mpeg") && !strstr(v, "mp3") && !strstr(v, "octet") &&
                !(st.finite && strstr(v, "flac"))) {
                sprintf(req, "%s sends %s, not MP3", st.finite ? "The server" : "The station", v);
                fail(req);
                return FALSE;
            }
        }
        return TRUE;
    }
    fail("The station sent us on too many times");
    return FALSE;
}

/* A title block: StreamTitle='...'; into st.title. */
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
    if (strcmp(t, st.title)) {
        amce_strlcpy(st.title, t, sizeof(st.title));
        st.title_seq++;
    }
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

/* ---------------------------------------------------------- decoding */
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
            ULONG got = Wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
            if (got & (1UL << st.sig))
                fill_half(st.want);
            continue;
        }
        n = MPEGA_decode_frame(ms, pcm);
        if (n < 0) {
            if (st.eof) {
                if (!st.started && st.f_in != st.f_out) {
                    st.state = ST_PLAYING;  /* shorter than the prebuffer: it plays now */
                    paula_start();
                }
                break;                      /* a track's end: what is left plays out */
            }
            if (n == -2)                    /* a bad frame: the next one */
                continue;
            fail(st.err[0] ? st.err : "The stream could not be decoded");
            break;
        }
        o = st.f_in;
        for (i = 0; i < n; i++, o++) {
            st.fl[o & (FIFO_N - 1)] = (BYTE)(pcm0[i] >> 8);
            if (st.dec_channels == 2)
                st.fr[o & (FIFO_N - 1)] = (BYTE)(pcm1[i] >> 8);
        }
        st.f_in = o;
        if (!st.started && st.f_in - st.f_out >= prebuf) {
            st.state = ST_PLAYING;
            paula_start();
        }
    }
    /* what is left in the FIFO played out, unless stopping */
    while (!st.quit && st.started && st.f_in != st.f_out) {
        ULONG got = Wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C)
            break;
        fill_half(st.want);
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
        /* far, or back: the file asked for again from there */
        {
            UBYTE was = st.state;
            if (st.sock >= 0)
                CloseSocket(st.sock);
            st.sock = -1;
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

/* Decoded frames into the FIFO, Paula started once the prebuffer is in,
 * till the end (or a stop). */
static void flac_loop(APTR dec, UWORD ch, UWORD div, BOOL mono)
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
            ULONG got = Wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
            if (got & (1UL << st.sig))
                fill_half(st.want);
            continue;
        }
        n = FLACDecRead(dec, buf, frames);
        if (n <= 0) {
            if (st.err[0] && !st.eof) {
                fail(st.err);
                break;
            }
            if (!st.started && st.f_in != st.f_out) {
                st.state = ST_PLAYING;      /* shorter than the prebuffer: it plays now */
                paula_start();
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
                st.fl[o & (FIFO_N - 1)] = (BYTE)(((acc_l + acc_r) / (2 * (LONG)div)) >> 8);
            } else {
                st.fl[o & (FIFO_N - 1)] = (BYTE)((acc_l / (LONG)div) >> 8);
                st.fr[o & (FIFO_N - 1)] = (BYTE)((acc_r / (LONG)div) >> 8);
            }
            o++;
            acc_l = acc_r = 0;
            acc_n = 0;
        }
        st.f_in = o;
        if (!st.started && st.f_in - st.f_out >= prebuf) {
            st.state = ST_PLAYING;
            paula_start();
        }
    }
    while (!st.quit && st.started && st.f_in != st.f_out) {
        ULONG got = Wait((1UL << st.sig) | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C)
            break;
        fill_half(st.want);
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
    LONG fmax = st.cpu >= 40 ? 28000 : st.cpu >= 30 ? 24000 : 12000;
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
    st.dec_freq = info.fdi_SampleRate / div;
    st.dec_channels = mono ? 1 : 2;
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
    flac_loop(dec, info.fdi_Channels, div, mono);
out:
    if (dec)
        FLACDecClose(dec);
    CloseLibrary(FLACDecBase);
    FLACDecBase = NULL;
}

static void radio_main(void)
{
    MPEGA_STREAM *ms = NULL;
    static struct Hook hook;
    MPEGA_CTRL ctrl;
    BYTE s = AllocSignal(-1);
    st.sig = s;
    st.sock = -1;
    if (s < 0) {
        fail("No signal free");
        goto out;
    }
    if (!(st_SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4))) {
        fail("No TCP/IP stack is running (bsdsocket.library)");
        goto out;
    }
    if (!open_stream())
        goto out;
    if (st.finite && !skip_id3()) {
        fail(st.err[0] ? st.err : "The track ended before its music began");
        goto out;
    }
    if (st.finite && is_flac()) {
        flac_play();                        /* a FLAC file, as stored */
        goto out;
    }
    if (!(MPEGABase = OpenLibrary((CONST_STRPTR)"mpega.library", 2))) {
        fail("mpega.library is needed to play MP3");
        goto out;
    }
    if (st.skipping && !skip_to()) {
        fail(st.err[0] ? st.err : "The track ended before that time");
        goto out;
    }
    st.state = ST_BUFFERING;
    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (HOOKFUNC)(APTR)hook_func;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bs_access = &hook;
    {
        /* what the CPU can decode in time, and never more than Paula plays */
        WORD q = st.cpu >= 40 ? 1 : 0, mono = st.cpu < 40, div = 1;
        LONG fmax = st.cpu >= 40 ? 28000 : st.cpu >= 30 ? 24000 : 12000;
        LONG f = first_frame_freq();
        MPEGA_LAYER *ls[2];
        int k;
        if (!f) {
            fail(st.err[0] ? st.err : "The stream does not look like MP3");
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
    if (!(ms = MPEGA_open((char *)"AmigaMCE radio", &ctrl))) {
        fail(st.err[0] ? st.err : "The stream is not MP3 that mpega.library can decode");
        goto out;
    }
    st.freq = ms->frequency;
    st.channels = ms->channels;
    st.dec_freq = ms->dec_frequency;
    st.dec_channels = ms->dec_channels;
    if (!st.bitrate)
        st.bitrate = ms->bitrate;
    if (st.dec_freq > 28867 || st.dec_freq < 4000)
        st.dec_freq = st.dec_freq > 28867 ? st.dec_freq / 2 : 22050;
    {
        /* named by its station, else its URL without the query: a Plex
         * track's carries the sign-in token, which stays out of the log */
        char m[360], u[200], *q;
        amce_strlcpy(u, st.url, sizeof(u));
        if ((q = strchr(u, '?')))
            *q = 0;
        sprintf(m, "%s: MP3 %ld kbit/s %ld Hz %s, decoded at %ld Hz %s (quality %ld)",
                st.name[0] && !st.finite ? st.name : u, (long)st.bitrate, (long)st.freq,
                st.channels == 2 ? "stereo" : "mono", (long)st.dec_freq,
                st.dec_channels == 2 ? "stereo" : "mono", (long)ms->dec_quality);
        amce_log(st.finite ? "track" : "radio", m);
    }
    decode_loop(ms);
out:
    if (st.started)
        paula_stop();
    if (ms)
        MPEGA_close(ms);
    if (st.sock >= 0)
        CloseSocket(st.sock);
    st.sock = -1;
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
    st.skipping = FALSE;
    st.eof = FALSE;
    memset(&st.err, 0, sizeof(st.err));
    st.name[0] = st.genre[0] = st.title[0] = 0;
    st.title_seq++;
    st.bitrate = st.freq = st.channels = st.dec_freq = st.dec_channels = 0;
    st.f_in = st.f_out = st.underruns = st.played = 0;
    st.mp3_bytes = 0;
    st.peak_l = st.peak_r = 0;
    st.quit = st.started = st.paused = FALSE;
    st.master = master;
    st.sig = -1;
    amce_strlcpy(st.url, url, sizeof(st.url));
    st.fifo_n = fifo_sizes[buf_level];
    st.nb = AllocVec(NETBUF + 1, MEMF_ANY);
    st.fl = AllocVec(FIFO_N, MEMF_ANY | MEMF_CLEAR);
    st.fr = AllocVec(FIFO_N, MEMF_ANY | MEMF_CLEAR);
    st.chip = AllocMem(HALF * 4, MEMF_CHIP | MEMF_CLEAR);
    if (!st.nb || !st.fl || !st.fr || !st.chip) {
        st_stop();
        amce_strlcpy(err, "Not enough memory to play a station", errmax);
        return FALSE;
    }
    st.state = ST_CONNECTING;
    st.cpu = cpu;
    st.proc = CreateNewProcTags(NP_Entry, (ULONG)radio_main, NP_Name, (ULONG)"AmigaMCE radio",
                                NP_Priority, 3, NP_StackSize, 32768, NP_WindowPtr, (ULONG)-1,
                                TAG_DONE);
    if (!st.proc) {
        st_stop();
        amce_strlcpy(err, "Could not start the radio process", errmax);
        return FALSE;
    }
    return TRUE;
}

void st_stop(void)
{
    if (st.proc) {
        st.quit = TRUE;
        Signal(&st.proc->pr_Task, SIGBREAKF_CTRL_C);
        while (st.proc)
            Delay(1);                       /* it stops within a fifth of a second */
    }
    if (st.started)
        paula_stop();
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

void st_pause(BOOL on)
{
    st.paused = on;
    if (st.started) {
        CU->aud[0].ac_vol = on ? 0 : st.master;
        CU->aud[1].ac_vol = on ? 0 : st.master;
    }
}

void st_volume(int v)
{
    st.master = (UWORD)v;
    if (st.started && !st.paused) {
        CU->aud[0].ac_vol = st.master;
        CU->aud[1].ac_vol = st.master;
    }
}

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
    i->played_ms = (st.played / rate) * 1000 + (st.played % rate) * 1000 / rate;
    i->underruns = st.underruns;
    i->avg_kbps = st_avg_kbps();
    i->peak_l = st.peak_l;
    i->peak_r = st.peak_r;
    st.peak_l = st.peak_l * 3 / 4;
    st.peak_r = st.peak_r * 3 / 4;
}
