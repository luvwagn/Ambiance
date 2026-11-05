/*
 * AmigaMCE - Plex: a Plex Media Server's music, browsed and played
 * Copyright (c) 2026 Brian King
 *
 * A Plex Media Server on the home network answers plain HTTP on port
 * 32400 (its "Secure connections: Preferred" falls back to it for clients
 * that have no TLS), so the network process asks it as it asks
 * radio-browser.info (net.c), and its MP3 files play as the radio's
 * streams do (stream.c), with an end. The server is found on the network
 * by GDM (net.c: a UDP "M-SEARCH" the servers answer), or named in
 * ENVARC:AmigaMCE/PlexServer ("address:port"). Its sign-in token, which a
 * server asks for unless it lets this address in without one, is
 * ENVARC:AmigaMCE/PlexToken (from the Shell: SetEnv SAVE AmigaMCE/PlexToken
 * ...; signing in at plex.tv needs HTTPS, so AmiSSL, and is for later).
 *
 * The lists are pages of items, as Radio's: IT_PLEXDIR rows open another
 * list (path: the server's key for it, "@<n>" a music library's own menu,
 * "#gdm" the servers found), IT_PLEXTRACK rows are tracks, IT_PLEXSRV rows
 * servers found. A track's path: its file's key, then, after tabs, its
 * codec, length in ms, artist, album and its own key (radio_field picks
 * one out).
 *
 * A track plays as stored (an MP3 file) or converted by the server
 * (Plex's universal transcoder, progressive MP3 over plain HTTP): all of
 * them when Settings > Plex: convert music is on (the default), the ones
 * not MP3 always. Converted to 22050 Hz at most, so the Amiga decodes half
 * the samples of a 44.1 kHz file: mono at 64 kbit/s for a 68030 or less
 * (it decodes mono anyway), stereo at 96 kbit/s for a 68040/060. Plex
 * picks a profile by the client's platform and device: without them it
 * answers 400.
 */
#include "amce.h"
#include "plexxml.h"
#include "radiocsv.h"

#include <string.h>
#include <stdio.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define SERVER_VAR "AmigaMCE/PlexServer"
#define NAME_VAR   "AmigaMCE/PlexName"
#define TOKEN_VAR  "AmigaMCE/PlexToken"
#define CID_VAR    "AmigaMCE/PlexClient"
#define CONV_VAR   "AmigaMCE/PlexConvert"
#define LIST_MAX   1000         /* entries asked for at once */

struct PlexConf plex;
static ULONG sessions;          /* conversions asked for: each its own session */

static void get_var(const char *name, char *out, LONG max)
{
    out[0] = 0;
    if (GetVar((CONST_STRPTR)name, (STRPTR)out, max, GVF_GLOBAL_ONLY) < 0)
        out[0] = 0;
    squeeze_spaces(out);
}

static void set_var(const char *name, const char *value)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    BPTR l;
    me->pr_WindowPtr = (APTR)-1;
    if ((l = Lock((CONST_STRPTR)"ENVARC:AmigaMCE", SHARED_LOCK)))
        UnLock(l);
    else if ((l = CreateDir((CONST_STRPTR)"ENVARC:AmigaMCE")))
        UnLock(l);
    if ((l = Lock((CONST_STRPTR)"ENV:AmigaMCE", SHARED_LOCK)))
        UnLock(l);
    else if ((l = CreateDir((CONST_STRPTR)"ENV:AmigaMCE")))
        UnLock(l);
    SetVar((CONST_STRPTR)name, (CONST_STRPTR)value, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    me->pr_WindowPtr = oldwin;
}

/* "address[:port]" into plex.host and plex.port. */
static void take_server(const char *s)
{
    const char *c = strchr(s, ':');
    LONG n = c ? c - s : (LONG)strlen(s), p = 0;
    plex.host[0] = 0;
    plex.port = 32400;
    if (n <= 0 || n >= (LONG)sizeof(plex.host))
        return;
    memcpy(plex.host, s, n);
    plex.host[n] = 0;
    if (c)
        for (c++; *c >= '0' && *c <= '9'; c++)
            p = p * 10 + (*c - '0');
    if (p > 0 && p < 65536)
        plex.port = (UWORD)p;
}

void plex_load(void)
{
    char v[96];
    get_var(SERVER_VAR, v, sizeof(v));
    take_server(v);
    get_var(NAME_VAR, plex.name, sizeof(plex.name));
    get_var(TOKEN_VAR, plex.token, sizeof(plex.token));
    get_var(CID_VAR, plex.cid, sizeof(plex.cid));
    get_var(CONV_VAR, v, sizeof(v));
    plex.convert = amce_stricmp(v, "off") != 0;     /* on unless turned off */
    if (!plex.cid[0]) {
        /* who this AmigaMCE is, to the server: made up once, then kept */
        ULONG a = time_ms() ^ (ULONG)FindTask(NULL), b = (ULONG)AvailMem(MEMF_ANY) * 2654435761UL;
        sprintf(plex.cid, "amigamce-%08lx%08lx", (unsigned long)a, (unsigned long)b);
        set_var(CID_VAR, plex.cid);
    }
}

