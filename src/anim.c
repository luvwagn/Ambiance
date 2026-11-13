/*
 * AmigaMCE - IFF ANIM player (Videos)
 * Copyright (c) 2026 Brian King
 *
 * An ANIM plays on a screen of its own, in its own display mode (HAM,
 * extra halfbrite, interlace, overscan: what it was drawn for), opened in
 * front of AmigaMCE's, which waits behind it as it was. Its frames come
 * from the file one at a time, never the whole of it (Tron2 is 28 MB),
 * and are decoded straight into the bitplanes (animdec.c), two buffers
 * shown in turn as the format intends: a frame changes the one two back.
 *
 * Each frame is shown when its time comes (ANHD: sixtieths of a second);
 * a frame that takes longer to read and decode is shown late and the
 * rest follow it, none skipped (a delta needs the frame before). The log
 * says how it went: frames a second wanted and got, the slowest frame.
 *
 * Sound: an ANIM can carry 8SVX samples (before its first frame) and SCTL
 * chunks in its frames, each saying "play sound n on these channels, at
 * this volume and rate, so many times" (or stop them). The samples go to
 * chip RAM when it opens, and each SCTL is acted on the moment its frame
 * is shown, so the sound keeps to the pictures. What played meanwhile
 * waits (main.c: a module paused, a station or a track stopped and played
 * again after). With AHI the sound output (Settings > Sound), the sounds
 * go to ahi.device's default unit, a request for each side, panned; else
 * the music player lends Paula's channels 2 and 3 (mp_lend). A sound asked
 * for on the left (0, 3) plays on the left (Paula's 3), on the right (1,
 * 2) on the right (2). Up and Down set its volume, scaling each sound's
 * own (with AHI from the sound's next start).
 *
 * Move the mouse and the controls come up over the bottom of the picture
 * (restart, play or pause, stop, volume, the time, how far it has got);
 * they go again a moment after it stops, and stay while it is paused.
 *
 * Keys: Esc, Backspace, the right button or the pad's blue stop it; Space,
 * Return, the left button (not on the controls) or fire pause it; Up and Down (the stick, the
 * wheel) set the volume of what plays meanwhile, a station or a module,
 * as on Now playing. A short one (under 8 s) plays
 * again until stopped; a longer one goes back to the list at its end.
 */
#include "amce.h"
#include "animdec.h"
#include "animdraw.h"

#include <string.h>
#include <stdio.h>
#include <exec/ports.h>
#include <dos/dosextens.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <libraries/lowlevel.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/lowlevel.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

extern struct GfxBase *GfxBase;
extern struct Library *LowLevelBase, *CyberGfxBase;

#define ID(a, b, c, d) (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (d))
#define LOOP_MS 8000            /* a whole run shorter than this plays again */

static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static UWORD be16(const UBYTE *p)
{
    return (UWORD)((p[0] << 8) | p[1]);
}

/* One frame's FORM, as read */
#define FRAME_SCTL 8
struct Frame {
    const UBYTE *bmhd, *cmap, *body, *dlta, *anhd;
    const UBYTE *sctl[FRAME_SCTL];  /* its sound commands, 16 bytes each */
    ULONG cmaplen, bodylen, dltalen, camg;
    int nsctl;
    BOOL has_camg;
};

/* ---- sound */
#define CU ((volatile struct Custom *)0xDFF000)
#define SOUNDS 32
static struct Sound {
    UBYTE *data;                /* chip RAM */
    ULONG len;                  /* bytes (even, 128 KB at the most: one Paula block) */
    UWORD rate;                 /* samples a second (VHDR) */
    UWORD vol;                  /* its own, 0-64 (VHDR's 1.0 = 64) */
} snd[SOUNDS];
static int nsnd;
static UBYTE *silence;          /* a quiet word for a channel to rest on */
static BOOL snd_on;             /* channels 2 and 3 lent (mp_lend) */
static BOOL snd_read;           /* its sounds read (the first frame reached) */
static struct {
    BOOL busy, last;            /* sounding; on its last time round (silence queued) */
    UWORD svol;                 /* SCTL's volume x the sound's (0-64) */
    ULONG start, dur;           /* ms: started, one time through */
    UWORD times;                /* times to play (0: until stopped) */
    const struct Sound *s;
} ch[4];

static void lines(int n)
{
    while (n-- > 0) {
        UBYTE v = CU->vhposr >> 8;
        int guard = 2000;
        while ((UBYTE)(CU->vhposr >> 8) == v && --guard)
            ;
    }
}

static UWORD ch_vol(int c)
{
    return (UWORD)((ULONG)ch[c].svol * mp_volume() / 64);
}

/* ---- by AHI: a request for each side (2 right, 3 left), sent again for
 * each time round; AHI mixes them with whatever else uses it. */
static BOOL snd_ahi;
static struct MsgPort *ahi_port;
static struct AHIRequest *ahi_dev, *ahi_io[4];
static BOOL ahi_busy[4];
static UWORD ahi_rate[4], ahi_done[4];

static void ahi_close(void)
{
    int c;
    for (c = 2; c < 4; c++) {
        if (ahi_io[c] && ahi_busy[c]) {
            AbortIO((struct IORequest *)ahi_io[c]);
            WaitIO((struct IORequest *)ahi_io[c]);
        }
        ahi_busy[c] = FALSE;
        FreeVec(ahi_io[c]);
        ahi_io[c] = NULL;
    }
    if (ahi_dev) {
        CloseDevice((struct IORequest *)ahi_dev);
        DeleteIORequest((struct IORequest *)ahi_dev);
        ahi_dev = NULL;
    }
    if (ahi_port)
        DeleteMsgPort(ahi_port);
    ahi_port = NULL;
}

static BOOL ahi_open(void)
{
    int c;
    if (!(ahi_port = CreateMsgPort()) ||
        !(ahi_dev = (struct AHIRequest *)CreateIORequest(ahi_port, sizeof(struct AHIRequest))))
        goto fail;
    ahi_dev->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)AHINAME, AHI_DEFAULT_UNIT, (struct IORequest *)ahi_dev, 0)) {
        DeleteIORequest((struct IORequest *)ahi_dev);
        ahi_dev = NULL;
        goto fail;
    }
    for (c = 2; c < 4; c++) {
        if (!(ahi_io[c] = AllocVec(sizeof(struct AHIRequest), MEMF_PUBLIC)))
            goto fail;
        CopyMem(ahi_dev, ahi_io[c], sizeof(struct AHIRequest));
    }
    return TRUE;
fail:
    ahi_close();
    return FALSE;
}

static void ahi_send(int c)
{
    struct AHIRequest *io = ahi_io[c];
    io->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
    io->ahir_Std.io_Command = CMD_WRITE;
    io->ahir_Std.io_Data = ch[c].s->data;
    io->ahir_Std.io_Length = ch[c].s->len;
    io->ahir_Std.io_Offset = 0;
    io->ahir_Frequency = ahi_rate[c];
    io->ahir_Type = AHIST_M8S;
    io->ahir_Volume = (Fixed)((ULONG)ch_vol(c) << 10);     /* 64: 1.0 */
    io->ahir_Position = c == 3 ? 0 : 0x10000;               /* left, right */
    io->ahir_Link = NULL;
    SendIO((struct IORequest *)io);
    ahi_busy[c] = TRUE;
    ahi_done[c]++;
}

static void ahi_stop(int c)
{
    if (ahi_io[c] && ahi_busy[c]) {
        AbortIO((struct IORequest *)ahi_io[c]);
        WaitIO((struct IORequest *)ahi_io[c]);
    }
    ahi_busy[c] = FALSE;
}

/* ---- by Paula */
static void snd_quiet(int c)
{
    ch[c].busy = FALSE;
    if (snd_ahi) {
        ahi_stop(c);
        return;
    }
    CU->dmacon = (UWORD)(1 << c);
    CU->aud[c].ac_vol = 0;
}

/* Sound s on channel c (2 or 3), times times (0: until stopped). */
static void snd_play(int c, const struct Sound *s, UWORD vol, UWORD times, UWORD rate)
{
    ULONG clock = SysBase->ex_EClockFrequency * 5;  /* Paula's: 3.55 MHz PAL, 3.58 NTSC */
    ULONG per = rate ? clock / rate : 428;
    if (per < 124)
        per = 124;
    if (per > 65535)
        per = 65535;
    if (snd_ahi)
        ahi_stop(c);
    else {
        CU->dmacon = (UWORD)(1 << c);
        lines(2);                           /* stopped, so it starts from the beginning */
    }
    ch[c].svol = (UWORD)((ULONG)vol * s->vol / 64);
    ch[c].s = s;
    ch[c].times = times;
    ch[c].start = time_ms();
    ch[c].dur = (ULONG)s->len * 1000 / (rate ? rate : 8363);
    ch[c].busy = TRUE;
    ch[c].last = times == 1;
    if (snd_ahi) {
        ahi_rate[c] = rate ? rate : 8363;
        ahi_done[c] = 0;
        ahi_send(c);
        return;
    }
    CU->aud[c].ac_ptr = (UWORD *)s->data;
    CU->aud[c].ac_len = (UWORD)(s->len / 2);
    CU->aud[c].ac_per = (UWORD)per;
    CU->aud[c].ac_vol = ch_vol(c);
    CU->dmacon = DMAF_SETCLR | (UWORD)(1 << c);
    lines(1);                               /* taken: what follows it is set now */
    if (ch[c].last) {
        CU->aud[c].ac_ptr = (UWORD *)silence;
        CU->aud[c].ac_len = 1;
    }
}

/* Once a frame: repeats counted out, a change of volume followed. */
static void snd_tick(void)
{
    int c;
    ULONG now = time_ms();
    for (c = 2; c < 4; c++) {
        if (!ch[c].busy)
            continue;
        if (snd_ahi) {                      /* a time round done: the next, or quiet */
            if (ahi_busy[c] && CheckIO((struct IORequest *)ahi_io[c])) {
                WaitIO((struct IORequest *)ahi_io[c]);
                ahi_busy[c] = FALSE;
                if (!ch[c].times || ahi_done[c] < ch[c].times)
                    ahi_send(c);
                else
                    ch[c].busy = FALSE;
            }
            continue;
        }
        CU->aud[c].ac_vol = ch_vol(c);
        if (!ch[c].last && ch[c].times &&
            now - ch[c].start >= ch[c].dur * (ch[c].times - 1)) {
            CU->aud[c].ac_ptr = (UWORD *)silence;   /* after this time round: quiet */
            CU->aud[c].ac_len = 1;
            ch[c].last = TRUE;
        }
        if (ch[c].last && now - ch[c].start > ch[c].dur * (ch[c].times ? ch[c].times : 1) + 100)
            ch[c].busy = FALSE;             /* done: resting on the quiet word */
    }
}

/* A frame's SCTL: 0 command (1 play, 2 stop), 1 volume, 2 sound (from 1),
 * 4 times, 6 channels (bits 0-3), 8 rate (0: the sound's own), 10 flags. */
static void snd_command(const UBYTE *sc)
{
    UWORD n = be16(sc + 2), times = be16(sc + 4), mask = be16(sc + 6), rate = be16(sc + 8);
    BOOL left = (mask & 9) != 0, right = (mask & 6) != 0;
    UWORD vol = sc[1] > 64 ? 64 : sc[1];
    if (!snd_on)
        return;
    if (!left && !right)
        left = right = TRUE;
    if (sc[0] == 2) {
        if (right)
            snd_quiet(2);
        if (left)
            snd_quiet(3);
        return;
    }
    if (sc[0] != 1 || n < 1 || n > nsnd || !snd[n - 1].data)
        return;
    if (right)
        snd_play(2, &snd[n - 1], vol, times, rate ? rate : snd[n - 1].rate);
    if (left)
        snd_play(3, &snd[n - 1], vol, times, rate ? rate : snd[n - 1].rate);
}

/* ---- the file read ahead: frames are small (Tron2's 24 KB), and the file
 * comes off Amiberry's hard file at some 4 MB a second (6 ms a frame).
 * Read in RD_SIZE pieces, two of them: while frames are copied out of
 * one, the next is being read into the other, asked for with a DOS packet
 * (ACTION_READ, SendPkt) that is not waited for; the device moves it on a
 * thread of its own, and meanwhile frames are decoded and drawn. With no
 * handler to ask (or no memory) a piece is read when it is needed, as
 * Read does.
 *
 * Everything that reads or skips in the file goes through here, so its
 * place and the pieces' agree: rd_seek goes back (to the start), rd_sync
 * gives the file its place for one reading it itself (anim_length); both
 * wait for a piece being read first, as nothing else may use the file
 * while a packet is out. */
#define RD_SIZE (256UL * 1024)
static struct {
    BPTR fh;
    UBYTE *buf[2];                  /* the pieces: cur being read from, the other being read */
    int cur;
    LONG pos, len;                  /* what is left of cur: buf[cur][pos..len) */
    struct MsgPort *port;           /* the handler's (fh_Type), and the replies' */
    struct MsgPort *reply;
    struct DosPacket *dp;
    BOOL out;                       /* a piece being read into buf[!cur] */
    BOOL eof;                       /* the last piece came short: no more to ask for */
    ULONG waits, waitms;            /* (the log: pieces waited for, and how long) */
} rd;

static void rd_ask(void)                    /* the next piece, into the other buffer */
{
    struct FileHandle *h = (struct FileHandle *)BADDR(rd.fh);
    if (rd.out || rd.eof || !rd.dp)
        return;
    rd.dp->dp_Type = ACTION_READ;
    rd.dp->dp_Arg1 = h->fh_Arg1;
    rd.dp->dp_Arg2 = (LONG)rd.buf[!rd.cur];
    rd.dp->dp_Arg3 = RD_SIZE;
    SendPkt(rd.dp, rd.port, rd.reply);
    rd.out = TRUE;
}

static LONG rd_wait(void)                   /* the piece asked for: its length, -1 failed */
{
    ULONG t0 = time_ms();
    if (!rd.out)
        return 0;
    WaitPort(rd.reply);
    GetMsg(rd.reply);
    rd.out = FALSE;
    rd.waits++;
    rd.waitms += time_ms() - t0;
    return rd.dp->dp_Res1;
}

static void rd_begin(BPTR fh)
{
    struct FileHandle *h = (struct FileHandle *)BADDR(fh);
    memset(&rd, 0, sizeof(rd));
    rd.fh = fh;
    if (!(rd.buf[0] = AllocVec(RD_SIZE, MEMF_ANY)))
        return;                             /* none: straight from the file */
    if (h && h->fh_Type && (rd.buf[1] = AllocVec(RD_SIZE, MEMF_ANY)) &&
        (rd.reply = CreateMsgPort()) && (rd.dp = AllocDosObject(DOS_STDPKT, NULL)))
        rd.port = h->fh_Type;
    else {                                  /* one piece, read when needed */
        FreeVec(rd.buf[1]);
        rd.buf[1] = NULL;
        if (rd.reply)
            DeleteMsgPort(rd.reply);
        rd.reply = NULL;
        rd.dp = NULL;
    }
}

static void rd_end(void)
{
    rd_wait();
    if (rd.dp)
        FreeDosObject(DOS_STDPKT, rd.dp);
    if (rd.reply)
        DeleteMsgPort(rd.reply);
    FreeVec(rd.buf[0]);
    FreeVec(rd.buf[1]);
    memset(&rd, 0, sizeof(rd));
}

/* The next piece in cur (the one asked for, or read now), the one after
 * it asked for: FALSE at the end. */
