/*
 * Ambiance - iGame's list of games and demos (see igame.h)
 * Copyright (c) 2026 Brian King
 *
 * iGame (George Sokianos, GPL-3.0) keeps everything it found in one file
 * in its own drawer, gameslist.csv (igamecsv.c reads a line). Ambiance
 * reads that file and nothing of iGame's code: no folders are walked, so
 * no index is kept. The screenshot is igame.iff in the game's folder, as
 * iGame shows it; igame.data there may name who made it. A WHDLoad entry
 * is started as iGame starts it: WHDLoad with its icon's tooltypes as
 * arguments, from the game's folder. The list is only read: play counts
 * stay iGame's.
 */
#include "igame.h"
#include "igamecsv.h"

#include <stdio.h>
#include <string.h>
#include <dos/dosextens.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>

extern struct Library *IconBase;        /* (main.c's; opened here while needed) */

#define LIST_FILE "gameslist.csv"

BOOL ig_folder_ok(const char *dir)
{
    char f[300];
    if (!dir || !dir[0])
        return FALSE;
    amb_join_path(f, sizeof(f), dir, LIST_FILE);
    return amb_exists(f);
}

/* ------------------------------------------------------------ finding it
 * iGame's installer asks where to put it (RAM: offered), and nothing
 * records where it went. Looked for, in order: an iGame: assign; the
 * usual drawers; then (deep) on each hard disk's volume, iGame at its top
 * or one drawer down (Work:Utilities/iGame). Floppies (DF0:-DF3:) are not
 * read: a disk's root takes seconds. Only while the setting is unset. */

struct Tops {
    char vol[40];
    char names[48][32];
    LONG n;
};

static void top_fn(void *ctx, const char *name, BOOL isdir)
{
    struct Tops *t = ctx;
    if (isdir && t->n < 48 && strlen(name) < sizeof(t->names[0]))
        strcpy(t->names[t->n++], name);
}

BOOL ig_discover(char *out, LONG max, BOOL deep)
{
    static const char *const usual[] = {
        "iGame:", "SYS:iGame", "SYS:Utilities/iGame", "SYS:Tools/iGame", "SYS:Games/iGame",
        "Work:iGame", "Work:Utilities/iGame", "Work:Tools/iGame", "Work:Games/iGame",
        "Games:iGame", "WHD:iGame", "WHDLoad:iGame", "PROGDIR:/iGame", NULL };
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    struct MsgPort *ports[12];
    char vols[12][40], path[300];
    LONG nv = 0, i, k;
    BOOL found = FALSE;
    me->pr_WindowPtr = (APTR)-1;
    for (i = 0; usual[i] && !found; i++)
        if (ig_folder_ok(usual[i])) {
            amb_strlcpy(out, usual[i], max);
            found = TRUE;
        }
    if (!found && deep) {
        /* volumes with a disk in (a volume with no handler is not in a
         * drive), then those whose device is a floppy drive left out */
        struct DosList *dl = LockDosList(LDF_VOLUMES | LDF_DEVICES | LDF_READ);
        struct DosList *v = dl;
        while ((v = NextDosEntry(v, LDF_VOLUMES)) != NULL && nv < 12) {
            const UBYTE *b = (const UBYTE *)BADDR(v->dol_Name);
            if (v->dol_Task && b && b[0] && b[0] < sizeof(vols[0]) - 1) {
                memcpy(vols[nv], b + 1, b[0]);
                vols[nv][b[0]] = ':';
                vols[nv][b[0] + 1] = 0;
                ports[nv++] = v->dol_Task;
            }
        }
        v = dl;
        while ((v = NextDosEntry(v, LDF_DEVICES)) != NULL) {
            const UBYTE *b = (const UBYTE *)BADDR(v->dol_Name);
            if (v->dol_Task && b && b[0] == 3 && (b[1] == 'D' || b[1] == 'd') &&
                (b[2] == 'F' || b[2] == 'f') && b[3] >= '0' && b[3] <= '3')
                for (i = 0; i < nv; i++)
                    if (ports[i] == v->dol_Task)
                        vols[i][0] = 0;     /* a floppy's */
        }
        UnLockDosList(LDF_VOLUMES | LDF_DEVICES | LDF_READ);
    }
    for (i = 0; i < nv && !found; i++) {
        static struct Tops t;               /* (1.5 KB: off the stack) */
        if (!vols[i][0] || !amb_stricmp(vols[i], "RAM Disk:"))
            continue;
        amb_join_path(path, sizeof(path), vols[i], "iGame");
        if (ig_folder_ok(path)) {
            amb_strlcpy(out, path, max);
            found = TRUE;
            break;
        }
        t.n = 0;
        if (dir_scan(vols[i], top_fn, &t) < 0)
            continue;
        for (k = 0; k < t.n && !found; k++) {
            char d[300];
            amb_join_path(d, sizeof(d), vols[i], t.names[k]);
            if (!amb_stricmp(t.names[k], "iGame"))
                continue;                   /* (looked at above) */
            amb_join_path(path, sizeof(path), d, "iGame");
            if (ig_folder_ok(path)) {
                amb_strlcpy(out, path, max);
                found = TRUE;
            }
        }
    }
    me->pr_WindowPtr = oldwin;
    {
        char m[320];
        sprintf(m, "%.300s", found ? out : deep ? "not found (assign, usual drawers, hard disks' tops)"
                                              : "not found (assign, usual drawers)");
        amb_log("igame: looked for iGame", m);
    }
    return found;
}

