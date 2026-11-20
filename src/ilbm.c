/*
 * Ambiance - IFF ILBM pictures, read without a datatype
 * Copyright (c) 2026 Brian King
 *
 * AGS's screenshots (320 x 128, up to 64 colours) and many Pictures are
 * IFF ILBMs. Through picture.datatype one took 200-300 ms on a 68030:
 * the class looked up and opened, the whole picture unpacked and turned
 * into chunky pixels, then read back. AGS reads them itself, as this does:
 * the file in one read, its rows unpacked in turn (ByteRun1 or plain), and
 * only the rows and pixels the thumbnail needs taken from the bitplanes.
 *
 * Colour-table pictures of up to 8 planes, extra halfbrite too, and PBMs
 * (Deluxe Paint's on the PC: the same, its rows 8-bit pixels, not
 * bitplanes; Mark Ferrari's slides). Deep (24-bit) pictures are left to the
 * datatype (NULL).
 *
 * An IFF ANIM (Videos) is its first frame, an ILBM, read alone (Tron2 is
 * 28 MB): HAM ones too, made into colours here, as the datatype would read
 * the whole file. Its frames are counted on the way, a header each.
 */
#include "amb.h"
#include <stdio.h>
#include "animdec.h"

#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <exec/semaphores.h>

#define ID(a, b, c, d) (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (d))
#define ILBM_MAX (4UL << 20)    /* larger files: the datatype */
#define CAMG_HAM 0x800
#define CAMG_EHB 0x80

/* ---- an ANIM's frame count and length, kept (PROGDIR:Cache/Animations)
 * by its path, date and size: counting them walks every frame of the file
 * (Reco_Main: 523 across 19 MB, half a second, and under an emulator the
 * whole Amiga waits on it), so it is done once. The pane and the player
 * both ask; two loader processes may at once (a semaphore). */
#define AF_FILE "PROGDIR:Cache/Animations"
#define AF_MAX  1024
struct AnimFact {
    ULONG hash, days, minute, tick, size, frames, ms;
};
static struct AnimFact *af;
static LONG afn;
static BOOL af_loaded, af_sem_ready;
static struct SignalSemaphore af_sem;

/* No "write protected" or "please insert" requester for the Cache: on a
 * read-only volume a write just fails (for the pane's loader process it
 * would come up on the Workbench screen, behind Ambiance, and wait). */
static APTR quiet_on(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    return old;
}

static void quiet_off(APTR old)
{
    ((struct Process *)FindTask(NULL))->pr_WindowPtr = old;
}

static void af_lock(void)
{
    Forbid();
    if (!af_sem_ready) {
        InitSemaphore(&af_sem);
        af_sem_ready = TRUE;
    }
    Permit();
    ObtainSemaphore(&af_sem);
}

static ULONG af_hash(const char *s)
{
    ULONG h = 2166136261UL;
    for (; *s; s++)
        h = (h ^ (UBYTE)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s)) * 16777619UL;
    return h;
}

static void af_load(void)
{
    BPTR fh;
    if (af_loaded)
        return;
    APTR q;
    af_loaded = TRUE;
    if (!(af = AllocVec(AF_MAX * sizeof(*af), MEMF_ANY)))
        return;
    q = quiet_on();
    if ((fh = Open((CONST_STRPTR)AF_FILE, MODE_OLDFILE))) {
        LONG n = Read(fh, af, AF_MAX * sizeof(*af));
        afn = n > 0 ? n / (LONG)sizeof(*af) : 0;
        Close(fh);
    }
    quiet_off(q);
}

/* fh's file as a fact's key (FALSE: not known) */
static BOOL af_key(BPTR fh, const char *path, struct AnimFact *k)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BOOL ok = fib && ExamineFH(fh, fib);
    if (ok) {
        k->hash = af_hash(path);
        k->days = fib->fib_Date.ds_Days;
        k->minute = fib->fib_Date.ds_Minute;
        k->tick = fib->fib_Date.ds_Tick;
        k->size = fib->fib_Size;
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    return ok;
}

/* The facts read in let go (quitting: the loader's processes stopped
 * first, so nothing else holds them); read again if asked for after. */
void anim_fact_forget(void)
{
    if (af_sem_ready)
        ObtainSemaphore(&af_sem);
    if (af)
        FreeVec(af);
    af = NULL;
    afn = 0;
    af_loaded = FALSE;
    if (af_sem_ready)
        ReleaseSemaphore(&af_sem);
}

BOOL anim_fact_get(BPTR fh, const char *path, ULONG *frames, ULONG *ms)
{
    struct AnimFact k;
    LONG i;
    BOOL r = FALSE;
    if (!af_key(fh, path, &k))
        return FALSE;
    af_lock();
    af_load();
    for (i = afn - 1; i >= 0 && af; i--)    /* the newest first: a file changed is added again */
        if (af[i].hash == k.hash && af[i].days == k.days && af[i].minute == k.minute &&
            af[i].tick == k.tick && af[i].size == k.size) {
            *frames = af[i].frames;
            *ms = af[i].ms;
            r = TRUE;
            break;
        }
    ReleaseSemaphore(&af_sem);
    return r;
}

