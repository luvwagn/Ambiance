/* work.h: m68kprof's work (work.c), and what the side running it gives it */
#ifndef M68KPROF_WORK_H
#define M68KPROF_WORK_H
#include <stdint.h>
struct Work {
    const uint8_t *in;          /* the JPEG */
    uint32_t in_len;
    uint32_t box_w, box_h;      /* the box it is fitted in */
    uint32_t dw, dh, bw, bh;    /* out: the size made, jdec's size */
    int32_t scale;              /* out: jdec's 1/(1 << scale) */
    uint8_t *out;               /* out: dw x dh RGB */
};
enum { WORK_OPEN = 1, WORK_DECODE, WORK_DONE };
int work_run(struct Work *w);
void *work_alloc(uint32_t n);
void work_free(void *p);
void work_mark(int what);       /* a phase begins (counted apart on the 68k) */
#endif
