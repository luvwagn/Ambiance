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
 * codec, length in ms, artist, album, its own key, and the file's bitrate,
 * channels and sample rate, the cover's key, the file's size, and its
 * album's and artist's rating keys (radio_field picks one out; the rate is
 * only in a track's own answer, plex_stream_rate, not in an album's).
 *
 * A track plays as stored (an MP3 file) or converted by the server
 * (Plex's universal transcoder, progressive MP3 over plain HTTP), made for
 * what this Amiga can decode (plex_tier: its CPU, and how fast it really
 * is, a short timed loop the first time):
 *   0: a 68020/030: 22050 Hz mono, 64 kbit/s (the floor: never less)
 *   1: a 68040/060: 24000 Hz stereo, 112 kbit/s
 *   2: a fast one (a quick 060, an emulated or accelerated 68k): 24000 Hz
 *      stereo, 160 kbit/s.
 * Paula plays up to about 28 kHz, so nothing asks for more than 24 kHz.
 * Settings > Networking > Plex > Request lower quality audio (on unless turned off;
 * offered on a fast Amiga only, always on otherwise) off: MP3s as stored,
 * decoded to Paula's rate (others converted either way). Plex
 * picks a profile by the client's platform and device: without them it
 * answers 400. The limits are on the music codec (scope musicCodec, as in
 * Plex's own profiles' MusicCodec, scopeName mp3): on audioCodec they were
 * let pass (stereo came back); scopeName * unencoded was a 400.
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
#define CONV_VAR   "AmigaMCE/PlexLowAudio"     /* was PlexConvert: on again by default */
#define ON_VAR     "AmigaMCE/Plex"
#define LIB_VAR    "AmigaMCE/PlexLibrary"
#define PHOTO_VAR  "AmigaMCE/PlexPhotos"    /* its photo libraries: "key\ttitle" lines */
#define LIST_MAX   1000         /* entries asked for at once */

struct PlexConf plex;
static char photo_libs[400];
static ULONG num(const char *s);
static ULONG sessions;          /* conversions asked for: each its own session */
static char last_session[64];   /* the conversion asked for last: stopped before the next */
static char last_item[160];     /* its track: asked for again (a seek), the same session */

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

void plex_load(UWORD cpu)
{
    char v[96];
    plex.cpu = cpu;
    get_var(SERVER_VAR, v, sizeof(v));
    take_server(v);
    get_var(NAME_VAR, plex.name, sizeof(plex.name));
    get_var(TOKEN_VAR, plex.token, sizeof(plex.token));
    get_var(CID_VAR, plex.cid, sizeof(plex.cid));
    get_var(CONV_VAR, v, sizeof(v));
    plex.convert = amce_stricmp(v, "off") != 0;     /* on unless turned off */
    get_var(ON_VAR, v, sizeof(v));
    plex.on = amce_stricmp(v, "off") != 0;
    if (GetVar((CONST_STRPTR)PHOTO_VAR, (STRPTR)photo_libs, sizeof(photo_libs),
               GVF_GLOBAL_ONLY) < 0)
        photo_libs[0] = 0;
    get_var(LIB_VAR, v, sizeof(v));         /* "<key> <count>" */
    {
        char *sp = strchr(v, ' ');
        if (sp) {
            *sp = 0;
            plex.libraries = (UWORD)num(sp + 1);
        }
        amce_strlcpy(plex.library, v, sizeof(plex.library));
    }
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
    plex_set_library("", 0);                /* another server: its libraries not known */
    plex_set_photo_libs("");
}

/* The server's photo libraries as last listed ("key\ttitle" lines; ""
 * not listed yet): Pictures shows each. */
const char *plex_photo_libs(void)
{
    return photo_libs;
}

void plex_set_photo_libs(const char *v)
{
    amce_strlcpy(photo_libs, v, sizeof(photo_libs));
    set_var(PHOTO_VAR, photo_libs);
}

/* The libraries' list asked for Pictures (its photo libraries), not Music. */
BOOL plex_photo_sections(const char *key)
{
    return !strncmp(key, "/library/sections?", 18) && strstr(key, "photos=1") != NULL;
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
    if (strstr(key, "/all?type="))
        /* a library's artists or albums: without their biographies and
         * reviews (over 1.5 MB for 900 artists, seconds to read on a
         * 68030); the one shown is asked for its own (plex_summary) */
        amce_strlcat(out, "&excludeFields=summary", max);
}

/* The music library Music opens on (key; count: the server's music
 * libraries, more than one: Music offers the others too). */
void plex_set_library(const char *key, UWORD count)
{
    char v[40];
    amce_strlcpy(plex.library, key, sizeof(plex.library));
    plex.libraries = count;
    sprintf(v, "%s %lu", key, (unsigned long)count);
    set_var(LIB_VAR, v);
}

void plex_set_on(BOOL on)
{
    plex.on = on;
    set_var(ON_VAR, on ? "on" : "off");
}

void plex_set_convert(BOOL on)
{
    plex.convert = on;
    set_var(CONV_VAR, on ? "on" : "off");
}

/* How much this Amiga can decode (see the top): its CPU, and a timed
 * loop of multiplies (thousands a second). Built for the 68020 they are
 * MULU.L, about 15 times what the 68000 build's library calls managed (an
 * emulated 68030 here: 530-790 then, 9660 now), so the marks are 15 times
 * theirs: 30000 for tier 1, 120000 for tier 2 (68060 class and Emu68). */
int plex_tier(void)
{
    static int tier = -1;
    if (tier < 0 && !time_ms())
        return plex.cpu >= 40 ? 1 : 0;      /* no timer yet: by the CPU, not kept */
    if (tier < 0) {
        static volatile ULONG sink __attribute__((unused));
        ULONG t0 = time_ms(), n = 0, x = 1, i, speed;
        char m[80];
        while (time_ms() - t0 < 100 && n < 100000) {
            for (i = 0; i < 1000; i++)
                x = x * 1103515245UL + 12345;
            n++;
        }
        sink = x;
        speed = n * 10;                     /* thousands a second */
        tier = (plex.cpu >= 40 || speed >= 30000) ? 1 : 0;
        if (speed >= 120000)
            tier = 2;
        sprintf(m, "%lu thousand multiplies a second, a 680%02ld: tier %ld", (unsigned long)speed,
                (long)plex.cpu, (long)tier);
        amce_log("plex: this Amiga", m);
    }
    return tier;
}

/* The CPU the decoder is told of (stream.c): a fast tier as a 68060. */
UWORD plex_decoder_cpu(void)
{
    return plex_tier() >= 2 && plex.cpu < 60 ? 60 : plex.cpu;
}

/* Where in a track's file (stored, not converted) from_ms is: its size in
 * proportion (an MP3's bitrate is near enough even), 0 not known. */
ULONG plex_track_byte(const struct Item *it, ULONG from_ms)
{
    char f[16];
    ULONG size, total;
    radio_field(it, 1, f, sizeof(f));
    if (amce_stricmp(f, "mp3"))
        return 0;                           /* FLAC: seeks by its frames, from the start */
    radio_field(it, 10, f, sizeof(f));
    size = num(f);
    radio_field(it, 2, f, sizeof(f));
    total = num(f);
    if (!size || !total || from_ms >= total)
        return 0;
    return (size / 1000) * (from_ms / 10) / (total / 10000);
}

/* A track is played converted by the server (see the top). */
BOOL plex_converts(const struct Item *it)
{
    char codec[16], key[160];
    radio_field(it, 1, codec, sizeof(codec));
    radio_field(it, 5, key, sizeof(key));
    if (!key[0])
        return FALSE;
    if (amce_stricmp(codec, "mp3") && !(plex.flac && !amce_stricmp(codec, "flac")))
        return TRUE;                        /* MP3, and FLAC with flacdec.library, as stored */
    if (plex.mhi && !amce_stricmp(codec, "mp3"))
        return plex.convert;                /* MHI decodes it, however fast the Amiga */
    return plex.convert || plex_tier() < 2;     /* a slower Amiga: always converted */
}

/* A track as a URL to play (out max bytes): its file, or the server's
 * conversion of it (cpu: 20, 30, 40, 60, which it is made for). */
void plex_track_url(const struct Item *it, UWORD cpu, ULONG from_ms, char *out, LONG max)
{
    char key[300], port[12], enc[400], n[160];
    int tier = plex.mhi ? 3 : plex_tier();  /* MHI: what its driver plays */
    BOOL mono = tier == 0;
    (void)cpu;
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
                   "&hasMDE=1&download=0&musicBitrate=%ld",
                (long)(tier == 0 ? 64 : tier == 1 ? 112 : tier == 2 ? 160 : 256));
        amce_strlcat(out, n, max);
        if (from_ms) {
            sprintf(n, "&offset=%lu", (unsigned long)(from_ms / 1000));    /* a seek */
            amce_strlcat(out, n, max);
        }
        if (last_session[0]) {
            /* the one before stopped first (the stream process asks): Plex
             * refuses another conversion of a track while one runs (400) */
            char stop[600];
            sprintf(stop, "http://%s:%lu/music/:/transcode/universal/stop?session=%s"
                          "&X-Plex-Client-Identifier=%s%s%s", plex.host,
                    (unsigned long)plex.port, last_session, plex.cid,
                    plex.token[0] ? "&X-Plex-Token=" : "", plex.token);
            st_set_stop_url(stop);
        }
        if (last_session[0] && !strcmp(last_item, key)) {
            /* the same track again (a seek): its session again, as Plex's
             * own players do; another session for it is refused (400) */
            sprintf(n, "&session=%s", last_session);
        } else {
            sprintf(n, "&session=amce%lu-%s", (unsigned long)++sessions, plex.cid + 9);
            amce_strlcpy(last_session, n + 9, sizeof(last_session));
        }
        amce_strlcpy(last_item, key, sizeof(last_item));
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
                          "%2Badd-limitation(scope%3DmusicCodec%26scopeName%3Dmp3"
                          "%26type%3DupperBound%26name%3Daudio.samplingRate%26value%3D",
                    max);
        amce_strlcat(out, mono ? "22050)" : tier >= 3 ? "48000)" : "24000)", max);
        if (mono)
            amce_strlcat(out, "%2Badd-limitation(scope%3DmusicCodec%26scopeName%3Dmp3"
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
    BOOL sections, albums_all, photos;
    LONG total, skipped, added;
    /* a track, while its Media and Part come */
    BOOL track;
    int track_depth;
    char t_title[128], t_by[200], t_album[100], t_codec[16], t_part[300], t_key[120];
    char t_thumb[160], t_album_rk[16], t_artist_rk[16];
    ULONG t_ms, t_kbps, t_ch, t_rate, t_size;
    /* a photo, while its Media and Part come */
    BOOL ph;
    int ph_depth;
    char ph_title[128], ph_thumb[160], ph_part[300], ph_date[16], ph_summary[300];
    ULONG ph_w, ph_h;
    BOOL full;
};

