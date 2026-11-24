/*
 * ambbench - times on a real Amiga what Ambiance's 68k code does, for
 * the figures tools/m68kprof's 68060 model can only estimate:
 *
 *   MEM    chip and fast RAM: longwords written and read, MB/s and ns each
 *   C2P    a 320 x 256 frame of pens to 8 bitplanes, in chip RAM and in
 *          fast RAM: the old C (gfx.c's c2p32), va_c2p68k (68020+),
 *          va_c2p68k_sb (the store buffer's, 68040/060); half size's
 *          va_c2p2x68k twice against va_c2p2x68k_twice. The planes are
 *          checked to be the same from each.
 *   PENS   AGA video's pictures made bytes for the planes, a 320 x 256
 *          4:2:0 frame: as pens of a palette (va_pens68k, pens68k.s) and
 *          as HAM8 (ham8.c and ham8_68k.s: once and twice across)
 *   MP3    Ambiance's own decoders, PROGDIR:Libs/ambmpega.library and
 *          ambmpega060.library (68060), through MPGA_decode_to as
 *          Ambiance calls them: high, medium, 22 kHz mono
 *   VIDEO  mpegv.library (PROGDIR:Libs): each file's pictures decoded,
 *          ms a picture; again with B pictures passed over; and at half
 *          size
 *
 *   ambbench [ONLY=MEM|C2P|PENS|MP3|VIDEO] [MP3=<file>] [VIDEO=<file>...] [QUICK]
 *
 * Default files: PROGDIR:Bench/g128.mp3, PROGDIR:Bench/pal.m2v and
 * PROGDIR:Bench/ps.mpg. CTRL-C stops it between tests. The report goes to
 * the Shell and is added to PROGDIR:ambbench.txt. (JPEG: zzprobe's own
 * column times jdec and the shrink.) Copyright (c) 2026 Brian King, MIT
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <dos/rdargs.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <libraries/mpegv.h>
#include <proto/mpegv.h>
#include <libraries/mpega.h>
#include "../../src/ham8.h"
#include <string.h>
#include <stdio.h>
#include "../zzprobe/stackcall.h"

static const char __attribute__((used)) version[] = "$VER: ambbench 1.0 (23.11.2026)";

extern struct ExecBase *SysBase;
struct Device *TimerBase;
struct Library *MpegvBase;
static struct Library *MPEGABase;

static struct timerequest treq;
static ULONG efreq;
static BOOL quick, brk;

/* ------------------------------------------------------------ the report */
static char *out;
static ULONG outn, outmax = 65536;

static void say(const char *s)
{
    ULONG n = strlen(s);
    PutStr((CONST_STRPTR)s);
    if (out && outn + n < outmax) {
        memcpy(out + outn, s, n);
        outn += n;
    }
}

static void sayf(const char *s)
{
    say(s);
    say("\n");
}

static void heading(const char *s)
{
    say("\n");
    sayf(s);
}

static ULONG now_us(void)
{
    struct EClockVal e;
    ReadEClock(&e);
    return (ULONG)((((unsigned long long)e.ev_hi << 32) | e.ev_lo) * 1000000ULL / efreq);
}

static void fmt_ms(char *s, ULONG us)
{
    if (us >= 100000)
        sprintf(s, "%ld ms", (long)((us + 500) / 1000));
    else
        sprintf(s, "%ld.%02ld ms", (long)(us / 1000), (long)(us % 1000 / 10));
}

static BOOL stop(void)
{
    if (!brk && (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)) {
        brk = TRUE;
        sayf("  *** stopped (CTRL-C)");
    }
    return brk;
}

static BOOL same(const char *a, const char *b)   /* (ASCII, case ignored) */
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return FALSE;
    return !*a && !*b;
}

static UWORD cpu_of(void)
{
    UWORD af = SysBase->AttnFlags;
    return (af & AFF_68060) ? 60 : (af & AFF_68040) ? 40 : (af & AFF_68030) ? 30 :
           (af & AFF_68020) ? 20 : (af & AFF_68010) ? 10 : 0;
}

/* a file into memory (fast if there is some), its first max bytes at
 * most (a DVD's VOB is gigabytes); the length in *n */
