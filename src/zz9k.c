/*
 * Ambiance - the ZZ9000's zz9k.library: its media sessions (MPEG video
 * decoded on the card, shown on its overlay, its sound given back)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * The only file built with the ZZ9000 SDK's headers (MIT, fetched by the
 * Makefile: third_party/zz9k-sdk.txt; zz9k.library itself is GPL-3, with a
 * section 7 permission for any program that opens it and calls it through
 * these headers). Its request builders and reply decoders
 * (zz9k/request.h, reply.h) make each call; zz9k.library carries it
 * (ZZ9KCall; before 2.34, zz_call's own wait), shared safely with the
 * card's picture datatype, mpega and AmiSSL. The UI and the playback process both call it: the library
 * serialises the mailbox.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "proto/zz9k.h"
#include "zz9k/request.h"
#include "zz9k/reply.h"
#include "amb.h"
#include "zz9k.h"

static void zz_card_wrote(volatile const void *p, ULONG len);

struct Library *ZZ9KBase;
static int zz_opens;

BOOL zz_open(void)
{
    if (zz_opens++ > 0)
        return ZZ9KBase != NULL;
    ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME, ZZ9K_LIBRARY_VERSION);
    if (!ZZ9KBase)
        amb_log("zz9k", "zz9k.library 2 could not be opened");
    return ZZ9KBase != NULL;
}

void zz_close(void)
{
    if (zz_opens <= 0 || --zz_opens > 0)
        return;
    if (ZZ9KBase)
        CloseLibrary(ZZ9KBase);
    ZZ9KBase = NULL;
}

/* What the card offers beyond video, once at start-up (caps.c): the
 * library's revision and capabilities; one-shot JPEG (ZZ9KDecodeJpeg,
 * revision 15, ZZ9K_CAP_IMAGE_DECODE); and whether a surface allocated
 * without ARM_LOCAL is one the 68k can read. No flag says that: an
 * Amiberry before #2733 answered UNSUPPORTED, and a wrong answer given
 * as a success has been seen (picture-paths-eval.md, gap 4). So it is
 * tried: a 16 x 16 BGRA8888 surface filled with one colour, a square of
 * it with another, its pixels read back from the 68k's side and every
 * one's red, green and blue checked, and freed. (Fill takes its colour
 * as 0xAARRGGBB and writes it in the surface's format, alpha opaque:
 * bytes B, G, R, FF, as Amiberry 8.6.0 showed.) */
#define PROBE_C1 0x00345678UL   /* (R 34, G 56, B 78) */
#define PROBE_C2 0x00BCDEF0UL

/* a BGRA8888 pixel (bytes B, G, R, A) as 0x00RRGGBB */
static ULONG bgra_rgb(ULONG v)
{
    return ((v >> 8) & 0xFF) << 16 | ((v >> 16) & 0xFF) << 8 | (v >> 24);
}

static const char *probe_surface(void)
{
    static char why[80];
    ZZ9KSurface sf;
    ZZ9KSurfaceFillDesc fd;
    int s;
    ULONG x, y;
    memset(&sf, 0, sizeof(sf));
    amb_card_lock();
    s = ZZ9KAllocSurfaceEx(16, 16, ZZ9K_SURFACE_FORMAT_BGRA8888, 0, 0, &sf);
    amb_card_unlock();
    if (s != ZZ9K_STATUS_OK) {
        sprintf(why, "allocating it: %s", zz_name(s));
        return why;
    }
    *why = 0;
    memset(&fd, 0, sizeof(fd));
    fd.surface = sf.handle;
    fd.width = 16;
    fd.height = 16;
    fd.color = 0xFF000000UL | PROBE_C1;
    amb_card_lock();
    s = ZZ9KFillSurface(&fd);
    if (s == ZZ9K_STATUS_OK) {
        fd.x = fd.y = 4;
        fd.width = fd.height = 8;
        fd.color = 0xFF000000UL | PROBE_C2;
        s = ZZ9KFillSurface(&fd);
    }
    amb_card_unlock();
    if (s != ZZ9K_STATUS_OK)
        sprintf(why, "filling it: %s", zz_name(s));
    else if (!sf.data || sf.pitch < 16 * 4)
        sprintf(why, "no 68k address (%08lx, pitch %lu)", (unsigned long)sf.data,
                (unsigned long)sf.pitch);
    else
        for (zz_card_wrote(sf.data, sf.pitch * 16), y = 0; y < 16 && !*why; y++) {
            volatile const ULONG *row = (volatile const ULONG *)((volatile const UBYTE *)sf.data +
                                                                 y * sf.pitch);
            for (x = 0; x < 16; x++) {
                ULONG want = x >= 4 && x < 12 && y >= 4 && y < 12 ? PROBE_C2 : PROBE_C1, got = row[x];
                if (bgra_rgb(got) != want) {
                    sprintf(why, "pixel %lu,%lu read %08lx, not %06lx", (unsigned long)x,
                            (unsigned long)y, (unsigned long)got, (unsigned long)want);
                    break;
                }
            }
        }
    amb_card_lock();
    ZZ9KFreeSurface(sf.handle);
    amb_card_unlock();
    return *why ? why : NULL;
}

/* An RGB565 surface the 68k can read, and its byte order: filled with
 * yellow (0xFFE0) and a square of blue (0x001F), read back as words.
 * 1: big-endian (the 68k's: as the ZZ9000's 16-bit screens), 2:
 * byte-swapped, 0: not offered or wrong. The viewer then reads back half
 * as many bytes as BGRA8888 on a 16-bit screen. */
static UBYTE zz_565;

