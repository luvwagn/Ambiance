/*
 * AmigaMCE - Radio: stations from radio-browser.info
 * Copyright (c) 2026 Brian King
 *
 * radio-browser.info is a free, community-kept directory of internet radio
 * stations with an open API; every station there has its codec, bitrate,
 * country, tags and whether it passed the last check. AmigaMCE asks it for
 * lists (net.c does the asking) in CSV and keeps the stations an Amiga can
 * play: MP3 (mpega.library's format), working, not HLS, and plain http:
 * unless Settings allows HTTPS (that needs AmiSSL).
 *
 * The lists are pages of items: IT_NET rows open another list (a query,
 * in path), IT_STATION rows are stations. A station's item carries what
 * its page shows: label its name, sortkey the tag shown at the right
 * ("128k"), path the stream's URL and, after tabs, its country code, tags,
 * homepage, votes, codec, bitrate (radio_field picks one out).
 */
#include "amce.h"
#include "radiocsv.h"

#include <string.h>
#include <stdio.h>

#define STATIONS_MAX 200

/* Settings > Networking > Radio: the highest bitrate listed (kbit/s; 0 any),
 * and this Amiga's country (ISO 3166-1, "" not known), first in Countries. */
static UWORD max_kbps;
static char home_cc[4];

void radio_set_max_kbps(UWORD kbps)
{
    max_kbps = kbps;
}

void radio_set_home(const char *iso)
{
    amce_strlcpy(home_cc, iso ? iso : "", sizeof(home_cc));
}

/* The request for a query (an IT_NET item's path) into out. */
void radio_request(const char *query, BOOL https, char *out, LONG max)
{
    if (!strcmp(query, RADIO_TAGS)) {
        amce_strlcpy(out, "/csv/tags?order=stationcount&reverse=true&hidebroken=true&limit=150",
                     max);
    } else if (!strcmp(query, RADIO_COUNTRIES)) {
        amce_strlcpy(out, "/csv/countries?order=stationcount&reverse=true&hidebroken=true",
                     max);
    } else {
        char n[32];
        sprintf(n, "%ld", (long)STATIONS_MAX);
        amce_strlcpy(out, "/csv/stations/search?codec=MP3&hidebroken=true&limit=", max);
        amce_strlcat(out, n, max);
        if (!https)
            amce_strlcat(out, "&is_https=false", max);
        if (max_kbps) {
            /* asked of the server, so the list is the 200 best that fit */
            sprintf(n, "&bitrateMax=%lu", (unsigned long)max_kbps);
            amce_strlcat(out, n, max);
        }
        if (query[0]) {
            amce_strlcat(out, "&", max);
            amce_strlcat(out, query, max);
        }
    }
}

struct fill {
    struct ItemList *l;
    BOOL https, header;
    LONG skipped;
    int c_name, c_url, c_urlr, c_cc, c_tags, c_home, c_votes, c_codec, c_bitrate, c_hls;
    int c_count, c_iso;
    BOOL tags, countries;
};

static const char *col(char **f, int n, int c)
{
    return c >= 0 && c < n ? f[c] : "";
}

