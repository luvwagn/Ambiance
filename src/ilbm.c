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
 */
#include "amce.h"

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

/* The whole file, if it is an ILBM (else NULL). */
static UBYTE *read_file(const char *path, ULONG *len)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    UBYTE head[12], *buf = NULL;
    LONG size;
    BPTR fh;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return NULL;
    if (Read(fh, head, 12) == 12 && be32(head) == ID('F', 'O', 'R', 'M') &&
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

    if (!(file = read_file(path, &len)))
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
    if (!sw || !sh || !planes || planes > 8 || comp > 1 || (camg & CAMG_HAM) || cmaplen < 6)
        goto out;

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
        if (!unpack(&p, e, row, rb * np, comp == 1))
            break;                          /* cut short: the rest stays colour 0 */
        if (y < dh && (y * sh + sh / 2) / dh == sy) {
            const UBYTE *c = (const UBYTE *)chunky;
            row_chunky(row, rb, planes, chunky);
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
    strcpy(pi->fmt, "ILBM");
out:
    FreeVec(row);
    FreeVec(xoff);
    FreeVec(chunky);
    FreeVec(file);
    return t;
}
