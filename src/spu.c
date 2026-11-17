/*
 * Ambiance - a DVD's subpictures: its menus' buttons and its subtitles
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * A subpicture unit comes in private stream 1's packets (substreams
 * 0x20-0x3F), its first two bytes its size, the next two where its
 * control sequences start. Those say when it shows and stops (in 1024ths
 * of the 90 kHz clock after the first packet's time), its area, each of
 * its four pixel values' colour (an index into the disc's 16-colour table)
 * and opacity, and where its two fields' pixels are: run lengths of 2-bit
 * values in nibbles (1 to 4 of them a run), each line ending on a byte.
 *
 * Blended into the picture where it is shown, at the picture's own size,
 * in YUV (the disc's colours are YUV): over the card's overlay and drawn
 * alike. A menu's highlighted button is the same subpicture, its values
 * inside the button's rectangle given the highlight's colours.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "spu.h"

#define SPU_MAX   65536             /* a unit's size is 16 bits */
#define SPU_QUEUE 4                 /* decoded, waiting for their time */

struct Spu {
    UBYTE *buf;                     /* the unit being gathered */
    ULONG have, size;
    ULONG pts;
    BOOL pts_ok;
    struct SpuPic q[SPU_QUEUE];     /* decoded, oldest first */
    int nq;
    ULONG decoded, bad;
};

struct Spu *spu_new(void)
{
    struct Spu *s = AllocVec(sizeof(*s), MEMF_ANY | MEMF_CLEAR);
    if (s && !(s->buf = AllocVec(SPU_MAX, MEMF_ANY))) {
        FreeVec(s);
        s = NULL;
    }
    return s;
}

static void pic_free(struct SpuPic *p)
{
    if (p->pix)
        FreeVec(p->pix);
    memset(p, 0, sizeof(*p));
}

static void drop_first(struct Spu *s)
{
    int i;
    pic_free(&s->q[0]);
    for (i = 1; i < s->nq; i++)
        s->q[i - 1] = s->q[i];
    memset(&s->q[--s->nq], 0, sizeof(s->q[0]));
}

void spu_reset(struct Spu *s)
{
    if (!s)
        return;
    while (s->nq)
        drop_first(s);
    s->have = s->size = 0;
    s->pts_ok = FALSE;
}

void spu_free(struct Spu *s)
{
    if (!s)
        return;
    spu_reset(s);
    FreeVec(s->buf);
    FreeVec(s);
}

void spu_counts(struct Spu *s, ULONG *decoded, ULONG *bad)
{
    *decoded = s ? s->decoded : 0;
    *bad = s ? s->bad : 0;
}

/* ---- decoding */

struct Nib {
    const UBYTE *d;
    ULONG at, end;                  /* in nibbles */
};

static ULONG nib(struct Nib *b)
{
    ULONG v;
    if (b->at >= b->end)
        return 0;
    v = b->d[b->at >> 1];
    v = b->at & 1 ? v & 15 : v >> 4;
    b->at++;
    return v;
}

/* One field's lines (every other row from row0) into pix, from offset
 * off in the unit: FALSE if it runs out first. */
static BOOL field(const UBYTE *u, ULONG n, ULONG off, UBYTE *pix, UWORD w, UWORD h, int row0)
{
    struct Nib b;
    int row;
    if (off >= n)
        return FALSE;
    b.d = u;
    b.at = off * 2;
    b.end = n * 2;
    for (row = row0; row < h; row += 2) {
        UBYTE *o = pix + (ULONG)row * w;
        ULONG x = 0;
        while (x < w) {
            ULONG v = nib(&b), len;
            if (v < 0x4) {
                v = (v << 4) | nib(&b);
                if (v < 0x10) {
                    v = (v << 4) | nib(&b);
                    if (v < 0x40) {
                        v = (v << 4) | nib(&b);
                        if (v < 0x4)
                            v |= (w - x) << 2;     /* (a run of 0: to the line's end) */
                    }
                }
            }
            len = v >> 2;
            if (!len || x + len > w)
                len = w - x;
            memset(o + x, (int)(v & 3), len);
            x += len;
            if (b.at > b.end)
                return FALSE;
        }
        b.at = (b.at + 1) & ~1UL;           /* each line ends on a byte */
    }
    return TRUE;
}

