/*
 * Ambiance - iGame's files read (see igamecsv.h)
 * Copyright (c) 2026 Brian King
 *
 * gameslist.csv is iGame's list of everything it found, one line an entry:
 *   instance;"title";"genre";"path";favourite;times_played;last_played;
 *   hidden;deleted;"user_title";year;players;"chipset"
 * The quotes and the last four columns are from iGame 2.3.1 on. iGame
 * writes no escapes: a quoted field runs to the next quote that a ';' or
 * the line's end follows.
 */
#include "igamecsv.h"

#include <string.h>
#include <stdio.h>

static int lower(int c)
{
    c &= 0xFF;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        c += 32;
    return c;
}

static int eq_nocase(const char *a, const char *b, long n)
{
    long i;
    for (i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i]))
            return 0;
    return 1;
}

static long to_long(const char *s)
{
    long v = 0;
    int neg = 0;
    if (!s)
        return 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* The next field from *pp, ended with a NUL in place; NULL past the last. */
static char *next_field(char **pp)
{
    char *p = *pp, *f, *e;
    if (!p)
        return NULL;
    if (*p == '"') {
        f = ++p;
        for (e = p; *e; e++)
            if (*e == '"' && (e[1] == ';' || e[1] == 0 || e[1] == '\n' || e[1] == '\r'))
                break;
        if (*e == '"') {
            *e++ = 0;
            if (*e == ';')
                *pp = e + 1;
            else
                *pp = NULL;
            return f;
        }
        /* no closing quote: the rest of the line */
        p = f;
    }
    f = p;
    for (e = p; *e && *e != ';' && *e != '\n' && *e != '\r'; e++)
        ;
    if (*e == ';') {
        *e = 0;
        *pp = e + 1;
    } else {
        *e = 0;
        *pp = NULL;
    }
    return f;
}

int ig_parse_line(char *line, struct IgRow *r)
{
    static char empty[1];
    char *p = line, *f[13];
    int n;
    for (n = 0; n < 13; n++)
        f[n] = next_field(&p);
    empty[0] = 0;
    memset(r, 0, sizeof(*r));
    if (!f[0] || !f[3] || !f[3][0])
        return 0;
    r->instance = to_long(f[0]);
    r->title = f[1] ? f[1] : empty;
    r->genre = f[2] && f[2][0] ? f[2] : empty;
    r->path = f[3];
    r->favourite = to_long(f[4]);
    r->times_played = to_long(f[5]);
    r->last_played = to_long(f[6]);
    r->hidden = to_long(f[7]);
    r->deleted = to_long(f[8]);
    r->user_title = f[9] ? f[9] : empty;
    r->year = to_long(f[10]);
    r->players = to_long(f[11]);
    r->chipset = f[12] ? f[12] : empty;
    return 1;
}

const char *ig_row_title(const struct IgRow *r)
{
    if (r->user_title && r->user_title[0])
        return r->user_title;
    if (r->title && r->title[0])
        return r->title;
    return r->path;
}

/* A folder name that says demos: "Demo", "Demos", "Demos_AGA", "Demos AGA"
 * (not "DemolitionMan"). */
static int demo_word(const char *s, long n)
{
    long k;
    if (n >= 5 && eq_nocase(s, "demos", 5))
        k = 5;
    else if (n >= 4 && eq_nocase(s, "demo", 4))
        k = 4;
    else
        return 0;
    return k == n || !((lower(s[k]) >= 'a' && lower(s[k]) <= 'z'));
}

int ig_is_demo(const char *genre, const char *path)
{
    const char *s, *e;
    if (genre && genre[0]) {
        long n = strlen(genre);
        if (demo_word(genre, n))
            return 1;
        if ((n >= 5 && eq_nocase(genre, "intro", 5)) || (n >= 7 && eq_nocase(genre, "diskmag", 7)) ||
            (n >= 8 && eq_nocase(genre, "disk mag", 8)) || (n >= 8 && eq_nocase(genre, "cracktro", 8)))
            return 1;
    }
    if (!path)
        return 0;
    /* every folder on the way (not the file at its end) */
    s = strchr(path, ':');
    s = s ? s + 1 : path;
    while ((e = strchr(s, '/')) != NULL) {
        if (demo_word(s, e - s))
            return 1;
        s = e + 1;
    }
    return 0;
}

static int is_key_char(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

int ig_tooltype_arg(const char *tt, char *out, long max)
{
    static const char *const not_whdload[] = { "DONOTWAIT", "TOOLPRI", "STARTPRI", "WAIT", "STACK",
                                               NULL };
    char key[48], val[200];
    const char *eq, *v;
    long kn, vn, i;
    unsigned char c;
    out[0] = 0;
    while (*tt == ' ' || *tt == '\t')
        tt++;
    if (!strncmp(tt, "***", 3) || strstr(tt, "DON'T EDIT"))
        return -1;                          /* NewIcons' image data follows */
    c = (unsigned char)tt[0];
    if (!c || strchr("(*;.=#!\"[", c) || c == 0xBB || c == 0xAB)
        return 0;                           /* commented out, as iGame takes them */
    eq = strchr(tt, '=');
    kn = eq ? eq - tt : (long)strlen(tt);
    while (kn > 0 && (tt[kn - 1] == ' ' || tt[kn - 1] == '\t' || tt[kn - 1] == '\n' || tt[kn - 1] == '\r'))
        kn--;
    if (kn <= 0 || kn >= (long)sizeof(key))
        return 0;
    for (i = 0; i < kn; i++) {
        if (!is_key_char((unsigned char)tt[i]))
            return 0;                       /* not a keyword WHDLoad could take */
        key[i] = tt[i];
    }
    key[kn] = 0;
    if ((kn == 3 && (key[0] == 'I' || key[0] == 'i') && (key[1] == 'M' || key[1] == 'm') &&
         (key[2] == '1' || key[2] == '2')))
        return 0;                           /* an icon's image */
    for (i = 0; not_whdload[i]; i++)
        if ((long)strlen(not_whdload[i]) == kn && eq_nocase(key, not_whdload[i], kn))
            return 0;                       /* Workbench's, not WHDLoad's */
    if (!eq) {
        if (kn + 1 > max)
            return 0;
        strcpy(out, key);                   /* a switch: PRELOAD, NOCACHE... */
        return 1;
    }
    v = eq + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    vn = strlen(v);
    while (vn > 0 && (v[vn - 1] == ' ' || v[vn - 1] == '\t' || v[vn - 1] == '\n' || v[vn - 1] == '\r'))
        vn--;
    if (vn >= 2 && v[0] == '"' && v[vn - 1] == '"') {
        v++;
        vn -= 2;
    }
    if (vn >= (long)sizeof(val))
        return 0;
    memcpy(val, v, vn);
    val[vn] = 0;
    if (val[0] == '$' && val[1]) {
        /* ReadArgs' /N takes no hex: as iGame does, in decimal */
        unsigned long h = 0;
        for (i = 1; val[i]; i++) {
            int d = lower(val[i]);
            if (d >= '0' && d <= '9')
                d -= '0';
            else if (d >= 'a' && d <= 'f')
                d -= 'a' - 10;
            else
                break;
            h = h * 16 + d;
        }
        if (!val[i]) {
            char num[24];
            sprintf(num, "%lu", h);
            if (kn + 1 + (long)strlen(num) + 1 > max)
                return 0;
            sprintf(out, "%s=%s", key, num);
            return 1;
        }
    }
    {
        int numeric = val[0] != 0;
        for (i = (val[0] == '-' ? 1 : 0); val[i]; i++)
            if (val[i] < '0' || val[i] > '9')
                numeric = 0;
        if (val[0] == '-' && !val[1])
            numeric = 0;
        if (numeric) {
            if (kn + 1 + vn + 1 > max)
                return 0;
            sprintf(out, "%s=%s", key, val);
        } else {
            if (strchr(val, '"') || kn + 3 + vn + 1 > max)
                return 0;
            sprintf(out, "%s=\"%s\"", key, val);
        }
    }
    return 1;
}

void ig_split_path(const char *path, char *dir, long dmax, char *name, long nmax)
{
    const char *s = strrchr(path, '/'), *c = strrchr(path, ':');
    const char *cut = s ? s : c;            /* the last '/', else the ':' */
    long n;
    if (!cut) {
        if (dmax > 0)
            dir[0] = 0;
        if (nmax > 0) {
            strncpy(name, path, nmax - 1);
            name[nmax - 1] = 0;
        }
        return;
    }
    n = cut - path + (cut == c && !s ? 1 : 0);  /* "Vol:" keeps its colon */
    if (dmax > 0) {
        if (n >= dmax)
            n = dmax - 1;
        memcpy(dir, path, n);
        dir[n] = 0;
    }
    if (nmax > 0) {
        strncpy(name, cut + 1, nmax - 1);
        name[nmax - 1] = 0;
    }
}
