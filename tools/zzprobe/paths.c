/*
 * zzprobe - picture paths, end to end (included by zzprobe.c)
 * Copyright (c) 2026 Brian King, MIT
 *
 *   zzprobe PATHS [SLIDE] [NODT] [NOZZ] [PICS file file ...]
 *
 * SLIDE: full-screen slideshow sizes (800x600, 1024x768, 1280x1024) in
 * place of a tile, about a screen and HD.
 *
 * Each JPEG decoded every way Ambiance could, from the file to its pixels
 * at the size shown (fitted in the box, its shape kept), timed the same way
 * as the rest of the bench (best of up to 10 runs, mean in brackets):
 *
 *   own        jdec at the smallest of 1/1..1/8 still at least the size,
 *              then averaged down on the 68k (as Ambiance's thumbnails and
 *              viewer do): the file read, decoded, made small, in fast RAM
 *   zz9k dt    zz9k-picture.datatype's class (decoding on the card), its
 *              object from the file, the whole picture read back
 *              (PDTM_READPIXELARRAY), then averaged down on the 68k: what
 *              Ambiance's datatype route costs with the card's datatype
 *   jfif dt    the same through jfif.datatype: today's route for what jdec
 *              declines, for scale
 *   API back   zz9k.library: the file read into a shared buffer on the
 *              card, an image session fitting it (FIT | PRESERVE_ASPECT:
 *              libjpeg's 1/2..1/8, then the card's scaler) into a
 *              68k-visible BGRA8888 surface the size shown, read back
 *   API card   the same into a surface on the card alone: nothing read
 *              back (what drawing straight into the screen would cost,
 *              less the blit itself)
 *   API full   ZZ9KDecodeJpeg: the whole picture (the one-shot call never
 *              scales: the picture must fit the surface whole, and any
 *              flag is UNSUPPORTED) into a 68k-visible surface, read back
 *              whole and averaged down on the 68k
 *   API tiles  an image session in tile mode: the picture decoded on the
 *              card, its BGRA tiles copied out of a small shared buffer as
 *              each comes, averaged down on the 68k if not scaled. No
 *              surface: what Zorro II and the card's own datatype can use.
 *
 * BGRA8888 throughout: the card's native order, and the ZZ9000's 32-bit
 * RTG screens' (RGBFB_B8G8R8A8).
 *
 * Amiberry's emulated card (to 0f97f68, November 2026) has no 68k-visible
 * surfaces and no one-shot decode, so API back and API full stay empty
 * there until its patch for both (offered to BlitterStudio) is in; API card
 * and API tiles work.
 *
 * The pixels are checked: the API's and the datatype's against own's (the
 * mean difference of each colour, 0-255), so a wrong channel order or a
 * failed decode shows. On Amiberry the card's work runs on the PC: its
 * times say nothing about a real ZZ9000's; the 68k's parts (reading back,
 * averaging) are still the emulated 68k's.
 */
#include "proto/zz9k.h"
#include "zz9k/surface.h"
#include "zz9k/request.h"
#include "zz9k/reply.h"

/* A request and its reply, waited for by polling: zz9k.library before 2.34
 * counts its own call's timeout in a few hundred tight polls (milliseconds),
 * not ticks, so on Amiberry 8.6.0 (whose card works alongside the 68k) a
 * photo's feed came back as a timeout. Sent asynchronously here and polled:
 * tightly, then a tick at a time, 10 s at most (as Ambiance's zz9k.c). */
/* 1.21: polled for up to PB_SPIN_US before a tick at a time. On the real
 * card a session's call is answered from its second core a few ms later;
 * 1.20's 400 polls (about 3 ms on a 68060) then Delay(1) made every call
 * 20-40 ms (750 tiles: 29 s). The calls, and the time spent in them, are
 * counted for the columns' notes. */
#define PB_SPIN_US 50000UL
static ULONG pb_ncalls, pb_call_us, pb_call_max, pb_nslept;

static void pb_calls_reset(void)
{
    pb_ncalls = pb_call_us = pb_call_max = pb_nslept = 0;
}

static int pb_call(ZZ9KRequest *rq, ZZ9KMailboxEntry *reply)
{
    ZZ9KAsyncRequest a;
    ULONG done, t0 = now_us(), t, ticks = 0;
    int s;
    if (ZZ9KBase->lib_Version > 2 || ZZ9KBase->lib_Revision >= 34) {
        s = ZZ9KCall(rq, reply, 500);
        goto counted;
    }
    memset(&a, 0, sizeof(a));
    if ((s = ZZ9KCallAsync(&a, rq, NULL, NULL)) != ZZ9K_STATUS_QUEUED)
        return s;
    for (;;) {
        done = 0;
        s = ZZ9KPoll(8, &done);
        if (a.completed) {
            *reply = a.reply;
            s = a.status;
            break;
        }
        if (s != ZZ9K_STATUS_OK)
            break;
        if (now_us() - t0 >= PB_SPIN_US) {
            if (++ticks > 500) {
                s = ZZ9K_STATUS_TIMEOUT;
                break;
            }
            Delay(1);
        }
    }
    if (a.queued && !a.completed)
        ZZ9KCancelAsync(&a);
counted:
    t = now_us() - t0;
    pb_ncalls++;
    pb_call_us += t;
    if (t > pb_call_max)
        pb_call_max = t;
    if (ticks)
        pb_nslept++;
    return s;
}

static int pb_sess_begin(const ZZ9KImageSessionBeginDesc *d, ZZ9KImageSessionResult *r)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry rp;
    int s;
    memset(&rp, 0, sizeof(rp));
    if ((s = zz9k_request_image_session_begin(&rq, d)) != ZZ9K_STATUS_OK || (s = pb_call(&rq, &rp)) != ZZ9K_STATUS_OK)
        return s;
    return zz9k_reply_image_session_result(&rp, ZZ9K_OP_IMAGE_SESSION_BEGIN, r);
}

static int pb_sess_feed(const ZZ9KImageSessionFeedDesc *d, ZZ9KImageSessionResult *r)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry rp;
    int s;
    memset(&rp, 0, sizeof(rp));
    if ((s = zz9k_request_image_session_feed(&rq, d)) != ZZ9K_STATUS_OK || (s = pb_call(&rq, &rp)) != ZZ9K_STATUS_OK)
        return s;
    return zz9k_reply_image_session_result(&rp, ZZ9K_OP_IMAGE_SESSION_FEED, r);
}

static void pb_sess_close(ULONG sess)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry rp;
    if (zz9k_request_image_session_close(&rq, sess, 0) == ZZ9K_STATUS_OK)
        pb_call(&rq, &rp);
}

static int pb_decode_jpeg(const ZZ9KImageDecodeDesc *d, ZZ9KImageDecodeResult *r)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry rp;
    int s;
    memset(&rp, 0, sizeof(rp));
    if ((s = zz9k_request_decode_jpeg(&rq, d)) != ZZ9K_STATUS_OK || (s = pb_call(&rq, &rp)) != ZZ9K_STATUS_OK)
        return s;
    return zz9k_reply_image_decode_result(&rp, ZZ9K_OP_DECODE_JPEG, r);
}

struct Library *ZZ9KBase;
static BOOL pb_zzdt = TRUE;             /* the datatypes' columns (NODT: left out) */
static BOOL pb_nozz;                    /* NOZZ: zz9k.library left alone (no API columns) */

#define PB_N 7
enum { PB_OWN, PB_ZZDT, PB_JFIF, PB_APIBACK, PB_APICARD, PB_APIFULL, PB_APITILE };
static const char *const pb_name[PB_N] = { "own", "zz9k dt", "jfif dt", "API back", "API card", "API full",
                                           "API tiles" };

struct Box {
    const char *name;
    UWORD w, h;
};
static const struct Box pb_box_std[] = { { "tile", 160, 160 }, { "screen", 640, 512 }, { "HD", 1280, 720 } };
/* SLIDE: full-screen slideshow sizes (an AGA or RTG 800x600, 1024x768 and 1280x1024 screen) */
static const struct Box pb_box_slide[] = { { "800x600", 800, 600 }, { "1024x768", 1024, 768 },
                                           { "1280x1024", 1280, 1024 } };