void anim_fact_put(BPTR fh, const char *path, ULONG frames, ULONG ms)
{
    struct AnimFact k;
    BPTR out, l;
    ULONG f0, m0;
    if (!frames || !af_key(fh, path, &k) || anim_fact_get(fh, path, &f0, &m0))
        return;
    k.frames = frames;
    k.ms = ms;
    af_lock();
    if (af && afn < AF_MAX) {
        APTR q = quiet_on();
        af[afn++] = k;                      /* kept for this run whatever happens to the file */
        if (!(l = Lock((CONST_STRPTR)"PROGDIR:Cache", SHARED_LOCK)))
            l = CreateDir((CONST_STRPTR)"PROGDIR:Cache");
        if (l)
            UnLock(l);
        if ((out = Open((CONST_STRPTR)AF_FILE, MODE_READWRITE))) {
            Seek(out, 0, OFFSET_END);
            Write(out, &k, sizeof(k));
            Close(out);
        }
        quiet_off(q);
    }
    ReleaseSemaphore(&af_sem);
}

/* An ANIM's frames from here on, and their time: a few bytes of each
 * read, the rest passed by; frames and jiffies: those counted already. */
static void count_frames(BPTR fh, struct PicInfo *pi, ULONG frames, ULONG jiffies)
{
    UBYTE h[52];
    while (frames < 20000 && Read(fh, h, 12) == 12 && amb_be32(h) == ID('F', 'O', 'R', 'M')) {
        LONG len = amb_be32(h + 4) - 4, got = 0;
        if (len < 0)
            break;
        if (amb_be32(h + 8) == ID('I', 'L', 'B', 'M')) {
            frames++;
            got = Read(fh, h, len < (LONG)sizeof(h) ? len : (LONG)sizeof(h));
            if (got >= 8 + 18 && amb_be32(h) == ID('A', 'N', 'H', 'D'))
                jiffies += amb_be32(h + 8 + 14) ? amb_be32(h + 8 + 14) : 1;    /* as it plays */
            if (got < 0)
                break;
        }
        if (Seek(fh, len - got + (len & 1), OFFSET_CURRENT) < 0)
            break;
    }
    pi->frames = frames > 65535 ? 65535 : frames;
    pi->ms = jiffies * 1000 / 60;
}

BOOL ilbm_is_anim(const char *path)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    UBYTE head[12];
    BOOL r = FALSE;
    BPTR fh;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return FALSE;
    r = Read(fh, head, 12) == 12 && amb_be32(head) == ID('F', 'O', 'R', 'M') &&
        amb_be32(head + 8) == ID('A', 'N', 'I', 'M');
    Close(fh);
    return r;
}

/* The whole file, if it is an ILBM (else NULL); of an ANIM, its first
 * frame (*anim set; *afh: the file, open after it, to be closed). */
static UBYTE *read_file(const char *path, ULONG *len, BOOL *anim, struct PicInfo *pi, BPTR *afh)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    UBYTE head[12], *buf = NULL;
    LONG size;
    BPTR fh;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    *anim = FALSE;
    memset(head, 0, sizeof(head));
    pi->frames = 0;
    pi->ms = 0;
    if (!fh)
        return NULL;
    if (Read(fh, head, 12) == 12 && amb_be32(head) == ID('F', 'O', 'R', 'M') &&
        amb_be32(head + 8) == ID('A', 'N', 'I', 'M')) {
        *anim = TRUE;                       /* its first frame: FORM ILBM */
        /* its sounds first, if it has any (8SVX): passed by */
        while (Read(fh, head, 12) == 12 && amb_be32(head) == ID('F', 'O', 'R', 'M') &&
               amb_be32(head + 8) != ID('I', 'L', 'B', 'M') && (size = amb_be32(head + 4)) >= 4 &&
               Seek(fh, size - 4 + (size & 1), OFFSET_CURRENT) >= 0)
            ;
        if (amb_be32(head) == ID('F', 'O', 'R', 'M') &&
            amb_be32(head + 8) == ID('I', 'L', 'B', 'M') && (size = amb_be32(head + 4)) >= 4 &&
            (ULONG)size <= ILBM_MAX && (buf = AllocVec(size + 8, MEMF_ANY))) {
            CopyMem(head, buf, 12);
            if (Read(fh, buf + 12, size - 4) != size - 4) {
                FreeVec(buf);
                buf = NULL;
            } else {
                *len = size + 8;
                if (size & 1)
                    Seek(fh, 1, OFFSET_CURRENT);
                *afh = fh;                  /* the frames after it: ilbm_thumb */
                return buf;
            }
        }
        Close(fh);
        return buf;
    }
    if (amb_be32(head) == ID('F', 'O', 'R', 'M') &&
        (amb_be32(head + 8) == ID('I', 'L', 'B', 'M') || amb_be32(head + 8) == ID('P', 'B', 'M', ' ')) &&
        Seek(fh, 0, OFFSET_END) >= 0 &&
        (size = Seek(fh, 0, OFFSET_BEGINNING)) > 12 && (ULONG)size <= ILBM_MAX &&
        (buf = AllocVec(size, MEMF_ANY))) {
        if (Read(fh, buf, size) != size) {
            FreeVec(buf);
            buf = NULL;
        }
        *len = size;
    }
    Close(fh);
    return buf;
}