static BOOL rd_next(void)
{
    LONG n;
    if (rd.out) {
        n = rd_wait();
        rd.cur = !rd.cur;
    } else if (rd.eof)
        n = 0;
    else
        n = Read(rd.fh, rd.buf[rd.cur], RD_SIZE);
    rd.pos = 0;
    rd.len = n > 0 ? n : 0;
    if (n < (LONG)RD_SIZE)
        rd.eof = TRUE;                      /* short (or failed): the file's end */
    rd_ask();
    return rd.len > 0;
}

static LONG rd_read(void *dst, LONG n)
{
    UBYTE *d = dst;
    LONG got = 0;
    if (!rd.buf[0])
        return Read(rd.fh, dst, n);
    while (n > 0) {
        LONG have = rd.len - rd.pos, k;
        if (!have) {
            if (!rd_next())
                break;
            have = rd.len;
        }
        k = n < have ? n : have;
        CopyMem(rd.buf[rd.cur] + rd.pos, d, k);
        rd.pos += k;
        d += k;
        got += k;
        n -= k;
    }
    return got;
}

/* The file at the place reading has got to, nothing held: a piece being
 * read waited for (its bytes not used: the file is gone back over them). */
static void rd_sync(void)
{
    LONG back = rd.len - rd.pos;
    if (rd.out) {
        LONG n = rd_wait();
        if (n > 0)
            back += n;
    }
    if (back)
        Seek(rd.fh, -back, OFFSET_CURRENT);
    rd.pos = rd.len = 0;
    rd.eof = FALSE;
}

/* n bytes on (Seek's OFFSET_CURRENT, forward): -1 if it cannot */
static LONG rd_skip(LONG n)
{
    LONG have = rd.len - rd.pos;
    if (rd.buf[0] && n <= have) {
        rd.pos += n;
        return 0;
    }
    if (rd.buf[0] && rd.out && n - have <= (LONG)RD_SIZE) {
        rd.pos = rd.len;                    /* into the next piece: taken as it comes */
        n -= have;
        if (!rd_next() && n)
            return -1;
        if (n > rd.len)
            return -1;
        rd.pos = n;
        return 0;
    }
    rd_sync();
    return Seek(rd.fh, n, OFFSET_CURRENT) < 0 ? -1 : 0;
}

static LONG rd_seek(LONG at)                /* from the file's start, as Seek */
{
    rd_wait();                              /* (its bytes not wanted) */
    rd.pos = rd.len = 0;
    rd.eof = FALSE;
    return Seek(rd.fh, at, OFFSET_BEGINNING);
}

/* A FORM 8SVX's sound (its first octave), len bytes after the type, from
 * fh into chip RAM; Fibonacci-delta packed ones unpacked. */
static void snd_load(BPTR fh, ULONG len)
{
    static const BYTE fib[16] = { -34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21 };
    UBYTE *b, *p, *e;
    const UBYTE *vhdr = NULL, *body = NULL;
    ULONG bl = 0, n;
    struct Sound *s;
    (void)fh;                               /* (read through rd) */
    if (nsnd >= SOUNDS) {
        rd_skip(len + (len & 1));
        return;
    }
    s = &snd[nsnd++];                       /* numbered as they come, read or not */
    if (!(b = AllocVec(len, MEMF_ANY)) || rd_read(b, len) != (LONG)len) {
        FreeVec(b);
        return;
    }
    if (len & 1)
        rd_skip(1);
    for (p = b, e = b + len; p + 8 <= e;) {
        ULONG id = be32(p), cl = be32(p + 4);
        if (cl > (ULONG)(e - p - 8))
            cl = e - p - 8;
        if (id == ID('V', 'H', 'D', 'R') && cl >= 20)
            vhdr = p + 8;
        else if (id == ID('B', 'O', 'D', 'Y')) {
            body = p + 8;
            bl = cl;
        }
        p += 8 + ((cl + 1) & ~1UL);
    }
    if (vhdr && body && bl > 2) {
        ULONG oct = be32(vhdr) + be32(vhdr + 4);    /* one-shot and repeat parts */
        BOOL packed = vhdr[15] == 1;
        n = packed ? (bl - 2) * 2 : bl;
        if (oct && oct < n)
            n = oct;
        n &= ~1UL;
        if (n > 131070)
            n = 131070;                     /* one Paula block */
        if (n >= 2 && (s->data = AllocVec(n, MEMF_CHIP))) {
            if (packed) {
                BYTE x = (BYTE)body[1];
                ULONG i;
                for (i = 0; i < n; i++) {
                    UBYTE c = body[2 + i / 2];
                    x += fib[i & 1 ? c & 15 : c >> 4];
                    s->data[i] = (UBYTE)x;
                }
            } else {
                CopyMem((APTR)body, s->data, n);
            }
            s->len = n;
            s->rate = be16(vhdr + 12);
            s->vol = (UWORD)(be32(vhdr + 16) >> 10);    /* 1.0 (0x10000) = 64 */
            if (!s->vol || s->vol > 64)
                s->vol = 64;
        }
    }
    FreeVec(b);
}

static void snd_free(void)
{
    int i;
    if (snd_on && snd_ahi) {
        ahi_close();
        snd_ahi = snd_on = FALSE;
    } else if (snd_on) {
        snd_quiet(2);
        snd_quiet(3);
        lines(3);
        mp_lend(FALSE);
        snd_on = FALSE;
    }
    for (i = 0; i < nsnd; i++) {
        FreeVec(snd[i].data);
        snd[i].data = NULL;
    }
    nsnd = 0;
    snd_read = FALSE;
    if (silence)
        FreeMem(silence, 4);
    silence = NULL;
    memset(ch, 0, sizeof(ch));
}

static UBYTE *fbuf;             /* the frame being read */
static ULONG fbuf_size;

/* The next FORM ILBM from fh into fbuf, its chunks found; FALSE at the
 * end (or anything that is not one). */
static BOOL read_frame(BPTR fh, struct Frame *f, ULONG *readms)
{
    UBYTE h[12];
    ULONG len, t0 = time_ms();
    const UBYTE *p, *e;
    for (;;) {
        if (rd_read(h, 12) != 12 || be32(h) != ID('F', 'O', 'R', 'M'))
            return FALSE;
        len = be32(h + 4);
        if (len < 4)
            return FALSE;
        len -= 4;
        if (be32(h + 8) == ID('I', 'L', 'B', 'M'))
            break;
        if (be32(h + 8) == ID('8', 'S', 'V', 'X') && !snd_read && nsnd < SOUNDS) {
            snd_load(fh, len);              /* a sound, before the first frame */
            continue;
        }
        if (rd_skip(len + (len & 1)) < 0)  /* the rest: passed */
            return FALSE;
    }
    if (len > fbuf_size) {
        FreeVec(fbuf);
        fbuf_size = len + 4096;
        if (!(fbuf = AllocVec(fbuf_size, MEMF_ANY))) {
            fbuf_size = 0;
            return FALSE;
        }
    }
    if (rd_read(fbuf, len) != (LONG)len)
        return FALSE;
    if (len & 1)
        rd_skip(1);
    memset(f, 0, sizeof(*f));
    for (p = fbuf, e = fbuf + len; p + 8 <= e;) {
        ULONG id = be32(p), cl = be32(p + 4);
        const UBYTE *d = p + 8;
        if (cl > (ULONG)(e - d))
            cl = e - d;
        if (id == ID('B', 'M', 'H', 'D') && cl >= 20)
            f->bmhd = d;
        else if (id == ID('C', 'M', 'A', 'P')) {
            f->cmap = d;
            f->cmaplen = cl;
        } else if (id == ID('C', 'A', 'M', 'G') && cl >= 4) {
            f->camg = be32(d);
            f->has_camg = TRUE;
        } else if (id == ID('A', 'N', 'H', 'D') && cl >= 24)
            f->anhd = d;
        else if (id == ID('B', 'O', 'D', 'Y')) {
            f->body = d;
            f->bodylen = cl;
        } else if (id == ID('D', 'L', 'T', 'A')) {
            f->dlta = d;
            f->dltalen = cl;
        } else if (id == ID('S', 'C', 'T', 'L') && cl >= 12 && f->nsctl < FRAME_SCTL)
            f->sctl[f->nsctl++] = d;
        p = d + ((cl + 1) & ~1UL);
    }
    *readms += time_ms() - t0;
    return TRUE;
}

/* A CMAP as a LoadRGB32 table (n colours of the screen's, the rest black). */
static void cmap_table(const UBYTE *c, ULONG len, ULONG ncol, ULONG *tab)
{
    ULONG i, n = len / 3, lo = 0;
    if (n > ncol)
        n = ncol;
    for (i = 0; i < n * 3; i++)
        lo |= c[i] & 0x0F;
    tab[0] = ncol << 16;
    for (i = 0; i < ncol * 3; i++) {
        ULONG v = i < n * 3 ? c[i] : 0;
        if (!lo)
            v |= v >> 4;                    /* old 4-bit colours, 0xF0: widened */
        tab[1 + i] = v * 0x01010101UL;
    }
    tab[1 + ncol * 3] = 0;
}

/* The display mode for the ANIM's CAMG: as it is if this Amiga has it,
 * else on the default monitor (an NTSC one on a PAL Amiga), else the best
 * one there is for its size, depth and kind. INVALID_ID if none. */
static ULONG pick_mode(ULONG camg, BOOL has_camg, UWORD w, UWORD h, UBYTE depth)
{
    ULONG keys, id, need = 0;
    if (!has_camg)                          /* none said: by its size */
        camg = (w > 400 ? HIRES_KEY : 0) | (h > 300 ? LACE : 0);
    camg &= ~(ULONG)(SPRITES | VP_HIDE | GENLOCK_AUDIO | GENLOCK_VIDEO);
    if (!(camg & 0xFFFF0000UL))
        camg &= ~(ULONG)EXTENDED_MODE;      /* an old 16-bit one */
    keys = camg & (HAM_KEY | EXTRAHALFBRITE_KEY | LACE | HIRES | SUPERHIRES);
    if (!ModeNotAvailable(camg))
        return camg;
    if (!ModeNotAvailable(keys))
        return keys;
    if (camg & HAM_KEY)
        need = DIPF_IS_HAM;
    else if (camg & EXTRAHALFBRITE_KEY)
        need = DIPF_IS_EXTRAHALFBRITE;
    id = BestModeID(BIDTAG_NominalWidth, w, BIDTAG_NominalHeight, h, BIDTAG_Depth, depth,
                    BIDTAG_DIPFMustHave, need,
                    BIDTAG_DIPFMustNotHave, (SPECIAL_FLAGS & ~need) | DIPF_IS_FOREIGN,
                    TAG_DONE);
    return id;
}

/* The mode's deepest bitmap (AGA's 8 planes, ECS's 5 or 6) */
static UWORD mode_depth(ULONG id)
{
    struct DimensionInfo dim;
    if (!GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, id))
        return 0;
    return dim.MaxDepth;
}

/* Its place: centred on the mode's normal (text) area, the display clip
 * as far as the mode's overscan goes. */
static void place(ULONG id, UWORD w, UWORD h, WORD *left, WORD *top, struct Rectangle *clip)
{
    struct Rectangle txt, max;
    WORD tw, th;
    if (!QueryOverscan(id, &txt, OSCAN_TEXT) || !QueryOverscan(id, &max, OSCAN_MAX)) {
        *left = *top = 0;
        clip->MinX = clip->MinY = 0;
        clip->MaxX = w - 1;
        clip->MaxY = h - 1;
        return;
    }
    tw = txt.MaxX - txt.MinX + 1;
    th = txt.MaxY - txt.MinY + 1;
    *left = txt.MinX + (tw - (WORD)w) / 2;
    *top = txt.MinY + (th - (WORD)h) / 2;
    clip->MinX = *left > max.MinX ? *left : max.MinX;
    clip->MinY = *top > max.MinY ? *top : max.MinY;
    clip->MaxX = *left + (WORD)w - 1 < max.MaxX ? *left + (WORD)w - 1 : max.MaxX;
    clip->MaxY = *top + (WORD)h - 1 < max.MaxY ? *top + (WORD)h - 1 : max.MaxY;
}

static void planes_of(struct BitMap *bm, UWORD w, UWORD h, UBYTE depth, struct AnimPlanes *pl)
{
    int i;
    for (i = 0; i < 8; i++)
        pl->plane[i] = i < depth ? (unsigned char *)bm->Planes[i] : NULL;
    pl->stride = bm->BytesPerRow;
    pl->depth = depth;
    pl->rb = ((w + 15) >> 4) << 1;
    if (pl->rb > bm->BytesPerRow)
        pl->rb = bm->BytesPerRow;
    pl->h = h < bm->Rows ? h : bm->Rows;
}

/* The last buffer change's message in: the buffer it let go is free. */
static void safe_done(struct MsgPort *safe, BOOL *wait)
{
    while (safe && (wait[0] || wait[1])) {
        if (GetMsg(safe))
            wait[0] = wait[1] = FALSE;
        else
            Wait(1UL << safe->mp_SigBit);
    }
}

/* ---- the controls, over the bottom of the picture while the mouse moves
 * (and while paused): restart, play or pause, stop, volume down and up,
 * the time and the length, and how far it has got. They are drawn into
 * the frame's own buffer, in its darkest and lightest colours, and what
 * they cover is kept (save[k]) and put back before the next frame is
 * decoded into that buffer, as the deltas build on what was there. */
#define BAR_HIDE 2500           /* ms after the mouse stops, playing */
enum { BT_RESTART, BT_PLAY, BT_STOP, BT_VDOWN, BT_VUP, BT_FULL, BT_N };   /* full: AmigaMCE's screen */

/* No sound of its own: the volume's buttons (and its figure) not shown. */
static BOOL btn_hidden(int b)
{
    return (b == BT_VDOWN || b == BT_VUP) && !nsnd;
}

static struct {
    BOOL on, ok;                /* shown; it can be (its memory and font) */
    BOOL saved[2];              /* buffer k has it drawn, what it covers in save[k] */
    struct BitMap *save[2];
    struct RastPort rp;
    struct TextFont *font;
    WORD x0, x1, y, h, cw;      /* its place; a button's width */
    WORD px0, px1;              /* the progress line */
    WORD bx[BT_N];              /* each button's left */
    UBYTE bg, fg, dim;
    ULONG total_ms, at_ms;
    BOOL paused;
} bar;

/* ---- the controls' comings and goings, the same on either screen. The
 * mouse brings them, with its pointer; they go again BAR_HIDE ms after it
 * stops (not while paused). A key or the pad brings them to stay, without
 * the pointer, a button in focus (the yellow ring, as Now playing's):
 * Left and Right move it, Return or fire presses it. Hidden, the focus
 * goes back to play/pause, so Return or fire pauses, as ever; Space
 * always does, Up and Down set the volume, Esc stops. Each player draws
 * them its own way: AmigaMCE's screen beneath the picture, fading in and
 * out; a screen of its own over the picture's foot, in its colours. */
enum { CM_NONE, CM_MOUSE, CM_KEY };
static struct {
    ULONG moved, opened, key;       /* clocks: the mouse last moved, the window opened, a key */
    ULONG moved_seen, key_seen;
    int mode, focus, nbtn;          /* nbtn: the buttons it has (BT_FULL: not that one) */
    UWORD off;                      /* buttons greyed (1 << BT_*): not pressed, not in focus */
    BOOL shown, ptr;
    UWORD *blank;                   /* an empty pointer (chip memory) */
    struct Window *win;
} ctl;

static void ctl_begin(struct Window *win, UWORD *blank)
{
    memset(&ctl, 0, sizeof(ctl));
    ctl.win = win;
    ctl.blank = blank;
    ctl.opened = time_ms();
    ctl.focus = 1;                  /* BT_PLAY */
    ctl.nbtn = 5;                   /* BT_FULL: not on a screen of its own */
    if (blank)
        SetPointer(win, blank, 1, 16, 0, 0);
}