static UBYTE probe_565(void)
{
    ZZ9KSurface sf;
    ZZ9KSurfaceFillDesc fd;
    ULONG x, y, be = 0, le = 0, bad = 0;
    int s;
    memset(&sf, 0, sizeof(sf));
    amb_card_lock();
    s = ZZ9KAllocSurfaceEx(16, 16, ZZ9K_SURFACE_FORMAT_RGB565, 0, 0, &sf);
    amb_card_unlock();
    if (s != ZZ9K_STATUS_OK)
        return 0;
    memset(&fd, 0, sizeof(fd));
    fd.surface = sf.handle;
    fd.width = fd.height = 16;
    fd.color = 0xFFF8FC00UL;                /* yellow: 0xFFE0 */
    amb_card_lock();
    s = ZZ9KFillSurface(&fd);
    if (s == ZZ9K_STATUS_OK) {
        fd.x = fd.y = 4;
        fd.width = fd.height = 8;
        fd.color = 0xFF0000F8UL;            /* blue: 0x001F */
        s = ZZ9KFillSurface(&fd);
    }
    amb_card_unlock();
    if (s == ZZ9K_STATUS_OK && sf.data && sf.pitch >= 32)
        for (zz_card_wrote(sf.data, sf.pitch * 16), y = 0; y < 16; y++) {
            volatile const UWORD *row = (volatile const UWORD *)((volatile const UBYTE *)sf.data +
                                                                 y * sf.pitch);
            for (x = 0; x < 16; x++) {
                UWORD want = x >= 4 && x < 12 && y >= 4 && y < 12 ? 0x001F : 0xFFE0, got = row[x];
                if (got == want)
                    be++;
                else if (got == (UWORD)((want >> 8) | (want << 8)))
                    le++;
                else
                    bad++;
            }
        }
    amb_card_lock();
    ZZ9KFreeSurface(sf.handle);
    amb_card_unlock();
    return s != ZZ9K_STATUS_OK || bad || (be && le) || (!be && !le) ? 0 : be ? 1 : 2;
}

void zz_probe(struct Caps *c)
{
    ZZ9KCaps zc;
    char m[240];
    const char *why = "the library is older than revision 11";
    ULONG t0;
    int s;
    if (!c->zz9000 || !zz_open())
        return;
    memset(&zc, 0, sizeof(zc));
    amb_card_lock();
    s = ZZ9KQueryCaps(&zc);
    amb_card_unlock();
    c->zz_rev = ZZ9KBase->lib_Revision;
    if (s != ZZ9K_STATUS_OK) {
        sprintf(m, "zz9k.library 2.%lu: the card's capabilities could not be read (%s)",
                (unsigned long)c->zz_rev, zz_name(s));
        amb_log("zz9k", m);
        zz_close();
        return;
    }
    c->zz_capbits = zc.capability_bits;
    c->zz_fw = zc.firmware_version;
    c->zz_jpeg1 = c->zz_rev >= ZZ9K_LIBRARY_MIN_REVISION_TYPED_IMAGE_DECODE &&
                  (zc.capability_bits & ZZ9K_CAP_IMAGE_DECODE);
    t0 = time_ms();
    if (!(zc.capability_bits & ZZ9K_CAP_SURFACE_OPS))
        why = "the card has no surface operations";
    else if (c->zz_rev >= ZZ9K_LIBRARY_MIN_REVISION_SURFACE_OPS)
        why = probe_surface();
    c->zz_cpusurf = why == NULL;
    if (c->zz_cpusurf)
        zz_565 = probe_565();
    sprintf(m, "zz9k.library 2.%lu, ABI %lu.%lu, firmware %08lx, caps %08lx; one-shot JPEG %s; "
               "CPU-visible surfaces %s%s%s (%lu ms)",
            (unsigned long)c->zz_rev, (unsigned long)zc.abi_major, (unsigned long)zc.abi_minor,
            (unsigned long)zc.firmware_version, (unsigned long)zc.capability_bits,
            c->zz_jpeg1 ? "yes" : "no",
            c->zz_cpusurf ? "yes" : "no: ", c->zz_cpusurf ? "" : why,
            !c->zz_cpusurf ? "" : zz_565 == 1 ? ", RGB565 too" : zz_565 == 2 ? ", RGB565 too (byte-swapped)"
                                                : ", not RGB565",
            (unsigned long)(time_ms() - t0));
    amb_log("zz9k", m);
    zz_close();
}

/* ---- the viewer's JPEG decoded on the card (v0.9.5): an image session
 * fits it to the size shown (FIT | PRESERVE_ASPECT) into a BGRA8888
 * surface the 68k can read (no ARM_LOCAL), fed from the file 256 KB at a
 * time through one shared buffer; its rows are read back into a Thumb:
 * true colour (argb) for a deep screen, else cut to maxcol colours as
 * jdec's are (thumb_from_rgb). One screen's pixels cross the bus, not the
 * whole photo (the datatypes' 36-96 MB). Only where caps.zz_cpusurf (and
 * Ambiance/ViewCard not "0"); anything other than OK goes back to jdec,
 * said once per kind. */
BOOL zz_view_on;
static BOOL view_held;
static int zz_call_ticks(ZZ9KRequest *rq, ZZ9KMailboxEntry *reply, ULONG ticks, ULONG spin_ms);
#define ZZ_IMG_TICKS 500        /* a feed's budget: 10 s (a 24 MP photo on the real card) */
/* Polled this long before a tick at a time: on the real card a session's
 * call is answered from its second core a few ms later, and 400 polls (3 ms
 * on a 68060) then Delay(1) made each 20-40 ms; a 12 MP photo is some 250
 * calls (zzprobe 1.21) */
#define ZZ_IMG_SPIN_MS 50

/* An image session's request, sent through zz_call_ticks (the library's
 * own wait before 2.34 is a few milliseconds of polls: Amiberry 8.6.0,
 * whose card works alongside the 68k, timed most photos out), and its
 * result read back */
