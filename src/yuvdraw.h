/*
 * AmigaMCE - a video picture's YUV planes drawn straight into an RTG
 * bitmap's memory (yuvdraw.c)
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef AMCE_YUVDRAW_H
#define AMCE_YUVDRAW_H

#include <exec/types.h>

/* fmt (cybergraphics' PIXFMT_) one yuv_draw writes: 15, 16, 24 or 32 bits */
BOOL yuv_format_ok(ULONG fmt);
/* The sw x sh picture (4:2:0: y, cb and cr, ymod and cmod bytes a row)
 * written at dst (bpr bytes a row, fmt pixels) scaled to dw x dh, nearest
 * pixel. FALSE if fmt is not one it writes (or no memory). */
BOOL yuv_draw(const UBYTE *y, const UBYTE *cb, const UBYTE *cr, ULONG ymod, ULONG cmod, ULONG sw,
              ULONG sh, UBYTE *dst, ULONG bpr, ULONG fmt, ULONG dw, ULONG dh);

#endif