static BOOL ctl_ring(void)
{
    return ctl.mode == CM_KEY;
}

/* Should they show now (the pointer set to match)? */
static BOOL ctl_want(BOOL paused)
{
    ULONG now = time_ms();
    BOOL want, ptr;
    if (ctl.moved && ctl.moved != ctl.moved_seen) {
        ctl.moved_seen = ctl.moved;
        ctl.mode = CM_MOUSE;
    }
    if (ctl.key && ctl.key != ctl.key_seen) {
        ctl.key_seen = ctl.key;
        ctl.mode = CM_KEY;
    }
    want = ctl.mode == CM_KEY || (ctl.mode == CM_MOUSE && (paused || now - ctl.moved < BAR_HIDE));
    ptr = ctl.mode == CM_MOUSE && want;
    if (ptr != ctl.ptr && ctl.win) {
        if (ptr)
            ClearPointer(ctl.win);
        else if (ctl.blank)
            SetPointer(ctl.win, ctl.blank, 1, 16, 0, 0);
        ctl.ptr = ptr;
    }
    if (!want && ctl.shown)
        ctl.focus = 1;              /* BT_PLAY */
    ctl.shown = want;
    return want;
}

/* Its pens: the darkest and the lightest of the colours it may use (HAM:
 * the base ones; extra halfbrite: the first 32), one between for lines. */
static void bar_pens(const ULONG *tab, ULONG ncol, ULONG camg, UBYTE depth)
{
    ULONG i, n = ncol, lo = 0xFFFFFFFF, hi = 0, best = 0xFFFFFFFF, l[256];
    if (camg & HAM_KEY)
        n = depth == 8 ? 64 : 16;
    if (n > 256)
        n = 256;
    for (i = 0; i < n; i++) {
        ULONG r = tab[1 + i * 3] >> 24, g = tab[2 + i * 3] >> 24, b = tab[3 + i * 3] >> 24;
        l[i] = r * 3 + g * 6 + b;
        if (l[i] < lo) {
            lo = l[i];
            bar.bg = (UBYTE)i;
        }
        if (l[i] >= hi) {
            hi = l[i];
            bar.fg = (UBYTE)i;
        }
    }
    bar.dim = bar.fg;
    for (i = 0; i < n; i++) {
        ULONG d = l[i] > (lo + hi) / 2 ? l[i] - (lo + hi) / 2 : (lo + hi) / 2 - l[i];
        if (i != bar.bg && d < best) {
            best = d;
            bar.dim = (UBYTE)i;
        }
    }
}

static void bar_setup(struct BitMap *friend, UWORD w, UWORD h, UBYTE depth, ULONG id,
                      const struct Rectangle *clip, WORD left, WORD top)
{
    static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
    WORD vis_bottom = clip->MaxY - top + 1;
    memset(&bar, 0, sizeof(bar));
    if (!(bar.font = OpenFont(&topaz8)))
        return;
    bar.h = (id & LACE) ? 24 : 14;
    bar.x0 = clip->MinX - left > 0 ? clip->MinX - left : 0;
    bar.x1 = clip->MaxX - left < (WORD)w - 1 ? clip->MaxX - left : (WORD)w - 1;
    bar.y = (vis_bottom < (WORD)h ? vis_bottom : (WORD)h) - bar.h;
    if (bar.y < 0 || bar.x1 - bar.x0 < 160)
        return;
    bar.cw = bar.h + 4;
    if (!(bar.save[0] = AllocBitMap(w, bar.h, depth, 0, friend)) ||
        !(bar.save[1] = AllocBitMap(w, bar.h, depth, 0, friend)))
        return;
    InitRastPort(&bar.rp);
    SetFont(&bar.rp, bar.font);
    SetDrMd(&bar.rp, JAM1);
    bar.ok = TRUE;
}

static void bar_cleanup(void)
{
    int k;
    WaitBlit();
    for (k = 0; k < 2; k++)
        if (bar.save[k])
            FreeBitMap(bar.save[k]);
    if (bar.font)
        CloseFont(bar.font);
    memset(&bar, 0, sizeof(bar));
}

static void bar_icon(int b, WORD x, WORD y, WORD s)
{
    struct RastPort *rp = &bar.rp;
    WORD cy = y + s / 2, i, t = s > 12 ? 2 : 1;
    SetAPen(rp, bar.fg);
    switch (b) {
    case BT_RESTART:                        /* |< */
        RectFill(rp, x + s / 6, y + s / 6, x + s / 6 + t, y + s - s / 6);
        for (i = 0; i <= s / 3; i++) {
            Move(rp, x + s / 6 + t + 2 + i, cy - i);
            Draw(rp, x + s / 6 + t + 2 + i, cy + i);
        }
        break;
    case BT_PLAY:
        if (bar.paused)                     /* > */
            for (i = 0; i <= s / 3; i++) {
                Move(rp, x + s / 3 + i, y + s / 6 + i);
                Draw(rp, x + s / 3 + i, y + s - s / 6 - i);
            }
        else {                              /* || */
            RectFill(rp, x + s / 4, y + s / 6, x + s / 4 + t, y + s - s / 6);
            RectFill(rp, x + s - s / 4 - t, y + s / 6, x + s - s / 4, y + s - s / 6);
        }
        break;
    case BT_STOP:
        RectFill(rp, x + s / 4, y + s / 4, x + s - s / 4, y + s - s / 4);
        break;
    case BT_VDOWN:
    case BT_VUP:
        RectFill(rp, x + s / 4, cy - t / 2, x + s - s / 4, cy - t / 2 + t - 1);
        if (b == BT_VUP)
            RectFill(rp, x + s / 2 - t / 2, y + s / 4, x + s / 2 - t / 2 + t - 1, y + s - s / 4);
        break;
    }
}

static void time_text(char *out, ULONG ms)
{
    ULONG s = ms / 1000;
    sprintf(out, "%ld:%02ld", (long)(s / 60), (long)(s % 60));
}

/* Drawn into buffer k (what it covers kept first). */
static void bar_put(struct BitMap *bm, int k)
{
    struct RastPort *rp = &bar.rp;
    char t[48], a[12], b[12];
    WORD x, s = bar.h - 4, ty;
    int i;
    if (!bar.on || !bar.ok)
        return;
    if (!bar.saved[k]) {
        BltBitMap(bm, 0, bar.y, bar.save[k], 0, 0, bar.x1 + 1, bar.h, 0xC0, 0xFF, NULL);
        bar.saved[k] = TRUE;
    }
    rp->BitMap = bm;
    SetAPen(rp, bar.bg);
    RectFill(rp, bar.x0, bar.y, bar.x1, bar.y + bar.h - 1);
    SetAPen(rp, bar.dim);
    RectFill(rp, bar.x0, bar.y, bar.x1, bar.y);
    ty = bar.y + (bar.h - bar.font->tf_YSize) / 2 + bar.font->tf_Baseline;
    for (i = 0, x = bar.x0 + 4; i < BT_FULL; i++, x += bar.cw) {
        WORD ix;
        if (btn_hidden(i)) {
            bar.bx[i] = -32000;
            x -= bar.cw;
            continue;
        }
        if (i == BT_VUP) {                  /* the volume between - and + */
            WORD vw = TextLength(rp, (STRPTR)"100%", 4) + 4;
            sprintf(t, "%ld%%", (long)(mp_volume() * 100 / 64));
            SetAPen(rp, bar.fg);
            Move(rp, x + (vw - TextLength(rp, (STRPTR)t, strlen(t))) / 2, ty);
            Text(rp, (STRPTR)t, strlen(t));
            x += vw;
        }
        bar.bx[i] = x;
        ix = x + (bar.cw - s) / 2 - 2;
        bar_icon(i, ix, bar.y + 2, s);
        if (i == ctl.focus && ctl_ring()) {     /* the focus: a ring, its corners off */
            WORD x0 = ix - 2, y0 = bar.y + 1, x1 = ix + s + 1, y1 = bar.y + bar.h - 1;
            SetAPen(rp, bar.fg);
            RectFill(rp, x0 + 1, y0, x1 - 1, y0);
            RectFill(rp, x0 + 1, y1, x1 - 1, y1);
            RectFill(rp, x0, y0 + 1, x0, y1 - 1);
            RectFill(rp, x1, y0 + 1, x1, y1 - 1);
        }
    }
    time_text(a, bar.at_ms);
    if (bar.total_ms) {
        time_text(b, bar.total_ms);
        sprintf(t, "%s / %s", a, b);
    } else
        strcpy(t, a);
    x += 6;
    SetAPen(rp, bar.fg);
    Move(rp, x, ty);
    Text(rp, (STRPTR)t, strlen(t));
    x += TextLength(rp, (STRPTR)t, strlen(t)) + 8;
    bar.px0 = x;
    bar.px1 = bar.x1 - 6;
    if (bar.px1 - bar.px0 > 16) {           /* how far: a line, filled so far */
        WORD my = bar.y + bar.h / 2, fill = bar.px0;
        if (bar.total_ms)
            fill = bar.px0 + (WORD)((bar.at_ms > bar.total_ms ? bar.total_ms : bar.at_ms) *
                                    (ULONG)(bar.px1 - bar.px0) / bar.total_ms);
        SetAPen(rp, bar.dim);
        RectFill(rp, bar.px0, my - 1, bar.px1, my);
        SetAPen(rp, bar.fg);
        if (fill > bar.px0)
            RectFill(rp, bar.px0, my - 1, fill, my);
        RectFill(rp, fill - 1, my - 3, fill + 1, my + 2);
    }
}

/* What it covers back in buffer k. */
static void bar_take(struct BitMap *bm, int k)
{
    if (!bar.saved[k])
        return;
    BltBitMap(bar.save[k], 0, 0, bm, 0, bar.y, bar.x1 + 1, bar.h, 0xC0, 0xFF, NULL);
    WaitBlit();
    bar.saved[k] = FALSE;
}

/* The button at (x, y), -1 none (or not on the bar). */
static int bar_hit(WORD x, WORD y)
{
    int i;
    if (!bar.on || y < bar.y || y >= bar.y + bar.h)
        return -1;
    for (i = 0; i < BT_FULL; i++)
        if (x >= bar.bx[i] && x < bar.bx[i] + bar.cw)
            return i;
    return -1;
}

/* All the frames' time: kept from before (ilbm.c), or a header each read
 * (from where fh is, put back) and kept. */
static ULONG anim_length(BPTR fh, const char *path, ULONG first)
{
    LONG pos;
    UBYTE h[52];
    ULONG jif = first, frames = 0, kf, kms;
    if (anim_fact_get(fh, path, &kf, &kms))
        return kms;
    rd_sync();                              /* (it reads the file itself) */
    pos = Seek(fh, 0, OFFSET_CURRENT);
    while (frames < 20000 && Read(fh, h, 12) == 12 && be32(h) == ID('F', 'O', 'R', 'M')) {
        LONG len = be32(h + 4) - 4, got = 0;
        if (len < 0)
            break;
        if (be32(h + 8) == ID('I', 'L', 'B', 'M')) {
            frames++;
            if ((got = Read(fh, h, len < (LONG)sizeof(h) ? len : (LONG)sizeof(h))) < 0)
                break;
            if (got >= 8 + 18 && be32(h) == ID('A', 'N', 'H', 'D'))
                jif += be32(h + 8 + 14) ? be32(h + 8 + 14) : 1;
        }
        if (Seek(fh, len - got + (len & 1), OFFSET_CURRENT) < 0)
            break;
    }
    Seek(fh, pos, OFFSET_BEGINNING);
    anim_fact_put(fh, path, frames + 1, jif * 1000 / 60);
    return jif * 1000 / 60;
}

#define VOL_STEP    4           /* of 64, as Now playing's */
#define VOL_DELAY 300           /* ms the stick is held before it repeats */
#define VOL_REPEAT 100          /* ...and then a step every */

static BOOL bar_redraw;         /* the controls to be drawn again (the volume changed) */

static void volume(int d)
{
    int v = mp_volume() + d;
    mp_set_volume(v < 0 ? 0 : v > 64 ? 64 : v);
    bar_redraw = TRUE;
}

/* A button pressed: 1 stop, 2 pause or go on, 3 from the start, 4 full
 * screen or not, 0 else. */
static int ctl_press(int b)
{
    if (b >= 0 && (ctl.off & (1 << b)))
        return 0;                           /* greyed */
    switch (b) {
    case BT_RESTART:
        return 3;
    case BT_PLAY:
        return 2;
    case BT_STOP:
        return 1;
    case BT_VDOWN:
        volume(-VOL_STEP);
        break;
    case BT_VUP:
        volume(VOL_STEP);
        break;
    case BT_FULL:
        return 4;
    }
    return 0;
}

/* Left or right a button (only once they show: the first press brings
 * them). */
static void ctl_move(int d)
{
    if (ctl.shown && ctl.mode == CM_KEY) {
        int f = ctl.focus + d;
        while (f >= 0 && f < ctl.nbtn && ((ctl.off & (1 << f)) || btn_hidden(f)))
            f += d;                         /* greyed or not shown: passed over */
        if (f >= 0 && f < ctl.nbtn)
            ctl.focus = f;
    }
    bar_redraw = TRUE;
}

/* Return or fire: the button in focus, or (hidden) play/pause. */
static int ctl_enter(void)
{
    return ctl_press(ctl.shown && ctl.mode == CM_KEY ? ctl.focus : BT_PLAY);
}

/* Input while it plays: 1 stop, 2 pause or go on, 3 from the start, 4
 * full screen or not (AmigaMCE's screen), 0 nothing; the volume set here, the mouse's moves noted (the controls).
 * hit: the button at a click (BT_*), -1 none (the picture: pause), -2 on
 * the controls but no button. */
static struct Gfx *anim_g;      /* AmigaMCE's screen or window, while playing on it */

