/*
 * AmigaMCE - Amiga Game Selector 3 integration: finding AGS, reading its
 * menu tree and entry files, and writing the launch script.
 * Copyright (c) 2026 Brian King
 */
#include "amce.h"

#include <string.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------ lists */

BOOL list_init(struct ItemList *l)
{
    l->items = NULL;
    l->count = l->cap = 0;
    l->pool = CreatePool(MEMF_ANY, 8192, 4096);
    return l->pool != NULL;
}

void list_free(struct ItemList *l)
{
    if (l->pool)
        DeletePool(l->pool);
    l->pool = NULL;
    l->items = NULL;
    l->count = l->cap = 0;
}

static char *pool_strdup(APTR pool, const char *s)
{
    LONG n = strlen(s) + 1;
    char *d = AllocPooled(pool, n);
    if (d)
        CopyMem((APTR)s, d, n);
    return d;
}

struct Item *list_add(struct ItemList *l, const char *label, const char *path,
                      const char *sortkey, UBYTE kind)
{
    struct Item *it;
    if (l->count == l->cap) {
        LONG ncap = l->cap ? l->cap * 2 : 64;
        struct Item *n = AllocPooled(l->pool, ncap * sizeof(struct Item));
        if (!n)
            return NULL;
        if (l->items)
            CopyMem(l->items, n, l->count * sizeof(struct Item));
        l->items = n;           /* old block stays in the pool until list_free */
        l->cap = ncap;
    }
    it = &l->items[l->count];
    it->label = pool_strdup(l->pool, label ? label : "");
    it->path = pool_strdup(l->pool, path ? path : "");
    it->sortkey = pool_strdup(l->pool, sortkey ? sortkey : (label ? label : ""));
    it->kind = kind;
    it->action = 0;
    it->mark = kind == IT_DIR ? MARK_ARROW : MARK_NONE;
    it->flags = 0;
    if (!it->label || !it->path || !it->sortkey)
        return NULL;
    l->count++;
    return it;
}

static int cmp_items(const void *a, const void *b)
{
    return amce_stricmp(((const struct Item *)a)->sortkey, ((const struct Item *)b)->sortkey);
}

/* Small insertion-merge sort to avoid pulling in qsort's recursion depth on
 * a 4 KB stack: a bottom-up merge sort with a temporary pooled buffer. */
void list_sort(struct ItemList *l)
{
    struct Item *tmp, *src, *dst;
    LONG n = l->count, width, i;
    if (n < 2)
        return;
    tmp = AllocPooled(l->pool, n * sizeof(struct Item));
    if (!tmp)
        return;
    src = l->items;
    dst = tmp;
    for (width = 1; width < n; width *= 2) {
        for (i = 0; i < n; i += 2 * width) {
            LONG lo = i, mid = i + width < n ? i + width : n, hi = i + 2 * width < n ? i + 2 * width : n;
            LONG a = lo, b = mid, k = lo;
            while (a < mid && b < hi)
                dst[k++] = cmp_items(&src[a], &src[b]) <= 0 ? src[a++] : src[b++];
            while (a < mid)
                dst[k++] = src[a++];
            while (b < hi)
                dst[k++] = src[b++];
        }
        { struct Item *t = src; src = dst; dst = t; }
    }
    if (src != l->items)
        CopyMem(src, l->items, n * sizeof(struct Item));
}

/* ------------------------------------------------------------ names */

static BOOL ends_with(const char *s, const char *ext)
{
    LONG a = strlen(s), b = strlen(ext);
    return a > b && amce_stricmp(s + a - b, ext) == 0;
}

/* AGS orders its menus with leading spaces and the prefixes "+  ", "-  "
 * and "-- "; the display name drops them, plus the .ags/.run extension. */
void ags_clean_name(const char *raw, char *out, LONG max)
{
    char buf[128];
    char *p, *e;
    amce_strlcpy(buf, raw, sizeof(buf));
    if (ends_with(buf, ".ags") || ends_with(buf, ".run") || ends_with(buf, ".agz") || ends_with(buf, ".rub"))
        buf[strlen(buf) - 4] = 0;
    p = buf;
    while (*p == ' ')
        p++;
    if ((p[0] == '+' || p[0] == '-') && p[1] == ' ' && p[2] == ' ')
        p += 3;
    else if (p[0] == '-' && p[1] == '-' && p[2] == ' ')
        p += 3;
    while (*p == ' ')
        p++;
    e = p + strlen(p);
    while (e > p && e[-1] == ' ')
        *--e = 0;
    amce_strlcpy(out, p, max);
}

