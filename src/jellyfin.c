/*
 * Ambiance - Jellyfin: a Jellyfin server's music, films and photos
 * Copyright (c) 2026 Brian King
 *
 * Jellyfin (jellyfin.org) is a free media server, as Plex is. It answers
 * plain HTTP on port 8096, in JSON (jfjson.c), and makes what a client
 * asks for: MP3 of any track, MPEG-1 or MPEG-2 of any film (Plex will
 * not: claude/plex-jellyfin-mpeg-transcode.md), photos and covers scaled
 * to the size shown.
 *
 * Its pages are Plex's: the same kinds of row (IT_PLEXDIR, IT_PLEXTRACK,
 * IT_PLEXPHOTO) with the same fields after tabs (plex.c), so album walls,
 * an artist's and an album's pages, Now playing, covers and the layouts'
 * screens are the ones Plex's use. Only the keys differ: a Jellyfin key
 * begins with '~', and plex.c hands every such key here (plex_job,
 * plex_fill, plex_track_url, plex_art). After the '~', a letter says what
 * it lists:
 *   V m|v|p     the user's libraries, for Music, Videos or Pictures
 *   M <lib>     a music library's own menu (made here, nothing asked)
 *   A <lib>     its album artists      L <lib>  its albums
 *   R <lib>     its albums, newest first
 *   a <artist>  an artist's albums     t <album> an album's tracks
 *   F p|v <id>  a folder (a library, a series, a photo album), for
 *               Pictures (no videos) or Videos (no photos)
 *   I <id>/<tag> an item's primary picture (a cover, a photo)
 *   Y <id>      a film or episode (Videos plays it: jf_video_url)
 *   S <id>      a track, to play (jf_track_url)
 *
 * Who is asking goes in every request's Authorization header (the client,
 * this Amiga, a device id made once and kept, and the sign-in's token);
 * stream and picture URLs, opened elsewhere, carry the token as ApiKey.
 * Sign in is a user name and password (Settings > Networking > Jellyfin),
 * sent once; the token is kept (ENVARC:Ambiance/JellyfinToken), the
 * password never.
 */
#include "amb.h"
#include "jfjson.h"
#include "radiocsv.h"

#include <string.h>
#include <stdio.h>
#include <dos/var.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define ON_VAR     "Ambiance/Jellyfin"
#define SERVER_VAR "Ambiance/JellyfinServer"    /* "host:port" */
#define NAME_VAR   "Ambiance/JellyfinName"
#define ID_VAR     "Ambiance/JellyfinId"        /* the server's id (discovery) */
#define USER_VAR   "Ambiance/JellyfinUser"      /* the user signed in as */
#define UID_VAR    "Ambiance/JellyfinUserId"
#define TOKEN_VAR  "Ambiance/JellyfinToken"
#define DEV_VAR    "Ambiance/JellyfinDevice"    /* this Amiga's device id, made once */
#define VSIZE_VAR  "Ambiance/JellyfinVideo"     /* films: "auto", "small", "large" */

#define PART 1000                   /* entries asked for at once (plex.c's PLEX_PART) */

