/*
 * AmigaMCE - JPEG thumbnails without a datatype
 * Copyright (c) 2026 Brian King, MIT
 *
 * A JPEG made into a thumbnail by jdec (datatypes/jpeg/jdec.c), built in:
 * decoded straight at 1/2, 1/4 or 1/8 of its size where that is still at
 * least the thumbnail's, from memory, with no datatype object, layout or
 * remapping. On OS 3.2 (68030) a 300 x 300 cover at 1/8 took 3 ms this
 * way against 16-26 ms through any JPEG datatype (zzprobe). As ilbm_thumb
 * does for ILBMs: the caller tries this first and falls back to the
 * datatypes when it returns NULL.
 *
 * What jdec does not decode (progressive, CMYK, 12-bit, arithmetic
 * coding) goes to the datatypes as before, and is noted once per file in
 * AmigaMCE-jpeg.txt beside the program (or in RAM: on a read-only drive):
 * where it came from (a game, a picture, a music cover, a streamed one),
 * why, and its path, so we know which of them need progressive support.
 */
#include "amce.h"
#include "../datatypes/jpeg/jdec.h"

#include <string.h>
#include <stdio.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define WHOLE_MAX (4L << 20)    /* a file up to this read whole; bigger, streamed */

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
    static const char *const files[] = { "PROGDIR:AmigaMCE-jpeg.txt", "RAM:AmigaMCE-jpeg.txt", NULL };
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
    amce_log("jpeg thumbnail, by datatype", line);
}

/* ---- jdec's input, memory and output */
struct JSrc {
    BPTR fh;                    /* streamed, or */
    const UBYTE *mem;           /* the file read whole */
    ULONG len, pos;
    UBYTE *rgb;                 /* the picture decoded, w * 3 a row */
    ULONG w, h;
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

struct Thumb *jpeg_thumb(const char *path, WORD w, WORD h, BOOL tall, BOOL whole, LONG maxcol,
                         struct PicInfo *pi, const char *kind)
{
    struct JSrc s;
    struct Thumb *t = NULL;
    UBYTE *file = NULL, sig[3];
    JDecIO io;
    JDecInfo info;
    JDec *d = NULL;
    ULONG dw, dh;
    LONG len;
    int32_t ow, oh;
    int r, scale;
    const char *why;

    memset(&s, 0, sizeof(s));
    if (!(s.fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
        return NULL;
    if (Read(s.fh, sig, 3) != 3 || sig[0] != 0xFF || sig[1] != 0xD8 || sig[2] != 0xFF)
        goto out;                           /* not a JPEG: someone else's */
    Seek(s.fh, 0, OFFSET_END);
    len = Seek(s.fh, 0, OFFSET_BEGINNING);
    if (len > 0 && len <= WHOLE_MAX && (file = AllocVec((ULONG)len, MEMF_ANY))) {
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
    pic_fit((ULONG)info.width, (ULONG)info.height, tall, w, h, whole, &dw, &dh);
    scale = jd_scale_for(&info, (int32_t)dw, (int32_t)dh);
    jd_out_size(&info, scale, &ow, &oh);
    s.w = (ULONG)ow;
    s.h = (ULONG)oh;
    if (!(s.rgb = AllocVec(s.w * s.h * 3, MEMF_ANY | MEMF_CLEAR)))
        goto out;
    if ((r = jd_decode(d, scale))) {
        if ((why = why_not(r)))
            note(kind ? kind : "picture", why, path);
        goto out;
    }
    if (!(t = thumb_from_rgb(s.rgb, s.w, s.h, dw, dh, maxcol)))
        goto out;
    memset(pi, 0, sizeof(*pi));
    pi->w = (UWORD)info.width;
    pi->h = (UWORD)info.height;
    pi->depth = info.ncomp == 1 ? 8 : 24;
    strcpy(pi->fmt, "JPEG");
    strcpy(pi->cls, "jdec");                /* built in, as ilbm.c's are "" */
out:
    if (d)
        jd_close(d);
    FreeVec(s.rgb);
    FreeVec(file);
    Close(s.fh);
    return t;
}