static UBYTE *load(const char *path, ULONG *n, ULONG max)
{
    BPTR fh, lk;
    UBYTE *b = NULL;
    LONG len = 0;
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    *n = 0;
    if (!fib)
        return NULL;
    if ((lk = Lock((CONST_STRPTR)path, SHARED_LOCK))) {
        if (Examine(lk, fib))
            len = fib->fib_Size;
        UnLock(lk);
    }
    FreeDosObject(DOS_FIB, fib);
    if (max && (ULONG)len > max)
        len = (LONG)max;
    if (!(fh = Open((CONST_STRPTR)path, MODE_OLDFILE)))
        return NULL;
    if (len > 0 && (b = AllocVec((ULONG)len + 16, MEMF_ANY))) {
        if (Read(fh, b, len) != len) {
            FreeVec(b);
            b = NULL;
        } else {
            *n = (ULONG)len;
        }
    }
    Close(fh);
    return b;
}

/* a library out of memory if nothing has it open, so that the one in
 * PROGDIR:Libs is the one opened (not one Ambiance left there) */
static void expunge_lib(const char *name)
{
    struct Library *l;
    Forbid();
    if ((l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)name)) && !l->lib_OpenCnt)
        RemLibrary(l);
    Permit();
}

static void lib_line(struct Library *l, const char *path)
{
    char line[200];
    sprintf(line, "  %.60s: %ld.%ld %.80s", path, (long)l->lib_Version, (long)l->lib_Revision,
            l->lib_IdString ? (const char *)l->lib_IdString : "");
    sayf(line);
}

/* ------------------------------------------------------------ machine */
static void machine(void)
{
    char line[200];
    UWORD af = SysBase->AttnFlags;
    heading("Machine");
    sprintf(line, "  CPU 680%02ld%s, Kickstart %ld.%ld; chip RAM free %ld K, fast %ld K", (long)cpu_of(),
            (af & AFF_FPU40) ? " (FPU)" : (af & (AFF_68881 | AFF_68882)) ? " + FPU" : "",
            (long)SysBase->LibNode.lib_Version, (long)SysBase->SoftVer,
            (long)(AvailMem(MEMF_CHIP) >> 10), (long)(AvailMem(MEMF_FAST) >> 10));
    sayf(line);
    sprintf(line, "  E clock %ld Hz (the timer)", (long)efreq);
    sayf(line);
    sayf("  (Please note the CPU's clock with the report: 50 MHz, 28 MHz, ...)");
}

/* ------------------------------------------------------------ MEM */
/* n longwords (a multiple of 16) written, read */
static void mem_write(ULONG *p, ULONG n)
{
    register ULONG *a0 __asm("a0") = p;
    register ULONG d1 __asm("d1") = n >> 4;
    __asm volatile("moveq #0,%%d0\n"
                   "1:\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\t"
                   "move.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\t"
                   "move.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\t"
                   "move.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\tmove.l %%d0,(%0)+\n\t"
                   "subq.l #1,%1\n\tbne.s 1b"
                   : "+a"(a0), "+d"(d1)
                   :
                   : "d0", "cc", "memory");
}

static ULONG mem_read(const ULONG *p, ULONG n)
{
    register const ULONG *a0 __asm("a0") = p;
    register ULONG d1 __asm("d1") = n >> 4;
    register ULONG d0 __asm("d0") = 0;
    __asm volatile("1:\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\t"
                   "add.l (%0)+,%2\n\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\t"
                   "add.l (%0)+,%2\n\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\t"
                   "add.l (%0)+,%2\n\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\tadd.l (%0)+,%2\n\t"
                   "subq.l #1,%1\n\tbne.s 1b"
                   : "+a"(a0), "+d"(d1), "+d"(d0)
                   :
                   : "cc", "memory");
    return d0;
}

