/*
 * AmigaMCE - Mods: folders of music modules
 * Copyright (c) 2026 Brian King
 *
 * A Mods list holds folders (IT_DIR) and modules (IT_MOD), folders first,
 * each part sorted by name. A module is known by its name, as the Amiga
 * names them: "mod.Title" (ProTracker's own way), "Title.mod", or the
 * other trackers' ".nst"/".stk"/".m15"; a name with no dot at all is
 * looked inside (the ProTracker tag at byte 1080). Its label drops the
 * "mod." or ".mod". Whether it really is one AmigaMCE can play is found
 * out when it is played (modplay.c says why not).
 */
#include "amce.h"

#include <string.h>
#include <proto/exec.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

static BOOL ends_with(const char *s, const char *ext)
{
    LONG n = strlen(s), k = strlen(ext);
    return n > k && amce_stricmp(s + n - k, ext) == 0;
}

static BOOL starts_mod(const char *s)
{
    return (s[0] == 'm' || s[0] == 'M') && (s[1] == 'o' || s[1] == 'O') &&
           (s[2] == 'd' || s[2] == 'D') && s[3] == '.' && s[4];
}

/* The label shown for a module's file name. */
void mods_label(const char *name, char *out, LONG max)
{
    LONG n;
    if (starts_mod(name))
        name += 4;
    amce_strlcpy(out, name, max);
    n = strlen(out);
    if (n > 4 && (ends_with(out, ".mod") || ends_with(out, ".nst") || ends_with(out, ".stk") ||
                  ends_with(out, ".m15")))
        out[n - 4] = 0;
}

static void join_path(char *out, LONG max, const char *dir, const char *name)
{
    LONG n;
    amce_strlcpy(out, dir, max);
    n = strlen(out);
    if (n && out[n - 1] != ':' && out[n - 1] != '/')
        amce_strlcat(out, "/", max);
    amce_strlcat(out, name, max);
}

/* A file with no dot in its name: a ProTracker module inside? */
static BOOL sniff(const char *path)
{
    UBYTE b[4];
    BOOL yes = FALSE;
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return FALSE;
    if (Seek(fh, 1080, OFFSET_BEGINNING) >= 0 && Read(fh, b, 4) == 4)
        yes = !memcmp(b, "M.K.", 4) || !memcmp(b, "M!K!", 4) || !memcmp(b, "FLT4", 4) ||
              !memcmp(b, "4CHN", 4) || !memcmp(b, "M&K!", 4);
    Close(fh);
    return yes;
}

BOOL mods_is_module(const char *dir, const char *name)
{
    char path[300];
    if (starts_mod(name) || ends_with(name, ".mod") || ends_with(name, ".nst") ||
        ends_with(name, ".stk") || ends_with(name, ".m15"))
        return !ends_with(name, ".info");
    if (strchr(name, '.'))
        return FALSE;
    join_path(path, sizeof(path), dir, name);
    return sniff(path);
}

struct collect_ctx {
    const char *dir;
    struct ItemList *out;
    LONG mods;
};

static void collect_cb(void *ctx, const char *name, BOOL isdir)
{
    struct collect_ctx *c = ctx;
    char path[300], key[140], label[120];
    if (!isdir && !mods_is_module(c->dir, name))
        return;
    join_path(path, sizeof(path), c->dir, name);
    if (isdir)
        amce_strlcpy(label, name, sizeof(label));
    else
        mods_label(name, label, sizeof(label));
    key[0] = isdir ? '0' : '1';             /* folders first */
    amce_strlcpy(key + 1, label, sizeof(key) - 1);
    if (list_add(c->out, label, path, key, isdir ? IT_DIR : IT_MOD) && !isdir)
        c->mods++;
}

/* dir's folders and modules into out, sorted; -1 if it cannot be read. */
LONG mods_read_dir(const char *dir, struct ItemList *out)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    struct collect_ctx c;
    LONG n;
    me->pr_WindowPtr = (APTR)-1;
    c.dir = dir;
    c.out = out;
    c.mods = 0;
    n = dir_scan(dir, collect_cb, &c);
    me->pr_WindowPtr = old;
    if (n < 0)
        return -1;
    list_sort(out);
    return out->count;
}
