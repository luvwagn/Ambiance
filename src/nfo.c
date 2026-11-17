/*
 * Ambiance - Kodi's .nfo files and artwork beside the music
 * Copyright (c) 2026 Brian King
 *
 * Kodi keeps what it scraped about an album or an artist in XML files
 * beside the music (Kodi's own scrapers, its "Export library: separate
 * files", or tools such as MusicBrainz Picard plugins and tinyMediaManager
 * write them): an album folder's album.nfo, the folder above it its
 * artist's artist.nfo, and pictures named as Kodi names them:
 *
 *   Artist/            artist.nfo, folder.jpg (the artist), fanart.jpg,
 *                      extrafanart/, logo.png, banner.jpg
 *   Artist/Album/      album.nfo, folder.jpg or cover.jpg, discart.png
 *                      (cdart.png), back.jpg
 *   Artist/Album/CD1/  a disc's own folder: the album is the one above
 *
 * Read here: the root element's leaf children (album: title, artistdesc
 * or artist, year or releasedate, label, genre, style, mood, rating,
 * review; artist: name, type, genre, style, mood, formed, born, disbanded,
 * died, biography). Repeated elements (genre, style, mood) are joined
 * with ", ". Nested ones (an album's tracks, an artist's discography, the
 * thumbs) are passed over. Text is UTF-8 (entities and CDATA undone),
 * made Latin-1 as tags are (utf8_to_latin1, which keeps CJK for cjk.c).
 * The review or biography comes in its own AllocVec'd buffer (nfo_free),
 * paragraphs separated by single newlines.
 *
 * Only the folder's own files are looked at, and only when the details
 * pane asks (the loader's process), as covers are: a share can be slow.
 */
#include "amce.h"
#include "plexxml.h"
#include "radiocsv.h"

#include <string.h>
#include <stdio.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define NFO_MAX   (96L * 1024)      /* bigger than any real one: larger files are not read */
#define TEXT_MAX  (12L * 1024)      /* a review or biography, at most */

static void join(char *out, LONG max, const char *dir, const char *name)
{
    LONG n;
    amce_strlcpy(out, dir, max);
    n = strlen(out);
    if (n && out[n - 1] != ':' && out[n - 1] != '/')
        amce_strlcat(out, "/", max);
    amce_strlcat(out, name, max);
}

/* dir's parent into out (FALSE: a volume's root, no parent) */
static BOOL parent_of(const char *dir, char *out, LONG max)
{
    char *pp;
    LONG n;
    amce_strlcpy(out, dir, max);
    n = strlen(out);
    while (n && out[n - 1] == '/')
        out[--n] = 0;
    if (!n || out[n - 1] == ':')
        return FALSE;
    pp = (char *)PathPart((STRPTR)out);
    *pp = 0;
    return out[0] != 0;
}

/* A disc's own folder ("CD1", "CD 2", "Disc 1", "Disk2", "Disc 3 - Live"):
 * its album's files are in the folder above. */
BOOL nfo_disc_folder(const char *name)
{
    const char *s = name;
    if (!amce_strnicmp(s, "cd", 2))
        s += 2;
    else if (!amce_strnicmp(s, "disc", 4) || !amce_strnicmp(s, "disk", 4))
        s += 4;
    else
        return FALSE;
    while (*s == ' ' || *s == '_' || *s == '-' || *s == '.')
        s++;
    if (*s < '0' || *s > '9')
        return FALSE;
    while (*s >= '0' && *s <= '9')
        s++;
    return !*s || *s == ' ' || *s == '-' || *s == '_' || *s == '(';
}

/* The album folder of a music folder: itself, or the one above a disc's. */
static void album_folder(const char *dir, char *out, LONG max)
{
    char tmp[300];
    amce_strlcpy(tmp, dir, sizeof(tmp));
    {
        LONG n = strlen(tmp);
        while (n && tmp[n - 1] == '/')
            tmp[--n] = 0;
    }
    if (nfo_disc_folder((const char *)FilePart((STRPTR)tmp)) && parent_of(tmp, out, max))
        return;
    amce_strlcpy(out, dir, max);
}

/* ---- reading one */

static void add_list(char *field, LONG max, const char *v)
{
    if (!v[0])
        return;
    if (field[0]) {
        /* the same one twice (a genre also a style): once */
        const char *f = field;
        LONG k = strlen(v);
        while ((f = strstr(f, v))) {
            if ((f == field || f[-1] == ' ') && (f[k] == 0 || f[k] == ','))
                return;
            f++;
        }
        amce_strlcat(field, ", ", max);
    }
    amce_strlcat(field, v, max);
}