/* A photo (Photo, in a photo library): its key (the file as stored), then
 * by tabs as a folder's details are, so the pane shows it: "photo", its
 * picture's key, "" (no year: the date has it), size, date, "", summary. */
static void flush_photo(struct fill *x)
{
    char path[1100], dims[32];
    struct Item *it;
    if (!x->ph)
        return;
    x->ph = FALSE;
    if (!x->ph_thumb[0] && !x->ph_part[0]) {
        x->skipped++;
        return;
    }
    dims[0] = 0;
    if (x->ph_w && x->ph_h)
        sprintf(dims, "%lu x %lu", (unsigned long)x->ph_w, (unsigned long)x->ph_h);
    sprintf(path, "%s\tphoto\t%s\t\t%s\t%s\t\t%s", x->ph_part[0] ? x->ph_part : "-",
            x->ph_thumb[0] ? x->ph_thumb : x->ph_part, dims, x->ph_date, x->ph_summary);
    if (!(it = list_add(x->l, x->ph_title[0] ? x->ph_title : "(no title)", path, x->ph_date,
                        IT_PLEXPHOTO))) {
        x->full = TRUE;
        return;
    }
    x->added++;
}

static void flush_track(struct fill *x)
{
    char path[1100], tag[24];
    struct Item *it;
    if (!x->track)
        return;
    x->track = FALSE;
    if (!x->t_part[0]) {
        x->skipped++;
        return;
    }
    sprintf(path, "%s\t%s\t%lu\t%s\t%s\t%s\t%lu\t%lu\t%lu\t%s\t%lu\t%s\t%s", x->t_part,
            x->t_codec, (unsigned long)x->t_ms, x->t_by, x->t_album, x->t_key,
            (unsigned long)x->t_kbps, (unsigned long)x->t_ch, (unsigned long)x->t_rate,
            x->t_thumb, (unsigned long)x->t_size, x->t_album_rk, x->t_artist_rk);
    dur_text(x->t_ms, tag);
    if (!(it = list_add(x->l, x->t_title[0] ? x->t_title : "(no title)", path, tag,
                        IT_PLEXTRACK))) {
        x->full = TRUE;
        return;
    }
    if (amce_stricmp(x->t_codec, "mp3") && !(plex.flac && !amce_stricmp(x->t_codec, "flac")) &&
        !x->t_key[0])
        it->flags |= ITEM_DISABLED;         /* not playable as stored, and no key to convert it by */
    x->added++;
}

