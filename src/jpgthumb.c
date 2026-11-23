/*
 * Ambiance - JPEG thumbnails without a datatype
 * Copyright (c) 2026 Brian King, MIT
 *
 * A JPEG made into a thumbnail by jdec (src/jdec/jdec.c), built in:
 * decoded straight at 1/2, 1/4 or 1/8 of its size where that is still at
 * least the thumbnail's, from memory, with no datatype object, layout or
 * remapping. On OS 3.2 (68030) a 300 x 300 cover at 1/8 took 3 ms this
 * way against 16-26 ms through any JPEG datatype (zzprobe). As ilbm_thumb
 * does for ILBMs: the caller tries this first and falls back to the
 * datatypes when it returns NULL.
 *
 * What jdec does not decode (CMYK, 12-bit, arithmetic coding; a
 * progressive picture too big to keep at the scale asked) goes to the
 * datatypes as before, and is noted once per file in Ambiance-jpeg.txt
 * beside the program (or in RAM: on a read-only drive): where it came
 * from (a game, a picture, a music cover, a streamed one), why, and its
 * path.
 */
#include "amb.h"
#include "jdec/jdec.h"

#include <string.h>
#include <stdio.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

#define WHOLE_MAX (4L << 20)    /* a file up to this read whole; bigger, streamed */
/* Read whole only with this much left over in one piece besides (the
 * decoded picture comes next: 9 MB for a 12 MP photo at half size); else
 * streamed through a 64 KB window, a few big reads rather than jdec's 4 KB
 * ones (each a disk's latency: an A3000's SCSI read 1.9 MB/s at best) */
#define WHOLE_SPARE (4L << 20)
#define STREAM_BUF (64L * 1024)

static BOOL whole_ok(LONG len)
{
    return len > 0 && len <= WHOLE_MAX && AvailMem(MEMF_ANY | MEMF_LARGEST) >= (ULONG)len + WHOLE_SPARE;
}

/* ---- the files jdec turned down, once each */
static struct SignalSemaphore note_lock;
static BOOL note_ready;
#define NOTED_MAX 128
static ULONG noted[NOTED_MAX];  /* hashes of paths already noted this run */
static int nnoted;

void jpeg_thumb_init(void)
{
    InitSemaphore(&note_lock);
    note_ready = TRUE;
}

static ULONG path_hash(const char *s)
{
    ULONG h = 2166136261UL;
    while (*s)
        h = (h ^ (UBYTE)*s++) * 16777619UL;
    return h;
}

static void note(const char *kind, const char *why, const char *path)
{
    static const char *const files[] = { "PROGDIR:Ambiance-jpeg.txt", "RAM:Ambiance-jpeg.txt", NULL };
    char line[420], date[LEN_DATSTRING], clock[LEN_DATSTRING];
    struct DateTime dt;
    ULONG h = path_hash(path);
    BPTR fh = 0;
    int i;
    if (!note_ready)
        return;
    ObtainSemaphore(&note_lock);
    for (i = 0; i < nnoted && noted[i] != h; i++)
        ;
    if (i < nnoted) {                       /* noted already this run */
        ReleaseSemaphore(&note_lock);
        return;
    }
    if (nnoted < NOTED_MAX)
        noted[nnoted++] = h;
    DateStamp(&dt.dat_Stamp);
    dt.dat_Format = FORMAT_INT;
    dt.dat_Flags = 0;
    dt.dat_StrDay = NULL;
    dt.dat_StrDate = (STRPTR)date;
    dt.dat_StrTime = (STRPTR)clock;
    date[0] = clock[0] = 0;
    DateToStr(&dt);
    snprintf(line, sizeof(line), "%s %s\t%s\t%s\t%s\n", date, clock, kind, why, path);
    for (i = 0; files[i] && !fh; i++)
        if ((fh = Open((CONST_STRPTR)files[i], MODE_READWRITE)))
            Seek(fh, 0, OFFSET_END);
    if (fh) {
        Write(fh, line, strlen(line));
        Close(fh);
    }
    ReleaseSemaphore(&note_lock);
    amb_log("jpeg thumbnail, by datatype", line);
}