static int input_with(struct Window *win, ULONG *jlast, int (*hit)(WORD, WORD))
{
    static ULONG held_since, held_step;
    struct IntuiMessage *im;
    int r = 0;
    while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        ULONG cl = im->Class;
        UWORD code = im->Code;
        WORD mx = im->MouseX - win->BorderLeft, my = im->MouseY - win->BorderTop;
        ReplyMsg((struct Message *)im);
        if (anim_g && win == anim_g->win && gfx_window_msg(anim_g, cl)) {
            if (gfx_close_asked)
                r = 1;                      /* the window's close gadget: out */
            continue;
        }
        if ((cl == IDCMP_MOUSEMOVE || cl == IDCMP_MOUSEBUTTONS ||
             (cl == IDCMP_RAWKEY && (code == 0x7A || code == 0x7B))) && time_ms() - ctl.opened > 500)
            ctl.moved = time_ms() ? time_ms() : 1;  /* (not the window's own first) */
        else if ((cl == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX) && code != 0x7A && code != 0x7B) ||
                 cl == IDCMP_VANILLAKEY)
            ctl.key = time_ms() ? time_ms() : 1;
        if (cl == IDCMP_RAWKEY) {
            if (code == 0x45 || code == 0x41 || code == 0x10)   /* Esc, Backspace, Q */
                r = 1;
            else if (code == 0x40 && !r)            /* Space */
                r = 2;
            else if ((code == 0x44 || code == 0x43) && !r)  /* Return, Enter */
                r = ctl_enter();
            else if (code == 0x23 && !r)            /* F: full screen or not */
                r = ctl_press(BT_FULL);
            else if (code == 0x4F)                  /* Left, Right: the focus */
                ctl_move(-1);
            else if (code == 0x4E)
                ctl_move(1);
            else if ((code == 0x4C || code == 0x7A) && nsnd)    /* Up (held: it repeats), */
                volume(VOL_STEP);                               /* the wheel: its sound's */
            else if ((code == 0x4D || code == 0x7B) && nsnd)
                volume(-VOL_STEP);
        } else if (cl == IDCMP_VANILLAKEY) {   /* AmigaMCE's own window has these too */
            if (code == 27 || code == 8 || code == 'q' || code == 'Q')
                r = 1;
            else if (code == ' ' && !r)
                r = 2;
            else if (code == 13 && !r)
                r = ctl_enter();
            else if ((code == 'f' || code == 'F') && !r)
                r = ctl_press(BT_FULL);
        } else if (cl == IDCMP_MOUSEBUTTONS) {
            if (code == MENUDOWN)
                r = 1;
            else if (code == SELECTDOWN) {
                int b = hit(mx, my);
                if (b >= 0) {
                    int k = ctl_press(b);
                    if (k && !r)
                        r = k;
                } else if (b == -1 && !r)
                    r = 2;                  /* anywhere off the controls: play/pause */
            }
        }
    }
    if (LowLevelBase) {
        ULONG st = anim_g && win == anim_g->win ? gfx_joy(anim_g) : ReadJoyPort(1);
        ULONG pressed = st & ~*jlast;
        if ((st & JP_TYPE_MASK) == JP_TYPE_JOYSTK || (st & JP_TYPE_MASK) == JP_TYPE_GAMECTLR) {
            if (pressed || (st & (JPF_JOY_UP | JPF_JOY_DOWN)))
                ctl.key = time_ms() ? time_ms() : 1;
            if (pressed & JPF_BUTTON_BLUE)
                r = 1;
            else if ((pressed & JPF_BUTTON_PLAY) && !r)
                r = 2;
            else if ((pressed & JPF_BUTTON_RED) && !r)
                r = ctl_enter();
            if (pressed & JPF_JOY_LEFT)
                ctl_move(-1);
            else if (pressed & JPF_JOY_RIGHT)
                ctl_move(1);
            if (st & (JPF_JOY_UP | JPF_JOY_DOWN)) {
                ULONG now = time_ms();
                int d = st & JPF_JOY_UP ? VOL_STEP : -VOL_STEP;
                if (pressed & (JPF_JOY_UP | JPF_JOY_DOWN)) {
                    volume(d);
                    held_since = held_step = now;
                } else if (now - held_since >= VOL_DELAY && now - held_step >= VOL_REPEAT) {
                    volume(d);
                    held_step = now;
                }
            }
        }
        *jlast = st;
    }
    return r;
}

/* The controls over the picture, on its own screen */
static int classic_hit(WORD x, WORD y)
{
    int b = bar_hit(x, y);
    return b < 0 && bar.on && y >= bar.y ? -2 : b;
}

static int input(struct Window *win, ULONG *jlast)
{
    return input_with(win, jlast, classic_hit);
}

/* 8SVX samples in it: the FORM after its header one (they come first). */
BOOL anim_has_sound(const char *path)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    UBYTE h[24];
    BOOL r = FALSE;
    BPTR fh;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return FALSE;
    r = Read(fh, h, 24) == 24 && be32(h) == ID('F', 'O', 'R', 'M') &&
        be32(h + 8) == ID('A', 'N', 'I', 'M') && be32(h + 12) == ID('F', 'O', 'R', 'M') &&
        be32(h + 20) == ID('8', 'S', 'V', 'X');
    Close(fh);
    return r;
}

/* ---- playing on AmigaMCE's own screen (RTG, 16-bit): the frames decoded
 * into bitplanes in ordinary memory, as on a screen of their own, each
 * then made chunky (a byte a pixel) and drawn by the graphics card, its
 * colours from the frame's colour table (WriteLUTPixelArray), HAM made
 * into RGB pixel by pixel, fringes and all (WritePixelArray). Scaled by
 * whole numbers to fit, its pixels' shape kept (lowres twice as wide as
 * high-res, non-laced twice as tall as laced); the controls beneath it,
 * AmigaMCE's own. No mode switch, and no colours shared with anything.
 *
 * Where the screen's memory can be had (LockBitMapTags, a 15 or 16-bit
 * format), the pixels are written straight into it, and only where the
 * frame differs from the one shown: a row at a time, from its first
 * changed byte column to its last (HAM's whole row, each pixel hanging on
 * the one before). Behind time, frames are decoded and not drawn, so it
 * keeps its speed and its sound in step: as many as it takes, one drawn
 * at least every SKIP_MS.
 *
 * Full screen too slow (over three times what it wants, over its first
 * SLOW_FRAMES drawn), it goes back to its whole scale for that one. It
 * never goes to a screen of its own for being slow: on Amiberry the
 * change from RTG to the Amiga's own display stops everything a long
 * while; Settings > Display has that way for those who want it. */
#define SLOW_FRAMES 12
#define SLOW_MS     50
#define SKIP_MS    250

/* Where the picture goes on AmigaMCE's screen: dw x dh at vx, vy; source
 * pixel x at xo[x] (to xo[x + 1]), row y at yo[y] (to yo[y + 1]); fx, fy
 * the whole scales, or 0. Full: as large as the screen allows, its shape
 * kept, under the controls when they show; else the largest whole scale
 * above them. */
static struct {
    WORD dw, dh, vx, vy, fx, fy;
    UWORD *xo, *yo;
    BOOL full, can_full;
    struct BitMap *pbm;             /* the picture, drawn here, then blitted onto the screen */
    void (*reblit)(struct Gfx *, WORD, WORD);   /* the picture's rows again (the card's way) */
} lay;
static BOOL anim_full;          /* full screen chosen (for this run) */

/* The controls beneath the picture */
static struct {
    WORD y, h, x0, cw, px0, px1, cy, fill;
    ULONG sec;
    UWORD vol;
    BOOL paused, bar, ring, full;
    int focus;
    UWORD off;
    WORD bx[BT_N];                  /* each button's left */
} st;

static void strip_draw(struct Gfx *g, BOOL paused, ULONG at, ULONG total, const char *name)
{
    WORD x, s = st.h - GU(g, 6), cy = st.y + st.h / 2, t = s > 14 ? 2 : 1, i, xt = -1;
    char a[16], b[16], line[64];
    gfx_fill(g, 0, st.y, g->w - 1, st.y + st.h - 1, PEN_BG);
    st.ring = ctl_ring();
    st.focus = ctl.focus;
    st.full = lay.full;
    st.off = ctl.off;
    for (i = 0, x = st.x0; i < ctl.nbtn; i++, x += st.cw) {
        WORD ix, iy = st.y + (st.h - s) / 2, j, bx, by, bw, bh, ry, sx, rx;
        if (btn_hidden(i)) {
            st.bx[i] = -32000;              /* (never hit) */
            x -= st.cw;
            continue;
        }
        if (i == BT_FULL) {                 /* full screen: at the right end */
            xt = x;
            x = g->w - GU(g, 8) - st.cw;
        }
        if (i == BT_VUP) {                  /* the volume between - and + */
            WORD vw = gfx_textlen(g, "100%", TS_BODY) + GU(g, 8);
            sprintf(line, "%ld%%", (long)(mp_volume() * 100 / 64));
            gfx_text(g, x + (vw - gfx_textlen(g, line, TS_BODY)) / 2, cy - g->font_h / 2, line,
                     PEN_TEXT, PEN_BG, 0, TS_BODY);
            x += vw;
        }
        st.bx[i] = x;
        ix = x + (st.cw - s) / 2;
        bx = x + 1, by = st.y + 2, bw = st.cw - 2, bh = st.h - 4;
        ry = bh / 5 > 2 ? bh / 5 : 2;
        sx = gfx_mode_tall(g->mode) ? 2 : 1;
        if (sx > 1)
            ry = ry / 2 > 1 ? ry / 2 : 1;
        rx = ry * sx;
        if (st.ring && i == st.focus && !(st.off & (1 << i))) {    /* the focus: a yellow ring */
            WORD ft = GU(g, 2) > 1 ? GU(g, 2) : 1, fx = ft * sx;
            gfx_rrect(g, bx, by, bw, bh, rx, ry, PEN_YELLOW);
            gfx_rrect(g, bx + fx, by + ft, bw - 2 * fx, bh - 2 * ft, rx > fx ? rx - fx : 1,
                      ry > ft ? ry - ft : 1, PEN_BAR);
        } else
            gfx_rrect(g, bx, by, bw, bh, rx, ry, PEN_BAR);
        SetAPen(g->rp, GPEN(st.off & (1 << i) ? PEN_RULE : PEN_WHITE));    /* greyed, as Now playing's */
        switch (i) {
        case BT_RESTART:
            RectFill(g->rp, ix + s / 5, iy + s / 5, ix + s / 5 + t, iy + s - s / 5);
            for (j = 0; j <= s / 3; j++) {
                Move(g->rp, ix + s / 5 + t + 2 + j, cy - j);
                Draw(g->rp, ix + s / 5 + t + 2 + j, cy + j);
            }
            break;
        case BT_PLAY:
            if (paused)
                for (j = 0; j <= s / 3; j++) {
                    Move(g->rp, ix + s / 3 + j, iy + s / 6 + j);
                    Draw(g->rp, ix + s / 3 + j, iy + s - s / 6 - j);
                }
            else {
                RectFill(g->rp, ix + s / 4, iy + s / 6, ix + s / 4 + t, iy + s - s / 6);
                RectFill(g->rp, ix + s - s / 4 - t, iy + s / 6, ix + s - s / 4, iy + s - s / 6);
            }
            break;
        case BT_STOP:
            RectFill(g->rp, ix + s / 4, iy + s / 4, ix + s - s / 4, iy + s - s / 4);
            break;
        case BT_FULL: {                     /* four corners: out to fill, in to come back */
            WORD a = s / 5, L = s / 4, c, ccx = ix + s / 2, ccy = iy + s / 2, gap = s / 8 + t;
            for (c = 0; c < 4; c++) {
                WORD dx = c & 1 ? -1 : 1, dy = c & 2 ? -1 : 1, px, py;
                if (!lay.full && !gfx_fs_on()) { /* at the box's corners, arms along its edges */
                    px = dx > 0 ? ix + a : ix + s - 1 - a;
                    py = dy > 0 ? iy + a : iy + s - 1 - a;
                } else {                    /* by the middle, arms outwards */
                    px = ccx - dx * gap;
                    py = ccy - dy * gap;
                    dx = -dx;
                    dy = -dy;
                }
                RectFill(g->rp, dx > 0 ? px : px - L + 1, dy > 0 ? py : py - t + 1,
                         dx > 0 ? px + L - 1 : px, dy > 0 ? py + t - 1 : py);
                RectFill(g->rp, dx > 0 ? px : px - t + 1, dy > 0 ? py : py - L + 1,
                         dx > 0 ? px + t - 1 : px, dy > 0 ? py + L - 1 : py);
            }
            break;
        }
        default:
            RectFill(g->rp, ix + s / 4, cy - t / 2, ix + s - s / 4, cy - t / 2 + t - 1);
            if (i == BT_VUP)
                RectFill(g->rp, ix + s / 2 - t / 2, iy + s / 4, ix + s / 2 - t / 2 + t - 1,
                         iy + s - s / 4);
            break;
        }
    }
    if (xt >= 0)
        x = xt;
    time_text(a, at);
    if (total) {
        time_text(b, total);
        sprintf(line, "%s / %s", a, b);
    } else
        strcpy(line, a);
    x += GU(g, 10);
    gfx_text(g, x, cy - g->font_h / 2, line, PEN_TEXT, PEN_BG, 0, TS_BODY);
    x += gfx_textlen(g, line, TS_BODY) + GU(g, 8);
    st.px0 = x;
    st.px1 = (xt >= 0 ? st.bx[BT_FULL] : g->w) - 1 - GU(g, 8);
    st.cy = cy;
    st.sec = at / 1000;
    st.vol = mp_volume();
    st.paused = paused;
    st.bar = FALSE;
    st.fill = -1;
    if (st.px1 - st.px0 > GU(g, 40)) {          /* the name, then how far it has got */
        WORD nw = gfx_textlen(g, name, TS_SMALL), pw;
        if (nw > (st.px1 - st.px0) / 3)
            nw = (st.px1 - st.px0) / 3;
        gfx_text(g, st.px1 - nw, cy - g->font_h / 2, name, PEN_DIM, PEN_BG, nw, TS_SMALL);
        st.px1 -= nw + GU(g, 8);
        pw = st.px1 - st.px0;
        if (pw > GU(g, 16)) {
            WORD fill = st.px0 + (total ? (WORD)((at > total ? total : at) * (ULONG)pw / total) : 0);
            gfx_fill(g, st.px0, cy - 1, st.px1, cy + 1, PEN_RULE);
            if (fill > st.px0)
                gfx_fill(g, st.px0, cy - 1, fill, cy + 1, PEN_WHITE);
            gfx_fill(g, fill - 2, cy - 4, fill + 2, cy + 4, PEN_WHITE);
            st.bar = TRUE;
            st.fill = fill;
        }
    }
}

/* How far it has got, drawn again if it has moved a pixel: TRUE if so. */
static BOOL strip_progress(struct Gfx *g, ULONG at, ULONG total)
{
    WORD fill, cy = st.cy;
    if (!st.bar || !total)
        return FALSE;
    fill = st.px0 + (WORD)((at > total ? total : at) * (ULONG)(st.px1 - st.px0) / total);
    if (fill == st.fill)
        return FALSE;
    gfx_fill(g, st.px0 - 2, cy - 4, st.px1 + 2, cy + 4, PEN_BG);
    gfx_fill(g, st.px0, cy - 1, st.px1, cy + 1, PEN_RULE);
    if (fill > st.px0)
        gfx_fill(g, st.px0, cy - 1, fill, cy + 1, PEN_WHITE);
    gfx_fill(g, fill - 2, cy - 4, fill + 2, cy + 4, PEN_WHITE);
    st.fill = fill;
    return TRUE;
}

/* The controls beneath the picture, when ctl_want says: drawn into a
 * bitmap of their own (sf.bm) and put on the screen from there, at a
 * level of brightness while fading (FADE_N steps over FADE_MS); their
 * text, the costly part, drawn again once a second, or paused, the volume
 * or the focus changed, and only while they show. Without that bitmap,
 * straight onto the screen, and no fading. */
#define FADE_MS 240
#define FADE_N  8
#define FADE_ON 0               /* off: each step's ReadPixelArray and RGB write made the
                                 * frames uneven on Amiberry; they come and go at once */
static struct {
    struct BitMap *bm;
    struct RastPort rp;
    UBYTE *rgb, *tmp;               /* its pixels, and at a level */
    UWORD *blank;                   /* an empty pointer */
    int lv;
    BOOL drawn, rgb_ok;
    ULONG step_at;
} sf;

/* The picture's x0-x1, y0-y1 (one past) onto the screen from its bitmap,
 * not over the controls while they show. */
static void pic_blit(struct Gfx *g, WORD x0, WORD y0, WORD x1, WORD y1)
{
    if (!lay.pbm)
        return;
    if (sf.lv > 0 && lay.vy + y1 > st.y)
        y1 = st.y - lay.vy;
    if (x1 > x0 && y1 > y0)
        BltBitMapRastPort(lay.pbm, x0, y0, g->rp, lay.vx + x0, lay.vy + y0, x1 - x0, y1 - y0, 0xC0);
}

/* The controls gone: black there, and the picture where it reaches. */
static void strip_clear(struct Gfx *g)
{
    FillPixelArray(g->rp, 0, st.y, g->w, st.h, 0x000000);
    if (lay.vy + lay.dh > st.y) {
        if (lay.reblit)
            lay.reblit(g, st.y - lay.vy, lay.dh);
        else
            pic_blit(g, 0, st.y - lay.vy, lay.dw, lay.dh);
    }
}

