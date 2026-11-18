/*
 * Ambiance - debug overlay measurements: CPU load and the app's memory
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
 * every point where Ambiance sleeps (Wait, WaitTOF), and the time spent
 * outside those is Ambiance's own work.
 *
 * Memory. Ambiance's own use is its code (the loaded segments) plus the
 * memory that has left the free pool since it started, chip shown apart.
 * Other programs allocating or freeing meanwhile shift the figure, so it
 * is a close estimate, not an exact count.
 */
#include <stdio.h>
#include <string.h>
#include "amb.h"

#include <exec/tasks.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <exec/execbase.h>
#include <graphics/gfxbase.h>
#include <proto/graphics.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>

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

/* ---- which tasks have the CPU: a sampler on the vertical blank
 * Each frame's interrupt notes the task it interrupted (exec's ThisTask);
 * over five seconds that is some 250 samples, a share each. Work done in
 * interrupts (a sound card's, the blitter's) is not a task's: it falls to
 * whichever task it interrupted (mostly the idle meter), so it shows as
 * the system's load less the tasks' shares. A task woken by the vertical
 * blank itself (WaitTOF) runs just after the sample: its share reads low
 * (Ambiance's own is measured apart, by the clock). */
#define PROF_N 32
static struct Interrupt prof_int;
static BOOL prof_on;
static struct Task *volatile prof_t[PROF_N];
static volatile ULONG prof_c[PROF_N], prof_total, prof_other;

static ULONG prof_code(void)
{
    struct Task *t = SysBase->ThisTask;
    int i;
    prof_total++;
    for (i = 0; i < PROF_N; i++) {
        if (prof_t[i] == t) {
            prof_c[i]++;
            return 0;
        }
        if (!prof_t[i]) {
            prof_t[i] = t;
            prof_c[i] = 1;
            return 0;
        }
    }
    prof_other++;
    return 0;
}

static void prof_start(void)
{
    int i;
    if (prof_on)
        return;
    for (i = 0; i < PROF_N; i++) {
        prof_t[i] = NULL;
        prof_c[i] = 0;
    }
    prof_total = prof_other = 0;
    prof_int.is_Node.ln_Type = NT_INTERRUPT;
    prof_int.is_Node.ln_Pri = -60;
    prof_int.is_Node.ln_Name = (char *)"Ambiance task sampler";
    prof_int.is_Data = NULL;
    prof_int.is_Code = (void (*)())prof_code;
    AddIntServer(INTB_VERTB, &prof_int);
    prof_on = TRUE;
}

static void prof_stop(void)
{
    if (!prof_on)
        return;
    RemIntServer(INTB_VERTB, &prof_int);
    prof_on = FALSE;
}

/* t still a task (in exec's lists, or running): its name into out */
static BOOL prof_name(struct Task *t, char *out, LONG max)
{
    struct Node *n;
    struct List *ls[2];
    int k;
    BOOL found = t == SysBase->ThisTask;
    ls[0] = &SysBase->TaskReady;
    ls[1] = &SysBase->TaskWait;
    for (k = 0; k < 2 && !found; k++)
        for (n = ls[k]->lh_Head; n->ln_Succ && !found; n = n->ln_Succ)
            if ((struct Task *)n == t)
                found = TRUE;
    if (!found)
        return FALSE;
    out[0] = 0;
    if (t->tc_Node.ln_Type == NT_PROCESS && ((struct Process *)t)->pr_CLI) {
        struct CommandLineInterface *cli = BADDR(((struct Process *)t)->pr_CLI);
        UBYTE *b = cli ? (UBYTE *)BADDR(cli->cli_CommandName) : NULL;
        if (b && b[0]) {
            LONG len = b[0] < max - 1 ? b[0] : max - 1;
            memcpy(out, b + 1, len);
            out[len] = 0;
        }
    }
    if (!out[0]) {
        const char *nm = t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "?";
        strncpy(out, nm, max - 1);
        out[max - 1] = 0;
    }
    return TRUE;
}

