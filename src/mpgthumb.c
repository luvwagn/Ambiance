/*
 * Ambiance - an MPEG video's picture, for its tile (the layouts' rows,
 * walls and pages)
 * Copyright (c) 2026 Brian King
 *
 * As an animation's (ilbm.c's anim_pick), not its first picture when that
 * is blank: a film's first is black, or a title on black, or a studio's
 * card. Its I pictures at points through it (from a tenth of the way in)
 * are decoded by mpegv.library and looked at, as anim_pick looks at an
 * ANIM's frames (40 x 30 points: how much of it one colour, how bright),
 * and the first that is neither blank nor dark is taken; with none, the
 * least blank seen. Kept small (as RGB, MT_KEEP_W across at the most) in
 * PROGDIR:Cache/<hash>.vthumb, by the file's path, date and size: the
 * next time, nothing is decoded.
 *
 * In the loader's processes (loader.c's decode): mpegv.library opened
 * here, its base our own (the UI's MpegvBase is let go after each video
 * it plays), kept until quitting (mpeg_thumb_forget).
 */
#include "amb.h"
#include "vprobe.h"
#include "tspack.h"

#define MpegvBase mt_MpegvBase              /* (our own base: proto/mpegv.h's calls through it) */
#include <libraries/mpegv.h>
#include <proto/mpegv.h>

#include <string.h>
#include <stdio.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Library *mt_MpegvBase;
static struct SignalSemaphore mt_sem;       /* (two loader processes: one opens it) */
static BOOL mt_sem_ready, mt_tried;

#define MT_MAGIC    0x414D5654UL            /* "AMVT" */
#define MT_KEEP_W   192                     /* the picture kept: this wide at the most */
#define MT_POINTS   9                       /* places looked at: its start (its sequence header:
                                             * mpegv decodes nothing without it), then 10% ... 45% */
#define MT_TRIES    60                      /* decodes for an I picture from each place */
#define MT_BLANK    900                     /* of 1000 points one colour: blank */
#define MT_DARK     (24 * 12)               /* mean under about 24 of 255: too dark */
#define MT_LIMIT    4000                    /* ms at the most, a picture or none */
#define MT_BUDGET   1500                    /* ms of looking, then the best seen (a slow Amiga, a big
                                             * MPEG-2's pictures: a few looked at, not eight) */

struct MtHead {
    ULONG magic, hash, days, minute, tick, size, w, h, sw, sh;
};

/* ---- its library */
static BOOL mt_open(void)
{
    if (!mt_sem_ready) {
        Forbid();
        if (!mt_sem_ready) {
            InitSemaphore(&mt_sem);
            mt_sem_ready = TRUE;
        }
        Permit();
    }
    ObtainSemaphore(&mt_sem);
    if (!mt_MpegvBase && !mt_tried) {
        mt_tried = TRUE;                    /* (not there: not asked again each tile) */
        mt_MpegvBase = amb_open_own_lib(MPEGVNAME, MPEGVVERSION);
    }
    ReleaseSemaphore(&mt_sem);
    return mt_MpegvBase != NULL;
}

void mpeg_thumb_forget(void)
{
    if (mt_MpegvBase)
        CloseLibrary(mt_MpegvBase);
    mt_MpegvBase = NULL;
    mt_tried = FALSE;
}

/* ---- the file */
/* A transport stream (vprobe's reading of it): its video packed as a
 * Program Stream as it is read (tspack.c, an instance of its own: what
 * plays may be reading one too), its sound left out */
struct MtTs {
    UBYTE size;                             /* 188, or 192 (M2TS: a time before each) */
    UWORD vpid;
    BOOL mpeg2;
    ULONG first;                            /* its first packet's offset */
};

struct MtSrc {
    BPTR fh;
    volatile UBYTE *cancel;                 /* the job's: set, the file ends here (decoding stops) */
    ULONG t0;                               /* (MT_LIMIT ms after it, too) */
    struct Tsp *tsp;                        /* a transport stream's packing, or NULL */
};