/* ------------------------------------------------------------ reading */

static char *pool_copy(APTR pool, const char *s)
{
    LONG n = strlen(s) + 1;
    char *d = AllocPooled(pool, n);
    if (d)
        memcpy(d, s, n);
    return d;
}

/* One copy of each genre and chipset name. */
static const char *intern(struct IgCat *c, const char *s)
{
    LONG i;
    for (i = 0; i < c->nnames; i++)
        if (!strcmp(c->names[i], s))
            return c->names[i];
    if (c->nnames == c->capnames) {
        LONG cap = c->capnames ? c->capnames * 2 : 64;
        const char **n = AllocPooled(c->pool, cap * sizeof(*n));
        if (!n)
            return "";
        if (c->nnames)
            memcpy(n, c->names, c->nnames * sizeof(*n));
        c->names = n;                       /* (the old array stays in the pool) */
        c->capnames = cap;
    }
    if (!(c->names[c->nnames] = pool_copy(c->pool, s)))
        return "";
    return c->names[c->nnames++];
}

void ig_free(struct IgCat *c)
{
    if (c->g)
        FreeVec(c->g);
    if (c->pool)
        DeletePool(c->pool);
    memset(c, 0, sizeof(*c));
}

LONG ig_load(struct IgCat *c, const char *dir)
{
    char f[300], m[300];
    ULONG len = 0;
    UBYTE *buf;
    char *p, *e;
    LONG lines = 0, skipped = 0;
    ULONG t0 = time_ms();
    ig_free(c);
    amb_join_path(f, sizeof(f), dir, LIST_FILE);
    if (!(buf = unpack_read_whole(f, &len))) {
        amb_log("igame: cannot read", f);
        return -1;
    }
    if (!(c->pool = CreatePool(MEMF_ANY, 16384, 8192))) {
        FreeVec(buf);
        return -1;
    }
    amb_strlcpy(c->dir, dir, sizeof(c->dir));
    for (p = (char *)buf; p < (char *)buf + len && *p; p = e) {
        struct IgRow r;
        for (e = p; *e && *e != '\n'; e++)
            ;
        if (*e)
            *e++ = 0;
        lines++;
        if (!ig_parse_line(p, &r))
            continue;
        if (r.hidden || r.deleted) {
            skipped++;
            continue;
        }
        if (c->n == c->cap) {
            LONG cap = c->cap ? c->cap * 2 : 256;
            struct IgGame *n = AllocVec(cap * sizeof(*n), MEMF_ANY);
            if (!n)
                break;
            if (c->n)
                CopyMem(c->g, n, c->n * sizeof(*n));
            if (c->g)
                FreeVec(c->g);
            c->g = n;
            c->cap = cap;
        }
        {
            struct IgGame *gm = &c->g[c->n];
            if (!(gm->title = pool_copy(c->pool, ig_row_title(&r))) ||
                !(gm->path = pool_copy(c->pool, r.path)))
                break;
            gm->genre = intern(c, r.genre);
            gm->chipset = intern(c, r.chipset);
            gm->year = r.year > 0 && r.year < 3000 ? (UWORD)r.year : 0;
            gm->played = r.times_played > 0 ? (r.times_played > 65535 ? 65535 : (UWORD)r.times_played) : 0;
            gm->players = r.players > 0 && r.players < 256 ? (UBYTE)r.players : 0;
            gm->flags = (r.favourite ? IGF_FAV : 0) |
                        (ig_is_demo(r.genre, r.path) ? IGF_DEMO : 0) |
                        (amb_ends_with(r.path, ".slave") ? IGF_SLAVE : 0);
            if (gm->flags & IGF_DEMO)
                c->demos++;
            else
                c->games++;
            c->n++;
        }
    }
    FreeVec(buf);
    sprintf(m, "%ld games, %ld demos (%ld lines, %ld hidden or gone) from %.120s in %lu ms",
            (long)c->games, (long)c->demos, (long)lines, (long)skipped, f,
            (unsigned long)(time_ms() - t0));
    amb_log("igame", m);
    return c->n;
}

