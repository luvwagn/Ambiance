/*
 * AmigaMCE - a video picture (4:2:0 YUV planes, mpegv.library's) drawn
 * straight into an RTG bitmap's memory at the size shown, in its own pixel
 * format: 15, 16, 24 or 32 bits, either byte order. One pass, nearest
 * pixel, no RGB picture in between and no ScalePixelArray (which took
 * 59 ms a DVD picture under Amiberry's JIT: more than a picture's time).
 * Copyright (c) 2026 Brian King, MIT licence
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include "yuvdraw.h"

/* BT.601, studio range, in 8.8 fixed point: R = Y' + 1.596 Cr,
 * G = Y' - 0.391 Cb - 0.813 Cr, B = Y' + 2.018 Cb, Y' = 1.164 (Y - 16) */
static LONG t_y[256], t_rv[256], t_gu[256], t_gv[256], t_bu[256];   /* (more than a WORD holds) */
static UBYTE clip_[1024];
#define CLIP(v) clip_[((v) >> 8) + 384]
static BOOL tables;

static void make_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        t_y[i] = (LONG)(298 * (i - 16) + 128);
        t_rv[i] = (LONG)(409 * (i - 128));
        t_gu[i] = (LONG)(-100 * (i - 128));
        t_gv[i] = (LONG)(-208 * (i - 128));
        t_bu[i] = (LONG)(516 * (i - 128));
    }
    for (i = 0; i < 1024; i++)
        clip_[i] = (UBYTE)(i < 384 ? 0 : i > 384 + 255 ? 255 : i - 384);
    tables = TRUE;
}

BOOL yuv_format_ok(ULONG fmt)
{
    switch (fmt) {
    case PIXFMT_RGB15: case PIXFMT_BGR15: case PIXFMT_RGB15PC: case PIXFMT_BGR15PC:
    case PIXFMT_RGB16: case PIXFMT_BGR16: case PIXFMT_RGB16PC: case PIXFMT_BGR16PC:
    case PIXFMT_RGB24: case PIXFMT_BGR24:
    case PIXFMT_ARGB32: case PIXFMT_BGRA32: case PIXFMT_RGBA32:
        return TRUE;
    }
    return FALSE;
}

/* Bytes a pixel of fmt. */
static ULONG fmt_bytes(ULONG fmt)
{
    return fmt >= PIXFMT_ARGB32 ? 4 : fmt >= PIXFMT_RGB24 ? 3 : 2;
}

BOOL yuv_draw(const UBYTE *y, const UBYTE *cb, const UBYTE *cr, ULONG ymod, ULONG cmod, ULONG sw,
              ULONG sh, UBYTE *dst, ULONG bpr, ULONG fmt, ULONG dw, ULONG dh)
{
    UWORD *xs;
    ULONG i, j, pb = fmt_bytes(fmt);
    if (!yuv_format_ok(fmt) || !dw || !dh || !sw || !sh)
        return FALSE;
    if (!tables)
        make_tables();
    if (!(xs = AllocVec(dw * sizeof(UWORD), MEMF_ANY)))
        return FALSE;
    for (i = 0; i < dw; i++)
        xs[i] = (UWORD)(i * sw / dw);
    /* one loop per format (the pixel's packing inside it, not a switch a
     * pixel) */
#define ROWS(PUT)                                                                              \
    for (j = 0; j < dh; j++) {                                                                 \
        ULONG sy = j * sh / dh;                                                                \
        const UBYTE *py = y + sy * ymod, *pu = cb + (sy >> 1) * cmod, *pv = cr + (sy >> 1) * cmod; \
        UBYTE *o = dst + j * bpr;                                                              \
        for (i = 0; i < dw; i++, o += pb) {                                                    \
            ULONG sx = xs[i], c = sx >> 1;                                                     \
            LONG yy = t_y[py[sx]], u = pu[c], v = pv[c];                                       \
            ULONG r = CLIP(yy + t_rv[v]), g = CLIP(yy + t_gu[u] + t_gv[v]), b = CLIP(yy + t_bu[u]); \
            PUT;                                                                               \
        }                                                                                      \
    }
#define W16(x) (*(UWORD *)o = (UWORD)(x))
#define W16PC(x) do { UWORD w_ = (UWORD)(x); o[0] = (UBYTE)w_; o[1] = (UBYTE)(w_ >> 8); } while (0)
    switch (fmt) {
    case PIXFMT_RGB16:   ROWS(W16(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))); break;
    case PIXFMT_RGB16PC: ROWS(W16PC(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))); break;
    case PIXFMT_BGR16:   ROWS(W16(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3))); break;
    case PIXFMT_BGR16PC: ROWS(W16PC(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3))); break;
    case PIXFMT_RGB15:   ROWS(W16(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))); break;
    case PIXFMT_RGB15PC: ROWS(W16PC(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))); break;
    case PIXFMT_BGR15:   ROWS(W16(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3))); break;
    case PIXFMT_BGR15PC: ROWS(W16PC(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3))); break;
    case PIXFMT_RGB24:   ROWS((o[0] = (UBYTE)r, o[1] = (UBYTE)g, o[2] = (UBYTE)b)); break;
    case PIXFMT_BGR24:   ROWS((o[0] = (UBYTE)b, o[1] = (UBYTE)g, o[2] = (UBYTE)r)); break;
    case PIXFMT_ARGB32:  ROWS(*(ULONG *)o = 0xFF000000UL | (r << 16) | (g << 8) | b); break;
    case PIXFMT_BGRA32:  ROWS(*(ULONG *)o = (b << 24) | (g << 16) | (r << 8) | 0xFF); break;
    case PIXFMT_RGBA32:  ROWS(*(ULONG *)o = (r << 24) | (g << 16) | (b << 8) | 0xFF); break;
    }
#undef ROWS
#undef W16
#undef W16PC
    FreeVec(xs);
    return TRUE;
}