static int img_call(ZZ9KRequest *rq, UWORD op, ZZ9KImageSessionResult *r)
{
    ZZ9KMailboxEntry reply;
    int st;
    memset(&reply, 0, sizeof(reply));
    if ((st = zz_call_ticks(rq, &reply, ZZ_IMG_TICKS, ZZ_IMG_SPIN_MS)) != ZZ9K_STATUS_OK)
        return st;
    return r ? zz9k_reply_image_session_result(&reply, op, r) : (int)reply.status;
}          /* zz9k.library opened for it (zz_view_hold) */

/* The library held open for the viewer's JPEGs while Ambiance runs, let go
 * before a game (and at quit: its last close reclaims anything left on the
 * card, from revision 38), opened again after */
void zz_view_hold(BOOL on)
{
    if (on && zz_view_on && !view_held)
        view_held = zz_open();
    else if (!on && view_held) {
        zz_close();
        view_held = FALSE;
    }
}

/* A JPEG's size from its frame header (SOFn), the APPn before it skipped;
 * *prog: progressive or lossless (SOF2.., which the card may not take) */
static BOOL jpeg_dims(BPTR fh, ULONG *w, ULONG *h, BOOL *prog)
{
    UBYTE b[10];
    LONG pos = 2;
    if (Read(fh, b, 2) != 2 || b[0] != 0xFF || b[1] != 0xD8)
        return FALSE;
    for (;;) {
        ULONG len;
        if (Seek(fh, pos, OFFSET_BEGINNING) < 0 || Read(fh, b, 4) != 4 || b[0] != 0xFF)
            return FALSE;
        while (b[1] == 0xFF) {              /* fill bytes */
            b[1] = b[2];
            b[2] = b[3];
            pos++;
            if (Read(fh, b + 3, 1) != 1)
                return FALSE;
        }
        len = ((ULONG)b[2] << 8) | b[3];
        if (b[1] >= 0xC0 && b[1] <= 0xCF && b[1] != 0xC4 && b[1] != 0xC8 && b[1] != 0xCC) {
            *prog = b[1] != 0xC0 && b[1] != 0xC1;   /* (not baseline or extended) */
            if (Read(fh, b, 5) != 5)
                return FALSE;
            *h = ((ULONG)b[1] << 8) | b[2];
            *w = ((ULONG)b[3] << 8) | b[4];
            return *w && *h;
        }
        if (b[1] == 0xD9 || b[1] == 0xDA || len < 2)
            return FALSE;
        pos += 2 + (LONG)len;
    }
}

static void view_say(const char *path, const char *what, int st, ULONG ms)
{
    static UWORD said;
    char m[320];
    if (said >= 8)
        return;
    sprintf(m, "%.50s: on the card, %s (%s) after %lu ms: jdec instead%s", FilePart((STRPTR)path), what,
            st >= 0 ? zz_name(st) : "no", (unsigned long)ms, ++said == 8 ? " (no more of these said)" : "");
    amb_log("viewer", m);
}

static BOOL no565;              /* an RGB565 session refused once: BGRA8888 from then on */

/* The 68k's data cache and the card. A 68040/060's copyback cache keeps the
 * last lines the 68k wrote until they are pushed out, and what it read of
 * the card's memory after the card has changed it; the ZZ9000 docs ask for
 * the card's memory to be mapped Writethrough or NoCache, which Ambiance
 * cannot count on. And a disk's DMA may not reach Zorro III memory (the
 * A3000's SCSI). So a file goes through fast RAM, the 68k copies it to the
 * card and pushes its cache out (zz_view_jpeg's feed loop, zz_read); before
 * the 68k reads what the card made, its own lines are dropped
 * (zz_card_wrote). (On the A3000 the cause of the sessions' trouble proved
 * to be the feeding, below; these stay for CopyBack setups.) */
#define ZZ_BOUNCE (32 * 1024)
/* A session's file goes through one 32 KB buffer on the card, every feed
 * from its start, holding the file from where the card got to, as the
 * card's own zz9k-jpeg feeds it. On a real A3000 256 KB chunks fed at an
 * offset got nothing taken at all; this way (zzprobe 1.20) every photo
 * decoded. The file comes from a window in fast RAM read in big pieces:
 * 32 KB reads cost a disk's latency each, some 3 s for a 12 MP photo
 * (zzprobe 1.21). The window is sized to the memory free: 512 KB (a
 * 1080p photo whole, a 12 MP one in five reads) with more than 4 MB in
 * one piece, 128 KB with more than 1 MB, else the 32 KB itself; with not
 * even that, the picture is jdec's (or says it hasn't the memory) */
#define ZZ_WIN_BIG (512 * 1024)
#define ZZ_WIN_MID (128 * 1024)
#define ZZ_WIN_ALIGN 4096       /* the window's reads: at and in multiples of this */
#define ZZ_WIN_MIN (ZZ_STAGE + ZZ_WIN_ALIGN)   /* a feed's 32 KB, from anywhere in a 4 KB step */
#define ZZ_STAGE ZZ_BOUNCE
#define ZZ_STALL_MS 3000        /* a session taking none of the file this long is stuck */

static void zz_card_wrote(volatile const void *p, ULONG len)
{
    CacheClearE((APTR)p, len, CACRF_ClearD);
}

