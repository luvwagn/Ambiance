/*
 * Ambiance - the program's side of the layout engines: finding, opening
 * and closing the layout libraries (lhost.h, layout.h)
 * Copyright (c) 2026 Brian King
 *
 * A layout is PROGDIR:Layouts/<name>.layout, an Exec library whose name
 * is its file's; its one function, GetLayout(abi), gives its table only
 * to a program built against the same layout.h (LAYOUT_ABI). Opened only
 * while it is in use, closed and put out of memory after (as
 * amce_open_own_lib's libraries are), so a new build's file is never
 * shadowed by an old copy still in memory.
 */
#include "amce.h"
#include "lhost.h"

#include <string.h>
#include <stdio.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>


#ifdef AMCE_NO_EXPUNGE                      /* (the tests: vamos opens a library put out of memory once only) */
#undef RemLibrary
#define RemLibrary(l) ((void)(l))
#endif

extern struct ExecBase *SysBase;

#ifndef LAYOUTS_DIR
#define LAYOUTS_DIR "PROGDIR:Layouts"   /* (the tests': "Layouts", vamos locks no PROGDIR:) */
#endif
#ifndef LAYOUT_EXT
#define LAYOUT_EXT ".layout"    /* (the tests': ".library", the only names vamos opens) */
#endif

static struct Library *LayoutBase;
static const struct LayoutEngine *engine;

/* the library's one function (bias 30) */
#define GetLayout(abi) \
    LP1(0x1e, const struct LayoutEngine *, GetLayout, ULONG, abi, d0, , LayoutBase)

/* file opened (an unused copy in memory put out first), its table asked
 * for; the library left open in LayoutBase, or NULL */
static const struct LayoutEngine *open_one(const char *file, BOOL quiet)
{
    struct Library *l;
    const struct LayoutEngine *e;
    char path[64], m[160];
    Forbid();
    if ((l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)file)) && l->lib_OpenCnt == 0)
        RemLibrary(l);
    Permit();
    snprintf(path, sizeof(path), LAYOUTS_DIR "/%s", file);
    if (!(LayoutBase = OpenLibrary((CONST_STRPTR)path, 1))) {
        if (!quiet) {
            snprintf(m, sizeof(m), "%s would not open (error %ld)", path, (long)IoErr());
            amce_log("layout", m);
        }
        return NULL;
    }
    e = GetLayout((ULONG)LAYOUT_ABI);
    if (!e || e->abi != (ULONG)LAYOUT_ABI) {
        snprintf(m, sizeof(m), "%s %ld.%ld is for another Ambiance (its layout.h differs): not used",
                 file, (long)LayoutBase->lib_Version, (long)LayoutBase->lib_Revision);
        amce_log("layout", m);
        layout_unload();
        return NULL;
    }
    if (!quiet) {
        snprintf(m, sizeof(m), "%s %ld.%ld: %s", file, (long)LayoutBase->lib_Version,
                 (long)LayoutBase->lib_Revision, e->name);
        amce_log("layout", m);
    }
    return e;
}

void layout_unload(void)
{
    struct Library *l = LayoutBase;
    char name[40];
    engine = NULL;
    if (!l)
        return;
    strncpy(name, (const char *)l->lib_Node.ln_Name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    Forbid();
    CloseLibrary(l);
    if ((l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)name)) && l->lib_OpenCnt == 0)
        RemLibrary(l);                      /* (out of memory now, not when it is wanted) */
    Permit();
    LayoutBase = NULL;
}

const struct LayoutEngine *layout_load(const char *file)
{
    if (LayoutBase)
        layout_unload();
    return engine = open_one(file, FALSE);
}

const struct LayoutEngine *layout_loaded(void)
{
    return engine;
}

int layout_list(struct LayoutInfo *out, int max)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int n = 0;
    struct Library *keep = LayoutBase;
    const struct LayoutEngine *keep_e = engine;
    if (!(lock = Lock((CONST_STRPTR)LAYOUTS_DIR, ACCESS_READ)))
        return 0;
    if (!(fib = AllocDosObject(DOS_FIB, NULL))) {
        UnLock(lock);
        return 0;
    }
    LayoutBase = NULL;                      /* (the one in use kept aside meanwhile) */
    if (Examine(lock, fib))
        while (n < max && ExNext(lock, fib)) {
            const char *f = (const char *)fib->fib_FileName;
            size_t len = strlen(f);
            const struct LayoutEngine *e;
            if (fib->fib_DirEntryType >= 0 || len <= sizeof(LAYOUT_EXT) - 1 || len >= sizeof(out[n].file) ||
                amce_stricmp(f + len - (sizeof(LAYOUT_EXT) - 1), LAYOUT_EXT))
                continue;
            if (keep && !amce_stricmp(f, (const char *)keep->lib_Node.ln_Name)) {
                e = keep_e;                 /* the one open: as it is */
            } else {
                if (!(e = open_one(f, TRUE)))
                    continue;
            }
            if (e) {
                strcpy(out[n].file, f);
                strncpy(out[n].name, e->name, sizeof(out[n].name) - 1);
                out[n].name[sizeof(out[n].name) - 1] = 0;
                out[n].min_bits = e->min_bits;
                out[n].min_h = e->min_h;
                n++;
            }
            if (LayoutBase)
                layout_unload();
        }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    LayoutBase = keep;
    engine = keep_e;
    return n;
}