/* Unpack the next row (n bytes) from the body at *p (up to e). */
static BOOL unpack(const UBYTE **pp, const UBYTE *e, UBYTE *out, ULONG n, BOOL packed)
{
    const UBYTE *p = *pp;
    if (!packed) {
        if ((ULONG)(e - p) < n)
            return FALSE;
        CopyMem((APTR)p, out, n);
        *pp = p + n;
        return TRUE;
    }
    while (n) {
        BYTE c;
        if (p >= e)
            return FALSE;
        c = (BYTE)*p++;
        if (c >= 0) {
            ULONG k = (ULONG)c + 1;
            if (k > n || (ULONG)(e - p) < k)
                return FALSE;
            CopyMem((APTR)p, out, k);
            p += k;
            out += k;
            n -= k;
        } else if (c != -128) {
            ULONG k = (ULONG)(-c) + 1;
            if (k > n || p >= e)
                return FALSE;
            memset(out, *p++, k);
            out += k;
            n -= k;
        }
    }
    *pp = p;
    return TRUE;
}

/* Bitplane bytes to chunky pixels, 8 at a time: for a byte of plane 0,
 * its 8 pixels as two longs, a 1 in the low bit of each pixel's byte
 * that is set (shifted up for the other planes). */
static ULONG c2p[256][2];
static BOOL c2p_ready;

static void c2p_init(void)
{
    ULONG b, j;
    if (c2p_ready)
        return;
    for (b = 0; b < 256; b++) {
        ULONG hi = 0, lo = 0;
        for (j = 0; j < 4; j++) {
            if (b & (0x80 >> j))
                hi |= 1UL << (24 - 8 * j);
            if (b & (0x08 >> j))
                lo |= 1UL << (24 - 8 * j);
        }
        c2p[b][0] = hi;
        c2p[b][1] = lo;
    }
    c2p_ready = TRUE;
}

/* A row of bitplanes (rb bytes each) as chunky pixels into out (rb * 8
 * bytes, long-aligned). */
static void row_chunky(const UBYTE *row, ULONG rb, UBYTE planes, ULONG *out)
{
    ULONG i, k;
    memset(out, 0, rb * 8);
    for (k = 0; k < planes; k++, row += rb) {
        ULONG *o = out;
        for (i = 0; i < rb; i++, o += 2) {
            UBYTE b = row[i];
            if (b) {
                o[0] |= c2p[b][0] << k;
                o[1] |= c2p[b][1] << k;
            }
        }
    }
}

/* ---- an ANIM's picture: its first frame, unless that is blank (one
 * colour nearly all over: a black start, a title on black), then the
 * frames after it decoded (animdec.c) until one is not, every few looked
 * at, PICK_FRAMES or PICK_BYTES at the most: the least blank one seen. */
#define PICK_FRAMES 360             /* AL1-4's credits and title take 297 */
#define PICK_BYTES  (2UL << 20)
#define PICK_EVERY  3
#define PICK_BLANK  940             /* of 1000 points one colour: blank */
#define PICK_SETTLE 24              /* frames on from the first not blank, for a brighter one */

/* How much of a frame (interleaved rows: rb bytes a plane) is one colour,
 * of 1000: 40 x 30 points looked at, by the colours they show (cm: the
 * CMAP as it is then; HAM by its base colours, extra halfbrite halved).
 * Nearly black all over is blank too: one fading in from black by its
 * colours (WalkerDemo2) has every pen used and none yet lit. */
static ULONG blank_score(const UBYTE *img, ULONG rb, UBYTE planes, ULONG sw, ULONG sh,
                         const UBYTE *cm, ULONG cml, ULONG camg, ULONG *lit, UWORD *hist)
{
    ULONG x, y, most = 0, luma = 0, n = cml / 3;
    UBYTE k;
    memset(hist, 0, 4096 * sizeof(UWORD));
    for (y = 0; y < 30; y++) {
        const UBYTE *r = img + (y * sh / 30) * rb * planes;
        for (x = 0; x < 40; x++) {
            ULONG px = x * sw / 40, c = 0, rr = 0, gg = 0, bb = 0, half = 0;
            UWORD key;
            for (k = 0; k < planes; k++)
                if (r[k * rb + (px >> 3)] & (0x80 >> (px & 7)))
                    c |= 1UL << k;
            if (camg & CAMG_HAM)
                c &= planes == 8 ? 63 : 15;
            else if ((camg & CAMG_EHB) && planes == 6 && c >= 32) {
                c -= 32;
                half = 1;
            }
            if (c < n) {
                rr = cm[c * 3] >> half;
                gg = cm[c * 3 + 1] >> half;
                bb = cm[c * 3 + 2] >> half;
            }
            luma += rr * 3 + gg * 6 + bb;
            key = (UWORD)(((rr >> 4) << 8) | ((gg >> 4) << 4) | (bb >> 4));
            if (++hist[key] > most)
                most = hist[key];
        }
    }
    *lit = luma / 1200;                     /* 0-2550: how bright, on the whole */
    if (*lit < 10 * 12)                     /* mean under about 12 of 255: dark */
        return 1000;
    return most * 1000 / 1200;
}