LONG ig_count(const struct IgCat *c, BOOL demos)
{
    return demos ? c->demos : c->games;
}

/* ------------------------------------------------------------ pages
 * A page's path: IG_PREFIX, 'G' or 'D', ':', and what it lists:
 *   root  all  fav  most  genres  g=<genre>  chips  c=<chipset>
 *   years  y=<year> */

BOOL ig_page(const char *path)
{
    return path && !strncmp(path, IG_PREFIX, sizeof(IG_PREFIX) - 1);
}

void ig_page_path(char *out, LONG max, BOOL demos, const char *query)
{
    amb_strlcpy(out, IG_PREFIX, max);
    amb_strlcat(out, demos ? "D:" : "G:", max);
    amb_strlcat(out, query, max);
}

static BOOL wanted(const struct IgGame *gm, BOOL demos)
{
    return !(gm->flags & IGF_DEMO) == !demos;
}

static void add_folder(struct ItemList *out, BOOL demos, const char *label, const char *query)
{
    char path[200];
    ig_page_path(path, sizeof(path), demos, query);
    list_add(out, label, path, label, IT_DIR);
}

static void add_game(struct ItemList *out, const struct IgGame *gm, const char *sortkey)
{
    list_add(out, gm->title, gm->path, sortkey ? sortkey : gm->title, IT_RUN);
}

/* The distinct genres (what 'g'), chipsets ('c') or years ('y') of the
 * entries wanted, as folders. */
static LONG add_groups(const struct IgCat *c, struct ItemList *out, BOOL demos, char what)
{
    LONG i, k, n = 0;
    for (i = 0; i < c->n; i++) {
        const struct IgGame *gm = &c->g[i];
        char q[120], label[100];
        BOOL seen = FALSE;
        if (!wanted(gm, demos))
            continue;
        if (what == 'c' && !gm->chipset[0])
            continue;
        if (what == 'y' && !gm->year)
            continue;
        for (k = 0; k < i && !seen; k++) {
            const struct IgGame *o = &c->g[k];
            if (!wanted(o, demos))
                continue;
            seen = what == 'g' ? o->genre == gm->genre
                 : what == 'c' ? o->chipset == gm->chipset
                 : o->year == gm->year;
        }
        if (seen)
            continue;
        if (what == 'y') {
            sprintf(label, "%lu", (unsigned long)gm->year);
            sprintf(q, "y=%lu", (unsigned long)gm->year);
        } else {
            const char *s = what == 'g' ? gm->genre : gm->chipset;
            amb_strlcpy(label, s[0] ? s : _("Unknown"), sizeof(label));
            q[0] = what;
            q[1] = '=';
            amb_strlcpy(q + 2, s, sizeof(q) - 2);
        }
        add_folder(out, demos, label, q);
        n++;
    }
    list_sort(out);
    return n;
}

LONG ig_read(const struct IgCat *c, const char *page, struct ItemList *out, BOOL *ordered)
{
    const char *q;
    BOOL demos;
    LONG i, n = 0;
    *ordered = FALSE;
    if (!ig_page(page) || !c->n)
        return -1;
    q = page + sizeof(IG_PREFIX) - 1;
    demos = q[0] == 'D';
    q += 2;
    if (!strcmp(q, "root")) {
        LONG fav = 0, played = 0, genres = 0, chips = 0, years = 0;
        const char *g1 = NULL;
        for (i = 0; i < c->n; i++) {
            const struct IgGame *gm = &c->g[i];
            if (!wanted(gm, demos))
                continue;
            n++;
            fav += (gm->flags & IGF_FAV) != 0;
            played += gm->played != 0;
            if (!g1)
                g1 = gm->genre;
            else if (gm->genre != g1)
                genres = 1;
            chips += gm->chipset[0] != 0;
            years += gm->year != 0;
        }
        if (!n)
            return 0;
        if (fav)
            add_folder(out, demos, _("Favourites"), "fav");
        add_folder(out, demos, demos ? _("All demos") : _("All games"), "all");
        if (played)
            add_folder(out, demos, _("Most played"), "most");
        if (genres)
            add_folder(out, demos, _("By genre"), "genres");
        if (chips)
            add_folder(out, demos, _("By chipset"), "chips");
        if (years)
            add_folder(out, demos, _("By year"), "years");
        return out->count;
    }
    if (!strcmp(q, "genres"))
        return add_groups(c, out, demos, 'g');
    if (!strcmp(q, "chips"))
        return add_groups(c, out, demos, 'c');
    if (!strcmp(q, "years"))
        return add_groups(c, out, demos, 'y');
    for (i = 0; i < c->n; i++) {
        const struct IgGame *gm = &c->g[i];
        char key[120];
        if (!wanted(gm, demos))
            continue;
        if (!strcmp(q, "all"))
            add_game(out, gm, NULL);
        else if (!strcmp(q, "fav")) {
            if (gm->flags & IGF_FAV)
                add_game(out, gm, NULL);
        } else if (!strcmp(q, "most")) {
            if (gm->played) {
                /* most played first, then by title */
                sprintf(key, "%05lu", (unsigned long)(65535 - gm->played));
                amb_strlcat(key, gm->title, sizeof(key));
                add_game(out, gm, key);
            }
        } else if (q[0] == 'g' && q[1] == '=') {
            if (!strcmp(gm->genre, q + 2))
                add_game(out, gm, NULL);
        } else if (q[0] == 'c' && q[1] == '=') {
            if (!strcmp(gm->chipset, q + 2))
                add_game(out, gm, NULL);
        } else if (q[0] == 'y' && q[1] == '=') {
            if (gm->year == (UWORD)amb_num(q + 2))
                add_game(out, gm, NULL);
        } else
            return -1;
    }
    list_sort(out);
    *ordered = !strcmp(q, "most");
    return out->count;
}