static int element(void *ctx, const char *name, char **a, int n, int depth)
{
    struct fill *x = ctx;
    char title[160], key[1000], tag[24];
    const char *v;
    if (x->full)
        return 0;
    if (x->track && depth <= x->track_depth)
        flush_track(x);                     /* a sibling: the track before is complete */
    if (x->ph && depth <= x->ph_depth)
        flush_photo(x);
    if (x->ph && !strcmp(name, "Media")) {
        x->ph_w = num(xml_attr(a, n, "width"));
        x->ph_h = num(xml_attr(a, n, "height"));
        return 1;
    }
    if (x->ph && !strcmp(name, "Part")) {
        if (!x->ph_part[0] && (v = xml_attr(a, n, "key")))
            amce_strlcpy(x->ph_part, v, sizeof(x->ph_part));
        return 1;
    }
    if (!strcmp(name, "Photo") && depth == 1) {
        x->ph = TRUE;
        x->ph_depth = depth;
        text(xml_attr(a, n, "title"), x->ph_title, sizeof(x->ph_title));
        amce_strlcpy(x->ph_thumb, xml_attr(a, n, "thumb") ? xml_attr(a, n, "thumb") : "",
                     sizeof(x->ph_thumb));
        text(xml_attr(a, n, "originallyAvailableAt"), x->ph_date, sizeof(x->ph_date));
        if (!x->ph_date[0])
            text(xml_attr(a, n, "year"), x->ph_date, sizeof(x->ph_date));
        text(xml_attr(a, n, "summary"), x->ph_summary, sizeof(x->ph_summary));
        x->ph_part[0] = 0;
        x->ph_w = x->ph_h = 0;
        return 1;
    }
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
        x->t_kbps = num(xml_attr(a, n, "bitrate"));
        x->t_ch = num(xml_attr(a, n, "audioChannels"));
        return 1;
    }
    if (x->track && !strcmp(name, "Stream") && num(xml_attr(a, n, "streamType")) == 2) {
        x->t_rate = num(xml_attr(a, n, "samplingRate"));
        if (!x->t_ch)
            x->t_ch = num(xml_attr(a, n, "channels"));
        return 1;
    }
    if (x->track && !strcmp(name, "Part")) {
        if (!x->t_part[0] && (v = xml_attr(a, n, "key"))) {
            amce_strlcpy(x->t_part, v, sizeof(x->t_part));
            x->t_size = num(xml_attr(a, n, "size"));
        }
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
        v = xml_attr(a, n, "parentThumb");      /* the album's cover, else its own */
        if (!v || !v[0])
            v = xml_attr(a, n, "thumb");
        amce_strlcpy(x->t_thumb, v ? v : "", sizeof(x->t_thumb));
        /* its album and artist, so their rows can show it playing */
        text(xml_attr(a, n, "parentRatingKey"), x->t_album_rk, sizeof(x->t_album_rk));
        text(xml_attr(a, n, "grandparentRatingKey"), x->t_artist_rk, sizeof(x->t_artist_rk));
        x->t_ms = num(xml_attr(a, n, "duration"));
        x->t_kbps = x->t_ch = x->t_rate = x->t_size = 0;
        return 1;
    }
    if (strcmp(name, "Directory") || depth != 1)
        return 1;
    /* a folder of the server's: a library, an artist, an album */
    v = xml_attr(a, n, "type");
    if (x->sections && x->photos) {
        /* Pictures' libraries: the photo ones, each opened at its top */
        if (!v || strcmp(v, "photo")) {
            x->skipped++;
            return 1;
        }
        text(xml_attr(a, n, "title"), title, sizeof(title));
        sprintf(key, "/library/sections/%.40s/all",
                xml_attr(a, n, "key") ? xml_attr(a, n, "key") : "");
        tag[0] = 0;
    } else if (x->sections) {
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
        BOOL photo = v && !strcmp(v, "photo");     /* an album of a photo library */
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
        else if (photo && (v = xml_attr(a, n, "leafCount")) && num(v))
            sprintf(tag, num(v) == 1 ? "%lu photo" : "%lu photos", (unsigned long)num(v));
        else if (photo && (v = xml_attr(a, n, "childCount")) && num(v))
            sprintf(tag, num(v) == 1 ? "%lu item" : "%lu items", (unsigned long)num(v));
        else if ((v = xml_attr(a, n, "leafCount")) && num(v))
            sprintf(tag, num(v) == 1 ? "%lu track" : "%lu tracks", (unsigned long)num(v));
        {
            /* what the details pane shows of it: after the key, by tabs */
            static char more[700];
            char type[16], thumb[160], year[8], by[100], count[24], studio[60], summary[300];
            text(xml_attr(a, n, "type"), type, sizeof(type));
            amce_strlcpy(thumb, xml_attr(a, n, "thumb") ? xml_attr(a, n, "thumb") : "",
                         sizeof(thumb));
            text(xml_attr(a, n, "year"), year, sizeof(year));
            text(xml_attr(a, n, "parentTitle"), by, sizeof(by));
            count[0] = 0;
            if (!strcmp(type, "artist") && num(xml_attr(a, n, "childCount")))
                sprintf(count, num(xml_attr(a, n, "childCount")) == 1 ? "%lu album" : "%lu albums",
                        (unsigned long)num(xml_attr(a, n, "childCount")));
            else if (!strcmp(type, "photo"))
                amce_strlcpy(count, tag, sizeof(count));    /* photos, or items */
            else if (num(xml_attr(a, n, "leafCount")))
                sprintf(count, num(xml_attr(a, n, "leafCount")) == 1 ? "%lu track" : "%lu tracks",
                        (unsigned long)num(xml_attr(a, n, "leafCount")));
            text(xml_attr(a, n, "studio"), studio, sizeof(studio));
            text(xml_attr(a, n, "summary"), summary, sizeof(summary));
            sprintf(more, "\t%s\t%s\t%s\t%s\t%s\t%s\t%s", type, thumb, year, by, count, studio,
                    summary);
            amce_strlcat(key, more, sizeof(key));
        }
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
    x.sections = !strcmp(key, "/library/sections") || plex_photo_sections(key);
    x.photos = plex_photo_sections(key);
    x.albums_all = strstr(key, "type=9") != NULL;
    if (len < 16 || !strstr((char *)body, "<MediaContainer"))
        return -1;
    xml_scan((char *)body, len, element, &x);
    flush_track(&x);
    flush_photo(&x);
    if (total)
        *total = x.total;
    return x.added;
}