static const struct Box *pb_box = pb_box_std;
#define PB_BOXES 3

/* the picture fitted in w x h, its shape kept, never made larger */
static void pb_fit(ULONG sw, ULONG sh, ULONG w, ULONG h, ULONG *dw, ULONG *dh)
{
    if (sw <= w && sh <= h) {
        *dw = sw;
        *dh = sh;
        return;
    }
    *dw = w;
    *dh = sh * w / sw;
    if (*dh > h) {
        *dh = h;
        *dw = sw * h / sh;
    }
    if (!*dw)
        *dw = 1;
    if (!*dh)
        *dh = 1;
}

/* src (sw x sh, 0xAARRGGBB) averaged down into dst (dw x dh): each small
 * pixel the mean of the big ones under it (a box), as thumb.c does */
/* Box averaging, src sw x sh to dst dw x dh. 1.21: the spans worked out
 * once, and a multiply by 65536/n in place of three divides a pixel (as
 * Ambiance's AGA path did already): on a real 68060 the divides were about
 * 4.5 us an output pixel, a third of "own" at 1280 wide */
static void pb_shrink(const ULONG *src, ULONG sw, ULONG sh, ULONG *dst, ULONG dw, ULONG dh)
{
    ULONG x, y, i, j, *xb = AllocVec((dw + 1) * sizeof(ULONG), MEMF_ANY), last_n = 0, rec = 0;
    if (!xb)
        return;
    for (x = 0; x <= dw; x++)
        xb[x] = x * sw / dw;
    for (y = 0; y < dh; y++) {
        ULONG y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (x = 0; x < dw; x++) {
            ULONG x0 = xb[x], x1 = xb[x + 1], r = 0, g = 0, b = 0, n, v;
            if (x1 <= x0)
                x1 = x0 + 1;
            for (j = y0; j < y1; j++) {
                const ULONG *p = src + j * sw;
                for (i = x0; i < x1; i++) {
                    ULONG c = p[i];
                    r += (c >> 16) & 255;
                    g += (c >> 8) & 255;
                    b += c & 255;
                }
            }
            n = (x1 - x0) * (y1 - y0);
            if (n != last_n) {
                last_n = n;
                rec = 65536UL / n + 1;
            }
            r = (r * rec) >> 16;
            g = (g * rec) >> 16;
            v = (b * rec) >> 16;
            dst[y * dw + x] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (v > 255 ? 255 : v);
        }
    }
    FreeVec(xb);
}

/* ---- own: jdec */
struct PbOwn {
    const char *path;
    ULONG w, h;                 /* the box */
    ULONG *out;                 /* dw x dh */
    ULONG dw, dh, iw, ih;       /* made; the picture's own size */
    ULONG *big;                 /* decoded, at its scale */
    ULONG bw, bh;
    int err;
    int scale;                  /* jdec's: 1/(1 << scale), the last run's */
    ULONG read_us, dec_us, avg_us;  /* the last run's parts */
};

/* jdec's one context: its reader's (the file in memory) and the rows' */
struct PbOwnIo {
    struct MemSrc m;
    struct PbOwn *c;
};

static int32_t pb_mread(void *ctx, uint8_t *buf, int32_t n)
{
    return mem_read(&((struct PbOwnIo *)ctx)->m, buf, n);
}

static int pb_rows(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride)
{
    struct PbOwn *c = ((struct PbOwnIo *)ctx)->c;
    int32_t i;
    ULONG x;
    for (i = 0; i < n && (ULONG)(y + i) < c->bh; i++) {
        const uint8_t *s = rgb + i * stride;
        ULONG *d = c->big + (ULONG)(y + i) * c->bw;
        for (x = 0; x < c->bw; x++, s += 3)
            d[x] = ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
    }
    return 0;
}

static LONG pb_own_once(void *p)
{
    struct PbOwn *c = p;
    struct PbOwnIo m;
    JDecIO io;
    JDecInfo info;
    JDec *d = NULL;
    LONG len = file_size(c->path);
    UBYTE *file;
    int32_t ow, oh;
    int r, s;
    ULONG t0 = now_us(), t1;
    if (len <= 0 || !(file = AllocVec((ULONG)len, MEMF_ANY)))
        return -1;
    if (!file_read(c->path, file, len)) {
        FreeVec(file);
        return -1;
    }
    t1 = now_us();
    c->read_us = t1 - t0;
    m.m.p = file;
    m.m.n = (ULONG)len;
    m.m.pos = 0;
    m.c = c;
    io.ctx = &m;
    io.read = pb_mread;
    io.alloc = mem_alloc;
    io.free = mem_free;
    io.rows = pb_rows;
    if ((r = jd_open(&d, &io, &info))) {
        c->err = r;
        FreeVec(file);
        return -1;
    }
    c->iw = (ULONG)info.width;
    c->ih = (ULONG)info.height;
    pb_fit(c->iw, c->ih, c->w, c->h, &c->dw, &c->dh);
    s = jd_scale_for(&info, (int32_t)c->dw, (int32_t)c->dh);
    jd_out_size(&info, s, &ow, &oh);
    c->bw = (ULONG)ow;
    c->bh = (ULONG)oh;
    if (!(c->big = AllocVec(c->bw * c->bh * 4, MEMF_ANY))) {
        jd_close(d);
        FreeVec(file);
        return -1;
    }
    r = jd_decode(d, s);
    jd_close(d);
    FreeVec(file);
    c->scale = s;
    c->dec_us = now_us() - t1;
    if (r) {
        c->err = r;
        FreeVec(c->big);
        c->big = NULL;
        return -1;
    }
    FreeVec(c->out);
    t1 = now_us();
    if ((c->out = AllocVec(c->dw * c->dh * 4, MEMF_ANY)))
        pb_shrink(c->big, c->bw, c->bh, c->out, c->dw, c->dh);
    c->avg_us = now_us() - t1;
    FreeVec(c->big);
    c->big = NULL;
    return c->out ? 0 : -1;
}

/* ---- a datatype's class: its object from the file, read back whole, made small */
struct PbDt {
    Class *cl;
    struct DataType *dtn;
    const char *path;
    ULONG w, h;
    ULONG *out, dw, dh, iw, ih;
    LONG err;
};

static LONG pb_dt_once(void *p)
{
    struct PbDt *c = p;
    struct BitMapHeader *bmh = NULL;
    struct pdtBlitPixelArray pa;
    ULONG *big;
    Object *o;
    /* DTA_Handle for DTST_FILE is a lock (datatypesclass Examine()s it and
     * opens the file from it), as NewDTObject passes, and the object's
     * from then on. A file handle here (as cls_once had it) was Examine()d
     * as a lock: DOS sent its packet to a nonsense port, PutMsg "called"
     * the port (PA 3) into the illegal-instruction vector, and SmartCrash's
     * trap handler, run in user mode, faulted (Exception 8, privilege
     * violation): every "hang" was SmartCrash's requester waiting */
    BPTR fh = Lock((CONST_STRPTR)c->path, SHARED_LOCK);
    if (!fh)
        return IoErr() ? IoErr() : -1;
    o = NewObject(c->cl, NULL, DTA_SourceType, DTST_FILE, DTA_Handle, (ULONG)fh,
                  DTA_DataType, (ULONG)c->dtn, DTA_Name, (ULONG)c->path,
                  PDTA_Remap, FALSE, PDTA_DestMode, PMODE_V43, TAG_END);
    if (!o) {
        LONG e = IoErr();
        return e ? e : -1;
    }
    /* no layout: a deep picture's pixels are read from the object as RGB,
     * as Ambiance's thumbnails do (gfx.c pic_decode_fit); laid out, PMODE_V43,
     * READPIXELARRAY failed */
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_END);

    if (!bmh || !bmh->bmh_Width || !bmh->bmh_Height) {
        DisposeObject(o);
        return -1;
    }
    c->iw = bmh->bmh_Width;
    c->ih = bmh->bmh_Height;
    pb_fit(c->iw, c->ih, c->w, c->h, &c->dw, &c->dh);
    if (!(big = AllocVec(c->iw * c->ih * 4, MEMF_ANY))) {
        DisposeObject(o);
        return ERROR_NO_FREE_STORE;
    }
    {
        /* as Ambiance's thumb.c reads them: RGB, a band of rows at a time
         * (as thumb.c's read_rows) */
        ULONG top, n, x, band = 32;
        UBYTE *rgb = AllocVec(c->iw * 3 * band, MEMF_ANY);
        if (!rgb) {
            FreeVec(big);
            DisposeObject(o);
            return ERROR_NO_FREE_STORE;
        }
        for (top = 0; top < c->ih; top += n) {
            ULONG k;
            n = c->ih - top < band ? c->ih - top : band;
            memset(&pa, 0, sizeof(pa));
            pa.MethodID = PDTM_READPIXELARRAY;
            pa.pbpa_PixelData = rgb;
            pa.pbpa_PixelFormat = PBPAFMT_RGB;
            pa.pbpa_PixelArrayMod = c->iw * 3;
            pa.pbpa_Left = 0;
            pa.pbpa_Top = top;
            pa.pbpa_Width = c->iw;
            pa.pbpa_Height = n;
            if (!DoMethodA(o, (Msg)&pa)) {
                FreeVec(rgb);
                FreeVec(big);
                DisposeObject(o);
                return -1;
            }
            for (k = 0; k < n; k++) {
                const UBYTE *s = rgb + k * c->iw * 3;
                ULONG *d = big + (top + k) * c->iw;
                for (x = 0; x < c->iw; x++, s += 3)
                    d[x] = ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
            }
        }
        FreeVec(rgb);
    }
    DisposeObject(o);
    FreeVec(c->out);
    if ((c->out = AllocVec(c->dw * c->dh * 4, MEMF_ANY)))
        pb_shrink(big, c->iw, c->ih, c->out, c->dw, c->dh);
    FreeVec(big);
    return c->out ? 0 : ERROR_NO_FREE_STORE;
}

