/*
 * jdec - a JPEG decoder for 68020 Amigas without an FPU
 * Copyright (c) 2026 Brian King, MIT
 *
 * See jdec.h. Written from ITU-T T.81 (the JPEG standard) and the
 * Arai-Agui-Nakajima scaled IDCT; the chroma upsampling filters are the
 * ones libjpeg uses ("fancy" upsampling), so full-size output matches it.
 *
 * 68020 notes: every multiply in the decode is 16 x 16 -> 32 bits
 * (muls.w): MULF() takes a 16-bit value and a constant below 0.5 in Q16,
 * and keeps the high word. Coefficients are kept as 16-bit values.
 */
#include <string.h>
#include "jdec.h"

#define JD_INBUF 4096
#define JD_EOI 1                /* (read_markers between progressive scans: the end) */
#ifndef JD_PROG_LOW
#define JD_PROG_LOW 0           /* (1: the tests' build, prog_low always) */
#endif
#ifndef JD_PROG_MAX
#define JD_PROG_MAX (24L << 20) /* a progressive picture's coefficients kept: at most */
#endif
#define PS 2                    /* extra fraction bits through the 8x8 IDCT */

#ifndef FB
#define FB 9                    /* the fast AC table's lookahead, 9-15 bits */
#endif
#define FMASK ((1 << FB) - 1)

typedef struct {
    uint16_t look[512];         /* 9-bit lookahead: (length << 8) | symbol; 0: longer */
    int32_t fast[1 << FB];      /* AC, code and value in FB bits: value << 16 | k step << 8 | bits (0: no) */
    int32_t maxcode[18];        /* largest code of each length (-1 none) */
    int32_t valoff[18];         /* val[] index of a length's code: valoff + code */
    uint8_t val[256];
    uint8_t ok;
} JHuff;

typedef struct {
    int id, h, v, tq, td, ta;
    int bs;                     /* its blocks' size in output samples: 1, 2, 4, 8 */
    int upx, upy;               /* then made this many times wider and taller, */
    int dnx, dny;               /* or averaged down this many times */
    int32_t rowlen;             /* samples in a band row (blocks in a row x bs) */
    int rows;                   /* rows in a band (v x bs) */
    int32_t vw;                 /* samples in a row that hold the picture */
    int32_t vrows;              /* rows that hold the picture, over all bands */
    uint8_t *buf[2];            /* two bands: this one and the next */
    uint8_t *prev;              /* the last row of the band before */
    uint8_t *up;                /* a row made output width */
    uint8_t *tmp;               /* a row averaged down from dny rows */
    int16_t q[64];              /* its dequantising table at this scale (natural order) */
    const int16_t *sc;          /* each coefficient's scale after q, Q(14-PS): aan[] or flat */
    int dc;                     /* DC prediction */
    /* progressive: every block's coefficients, for sbw x sbh blocks (the
     * MCUs' grid): kn of each, those this scale's IDCT reads (zigzag
     * position k in coef slot[k]; 255: not kept). Of those not kept only
     * whether each is nonzero (nzm: 64 bits a block), which the AC
     * refinement scans need to be read right. kn 1: the DC alone, the AC
     * scans passed over. */
    int16_t *coef;
    uint32_t *nzm;
    int32_t sbw, sbh;
    int kn;
    uint8_t slot[64];
} JComp;

/* A Cr or Cb value's colour terms; 8 bytes, for the 68020's index * 8 */
typedef struct {
    int16_t rb;                 /* Cr: to red; Cb: to blue */
    int16_t pad;
    int32_t g;                  /* to green in Q16; their sum >> 16 (Cb's has the rounding) */
} JCol;

struct JDec {
    JDecIO io;
    JDecInfo info;
    int32_t inpos, inlen;
    int eof, readerr;
    uint32_t bits;
    int nbits;
    int marker;                 /* a marker met in the entropy-coded data */
    int32_t pad;                /* zero bytes fed in after it */
    int damaged;
    int ncomp, hmax, vmax;
    int adobe, adobe_transform, jfif;
    int restart;
    int progressive;            /* SOF2: its scans read into each component's coef first */
    int prog_low;               /* too big kept whole: chroma a step smaller, made bigger */
    int ss, se, ah, al;         /* the progressive scan's spectral selection, approximation */
    int32_t eobrun;             /* blocks left in an end-of-band run */
    int pending;                /* a marker the last scan's data ran into, read next */
    int ns;                     /* components in the scan, their indexes */
    int scomp[4];
    int scale, bsize;           /* 1/2^scale; luma block size out */
    int32_t mcux, mcuy;
    int32_t outw, outh;         /* output size */
    int32_t outwpad;            /* output width of whole MCUs */
    int bandrows;               /* output rows a band */
    uint8_t *rgb;
    uint16_t qt[4][64];
    uint8_t qtok[4];
    int16_t qd[4][64];          /* dequant tables (natural order) */
    JHuff hd[4], ha[4];
    JComp c[4];
    int16_t blk[64];
    uint8_t nz[64];             /* where blk has values, to clear them after */
    int32_t ws[64];
    uint8_t clamp[1024];        /* index & 1023: 0-255 as is, 256-639 255, 640-1023 0 */
    uint8_t cclamp[768];        /* colour sums -256..511, from cclamp + 256 */
    JCol cr[256], cb[256];      /* colour: R (Cr) or B (Cb) offset, and G's part */
    uint8_t in[JD_INBUF];
};

static const uint8_t zigzag[64] = {     /* natural index of each zigzag position */
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const int16_t flat[64] = {        /* the reduced IDCTs: 1 in Q(14-PS) */
#define F4 (1 << (14 - 2))
    F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4,
    F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4,
    F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4,
    F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4, F4
#undef F4
};

static const int16_t aan[64] = {       /* s(u)s(v) in Q14, s(0)=1, s(k)=sqrt2 cos(k pi/16) */
    16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
    22725, 31521, 29692, 26722, 22725, 17855, 12299,  6270,
    21407, 29692, 27969, 25172, 21407, 16819, 11585,  5906,
    19266, 26722, 25172, 22654, 19266, 15137, 10426,  5315,
    16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
    12873, 17855, 16819, 15137, 12873, 10114,  6967,  3552,
     8867, 12299, 11585, 10426,  8867,  6967,  4799,  2446,
     4520,  6270,  5906,  5315,  4520,  3552,  2446,  1247
};

/* x (16 bits) times a constant below 0.5 in Q16: one muls.w, the high word */
#define MULF(x, c) ((int32_t)(int16_t)(x) * (int16_t)(c) >> 16)
#define F0414 27146             /* sqrt2 - 1 */
#define F0152 9977              /* 2 - 2cos(pi/8): 1.847759065 = 2 - this */
#define F0082 5400              /* 1.082392200 - 1 */
#define F0387 25354             /* 3 - 2.613125930 */
/* reduced IDCTs: each output the exact average of the 8 x 8 IDCT's
 * pixels it covers (as libjpeg's), w(u) = c(u)/2 cos(u pi/16) folded in */
#define E0 23170                /* 1/(2 sqrt2) */
#define A1 32138                /* cos(pi/16)/2 */
#define A7 6393                 /* cos(7pi/16)/2 */
#define B3 27246                /* cos(3pi/16)/2 */
#define B5 18205                /* cos(5pi/16)/2 */
#define P2 21407                /* cos(pi/4) cos(pi/8)/2 */
#define P6 8867                 /* cos(pi/4) cos(3pi/8)/2 */
#define C8 4989                 /* 1 - cos(pi/8) */
#define S8 25080                /* cos(3pi/8) */
#define Z1 20995                /* the 8 -> 2 sums: (cos(pi/8) + cos(3pi/8))/2 w(1) */
#define Z7 4176
#define Z3 7373                 /* (cos(pi/8) - cos(3pi/8))/2 w(3) */
#define Z5 4926

/* 8 inputs (f4 drops out) -> 4 averages of pairs */
#define AVG4(f0, f1, f2, f3, f5, f6, f7, y0, y1, y2, y3) do { \
    int32_t E_ = MULF(f0, E0), P_ = MULF(f2, P2) - MULF(f6, P6); \
    int32_t A_ = MULF(f1, A1) - MULF(f7, A7), B_ = MULF(f3, B3) - MULF(f5, B5); \
    int32_t o0_ = A_ - MULF(A_, C8) + MULF(B_, S8), o1_ = MULF(A_, S8) - (B_ - MULF(B_, C8)); \
    y0 = E_ + P_ + o0_; y3 = E_ + P_ - o0_; y1 = E_ - P_ + o1_; y2 = E_ - P_ - o1_; \
} while (0)
/* -> 2 averages of four: E +- s (f2, f4, f6 drop out) */
#define AVG2S(f1, f3, f5, f7) (MULF(f1, Z1) - MULF(f7, Z7) - MULF(f3, Z3) + MULF(f5, Z5))