void plex_set_server(const char *host, UWORD port, const char *name)
{
    char v[96];
    sprintf(v, "%s:%lu", host, (unsigned long)port);
    take_server(v);
    set_var(SERVER_VAR, v);
    amce_strlcpy(plex.name, name[0] ? name : host, sizeof(plex.name));
    set_var(NAME_VAR, plex.name);
}

/* The request for key (a path on the server) into out: who asks, its
 * token, and (lists) how many entries. */
void plex_request(const char *key, char *out, LONG max)
{
    char n[64];
    amce_strlcpy(out, key, max);
    amce_strlcat(out, strchr(key, '?') ? "&" : "?", max);
    amce_strlcat(out, "X-Plex-Product=AmigaMCE&X-Plex-Version=" AMCE_VERSION
                      "&X-Plex-Platform=AmigaOS&X-Plex-Client-Identifier=", max);
    amce_strlcat(out, plex.cid, max);
    if (plex.token[0]) {
        amce_strlcat(out, "&X-Plex-Token=", max);
        amce_strlcat(out, plex.token, max);
    }
    sprintf(n, "&X-Plex-Container-Start=0&X-Plex-Container-Size=%ld", (long)LIST_MAX);
    amce_strlcat(out, n, max);
}

void plex_set_convert(BOOL on)
{
    plex.convert = on;
    set_var(CONV_VAR, on ? "on" : "off");
}

/* A track is played converted by the server (see the top). */
BOOL plex_converts(const struct Item *it)
{
    char codec[16], key[160];
    radio_field(it, 1, codec, sizeof(codec));
    radio_field(it, 5, key, sizeof(key));
    return key[0] && (plex.convert || amce_stricmp(codec, "mp3"));
}

/* A track as a URL to play (out max bytes): its file, or the server's
 * conversion of it (cpu: 20, 30, 40, 60, which it is made for). */
void plex_track_url(const struct Item *it, UWORD cpu, char *out, LONG max)
{
    char key[300], port[12], enc[400], n[160];
    BOOL mono = cpu < 40;
    sprintf(port, ":%lu", (unsigned long)plex.port);
    amce_strlcpy(out, "http://", max);
    amce_strlcat(out, plex.host, max);
    amce_strlcat(out, port, max);
    if (plex_converts(it)) {
        radio_field(it, 5, key, sizeof(key));
        url_encode(key, enc, sizeof(enc));
        amce_strlcat(out, "/music/:/transcode/universal/start.mp3?path=", max);
        amce_strlcat(out, enc, max);
        sprintf(n, "&mediaIndex=0&partIndex=0&protocol=http&directPlay=0&directStream=0"
                   "&hasMDE=1&download=0&musicBitrate=%ld", (long)(mono ? 64 : 96));
        amce_strlcat(out, n, max);
        sprintf(n, "&session=amce%lu-%s", (unsigned long)++sessions, plex.cid + 9);
        amce_strlcat(out, n, max);
        amce_strlcat(out, "&X-Plex-Session-Identifier=", max);
        amce_strlcat(out, n + 9, max);
        amce_strlcat(out, "&X-Plex-Client-Identifier=", max);
        amce_strlcat(out, plex.cid, max);
        amce_strlcat(out, "&X-Plex-Product=AmigaMCE&X-Plex-Version=" AMCE_VERSION
                          "&X-Plex-Platform=Generic&X-Plex-Platform-Version=3"
                          "&X-Plex-Device=Amiga&X-Plex-Model=Amiga"
                          "&X-Plex-Client-Profile-Extra="
                          "add-transcode-target(type%3DmusicProfile%26context%3Dstreaming"
                          "%26protocol%3Dhttp%26container%3Dmp3%26audioCodec%3Dmp3)"
                          "%2Badd-limitation(scope%3DaudioCodec%26scopeName%3Dmp3"
                          "%26type%3DupperBound%26name%3Daudio.samplingRate%26value%3D22050)",
                    max);
        if (mono)
            amce_strlcat(out, "%2Badd-limitation(scope%3DaudioCodec%26scopeName%3Dmp3"
                              "%26type%3DupperBound%26name%3Daudio.channels%26value%3D1)",
                         max);
        if (plex.token[0]) {
            amce_strlcat(out, "&X-Plex-Token=", max);
            amce_strlcat(out, plex.token, max);
        }
        return;
    }
    radio_field(it, 0, key, sizeof(key));
    amce_strlcat(out, key, max);
    if (plex.token[0]) {
        amce_strlcat(out, strchr(key, '?') ? "&" : "?", max);
        amce_strlcat(out, "X-Plex-Token=", max);
        amce_strlcat(out, plex.token, max);
    }
}