BOOL spu_decode(const UBYTE *u, ULONG n, ULONG pts, struct SpuPic *out)
{
    ULONG size, dcsq, at, last = 0, off1 = 0, off2 = 0;
    BOOL area = FALSE, started = FALSE;
    int guard;
    memset(out, 0, sizeof(*out));
    if (n < 4)
        return FALSE;
    size = ((ULONG)u[0] << 8) | u[1];
    dcsq = ((ULONG)u[2] << 8) | u[3];
    if (size > n || dcsq + 4 > size)
        return FALSE;
    out->alpha[0] = 0;
    out->alpha[1] = out->alpha[2] = out->alpha[3] = 15;
    out->col[1] = 1, out->col[2] = 2, out->col[3] = 3;
    /* its control sequences, each pointing to the next (the last to itself) */
    for (at = dcsq, guard = 0; at + 4 <= size && guard < 64; guard++) {
        ULONG delay = ((ULONG)u[at] << 8) | u[at + 1], next = ((ULONG)u[at + 2] << 8) | u[at + 3];
        ULONG c = at + 4, t = pts + delay * 1024;
        BOOL end = FALSE;
        while (c < size && !end) {
            switch (u[c++]) {
            case 0x00:                      /* forced start */
                out->forced = TRUE;
                /* fall through */
            case 0x01:                      /* start */
                if (!started) {
                    out->start = t;
                    started = TRUE;
                }
                break;
            case 0x02:                      /* stop */
                out->stop = t;
                out->has_stop = TRUE;
                break;
            case 0x03:                      /* colours: values 3, 2, 1, 0 */
                if (c + 2 > size)
                    return FALSE;
                out->col[3] = u[c] >> 4, out->col[2] = u[c] & 15;
                out->col[1] = u[c + 1] >> 4, out->col[0] = u[c + 1] & 15;
                c += 2;
                break;
            case 0x04:                      /* opacities, the same order */
                if (c + 2 > size)
                    return FALSE;
                out->alpha[3] = u[c] >> 4, out->alpha[2] = u[c] & 15;
                out->alpha[1] = u[c + 1] >> 4, out->alpha[0] = u[c + 1] & 15;
                c += 2;
                break;
            case 0x05: {                    /* the area: x0, x1, y0, y1, 12 bits each */
                ULONG x0, x1, y0, y1;
                if (c + 6 > size)
                    return FALSE;
                x0 = ((ULONG)u[c] << 4) | (u[c + 1] >> 4);
                x1 = (((ULONG)u[c + 1] & 15) << 8) | u[c + 2];
                y0 = ((ULONG)u[c + 3] << 4) | (u[c + 4] >> 4);
                y1 = (((ULONG)u[c + 4] & 15) << 8) | u[c + 5];
                c += 6;
                if (x1 < x0 || y1 < y0 || x1 >= 1024 || y1 >= 1024)
                    return FALSE;
                out->x = (UWORD)x0, out->y = (UWORD)y0;
                out->w = (UWORD)(x1 - x0 + 1), out->h = (UWORD)(y1 - y0 + 1);
                area = TRUE;
                break;
            }
            case 0x06:                      /* the fields' pixels */
                if (c + 4 > size)
                    return FALSE;
                off1 = ((ULONG)u[c] << 8) | u[c + 1];
                off2 = ((ULONG)u[c + 2] << 8) | u[c + 3];
                c += 4;
                break;
            case 0x07: {                    /* colour and contrast changes: passed over */
                ULONG k;
                if (c + 2 > size)
                    return FALSE;
                k = ((ULONG)u[c] << 8) | u[c + 1];
                if (k < 2)
                    return FALSE;
                c += k;
                break;
            }
            case 0xFF:
                end = TRUE;
                break;
            default:
                return FALSE;               /* (not a command: not a unit) */
            }
        }
        if (next == at || next <= last)
            break;
        last = at;
        at = next;
    }
    if (!area || !off1 || !off2 || !out->w || !out->h)
        return FALSE;
    if (!started)
        out->start = pts;
    {
        /* its pixels, then each row's span of values other than 0 */
        ULONG np = ((ULONG)out->w * out->h + 1) & ~1UL, r;
        if (!(out->pix = AllocVec(np + (ULONG)out->h * 4, MEMF_ANY | MEMF_CLEAR)))
            return FALSE;
        out->span = (UWORD *)(out->pix + np);
        if (!field(u, size, off1, out->pix, out->w, out->h, 0) ||
            !field(u, size, off2, out->pix, out->w, out->h, 1)) {
            /* (cut short: what was decoded is kept, the rest clear) */
        }
        out->nx0 = out->w;
        out->ny0 = out->h;
        out->nx1 = out->ny1 = 0;
        for (r = 0; r < out->h; r++) {
            const UBYTE *row = out->pix + r * out->w;
            ULONG a = 0, e = out->w;
            while (a < e && !row[a])
                a++;
            while (e > a && !row[e - 1])
                e--;
            out->span[2 * r] = (UWORD)a;
            out->span[2 * r + 1] = (UWORD)e;
            if (a < e) {
                if (a < out->nx0)
                    out->nx0 = (UWORD)a;
                if (e > out->nx1)
                    out->nx1 = (UWORD)e;
                if (r < out->ny0)
                    out->ny0 = (UWORD)r;
                out->ny1 = (UWORD)(r + 1);
            }
        }
        if (!out->nx1)
            out->nx0 = out->ny0 = 0;
    }
    return TRUE;
}

