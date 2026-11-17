/*
 * Ambiance - WHDLoad itself, found or installed (see whdinst.h)
 * Copyright (c) 2026 Brian King
 *
 * WHDLoad (Bert Jahn's, free of charge since 18.2) is fetched from its own
 * site at the user's word, never carried by Ambiance. Its package has an
 * Installer script; Ambiance does what the user part of it comes to
 * without asking questions on a screen it cannot show: the programs of
 * the package's C drawer (WHDLoad, and DIC, RawDIC, Patcher... where they
 * are) into C:, its WHDLoad.prf into S: where there is none, and the
 * Devs:Kickstarts drawer that installs look in for Kickstart images. The
 * C drawer is found by looking for WHDLoad itself in what was unpacked,
 * so the package's layout is not assumed.
 */
#include "whdinst.h"

#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>

BOOL whd_present(char *where, LONG max)
{
    static const char *const places[] = { "C:WHDLoad", "SYS:C/WHDLoad", NULL };
    int i;
    for (i = 0; places[i]; i++)
        if (amce_exists(places[i])) {
            if (where)
                amce_strlcpy(where, places[i], max);
            return TRUE;
        }
    if (where && max > 0)
        where[0] = 0;
    return FALSE;
}

/* ------------------------------------------------------------ finding it */

struct Names {
    char n[64][108];
    UBYTE dir[64];
    LONG count;
};

static void names_fn(void *ctx, const char *name, BOOL isdir)
{
    struct Names *t = ctx;
    if (t->count < 64 && strlen(name) < sizeof(t->n[0])) {
        strcpy(t->n[t->count], name);
        t->dir[t->count++] = isdir;
    }
}

/* The drawer under dir (depth levels at most) with a file named WHDLoad. */
static BOOL find_c(const char *dir, int depth, char *out, LONG max)
{
    struct Names *t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR);
    LONG i;
    BOOL found = FALSE;
    if (!t)
        return FALSE;
    if (dir_scan(dir, names_fn, t) >= 0) {
        for (i = 0; i < t->count && !found; i++)
            if (!t->dir[i] && !amce_stricmp(t->n[i], "WHDLoad")) {
                amce_strlcpy(out, dir, max);
                found = TRUE;
            }
        for (i = 0; i < t->count && !found && depth > 0; i++)
            if (t->dir[i]) {
                char sub[300];
                amce_join_path(sub, sizeof(sub), dir, t->n[i]);
                found = find_c(sub, depth - 1, out, max);
            }
    }
    FreeVec(t);
    return found;
}

/* ------------------------------------------------------------ copying */

static BOOL copy_file(const char *from, const char *to)
{
    BPTR in, out;
    UBYTE *buf;
    LONG n;
    BOOL ok = TRUE;
    if (!(buf = AllocVec(16384, MEMF_ANY)))
        return FALSE;
    if (!(in = Open((CONST_STRPTR)from, MODE_OLDFILE))) {
        FreeVec(buf);
        return FALSE;
    }
    if (!(out = Open((CONST_STRPTR)to, MODE_NEWFILE))) {
        Close(in);
        FreeVec(buf);
        return FALSE;
    }
    while ((n = Read(in, buf, 16384)) > 0)
        if (Write(out, buf, n) != n) {
            ok = FALSE;
            break;
        }
    if (n < 0)
        ok = FALSE;
    Close(out);
    Close(in);
    FreeVec(buf);
    if (!ok)
        DeleteFile((CONST_STRPTR)to);
    return ok;
}

BOOL whd_install_from(const char *dir, char *why, LONG wmax, char *done, LONG dmax)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    struct Names *t;
    char cdir[300], from[300], to[160], m[200];
    LONG i, put = 0, kept = 0;
    BOOL ok = TRUE;
    why[0] = done[0] = 0;
    if (!find_c(dir, 4, cdir, sizeof(cdir))) {
        amce_strlcpy(why, _("WHDLoad was not in the package as expected"), wmax);
        amce_log("whdload: no WHDLoad under", dir);
        return FALSE;
    }
    if (!(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR))) {
        amce_strlcpy(why, _("Not enough memory"), wmax);
        return FALSE;
    }
    me->pr_WindowPtr = (APTR)-1;
    dir_scan(cdir, names_fn, t);
    for (i = 0; i < t->count && ok; i++) {
        BOOL whd = !amce_stricmp(t->n[i], "WHDLoad");
        if (t->dir[i] || amce_ends_with(t->n[i], ".info"))
            continue;
        amce_join_path(from, sizeof(from), cdir, t->n[i]);
        amce_join_path(to, sizeof(to), "C:", t->n[i]);
        if (!whd && amce_exists(to)) {
            kept++;                     /* another program of that name: left as it is */
            amce_log("whdload: kept", to);
            continue;
        }
        if (!copy_file(from, to)) {
            sprintf(m, _("%.60s could not be written"), to);
            amce_strlcpy(why, m, wmax);
            ok = FALSE;
        } else {
            put++;
            amce_log("whdload: installed", to);
        }
    }
    if (ok) {
        /* WHDLoad.prf: in an S drawer beside the C drawer, if S: has none */
        char sdir[300], *slash;
        amce_strlcpy(sdir, cdir, sizeof(sdir));
        slash = strrchr(sdir, '/');
        if (slash) {
            strcpy(slash + 1, "S/WHDLoad.prf");
            if (amce_exists(sdir) && !amce_exists("S:WHDLoad.prf") &&
                copy_file(sdir, "S:WHDLoad.prf"))
                amce_log("whdload: installed", "S:WHDLoad.prf");
        }
        if (!amce_exists("DEVS:Kickstarts")) {
            BPTR l = CreateDir((CONST_STRPTR)"DEVS:Kickstarts");
            if (l) {
                UnLock(l);
                amce_log("whdload: made", "DEVS:Kickstarts");
            }
        }
        sprintf(m, _("WHDLoad and %ld more programs put in C:"), (long)(put > 0 ? put - 1 : 0));
        if (kept) {
            char k[60];
            sprintf(k, _(" (%ld already there kept)"), (long)kept);
            amce_strlcat(m, k, sizeof(m));
        }
        amce_strlcpy(done, m, dmax);
    }
    me->pr_WindowPtr = oldwin;
    FreeVec(t);
    return ok && amce_exists("C:WHDLoad");
}

/* ------------------------------------------------------------ tidying */

static void delete_tree(const char *dir, int depth)
{
    struct Names *t;
    LONG i;
    if (depth < 0 || !(t = AllocVec(sizeof(*t), MEMF_ANY | MEMF_CLEAR)))
        return;
    /* a drawer may hold more than one scan's worth: again until empty */
    for (;;) {
        LONG before;
        t->count = 0;
        if (dir_scan(dir, names_fn, t) < 0 || !t->count)
            break;
        before = t->count;
        for (i = 0; i < t->count; i++) {
            char p[300];
            amce_join_path(p, sizeof(p), dir, t->n[i]);
            if (t->dir[i])
                delete_tree(p, depth - 1);
            DeleteFile((CONST_STRPTR)p);
        }
        if (before < 64)
            break;
    }
    FreeVec(t);
}

void whd_cleanup(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    DeleteFile((CONST_STRPTR)WHD_ARCHIVE);
    if (amce_exists(WHD_DIR)) {
        delete_tree(WHD_DIR, 6);
        DeleteFile((CONST_STRPTR)WHD_DIR);
    }
    me->pr_WindowPtr = oldwin;
}
