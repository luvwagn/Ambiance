/*
 * jdec - a JPEG decoder for 68020 Amigas without an FPU
 * Copyright (c) 2026 Brian King, MIT
 *
 * Baseline JPEG (SOF0, SOF1), 8-bit, one interleaved scan (or one
 * greyscale component), Huffman coded; JFIF YCbCr, Adobe RGB, greyscale;
 * any sampling factors 1-4 that divide the largest; restart markers.
 * Decodes at 1/1, 1/2, 1/4 or 1/8 of its size: the smaller scales skip
 * most of the inverse DCT and colour work, and chroma is decoded straight
 * at the size it covers. Output is RGB, a band of rows at a time, so
 * memory stays a few MCU rows whatever the picture's size.
 *
 * Written for the 68020: 16-bit multiplies only (muls.w), no floating
 * point, no 64-bit arithmetic. Every read of the file is bounds-checked;
 * a damaged or cut-off file decodes to grey where its data ends, as
 * libjpeg does, and never reads or writes outside its buffers.
 *
 * Progressive (SOF2), 8-bit, Huffman coded: its scans are read into
 * each component's coefficients first (no rows until the last scan), then
 * decoded as a baseline picture's are. At 1/8 every component keeps its
 * DC alone (2 bytes a block; chroma made bigger after, so a little softer
 * at colour edges than libjpeg's) and the AC scans are passed over
 * unread. At the other scales only the coefficients the scale's IDCT
 * reads are kept, with a bit for each other one that is nonzero (the
 * refinement scans need it): 1/4 58 bytes a block, 1/2 106, 1/1 128.
 * Where that is over JD_PROG_MAX (24 MB), chroma is kept a step smaller
 * and made bigger, as at 1/8; still over: JD_ENOMEM (jpgthumb.c then
 * tries a scale smaller). A cut-off or damaged progressive file shows
 * what its scans gave.
 *
 * Not (yet): CMYK and YCCK (JD_ECMYK), 12-bit, arithmetic coding,
 * multi-scan sequential (JD_EUNSUPPORTED).
 */
#ifndef JDEC_H
#define JDEC_H

#include <stdint.h>

enum {
    JD_OK = 0,
    JD_ENOMEM = -1,          /* alloc() failed */
    JD_EREAD = -2,           /* read() failed */
    JD_EFORMAT = -3,         /* not a JPEG, or its headers are damaged */
    JD_EUNSUPPORTED = -4,    /* a kind of JPEG not decoded (see above) */
    JD_EPROGRESSIVE = -5,    /* (progressive JPEG: no longer returned) */
    JD_ECMYK = -6,           /* four components: CMYK or YCCK */
    JD_EABORT = -7,          /* rows() asked to stop */
    JD_ETOOBIG = -8          /* wider or taller than 32767 */
};

typedef struct JDecIO {
    void *ctx;
    /* up to n bytes into buf: how many (0 at the end, <0 an error) */
    int32_t (*read)(void *ctx, uint8_t *buf, int32_t n);
    void *(*alloc)(void *ctx, uint32_t n);         /* NULL: none */
    void (*free)(void *ctx, void *p);
    /* n rows of RGB (3 bytes a pixel, out_width pixels), the first one
     * row y of the output, each stride bytes after the last.
     * Return 0 to go on, anything else to stop (JD_EABORT). */
    int (*rows)(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride);
} JDecIO;

typedef struct JDecInfo {
    int32_t width, height;   /* the picture's own size */
    int ncomp;               /* 1 greyscale, 3 colour, 4 CMYK/YCCK */
    int progressive;         /* SOF2 */
    int rgb;                 /* stored as RGB (Adobe), not YCbCr */
    uint8_t h[4], v[4];      /* sampling factors (4:2:0 is 2,2 1,1 1,1) */
    int restart;             /* restart interval, in MCUs (0 none) */
    int density_unit;        /* JFIF: 0 aspect only, 1 dpi, 2 dots/cm */
    int xdensity, ydensity;  /* JFIF (or 1, 1) */
} JDecInfo;

typedef struct JDec JDec;

/* Reads the headers up to the frame (SOF): its size and kind into info.
 * On an error *pd is NULL; info still has what was read (for
 * JD_EPROGRESSIVE and JD_ECMYK, all of it), so the caller can say what
 * the picture is. */
int jd_open(JDec **pd, const JDecIO *io, JDecInfo *info);

/* The scale (0: 1/1, 1: 1/2, 2: 1/4, 3: 1/8) that decodes to the
 * smallest size still at least maxw x maxh (0: no limit). */
int jd_scale_for(const JDecInfo *info, int32_t maxw, int32_t maxh);

/* The output size at a scale. */
void jd_out_size(const JDecInfo *info, int scale, int32_t *w, int32_t *h);

/* Decodes the picture at a scale, giving its rows to io->rows.
 * JD_OK also when the data ends early (the rest is grey); then
 * jd_damaged() is nonzero. */
int jd_decode(JDec *d, int scale);
int jd_damaged(const JDec *d);

void jd_close(JDec *d);

#endif