/* ---- jdec's input, memory and output */
struct JSrc {
    BPTR fh;                    /* streamed, or */
    const UBYTE *mem;           /* the file read whole */
    ULONG len, pos;
    UBYTE *buf;                 /* streamed: the window (STREAM_BUF), bpos of blen read */
    ULONG bpos, blen;
    BOOL nobuf;                 /* (no memory even for that: jdec's own 4 KB reads) */
    UBYTE *rgb;                 /* the picture decoded, w * 3 a row, or */
    struct Shrink *sk;          /* its rows made small as they come (to dw x dh) */
    ULONG w, h;
    ULONG dw, dh;               /* made smaller to this (0: kept whole: jpeg_bitmap's) */
};

static int32_t j_read(void *ctx, uint8_t *buf, int32_t n)
{
    struct JSrc *s = ctx;
    if (s->mem) {
        if ((ULONG)n > s->len - s->pos)
            n = (int32_t)(s->len - s->pos);
        CopyMem((APTR)(s->mem + s->pos), buf, (ULONG)n);
        s->pos += (ULONG)n;
        return n;
    }
    if (!s->buf && !s->nobuf && !(s->buf = AllocVec(STREAM_BUF, MEMF_ANY)))
        s->nobuf = TRUE;
    if (s->buf) {
        if (s->bpos >= s->blen) {
            LONG r = Read(s->fh, s->buf, STREAM_BUF);
            if (r <= 0)
                return (int32_t)r;
            s->blen = (ULONG)r;
            s->bpos = 0;
        }
        if ((ULONG)n > s->blen - s->bpos)
            n = (int32_t)(s->blen - s->bpos);
        CopyMem(s->buf + s->bpos, buf, (ULONG)n);
        s->bpos += (ULONG)n;
        return n;
    }
    return (int32_t)Read(s->fh, buf, n);
}

static void *j_alloc(void *ctx, uint32_t n)
{
    (void)ctx;
    return AllocVec(n, MEMF_ANY);
}

static void j_free(void *ctx, void *p)
{
    (void)ctx;
    FreeVec(p);
}

static int j_rows(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride)
{
    struct JSrc *s = ctx;
    int32_t i;
    if (s->sk) {
        shrink_rows(s->sk, rgb, y, n, stride);
        return 0;
    }
    for (i = 0; i < n && (ULONG)(y + i) < s->h; i++)
        CopyMem((APTR)(rgb + i * stride), s->rgb + (ULONG)(y + i) * s->w * 3, s->w * 3);
    return 0;
}

static const char *why_not(int r)
{
    switch (r) {
    case JD_EPROGRESSIVE: return "progressive";
    case JD_ECMYK: return "CMYK or YCCK";
    case JD_EUNSUPPORTED: return "not baseline (12-bit, arithmetic or multi-scan)";
    case JD_ETOOBIG: return "too big";
    case JD_ENOMEM: return "out of memory";
    default: return NULL;       /* not a JPEG, or damaged: the datatypes say so themselves */
    }
}

/* jd_decode at *scale into s->rgb (made here, s->w x s->h). A progressive
 * picture too big to keep at that scale (JD_ENOMEM: jdec keeps its
 * coefficients, JD_PROG_MAX at most) is opened again and decoded a scale
 * smaller: a 12-megapixel photo for a 1280 x 1024 slideshow then comes
 * 768 x 1020 for a 771 x 1024 place, the rest made up by the scaling
 * after, rather than going to a datatype that takes seconds or fails. */
/* said once a run: a picture decoded smaller for the memory */
static void mem_short(ULONG w, ULONG h)
{
    static BOOL said;
    char m[120];
    if (said)
        return;
    said = TRUE;
    sprintf(m, "not enough memory in one piece for %lu x %lu: decoded a scale smaller (said once)",
            (unsigned long)w, (unsigned long)h);
    amb_log("jpeg", m);
}

static int j_decode(JDec **pd, struct JSrc *s, const JDecIO *io, JDecInfo *info, int *scale)
{
    for (;;) {
        int32_t ow, oh;
        int r;
        jd_out_size(info, *scale, &ow, &oh);
        s->w = (ULONG)ow;
        s->h = (ULONG)oh;
        FreeVec(s->rgb);
        s->rgb = NULL;
        shrink_free(s->sk);
        s->sk = NULL;
        /* made smaller: as the rows come, the whole picture never held */
        if (s->dw && s->dw <= s->w && s->dh <= s->h)
            s->sk = shrink_new(s->w, s->h, s->dw, s->dh);
        if (!s->sk && !(s->rgb = AllocVec(s->w * s->h * 3, MEMF_ANY | MEMF_CLEAR))) {
            /* short of memory: a scale smaller (a quarter of the bytes), the
             * rest made up by the scaling after, rather than no picture */
            if (*scale >= 3)
                return JD_ENOMEM;
            (*scale)++;
            mem_short(s->w, s->h);
            continue;
        }
        r = jd_decode(*pd, *scale);
        if (r != JD_ENOMEM || !info->progressive || *scale >= 3)
            return r;
        jd_close(*pd);
        *pd = NULL;
        if (s->mem)
            s->pos = 0;
        else {
            Seek(s->fh, 0, OFFSET_BEGINNING);
            s->bpos = s->blen = 0;
        }
        if ((r = jd_open(pd, io, info)))
            return r;
        (*scale)++;
    }
}

