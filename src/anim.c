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
 * Keys: Esc, Backspace, the right button or the pad's blue stop it; Space,
 * Return, the left button or fire pause it; Up and Down (the stick, the
 * wheel) set the volume of what plays meanwhile, a station or a module,
 * as on Now playing. A short one (under 8 s) plays
 * again until stopped; a longer one goes back to the list at its end.
 */
#include "amce.h"
#include "animdec.h"

#include <string.h>
#include <stdio.h>
#include <exec/ports.h>
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

extern struct GfxBase *GfxBase;
extern struct Library *LowLevelBase;

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

/* A FORM 8SVX's sound (its first octave), len bytes after the type, from
 * fh into chip RAM; Fibonacci-delta packed ones unpacked. */
static void snd_load(BPTR fh, ULONG len)
{
    static const BYTE fib[16] = { -34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21 };
    UBYTE *b, *p, *e;
    const UBYTE *vhdr = NULL, *body = NULL;
    ULONG bl = 0, n;
    struct Sound *s;
    if (nsnd >= SOUNDS) {
        Seek(fh, len + (len & 1), OFFSET_CURRENT);
        return;
    }
    s = &snd[nsnd++];                       /* numbered as they come, read or not */
    if (!(b = AllocVec(len, MEMF_ANY)) || Read(fh, b, len) != (LONG)len) {
        FreeVec(b);
        return;
    }
    if (len & 1)
        Seek(fh, 1, OFFSET_CURRENT);
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
        if (Read(fh, h, 12) != 12 || be32(h) != ID('F', 'O', 'R', 'M'))
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
        if (Seek(fh, len + (len & 1), OFFSET_CURRENT) < 0)  /* the rest: passed */
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
    if (Read(fh, fbuf, len) != (LONG)len)
        return FALSE;
    if (len & 1)
        Seek(fh, 1, OFFSET_CURRENT);
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

#define VOL_STEP    4           /* of 64, as Now playing's */
#define VOL_DELAY 300           /* ms the stick is held before it repeats */
#define VOL_REPEAT 100          /* ...and then a step every */

static void volume(int d)
{
    int v = mp_volume() + d;
    mp_set_volume(v < 0 ? 0 : v > 64 ? 64 : v);
}

/* Input while it plays: 1 stop, 2 pause or go on, 0 nothing; the volume
 * set here. */
static int input(struct Window *win, ULONG *jlast)
{
    static ULONG held_since, held_step;
    struct IntuiMessage *im;
    int r = 0;
    while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        ULONG cl = im->Class;
        UWORD code = im->Code;
        ReplyMsg((struct Message *)im);
        if (cl == IDCMP_RAWKEY) {
            if (code == 0x45 || code == 0x41 || code == 0x10)   /* Esc, Backspace, Q */
                r = 1;
            else if ((code == 0x40 || code == 0x44 || code == 0x43) && !r)  /* Space, Return */
                r = 2;
            else if (code == 0x4C || code == 0x7A)  /* Up (held: it repeats), the wheel */
                volume(VOL_STEP);
            else if (code == 0x4D || code == 0x7B)
                volume(-VOL_STEP);
        } else if (cl == IDCMP_MOUSEBUTTONS) {
            if (code == MENUDOWN)
                r = 1;
            else if (code == SELECTDOWN && !r)
                r = 2;
        }
    }
    if (LowLevelBase) {
        ULONG st = ReadJoyPort(1), pressed = st & ~*jlast;
        if ((st & JP_TYPE_MASK) == JP_TYPE_JOYSTK || (st & JP_TYPE_MASK) == JP_TYPE_GAMECTLR) {
            if (pressed & JPF_BUTTON_BLUE)
                r = 1;
            else if ((pressed & (JPF_BUTTON_RED | JPF_BUTTON_PLAY)) && !r)
                r = 2;
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

LONG anim_play(struct Gfx *g, const char *path, BOOL ahi, char *err, LONG errmax)
{
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
    if (Read(fh, head, 12) != 12 || be32(head) != ID('F', 'O', 'R', 'M') ||
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
                         WA_NoCareRefresh, TRUE,
                         WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS, TAG_DONE);
    if (!win)
        goto nomem;
    SetPointer(win, blank, 1, 16, 0, 0);
    planes_of(sb[0]->sb_BitMap, w, h, depth, &pl[0]);
    planes_of(sb[1]->sb_BitMap, w, h, depth, &pl[1]);

    sprintf(m, "%s: %lux%lux%lu, mode %08lx (CAMG %05lx), op %ld", FilePart((STRPTR)path),
            (unsigned long)w, (unsigned long)h, (unsigned long)depth, (unsigned long)id, (unsigned long)camg,
            f.anhd ? (long)f.anhd[0] : 0L);
    amce_log("anim", m);
    if (LowLevelBase)
        jlast = ReadJoyPort(1);
    if (nsnd) {                             /* its own sound (what played waits: main.c) */
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
    if (runs && f.cmap) {                   /* its first colours again */
        cmap_table(f.cmap, f.cmaplen, ncol, tab);
        LoadRGB32(&scr->ViewPort, tab);
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
    start_ms = t0 = time_ms();
    jif = f.anhd ? be32(f.anhd + 14) : 0;
    due = 0;
    n = 1;
    for (;;) {
        ULONG td, il, op, bits, dt;
        int r = input(win, &jlast);
        if (r == 1) {
            quit = 1;
            break;
        }
        if (r == 2) {
            paused = !paused;
            if (paused && snd_on) {
                snd_quiet(2);               /* sounds stop with the pictures */
                snd_quiet(3);
            }
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
        if (il == 1)                        /* this one changes the frame before */
            BltBitMap(sb[shown]->sb_BitMap, 0, 0, sb[k]->sb_BitMap, 0, 0, w, h, 0xC0, 0xFF, NULL),
                WaitBlit();
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
        if ((LONG)(time_ms() - t0) > (LONG)due + 20)
            lates++;
        while ((LONG)(time_ms() - t0) < (LONG)due - 10)
            WaitTOF();
        if (f.cmap) {
            cmap_table(f.cmap, f.cmaplen, ncol, tab);
            LoadRGB32(&scr->ViewPort, tab);
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
        if (loop && Seek(fh, loop_pos, OFFSET_BEGINNING) >= 0 && read_frame(fh, &f, &readms) &&
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
        if (g && g->win)
            ActivateWindow(g->win);
    }
    if (safe) {
        while (GetMsg(safe))
            ;
        DeleteMsgPort(safe);
    }
    FreeVec(tab);
    if (blank)
        FreeMem(blank, 6 * sizeof(UWORD));
    snd_free();
    FreeVec(fbuf);
    fbuf = NULL;
    fbuf_size = 0;
    Close(fh);
    return result;
}
