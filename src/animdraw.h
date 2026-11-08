/*
 * AmigaMCE - an ANIM's frames drawn as 15 or 16-bit pixels
 * Copyright (c) 2026 Brian King
 *
 * Plain C with no Amiga headers, so the tests build it on the host too.
 * Rows of planes are interleaved: plane p of a row at row + p * rb.
 */
#ifndef ANIMDRAW_H
#define ANIMDRAW_H

void ad_init(void);                     /* its tables, once */

/* A row of planes as w bytes, a pixel's colour number each. */
void ad_p2c_row(const unsigned char *row, unsigned long rb, unsigned char depth, unsigned long w,
                unsigned char *out);

/* A CMAP as 256 0xRRGGBB colours (old 4-bit ones widened; extra
 * halfbrite's 32-63 the first 32 at half when camg says so). */
void ad_lut(const unsigned char *cmap, unsigned long len, unsigned long camg, unsigned char depth,
            unsigned long *lut);

/* The screen's pixel format (CyberGraphX's PIXFMT_*): 1 if 15 or 16-bit
 * and so drawn here (its tables made), 0 if not. */
int ad_pix_tables(unsigned long fmt);

/* A colour (0xRRGGBB) as a pixel of that format. */
unsigned short ad_rgb16(unsigned long rgb);

/* The colour table as pixels; HAM's steps from them (after ad_lut16). */
void ad_lut16(const unsigned long *lut, unsigned short *l16);
void ad_ham(const unsigned short *l16, unsigned char depth);

/* Where row a differs from row b: its first byte column, one past its
 * last; 0 if nowhere. */
int ad_row_span(const unsigned char *a, const unsigned char *b, unsigned long rb,
                unsigned char depth, unsigned long *x0, unsigned long *x1);

/* Byte columns x0 to x1 (one past) of a row into out, the row's first
 * pixel: source pixel px to pixels xo[px] to xo[px + 1] - 1; fx the
 * whole scale that is the same, or 0. HAM: worked out from the row's
 * start, written from x0 to its end. */
void ad_row_put(const unsigned char *row, unsigned long rb, unsigned char depth, unsigned long w,
                unsigned long x0, unsigned long x1, unsigned short fx, const unsigned short *xo,
                int ham, const unsigned long *lut, const unsigned short *l16, unsigned char *out);

#endif
