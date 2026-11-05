/*
 * AmigaMCE - opt-in timing log for diagnosing slow starts and exits
 * Copyright (c) 2026 Brian King
 *
 * Off unless Debug is on in Settings or a file named AmigaMCE.debug sits
 * next to the program. Then each step is appended to AmigaMCE.log with a
 * 1/50 s timestamp and flushed at once, so the log survives a hang.
 */
#include "amce.h"

#include <stdio.h>
#include <dos/dos.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include <proto/dos.h>

static BPTR logfh;
static const char *logpath;
static struct SignalSemaphore loglock;  /* the radio process logs too */
static BOOL lock_ready;

/* Opens the log when Debug is on in Settings (force) or the file
 * AmigaMCE.debug sits next to the program. It goes to PROGDIR, or to RAM:
 * when the program's drawer cannot be written to (a read-only drive). */
void amce_log_open(BOOL force)
{
    static const char *paths[] = { "PROGDIR:AmigaMCE.log", "RAM:AmigaMCE.log", NULL };
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    BPTR l;
    int i;
    if (logfh)
        return;
    if (!lock_ready) {
        InitSemaphore(&loglock);
        lock_ready = TRUE;
    }
    me->pr_WindowPtr = (APTR)-1;
    if (!force && (l = Lock((CONST_STRPTR)"PROGDIR:AmigaMCE.debug", SHARED_LOCK))) {
        UnLock(l);
        force = TRUE;
    }
    for (i = 0; force && paths[i] && !logfh; i++)
        if ((logfh = Open((CONST_STRPTR)paths[i], MODE_READWRITE))) {
            Seek(logfh, 0, OFFSET_END);
            logpath = paths[i];
        }
    me->pr_WindowPtr = oldwin;
    amce_log("---- AmigaMCE " AMCE_VERSION, "");
}

const char *amce_log_path(void)
{
    return logfh ? logpath : NULL;
}

void amce_log(const char *what, const char *detail)
{
    struct DateStamp ds;
    char stamp[48];
    if (!logfh || !lock_ready)
        return;
    ObtainSemaphore(&loglock);
    DateStamp(&ds);
    /* amiga.lib sprintf is RawDoFmt: every number is %l with a long. Only
     * the time stamp is formatted into a buffer; the text is written as it
     * is, however long (a line once overran a 200-byte buffer here and
     * crashed AmigaMCE with #80000003). */
    sprintf(stamp, "%02ld:%02ld:%02ld.%02ld  ", (long)ds.ds_Minute / 60, (long)ds.ds_Minute % 60,
            (long)ds.ds_Tick / 50, (long)(ds.ds_Tick % 50) * 2);
    FPuts(logfh, (CONST_STRPTR)stamp);
    FPuts(logfh, (CONST_STRPTR)what);
    if (detail[0]) {
        FPuts(logfh, (CONST_STRPTR)": ");
        FPuts(logfh, (CONST_STRPTR)detail);
    }
    FPuts(logfh, (CONST_STRPTR)"\n");
    Flush(logfh);
    ReleaseSemaphore(&loglock);
}

void amce_log_close(void)
{
    if (logfh) {
        amce_log("log closed", "");
        Close(logfh);
        logfh = 0;
    }
}