/* ---- the frame chosen, kept (PROGDIR:Cache/<hash>.frame) when it is not
 * the first: the next time the pane shows it straight away, no frames
 * read or decoded (AL1-4: 309 of them). By the file's path, date and
 * size, as the frame counts are; its planes and colours as they were. */
#define PICK_MAGIC 0x414D4346UL         /* "AMCF" */
struct PickHead {
    ULONG magic, hash, days, minute, tick, size, sw, sh, planes, cml;
};

static void pick_name(char *out, ULONG hash)
{
    sprintf(out, "PROGDIR:Cache/%08lx.frame", (unsigned long)hash);
}

static BOOL pick_load(BPTR fh, const char *path, ULONG sw, ULONG sh, UBYTE planes, UBYTE *img,
                      ULONG fsz, UBYTE *cm, ULONG *cml)
{
    struct AnimFact k;
    struct PickHead ph;
    char name[48];
    BPTR in;
    BOOL ok = FALSE;
    if (!af_key(fh, path, &k))
        return FALSE;
    pick_name(name, k.hash);
    {
        APTR q = quiet_on();
        in = Open((CONST_STRPTR)name, MODE_OLDFILE);
        quiet_off(q);
    }
    if (!in)
        return FALSE;
    if (Read(in, &ph, sizeof(ph)) == sizeof(ph) && ph.magic == PICK_MAGIC && ph.hash == k.hash &&
        ph.days == k.days && ph.minute == k.minute && ph.tick == k.tick && ph.size == k.size &&
        ph.sw == sw && ph.sh == sh && ph.planes == planes && ph.cml <= 768 &&
        Read(in, cm, ph.cml) == (LONG)ph.cml && Read(in, img, fsz) == (LONG)fsz) {
        *cml = ph.cml;
        ok = TRUE;
    }
    Close(in);
    return ok;
}

static void pick_save(BPTR fh, const char *path, ULONG sw, ULONG sh, UBYTE planes,
                      const UBYTE *img, ULONG fsz, const UBYTE *cm, ULONG cml)
{
    struct AnimFact k;
    struct PickHead ph;
    char name[48];
    BPTR out, l;
    APTR q;
    if (!af_key(fh, path, &k))
        return;
    q = quiet_on();
    if (!(l = Lock((CONST_STRPTR)"PROGDIR:Cache", SHARED_LOCK)))
        l = CreateDir((CONST_STRPTR)"PROGDIR:Cache");
    if (l)
        UnLock(l);
    pick_name(name, k.hash);
    ph.magic = PICK_MAGIC;
    ph.hash = k.hash;
    ph.days = k.days;
    ph.minute = k.minute;
    ph.tick = k.tick;
    ph.size = k.size;
    ph.sw = sw;
    ph.sh = sh;
    ph.planes = planes;
    ph.cml = cml;
    if ((out = Open((CONST_STRPTR)name, MODE_NEWFILE))) {
        BOOL ok = Write(out, &ph, sizeof(ph)) == sizeof(ph) && Write(out, (APTR)cm, cml) == (LONG)cml &&
                  Write(out, (APTR)img, fsz) == (LONG)fsz;
        Close(out);
        if (!ok)
            DeleteFile((CONST_STRPTR)name);     /* not half a frame for next time */
    }
    quiet_off(q);
}

/* The frame to show, decoded into a new buffer of interleaved rows (the
 * caller frees it); its colours into cm (*cml bytes) if a frame changed
 * them. Frames read meanwhile counted into *frames and *jif. */