static void mem_one(const char *what, ULONG flags, ULONG bytes)
{
    ULONG *b = AllocMem(bytes, flags), pass, t0, us, best_w = ~0UL, best_r = ~0UL, n = bytes / 4;
    ULONG passes = quick ? 4 : 12;
    char line[200];
    volatile ULONG sink = 0;
    if (!b) {
        sprintf(line, "  %s: %ld K could not be had", what, (long)(bytes >> 10));
        sayf(line);
        return;
    }
    for (pass = 0; pass < passes && !stop(); pass++) {
        t0 = now_us();
        mem_write(b, n);
        us = now_us() - t0;
        if (us < best_w)
            best_w = us;
        t0 = now_us();
        sink += mem_read(b, n);
        us = now_us() - t0;
        if (us < best_r)
            best_r = us;
    }
    if (!brk && best_w && best_r) {
        sprintf(line, "  %-34s write %ld.%ld MB/s (%ld ns a long), read %ld.%ld MB/s (%ld ns a long)", what,
                (long)(bytes / best_w), (long)(bytes * 10 / best_w % 10), (long)(best_w * 1000 / n),
                (long)(bytes / best_r), (long)(bytes * 10 / best_r % 10), (long)(best_r * 1000 / n));
        sayf(line);
    }
    FreeMem(b, bytes);
    (void)sink;
}

static void bench_mem(void)
{
    heading("MEM: longwords written and read (the best of several passes)");
    mem_one("chip RAM, 256 KB", MEMF_CHIP, 256 * 1024);
    mem_one("fast RAM, 1 MB (past the caches)", MEMF_FAST, 1024 * 1024);
    mem_one("fast RAM, 4 KB (in the data cache)", MEMF_FAST, 4096);
}

/* ------------------------------------------------------------ C2P */
void va_c2p68k(const UBYTE *src, ULONG groups, UBYTE **planes);
void va_c2p68k_sb(const UBYTE *src, ULONG groups, UBYTE **planes);
void va_c2p2x68k(const UBYTE *src, ULONG groups, UBYTE **planes);
void va_c2p2x68k_twice(const UBYTE *src, ULONG groups, UBYTE **planes, ULONG bpr);

/* gfx.c's c2p32 and gfx_chunky's row, as they were before 0.9.6 */
#define MERGE(lo, hi, sh, m) \
    do { ULONG t_ = (((hi) >> (sh)) ^ (lo)) & (m); (lo) ^= t_; (hi) ^= t_ << (sh); } while (0)
static void c2p32(const ULONG *c, ULONG *o)
{
    ULONG a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3], a4 = c[4], a5 = c[5], a6 = c[6], a7 = c[7];
    MERGE(a0, a4, 16, 0x0000FFFFUL);
    MERGE(a1, a5, 16, 0x0000FFFFUL);
    MERGE(a2, a6, 16, 0x0000FFFFUL);
    MERGE(a3, a7, 16, 0x0000FFFFUL);
    MERGE(a0, a2, 8, 0x00FF00FFUL);
    MERGE(a1, a3, 8, 0x00FF00FFUL);
    MERGE(a4, a6, 8, 0x00FF00FFUL);
    MERGE(a5, a7, 8, 0x00FF00FFUL);
    MERGE(a0, a1, 4, 0x0F0F0F0FUL);
    MERGE(a2, a3, 4, 0x0F0F0F0FUL);
    MERGE(a4, a5, 4, 0x0F0F0F0FUL);
    MERGE(a6, a7, 4, 0x0F0F0F0FUL);
    MERGE(a0, a4, 2, 0x33333333UL);
    MERGE(a1, a5, 2, 0x33333333UL);
    MERGE(a2, a6, 2, 0x33333333UL);
    MERGE(a3, a7, 2, 0x33333333UL);
    MERGE(a0, a2, 1, 0x55555555UL);
    MERGE(a1, a3, 1, 0x55555555UL);
    MERGE(a4, a6, 1, 0x55555555UL);
    MERGE(a5, a7, 1, 0x55555555UL);
    o[0] = a7; o[1] = a5; o[2] = a3; o[3] = a1;
    o[4] = a6; o[5] = a4; o[6] = a2; o[7] = a0;
}

static void c2p_c(const UBYTE *src, ULONG nl, UBYTE **planes)
{
    ULONG x, k, pl[8];
    UWORD *dst[8];
    const ULONG *c = (const ULONG *)src;
    for (k = 0; k < 8; k++)
        dst[k] = (UWORD *)planes[k];
    for (x = 0; x < nl; x++, c += 8) {
        c2p32(c, pl);
        for (k = 0; k < 8; k++) {
            *(ULONG *)dst[k] = pl[k];
            dst[k] += 2;
        }
    }
}

