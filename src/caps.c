/*
 * AmigaMCE - one-time hardware and software capability detection
 * Copyright (c) 2026 Brian King
 */
#include "amce.h"

#include <exec/execbase.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>

extern struct ExecBase *SysBase;
extern struct GfxBase *GfxBase;

static BOOL lib_present(const char *name, ULONG ver)
{
    struct Library *l = OpenLibrary((CONST_STRPTR)name, ver);
    if (!l)
        return FALSE;
    CloseLibrary(l);
    return TRUE;
}

static BOOL path_present(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!l)
        return FALSE;
    UnLock(l);
    return TRUE;
}

void caps_detect(struct Caps *c)
{
    UWORD af = SysBase->AttnFlags;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;

    c->cpu = (af & AFF_68060) ? 60 : (af & AFF_68040) ? 40 : (af & AFF_68030) ? 30 :
             (af & AFF_68020) ? 20 : (af & AFF_68010) ? 10 : 0;
    c->kick = SysBase->LibNode.lib_Version;
    c->aga = (GfxBase->ChipRevBits0 & GFXF_AA_ALICE) != 0;
    c->ecs = (GfxBase->ChipRevBits0 & GFXF_HR_AGNUS) != 0;
    c->pal = (GfxBase->DisplayFlags & PAL) != 0;
    c->ehb = ModeNotAvailable(LORES_KEY | EXTRAHALFBRITE_KEY) == 0;
    c->chipfree = AvailMem(MEMF_CHIP);
    c->fastfree = AvailMem(MEMF_FAST);

    /* No "insert volume" requesters while probing. */
    me->pr_WindowPtr = (APTR)-1;
    c->rtg = lib_present("cybergraphics.library", 40);
    c->net = lib_present("bsdsocket.library", 4);
    c->mp3_mpega = lib_present("mpega.library", 0);
    c->mp3_mhi = path_present("LIBS:MHI");
    me->pr_WindowPtr = oldwin;
}