static UBYTE *anim_pick(BPTR fh, const char *path, const UBYTE *body, ULONG bodylen, ULONG sw,
                        ULONG sh, UBYTE planes, UBYTE masking, UBYTE comp, ULONG camg, UBYTE *cm,
                        ULONG *cml, ULONG *frames, ULONG *jif)
{
    ULONG rb = ((sw + 15) >> 4) << 1, fsz = rb * planes * sh, best, got = 0, fl = 0, bestlit, lit;
    LONG found = -1;                    /* the first frame not blank */
    BOOL later = FALSE;                 /* a frame after the first kept: to the cache */
    UBYTE *a, *buf[2], *keep, *f = NULL, *cur = NULL, h[12];
    UBYTE cm_now[768];
    UWORD *hist;
    ULONG cmn = *cml;
    struct AnimPlanes pl[2];
    int k, i;
    LONG n;
    if (!(a = AllocVec(fsz * 3, MEMF_ANY | MEMF_CLEAR))) {
        if (!(a = AllocVec(fsz, MEMF_ANY | MEMF_CLEAR)))   /* the first frame alone */
            return NULL;
        keep = NULL;
    } else
        keep = a + 2 * fsz;
    buf[0] = a;
    buf[1] = a + fsz;
    if (pick_load(fh, path, sw, sh, planes, a, fsz, cm, cml))
        return a;                           /* chosen before */
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 8; i++)
            pl[k].plane[i] = buf[k] + i * rb;
        pl[k].stride = rb * planes;
        pl[k].depth = planes;
        pl[k].rb = rb;
        pl[k].h = sh;
    }
    animdec_body(body, bodylen, &pl[0], comp, masking);
    if (!keep || !(hist = AllocVec(4096 * sizeof(UWORD), MEMF_ANY)))
        return a;                           /* the first frame, as it is */
    best = blank_score(buf[0], rb, planes, sw, sh, cm, *cml, camg, &bestlit, hist);
    if (best < PICK_BLANK) {
        FreeVec(hist);
        return a;
    }
    CopyMem(buf[0], buf[1], fsz);
    CopyMem(buf[0], keep, fsz);
    CopyMem(cm, cm_now, cmn);
    for (n = 1; n < PICK_FRAMES && got < PICK_BYTES; n++) {
        const UBYTE *anhd = NULL, *dlta = NULL, *bd = NULL, *p, *e;
        ULONG dl = 0, bl = 0, len, sc;
        if (Read(fh, h, 12) != 12 || amb_be32(h) != ID('F', 'O', 'R', 'M') || amb_be32(h + 4) < 4)
            break;
        len = amb_be32(h + 4) - 4;
        if (amb_be32(h + 8) != ID('I', 'L', 'B', 'M')) {
            if (Seek(fh, len + (len & 1), OFFSET_CURRENT) < 0)
                break;
            n--;
            continue;
        }
        if (len > fl) {
            FreeVec(f);
            if (!(f = AllocVec(fl = len + 1024, MEMF_ANY)))
                break;
        }
        if (Read(fh, f, len) != (LONG)len)
            break;
        if (len & 1)
            Seek(fh, 1, OFFSET_CURRENT);
        got += len;
        (*frames)++;
        for (p = f, e = f + len; p + 8 <= e;) {
            ULONG id = amb_be32(p), cl = amb_be32(p + 4);
            if (cl > (ULONG)(e - p - 8))
                cl = e - p - 8;
            if (id == ID('A', 'N', 'H', 'D') && cl >= 24)
                anhd = p + 8;
            else if (id == ID('D', 'L', 'T', 'A')) {
                dlta = p + 8;
                dl = cl;
            } else if (id == ID('B', 'O', 'D', 'Y')) {
                bd = p + 8;
                bl = cl;
            } else if (id == ID('C', 'M', 'A', 'P') && cl >= 6) {
                cmn = cl < sizeof(cm_now) ? cl : sizeof(cm_now);
                CopyMem((APTR)(p + 8), cm_now, cmn);
            }
            p += 8 + ((cl + 1) & ~1UL);
        }
        if (anhd)
            *jif += amb_be32(anhd + 14) ? amb_be32(anhd + 14) : 1;
        k = n & 1;
        if (anhd && anhd[18] == 1)          /* from the last frame, not two back */
            CopyMem(buf[!k], buf[k], fsz);
        if (anhd && anhd[0] == 0 && bd)
            animdec_body(bd, bl, &pl[k], comp, masking);
        else if (dlta && animdec_delta(dlta, dl, &pl[k], anhd ? anhd[0] : 5,
                                       anhd ? amb_be32(anhd + 20) : 0) == -2)
            break;                          /* not one this knows: the best so far */
        cur = buf[k];
        if (n % PICK_EVERY)
            continue;
        sc = blank_score(cur, rb, planes, sw, sh, cm_now, cmn, camg, &lit, hist);
        if (sc < PICK_BLANK) {
            /* not blank: the brightest of this and the next few (a fade in
             * from black lit at last, not its first glimmer) */
            if (found < 0 || lit > bestlit) {
                best = sc;
                bestlit = lit;
                CopyMem(cur, keep, fsz);
                CopyMem(cm_now, cm, cmn);
                *cml = cmn;
                later = TRUE;
            }
            if (found < 0)
                found = n;
            if (n - found >= PICK_SETTLE)
                break;
        } else if (found >= 0) {
            if (n - found >= PICK_SETTLE)   /* dark a moment (a scene lit in flickers) */
                break;
        } else if (sc < best || (sc == best && lit > bestlit)) {
            /* the least blank, should none do; of the dark ones the
             * brightest (a title on black, not the black before it) */
            best = sc;
            bestlit = lit;
            CopyMem(cur, keep, fsz);
            CopyMem(cm_now, cm, cmn);
            *cml = cmn;
            later = TRUE;
        }
    }
    FreeVec(f);
    FreeVec(hist);
    CopyMem(keep, a, fsz);
    if (later)                              /* not the first: kept for next time */
        pick_save(fh, path, sw, sh, planes, a, fsz, cm, *cml);
    return a;
}

/* A filter's taps, for making sn pixels dn (fewer): each small pixel's
 * first big one (first[]) and how much of it and the next ones it takes
 * (wt[], k of them, in 256ths of a pixel). The window is the small pixel's
 * share of the big ones, but never under two: a screenshot dithered in
 * stripes or checks of two pixels averages out, wherever it falls. */