#define FW 320
#define FH 256
#define BPR (FW / 8)

static ULONG sum_planes(const UBYTE *pl)
{
    ULONG s = 0, i;
    const ULONG *p = (const ULONG *)pl;
    for (i = 0; i < BPR * FH * 8 / 4; i++)
        s = (s << 5 | s >> 27) ^ p[i];
    return s;
}

/* how: 0 the C, 1 va_c2p68k, 2 va_c2p68k_sb, 3 va_c2p2x68k twice a row
 * pair, 4 va_c2p2x68k_twice; a frame into pl (8 planes one after another) */
static void c2p_frame(int how, const UBYTE *pens, UBYTE *pl)
{
    UBYTE *pp[8];
    ULONG y, k;
    if (how >= 3) {
        /* half size: 160 pens a row, each twice across, each row twice down */
        for (y = 0; y < FH; y += 2) {
            const UBYTE *s = pens + (y / 2) * (FW / 2);
            for (k = 0; k < 8; k++)
                pp[k] = pl + k * BPR * FH + y * BPR;
            if (how == 4) {
                va_c2p2x68k_twice(s, FW / 32, pp, BPR);
            } else {
                va_c2p2x68k(s, FW / 32, pp);
                for (k = 0; k < 8; k++)
                    pp[k] += BPR;
                va_c2p2x68k(s, FW / 32, pp);
            }
        }
        return;
    }
    for (y = 0; y < FH; y++) {
        for (k = 0; k < 8; k++)
            pp[k] = pl + k * BPR * FH + y * BPR;
        if (how == 0)
            c2p_c(pens + y * FW, FW / 32, pp);
        else if (how == 1)
            va_c2p68k(pens + y * FW, FW / 32, pp);
        else
            va_c2p68k_sb(pens + y * FW, FW / 32, pp);
    }
}

static void bench_c2p(void)
{
    static const char *const name[] = { "the old C (gfx.c's c2p32)", "va_c2p68k (68020+)",
                                        "va_c2p68k_sb (store buffer's)", "half size: va_c2p2x68k x 2",
                                        "half size: va_c2p2x68k_twice" };
    UBYTE *pens = AllocVec(FW * FH + 32, MEMF_ANY), *pl;
    ULONG mem, i, seed = 12345, frames = quick ? 10 : 40;
    char line[200], t[24];
    heading("C2P: a 320 x 256 frame of pens to 8 bitplanes (the best frame of several)");
    if (!pens)
        return;
    for (i = 0; i < FW * FH; i++) {
        seed = seed * 1103515245UL + 12345UL;
        pens[i] = (UBYTE)(seed >> 16);
    }
    for (mem = 0; mem < 2 && !brk; mem++) {
        ULONG flags = mem ? MEMF_FAST : MEMF_CHIP, sum[5];
        int how;
        if (!(pl = AllocMem(BPR * FH * 8, flags)))
            continue;
        sayf(mem ? "  into fast RAM:" : "  into chip RAM (as to the screen):");
        for (how = 0; how < 5 && !stop(); how++) {
            ULONG f, best = ~0UL, total = 0, t0, us;
            for (f = 0; f < frames && !stop(); f++) {
                t0 = now_us();
                c2p_frame(how, pens, pl);
                us = now_us() - t0;
                total += us;
                if (us < best)
                    best = us;
            }
            if (brk)
                break;
            sum[how] = sum_planes(pl);
            fmt_ms(t, best);
            sprintf(line, "    %-32s %s a frame (mean %ld.%02ld ms)%s", name[how], t, (long)(total / frames / 1000),
                    (long)(total / frames % 1000 / 10),
                    (how == 1 || how == 2) && sum[how] != sum[0] ? "  PLANES DIFFER from the C's"
                    : how == 4 && sum[4] != sum[3] ? "  PLANES DIFFER from twice over" : "");
            sayf(line);
        }
        FreeMem(pl, BPR * FH * 8);
    }
    FreeVec(pens);
}

