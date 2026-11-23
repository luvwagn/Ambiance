/*
 * t68k.c - m68kprof's 68k side: no AmigaOS, no libraries. The host puts
 * the JPEG and the box in a mailbox at 0x400, runs from start.s, and reads
 * the results back from the same mailbox. Copyright (c) 2026 Brian King, MIT
 */
#include <stdint.h>
#include <stddef.h>
#include "work.h"

#define BOX ((volatile uint32_t *)0x400)
enum { B_IN, B_INLEN, B_BOXW, B_BOXH, B_DW, B_DH, B_BW, B_BH, B_SCALE, B_OUT, B_R, B_HEAP, B_HEAPEND, B_OUTLEN };
#define MARK (*(volatile uint32_t *)0x500)
#define END (*(volatile uint32_t *)0x504)

static uint8_t *heap, *heap_end;

void *work_alloc(uint32_t n)
{
    uint8_t *p = heap;
    n = (n + 15) & ~15UL;
    if (p + n > heap_end)
        return NULL;
    heap += n;
    return p;
}

void work_free(void *p)
{
    (void)p;                    /* (a bump allocator: memory enough for one run) */
}

void work_mark(int what)
{
    MARK = (uint32_t)what;
}

/* what shrink.c (built SHRINK_HOST) and jdec call */
void *calloc(size_t n, size_t s)
{
    uint8_t *p = work_alloc((uint32_t)(n * s));
    uint32_t i, k = (uint32_t)(n * s);
    if (p)
        for (i = 0; i < k; i++)
            p[i] = 0;
    return p;
}

void free(void *p)
{
    work_free(p);
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dd = d;
    const uint8_t *ss = s;
    if (!(((uint32_t)dd | (uint32_t)ss) & 3)) {
        uint32_t *dl = (uint32_t *)dd;
        const uint32_t *sl = (const uint32_t *)ss;
        for (; n >= 4; n -= 4)
            *dl++ = *sl++;
        dd = (uint8_t *)dl;
        ss = (const uint8_t *)sl;
    }
    while (n--)
        *dd++ = *ss++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dd = d;
    const uint8_t *ss = s;
    if (dd <= ss || dd >= ss + n)
        return memcpy(d, s, n);
    while (n--)
        dd[n] = ss[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dd = d;
    while (n--)
        *dd++ = (uint8_t)c;
    return d;
}

void t_main(void)
{
    struct Work w;
    uint32_t i;
    uint8_t *z = (uint8_t *)&w;
    for (i = 0; i < sizeof(w); i++)
        z[i] = 0;
    heap = (uint8_t *)BOX[B_HEAP];
    heap_end = (uint8_t *)BOX[B_HEAPEND];
    w.in = (const uint8_t *)BOX[B_IN];
    w.in_len = BOX[B_INLEN];
    w.box_w = BOX[B_BOXW];
    w.box_h = BOX[B_BOXH];
    BOX[B_R] = (uint32_t)work_run(&w);
    BOX[B_DW] = w.dw;
    BOX[B_DH] = w.dh;
    BOX[B_BW] = w.bw;
    BOX[B_BH] = w.bh;
    BOX[B_SCALE] = (uint32_t)w.scale;
    BOX[B_OUT] = (uint32_t)w.out;
    BOX[B_OUTLEN] = w.out_len;
    END = 1;
}

/* GCC's memmove for AmigaOS (bcopy's order: from, to, length) */
void __bcopz(const void *s, void *d, size_t n)
{
    memmove(d, s, n);
}
