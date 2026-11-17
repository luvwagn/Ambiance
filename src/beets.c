/*
 * Ambiance - a beets music library (its SQLite database) as Music pages
 * Copyright (c) 2026 Brian King
 *
 * beets (https://beets.io) keeps a music collection's tags in one SQLite 3
 * file. With it in MUSIC: (musiclibrary.db or library.db), Music shows the
 * library by artist and album without walking its folders, which on a
 * network share took minutes: Music > Music library (beets) > Artists,
 * Albums, Recently added, then an artist's albums, then an album's tracks,
 * which are ordinary music files (IT_SONG) and play as Music files do.
 *
 * The database is read through PROGDIR:Libs/ambsql.library (src/sqlib:
 * SQLite 3.34, read only and immutable: nothing is ever written to it),
 * opened only while it is read.
 *
 * The albums are read once into an index of Ambiance's own (id, year,
 * album artist, album), kept in PROGDIR:Cache/beets.idx with the
 * database's size and date and read from there while they are unchanged:
 * reading the albums table took a few seconds of an 030 (its "added" is a
 * float, so it is not read: the newest albums are those with the highest
 * ids). An album's tracks come from the database each time (items has an
 * index on album_id: a few pages).
 *
 * Its paths are the PC's that ran beets (C:\Users\...\Music\Artist\Album\
 * 01 Track.mp3, UTF-8). The part that is MUSIC: is found once, by trying
 * the first track's path from each of its folders down until a file is
 * there; the rest are mapped the same way. A path with characters Latin-1
 * lacks cannot be reached through the emulator's shared folder, and is left
 * out, as are formats Ambiance cannot play (WMA, AAC).
 *
 * Pages are opened by their path (IT_DIR, so the folder machinery opens
 * them): "beets:" the menu, "beets:artists", "beets:albums",
 * "beets:recent", "beets:artist/<name>", "beets:album/<id>".
 */
#include "amb.h"
#include "radiocsv.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ambsql.h>

struct Library *AmbSqlBase;

#ifndef IDX_FILE
#define IDX_FILE  "PROGDIR:Cache/beets.idx"
#endif
#define IDX_MAGIC 0x414D4254UL      /* "AMBT" */
#define IDX_VER   2
#define RECENT    100

struct BAlbum {
    ULONG id;
    UWORD year;
    const char *artist, *album;     /* Ambiance's text (Latin-1, cjk.c's bytes) */
};

static const char *const db_names[] = { "MUSIC:musiclibrary.db", "MUSIC:library.db" };

static struct {
    char db[40];                    /* the database read */
    ULONG size;
    struct DateStamp date;
    UBYTE *buf;                     /* the index file's contents */
    ULONG buf_size;
    struct BAlbum *al;              /* by artist, year, album */
    LONG n;
    LONG *by_name;                  /* album indexes by album name */
} ix;

static char err[160];
static char prefix[260];            /* the start of a beets path that is MUSIC: */
static LONG prefix_len = -1;        /* -1: not found yet */

const char *beets_error(void)
{
    return err;
}

BOOL beets_page(const char *path)
{
    return path && !strncmp(path, "beets:", 6);
}

static APTR quiet(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    return old;
}

static void loud(APTR old)
{
    ((struct Process *)FindTask(NULL))->pr_WindowPtr = old;
}

/* The database in MUSIC:, its size and date: FALSE if there is none. */
static BOOL find_db(char *path, ULONG *size, struct DateStamp *date)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    APTR old = quiet();
    BOOL ok = FALSE;
    unsigned i;
    if (!fib) {
        loud(old);
        return FALSE;
    }
    for (i = 0; i < sizeof(db_names) / sizeof(db_names[0]) && !ok; i++) {
        BPTR l = Lock((CONST_STRPTR)db_names[i], SHARED_LOCK);
        if (!l)
            continue;
        if (Examine(l, fib) && fib->fib_DirEntryType < 0 && fib->fib_Size > 0) {
            strcpy(path, db_names[i]);
            *size = (ULONG)fib->fib_Size;
            *date = fib->fib_Date;
            ok = TRUE;
        }
        UnLock(l);
    }
    loud(old);
    FreeDosObject(DOS_FIB, fib);
    return ok;
}

BOOL beets_there(void)
{
    char p[40];
    ULONG s;
    struct DateStamp d;
    return find_db(p, &s, &d);
}