static void strip_render(struct Gfx *g, BOOL full, BOOL paused, ULONG at, ULONG total,
                         const char *name, BOOL *changed)
{
    struct RastPort *keep = g->rp;
    WORD y = st.y;
    if (sf.bm) {
        g->rp = &sf.rp;
        st.y = 0;
    }
    if (full) {
        strip_draw(g, paused, at, total, name);
        *changed = TRUE;
    } else if (strip_progress(g, at, total))
        *changed = TRUE;
    g->rp = keep;
    st.y = y;
    if (*changed)
        sf.rgb_ok = FALSE;
}

static void strip_show(struct Gfx *g)
{
    if (!sf.bm) {
        if (sf.lv <= 0)
            strip_clear(g);
        return;                             /* else drawn already */
    }
    if (sf.lv >= FADE_N)
        BltBitMapRastPort(sf.bm, 0, 0, g->rp, 0, st.y, g->w, st.h, 0xC0);
    else if (sf.lv <= 0)
        strip_clear(g);
    else {
        UBYTE t[256];
        ULONG i, n = (ULONG)g->w * st.h * 3, k = (ULONG)sf.lv * sf.lv * 256 / (FADE_N * FADE_N);
        if (!sf.rgb_ok) {
            ReadPixelArray(sf.rgb, 0, 0, g->w * 3, &sf.rp, 0, 0, g->w, st.h, RECTFMT_RGB);
            sf.rgb_ok = TRUE;
        }
        for (i = 0; i < 256; i++)
            t[i] = (UBYTE)(i * k >> 8);
        for (i = 0; i < n; i++)
            sf.tmp[i] = t[sf.rgb[i]];
        WritePixelArray(sf.tmp, 0, 0, g->w * 3, g->rp, 0, st.y, g->w, st.h, RECTFMT_RGB);
    }
}

/* TRUE if the controls' place was drawn (a window copies it in then). */
static BOOL strip_tick(struct Gfx *g, BOOL paused, ULONG at, ULONG total, const char *name)
{
    ULONG now = time_ms();
    int target = ctl_want(paused) ? FADE_N : 0, lv = sf.lv;
    BOOL changed = FALSE, drew = FALSE;
    if (lv != target) {
        if (!sf.bm || !FADE_ON)
            lv = target;
        else if (now - sf.step_at >= FADE_MS / FADE_N) {
            lv += lv < target ? 1 : -1;
            sf.step_at = now;
        }
    }
    if (lv > 0 || target > 0) {
        if (!sf.drawn || at / 1000 != st.sec || paused != st.paused || mp_volume() != st.vol ||
            ctl_ring() != st.ring || ctl.focus != st.focus || lay.full != st.full ||
            ctl.off != st.off) {
            strip_render(g, TRUE, paused, at, total, name, &changed);
            sf.drawn = TRUE;
        } else
            strip_render(g, FALSE, paused, at, total, name, &changed);
    }
    if (changed || lv != sf.lv) {
        sf.lv = lv;
        if (!sf.bm && !lv && !changed)
            strip_show(g);
        else if (sf.bm)
            strip_show(g);
        drew = TRUE;
    }
    if (!sf.lv)
        sf.drawn = FALSE;                   /* drawn afresh when it next shows */
    return drew;
}

/* A window: the frame (vx..: the picture's place) and the controls' place,
 * if drawn, copied in; only those, not the whole window: a pointer the
 * card draws itself (PrismRTG's) is taken off for each copy under it, and
 * copied 50 times a second it was not seen at all. */
static void win_show(struct Gfx *g, BOOL frame, BOOL strip, WORD vx, WORD vy, WORD dw, WORD dh)
{
    if (!g->wb)
        return;
    if (frame)
        gfx_window_show_rect(g, vx, vy, vx + dw - 1, vy + dh - 1);
    if (strip)
        gfx_window_show_rect(g, 0, st.y, g->w - 1, g->h - 1);
}

