/*
 * Ambiance - the CPU it needs, asked first
 * Copyright (c) 2026 Brian King
 *
 * Ambiance is built for a 68020 or better (-m68020-60): 32-bit multiplies
 * and divides in one instruction each, not a library call. This file
 * alone is built for the 68000 (Makefile), so on a 68000 or 68010 it can
 * still say so in a requester, before any code that would not run there.
 * The chipsets stay as they were: OCS, ECS and AGA, with or without RTG.
 */
#include <exec/execbase.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

int amb_cpu_ok(void);

int amb_cpu_ok(void)
{
    struct IntuitionBase *IntuitionBase;
    if (SysBase->AttnFlags & AFF_68020)
        return 1;
    if ((IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library",
                                                             36))) {
        struct EasyStruct es = {
            sizeof(struct EasyStruct), 0, (UBYTE *)"Ambiance",
            (UBYTE *)"Ambiance needs a 68020 or better.\nThis Amiga has a 68000 or 68010.",
            (UBYTE *)"OK"
        };
        EasyRequestArgs(NULL, &es, NULL, NULL);
        CloseLibrary((struct Library *)IntuitionBase);
    }
    return 0;
}