/* each of list's ", "-separated items into field, those there already passed over */
static void add_each(char *field, LONG max, const char *list)
{
    char item[100];
    while (*list) {
        LONG k = 0;
        while (list[k] && list[k] != ',')
            k++;
        if (k >= (LONG)sizeof(item))
            k = sizeof(item) - 1;
        memcpy(item, list, k);
        item[k] = 0;
        add_list(field, max, item);
        list += k;
        while (*list == ',' || *list == ' ')
            list++;
    }
}

/* s trimmed, runs of spaces and tabs made one space, line ends kept as
 * single newlines (paragraphs); in place. */
static void tidy(char *s, BOOL keep_lines)
{
    char *p = s, *o = s;
    BOOL sp = FALSE, nl = FALSE;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    for (; *p; p++) {
        if (*p == '\r')
            continue;
        if (*p == '\n') {
            nl = TRUE;
            continue;
        }
        if (*p == ' ' || *p == '\t') {
            sp = TRUE;
            continue;
        }
        if (nl && keep_lines)
            *o++ = '\n';
        else if (sp || nl)
            *o++ = ' ';
        sp = nl = FALSE;
        *o++ = *p;
    }
    *o = 0;
}

/* "8.500000" -> "8.5"; "4" -> "4"; "" if not a number above 0 */
static void rating_text(const char *v, char *out, LONG max)
{
    char t[16];
    LONG n = 0;
    BOOL dot = FALSE, nonzero = FALSE;
    while (*v == ' ')
        v++;
    for (; *v && n < (LONG)sizeof(t) - 1; v++) {
        if (*v >= '0' && *v <= '9') {
            if (*v != '0')
                nonzero = TRUE;
            t[n++] = *v;
        } else if ((*v == '.' || *v == ',') && !dot) {
            dot = TRUE;
            t[n++] = '.';
        } else {
            break;
        }
    }
    t[n] = 0;
    if (dot) {
        while (n && t[n - 1] == '0')
            t[--n] = 0;
        if (n && t[n - 1] == '.')
            t[--n] = 0;
    }
    if (!nonzero || !n)
        t[0] = 0;
    amce_strlcpy(out, t, max);
}

struct pctx {
    struct Nfo *n;
    char *text;                 /* the review or biography, in the file's buffer */
};

static void leaf(struct pctx *c, const char *name, const char *attrs, char *v)
{
    struct Nfo *n = c->n;
    xml_unescape(v);
    if (!amce_stricmp(name, "review") || !amce_stricmp(name, "biography")) {
        tidy(v, TRUE);
        if (v[0] && !c->text)
            c->text = v;
        return;
    }
    tidy(v, FALSE);
    utf8_to_latin1(v);
    if (!v[0])
        return;
    if (!amce_stricmp(name, "title") || !amce_stricmp(name, "name")) {
        if (!n->title[0])
            amce_strlcpy(n->title, v, sizeof(n->title));
    } else if (!amce_stricmp(name, "artistdesc")) {
        amce_strlcpy(n->artist, v, sizeof(n->artist));      /* the album's credit as shown */
    } else if (!amce_stricmp(name, "artist")) {
        if (!n->artist[0])
            amce_strlcpy(n->artist, v, sizeof(n->artist));
    } else if (!amce_stricmp(name, "year")) {
        amce_strlcpy(n->year, v, sizeof(n->year));
    } else if (!amce_stricmp(name, "releasedate") || !amce_stricmp(name, "originalreleasedate")) {
        if (!n->year[0] && v[0] >= '0' && v[0] <= '9') {
            amce_strlcpy(n->year, v, 5);                     /* "1993-05-01": 1993 */
        }
    } else if (!amce_stricmp(name, "label")) {
        amce_strlcpy(n->label, v, sizeof(n->label));
    } else if (!amce_stricmp(name, "genre")) {
        add_list(n->genres, sizeof(n->genres), v);
    } else if (!amce_stricmp(name, "style")) {
        add_list(n->styles, sizeof(n->styles), v);
    } else if (!amce_stricmp(name, "mood")) {
        add_list(n->moods, sizeof(n->moods), v);
    } else if (!amce_stricmp(name, "rating")) {
        /* <rating max="10">8.5</rating>; an old one without max is out of 5 */
        const char *m = attrs ? strstr(attrs, "max=") : NULL;
        int mx = 0;
        rating_text(v, n->rating, sizeof(n->rating));
        if (m) {
            m += 4;
            if (*m == '"' || *m == '\'')
                m++;
            while (*m >= '0' && *m <= '9' && mx < 1000)
                mx = mx * 10 + (*m++ - '0');
        } else if (n->rating[0] && n->rating[0] <= '5' && !n->rating[1]) {
            mx = 5;
        }
        n->rating_max = mx > 0 && mx < 1000 ? (UWORD)mx : 10;
    } else if (!amce_stricmp(name, "type")) {
        amce_strlcpy(n->type, v, sizeof(n->type));
    } else if (!amce_stricmp(name, "formed")) {
        amce_strlcpy(n->formed, v, sizeof(n->formed));
    } else if (!amce_stricmp(name, "born")) {
        if (!n->formed[0])
            amce_strlcpy(n->formed, v, sizeof(n->formed)), n->born = TRUE;
    } else if (!amce_stricmp(name, "disbanded") || !amce_stricmp(name, "died")) {
        amce_strlcpy(n->ended, v, sizeof(n->ended));
    }
}