/* ------------------------------------------------------------ PENS */
void va_pens68k(const UBYTE *y, const UWORD *cl, UBYTE *dst, ULONG quads, const UWORD *yt4,
                const UBYTE *lut);          /* pens68k.s */

static void bench_pens(void)
{
    static const char *const name[] = { "pens (va_pens68k, a palette)", "HAM8 once across (lowres)",
                                        "HAM8 twice across (hires)" };
    ULONG fsz = FW * FH * 3 / 2, i, seed = 777, frames = quick ? 10 : 30, x, y;
    UBYTE *yuv = AllocVec(fsz, MEMF_ANY), *row = AllocVec(FW * 2 + 64, MEMF_ANY);
    UBYTE *lut = AllocVec(8192, MEMF_ANY);
    UWORD *yt4 = AllocVec(4096 * 2, MEMF_ANY), *cl = AllocVec(FW * 2, MEMF_ANY);
    struct Ham8 *h = AllocVec(sizeof(*h), MEMF_ANY);
    char line[200], t[24];
    int how;
    heading("PENS: AGA video, a 320 x 256 picture made bytes for its planes (the best of several)");
    if (!yuv || !row || !lut || !yt4 || !cl || !h)
        goto out;
    /* a picture like a film's: smooth areas, edges and noise */
    for (y = 0; y < FH; y++)
        for (x = 0; x < FW; x++) {
            seed = seed * 1103515245UL + 12345UL;
            yuv[y * FW + x] = (UBYTE)(40 + (x + y) % 150 + ((x / 40) & 1) * 40 + ((seed >> 16) & 7));
        }
    for (i = 0; i < FW * FH / 2; i++) {
        seed = seed * 1103515245UL + 12345UL;
        yuv[FW * FH + i] = (UBYTE)(100 + (i % 160) / 4 + ((seed >> 16) & 3));
    }
    for (i = 0; i < 4096; i++)
        yt4[i] = (UWORD)(((i >> 2) >> 3) << 8);
    for (i = 0; i < 8192; i++)
        lut[i] = (UBYTE)(i * 37);
    {
        ULONG t0 = now_us();
        ham8_init(h);
        fmt_ms(t, now_us() - t0);
        sprintf(line, "    HAM8's tables made in %s (once a video)", t);
        sayf(line);
    }
    for (how = 0; how < 3 && !stop(); how++) {
        ULONG f, best = ~0UL, total = 0, t0, us;
        for (f = 0; f < frames && !stop(); f++) {
            t0 = now_us();
            for (y = 0; y < FH; y++) {
                const UBYTE *Y = yuv + y * FW, *U = yuv + FW * FH + (y >> 1) * (FW / 2),
                            *V = yuv + FW * FH + FW * FH / 4 + (y >> 1) * (FW / 2);
                if (how == 0) {
                    if (!(y & 1))
                        for (x = 0; x < FW / 2; x++)
                            cl[x] = (UWORD)(((U[x] >> 4) << 4) | (V[x] >> 4));
                    va_pens68k(Y, cl, row, FW / 4, yt4 + ((y & 3) << 10), lut);
                } else
                    ham8_row(h, Y, U, V, FW, row, how == 2);
            }
            us = now_us() - t0;
            total += us;
            if (us < best)
                best = us;
        }
        if (brk)
            break;
        fmt_ms(t, best);
        sprintf(line, "    %-32s %s a frame (mean %ld.%02ld ms)", name[how], t, (long)(total / frames / 1000),
                (long)(total / frames % 1000 / 10));
        sayf(line);
    }
out:
    FreeVec(yuv);
    FreeVec(row);
    FreeVec(lut);
    FreeVec(yt4);
    FreeVec(cl);
    FreeVec(h);
}

/* ------------------------------------------------------------ MP3 */
#define MPGA_decode_to(s, o) \
    LP2(72, LONG, MPGA_decode_to, MPEGA_STREAM *, s, a0, struct MpgaOut *, o, a1, , MPEGABase)
#define MPEGA_open(n, c) \
    LP2(30, MPEGA_STREAM *, MPEGA_open, char *, n, a0, MPEGA_CTRL *, c, a1, , MPEGABase)
