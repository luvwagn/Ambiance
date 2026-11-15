/*
 * AmigaMCE - opening its own libraries (Libs: amcedvd, ac3, ac3fpu, mpegv)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Exec finds a library already in memory by its name, whatever path is
 * asked for: a library AmigaMCE opened before, since closed but not yet
 * put out of memory, would be opened again in place of a newer file in
 * AmigaMCE's Libs (a new build's fixes not there until a reboot or Avail
 * FLUSH). So a copy in memory that nothing has open is put out first (its
 * expunge, as Avail FLUSH does), and the one opened is logged. Closed, it
 * is put out at once if nothing else has it open: Exec would keep it in
 * memory till memory was wanted (a delayed expunge), amcesql.library's
 * 574 KB among them. (Built for vamos's tests with AMCE_NO_EXPUNGE: vamos
 * cannot open a native library again once it has been put out.)
 */
#include <string.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <stdio.h>
#include "amce.h"

#ifdef AMCE_NO_EXPUNGE
#define RemLibrary(l) ((void)(l))
#endif

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

/* One no longer open (closed, kept in memory meanwhile) put out of it now:
 * at quitting, for one opened and closed again and again (amcesql.library,
 * an album's tracks each time: kept between, not loaded again). */
void amce_flush_own_lib(const char *name)
{
    struct Library *l;
    Forbid();
    if ((l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)name)) && l->lib_OpenCnt == 0)
        RemLibrary(l);
    Permit();
}

void amce_close_own_lib(struct Library **base)
{
    struct Library *l = *base;
    char name[48];
    if (!l)
        return;
    strncpy(name, (const char *)l->lib_Node.ln_Name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    Forbid();
    CloseLibrary(l);
    if ((l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)name)) && l->lib_OpenCnt == 0)
        RemLibrary(l);                      /* (out of memory now, not when it is wanted) */
    Permit();
    *base = NULL;
}