/* ---- zz9k.library into a surface (BGRA8888: the card's own, and the
 * ZZ9000's 32-bit screens'). Fitting a picture to a size is an image
 * session's (FIT | PRESERVE_ASPECT); ZZ9KDecodeJpeg never scales (the
 * picture must fit the surface whole; any flag is UNSUPPORTED), so:
 *   PBA_BACK  a session fitting it into a 68k-visible surface, read back
 *   PBA_CARD  the same into a surface on the card alone, nothing read back
 *   PBA_FULL  ZZ9KDecodeJpeg whole into a 68k-visible surface the picture's
 *             size, read back whole and averaged down on the 68k */
enum { PBA_BACK, PBA_CARD, PBA_FULL };
struct PbApi {
    const char *path;
    ULONG dw, dh;               /* the size shown (own's) */
    ULONG iw, ih;               /* the picture's (own's): PBA_FULL's surface */
    int mode;
    ULONG *out;                 /* dw x dh, 0x00RRGGBB (read back) */
    ULONG ow, oh;               /* what the card made */
    int status;
    const char *stage;          /* the call that failed */
    char note[120];             /* what a stuck session said */
    ULONG ncalls, call_us, call_max, nslept, feeds;  /* the last run's calls to the card */
};

/* ---- the file to the card a chunk at a time, through one shared buffer
 * (PB_CHUNK): what Ambiance would do with a camera's 5-10 MB JPEG (one
 * shared buffer holds at most about 4 MB on Amiberry, and a Zorro II card's
 * window far less). Each feed offers what is left of the chunk loaded; once
 * it is used up, the next is read in; the last carries EOF. */
#define PB_CHUNK (256 * 1024)

/* ---- the 68k's data cache and the card. A 68040/060's copyback cache
 * keeps the last lines the 68k wrote until they are pushed out, and keeps
 * what it read of the card's memory after the card changes it; the ZZ9000
 * docs ask for its memory to be mapped Writethrough or NoCache (MMULib), but
 * a program cannot count on that. And a disk's DMA may not reach Zorro III
 * memory at all (the A3000's SCSI). So: a file goes through fast RAM, the
 * 68k copies it to the card and pushes its cache out; before the 68k reads
 * what the card wrote, its own lines are dropped. */
#define PB_BOUNCE (32 * 1024)
static UBYTE *pb_bounce;

static BOOL pb_read_card(BPTR fh, volatile UBYTE *dst, ULONG len)
{
    volatile UBYTE *d = dst;
    ULONG left = len;
    if (!pb_bounce && !(pb_bounce = AllocVec(PB_BOUNCE, MEMF_ANY)))
        return FALSE;
    while (left) {
        ULONG n = left < PB_BOUNCE ? left : PB_BOUNCE;
        if (Read(fh, pb_bounce, (LONG)n) != (LONG)n)
            return FALSE;
        CopyMem(pb_bounce, (APTR)d, n);
        d += n;
        left -= n;
    }
    CacheClearE((APTR)dst, len, CACRF_ClearD);
    return TRUE;
}

static void pb_card_wrote(volatile const void *p, ULONG len)
{
    CacheClearE((APTR)p, len, CACRF_ClearD);
}

/* a session that takes none of the file and makes nothing for this long
 * is stuck (on a real A3000 with the cache in the way: 100,000 feeds, 78 s) */
#define PB_STALL_US 3000000UL

/* The file through one staging buffer, as the card's own zz9k-jpeg feeds
 * it (zzprobe 1.20): 32 KB, every feed from its start; what the card has
 * not taken moved to the front and the rest topped up from the file. (Up
 * to 1.19: 256 KB chunks fed at an offset; on a real A3000 the sessions
 * took none of it.) */
#define PB_STAGE (32 * 1024)
struct PbFeed {
    BPTR fh;
    LONG len;                   /* the file's */
    ZZ9KSharedBuffer buf;
    ULONG size;                 /* the buffer's */
    ULONG foff, buffered;       /* the file read so far; held in the buffer */
    ULONG feeds;
    UBYTE *mirror;              /* the buffer's bytes in fast RAM (1.21): moved here, not on the card */
    BOOL moved;                 /* the front moved: all of it to the card again */
};

static void pb_feed_close(struct PbFeed *f);