static LONG num(const char *s)
{
    LONG v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static BOOL is_scheme(const char *u, const char *s)
{
    return !amce_strnicmp(u, s, strlen(s));
}

static int station_row(struct fill *x, char **f, int n)
{
    const char *url = col(f, n, x->c_urlr), *codec = col(f, n, x->c_codec);
    char name[96], tags[120], home[160], path[600], tag[16];
    LONG i, br = num(col(f, n, x->c_bitrate));
    if (!url[0])
        url = col(f, n, x->c_url);
    if (num(col(f, n, x->c_hls)) || amce_stricmp(codec, "MP3") || (max_kbps && br > max_kbps) ||
        !(is_scheme(url, "http://") || (x->https && is_scheme(url, "https://")))) {
        x->skipped++;
        return 1;
    }
    for (i = 0; i < x->l->count; i++)       /* the same stream listed twice: once */
        if (!strncmp(x->l->items[i].path, url, strlen(url)) &&
            x->l->items[i].path[strlen(url)] == '\t') {
            x->skipped++;
            return 1;
        }
    amce_strlcpy(name, col(f, n, x->c_name), sizeof(name));
    utf8_to_latin1(name);
    squeeze_spaces(name);
    if (!name[0])
        amce_strlcpy(name, _("(no name)"), sizeof(name));
    amce_strlcpy(tags, col(f, n, x->c_tags), sizeof(tags));
    utf8_to_latin1(tags);
    squeeze_spaces(tags);
    amce_strlcpy(home, col(f, n, x->c_home), sizeof(home));
    squeeze_spaces(home);
    sprintf(path, "%s\t%s\t%s\t%s\t%ld\t%s\t%ld", url, col(f, n, x->c_cc), tags, home,
            (long)num(col(f, n, x->c_votes)), codec, (long)br);
    if (br > 0)
        sprintf(tag, "%ldk", (long)br);
    else
        amce_strlcpy(tag, codec, sizeof(tag));
    if (is_scheme(url, "https://"))
        amce_strlcat(tag, " S", sizeof(tag));     /* a secure stream (AmiSSL) */
    if (!list_add(x->l, name, path, tag, IT_STATION))
        return 0;
    return x->l->count < STATIONS_MAX;
}

static int folder_row(struct fill *x, char **f, int n)
{
    char name[64], q[240], enc[160], count[16];
    LONG c = num(col(f, n, x->c_count));
    struct Item *it;
    amce_strlcpy(name, col(f, n, x->c_name), sizeof(name));
    utf8_to_latin1(name);
    squeeze_spaces(name);
    if (!name[0] || c < 3)
        return 1;                           /* nameless, or next to nothing in it */
    if (x->countries && col(f, n, x->c_iso)[0]) {
        url_encode(col(f, n, x->c_iso), enc, sizeof(enc));
        sprintf(q, "countrycode=%s&order=votes&reverse=true", enc);
    } else {
        url_encode(col(f, n, x->c_name), enc, sizeof(enc));
        sprintf(q, "%s=%s&%s=true&order=votes&reverse=true", x->tags ? "tag" : "country", enc,
                x->tags ? "tagExact" : "countryExact");
    }
    sprintf(count, "%ld", (long)c);
    if (!(it = list_add(x->l, name, q, count, IT_NET)))
        return 0;
    it->mark = MARK_ARROW;
    return 1;
}

static int row(void *ctx, char **f, int n)
{
    struct fill *x = ctx;
    if (x->header) {
        x->header = FALSE;
        x->c_name = csv_col(f, n, "name");
        x->c_url = csv_col(f, n, "url");
        x->c_urlr = csv_col(f, n, "url_resolved");
        x->c_cc = csv_col(f, n, "countrycode");
        x->c_tags = csv_col(f, n, "tags");
        x->c_home = csv_col(f, n, "homepage");
        x->c_votes = csv_col(f, n, "votes");
        x->c_codec = csv_col(f, n, "codec");
        x->c_bitrate = csv_col(f, n, "bitrate");
        x->c_hls = csv_col(f, n, "hls");
        x->c_count = csv_col(f, n, "stationcount");
        x->c_iso = csv_col(f, n, "iso_3166_1");
        return x->c_name >= 0;              /* not the CSV expected: stop */
    }
    if (x->tags || x->countries)
        return folder_row(x, f, n);
    return station_row(x, f, n);
}

/* The answer to query (body, len bytes and a byte spare, changed) into l:
 * the rows found, -1 if it was not the CSV expected. *skipped: stations
 * left out (another codec, HLS, https:, a duplicate). */
LONG radio_fill(const char *query, UBYTE *body, ULONG len, struct ItemList *l, BOOL https,
                LONG *skipped)
{
    struct fill x;
    memset(&x, 0, sizeof(x));
    x.l = l;
    x.https = https;
    x.header = TRUE;
    x.tags = !strcmp(query, RADIO_TAGS);
    x.countries = !strcmp(query, RADIO_COUNTRIES);
    csv_parse((char *)body, len, row, &x);
    if (skipped)
        *skipped = x.skipped;
    if (x.header || x.c_name < 0)
        return -1;
    if (x.countries && l->count > 1) {
        /* A to Z (the server's order is by size), this Amiga's own first */
        char want[24];
        LONG i, k, home = -1;
        struct Item t;
        for (i = 1; i < l->count; i++) {
            t = l->items[i];
            for (k = i; k > 0 && amce_stricmp(l->items[k - 1].label, t.label) > 0; k--)
                l->items[k] = l->items[k - 1];
            l->items[k] = t;
        }
        if (home_cc[0]) {
            sprintf(want, "countrycode=%s&", home_cc);
            for (i = 0; i < l->count && home < 0; i++)
                if (!amce_strnicmp(l->items[i].path, want, strlen(want)))
                    home = i;
        }
        if (home > 0) {
            t = l->items[home];
            for (k = home; k > 0; k--)
                l->items[k] = l->items[k - 1];
            l->items[0] = t;
        }
    }
    return l->count;
}

/* Field i of a station item's path (0 its URL, 1 country, 2 tags,
 * 3 homepage, 4 votes, 5 codec, 6 bitrate) into out. */
char *radio_field(const struct Item *it, int i, char *out, LONG max)
{
    const char *p = it->path, *e;
    while (i-- > 0 && p) {
        p = strchr(p, '\t');
        if (p)
            p++;
    }
    out[0] = 0;
    if (!p)
        return out;
    e = strchr(p, '\t');
    if (!e)
        e = p + strlen(p);
    if (e - p < max) {
        memcpy(out, p, e - p);
        out[e - p] = 0;
    } else {
        amce_strlcpy(out, p, max);
    }
    return out;
}
