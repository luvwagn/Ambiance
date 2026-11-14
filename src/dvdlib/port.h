/*
 * port.h: between amcedvd.c and port.c (the C library for libdvdread and
 * libdvdnav inside amcedvd.library).
 * (c) 2026 Brian King. Part of amcedvd.library: GPL-2.0-or-later.
 */
#ifndef AMCEDVD_PORT_H
#define AMCEDVD_PORT_H
#include <setjmp.h>

/* the opener's context, set by each library call (amcedvd.c) */
struct dvdp_ctx {
    void (*log)(struct dvdp_ctx *ctx, int level, const char *line); /* DVDLOG_ levels */
    jmp_buf *jb;                    /* abort() returns here: the call fails */
};
extern struct dvdp_ctx *dvdp_cur;

unsigned long dvdp_memory(int peak);
void dvdp_clock(unsigned long *sec, unsigned long *usec);  /* amitime.c */
#endif