/* ---- gathering */

static void unit_done(struct Spu *s)
{
    struct SpuPic p;
    if (spu_decode(s->buf, s->size, s->pts_ok ? s->pts : 0, &p)) {
        if (!s->pts_ok && s->nq)
            p.start = s->q[s->nq - 1].start;    /* (no time of its own: with the last) */
        if (s->nq == SPU_QUEUE)
            drop_first(s);
        s->q[s->nq++] = p;
        s->decoded++;
    } else
        s->bad++;
    s->have = s->size = 0;
    s->pts_ok = FALSE;
}

void spu_put(struct Spu *s, const UBYTE *d, ULONG n, BOOL has_pts, ULONG pts)
{
    if (!s || !n)
        return;
    if (!s->have) {
        s->pts = pts;
        s->pts_ok = has_pts;
    }
    while (n) {
        ULONG k;
        if (s->have < 2) {
            s->buf[s->have++] = *d++;
            n--;
            if (s->have == 2) {
                s->size = ((ULONG)s->buf[0] << 8) | s->buf[1];
                if (s->size < 4) {          /* (not a unit's start: dropped) */
                    s->have = s->size = 0;
                    s->bad++;
                }
            }
            continue;
        }
        k = s->size - s->have;
        if (k > n)
            k = n;
        CopyMem((APTR)d, s->buf + s->have, k);
        s->have += k;
        d += k;
        n -= k;
        if (s->have >= s->size) {
            unit_done(s);
            if (n)                          /* (a packet's rest: the next unit's start) */
                s->pts_ok = FALSE;
        }
    }
}

const struct SpuPic *spu_at(struct Spu *s, ULONG pts, BOOL forced_only)
{
    struct SpuPic *p;
    if (!s)
        return NULL;
    /* the newest whose time has come; those before it are over */
    while (s->nq > 1 && (LONG)(pts - s->q[1].start) >= 0)
        drop_first(s);
    if (!s->nq)
        return NULL;
    p = &s->q[0];
    if ((LONG)(pts - p->start) < 0)
        return NULL;                        /* (not yet) */
    if (p->has_stop && (LONG)(pts - p->stop) >= 0)
        return NULL;
    if (forced_only && !p->forced)
        return NULL;
    return p;
}

/* ---- blending */

static UBYTE mix(UBYTE o, UBYTE n, UBYTE a)
{
    return a >= 15 ? n : (UBYTE)(o + (((LONG)n - (LONG)o) * ((LONG)a * 17) + 128) / 255);
}