/* buf (len bytes, 0 after them): its root element's leaf children. */
static void parse(char *buf, struct pctx *c)
{
    char *p = buf;
    int depth = 0;
    while ((p = strchr(p, '<'))) {
        char *name, *e, *attrs = NULL, save;
        BOOL closing, empty;
        if (!strncmp(p, "<!--", 4)) {
            if (!(p = strstr(p + 4, "-->")))
                return;
            p += 3;
            continue;
        }
        if (p[1] == '?' || p[1] == '!') {           /* <?xml ...?>, <!DOCTYPE>, stray CDATA */
            if (!strncmp(p, "<![CDATA[", 9)) {
                if (!(p = strstr(p + 9, "]]>")))
                    return;
                p += 3;
            } else if (!(p = strchr(p + 1, '>'))) {
                return;
            } else {
                p++;
            }
            continue;
        }
        closing = p[1] == '/';
        name = p + 1 + closing;
        for (e = name; *e && *e != '>' && *e != '/' && *e != ' ' && *e != '\t' && *e != '\r' &&
                       *e != '\n'; e++)
            ;
        {   /* the tag's end, quoted values passed over */
            char *q = e, quote = 0;
            for (; *q && (quote || *q != '>'); q++)
                if (*q == '"' || *q == '\'')
                    quote = quote == *q ? 0 : quote ? quote : *q;
            if (!*q)
                return;
            empty = q[-1] == '/';
            save = *e;
            if (!closing && e < q && save != '/')
                attrs = e + 1;                      /* ' max="10"': after the name's end */
            *e = 0;                                 /* the name ends here */
            p = q + 1;
        }
        if (closing) {
            depth--;
            continue;
        }
        if (empty)
            continue;
        depth++;
        if (depth == 2) {
            /* a child of the root: a leaf if its text runs to its own end tag */
            char *t = p, *end;
            LONG k = strlen(name);
            if (!strncmp(t, "<![CDATA[", 9)) {
                char *ce = strstr(t + 9, "]]>");
                if (!ce)
                    return;
                *ce = 0;
                end = ce + 3;
                while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
                    end++;
                if (end[0] == '<' && end[1] == '/' && !amce_strnicmp(end + 2, name, k)) {
                    char *gt = strchr(end, '>');
                    leaf(c, name, attrs, t + 9);
                    if (!gt)
                        return;
                    p = gt + 1;
                    depth--;
                }
                continue;
            }
            if ((end = strchr(t, '<')) && end[1] == '/' && !amce_strnicmp(end + 2, name, k) &&
                (end[2 + k] == '>' || end[2 + k] == ' ')) {
                char *gt = strchr(end, '>');
                *end = 0;
                leaf(c, name, attrs, t);
                if (!gt)
                    return;
                p = gt + 1;
                depth--;
            }
        } else if (depth == 1) {
            c->n->kind = !amce_stricmp(name, "album") ? NFO_ALBUM
                       : !amce_stricmp(name, "artist") ? NFO_ARTIST : NFO_OTHER;
        }
    }
}

BOOL nfo_read(const char *path, struct Nfo *n)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    struct FileInfoBlock *fib;
    struct pctx c;
    BPTR fh;
    char *buf;
    LONG len = -1;
    memset(n, 0, sizeof(*n));
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = old;
    if (!fh)
        return FALSE;
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (ExamineFH(fh, fib))
            len = fib->fib_Size;
        FreeDosObject(DOS_FIB, fib);
    }
    if (len <= 0 || len > NFO_MAX || !(buf = AllocVec(len + 1, MEMF_ANY))) {
        Close(fh);
        return FALSE;
    }
    len = Read(fh, buf, len);
    Close(fh);
    if (len <= 0) {
        FreeVec(buf);
        return FALSE;
    }
    buf[len] = 0;
    c.n = n;
    c.text = NULL;
    parse(buf, &c);
    if (c.text) {
        LONG k = strlen(c.text);
        if (k > TEXT_MAX) {
            k = TEXT_MAX;
            while (k > 0 && (c.text[k] & 0xC0) == 0x80)
                k--;                                /* not inside a character */
            c.text[k] = 0;
        }
        utf8_to_latin1(c.text);
        if ((n->text = AllocVec(strlen(c.text) + 1, MEMF_ANY)))
            strcpy(n->text, c.text);
    }
    FreeVec(buf);
    return n->kind != NFO_NONE;
}