static int pb_feed_open(struct PbFeed *f, const char *path, ULONG size)
{
    int st;
    memset(f, 0, sizeof(*f));
    if ((f->len = file_size(path)) <= 0 || !(f->fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
        return ZZ9K_STATUS_IO_ERROR;
    f->size = (ULONG)f->len < size ? (ULONG)f->len : size;
    if ((st = ZZ9KAllocShared(f->size, 16, ZZ9K_ALLOC_HOST_WINDOW, &f->buf)) != ZZ9K_STATUS_OK) {
        Close(f->fh);
        f->fh = 0;
        f->buf.handle = 0;
    } else if (size <= PB_STAGE && !(f->mirror = AllocVec(f->size, MEMF_ANY))) {
        pb_feed_close(f);
        st = ZZ9K_STATUS_NO_MEMORY;
    }
    return st;
}

/* the buffer topped up and the next feed described; FALSE: the file could not be read */
static BOOL pb_feed_next(struct PbFeed *f, ZZ9KImageSessionFeedDesc *fd, ULONG session)
{
    ULONG space = f->size - f->buffered, rest = (ULONG)f->len - f->foff, n = rest < space ? rest : space;
    if (n && Read(f->fh, f->mirror + f->buffered, (LONG)n) != (LONG)n)
        return FALSE;
    if (f->moved) {                         /* what the card left, now at the front, and the new */
        CopyMem(f->mirror, (APTR)f->buf.data, f->buffered + n);
        CacheClearE((APTR)f->buf.data, f->buffered + n, CACRF_ClearD);
        f->moved = FALSE;
    } else if (n) {
        CopyMem(f->mirror + f->buffered, (APTR)((volatile UBYTE *)f->buf.data + f->buffered), n);
        CacheClearE((APTR)((volatile UBYTE *)f->buf.data + f->buffered), n, CACRF_ClearD);
    }
    f->foff += n;
    f->buffered += n;
    memset(fd, 0, sizeof(*fd));
    fd->session = session;
    fd->src_handle = f->buf.handle;
    fd->src_offset = 0;
    fd->src_length = f->buffered;
    fd->flags = f->foff >= (ULONG)f->len ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0;
    f->feeds++;
    return TRUE;
}

/* what the card took: the rest moved to the front */
static void pb_feed_took(struct PbFeed *f, ULONG took)
{
    if (took >= f->buffered) {
        f->buffered = 0;
        return;
    }
    if (took) {
        f->buffered -= took;
        memmove(f->mirror, f->mirror + took, f->buffered);
        f->moved = TRUE;
    }
}

/* the card made no progress with a full buffer or the whole file: it never will */
static BOOL pb_feed_stuck(const struct PbFeed *f, const ZZ9KImageSessionResult *r, char *note, ULONG len)
{
    if (r->bytes_consumed || r->bytes_written || r->state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE ||
        r->state == ZZ9K_IMAGE_SESSION_STATE_TILE_READY)
        return FALSE;
    if (f->foff < (ULONG)f->len && f->buffered < f->size)
        return FALSE;
    snprintf(note, len, "the session took nothing (state %lu, flags %lu, %lu bytes offered, file %lu of %lu "
             "read, feed %lu)", (unsigned long)r->state, (unsigned long)r->flags, (unsigned long)f->buffered,
             (unsigned long)f->foff, (unsigned long)f->len, (unsigned long)f->feeds);
    return TRUE;
}

static void pb_feed_close(struct PbFeed *f)
{
    FreeVec(f->mirror);
    if (f->buf.handle)
        ZZ9KFreeShared(f->buf.handle);
    if (f->fh)
        Close(f->fh);
    memset(f, 0, sizeof(*f));
}

/* rows of BGRA8888 at src (pitch bytes apart) to 0x00RRGGBB */
static void pb_bgra_rows(volatile const UBYTE *src, ULONG pitch, ULONG w, ULONG h, ULONG *dst)
{
    ULONG x, y;
    for (y = 0; y < h; y++) {
        const ULONG *s = (const ULONG *)(src + y * pitch);
        for (x = 0; x < w; x++) {
            ULONG v = s[x];
            *dst++ = ((v >> 8) & 0xFF) << 16 | ((v >> 16) & 0xFF) << 8 | (v >> 24);
        }
    }
}

static LONG pb_api_once(void *p)
{
    struct PbApi *c = p;
    ZZ9KSurface surf;
    struct PbFeed f;
    ULONG sw = c->mode == PBA_FULL ? c->iw : c->dw, sh = c->mode == PBA_FULL ? c->ih : c->dh;
    LONG ok = -1;
    if (!sw || !sh)
        return -1;
    pb_calls_reset();
    memset(&surf, 0, sizeof(surf));
    c->stage = "ZZ9KAllocShared";
    /* the one-shot call takes the whole file in one buffer; a session, chunks */
    if ((c->status = pb_feed_open(&f, c->path, c->mode == PBA_FULL ? 0x7fffffffUL : PB_STAGE)) !=
        ZZ9K_STATUS_OK) {
        if (c->mode == PBA_FULL)
            c->stage = "ZZ9KAllocShared (the whole file, for the one-shot call)";
        return -1;
    }
    c->stage = "ZZ9KAllocSurface";
    if ((c->status = ZZ9KAllocSurface(sw, sh, ZZ9K_SURFACE_FORMAT_BGRA8888,
                                      c->mode == PBA_CARD ? ZZ9K_SURFACE_FLAG_ARM_LOCAL : 0, &surf)) !=
        ZZ9K_STATUS_OK) {
        pb_feed_close(&f);
        return -1;
    }
    if (c->mode != PBA_CARD && !surf.data) {
        c->stage = "the surface (not 68k-visible)";
        c->status = ZZ9K_STATUS_UNSUPPORTED;
        goto out;
    }
    if (c->mode == PBA_FULL) {
        ZZ9KImageDecodeDesc d;
        ZZ9KImageDecodeResult res;
        if (!pb_read_card(f.fh, (volatile UBYTE *)f.buf.data, (ULONG)f.len)) {
            c->stage = "reading the file";
            c->status = ZZ9K_STATUS_IO_ERROR;
            goto out;
        }
        memset(&d, 0, sizeof(d));
        memset(&res, 0, sizeof(res));
        d.src_handle = f.buf.handle;
        d.src_length = (ULONG)f.len;
        d.dst_surface = surf.handle;
        d.dst_width = sw;
        d.dst_height = sh;
        d.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
        c->stage = "ZZ9KDecodeJpeg";
        if ((c->status = pb_decode_jpeg(&d, &res)) != ZZ9K_STATUS_OK)
            goto out;
        c->ow = res.width;
        c->oh = res.height;
    } else {
        ZZ9KImageSessionBeginDesc bd;
        ZZ9KImageSessionFeedDesc fd;
        ZZ9KImageSessionResult r;
        ULONG sess, quiet = now_us();
        memset(&bd, 0, sizeof(bd));
        memset(&r, 0, sizeof(r));
        bd.codec = ZZ9K_IMAGE_CODEC_JPEG;
        bd.output_mode = ZZ9K_IMAGE_OUTPUT_SURFACE;
        bd.dst_surface = surf.handle;
        bd.dst_width = sw;
        bd.dst_height = sh;
        bd.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
        bd.flags = ZZ9K_IMAGE_DECODE_FLAG_FIT | ZZ9K_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
        c->stage = "ZZ9KImageSessionBegin";
        if ((c->status = pb_sess_begin(&bd, &r)) != ZZ9K_STATUS_OK)
            goto out;
        sess = r.session;                   /* (a failed feed's result is not to be trusted for it) */
        for (;;) {
            if (!pb_feed_next(&f, &fd, sess)) {
                c->stage = "reading the file";
                c->status = ZZ9K_STATUS_IO_ERROR;
                break;
            }
            c->stage = "ZZ9KImageSessionFeed";
            if ((c->status = pb_sess_feed(&fd, &r)) != ZZ9K_STATUS_OK)
                break;
            if (pb_feed_stuck(&f, &r, c->note, sizeof(c->note))) {
                c->stage = c->note;
                c->status = ZZ9K_STATUS_TIMEOUT;
                break;
            }
            pb_feed_took(&f, r.bytes_consumed);
            if (r.bytes_consumed)
                quiet = now_us();
            if (r.state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
                c->ow = r.tile_width ? r.tile_width : sw;
                c->oh = r.tile_height ? r.tile_height : sh;
                c->stage = NULL;
                break;
            }
            if (r.state == ZZ9K_IMAGE_SESSION_STATE_ERROR) {
                c->stage = "the session (state ERROR)";
                c->status = ZZ9K_STATUS_IO_ERROR;
                break;
            }
            if (now_us() - quiet > PB_STALL_US) {
                c->stage = "the session (stuck: none of the file taken for 3 s)";
                c->status = ZZ9K_STATUS_TIMEOUT;
                break;
            }
        }
        pb_sess_close(sess);
        if (c->stage)
            goto out;
    }
    c->stage = NULL;
    ok = 0;
    if (c->mode != PBA_CARD) {
        ULONG ow = c->ow > sw ? sw : c->ow, oh = c->oh > sh ? sh : c->oh, *tmp = NULL;
        FreeVec(c->out);
        c->out = NULL;
        if (!ow || !oh || !(c->out = AllocVec(c->dw * c->dh * 4, MEMF_ANY)) ||
            !(tmp = AllocVec(ow * oh * 4, MEMF_ANY))) {
            c->stage = "memory for the read-back";
            c->status = ZZ9K_STATUS_NO_MEMORY;
            ok = -1;
        } else {
            pb_card_wrote(surf.data, surf.pitch * oh);
            pb_bgra_rows((volatile const UBYTE *)surf.data, surf.pitch, ow, oh, tmp);
            if (ow == c->dw && oh == c->dh)
                CopyMem(tmp, c->out, ow * oh * 4);
            else
                pb_shrink(tmp, ow, oh, c->out, c->dw, c->dh);
        }
        FreeVec(tmp);
    }
out:
    ZZ9KFreeSurface(surf.handle);
    c->ncalls = pb_ncalls;
    c->call_us = pb_call_us;
    c->call_max = pb_call_max;
    c->nslept = pb_nslept;
    c->feeds = f.feeds;
    pb_feed_close(&f);
    return ok;
}

/* each step's start, into PROGDIR:paths-progress.txt (closed each time:
 * a run that hangs shows where) */
static void pb_step(const char *file, const char *box, const char *path)
{
    char l[160];
    BPTR fh = Open((CONST_STRPTR)"PROGDIR:paths-progress.txt", MODE_READWRITE);
    if (!fh)
        return;
    Seek(fh, 0, OFFSET_END);
    snprintf(l, sizeof(l), "%lu %.50s %s %s\n", (unsigned long)(now_us() / 1000), FilePart((STRPTR)file), box,
             path);
    Write(fh, l, strlen(l));
    Close(fh);
    /* in the Shell too (not in zzprobe.txt): a row only shows once all of
     * its columns are done, minutes apart on a real 68060 */
    snprintf(l, sizeof(l), "    ...%.40s at %s: %s\n", FilePart((STRPTR)file), box, path);
    PutStr((CONST_STRPTR)l);
}

static void pb_say(const char *s)
{
    BPTR fh;
    sayf(s);
    if ((fh = Open((CONST_STRPTR)"PROGDIR:paths-progress.txt", MODE_READWRITE))) {
        Seek(fh, 0, OFFSET_END);
        Write(fh, (APTR)s, strlen(s));
        Write(fh, (APTR)"\n", 1);
        Close(fh);
    }
}

static const char *zz_status_name(int s)
{
    static const char *const n[] = { "OK", "queued", "busy", "unsupported", "bad request", "bad handle",
                                     "no memory", "timeout", "cancelled", "I/O error", "not found",
                                     "module missing", "module mismatch" };
    return s >= 0 && s < (int)(sizeof(n) / sizeof(n[0])) ? n[s] : "error";
}

/* ---- zz9k.library: an image session, its output in tiles (BGRA) into a
 * shared buffer, copied out as each comes (no surface: what the card's
 * datatype and Zorro II can use). The size shown asked for (dst): if the
 * card scales (RESULT_SCALED) the tiles are that size, else the whole
 * picture's, averaged down on the 68k */
#define PB_TSTRIDE (4 * 8192)                /* (a row of up to 8192 pixels: a 24 MP photo's 6000) */
#define PB_TROWS 4
struct PbTile {
    const char *path;
    ULONG dw, dh;
    ULONG *out;
    ULONG iw, ih, ow, oh, ntiles, flags;
    int status;
    const char *stage;
    BOOL said;
    char note[120];
    ULONG ncalls, call_us, call_max, nslept, feeds;
};

static LONG pb_tile_once(void *p)
{
    struct PbTile *c = p;
    ZZ9KSharedBuffer tb;
    struct PbFeed f;
    ZZ9KImageSessionBeginDesc bd;
    ZZ9KImageSessionFeedDesc fd;
    ZZ9KImageSessionResult r;
    ULONG *canvas = NULL, cw = 0, ch = 0, guard = 0, sess = 0, quiet = now_us();
    LONG ok = -1;
    BOOL eof = FALSE, open = FALSE;
    pb_calls_reset();
    memset(&tb, 0, sizeof(tb));
    c->stage = "ZZ9KAllocShared (file)";
    if ((c->status = pb_feed_open(&f, c->path, PB_STAGE)) != ZZ9K_STATUS_OK)
        return -1;
    c->stage = "ZZ9KAllocShared (tiles)";
    if ((c->status = ZZ9KAllocShared(PB_TSTRIDE * PB_TROWS, 16, ZZ9K_ALLOC_HOST_WINDOW, &tb)) != ZZ9K_STATUS_OK)
        goto out;
    memset(&bd, 0, sizeof(bd));
    bd.codec = ZZ9K_IMAGE_CODEC_JPEG;
    bd.output_mode = ZZ9K_IMAGE_OUTPUT_TILE_BUFFER;
    bd.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
    bd.tile_handle = tb.handle;
    bd.tile_stride = PB_TSTRIDE;
    bd.tile_rows = PB_TROWS;
    bd.dst_width = c->dw;                   /* (the size shown: scaled to it, if the card does) */
    bd.dst_height = c->dh;
    memset(&r, 0, sizeof(r));
    c->stage = "ZZ9KImageSessionBegin";
    if ((c->status = pb_sess_begin(&bd, &r)) != ZZ9K_STATUS_OK)
        goto out;
    open = TRUE;
    sess = r.session;                       /* (a failed feed's result is not to be trusted for it) */
    c->ntiles = 0;
    c->flags = 0;
    while (guard++ < 1000000) {
        if (!pb_feed_next(&f, &fd, sess)) {
            c->stage = "reading the file";
            goto out;
        }
        if (fd.flags)
            eof = TRUE;
        c->stage = "ZZ9KImageSessionFeed";
        if ((c->status = pb_sess_feed(&fd, &r)) != ZZ9K_STATUS_OK)
            goto out;
        if (pb_feed_stuck(&f, &r, c->note, sizeof(c->note))) {
            c->stage = c->note;
            c->status = ZZ9K_STATUS_TIMEOUT;
            goto out;
        }
        pb_feed_took(&f, r.bytes_consumed);
        if (r.bytes_consumed || r.state == ZZ9K_IMAGE_SESSION_STATE_TILE_READY)
            quiet = now_us();
        else if (now_us() - quiet > PB_STALL_US) {
            c->stage = "the session (stuck: none of the file taken, no tile, for 3 s)";
            c->status = ZZ9K_STATUS_TIMEOUT;
            goto out;
        }
        c->flags |= r.flags;
        if (r.image_width && !c->iw) {
            c->iw = r.image_width;
            c->ih = r.image_height;
        }
        if (r.state == ZZ9K_IMAGE_SESSION_STATE_TILE_READY && r.tile_width && r.tile_height) {
            ULONG y, x;
            if (!canvas) {
                /* the output's size: the size asked for if scaled, else the picture's */
                cw = (r.flags & ZZ9K_IMAGE_SESSION_RESULT_SCALED) ? c->dw : c->iw;
                ch = (r.flags & ZZ9K_IMAGE_SESSION_RESULT_SCALED) ? c->dh : c->ih;
                if (!cw || !ch || !(canvas = AllocVec(cw * ch * 4, MEMF_ANY | MEMF_CLEAR))) {
                    c->stage = "the canvas";
                    goto out;
                }
            }
            c->ntiles++;
            pb_card_wrote(tb.data, PB_TSTRIDE * r.tile_height);
            for (y = 0; y < r.tile_height && r.tile_y + y < ch; y++) {
                const ULONG *s = (const ULONG *)((volatile UBYTE *)tb.data + y * PB_TSTRIDE);
                ULONG *d = canvas + (r.tile_y + y) * cw + r.tile_x;
                for (x = 0; x < r.tile_width && r.tile_x + x < cw; x++) {
                    ULONG v = s[x];             /* B G R A bytes: to 0x00RRGGBB */
                    d[x] = ((v >> 8) & 0xFF) << 16 | ((v >> 16) & 0xFF) << 8 | (v >> 24);
                }
            }
        } else if (r.state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
            ok = 0;
            break;
        } else if (r.state == ZZ9K_IMAGE_SESSION_STATE_ERROR) {
            c->stage = "the session (state ERROR)";
            goto out;
        } else if (r.state == ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT && eof) {
            c->stage = "the session (wants input after EOF)";
            goto out;
        }
    }
    if (ok == 0 && canvas) {
        c->ow = cw;
        c->oh = ch;
        FreeVec(c->out);
        if ((c->out = AllocVec(c->dw * c->dh * 4, MEMF_ANY))) {
            if (cw == c->dw && ch == c->dh)
                CopyMem(canvas, c->out, cw * ch * 4);
            else
                pb_shrink(canvas, cw, ch, c->out, c->dw, c->dh);
        } else {
            ok = -1;
        }
    } else if (ok == 0) {
        c->stage = "the session (no tiles)";
        ok = -1;
    }
    if (!ok)
        c->stage = NULL;
out:
    if (open)
        pb_sess_close(sess);
    FreeVec(canvas);
    if (tb.handle)
        ZZ9KFreeShared(tb.handle);
    c->ncalls = pb_ncalls;
    c->call_us = pb_call_us;
    c->call_max = pb_call_max;
    c->nslept = pb_nslept;
    c->feeds = f.feeds;
    pb_feed_close(&f);
    return ok;
}

/* what the card says about surfaces, and which allocations it takes */
static void pb_surface_probe(void)
{
    static const ULONG fmts[] = { ZZ9K_SURFACE_FORMAT_ARGB8888, ZZ9K_SURFACE_FORMAT_BGRA8888,
                                  ZZ9K_SURFACE_FORMAT_RGB565 };
    static const char *const fname[] = { "ARGB8888", "BGRA8888", "RGB565" };
    static const ULONG flags[] = { 0, ZZ9K_SURFACE_FLAG_CPU_VISIBLE, ZZ9K_SURFACE_FLAG_ARM_LOCAL,
                                   ZZ9K_SURFACE_FLAG_CPU_VISIBLE | ZZ9K_SURFACE_FLAG_SHARED_BUFFER };
    static const char *const flname[] = { "none", "cpu-visible", "arm-local", "cpu-visible+shared" };
    ZZ9KCaps caps;
    ZZ9KServiceInfo sv;
    char l[200];
    int f, g, st;
    memset(&caps, 0, sizeof(caps));
    st = ZZ9KQueryCaps(&caps);
    snprintf(l, sizeof(l), "  caps (%s): bits $%08lx, surfaces %s, max %lu, image decode %s, scale %s; "
             "host-window heap %lu; firmware $%08lx, ABI %lu.%lu",
             zz_status_name(st), (unsigned long)caps.capability_bits,
             (caps.capability_bits & ZZ9K_CAP_SURFACES) ? "yes" : "no", (unsigned long)caps.max_surfaces,
             (caps.capability_bits & ZZ9K_CAP_IMAGE_DECODE) ? "yes" : "no",
             (caps.capability_bits & ZZ9K_CAP_IMAGE_SCALE) ? "yes" : "no",
             (unsigned long)caps.host_window_heap_size, (unsigned long)caps.firmware_version,
             (unsigned long)caps.abi_major, (unsigned long)caps.abi_minor);
    pb_say(l);
    memset(&sv, 0, sizeof(sv));
    st = ZZ9KQueryService(ZZ9K_SERVICE_SURFACE, &sv);
    snprintf(l, sizeof(l), "  surface service (%s): version %lu, flags $%08lx, %.20s", zz_status_name(st),
             (unsigned long)sv.version, (unsigned long)sv.flags, sv.name);
    pb_say(l);
    for (f = 0; f < 3; f++)
        for (g = 0; g < 4; g++) {
            ZZ9KSurface su;
            int a, b2;
            ULONG pitch = 0, len = 0;
            memset(&su, 0, sizeof(su));
            a = ZZ9KAllocSurface(160, 120, fmts[f], flags[g], &su);
            if (a == ZZ9K_STATUS_OK)
                ZZ9KFreeSurface(su.handle);
            zz9k_surface_layout(160, 120, fmts[f], (uint32_t *)&pitch, (uint32_t *)&len);
            memset(&su, 0, sizeof(su));
            b2 = ZZ9KAllocSurfaceEx(160, 120, fmts[f], flags[g], pitch, &su);
            snprintf(l, sizeof(l), "  surface 160x120 %-8s %-18s: AllocSurface %s, Ex(pitch %lu) %s%s",
                     fname[f], flname[g], zz_status_name(a), (unsigned long)pitch, zz_status_name(b2),
                     b2 == ZZ9K_STATUS_OK && su.data ? " (68k can read it)" : "");
            if (b2 == ZZ9K_STATUS_OK)
                ZZ9KFreeSurface(su.handle);
            pb_say(l);
        }
}

/* the mean difference of each colour, 0-255 x 10 */
static void pb_diff(const ULONG *a, const ULONG *b, ULONG n, ULONG *dr, ULONG *dg, ULONG *db)
{
    ULONG i, r = 0, g = 0, bl = 0;
    for (i = 0; i < n; i++) {
        LONG x = (LONG)((a[i] >> 16) & 255) - (LONG)((b[i] >> 16) & 255);
        LONG y = (LONG)((a[i] >> 8) & 255) - (LONG)((b[i] >> 8) & 255);
        LONG z = (LONG)(a[i] & 255) - (LONG)(b[i] & 255);
        r += x < 0 ? -x : x;
        g += y < 0 ? -y : y;
        bl += z < 0 ? -z : z;
    }
    *dr = n ? r * 10 / n : 0;
    *dg = n ? g * 10 / n : 0;
    *db = n ? bl * 10 / n : 0;
}

static struct Library *pb_class(const char *name, Class **cl)
{
    char path[64];
    struct Library *lib;
    snprintf(path, sizeof(path), "datatypes/%s", name);
    *cl = NULL;
    if ((lib = OpenLibrary((CONST_STRPTR)path, 0)))
        *cl = ObtainEngine(lib);
    if (lib && !*cl) {
        CloseLibrary(lib);
        lib = NULL;
    }
    return lib;
}

static void pb_check(char *out, LONG max, const char *what, const ULONG *a, const ULONG *own, ULONG n)
{
    ULONG r, g, b;
    if (!a || !own) {
        snprintf(out, max, "%s -", what);
        return;
    }
    pb_diff(a, own, n, &r, &g, &b);
    snprintf(out, max, "%s %ld.%ld/%ld.%ld/%ld.%ld", what, (long)(r / 10), (long)(r % 10), (long)(g / 10),
             (long)(g % 10), (long)(b / 10), (long)(b % 10));
}

/* ---- does what the 68k writes reach the card, and what the card writes
 * the 68k? 64 KB through two shared buffers, the card copying or filling
 * (MEM_COPY, MEM_FILL); and a file read straight into the card's memory, as
 * zzprobe 1.17 and Ambiance 0.9.4's first card route did (a disk's DMA may
 * not reach Zorro III memory) */
#define PB_CO_LEN (64 * 1024)

static int pb_mem_op(int copy, ULONG dst, ULONG src, ULONG len, UBYTE v)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry rp;
    int st;
    memset(&rp, 0, sizeof(rp));
    st = copy ? zz9k_request_mem_copy(&rq, dst, 0, src, 0, len) : zz9k_request_mem_fill(&rq, dst, 0, len, v);
    if (st == ZZ9K_STATUS_OK && (st = pb_call(&rq, &rp)) == ZZ9K_STATUS_OK)
        st = (int)rp.status;
    return st;
}

static ULONG pb_co_diff(volatile const UBYTE *a, const UBYTE *ref, UBYTE want, ULONG n)
{
    ULONG i, d = 0;
    for (i = 0; i < n; i++)
        if (a[i] != (ref ? ref[i] : want))
            d++;
    return d;
}

static void pb_co_line(const char *what, int st, ULONG bad, const char *ok, const char *why)
{
    char l[200];
    if (st != ZZ9K_STATUS_OK)
        snprintf(l, sizeof(l), "    %-34s the card said %s", what, zz_status_name(st));
    else if (!bad)
        snprintf(l, sizeof(l), "    %-34s %s", what, ok);
    else
        snprintf(l, sizeof(l), "    %-34s %lu bytes of %lu wrong (%s)", what, (unsigned long)bad,
                 (unsigned long)PB_CO_LEN, why);
    pb_say(l);
}

static void pb_cache_check(const char *file)
{
    ZZ9KSharedBuffer a, b;
    UBYTE *ref = NULL;
    ULONG i, cacr, sum = 0;
    int st;
    char l[200];
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    cacr = CacheControl(0, 0);
    snprintf(l, sizeof(l), "  Cache check, 64 KB through the card (68k data cache %s, copyback %s):",
             (cacr & CACRF_EnableD) ? "on" : "off", (cacr & CACRF_CopyBack) ? "on" : "off");
    pb_say(l);
    if (ZZ9KAllocShared(PB_CO_LEN, 16, ZZ9K_ALLOC_HOST_WINDOW, &a) != ZZ9K_STATUS_OK ||
        ZZ9KAllocShared(PB_CO_LEN, 16, ZZ9K_ALLOC_HOST_WINDOW, &b) != ZZ9K_STATUS_OK || !a.data || !b.data ||
        !(ref = AllocVec(PB_CO_LEN, MEMF_ANY))) {
        pb_say("    two 64 KB shared buffers could not be had: left out");
        goto out;
    }
    /* 1: the 68k writes, no flush; the card copies a to b */
    for (i = 0; i < PB_CO_LEN; i++)
        ref[i] = (UBYTE)(i * 7 + 0x35) ^ (UBYTE)(i >> 9);
    pb_mem_op(0, a.handle, 0, PB_CO_LEN, 0);
    pb_mem_op(0, b.handle, 0, PB_CO_LEN, 0);
    CacheClearE((APTR)a.data, PB_CO_LEN, CACRF_ClearD);
    CacheClearE((APTR)b.data, PB_CO_LEN, CACRF_ClearD);
    CopyMem(ref, (APTR)a.data, PB_CO_LEN);
    st = pb_mem_op(1, b.handle, a.handle, PB_CO_LEN, 0);
    CacheClearE((APTR)b.data, PB_CO_LEN, CACRF_ClearD);
    pb_co_line("68k writes, no cache flush:", st, pb_co_diff(b.data, ref, 0, PB_CO_LEN),
               "all reach the card", "held in the 68k's copyback cache: map the ZZ9000 Writethrough or NoCache");
    /* 2: the same, flushed */
    for (i = 0; i < PB_CO_LEN; i++)
        ref[i] ^= 0xFF;
    CopyMem(ref, (APTR)a.data, PB_CO_LEN);
    CacheClearE((APTR)a.data, PB_CO_LEN, CACRF_ClearD);
    st = pb_mem_op(1, b.handle, a.handle, PB_CO_LEN, 0);
    CacheClearE((APTR)b.data, PB_CO_LEN, CACRF_ClearD);
    pb_co_line("68k writes, cache flushed:", st, pb_co_diff(b.data, ref, 0, PB_CO_LEN),
               "all reach the card", "not the cache: the card or the bus");
    /* 3: the 68k reads b (into its cache), the card fills it, the 68k reads it again unflushed */
    for (i = 0; i < PB_CO_LEN; i += 4)
        sum += *(volatile const ULONG *)((volatile const UBYTE *)b.data + i);
    st = pb_mem_op(0, b.handle, 0, PB_CO_LEN, 0xA5);
    pb_co_line("card writes, 68k reads unflushed:", st, pb_co_diff(b.data, NULL, 0xA5, PB_CO_LEN),
               "the 68k sees them", "the 68k read its own cache");
    CacheClearE((APTR)b.data, PB_CO_LEN, CACRF_ClearD);
    pb_co_line("card writes, 68k reads flushed:", st, pb_co_diff(b.data, NULL, 0xA5, PB_CO_LEN),
               "the 68k sees them", "not the cache: the card or the bus");
    /* 4: the file read straight into card memory (the disk's own way: DMA, perhaps) */
    if (file) {
        BPTR fh = Open((CONST_STRPTR)file, MODE_OLDFILE);
        LONG n = 0, m = 0;
        if (fh) {
            pb_mem_op(0, a.handle, 0, PB_CO_LEN, 0);
            CacheClearE((APTR)a.data, PB_CO_LEN, CACRF_ClearD);
            n = Read(fh, (APTR)a.data, PB_CO_LEN);
            CacheClearE((APTR)a.data, PB_CO_LEN, CACRF_ClearD);
            Seek(fh, 0, OFFSET_BEGINNING);
            m = Read(fh, ref, PB_CO_LEN);
            Close(fh);
        }
        if (n == PB_CO_LEN && m == PB_CO_LEN) {
            st = pb_mem_op(1, b.handle, a.handle, PB_CO_LEN, 0);
            CacheClearE((APTR)b.data, PB_CO_LEN, CACRF_ClearD);
            pb_co_line("a file read straight onto the card:", st, pb_co_diff(b.data, ref, 0, PB_CO_LEN),
                       "the card has it", "the disk's DMA does not reach the card's memory");
        } else {
            pb_say("    a file read straight onto the card:  the file could not be read (64 KB)");
        }
    }
    (void)sum;
out:
    FreeVec(ref);
    if (a.handle)
        ZZ9KFreeShared(a.handle);
    if (b.handle)
        ZZ9KFreeShared(b.handle);
}

static void bench_paths(const char *const *files, int nfiles)
{
    struct Library *zzlib = NULL, *zzdtl, *jfifl;
    Class *zzcl, *jfifcl;
    ZZ9KServiceInfo svc;
    BOOL api = FALSE;
    char line[320], c[PB_N][24];
    int fi;
    ULONG b;

    heading("Bench: picture paths, file to pixels at the size shown (ms, best of up to 10, mean)");
    if (!pb_nozz && (zzlib = OpenLibrary((CONST_STRPTR)"zz9k.library", 2))) {
        ZZ9KBase = zzlib;
        memset(&svc, 0, sizeof(svc));
        if (zzlib->lib_Version > 2 || zzlib->lib_Revision >= ZZ9K_LIBRARY_MIN_REVISION_TYPED_IMAGE_DECODE) {
            if (ZZ9KQueryService(ZZ9K_SERVICE_IMAGE, &svc) == ZZ9K_STATUS_OK &&
                (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_JPEG_BASELINE))
                api = TRUE;
            snprintf(line, sizeof(line), "  zz9k.library %ld.%ld; image service %s, flags $%08lx (JPEG scaling %s, "
                     "progressive %s, WebP %s, to the screen %s)",
                     (long)zzlib->lib_Version, (long)zzlib->lib_Revision, api ? "there" : "not there",
                     (unsigned long)svc.flags, (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_JPEG_SCALING) ? "yes" : "no",
                     (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_JPEG_PROGRESSIVE) ? "yes" : "no",
                     (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_WEBP) ? "yes" : "no",
                     (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT) ? "yes" : "no");
        } else {
            snprintf(line, sizeof(line), "  zz9k.library %ld.%ld: too old for ZZ9KDecodeJpeg", (long)zzlib->lib_Version,
                     (long)zzlib->lib_Revision);
        }
        pb_say(line);
        if (api) {
            pb_surface_probe();
            pb_cache_check(nfiles ? files[0] : NULL);
        }
    } else {
        pb_say("  zz9k.library 2 not there: the API's columns are left out");
    }
    zzdtl = NULL;
    zzcl = NULL;
    if (pb_zzdt)
        zzdtl = pb_class("zz9k-picture.datatype", &zzcl);
    jfifl = pb_class("jfif.datatype", &jfifcl);
    if (!zzdtl && pb_zzdt)
        pb_say("  zz9k-picture.datatype not there: its column is left out");
    if (!pb_zzdt)
        pb_say("  The datatypes' columns left out (NODT)");

    for (fi = 0; fi < nfiles && !brk_check(); fi++) {
        const char *path = files[fi];
        struct DataType *dtn = NULL;
        BPTR lock;
        if (!exists(path)) {
            snprintf(line, sizeof(line), "  %.60s: not found", path);
            pb_say(line);
            continue;
        }
        if ((lock = Lock((CONST_STRPTR)path, SHARED_LOCK))) {
            dtn = ObtainDataTypeA(DTST_FILE, (APTR)lock, NULL);
            UnLock(lock);
        }
        for (b = 0; b < PB_BOXES && !brk_check(); b++) {
            struct PbOwn own;
            struct PbDt zd, jd;
            struct PbApi ab, ac, af;
            struct PbTile at;
            ULONG best[PB_N], mean[PB_N];
            char chk[5][48];
            int k;
            memset(best, 0, sizeof(best));
            memset(mean, 0, sizeof(mean));
            memset(&own, 0, sizeof(own));
            own.path = path;
            own.w = pb_box[b].w;
            own.h = pb_box[b].h;
            pb_step(path, pb_box[b].name, "own");
            if (timed(pb_own_once, &own, NULL, &best[PB_OWN], &mean[PB_OWN]))
                best[PB_OWN] = 0;
            memset(&zd, 0, sizeof(zd));
            memset(&jd, 0, sizeof(jd));
            zd.path = jd.path = path;
            zd.w = jd.w = pb_box[b].w;
            zd.h = jd.h = pb_box[b].h;
            zd.dtn = jd.dtn = dtn;
            zd.cl = zzcl;
            jd.cl = jfifcl;
            pb_step(path, pb_box[b].name, "jfif dt");
            if (jfifcl && pb_zzdt && (jd.err = timed(pb_dt_once, &jd, NULL, &best[PB_JFIF], &mean[PB_JFIF])))
                best[PB_JFIF] = 0;
            memset(&ab, 0, sizeof(ab));
            memset(&ac, 0, sizeof(ac));
            memset(&af, 0, sizeof(af));
            memset(&at, 0, sizeof(at));
            if (api && own.dw) {
                ab.path = ac.path = af.path = path;
                ab.dw = ac.dw = af.dw = own.dw;
                ab.dh = ac.dh = af.dh = own.dh;
                af.iw = own.iw;
                af.ih = own.ih;
                ab.mode = PBA_BACK;
                ac.mode = PBA_CARD;
                af.mode = PBA_FULL;
                pb_step(path, pb_box[b].name, "API back");
                if (timed(pb_api_once, &ab, NULL, &best[PB_APIBACK], &mean[PB_APIBACK]))
                    best[PB_APIBACK] = 0;
                pb_step(path, pb_box[b].name, "API card");
                if (timed(pb_api_once, &ac, NULL, &best[PB_APICARD], &mean[PB_APICARD]))
                    best[PB_APICARD] = 0;
                pb_step(path, pb_box[b].name, "API full");
                if (timed(pb_api_once, &af, NULL, &best[PB_APIFULL], &mean[PB_APIFULL]))
                    best[PB_APIFULL] = 0;
                memset(&at, 0, sizeof(at));
                at.path = path;
                at.dw = own.dw;
                at.dh = own.dh;
                pb_step(path, pb_box[b].name, "API tiles");
                if (timed(pb_tile_once, &at, NULL, &best[PB_APITILE], &mean[PB_APITILE]))
                    best[PB_APITILE] = 0;
                snprintf(line, sizeof(line), "      API tiles: %s%s%s; picture %ldx%ld, made %ldx%ld on the card (%s), "
                         "%ld tiles", at.stage ? at.stage : "ok", at.stage ? " said " : "",
                         at.stage ? zz_status_name(at.status) : "", (long)at.iw, (long)at.ih, (long)at.ow,
                         (long)at.oh, (at.flags & ZZ9K_IMAGE_SESSION_RESULT_SCALED) ? "scaled" : "whole",
                         (long)at.ntiles);
                pb_say(line);
            }
            pb_step(path, pb_box[b].name, "zz9k dt");
            if (zzcl && pb_zzdt && (zd.err = timed(pb_dt_once, &zd, NULL, &best[PB_ZZDT], &mean[PB_ZZDT])))
                best[PB_ZZDT] = 0;
            if (b == 0) {
                snprintf(line, sizeof(line), "  %.50s, %ld x %ld", FilePart((STRPTR)path),
                         (long)(own.iw ? own.iw : zd.iw), (long)(own.ih ? own.ih : zd.ih));
                pb_say(line);
                snprintf(line, sizeof(line), "    %-16s %-11s %-11s %-11s %-11s %-11s %-11s %-11s", "shown at",
                         pb_name[0], pb_name[1], pb_name[2], pb_name[3], pb_name[4], pb_name[5], pb_name[6]);
                pb_say(line);
            }
            for (k = 0; k < PB_N; k++)
                cell(c[k], best[k], mean[k]);
            {
                char atx[24];
                snprintf(atx, sizeof(atx), "%s %ldx%ld", pb_box[b].name, (long)own.dw, (long)own.dh);
                snprintf(line, sizeof(line), "    %-16s %-11s %-11s %-11s %-11s %-11s %-11s %-11s", atx, c[0], c[1],
                         c[2], c[3], c[4], c[5], c[6]);
                pb_say(line);
            }
            if (!best[PB_OWN] && own.err) {
                snprintf(line, sizeof(line), "      own: %s", jd_why(own.err));
                pb_say(line);
            }
            if (api) {
                const struct PbApi *const w[3] = { &ab, &ac, &af };
                int i;
                for (i = 0; i < 3; i++)
                    if (!best[PB_APIBACK + i]) {
                        snprintf(line, sizeof(line), "      %s: %s said %s", pb_name[PB_APIBACK + i],
                                 w[i]->stage ? w[i]->stage : "read back", zz_status_name(w[i]->status));
                        pb_say(line);
                    }
            }
            if (zzcl && pb_zzdt && !best[PB_ZZDT]) {
                why(zd.err, chk[0], sizeof(chk[0]));
                snprintf(line, sizeof(line), "      zz9k dt: %s", chk[0]);
                pb_say(line);
            }
            /* the pixels against own's (when both are the same size) */
            pb_check(chk[0], sizeof(chk[0]), "zz9k dt",
                     zd.dw == own.dw && zd.dh == own.dh ? zd.out : NULL, own.out, own.dw * own.dh);
            pb_check(chk[1], sizeof(chk[1]), "jfif dt",
                     jd.dw == own.dw && jd.dh == own.dh ? jd.out : NULL, own.out, own.dw * own.dh);
            pb_check(chk[2], sizeof(chk[2]), "API back", ab.out, own.out, own.dw * own.dh);
            pb_check(chk[3], sizeof(chk[3]), "full", af.out, own.out, own.dw * own.dh);
            pb_check(chk[4], sizeof(chk[4]), "tiles", at.out, own.out, own.dw * own.dh);
            snprintf(line, sizeof(line), "      differs from own's, R/G/B 0-255: %s; %s; %s; %s; %s", chk[0], chk[1],
                     chk[2], chk[3], chk[4]);
            pb_say(line);
            /* 1.21: where the time went (the last run of each) */
            if (best[PB_OWN]) {
                snprintf(line, sizeof(line), "      own's parts: the file %lu ms, jdec at 1/%d to %lux%lu %lu ms, "
                         "averaging %lu ms", (unsigned long)(own.read_us / 1000), 1 << own.scale,
                         (unsigned long)own.bw, (unsigned long)own.bh, (unsigned long)(own.dec_us / 1000),
                         (unsigned long)(own.avg_us / 1000));
                pb_say(line);
            }
            if (api) {
                static const char *const nm[3] = { "API back", "API card", "API tiles" };
                const ULONG nc[3] = { ab.ncalls, ac.ncalls, at.ncalls }, cu[3] = { ab.call_us, ac.call_us, at.call_us },
                            cm[3] = { ab.call_max, ac.call_max, at.call_max },
                            ns[3] = { ab.nslept, ac.nslept, at.nslept }, fe[3] = { ab.feeds, ac.feeds, at.feeds };
                int i;
                for (i = 0; i < 3; i++)
                    if (nc[i]) {
                        snprintf(line, sizeof(line), "      %s's calls to the card: %lu (%lu feeds), %lu ms in "
                                 "them, the longest %lu ms, %lu slept a tick", nm[i], (unsigned long)nc[i],
                                 (unsigned long)fe[i], (unsigned long)(cu[i] / 1000),
                                 (unsigned long)(cm[i] / 1000), (unsigned long)ns[i]);
                        pb_say(line);
                    }
            }
            FreeVec(own.out);
            FreeVec(zd.out);
            FreeVec(jd.out);
            FreeVec(ab.out);
            FreeVec(af.out);
            FreeVec(at.out);
        }
        if (dtn)
            ReleaseDataType(dtn);
    }
    pb_say("  own: jdec at 1/1..1/8, then averaged on the 68k. dt: the class's object from the file,");
    pb_say("  read back whole, averaged on the 68k. API back: a session fitting it on the card into a");
    pb_say("  68k-visible BGRA surface, read back; API card: the same left on the card (straight-to-");
    pb_say("  screen's cost, less the blit). API full: ZZ9KDecodeJpeg whole (it never scales), read");
    pb_say("  back whole, averaged on the 68k. API tiles: a session's tiles through a shared buffer.");
    pb_say("  Every column includes reading the file. On Amiberry the card's own work runs on the PC.");
    pb_say("  The API's columns read the file into fast RAM, copy it to the card and flush the cache;");
    pb_say("  sessions are fed as the card's own zz9k-jpeg feeds them: 32 KB, each feed from its start.");
    FreeVec(pb_bounce);
    pb_bounce = NULL;
    if (zzdtl)
        CloseLibrary(zzdtl);
    if (jfifl)
        CloseLibrary(jfifl);
    if (zzlib)
        CloseLibrary(zzlib);
    ZZ9KBase = NULL;
}
