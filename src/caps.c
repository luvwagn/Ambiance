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
#include <proto/expansion.h>
#include <libraries/configvars.h>

extern struct ExecBase *SysBase;
extern struct GfxBase *GfxBase;
struct ExpansionBase *ExpansionBase;

/* UAE-family emulators (Amiberry, WinUAE, FS-UAE) autoconfigure their own
 * boards (directory/hardfile boot ROM, UAE RTG, Z3 fast) with the
 * "hackers" manufacturer ID 2011, unless "hide UAE boards" is set; then
 * they still show uaehf.device whenever a hardfile or directory is
 * mounted. Both are read through the OS, never by poking hardware. */
#define UAE_MANUFACTURER 2011

static BOOL emulator_detect(void)
{
    BOOL found = FALSE;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 36);
    if (ExpansionBase) {
        found = FindConfigDev(NULL, UAE_MANUFACTURER, -1) != NULL;
        CloseLibrary((struct Library *)ExpansionBase);
        ExpansionBase = NULL;
    }
    if (!found) {
        Forbid();
        found = FindName(&SysBase->DeviceList, (CONST_STRPTR)"uaehf.device") != NULL;
        Permit();
    }
    return found;
}

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

/* Release family for a system library version (wiki.amigaos.net, AmigaOS
 * Versions). Point releases such as 3.2.3 share the major number, so the
 * exact version.revision is shown beside this name. */
const char *caps_release_name(UWORD v)
{
    switch (v) {
    case 33: return "1.2";
    case 34: return "1.3";
    case 36: return "2.0";
    case 37: return "2.04";
    case 38: return "2.1";
    case 39: return "3.0";
    case 40: return "3.1";
    case 44: return "3.5";
    case 45: return "3.9";
    case 46: return "3.1.4";
    case 47: return "3.2";
    default: return v > 47 ? "3.2+" : "?";
    }
}

void caps_detect(struct Caps *c)
{
    UWORD af = SysBase->AttnFlags;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;

    c->cpu = (af & AFF_68060) ? 60 : (af & AFF_68040) ? 40 : (af & AFF_68030) ? 30 :
             (af & AFF_68020) ? 20 : (af & AFF_68010) ? 10 : 0;
    c->kick = SysBase->LibNode.lib_Version;
    c->kick_rev = SysBase->SoftVer;
    c->aga = (GfxBase->ChipRevBits0 & GFXF_AA_ALICE) != 0;
    c->ecs = (GfxBase->ChipRevBits0 & GFXF_HR_AGNUS) != 0;
    c->pal = (GfxBase->DisplayFlags & PAL) != 0;
    c->ehb = ModeNotAvailable(LORES_KEY | EXTRAHALFBRITE_KEY) == 0;
    c->emulator = emulator_detect();
    c->chipfree = AvailMem(MEMF_CHIP);
    c->fastfree = AvailMem(MEMF_FAST);

    /* No "insert volume" requesters while probing. */
    me->pr_WindowPtr = (APTR)-1;
    c->rtg = lib_present("cybergraphics.library", 40);
    c->net = lib_present("bsdsocket.library", 4);
    c->mp3_mpega = lib_present("mpega.library", 0);
    c->mp3_mhi = path_present("LIBS:MHI");
    {
        struct Library *v = OpenLibrary((CONST_STRPTR)"version.library", 0);
        c->wb = c->wb_rev = 0;
        if (v) {
            c->wb = v->lib_Version;
            c->wb_rev = v->lib_Revision;
            CloseLibrary(v);
        }
    }
    me->pr_WindowPtr = oldwin;
}
