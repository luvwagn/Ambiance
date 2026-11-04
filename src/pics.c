/*
 * AmigaMCE - Pictures: folders of picture files
 * Copyright (c) 2026 Brian King
 *
 * A Pictures list holds folders (IT_DIR) and picture files (IT_PIC),
 * folders first, each part sorted by name. Once sorted, an entry's sortkey
 * holds the tag shown at the right of its row: a picture's format ("GIF"),
 * or a playable folder's picture count. A folder with pictures and no
 * folders inside is playable (MARK_PLAY: Fire runs its slideshow, Right
 * opens it); one with folders inside opens either way (MARK_ARROW); one
 * with neither is greyed out.
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

static const struct { const char *ext, *tag; } exts[] = {
    { "iff", "ILBM" }, { "ilbm", "ILBM" }, { "lbm", "ILBM" }, { "ham", "ILBM" },
    { "gif", "GIF" }, { "png", "PNG" }, { "jpg", "JPEG" }, { "jpeg", "JPEG" },
    { "jpe", "JPEG" }, { "bmp", "BMP" }, { "pcx", "PCX" }, { "tga", "TGA" },
};

/* The format a picture file's name says it is, NULL if none. */
const char *pics_format(const char *name)
{
    const char *dot = strrchr(name, '.');
    LONG i;
    if (!dot || dot == name)
        return NULL;
    for (i = 0; i < (LONG)(sizeof(exts) / sizeof(exts[0])); i++)
        if (amce_stricmp(dot + 1, exts[i].ext) == 0)
            return exts[i].tag;
    return NULL;
}

/* A name with no dot (cut short by an old file system, or never given
 * one): look at its first bytes. */
static const char *sniff(const char *path)
{
    UBYTE b[12];
    LONG n;
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return NULL;
    n = Read(fh, b, sizeof(b));
    Close(fh);
    if (n < 12)
        return NULL;
    if (!memcmp(b, "FORM", 4) && (!memcmp(b + 8, "ILBM", 4) || !memcmp(b + 8, "PBM ", 4)))
        return "ILBM";
    if (!memcmp(b, "GIF8", 4))
        return "GIF";
    if (b[0] == 0x89 && !memcmp(b + 1, "PNG", 3))
        return "PNG";
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return "JPEG";
    return NULL;
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

/* The format of a directory entry, NULL if it is not a picture. */
static const char *entry_format(const char *dir, const char *name)
{
    char path[300];
    if (strchr(name, '.'))
        return pics_format(name);
    join_path(path, sizeof(path), dir, name);
    return sniff(path);
}

/* No "Please insert volume" requesters while looking (Media: missing). */
static APTR req_off(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    return old;
}

static void req_on(APTR old)
{
    ((struct Process *)FindTask(NULL))->pr_WindowPtr = old;
}

static LONG scan(const char *dir, LONG *ndirs, char *first, LONG firstmax);

LONG pics_scan(const char *dir, LONG *ndirs, char *first, LONG firstmax)
{
    APTR old = req_off();
    LONG n = scan(dir, ndirs, first, firstmax);
    req_on(old);
    return n;
}

static LONG scan(const char *dir, LONG *ndirs, char *first, LONG firstmax)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    LONG npics = 0;
    if (ndirs)
        *ndirs = 0;
    if (first && firstmax)
        first[0] = 0;
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
            if (fib->fib_DirEntryType > 0) {
                if (ndirs)
                    (*ndirs)++;
            } else if (entry_format(dir, name)) {
                npics++;
                if (first && (!first[0] || amce_stricmp(name, first) < 0))
                    amce_strlcpy(first, name, firstmax);
            }
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return npics;
}

static LONG read_dir(const char *dir, struct ItemList *out);

LONG pics_read_dir(const char *dir, struct ItemList *out)
{
    APTR old = req_off();
    LONG n = read_dir(dir, out);
    req_on(old);
    return n;
}

static LONG read_dir(const char *dir, struct ItemList *out)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    char path[300], key[140];
    LONG n = 0, i;

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
            BOOL isdir = fib->fib_DirEntryType > 0;
            if (!isdir && !entry_format(dir, name))
                continue;
            join_path(path, sizeof(path), dir, name);
            key[0] = isdir ? '0' : '1';         /* folders first */
            amce_strlcpy(key + 1, name, sizeof(key) - 1);
            if (list_add(out, name, path, key, isdir ? IT_DIR : IT_PIC))
                n++;
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    list_sort(out);

    /* now the tags (and what each folder holds) */
    for (i = 0; i < out->count; i++) {
        struct Item *it = &out->items[i];
        if (it->kind == IT_PIC) {
            const char *f = entry_format(dir, it->label);
            it->sortkey = (char *)(f ? f : "");
        } else {
            LONG nd, np = pics_scan(it->path, &nd, NULL, 0);
            it->sortkey = (char *)"";
            if (np > 0 && nd == 0) {
                char num[12];
                it->mark = MARK_PLAY;
                sprintf(num, "%ld", (long)np);
                it->sortkey = list_strdup(out, num);
                if (!it->sortkey)
                    it->sortkey = (char *)"";
            } else if (np <= 0 && nd == 0) {
                it->flags |= ITEM_DISABLED;     /* nothing in it to show */
            }
        }
    }
    return n;
}