/* view: the viewer's (w x h the screen, view_fit), else a thumbnail's
 * (w x h its box, pic_fit) */
static struct Thumb *jpeg_read(const char *path, WORD w, WORD h, BOOL tall, BOOL whole, LONG maxcol,
                               struct PicInfo *pi, const char *kind, BOOL view)
{
    struct JSrc s;
    struct Thumb *t = NULL;
    UBYTE *file = NULL, sig[3];
    JDecIO io;
    JDecInfo info;
    JDec *d = NULL;
    ULONG dw, dh;
    LONG len;
    int r, scale;
    const char *why;

    memset(&s, 0, sizeof(s));
    if (!(s.fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
        return NULL;
    if (Read(s.fh, sig, 3) != 3 || sig[0] != 0xFF || sig[1] != 0xD8 || sig[2] != 0xFF)
        goto out;                           /* not a JPEG: someone else's */
    Seek(s.fh, 0, OFFSET_END);
    len = Seek(s.fh, 0, OFFSET_BEGINNING);
    if (whole_ok(len) && (file = AllocVec((ULONG)len, MEMF_ANY))) {
        if (Read(s.fh, file, len) == len) {
            s.mem = file;
            s.len = (ULONG)len;
        } else {
            Seek(s.fh, 0, OFFSET_BEGINNING);
        }
    }
    io.ctx = &s;
    io.read = j_read;
    io.alloc = j_alloc;
    io.free = j_free;
    io.rows = j_rows;
    if ((r = jd_open(&d, &io, &info))) {
        if ((why = why_not(r)))
            note(kind ? kind : "picture", why, path);
        goto out;
    }

    /* the thumbnail's size from the picture's own, then the smallest
     * scale still at least that big */
    memset(pi, 0, sizeof(*pi));
    pi->w = (UWORD)info.width;
    pi->h = (UWORD)info.height;
    pi->depth = info.ncomp == 1 ? 8 : 24;
    strcpy(pi->fmt, "JPEG");
    strcpy(pi->cls, "jdec");                /* built in, as ilbm.c's are "" */
    if (view)
        view_fit((UWORD)w, (UWORD)h, tall, pi, &dw, &dh, NULL, 0);
    else
        pic_fit((ULONG)info.width, (ULONG)info.height, tall, w, h, whole, &dw, &dh);
    scale = jd_scale_for(&info, (int32_t)dw, (int32_t)dh);
    s.dw = dw;
    s.dh = dh;
    if ((r = j_decode(&d, &s, &io, &info, &scale))) {
        if ((why = why_not(r)))
            note(kind ? kind : "picture", why, path);
        goto out;
    }
    if (s.sk) {
        /* made small as it came: dw x dh, the rest as before at 1:1 */
        UBYTE *small = shrink_done(s.sk);
        s.sk = NULL;
        if (small) {
            t = thumb_from_rgb(small, dw, dh, dw, dh, maxcol);
            FreeVec(small);
        }
    } else if (view && maxcol <= THUMB_DEEP && (dw > s.w || dh > s.h)) {
        /* true colour, to be made larger: as decoded, the screen's scaler
         * then (averaging makes nothing larger) */
        ULONG i, n = s.w * s.h;
        if (!(t = AllocVec(sizeof(struct Thumb) + n * 4, MEMF_ANY | MEMF_CLEAR)))
            goto out;
        t->w = (UWORD)s.w;
        t->h = (UWORD)s.h;
        t->mod = (UWORD)s.w;
        t->argb = (ULONG *)(t + 1);
        for (i = 0; i < n; i++)
            t->argb[i] = ((ULONG)s.rgb[i * 3] << 16) | ((ULONG)s.rgb[i * 3 + 1] << 8) | s.rgb[i * 3 + 2];
        t->raw = 1;
    } else if (!(t = thumb_from_rgb(s.rgb, s.w, s.h, dw, dh, maxcol))) {
        goto out;
    }
out:
    if (d)
        jd_close(d);
    FreeVec(s.rgb);
    shrink_free(s.sk);
    FreeVec(s.buf);
    FreeVec(file);
    Close(s.fh);
    return t;
}

struct Thumb *jpeg_thumb(const char *path, WORD w, WORD h, BOOL tall, BOOL whole, LONG maxcol,
                         struct PicInfo *pi, const char *kind)
{
    return jpeg_read(path, w, h, tall, whole, maxcol, pi, kind, FALSE);
}

struct Thumb *jpeg_view_read(const char *path, UWORD gw, UWORD gh, BOOL tall, LONG maxcol,
                             struct PicInfo *pi)
{
    return jpeg_read(path, (WORD)gw, (WORD)gh, tall, FALSE, maxcol, pi, "picture (viewer)", TRUE);
}

/* The viewer's JPEG when no datatype takes it (on a true-colour screen):
 * decoded by jdec at the smallest of 1/1, 1/2, 1/4, 1/8 still at least
 * maxw x maxh, into a bitmap like friend (the screen's); *sw x *sh its
 * size, pi the picture's own. NULL if jdec cannot either. */
struct BitMap *jpeg_bitmap(const char *path, WORD maxw, WORD maxh, struct BitMap *friend,
                           ULONG *sw, ULONG *sh, struct PicInfo *pi)
{
    struct JSrc s;
    struct BitMap *bm = NULL;
    UBYTE *file = NULL, sig[4];
    JDecIO io;
    JDecInfo info;
    JDec *d = NULL;
    LONG len;
    int r, scale;
    const char *why;

    if (!CyberGfxBase)
        return NULL;
    memset(&s, 0, sizeof(s));
    if (!(s.fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
        return NULL;
    if (Read(s.fh, sig, 4) != 4 || sig[0] != 0xFF || sig[1] != 0xD8 || sig[2] != 0xFF)
        goto out;                           /* not a JPEG */
    Seek(s.fh, 0, OFFSET_END);
    len = Seek(s.fh, 0, OFFSET_BEGINNING);
    if (whole_ok(len) && (file = AllocVec((ULONG)len, MEMF_ANY))) {
        if (Read(s.fh, file, len) == len) {
            s.mem = file;
            s.len = (ULONG)len;
        } else {
            Seek(s.fh, 0, OFFSET_BEGINNING);
        }
    }
    io.ctx = &s;
    io.read = j_read;
    io.alloc = j_alloc;
    io.free = j_free;
    io.rows = j_rows;
    if ((r = jd_open(&d, &io, &info))) {
        if ((why = why_not(r)))
            note("picture (viewer)", why, path);
        goto out;
    }
    scale = jd_scale_for(&info, maxw, maxh);
    if ((r = j_decode(&d, &s, &io, &info, &scale))) {
        if ((why = why_not(r)))
            note("picture (viewer)", why, path);
        goto out;
    }
    if ((bm = AllocBitMap(s.w, s.h, GetBitMapAttr(friend, BMA_DEPTH), BMF_MINPLANES, friend))) {
        struct RastPort rp;
        InitRastPort(&rp);
        rp.BitMap = bm;
        WritePixelArray(s.rgb, 0, 0, (UWORD)(s.w * 3), &rp, 0, 0, (UWORD)s.w, (UWORD)s.h,
                        RECTFMT_RGB);
        WaitBlit();
        *sw = s.w;
        *sh = s.h;
        memset(pi, 0, sizeof(*pi));
        pi->w = (UWORD)info.width;
        pi->h = (UWORD)info.height;
        pi->depth = info.ncomp == 1 ? 8 : 24;
        strcpy(pi->fmt, "JPEG");
        strcpy(pi->cls, "jdec");
        {
            char m[96];             /* (what the datatypes may not have known it by) */
            sprintf(m, "%lu x %lu, its first marker FF%02lx, read at 1/%ld", (unsigned long)info.width,
                    (unsigned long)info.height, (unsigned long)sig[3], (long)(1 << scale));
            amb_log("viewer: jdec", m);
        }
    }
out:
    if (d)
        jd_close(d);
    FreeVec(s.rgb);
    FreeVec(s.buf);
    FreeVec(file);
    Close(s.fh);
    return bm;
}
