/*
 * AmigaMCE - Pictures: folders of picture files
 * Copyright (c) 2026 Brian King
 *
 * A Pictures list holds folders (IT_DIR), picture files (IT_PIC) and IFF
 * ANIMs (IT_ANIM: Videos lists are these lists too, of animations),
 * folders first, each part sorted by name. Once sorted, an entry's sortkey
 * holds the tag shown at the right of its row: a picture's format ("GIF"),
 * or a playable folder's picture count. A folder with pictures and no
 * folders inside is playable (MARK_PLAY: Fire runs its slideshow, Right
 * opens it); one with folders inside opens either way (MARK_ARROW); one
 * with neither is greyed out. Letter folders (A-Z, as Media:Images sorts
 * its game art) are not shown: their pictures are in their parent's list,
 * among its own, as AGS's letter menus are merged in Games.
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
    { "anim", "ANIM" }, { "anm", "ANIM" },
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
    if (!memcmp(b, "FORM", 4) && !memcmp(b + 8, "ANIM", 4))
        return "ANIM";
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

/* An index folder ("A".."Z", a digit, "0-9" or "#"): its pictures are
 * shown in its parent's list, as AGS's letter menus are in Games. */
static BOOL letter_dir(const char *name)
{
    UBYTE c = (UBYTE)name[0];
    if (!c)
        return FALSE;
    if (!name[1])
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '#';
    return amce_stricmp(name, "0-9") == 0;
}

/* Count dir's pictures (with those of its letter folders) and its other
 * folders; *first: the path of its first picture by name. */
struct scan_ctx {
    const char *dir;
    LONG *ndirs, npics, nanims, firstmax;
    char *first;
    BOOL top;
};

static LONG scan(const char *dir, LONG *ndirs, char *first, LONG firstmax, BOOL top,
                 LONG *nanims);

static void scan_cb(void *ctx, const char *name, BOOL isdir)
{
    struct scan_ctx *c = ctx;
    char path[300];
    const char *f;
    if (isdir) {
        if (c->top && letter_dir(name)) {
            LONG n;
            LONG na = 0;
            join_path(path, sizeof(path), c->dir, name);
            n = scan(path, c->ndirs, c->first, c->firstmax, FALSE, &na);
            if (n > 0)
                c->npics += n;
            c->nanims += na;
        } else if (c->ndirs) {
            (*c->ndirs)++;
        }
    } else if ((f = entry_format(c->dir, name)) && !strcmp(f, "ANIM")) {
        c->nanims++;                        /* played, not shown: not a slideshow's */
        if (c->first && c->firstmax && !c->first[0] && !c->npics)
            join_path(c->first, c->firstmax, c->dir, name);
    } else if (f) {
        c->npics++;
        if (c->first && c->firstmax &&
            (!c->first[0] || amce_stricmp(name, (const char *)FilePart((STRPTR)c->first)) < 0))
            join_path(c->first, c->firstmax, c->dir, name);
    }
}

static LONG scan(const char *dir, LONG *ndirs, char *first, LONG firstmax, BOOL top,
                 LONG *nanims)
{
    struct scan_ctx c;
    LONG r;
    c.dir = dir;
    c.ndirs = ndirs;
    c.npics = 0;
    c.nanims = 0;
    c.first = first;
    c.firstmax = firstmax;
    c.top = top;
    r = dir_scan(dir, scan_cb, &c) < 0 ? -1 : c.npics;
    if (nanims)
        *nanims = c.nanims;
    return r;
}

LONG pics_scan(const char *dir, LONG *ndirs, char *first, LONG firstmax)
{
    APTR old = req_off();
    LONG n;
    if (ndirs)
        *ndirs = 0;
    if (first && firstmax)
        first[0] = 0;
    n = scan(dir, ndirs, first, firstmax, TRUE, NULL);
    req_on(old);
    return n;
}

/* Add dir's folders and pictures to out, unsorted; a letter folder's
 * entries instead of the folder itself. */
struct collect_ctx {
    const char *dir;
    struct ItemList *out;
    BOOL top;
};

static void collect(const char *dir, struct ItemList *out, BOOL top);

static void collect_cb(void *ctx, const char *name, BOOL isdir)
{
    struct collect_ctx *c = ctx;
    char path[300], key[140];
    const char *f = isdir ? NULL : entry_format(c->dir, name);
    if (!isdir && !f)
        return;
    join_path(path, sizeof(path), c->dir, name);
    if (isdir && c->top && letter_dir(name)) {
        collect(path, c->out, FALSE);
        return;
    }
    key[0] = isdir ? '0' : '1';             /* folders first */
    amce_strlcpy(key + 1, name, sizeof(key) - 1);
    list_add(c->out, name, path, key, isdir ? IT_DIR : !strcmp(f, "ANIM") ? IT_ANIM : IT_PIC);
}

static void collect(const char *dir, struct ItemList *out, BOOL top)
{
    struct collect_ctx c;
    c.dir = dir;
    c.out = out;
    c.top = top;
    dir_scan(dir, collect_cb, &c);
}

static LONG read_dir(const char *dir, struct ItemList *out)
{
    BPTR lock = Lock((CONST_STRPTR)dir, SHARED_LOCK);
    LONG i;
    if (!lock)
        return -1;
    UnLock(lock);
    collect(dir, out, TRUE);
    list_sort(out);

    /* now the tags (and what each folder holds) */
    for (i = 0; i < out->count; i++) {
        struct Item *it = &out->items[i];
        if (it->kind == IT_PIC || it->kind == IT_ANIM) {
            const char *f = strchr(it->label, '.') ? pics_format(it->label) : sniff(it->path);
            it->sortkey = (char *)(f ? f : "");
        } else {
            LONG nd = 0, na = 0, np = scan(it->path, &nd, NULL, 0, TRUE, &na);
            it->sortkey = (char *)"";
            if (na > 0 && np <= 0 && nd == 0) {
                char num[12];                   /* animations: opened, one played at a time */
                sprintf(num, "%ld", (long)na);
                it->sortkey = list_strdup(out, num);
                if (!it->sortkey)
                    it->sortkey = (char *)"";
            } else if (np > 0 && nd == 0) {
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
    return out->count;
}

LONG pics_read_dir(const char *dir, struct ItemList *out)
{
    APTR old = req_off();
    LONG n = read_dir(dir, out);
    req_on(old);
    return n;
}