#define MT_READ 16384                       /* (mpegv's reads: tspack wants 4192 or more) */

static LONG mt_file(void *ctx, UBYTE *b, LONG n)
{
    struct MtSrc *src = (struct MtSrc *)ctx;
    if ((src->cancel && *src->cancel) || time_ms() - src->t0 > MT_LIMIT)
        return 0;
    return Read(src->fh, b, n);
}

static ULONG mt_read(register struct Hook *h __asm("a0"), register APTR mpv __asm("a2"),
                     register struct MpvRead *r __asm("a1"))
{
    struct MtSrc *src = (struct MtSrc *)h->h_Data;
    LONG n;
    (void)mpv;
    if (src->tsp)
        n = tsp_pack_in(src->tsp, mt_file, src, r->mr_Buffer, (ULONG)r->mr_Length);
    else
        n = mt_file(src, r->mr_Buffer, r->mr_Length);
    return n > 0 ? (ULONG)n : 0;
}

/* An MPEG stream mpegv decodes: its first start code (after any zero
 * bytes of padding) a Program Stream's pack header, or a bare video
 * stream's sequence header. Its kind + 1; 0: not one (a transport
 * stream, *first its first bytes, for the log) */
static ULONG mt_is_mpeg(BPTR fh, char *first)
{
    UBYTE b[2048];
    LONG n = Read(fh, b, sizeof(b)), i;
    sprintf(first, "%02lx %02lx %02lx %02lx", (unsigned long)(n > 0 ? b[0] : 0),
            (unsigned long)(n > 1 ? b[1] : 0), (unsigned long)(n > 2 ? b[2] : 0),
            (unsigned long)(n > 3 ? b[3] : 0));
    for (i = 0; i + 3 < n; i++) {
        if (b[i])
            return 0;                       /* (not zero padding, and no start code yet) */
        if (b[i + 1] == 0 && b[i + 2] == 1)
            /* (said, not left to mpegv to find: read from the middle, its
             * first bytes are not a pack's or a sequence's) */
            return b[i + 3] == 0xBA ? MPVS_PROGRAM + 1 : b[i + 3] == 0xB3 ? MPVS_VIDEO + 1 : 0;
    }
    return 0;
}

static ULONG mt_hash(const char *s)
{
    ULONG h = 2166136261UL;
    for (; *s; s++)
        h = (h ^ (UBYTE)*s) * 16777619UL;
    return h;
}

static BOOL mt_key(BPTR fh, const char *path, struct MtHead *k)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BOOL ok = fib && ExamineFH(fh, fib);
    memset(k, 0, sizeof(*k));
    if (ok) {
        k->magic = MT_MAGIC;
        k->hash = mt_hash(path);
        k->days = fib->fib_Date.ds_Days;
        k->minute = fib->fib_Date.ds_Minute;
        k->tick = fib->fib_Date.ds_Tick;
        k->size = fib->fib_Size;
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    return ok;
}

static void mt_name(char *out, ULONG hash)
{
    sprintf(out, "PROGDIR:Cache/%08lx.vthumb", (unsigned long)hash);
}

static APTR mt_quiet(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    return old;
}

static void mt_loud(APTR old)
{
    ((struct Process *)FindTask(NULL))->pr_WindowPtr = old;
}

/* The picture kept for this file, if there is one: its RGB (freed by the
 * caller), *k's w and h its size, sw and sh the video's */
static UBYTE *mt_load(const struct MtHead *key, struct MtHead *k)
{
    char name[48];
    UBYTE *rgb = NULL;
    BPTR in;
    APTR q = mt_quiet();
    mt_name(name, key->hash);
    in = Open((CONST_STRPTR)name, MODE_OLDFILE);
    mt_loud(q);
    if (!in)
        return NULL;
    if (Read(in, k, sizeof(*k)) == sizeof(*k) && k->magic == MT_MAGIC && k->hash == key->hash &&
        k->days == key->days && k->minute == key->minute && k->tick == key->tick &&
        k->size == key->size && k->w && k->h && k->w <= MT_KEEP_W && k->h <= 1024 &&
        (rgb = AllocVec(k->w * k->h * 3, MEMF_ANY)) &&
        Read(in, rgb, (LONG)(k->w * k->h * 3)) != (LONG)(k->w * k->h * 3)) {
        FreeVec(rgb);
        rgb = NULL;
    }
    Close(in);
    return rgb;
}