/* The library and the database: NULL (err said) if either cannot be had. */
static APTR db_open(void)
{
    APTR db;
    if (!AmbSqlBase) {
        APTR old = quiet();
        AmbSqlBase = amb_open_own_lib("ambsql.library", 1);
        loud(old);
        if (!AmbSqlBase) {
            strcpy(err, "ambsql.library");
            return NULL;
        }
    }
    if (!(db = SqlOpen((STRPTR)ix.db))) {
        sprintf(err, "%.60s", (const char *)SqlError(NULL));
        CloseLibrary(AmbSqlBase);
        AmbSqlBase = NULL;
    }
    return db;
}

static void db_close(APTR db)
{
    if (db)
        SqlClose(db);
    if (AmbSqlBase) {
        char m[80];
        sprintf(m, "SQLite memory at its most %lu KB", (unsigned long)(SqlMemory(1) >> 10));
        amb_log("beets", m);
        CloseLibrary(AmbSqlBase);  /* kept in memory: the next album's tracks soon */
    }
    AmbSqlBase = NULL;
}

/* The spinner, as a folder read turns it (dirscan.c). */
static void tick(void)
{
    if (dir_tick && FindTask(NULL) == dir_tick_task)
        dir_tick();
}

/* ---- the index */

static void ix_free(void)
{
    if (ix.buf)
        FreeVec(ix.buf);
    if (ix.al)
        FreeVec(ix.al);
    if (ix.by_name)
        FreeVec(ix.by_name);
    ix.buf = NULL;
    ix.al = NULL;
    ix.by_name = NULL;
    ix.n = 0;
    ix.db[0] = 0;
}

void beets_forget(void)
{
    ix_free();
    prefix_len = -1;
    amb_flush_own_lib("ambsql.library");  /* (Ambiance quits: its 574 KB given back) */
}

static void wr32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);
    p[3] = (UBYTE)v;
}

/* The file: "AMBT", version, the database's size, date (days, minute,
 * tick), 1 if sorted, album count (8 ULONGs); then per album ULONG id,
 * UWORD year, the album artist and the album, each NUL-ended; sorted:
 * the albums by artist, then year, then album, followed by their order
 * by album name (count ULONGs). Sorting them on an 030 took longer than
 * reading them, so it is done once, when the file is made. */
#define HEAD 32

static int cmp_artist(const struct BAlbum *x, const struct BAlbum *y)
{
    int c = amb_stricmp(x->artist, y->artist);
    if (c)
        return c;
    if (x->year != y->year)
        return (int)x->year - (int)y->year;
    return amb_stricmp(x->album, y->album);
}

static int cmp_name(const struct BAlbum *x, const struct BAlbum *y)
{
    int c = amb_stricmp(x->album, y->album);
    return c ? c : amb_stricmp(x->artist, y->artist);
}

/* idx[0..n) sorted by cmp of the albums they name: a merge sort through
 * tmp (libnix's qsort took over ten times as long). */
static void msort(LONG *idx, LONG *tmp, LONG n,
                  int (*cmp)(const struct BAlbum *, const struct BAlbum *))
{
    LONG w, i;
    for (w = 1; w < n; w *= 2) {
        LONG *t;
        for (i = 0; i < n; i += 2 * w) {
            LONG a = i, am = i + w < n ? i + w : n, b = am, bm = i + 2 * w < n ? i + 2 * w : n,
                 o = i;
            while (a < am && b < bm)
                tmp[o++] = cmp(&ix.al[idx[b]], &ix.al[idx[a]]) < 0 ? idx[b++] : idx[a++];
            while (a < am)
                tmp[o++] = idx[a++];
            while (b < bm)
                tmp[o++] = idx[b++];
        }
        t = idx;
        idx = tmp;
        tmp = t;
        if (!(w & 15))
            tick();
    }
}

