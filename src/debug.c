/*
 * AmigaMCE - debug overlay measurements: CPU load and the app's memory
 * Copyright (c) 2026 Brian King
 *
 * CPU load. AmigaOS 3.x keeps no per-task CPU time, so this measures the
 * whole system the classic way: a task at the lowest priority (-128)
 * counts in a loop and only ever runs when nothing else wants the CPU.
 * Load is how far its count per second falls below the highest count seen
 * so far (the self-calibrating "fully idle" rate). AmigaMCE is the
 * foreground program, so it is most of what this shows. The task runs
 * only while the overlay is on, and is stopped while a game runs.
 *
 * Memory. AmigaMCE's own use is its code (the loaded segments) plus the
 * memory that has left the free pool since it started, chip shown apart.
 * Other programs allocating or freeing meanwhile shift the figure, so it
 * is a close estimate, not an exact count.
 */
#include "amce.h"

#include <exec/tasks.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>

static volatile ULONG idle_count;
static volatile BOOL idle_quit;
static struct Task *idle_task, *parent;
static BYTE done_sig = -1;
static ULONG last_count, best_rate;
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
    ULONG now = idle_count, rate = now - last_count;
    BOOL first = last_count == 0;
    last_count = now;
    if (!idle_task || first)
        return -1;
    if (rate > best_rate)
        best_rate = rate;
    if (!best_rate)
        return 100;
    return 100 - (LONG)((rate / 64) * 100 / (best_rate / 64 + 1));
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
