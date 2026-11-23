/*
 * work_mpv.c - m68kprof's mpegv work: the stream (the input file) pushed
 * in whole, box_w pictures decoded at most (MpvDecode), each picture's Y,
 * Cb and Cr summed (a word each) so a change can be checked for the same
 * pictures; box_h's bits: 1 each made RGB565 too (MpvToRGB), 2 half size
 * (MPVA_Half), 4 B pictures skipped (MPVA_SkipB). mpegv built static
 * (MPV_STATIC): exec's AllocVec and FreeVec through a stand-in library
 * base, onto m68kprof's allocator. Copyright (c) 2026 Brian King, MIT
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <utility/tagitem.h>
#include <libraries/mpegv.h>
#include <stdint.h>
#include "work.h"

APTR MpvOpen(struct TagItem *tags);
VOID MpvClose(APTR mpv);
LONG MpvWrite(APTR mpv, CONST UBYTE *buf, LONG len);
struct MpvPicture *MpvDecode(APTR mpv, LONG *error);
LONG MpvControl(APTR mpv, ULONG cmd, ULONG value);
VOID MpvToRGB(struct MpvPicture *pic, UBYTE *dst, ULONG modulo, ULONG format);

struct ExecBase *SysBase;
static uint16_t fake[512];              /* the base's jump table below it */

void *fake_allocvec(uint32_t n)
{
    uint8_t *p = work_alloc(n);         /* (MEMF_ANY: as it comes) */
    return p;
}

void fake_freevec(void *p)
{
    work_free(p);
}

/* exec's register calls: AllocVec d0 (d1 flags), FreeVec a1 */
__asm__("_fake_allocvec_r: movel d0,sp@-\n\tjsr _fake_allocvec\n\taddql #4,sp\n\trts\n"
        "_fake_freevec_r: movel a1,sp@-\n\tjsr _fake_freevec\n\taddql #4,sp\n\trts\n");
extern char fake_allocvec_r[] __asm__("_fake_allocvec_r"), fake_freevec_r[] __asm__("_fake_freevec_r");

static void jump(int lvo, void *to)
{
    uint16_t *j = (uint16_t *)((uint8_t *)fake + 1000 - lvo);
    j[0] = 0x4EF9;                      /* jmp abs.l */
    j[1] = (uint16_t)((uint32_t)to >> 16);
    j[2] = (uint16_t)(uint32_t)to;
}

int work_run(struct Work *w)
{
    struct TagItem tags[4];
    APTR m;
    struct MpvPicture *p;
    LONG e;
    uint32_t n = 0, more = 0;
    uint16_t *sum = work_alloc(w->box_w * 6 + 4);
    UBYTE *rgb = NULL;
    SysBase = (struct ExecBase *)((uint8_t *)fake + 1000);
    jump(684, fake_allocvec_r);
    jump(690, fake_freevec_r);
    tags[0].ti_Tag = MPVA_Half;
    tags[0].ti_Data = (w->box_h & 2) != 0;
    tags[1].ti_Tag = MPVA_SkipB;
    tags[1].ti_Data = (w->box_h & 4) != 0;
    tags[2].ti_Tag = TAG_DONE;
    work_mark(WORK_OPEN);
    if (!(m = MpvOpen(tags)))
        return -1;
    MpvWrite(m, w->in, (LONG)w->in_len);
    MpvControl(m, MPVC_END, 0);
    work_mark(WORK_DECODE);
    while (n < w->box_w) {
        uint32_t r, x, a;
        if (!(p = MpvDecode(m, &e))) {
            if (e == MPVE_MORE && ++more < 1000)
                continue;
            break;
        }
        if (w->box_h & 1) {
            if (!rgb && !(rgb = work_alloc((uint32_t)p->mpic_Width * p->mpic_Height * 2)))
                break;
            MpvToRGB(p, rgb, (ULONG)p->mpic_Width * 2, MPVR_RGB565);
        }
        for (a = r = 0; r < p->mpic_Height; r++)
            for (x = 0; x < p->mpic_Width; x++)
                a += p->mpic_Y[r * p->mpic_YModulo + x] * (x + 1);
        sum[n * 3] = (uint16_t)a;
        for (a = r = 0; r < p->mpic_Height / 2u; r++)
            for (x = 0; x < p->mpic_Width / 2u; x++)
                a += p->mpic_Cb[r * p->mpic_CModulo + x] * (x + 1);
        sum[n * 3 + 1] = (uint16_t)a;
        for (a = r = 0; r < p->mpic_Height / 2u; r++)
            for (x = 0; x < p->mpic_Width / 2u; x++)
                a += p->mpic_Cr[r * p->mpic_CModulo + x] * (x + 1);
        sum[n * 3 + 2] = (uint16_t)a;
        if (rgb) {
            for (a = x = 0; x < (uint32_t)p->mpic_Width * p->mpic_Height; x++)
                a = a * 31 + rgb[x * 2] + (rgb[x * 2 + 1] << 8);
            sum[n * 3] ^= (uint16_t)(a >> 16);
        }
        w->dw = p->mpic_Width;
        w->dh = p->mpic_Height;
        n++;
    }
    work_mark(WORK_DONE);
    MpvClose(m);
    w->out = (uint8_t *)sum;
    w->out_len = n * 6;
    w->scale = (int32_t)n;
    return 0;
}