static void mt_save(const struct MtHead *k, const UBYTE *rgb)
{
    char name[48];
    BPTR out, l;
    APTR q = mt_quiet();
    if (!(l = Lock((CONST_STRPTR)"PROGDIR:Cache", SHARED_LOCK)))
        l = CreateDir((CONST_STRPTR)"PROGDIR:Cache");
    if (l)
        UnLock(l);
    mt_name(name, k->hash);
    if ((out = Open((CONST_STRPTR)name, MODE_NEWFILE))) {
        BOOL ok = Write(out, (APTR)k, sizeof(*k)) == sizeof(*k) &&
                  Write(out, (APTR)rgb, (LONG)(k->w * k->h * 3)) == (LONG)(k->w * k->h * 3);
        Close(out);
        if (!ok)
            DeleteFile((CONST_STRPTR)name);     /* (not half a picture for next time) */
    }
    mt_loud(q);
}

/* ---- a picture looked at: how much of it is one colour (of 1000: 40 x
 * 30 points, by their colours at 4 bits each), and how bright on the
 * whole (*lit: 0-2550). Nearly black all over is blank too. */
static ULONG mt_blank(const struct MpvPicture *p, ULONG *lit, UWORD *hist)
{
    ULONG x, y, most = 0, luma = 0;
    memset(hist, 0, 4096 * sizeof(UWORD));
    for (y = 0; y < 30; y++) {
        ULONG py = (y * p->mpic_Height / 30) + p->mpic_Height / 60;
        const UBYTE *Y = p->mpic_Y + py * p->mpic_YModulo;
        const UBYTE *U = p->mpic_Cb + (py >> 1) * p->mpic_CModulo;
        const UBYTE *V = p->mpic_Cr + (py >> 1) * p->mpic_CModulo;
        for (x = 0; x < 40; x++) {
            ULONG px = (x * p->mpic_Width / 40) + p->mpic_Width / 80;
            LONG c = ((LONG)Y[px] - 16) * 298, d = (LONG)U[px >> 1] - 128, e = (LONG)V[px >> 1] - 128;
            LONG r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8,
                 b = (c + 516 * d + 128) >> 8;
            UWORD key;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            luma += (ULONG)(r * 3 + g * 6 + b);
            key = (UWORD)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
            if (++hist[key] > most)
                most = hist[key];
        }
    }
    *lit = luma / 1200;
    if (*lit < MT_DARK)
        return 1000;
    return most * 1000 / 1200;
}

/* The picture to show, chosen and made RGB (kw x kh): NULL if none could
 * be decoded */
