/*
 * Ambiance - a Jellyfin server's JSON, read just enough
 * Copyright (c) 2026 Brian King
 *
 * Jellyfin answers only in JSON. This walks an answer once, where it lies
 * (strings are decoded in place: an escape is never shorter decoded than
 * written, so the text never overtakes the reading), and hands each value
 * to a callback with its depth and member name, as plexxml.c hands each
 * element. Nothing is allocated, nothing is kept: jf_items() picks the few
 * members Ambiance shows out of each item as they pass. No Amiga headers:
 * the host tests (tests/jf_host.c) use it as it is, on saved answers.
 *
 * Jellyfin writes every character past ASCII as \uXXXX (and a quote as
 * "), so the decoding matters: surrogate pairs become one 4-byte
 * UTF-8 character, a lone surrogate or \u0000 a '?'.
 */
#include "jfjson.h"

#include <string.h>

#define MAX_DEPTH 40

struct jp {
    char *p, *e;
    jf_val_fn val;
    jf_end_fn end;
    void *ctx;
    int stop, bad;
};

static void ws(struct jp *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static int hex4(const char *s, unsigned long *v)
{
    int i;
    *v = 0;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        *v <<= 4;
        if (c >= '0' && c <= '9')
            *v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f')
            *v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            *v |= (unsigned long)(c - 'A' + 10);
        else
            return 0;
    }
    return 1;
}

static char *utf8(char *w, unsigned long c)
{
    if (c < 0x80) {
        *w++ = (char)c;
    } else if (c < 0x800) {
        *w++ = (char)(0xC0 | (c >> 6));
        *w++ = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        *w++ = (char)(0xE0 | (c >> 12));
        *w++ = (char)(0x80 | ((c >> 6) & 0x3F));
        *w++ = (char)(0x80 | (c & 0x3F));
    } else {
        *w++ = (char)(0xF0 | (c >> 18));
        *w++ = (char)(0x80 | ((c >> 12) & 0x3F));
        *w++ = (char)(0x80 | ((c >> 6) & 0x3F));
        *w++ = (char)(0x80 | (c & 0x3F));
    }
    return w;
}

/* A string (j->p at its quote): decoded where it is, NUL-ended; NULL if
 * it does not end. */
static char *string(struct jp *j)
{
    char *s = ++j->p, *w = s;
    while (j->p < j->e) {
        char c = *j->p++;
        if (c == '"') {
            *w = 0;
            return s;
        }
        if (c != '\\') {
            *w++ = c;
            continue;
        }
        if (j->p >= j->e)
            break;
        c = *j->p++;
        switch (c) {
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            unsigned long u, lo;
            if (j->e - j->p < 4 || !hex4(j->p, &u))
                return NULL;
            j->p += 4;
            if (u >= 0xD800 && u <= 0xDBFF && j->e - j->p >= 6 && j->p[0] == '\\' &&
                j->p[1] == 'u' && hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                j->p += 6;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else if ((u >= 0xD800 && u <= 0xDFFF) || !u) {
                u = '?';                    /* a lone half, or a NUL: not text */
            }
            w = utf8(w, u);
            break;
        }
        default:                            /* \" \\ \/ (and anything else as itself) */
            *w++ = c;
            break;
        }
    }
    return NULL;
}

static int value(struct jp *j, int depth, const char *name);

static int container(struct jp *j, int depth, const char *name, char close)
{
    int kind = close == '}' ? JF_OBJ : JF_ARR;
    if (!j->stop && j->val && !j->val(j->ctx, depth, name, kind, NULL))
        j->stop = 1;
    j->p++;
    ws(j);
    if (j->p < j->e && *j->p == close) {
        j->p++;
    } else {
        for (;;) {
            const char *key = NULL;
            ws(j);
            if (close == '}') {
                if (j->p >= j->e || *j->p != '"' || !(key = string(j)))
                    return 0;
                ws(j);
                if (j->p >= j->e || *j->p != ':')
                    return 0;
                j->p++;
            }
            if (!value(j, depth + 1, key))
                return 0;
            ws(j);
            if (j->p >= j->e)
                return 0;
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (*j->p == close) {
                j->p++;
                break;
            }
            return 0;
        }
    }
    if (!j->stop && j->end && !j->end(j->ctx, depth, kind))
        j->stop = 1;
    return 1;
}