BOOL ags_is_separator(const char *clean)
{
    const char *p = clean;
    if (!*p)
        return TRUE;
    for (; *p; p++)
        if (!strchr("-~.=_ ", *p))
            return FALSE;
    return TRUE;
}

static BOOL contains_nocase(const char *hay, const char *needle)
{
    LONG n = strlen(needle);
    char tmp[64];
    for (; *hay; hay++) {
        amce_strlcpy(tmp, hay, n + 1 < (LONG)sizeof(tmp) ? n + 1 : (LONG)sizeof(tmp));
        if (amce_stricmp(tmp, needle) == 0)
            return TRUE;
    }
    return FALSE;
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

/* Read one AGS menu folder: .ags subfolders and .run entries. */
/* Add the entries of one AGS folder to out, unsorted. */
static LONG read_entries(const char *dir, struct ItemList *out, BOOL adult)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    char clean[128], path[300];
    LONG n = 0;

    lock = Lock((CONST_STRPTR)dir, SHARED_LOCK);
    if (!lock)
        return -1;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        return -1;
    }
    if (Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            const char *name = (const char *)fib->fib_FileName;
            UBYTE kind;
            if (fib->fib_DirEntryType > 0) {
                if (!ends_with(name, ".ags"))
                    continue;           /* .agz = hidden by AGS; other drawers are data */
                kind = IT_DIR;
            } else {
                if (!ends_with(name, ".run"))
                    continue;           /* .txt/.iff are read with their entry; .rub = disabled */
                kind = IT_RUN;
            }
            ags_clean_name(name, clean, sizeof(clean));
            if (ags_is_separator(clean) || clean[0] == '[')
                continue;               /* AGS's own option entries */
            if (!adult && contains_nocase(clean, "adult"))
                continue;
            join_path(path, sizeof(path), dir, name);
            if (kind == IT_RUN)
                path[strlen(path) - 4] = 0;  /* keep the base; .run/.txt/.iff hang off it */
            if (list_add(out, clean, path, name, kind))
                n++;
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return n;
}

/* The entries of one AGS folder, sorted. -1 if it cannot be read. */
LONG ags_read_dir(const char *dir, struct ItemList *out, BOOL adult)
{
    LONG n = read_entries(dir, out, adult);
    if (n >= 0)
        list_sort(out);
    return n;
}

/* An AGS letter-index folder: "A".."Z", a digit, "0-9" or "#". */
static BOOL letter_folder(const char *label)
{
    UBYTE c = (UBYTE)label[0];
    if (!c)
        return FALSE;
    if (!label[1])
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '#';
    return amce_stricmp(label, "0-9") == 0;
}

/* A menu as AmigaMCE shows it: one whose sub-folders are all a letter
 * index (AGA, Beta, NTSC... split A-Z) is one flat sorted list of their
 * entries; any other (By Genre, By Publisher...) keeps its folders. */
LONG ags_read_menu(const char *dir, struct ItemList *out, BOOL adult)
{
    struct ItemList subs;
    LONG i, dirs = 0;
    BOOL flat = TRUE;
    if (!list_init(&subs))
        return -1;
    if (read_entries(dir, &subs, adult) < 0) {
        list_free(&subs);
        return -1;
    }
    for (i = 0; i < subs.count; i++)
        if (subs.items[i].kind == IT_DIR) {
            dirs++;
            if (!letter_folder(subs.items[i].label))
                flat = FALSE;
        }
    for (i = 0; i < subs.count; i++) {
        struct Item *it = &subs.items[i];
        if (it->kind == IT_DIR && flat && dirs)
            read_entries(it->path, out, adult);
        else
            list_add(out, it->label, it->path, it->sortkey, it->kind);
    }
    list_free(&subs);
    list_sort(out);
    return out->count;
}

/* A folder of folders (AGS's "Games - 0-Z": one per letter) as one list:
 * the entries of every sub-folder, and any of its own, sorted together. */