static UBYTE *mt_pick(BPTR fh, ULONG type, const struct MtTs *ts, ULONG size, struct MtHead *k, ULONG *looked,
                      volatile UBYTE *cancel, const char **why)
{
    struct Hook rh;
    APTR mpv;
    UWORD *hist = AllocVec(4096 * sizeof(UWORD), MEMF_ANY);
    UBYTE *rgb = NULL;
    ULONG best = 1001, bestlit = 0, i, t0 = time_ms();
    *why = "no memory";
    if (!hist)
        return NULL;
    memset(&rh, 0, sizeof(rh));
    struct MtSrc src;
    src.fh = fh;
    src.cancel = cancel;
    src.t0 = t0;
    src.tsp = NULL;
    if (ts) {
        if (!(src.tsp = AllocVec(tsp_size(), MEMF_ANY))) {
            FreeVec(hist);
            return NULL;
        }
        tsp_begin_in(src.tsp, ts->size, ts->vpid, 0x1FFF, FALSE, ts->mpeg2);
        type = MPVS_PROGRAM;                /* (what tspack makes of it) */
    }
    rh.h_Entry = (HOOKFUNC)(APTR)mt_read;
    rh.h_Data = (APTR)&src;
    Seek(fh, ts ? (LONG)ts->first : 0, OFFSET_BEGINNING);
    if (!(mpv = MpvOpenTags(MPVA_ReadHook, (ULONG)&rh, MPVA_Stream, type, MPVA_ReadSize, MT_READ,
                            MPVA_SkipB, TRUE, TAG_DONE))) {
        FreeVec(hist);
        FreeVec(src.tsp);
        *why = "mpegv could not open it";
        return NULL;
    }
    *why = "no I picture decoded";
    for (i = 0; i < MT_POINTS && best > 0; i++) {
        if (cancel && *cancel) {
            *why = "given up (not wanted any more)";
            break;
        }
        if (time_ms() - t0 > (rgb ? MT_BUDGET : MT_LIMIT)) {
            if (!rgb)
                *why = "too slow here";
            break;                          /* (long enough: the best seen) */
        }
        ULONG at = i ? (ULONG)((unsigned long long)size * (1 + i) / 20) : 0, lit, s;
        LONG e = 0, tries;
        struct MpvPicture *p = NULL;
        if (i || at) {
            if (ts) {
                /* (on a packet's start; packed from there, from its video's
                 * next PES packet) */
                if (at < ts->first)
                    at = ts->first;
                at = ts->first + (at - ts->first) / ts->size * ts->size;
                tsp_seek_in(src.tsp);       /* (the decoder has its start's sequence header) */
            }
            if (Seek(fh, (LONG)at, OFFSET_BEGINNING) < 0)
                break;
            MpvControl(mpv, MPVC_FLUSH, 0);
        }
        for (tries = 0; tries < MT_TRIES && !(cancel && *cancel); tries++) {
            p = MpvDecode(mpv, &e);
            if (p && p->mpic_Type == MPVT_I)
                break;
            p = NULL;
            if (e != MPVE_OK && e != MPVE_MORE)
                break;
        }
        if (!p || !p->mpic_Width || !p->mpic_Height)
            continue;
        (*looked)++;
        s = mt_blank(p, &lit, hist);
        if (s < best || (s == best && lit > bestlit)) {
            ULONG w = p->mpic_Width, h = p->mpic_Height, kw = w, kh = h;
            if (kw > MT_KEEP_W) {
                kw = MT_KEEP_W;
                kh = (h * kw + w / 2) / w;
            }
            if (!kh)
                kh = 1;
            if (rgb && (kw != k->w || kh != k->h)) {
                FreeVec(rgb);
                rgb = NULL;
            }
            if (!rgb && !(rgb = AllocVec(kw * kh * 3, MEMF_ANY)))
                break;
            if (kw == w && kh == h)
                MpvToRGB(p, rgb, kw * 3, MPVR_RGB24);
            else
                MpvToRGBScaled(p, rgb, kw * 3, MPVR_RGB24, kw, kh);
            k->w = kw;
            k->h = kh;
            k->sw = w;
            k->sh = h;
            best = s;
            bestlit = lit;
            if (s < MT_BLANK)
                break;                      /* (neither blank nor dark: this one) */
        }
        if (e == MPVE_END)
            break;
    }
    MpvClose(mpv);
    FreeVec(hist);
    FreeVec(src.tsp);
    return rgb;
}

/* A video file's name (its picture is ours to find, never a datatype's) */
BOOL mpeg_thumb_wants(const char *path)
{
    static const char *const ext[] = { "mpg", "mpeg", "vob", "m1v", "m2v", "mpv", "ts", "m2ts", "mts" };
    const char *dot = strrchr(path, '.');
    ULONG i;
    if (!dot || strchr(dot, '/') || strchr(dot, ':'))
        return FALSE;
    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (!amb_stricmp(dot + 1, ext[i]))
            return TRUE;
    return FALSE;
}