/* ------------------------------------------------------------ one entry */

const struct IgGame *ig_lookup(const struct IgCat *c, const char *path)
{
    static const struct IgCat *lc;
    static LONG li = -1;
    LONG i;
    if (!path || !c->n)
        return NULL;
    if (lc == c && li >= 0 && li < c->n && !strcmp(c->g[li].path, path))
        return &c->g[li];
    for (i = 0; i < c->n; i++)
        if (!strcmp(c->g[i].path, path)) {
            lc = c;
            li = i;
            return &c->g[i];
        }
    return NULL;
}

void ig_shot_base(const char *path, char *out, LONG max)
{
    char dir[256], name[108];
    ig_split_path(path, dir, sizeof(dir), name, sizeof(name));
    amb_join_path(out, max, dir, "igame");    /* + ".iff": iGame's screenshot */
}

/* igame.data's "by" (who made it), read once per entry asked about. */
static const char *who_made(const char *path)
{
    static char last[256], by[80];
    char dir[256], name[108], f[300];
    ULONG len = 0;
    UBYTE *buf;
    char *p, *e;
    if (!strcmp(last, path))
        return by;
    amb_strlcpy(last, path, sizeof(last));
    by[0] = 0;
    ig_split_path(path, dir, sizeof(dir), name, sizeof(name));
    amb_join_path(f, sizeof(f), dir, "igame.data");
    if (!amb_exists(f) || !(buf = unpack_read_whole(f, &len)))
        return by;
    for (p = (char *)buf; *p; p = e) {
        for (e = p; *e && *e != '\n'; e++)
            ;
        if (*e)
            *e++ = 0;
        if (!amb_strnicmp(p, "by=", 3)) {
            char *s = p + 3;
            LONG n = strlen(s);
            while (n && (s[n - 1] == '\r' || s[n - 1] == ' '))
                s[--n] = 0;
            amb_strlcpy(by, s, sizeof(by));
            break;
        }
    }
    FreeVec(buf);
    return by;
}

LONG ig_facts(const struct IgGame *gm, char lines[][96], LONG maxlines)
{
    char dir[256], name[108], t[300];
    const char *by;
    LONG n = 0;
    /* (sprintf is RawDoFmt's: numbers as longs; then cut to a line) */
#define LINE(...) do { if (n < maxlines) { sprintf(t, __VA_ARGS__); amb_strlcpy(lines[n], t, 96); n++; } } while (0)
    if (!gm)
        return 0;
    LINE("%.200s", gm->title);
    by = who_made(gm->path);
    if (by[0])
        LINE(_("By %.80s"), by);
    LINE("%s", "");
    if (gm->genre[0])
        LINE(_("Genre: %.80s"), gm->genre);
    if (gm->year)
        LINE(_("Year: %lu"), (unsigned long)gm->year);
    if (gm->players)
        LINE(_("Players: %lu"), (unsigned long)gm->players);
    if (gm->chipset[0])
        LINE(_("Chipset: %.80s"), gm->chipset);
    if (gm->played)
        LINE(gm->played == 1 ? _("Played once") : _("Played %lu times"), (unsigned long)gm->played);
    if (gm->flags & IGF_FAV)
        LINE("%s", _("A favourite in iGame"));
    ig_split_path(gm->path, dir, sizeof(dir), name, sizeof(name));
    LINE((gm->flags & IGF_SLAVE) ? _("WHDLoad: %s") : _("Program: %s"), name);
#undef LINE
    return n;
}

