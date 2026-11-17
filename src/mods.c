/*
 * Ambiance - Mods: folders of music modules
 * Copyright (c) 2026 Brian King
 *
 * A Mods list holds folders (IT_DIR) and music files (IT_MOD), folders
 * first, each part sorted by name. With EaglePlayer's players Ambiance
 * plays hundreds of formats, named every which way ("MED.title",
 * "title.mod", "mdat.title", "TFMX.title", no suffix at all), so every file
 * is listed but icons, texts, archives, pictures and the files that go
 * with another (TFMX's smpl., StarTrekker's .nt). Whether one plays is found
 * out when it is played (modplay.c says why not). The label drops the
 * format's prefix or suffix when it is one of the usual ones.
 */
#include "amb.h"

#include <string.h>
#include <proto/exec.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

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
    amb_strlcpy(out, name, max);
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

BOOL mods_is_module(const char *dir, const char *name)
{
    int i;
    (void)dir;
    if (name[0] == '.')
        return FALSE;
    for (i = 0; not_music[i]; i++)
        if (amb_ends_with(name, not_music[i]))
            return FALSE;
    /* the second file of a pair (mdat. plays, smpl. is its samples) */
    if (!amb_stricmp(name, "smpl") || (strlen(name) > 5 && !memcmp(name, "smpl.", 5)) ||
        (strlen(name) > 5 && !memcmp(name, "SMPL.", 5)) || amb_ends_with(name, ".smpl") ||
        amb_ends_with(name, ".ssd") || amb_ends_with(name, ".instr"))
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
    amb_join_path(path, sizeof(path), c->dir, name);
    if (isdir)
        amb_strlcpy(label, name, sizeof(label));
    else
        mods_label(name, label, sizeof(label));
    key[0] = isdir ? '0' : '1';             /* folders first */
    amb_strlcpy(key + 1, label, sizeof(key) - 1);
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

/* ---- Music's own files (Settings > Sound > Music folders): folders with
 * music somewhere inside, and the files Ambiance plays (MP3 with
 * mpega.library, FLAC with flacdec.library, WAV, AIFF and 8SVX always),
 * folders first, by name. */
static BOOL can_mp3 = TRUE, can_flac = TRUE;

void songs_can_play(BOOL mp3, BOOL flac)
{
    can_mp3 = mp3;
    can_flac = flac;
}

BOOL song_name_ok(const char *name)
{
    if (name[0] == '.')
        return FALSE;
    return (can_mp3 && amb_ends_with(name, ".mp3")) || (can_flac && amb_ends_with(name, ".flac")) ||
           amb_ends_with(name, ".wav") || amb_ends_with(name, ".aif") || amb_ends_with(name, ".aiff") ||
           amb_ends_with(name, ".aifc") || amb_ends_with(name, ".8svx") || amb_ends_with(name, ".svx");
}

/* ---- whether a folder has music in it, anywhere below: looked through
 * (a few levels down; in a very large tree it is taken to have), and the
 * answer kept for the session */
#define HAS_KEPT   512
#define HAS_DEPTH  4
#define HAS_BUDGET 1500             /* entries looked at, at most */
static struct { ULONG hash; BOOL yes; } has_kept[HAS_KEPT];
static int has_next;

struct has_ctx {
    BOOL found;
    LONG seen, ndirs;
    char dirs[16][108];             /* the first folders inside, to look in next */
};

static void has_cb(void *ctx, const char *name, BOOL isdir)
{
    struct has_ctx *c = ctx;
    c->seen++;
    if (c->found)
        return;
    if (!isdir) {
        if (song_name_ok(name))
            c->found = TRUE;
    } else if (c->ndirs < 16 && strlen(name) < sizeof(c->dirs[0])) {
        strcpy(c->dirs[c->ndirs++], name);
    }
}

static BOOL has_music(const char *dir, int depth, LONG *budget)
{
    struct has_ctx *c;
    BOOL yes = FALSE;
    LONG i;
    if (depth > HAS_DEPTH || *budget <= 0)
        return *budget <= 0;                /* too big to look through: shown */
    if (!(c = AllocVec(sizeof(*c), MEMF_ANY | MEMF_CLEAR)))
        return TRUE;
    if (dir_scan(dir, has_cb, c) < 0) {
        FreeVec(c);
        return FALSE;
    }
    *budget -= c->seen;
    yes = c->found;
    for (i = 0; !yes && i < c->ndirs; i++) {
        char sub[300];
        amb_join_path(sub, sizeof(sub), dir, c->dirs[i]);
        yes = has_music(sub, depth + 1, budget);
    }
    FreeVec(c);
    return yes;
}

static BOOL has_music_kept(const char *dir)
{
    ULONG h = 5381;
    const char *s;
    LONG budget = HAS_BUDGET;
    int i;
    BOOL yes;
    for (s = dir; *s; s++)
        h = h * 33 + (UBYTE)(*s | 0x20);
    h = h * 2 + (can_flac ? 1 : 0);
    for (i = 0; i < HAS_KEPT; i++)
        if (has_kept[i].hash == h)
            return has_kept[i].yes;
    yes = has_music(dir, 1, &budget);
    has_kept[has_next].hash = h;
    has_kept[has_next].yes = yes;
    has_next = (has_next + 1) % HAS_KEPT;
    return yes;
}

static void song_cb(void *ctx, const char *name, BOOL isdir)
{
    struct collect_ctx *c = ctx;
    char path[300], key[140], label[120], *dot;
    if (!isdir && !song_name_ok(name))
        return;
    amb_join_path(path, sizeof(path), c->dir, name);
    amb_strlcpy(label, name, sizeof(label));
    if (!isdir && (dot = strrchr(label, '.')))
        *dot = 0;                           /* the name without .mp3, .flac */
    key[0] = isdir ? '0' : '1';
    amb_strlcpy(key + 1, label, sizeof(key) - 1);
    if (list_add(c->out, label, path, key, isdir ? IT_DIR : IT_SONG) && !isdir)
        c->mods++;
}

/* A folder with more folders than this has them all listed, without
 * looking inside each for music: an artist folder of a big library on a
 * network share was thousands of folder reads (minutes) before its list
 * showed. */
#define SONGS_LOOK_MAX 64

/* dir's folders with music and its music files into out, sorted; -1 if it
 * cannot be read. */
LONG songs_read_dir(const char *dir, struct ItemList *out)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    struct collect_ctx c;
    LONG n;
    me->pr_WindowPtr = (APTR)-1;
    c.dir = dir;
    c.out = out;
    c.mods = 0;
    n = dir_scan(dir, song_cb, &c);
    if (n >= 0) {
        LONG i, k, dirs = 0;
        for (i = 0; i < out->count; i++)
            if (out->items[i].kind == IT_DIR)
                dirs++;
        if (dirs && dirs <= SONGS_LOOK_MAX) {
            /* the folders with no music anywhere in them left out */
            for (i = k = 0; i < out->count; i++)
                if (out->items[i].kind != IT_DIR || has_music_kept(out->items[i].path))
                    out->items[k++] = out->items[i];
            out->count = k;
        }
    }
    me->pr_WindowPtr = old;
    if (n < 0)
        return -1;
    list_sort(out);
    return out->count;
}

/* A music file's format, for Now Playing: "FLAC", "MP3". */
const char *song_format(const char *path)
{
    return amb_ends_with(path, ".flac") ? "FLAC" : amb_ends_with(path, ".wav") ? "WAV"
           : amb_ends_with(path, ".8svx") || amb_ends_with(path, ".svx") ? "8SVX"
           : amb_ends_with(path, ".aif") || amb_ends_with(path, ".aiff") || amb_ends_with(path, ".aifc")
             ? "AIFF" : "MP3";
}
