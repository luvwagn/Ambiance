/*
 * AmigaMCE - archives unpacked (see unarc.h)
 * Copyright (c) 2026 Brian King
 *
 * xadmaster.library (Dirk Stöcker's, Aminet util/arc/xadmaster000.lha) is
 * the way: each entry checked, then unpacked to its place under dest by
 * xadFileUnArc, its drawers made. Without it, Amiga LhA in C: (Stefan
 * Boberg's, "LhA x archive dest/") does an LhA archive; its entries are
 * not checked first, so only an archive from a known place goes that way.
 */
#include "unarc.h"

#include <stdio.h>
#include <string.h>
#include <dos/dostags.h>
#include <libraries/xadmaster.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/xadmaster.h>

struct xadMasterBase *xadMasterBase;

BOOL unarc_safe_name(const char *name)
{
    if (!name || !name[0] || name[0] == '/' || strchr(name, ':') || strstr(name, "//"))
        return FALSE;
    return TRUE;
}

static BOOL lha_there(void)
{
    return amce_exists("C:LhA");
}

int unarc_how(void)
{
    struct Library *x = OpenLibrary((CONST_STRPTR)"xadmaster.library", 12);
    if (x) {
        CloseLibrary(x);
        return UNARC_XAD;
    }
    return lha_there() ? UNARC_LHA : UNARC_NONE;
}

const char *unarc_name(int how)
{
    return how == UNARC_XAD ? "xadmaster.library" : how == UNARC_LHA ? "C:LhA" : "";
}

/* Every drawer on the way to path's file made (dest's own and below). */
static void make_dirs(const char *path)
{
    char d[300];
    char *s;
    amce_strlcpy(d, path, sizeof(d));
    s = strchr(d, ':');
    s = s ? s + 1 : d;
    while ((s = strchr(s, '/')) != NULL) {
        BPTR l;
        *s = 0;
        if ((l = Lock((CONST_STRPTR)d, SHARED_LOCK)))
            UnLock(l);
        else if ((l = CreateDir((CONST_STRPTR)d)))
            UnLock(l);
        *s++ = '/';
    }
}

static BOOL xad_extract(const char *archive, const char *dest, char *why, LONG wmax)
{
    struct xadArchiveInfo *ai;
    struct xadFileInfo *fi;
    LONG e, files = 0, skipped = 0;
    BOOL ok = TRUE;
    char m[200];
    if (!(xadMasterBase = (struct xadMasterBase *)OpenLibrary((CONST_STRPTR)"xadmaster.library", 12))) {
        amce_strlcpy(why, _("xadmaster.library could not be opened"), wmax);
        return FALSE;
    }
    if (!(ai = (struct xadArchiveInfo *)xadAllocObjectA(XADOBJ_ARCHIVEINFO, NULL))) {
        CloseLibrary((struct Library *)xadMasterBase);
        xadMasterBase = NULL;
        amce_strlcpy(why, _("Not enough memory"), wmax);
        return FALSE;
    }
    if ((e = xadGetInfo(ai, XAD_INFILENAME, (ULONG)archive, TAG_DONE)) != XADERR_OK) {
        sprintf(m, _("The archive could not be read (%.60s)"), (const char *)xadGetErrorText(e));
        amce_strlcpy(why, m, wmax);
        ok = FALSE;
    } else {
        for (fi = ai->xai_FileInfo; fi && ok; fi = fi->xfi_Next) {
            char out[300];
            const char *name = (const char *)fi->xfi_FileName;
            if (!unarc_safe_name(name) || (fi->xfi_Flags & XADFIF_LINK)) {
                skipped++;
                amce_log("unarc: left out", name ? name : "(no name)");
                continue;
            }
            amce_join_path(out, sizeof(out), dest, name);
            if (fi->xfi_Flags & XADFIF_DIRECTORY) {
                char d[304];
                sprintf(d, "%.300s/", out);
                make_dirs(d);
                continue;
            }
            make_dirs(out);
            e = xadFileUnArc(ai, XAD_ENTRYNUMBER, fi->xfi_EntryNumber, XAD_OUTFILENAME, (ULONG)out,
                             XAD_MAKEDIRECTORY, TRUE, XAD_OVERWRITE, TRUE, TAG_DONE);
            if (e != XADERR_OK) {
                sprintf(m, _("%.40s could not be unpacked (%.60s)"), name,
                        (const char *)xadGetErrorText(e));
                amce_strlcpy(why, m, wmax);
                ok = FALSE;
            } else
                files++;
        }
        xadFreeInfo(ai);
    }
    xadFreeObjectA(ai, NULL);
    CloseLibrary((struct Library *)xadMasterBase);
    xadMasterBase = NULL;
    sprintf(m, "%.120s: %ld files by xadmaster.library, %ld left out%s", archive, (long)files,
            (long)skipped, ok ? "" : " (failed)");
    amce_log("unarc", m);
    return ok;
}

static BOOL lha_extract(const char *archive, const char *dest, char *why, LONG wmax)
{
    char cmd[700], d[300];
    LONG rc;
    BPTR nil;
    LONG n;
    amce_strlcpy(d, dest, sizeof(d));
    n = strlen(d);
    if (n && d[n - 1] != ':' && d[n - 1] != '/')
        amce_strlcat(d, "/", sizeof(d));        /* LhA takes a drawer with its '/' */
    make_dirs(d);
    sprintf(cmd, "C:LhA x \"%.290s\" \"%.300s\"", archive, d);    /* (into a new drawer: no questions) */
    if (!(nil = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE))) {
        amce_strlcpy(why, _("NIL: could not be opened"), wmax);
        return FALSE;
    }
    rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, 0, SYS_Output, (ULONG)nil, NP_StackSize, 16384,
                    TAG_DONE);
    Close(nil);
    {
        char m[760];
        sprintf(m, "%.700s: returned %ld", cmd, (long)rc);
        amce_log("unarc", m);
    }
    if (rc != 0) {
        char m[80];
        sprintf(m, _("LhA could not unpack it (it returned %ld)"), (long)rc);
        amce_strlcpy(why, m, wmax);
        return FALSE;
    }
    return TRUE;
}

BOOL unarc_extract(const char *archive, const char *dest, char *why, LONG wmax)
{
    int how = unarc_how();
    why[0] = 0;
    if (how == UNARC_XAD)
        return xad_extract(archive, dest, why, wmax);
    if (how == UNARC_LHA && amce_ends_with(archive, ".lha"))
        return lha_extract(archive, dest, why, wmax);
    amce_strlcpy(why, how == UNARC_LHA ? _("LhA unpacks only .lha archives: install xadmaster.library")
                                       : _("Nothing here unpacks archives: install xadmaster.library "
                                           "(Aminet util/arc/xadmaster000.lha)"),
                 wmax);
    return FALSE;
}