static int ilbm_taps_n(ULONG sn, ULONG dn)
{
    ULONG s256 = sn * 256 / dn, win = s256 < 512 ? 512 : s256;
    return (int)(win / 256 + 2);
}

static void ilbm_taps(ULONG sn, ULONG dn, UWORD *first, UWORD *wt, int k)
{
    ULONG s256 = sn * 256 / dn, win = s256 < 512 ? 512 : s256, d;
    for (d = 0; d < dn; d++) {
        LONG c = (LONG)((2 * d + 1) * sn * 128 / dn), a = c - (LONG)win / 2, b = c + (LONG)win / 2, i0;
        int j;
        if (a < 0)
            a = 0;
        if (b > (LONG)(sn * 256))
            b = (LONG)(sn * 256);
        i0 = a >> 8;
        first[d] = (UWORD)i0;
        for (j = 0; j < k; j++) {
            LONG lo = (i0 + j) * 256, hi = lo + 256;
            if (lo < a)
                lo = a;
            if (hi > b)
                hi = b;
            wt[d * k + j] = (UWORD)(hi > lo ? hi - lo : 0);
        }
    }
}

struct Thumb *ilbm_thumb(const char *path, WORD w, WORD h, BOOL tall, BOOL whole, LONG maxcol,
                         struct PicInfo *pi)
{
    UBYTE *file, *row = NULL;
    const UBYTE *p, *e, *bmhd = NULL, *cmap = NULL, *body = NULL;
    ULONG len = 0, cmaplen = 0, bodylen = 0, camg = 0, sw, sh, dw, dh, rb, np, x, y, sy, i, n;
    UBYTE planes, masking, comp;
    UWORD *xoff = NULL;
    ULONG *chunky = NULL;
    struct Thumb *t = NULL;
    BOOL anim, ham, pbm;
    ULONG base[64], prb;
    BPTR afh = 0;
    UBYTE *img = NULL;
    UBYTE *cm = NULL;               /* an ANIM's colours as of its frame shown: its own, as
                                     * loaders make thumbnails side by side (a static one was
                                     * overwritten by another's: Windmill2 in strange colours) */

    if (!(file = read_file(path, &len, &anim, pi, &afh)))
        return NULL;
    pbm = !anim && amb_be32(file + 8) == ID('P', 'B', 'M', ' ');
    /* the chunks */
    e = file + (amb_be32(file + 4) + 8 < len ? amb_be32(file + 4) + 8 : len);
    for (p = file + 12; p + 8 <= e;) {
        ULONG id = amb_be32(p), cl = amb_be32(p + 4);
        const UBYTE *d = p + 8;
        if (cl > (ULONG)(e - d))
            cl = e - d;
        if (id == ID('B', 'M', 'H', 'D') && cl >= 20)
            bmhd = d;
        else if (id == ID('C', 'M', 'A', 'P')) {
            cmap = d;
            cmaplen = cl;
        } else if (id == ID('C', 'A', 'M', 'G') && cl >= 4)
            camg = amb_be32(d);
        else if (id == ID('B', 'O', 'D', 'Y')) {
            body = d;
            bodylen = cl;
            break;                          /* the rest is after the picture */
        }
        p = d + ((cl + 1) & ~1UL);
    }
    if (!bmhd || !cmap || !body)
        goto out;
    sw = amb_be16(bmhd);
    sh = amb_be16(bmhd + 2);
    planes = bmhd[8];
    masking = bmhd[9];
    comp = bmhd[10];
    ham = (camg & CAMG_HAM) && (planes == 6 || planes == 8);
    if (!sw || !sh || !planes || planes > 8 || comp > 1 || ((camg & CAMG_HAM) && !(anim && ham)) ||
        cmaplen < 6 || (pbm && (planes != 8 || masking == 1)))
        goto out;
    if (pbm)
        ham = FALSE;
    prb = (sw + 1) & ~1UL;                  /* a PBM's row: a byte a pixel, to an even count */
    if (afh) {                              /* an ANIM: the frame worth showing, and the count */
        ULONG frames = 1, jif = 0;
        if (!(cm = AllocVec(768, MEMF_ANY)))
            goto out;
        cmaplen = cmaplen < 768 ? cmaplen : 768;
        CopyMem((APTR)cmap, cm, cmaplen);
        ULONG kf, kms;
        img = anim_pick(afh, path, body, bodylen, sw, sh, planes, masking, comp, camg, cm,
                        &cmaplen, &frames, &jif);
        cmap = cm;
        if (anim_fact_get(afh, path, &kf, &kms)) {
            pi->frames = kf > 65535 ? 65535 : kf;   /* counted before */
            pi->ms = kms;
        } else {
            count_frames(afh, pi, frames, jif);
            anim_fact_put(afh, path, pi->frames, pi->ms);
        }
        Close(afh);
        afh = 0;
    }