#define MPEGA_close(s) LP1NR(36, MPEGA_close, MPEGA_STREAM *, s, a0, , MPEGABase)

static const UBYTE *mp3_data;
static LONG mp3_len, mp3_pos;

static ULONG bs_func(register struct Hook *h __asm("a0"), register APTR handle __asm("a2"),
                     register MPEGA_ACCESS *a __asm("a1"))
{
    (void)h;
    (void)handle;
    switch (a->func) {
    case MPEGA_BSFUNC_OPEN:
        mp3_pos = 0;
        a->data.open.stream_size = mp3_len;
        return 1;
    case MPEGA_BSFUNC_CLOSE:
        return 0;
    case MPEGA_BSFUNC_READ: {
        LONG n = a->data.read.num_bytes;
        if (n > mp3_len - mp3_pos)
            n = mp3_len - mp3_pos;
        if (n > 0) {
            CopyMem((APTR)(mp3_data + mp3_pos), a->data.read.buffer, (ULONG)n);
            mp3_pos += n;
        }
        return n < 0 ? 0 : (ULONG)n;
    }
    case MPEGA_BSFUNC_SEEK:
        if (a->data.seek.abs_byte_seek_pos < 0 || a->data.seek.abs_byte_seek_pos > mp3_len)
            return 1;
        mp3_pos = a->data.seek.abs_byte_seek_pos;
        return 0;
    }
    return 0;
}

static void mp3_one(const char *label, WORD q, WORD div, WORD mono, ULONG frames)
{
    static WORD r0[4096], r1[4096];
    static struct Hook hook;
    MPEGA_CTRL c;
    MPEGA_STREAM *s;
    struct MpgaOut o;
    ULONG nf = 0, samples = 0, t0, us;
    char line[240], t[24];
    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (ULONG (*)())bs_func;
    memset(&c, 0, sizeof(c));
    c.bs_access = &hook;
    c.layer_1_2.mono.freq_div = c.layer_1_2.stereo.freq_div = div;
    c.layer_3.mono.freq_div = c.layer_3.stereo.freq_div = div;
    c.layer_1_2.mono.quality = c.layer_1_2.stereo.quality = q;
    c.layer_3.mono.quality = c.layer_3.stereo.quality = q;
    c.layer_1_2.force_mono = c.layer_3.force_mono = mono;
    c.check_mpeg = 1;
    if (!(s = MPEGA_open(NULL, &c))) {
        sprintf(line, "    %s: not opened", label);
        sayf(line);
        return;
    }
    memset(&o, 0, sizeof(o));
    o.mo_Format = MPGA_FMT_S16;
    o.mo_Buffer[0] = r0;
    o.mo_Buffer[1] = r1;
    o.mo_Mask = 4095;
    t0 = now_us();
    while (nf < frames) {
        LONG n = MPGA_decode_to(s, &o);
        if (n <= 0)
            break;
        samples += (ULONG)n;
        nf++;
        if ((nf & 15) == 0 && stop())
            break;
    }
    us = now_us() - t0;
    if (nf && s->dec_frequency) {
        ULONG fus = us / nf, audio_us = (ULONG)((unsigned long long)samples * 1000000 / (ULONG)s->dec_frequency);
        fmt_ms(t, fus);
        sprintf(line, "    %-26s %s a frame (%ld frames, %ld Hz %s): %ld%% of real time", label, t, (long)nf,
                (long)s->dec_frequency, s->dec_channels == 2 ? "stereo" : "mono",
                (long)(audio_us ? (unsigned long long)us * 100 / audio_us : 0));
    } else {
        sprintf(line, "    %s: nothing decoded", label);
    }
    sayf(line);
    MPEGA_close(s);
}

