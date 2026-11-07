/*
 * AmigaMCE - IFF ANIM frames decoded into bitplanes
 * Copyright (c) 2026 Brian King
 *
 * Plain C with no Amiga headers, so the tests build it on the host too
 * (tests/anim_host.c) and check the frames against the real files.
 */
#ifndef ANIMDEC_H
#define ANIMDEC_H

/* Where a frame goes: one pointer a plane, `stride` bytes from a row to
 * the next (a screen's bitmap can be wider than the picture), `rb` bytes
 * of picture a row (its width rounded up to 16 pixels), `h` rows. */
struct AnimPlanes {
    unsigned char *plane[8];
    long stride;
    int depth, rb, h;
};

/* An ILBM BODY: ByteRun1 (comp 1) or plain (0); masking 1 has a mask
 * plane after each row's planes, skipped. 0: done, -1: cut short (the
 * rows it had are drawn). */
int animdec_body(const unsigned char *body, long len, const struct AnimPlanes *pl,
                 int comp, int masking);

/* A DLTA chunk, by ANHD's operation: 5 (byte vertical delta), 7 (short or
 * long, opcodes and data apart), 8 (short or long, together); `bits` is
 * ANHD's: 1 long data (7, 8), 2 XOR (5). 0: done, -1: cut short or bad
 * (the planes are as far as it got), -2: an operation not known. */
int animdec_delta(const unsigned char *d, long len, const struct AnimPlanes *pl, int op,
                  unsigned long bits);

/* Operations animdec_delta knows. */
int animdec_known(int op);

#endif