/* The tasks' shares since the last call, the busiest first, into out ("" if
 * none); *busy: their sum, in percent (the idle meter apart) */
BOOL dbg_tasks_report(char *out, LONG max, LONG *busy)
{
    struct { ULONG c; char name[32]; } e[PROF_N];
    ULONG c[PROF_N], total, other;
    struct Task *t[PROF_N];
    int i, j, n = 0;
    LONG sum = 0;
    out[0] = 0;
    *busy = 0;
    if (!prof_on)
        return FALSE;
    Disable();
    for (i = 0; i < PROF_N; i++) {
        t[i] = prof_t[i];
        c[i] = prof_c[i];
        prof_t[i] = NULL;
        prof_c[i] = 0;
    }
    total = prof_total;
    other = prof_other;
    prof_total = prof_other = 0;
    Enable();
    if (!total)
        return FALSE;
    Forbid();
    for (i = 0; i < PROF_N; i++) {
        if (!t[i] || !c[i] || t[i] == idle_task)
            continue;
        if (!prof_name(t[i], e[n].name, sizeof(e[n].name)))
            strcpy(e[n].name, "(gone)");
        e[n].c = c[i];
        n++;
    }
    Permit();
    for (i = 1; i < n; i++)                 /* the busiest first */
        for (j = i; j > 0 && e[j].c > e[j - 1].c; j--) {
            ULONG tc = e[j].c;
            char tn[32];
            strcpy(tn, e[j].name);
            e[j].c = e[j - 1].c;
            strcpy(e[j].name, e[j - 1].name);
            e[j - 1].c = tc;
            strcpy(e[j - 1].name, tn);
        }
    sprintf(out, "%lu samples:", (unsigned long)total);
    for (i = 0; i < n; i++) {
        LONG pc10 = (LONG)(e[i].c * 1000 / total);
        char one[60];
        sum += pc10;
        if (i < 8 && pc10 >= 5) {
            sprintf(one, " %s %ld.%ld%%,", e[i].name, (long)(pc10 / 10), (long)(pc10 % 10));
            if ((LONG)(strlen(out) + strlen(one)) < max - 1)
                strcat(out, one);
        }
    }
    if (other) {
        char one[40];
        sprintf(one, " (%lu more)", (unsigned long)other);
        if ((LONG)(strlen(out) + strlen(one)) < max - 1)
            strcat(out, one);
    }
    *busy = sum / 10;
    return TRUE;
}

BOOL dbg_cpu_start(void)
{
    if (idle_task)
        return TRUE;
    prof_start();
    if ((done_sig = AllocSignal(-1)) < 0)
        return FALSE;
    parent = FindTask(NULL);
    idle_quit = FALSE;
    idle_count = last_count = 0;
    idle_task = CreateTask((CONST_STRPTR)"Ambiance idle meter", -128, (APTR)idle_entry, 4096);
    if (!idle_task) {
        FreeSignal(done_sig);
        done_sig = -1;
        return FALSE;
    }
    return TRUE;
}