/* ------------------------------------------------------------ starting */

struct Infos {
    char names[24][108];
    LONG n;
};

static void info_fn(void *ctx, const char *name, BOOL isdir)
{
    struct Infos *t = ctx;
    LONG n = strlen(name);
    if (!isdir && n > 5 && n < (LONG)sizeof(t->names[0]) && !amb_stricmp(name + n - 5, ".info") &&
        t->n < 24) {
        memcpy(t->names[t->n], name, n - 5);
        t->names[t->n][n - 5] = 0;
        t->n++;
    }
}

/* WHDLoad's arguments from the icon in dir whose SLAVE names this slave
 * (the slave's own name first, then any other): as iGame passes them.
 * FALSE: no such icon. */
static BOOL icon_args(const char *dir, const char *slave, char *args, LONG max)
{
    static struct Infos t;                  /* (2.6 KB: off the stack) */
    BOOL opened = FALSE, found = FALSE;
    BPTR lock, old;
    LONG i;
    args[0] = 0;
    t.n = 0;
    if (dir_scan(dir, info_fn, &t) < 0 || !t.n)
        return FALSE;
    if (!IconBase) {
        if (!(IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 37)))
            return FALSE;
        opened = TRUE;
    }
    if (!(lock = Lock((CONST_STRPTR)dir, ACCESS_READ))) {
        if (opened) {
            CloseLibrary(IconBase);
            IconBase = NULL;
        }
        return FALSE;
    }
    old = CurrentDir(lock);
    for (i = 0; i < t.n && !found; i++) {
        struct DiskObject *d = GetDiskObject((CONST_STRPTR)t.names[i]);
        STRPTR *tt;
        STRPTR s;
        if (!d)
            continue;
        tt = (STRPTR *)d->do_ToolTypes;
        if (d->do_Type == WBPROJECT && tt &&
            (s = FindToolType((CONST_STRPTR *)tt, (CONST_STRPTR)"SLAVE")) != NULL &&
            !amb_stricmp((const char *)s, slave)) {
            LONG k;
            found = TRUE;
            for (k = 0; tt[k]; k++) {
                char a[200];
                int r = ig_tooltype_arg((const char *)tt[k], a, sizeof(a));
                if (r < 0)
                    break;
                if (r > 0 && (LONG)(strlen(args) + 1 + strlen(a)) < max) {
                    if (args[0])
                        amb_strlcat(args, " ", max);
                    amb_strlcat(args, a, max);
                }
            }
        }
        FreeDiskObject(d);
    }
    CurrentDir(old);
    UnLock(lock);
    if (opened) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
    return found;
}

BOOL ig_write_launch_script(const struct IgGame *gm, const char *script, char *why, LONG wmax)
{
    char dir[256], name[108], args[400], line[480];
    BPTR fh;
    why[0] = 0;
    ig_split_path(gm->path, dir, sizeof(dir), name, sizeof(name));
    if (!amb_exists(gm->path)) {
        amb_strlcpy(why, _("Its file is not there (rescan in iGame)"), wmax);
        return FALSE;
    }
    if (gm->flags & IGF_SLAVE) {
        if (!icon_args(dir, name, args, sizeof(args)) || !strstr(args, "SLAVE=")) {
            /* no icon for it: the slave alone, WHDLoad's settings for the rest */
            sprintf(args, "SLAVE=\"%.100s\"", name);
        }
        sprintf(line, "WHDLoad %s\n", args);
    } else
        sprintf(line, "\"%.200s\"\n", name);
    if (!(fh = Open((CONST_STRPTR)script, MODE_NEWFILE))) {
        amb_strlcpy(why, _("Could not write T:Ambiance-run"), wmax);
        return FALSE;
    }
    FPuts(fh, (CONST_STRPTR)"; Ambiance launch script (iGame's entry) - deleted after use\n");
    FPuts(fh, (CONST_STRPTR)"FailAt 21\n");
    FPuts(fh, (CONST_STRPTR)"Stack 20000\n");
    FPuts(fh, (CONST_STRPTR)"CD \"");
    FPuts(fh, (CONST_STRPTR)dir);
    FPuts(fh, (CONST_STRPTR)"\"\n");
    FPuts(fh, (CONST_STRPTR)line);
    Close(fh);
    amb_log("igame: launch", line);
    return TRUE;
}