/* ix.al sorted by artist, and ix.by_name made: FALSE without memory. */
static BOOL ix_sort(void)
{
    LONG n = ix.n, i, *ord, *tmp, *res;
    struct BAlbum *al;
    LONG passes = 0, w;
    for (w = 1; w < n; w *= 2)
        passes++;
    ord = AllocVec(sizeof(LONG) * (n + 1), MEMF_ANY);
    tmp = AllocVec(sizeof(LONG) * (n + 1), MEMF_ANY);
    al = AllocVec(sizeof(struct BAlbum) * (n + 1), MEMF_ANY);
    if (!ord || !tmp || !al) {
        if (ord)
            FreeVec(ord);
        if (tmp)
            FreeVec(tmp);
        if (al)
            FreeVec(al);
        return FALSE;
    }
    for (i = 0; i < n; i++)
        ord[i] = i;
    msort(ord, tmp, n, cmp_artist);
    res = passes & 1 ? tmp : ord;           /* where the last pass put it */
    for (i = 0; i < n; i++)
        al[i] = ix.al[res[i]];
    FreeVec(ix.al);
    ix.al = al;
    for (i = 0; i < n; i++)
        ord[i] = i;
    msort(ord, tmp, n, cmp_name);
    res = passes & 1 ? tmp : ord;
    for (i = 0; i < n; i++)
        ix.by_name[i] = res[i];
    FreeVec(ord);
    FreeVec(tmp);
    return TRUE;
}

/* ix.buf (a file's contents) into ix.al and ix.by_name. */
static BOOL ix_parse(void)
{
    const UBYTE *p = ix.buf + HEAD, *end = ix.buf + ix.buf_size;
    LONG n = (LONG)amb_be32(ix.buf + 28), i;
    BOOL sorted = amb_be32(ix.buf + 24) == 1;
    if (n < 0 || n > 200000 || !(ix.al = AllocVec(sizeof(struct BAlbum) * (n + 1), MEMF_ANY)) ||
        !(ix.by_name = AllocVec(sizeof(LONG) * (n + 1), MEMF_ANY)))
        return FALSE;
    for (i = 0; i < n; i++) {
        struct BAlbum *a = &ix.al[i];
        if (p + 8 > end)
            return FALSE;
        a->id = amb_be32(p);
        a->year = (UWORD)((p[4] << 8) | p[5]);
        p += 6;
        a->artist = (const char *)p;
        while (p < end && *p)
            p++;
        if (++p >= end)
            return FALSE;
        a->album = (const char *)p;
        while (p < end && *p)
            p++;
        if (p++ >= end)
            return FALSE;
        ix.by_name[i] = i;
    }
    ix.n = n;
    if (!sorted)
        return ix_sort();
    for (i = 0; i < n; i++, p += 4) {
        if (p + 4 > end || amb_be32(p) >= (ULONG)n)
            return FALSE;
        ix.by_name[i] = (LONG)amb_be32(p);
    }
    return TRUE;
}

/* The index, sorted, into IDX_FILE for next time (no Cache drawer: no
 * keeping, it is read from the database again). */
static void ix_save(void)
{
    ULONG len = HEAD + 4 * (ULONG)ix.n, o = HEAD;
    LONG i;
    UBYTE *b;
    BPTR fh;
    APTR old;
    for (i = 0; i < ix.n; i++)
        len += 6 + strlen(ix.al[i].artist) + 1 + strlen(ix.al[i].album) + 1;
    if (!(b = AllocVec(len, MEMF_ANY)))
        return;
    CopyMem(ix.buf, b, HEAD);
    wr32(b + 24, 1);
    for (i = 0; i < ix.n; i++) {
        ULONG k;
        wr32(b + o, ix.al[i].id);
        b[o + 4] = (UBYTE)(ix.al[i].year >> 8);
        b[o + 5] = (UBYTE)ix.al[i].year;
        o += 6;
        k = strlen(ix.al[i].artist) + 1;
        CopyMem((APTR)ix.al[i].artist, b + o, k);
        o += k;
        k = strlen(ix.al[i].album) + 1;
        CopyMem((APTR)ix.al[i].album, b + o, k);
        o += k;
    }
    for (i = 0; i < ix.n; i++, o += 4)
        wr32(b + o, (ULONG)ix.by_name[i]);
    old = quiet();
    fh = Open((CONST_STRPTR)IDX_FILE, MODE_NEWFILE);
    loud(old);
    if (fh) {
        LONG w = Write(fh, b, len);
        Close(fh);
        if (w != (LONG)len)
            DeleteFile((CONST_STRPTR)IDX_FILE);
    }
    FreeVec(b);
}