static void bench_mp3(const char *path)
{
    static const char *const libs[] = { "ambmpega060.library", "ambmpega.library" };
    char line[200], lp[80];
    ULONG n;
    int k;
    heading("MP3: Ambiance's decoders (PROGDIR:Libs), MPGA_decode_to as Ambiance calls it");
    if (!(mp3_data = load(path, &n, 4UL << 20))) {
        sprintf(line, "  no MP3 (%.100s): copy the Bench drawer beside ambbench, or give MP3=<file>", path);
        sayf(line);
        return;
    }
    mp3_len = (LONG)n;
    sprintf(line, "  %.100s, %ld bytes", path, (long)n);
    sayf(line);
    for (k = 0; k < 2 && !stop(); k++) {
        BOOL trap = k == 1 && cpu_of() == 60;   /* (the 68020's build on a 68060: its 64-bit
                                                 * multiplies emulated, slow: a few frames) */
        ULONG frames = trap ? 10 : quick ? 100 : 300;
        if (k == 0 && cpu_of() != 60)
            continue;                   /* (ambmpega060 is for a 68060 only) */
        expunge_lib(libs[k]);
        sprintf(lp, "PROGDIR:Libs/%s", libs[k]);
        if (!(MPEGABase = OpenLibrary((CONST_STRPTR)lp, 1))) {
            sprintf(line, "  %s: not opened", lp);
            sayf(line);
            continue;
        }
        lib_line(MPEGABase, lp);
        if (trap)
            sayf("    (on a 68060 its 64-bit multiplies are emulated: 10 frames)");
        mp3_one("high", MPEGA_QUALITY_HIGH, 1, 0, frames);
        if (!brk)
            mp3_one("medium", MPEGA_QUALITY_MEDIUM, 1, 0, frames);
        if (!brk)
            mp3_one("high, 22 kHz mono", MPEGA_QUALITY_HIGH, 2, 1, frames);
        if (!brk && cpu_of() < 40)
            mp3_one("medium, 22 kHz mono", MPEGA_QUALITY_MEDIUM, 2, 1, frames);
        CloseLibrary(MPEGABase);
        MPEGABase = NULL;
    }
    FreeVec((APTR)mp3_data);
    mp3_data = NULL;
}

/* ------------------------------------------------------------ VIDEO */
static void video_one(const char *label, const UBYTE *data, ULONG len, BOOL half, BOOL skipb)
{
    struct TagItem tags[3];
    APTR m;
    struct MpvPicture *p;
    LONG e;
    ULONG n = 0, more = 0, t0, us, w = 0, h = 0, kinds[4] = { 0, 0, 0, 0 };
    char line[240], t[24];
    tags[0].ti_Tag = MPVA_Half;
    tags[0].ti_Data = half;
    tags[1].ti_Tag = MPVA_SkipB;
    tags[1].ti_Data = skipb;
    tags[2].ti_Tag = TAG_DONE;
    if (!(m = MpvOpen(tags))) {
        sprintf(line, "    %s: MpvOpen failed", label);
        sayf(line);
        return;
    }
    t0 = now_us();
    MpvWrite(m, (CONST UBYTE *)data, (LONG)len);
    MpvControl(m, MPVC_END, 0);
    for (;;) {
        if (!(p = MpvDecode(m, &e))) {
            if (e == MPVE_MORE && ++more < 1000)
                continue;
            break;
        }
        w = p->mpic_Width;
        h = p->mpic_Height;
        kinds[p->mpic_Type & 3]++;
        n++;
        if ((n & 7) == 0 && stop())
            break;
    }
    us = now_us() - t0;
    if (n) {
        fmt_ms(t, us / n);
        sprintf(line, "    %-22s %ld pictures %ldx%ld (I %ld, P %ld, B %ld): %s a picture, %ld.%ld a second",
                label, (long)n, (long)w, (long)h, (long)kinds[1], (long)kinds[2], (long)kinds[3], t,
                (long)(us ? (unsigned long long)n * 1000000 / us : 0),
                (long)(us ? (unsigned long long)n * 10000000 / us % 10 : 0));
    } else {
        sprintf(line, "    %s: no pictures (error %ld)", label, (long)e);
    }
    sayf(line);
    MpvClose(m);
}