static int value(struct jp *j, int depth, const char *name)
{
    char nb[48], *s;
    int n;
    ws(j);
    if (j->p >= j->e || depth > MAX_DEPTH)
        return 0;
    switch (*j->p) {
    case '{':
        return container(j, depth, name, '}');
    case '[':
        return container(j, depth, name, ']');
    case '"':
        if (!(s = string(j)))
            return 0;
        if (!j->stop && j->val && !j->val(j->ctx, depth, name, JF_STR, s))
            j->stop = 1;
        return 1;
    case 't':
        if (j->e - j->p < 4 || memcmp(j->p, "true", 4))
            return 0;
        j->p += 4;
        if (!j->stop && j->val && !j->val(j->ctx, depth, name, JF_TRUE, "true"))
            j->stop = 1;
        return 1;
    case 'f':
        if (j->e - j->p < 5 || memcmp(j->p, "false", 5))
            return 0;
        j->p += 5;
        if (!j->stop && j->val && !j->val(j->ctx, depth, name, JF_FALSE, "false"))
            j->stop = 1;
        return 1;
    case 'n':
        if (j->e - j->p < 4 || memcmp(j->p, "null", 4))
            return 0;
        j->p += 4;
        if (!j->stop && j->val && !j->val(j->ctx, depth, name, JF_NULL, ""))
            j->stop = 1;
        return 1;
    }
    /* a number: its text as written */
    for (n = 0; j->p < j->e && (strchr("0123456789+-.eE", *j->p)) && *j->p; j->p++)
        if (n < (int)sizeof(nb) - 1)
            nb[n++] = *j->p;
    if (!n)
        return 0;
    nb[n] = 0;
    if (!j->stop && j->val && !j->val(j->ctx, depth, name, JF_NUM, nb))
        j->stop = 1;
    return 1;
}

int jf_walk(char *buf, unsigned long len, jf_val_fn val, jf_end_fn end, void *ctx)
{
    struct jp j;
    j.p = buf;
    j.e = buf + len;
    j.val = val;
    j.end = end;
    j.ctx = ctx;
    j.stop = j.bad = 0;
    /* (a UTF-8 byte order mark: some servers' proxies add one) */
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF)
        j.p += 3;
    if (!value(&j, 0, NULL))
        return j.stop ? 0 : -1;
    return 0;
}

/* ---------------------------------------------------------------- items */

static void put(char *out, unsigned long max, const char *s)
{
    unsigned long n = strlen(s);
    if (n >= max) {
        n = max - 1;
        /* (not in the middle of a UTF-8 character) */
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, s, n);
    out[n] = 0;
}
#define PUT(f, s) put(f, sizeof(f), s)

struct items {
    int (*item)(void *ctx, const struct JfItem *it);
    void *ctx;
    long n, total;
    int in_items;               /* inside the top's "Items" array */
    int item_depth;             /* the item object being read opened at this depth (0: none) */
    int sub;                    /* that item's member being read: */
    int sub_first;              /* ...its elements seen (Artists, ArtistItems) */
    struct JfItem it;
};
enum { SUB_NONE, SUB_IMAGES, SUB_ARTISTS, SUB_ARTIST_ITEMS, SUB_OTHER };

static void member(struct items *x, const char *name, int kind, const char *text)
{
    if (kind == JF_OBJ || kind == JF_ARR) {
        x->sub = !strcmp(name, "ImageTags") ? SUB_IMAGES
                 : !strcmp(name, "Artists") ? SUB_ARTISTS
                 : !strcmp(name, "ArtistItems") ? SUB_ARTIST_ITEMS : SUB_OTHER;
        x->sub_first = 0;
        return;
    }
    x->sub = SUB_NONE;
    if (kind == JF_TRUE && !strcmp(name, "IsFolder"))
        x->it.folder = 1;
    if (kind != JF_STR && kind != JF_NUM)
        return;
    switch (name[0]) {
    case 'A':
        if (!strcmp(name, "Album"))
            PUT(x->it.album, text);
        else if (!strcmp(name, "AlbumId"))
            PUT(x->it.album_id, text);
        else if (!strcmp(name, "AlbumArtist"))
            PUT(x->it.album_artist, text);
        else if (!strcmp(name, "AlbumPrimaryImageTag"))
            PUT(x->it.album_image, text);
        break;
    case 'C':
        if (!strcmp(name, "CollectionType"))
            PUT(x->it.coll, text);
        else if (!strcmp(name, "ChildCount"))
            PUT(x->it.children, text);
        else if (!strcmp(name, "Container"))
            PUT(x->it.container, text);
        break;
    case 'H':
        if (!strcmp(name, "Height"))
            PUT(x->it.height, text);
        break;
    case 'I':
        if (!strcmp(name, "Id"))
            PUT(x->it.id, text);
        else if (!strcmp(name, "IndexNumber"))
            PUT(x->it.index, text);
        break;
    case 'M':
        if (!strcmp(name, "MediaType"))
            PUT(x->it.media, text);
        break;
    case 'N':
        if (!strcmp(name, "Name"))
            PUT(x->it.name, text);
        break;
    case 'O':
        if (!strcmp(name, "Overview"))
            PUT(x->it.overview, text);
        else if (!strcmp(name, "OfficialRating"))
            PUT(x->it.rating, text);
        break;
    case 'P':
        if (!strcmp(name, "ProductionYear"))
            PUT(x->it.year, text);
        else if (!strcmp(name, "PremiereDate"))
            PUT(x->it.date, text);
        else if (!strcmp(name, "ParentIndexNumber"))
            PUT(x->it.disc, text);
        break;
    case 'R':
        if (!strcmp(name, "RunTimeTicks"))
            PUT(x->it.ticks, text);
        break;
    case 'T':
        if (!strcmp(name, "Type"))
            PUT(x->it.type, text);
        break;
    case 'W':
        if (!strcmp(name, "Width"))
            PUT(x->it.width, text);
        break;
    }
}