static BOOL ix_read_file(ULONG size, const struct DateStamp *date)
{
    BPTR fh;
    LONG len;
    APTR old = quiet();
    fh = Open((CONST_STRPTR)IDX_FILE, MODE_OLDFILE);
    loud(old);
    if (!fh)
        return FALSE;
    Seek(fh, 0, OFFSET_END);
    len = Seek(fh, 0, OFFSET_BEGINNING);
    if (len > HEAD && len < 8000000 && (ix.buf = AllocVec(len + 1, MEMF_ANY)) &&
        Read(fh, ix.buf, len) == len) {
        ix.buf_size = (ULONG)len;
        ix.buf[len] = 0;
        Close(fh);
        if (amb_be32(ix.buf) == IDX_MAGIC && amb_be32(ix.buf + 4) == IDX_VER && amb_be32(ix.buf + 8) == size &&
            amb_be32(ix.buf + 12) == (ULONG)date->ds_Days && amb_be32(ix.buf + 16) == (ULONG)date->ds_Minute &&
            amb_be32(ix.buf + 20) == (ULONG)date->ds_Tick && ix_parse())
            return TRUE;
        ix_free();
        return FALSE;
    }
    Close(fh);
    ix_free();
    return FALSE;
}

/* A growing buffer for the index while it is built. */
static BOOL put(UBYTE **b, ULONG *len, ULONG *cap, const void *d, ULONG n)
{
    if (*len + n > *cap) {
        ULONG nc = *cap ? *cap * 2 : 65536;
        UBYTE *nb;
        while (nc < *len + n)
            nc *= 2;
        if (!(nb = AllocVec(nc, MEMF_ANY)))
            return FALSE;
        if (*b) {
            CopyMem(*b, nb, *len);
            FreeVec(*b);
        }
        *b = nb;
        *cap = nc;
    }
    CopyMem((APTR)d, *b + *len, n);
    *len += n;
    return TRUE;
}

/* A text column as Ambiance's text, at most max - 1 bytes. */
static void text_of(APTR st, LONG col, char *out, LONG max)
{
    STRPTR t = SqlText(st, col);
    amb_strlcpy(out, t ? (const char *)t : "", max);
    utf8_to_latin1(out);
}

static BOOL ix_build(ULONG size, const struct DateStamp *date)
{
    APTR db, st;
    UBYTE *b = NULL, hd[HEAD];
    ULONG len = 0, cap = 0, n = 0, t0 = time_ms();
    LONG r;
    BOOL ok = FALSE;
    if (!(db = db_open()))
        return FALSE;
    memset(hd, 0, sizeof(hd));
    if (!put(&b, &len, &cap, hd, HEAD))
        goto out;
    st = SqlPrepare(db, (STRPTR) "SELECT id, year, albumartist, album FROM albums");
    if (!st) {
        sprintf(err, "%.60s", (const char *)SqlError(db));
        goto out;
    }
    while ((r = SqlStep(st)) > 0) {
        char artist[120], album[160];
        UBYTE rec[6];
        LONG y = SqlInt(st, 1);
        wr32(rec, (ULONG)SqlInt(st, 0));
        rec[4] = (UBYTE)((y > 0 && y < 65536 ? y : 0) >> 8);
        rec[5] = (UBYTE)(y > 0 && y < 65536 ? y : 0);
        text_of(st, 2, artist, sizeof(artist));
        text_of(st, 3, album, sizeof(album));
        if (!put(&b, &len, &cap, rec, 6) || !put(&b, &len, &cap, artist, strlen(artist) + 1) ||
            !put(&b, &len, &cap, album, strlen(album) + 1)) {
            SqlFinalize(st);
            strcpy(err, "memory");
            goto out;
        }
        if (!(++n & 31))
            tick();
    }
    if (r < 0)
        sprintf(err, "%.60s", (const char *)SqlError(db));
    SqlFinalize(st);
    if (r < 0)
        goto out;
    wr32(b, IDX_MAGIC);
    wr32(b + 4, IDX_VER);
    wr32(b + 8, size);
    wr32(b + 12, (ULONG)date->ds_Days);
    wr32(b + 16, (ULONG)date->ds_Minute);
    wr32(b + 20, (ULONG)date->ds_Tick);
    wr32(b + 28, n);
    ix.buf = b;
    ix.buf_size = len;
    b = NULL;
    ok = ix_parse();
    if (ok) {
        ix_save();
        char m[120];
        sprintf(m, "%ld albums read from %s in %lu ms", (long)ix.n, ix.db,
                (unsigned long)(time_ms() - t0));
        amb_log("beets", m);
    } else {
        ix_free();
        strcpy(err, "memory");
    }
out:
    if (b)
        FreeVec(b);
    db_close(db);
    return ok;
}