struct Thumb *zz_view_jpeg(const char *path, UWORD gw, UWORD gh, BOOL tall, LONG maxcol,
                           struct PicInfo *pi, BOOL s16)
{
    ZZ9KSharedBuffer buf;
    ZZ9KSurface sf;
    ZZ9KImageSessionBeginDesc bd;
    ZZ9KImageSessionFeedDesc fd;
    ZZ9KImageSessionResult r;
    struct PicInfo p;
    struct Thumb *t = NULL;
    BPTR fh;
    LONG flen;
    ULONG w = 0, h = 0, dw, dh, ow = 0, oh = 0, size, cpos = 0, spos = 0, staged = 0, sess = 0,
          t0 = time_ms(), t1, x, y, feeds = 0, feed_max = 0, quiet;
    BOOL ready = FALSE;
    UBYTE *win = NULL;          /* the file from wstart, wlen bytes (wsize at most) */
    ULONG wsize, wstart = 0, wlen = 0;
    BOOL prog = FALSE, began = FALSE, w16 = s16 && zz_565 && !no565 && maxcol <= THUMB_DEEP;
    ZZ9KSurface s16f;           /* w16: the card's scaler copies the picture into it as RGB565 */
    int st = ZZ9K_STATUS_OK;
    const char *stage = NULL;
    if (!zz_view_on || !view_held || !ZZ9KBase || !(fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
        return NULL;
    if (!jpeg_dims(fh, &w, &h, &prog) || Seek(fh, 0, OFFSET_END) < 0 ||
        (flen = Seek(fh, 0, OFFSET_BEGINNING)) <= 0) {
        Close(fh);
        return NULL;
    }
    memset(&p, 0, sizeof(p));
    p.w = (UWORD)w;
    p.h = (UWORD)h;
    p.depth = 24;
    strcpy(p.fmt, "JPEG");
    strcpy(p.cls, "zz9k");
    view_fit(gw, gh, tall, &p, &dw, &dh, NULL, 0);
    if (dw >= w || dh >= h) {               /* (made larger: jdec's, quick at its size) */
        Close(fh);
        return NULL;
    }
    memset(&buf, 0, sizeof(buf));
    memset(&sf, 0, sizeof(sf));
    memset(&s16f, 0, sizeof(s16f));
    size = (ULONG)flen < ZZ_STAGE ? (ULONG)flen : ZZ_STAGE;
    {
        ULONG big = AvailMem(MEMF_ANY | MEMF_LARGEST);
        wsize = big > 4UL << 20 ? ZZ_WIN_BIG : big > 1UL << 20 ? ZZ_WIN_MID : ZZ_WIN_MIN;
        if (wsize > (ULONG)flen)
            wsize = (ULONG)flen;
        if (!(win = AllocVec(wsize, MEMF_ANY)) && wsize > ZZ_WIN_MIN &&
            (wsize = (ULONG)flen < ZZ_WIN_MIN ? (ULONG)flen : ZZ_WIN_MIN))
            win = AllocVec(wsize, MEMF_ANY);
        if (!win) {
            view_say(path, "memory for reading it (36 KB)", ZZ9K_STATUS_NO_MEMORY, time_ms() - t0);
            Close(fh);
            return NULL;
        }
    }
    amb_card_lock();
    if ((st = ZZ9KAllocShared(size, 16, ZZ9K_ALLOC_HOST_WINDOW, &buf)) != ZZ9K_STATUS_OK)
        stage = "a shared buffer";
    else if ((st = ZZ9KAllocSurfaceEx(dw, dh, ZZ9K_SURFACE_FORMAT_BGRA8888,
                                      w16 ? ZZ9K_SURFACE_FLAG_ARM_LOCAL : 0, 0, &sf)) != ZZ9K_STATUS_OK)
        stage = "its surface";
    else if (!w16 && !sf.data)
        stage = "its surface (no 68k address)", st = -1;
    else if (w16 && (st = ZZ9KAllocSurfaceEx(dw, dh, ZZ9K_SURFACE_FORMAT_RGB565, 0, 0, &s16f)) != ZZ9K_STATUS_OK)
        stage = "its RGB565 surface";
    else if (w16 && !s16f.data)
        stage = "its RGB565 surface (no 68k address)", st = -1;
    amb_card_unlock();
    if (!stage) {
        memset(&bd, 0, sizeof(bd));
        memset(&r, 0, sizeof(r));
        bd.codec = ZZ9K_IMAGE_CODEC_JPEG;
        bd.output_mode = ZZ9K_IMAGE_OUTPUT_SURFACE;
        bd.dst_surface = sf.handle;
        bd.dst_width = dw;
        bd.dst_height = dh;
        bd.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
        bd.flags = ZZ9K_IMAGE_DECODE_FLAG_FIT | ZZ9K_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
        {
            ZZ9KRequest rq;
            if ((st = zz9k_request_image_session_begin(&rq, &bd)) == ZZ9K_STATUS_OK)
                st = img_call(&rq, ZZ9K_OP_IMAGE_SESSION_BEGIN, &r);
        }
        if (st != ZZ9K_STATUS_OK)
            stage = "beginning its session";
        else {
            began = TRUE;
            sess = r.session;
        }
    }
    quiet = time_ms();
    while (!stage) {
        ULONG n = (ULONG)flen - cpos < size ? (ULONG)flen - cpos : size, took;
        if (!ready || spos != cpos || staged != n) {   /* the file from where the card got to */
            if (cpos + n > wstart + wlen) {   /* the window moved on */
                /* Every read 4 KB aligned in the file and in the window, as
                 * one read of the whole file is: on the A3000 that went at
                 * 1.9 MB/s, and reads at odd places or into odd addresses
                 * (zzprobe 1.24, 1.26) at 0.6 MB/s, the filesystem copying
                 * through its own buffers. The window starts at the 4 KB
                 * boundary below cpos; what it already has from there is
                 * moved down, the rest read straight on (no Seek). */
                ULONG base = cpos & ~(ZZ_WIN_ALIGN - 1), end = wstart + wlen, keep = end > base ? end - base : 0;
                ULONG want = (ULONG)flen - end < wsize - keep ? (ULONG)flen - end : (wsize - keep) & ~(ZZ_WIN_ALIGN - 1);
                if (keep)
                    memmove(win, win + (base - wstart), keep);
                if (want && Read(fh, win + keep, (LONG)want) != (LONG)want) {
                    stage = "reading the file", st = -1;
                    break;
                }
                wstart = base;
                wlen = keep + want;
            }
            CopyMem(win + (cpos - wstart), (APTR)buf.data, n);
            CacheClearE((APTR)buf.data, n, CACRF_ClearD);
            spos = cpos;
            staged = n;
            ready = TRUE;
        }
        memset(&fd, 0, sizeof(fd));
        fd.session = sess;
        fd.src_handle = buf.handle;
        fd.src_offset = 0;
        fd.src_length = n;
        fd.flags = cpos + n >= (ULONG)flen ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0;
        {
            ZZ9KRequest rq;
            ULONG tf = time_ms();
            if ((st = zz9k_request_image_session_feed(&rq, &fd)) == ZZ9K_STATUS_OK)
                st = img_call(&rq, ZZ9K_OP_IMAGE_SESSION_FEED, &r);
            if (time_ms() - tf > feed_max)
                feed_max = time_ms() - tf;
            feeds++;
        }
        if (st != ZZ9K_STATUS_OK) {
            stage = "feeding its session";
            break;
        }
        took = r.bytes_consumed < staged ? r.bytes_consumed : staged;
        cpos += took;
        if (took || r.bytes_written)
            quiet = time_ms();
        if (r.state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE)
            break;
        if (r.state == ZZ9K_IMAGE_SESSION_STATE_ERROR)
            stage = "its session (an error)", st = -1;
        else if (!took && !r.bytes_written && r.state != ZZ9K_IMAGE_SESSION_STATE_TILE_READY &&
                 (cpos + staged >= (ULONG)flen || staged >= size))
            stage = "its session (the card took no more of the file)", st = -1;
        else if (time_ms() - quiet > ZZ_STALL_MS) {
            /* stuck: not to cost 3 s a picture from now on */
            stage = "its session (stuck: the card took none of the file for 3 s; the card's route is off "
                    "until Ambiance starts again)", st = -1;
            zz_view_on = FALSE;
        }
    }
    FreeVec(win);
    win = NULL;
    if (began) {
        ZZ9KRequest rq;
        if (zz9k_request_image_session_close(&rq, sess, 0) == ZZ9K_STATUS_OK)
            img_call(&rq, ZZ9K_OP_IMAGE_SESSION_CLOSE, NULL);
    }
    Close(fh);
    if (!stage) {
        ow = r.tile_width && r.tile_width <= dw ? r.tile_width : dw;
        oh = r.tile_height && r.tile_height <= dh ? r.tile_height : dh;
    }
    if (!stage && w16) {
        /* the card's scaler, 1:1, BGRA8888 to RGB565: half the bytes to read */
        ZZ9KScaleImageDesc sd;
        ZZ9KRequest rq;
        ZZ9KMailboxEntry rp;
        memset(&sd, 0, sizeof(sd));
        memset(&rp, 0, sizeof(rp));
        sd.src_surface = sf.handle;
        sd.dst_surface = s16f.handle;
        sd.src_w = sd.dst_w = ow;
        sd.src_h = sd.dst_h = oh;
        sd.filter = ZZ9K_SCALE_NEAREST;
        if ((st = zz9k_request_scale_image(&rq, &sd)) == ZZ9K_STATUS_OK &&
            (st = zz_call_ticks(&rq, &rp, ZZ_IMG_TICKS, ZZ_IMG_SPIN_MS)) == ZZ9K_STATUS_OK)
            st = (int)rp.status;
        if (st != ZZ9K_STATUS_OK) {
            char m[120];
            no565 = TRUE;               /* BGRA8888 read back from now on */
            sprintf(m, "the card would not convert to RGB565 (%s): BGRA8888 from now on", zz_name(st));
            amb_log("viewer", m);
            amb_card_lock();
            ZZ9KFreeSurface(s16f.handle);
            ZZ9KFreeSurface(sf.handle);
            ZZ9KFreeShared(buf.handle);
            amb_card_unlock();
            return zz_view_jpeg(path, gw, gh, tall, maxcol, pi, FALSE);
        }
    }
    t1 = time_ms();
    if (!stage) {
        if (w16)
            zz_card_wrote(s16f.data, s16f.pitch * oh);
        else
            zz_card_wrote(sf.data, sf.pitch * oh);
        if (maxcol <= THUMB_DEEP) {
            if ((t = AllocVec(sizeof(struct Thumb) + ow * oh * 4, MEMF_ANY | MEMF_CLEAR))) {
                ULONG *d;
                t->w = (UWORD)ow;
                t->h = (UWORD)oh;
                t->mod = (UWORD)ow;
                t->argb = d = (ULONG *)(t + 1);
                for (y = 0; y < oh && w16; y++) {
                    /* R5G6B5 words: half the bytes over the bus */
                    volatile const UWORD *row = (volatile const UWORD *)((volatile const UBYTE *)s16f.data +
                                                                         y * s16f.pitch);
                    for (x = 0; x < ow; x++) {
                        ULONG v = row[x], r, gg, b;
                        if (zz_565 == 2)
                            v = ((v >> 8) | (v << 8)) & 0xFFFF;
                        r = (v >> 11) & 31;
                        gg = (v >> 5) & 63;
                        b = v & 31;
                        *d++ = ((r << 3) | (r >> 2)) << 16 | ((gg << 2) | (gg >> 4)) << 8 | (b << 3) | (b >> 2);
                    }
                }
                for (y = 0; y < oh && !w16; y++) {
                    volatile const ULONG *row = (volatile const ULONG *)((volatile const UBYTE *)sf.data +
                                                                         y * sf.pitch);
                    for (x = 0; x < ow; x++)
                        *d++ = bgra_rgb(row[x]);    /* bytes B, G, R, A */
                }
            }
        } else {
            UBYTE *rgb = AllocVec(ow * oh * 3, MEMF_ANY), *d = rgb;
            if (rgb) {
                for (y = 0; y < oh; y++) {
                    volatile const ULONG *row = (volatile const ULONG *)((volatile const UBYTE *)sf.data +
                                                                         y * sf.pitch);
                    for (x = 0; x < ow; x++) {
                        ULONG v = row[x];
                        *d++ = (UBYTE)(v >> 8);
                        *d++ = (UBYTE)(v >> 16);
                        *d++ = (UBYTE)(v >> 24);
                    }
                }
                t = thumb_from_rgb(rgb, ow, oh, ow, oh, maxcol);
                FreeVec(rgb);
            }
        }
        if (!t)
            stage = "memory for its pixels", st = ZZ9K_STATUS_NO_MEMORY;
    }
    amb_card_lock();
    if (s16f.handle)
        ZZ9KFreeSurface(s16f.handle);
    if (sf.handle)
        ZZ9KFreeSurface(sf.handle);
    if (buf.handle)
        ZZ9KFreeShared(buf.handle);
    amb_card_unlock();
    if (stage) {
        view_say(path, stage, st, time_ms() - t0);
        return NULL;
    }
    {
        char m[300];
        sprintf(m, "%.60s, %lu x %lu%s: decoded on the card to %lu x %lu in %lu ms (%lu feeds, the longest "
                   "%lu ms), read back in %lu ms%s",
                FilePart((STRPTR)path), (unsigned long)w, (unsigned long)h,
                prog ? " (progressive)" : "", (unsigned long)ow,
                (unsigned long)oh, (unsigned long)(t1 - t0), (unsigned long)feeds, (unsigned long)feed_max,
                (unsigned long)(time_ms() - t1),
                maxcol > THUMB_DEEP ? " (and its colours cut)" : w16 ? " (RGB565)" : "");
        amb_log("viewer", m);
    }
    *pi = p;
    return t;                               /* (made to fit: not raw) */
}

const char *zz_name(int s)
{
    static const char *const names[] = { "OK", "queued", "busy", "unsupported", "bad request",
                                         "bad handle", "no memory", "timeout", "cancelled",
                                         "I/O error", "not found", "module missing",
                                         "module mismatch" };
    if (s >= 0 && s < (int)(sizeof(names) / sizeof(names[0])))
        return names[s];
    return s == ZZ9K_STATUS_INTERNAL_ERROR ? "internal error" : "?";
}

const char *zz_video_check(BOOL mpeg2, BOOL mp2)
{
    static char said;
    ZZ9KCaps caps;
    ZZ9KServiceInfo svc;
    ULONG need = ZZ9K_SERVICE_FLAG_VIDEO_DIRECT_OVERLAY | ZZ9K_SERVICE_FLAG_VIDEO_STREAMING_INPUT |
                 ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_SESSION | ZZ9K_SERVICE_FLAG_VIDEO_MPEG_PS |
                 (mpeg2 ? ZZ9K_SERVICE_FLAG_VIDEO_MPEG2 : ZZ9K_SERVICE_FLAG_VIDEO_MPEG1) |
                 (mp2 ? ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_MP2 : 0);
    char m[200];
    int s;
    if (!ZZ9KBase)
        return "zz9k.library is not there";
    memset(&caps, 0, sizeof(caps));
    memset(&svc, 0, sizeof(svc));
    amb_card_lock();
    s = ZZ9KQueryCaps(&caps);
    amb_card_unlock();
    if (s != ZZ9K_STATUS_OK) {
        sprintf(m, "the card's capabilities could not be read (%s)", zz_name(s));
        amb_log("zz9k", m);
        return "The ZZ9000 did not answer";
    }
    amb_card_lock();
    s = ZZ9KQueryService(ZZ9K_SERVICE_VIDEO, &svc);
    amb_card_unlock();
    if (!said) {
        said = 1;
        sprintf(m, "zz9k.library %ld.%ld, ABI %ld.%ld, firmware %08lx, caps %08lx; video service "
                   "%s, flags %08lx%s",
                (long)ZZ9KBase->lib_Version, (long)ZZ9KBase->lib_Revision, (long)caps.abi_major,
                (long)caps.abi_minor, (unsigned long)caps.firmware_version,
                (unsigned long)caps.capability_bits, s == ZZ9K_STATUS_OK ? "there" : zz_name(s),
                (unsigned long)svc.flags,
                ZZ9KBase->lib_Version == 2 && ZZ9KBase->lib_Revision >= 2 && ZZ9KBase->lib_Revision < 34
                    ? "; its replies waited for by Ambiance (before 2.34 its timeout is 250 polls)" : "");
        amb_log("zz9k", m);
    }
    if ((caps.capability_bits & (ZZ9K_CAP_VIDEO_DECODE | ZZ9K_CAP_MEDIA_SESSION)) !=
            (ZZ9K_CAP_VIDEO_DECODE | ZZ9K_CAP_MEDIA_SESSION) ||
        s != ZZ9K_STATUS_OK)
        return "This ZZ9000's firmware plays no video";
    if ((svc.flags & need) != need) {
        if (!(svc.flags & (mpeg2 ? ZZ9K_SERVICE_FLAG_VIDEO_MPEG2 : ZZ9K_SERVICE_FLAG_VIDEO_MPEG1)))
            return mpeg2 ? "This ZZ9000's firmware plays no MPEG-2" : "This ZZ9000's firmware plays no MPEG-1";
        if (mp2 && !(svc.flags & ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_MP2))
            return "This ZZ9000's firmware has no MPEG sound for video";
        return "This ZZ9000's firmware has no media sessions for video";
    }
    return NULL;
}

int zz_alloc(ULONG length, struct ZzBuf *b)
{
    ZZ9KSharedBuffer sb;
    int s;
    memset(&sb, 0, sizeof(sb));
    memset(b, 0, sizeof(*b));
    amb_card_lock();
    s = ZZ9KAllocShared(length, 64, ZZ9K_ALLOC_HOST_WINDOW, &sb);
    if (s == ZZ9K_STATUS_OK && !sb.data) {
        if (sb.handle)
            ZZ9KFreeShared(sb.handle);
        amb_card_unlock();
        return ZZ9K_STATUS_NO_MEMORY;
    }
    amb_card_unlock();
    b->handle = sb.handle;
    b->data = (volatile UBYTE *)sb.data;
    b->length = sb.length;
    return s;
}

/* Up to len bytes of fh into b, through fast RAM (a disk's DMA may not
 * reach the card's memory), the 68k's cache pushed out after; the bytes
 * read, or -1 */
LONG zz_read(BPTR fh, struct ZzBuf *b, ULONG len)
{
    ULONG got = 0;
    if (len > b->length)
        len = b->length;
    if (!b->bounce && !(b->bounce = AllocVec(ZZ_BOUNCE, MEMF_ANY)))
        return -1;
    while (got < len) {
        ULONG n = len - got < ZZ_BOUNCE ? len - got : ZZ_BOUNCE;
        LONG r = Read(fh, b->bounce, (LONG)n);
        if (r < 0)
            return got ? (LONG)got : -1;
        if (r)
            CopyMem(b->bounce, (APTR)(b->data + got), (ULONG)r);
        got += (ULONG)r;
        if ((ULONG)r < n)
            break;
    }
    if (got)
        CacheClearE((APTR)b->data, got, CACRF_ClearD);
    return (LONG)got;
}

void zz_fresh(const struct ZzBuf *b)
{
    if (b->data)
        CacheClearE((APTR)b->data, b->length, CACRF_ClearD);
}

void zz_free(struct ZzBuf *b)
{
    FreeVec(b->bounce);
    if (b->handle) {
        amb_card_lock();
        ZZ9KFreeShared(b->handle);
        amb_card_unlock();
    }
    memset(b, 0, sizeof(*b));
}

static void main_out(const ZZ9KMediaSessionMainResult *a, struct ZzMain *r)
{
    r->session = a->session;
    r->state = a->state;
    r->width = a->width;
    r->height = a->height;
    r->rate_num = a->frame_rate_num;
    r->rate_den = a->frame_rate_den;
    r->frame = a->frame_number;
    r->pts = a->video_pts;
    r->accepted = a->bytes_accepted;
    r->written = a->bytes_written;
    r->flags = a->flags;
}

/* A request sent and its reply waited for, under the card lock.
 * zz9k.library before 2.34 counted ZZ9KCall's timeout in tight polls of
 * the mailbox (250 of them: a few milliseconds), not in DOS ticks as it
 * says: a reply the card was a little slower over (a PRESENT while it
 * composes the last picture) came back as a timeout, and the video
 * stopped. There it is sent asynchronously and polled here: tightly at
 * first, then a tick at a time, ZZ_WAIT_TICKS at most (and taken off the
 * library's list if it never comes, as it lives on this stack). */
#define ZZ_SPIN_POLLS 400
#define ZZ_WAIT_TICKS 150                   /* (three seconds) */
static int zz_call_ticks(ZZ9KRequest *rq, ZZ9KMailboxEntry *reply, ULONG ticks, ULONG spin_ms);
static int zz_call(ZZ9KRequest *rq, ZZ9KMailboxEntry *reply)
{
    return zz_call_ticks(rq, reply, ZZ_WAIT_TICKS, 0);
}

/* spin_ms: polled for that long (and ZZ_SPIN_POLLS at least) before a tick
 * at a time; 0: ZZ_SPIN_POLLS (video's, as it was) */
#define ZZ_SPIN_MAX 200000UL                /* (polls: a bound if the clock stands still) */
static int zz_call_ticks(ZZ9KRequest *rq, ZZ9KMailboxEntry *reply, ULONG ticks, ULONG spin_ms)
{
    ZZ9KAsyncRequest a;
    ULONG n, done, slept = 0, t0 = spin_ms ? time_ms() : 0;
    int s;
    if (ZZ9KBase->lib_Version > 2 || ZZ9KBase->lib_Revision >= 34 || ZZ9KBase->lib_Revision < 2) {
        amb_card_lock();                   /* (2.34 on: its timeout is ticks; before 2.2 no cancel) */
        s = ZZ9KCall(rq, reply, ticks);
        amb_card_unlock();
        return s;
    }
    memset(&a, 0, sizeof(a));
    amb_card_lock();
    s = ZZ9KCallAsync(&a, rq, NULL, NULL);
    if (s != ZZ9K_STATUS_QUEUED) {
        amb_card_unlock();
        return s;
    }
    for (n = 0;; n++) {
        done = 0;
        s = ZZ9KPoll(8, &done);
        if (a.completed) {
            *reply = a.reply;
            s = a.status;
            break;
        }
        if (s != ZZ9K_STATUS_OK)
            break;
        if (n < ZZ_SPIN_POLLS || (spin_ms && n < ZZ_SPIN_MAX && time_ms() - t0 < spin_ms))
            continue;
        if (++slept > ticks) {
            s = ZZ9K_STATUS_TIMEOUT;
            break;
        }
        Delay(1);
    }
    if (a.queued && !a.completed)
        ZZ9KCancelAsync(&a);
    amb_card_unlock();
    return s;
}

/* A request sent, its main result read back. */
static int main_call(ZZ9KRequest *rq, UWORD op, struct ZzMain *r)
{
    ZZ9KMailboxEntry reply;
    ZZ9KMediaSessionMainResult res;
    int s;
    memset(&reply, 0, sizeof(reply));
    memset(r, 0, sizeof(*r));
    s = zz_call(rq, &reply);
    if (s != ZZ9K_STATUS_OK)
        return s;
    if ((s = zz9k_reply_media_session_main(&reply, op, &res)) != ZZ9K_STATUS_OK)
        return s;
    main_out(&res, r);
    return ZZ9K_STATUS_OK;
}

int zz_begin(ULONG w, ULONG h, BOOL mpeg2, int audio, const struct ZzBuf *pcm, struct ZzMain *r)
{
    ZZ9KMediaSessionBeginDesc d;
    ZZ9KRequest rq;
    int s;
    memset(&d, 0, sizeof(d));
    d.video_codec = mpeg2 ? ZZ9K_VIDEO_CODEC_MPEG2 : ZZ9K_VIDEO_CODEC_MPEG1;
    d.container = ZZ9K_VIDEO_CONTAINER_MPEG_PS;
    d.width = w;
    d.height = h;
    d.output_format = ZZ9K_VIDEO_OUTPUT_DIRECT_OVERLAY;
    d.audio_codec = audio == ZZ_AUDIO_AC3 ? ZZ9K_MEDIA_AUDIO_AC3
                    : audio == ZZ_AUDIO_MP2 ? ZZ9K_MEDIA_AUDIO_MP2 : ZZ9K_MEDIA_AUDIO_NONE;
    if (audio && pcm) {
        ULONG high = pcm->length - pcm->length / 4, low = pcm->length / 4;
        d.pcm_ring_handle = pcm->handle;
        d.pcm_ring_capacity = pcm->length;
        d.pcm_low_water_bytes = low;
        d.pcm_high_water_bytes = high;
    }
    if ((s = zz9k_request_media_session_begin(&rq, &d)) != ZZ9K_STATUS_OK)
        return s;
    return main_call(&rq, ZZ9K_OP_MEDIA_SESSION_BEGIN, r);
}

int zz_write(ULONG session, const struct ZzBuf *in, ULONG off, ULONG len, BOOL eof,
             struct ZzMain *r)
{
    ZZ9KMediaSessionWriteDesc d;
    ZZ9KRequest rq;
    int s;
    if (len)                            /* what the 68k wrote, out of its cache */
        CacheClearE((APTR)(in->data + off), len, CACRF_ClearD);
    memset(&d, 0, sizeof(d));
    d.session = session;
    d.src_handle = in->handle;
    d.src_offset = off;
    d.src_length = len;
    d.flags = eof ? ZZ9K_MEDIA_SESSION_WRITE_EOF : 0;
    if ((s = zz9k_request_media_session_write(&rq, &d)) != ZZ9K_STATUS_OK)
        return s;
    return main_call(&rq, ZZ9K_OP_MEDIA_SESSION_WRITE, r);
}

int zz_cmd(int op, ULONG session, ULONG flags, struct ZzMain *r)
{
    static const UWORD ops[] = { ZZ9K_OP_MEDIA_SESSION_DECODE, ZZ9K_OP_MEDIA_SESSION_PRESENT,
                                 ZZ9K_OP_MEDIA_SESSION_DISCARD, ZZ9K_OP_MEDIA_SESSION_CLOSE };
    ZZ9KRequest rq;
    int s;
    if ((s = zz9k_request_media_session_command(&rq, ops[op], session, flags)) != ZZ9K_STATUS_OK)
        return s;
    return main_call(&rq, ops[op], r);
}

int zz_audio_read(ULONG session, ZzU64 acknowledged, struct ZzAudio *r)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry reply;
    ZZ9KMediaSessionAudioResult a;
    int s;
    memset(r, 0, sizeof(*r));
    if ((s = zz9k_request_media_session_command_value(&rq, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ,
                                                      session, acknowledged, 0)) != ZZ9K_STATUS_OK)
        return s;
    memset(&reply, 0, sizeof(reply));
    s = zz_call(&rq, &reply);
    if (s != ZZ9K_STATUS_OK)
        return s;
    if ((s = zz9k_reply_media_session_audio(&reply, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ, &a)) !=
        ZZ9K_STATUS_OK)
        return s;
    r->session = a.session;
    r->state = a.state;
    r->rate = a.sample_rate;
    r->channels = a.channels;
    r->format = a.sample_format;
    r->produced = a.pcm_produced;
    r->acknowledged = a.pcm_acknowledged;
    r->pts = a.audio_pts;
    r->flags = a.flags;
    return ZZ9K_STATUS_OK;
}

int zz_audio_origin(ULONG session, ZzU64 *pts)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry reply;
    ZZ9KMediaSessionStatusResult res;
    int s;
    *pts = ZZ_NO_PTS;
    if ((s = zz9k_request_media_session_status(&rq, session, ZZ9K_MEDIA_STATUS_AUDIO, 0)) !=
        ZZ9K_STATUS_OK)
        return s;
    memset(&reply, 0, sizeof(reply));
    s = zz_call(&rq, &reply);
    if (s != ZZ9K_STATUS_OK)
        return s;
    if ((s = zz9k_reply_media_session_status(&reply, &res)) != ZZ9K_STATUS_OK)
        return s;
    *pts = res.value[3];
    return ZZ9K_STATUS_OK;
}