static void strip_setup(struct Gfx *g)
{
    memset(&sf, 0, sizeof(sf));
    sf.blank = AllocMem(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
    if ((sf.bm = AllocBitMap(g->w, st.h, GetBitMapAttr(g->rp->BitMap, BMA_DEPTH), BMF_MINPLANES,
                             g->rp->BitMap)) &&
        (sf.rgb = AllocVec((ULONG)g->w * st.h * 3, MEMF_ANY)) &&
        (sf.tmp = AllocVec((ULONG)g->w * st.h * 3, MEMF_ANY))) {
        InitRastPort(&sf.rp);
        sf.rp.BitMap = sf.bm;
        SetFont(&sf.rp, g->rp->Font);
        SetDrMd(&sf.rp, JAM1);
    } else {
        if (sf.bm)
            FreeBitMap(sf.bm);
        FreeVec(sf.rgb);
        sf.bm = NULL;
        sf.rgb = NULL;
    }
    ctl_begin(g->win, sf.blank);
}

static void strip_cleanup(struct Gfx *g)
{
    ClearPointer(g->win);
    WaitBlit();
    if (sf.bm)
        FreeBitMap(sf.bm);
    FreeVec(sf.rgb);
    FreeVec(sf.tmp);
    if (sf.blank)
        FreeMem(sf.blank, 6 * sizeof(UWORD));
    memset(&sf, 0, sizeof(sf));
}

static int strip_hit(WORD x, WORD y)
{
    int i;
    if (y < st.y)
        return -1;                          /* the picture: pauses */
    if (!ctl.shown)
        return -1;                          /* hidden: anywhere pauses */
    for (i = 0; i < ctl.nbtn; i++)
        if (x >= st.bx[i] && x < st.bx[i] + st.cw)
            return i;
    return -2;                              /* the controls, no button */
}

/* ---- frames drawn by the graphics card: a colour-table animation's planes
 * handed to it as a bitmap, BltBitMap converting them to the screen's
 * pixels through the screen's colours (Picasso96's BlitPlanar2Direct; on
 * Amiberry, PrismRTG passes it to the emulator, which does it on the PC:
 * 0.2 ms a 384 x 290 frame, against 36 ms on the 68k). The animation's
 * colours are loaded for the blit and the screen's put back after it.
 *   CARD_PLANES   at 1:1, the planes as they are (the decoder's buffer)
 *   CARD_STRETCH  at any other scale, the changed rows' planes stretched
 *                 first, in fast RAM: a byte's 8 pixels as their run of
 *                 bits (each pixel as many times as its columns, lay.xo),
 *                 a table a byte column made for the size, or'd in at the
 *                 column's place; a row as many times as lay.yo says
 * (Stretching the pixels instead, in the card's memory, cost 55 ms a
 * frame full screen: the CPU writes card memory at some 45 MB a second.)
 * Stretching costs a plane at a time, so with many planes it can cost
 * more than animdraw's 68k drawing the pixels (Frog, 8 planes at 2:1:
 * 66 ms against 20): a stretched animation's first CARD_TRIAL drawn
 * frames are the card's, the next CARD_TRIAL the 68k's, each timed by the
 * row, and the quicker draws the rest.
 * HAM is not the card's: drawn by animdraw as before. The first frame is
 * read back and checked against the colours it should have; any other,
 * and the CPU draws it. */
enum { CARD_OFF, CARD_PLANES, CARD_STRETCH };
static struct {
    int mode;
    UBYTE depth;
    UWORD w, h;
    ULONG rb;                       /* the decoder's bytes a plane row */
    struct BitMap src;              /* the frame's planes as a bitmap (rows interleaved) */
    struct BitMap xb;               /* stretched (CARD_STRETCH) */
    UBYTE *xbuf;
    ULONG rbx, xsize;               /* its bytes a plane row; all of it */
    ULONG *ct;                      /* a byte column's 256 bit runs (left-aligned) */
    ULONG load[256 * 3 + 2];        /* LoadRGB32's: the animation's colours */
    ULONG save[256 * 3 + 2];        /* ...and the screen's, to put back */
    int ncol;
    struct ViewPort *vp;
    BOOL checked;                   /* the first frame's colours looked at */
    int trial;                      /* CARD_STRETCH: 0 the card timed, 1 the 68k, 2 chosen */
    ULONG tframes, tms[2], trows[2];
} cd;
#define CARD_TRIAL 8

static void card_free(void)
{
    WaitBlit();
    FreeVec(cd.xbuf);
    cd.xbuf = NULL;
    FreeVec(cd.ct);
    cd.ct = NULL;
    cd.mode = CARD_OFF;
    lay.reblit = NULL;
}

/* The animation's colours (lut) for the card's blits */
static void card_colours(const ULONG *lut)
{
    int i;
    cd.load[0] = (ULONG)cd.ncol << 16;
    for (i = 0; i < cd.ncol; i++) {
        cd.load[1 + i * 3] = ((lut[i] >> 16) & 255) * 0x01010101UL;
        cd.load[2 + i * 3] = ((lut[i] >> 8) & 255) * 0x01010101UL;
        cd.load[3 + i * 3] = (lut[i] & 255) * 0x01010101UL;
    }
    cd.load[1 + cd.ncol * 3] = 0;
}

/* The frame in planes (buffer pl0) as the source bitmap */
static void card_source(const UBYTE *pl0)
{
    int p;
    for (p = 0; p < cd.depth; p++)
        cd.src.Planes[p] = (PLANEPTR)(pl0 + p * cd.rb);
}

/* A blit of planes, the animation's colours loaded for it */
static void card_blit(struct BitMap *bm, WORD sx, WORD sy, struct BitMap *to, struct RastPort *rp,
                      WORD dx, WORD dy, WORD ww, WORD hh)
{
    if (ww <= 0 || hh <= 0)
        return;
    LoadRGB32(cd.vp, cd.load);
    if (rp)
        BltBitMapRastPort(bm, sx, sy, rp, dx, dy, ww, hh, 0xC0);
    else
        BltBitMap(bm, sx, sy, to, dx, dy, ww, hh, 0xC0, 0xFF, NULL);
    WaitBlit();
    LoadRGB32(cd.vp, cd.save);
}

/* Picture rows y0-y1 (one past, the screen's scale) onto the screen from
 * the card's planes, not over the controls while they show */
static void card_reblit(struct Gfx *g, WORD y0, WORD y1)
{
    struct BitMap *bm = cd.mode == CARD_STRETCH ? &cd.xb : &cd.src;
    if (sf.lv > 0 && lay.vy + y1 > st.y)
        y1 = st.y - lay.vy;
    card_blit(bm, 0, y0, NULL, g->rp, lay.vx, lay.vy + y0, lay.dw, y1 - y0);
}

/* Planes row y (of the frame's) stretched into the bitmap's rows: each
 * plane's row cleared, each byte column's run of bits or'd in at its
 * column's place (lay.xo); what spills past a run is 0 bits */
static void card_stretch_row(const UBYTE *row, ULONG y)
{
    ULONG stride = cd.rbx * cd.depth, x, j, n = lay.yo[y + 1] - lay.yo[y], cols = (cd.w + 7) >> 3;
    UBYTE *o = cd.xbuf + (ULONG)lay.yo[y] * stride, p;
    if (!n)
        return;
    for (p = 0; p < cd.depth; p++) {
        const UBYTE *s = row + p * cd.rb;
        const ULONG *t = cd.ct;
        UBYTE *d = o + p * cd.rbx;
        memset(d, 0, cd.rbx);
        for (x = 0; x < cols; x++, t += 256) {
            ULONG v = t[s[x]], bo, sh, hi;
            UBYTE *q;
            if (!v)
                continue;
            bo = lay.xo[x * 8];
            sh = bo & 7;
            q = d + (bo >> 3);
            hi = v >> sh;
            q[0] |= (UBYTE)(hi >> 24);
            q[1] |= (UBYTE)(hi >> 16);
            q[2] |= (UBYTE)(hi >> 8);
            q[3] |= (UBYTE)hi;
            if (sh)
                q[4] |= (UBYTE)(v << (8 - sh));
        }
    }
    for (j = 1; j < n; j++)
        CopyMem(o, o + j * stride, stride);
}

/* The card's way for this layout (lay), if it can: its mode */
static int card_choose(struct Gfx *g, UWORD w, UWORD h, UBYTE depth, BOOL ham, ULONG rb)
{
    int i, b, mode = CARD_OFF;
    card_free();
    if (ham || !CyberGfxBase || !g->truecolor || !g->scr || g->wb || depth > 8 || cd.ncol == 0)
        return CARD_OFF;                    /* (a window: Workbench's palette is not ours to load) */
    cd.vp = &g->scr->ViewPort;
    cd.depth = depth;
    cd.w = w;
    cd.h = h;
    cd.rb = rb;
    InitBitMap(&cd.src, depth, w, h);
    cd.src.BytesPerRow = rb * depth;        /* rows interleaved, as decoded */
    if (lay.dw == w && lay.dh == h)
        mode = CARD_PLANES;
    else {
        ULONG cols = (w + 7) >> 3, x, most = 0;
        for (x = 0; x < w; x++)             /* the widest pixel: a byte's run fits 32 bits? */
            if ((ULONG)(lay.xo[x + 1] - lay.xo[x]) > most)
                most = lay.xo[x + 1] - lay.xo[x];
        cd.rbx = (((ULONG)lay.dw + 15) >> 4) << 1;
        cd.xsize = cd.rbx * depth * lay.dh + 16;    /* (+: a last run's spill of 0 bits) */
        if (most * 8 <= 32 && (cd.ct = AllocVec(cols * 256 * sizeof(ULONG), MEMF_ANY)) &&
            (cd.xbuf = AllocVec(cd.xsize, MEMF_ANY | MEMF_CLEAR))) {
            InitBitMap(&cd.xb, depth, lay.dw, lay.dh);
            cd.xb.BytesPerRow = cd.rbx * depth;
            for (i = 0; i < depth; i++)
                cd.xb.Planes[i] = (PLANEPTR)(cd.xbuf + i * cd.rbx);
            for (x = 0; x < cols; x++)      /* column x's runs: pixel k as its columns */
                for (b = 0; b < 256; b++) {
                    ULONG v = 0, nb = 0;
                    int k;
                    for (k = 0; k < 8; k++) {
                        ULONG px = x * 8 + k, r = px < w ? lay.xo[px + 1] - lay.xo[px] : 0, bit;
                        bit = (b >> (7 - k)) & 1;
                        while (r--) {
                            v = (v << 1) | bit;
                            nb++;
                        }
                    }
                    cd.ct[x * 256 + b] = nb ? v << (32 - nb) : 0;
                }
            mode = CARD_STRETCH;
        } else {
            FreeVec(cd.ct);
            cd.ct = NULL;
        }
    }
    cd.mode = mode;
    cd.checked = FALSE;
    cd.trial = mode == CARD_STRETCH ? 0 : 2;
    cd.tframes = 0;
    cd.tms[0] = cd.tms[1] = cd.trows[0] = cd.trows[1] = 0;
    lay.reblit = mode == CARD_PLANES || mode == CARD_STRETCH ? card_reblit : NULL;
    return mode;
}

/* The first frame drawn: do its pixels have the colours they should? A
 * sample of rows read back (from where it was drawn), against the colour
 * table, to the screen's bits. FALSE: not the card's to draw. */
static BOOL card_check(struct Gfx *g, const UBYTE *planes, const ULONG *lut)
{
    UBYTE *rgb = AllocVec((ULONG)lay.dw * 3, MEMF_ANY);
    ULONG y, x, bad = 0;
    UBYTE p;
    if (!rgb)
        return TRUE;                        /* (no memory to look: as it is) */
    for (y = 0; y < cd.h && !bad; y += cd.h / 12 + 1) {
        const UBYTE *row = planes + y * cd.rb * cd.depth;
        if (lay.yo[y + 1] == lay.yo[y] || lay.xo[cd.w] == 0)
            continue;
        if (sf.lv > 0 && lay.vy + lay.yo[y] >= st.y)
            break;                          /* (under the controls) */
        ReadPixelArray(rgb, 0, 0, lay.dw * 3, g->rp, lay.vx, lay.vy + lay.yo[y], lay.dw, 1,
                       RECTFMT_RGB);
        for (x = 0; x < cd.w; x++) {
            ULONG c = 0, want;
            const UBYTE *got = rgb + (ULONG)lay.xo[x] * 3;
            if (lay.xo[x + 1] == lay.xo[x])
                continue;
            for (p = 0; p < cd.depth; p++)
                if (row[p * cd.rb + (x >> 3)] & (0x80 >> (x & 7)))
                    c |= 1UL << p;
            want = lut[c];
            if ((((want >> 16) ^ got[0]) & 0xF0) || (((want >> 8) ^ got[1]) & 0xF0) ||
                ((want ^ got[2]) & 0xF0))
                bad++;
        }
    }
    FreeVec(rgb);
    return bad == 0;
}

/* The picture's place (lay): the largest whole scale above the controls,
 * or (full) as large as the screen allows, its shape kept; never smaller
 * than the first. In a window, one bigger than it is made smaller to fit
 * (full screen shows it as large as the display: fs_enter). FALSE if it is
 * bigger than the screen. */
static BOOL lay_out(struct Gfx *g, UWORD w, UWORD h, WORD ax, WORD ay, BOOL full)
{
    WORD avail = st.y - GU(g, 8), kk, fx, fy, dw, dh;
    ULONG i;
    kk = (g->w - GU(g, 16)) / (w * ax);
    if (avail / (h * ay) < kk)
        kk = avail / (h * ay);
    if (kk >= 1) {
        fx = ax * kk;
        fy = ay * kk;
        dw = w * fx;
        dh = h * fy;
    } else if (w <= g->w && h <= avail && !g->wb) {
        fx = fy = 1;                        /* as it is, its shape aside */
        dw = w;
        dh = h;
    } else if (g->wb) {
        /* a window: as large as it holds, its shape kept, larger or
         * smaller (fx, fy 0: lay.xo/yo say where each pixel goes, some of
         * a smaller one's nowhere) */
        LONG sw = (LONG)w * ax, sh = (LONG)h * ay, W = g->w - GU(g, 16), H = avail;
        if (sw * H <= sh * W) {
            dh = (WORD)H;
            dw = (WORD)(sw * H / sh);
        } else {
            dw = (WORD)W;
            dh = (WORD)(sh * W / sw);
        }
        if (dw < 16 || dh < 16)
            return FALSE;
        fx = fy = 0;
    } else
        return FALSE;
    lay.vx = (g->w - dw) / 2;
    lay.vy = GU(g, 4) + (avail - dh) / 2;
    lay.full = full;
    if (full) {
        LONG sw = (LONG)w * ax, sh = (LONG)h * ay, W = g->w, H = g->h, fw, fh;
        if (sw * H <= sh * W) {
            fh = H;
            fw = sw * H / sh;
        } else {
            fw = W;
            fh = sh * W / sw;
        }
        if (fw >= dw && fh >= dh) {
            dw = (WORD)fw;
            dh = (WORD)fh;
            fx = dw % w ? 0 : dw / w;
            fy = dh % h ? 0 : dh / h;
            lay.vx = (g->w - dw) / 2;
            lay.vy = (g->h - dh) / 2;
        }
    }
    lay.dw = dw;
    lay.dh = dh;
    lay.fx = fx;
    lay.fy = fy;
    for (i = 0; i <= w; i++)
        lay.xo[i] = (UWORD)(i * dw / w);
    for (i = 0; i <= h; i++)
        lay.yo[i] = (UWORD)(i * dh / h);
    return TRUE;
}

/* Full screen from a window: a screen of its own in Workbench's mode, size
 * and depth (the monitor does not change mode), with Workbench's colours
 * (the pens AmigaMCE has there draw the same), in place of the window for
 * as long as it is chosen. The picture's bitmap is made again for it. */
static BOOL pb_direct;          /* written straight (into lay.pbm), not made chunky */

static BOOL fs_pbm(struct Gfx *g, UWORD w, UWORD h, WORD ax, WORD ay)
{
    if (lay.pbm) {
        WaitBlit();
        FreeBitMap(lay.pbm);
        lay.pbm = NULL;
    }
    if (!pb_direct)
        return TRUE;                        /* (the 68k's way draws on the screen itself) */
    lay_out(g, w, h, ax, ay, TRUE);         /* (the largest it will be here) */
    lay.pbm = AllocBitMap(lay.dw, lay.dh, GetBitMapAttr(g->rp->BitMap, BMA_DEPTH), BMF_MINPLANES,
                          g->rp->BitMap);
    return lay.pbm != NULL;
}

/* Full screen from the window (gfx_fs_enter), the picture's bitmap made
 * again for it; and back. */
static BOOL fs_enter(struct Gfx *g, UWORD w, UWORD h, WORD ax, WORD ay)
{
    if (!gfx_fs_enter(g))
        return FALSE;
    st.y = g->h - st.h;                     /* the controls at its foot */
    if (!fs_pbm(g, w, h, ax, ay)) {
        gfx_fs_leave(g);
        st.y = g->h - st.h;
        fs_pbm(g, w, h, ax, ay);
        return FALSE;
    }
    return TRUE;
}

static void fs_leave(struct Gfx *g)
{
    if (!gfx_fs_on())
        return;
    if (lay.pbm) {                          /* (the screen's friend: the window's again) */
        WaitBlit();
        FreeBitMap(lay.pbm);
        lay.pbm = NULL;
    }
    gfx_fs_leave(g);
    st.y = g->h - st.h;
}

/* 0 played, -1 an error (err), 1 too slow here: the caller plays it on a
 * screen of its own. fh is just past the first frame (f). */
static LONG play_on_screen(struct Gfx *g, BPTR fh, struct Frame *f, const char *path, BOOL ahi,
                           UWORD w, UWORD h, UBYTE depth, UBYTE masking, UBYTE comp, ULONG camg,
                           BOOL has_camg, char *err, LONG errmax)
{
    ULONG rb = ((w + 15) >> 4) << 1, fsz = rb * depth * h, *lut = NULL, jlast = 0, readms = 0;
    ULONG convms = 0, worst = 0, total, t0, due, jif, start_ms, runs = 0, nconv = 0, nframes = 0;
    ULONG stripms = 0, nrows = 0, nskip = 0, nnodec = 0, fmt = 0, drawn_at = 0, decms = 0;
    BOOL whole_only = TRUE;         /* every frame so far a whole one (a BODY, op 0) */
    UBYTE *planar = NULL, *chunky = NULL, *line = NULL, *last = NULL;
    ULONG chunky_sz = 0;            /* (made, and made again, as its size on screen needs) */
    UWORD *l16 = NULL;
    BOOL ham = (camg & HAM_KEY) && (depth == 6 || depth == 8), paused = FALSE, loop = FALSE;
    BOOL direct = FALSE, fresh = TRUE, skip = FALSE, relaid = FALSE, whole = TRUE;
    WORD ax, ay, fx, fy, dw, dh, vx, vy, ox = 0, oy = 0;
    struct AnimPlanes pl[2];
    LONG result = -1;
    int k, i, n, quit = 0, shown;
    char m[400];
    const char *name = (const char *)FilePart((STRPTR)path);

    /* the picture's pixels' shape, then the largest whole scale that fits */
    if (!has_camg) {
        ax = w > 400 ? 1 : 2;
        ay = h > 300 ? 1 : 2;
    } else {
        ax = (camg & (HIRES | SUPERHIRES)) ? 1 : 2;
        ay = (camg & LACE) ? 1 : 2;
    }
    st.h = g->font_h + GU(g, 12);
    st.y = g->h - st.h;
    memset(&lay, 0, sizeof(lay));
    if (!(lay.xo = AllocVec((w + 1) * sizeof(UWORD), MEMF_ANY)) ||
        !(lay.yo = AllocVec((h + 1) * sizeof(UWORD), MEMF_ANY)) || !lay_out(g, w, h, ax, ay, FALSE)) {
        FreeVec(lay.xo);
        FreeVec(lay.yo);
        memset(&lay, 0, sizeof(lay));
        return 1;                           /* bigger than this screen: its own */
    }
    dw = lay.dw;
    dh = lay.dh;
    st.cw = st.h + GU(g, 6);
    st.x0 = GU(g, 8);

    ad_init();
    /* the screen's memory, to be written straight, if it can be had */
    if (GetCyberMapAttr(g->rp->BitMap, CYBRMATTR_ISCYBERGFX)) {
        UBYTE *base = NULL;
        ULONG bpr = 0;
        APTR lk = LockBitMapTags(g->rp->BitMap, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW,
                                 (ULONG)&bpr, LBMI_PIXFMT, (ULONG)&fmt, TAG_DONE);
        if (lk) {
            UnLockBitMap(lk);
            direct = base && bpr && ad_pix_tables(fmt);
        }
    }
    if (g->rp->Layer) {
        ox = g->rp->Layer->bounds.MinX;
        oy = g->rp->Layer->bounds.MinY;
    }
    /* Written into a bitmap of its own, not the screen's: locking what is
     * shown hides a pointer the system draws itself (PrismRTG's, Picasso96's
     * software one) each frame, and it flickers. Its changes blitted over. */
    if (direct) {                           /* big enough for it full screen, if it can be */
        lay_out(g, w, h, ax, ay, TRUE);
        lay.can_full = TRUE;
        if (!(lay.pbm = AllocBitMap(lay.dw, lay.dh, GetBitMapAttr(g->rp->BitMap, BMA_DEPTH),
                                    BMF_MINPLANES, g->rp->BitMap))) {
            lay.can_full = FALSE;
            lay.pbm = AllocBitMap(dw, dh, GetBitMapAttr(g->rp->BitMap, BMA_DEPTH), BMF_MINPLANES,
                                  g->rp->BitMap);
        }
    }
    if (lay.pbm) {
        UBYTE *base = NULL;
        ULONG bpr = 0, f2 = 0;
        APTR lk = GetCyberMapAttr(lay.pbm, CYBRMATTR_ISCYBERGFX)
                      ? LockBitMapTags(lay.pbm, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW,
                                       (ULONG)&bpr, LBMI_PIXFMT, (ULONG)&f2, TAG_DONE) : NULL;
        if (lk)
            UnLockBitMap(lk);
        if (!lk || !base || f2 != fmt) {
            FreeBitMap(lay.pbm);
            lay.pbm = NULL;
        }
    }
    if (!direct)
        lay.can_full = TRUE;                /* made chunky by the table: any size */
    else if (!lay.pbm)
        lay.can_full = FALSE;               /* straight onto the screen: whole scales */
    pb_direct = direct;
    if (g->wb && anim_full && lay.can_full && !fs_enter(g, w, h, ax, ay))
        anim_full = FALSE;                  /* (the screen would not open: in the window) */
    if (gfx_fs_on() && g->rp->Layer) {
        ox = g->rp->Layer->bounds.MinX;
        oy = g->rp->Layer->bounds.MinY;
    }
    lay_out(g, w, h, ax, ay, anim_full && lay.can_full);
    fx = lay.fx;
    fy = lay.fy;
    dw = lay.dw;
    dh = lay.dh;
    vx = lay.vx;
    vy = lay.vy;
    if (!(planar = AllocVec(fsz * 2, MEMF_ANY | MEMF_CLEAR)) ||
        !(last = AllocVec(fsz, MEMF_ANY | MEMF_CLEAR)) ||
        !(l16 = AllocVec(256 * sizeof(UWORD), MEMF_ANY | MEMF_CLEAR)) ||
        (!direct && !(chunky = AllocVec(chunky_sz = (ULONG)dw * dh * (ham ? 3 : 1), MEMF_ANY))) ||
        !(line = AllocVec(rb * 8 + 8, MEMF_ANY)) || !(lut = AllocVec(512 * sizeof(ULONG), MEMF_ANY))) {
        FreeVec(planar);
        FreeVec(last);
        FreeVec(l16);
        FreeVec(chunky);
        FreeVec(line);
        FreeVec(lut);
        if (lay.pbm)
            FreeBitMap(lay.pbm);
        FreeVec(lay.xo);
        FreeVec(lay.yo);
        memset(&lay, 0, sizeof(lay));
        return 1;
    }
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 8; i++)
            pl[k].plane[i] = planar + k * fsz + i * rb;
        pl[k].stride = rb * depth;
        pl[k].depth = depth;
        pl[k].rb = rb;
        pl[k].h = h;
    }
    if (f->cmap)
        ad_lut(f->cmap, f->cmaplen, camg, depth, lut);
    else
        memset(lut, 0, 256 * sizeof(ULONG));
    total = anim_length(fh, path, 0);
    FillPixelArray(g->rp, 0, 0, g->w, g->h, 0x000000);
    strip_setup(g);
    ctl.nbtn = lay.can_full ? BT_N : BT_FULL;
    /* the card's way, for a colour-table animation: the screen's colours
     * kept, to put back after each of its blits */
    {
        struct ColorMap *cm = g->scr ? g->scr->ViewPort.ColorMap : NULL;
        memset(&cd, 0, sizeof(cd));
        cd.ncol = depth >= 8 ? 256 : 1 << depth;
        if (cm && cm->Count < cd.ncol)
            cd.ncol = cm->Count;
        if (cm && !ham) {
            GetRGB32(cm, 0, cd.ncol, cd.save + 1);
            cd.save[0] = (ULONG)cd.ncol << 16;
            cd.save[1 + cd.ncol * 3] = 0;
            card_colours(lut);
            card_choose(g, w, h, depth, ham, rb);
        } else
            cd.ncol = 0;
    }
    sprintf(m, "%s: %lux%lux%lu (CAMG %05lx), on AmigaMCE's screen at %ld x %ld%s, %s", name,
            (unsigned long)w, (unsigned long)h, (unsigned long)depth, (unsigned long)camg,
            (long)dw, (long)dh, lay.full ? " (full screen)" : "",
            cd.mode == CARD_PLANES ? "its planes blitted by the card" :
            cd.mode == CARD_STRETCH ? "its planes stretched, then blitted by the card" :
            lay.pbm ? "written apart, then blitted" : direct ? "written straight" : "by WritePixelArray");
    amce_log("anim", m);
    if (LowLevelBase)
        jlast = (anim_g ? gfx_joy(anim_g) : ReadJoyPort(1));
    if (nsnd && !snd_on) {
        if (ahi && ahi_open())
            snd_ahi = snd_on = TRUE;
        else if ((silence = AllocMem(4, MEMF_CHIP | MEMF_CLEAR)) && mp_lend(TRUE))
            snd_on = TRUE;
    }