LONG ags_read_dir_flat(const char *dir, struct ItemList *out, BOOL adult)
{
    struct ItemList subs;
    LONG i, n;
    if (!list_init(&subs))
        return -1;
    n = read_entries(dir, &subs, adult);
    if (n < 0) {
        list_free(&subs);
        return -1;
    }
    for (i = 0; i < subs.count; i++) {
        struct Item *it = &subs.items[i];
        if (it->kind == IT_DIR)
            read_entries(it->path, out, adult);
        else
            list_add(out, it->label, it->path, it->sortkey, it->kind);
    }
    list_free(&subs);
    list_sort(out);
    return out->count;
}

/* ------------------------------------------------------------ AGS */

static BOOL exists(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!l)
        return FALSE;
    UnLock(l);
    return TRUE;
}

/* Assign name: to target unless name: already exists or target does not.
 * Each assign made is noted in a->added for System information. */
static void assign_if_missing(struct Ags *a, const char *name, const char *target)
{
    char dev[40];
    BPTR l;
    if (!target || !target[0])
        return;
    amce_strlcpy(dev, name, sizeof(dev));
    amce_strlcat(dev, ":", sizeof(dev));
    if (exists(dev))
        return;
    l = Lock((CONST_STRPTR)target, SHARED_LOCK);
    if (!l)
        return;
    if (!AssignLock((CONST_STRPTR)name, l)) {
        UnLock(l);
        return;
    }
    if (a->added[0])
        amce_strlcat(a->added, " ", sizeof(a->added));
    amce_strlcat(a->added, dev, sizeof(a->added));
}

static void get_env(const char *name, char *buf, LONG max)
{
    LONG n = GetVar((CONST_STRPTR)name, (STRPTR)buf, max, GVF_GLOBAL_ONLY);
    if (n < 0)
        buf[0] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = 0;
}

/* A path as an environment variable may hold it, made fit for Lock():
 * AGS stores whdlsaves as "AGS:WHDSaves/" with the quotes (the Shell
 * drops them when it expands $whdlsaves; Lock would not), and a trailing
 * slash after a name is dropped too ("dir/" -> "dir", "Vol:" kept). */
void ags_unquote_path(const char *in, char *out, LONG max)
{
    LONG n;
    while (*in == ' ')
        in++;
    if (*in == '"')
        in++;
    amce_strlcpy(out, in, max);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == '"' || out[n - 1] == ' '))
        out[--n] = 0;
    if (n > 1 && out[n - 1] == '/' && out[n - 2] != ':' && out[n - 2] != '/')
        out[--n] = 0;
}

/* Find AGS. A normal install assigns AGS: on every boot (S:AGS-Stuff);
 * a portable install does not, so set up what WHDLoad:AGS would. */
BOOL ags_init(struct Ags *a)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    static const char *cands[] = { "PROGDIR:/AGS2", "WHDLoad:AGS2", NULL };
    int i;

    memset(a, 0, sizeof(*a));
    list_init(&a->top);
    me->pr_WindowPtr = (APTR)-1;
    if (exists("AGS:")) {
        a->found = TRUE;
    } else {
        for (i = 0; cands[i]; i++) {
            BPTR l = Lock((CONST_STRPTR)cands[i], SHARED_LOCK);
            if (!l)
                continue;
            if (AssignLock((CONST_STRPTR)"AGS", l)) {
                a->found = a->set_assigns = TRUE;
                amce_strlcpy(a->added, "AGS:", sizeof(a->added));
                break;
            }
            UnLock(l);
        }
    }
    if (a->found) {
        /* What S:AGS-Stuff assigns at boot (AGS 3), for systems where it
         * did not run. Each only if missing and its target exists. */
        char saves[128];
        amce_strlcpy(a->root, "AGS:", sizeof(a->root));
        assign_if_missing(a, "Scripts", "AGS:Scripts");
        assign_if_missing(a, "AGSOS", "AGS:OS");
        assign_if_missing(a, "Admin", "Scripts:Admin");
        if (exists("WHDLoad:")) {
            assign_if_missing(a, "WHD_Games", "WHDLoad:");
            assign_if_missing(a, "WHD_Demos", "WHDLoad:");
        }
        assign_if_missing(a, "ST-00", "Media:ST-00");
        assign_if_missing(a, "Emulators", "Work:Emulators");
        assign_if_missing(a, "Premium", "Games:Premium");
        {
            char raw[128];
            get_env("whdlsaves", raw, sizeof(raw));
            ags_unquote_path(raw, saves, sizeof(saves));
            assign_if_missing(a, "WHDSaves", saves);
        }
    }
    get_env("HW", a->hw, sizeof(a->hw));
    get_env("ADULT", a->adult, sizeof(a->adult));
    get_env("EXPERTPERM", a->expertperm, sizeof(a->expertperm));
    if (a->found)
        ags_read_dir(a->root, &a->top, a->adult[0] == '1');
    me->pr_WindowPtr = oldwin;
    return a->found;
}

