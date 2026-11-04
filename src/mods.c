/*
 * AmigaMCE - Mods: folders of music modules
 * Copyright (c) 2026 Brian King
 *
 * A Mods list holds folders (IT_DIR) and music files (IT_MOD), folders
 * first, each part sorted by name. With EaglePlayer's players AmigaMCE
 * plays hundreds of formats, named every which way ("MED.title",
 * "title.mod", "mdat.title", "TFMX.title", no suffix at all), so every file
 * is listed but icons, texts, archives, pictures and the files that go
 * with another (TFMX's smpl., StarTrekker's .nt). Whether one plays is found
 * out when it is played (modplay.c says why not). The label drops the
 * format's prefix or suffix when it is one of the usual ones.
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

/* Formats' names, as file name prefixes and suffixes. */
static const char *const kinds[] = {
    "mod", "med", "mmd0", "mmd1", "mmd2", "mmd3", "omed", "nst", "stk", "m15", "ust",
    "mdat", "tfmx", "fc", "fc13", "fc14", "smod", "bp", "bp3", "sndmon", "dw", "dwold",
    "hip", "hipc", "hip7", "rh", "rho", "ahx", "thx", "okt", "okta", "sfx", "digi", "dbm",
    "dbm0", "jam", "jc", "sa", "sonic", "dm", "dm2", "dmu", "mug", "mug2", "ps", "pt",
    "p61", "p61a", "the", "tme", "ims", "emod", "ftm", "fp", "sng", "ss", "symmod", "sym",
    "xm", "s3m", "it", "mtm", "669", "ult", "far", "stm", "mo3", "aon", "aon4", "aon8",
    "ml", "sid", "psid", "mus", "cus", "cust", "custom", "spl", "tw", "dl", "jmf", "riff",
    NULL
};

static BOOL is_kind(const char *s, LONG n)
{
    int i;
    for (i = 0; kinds[i]; i++)
        if ((LONG)strlen(kinds[i]) == n) {
            LONG k;
            for (k = 0; k < n && (s[k] | 0x20) == kinds[i][k]; k++)
                ;
            if (k == n)
                return TRUE;
        }
    return FALSE;
}

/* The label shown for a music file's name: its format's prefix
 * ("MED.") and suffix (".mod") left off. */
void mods_label(const char *name, char *out, LONG max)
{
    const char *dot = strchr(name, '.'), *last;
    LONG n;
    if (dot && dot[1] && is_kind(name, dot - name))
        name = dot + 1;
    amce_strlcpy(out, name, max);
    n = strlen(out);
    last = strrchr(out, '.');
    if (last && last != out && is_kind(last + 1, n - (last + 1 - out)))
        out[last - out] = 0;
}

/* Not music: what else lives in module folders. */
static const char *const not_music[] = {
    ".info", ".txt", ".doc", ".readme", ".guide", ".nfo", ".diz", ".lha", ".lzh", ".lzx",
    ".zip", ".gz", ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".pcx", ".nt", ".as", ".ins",
    ".exe", ".rexx", ".py", ".html", ".htm", ".pdf", NULL
};

static void join_path(char *out, LONG max, const char *dir, const char *name)
{
    LONG n;
    amce_strlcpy(out, dir, max);
    n = strlen(out);
    if (n && out[n - 1] != ':' && out[n - 1] != '/')
        amce_strlcat(out, "/", max);
    amce_strlcat(out, name, max);
}

BOOL mods_is_module(const char *dir, const char *name)
{
    int i;
    (void)dir;
    if (name[0] == '.')
        return FALSE;
    for (i = 0; not_music[i]; i++)
        if (ends_with(name, not_music[i]))
            return FALSE;
    /* the second file of a pair (mdat. plays, smpl. is its samples) */
    if (!amce_stricmp(name, "smpl") || (strlen(name) > 5 && !memcmp(name, "smpl.", 5)) ||
        (strlen(name) > 5 && !memcmp(name, "SMPL.", 5)) || ends_with(name, ".smpl") ||
        ends_with(name, ".ssd") || ends_with(name, ".instr"))
        return FALSE;
    return TRUE;
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