struct Thumb *mpeg_thumb(const char *path, WORD w, WORD h, BOOL tall, BOOL whole, LONG maxcol,
                         struct PicInfo *pi, BOOL *is_mpeg, volatile UBYTE *cancel)
{
    const char *why = "";
    ULONG type;
    char first[16];
    struct MtTs tsi, *ts = NULL;
    struct MtHead key, k;
    struct Thumb *t = NULL;
    UBYTE *rgb;
    ULONG dw, dh, t0 = time_ms(), looked = 0;
    BOOL kept = FALSE;
    BPTR fh;
    APTR q = mt_quiet();
    *is_mpeg = FALSE;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    mt_loud(q);
    if (!fh)
        return NULL;
    if (!(type = mt_is_mpeg(fh, first))) {
        /* a transport stream (.ts, .m2ts, or one called .mpg): what vprobe
         * finds in it (its packets, its video's PID and kind) */
        struct VProbe v;
        char m[200];
        BOOL found = vprobe(path, &v, 0);
        if (v.ts && v.h264) {
            sprintf(m, "%.80s: none (%s video in a transport stream: mpegv decodes MPEG-1 and MPEG-2)",
                    (const char *)FilePart((STRPTR)path), vprobe_kind(&v));
            amb_log("video picture", m);
            Close(fh);
            *is_mpeg = TRUE;
            return NULL;
        }
        if (!v.ts || !found || v.ts_vpid == 0x1FFF) {
            if (v.ts)
                sprintf(m, "%.80s: none (a transport stream, no MPEG video's sequence header found in it)",
                        (const char *)FilePart((STRPTR)path));
            else
                sprintf(m, "%.80s: none (not an MPEG stream mpegv reads: it begins %s)",
                        (const char *)FilePart((STRPTR)path), first);
            amb_log("video picture", m);
            Close(fh);
            return NULL;
        }
        tsi.size = v.ts;
        tsi.vpid = v.ts_vpid;
        tsi.mpeg2 = v.mpeg2;
        tsi.first = v.ts_first;
        ts = &tsi;
        type = MPVS_PROGRAM + 1;
    }
    if (!mt_key(fh, path, &key)) {
        Close(fh);
        return NULL;
    }
    *is_mpeg = TRUE;
    k = key;
    if ((rgb = mt_load(&key, &k)))
        kept = TRUE;
    else if (!mt_open())
        why = "no mpegv.library";
    else if ((rgb = mt_pick(fh, type - 1, ts, key.size, &k, &looked, cancel, &why)) && !(cancel && *cancel))
        mt_save(&k, rgb);
    Close(fh);
    if (!rgb) {
        char m[200];
        sprintf(m, "%.80s: none (%s; %lu looked at, %lu ms)", (const char *)FilePart((STRPTR)path), why,
                (unsigned long)looked, (unsigned long)(time_ms() - t0));
        amb_log("video picture", m);
        return NULL;
    }
    pic_fit(k.w, k.h, tall, w, h, whole, &dw, &dh);
    t = thumb_from_rgb(rgb, k.w, k.h, dw, dh, maxcol);
    FreeVec(rgb);
    if (t) {
        memset(pi, 0, sizeof(*pi));
        pi->w = (UWORD)k.sw;
        pi->h = (UWORD)k.sh;
        pi->depth = 24;
        strcpy(pi->fmt, "MPEG");
        strcpy(pi->cls, "mpegv");
    }
    if (!kept) {
        char m[200];
        const char *f = (const char *)FilePart((STRPTR)path);
        sprintf(m, "%.80s: %s, %lu I pictures looked at (%lux%lu%s), %lu ms", f,
                t ? "a picture chosen" : "none could be made", (unsigned long)looked,
                (unsigned long)k.sw, (unsigned long)k.sh,
                ts ? (ts->size == 192 ? ", M2TS" : ", a transport stream") : "",
                (unsigned long)(time_ms() - t0));
        amb_log("video picture", m);
    }
    return t;
}