static void bench_video(const char *const *files)
{
    char line[200];
    int i;
    heading("VIDEO: mpegv.library (PROGDIR:Libs), every picture decoded (no output)");
    expunge_lib("mpegv.library");
    if (!(MpegvBase = OpenLibrary((CONST_STRPTR)"PROGDIR:Libs/mpegv.library", 2))) {
        sayf("  PROGDIR:Libs/mpegv.library: not opened");
        return;
    }
    lib_line(MpegvBase, "PROGDIR:Libs/mpegv.library");
    for (i = 0; files[i] && !stop(); i++) {
        ULONG n, max = AvailMem(MEMF_LARGEST) / 2;
        UBYTE *d;
        if (max > (6UL << 20))
            max = 6UL << 20;            /* (a DVD's VOB: its first 6 MB, some 8 s) */
        d = load(files[i], &n, max);
        if (!d) {
            sprintf(line, "  %.100s: not read", files[i]);
            sayf(line);
            continue;
        }
        sprintf(line, "  %.100s, %ld bytes", files[i], (long)n);
        sayf(line);
        video_one("every picture", d, n, FALSE, FALSE);
        if (!brk)
            video_one("B pictures passed over", d, n, FALSE, TRUE);
        if (!brk)
            video_one("half size", d, n, TRUE, FALSE);
        FreeVec(d);
    }
    CloseLibrary(MpegvBase);
    MpegvBase = NULL;
}

/* ------------------------------------------------------------ main */
static ULONG run(void *unused)
{
    static const char tmpl[] = "ONLY/K,MP3/K,VIDEO/M,QUICK/S";
    static const char *const def_video[] = { "PROGDIR:Bench/pal.m2v", "PROGDIR:Bench/ps.mpg", NULL };
    LONG args[4] = { 0, 0, 0, 0 };
    struct RDArgs *ra = ReadArgs((CONST_STRPTR)tmpl, args, NULL);
    const char *only = NULL, *mp3 = "PROGDIR:Bench/g128.mp3";
    const char *const *video = def_video;
    struct DateTime dt;
    char date[LEN_DATSTRING], clock[LEN_DATSTRING], line[160];
    BPTR fh;
    (void)unused;
    if (!ra) {
        PutStr((CONST_STRPTR)"ambbench [ONLY=MEM|C2P|PENS|MP3|VIDEO] [MP3=<file>] [VIDEO=<file>...] [QUICK]\n");
        return 20;
    }
    only = (const char *)args[0];
    if (args[1])
        mp3 = (const char *)args[1];
    if (args[2] && ((const char **)args[2])[0])
        video = (const char *const *)args[2];
    quick = args[3] != 0;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&treq, 0)) {
        PutStr((CONST_STRPTR)"ambbench: timer.device did not open\n");
        FreeArgs(ra);
        return 20;
    }
    TimerBase = treq.tr_node.io_Device;
    {
        struct EClockVal e;
        efreq = ReadEClock(&e);
    }
    out = AllocVec(outmax, MEMF_ANY);
    DateStamp(&dt.dat_Stamp);
    dt.dat_Format = FORMAT_INT;
    dt.dat_Flags = 0;
    dt.dat_StrDay = NULL;
    dt.dat_StrDate = (STRPTR)date;
    dt.dat_StrTime = (STRPTR)clock;
    date[0] = clock[0] = 0;
    DateToStr(&dt);
    sprintf(line, "======== ambbench 1.1, %s %s%s ========", date, clock, quick ? " (QUICK)" : "");
    sayf(line);
    machine();
    if (!only || same(only, "MEM"))
        bench_mem();
    if (!brk && (!only || same(only, "C2P")))
        bench_c2p();
    if (!brk && (!only || same(only, "PENS")))
        bench_pens();
    if (!brk && (!only || same(only, "MP3")))
        bench_mp3(mp3);
    if (!brk && (!only || same(only, "VIDEO")))
        bench_video(video);
    if (out && (fh = Open((CONST_STRPTR)"PROGDIR:ambbench.txt", MODE_READWRITE))) {
        Seek(fh, 0, OFFSET_END);
        Write(fh, out, outn);
        Write(fh, (APTR)"\n", 1);
        Close(fh);
        PutStr((CONST_STRPTR)"\nAdded to ambbench.txt beside the program\n");
    }
    FreeVec(out);
    CloseDevice((struct IORequest *)&treq);
    FreeArgs(ra);
    return 0;
}

int main(void)
{
    return (int)stack_call(65536, run, NULL);
}
