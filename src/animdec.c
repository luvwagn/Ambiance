/*
 * Ambiance - IFF ANIM frames decoded into bitplanes (animdec.h)
 * Copyright (c) 2026 Brian King
 *
 * The deltas are vertical: a plane at a time, a column (a byte, word or
 * long wide) at a time, top to bottom, each run of rows skipped, filled
 * with one value or copied. Written straight into the planes shown, so
 * on a 68030 a frame costs about what changed in it. Every count is held
 * to the picture: a damaged file draws wrongly, never outside it.
 */
#include <string.h>
#include "animdec.h"

typedef unsigned char u8;

static unsigned long be32(const u8 *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

static unsigned long unit_at(const u8 *p, int size)
{
    return size == 4 ? be32(p) : size == 2 ? (unsigned long)((p[0] << 8) | p[1]) : p[0];
}

int animdec_known(int op)
{
    return op == 5 || op == 7 || op == 8;
}

/* ---- ILBM BODY */
static int unpack_row(const u8 **pp, const u8 *e, u8 *out, int n, int comp)
{
    const u8 *p = *pp;
    if (!comp) {
        if (e - p < n)
            return -1;
        if (out)
            memcpy(out, p, n);
        *pp = p + n;
        return 0;
    }
    while (n > 0) {
        int c;
        if (p >= e)
            return -1;
        c = (signed char)*p++;
        if (c >= 0) {
            c++;
            if (c > n || e - p < c)
                return -1;
            if (out) {
                memcpy(out, p, c);
                out += c;
            }
            p += c;
            n -= c;
        } else if (c != -128) {
            c = -c + 1;
            if (c > n || p >= e)
                return -1;
            if (out) {
                memset(out, *p, c);
                out += c;
            }
            p++;
            n -= c;
        }
    }
    *pp = p;
    return 0;
}

int animdec_body(const u8 *body, long len, const struct AnimPlanes *pl, int comp, int masking)
{
    const u8 *p = body, *e = body + len;
    int y, k, np = pl->depth + (masking == 1);
    for (y = 0; y < pl->h; y++)
        for (k = 0; k < np; k++)
            if (unpack_row(&p, e, k < pl->depth ? pl->plane[k] + y * pl->stride : NULL, pl->rb,
                           comp == 1) < 0)
                return -1;
    return 0;
}

/* ---- operation 5: byte vertical delta. Per plane, per byte column: a
 * count of ops, then each op: 0 a run (count, value), with the top bit
 * that many bytes (the low 7 bits), a row each, else a skip of that many
 * rows. */
static int delta5(const u8 *d, long len, const struct AnimPlanes *pl, int xor)
{
    const u8 *e = d + len;
    long stride = pl->stride;
    int p;
    if (len < 32)
        return -1;
    for (p = 0; p < pl->depth && p < 8; p++) {
        unsigned long off = be32(d + p * 4);
        const u8 *s;
        int col;
        if (!off)
            continue;                       /* the plane is as it was */
        if (off >= (unsigned long)len)
            return -1;
        s = d + off;
        for (col = 0; col < pl->rb; col++) {
            u8 *dst = pl->plane[p] + col;
            int ops, row = 0;
            if (s >= e)
                return -1;
            for (ops = *s++; ops > 0; ops--) {
                int op, n;
                if (s >= e)
                    return -1;
                op = *s++;
                if (op == 0) {                  /* a run of one value */
                    u8 v;
                    if (e - s < 2)
                        return -1;
                    n = s[0];
                    v = s[1];
                    s += 2;
                    if (n > pl->h - row)
                        n = pl->h - row > 0 ? pl->h - row : 0;
                    row += n;
                    if (xor)
                        for (; n > 0; n--, dst += stride)
                            *dst ^= v;
                    else
                        for (; n > 0; n--, dst += stride)
                            *dst = v;
                } else if (!(op & 0x80)) {      /* rows left as they were */
                    n = op;
                    if (n > pl->h - row)
                        n = pl->h - row > 0 ? pl->h - row : 0;
                    row += n;
                    dst += n * stride;
                } else {                        /* bytes, a row each */
                    int k;
                    op &= 0x7F;
                    if (e - s < op)
                        return -1;
                    n = op;
                    if (n > pl->h - row)
                        n = pl->h - row > 0 ? pl->h - row : 0;
                    row += n;
                    if (xor)
                        for (k = 0; k < n; k++, dst += stride)
                            *dst ^= s[k];
                    else
                        for (k = 0; k < n; k++, dst += stride)
                            *dst = s[k];
                    s += op;
                }
            }
        }
    }
    return 0;
}

static void put_unit(u8 *dst, const u8 *src, int size)
{
    if (size == 4)
        memcpy(dst, src, 4);
    else
        memcpy(dst, src, 2);
}

/* ---- operations 7 and 8: the same runs, in words or longs (bits 1)
 * across the columns. In 8 the counts and ops are units too, in one list
 * with the data; in 7 they are bytes in a list of their own (pointers
 * 0-7), the data in another (8-15). A width not a whole number of longs
 * ends in a word column. */
static int delta78(const u8 *d, long len, const struct AnimPlanes *pl, int op8, int longs)
{
    const u8 *e = d + len;
    long stride = pl->stride;
    int p, need = op8 ? 32 : 64;
    if (len < need)
        return -1;
    for (p = 0; p < pl->depth && p < 8; p++) {
        unsigned long off = be32(d + p * 4), doff = op8 ? 0 : be32(d + 32 + p * 4);
        const u8 *s, *v;
        int bx;
        if (!off)
            continue;
        if (off >= (unsigned long)len || (!op8 && (!doff || doff >= (unsigned long)len)))
            return -1;
        s = d + off;                        /* ops (and, for 8, the data) */
        v = op8 ? NULL : d + doff;          /* 7: the data */
        for (bx = 0; bx < pl->rb;) {
            int size = longs && pl->rb - bx >= 4 ? 4 : 2;
            int osz = op8 ? size : 1;       /* an op's size */
            u8 *dst = pl->plane[p] + bx;
            unsigned long ops;
            int row = 0;
            if (e - s < osz)
                return -1;
            ops = unit_at(s, osz);
            s += osz;
            for (; ops > 0; ops--) {
                unsigned long op, top = osz == 4 ? 0x80000000UL : osz == 2 ? 0x8000UL : 0x80UL;
                long n;
                if (e - s < osz)
                    return -1;
                op = unit_at(s, osz);
                s += osz;
                if (op == 0) {                  /* a run: its count, then the value */
                    const u8 *val;
                    if (e - s < osz)
                        return -1;
                    n = (long)unit_at(s, osz);
                    s += osz;
                    if (op8) {
                        if (e - s < size)
                            return -1;
                        val = s;
                        s += size;
                    } else {
                        if (e - v < size)
                            return -1;
                        val = v;
                        v += size;
                    }
                    if (n > pl->h - row)
                        n = pl->h - row > 0 ? pl->h - row : 0;
                    row += n;
                    for (; n > 0; n--, dst += stride)
                        put_unit(dst, val, size);
                } else if (!(op & top)) {       /* skip */
                    n = (long)op;
                    if (n > pl->h - row)
                        n = pl->h - row > 0 ? pl->h - row : 0;
                    row += n;
                    dst += n * stride;
                } else {                        /* that many units, a row each */
                    const u8 **src = op8 ? &s : &v;
                    long k, want = (long)(op & (top - 1));
                    if (want > (e - *src) / size)
                        return -1;
                    n = want;
                    if (n > pl->h - row)
                        n = pl->h - row > 0 ? pl->h - row : 0;
                    row += n;
                    for (k = 0; k < n; k++, dst += stride)
                        put_unit(dst, *src + k * size, size);
                    *src += want * size;
                }
            }
            bx += size;
        }
    }
    return 0;
}

/* ---- operation 8 in words, the usual kind (Tron2): delta78's runs with
 * every size fixed, so each is a few moves; a value copied a word at a
 * time (the planes' columns and the data both start on even bytes, IFF
 * chunks being word aligned). */
static unsigned be16(const u8 *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

/* A word moved as it is (its bytes in their order, so on any host): the
 * planes' columns and the data are on even bytes; a damaged file's odd
 * offset is a slower access on a 68020, not a fault. */
typedef unsigned short __attribute__((may_alias)) w16;

static int delta8w(const u8 *d, long len, const struct AnimPlanes *pl)
{
    const u8 *e = d + len;
    long stride = pl->stride;
    int p, h = pl->h;
    if (len < 32)
        return -1;
    for (p = 0; p < pl->depth && p < 8; p++) {
        unsigned long off = be32(d + p * 4);
        const u8 *s;
        int bx;
        if (!off)
            continue;
        if (off >= (unsigned long)len)
            return -1;
        s = d + off;
        for (bx = 0; bx < pl->rb; bx += 2) {
            u8 *dst = pl->plane[p] + bx;
            unsigned ops;
            int row = 0;
            if (e - s < 2)
                return -1;
            ops = be16(s);
            s += 2;
            for (; ops > 0; ops--) {
                unsigned op;
                int n;
                if (e - s < 2)
                    return -1;
                op = be16(s);
                s += 2;
                if (op == 0) {                  /* a run: its count, then the value */
                    w16 v;
                    if (e - s < 4)
                        return -1;
                    n = (int)be16(s);
                    v = *(const w16 *)(s + 2);
                    s += 4;
                    if (n > h - row)
                        n = h - row > 0 ? h - row : 0;
                    row += n;
                    for (; n > 0; n--, dst += stride)
                        *(w16 *)dst = v;
                } else if (!(op & 0x8000)) {    /* skip */
                    n = (int)op;
                    if (n > h - row)
                        n = h - row > 0 ? h - row : 0;
                    row += n;
                    dst += n * stride;
                } else {                        /* that many words, a row each */
                    int want = (int)(op & 0x7FFF), k;
                    if (want > (e - s) / 2)
                        return -1;
                    n = want;
                    if (n > h - row)
                        n = h - row > 0 ? h - row : 0;
                    row += n;
                    for (k = 0; k < n; k++, dst += stride)
                        *(w16 *)dst = ((const w16 *)s)[k];
                    s += 2 * want;
                }
            }
        }
    }
    return 0;
}

int animdec_delta(const u8 *d, long len, const struct AnimPlanes *pl, int op, unsigned long bits)
{
    switch (op) {
    case 5:
        return delta5(d, len, pl, (bits & 2) != 0);
    case 7:
        return delta78(d, len, pl, 0, (bits & 1) != 0);
    case 8:
        return bits & 1 ? delta78(d, len, pl, 1, 1) : delta8w(d, len, pl);
    }
    return -2;
}