    if (!w && !h) {
        dw = sw;                            /* (0 x 0: at its own size, its own colours: the viewer's) */
        dh = sh;
    } else {
        pic_fit(sw, sh, tall, w, h, whole, &dw, &dh);
    }
    if (!(t = thumb_new(dw, dh)))
        goto out;
    /* the colours; old 4-bit ones (0xF0) widened to 8 */
    n = cmaplen / 3;
    if (n > 256)
        n = 256;
    {
        UBYTE lo = 0;
        for (i = 0; i < n * 3; i++)
            lo |= cmap[i] & 0x0F;
        for (i = 0; i < n; i++) {
            ULONG r = cmap[i * 3], g = cmap[i * 3 + 1], b = cmap[i * 3 + 2];
            if (!lo) {
                r |= r >> 4;
                g |= g >> 4;
                b |= b >> 4;
            }
            t->pal[i] = (r << 16) | (g << 8) | b;
        }
    }
    if ((camg & CAMG_EHB) && planes == 6 && n <= 32) {
        for (i = 0; i < 32; i++)
            t->pal[32 + i] = (t->pal[i] >> 1) & 0x7F7F7F;
        n = 64;
    }
    t->ncol = (1UL << planes) > n ? (1UL << planes) : n;   /* the rest black */
    if (t->ncol > 256)
        t->ncol = 256;
    if (ham) {                              /* its base colours kept; pixels in 3-3-2 */
        for (i = 0; i < 64; i++)
            base[i] = i < n ? t->pal[i] : 0;
        for (i = 0; i < 256; i++) {
            ULONG r = (i >> 5) * 255 / 7, g = ((i >> 2) & 7) * 255 / 7, b = (i & 3) * 255 / 3;
            t->pal[i] = (r << 16) | (g << 8) | b;
        }
        t->ncol = 256;
    }

