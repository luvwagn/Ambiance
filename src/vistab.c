/*
 * AmigaMCE - the spectrum's tables (vis.c), apart so the FFT can be
 * tested with them under vamos (tests/test_fft.c)
 * Copyright (c) 2026 Brian King
 */
#include <exec/types.h>

#define LEVELS 64

void vis_tables(WORD *sine);
void vis_loud_table(ULONG *t);

/* HippoPlayer's sine (prepareSpectrumSineTable: 32767 * sin, then >> 1),
 * by a recurrence instead of mathffp.library: s(n+1) = 2 cos(d) s(n) -
 * s(n-1), in 2.30 fixed point, a quarter of it then mirrored. Within one
 * of HippoPlayer's values. */
void vis_tables(WORD *sine)
{
    const LONG c = 1073721611L;         /* cos(2 pi / 1024) * 2^30 */
    LONG a = 0, b = 6588388L, t;        /* sin(0), sin(2 pi / 1024) * 2^30 */
    int i;
    for (i = 0; i <= 256; i++) {
        WORD v = (WORD)(((a >> 15) * 32767L) >> 15);
        sine[i] = (WORD)(v >> 1);
        if (i < 256) {
            t = (LONG)(((long long)c * b) >> 29) - a;
            a = b;
            b = t;
        }
    }
    for (i = 257; i < 512; i++)
        sine[i] = sine[512 - i];
    for (i = 512; i < 1024; i++)
        sine[i] = (WORD)-sine[i - 512];
}

/* HippoPlayer's loudness steps (prepareSpectrumExpTable): (32767 e^(-0.16
 * i))^2 for i 0-63, each the mean of it and the next, quietest first. */
void vis_loud_table(ULONG *t)
{
    ULONG e[LEVELS];
    LONG q = 32767L << 16;              /* 16.16 */
    int i;
    for (i = 0; i < LEVELS; i++) {
        ULONG r = (ULONG)(q >> 8);      /* 8 bits of fraction: its square fits 64 bits */
        e[i] = (ULONG)(((unsigned long long)r * r) >> 16);
        q = (LONG)(((long long)q * 55846) >> 16);   /* e^-0.16 * 65536 */
    }
    for (i = 0; i < LEVELS - 1; i++)
        e[i] = (e[i] + e[i + 1]) >> 1;
    for (i = 0; i < LEVELS; i++)
        t[i] = e[LEVELS - 1 - i];
}

