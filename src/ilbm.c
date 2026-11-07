/*
 * AmigaMCE - IFF ILBM pictures, read without a datatype
 * Copyright (c) 2026 Brian King
 *
 * AGS's screenshots (320 x 128, up to 64 colours) and many Pictures are
 * IFF ILBMs. Through picture.datatype one took 200-300 ms on a 68030:
 * the class looked up and opened, the whole picture unpacked and turned
 * into chunky pixels, then read back. AGS reads them itself, as this does:
 * the file in one read, its rows unpacked in turn (ByteRun1 or plain), and
 * only the rows and pixels the thumbnail needs taken from the bitplanes.
 *
 * Colour-table pictures of up to 8 planes, extra halfbrite too. HAM, deep
 * (24-bit) and PBM pictures are left to the datatype (NULL).
 *
 * An IFF ANIM (Videos) is its first frame, an ILBM, read alone (Tron2 is
 * 28 MB): HAM ones too, made into colours here, as the datatype would read
 * the whole file. Its frames are counted on the way, a header each.
 */
#include "amce.h"
#include "animdec.h"

#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define ID(a, b, c, d) (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (d))
#define ILBM_MAX (4UL << 20)    /* larger files: the datatype */
#define CAMG_HAM 0x800
#define CAMG_EHB 0x80

static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static UWORD be16(const UBYTE *p)
{
    return (UWORD)((p[0] << 8) | p[1]);
}

/* An ANIM's frames from here on, and their time: a few bytes of each
 * read, the rest passed by; frames and jiffies: those counted already. */