    rb = ((sw + 15) >> 4) << 1;
    np = planes + (masking == 1 ? 1 : 0);
    if (maxcol <= THUMB_DEEP && dw <= sw && dh <= sh && (dw < sw || dh < sh)) {
        /* true colour (a layout's), made smaller: each small pixel the
         * weighted average of the big ones under it, over at least two of
         * them each way (ilbm_taps). Picked one in so many, a screenshot's
         * dithered shades broke into specks; averaged in whole pixels (1 or
         * 2 each, at 320 -> 197), a 64-colour screenshot's dither still
         * showed in the ones that got one (Chaos Engine's stone). */
        struct Thumb *d = AllocVec(sizeof(struct Thumb) + dw * dh * 4, MEMF_ANY | MEMF_CLEAR);
        int kx = ilbm_taps_n(sw, dw), ky = ilbm_taps_n(sh, dh);
        UWORD *fx = AllocVec(dw * sizeof(UWORD), MEMF_ANY), *fy = AllocVec(dh * sizeof(UWORD), MEMF_ANY);
        UWORD *wx = AllocVec(dw * kx * sizeof(UWORD), MEMF_ANY), *wy = AllocVec(dh * ky * sizeof(UWORD), MEMF_ANY);
        ULONG *srow = AllocVec(sw * 4, MEMF_ANY), *tmp = AllocVec(dw * sh * 4, MEMF_ANY | MEMF_CLEAR);
        LONG k;
        row = AllocVec(rb * np, MEMF_ANY);
        chunky = AllocVec(rb * 8, MEMF_ANY);
        if (!d || !fx || !fy || !wx || !wy || !srow || !tmp || !row || !chunky) {
            FreeVec(d);
            FreeVec(fx);
            FreeVec(fy);
            FreeVec(wx);
            FreeVec(wy);
            FreeVec(srow);
            FreeVec(tmp);
            thumb_free(t);
            t = NULL;
            goto out;
        }
        d->w = dw;
        d->h = dh;
        d->mod = dw;
        d->argb = (ULONG *)(d + 1);
        ilbm_taps(sw, dw, fx, wx, kx);
        ilbm_taps(sh, dh, fy, wy, ky);
        c2p_init();
        p = body;
        e = body + bodylen;
        for (sy = 0; sy < sh; sy++) {       /* each row: its colours, then made narrow */
            const UBYTE *cc = (const UBYTE *)chunky;
            ULONG rgb = ham ? base[0] : 0, bits = planes == 8 ? 6 : 4, lo = (1UL << bits) - 1;
            ULONG *tr = tmp + sy * dw;
            if (img)
                CopyMem(img + sy * rb * planes, row, rb * planes);
            else if (!unpack(&p, e, pbm ? (UBYTE *)chunky : row, pbm ? prb : rb * np, comp == 1))
                break;                      /* cut short: the rest stays black */
            if (!pbm)
                row_chunky(row, rb, planes, chunky);
            for (x = 0; x < sw; x++) {
                if (ham) {
                    ULONG v = cc[x] & lo, ctl = cc[x] >> bits;
                    v = bits == 4 ? v * 17 : (v << 2) | (v >> 4);
                    if (ctl == 0)
                        rgb = base[cc[x] & lo];
                    else if (ctl == 1)
                        rgb = (rgb & 0xFFFF00) | v;
                    else if (ctl == 2)
                        rgb = (rgb & 0x00FFFF) | (v << 16);
                    else
                        rgb = (rgb & 0xFF00FF) | (v << 8);
                    srow[x] = rgb;
                } else {
                    srow[x] = t->pal[cc[x]];
                }
            }
            for (x = 0; x < dw; x++) {
                ULONG r = 0, g = 0, bl = 0, wsum = 0;
                const UWORD *w = wx + x * kx;
                for (k = 0; k < kx; k++) {
                    ULONG i = fx[x] + k, c;
                    if (!w[k] || i >= sw)
                        continue;
                    c = srow[i];
                    r += ((c >> 16) & 255) * w[k];
                    g += ((c >> 8) & 255) * w[k];
                    bl += (c & 255) * w[k];
                    wsum += w[k];
                }
                if (!wsum)
                    wsum = 1;
                tr[x] = ((r + wsum / 2) / wsum << 16) | ((g + wsum / 2) / wsum << 8) | (bl + wsum / 2) / wsum;
            }
        }
        for (sy = 0; sy < dh; sy++) {       /* then made short, a column at a time */
            const UWORD *w = wy + sy * ky;
            ULONG *o = d->argb + sy * dw;
            for (x = 0; x < dw; x++) {
                ULONG r = 0, g = 0, bl = 0, wsum = 0;
                for (k = 0; k < ky; k++) {
                    ULONG i = fy[sy] + k, c;
                    if (!w[k] || i >= sh)
                        continue;
                    c = tmp[i * dw + x];
                    r += ((c >> 16) & 255) * w[k];
                    g += ((c >> 8) & 255) * w[k];
                    bl += (c & 255) * w[k];
                    wsum += w[k];
                }
                if (!wsum)
                    wsum = 1;
                o[x] = ((r + wsum / 2) / wsum << 16) | ((g + wsum / 2) / wsum << 8) | (bl + wsum / 2) / wsum;
            }
        }
        FreeVec(fx);
        FreeVec(fy);
        FreeVec(wx);
        FreeVec(wy);
        FreeVec(srow);
        FreeVec(tmp);
        thumb_free(t);
        t = d;
        goto told;
    }
    /* the rows, one after another; the ones the thumbnail needs, sampled */
    row = AllocVec(rb * np, MEMF_ANY);
    xoff = AllocVec(dw * sizeof(UWORD), MEMF_ANY);
    chunky = AllocVec(rb * 8, MEMF_ANY);
    if (!row || !xoff || !chunky) {
        thumb_free(t);
        t = NULL;
        goto out;
    }
    c2p_init();
    for (x = 0; x < dw; x++)
        xoff[x] = (x * sw + sw / 2) / dw;
    p = body;
    e = body + bodylen;
    for (sy = 0, y = 0; sy < sh && y < dh; sy++) {
        if (img)                            /* an ANIM's frame, decoded already */
            CopyMem(img + sy * rb * planes, row, rb * planes);
        else if (!unpack(&p, e, pbm ? (UBYTE *)chunky : row, pbm ? prb : rb * np, comp == 1))
            break;                          /* cut short: the rest stays colour 0 */
        if (y < dh && (y * sh + sh / 2) / dh == sy) {
            const UBYTE *c = (const UBYTE *)chunky;
            if (!pbm)
                row_chunky(row, rb, planes, chunky);
            if (ham) {                      /* each pixel's colour, left to right */
                UBYTE *cc = (UBYTE *)chunky;
                ULONG rgb = base[0], bits = planes == 8 ? 6 : 4, lo = (1UL << bits) - 1;
                for (x = 0; x < sw; x++) {
                    ULONG v = cc[x] & lo, ctl = cc[x] >> bits;
                    v = bits == 4 ? v * 17 : (v << 2) | (v >> 4);
                    if (ctl == 0)
                        rgb = base[cc[x] & lo];
                    else if (ctl == 1)
                        rgb = (rgb & 0xFFFF00) | v;
                    else if (ctl == 2)
                        rgb = (rgb & 0x00FFFF) | (v << 16);
                    else
                        rgb = (rgb & 0xFF00FF) | (v << 8);
                    cc[x] = (UBYTE)(((rgb >> 16) & 0xE0) | ((rgb >> 11) & 0x1C) | ((rgb >> 6) & 3));
                }
            }
            do {
                UBYTE *d = t->px + y * t->mod;
                if (dw == sw)
                    CopyMem((APTR)c, d, dw);
                else
                    for (x = 0; x < dw; x++)
                        d[x] = c[xoff[x]];
                y++;
            } while (y < dh && (y * sh + sh / 2) / dh == sy);
        }
    }
    if (!thumb_reduce(t, maxcol)) {
        thumb_free(t);
        t = NULL;
        goto out;
    }
told:
    pi->w = sw;
    pi->h = sh;
    pi->depth = planes;
    pi->mode = camg;
    strcpy(pi->fmt, anim ? "ANIM" : pbm ? "PBM" : "ILBM");
    pi->cls[0] = 0;
out:
    if (afh)
        Close(afh);
    FreeVec(img);
    FreeVec(cm);
    FreeVec(row);
    FreeVec(xoff);
    FreeVec(chunky);
    FreeVec(file);
    return t;
}