static int summary_el(void *ctx, const char *name, char **a, int n, int depth)
{
    const char **out = ctx;
    const char *v;
    if (depth == 1 && (v = xml_attr(a, n, "summary"))) {
        *out = v;
        return 0;
    }
    return depth < 1 || strcmp(name, "Directory");  /* the first one only */
}

/* An artist's or album's own answer (/library/metadata/<n>): its
 * summary (a biography, a review) into out. FALSE: none. */
BOOL plex_summary(UBYTE *body, ULONG len, char *out, LONG max)
{
    const char *v = NULL;
    out[0] = 0;
    xml_scan((char *)body, len, summary_el, &v);
    if (v)
        text(v, out, max);
    return out[0] != 0;
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

static int rate_el(void *ctx, const char *name, char **a, int n, int depth)
{
    ULONG *r = ctx;
    (void)depth;
    if (!strcmp(name, "Stream") && num(xml_attr(a, n, "streamType")) == 2) {
        *r = num(xml_attr(a, n, "samplingRate"));
        return 0;
    }
    return 1;
}

/* A track's own answer (its key asked for): its audio's sample rate. */
ULONG plex_stream_rate(UBYTE *body, ULONG len)
{
    ULONG r = 0;
    xml_scan((char *)body, len, rate_el, &r);
    return r;
}

/* A picture of the server's (thumb: its key for it, an album's cover)
 * made w x h by the server (its photo transcoder: PNG, scaled there, so
 * the Amiga decodes no more than it shows; PNG for its cheaper decode than
 * JPEG's, notes/cover-decode-times.md): the request into req, the
 * file it is kept in (RAM:, by the key and size) into file. TRUE if that
 * file is there already. */
BOOL plex_art(const char *thumb, WORD w, WORD h, char *req, LONG rmax, char *file, LONG fmax)
{
    char enc[400], n[96];
    ULONG hash = 5381;
    const char *s;
    BPTR l;
    BOOL jpg;
    for (s = thumb; *s; s++)
        hash = hash * 33 + (UBYTE)*s;
    if ((l = Lock((CONST_STRPTR)"RAM:AmigaMCE-art", SHARED_LOCK)))
        UnLock(l);
    else if ((l = CreateDir((CONST_STRPTR)"RAM:AmigaMCE-art")))
        UnLock(l);
    /* bigger than a 640 x 512 screen (a photo on RTG): JPEG, a tenth of a
     * PNG's size (a 1024 x 768 PNG of a photo is past what is read, 2 MB) */
    jpg = (LONG)w * h > 640L * 512;
    sprintf(file, "RAM:AmigaMCE-art/%08lx-%ldx%ld-in.%s", (unsigned long)hash, (long)w, (long)h,
            jpg ? "jpg" : "png");
    (void)fmax;
    url_encode(thumb, enc, sizeof(enc));
    /* minSize=0: within w x h, so never bigger than it is shown (a tall
     * cover came 160 x 279, read and averaged down to 91 x 160) */
    sprintf(n, "/photo/:/transcode?width=%ld&height=%ld&minSize=0&upscale=1&format=%s&url=",
            (long)w, (long)h, jpg ? "jpeg" : "png");
    {
        char k[520];
        amce_strlcpy(k, n, sizeof(k));
        amce_strlcat(k, enc, sizeof(k));
        plex_request(k, req, rmax);
    }
    if ((l = Lock((CONST_STRPTR)file, SHARED_LOCK))) {
        UnLock(l);
        return TRUE;
    }
    return FALSE;
}

/* A picture the server sent (plex_art) kept in file. */
BOOL plex_art_save(const char *file, const UBYTE *body, ULONG len)
{
    BPTR fh = Open((CONST_STRPTR)file, MODE_NEWFILE);
    LONG w;
    if (!fh)
        return FALSE;
    w = Write(fh, (APTR)body, len);
    Close(fh);
    if (w != (LONG)len) {
        DeleteFile((CONST_STRPTR)file);
        return FALSE;
    }
    return TRUE;
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
