/*
 * AmigaMCE - a DVD's subpictures (its menus' buttons, its subtitles):
 * gathered from their packets, decoded, and blended into a picture (spu.c)
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef AMCE_SPU_H
#define AMCE_SPU_H

#include <exec/types.h>

/* One subpicture, decoded: w x h pixel values (0-3) at x, y in the
 * video's picture (720 x 576 or 480); each value's colour (an index into
 * the disc's 16-colour table) and opacity (0 clear to 15 solid). */
struct SpuPic {
    UBYTE *pix;
    UWORD x, y, w, h;
    UBYTE col[4], alpha[4];
    ULONG start, stop;          /* 90 kHz; stop meaningful only with has_stop */
    BOOL has_stop;
    BOOL forced;                /* shown even with subtitles off (a menu's, a forced line) */
    /* where its values other than 0 are: each row's first and end (span[2
     * * row], [2 * row + 1]; equal: none), and their box (nx0 <= x < nx1,
     * ny0 <= y < ny1; nx1 0: none), from its own top left. Value 0 is
     * most often clear: then only these are blended. */
    UWORD *span;
    UWORD nx0, ny0, nx1, ny1;
};

/* A button's highlight over it (DvdHighlight's, dvdnav.h): inside the
 * rectangle (inclusive) each value's colour and opacity are these. */
struct SpuHl {
    BOOL on;
    UWORD x0, y0, x1, y1;
    UBYTE col[4], alpha[4];
};

struct Spu;

struct Spu *spu_new(void);
void spu_free(struct Spu *s);
/* Everything dropped: a jump, a new stream. */
void spu_reset(struct Spu *s);
/* A packet's payload (private stream 1, after its substream byte); its
 * time if it has one. A whole subpicture is decoded when its last part
 * comes. */
void spu_put(struct Spu *s, const UBYTE *d, ULONG n, BOOL has_pts, ULONG pts);
/* The subpicture to show at pts (NULL: none); the ones before it let go.
 * forced_only: only those meant to show with subtitles off. */
const struct SpuPic *spu_at(struct Spu *s, ULONG pts, BOOL forced_only);
/* How many decoded so far, and of those not decodable (for the log). */
void spu_counts(struct Spu *s, ULONG *decoded, ULONG *bad);

/* p (and hl over it, if on) blended into a 4:2:0 picture's planes (pw x
 * ph, ymod and cmod bytes a row), the colours from clut (16 entries
 * 0x00YYCrCb, as on the disc). */
void spu_blend420(const struct SpuPic *p, const struct SpuHl *hl, const ULONG *clut, UBYTE *y,
                  UBYTE *cb, UBYTE *cr, ULONG ymod, ULONG cmod, ULONG pw, ULONG ph);

/* What blending p (with hl) can change, in the picture's pixels (x0 <= x
 * < x1, y0 <= y < y1): FALSE if nothing. */
BOOL spu_extent(const struct SpuPic *p, const struct SpuHl *hl, ULONG *x0, ULONG *y0, ULONG *x1,
                ULONG *y1);

/* (for the tests: one subpicture unit decoded from its bytes; FALSE if
 * it is not one) */
BOOL spu_decode(const UBYTE *u, ULONG n, ULONG pts, struct SpuPic *out);

#endif