void ags_free(struct Ags *a)
{
    list_free(&a->top);
}

/* ------------------------------------------------------------ entries */

/* Read an entry's fact sheet: trimmed lines, blank runs collapsed. */
LONG ags_read_txt(const char *base, char lines[][96], LONG maxlines)
{
    char path[300], buf[2048];
    BPTR fh;
    LONG len, n = 0, i = 0;
    BOOL last_blank = TRUE;

    amce_strlcpy(path, base, sizeof(path));
    amce_strlcat(path, ".txt", sizeof(path));
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return 0;
    len = Read(fh, buf, sizeof(buf) - 1);
    Close(fh);
    if (len <= 0)
        return 0;
    buf[len] = 0;
    while (i < len && n < maxlines) {
        char line[160];
        LONG k = 0, s, e;
        while (i < len && buf[i] != '\n' && buf[i] != '\r') {
            if (k < (LONG)sizeof(line) - 1)
                line[k++] = buf[i] == '\t' ? ' ' : buf[i];
            i++;
        }
        if (i < len && buf[i] == '\r')
            i++;
        if (i < len && buf[i] == '\n')
            i++;
        line[k] = 0;
        for (s = 0; line[s] == ' '; s++)
            ;
        for (e = k; e > s && line[e - 1] == ' '; e--)
            ;
        line[e] = 0;
        if (!line[s]) {
            if (last_blank)
                continue;
            last_blank = TRUE;
        } else {
            last_blank = FALSE;
        }
        amce_strlcpy(lines[n++], line + s, 96);
    }
    while (n > 0 && !lines[n - 1][0])
        n--;
    return n;
}

BOOL ags_screenshot_path(const char *base, char *out, LONG max)
{
    amce_strlcpy(out, base, max);
    amce_strlcat(out, "-1.iff", max);
    if (exists(out))
        return TRUE;
    amce_strlcpy(out, base, max);
    amce_strlcat(out, ".iff", max);
    return exists(out);
}

/* The launch script runs the entry's own .run, as AGS does:
 *  - Play 0 keeps the .run from restarting AGS's background player,
 *    because AmigaMCE plays its own menu music;
 *  - Expert is reset to 0 unless the user made it permanent, exactly as
 *    AGS's Start_AGS does when AGS starts. */
BOOL ags_write_launch_script(const struct Ags *a, const char *base, const char *script)
{
    BPTR fh = Open((CONST_STRPTR)script, MODE_NEWFILE);
    if (!fh)
        return FALSE;
    FPuts(fh, (CONST_STRPTR)"; AmigaMCE launch script - deleted after use\n");
    FPuts(fh, (CONST_STRPTR)"FailAt 21\n");
    FPuts(fh, (CONST_STRPTR)"Path AGSOS:C C: SYS:Utilities SYS:System S: SYS:Prefs SYS:Tools ADD\n");
    FPuts(fh, (CONST_STRPTR)"FailAt 10\n");
    FPuts(fh, (CONST_STRPTR)"Set Play 0\n");
    if (a->expertperm[0] != '1')
        FPuts(fh, (CONST_STRPTR)"Set Expert 0\n");
    FPuts(fh, (CONST_STRPTR)"CD AGS:\n");
    FPuts(fh, (CONST_STRPTR)"Execute \"");
    FPuts(fh, (CONST_STRPTR)base);
    FPuts(fh, (CONST_STRPTR)".run\"\n");
    Close(fh);
    return TRUE;
}
