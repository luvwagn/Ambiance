/*
 * AmigaMCE - opening its own libraries (Libs: amcedvd, ac3, ac3fpu, mpegv)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Exec finds a library already in memory by its name, whatever path is
 * asked for: a library AmigaMCE opened before, since closed but not yet
 * put out of memory, would be opened again in place of a newer file in
 * AmigaMCE's Libs (a new build's fixes not there until a reboot or Avail
 * FLUSH). So a copy in memory that nothing has open is put out first (its
 * expunge, as Avail FLUSH does), and the one opened is logged.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <stdio.h>
#include "amce.h"

extern struct ExecBase *SysBase;

struct Library *amce_open_own_lib(const char *name, ULONG version)
{
    struct Library *l;
    char path[80], m[160];
    BOOL flushed = FALSE;
    Forbid();
    l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)name);
    if (l && l->lib_OpenCnt == 0) {
        RemLibrary(l);                      /* (its expunge: out of memory if it can be) */
        flushed = TRUE;
    }
    Permit();
    sprintf(path, "PROGDIR:Libs/%s", name);
    if (!(l = OpenLibrary((CONST_STRPTR)path, version)))
        l = OpenLibrary((CONST_STRPTR)name, version);
    if (l) {
        sprintf(m, "%s %ld.%ld%s", name, (long)l->lib_Version, (long)l->lib_Revision,
                flushed ? " (an older copy in memory put out first)" : "");
        amce_log("library", m);
    }
    return l;
}