static void count_frames(BPTR fh, struct PicInfo *pi, ULONG frames, ULONG jiffies)
{
    UBYTE h[52];
    while (frames < 20000 && Read(fh, h, 12) == 12 && be32(h) == ID('F', 'O', 'R', 'M')) {
        LONG len = be32(h + 4) - 4, got = 0;
        if (len < 0)
            break;
        if (be32(h + 8) == ID('I', 'L', 'B', 'M')) {
            frames++;
            got = Read(fh, h, len < (LONG)sizeof(h) ? len : (LONG)sizeof(h));
            if (got >= 8 + 18 && be32(h) == ID('A', 'N', 'H', 'D'))
                jiffies += be32(h + 8 + 14);
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
    r = Read(fh, head, 12) == 12 && be32(head) == ID('F', 'O', 'R', 'M') &&
        be32(head + 8) == ID('A', 'N', 'I', 'M');
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
    if (Read(fh, head, 12) == 12 && be32(head) == ID('F', 'O', 'R', 'M') &&
        be32(head + 8) == ID('A', 'N', 'I', 'M')) {
        *anim = TRUE;                       /* its first frame: FORM ILBM */
        /* its sounds first, if it has any (8SVX): passed by */
        while (Read(fh, head, 12) == 12 && be32(head) == ID('F', 'O', 'R', 'M') &&
               be32(head + 8) != ID('I', 'L', 'B', 'M') && (size = be32(head + 4)) >= 4 &&
               Seek(fh, size - 4 + (size & 1), OFFSET_CURRENT) >= 0)
            ;
        if (be32(head) == ID('F', 'O', 'R', 'M') &&
            be32(head + 8) == ID('I', 'L', 'B', 'M') && (size = be32(head + 4)) >= 4 &&
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
    if (be32(head) == ID('F', 'O', 'R', 'M') &&
        be32(head + 8) == ID('I', 'L', 'B', 'M') && Seek(fh, 0, OFFSET_END) >= 0 &&
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
#define PICK_FRAMES 150
#define PICK_BYTES  (2UL << 20)
#define PICK_EVERY  3
#define PICK_BLANK  940             /* of 1000 points one colour: blank */

/* How much of a frame (interleaved rows: rb bytes a plane) is one colour,
 * of 1000: 40 x 30 points looked at. */
static ULONG blank_score(const UBYTE *img, ULONG rb, UBYTE planes, ULONG sw, ULONG sh)
{
    static UWORD hist[256];
    ULONG x, y, most = 0;
    UBYTE k;
    memset(hist, 0, sizeof(hist));
    for (y = 0; y < 30; y++) {
        const UBYTE *r = img + (y * sh / 30) * rb * planes;
        for (x = 0; x < 40; x++) {
            ULONG px = x * sw / 40, c = 0;
            for (k = 0; k < planes; k++)
                if (r[k * rb + (px >> 3)] & (0x80 >> (px & 7)))
                    c |= 1UL << k;
            if (++hist[c] > most)
                most = hist[c];
        }
    }
    return most * 1000 / 1200;
}

/* The frame to show, decoded into a new buffer of interleaved rows (the
 * caller frees it); its colours into cm (*cml bytes) if a frame changed
 * them. Frames read meanwhile counted into *frames and *jif. */
static UBYTE *anim_pick(BPTR fh, const UBYTE *body, ULONG bodylen, ULONG sw, ULONG sh,
                        UBYTE planes, UBYTE masking, UBYTE comp, UBYTE *cm, ULONG *cml,
                        ULONG *frames, ULONG *jif)
{
    ULONG rb = ((sw + 15) >> 4) << 1, fsz = rb * planes * sh, best, got = 0, fl = 0;
    UBYTE *a, *buf[2], *keep, *f = NULL, *cur = NULL, h[12];
    static UBYTE cm_now[768];
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
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 8; i++)
            pl[k].plane[i] = buf[k] + i * rb;
        pl[k].stride = rb * planes;
        pl[k].depth = planes;
        pl[k].rb = rb;
        pl[k].h = sh;
    }
    animdec_body(body, bodylen, &pl[0], comp, masking);
    best = blank_score(buf[0], rb, planes, sw, sh);
    if (best < PICK_BLANK || !keep)
        return a;
    CopyMem(buf[0], buf[1], fsz);
    CopyMem(buf[0], keep, fsz);
    CopyMem(cm, cm_now, cmn);
    for (n = 1; n < PICK_FRAMES && got < PICK_BYTES; n++) {
        const UBYTE *anhd = NULL, *dlta = NULL, *bd = NULL, *p, *e;
        ULONG dl = 0, bl = 0, len, sc;
        if (Read(fh, h, 12) != 12 || be32(h) != ID('F', 'O', 'R', 'M') || be32(h + 4) < 4)
            break;
        len = be32(h + 4) - 4;
        if (be32(h + 8) != ID('I', 'L', 'B', 'M')) {
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
            ULONG id = be32(p), cl = be32(p + 4);
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
            *jif += be32(anhd + 14);
        k = n & 1;
        if (anhd && anhd[18] == 1)          /* from the last frame, not two back */
            CopyMem(buf[!k], buf[k], fsz);
        if (anhd && anhd[0] == 0 && bd)
            animdec_body(bd, bl, &pl[k], comp, masking);
        else if (dlta && animdec_delta(dlta, dl, &pl[k], anhd ? anhd[0] : 5,
                                       anhd ? be32(anhd + 20) : 0) == -2)
            break;                          /* not one this knows: the best so far */
        cur = buf[k];
        if (n % PICK_EVERY)
            continue;
        if ((sc = blank_score(cur, rb, planes, sw, sh)) < best) {
            best = sc;
            CopyMem(cur, keep, fsz);
            CopyMem(cm_now, cm, cmn);
            *cml = cmn;
            if (best < PICK_BLANK)
                break;                      /* a good one */
        }
    }
    FreeVec(f);
    CopyMem(keep, a, fsz);
    return a;
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
    BOOL anim, ham;
    ULONG base[64];
    BPTR afh = 0;
    UBYTE *img = NULL;
    static UBYTE cm[768];

    if (!(file = read_file(path, &len, &anim, pi, &afh)))
        return NULL;
    /* the chunks */
    e = file + (be32(file + 4) + 8 < len ? be32(file + 4) + 8 : len);
    for (p = file + 12; p + 8 <= e;) {
        ULONG id = be32(p), cl = be32(p + 4);
        const UBYTE *d = p + 8;
        if (cl > (ULONG)(e - d))
            cl = e - d;
        if (id == ID('B', 'M', 'H', 'D') && cl >= 20)
            bmhd = d;
        else if (id == ID('C', 'M', 'A', 'P')) {
            cmap = d;
            cmaplen = cl;
        } else if (id == ID('C', 'A', 'M', 'G') && cl >= 4)
            camg = be32(d);
        else if (id == ID('B', 'O', 'D', 'Y')) {
            body = d;
            bodylen = cl;
            break;                          /* the rest is after the picture */
        }
        p = d + ((cl + 1) & ~1UL);
    }
    if (!bmhd || !cmap || !body)
        goto out;
    sw = be16(bmhd);
    sh = be16(bmhd + 2);
    planes = bmhd[8];
    masking = bmhd[9];
    comp = bmhd[10];
    ham = (camg & CAMG_HAM) && (planes == 6 || planes == 8);
    if (!sw || !sh || !planes || planes > 8 || comp > 1 || ((camg & CAMG_HAM) && !(anim && ham)) ||
        cmaplen < 6)
        goto out;
    if (afh) {                              /* an ANIM: the frame worth showing, and the count */
        ULONG frames = 1, jif = 0;
        cmaplen = cmaplen < sizeof(cm) ? cmaplen : sizeof(cm);
        CopyMem((APTR)cmap, cm, cmaplen);
        img = anim_pick(afh, body, bodylen, sw, sh, planes, masking, comp, cm, &cmaplen, &frames,
                        &jif);
        cmap = cm;
        count_frames(afh, pi, frames, jif);
        Close(afh);
        afh = 0;
    }

    pic_fit(sw, sh, tall, w, h, whole, &dw, &dh);
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

    /* the rows, one after another; the ones the thumbnail needs, sampled */
    rb = ((sw + 15) >> 4) << 1;
    np = planes + (masking == 1 ? 1 : 0);
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
        else if (!unpack(&p, e, row, rb * np, comp == 1))
            break;                          /* cut short: the rest stays colour 0 */
        if (y < dh && (y * sh + sh / 2) / dh == sy) {
            const UBYTE *c = (const UBYTE *)chunky;
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
    pi->w = sw;
    pi->h = sh;
    pi->depth = planes;
    pi->mode = camg;
    strcpy(pi->fmt, anim ? "ANIM" : "ILBM");
    pi->cls[0] = 0;
out:
    if (afh)
        Close(afh);
    FreeVec(img);
    FreeVec(row);
    FreeVec(xoff);
    FreeVec(chunky);
    FreeVec(file);
    return t;
}