struct JfConf jf;

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
    if ((l = Lock((CONST_STRPTR)"ENVARC:Ambiance", SHARED_LOCK)))
        UnLock(l);
    else if ((l = CreateDir((CONST_STRPTR)"ENVARC:Ambiance")))
        UnLock(l);
    if ((l = Lock((CONST_STRPTR)"ENV:Ambiance", SHARED_LOCK)))
        UnLock(l);
    else if ((l = CreateDir((CONST_STRPTR)"ENV:Ambiance")))
        UnLock(l);
    if (value[0])
        SetVar((CONST_STRPTR)name, (CONST_STRPTR)value, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    else
        DeleteVar((CONST_STRPTR)name, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    me->pr_WindowPtr = oldwin;
}

/* "http://host:port", "host:port" or "host" into jf.host and jf.port. */
static void take_server(const char *s)
{
    const char *c;
    LONG n, p = 0;
    jf.host[0] = 0;
    jf.port = 8096;
    if (!strncmp(s, "http://", 7))
        s += 7;
    for (c = s; *c && *c != ':' && *c != '/'; c++)
        ;
    n = c - s;
    if (n <= 0 || n >= (LONG)sizeof(jf.host))
        return;
    memcpy(jf.host, s, n);
    jf.host[n] = 0;
    if (*c == ':')
        for (c++; *c >= '0' && *c <= '9'; c++)
            p = p * 10 + (*c - '0');
    if (p > 0 && p < 65536)
        jf.port = (UWORD)p;
}

void jf_load(void)
{
    char v[96];
    get_var(ON_VAR, v, sizeof(v));
    jf.on = amb_stricmp(v, "off") != 0;
    get_var(SERVER_VAR, v, sizeof(v));
    take_server(v);
    get_var(NAME_VAR, jf.name, sizeof(jf.name));
    get_var(ID_VAR, jf.sid, sizeof(jf.sid));
    get_var(USER_VAR, jf.user, sizeof(jf.user));
    get_var(UID_VAR, jf.uid, sizeof(jf.uid));
    get_var(TOKEN_VAR, jf.token, sizeof(jf.token));
    get_var(VSIZE_VAR, v, sizeof(v));
    jf.vsize = !amb_stricmp(v, "small") ? 1 : !amb_stricmp(v, "large") ? 2 : 0;
    get_var(DEV_VAR, jf.dev, sizeof(jf.dev));
    if (!jf.dev[0]) {
        /* who this Ambiance is, to the server: made up once, then kept (the
         * server allows one token a device: a new id each run would leave a
         * session behind each time) */
        ULONG a = time_ms() ^ (ULONG)FindTask(NULL), b = (ULONG)AvailMem(MEMF_ANY) * 2654435761UL;
        sprintf(jf.dev, "ambiance-%08lx%08lx", (unsigned long)a, (unsigned long)b);
        set_var(DEV_VAR, jf.dev);
    }
}

void jf_set_on(BOOL on)
{
    jf.on = on;
    set_var(ON_VAR, on ? "on" : "off");
}

void jf_set_server(const char *address, const char *name, const char *sid)
{
    char v[96];
    BOOL other;
    take_server(address);
    sprintf(v, "%s:%lu", jf.host, (unsigned long)jf.port);
    other = strcmp(sid ? sid : "", jf.sid) || !sid || !sid[0];
    set_var(SERVER_VAR, jf.host[0] ? v : "");
    amb_strlcpy(jf.name, name && name[0] ? name : jf.host, sizeof(jf.name));
    set_var(NAME_VAR, jf.host[0] ? jf.name : "");
    amb_strlcpy(jf.sid, sid ? sid : "", sizeof(jf.sid));
    set_var(ID_VAR, jf.sid);
    if (other && jf.token[0])
        jf_signed_in("", "", "");           /* another server: its own sign-in */
}

void jf_signed_in(const char *user, const char *uid, const char *token)
{
    amb_strlcpy(jf.user, user, sizeof(jf.user));
    amb_strlcpy(jf.uid, uid, sizeof(jf.uid));
    amb_strlcpy(jf.token, token, sizeof(jf.token));
    set_var(USER_VAR, jf.user);
    set_var(UID_VAR, jf.uid);
    set_var(TOKEN_VAR, jf.token);
}

void jf_set_vsize(int v)
{
    jf.vsize = (UBYTE)v;
    set_var(VSIZE_VAR, v == 1 ? "small" : v == 2 ? "large" : "auto");
}

BOOL jf_ready(void)
{
    return jf.on && jf.host[0] && jf.token[0] && jf.uid[0];
}

/* ---------------------------------------------------------------- asking */

/* Who asks, as Jellyfin wants it on every request ("Name: value\r\n"). */
void jf_headers(char *out, LONG max)
{
    char model[40];
    sprintf(model, "Amiga %s", jf_cpu_name());
    sprintf(out, "Authorization: MediaBrowser Client=\"Ambiance\", Device=\"%s\", DeviceId=\"%s\", "
                 "Version=\"" AMB_VERSION "\"%s%s%s\r\n",
            model, jf.dev, jf.token[0] ? ", Token=\"" : "", jf.token, jf.token[0] ? "\"" : "");
    if ((LONG)strlen(out) >= max)
        out[0] = 0;
}

/* The CPU, by the name Jellyfin's dashboard shows for this device. */
const char *jf_cpu_name(void)
{
    UWORD af = SysBase->AttnFlags;
    return (af & AFF_68060) ? "68060" : (af & AFF_68040) ? "68040" : (af & AFF_68030) ? "68030"
           : (af & AFF_68020) ? "68020" : "68000";
}

/* The items lists' common tail: one picture tag each (the primary), no
 * per-user data, the total, a part of PART from start. */
static void tail(char *out, LONG max, LONG start)
{
    char n[200];
    sprintf(n, "&enableImageTypes=Primary&imageTypeLimit=1&enableUserData=false"
               "&enableTotalRecordCount=true&startIndex=%ld&limit=%ld",
            (long)start, (long)PART);
    amb_strlcat(out, n, max);
}

/* The request for key (from entry start on) into out: path and query. */
void jf_request(const char *key, LONG start, char *out, LONG max)
{
    const char *id = key + 2;
    char q[300];
    out[0] = 0;
    if (key[0] != '~' || !key[1])
        return;
    switch (key[1]) {
    case 'V':
        sprintf(out, "/UserViews?userId=%s", jf.uid);
        return;
    case 'A':
        sprintf(out, "/Artists/AlbumArtists?userId=%s&parentId=%.40s&sortBy=SortName", jf.uid, id);
        break;
    case 'L':
        sprintf(out, "/Items?userId=%s&parentId=%.40s&includeItemTypes=MusicAlbum&recursive=true"
                     "&sortBy=SortName&fields=ChildCount", jf.uid, id);
        break;
    case 'R':
        sprintf(out, "/Items?userId=%s&parentId=%.40s&includeItemTypes=MusicAlbum&recursive=true"
                     "&sortBy=DateCreated&sortOrder=Descending&fields=ChildCount"
                     "&enableImageTypes=Primary&imageTypeLimit=1&enableUserData=false"
                     "&startIndex=0&limit=100", jf.uid, id);
        return;                         /* (the newest hundred: no more parts) */
    case 'a':
        sprintf(out, "/Items?userId=%s&albumArtistIds=%.40s&includeItemTypes=MusicAlbum"
                     "&recursive=true&sortBy=ProductionYear,SortName&fields=ChildCount", jf.uid, id);
        break;
    case 't':
        sprintf(out, "/Items?userId=%s&parentId=%.40s&sortBy=ParentIndexNumber,IndexNumber,SortName",
                jf.uid, id);
        break;
    case 'F':
        /* a folder: folders first, then by name; Pictures without its
         * videos, Videos without its photos */
        sprintf(q, "/Items?userId=%s&parentId=%.40s&sortBy=IsFolder,SortName"
                   "&sortOrder=Descending,Ascending&fields=ChildCount%s", jf.uid, key + 3,
                key[2] == 'p' ? ",Width,Height&excludeItemTypes=Video,Movie,Episode,Audio"
                              : ",Overview,OfficialRating&excludeItemTypes=Photo,Audio");
        amb_strlcpy(out, q, max);
        break;
    default:
        return;
    }
    tail(out, max, start);
}

/* A job for key: who to ask, what, and who is asking. */
void jf_job(struct NetJob *j, const char *key, LONG start)
{
    amb_strlcpy(j->host, jf.host, sizeof(j->host));
    j->port = jf.port;
    jf_request(key, start, j->path, sizeof(j->path));
    jf_headers(j->extra, sizeof(j->extra));
}

/* A music library's menu, made here (key "~M<id>"). */
void jf_menu(const char *key, struct ItemList *l)
{
    static const struct { const char *label; char c; } rows[] = {
        { N_("Artists"), 'A' }, { N_("Albums"), 'L' }, { N_("Recently added"), 'R' },
    };
    char k[64];
    int i;
    for (i = 0; i < 3; i++) {
        struct Item *it;
        k[0] = '~';
        k[1] = rows[i].c;
        amb_strlcpy(k + 2, key + 2, 41);
        if ((it = list_add(l, _(rows[i].label), k, "", IT_PLEXDIR)))
            it->mark = MARK_ARROW;
    }
}

BOOL jf_is_menu(const char *key)
{
    return key[0] == '~' && key[1] == 'M';
}

/* ---------------------------------------------------------------- reading */

struct fill {
    struct ItemList *l;
    const char *key;
    char view;                  /* ~V's: m, v, p */
    BOOL all_albums;            /* a library's albums: "Album - Artist" */
    LONG added, skipped, music_libs;
    char one_lib[40];           /* the one music library (Music: its menu at once) */
    BOOL full;
};

static void text(const char *in, char *out, LONG max)
{
    amb_strlcpy(out, in ? in : "", max);
    utf8_to_latin1(out);
    squeeze_spaces(out);
}

/* RunTimeTicks (100 ns) as ms. */
static ULONG ms_of(const char *ticks)
{
    ULONG v = 0;
    int n = (int)strlen(ticks) - 4;         /* (ticks / 10000: its last four digits off) */
    const char *t;
    if (n <= 0)
        return 0;
    for (t = ticks; t < ticks + n && *t >= '0' && *t <= '9'; t++)
        v = v * 10 + (ULONG)(*t - '0');
    return v;
}

static void dur_text(ULONG ms, char *out)
{
    ULONG s = (ms + 500) / 1000;
    if (s >= 3600)
        sprintf(out, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                (unsigned long)(s % 60));
    else
        sprintf(out, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static BOOL is(const char *a, const char *b)
{
    return !strcmp(a, b);
}

static void add(struct fill *x, const char *label, const char *path, const char *tag, UBYTE kind,
                UBYTE mark)
{
    struct Item *it = list_add(x->l, label[0] ? label : _("(no name)"), path, tag, kind);
    if (!it) {
        x->full = TRUE;
        return;
    }
    it->mark = mark;
    x->added++;
}

static int item(void *ctx, const struct JfItem *it)
{
    struct fill *x = ctx;
    static char path[1100];
    char name[160], by[120], album[120], thumb[80], tag[32], count[32], year[8];
    const char *t = it->type;
    ULONG n;
    if (x->full)
        return 0;
    text(it->name, name, sizeof(name));
    year[0] = 0;
    amb_strlcpy(year, it->year, sizeof(year));
    thumb[0] = 0;
    if (it->image[0])
        sprintf(thumb, "~I%.36s/%.36s", it->id, it->image);
    n = amb_num(it->children);
    if (x->view) {
        /* the libraries, those for the section asking */
        const char *c = it->coll;
        if (x->view == 'm' ? !is(c, "music")
            : x->view == 'p' ? !(is(c, "homevideos") || is(c, "photos"))
                             : !(is(c, "movies") || is(c, "tvshows") || is(c, "homevideos") ||
                                 is(c, "musicvideos") || is(c, "boxsets") || !c[0])) {
            x->skipped++;
            return 1;
        }
        if (x->view == 'm') {
            x->music_libs++;
            amb_strlcpy(x->one_lib, it->id, sizeof(x->one_lib));
            sprintf(path, "~M%.40s", it->id);
        } else {
            sprintf(path, "~F%s%.40s\t%s\t%s\t\t\t\t\t", x->view == 'p' ? "p" : "v", it->id,
                    x->view == 'p' ? "photo" : "", thumb);
        }
        add(x, name, path, "", IT_PLEXDIR, MARK_ARROW);
        return 1;
    }
    if (is(t, "Audio")) {
        /* a track: Plex's fields (plex.c's flush_track) */
        ULONG ms = ms_of(it->ticks);
        char codec[16], alb_rk[48], art_rk[48];
        text(it->artist[0] ? it->artist : it->album_artist, by, sizeof(by));
        text(it->album, album, sizeof(album));
        amb_strlcpy(codec, it->container, sizeof(codec));
        if (it->album_image[0] && it->album_id[0])
            sprintf(thumb, "~I%.36s/%.36s", it->album_id, it->album_image);
        alb_rk[0] = art_rk[0] = 0;
        if (it->album_id[0])
            sprintf(alb_rk, "~t%.40s", it->album_id);
        if (it->artist_id[0])
            sprintf(art_rk, "~a%.40s", it->artist_id);
        sprintf(path, "~S%.36s\t%s\t%lu\t%s\t%s\t~S%.36s\t0\t0\t0\t%s\t0\t%s\t%s", it->id, codec,
                (unsigned long)ms, by, album, it->id, thumb, alb_rk, art_rk);
        dur_text(ms, tag);
        add(x, name, path, tag, IT_PLEXTRACK, MARK_NONE);
        return 1;
    }
    if (is(t, "Photo")) {
        char dims[32], date[16];
        if (!thumb[0]) {
            x->skipped++;
            return 1;
        }
        dims[0] = 0;
        if (amb_num(it->width) && amb_num(it->height))
            sprintf(dims, "%s x %s", it->width, it->height);
        amb_strlcpy(date, it->date, 11);    /* (yyyy-mm-dd of its ISO time) */
        sprintf(path, "%s\tphoto\t%s\t\t%s\t%s\t\t", thumb, thumb, dims, date);
        add(x, name, path, date, IT_PLEXPHOTO, MARK_NONE);
        return 1;
    }
    if (is(t, "Movie") || is(t, "Episode") || is(t, "Video") || is(t, "MusicVideo")) {
        /* a film: Videos plays it (jf_video_url); its details after tabs */
        ULONG ms = ms_of(it->ticks);
        char ov[300];
        dur_text(ms, tag);
        text(it->overview, ov, sizeof(ov));
        if (is(t, "Episode") && it->index[0]) {
            char e[200];
            sprintf(e, "%s. %s", it->index, name);
            amb_strlcpy(name, e, sizeof(name));
        }
        sprintf(path, "~Y%.36s\t%s\t%s\t%s\t%lu\t%s\t%s", it->id, t, thumb, year, (unsigned long)ms,
                it->rating, ov);
        add(x, name, path, ms ? tag : year, IT_ANIM, MARK_NONE);
        return 1;
    }
    if (!it->folder) {
        x->skipped++;                       /* (a book, a playlist entry...) */
        return 1;
    }
    /* a folder: Plex's folder fields (key, type, thumb, year, by, count,
     * studio, summary) */
    count[0] = tag[0] = 0;
    by[0] = 0;
    if (is(t, "MusicAlbum")) {
        text(it->album_artist, by, sizeof(by));
        if (n)
            sprintf(count, n == 1 ? _("%lu track") : _("%lu tracks"), (unsigned long)n);
        if (x->all_albums && by[0]) {
            amb_strlcat(name, " - ", sizeof(name));
            amb_strlcat(name, by, sizeof(name));
        }
        sprintf(path, "~t%.40s\talbum\t%s\t%s\t%s\t%s\t\t", it->id, thumb, year, by, count);
        add(x, name, path, year, IT_PLEXDIR, MARK_ARROW);
        return 1;
    }
    if (is(t, "MusicArtist")) {
        sprintf(path, "~a%.40s\tartist\t%s\t\t\t\t\t", it->id, thumb);
        add(x, name, path, "", IT_PLEXDIR, MARK_ARROW);
        return 1;
    }
    {
        /* a folder of a library: a photo album, a series, a season */
        char kind = x->key[1] == 'F' ? x->key[2] : 'v';
        if (n)
            sprintf(count, n == 1 ? _("%lu item") : _("%lu items"), (unsigned long)n);
        sprintf(path, "~F%s%.40s\t%s\t%s\t%s\t\t%s\t\t", kind == 'p' ? "p" : "v", it->id,
                kind == 'p' ? "photo" : "", thumb, year, count);
        add(x, name, path, count, IT_PLEXDIR, MARK_ARROW);
    }
    return 1;
}

/* The answer to the request for key (body, len bytes and a byte spare,
 * changed) into l: the entries added, -1 if it was not Jellyfin's JSON.
 * *total: how many the server has. */
LONG jf_fill(const char *key, UBYTE *body, ULONG len, struct ItemList *l, LONG *total)
{
    struct fill x;
    long t = 0, n;
    memset(&x, 0, sizeof(x));
    x.l = l;
    x.key = key;
    x.view = key[1] == 'V' ? key[2] : 0;
    x.all_albums = key[1] == 'L' || key[1] == 'R';
    n = jf_items((char *)body, len, item, &x, &t);
    if (n < 0)
        return -1;
    if (x.view == 'm' && x.music_libs == 1 && l->count == 1) {
        /* one music library: Music shows its menu at once */
        char k[48];
        sprintf(k, "~M%.40s", x.one_lib);
        list_free(l);
        list_init(l);
        jf_menu(k, l);
        x.added = l->count;
        t = l->count;
    }
    if (total)
        *total = key[1] == 'V' ? x.added : t;
    return x.added;
}

/* ---------------------------------------------------------------- playing */

/* A new play session's name: 8 hex digits. Jellyfin names a conversion's
 * file from the media, the client's device id and its play session only
 * (StreamingHelpers.GetOutputFilePath), not from where it starts: asked
 * again from another time under the same name, it hands over the
 * conversion already running, from its start. So each start and each seek
 * is a session of its own. */
static void jf_new_session(char *s)
{
    static ULONG n;
    sprintf(s, "%08lx", (unsigned long)(time_ms() ^ (++n * 2654435761UL) ^ (ULONG)FindTask(NULL)));
}

/* A track (its fields, plex.c's) as a URL to play: MP3 from the server,
 * made for this Amiga as Plex's are (plex_tier), or (an MP3, played as
 * stored where the Amiga is fast enough) the file itself. */
void jf_track_url(const struct Item *it, BOOL convert, int tier, ULONG from_ms, char *out, LONG max)
{
    char key[64], n[260];
    radio_field(it, 0, key, sizeof(key));
    sprintf(out, "http://%s:%lu/Audio/%.36s/stream%s", jf.host, (unsigned long)jf.port, key + 2,
            convert ? ".mp3" : "");
    if (convert) {
        BOOL mono = tier == 0;
        sprintf(n, "?static=false&audioCodec=mp3&audioBitRate=%ld&audioSampleRate=%ld"
                   "&maxAudioChannels=%d&audioChannels=%d",
                (long)(tier == 0 ? 64000 : tier == 1 ? 112000 : tier == 2 ? 160000 : 256000),
                (long)(mono ? 22050 : tier >= 3 ? 48000 : 24000), mono ? 1 : 2, mono ? 1 : 2);
        amb_strlcat(out, n, max);
        {
            char ses[12];
            jf_new_session(ses);
            sprintf(n, "&playSessionId=%s", ses);
            amb_strlcat(out, n, max);
        }
        if (from_ms) {
            sprintf(n, "&startTimeTicks=%lu0000", (unsigned long)from_ms);
            amb_strlcat(out, n, max);
        }
    } else {
        amb_strlcat(out, "?static=true", max);
    }
    sprintf(n, "&deviceId=%s&ApiKey=%s", jf.dev, jf.token);
    amb_strlcat(out, n, max);
}

/* A film as a URL: MPEG-1 for this Amiga's CPU (QVGA), or MPEG-2 for the
 * ZZ9000 (zz: it decodes): its picture size and rate by Settings >
 * Networking > Jellyfin > Films (auto: by the decoder). from_ms: later in
 * it (a new conversion from there: a live one cannot be sought in). */
void jf_video_url(const char *key, BOOL zz, ULONG from_ms, char *out, LONG max)
{
    char n[300];
    int big = jf.vsize == 2 || (jf.vsize == 0 && zz);
    sprintf(out, "http://%s:%lu/Videos/%.36s/stream.mpg?static=false", jf.host,
            (unsigned long)jf.port, key + 2);
    if (big)
        /* the card's decoder: MPEG-2 at DVD's size, its own rate */
        sprintf(n, "&videoCodec=mpeg2video&maxWidth=720&maxHeight=576&videoBitRate=4000000");
    else
        /* the CPU's: MPEG-1, a quarter of VGA (a 16:9 film 320 x 180) */
        sprintf(n, "&videoCodec=mpeg1video&maxWidth=320&maxHeight=240&videoBitRate=1000000");
    amb_strlcat(out, n, max);
    /* MP2 at 48 kHz (decoded here at half that, 24 kHz: Paula's), 96 or 128
     * kbit/s. (Jellyfin 12.x sends mp2 at 128k a channel whatever is asked:
     * its GetAudioBitrateParam's default ignores audioBitRate; a fix is
     * offered upstream.) */
    sprintf(n, "&audioCodec=mp2&audioBitRate=%s&audioSampleRate=48000&audioChannels=2"
               "&deviceId=%s&ApiKey=%s", big ? "128000" : "96000", jf.dev, jf.token);
    amb_strlcat(out, n, max);
    if (from_ms) {
        sprintf(n, "&startTimeTicks=%lu0000", (unsigned long)from_ms);
        amb_strlcat(out, n, max);
    }
}

/* A film's start and seeks, for the player to add to jf_video_url's URL
 * (anim.c: each '*' the time in ms): a session of their own each (see
 * jf_new_session), and the time in 100 ns units. */
void jf_video_seek(char *out, LONG max)
{
    char ses[12], n[80];
    jf_new_session(ses);
    sprintf(n, "&playSessionId=%s-*&startTimeTicks=*0000", ses);
    amb_strlcpy(out, n, max);
}

/* A picture ("~I<id>/<tag>") made w x h by the server: the request into
 * req (path and query: no token needed for pictures). */
void jf_art_request(const char *thumb, WORD w, WORD h, char *req, LONG max)
{
    char id[40];
    const char *slash = strchr(thumb, '/');
    LONG n = slash ? slash - thumb - 2 : (LONG)strlen(thumb) - 2;
    if (n < 0)
        n = 0;
    if (n >= (LONG)sizeof(id))
        n = sizeof(id) - 1;
    memcpy(id, thumb + 2, n);
    id[n] = 0;
    sprintf(req, "/Items/%s/Images/Primary?maxWidth=%ld&maxHeight=%ld&quality=85&format=Jpg%s%.36s",
            id, (long)w, (long)h, slash ? "&tag=" : "", slash ? slash + 1 : "");
    (void)max;
}

/* ---------------------------------------------------------------- sign in */

/* A string for JSON: quoted, its Latin-1 as UTF-8, quotes and controls
 * escaped. */
static void json_str(const char *s, char *out, LONG max)
{
    LONG n = 0;
    out[n++] = '"';
    for (; *s && n < max - 8; s++) {
        UBYTE c = (UBYTE)*s;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            sprintf(out + n, "\\u%04lx", (unsigned long)c);
            n += 6;
        } else if (c >= 0x80) {
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)c;
        }
    }
    out[n++] = '"';
    out[n] = 0;
}

/* The sign-in's job: POST /Users/AuthenticateByName, its body in body
 * (kept by the caller while the job is out). */
void jf_signin_job(struct NetJob *j, const char *user, const char *password, char *body, LONG max)
{
    char u[200], p[200];
    json_str(user, u, sizeof(u));
    json_str(password, p, sizeof(p));
    sprintf(body, "{\"Username\":%s,\"Pw\":%s}", u, p);
    memset(p, 0, sizeof(p));            /* (the password not left about) */
    (void)max;
    amb_strlcpy(j->host, jf.host, sizeof(j->host));
    j->port = jf.port;
    strcpy(j->path, "/Users/AuthenticateByName");
    jf_headers(j->extra, sizeof(j->extra));
    j->post = 1;
    j->send = body;
}

/* Its answer: the token and user kept (TRUE), or why not in why. */
BOOL jf_signin_came(struct NetJob *j, const char *user, char *why, LONG max)
{
    char token[80], uid[40];
    why[0] = 0;
    if (!j->body || j->status != 200) {
        if (j->status == 401 || j->status == 400)
            amb_strlcpy(why, _("The server did not accept that name and password"), max);
        else if (j->status)
            sprintf(why, _("The server said %ld"), (long)j->status);
        else
            amb_strlcpy(why, j->err[0] ? j->err : _("Could not reach the server"), max);
        return FALSE;
    }
    if (!jf_value((const char *)j->body, j->len, "AccessToken", token, sizeof(token)) ||
        !jf_value((const char *)j->body, j->len, "User.Id", uid, sizeof(uid)) || !token[0]) {
        amb_strlcpy(why, _("The server answered with something unexpected"), max);
        return FALSE;
    }
    jf_signed_in(user, uid, token);
    return TRUE;
}

/* Signing out: the server forgets the token (POST /Sessions/Logout, with
 * it), and so does Ambiance. */
void jf_signout_job(struct NetJob *j)
{
    amb_strlcpy(j->host, jf.host, sizeof(j->host));
    j->port = jf.port;
    strcpy(j->path, "/Sessions/Logout");
    jf_headers(j->extra, sizeof(j->extra));
    j->post = 1;
}