restart:
    animdec_body(f->body, f->bodylen, &pl[0], comp, masking);
    CopyMem(planar, planar + fsz, fsz);
    if (runs && f->cmap)
        ad_lut(f->cmap, f->cmaplen, camg, depth, lut);
    if (direct) {
        ad_lut16(lut, l16);
        if (ham)
            ad_ham(l16, depth);
    }
    if (cd.ncol)
        card_colours(lut);
    fresh = TRUE;
    skip = FALSE;
    shown = 0;
    start_ms = t0 = time_ms();
    due = 0;
    n = 0;
    for (;;) {
        ULONG td, op, bits, il, dt, rows0;
        int r;
        const UBYTE *src;
        /* the frame in pl[shown] to the screen, unless behind time */
        nframes++;
        if (skip)
            goto drawn;
        td = time_ms();
        src = planar + shown * fsz;
        rows0 = nrows;
        if (cd.mode != CARD_OFF && cd.trial != 1) {     /* the card's way */
            ULONG y, rowl = rb * depth;
            LONG ry0 = -1, ry1 = -1;
            card_source(src);
            if (cd.mode == CARD_PLANES) {   /* (the whole frame: a fraction of a ms) */
                card_reblit(g, 0, dh);
                nrows += h;
            } else {
                for (y = 0; y < h; y++) {   /* the rows changed */
                    const UBYTE *a = src + y * rowl;
                    UBYTE *b = last + y * rowl;
                    if (!fresh && !memcmp(a, b, rowl))
                        continue;
                    if (cd.mode == CARD_STRETCH)
                        card_stretch_row(a, y);
                    CopyMem((APTR)a, b, rowl);
                    if (ry0 < 0)
                        ry0 = y;
                    ry1 = y;
                    nrows++;
                }
                if (ry0 >= 0)
                    card_reblit(g, lay.yo[ry0], lay.yo[ry1 + 1]);
            }
            fresh = FALSE;
            if (!cd.checked) {              /* the first: the colours it should have? */
                cd.checked = TRUE;
                if (!card_check(g, src, lut)) {
                    amce_log("anim", "the card's colours are not the animation's: the CPU draws it");
                    card_free();
                    fresh = TRUE;
                    /* (the chunky buffer: made as the 68k's way draws) */
                }
            }
        } else if (direct) {
            UBYTE *base = NULL;
            ULONG bpr = 0, y, x0, x1, j, span, rowl = rb * depth, ry0 = h, ry1 = 0, cx0 = rb, cx1 = 0;
            APTR lk = LockBitMapTags(lay.pbm ? lay.pbm : g->rp->BitMap, LBMI_BASEADDRESS, (ULONG)&base,
                                     LBMI_BYTESPERROW, (ULONG)&bpr, TAG_DONE);
            if (lk && base) {
                UBYTE *o0 = lay.pbm ? base : base + (ULONG)(oy + vy) * bpr + (ULONG)(ox + vx) * 2;
                for (y = 0; y < h; y++) {
                    const UBYTE *a = src + y * rowl;
                    UBYTE *b = last + y * rowl, *o = o0 + (ULONG)lay.yo[y] * bpr;
                    ULONG px0, px1;
                    if (fresh) {
                        x0 = 0;
                        x1 = rb;
                    } else if (!ad_row_span(a, b, rb, depth, &x0, &x1))
                        continue;
                    if (ham)
                        x1 = rb;                /* what follows a change changes with it */
                    ad_row_put(a, rb, depth, w, x0, x1, fx, lay.xo, ham, lut, l16, o);
                    px0 = lay.xo[x0 * 8];
                    px1 = lay.xo[x1 * 8 > w ? w : x1 * 8];
                    span = (px1 - px0) * 2;
                    for (j = 1; j < (ULONG)(lay.yo[y + 1] - lay.yo[y]); j++)
                        CopyMem(o + px0 * 2, o + j * bpr + px0 * 2, span);
                    CopyMem((APTR)a, b, rowl);
                    nrows++;
                    if (y < ry0)
                        ry0 = y;
                    ry1 = y;
                    if (x0 < cx0)
                        cx0 = x0;
                    if (x1 > cx1)
                        cx1 = x1;
                }
                fresh = FALSE;
            }
            if (lk)
                UnLockBitMap(lk);
            if (lk && base && ry0 <= ry1)   /* what changed, onto the screen */
                pic_blit(g, lay.xo[cx0 * 8], lay.yo[ry0], lay.xo[cx1 * 8 > w ? w : cx1 * 8],
                         lay.yo[ry1 + 1]);
            if (!lk || !base) {                 /* not to be had after all: the other way */
                direct = pb_direct = FALSE;
                if (lay.pbm) {                  /* (that way draws on the screen) */
                    WaitBlit();
                    FreeBitMap(lay.pbm);
                    lay.pbm = NULL;
                }
                fresh = TRUE;
                goto drawn;                     /* (drawn that way next time) */
            }
        }
        if (!direct && (cd.mode == CARD_OFF || cd.trial == 1)) {
            /* made chunky at its size on screen, each pixel where the
             * table (lay.xo, lay.yo) puts it: any size, larger or smaller
             * (a pixel or a row of a smaller one going nowhere), then
             * drawn by the system at any depth */
            ULONG y, x, j;
            UBYTE *row;
            ULONG bpp = ham ? 3 : 1, rowb = (ULONG)dw * bpp, need = rowb * dh;
            if (need > chunky_sz) {
                FreeVec(chunky);
                chunky_sz = (chunky = AllocVec(need, MEMF_ANY)) ? need : 0;
                if (!chunky)
                    break;
            }
            for (y = 0; y < h; y++) {
                ULONG nr = lay.yo[y + 1] - lay.yo[y];
                if (!nr)
                    continue;                   /* (smaller: this row not shown) */
                row = chunky + (ULONG)lay.yo[y] * rowb;
                ad_p2c_row(src + y * rb * depth, rb, depth, w, line);
                if (ham) {                  /* each pixel from the one before it */
                    ULONG rgb = lut[0], hb = depth == 8 ? 6 : 4, lo = (1UL << hb) - 1;
                    UBYTE *o = row;
                    for (x = 0; x < w; x++) {
                        ULONG c = line[x], v = c & lo, ctl = c >> hb;
                        v = hb == 4 ? v * 17 : (v << 2) | (v >> 4);
                        if (ctl == 0)
                            rgb = lut[c & lo];
                        else if (ctl == 1)
                            rgb = (rgb & 0xFFFF00) | v;
                        else if (ctl == 2)
                            rgb = (rgb & 0x00FFFF) | (v << 16);
                        else
                            rgb = (rgb & 0xFF00FF) | (v << 8);
                        for (j = lay.xo[x]; j < lay.xo[x + 1]; j++) {
                            o = row + j * 3;
                            o[0] = rgb >> 16;
                            o[1] = rgb >> 8;
                            o[2] = rgb;
                        }
                    }
                } else if (fx == 1)
                    CopyMem(line, row, w);
                else if (fx > 1) {
                    UBYTE *o = row;
                    for (x = 0; x < w; x++)
                        for (j = 0; j < (ULONG)fx; j++)
                            *o++ = line[x];
                } else {
                    for (x = 0; x < w; x++)
                        for (j = lay.xo[x]; j < lay.xo[x + 1]; j++)
                            row[j] = line[x];
                }
                for (j = 1; j < nr; j++)
                    CopyMem(row, row + j * rowb, rowb);
            }
            if (ham)
                WritePixelArray(chunky, 0, 0, rowb, g->rp, vx, vy, dw, dh, RECTFMT_RGB);
            else
                WriteLUTPixelArray(chunky, 0, 0, dw, g->rp, lut, vx, vy, dw, dh, CTABFMT_XRGB8);
            nrows += h;
        }
        drawn_at = time_ms();
        dt = drawn_at - td;
        convms += dt;
        nconv++;
        if (cd.mode == CARD_STRETCH && cd.trial < 2 && direct) {
            /* the card against the 68k: each timed by the row (a frame
             * after the change, drawn whole, not counted) */
            if (cd.tframes++ > 0) {
                cd.tms[cd.trial] += dt;
                cd.trows[cd.trial] += nrows - rows0;
            }
            if (cd.tframes > CARD_TRIAL) {
                cd.tframes = 0;
                fresh = TRUE;               /* (each way's own picture drawn whole) */
                if (cd.trial == 0) {
                    cd.trial = 1;
                    lay.reblit = NULL;      /* (the 68k's picture is lay.pbm's) */
                } else {
                    ULONG c = cd.trows[0] ? cd.tms[0] * 1000 / cd.trows[0] : 0,
                          k = cd.trows[1] ? cd.tms[1] * 1000 / cd.trows[1] : 0;
                    sprintf(m, "the card %ld us a row, the 68k %ld: %s", (long)c, (long)k,
                            c <= k ? "the card draws it" : "the 68k draws it");
                    amce_log("anim", m);
                    if (c <= k) {
                        cd.trial = 2;
                        lay.reblit = card_reblit;
                    } else
                        card_free();
                }
            }
        }
        if (dt > worst)
            worst = dt;
        if (!runs && nconv == SLOW_FRAMES) {    /* can it keep up here? */
            ULONG avg = convms / nconv, want = due / (n ? n : 1);
            if (lay.full && avg > SLOW_MS && avg > want * 3) {
                /* full screen too much: its whole scale for this one, measured again */
                sprintf(m, "%ld ms a frame to draw full screen, %ld wanted: at its whole scale",
                        (long)avg, (long)want);
                amce_log("anim", m);
                lay_out(g, w, h, ax, ay, FALSE);
                dw = lay.dw;
                dh = lay.dh;
                vx = lay.vx;
                vy = lay.vy;
                fx = lay.fx;
                fy = lay.fy;
                if (cd.ncol)
                    card_choose(g, w, h, depth, ham, rb);   /* (its way for the new size) */
                FillPixelArray(g->rp, 0, 0, g->w, g->h, 0x000000);
                sf.lv = 0;
                sf.drawn = FALSE;
                fresh = TRUE;
                nconv = 0;
                convms = 0;
                if (!gfx_fs_on()) {              /* (from a window's: the way back stays) */
                    ctl.off |= 1 << BT_FULL;    /* greyed for the rest of this one */
                    if (ctl.focus == BT_FULL)
                        ctl.focus = BT_VUP;
                }
            } else if (avg > SLOW_MS && avg > want) {
                sprintf(m, "%ld ms a frame to draw on AmigaMCE's screen, %ld wanted: frames left out",
                        (long)avg, (long)want);
                amce_log("anim", m);
            }
        }
    drawn:
        td = time_ms();
        {
            BOOL sdrew = strip_tick(g, paused, due, total, name);
            if (whole)
                gfx_window_show(g);         /* a window: all of it, once laid out */
            else
                win_show(g, !skip, sdrew || sf.lv > 0, vx, vy, dw, dh);
            whole = FALSE;
        }
        stripms += time_ms() - td;
        /* input, and its time */
        for (;;) {
            r = input_with(g->win, &jlast, strip_hit);
            if (r == 1) {
                quit = 1;
                break;
            }
            if (r == 3)
                break;
            if (r == 4 && lay.can_full) {   /* full screen or not: laid out again, drawn afresh */
                anim_full = g->wb || gfx_fs_on() ? !gfx_fs_on() : !lay.full;
                if (g->wb || gfx_fs_on()) {
                    /* a window: full screen is the whole display, a screen
                     * of its own in Workbench's mode; back, the window */
                    int focus = ctl.focus;
                    strip_cleanup(g);
                    if (anim_full) {
                        if (!fs_enter(g, w, h, ax, ay))
                            anim_full = FALSE;
                    } else {
                        fs_leave(g);
                        fs_pbm(g, w, h, ax, ay);    /* the picture's bitmap, the window's */
                    }
                    strip_setup(g);
                    ctl.nbtn = BT_N;
                    ctl.focus = focus;
                    ox = oy = 0;
                    if (g->rp->Layer) {
                        ox = g->rp->Layer->bounds.MinX;
                        oy = g->rp->Layer->bounds.MinY;
                    }
                }
                lay_out(g, w, h, ax, ay, anim_full);
                dw = lay.dw;
                dh = lay.dh;
                vx = lay.vx;
                vy = lay.vy;
                fx = lay.fx;
                fy = lay.fy;
                if (cd.ncol)
                    card_choose(g, w, h, depth, ham, rb);   /* (its way for the new size) */
                FillPixelArray(g->rp, 0, 0, g->w, g->h, 0x000000);
                sf.lv = 0;
                sf.drawn = FALSE;
                fresh = TRUE;
                relaid = TRUE;
                break;
            }
            if (r == 2) {
                paused = !paused;
                if (paused && snd_on) {
                    snd_quiet(2);
                    snd_quiet(3);
                }
            }
            bar_redraw = FALSE;
            if (strip_tick(g, paused, due, total, name))
                win_show(g, FALSE, TRUE, 0, 0, 0, 0);   /* (only what changed: paused) */
            if (snd_on)
                snd_tick();
            if (!paused)
                break;
            WaitTOF();
            t0 = time_ms() - due;
        }
        if (quit)
            break;
        if (relaid) {                       /* the same frame drawn again, in its new place */
            relaid = FALSE;
            whole = TRUE;
            skip = FALSE;
            continue;
        }
        if (r == 3) {
            if (rd_seek(12) < 0 || !read_frame(fh, f, &readms) || !f->body)
                break;
            if (snd_on) {
                snd_quiet(2);
                snd_quiet(3);
            }
            paused = FALSE;
            runs++;
            goto restart;
        }
        if (!read_frame(fh, f, &readms))
            break;                          /* the end */
        op = f->anhd ? f->anhd[0] : 0;
        il = f->anhd ? f->anhd[18] : 0;
        bits = f->anhd ? be32(f->anhd + 20) : 0;
        jif = f->anhd ? be32(f->anhd + 14) : 1;
        k = shown ^ 1;
        if (op != 0 || !f->body)
            whole_only = FALSE;             /* deltas: each frame builds on the ones before */
        due += (jif ? jif : 1) * 1000 / 60;
        /* the one after this due already: this one not drawn; and, in an
         * animation of whole frames (Tron2: a BODY each), not decoded
         * either, as nothing builds on it */
        skip = time_ms() - drawn_at < SKIP_MS &&
               (LONG)(time_ms() - t0) > (LONG)(due + (jif ? jif : 1) * 1000 / 60);
        if (skip && whole_only) {
            nnodec++;
            k = shown;                      /* (the shown one stays the last decoded) */
        } else {
            ULONG tdec = time_ms();
            if (il == 1)
                CopyMem(planar + shown * fsz, planar + k * fsz, fsz);
            if (op == 0 && f->body)
                animdec_body(f->body, f->bodylen, &pl[k], comp, masking);
            else if (f->dlta && animdec_delta(f->dlta, f->dltalen, &pl[k], op, bits) == -2) {
                sprintf(err, _("ANIM-%ld compression is not supported"), (long)op);
                break;
            }
            decms += time_ms() - tdec;
        }
        if (f->cmap) {                      /* its colours changed: all of it again */
            ad_lut(f->cmap, f->cmaplen, camg, depth, lut + 256);
            if (memcmp(lut, lut + 256, 256 * sizeof(ULONG))) {
                CopyMem(lut + 256, lut, 256 * sizeof(ULONG));
                if (direct) {
                    ad_lut16(lut, l16);
                    if (ham)
                        ad_ham(l16, depth);
                }
                if (cd.ncol)
                    card_colours(lut);
                fresh = TRUE;
            }
        }
        if (skip)
            nskip++;
        while ((LONG)(time_ms() - t0) < (LONG)due - 10) {
            WaitTOF();
            if (strip_tick(g, paused, due, total, name))    /* a fade goes on meanwhile */
                win_show(g, FALSE, TRUE, 0, 0, 0, 0);
        }
        shown = k;
        n++;
        for (i = 0; i < f->nsctl; i++)
            snd_command(f->sctl[i]);
    }
    if (!quit && !err[0]) {                 /* the end: a short one again until stopped */
        ULONG ms = time_ms() - start_ms;
        if (!runs)
            loop = ms < LOOP_MS;
        if (loop && rd_seek(12) >= 0 && read_frame(fh, f, &readms) && f->body) {
            runs++;
            goto restart;
        }
    }
    sprintf(m, "%ld frames on AmigaMCE's screen, %ld drawn in %ld ms each (%ld at the most, %ld rows "
            "each), %ld not drawn (behind; %ld of them not decoded); decoding %ld ms; controls %ld ms "
            "each; reading %ld ms (%s: %ld ms waited for %ld pieces); played %ld time%s",
            (long)nframes, (long)nconv, (long)(nconv ? convms / nconv : 0), (long)worst,
            (long)(nconv ? nrows / nconv : 0), (long)nskip, (long)nnodec, (long)decms,
            (long)(nframes ? stripms / nframes : 0), (long)readms,
            rd.dp ? "read ahead" : "read when needed", (long)rd.waitms, (long)rd.waits,
            (long)runs + 1, runs ? "s" : "");
    amce_log("anim", m);
    result = err[0] ? -1 : 0;
    (void)errmax;
    (void)fy;                               /* (the rows go by lay.yo) */
    card_free();
    strip_cleanup(g);
    if (gfx_fs_on())
        fs_leave(g);                        /* the window again */
    WaitBlit();
    if (lay.pbm)
        FreeBitMap(lay.pbm);
    FreeVec(lay.xo);
    FreeVec(lay.yo);
    memset(&lay, 0, sizeof(lay));
    FreeVec(planar);
    FreeVec(last);
    FreeVec(l16);
    FreeVec(chunky);
    FreeVec(line);
    FreeVec(lut);
    return result;
}