/* A music library's own menu (its key: the library's number). */
void plex_section_menu(const char *sec, struct ItemList *l)
{
    static const struct { const char *label, *fmt; } rows[] = {
        { "Artists", "/library/sections/%s/all?type=8&sort=titleSort" },
        { "Albums", "/library/sections/%s/all?type=9&sort=titleSort" },
        { "Recently added", "/library/sections/%s/recentlyAdded" },
    };
    char k[160];
    int i;
    for (i = 0; i < 3; i++) {
        struct Item *it;
        sprintf(k, rows[i].fmt, sec);
        if ((it = list_add(l, rows[i].label, k, "", IT_PLEXDIR)))
            it->mark = MARK_ARROW;
    }
}

/* m:ss (h:mm:ss) of ms into out. */
static void dur_text(ULONG ms, char *out)
{
    ULONG s = (ms + 500) / 1000;
    if (s >= 3600)
        sprintf(out, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                (unsigned long)(s % 60));
    else
        sprintf(out, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static ULONG num(const char *s)
{
    ULONG v = 0;
    while (s && *s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static void text(const char *in, char *out, LONG max)
{
    amce_strlcpy(out, in ? in : "", max);
    utf8_to_latin1(out);
    squeeze_spaces(out);
}

struct fill {
    struct ItemList *l;
    const char *base;           /* the request's key: relative keys are under it */
    BOOL sections, albums_all;
    LONG total, skipped, added;
    /* a track, while its Media and Part come */
    BOOL track;
    int track_depth;
    char t_title[128], t_by[200], t_album[100], t_codec[16], t_part[300], t_key[120];
    ULONG t_ms;
    BOOL full;
};

static void flush_track(struct fill *x)
{
    char path[900], tag[24];
    struct Item *it;
    if (!x->track)
        return;
    x->track = FALSE;
    if (!x->t_part[0]) {
        x->skipped++;
        return;
    }
    sprintf(path, "%s\t%s\t%lu\t%s\t%s\t%s", x->t_part, x->t_codec, (unsigned long)x->t_ms,
            x->t_by, x->t_album, x->t_key);
    dur_text(x->t_ms, tag);
    if (!(it = list_add(x->l, x->t_title[0] ? x->t_title : "(no title)", path, tag,
                        IT_PLEXTRACK))) {
        x->full = TRUE;
        return;
    }
    if (amce_stricmp(x->t_codec, "mp3") && !x->t_key[0])
        it->flags |= ITEM_DISABLED;         /* not MP3, and no key to convert it by */
    x->added++;
}

static int element(void *ctx, const char *name, char **a, int n, int depth)
{
    struct fill *x = ctx;
    char title[160], key[300], tag[24];
    const char *v;
    if (x->full)
        return 0;
    if (x->track && depth <= x->track_depth)
        flush_track(x);                     /* a sibling: the track before is complete */
    if (!strcmp(name, "MediaContainer")) {
        x->total = num(xml_attr(a, n, "totalSize"));
        if (!x->total)
            x->total = num(xml_attr(a, n, "size"));
        return 1;
    }
    if (x->track && !strcmp(name, "Media")) {
        text(xml_attr(a, n, "audioCodec"), x->t_codec, sizeof(x->t_codec));
        if (!x->t_ms)
            x->t_ms = num(xml_attr(a, n, "duration"));
        return 1;
    }
    if (x->track && !strcmp(name, "Part")) {
        if (!x->t_part[0] && (v = xml_attr(a, n, "key")))
            amce_strlcpy(x->t_part, v, sizeof(x->t_part));
        return 1;
    }
    if (!strcmp(name, "Track")) {
        x->track = TRUE;
        x->track_depth = depth;
        text(xml_attr(a, n, "title"), x->t_title, sizeof(x->t_title));
        text(xml_attr(a, n, "originalTitle"), x->t_by, sizeof(x->t_by));
        if (!x->t_by[0])
            text(xml_attr(a, n, "grandparentTitle"), x->t_by, sizeof(x->t_by));
        text(xml_attr(a, n, "parentTitle"), x->t_album, sizeof(x->t_album));
        x->t_codec[0] = x->t_part[0] = 0;
        amce_strlcpy(x->t_key, xml_attr(a, n, "key") ? xml_attr(a, n, "key") : "",
                     sizeof(x->t_key));
        x->t_ms = num(xml_attr(a, n, "duration"));
        return 1;
    }
    if (strcmp(name, "Directory") || depth != 1)
        return 1;
    /* a folder of the server's: a library, an artist, an album */
    v = xml_attr(a, n, "type");
    if (x->sections) {
        /* the libraries: only music ones, each its own menu (@<key>) */
        if (!v || strcmp(v, "artist")) {
            x->skipped++;
            return 1;
        }
        text(xml_attr(a, n, "title"), title, sizeof(title));
        key[0] = '@';
        amce_strlcpy(key + 1, xml_attr(a, n, "key") ? xml_attr(a, n, "key") : "", sizeof(key) - 1);
        tag[0] = 0;
    } else {
        const char *k = xml_attr(a, n, "key");
        if (!k) {
            x->skipped++;
            return 1;
        }
        text(xml_attr(a, n, "title"), title, sizeof(title));
        if (x->albums_all && v && !strcmp(v, "album")) {
            char by[100];
            text(xml_attr(a, n, "parentTitle"), by, sizeof(by));
            if (by[0]) {
                amce_strlcat(title, " - ", sizeof(title));
                amce_strlcat(title, by, sizeof(title));
            }
        }
        if (k[0] == '/') {
            amce_strlcpy(key, k, sizeof(key));
        } else {
            /* relative: under the request's own key, its query left off */
            const char *q = strchr(x->base, '?');
            LONG bl = q ? q - x->base : (LONG)strlen(x->base);
            if (bl >= (LONG)sizeof(key) - 2)
                bl = sizeof(key) - 2;
            memcpy(key, x->base, bl);
            key[bl] = 0;
            if (bl && key[bl - 1] != '/')
                amce_strlcat(key, "/", sizeof(key));
            amce_strlcat(key, k, sizeof(key));
        }
        tag[0] = 0;
        if ((v = xml_attr(a, n, "year")))
            amce_strlcpy(tag, v, sizeof(tag));
        else if ((v = xml_attr(a, n, "leafCount")) && num(v))
            sprintf(tag, num(v) == 1 ? "%lu track" : "%lu tracks", (unsigned long)num(v));
    }
    {
        struct Item *it = list_add(x->l, title[0] ? title : "(no name)", key, tag, IT_PLEXDIR);
        if (!it) {
            x->full = TRUE;
            return 0;
        }
        it->mark = MARK_ARROW;
        x->added++;
    }
    return 1;
}

/* The answer to the request for key (body, len bytes and a byte spare,
 * changed) into l: the entries added, -1 if it was not Plex's XML.
 * *total: how many the server has (more than LIST_MAX: not all shown). */
LONG plex_fill(const char *key, UBYTE *body, ULONG len, struct ItemList *l, LONG *total)
{
    static struct fill x;               /* static: too big for the stack */
    memset(&x, 0, sizeof(x));
    x.l = l;
    x.base = key;
    x.sections = !strcmp(key, "/library/sections");
    x.albums_all = strstr(key, "type=9") != NULL;
    if (len < 16 || !strstr((char *)body, "<MediaContainer"))
        return -1;
    xml_scan((char *)body, len, element, &x);
    flush_track(&x);
    if (total)
        *total = x.total;
    return x.added;
}

/* The servers GDM found (net.c: "address\tport\tname" lines) into l. */
LONG plex_fill_servers(UBYTE *body, ULONG len, struct ItemList *l)
{
    char *p = (char *)body, *e = p + len, line[200];
    LONG n = 0;
    while (p < e) {
        char *nl = memchr(p, '\n', e - p), *t1, *t2, path[400], tag[40];
        LONG k = nl ? nl - p : e - p;
        if (k >= (LONG)sizeof(line))
            k = sizeof(line) - 1;
        memcpy(line, p, k);
        line[k] = 0;
        p = nl ? nl + 1 : e;
        if (!(t1 = strchr(line, '\t')) || !(t2 = strchr(t1 + 1, '\t')))
            continue;
        *t1++ = 0;
        *t2++ = 0;
        utf8_to_latin1(t2);
        sprintf(path, "%s:%s\t%s", line, t1, t2);
        amce_strlcpy(tag, line, sizeof(tag));
        if (list_add(l, t2[0] ? t2 : line, path, tag, IT_PLEXSRV))
            n++;
    }
    return n;
}

/* Why a track cannot play (a greyed one). */
const char *plex_why(const struct Item *it)
{
    static char m[80];
    char codec[16];
    radio_field(it, 1, codec, sizeof(codec));
    sprintf(m, "%s: not MP3, and the server gave no way to convert it",
            codec[0] ? codec : "That format");
    return m;
}