/* ------------------------------------------------------------------ input */

static int getbyte(JDec *d)
{
    if (d->inpos >= d->inlen) {
        int32_t n;
        if (d->eof)
            return -1;
        n = d->io.read(d->io.ctx, d->in, JD_INBUF);
        if (n <= 0) {
            d->eof = 1;
            if (n < 0)
                d->readerr = 1;
            return -1;
        }
        d->inlen = n > JD_INBUF ? JD_INBUF : n;
        d->inpos = 0;
    }
    return d->in[d->inpos++];
}

static int32_t get16(JDec *d)
{
    int a = getbyte(d), b;
    if (a < 0)
        return -1;
    b = getbyte(d);
    if (b < 0)
        return -1;
    return (a << 8) | b;
}

static int skip(JDec *d, int32_t n)
{
    while (n-- > 0)
        if (getbyte(d) < 0)
            return -1;
    return 0;
}

/* The next marker's code (after any fill bytes), or -1 at the end. */
static int next_marker(JDec *d)
{
    int c;
    do {
        if ((c = getbyte(d)) < 0)
            return -1;
    } while (c != 0xFF);
    do {
        if ((c = getbyte(d)) < 0)
            return -1;
    } while (c == 0xFF);
    return c;
}

/* ------------------------------------------------------------------ headers */

