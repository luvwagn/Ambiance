/*
 * AmigaMCE - opt-in timing log for diagnosing slow starts and exits
 * Copyright (c) 2026 Brian King
 *
 * Off unless a file named AmigaMCE.debug sits next to the program. Then
 * each step is appended to PROGDIR:AmigaMCE.log with a 1/50 s timestamp
 * and flushed at once, so the log survives a hang.
 */
#include "amce.h"

#include <stdio.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

static BPTR logfh;

void amce_log_open(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    BPTR l;
    me->pr_WindowPtr = (APTR)-1;
    l = Lock((CONST_STRPTR)"PROGDIR:AmigaMCE.debug", SHARED_LOCK);
    if (l) {
        UnLock(l);
        logfh = Open((CONST_STRPTR)"PROGDIR:AmigaMCE.log", MODE_READWRITE);
        if (logfh)
            Seek(logfh, 0, OFFSET_END);
    }
    me->pr_WindowPtr = oldwin;
    amce_log("---- AmigaMCE " AMCE_VERSION, "");
}

void amce_log(const char *what, const char *detail)
{
    struct DateStamp ds;
    char line[200];
    if (!logfh)
        return;
    DateStamp(&ds);
    /* amiga.lib sprintf is RawDoFmt: every number is %l with a long */
    sprintf(line, "%02ld:%02ld:%02ld.%02ld  %s%s%s\n",
            (long)ds.ds_Minute / 60, (long)ds.ds_Minute % 60, (long)ds.ds_Tick / 50,
            (long)(ds.ds_Tick % 50) * 2, what, detail[0] ? ": " : "", detail);
    FPuts(logfh, (CONST_STRPTR)line);
    Flush(logfh);
}

void amce_log_close(void)
{
    if (logfh) {
        amce_log("log closed", "");
        Close(logfh);
        logfh = 0;
    }
}
