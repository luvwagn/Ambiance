/*
 * AmigaMCE - reading a folder's entries quickly
 * Copyright (c) 2026 Brian King
 *
 * ExNext hands over one entry per call, each a round trip to the file
 * system's task, and AGS folders hold five or so files per game. ExAll
 * hands over a buffer full at a time (PFS3 and FFS do it natively; dos
 * emulates it for file systems that do not), so it is used, with ExNext
 * as the fallback should ExAll fail before giving anything.
 */
#include "amce.h"

#include <dos/exall.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define EXALL_BUF 4096

static LONG by_exnext(BPTR lock, DirFn fn, void *ctx)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    LONG n = 0;
    if (!fib)
        return -1;
    if (Examine(lock, fib))
        while (ExNext(lock, fib)) {
            fn(ctx, (const char *)fib->fib_FileName, fib->fib_DirEntryType > 0);
            n++;
        }
    FreeDosObject(DOS_FIB, fib);
    return n;
}

LONG dir_scan(const char *dir, DirFn fn, void *ctx)
{
    struct ExAllControl *eac;
    struct ExAllData *buf;
    BPTR lock = Lock((CONST_STRPTR)dir, SHARED_LOCK);
    LONG n = 0;
    BOOL more;
    if (!lock)
        return -1;
    eac = AllocDosObject(DOS_EXALLCONTROL, NULL);
    buf = AllocVec(EXALL_BUF, MEMF_ANY);
    if (!eac || !buf) {
        if (eac)
            FreeDosObject(DOS_EXALLCONTROL, eac);
        FreeVec(buf);
        n = by_exnext(lock, fn, ctx);
        UnLock(lock);
        return n;
    }
    eac->eac_LastKey = 0;
    eac->eac_MatchString = NULL;
    eac->eac_MatchFunc = NULL;
    do {
        struct ExAllData *ed;
        more = ExAll(lock, buf, EXALL_BUF, ED_TYPE, eac);
        if (!more && IoErr() != ERROR_NO_MORE_ENTRIES) {
            if (n == 0 && eac->eac_Entries == 0) {
                /* ExAll not to be had here: one by one, then */
                FreeDosObject(DOS_EXALLCONTROL, eac);
                FreeVec(buf);
                n = by_exnext(lock, fn, ctx);
                UnLock(lock);
                return n;
            }
            break;                          /* an error part-way: what we have */
        }
        if (eac->eac_Entries)
            for (ed = buf; ed; ed = ed->ed_Next) {
                fn(ctx, (const char *)ed->ed_Name, ed->ed_Type > 0);
                n++;
            }
    } while (more);
    FreeDosObject(DOS_EXALLCONTROL, eac);
    FreeVec(buf);
    UnLock(lock);
    return n;
}