static int build_huff(JHuff *h, const uint8_t *counts, const uint8_t *vals, int total, int ac)
{
    int32_t code = 0;
    int len, i, k = 0;
    h->ok = 0;
    memset(h->look, 0, sizeof(h->look));
    memset(h->val, 0, sizeof(h->val));
    memcpy(h->val, vals, total);
    for (len = 1; len <= 16; len++) {
        int n = counts[len - 1];
        h->valoff[len] = k - code;
        for (i = 0; i < n; i++) {
            if (code >= (1L << len))
                return -1;              /* more codes than this length holds */
            if (len <= 9) {
                int32_t idx = code << (9 - len), m = 1L << (9 - len);
                while (m-- > 0)
                    h->look[idx++] = (uint16_t)((len << 8) | vals[k]);
            }
            code++;
            k++;
        }
        h->maxcode[len] = n ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFFL;
    if (ac) {                           /* run, size and value in one lookup: by code */
        int32_t *f = h->fast;
        memset(f, 0, sizeof(h->fast));
        code = 0;
        k = 0;
        for (len = 1; len <= FB; len++) {
            for (i = 0; i < counts[len - 1]; i++, code++, k++) {
                int rs = vals[k], sz = rs & 15, sh = FB - len;
                int32_t base = code << sh, e, m, vb;
                if (!sz) {                  /* EOB: past the end; ZRL: 15 zeros and a 16th */
                    e = rs == 0 ? 64L * 256 + len : rs == 0xF0 ? 15L * 256 + len : 0;
                    for (m = 1L << sh; m-- > 0; )
                        f[base++] = e;
                } else if (sz <= sh) {
                    sh -= sz;
                    for (vb = 0; vb < (1L << sz); vb++) {
                        int32_t v = vb < (1L << (sz - 1)) ? vb - (1L << sz) + 1 : vb;
                        e = v * 65536 + (rs >> 4) * 256 + len + sz;
                        for (m = 1L << sh; m-- > 0; )
                            f[base++] = e;
                    }
                }
            }
            code <<= 1;
        }
    }
    h->ok = 1;
    return 0;
}

static int read_dht(JDec *d)
{
    int32_t len = get16(d) - 2;
    if (len < 0)
        return JD_EFORMAT;
    while (len > 0) {
        uint8_t counts[16], vals[256];
        int tc_th = getbyte(d), i, total = 0;
        if (tc_th < 0 || len < 17)
            return JD_EFORMAT;
        for (i = 0; i < 16; i++) {
            int c = getbyte(d);
            if (c < 0)
                return JD_EFORMAT;
            counts[i] = (uint8_t)c;
            total += c;
        }
        len -= 17;
        if (total > 256 || total > len || (tc_th & 0x0F) > 3 || (tc_th >> 4) > 1)
            return JD_EFORMAT;
        for (i = 0; i < total; i++) {
            int c = getbyte(d);
            if (c < 0)
                return JD_EFORMAT;
            vals[i] = (uint8_t)c;
        }
        len -= total;
        if (build_huff((tc_th >> 4) ? &d->ha[tc_th & 3] : &d->hd[tc_th & 3], counts, vals, total, tc_th >> 4))
            return JD_EFORMAT;
    }
    return JD_OK;
}

static int read_dqt(JDec *d)
{
    int32_t len = get16(d) - 2;
    if (len < 0)
        return JD_EFORMAT;
    while (len > 0) {
        int pq_tq = getbyte(d), i, wide;
        if (pq_tq < 0 || (pq_tq & 0x0F) > 3 || (pq_tq >> 4) > 1)
            return JD_EFORMAT;
        wide = pq_tq >> 4;
        if (len < 1 + 64 * (wide + 1))
            return JD_EFORMAT;
        for (i = 0; i < 64; i++) {
            int32_t q = wide ? get16(d) : getbyte(d);
            if (q < 0)
                return JD_EFORMAT;
            d->qt[pq_tq & 3][zigzag[i]] = (uint16_t)(q ? q : 1);
        }
        d->qtok[pq_tq & 3] = 1;
        len -= 1 + 64 * (wide + 1);
    }
    return JD_OK;
}

static int read_sof(JDec *d, int kind)
{
    int32_t len = get16(d), w, h;
    int p, n, i;
    if (len < 8 || (p = getbyte(d)) < 0)
        return JD_EFORMAT;
    h = get16(d);
    w = get16(d);
    n = getbyte(d);
    if (h < 0 || w < 0 || n < 0 || len != 8 + 3 * n)
        return JD_EFORMAT;
    d->info.width = w;
    d->info.height = h;
    d->info.ncomp = n;
    d->info.progressive = kind == 2;
    if (n != 1 && n != 3 && n != 4)
        return JD_EFORMAT;
    d->ncomp = n;
    d->hmax = d->vmax = 1;
    for (i = 0; i < n; i++) {
        int id = getbyte(d), hv = getbyte(d), tq = getbyte(d);
        if (id < 0 || hv < 0 || tq < 0)
            return JD_EFORMAT;
        d->c[i].id = id;
        d->c[i].h = hv >> 4;
        d->c[i].v = hv & 15;
        d->c[i].tq = tq;
        d->info.h[i] = (uint8_t)(hv >> 4);
        d->info.v[i] = (uint8_t)(hv & 15);
        if (d->c[i].h < 1 || d->c[i].h > 4 || d->c[i].v < 1 || d->c[i].v > 4 || tq > 3)
            return JD_EFORMAT;
        if (d->c[i].h > d->hmax)
            d->hmax = d->c[i].h;
        if (d->c[i].v > d->vmax)
            d->vmax = d->c[i].v;
    }
    if (n == 3)                 /* RGB: Adobe's transform 0, or ids R G B with no JFIF */
        d->info.rgb = d->adobe ? d->adobe_transform == 0
                    : !d->jfif && d->c[0].id == 'R' && d->c[1].id == 'G' && d->c[2].id == 'B';
    if (n == 4)
        return JD_ECMYK;
    d->progressive = kind == 2;
    if ((kind != 0 && kind != 2) || p != 8)
        return JD_EUNSUPPORTED;
    if (w == 0 || h == 0)
        return JD_EFORMAT;      /* height 0 (DNL) is not decoded */
    if (w > 32767 || h > 32767)
        return JD_ETOOBIG;
    for (i = 0; i < n; i++)
        if (d->hmax % d->c[i].h || d->vmax % d->c[i].v)
            return JD_EUNSUPPORTED;
    return JD_OK;
}

static int read_app(JDec *d, int m)
{
    int32_t len = get16(d) - 2;
    uint8_t b[14];
    int i, n;
    if (len < 0)
        return JD_EFORMAT;
    n = len < 14 ? (int)len : 14;
    for (i = 0; i < n; i++) {
        int c = getbyte(d);
        if (c < 0)
            return JD_EFORMAT;
        b[i] = (uint8_t)c;
    }
    if (m == 0xE0 && n >= 12 && !memcmp(b, "JFIF", 5)) {
        d->jfif = 1;
        d->info.density_unit = b[7];
        d->info.xdensity = (b[8] << 8) | b[9];
        d->info.ydensity = (b[10] << 8) | b[11];
    } else if (m == 0xEE && n >= 12 && !memcmp(b, "Adobe", 5)) {
        d->adobe = 1;
        d->adobe_transform = b[11];
    }
    return skip(d, len - n) ? JD_EFORMAT : JD_OK;
}

/* Markers up to the frame (until_sos 0) or the scan (1); 2: between a
 * progressive picture's scans, the end (EOI, or the file's) JD_EOI. */
static int read_markers(JDec *d, int until_sos)
{
    for (;;) {
        int m = d->pending ? d->pending : next_marker(d), r = JD_OK;
        int32_t len;
        d->pending = 0;
        if (m < 0 && until_sos == 2) {
            d->damaged = 1;             /* (cut off: what came is shown) */
            return d->readerr ? JD_EREAD : JD_EOI;
        }
        if (m < 0)
            return d->readerr ? JD_EREAD : JD_EFORMAT;
        switch (m) {
        case 0xC0: case 0xC1: case 0xC2:
            if (until_sos)
                return JD_EFORMAT;
            return read_sof(d, m - 0xC0);
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA:
        case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return until_sos ? JD_EFORMAT : JD_EUNSUPPORTED;
        case 0xC4:
            r = read_dht(d);
            break;
        case 0xDB:
            r = read_dqt(d);
            break;
        case 0xDD:
            if (get16(d) != 4)
                return JD_EFORMAT;
            d->restart = (int)get16(d);
            if (d->restart < 0)
                return JD_EFORMAT;
            d->info.restart = d->restart;
            break;
        case 0xDA:
            if (!until_sos)
                return JD_EFORMAT;
            return JD_OK;
        case 0xD9:
            return until_sos == 2 ? JD_EOI : JD_EFORMAT;
        case 0xD8:
            return JD_EFORMAT;
        case 0x00:
            break;              /* a stuffed FF of data passed over (a scan left unread) */
        default:
            if (m >= 0xE0 && m <= 0xEF) {
                r = read_app(d, m);
            } else if (m >= 0xD0 && m <= 0xD7) {
                ;               /* a stray restart marker: no length */
            } else {
                if ((len = get16(d)) < 2 || skip(d, len - 2))
                    return JD_EFORMAT;
            }
        }
        if (r)
            return r;
    }
}

static int read_sos(JDec *d)
{
    int32_t len = get16(d);
    int n = getbyte(d), i, j, k;
    if (len < 6 || n < 1 || n > 4 || len != 6 + 2 * n)
        return JD_EFORMAT;
    if (n != d->ncomp)
        return JD_EUNSUPPORTED;         /* sequential in several scans */
    d->ns = n;
    for (i = 0; i < n; i++) {
        int id = getbyte(d), t = getbyte(d);
        if (id < 0 || t < 0)
            return JD_EFORMAT;
        for (j = 0; j < d->ncomp && d->c[j].id != id; j++)
            ;
        if (j == d->ncomp)
            return JD_EFORMAT;
        for (k = 0; k < i; k++)
            if (d->scomp[k] == j)
                return JD_EFORMAT;      /* a component named twice */
        d->scomp[i] = j;
        d->c[j].td = t >> 4;
        d->c[j].ta = t & 15;
        if (d->c[j].td > 3 || d->c[j].ta > 3 || !d->hd[d->c[j].td].ok || !d->ha[d->c[j].ta].ok
            || !d->qtok[d->c[j].tq])
            return JD_EFORMAT;
    }
    if (skip(d, 3))                     /* Ss, Se, Ah/Al: fixed in baseline */
        return JD_EFORMAT;
    if (n == 1) {                       /* one component: MCUs of one block */
        d->c[d->scomp[0]].h = d->c[d->scomp[0]].v = 1;
        d->hmax = d->vmax = 1;
    }
    return JD_OK;
}

/* ------------------------------------------------------------------ entropy */

static void fill(JDec *d)
{
    uint32_t bits = d->bits;
    int nbits = d->nbits;
    if (!d->marker) {                   /* the usual bytes: straight from the buffer */
        const uint8_t *p = d->in + d->inpos, *e = d->in + d->inlen;
        while (nbits <= 24 && p < e && *p != 0xFF) {
            bits = (bits << 8) | *p++;
            nbits += 8;
        }
        d->inpos = (int32_t)(p - d->in);
    }
    while (nbits <= 24) {
        int c = 0;
        if (d->marker) {
            d->pad++;
        } else {
            c = getbyte(d);
            if (c < 0) {
                d->marker = 0xD9;       /* the file ended: zeros from here */
                d->pad++;
                c = 0;
            } else if (c == 0xFF) {
                int c2;
                do
                    c2 = getbyte(d);
                while (c2 == 0xFF);
                if (c2 == 0) {
                    c = 0xFF;
                } else {
                    d->marker = c2 < 0 ? 0xD9 : c2;
                    d->pad++;
                    c = 0;
                }
            }
        }
        bits = (bits << 8) | (uint32_t)c;
        nbits += 8;
    }
    d->bits = bits;
    d->nbits = nbits;
}

static int getbits(JDec *d, int n)      /* n 1-16 */
{
    if (d->nbits < n)
        fill(d);
    d->nbits -= n;
    return (int)((d->bits >> d->nbits) & ((1UL << n) - 1));
}

static int extend(int v, int s)
{
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static int decode_huff(JDec *d, const JHuff *h)
{
    uint32_t e;
    int len;
    if (d->nbits < 16)
        fill(d);
    e = h->look[(d->bits >> (d->nbits - 9)) & 511];
    if (e >> 8) {
        d->nbits -= (int)(e >> 8);
        return (int)(e & 0xFF);
    }
    for (len = 10; len <= 16; len++) {
        int32_t code = (int32_t)((d->bits >> (d->nbits - len)) & ((1UL << len) - 1));
        if (code <= h->maxcode[len]) {
            int32_t i = h->valoff[len] + code;
            d->nbits -= len;
            return i >= 0 && i < 256 ? h->val[i] : 0;
        }
    }
    d->nbits -= 16;                     /* no such code: damaged */
    d->damaged = 1;
    return 0;
}

static int16_t sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

/* One block's coefficients, dequantised, into d->blk (natural order),
 * which is all zeros on entry; where they went into d->nz.
 * Returns how many (1: the DC alone, for the IDCTs' fast path).
 * The bit buffer lives in registers here; d's copy is brought up to
 * date around every call that uses it. */
#define BITS_OUT() (d->bits = bits, d->nbits = nbits)
#define BITS_IN() (bits = d->bits, nbits = d->nbits)
#define NEED(n) do { if (nbits < (n)) { BITS_OUT(); fill(d); BITS_IN(); } } while (0)

static int decode_block(JDec *d, JComp *c)
{
    int16_t *blk = d->blk;
    uint8_t *nz = d->nz + 1;
    const int16_t *q = c->q, *sc = c->sc;
    const int32_t *fast = d->ha[c->ta].fast;
    uint32_t bits;
    int nbits, s, k;
    int32_t f;
    s = decode_huff(d, &d->hd[c->td]);
    if (s) {
        if (s > 15)
            s = 15;
        c->dc += extend(getbits(d, s), s);
    }
    blk[0] = sat16((int32_t)sat16(c->dc) * q[0]);
    blk[0] = sat16(((int32_t)blk[0] * sc[0] + (1L << (13 - PS))) >> (14 - PS));
    d->nz[0] = 0;
    BITS_IN();
    if (c->bs == 1) {                   /* the DC alone: the rest only passed over */
        for (k = 1; k < 64; k++) {
            NEED(FB);
            f = fast[(bits >> (nbits - FB)) & FMASK];
            if (f) {
                nbits -= f & 255;
                k += (f >> 8) & 255;
            } else {
                BITS_OUT();
                s = decode_huff(d, &d->ha[c->ta]);
                if (s & 15) {
                    if (d->nbits < (s & 15))
                        fill(d);
                    d->nbits -= s & 15;
                } else if (s != 0xF0) {
                    BITS_IN();
                    break;              /* end of block */
                }
                BITS_IN();
                k += s >> 4;
            }
        }
        BITS_OUT();
        return 1;
    }
    for (k = 1; k < 64; k++) {
        int z, v;
        NEED(FB);
        f = fast[(bits >> (nbits - FB)) & FMASK];
        if (f) {                        /* run, size and value in the FB bits */
            nbits -= f & 255;
            k += (f >> 8) & 255;
            if (k > 63)
                break;
            v = f >> 16;
        } else {
            int rs, r;
            BITS_OUT();
            rs = decode_huff(d, &d->ha[c->ta]);
            r = rs >> 4;
            s = rs & 15;
            if (!s) {
                BITS_IN();
                if (r != 15)
                    break;              /* end of block */
                k += 15;
                continue;
            }
            k += r;
            if (k > 63) {
                BITS_IN();
                break;
            }
            v = extend(getbits(d, s), s);
            BITS_IN();
        }
        z = zigzag[k];
        if (q[z]) {                     /* 0: outside what this scale reads */
            /* two muls.w: the quantiser, then the AAN scale (or 1). Valid
             * data stays in 16 bits; damaged data wraps, harmlessly */
            int16_t t = (int16_t)(v * q[z]);
            blk[z] = (int16_t)(((int32_t)t * sc[z] + (1L << (13 - PS))) >> (14 - PS));
            *nz++ = (uint8_t)z;
        }
    }
    BITS_OUT();
    return (int)(nz - d->nz);
}

/* After the IDCT: the block back to zeros, only where it was written. */
static void clear_block(JDec *d, int n)
{
    while (n-- > 0)
        d->blk[d->nz[n]] = 0;
}

/* ------------------------------------------------------------------ IDCT */

/* 8 x 8, AAN, from AAN-scaled coefficients */
static void idct8(JDec *d, uint8_t *out, int32_t stride, int ac)
{
    const int16_t *in = d->blk;
    int32_t *ws = d->ws;
    const uint8_t *cl = d->clamp;
    int i;
    if (!ac) {
        uint8_t v = cl[(((in[0] + (1 << (PS + 2))) >> (PS + 3)) + 128) & 1023];
        for (i = 0; i < 8; i++, out += stride)
            memset(out, v, 8);
        return;
    }
    for (i = 0; i < 8; i++) {           /* columns */
        const int16_t *p = in + i;
        int32_t *w = ws + i;
        int32_t t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13, z5, z10, z11, z12, z13;
        if (!(p[8] | p[16] | p[24] | p[32] | p[40] | p[48] | p[56])) {
            w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = p[0];
            continue;
        }
        t0 = p[0]; t1 = p[16]; t2 = p[32]; t3 = p[48];
        t10 = t0 + t2;
        t11 = t0 - t2;
        t13 = t1 + t3;
        t12 = (t1 - t3) + MULF(t1 - t3, F0414) - t13;
        t0 = t10 + t13;
        t3 = t10 - t13;
        t1 = t11 + t12;
        t2 = t11 - t12;
        t4 = p[8]; t5 = p[24]; t6 = p[40]; t7 = p[56];
        z13 = t6 + t5;
        z10 = t6 - t5;
        z11 = t4 + t7;
        z12 = t4 - t7;
        t7 = z11 + z13;
        t11 = (z11 - z13) + MULF(z11 - z13, F0414);
        z5 = 2 * (z10 + z12) - MULF(z10 + z12, F0152);
        t10 = z12 + MULF(z12, F0082) - z5;
        t12 = z5 - (3 * z10 - MULF(z10, F0387));
        t6 = t12 - t7;
        t5 = t11 - t6;
        t4 = t10 + t5;
        w[0] = t0 + t7;  w[56] = t0 - t7;
        w[8] = t1 + t6;  w[48] = t1 - t6;
        w[16] = t2 + t5; w[40] = t2 - t5;
        w[32] = t3 + t4; w[24] = t3 - t4;
    }
    for (i = 0; i < 8; i++, out += stride) {   /* rows */
        const int32_t *w = ws + i * 8;
        int32_t t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13, z5, z10, z11, z12, z13;
        const int32_t rnd = (1 << (PS + 2)) + (128 << (PS + 3));
        if (!(w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7])) {
            memset(out, cl[(w[0] + rnd) >> (PS + 3) & 1023], 8);
            continue;
        }
        t10 = w[0] + w[4];
        t11 = w[0] - w[4];
        t13 = w[2] + w[6];
        t12 = (w[2] - w[6]) + MULF(w[2] - w[6], F0414) - t13;
        t0 = t10 + t13;
        t3 = t10 - t13;
        t1 = t11 + t12;
        t2 = t11 - t12;
        z13 = w[5] + w[3];
        z10 = w[5] - w[3];
        z11 = w[1] + w[7];
        z12 = w[1] - w[7];
        t7 = z11 + z13;
        t11 = (z11 - z13) + MULF(z11 - z13, F0414);
        z5 = 2 * (z10 + z12) - MULF(z10 + z12, F0152);
        t10 = z12 + MULF(z12, F0082) - z5;
        t12 = z5 - (3 * z10 - MULF(z10, F0387));
        t6 = t12 - t7;
        t5 = t11 - t6;
        t4 = t10 + t5;
        out[0] = cl[(t0 + t7 + rnd) >> (PS + 3) & 1023];
        out[7] = cl[(t0 - t7 + rnd) >> (PS + 3) & 1023];
        out[1] = cl[(t1 + t6 + rnd) >> (PS + 3) & 1023];
        out[6] = cl[(t1 - t6 + rnd) >> (PS + 3) & 1023];
        out[2] = cl[(t2 + t5 + rnd) >> (PS + 3) & 1023];
        out[5] = cl[(t2 - t5 + rnd) >> (PS + 3) & 1023];
        out[4] = cl[(t3 + t4 + rnd) >> (PS + 3) & 1023];
        out[3] = cl[(t3 - t4 + rnd) >> (PS + 3) & 1023];
    }
}

/* 4 x 4: each pixel the average of a 2 x 2 of the 8 x 8 IDCT's (plain
 * dequantised coefficients, kept times 4 through both passes) */
static void idct4(JDec *d, uint8_t *out, int32_t stride, int ac)
{
    const int16_t *in = d->blk;
    int32_t *ws = d->ws;                /* 8 rows x 8 */
    const uint8_t *cl = d->clamp;
    int i;
    if (!ac) {
        uint8_t v = cl[(((in[0] + 4) >> 3) + 128) & 1023];
        for (i = 0; i < 4; i++, out += stride)
            out[0] = out[1] = out[2] = out[3] = v;
        return;
    }
    for (i = 0; i < 8; i++) {           /* columns: 8 -> 4 */
        const int16_t *p = in + i;
        if (!(p[8] | p[16] | p[24] | p[40] | p[48] | p[56])) {
            ws[i] = ws[8 + i] = ws[16 + i] = ws[24 + i] = MULF(p[0] * 8, E0);
            continue;
        }
        AVG4(p[0] * 8, p[8] * 8, p[16] * 8, p[24] * 8, p[40] * 8, p[48] * 8, p[56] * 8,
             ws[i], ws[8 + i], ws[16 + i], ws[24 + i]);
    }
    for (i = 0; i < 4; i++, out += stride) {   /* rows: 8 -> 4 */
        const int32_t *w = ws + i * 8;
        const int32_t rnd = 4 + (128 << 3);
        int32_t y0, y1, y2, y3;
        AVG4(w[0], w[1], w[2], w[3], w[5], w[6], w[7], y0, y1, y2, y3);
        out[0] = cl[((y0 + rnd) >> 3) & 1023];
        out[1] = cl[((y1 + rnd) >> 3) & 1023];
        out[2] = cl[((y2 + rnd) >> 3) & 1023];
        out[3] = cl[((y3 + rnd) >> 3) & 1023];
    }
}

/* 2 x 2: each pixel the average of a 4 x 4 of the 8 x 8 IDCT's */
static void idct2(JDec *d, uint8_t *out, int32_t stride, int ac)
{
    static const uint8_t col[5] = { 0, 1, 3, 5, 7 };
    const int16_t *in = d->blk;
    const uint8_t *cl = d->clamp;
    int32_t top[8], bot[8], e, sm;
    const int32_t rnd = 4 + (128 << 3);
    int i;
    if (!ac) {
        out[0] = out[1] = out[stride] = out[stride + 1] = cl[(((in[0] + 4) >> 3) + 128) & 1023];
        return;
    }
    for (i = 0; i < 5; i++) {           /* columns 0, 1, 3, 5, 7: 8 -> 2 */
        const int16_t *p = in + col[i];
        e = MULF(p[0] * 8, E0);
        sm = AVG2S(p[8] * 8, p[24] * 8, p[40] * 8, p[56] * 8);
        top[col[i]] = e + sm;
        bot[col[i]] = e - sm;
    }
    e = MULF(top[0], E0);
    sm = AVG2S(top[1], top[3], top[5], top[7]);
    out[0] = cl[((e + sm + rnd) >> 3) & 1023];
    out[1] = cl[((e - sm + rnd) >> 3) & 1023];
    e = MULF(bot[0], E0);
    sm = AVG2S(bot[1], bot[3], bot[5], bot[7]);
    out[stride] = cl[((e + sm + rnd) >> 3) & 1023];
    out[stride + 1] = cl[((e - sm + rnd) >> 3) & 1023];
}

static void idct1(JDec *d, uint8_t *out, int32_t stride, int ac)
{
    (void)stride;
    (void)ac;
    out[0] = d->clamp[(((d->blk[0] + 4) >> 3) + 128) & 1023];
}

/* ------------------------------------------------------------------ setup */

static void tables(JDec *d)
{
    int i;
    for (i = 0; i < 1024; i++)
        d->clamp[i] = (uint8_t)(i < 256 ? i : i < 640 ? 255 : 0);
    for (i = 0; i < 768; i++)
        d->cclamp[i] = (uint8_t)(i < 256 ? 0 : i < 512 ? i - 256 : 255);
    for (i = 0; i < 256; i++) {
        int32_t x = i - 128;
        d->cr[i].rb = (int16_t)((91881L * x + 32768) >> 16);
        d->cb[i].rb = (int16_t)((116130L * x + 32768) >> 16);
        d->cr[i].g = -46802L * x;
        d->cb[i].g = -22553L * x + 32768;
    }
}

static int setup(JDec *d, int scale)
{
    int i;
    int32_t bs = 8 >> scale;
    d->scale = scale;
    d->bsize = (int)bs;
    d->mcux = (d->info.width + 8 * d->hmax - 1) / (8 * d->hmax);
    d->mcuy = (d->info.height + 8 * d->vmax - 1) / (8 * d->vmax);
    jd_out_size(&d->info, scale, &d->outw, &d->outh);
    d->outwpad = d->mcux * d->hmax * bs;
    d->bandrows = (int)(d->vmax * bs);
    for (i = 0; i < 4; i++) {           /* dequant tables */
        int k;
        if (!d->qtok[i])
            continue;
        for (k = 0; k < 64; k++)
            d->qd[i][k] = (int16_t)(d->qt[i][k] > 32767 ? 32767 : d->qt[i][k]);
    }
    for (i = 0; i < d->ns; i++) {
        JComp *c = &d->c[d->scomp[i]];
        int rx = d->hmax / c->h, ry = d->vmax / c->v, r = rx > ry ? rx : ry, k;
        /* blocks decoded at the size the larger ratio needs (at most 8),
         * then the other direction averaged down, or both made bigger */
        c->bs = (int)(bs * r > 8 ? 8 : bs * r);
        c->upx = bs * rx >= c->bs ? (int)(bs * rx / c->bs) : 1;
        c->dnx = bs * rx >= c->bs ? 1 : (int)(c->bs / (bs * rx));
        c->upy = bs * ry >= c->bs ? (int)(bs * ry / c->bs) : 1;
        c->dny = bs * ry >= c->bs ? 1 : (int)(c->bs / (bs * ry));
        if (d->progressive && (bs == 1 || d->prog_low) && c->bs > bs) {
            /* a progressive picture at 1/8: chroma's DC too, made bigger,
             * so no component keeps more than its DC (and the scans of
             * the rest are passed over); at the other scales the same
             * where the picture would not fit in JD_PROG_MAX otherwise */
            c->bs = (int)bs;
            c->upx = rx;
            c->upy = ry;
            c->dnx = c->dny = 1;
        }
        c->rowlen = d->mcux * c->h * c->bs;
        c->rows = c->v * c->bs;
        c->vw = (d->outw * c->dnx + c->upx - 1) / c->upx;
        if (c->vw > c->rowlen)
            c->vw = c->rowlen;
        c->vrows = (d->outh * c->dny + c->upy - 1) / c->upy;
        c->dc = 0;
        /* the table this scale reads: AAN-scaled for 8 x 8, plain for less,
         * with zeros outside the top-left bs x bs (those are skipped) */
        /* the coefficients this block size reads (0 in q: skipped): all for
         * 8 x 8; for the averaging 4 x 4 all but row and column 4; for
         * 2 x 2 rows and columns 0, 1, 3, 5, 7; for 1 x 1 the DC */
        c->sc = c->bs == 8 ? aan : flat;
        for (k = 0; k < 64; k++) {
            int u = k >> 3, v = k & 7, use;
            switch (c->bs) {
            case 8: use = 1; break;
            case 4: use = u != 4 && v != 4; break;
            case 2: use = (u == 0 || (u & 1)) && (v == 0 || (v & 1)); break;
            default: use = k == 0; break;
            }
            c->q[k] = use ? d->qd[c->tq][k] : 0;
        }
        c->buf[0] = d->io.alloc(d->io.ctx, (uint32_t)c->rowlen * c->rows);
        c->buf[1] = d->io.alloc(d->io.ctx, (uint32_t)c->rowlen * c->rows);
        c->prev = d->io.alloc(d->io.ctx, (uint32_t)c->rowlen);
        c->up = d->io.alloc(d->io.ctx, (uint32_t)(d->outwpad > c->rowlen ? d->outwpad : c->rowlen) + 8);
        c->tmp = d->io.alloc(d->io.ctx, (uint32_t)c->rowlen + 8);
        if (!c->buf[0] || !c->buf[1] || !c->prev || !c->up || !c->tmp)
            return JD_ENOMEM;
    }
    d->rgb = d->io.alloc(d->io.ctx, (uint32_t)d->outw * 3 * d->bandrows);
    return d->rgb ? JD_OK : JD_ENOMEM;
}

/* ------------------------------------------------------------------ bands */

/* Zero bits fed in after the data ended were read as data: damaged. */
static void check_pad(JDec *d)
{
    if (d->pad * 8 > d->nbits)
        d->damaged = 1;
    d->pad = 0;
}

static void restart_marker(JDec *d)
{
    int i;
    check_pad(d);
    d->nbits = 0;
    if (d->marker) {
        if (d->marker >= 0xD0 && d->marker <= 0xD7)
            d->marker = 0;
    } else {
        int m = next_marker(d);
        if (m < 0 || m < 0xD0 || m > 0xD7) {
            d->marker = m < 0 ? 0xD9 : m;
            d->damaged = 1;
        }
    }
    for (i = 0; i < d->ncomp; i++)
        d->c[i].dc = 0;
}

static void decode_band(JDec *d, int which, int32_t *mcu)
{
    int32_t mx;
    uint8_t *mp[4];                     /* each scan component's next MCU in the band */
    int i;
    for (i = 0; i < d->ns; i++)
        mp[i] = d->c[d->scomp[i]].buf[which];
    for (mx = 0; mx < d->mcux; mx++) {
        if (d->restart && *mcu && *mcu % d->restart == 0)
            restart_marker(d);
        for (i = 0; i < d->ns; i++) {
            JComp *c = &d->c[d->scomp[i]];
            int bx, by, bs = c->bs;
            int32_t rl = c->rowlen;
            uint8_t *row = mp[i];
            for (by = 0; by < c->v; by++, row += bs * rl) {
                uint8_t *o = row;
                for (bx = 0; bx < c->h; bx++, o += bs) {
                    int n = decode_block(d, c), ac = n > 1;
                    switch (bs) {
                    case 8: idct8(d, o, rl, ac); break;
                    case 4: idct4(d, o, rl, ac); break;
                    case 2: idct2(d, o, rl, ac); break;
                    default: idct1(d, o, rl, ac); break;
                    }
                    clear_block(d, n);
                }
            }
            mp[i] += c->h * bs;
        }
        (*mcu)++;
    }
}

/* A component's sample row k (over the whole picture), from the band
 * before (its last row), this band (b) or the next. */
static const uint8_t *comp_row(JComp *c, int32_t k, int32_t b, int cur, int nxt)
{
    int32_t base = b * c->rows;
    if (k >= c->vrows)
        k = c->vrows - 1;
    if (k < 0)
        k = 0;
    if (k < base)
        return c->prev;
    if (k >= base + c->rows && nxt >= 0)
        return c->buf[nxt] + (k - base - c->rows) * c->rowlen;
    if (k >= base + c->rows)
        k = base + c->rows - 1;
    return c->buf[cur] + (k - base) * c->rowlen;
}

/* Output row r of band b, for one component, at output width. */
static const uint8_t *comp_out(JComp *c, int r, int32_t b, int cur, int nxt)
{
    int32_t k = b * c->rows + r / c->upy, i, n = c->vw;
    uint8_t *o = c->up;
    if (c->dnx > 1 || c->dny > 1) {     /* averaged down (then perhaps repeated) */
        const uint8_t *src[4];
        int j, sh = (c->dnx == 4) + (c->dnx >= 2) + (c->dny == 4) + (c->dny >= 2), x;
        int32_t m = c->rowlen / c->dnx, kk = b * c->rows + (int32_t)r * c->dny;
        uint8_t *t = c->upx > 1 ? c->tmp : o;
        for (j = 0; j < c->dny; j++)
            src[j] = c->buf[cur] + (kk - b * c->rows + j) * c->rowlen;
        for (i = 0; i < m; i++) {
            int32_t sum = 0;
            for (j = 0; j < c->dny; j++)
                for (x = 0; x < c->dnx; x++)
                    sum += src[j][i * c->dnx + x];
            t[i] = (uint8_t)((sum + (1 << sh >> 1)) >> sh);
        }
        if (c->upx > 1)
            for (i = 0; i < m * c->upx; i++)
                o[i] = t[i / c->upx];
        return o;
    }
    if (c->upx == 1 && c->upy == 1)
        return comp_row(c, k, b, cur, nxt);
    if (c->upx == 2 && c->upy <= 2) {   /* libjpeg's triangle filters */
        const uint8_t *near = comp_row(c, k, b, cur, nxt);
        if (c->upy == 1) {
            if (n == 1) {
                o[0] = o[1] = near[0];
                return o;
            }
            {
                const uint8_t *a = near + 1, *e = near + n - 1;
                uint8_t *q = o + 2;
                int last = near[0], this = near[1], t;
                o[0] = (uint8_t)last;
                o[1] = (uint8_t)((last * 3 + this + 2) >> 2);
                while (a < e) {
                    int next = a[1];
                    t = this * 3;
                    *q++ = (uint8_t)((t + last + 1) >> 2);
                    *q++ = (uint8_t)((t + next + 2) >> 2);
                    last = this;
                    this = next;
                    a++;
                }
                *q++ = (uint8_t)((this * 3 + last + 1) >> 2);
                *q = (uint8_t)this;
            }
        } else {
            const uint8_t *far = comp_row(c, (r & 1) ? k + 1 : k - 1, b, cur, nxt);
            int32_t last, this, next;
            if (n == 1) {
                this = near[0] * 3 + far[0];
                o[0] = (uint8_t)((this * 4 + 8) >> 4);
                o[1] = (uint8_t)((this * 4 + 7) >> 4);
                return o;
            }
            this = near[0] * 3 + far[0];
            next = near[1] * 3 + far[1];
            o[0] = (uint8_t)((this * 4 + 8) >> 4);
            o[1] = (uint8_t)((this * 3 + next + 7) >> 4);
            last = this;
            this = next;
            {
                const uint8_t *a = near + 2, *f = far + 2;
                uint8_t *q = o + 2, *e = o + 2 * (n - 1);
                while (q < e) {
                    int32_t t = this * 3;
                    next = *a++ * 3 + *f++;
                    *q++ = (uint8_t)((t + last + 8) >> 4);
                    *q++ = (uint8_t)((t + next + 7) >> 4);
                    last = this;
                    this = next;
                }
                q[0] = (uint8_t)((this * 3 + last + 8) >> 4);
                q[1] = (uint8_t)((this * 4 + 7) >> 4);
            }
        }
        return o;
    }
    {                                   /* other factors: repeated */
        const uint8_t *s = comp_row(c, k, b, cur, nxt);
        int32_t w = c->rowlen * c->upx;
        for (i = 0; i < w; i++)
            o[i] = s[i / c->upx];
        return o;
    }
}

/* One row of YCbCr to RGB, n > 0 pixels; cl is cclamp + 256 */
#if defined(__GNUC__) && !defined(JD_NO_ASM) && \
    (defined(__mc68020__) || defined(__mc68030__) || defined(__mc68040__) || defined(__mc68060__))
/* 16 instructions a pixel: the three samples' registers keep their upper
 * bits zero for word indexes, and the 68020's index * 8 reads the tables */
void jd_ycc_row(uint8_t *o, const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                int32_t n, const uint8_t *cl, const JCol *crt, const JCol *cbt);
#define JD_S2(x) #x
#define JD_S(x) JD_S2(x)
#define JD_SYM JD_S(__USER_LABEL_PREFIX__) "jd_ycc_row"
__asm__(
    "\t.text\n\t.even\n\t.globl " JD_SYM "\n" JD_SYM ":\n"
    "\tmovem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "\tmovem.l 48(%sp),%a0-%a3\n"            /* o, y, cb, cr (mask order) */
    "\tmove.l 64(%sp),%d7\n"                  /* n */
    "\tmovem.l 68(%sp),%a4-%a6\n"            /* cl, crt, cbt */
    "\tmoveq #0,%d0\n\tmoveq #0,%d1\n\tmoveq #0,%d2\n"
    "1:\tmove.b (%a1)+,%d0\n"                 /* Y */
    "\tmove.b (%a3)+,%d1\n"                   /* Cr */
    "\tmove.b (%a2)+,%d2\n"                   /* Cb */
    "\tmove.w (%a5,%d1.w*8),%d3\n"
    "\tadd.w %d0,%d3\n"
    "\tmove.b (%a4,%d3.w),(%a0)+\n"           /* R */
    "\tmove.l 4(%a5,%d1.w*8),%d4\n"
    "\tadd.l 4(%a6,%d2.w*8),%d4\n"
    "\tswap %d4\n"
    "\tadd.w %d0,%d4\n"
    "\tmove.b (%a4,%d4.w),(%a0)+\n"           /* G */
    "\tmove.w (%a6,%d2.w*8),%d3\n"
    "\tadd.w %d0,%d3\n"
    "\tmove.b (%a4,%d3.w),(%a0)+\n"           /* B */
    "\tsubq.l #1,%d7\n"
    "\tbne.s 1b\n"
    "\tmovem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "\trts\n");
#define ycc_row jd_ycc_row
#else
static void ycc_row(uint8_t *o, const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                    int32_t n, const uint8_t *cl, const JCol *crt, const JCol *cbt)
{
    const uint8_t *e = y + n;
    while (y < e) {
        int l = *y++;
        const JCol *r = crt + *cr++, *b = cbt + *cb++;
        *o++ = cl[l + r->rb];
        *o++ = cl[l + (int)((r->g + b->g) >> 16)];
        *o++ = cl[l + b->rb];
    }
}
#endif

static int emit_band(JDec *d, int32_t b, int cur, int nxt)
{
    int32_t y0 = b * d->bandrows, n = d->outh - y0, x;
    int r, i;
    if (n > d->bandrows)
        n = d->bandrows;
    for (r = 0; r < n; r++) {
        uint8_t *o = d->rgb + (int32_t)r * d->outw * 3;
        if (d->ncomp == 1) {
            const uint8_t *g = comp_out(&d->c[0], r, b, cur, nxt);
            for (x = 0; x < d->outw; x++, o += 3)
                o[0] = o[1] = o[2] = g[x];
        } else {
            const uint8_t *p0 = comp_out(&d->c[0], r, b, cur, nxt);
            const uint8_t *p1, *p2;
            /* comp_out returns a row of the component's own: one each */
            p1 = comp_out(&d->c[1], r, b, cur, nxt);
            p2 = comp_out(&d->c[2], r, b, cur, nxt);
            if (d->info.rgb) {
                for (x = 0; x < d->outw; x++, o += 3) {
                    o[0] = p0[x];
                    o[1] = p1[x];
                    o[2] = p2[x];
                }
            } else {
                ycc_row(o, p0, p1, p2, d->outw, d->cclamp + 256, d->cr, d->cb);
            }
        }
    }
    if (n > 0 && d->io.rows(d->io.ctx, d->rgb, y0, n, d->outw * 3))
        return JD_EABORT;
    for (i = 0; i < d->ns; i++) {       /* this band's last row: the next one's "before" */
        JComp *c = &d->c[d->scomp[i]];
        memcpy(c->prev, c->buf[cur] + (int32_t)(c->rows - 1) * c->rowlen, (size_t)c->rowlen);
    }
    return JD_OK;
}


/* ------------------------------------------------------------------ progressive */

/* A progressive scan's header: its components, Ss, Se, Ah, Al (T.81 G.1). */
static int read_sos_prog(JDec *d)
{
    int32_t len = get16(d);
    int n = getbyte(d), i, j, k, a;
    if (len < 6 || n < 1 || n > d->ncomp || len != 6 + 2 * n)
        return JD_EFORMAT;
    d->ns = n;
    for (i = 0; i < n; i++) {
        int id = getbyte(d), t = getbyte(d);
        if (id < 0 || t < 0)
            return JD_EFORMAT;
        for (j = 0; j < d->ncomp && d->c[j].id != id; j++)
            ;
        if (j == d->ncomp)
            return JD_EFORMAT;
        for (k = 0; k < i; k++)
            if (d->scomp[k] == j)
                return JD_EFORMAT;
        d->scomp[i] = j;
        d->c[j].td = t >> 4;
        d->c[j].ta = t & 15;
        if (d->c[j].td > 3 || d->c[j].ta > 3)
            return JD_EFORMAT;
    }
    d->ss = getbyte(d);
    d->se = getbyte(d);
    a = getbyte(d);
    if (d->ss < 0 || d->se < 0 || a < 0)
        return JD_EFORMAT;
    d->ah = a >> 4;
    d->al = a & 15;
    if (d->ss > d->se || d->se > 63 || d->al > 13 || d->ah > 13 ||
        (d->ss == 0 && d->se != 0) || (d->ss > 0 && n != 1))
        return JD_EFORMAT;
    for (i = 0; i < n; i++) {           /* the tables this scan reads */
        JComp *c = &d->c[d->scomp[i]];
        if ((d->ss == 0 && d->ah == 0 && !d->hd[c->td].ok) || (d->ss > 0 && !d->ha[c->ta].ok))
            return JD_EFORMAT;
    }
    return JD_OK;
}

static void prog_dc(JDec *d, JComp *c, int16_t *b)
{
    if (d->ah == 0) {
        int s = decode_huff(d, &d->hd[c->td]);
        if (s) {
            if (s > 15)
                s = 15;
            c->dc += extend(getbits(d, s), s);
        }
        b[0] = sat16((int32_t)sat16(c->dc) * (1L << d->al));
    } else if (getbits(d, 1)) {
        b[0] = (int16_t)(b[0] | (1 << d->al));
    }
}

/* Coefficient k (zigzag) of a block: kept (b[slot]), or only whether it is
 * nonzero (m's bit k) */
#define PC_NZ(k) (c->slot[k] != 255 ? b[c->slot[k]] != 0 : (m[(k) >> 5] >> ((k) & 31)) & 1)
#define PC_SET(k, v) do { if (c->slot[k] != 255) b[c->slot[k]] = (int16_t)(v); \
                          else m[(k) >> 5] |= 1UL << ((k) & 31); } while (0)

/* An AC scan's band of one block, its first pass (Ah 0) */
static void prog_ac_first(JDec *d, JComp *c, int16_t *b, uint32_t *m)
{
    int k = d->ss;
    if (d->eobrun > 0) {
        d->eobrun--;
        return;
    }
    while (k <= d->se) {
        int rs = decode_huff(d, &d->ha[c->ta]), r = rs >> 4, s = rs & 15;
        if (s) {
            int32_t v;
            k += r;
            if (k > d->se) {
                d->damaged = 1;
                return;
            }
            v = (int32_t)extend(getbits(d, s), s) * (1L << d->al);
            PC_SET(k, sat16(v));
            k++;
        } else if (r == 15) {
            k += 16;                    /* sixteen zeros */
        } else {
            d->eobrun = (1L << r) - 1;  /* this block and eobrun more end here */
            if (r)
                d->eobrun += getbits(d, r);
            return;
        }
    }
}

/* A nonzero coefficient's correction bit: added where it is kept */
static void prog_correct(JDec *d, JComp *c, int16_t *b, int k, int p1)
{
    if (getbits(d, 1) && c->slot[k] != 255) {
        int16_t *x = &b[c->slot[k]];
        if (!(*x & p1))
            *x = (int16_t)(*x >= 0 ? *x + p1 : *x - p1);
    }
}

/* Its refinements (Ah > 0): a bit more of each coefficient already there,
 * and new ones of +-1 << Al in the zeros (T.81 G.1.2.3) */
static void prog_ac_refine(JDec *d, JComp *c, int16_t *b, uint32_t *m)
{
    int k = d->ss, p1 = 1 << d->al;
    if (d->eobrun > 0) {
        d->eobrun--;
        for (; k <= d->se; k++)
            if (PC_NZ(k))
                prog_correct(d, c, b, k, p1);
        return;
    }
    while (k <= d->se) {
        int rs = decode_huff(d, &d->ha[c->ta]), r = rs >> 4, s = rs & 15, v = 0;
        if (s) {
            if (s != 1)
                d->damaged = 1;
            v = getbits(d, 1) ? p1 : -p1;
        } else if (r != 15) {
            d->eobrun = (1L << r) - 1;  /* this block's rest, and eobrun blocks more */
            if (r)
                d->eobrun += getbits(d, r);
            r = 64;                     /* (to the band's end: corrections only) */
        }                               /* (ZRL: 16 zeros passed, the 16th given 0) */
        while (k <= d->se) {
            int kk = k++;
            if (PC_NZ(kk)) {
                prog_correct(d, c, b, kk, p1);
            } else {
                if (r == 0) {
                    if (v)
                        PC_SET(kk, v);
                    break;
                }
                r--;
            }
        }
    }
}

static void prog_restart(JDec *d)
{
    restart_marker(d);
    d->eobrun = 0;
}

/* One scan into the components' coefficients. An AC scan of a component
 * kept as its DC alone is passed over unread (read_markers finds the next
 * marker). */
static void prog_scan(JDec *d)
{
    int32_t n = 0;
    int i;
    for (i = 0; i < d->ncomp; i++)
        d->c[i].dc = 0;
    d->eobrun = 0;
    if (d->ns == 1) {                   /* one component: its own blocks, in rows */
        JComp *c = &d->c[d->scomp[0]];
        int32_t bw = ((d->info.width * c->h + d->hmax - 1) / d->hmax + 7) / 8;
        int32_t bh = ((d->info.height * c->v + d->vmax - 1) / d->vmax + 7) / 8, bx, by;
        if (d->ss > 0 && c->kn == 1)
            return;
        if (bw > c->sbw)
            bw = c->sbw;
        if (bh > c->sbh)
            bh = c->sbh;
        for (by = 0; by < bh; by++)
            for (bx = 0; bx < bw; bx++, n++) {
                int32_t bi = by * c->sbw + bx;
                int16_t *b = c->coef + bi * c->kn;
                uint32_t *m = c->nzm ? c->nzm + bi * 2 : NULL;     /* (NULL: all kept) */
                if (d->restart && n && n % d->restart == 0)
                    prog_restart(d);
                if (d->ss == 0)
                    prog_dc(d, c, b);
                else if (d->ah == 0)
                    prog_ac_first(d, c, b, m);
                else
                    prog_ac_refine(d, c, b, m);
            }
    } else {                            /* DC, interleaved: in MCUs */
        int32_t mx, my;
        for (my = 0; my < d->mcuy; my++)
            for (mx = 0; mx < d->mcux; mx++, n++) {
                if (d->restart && n && n % d->restart == 0)
                    prog_restart(d);
                for (i = 0; i < d->ns; i++) {
                    JComp *c = &d->c[d->scomp[i]];
                    int bx, by;
                    for (by = 0; by < c->v; by++)
                        for (bx = 0; bx < c->h; bx++)
                            prog_dc(d, c, c->coef + ((my * c->v + by) * c->sbw + mx * c->h + bx) * c->kn);
                }
            }
    }
}

/* After a scan: the bit reader emptied, and a marker its data ran into
 * kept for read_markers */
static void prog_scan_end(JDec *d)
{
    check_pad(d);
    if (d->marker)
        d->pending = d->marker;
    d->marker = 0;
    d->bits = 0;
    d->nbits = 0;
}

/* A kept block into d->blk as decode_block leaves it: how many */
static int prog_block(JDec *d, const JComp *c, const int16_t *b)
{
    int16_t *blk = d->blk;
    uint8_t *nz = d->nz + 1;
    const int16_t *q = c->q, *sc = c->sc;
    int k;
    blk[0] = sat16((int32_t)b[0] * q[0]);
    blk[0] = sat16(((int32_t)blk[0] * sc[0] + (1L << (13 - PS))) >> (14 - PS));
    d->nz[0] = 0;
    if (c->kn == 1)
        return 1;
    for (k = 1; k < 64; k++) {
        int z, v;
        if (c->slot[k] == 255 || !(v = b[c->slot[k]]))
            continue;
        z = zigzag[k];
        if (q[z]) {
            int16_t t = (int16_t)(v * q[z]);
            blk[z] = (int16_t)(((int32_t)t * sc[z] + (1L << (13 - PS))) >> (14 - PS));
            *nz++ = (uint8_t)z;
        }
    }
    return (int)(nz - d->nz);
}

/* A band's MCU row from the kept coefficients, as decode_band */
static void prog_band(JDec *d, int which, int32_t my)
{
    int32_t mx;
    uint8_t *mp[4];
    int i;
    for (i = 0; i < d->ns; i++)
        mp[i] = d->c[d->scomp[i]].buf[which];
    for (mx = 0; mx < d->mcux; mx++)
        for (i = 0; i < d->ns; i++) {
            JComp *c = &d->c[d->scomp[i]];
            int bx, by, bs = c->bs;
            int32_t rl = c->rowlen;
            uint8_t *row = mp[i];
            for (by = 0; by < c->v; by++, row += bs * rl) {
                uint8_t *o = row;
                const int16_t *b = c->coef + ((my * c->v + by) * c->sbw + mx * c->h) * c->kn;
                for (bx = 0; bx < c->h; bx++, o += bs, b += c->kn) {
                    int n = prog_block(d, c, b), ac = n > 1;
                    switch (bs) {
                    case 8: idct8(d, o, rl, ac); break;
                    case 4: idct4(d, o, rl, ac); break;
                    case 2: idct2(d, o, rl, ac); break;
                    default: idct1(d, o, rl, ac); break;
                    }
                    clear_block(d, n);
                }
            }
            mp[i] += c->h * bs;
        }
}

static int prog_decode(JDec *d, int scale)
{
    int32_t b;
    int i, r, cur = 0;
    if (d->ncomp == 1) {                /* (one component: blocks of their own, as baseline) */
        d->c[0].h = d->c[0].v = 1;
        d->hmax = d->vmax = 1;
    }
    d->ns = d->ncomp;
    for (i = 0; i < d->ncomp; i++)
        d->scomp[i] = i;
    if ((r = read_markers(d, 1)))
        return r;
    for (i = 0; i < d->ncomp; i++)
        if (!d->qtok[d->c[i].tq])
            return JD_EFORMAT;
    /* what it keeps: past JD_PROG_MAX, chroma a step smaller (prog_low) */
    for (d->prog_low = JD_PROG_LOW; d->prog_low < 2; d->prog_low++) {
        int32_t need = 0, bs = 8 >> scale, mcux, mcuy;
        int over = 0;
        mcux = (d->info.width + 8 * d->hmax - 1) / (8 * d->hmax);
        mcuy = (d->info.height + 8 * d->vmax - 1) / (8 * d->vmax);
        for (i = 0; i < d->ncomp && !over; i++) {
            JComp *c = &d->c[i];
            int rx = d->hmax / c->h, ry = d->vmax / c->v, cbs = (int)bs * (rx > ry ? rx : ry);
            int32_t blocks = mcux * c->h * mcuy * c->v, per;
            if (cbs > 8)
                cbs = 8;
            if ((bs == 1 || d->prog_low) && cbs > bs)
                cbs = (int)bs;
            per = cbs == 8 ? 128 : cbs == 4 ? 49 * 2 + 8 : cbs == 2 ? 25 * 2 + 8 : 2;
            if (blocks > (JD_PROG_MAX - need) / per)
                over = 1;
            else
                need += blocks * per;
        }
        if (!over)
            break;
    }
    if (d->prog_low == 2)
        return JD_ENOMEM;               /* (more than is sensible to keep) */
    if ((r = setup(d, scale)))
        return r;
    for (i = 0; i < d->ncomp; i++) {
        JComp *c = &d->c[i];
        int k;
        uint32_t nb;
        c->kn = 0;
        for (k = 0; k < 64; k++)        /* (q 0: a coefficient this scale does not read) */
            c->slot[k] = (uint8_t)(k == 0 || c->q[zigzag[k]] ? c->kn++ : 255);
        if (c->bs == 1) {
            c->kn = 1;
            memset(c->slot + 1, 255, 63);
        }
        c->sbw = d->mcux * c->h;
        c->sbh = d->mcuy * c->v;
        nb = (uint32_t)(c->sbw * c->sbh);
        if (!(c->coef = d->io.alloc(d->io.ctx, nb * 2 * c->kn)))
            return JD_ENOMEM;
        memset(c->coef, 0, (size_t)nb * 2 * c->kn);
        if (c->kn > 1 && c->kn < 64) {
            if (!(c->nzm = d->io.alloc(d->io.ctx, nb * 8)))
                return JD_ENOMEM;
            memset(c->nzm, 0, (size_t)nb * 8);
        }
    }
    for (;;) {                          /* the scans */
        if ((r = read_sos_prog(d))) {
            d->damaged = 1;
            break;
        }
        prog_scan(d);
        prog_scan_end(d);
        r = read_markers(d, 2);
        if (r == JD_EOI)
            break;
        if (r == JD_EREAD)
            return r;
        if (r) {
            d->damaged = 1;             /* (what came is shown) */
            break;
        }
    }
    d->ns = d->ncomp;
    for (i = 0; i < d->ncomp; i++)
        d->scomp[i] = i;
    prog_band(d, cur, 0);
    for (b = 0; b < d->mcuy; b++) {
        int nxt = cur ^ 1, has = b + 1 < d->mcuy;
        if (has)
            prog_band(d, nxt, b + 1);
        if ((r = emit_band(d, b, cur, has ? nxt : -1)))
            return r;
        cur = nxt;
    }
    return d->readerr ? JD_EREAD : JD_OK;
}

/* ------------------------------------------------------------------ API */

int jd_open(JDec **pd, const JDecIO *io, JDecInfo *info)
{
    JDec *d;
    int r;
    *pd = NULL;
    memset(info, 0, sizeof(*info));
    d = io->alloc(io->ctx, sizeof(JDec));
    if (!d)
        return JD_ENOMEM;
    memset(d, 0, sizeof(*d));
    d->io = *io;
    d->info.xdensity = d->info.ydensity = 1;
    if (getbyte(d) != 0xFF || getbyte(d) != 0xD8) {
        r = d->readerr ? JD_EREAD : JD_EFORMAT;
    } else {
        r = read_markers(d, 0);
    }
    *info = d->info;
    if (r) {
        io->free(io->ctx, d);
        return r;
    }
    *pd = d;
    return JD_OK;
}

void jd_out_size(const JDecInfo *info, int scale, int32_t *w, int32_t *h)
{
    int32_t m = (1L << scale) - 1;
    *w = (info->width + m) >> scale;
    *h = (info->height + m) >> scale;
}

int jd_scale_for(const JDecInfo *info, int32_t maxw, int32_t maxh)
{
    int s;
    if (maxw <= 0 && maxh <= 0)
        return 0;
    for (s = 3; s > 0; s--) {
        int32_t w, h;
        jd_out_size(info, s, &w, &h);
        if ((maxw <= 0 || w >= maxw) && (maxh <= 0 || h >= maxh))
            return s;
    }
    return 0;
}

int jd_decode(JDec *d, int scale)
{
    int32_t b, mcu = 0;
    int r, cur = 0;
    if (scale < 0 || scale > 3 || d->rgb)
        return JD_EFORMAT;
    tables(d);
    if (d->progressive)
        return prog_decode(d, scale);
    if ((r = read_markers(d, 1)) || (r = read_sos(d)) || (r = setup(d, scale)))
        return r;
    decode_band(d, cur, &mcu);
    for (b = 0; b < d->mcuy; b++) {
        int nxt = cur ^ 1, has = b + 1 < d->mcuy;
        if (has)
            decode_band(d, nxt, &mcu);
        if ((r = emit_band(d, b, cur, has ? nxt : -1)))
            return r;
        cur = nxt;
    }
    check_pad(d);
    return d->readerr ? JD_EREAD : JD_OK;
}

int jd_damaged(const JDec *d)
{
    return d->damaged;
}

void jd_close(JDec *d)
{
    int i;
    if (!d)
        return;
    for (i = 0; i < 4; i++) {
        JComp *c = &d->c[i];
        if (c->buf[0])
            d->io.free(d->io.ctx, c->buf[0]);
        if (c->buf[1])
            d->io.free(d->io.ctx, c->buf[1]);
        if (c->prev)
            d->io.free(d->io.ctx, c->prev);
        if (c->up)
            d->io.free(d->io.ctx, c->up);
        if (c->tmp)
            d->io.free(d->io.ctx, c->tmp);
        if (c->coef)
            d->io.free(d->io.ctx, c->coef);
        if (c->nzm)
            d->io.free(d->io.ctx, c->nzm);
    }
    if (d->rgb)
        d->io.free(d->io.ctx, d->rgb);
    d->io.free(d->io.ctx, d);
}
