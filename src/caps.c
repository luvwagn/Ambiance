/*
 * Ambiance - one-time hardware and software capability detection
 * Copyright (c) 2026 Brian King
 */
#include "amb.h"

#include <exec/execbase.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/expansion.h>
#include <libraries/configvars.h>
#include <string.h>

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

/* The ZZ9000 (MNT's graphics and coprocessor card): its Zorro II and
 * Zorro III boards, by their autoconfig IDs (the ZZ9000 SDK's abi.h:
 * manufacturer 0x6d6e, products 3 and 4). Amiberry's emulated one shows
 * the same. */
#define ZZ9000_MANUFACTURER 0x6d6e
#define ZZ9000_PRODUCT_Z2   3
#define ZZ9000_PRODUCT_Z3   4

static UBYTE zz9000_detect(void)
{
    UBYTE z = 0;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 36);
    if (ExpansionBase) {
        if (FindConfigDev(NULL, ZZ9000_MANUFACTURER, ZZ9000_PRODUCT_Z3))
            z = 3;
        else if (FindConfigDev(NULL, ZZ9000_MANUFACTURER, ZZ9000_PRODUCT_Z2))
            z = 2;
        CloseLibrary((struct Library *)ExpansionBase);
        ExpansionBase = NULL;
    }
    return z;
}

/* A library whose ID string ($VER) contains what: the ZZ9000 SDK's
 * mpega.library says "ZZ9000 SDK" in its own. */
static BOOL lib_id_has(const char *name, const char *what)
{
    struct Library *l = OpenLibrary((CONST_STRPTR)name, 0);
    BOOL r = FALSE;
    if (l) {
        r = l->lib_IdString && strstr((const char *)l->lib_IdString, what) != NULL;
        CloseLibrary(l);
    }
    return r;
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

/* RTG drivers (Picasso96, PrismRTG, CyberGraphX) add their modes to
 * graphics.library's display database, so no RTG library is needed to find
 * them: native chipset monitors all have IDs up to DblPAL, RTG ones above. */
BOOL caps_mode_is_rtg(ULONG id)
{
    struct DisplayInfo di;
    if ((id & MONITOR_ID_MASK) > DBLPAL_MONITOR_ID)
        return TRUE;
    return GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, id) > 0 &&
           (di.PropertyFlags & DIPF_IS_FOREIGN) != 0;
}

/* Which of a size's RTG modes: 16-bit first (pictures in their own colours,
 * no colour reduction: covers 3.5x and full-screen pictures 2.5x faster
 * than at 8 bits, the interface no slower: rtgbench, 2026-11-07), 15-bit,
 * then 8-bit where the size has nothing deeper. 24 and 32-bit are not used:
 * twice 16-bit's bytes for nothing this interface shows. */
static UWORD depth_rank(UWORD d)
{
    return d == 16 ? 3 : d == 15 ? 2 : d == 8 ? 1 : 0;
}

/* The best available RTG mode with exactly this nominal size (depth_rank),
 * or INVALID_ID. */
ULONG caps_find_rtg_mode(UWORD w, UWORD h, UWORD *depth)
{
    ULONG id = INVALID_ID, best = INVALID_ID;
    UWORD best_rank = 0, best_depth = 0;
    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        struct DimensionInfo dim;
        UWORD r;
        if (!caps_mode_is_rtg(id) || ModeNotAvailable(id))
            continue;
        if (GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, id) <= 0)
            continue;
        if (dim.Nominal.MaxX - dim.Nominal.MinX + 1 != w ||
            dim.Nominal.MaxY - dim.Nominal.MinY + 1 != h)
            continue;
        r = depth_rank(dim.MaxDepth);
        if (r > best_rank) {
            best = id;
            best_rank = r;
            best_depth = dim.MaxDepth;
        }
    }
    if (depth)
        *depth = best_depth;
    return best;
}

static BOOL any_rtg_mode(void)
{
    ULONG id = INVALID_ID;
    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID)
        if (caps_mode_is_rtg(id))
            return TRUE;
    return FALSE;
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
    c->rtg = any_rtg_mode();
    c->net = lib_present("bsdsocket.library", 4);
    c->mp3_mpega = lib_present("mpega.library", 0);
    /* The ZZ9000 SDK's mpega.library decodes on the card: Ambiance need
     * not ask it for less than a fast Amiga would (stream.c, plex.c). */
    c->zz9000 = zz9000_detect();
    c->mpega_zz = c->mp3_mpega && lib_id_has("mpega.library", "ZZ9000");
    /* Ambiance's own MP3 decoder (extras/mpga): mpega's functions under
     * another name, so it never stands in for LIBS:mpega.library */
    c->mpega_own = path_present("PROGDIR:Libs/ambmpega.library");
    c->mp3_mhi = path_present("LIBS:MHI");
    c->ahi = path_present("DEVS:ahi.device");      /* AHI: Settings > Sound, later */
    c->flacdec = lib_present("flacdec.library", 1);  /* FLAC tracks as stored (Plex) */
    c->amissl = lib_present("amisslmaster.library", 5);  /* AmiSSL 5 or 6: HTTPS (tls.c) */
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