void dbg_cpu_stop(void)
{
    prof_stop();
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

/* The E-clock's ticks (its low long: differences of up to an hour or so
 * are right), its rate at *hz: for measuring short work. 0 without a
 * timer. */
ULONG time_ticks(ULONG *hz)
{
    struct EClockVal ev;
    if (!TimerBase) {
        *hz = 0;
        return 0;
    }
    *hz = ReadEClock(&ev);
    return ev.ev_lo;
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

/* Percent of the time since the last call that Ambiance was not asleep,
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

/* What was in memory at the baseline: the libraries' and devices' names
 * (those still there at quit, not there then, are what Ambiance brought
 * in and the system kept), and how much RAM: held (ENV: is there). */
#define SNAP_MAX 200
static char snap[SNAP_MAX][28];
static int snap_n;
static LONG ram0 = -1;

static void snap_take(struct List *l)
{
    struct Node *n;
    for (n = l->lh_Head; n->ln_Succ && snap_n < SNAP_MAX; n = n->ln_Succ)
        amb_strlcpy(snap[snap_n++], n->ln_Name ? n->ln_Name : "", sizeof(snap[0]));
}

static BOOL snap_has(const char *name)
{
    int i;
    for (i = 0; i < snap_n; i++)
        if (!strncmp(snap[i], name, sizeof(snap[0]) - 1))
            return TRUE;
    return FALSE;
}

/* RAM:'s bytes in use (-1 not known) */
static LONG ram_used(void)
{
    struct InfoData *id = AllocVec(sizeof(*id), MEMF_ANY | MEMF_CLEAR);
    BPTR l = Lock((CONST_STRPTR)"RAM:", SHARED_LOCK);
    LONG r = -1;
    if (id && l && Info(l, id))
        r = id->id_NumBlocksUsed * id->id_BytesPerBlock;
    if (l)
        UnLock(l);
    if (id)
        FreeVec(id);
    return r;
}

void dbg_mem_baseline(void)
{
    struct ExecBase *eb = SysBase;
    base_chip = AvailMem(MEMF_CHIP);
    base_fast = AvailMem(MEMF_FAST);
    snap_n = 0;
    Forbid();
    snap_take(&eb->LibList);
    snap_take(&eb->DeviceList);
    snap_take(&GfxBase->TextFonts);
    Permit();
    ram0 = ram_used();
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

/* Quitting, everything let go: the memory not back since the baseline, into
 * the log, with the libraries, devices and fonts loaded meanwhile that the
 * system keeps (their memory is in the figure: they stay for the next
 * start, which then finds them there). */
void dbg_mem_report(void)
{
    char m[120];
    LONG dc = (LONG)base_chip - (LONG)AvailMem(MEMF_CHIP);
    LONG df = (LONG)base_fast - (LONG)AvailMem(MEMF_FAST);
    char kept[600];
    struct ExecBase *eb = SysBase;
    struct List *ls[3];
    struct Node *n;
    LONG ram1 = ram_used(), sum = 0;
    int k;
    sprintf(m, "%ld bytes of chip RAM and %ld of fast not back since the start (the log's own "
               "file still open)", (long)dc, (long)df);
    amb_log("memory", m);
    /* where it may be: libraries and devices brought in and kept, RAM: */
    ls[0] = &eb->LibList;
    ls[1] = &eb->DeviceList;
    ls[2] = &GfxBase->TextFonts;
    kept[0] = 0;
    Forbid();
    for (k = 0; k < 3; k++)
        for (n = ls[k]->lh_Head; n->ln_Succ; n = n->ln_Succ) {
            if (!n->ln_Name || snap_has(n->ln_Name) || strlen(kept) > sizeof(kept) - 60)
                continue;
            if (k < 2) {
                struct Library *lib = (struct Library *)n;
                sprintf(kept + strlen(kept), "%s%.30s (open %ld, base %lu)", kept[0] ? ", " : "",
                        n->ln_Name, (long)lib->lib_OpenCnt,
                        (unsigned long)lib->lib_NegSize + lib->lib_PosSize);
            } else {
                struct TextFont *f = (struct TextFont *)n;
                sprintf(kept + strlen(kept), "%sfont %.30s %ld (open %ld)", kept[0] ? ", " : "",
                        n->ln_Name, (long)f->tf_YSize, (long)f->tf_Accessors);
            }
            sum++;
        }
    Permit();
    amb_log("memory: in memory now, not at the start", sum ? kept : "nothing");
    if (ram0 >= 0 && ram1 >= 0) {
        sprintf(m, "RAM: (ENV:, T:) holds %ld bytes more than at the start", (long)(ram1 - ram0));
        amb_log("memory", m);
    }
}

/* Bytes Ambiance is using: total, and how much of it is chip RAM. */
ULONG dbg_mem_used(ULONG *chip)
{
    ULONG c = AvailMem(MEMF_CHIP), f = AvailMem(MEMF_FAST);
    ULONG dc = base_chip > c ? base_chip - c : 0;
    ULONG df = base_fast > f ? base_fast - f : 0;
    if (chip)
        *chip = dc;
    return code_bytes() + dc + df;
}
