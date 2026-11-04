/*
 * AmigaMCE - the index cache of long AGS lists
 * Copyright (c) 2026 Brian King
 *
 * Counting a long list means reading every file in its letter folders,
 * nearly as slow as reading the list. So once read, a list is kept in a
 * file (PROGDIR:Cache/<hash>.idx): for each letter folder its date, its
 * entry count and its entries. Next time only the folders' dates are
 * looked at (a folder's date changes when files are added or removed):
 * the total is known at once, and an unchanged letter is read from the
 * file, in one go, instead of from its folder.
 *
 * File: "AMCX", UWORD version, UWORD letters; a table of IDX_SLOT bytes
 * per letter (ULONG days, minute, tick; LONG count; ULONG offset, length
 * of its block, 0 if only the count is known; its folder's name, NUL
 * padded); then the blocks. A block holds its entries, sorted: UBYTE kind,
 * UBYTE length + label, UBYTE length + file name.
 */
#include "amce.h"

#include <string.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define IDX_MAGIC   0x414D4358UL    /* "AMCX" */
#define IDX_VERSION 1               /* bump when names are cleaned differently */
#define IDX_HEAD    8
#define IDX_SLOT    80
#define IDX_NAME    (IDX_SLOT - 24)

static ULONG get32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static void put32(UBYTE *p, ULONG v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

void idx_name(const char *menu, BOOL adult, char *out, LONG max)
{
    ULONG h = 2166136261UL;
    const UBYTE *s = (const UBYTE *)menu;
    char t[40];
    while (*s)
        h = (h ^ *s++) * 16777619UL;
    h = (h ^ (adult ? 'A' : 'a')) * 16777619UL;
    h = (h ^ IDX_VERSION) * 16777619UL;
    strcpy(t, "PROGDIR:Cache/");
    {
        static const char hex[] = "0123456789abcdef";
        int i;
        char *e = t + strlen(t);
        for (i = 0; i < 8; i++)
            e[i] = hex[(h >> (28 - 4 * i)) & 15];
        strcpy(e + 8, ".idx");
    }
    amce_strlcpy(out, t, max);
}

BOOL idx_date(const char *dir, struct DateStamp *ds)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l;
    BOOL ok = FALSE;
    if (!fib)
        return FALSE;
    if ((l = Lock((CONST_STRPTR)dir, SHARED_LOCK))) {
        if (Examine(l, fib)) {
            *ds = fib->fib_Date;
            ok = TRUE;
        }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return ok;
}

BOOL idx_load(const char *file, struct Idx *ix)
{
    UBYTE head[IDX_HEAD], *tab;
    BPTR fh;
    LONG n, i, size;
    memset(ix, 0, sizeof(*ix));
    if (!(fh = Open((CONST_STRPTR)file, MODE_OLDFILE)))
        return FALSE;
    if (Read(fh, head, IDX_HEAD) != IDX_HEAD || get32(head) != IDX_MAGIC ||
        ((head[4] << 8) | head[5]) != IDX_VERSION || !(n = (head[6] << 8) | head[7])) {
        Close(fh);
        return FALSE;
    }
    size = n * IDX_SLOT;
    tab = AllocVec(size, MEMF_ANY);
    ix->l = AllocVec(n * sizeof(struct IdxLetter), MEMF_ANY | MEMF_CLEAR);
    if (!tab || !ix->l || Read(fh, tab, size) != size) {
        FreeVec(tab);
        FreeVec(ix->l);
        ix->l = NULL;
        Close(fh);
        return FALSE;
    }
    Close(fh);
    for (i = 0; i < n; i++) {
        const UBYTE *t = tab + i * IDX_SLOT;
        struct IdxLetter *L = &ix->l[i];
        L->date.ds_Days = get32(t);
        L->date.ds_Minute = get32(t + 4);
        L->date.ds_Tick = get32(t + 8);
        L->count = (LONG)get32(t + 12);
        L->off = get32(t + 16);
        L->len = get32(t + 20);
        memcpy(L->name, t + 24, IDX_NAME);
        L->name[IDX_NAME - 1] = 0;
    }
    FreeVec(tab);
    ix->n = n;
    return TRUE;
}

void idx_free(struct Idx *ix)
{
    FreeVec(ix->l);
    ix->l = NULL;
    ix->n = 0;
}

LONG idx_read_block(const char *file, ULONG off, ULONG len, const char *dir,
                    struct ItemList *out)
{
    UBYTE *buf, *p, *e;
    char *shared, label[256], name[256];
    BPTR fh;
    LONG n = 0;
    if (!len || !(buf = AllocVec(len, MEMF_ANY)))
        return -1;
    if (!(fh = Open((CONST_STRPTR)file, MODE_OLDFILE))) {
        FreeVec(buf);
        return -1;
    }
    if (Seek(fh, off, OFFSET_BEGINNING) < 0 || Read(fh, buf, len) != (LONG)len) {
        Close(fh);
        FreeVec(buf);
        return -1;
    }
    Close(fh);
    if (!(shared = list_strdup(out, dir))) {
        FreeVec(buf);
        return -1;
    }
    for (p = buf, e = buf + len; p + 3 <= e; n++) {
        UBYTE kind = *p++, ll = *p++, nl;
        if (p + ll + 1 > e)
            break;
        memcpy(label, p, ll);
        label[ll] = 0;
        p += ll;
        nl = *p++;
        if (p + nl > e)
            break;
        memcpy(name, p, nl);
        name[nl] = 0;
        p += nl;
        if (!list_add_split(out, label, shared, name, kind))
            break;
    }
    FreeVec(buf);
    return p == e ? n : -1;
}

/* ---- writing: the table first (filled in as letters are added) */
BOOL idx_begin(struct IdxOut *o, LONG letters)
{
    o->n = letters;
    o->cap = IDX_HEAD + letters * IDX_SLOT + 16384;
    o->len = IDX_HEAD + letters * IDX_SLOT;
    if (!(o->buf = AllocVec(o->cap, MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    put32(o->buf, IDX_MAGIC);
    o->buf[4] = IDX_VERSION >> 8;
    o->buf[5] = IDX_VERSION & 255;
    o->buf[6] = letters >> 8;
    o->buf[7] = letters & 255;
    return TRUE;
}

static BOOL room(struct IdxOut *o, LONG more)
{
    UBYTE *nb;
    LONG ncap;
    if (o->len + more <= o->cap)
        return TRUE;
    ncap = o->cap * 2 + more;
    if (!(nb = AllocVec(ncap, MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    CopyMem(o->buf, nb, o->len);
    FreeVec(o->buf);
    o->buf = nb;
    o->cap = ncap;
    return TRUE;
}

BOOL idx_letter(struct IdxOut *o, LONG k, const struct DateStamp *date, const char *name,
                LONG count, const struct Item *items, const UBYTE *block, ULONG blen)
{
    UBYTE *t = o->buf + IDX_HEAD + k * IDX_SLOT;
    ULONG off = o->len, len = 0;
    LONG i;
    if (items) {
        for (i = 0; i < count; i++) {
            const struct Item *it = &items[i];
            LONG ll = strlen(it->label), nl = strlen(it->sortkey);
            if (ll > 255)
                ll = 255;
            if (nl > 255)
                nl = 255;
            if (!room(o, 3 + ll + nl))
                return FALSE;
            t = o->buf + IDX_HEAD + k * IDX_SLOT;       /* the buffer may have moved */
            o->buf[o->len++] = it->kind;
            o->buf[o->len++] = ll;
            CopyMem((APTR)it->label, o->buf + o->len, ll);
            o->len += ll;
            o->buf[o->len++] = nl;
            CopyMem((APTR)it->sortkey, o->buf + o->len, nl);
            o->len += nl;
        }
        len = o->len - off;
    } else if (block && blen) {
        if (!room(o, blen))
            return FALSE;
        t = o->buf + IDX_HEAD + k * IDX_SLOT;
        CopyMem((APTR)block, o->buf + o->len, blen);
        o->len += blen;
        len = blen;
    }
    if (!len)
        off = 0;                            /* only the count is known */
    put32(t, date->ds_Days);
    put32(t + 4, date->ds_Minute);
    put32(t + 8, date->ds_Tick);
    put32(t + 12, count);
    put32(t + 16, off);
    put32(t + 20, len);
    amce_strlcpy((char *)t + 24, name, IDX_NAME);
    return TRUE;
}

void idx_abandon(struct IdxOut *o)
{
    FreeVec(o->buf);
    o->buf = NULL;
}

/* Write the file (a new one first, then in its place). Any process. */
BOOL idx_save(const char *file, const UBYTE *buf, LONG len)
{
    char tmp[300];
    BPTR fh;
    BOOL ok;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    if (!strncmp(file, "PROGDIR:Cache/", 14)) {
        BPTR l = Lock((CONST_STRPTR)"PROGDIR:Cache", SHARED_LOCK);
        if (!l)
            l = CreateDir((CONST_STRPTR)"PROGDIR:Cache");
        if (l)
            UnLock(l);
    }
    amce_strlcpy(tmp, file, sizeof(tmp));
    amce_strlcat(tmp, ".new", sizeof(tmp));
    ok = (fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE)) != 0;
    if (ok) {
        ok = Write(fh, (APTR)buf, len) == len;
        Close(fh);
        if (ok) {
            DeleteFile((CONST_STRPTR)file);
            ok = Rename((CONST_STRPTR)tmp, (CONST_STRPTR)file) != 0;
        }
        if (!ok)
            DeleteFile((CONST_STRPTR)tmp);
    }
    me->pr_WindowPtr = oldwin;
    return ok;
}

/* A letter's block as it is in the file (to carry it over unchanged). */
UBYTE *idx_block(const char *file, ULONG off, ULONG len)
{
    UBYTE *b;
    BPTR fh;
    if (!len || !(b = AllocVec(len, MEMF_ANY)))
        return NULL;
    if ((fh = Open((CONST_STRPTR)file, MODE_OLDFILE))) {
        if (Seek(fh, off, OFFSET_BEGINNING) >= 0 && Read(fh, b, len) == (LONG)len) {
            Close(fh);
            return b;
        }
        Close(fh);
    }
    FreeVec(b);
    return NULL;
}