/* The index, read or made: FALSE (err said) if it cannot be had. */
static BOOL ix_have(void)
{
    char db[40];
    ULONG size;
    struct DateStamp date;
    if (!find_db(db, &size, &date)) {
        strcpy(err, "MUSIC:musiclibrary.db");
        return FALSE;
    }
    if (ix.al && !strcmp(db, ix.db) && size == ix.size && !CompareDates(&date, &ix.date))
        return TRUE;
    ix_free();
    strcpy(ix.db, db);
    ix.size = size;
    ix.date = date;
    if (ix_read_file(size, &date)) {
        char m[80];
        sprintf(m, "%ld albums, from %s", (long)ix.n, IDX_FILE);
        amb_log("beets", m);
        strcpy(ix.db, db);
        return TRUE;
    }
    strcpy(ix.db, db);
    return ix_build(size, &date);
}

/* ---- the pages */

static void add_dir(struct ItemList *out, const char *label, const char *path, const char *key)
{
    struct Item *it = list_add(out, label[0] ? label : _("(no name)"), path, key, IT_DIR);
    if (it)
        it->mark = MARK_ARROW;
}

static LONG artists(struct ItemList *out)
{
    LONG i;
    char path[140];
    for (i = 0; i < ix.n; i++) {
        if (i && !amb_stricmp(ix.al[i].artist, ix.al[i - 1].artist))
            continue;
        sprintf(path, "beets:artist/%.120s", ix.al[i].artist);
        add_dir(out, ix.al[i].artist, path, "");
    }
    return out->count;
}

static void album_row(struct ItemList *out, const struct BAlbum *a, BOOL with_artist)
{
    char label[300], path[40];
    const char *al = a->album[0] ? a->album : _("(no name)");
    if (with_artist && a->artist[0])
        sprintf(label, "%.150s - %.120s", al, a->artist);
    else if (a->year)
        sprintf(label, "%.150s (%lu)", al, (unsigned long)a->year);
    else
        sprintf(label, "%.150s", al);
    sprintf(path, "beets:album/%lu", (unsigned long)a->id);
    add_dir(out, label, path, "");
}

static LONG artist_albums(const char *name, struct ItemList *out)
{
    LONG i;
    for (i = 0; i < ix.n; i++)
        if (!amb_stricmp(ix.al[i].artist, name))
            album_row(out, &ix.al[i], FALSE);
    return out->count;
}

static LONG all_albums(struct ItemList *out)
{
    LONG i;
    for (i = 0; i < ix.n; i++)
        album_row(out, &ix.al[ix.by_name[i]], TRUE);
    return out->count;
}

static LONG recent(struct ItemList *out)
{
    /* the highest ids: beets numbers albums as they are added */
    LONG k, i;
    ULONG below = 0xFFFFFFFFUL;
    for (k = 0; k < RECENT && k < ix.n; k++) {
        LONG best = -1;
        for (i = 0; i < ix.n; i++)
            if (ix.al[i].id < below && (best < 0 || ix.al[i].id > ix.al[best].id))
                best = i;
        if (best < 0)
            break;
        below = ix.al[best].id;
        album_row(out, &ix.al[best], TRUE);
    }
    return out->count;
}

/* A beets path (UTF-8, \ or /) as Latin-1 with /: FALSE if it has a
 * character Latin-1 lacks. */
static BOOL path_latin1(const UBYTE *s, char *out, LONG max)
{
    LONG o = 0;
    while (*s && o < max - 1) {
        ULONG c = *s++;
        if (c >= 0x80) {
            if ((c & 0xE0) != 0xC0 || (*s & 0xC0) != 0x80)
                return FALSE;
            c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
            if (c > 0xFF)
                return FALSE;
        }
        out[o++] = c == '\\' ? '/' : (char)c;
    }
    out[o] = 0;
    return !*s;
}

/* p (Latin-1, /) as the file in MUSIC:, into out: the part before it
 * found once by trying p from each of its folders down. */
