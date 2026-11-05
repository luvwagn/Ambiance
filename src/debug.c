/*
 * AmigaMCE - debug overlay measurements: CPU load and the app's memory
 * Copyright (c) 2026 Brian King
 *
 * CPU load. AmigaOS 3.x keeps no per-task CPU time, so this measures the
 * whole system the classic way: a task at the lowest priority (-128)
 * counts in a loop and only ever runs when nothing else wants the CPU.
 * Load is how far its count per second falls below the highest count seen
 * so far (the self-calibrating "fully idle" rate). The task runs only
 * while the overlay is on, and is stopped while a game runs.
 *
 * That system figure includes anything that slows the machine down, and
 * under an emulator set to "fastest possible" CPU speed, a heavier
 * chipset display (8 bitplanes, hires, interlace) leaves the emulated CPU
 * fewer cycles, which the idle task also reads as load. So the app's own
 * share is measured as well, directly: the timer's E-clock is read around
 * every point where AmigaMCE sleeps (Wait, WaitTOF), and the time spent
 * outside those is AmigaMCE's own work.
 *
 * Memory. AmigaMCE's own use is its code (the loaded segments) plus the
 * memory that has left the free pool since it started, chip shown apart.
 * Other programs allocating or freeing meanwhile shift the figure, so it
 * is a close estimate, not an exact count.
 */
#include "amce.h"

#include <exec/tasks.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>

struct Device *TimerBase;       /* set from the open timer request */

static volatile ULONG idle_count;
static volatile BOOL idle_quit;
static struct Task *idle_task, *parent;
static BYTE done_sig = -1;
static ULONG last_count, best_rate, last_ms;
static ULONG base_chip, base_fast;

static void idle_entry(void)
{
    while (!idle_quit)
        idle_count++;
    /* Signal under Forbid: the parent cannot run (and unload this code)
     * until exec has removed this task. */
    Forbid();
    Signal(parent, 1UL << done_sig);
}

BOOL dbg_cpu_start(void)
{
    if (idle_task)
        return TRUE;
    if ((done_sig = AllocSignal(-1)) < 0)
        return FALSE;
    parent = FindTask(NULL);
    idle_quit = FALSE;
    idle_count = last_count = 0;
    idle_task = CreateTask((CONST_STRPTR)"AmigaMCE idle meter", -128, (APTR)idle_entry, 4096);
    if (!idle_task) {
        FreeSignal(done_sig);
        done_sig = -1;
        return FALSE;
    }
    return TRUE;
}

void dbg_cpu_stop(void)
{
    if (!idle_task)
        return;
    SetSignal(0, 1UL << done_sig);
    idle_quit = TRUE;
    Wait(1UL << done_sig);
    idle_task = NULL;
    FreeSignal(done_sig);
    done_sig = -1;
}

/* Call once a second. Returns 0..100, or -1 until there is a first
 * second to compare with. The calibration survives stop/start. */
LONG dbg_cpu_sample(void)
{
    ULONG now = idle_count, diff = now - last_count, ms = time_ms(), el = ms - last_ms, rate;
    BOOL first = last_count == 0 || !last_ms || !el;
    last_count = now;
    last_ms = ms;
    /* counts per real second: the caller's "second" can run long when the
     * CPU is busy, and a longer one counted more and read as less load */
    rate = first ? 0 : (diff / el) * 1000 + ((diff % el) * 1000) / el;
    if (!idle_task || first)
        return -1;
    if (rate > best_rate)
        best_rate = rate;
    if (!best_rate)
        return 100;
    return 100 - (LONG)((rate / 64) * 100 / (best_rate / 64 + 1));
}

/* ---- the app's own CPU share, from E-clock time spent outside sleeps */
static struct EClockVal idle_from, sample_from;
static ULONG idle_acc;
static BOOL sleeping, sampled;

void dbg_set_timer(struct Device *timer)
{
    TimerBase = timer;
}

/* Milliseconds from the E-clock (wraps after ~49 days; differences are
 * what matter). 0 if the timer is not open. */
ULONG time_ms(void)
{
    struct EClockVal ev;
    ULONG hz, khz;
    if (!TimerBase)
        return 0;
    hz = ReadEClock(&ev);
    khz = hz / 1000;
    if (!khz)
        return 0;
    /* (hi:lo) / khz without 64-bit division: hi * (2^32 / khz) + lo / khz */
    return ev.ev_hi * (0xFFFFFFFFUL / khz) + ev.ev_lo / khz;
}

void dbg_sleep_begin(void)
{
    if (!TimerBase)
        return;
    ReadEClock(&idle_from);
    sleeping = TRUE;
}

void dbg_sleep_end(void)
{
    struct EClockVal now;
    if (!TimerBase || !sleeping)
        return;
    ReadEClock(&now);
    idle_acc += now.ev_lo - idle_from.ev_lo;
    sleeping = FALSE;
}

/* Percent of the time since the last call that AmigaMCE was not asleep,
 * or -1 on the first call. */
LONG dbg_app_sample(void)
{
    struct EClockVal now;
    ULONG span, busy;
    LONG r = -1;
    if (!TimerBase)
        return -1;
    ReadEClock(&now);
    if (sampled) {
        span = now.ev_lo - sample_from.ev_lo;
        busy = span > idle_acc ? span - idle_acc : 0;
        r = span ? (LONG)((busy / 64) * 100 / (span / 64 + 1)) : 0;
    }
    sample_from = now;
    idle_acc = 0;
    sampled = TRUE;
    return r;
}

void dbg_mem_baseline(void)
{
    base_chip = AvailMem(MEMF_CHIP);
    base_fast = AvailMem(MEMF_FAST);
}

static ULONG code_bytes(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    BPTR seg = 0;
    ULONG n = 0;
    if (me->pr_CLI)
        seg = ((struct CommandLineInterface *)BADDR(me->pr_CLI))->cli_Module;
    if (!seg && me->pr_SegList)
        seg = ((BPTR *)BADDR(me->pr_SegList))[3];   /* started from Workbench */
    while (seg) {
        ULONG *p = (ULONG *)BADDR(seg);
        n += p[-1];                 /* allocation size, stored before the link */
        seg = (BPTR)p[0];
    }
    return n;
}

/* Bytes AmigaMCE is using: total, and how much of it is chip RAM. */
ULONG dbg_mem_used(ULONG *chip)
{
    ULONG c = AvailMem(MEMF_CHIP), f = AvailMem(MEMF_FAST);
    ULONG dc = base_chip > c ? base_chip - c : 0;
    ULONG df = base_fast > f ? base_fast - f : 0;
    if (chip)
        *chip = dc;
    return code_bytes() + dc + df;
}
