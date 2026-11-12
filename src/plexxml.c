/*
 * AmigaMCE - a Plex server's answers: its XML, read just enough
 * Copyright (c) 2026 Brian King
 *
 * A Plex Media Server answers in XML: a MediaContainer of Directory,
 * Track, Photo... elements whose attributes say all a list needs (title,
 * key, year, duration), with Media and Part elements inside a Track for
 * its file. Nothing here needs a real XML parser: each start tag is found,
 * its name and attributes taken out (in place, entities decoded), and
 * handed on with its depth. Text between tags and end tags are only
 * counted for the depth. Plain C and nothing of the Amiga's, so the host
 * tests build it (tests/plex_host.c).
 */
#include "plexxml.h"

#include <string.h>

/* One UTF-8 character for code point c at o; its length. */
static int put_utf8(char *o, unsigned long c)
{
    if (c < 0x80) {
        o[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6));
        o[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    o[0] = (char)(0xE0 | ((c >> 12) & 0x0F));
    o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    o[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

/* Entities decoded in s, in place (it only gets shorter: "&#xFFFF;" is 8
 * bytes, its UTF-8 3). */
void xml_unescape(char *s)
{
    char *p = s, *o = s;
    while (*p) {
        if (*p != '&') {
            *o++ = *p++;
            continue;
        }
        if (!strncmp(p, "&amp;", 5)) {
            *o++ = '&';
            p += 5;
        } else if (!strncmp(p, "&lt;", 4)) {
            *o++ = '<';
            p += 4;
        } else if (!strncmp(p, "&gt;", 4)) {
            *o++ = '>';
            p += 4;
        } else if (!strncmp(p, "&quot;", 6)) {
            *o++ = '"';
            p += 6;
        } else if (!strncmp(p, "&apos;", 6)) {
            *o++ = '\'';
            p += 6;
        } else if (p[1] == '#') {
            unsigned long c = 0;
            char *q = p + 2;
            int hex = *q == 'x' || *q == 'X';
            if (hex)
                q++;
            while ((*q >= '0' && *q <= '9') || (hex && ((*q >= 'a' && *q <= 'f') ||
                                                         (*q >= 'A' && *q <= 'F')))) {
                int d = *q >= 'a' ? *q - 'a' + 10 : *q >= 'A' ? *q - 'A' + 10 : *q - '0';
                c = c * (hex ? 16 : 10) + d;
                if (c > 0xFFFF)
                    c = '?';
                q++;
            }
            if (*q == ';' && q > p + 2) {
                o += put_utf8(o, c);
                p = q + 1;
            } else {
                *o++ = *p++;
            }
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
}

/* Every start tag of buf (len bytes and a byte spare; changed in place)
 * to el(ctx, name, attr, n, depth): attr holds n name/value pairs, names
 * at even places. el returns 0 to stop. Returns the elements seen. */
long xml_scan(char *buf, unsigned long len,
              int (*el)(void *ctx, const char *name, char **attr, int n, int depth), void *ctx)
{
    char *p = buf, *end = buf + len, *attr[XML_MAX_ATTRS * 2];
    int depth = 0;
    long seen = 0;
    *end = 0;
    while (p < end) {
        char *name;
        int n = 0, closed = 0;
        p = memchr(p, '<', end - p);
        if (!p)
            break;
        p++;
        if (*p == '?' || *p == '!') {       /* declaration, comment, doctype */
            char *q = *p == '!' && !strncmp(p, "!--", 3) ? strstr(p, "-->") : strchr(p, '>');
            if (!q)
                break;
            p = q + 1;
            continue;
        }
        if (*p == '/') {                    /* an end tag */
            if (depth > 0)
                depth--;
            p = strchr(p, '>');
            if (!p)
                break;
            p++;
            continue;
        }
        name = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '>' &&
               *p != '/')
            p++;
        for (;;) {
            char *an, *av, q;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                *p++ = 0;
            if (p >= end)
                break;
            if (*p == '/') {
                closed = 1;
                *p++ = 0;
                continue;
            }
            if (*p == '>') {
                *p++ = 0;
                break;
            }
            an = p;
            while (p < end && *p != '=' && *p != ' ' && *p != '>' && *p != '/')
                p++;
            if (p >= end || *p != '=') {    /* an attribute without a value: passed over */
                if (p < end && *p != '>' && *p != '/')
                    *p++ = 0;
                continue;
            }
            *p++ = 0;
            q = *p;
            if (q != '"' && q != '\'')
                continue;
            av = ++p;
            p = memchr(p, q, end - p);
            if (!p)
                return seen;
            *p++ = 0;
            xml_unescape(av);
            if (n < XML_MAX_ATTRS) {
                attr[n * 2] = an;
                attr[n * 2 + 1] = av;
                n++;
            }
        }
        seen++;
        if (!el(ctx, name, attr, n, depth))
            break;
        if (!closed)
            depth++;
    }
    return seen;
}

/* The value of attribute name, or NULL. */
const char *xml_attr(char **attr, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(attr[i * 2], name))
            return attr[i * 2 + 1];
    return 0;
}

/* ------------------------------------------------------------ plex.tv */
static void cpy(char *d, const char *s, long n)
{
    long i;
    for (i = 0; i < n - 1 && s[i]; i++)
        d[i] = s[i];
    if (n > 0)
        d[i] = 0;
}

static void cat(char *d, const char *s, long n)
{
    long k = (long)strlen(d);
    if (k < n)
        cpy(d + k, s, n - k);
}

int plex_tv_value(const unsigned char *body, const char *name, char *out, long max)
{
    char pat[48];
    const char *p, *e;
    long k = (long)strlen(name);
    out[0] = 0;
    if (!body || k > 40 || max < 1)
        return 0;
    pat[0] = ' ';
    memcpy(pat + 1, name, k);
    memcpy(pat + 1 + k, "=\"", 3);
    if ((p = strstr((const char *)body, pat))) {
        p += k + 3;                         /* XML: name="value" */
        e = strchr(p, '"');
    } else {
        pat[0] = '"';
        memcpy(pat + 1, name, k);
        memcpy(pat + 1 + k, "\":", 3);
        if (!(p = strstr((const char *)body, pat)))
            return 0;
        p += k + 3;                         /* JSON: "name": "value" or a number */
        while (*p == ' ')
            p++;
        if (*p == '"') {
            e = strchr(++p, '"');
        } else {
            for (e = p; *e && *e != ',' && *e != '}' && *e != ' ' && *e != '\r' && *e != '\n'; e++)
                ;
            if (e - p == 4 && !strncmp(p, "null", 4))
                return 0;
        }
    }
    if (!e || e - p >= max)
        return 0;
    memcpy(out, p, e - p);
    out[e - p] = 0;
    return out[0] != 0;
}

struct res {
    char *out;
    long max, n;
    int server, done;
    char name[64], token[64], addr[48], port[8];
};

static void res_flush(struct res *r)
{
    if (r->server && r->addr[0] && r->token[0]) {
        cat(r->out, r->addr, r->max);
        cat(r->out, "\t", r->max);
        cat(r->out, r->port[0] ? r->port : "32400", r->max);
        cat(r->out, "\t", r->max);
        cat(r->out, r->name, r->max);
        cat(r->out, "\t", r->max);
        cat(r->out, r->token, r->max);
        cat(r->out, "\n", r->max);
        r->n++;
    }
    r->server = 0;
    r->addr[0] = 0;
}

static int res_el(void *ctx, const char *name, char **a, int n, int depth)
{
    struct res *r = ctx;
    const char *v;
    (void)depth;
    if (!strcmp(name, "Device")) {
        res_flush(r);
        r->server = (v = xml_attr(a, n, "provides")) && strstr(v, "server");
        cpy(r->name, (v = xml_attr(a, n, "name")) ? v : "", sizeof(r->name));
        cpy(r->token, (v = xml_attr(a, n, "accessToken")) ? v : "", sizeof(r->token));
        r->done = 0;
    } else if (!strcmp(name, "Connection") && r->server && !r->done) {
        const char *loc = xml_attr(a, n, "local"), *pr = xml_attr(a, n, "protocol");
        const char *ad = xml_attr(a, n, "address"), *po = xml_attr(a, n, "port");
        /* a local IPv4 address (no relay, no IPv6) */
        if (loc && loc[0] == '1' && ad && strchr(ad, '.') && !strchr(ad, ':')) {
            cpy(r->addr, ad, sizeof(r->addr));
            cpy(r->port, po ? po : "32400", sizeof(r->port));
            r->done = pr && !strcmp(pr, "http");    /* plain HTTP: no better */
        }
    }
    return 1;
}

long plex_resource_lines(unsigned char *body, unsigned long len, char *out, long max)
{
    struct res r;
    memset(&r, 0, sizeof(r));
    r.out = out;
    r.max = max;
    out[0] = 0;
    xml_scan((char *)body, len, res_el, &r);
    res_flush(&r);
    return r.n;
}