static BOOL music_path(const char *p, char *out, LONG max)
{
    const char *s;
    if (prefix_len >= 0 && !strncmp(p, prefix, prefix_len)) {
        sprintf(out, "MUSIC:%.*s", (int)(max - 8), p + prefix_len);
        return TRUE;
    }
    for (s = p; (s = strchr(s, '/')); s++) {
        sprintf(out, "MUSIC:%.*s", (int)(max - 8), s + 1);
        if (amb_exists(out)) {
            prefix_len = (LONG)(s + 1 - p);
            if (prefix_len >= (LONG)sizeof(prefix))
                prefix_len = -1;
            else {
                char m[300];
                memcpy(prefix, p, prefix_len);
                prefix[prefix_len] = 0;
                sprintf(m, "%.200s is MUSIC:", prefix);
                amb_log("beets", m);
            }
            return TRUE;
        }
    }
    return FALSE;
}

static LONG album_tracks(ULONG id, struct ItemList *out)
{
    APTR db, st;
    LONG r, left_out = 0, n = 0;
    char num[16], albumartist[120] = "";
    LONG i;
    for (i = 0; i < ix.n; i++)
        if (ix.al[i].id == id) {
            amb_strlcpy(albumartist, ix.al[i].artist, sizeof(albumartist));
            break;
        }
    if (!(db = db_open()))
        return -1;
    st = SqlPrepare(db, (STRPTR) "SELECT title, path, artist FROM items WHERE album_id = ?1 "
                                 "ORDER BY disc, track");
    if (!st) {
        sprintf(err, "%.60s", (const char *)SqlError(db));
        db_close(db);
        return -1;
    }
    sprintf(num, "%lu", (unsigned long)id);
    SqlBindText(st, 1, (STRPTR)num);
    while ((r = SqlStep(st)) > 0) {
        char title[160], artist[120], raw[300], file[300], key[16];
        STRPTR pt = SqlText(st, 1);
        const char *name;
        n++;
        if (!pt || !path_latin1(pt, raw, sizeof(raw)) ||
            !song_name_ok((name = (const char *)FilePart((STRPTR)raw))) ||
            !music_path(raw, file, sizeof(file))) {
            left_out++;
            continue;
        }
        text_of(st, 0, title, sizeof(title));
        text_of(st, 2, artist, sizeof(artist));
        if (!title[0])
            amb_strlcpy(title, name, sizeof(title));
        if (artist[0] && amb_stricmp(artist, albumartist)) {
            amb_strlcat(title, " - ", sizeof(title));
            amb_strlcat(title, artist, sizeof(title));
        }
        sprintf(key, "%04ld", (long)out->count);  /* the album's order, kept */
        list_add(out, title, file, key, IT_SONG);
    }
    if (r < 0)
        sprintf(err, "%.60s", (const char *)SqlError(db));
    SqlFinalize(st);
    db_close(db);
    if (left_out) {
        char m[120];
        sprintf(m, "album %lu: %ld of %ld tracks left out (format, or a name Latin-1 lacks)",
                (unsigned long)id, (long)left_out, (long)n);
        amb_log("beets", m);
    }
    if (r < 0)
        return -1;
    if (!out->count)
        list_add(out, _("Nothing here"), "", "", IT_TEXT);
    return out->count;
}

/* The page for path into out: its rows, -1 (beets_error() says why). */
LONG beets_read(const char *path, struct ItemList *out)
{
    const char *w = path + 6;
    err[0] = 0;
    if (!*w) {
        static const char *const menu[][2] = {
            { N_("Artists"), "beets:artists" }, { N_("Albums"), "beets:albums" },
            { N_("Recently added"), "beets:recent" },
        };
        unsigned i;
        if (!beets_there()) {
            strcpy(err, "MUSIC:musiclibrary.db");
            return -1;
        }
        for (i = 0; i < sizeof(menu) / sizeof(menu[0]); i++)
            add_dir(out, _(menu[i][0]), menu[i][1], "");
        return out->count;
    }
    if (!ix_have())
        return -1;
    if (!strcmp(w, "artists"))
        return artists(out);
    if (!strcmp(w, "albums"))
        return all_albums(out);
    if (!strcmp(w, "recent"))
        return recent(out);
    if (!strncmp(w, "artist/", 7))
        return artist_albums(w + 7, out);
    if (!strncmp(w, "album/", 6))
        return album_tracks(strtoul(w + 6, NULL, 10), out);
    amb_strlcpy(err, path, sizeof(err));
    return -1;
}