void spu_blend420(const struct SpuPic *p, const struct SpuHl *hl, const ULONG *clut, UBYTE *y,
                  UBYTE *cb, UBYTE *cr, ULONG ymod, ULONG cmod, ULONG pw, ULONG ph)
{
    UBYTE cy[2][4], ccb[2][4], ccr[2][4], ca[2][4];
    ULONG r, x;
    int k, v;
    BOOL sparse;
    if (!p || !p->pix || p->x >= pw || p->y >= ph)
        return;
    if (hl && !hl->on)
        hl = NULL;
    /* the values' colours: [0] the subpicture's own, [1] the highlight's */
    for (k = 0; k < (hl ? 2 : 1); k++)
        for (v = 0; v < 4; v++) {
            ULONG c = clut[(k ? hl->col[v] : p->col[v]) & 15];
            cy[k][v] = (UBYTE)(c >> 16);
            ccr[k][v] = (UBYTE)(c >> 8);
            ccb[k][v] = (UBYTE)c;
            ca[k][v] = k ? hl->alpha[v] : p->alpha[v];
        }
    /* value 0 clear (its own and the highlight's): only each row's span
     * of the others */
    sparse = p->span && !ca[0][0] && !(hl && ca[1][0]);
    for (r = sparse ? p->ny0 : 0; r < (sparse ? p->ny1 : p->h) && p->y + r < ph; r++) {
        ULONG py = p->y + r, xe = sparse ? p->span[2 * r + 1] : p->w;
        const UBYTE *s = p->pix + r * p->w;
        UBYTE *yo = y + py * ymod;
        BOOL hrow = hl && py >= hl->y0 && py <= hl->y1, crow = !(py & 1);
        UBYTE *cbo = cb + (py >> 1) * cmod, *cro = cr + (py >> 1) * cmod;
        for (x = sparse ? p->span[2 * r] : 0; x < xe && p->x + x < pw; x++) {
            ULONG px = p->x + x;
            int val = s[x], h = hrow && px >= hl->x0 && px <= hl->x1;
            UBYTE a = ca[h][val];
            if (!a)
                continue;
            yo[px] = mix(yo[px], cy[h][val], a);
            if (crow && !(px & 1)) {        /* (a 2 x 2 block's chroma: its top left's) */
                cbo[px >> 1] = mix(cbo[px >> 1], ccb[h][val], a);
                cro[px >> 1] = mix(cro[px >> 1], ccr[h][val], a);
            }
        }
    }
}

BOOL spu_extent(const struct SpuPic *p, const struct SpuHl *hl, ULONG *x0, ULONG *y0, ULONG *x1,
                ULONG *y1)
{
    ULONG a0, b0, a1, b1;
    if (!p || !p->pix || !p->w || !p->h)
        return FALSE;
    if (hl && !hl->on)
        hl = NULL;
    if (!p->span || p->alpha[0]) {
        a0 = p->x;                          /* (its value 0 shows: all of it) */
        b0 = p->y;
        a1 = p->x + p->w;
        b1 = p->y + p->h;
    } else {
        if (p->nx1) {
            a0 = p->x + p->nx0;
            b0 = p->y + p->ny0;
            a1 = p->x + p->nx1;
            b1 = p->y + p->ny1;
        } else
            a0 = b0 = a1 = b1 = 0;
        if (hl && hl->alpha[0]) {
            /* (the highlight's value 0 shows: its rectangle, inside it) */
            ULONG h0 = hl->x0 > p->x ? hl->x0 : p->x, v0 = hl->y0 > p->y ? hl->y0 : p->y;
            ULONG h1 = (ULONG)hl->x1 + 1 < (ULONG)p->x + p->w ? (ULONG)hl->x1 + 1 : (ULONG)p->x + p->w;
            ULONG v1 = (ULONG)hl->y1 + 1 < (ULONG)p->y + p->h ? (ULONG)hl->y1 + 1 : (ULONG)p->y + p->h;
            if (h1 > h0 && v1 > v0) {
                if (a1 == a0) {
                    a0 = h0, b0 = v0, a1 = h1, b1 = v1;
                } else {
                    a0 = h0 < a0 ? h0 : a0;
                    b0 = v0 < b0 ? v0 : b0;
                    a1 = h1 > a1 ? h1 : a1;
                    b1 = v1 > b1 ? v1 : b1;
                }
            }
        }
    }
    if (a1 <= a0 || b1 <= b0)
        return FALSE;
    *x0 = a0;
    *y0 = b0;
    *x1 = a1;
    *y1 = b1;
    return TRUE;
}