void nfo_free(struct Nfo *n)
{
    FreeVec(n->text);
    n->text = NULL;
}

/* ---- finding them */

BOOL nfo_album_of(const char *dir, struct Nfo *n)
{
    char a[300], path[320];
    album_folder(dir, a, sizeof(a));
    join(path, sizeof(path), a, "album.nfo");
    if (nfo_read(path, n) && n->kind == NFO_ALBUM)
        return TRUE;
    nfo_free(n);
    return FALSE;
}

BOOL nfo_artist_of(const char *dir, BOOL album, struct Nfo *n)
{
    char a[300], up[300], path[320];
    const char *at = dir;
    if (album) {
        album_folder(dir, a, sizeof(a));
        if (!parent_of(a, up, sizeof(up)))
            return FALSE;
        at = up;
    }
    join(path, sizeof(path), at, "artist.nfo");
    if (nfo_read(path, n) && n->kind == NFO_ARTIST)
        return TRUE;
    nfo_free(n);
    return FALSE;
}

/* Kodi's pictures for a music folder (dir: an album's, a disc's, or an
 * artist's when artist), the first that is there into out. */
BOOL nfo_art(const char *dir, int what, BOOL artist, char *out, LONG max)
{
    static const char *const fanart[] = { "fanart.jpg", "fanart.png", "backdrop.jpg", NULL };
    static const char *const logo[] = { "logo.png", "clearlogo.png", NULL };
    static const char *const disc[] = { "discart.png", "cdart.png", "disc.png", NULL };
    static const char *const thumb[] = { "folder.jpg", "folder.png", "thumb.jpg", "artist.jpg", NULL };
    static const char *const banner[] = { "banner.jpg", "banner.png", NULL };
    const char *const *names;
    char a[300], up[300];
    const char *at;
    int i;
    out[0] = 0;
    switch (what) {
    case NFO_ART_FANART: names = fanart; break;
    case NFO_ART_LOGO:   names = logo;   break;
    case NFO_ART_DISC:   names = disc;   break;
    case NFO_ART_ARTIST: names = thumb;  break;
    case NFO_ART_BANNER: names = banner; break;
    default: return FALSE;
    }
    album_folder(dir, a, sizeof(a));
    at = a;
    /* the disc art is the album's; the rest are the artist's, the folder above */
    if (what != NFO_ART_DISC && !artist) {
        if (!parent_of(a, up, sizeof(up)))
            return FALSE;
        at = up;
    }
    for (i = 0; names[i]; i++) {
        join(out, max, at, names[i]);
        if (amce_exists(out))
            return TRUE;
    }
    if (what == NFO_ART_DISC) {
        /* a disc's own folder may hold its own */
        for (i = 0; names[i] && strcmp(a, dir); i++) {
            join(out, max, dir, names[i]);
            if (amce_exists(out))
                return TRUE;
        }
    }
    out[0] = 0;
    return FALSE;
}

/* ---- the details pane's lines */

/* An album's lines (after the file's or folder's own): its label, genres
 * and styles, and rating. Returns how many were added. */
int nfo_album_lines(const struct Nfo *n, char lines[][96], int max)
{
    int k = 0;
    char t[200];
    if (n->label[0] && k < max)
        amce_strlcpy(lines[k++], n->label, 96);
    t[0] = 0;
    add_each(t, sizeof(t), n->genres);
    add_each(t, sizeof(t), n->styles);
    if (t[0] && k < max)
        amce_strlcpy(lines[k++], t, 96);
    if (n->rating[0] && k < max)
        sprintf(lines[k++], _("Rating %s/%ld"), n->rating, (long)n->rating_max);
    return k;
}

int nfo_artist_lines(const struct Nfo *n, char lines[][96], int max)
{
    int k = 0;
    char t[200];
    t[0] = 0;
    add_each(t, sizeof(t), n->genres);
    add_each(t, sizeof(t), n->styles);
    if (t[0] && k < max)
        amce_strlcpy(lines[k++], t, 96);
    if (n->formed[0] && k < max) {
        char y[40];
        amce_strlcpy(y, n->formed, sizeof(y));
        if (n->ended[0]) {
            amce_strlcat(y, " - ", sizeof(y));
            amce_strlcat(y, n->ended, sizeof(y));
        }
        sprintf(lines[k++], n->born ? _("Born %s") : _("Formed %s"), y);
    }
    return k;
}
