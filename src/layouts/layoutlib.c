/*
 * Ambiance - what every layout library is around its layout (layout.h)
 * Copyright (c) 2026 Brian King
 *
 * Built once per layout with src/sqlib/libinit.c's library skeleton and
 * the layout's own source, which defines layout_engine and these:
 *   LAYOUT_FILE  its file's (and library's) name, "showcase.layout"
 *   LAYOUT_ID    its id string, "showcase.layout 1.0 (17.11.2026)"
 * The layout draws only through the host's table: no C library here.
 */
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include "stabs.h"
#include "../layout.h"

extern const struct LayoutEngine layout_engine;     /* the layout's own */

struct ExecBase *SysBase;

int __UserLibInit(struct Library *lib)
{
    (void)lib;
    SysBase = *(struct ExecBase *volatile *)4L;
    return 0;
}

void __UserLibCleanup(void)
{
}

/* The layout's table, only for a program built against the same
 * layout.h. */
const struct LayoutEngine *GetLayout(ULONG abi)
{
    if (abi != (ULONG)LAYOUT_ABI || layout_engine.abi != (ULONG)LAYOUT_ABI)
        return NULL;
    return &layout_engine;
}

ADDTABL_1(GetLayout, d0);
ADDTABL_END();