LONG anim_play(struct Gfx *g, const char *path, BOOL ahi, BOOL classic, char *err, LONG errmax)
{
    anim_g = g;
    BPTR fh;
    UBYTE head[12];
    struct Frame f;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    struct ScreenBuffer *sb[2] = { NULL, NULL };
    struct MsgPort *safe = NULL;
    BOOL safe_wait[2] = { FALSE, FALSE };
    struct AnimPlanes pl[2];
    ULONG *tab = NULL, id, camg, ncol, jlast = 0, readms = 0, decms = 0, worst = 0, lates = 0;
    ULONG t0, due, jif, runs = 0, start_ms, loop_pos;
    UWORD w, h, *blank = NULL;
    UBYTE depth, masking, comp;
    WORD left, top;
    struct Rectangle clip;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    LONG result = -1;
    int shown = 0, n, k, quit = 0;
    BOOL paused = FALSE, loop = FALSE;
    char m[200];

    err[0] = 0;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh) {
        amce_strlcpy(err, _("The animation could not be opened"), errmax);
        return -1;
    }
    rd_begin(fh);                           /* read ahead from here on */
    if (rd_read(head, 12) != 12 || be32(head) != ID('F', 'O', 'R', 'M') ||
        be32(head + 8) != ID('A', 'N', 'I', 'M') || !read_frame(fh, &f, &readms) ||
        !f.bmhd || !f.body) {
        amce_strlcpy(err, _("Not an IFF ANIM this can play"), errmax);
        goto out;
    }
    loop_pos = 12;
    snd_read = TRUE;                        /* playing again: they are not read again */
    w = be16(f.bmhd);
    h = be16(f.bmhd + 2);
    depth = f.bmhd[8];
    masking = f.bmhd[9];
    comp = f.bmhd[10];
    camg = f.camg;
    if (!w || !h || !depth || depth > 8 || comp > 1) {
        amce_strlcpy(err, _("Not an IFF ANIM this can play"), errmax);
        goto out;
    }
    /* on a 16-bit RTG screen: on AmigaMCE's own, unless chosen otherwise */
    if (g && g->truecolor && g->win && CyberGfxBase && !classic) {
        LONG r = play_on_screen(g, fh, &f, path, ahi, w, h, depth, masking, comp, camg,
                                f.has_camg, err, errmax);
        if (r != 1) {
            result = r;
            goto out;
        }
        /* not here after all (bigger than the screen, no memory): its own screen */
        if (rd_seek(12) < 0 || !read_frame(fh, &f, &readms) || !f.body)
            goto out;
        camg = f.camg;
    }
    if ((id = pick_mode(camg, f.has_camg, w, h, depth)) == (ULONG)INVALID_ID || mode_depth(id) < depth) {
        (void)errmax;
        sprintf(err, _("This Amiga has no screen mode for it (%ld colours, %s)"),
                 (long)(1L << depth), (camg & HAM_KEY) ? "HAM" : (camg & EXTRAHALFBRITE_KEY) ? "EHB" :
                 depth > 6 ? "AGA" : "ECS");
        goto out;
    }
    ncol = 1UL << depth;
    if ((camg & EXTRAHALFBRITE_KEY) && depth == 6)
        ncol = 32;                          /* the other 32 are these at half */
    if (!(tab = AllocVec((ncol * 3 + 2) * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR)) ||
        !(blank = AllocMem(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR)))
        goto nomem;
    if (f.cmap)
        cmap_table(f.cmap, f.cmaplen, ncol, tab);
    else
        tab[0] = ncol << 16;
    place(id, w, h, &left, &top, &clip);
    scr = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, w, SA_Height, h, SA_Depth, depth,
                         SA_Left, left, SA_Top, top, SA_DClip, (ULONG)&clip,
                         SA_Colors32, (ULONG)tab, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
                         SA_Behind, TRUE, SA_Draggable, FALSE, SA_Exclusive, TRUE,
                         SA_Title, (ULONG)"AmigaMCE", TAG_DONE);
    if (!scr || !(sb[0] = AllocScreenBuffer(scr, NULL, SB_SCREEN_BITMAP)) ||
        !(sb[1] = AllocScreenBuffer(scr, NULL, SB_COPY_BITMAP)) || !(safe = CreateMsgPort()))
        goto nomem;
    sb[0]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe;
    sb[1]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe;
    if (!(GetBitMapAttr(sb[0]->sb_BitMap, BMA_FLAGS) & BMF_STANDARD) ||
        !(GetBitMapAttr(sb[1]->sb_BitMap, BMA_FLAGS) & BMF_STANDARD) ||
        GetBitMapAttr(sb[0]->sb_BitMap, BMA_DEPTH) < depth) {
        amce_strlcpy(err, _("Animations play on the Amiga's own screens, and none was found"), errmax);
        goto out;                           /* a graphics card's: no planes to draw into */
    }
    win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, w, WA_Height, h, WA_Backdrop, TRUE, WA_Borderless, TRUE,
                         WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_SimpleRefresh, TRUE,
                         WA_NoCareRefresh, TRUE, WA_ReportMouse, TRUE,
                         WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE, TAG_DONE);
    if (!win)
        goto nomem;
    ctl_begin(win, blank);
    planes_of(sb[0]->sb_BitMap, w, h, depth, &pl[0]);
    planes_of(sb[1]->sb_BitMap, w, h, depth, &pl[1]);
    bar_setup(sb[0]->sb_BitMap, w, h, depth, id, &clip, left, top);
    bar_pens(tab, ncol, camg, depth);
    bar.total_ms = anim_length(fh, path, 0);
    bar_redraw = FALSE;

    sprintf(m, "%s: %lux%lux%lu, mode %08lx (CAMG %05lx), op %ld", FilePart((STRPTR)path),
            (unsigned long)w, (unsigned long)h, (unsigned long)depth, (unsigned long)id, (unsigned long)camg,
            f.anhd ? (long)f.anhd[0] : 0L);
    amce_log("anim", m);
    if (LowLevelBase)
        jlast = ReadJoyPort(1);
    if (nsnd && !snd_on) {                  /* its own sound (what played waits: main.c) */
        if (ahi && ahi_open())
            snd_ahi = snd_on = TRUE;
        else if ((silence = AllocMem(4, MEMF_CHIP | MEMF_CLEAR)) && mp_lend(TRUE))
            snd_on = TRUE;
        sprintf(m, "%ld sounds; %s", (long)nsnd,
                snd_ahi ? "by AHI's default unit"
                : snd_on ? (ahi ? "AHI could not be opened: Paula's channels 2 and 3, lent"
                                : "Paula's channels 2 and 3, lent")
                         : "nothing to play them on (AHI has Paula?)");
        amce_log("anim", m);
    }

restart:
    /* the first frame, into both buffers: the next changes the second */
    k = shown ^ (runs ? 1 : 0);
    safe_done(safe, safe_wait);
    animdec_body(f.body, f.bodylen, &pl[k], comp, masking);
    bar.saved[k] = FALSE;                   /* all of it drawn anew */
    if (runs && f.cmap) {                   /* its first colours again */
        cmap_table(f.cmap, f.cmaplen, ncol, tab);
        LoadRGB32(&scr->ViewPort, tab);
        bar_pens(tab, ncol, camg, depth);
    }
    if (runs) {
        while (!ChangeScreenBuffer(scr, sb[k]))
            WaitTOF();
        safe_wait[shown] = TRUE;
        shown = k;
        safe_done(safe, safe_wait);
    }
    BltBitMap(sb[k]->sb_BitMap, 0, 0, sb[k ^ 1]->sb_BitMap, 0, 0, w, h, 0xC0, 0xFF, NULL);
    if (!runs) {
        ScreenToFront(scr);
        shown = 0;
    }
    WaitBlit();
    bar.saved[k ^ 1] = FALSE;
    bar.at_ms = 0;
    bar_put(sb[k]->sb_BitMap, k);
    start_ms = t0 = time_ms();
    jif = f.anhd ? be32(f.anhd + 14) : 0;
    due = 0;
    n = 1;
    for (;;) {
        ULONG td, il, op, bits, dt;
        int r = input(win, &jlast);
        BOOL want;
        if (r == 1) {
            quit = 1;
            break;
        }
        if (r == 3 && rd_seek(loop_pos) >= 0 && read_frame(fh, &f, &readms) &&
            f.body) {
            if (snd_on) {                   /* from the start: its sounds too */
                snd_quiet(2);
                snd_quiet(3);
            }
            paused = FALSE;
            bar.paused = FALSE;
            runs++;
            goto restart;
        }
        if (r == 2) {
            paused = !paused;
            bar_redraw = TRUE;
            if (paused && snd_on) {
                snd_quiet(2);               /* sounds stop with the pictures */
                snd_quiet(3);
            }
        }
        /* the controls: up while the mouse moves, and while paused */
        bar.paused = paused;
        want = ctl_want(paused) && bar.ok;
        if (want != bar.on) {
            bar.on = want;
            if (want)
                bar_put(sb[shown]->sb_BitMap, shown);
            else
                bar_take(sb[shown]->sb_BitMap, shown);
            bar_redraw = FALSE;
        } else if (bar_redraw && bar.on) {
            bar_put(sb[shown]->sb_BitMap, shown);
            bar_redraw = FALSE;
        }
        if (snd_on)
            snd_tick();
        if (paused) {
            WaitTOF();
            t0 = time_ms() - due;           /* the clock stopped meanwhile */
            continue;
        }
        if (!read_frame(fh, &f, &readms))
            break;                          /* the end */
        op = f.anhd ? f.anhd[0] : 0;
        il = f.anhd ? f.anhd[18] : 0;
        bits = f.anhd ? be32(f.anhd + 20) : 0;
        jif = f.anhd ? be32(f.anhd + 14) : 1;
        k = shown ^ 1;                      /* the buffer not shown: two frames back */
        safe_done(safe, safe_wait);
        td = time_ms();
        bar_take(sb[k]->sb_BitMap, k);      /* the frame as it was, under the controls */
        if (il == 1) {                      /* this one changes the frame before */
            BltBitMap(sb[shown]->sb_BitMap, 0, 0, sb[k]->sb_BitMap, 0, 0, w, h, 0xC0, 0xFF, NULL);
            if (bar.saved[shown])           /* ...as it is under the controls */
                BltBitMap(bar.save[shown], 0, 0, sb[k]->sb_BitMap, 0, bar.y, bar.x1 + 1, bar.h,
                          0xC0, 0xFF, NULL);
            WaitBlit();
        }
        if (op == 0 && f.body)
            animdec_body(f.body, f.bodylen, &pl[k], comp, masking);
        else if (f.dlta && animdec_delta(f.dlta, f.dltalen, &pl[k], op, bits) == -2) {
            sprintf(err, _("ANIM-%ld compression is not supported"), (long)op);
            quit = 1;
            break;
        }
        dt = time_ms() - td;
        decms += dt;
        if (dt > worst)
            worst = dt;
        /* its time: sixtieths of a second after the last (none: a frame) */
        due += (jif ? jif : 1) * 1000 / 60;
        bar.at_ms = due;
        bar_put(sb[k]->sb_BitMap, k);       /* the controls on it, if they are up */
        if ((LONG)(time_ms() - t0) > (LONG)due + 20)
            lates++;
        while ((LONG)(time_ms() - t0) < (LONG)due - 10)
            WaitTOF();
        if (f.cmap) {
            cmap_table(f.cmap, f.cmaplen, ncol, tab);
            LoadRGB32(&scr->ViewPort, tab);
            bar_pens(tab, ncol, camg, depth);
        }
        while (!ChangeScreenBuffer(scr, sb[k]))
            WaitTOF();                      /* the last change not taken yet */
        safe_wait[shown] = TRUE;
        shown = k;
        {
            int i;
            for (i = 0; i < f.nsctl; i++)   /* its sounds, as it is shown */
                snd_command(f.sctl[i]);
        }
        n++;
    }
    runs++;
    if (!quit && !err[0]) {
        ULONG ms = time_ms() - start_ms;
        if (runs == 1)
            loop = ms < LOOP_MS;
        if (loop && rd_seek(loop_pos) >= 0 && read_frame(fh, &f, &readms) &&
            f.body)
            goto restart;
    }
    {
        ULONG ms = time_ms() - start_ms;
        sprintf(m, "%ld frames in %ld ms (%ld a second), decoding %ld ms (%ld at the most), "
                "reading %ld ms, %ld late; played %ld time%s",
                (long)n, (long)ms, ms ? (long)(n * 1000 / ms) : 0L, (long)decms, (long)worst,
                (long)readms, (long)lates, (long)runs, runs == 1 ? "" : "s");
        amce_log("anim", m);
    }
    result = 0;
    goto out;
nomem:
    amce_strlcpy(err, _("Not enough memory for the animation's screen"), errmax);
out:
    if (scr) {
        safe_done(safe, safe_wait);         /* nothing left in flight */
        if (shown != 0 && sb[0]) {          /* the screen's own bitmap back before it closes */
            while (!ChangeScreenBuffer(scr, sb[0]))
                WaitTOF();
            safe_wait[1] = TRUE;
            safe_done(safe, safe_wait);
        }
        if (win)
            CloseWindow(win);
        if (sb[1])
            FreeScreenBuffer(scr, sb[1]);
        if (sb[0])
            FreeScreenBuffer(scr, sb[0]);
        CloseScreen(scr);
        if (g && g->scr)
            ScreenToFront(g->scr);
        if (g && g->win) {
            /* AmigaMCE's window active again before going back to it: Intuition
             * does it later, and until then a click only activates it (the
             * first of a double-click lost: the next animation did not open) */
            int i;
            ActivateWindow(g->win);
            for (i = 0; i < 25 && !(g->win->Flags & WFLG_WINDOWACTIVE); i++)
                WaitTOF();
            if (!(g->win->Flags & WFLG_WINDOWACTIVE))
                amce_log("anim", "AmigaMCE's window not active yet after half a second");
        }
    }
    if (safe) {
        while (GetMsg(safe))
            ;
        DeleteMsgPort(safe);
    }
    FreeVec(tab);
    if (blank)
        FreeMem(blank, 6 * sizeof(UWORD));
    bar_cleanup();
    snd_free();
    FreeVec(fbuf);
    fbuf = NULL;
    fbuf_size = 0;
    rd_end();
    Close(fh);
    return result;
}