static int items_val(void *ctx, int depth, const char *name, int kind, const char *text)
{
    struct items *x = ctx;
    int d;
    if (!x->item_depth) {
        if (depth == 1) {
            if (name && kind == JF_NUM && !strcmp(name, "TotalRecordCount")) {
                long v = 0;
                const char *t;
                for (t = text; *t >= '0' && *t <= '9'; t++)
                    v = v * 10 + (*t - '0');
                x->total = v;
            }
            x->in_items = name && kind == JF_ARR && !strcmp(name, "Items");
            if (name && kind == JF_OBJ && !strcmp(name, "User"))
                x->item_depth = 1;      /* a sign-in's answer: its user, the one item */
        } else if (depth == 2 && x->in_items && kind == JF_OBJ) {
            x->item_depth = 2;
        }
        if (x->item_depth) {
            memset(&x->it, 0, sizeof(x->it));
            x->sub = SUB_NONE;
        }
        return 1;
    }
    d = depth - x->item_depth;          /* 1: the item's own members */
    if (d == 1) {
        if (name)
            member(x, name, kind, text);
    } else if (d == 2) {
        if (x->sub == SUB_IMAGES && name && kind == JF_STR && !strcmp(name, "Primary"))
            PUT(x->it.image, text);
        else if (x->sub == SUB_ARTISTS && kind == JF_STR && !x->sub_first++)
            PUT(x->it.artist, text);
        else if (x->sub == SUB_ARTIST_ITEMS && kind == JF_OBJ)
            x->sub_first++;
    } else if (d == 3 && x->sub == SUB_ARTIST_ITEMS && x->sub_first == 1 && name &&
               kind == JF_STR) {
        if (!strcmp(name, "Id"))
            PUT(x->it.artist_id, text);
        else if (!strcmp(name, "Name") && !x->it.artist[0])
            PUT(x->it.artist, text);
    }
    return 1;
}

static int items_end(void *ctx, int depth, int kind)
{
    struct items *x = ctx;
    if (x->item_depth && depth == x->item_depth && kind == JF_OBJ) {
        x->item_depth = 0;
        x->n++;
        return x->item ? x->item(x->ctx, &x->it) : 1;
    }
    if (x->item_depth && depth == x->item_depth + 1)
        x->sub = SUB_NONE;
    if (depth == 1 && kind == JF_ARR)
        x->in_items = 0;
    return 1;
}

long jf_items(char *buf, unsigned long len, int (*item)(void *ctx, const struct JfItem *it),
              void *ctx, long *total)
{
    struct items x;
    memset(&x, 0, sizeof(x));
    x.item = item;
    x.ctx = ctx;
    x.total = -1;
    if (jf_walk(buf, len, items_val, items_end, &x) < 0)
        return -1;
    if (total)
        *total = x.total >= 0 ? x.total : x.n;
    return x.n;
}

/* ---------------------------------------------------------------- one value */

struct one {
    const char *a, *b;          /* the member, and (dotted) the one inside it */
    char *out;
    long max;
    int in, found;
};

static int one_val(void *ctx, int depth, const char *name, int kind, const char *text)
{
    struct one *x = ctx;
    if (depth == 1) {
        x->in = 0;
        if (!name || strcmp(name, x->a))
            return 1;
        if (x->b) {
            x->in = kind == JF_OBJ;
            return 1;
        }
    } else if (!(depth == 2 && x->in && name && !strcmp(name, x->b))) {
        return 1;
    }
    if (kind == JF_OBJ || kind == JF_ARR)
        return 1;
    put(x->out, (unsigned long)x->max, text);
    x->found = 1;
    return 0;                   /* (found: no more) */
}

int jf_value(const char *buf, unsigned long len, const char *name, char *out, long max)
{
    /* (a copy, so the answer stays as it was for the next question) */
    static char tmp[16384];
    char a[64];
    const char *dot = strchr(name, '.');
    struct one x;
    if (max <= 0)
        return 0;
    out[0] = 0;
    if (len > sizeof(tmp))
        len = sizeof(tmp);
    memcpy(tmp, buf, len);
    memset(&x, 0, sizeof(x));
    if (dot) {
        unsigned long n = (unsigned long)(dot - name);
        if (n >= sizeof(a))
            n = sizeof(a) - 1;
        memcpy(a, name, n);
        a[n] = 0;
        x.a = a;
        x.b = dot + 1;
    } else {
        x.a = name;
    }
    x.out = out;
    x.max = max;
    jf_walk(tmp, len, one_val, NULL, &x);
    return x.found;
}

int jf_discovered(char *buf, unsigned long len, char *address, long amax, char *id, long imax,
                  char *name, long nmax)
{
    jf_value(buf, len, "Id", id, imax);
    jf_value(buf, len, "Name", name, nmax);
    return jf_value(buf, len, "Address", address, amax) && address[0];
}
