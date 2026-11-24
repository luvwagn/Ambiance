/*
 * Ambiance - the network process: HTTP requests off the UI
 * Copyright (c) 2026 Brian King
 *
 * Asking radio-browser.info for a list of stations takes a DNS lookup, a
 * connection and a few seconds of reading on a slow line; the UI must go
 * on taking keys meanwhile. So, as the loader does for the disk, a process
 * of its own does it: the UI sends a job (net_submit), the process does
 * the request and sends it back done, the UI collects it (net_collect)
 * when its signal comes.
 *
 * bsdsocket.library belongs to the process that opened it, so this
 * process opens it, and is the only one that uses SocketBase. HTTP/1.0,
 * Connection: close, chunked bodies understood all the same; a redirect is
 * followed. HTTPS (a job's tls, or a redirect to https:) only with
 * Settings > Networking > HTTPS on: by AmiSSL (tls.c), opened here, for
 * this process, the first time it is wanted. Every wait has a timeout, and
 * a job given up by the UI (cancel) stops at the next one.
 *
 * A Plex server is asked the same way (host and port given). Plex servers
 * are also looked for (a gdm job): GDM, Plex's own discovery, a UDP
 * "M-SEARCH * HTTP/1.0" to 239.0.0.250 (and the broadcast address) port
 * 32414; each server answers from its address with its Name and Port.
 *
 * The radio-browser.info API is found as its documentation asks: the
 * addresses of all.api.radio-browser.info, one chosen at random, its name
 * by reverse lookup (the Host the server answers to); a server that fails
 * gives way to the next.
 */
#include "amb.h"
#include "jfjson.h"
#include "radiocsv.h"
#include "plexxml.h"

#include <string.h>
#include <stdio.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/filio.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <libraries/bsdsocket.h>
#include <utility/tagitem.h>

struct Library *SocketBase;         /* the network process's alone */

#define API_ALL     "all.api.radio-browser.info"
#define CONNECT_S   10              /* seconds to connect */
#define READ_S      20              /* seconds without a byte before giving up */
#define BODY_MAX    (1536UL << 10)  /* a body larger is cut off */
#define MAX_HOSTS   8
#define REDIRECTS   3

static struct TlsLib tls;           /* AmiSSL, the process's own (tls.c) */
static volatile BOOL use_https;     /* Settings > Networking > HTTPS, set by the UI */

void net_use_https(BOOL on)
{
    use_https = on;
}

static struct MsgPort *port;        /* the process's */
static struct MsgPort *reply_port;  /* the UI's */
static struct Task *parent, *proc;
static BYTE start_sig = -1;
static BOOL running;
static LONG out;

/* The API servers found (the process's own). */
static struct {
    int n, next;
    ULONG addr[MAX_HOSTS];
    char name[MAX_HOSTS][64];
} api;

static void set_err(struct NetJob *j, const char *s)
{
    if (!j->err[0])
        amb_strlcpy(j->err, s, sizeof(j->err));
}

/* Settings > Networking > Radio > Station list server: the one asked
 * first ("" any, at random), set by the main task; the list is made again
 * when it changes (pref_seq). */
static char api_pref[64];
static volatile ULONG pref_seq;
static ULONG pref_seen;

void net_radio_server(const char *name)
{
    amb_strlcpy(api_pref, name ? name : "", sizeof(api_pref));
    pref_seq++;
}

static BOOL find_api(struct NetJob *j)
{
    struct hostent *h;
    int i, n = 0;
    ULONG seed = time_ms();
    if (api.n && pref_seen == pref_seq)
        return TRUE;
    pref_seen = pref_seq;
    api.n = 0;
    if (!(h = gethostbyname((STRPTR)API_ALL)) || h->h_length != 4) {
        set_err(j, _("radio-browser.info could not be found (DNS)"));
        return FALSE;
    }
    for (i = 0; h->h_addr_list[i] && n < MAX_HOSTS; i++)
        memcpy(&api.addr[n++], h->h_addr_list[i], 4);
    /* shuffled, as the API asks: the load spread over its servers */
    for (i = n - 1; i > 0; i--) {
        int k;
        ULONG t;
        seed = seed * 1103515245UL + 12345UL;
        k = (int)((seed >> 8) % (ULONG)(i + 1));
        t = api.addr[i];
        api.addr[i] = api.addr[k];
        api.addr[k] = t;
    }
    for (i = 0; i < n; i++) {
        struct hostent *r = gethostbyaddr((STRPTR)&api.addr[i], 4, AF_INET);
        const char *nm = r && r->h_name ? (const char *)r->h_name : "";
        LONG k = strlen(nm);
        /* a server's own name (de1.api.radio-browser.info); a hosting
         * provider's name would not be one it answers to */
        if (k < 24 || amb_stricmp(nm + k - 23, ".api.radio-browser.info"))
            nm = API_ALL;
        amb_strlcpy(api.name[i], nm, sizeof(api.name[i]));
    }
    if (api_pref[0] && n) {
        /* the one chosen first, the rest after it should it fail; not among
         * them (a name gone from the list): looked up itself */
        int k;
        for (k = 0; k < n && amb_stricmp(api.name[k], api_pref); k++)
            ;
        if (k == n && (h = gethostbyname((STRPTR)api_pref)) && h->h_length == 4) {
            k = n < MAX_HOSTS ? n++ : n - 1;
            memcpy(&api.addr[k], h->h_addr_list[0], 4);
            amb_strlcpy(api.name[k], api_pref, sizeof(api.name[k]));
        }
        if (k < n && k > 0) {
            ULONG a = api.addr[k];
            char nm[64];
            amb_strlcpy(nm, api.name[k], sizeof(nm));
            memmove(&api.addr[1], &api.addr[0], k * sizeof(ULONG));
            memmove(api.name[1], api.name[0], k * sizeof(api.name[0]));
            api.addr[0] = a;
            amb_strlcpy(api.name[0], nm, sizeof(api.name[0]));
        }
    }
    api.n = n;
    api.next = 0;
    return n > 0;
}

/* Wait until s can be read (or written): 1, 0 timed out, -1 cancelled. */
static int wait_sock(struct NetJob *j, LONG s, BOOL wr, LONG secs)
{
    LONG left = secs * 4;
    while (left-- > 0) {
        fd_set fs;
        struct timeval tv;
        LONG r;
        if (j->cancel)
            return -1;
        FD_ZERO(&fs);
        FD_SET(s, &fs);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;                /* a quarter second: cancel heard soon */
        r = WaitSelect(s + 1, wr ? NULL : &fs, wr ? &fs : NULL, NULL, &tv, NULL);
        if (r > 0)
            return 1;
        if (r < 0)
            return 0;
    }
    return 0;
}

/* The address connections go out from (Settings > Networking): 0, the
 * stack's choice. Set by the main task, read here. */
static volatile ULONG bind_ip;

void net_bind_to(ULONG ip)
{
    bind_ip = ip;
}

/* A socket made to go out from the chosen interface's address. */
static void bind_out(LONG s)
{
    struct sockaddr_in la;
    ULONG ip = bind_ip;
    if (!ip)
        return;
    memset(&la, 0, sizeof(la));
    la.sin_len = sizeof(la);
    la.sin_family = AF_INET;
    memcpy(&la.sin_addr, &ip, 4);
    if (bind(s, (struct sockaddr *)&la, sizeof(la)) < 0)
        amb_log("network", "could not go out from the interface chosen: the stack's choice");
}

static LONG open_conn(struct NetJob *j, ULONG addr, UWORD port_no)
{
    struct sockaddr_in sa;
    LONG s, one = 1, e = 0, el = sizeof(e);
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        set_err(j, _("No socket (is the TCP/IP stack running?)"));
        return -1;
    }
    IoctlSocket(s, FIONBIO, (char *)&one);
    bind_out(s);
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = port_no;                  /* big-endian already: htons is nothing */
    memcpy(&sa.sin_addr, &addr, 4);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 && Errno() != EINPROGRESS) {
        CloseSocket(s);
        set_err(j, _("The connection was refused"));
        return -1;
    }
    if (wait_sock(j, s, TRUE, CONNECT_S) <= 0 ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
        CloseSocket(s);
        set_err(j, j->cancel ? _("Cancelled") : _("The server did not answer"));
        return -1;
    }
    return s;
}

/* TLS's handshake on s, waiting as it asks: the connection, NULL failed. */
static APTR tls_start(struct NetJob *j, LONG s, const char *host)
{
    APTR c;
    char m[200];
    if (!tls_open(&tls, SocketBase)) {
        set_err(j, _("HTTPS needs AmiSSL 5"));
        amb_log("network: https", tls.why);
        return NULL;
    }
    if (!(c = tls_new(&tls, s, host, TRUE))) {
        if (tls.why[0])
            amb_log("network: https", tls.why);
        set_err(j, tls.why[0] ? _("Ambiance's certificates are missing (its Certs drawer)")
                              : _("Not enough memory"));
        return NULL;
    }
    for (;;) {
        int r = tls_handshake(&tls, c);
        if (r == 1)
            return c;
        if (r == TLS_WANT_READ || r == TLS_WANT_WRITE) {
            int w = wait_sock(j, s, r == TLS_WANT_WRITE, READ_S);
            if (w > 0)
                continue;
            set_err(j, w < 0 ? _("Cancelled") : _("The server did not answer"));
        } else {
            sprintf(m, "%.60s: %.120s", host, tls.why);
            amb_log("network: https", m);
            set_err(j, strncmp(tls.why, "certificate", 11) ? _("The secure connection failed")
                     : _("The server's certificate was not accepted (is the Amiga's clock right?)"));
        }
        tls_free(&tls, c);
        return NULL;
    }
}

static BOOL send_all(struct NetJob *j, LONG s, APTR c, const char *buf, LONG n)
{
    while (n > 0) {
        LONG w;
        if (wait_sock(j, s, TRUE, READ_S) <= 0)
            return FALSE;
        if (c) {
            w = tls_write(&tls, c, buf, n);
            if (w == TLS_WANT_READ || w == TLS_WANT_WRITE)
                continue;
        } else {
            w = send(s, (APTR)buf, n, 0);
        }
        if (w <= 0)
            return FALSE;
        buf += w;
        n -= w;
    }
    return TRUE;
}

/* A download (j->file): once the headers are in b (hlen bytes up to and
 * with the blank line), a 200 that is not chunked has the rest of its
 * body written to the file, and then each read as it comes. TRUE: so
 * (the headers left in b); FALSE: not a download's answer (b as it was:
 * read on into memory, a redirect or an error). *err: the file failed. */
static BOOL to_file(struct NetJob *j, LONG s, APTR c, UBYTE *b, ULONG len, ULONG hlen, BOOL *err)
{
    char *p = (char *)b;
    BPTR fh;
    BOOL chunked = FALSE;
    LONG status = 0;
    *err = FALSE;
    if (len < 12 || amb_strnicmp(p, "HTTP/", 5) || !strchr(p, ' '))
        return FALSE;
    {
        char *sp = strchr(p, ' ');
        status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    }
    if (status != 200)
        return FALSE;
    {
        /* Content-Length (for progress), Transfer-Encoding */
        char save = b[hlen];
        char *q;
        b[hlen] = 0;
        for (q = p; q && *q; q = strstr(q, "\r\n") ? strstr(q, "\r\n") + 2 : NULL) {
            if (!amb_strnicmp(q, "Content-Length:", 15))
                j->size = (ULONG)amb_num(q + 15 + strspn(q + 15, " "));
            else if (!amb_strnicmp(q, "Transfer-Encoding:", 18) && strstr(q, "hunked"))
                chunked = TRUE;
        }
        b[hlen] = save;
    }
    if (chunked) {
        set_err(j, _("The server sent it in chunks, which downloads do not take"));
        *err = TRUE;
        return TRUE;
    }
    if (!(fh = Open((CONST_STRPTR)j->file, MODE_NEWFILE))) {
        set_err(j, _("The file could not be written"));
        *err = TRUE;
        return TRUE;
    }
    j->got = 0;
    if (len > hlen) {
        if (Write(fh, b + hlen, len - hlen) != (LONG)(len - hlen))
            *err = TRUE;
        j->got = len - hlen;
    }
    while (!*err) {
        LONG r;
        int w = c && tls_pending(&tls, c) > 0 ? 1
              : wait_sock(j, s, FALSE, j->wait_s ? j->wait_s : READ_S);
        if (w <= 0) {
            set_err(j, w < 0 ? _("Cancelled") : _("The server stopped answering"));
            *err = TRUE;
            break;
        }
        if (c) {
            r = tls_read(&tls, c, b + hlen, 32768 - hlen);
            if (r == TLS_WANT_READ || r == TLS_WANT_WRITE)
                continue;
        } else {
            r = recv(s, b + hlen, 32768 - hlen, 0);
            if (r < 0 && Errno() == EWOULDBLOCK)
                continue;
        }
        if (r <= 0)
            break;                          /* closed: all there */
        if (Write(fh, b + hlen, r) != r) {
            set_err(j, _("The file could not be written (a full disk?)"));
            *err = TRUE;
            break;
        }
        j->got += r;
    }
    Close(fh);
    if (!*err && j->size && j->got != j->size) {
        set_err(j, _("The download stopped short"));
        *err = TRUE;
    }
    if (*err)
        DeleteFile((CONST_STRPTR)j->file);
    b[hlen] = 0;
    return TRUE;
}

/* The whole response into j->body (AllocVec, 0-terminated): headers and all. */
static BOOL read_all(struct NetJob *j, LONG s, APTR c)
{
    ULONG cap = 32768, len = 0;
    UBYTE *b = AllocVec(cap + 1, MEMF_ANY);
    BOOL headed = FALSE;                    /* a download's headers seen */
    if (!b) {
        set_err(j, _("Not enough memory"));
        return FALSE;
    }
    for (;;) {
        LONG r;
        int w = c && tls_pending(&tls, c) > 0 ? 1
              : wait_sock(j, s, FALSE, j->wait_s ? j->wait_s : READ_S);
        if (w <= 0) {
            FreeVec(b);
            set_err(j, w < 0 ? _("Cancelled") : _("The server stopped answering"));
            return FALSE;
        }
        if (len == cap) {
            UBYTE *nb;
            if (cap >= BODY_MAX)
                break;                      /* enough: the rest is cut off */
            if (!(nb = AllocVec(cap * 2 + 1, MEMF_ANY))) {
                FreeVec(b);
                set_err(j, _("Not enough memory"));
                return FALSE;
            }
            CopyMem(b, nb, len);
            FreeVec(b);
            b = nb;
            cap *= 2;
        }
        if (c) {
            r = tls_read(&tls, c, b + len, cap - len);
            if (r == TLS_WANT_READ || r == TLS_WANT_WRITE)
                continue;
        } else {
            r = recv(s, b + len, cap - len, 0);
            if (r < 0 && Errno() == EWOULDBLOCK)
                continue;
        }
        if (r <= 0)
            break;                          /* closed: all there */
        len += r;
        if (j->upto && len >= j->upto)
            break;                          /* all that was wanted (a stream's start) */
        if (j->file[0] && !headed) {
            /* a download: to the file once the headers are in */
            char *e;
            b[len] = 0;
            if ((e = strstr((char *)b, "\r\n\r\n")) != NULL) {
                BOOL err;
                headed = TRUE;
                if (to_file(j, s, c, b, len, (UBYTE *)e + 4 - b, &err)) {
                    if (err) {
                        FreeVec(b);
                        return FALSE;
                    }
                    j->body = b;            /* the headers alone: parse_response's */
                    j->len = strlen((char *)b);
                    return TRUE;
                }
            }
        }
    }
    b[len] = 0;
    j->body = b;
    j->len = len;
    return TRUE;
}

/* A chunked body put back together, in place. */
static ULONG dechunk(UBYTE *b, ULONG len)
{
    ULONG in = 0, outp = 0;
    while (in < len) {
        ULONG n = 0;
        while (in < len && b[in] != '\r' && b[in] != '\n' && b[in] != ';') {
            UBYTE c = b[in++];
            n = n * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
        }
        while (in < len && b[in] != '\n')
            in++;
        in++;
        if (!n || in + n > len)
            break;
        memmove(b + outp, b + in, n);
        outp += n;
        in += n + 2;                        /* the chunk's CR LF */
    }
    b[outp] = 0;
    return outp;
}

static BOOL header_is(const char *line, const char *name)
{
    LONG n = strlen(name);
    return !amb_strnicmp(line, name, n) && line[n] == ':';
}

/* Status and headers taken off j->body; the body left in their place.
 * Location, if any, into loc. */
static LONG parse_response(struct NetJob *j, char *loc, LONG locmax)
{
    char *p = (char *)j->body, *end = p + j->len, *hdr_end;
    LONG status = 0;
    BOOL chunked = FALSE;
    loc[0] = 0;
    if (j->len < 12 || amb_strnicmp(p, "HTTP/", 5))
        return 0;
    hdr_end = strstr(p, "\r\n\r\n");
    if (!hdr_end)
        return 0;
    {
        char *sp = strchr(p, ' ');
        if (sp)
            status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    }
    while (p < hdr_end) {
        char *nl = strstr(p, "\r\n");
        *nl = 0;
        if (header_is(p, "Location")) {
            char *v = p + 9;
            while (*v == ' ')
                v++;
            amb_strlcpy(loc, v, locmax);
        } else if (header_is(p, "Transfer-Encoding") && strstr(p + 18, "hunked")) {
            chunked = TRUE;
        }
        p = nl + 2;
    }
    p = hdr_end + 4;
    j->len = end - p;
    memmove(j->body, p, j->len + 1);
    if (chunked)
        j->len = dechunk(j->body, j->len);
    return status;
}

/* host:port and path from an http: or https: URL (secure: which);
 * FALSE for nonsense. */
static BOOL split_url(const char *url, char *host, LONG hmax, UWORD *port_no, char *path,
                      LONG pmax, UBYTE *secure)
{
    const char *h, *slash, *colon;
    LONG n;
    *secure = !amb_strnicmp(url, "https://", 8);
    if (amb_strnicmp(url, "http://", 7) && !*secure)
        return FALSE;
    h = url + (*secure ? 8 : 7);
    slash = strchr(h, '/');
    n = slash ? slash - h : (LONG)strlen(h);
    colon = memchr(h, ':', n);
    *port_no = *secure ? 443 : 80;
    if (colon) {
        LONG v = 0;
        const char *c = colon + 1;
        while (c < h + n && *c >= '0' && *c <= '9')
            v = v * 10 + (*c++ - '0');
        *port_no = (UWORD)v;
        n = colon - h;
    }
    if (n <= 0 || n >= hmax)
        return FALSE;
    memcpy(host, h, n);
    host[n] = 0;
    amb_strlcpy(path, slash ? slash : "/", pmax);
    return TRUE;
}

static BOOL fetch(struct NetJob *j, ULONG addr, const char *host, UWORD port_no,
                  const char *path, BOOL secure)
{
    char req[900], clen[80];
    LONG s;
    APTR c = NULL;
    BOOL ok;
    if ((s = open_conn(j, addr, port_no)) < 0)
        return FALSE;
    if (secure && !(c = tls_start(j, s, host))) {
        CloseSocket(s);
        return FALSE;
    }
    /* HTTP/1.1 over HTTPS: plex.tv refuses 1.0 ("426 Upgrade Required");
     * Connection: close all the same, chunked answers put together */
    /* a POST's body: JSON (a Jellyfin sign-in), else none */
    if (j->post && j->send)
        sprintf(clen, "Content-Type: application/json\r\nContent-Length: %lu\r\n",
                (unsigned long)strlen(j->send));
    else
        strcpy(clen, j->post ? "Content-Length: 0\r\n" : "");
    sprintf(req,
            "%s %s HTTP/1.%s\r\nHost: %s\r\nUser-Agent: Ambiance/" AMB_VERSION
            "\r\nAccept: */*\r\n%s%sConnection: close\r\n\r\n",
            j->post ? "POST" : "GET", path, secure ? "1" : "0", host, clen, j->extra);
    if (!send_all(j, s, c, req, strlen(req)) ||
        (j->post && j->send && !send_all(j, s, c, j->send, strlen(j->send)))) {
        tls_free(&tls, c);
        CloseSocket(s);
        set_err(j, j->cancel ? "Cancelled" : "The request could not be sent");
        return FALSE;
    }
    ok = read_all(j, s, c);
    tls_free(&tls, c);
    CloseSocket(s);
    return ok;
}

/* host's address: a dotted quad as it is, a name looked up. */
static BOOL resolve(const char *host, ULONG *addr)
{
    struct hostent *h;
    ULONG a = inet_addr((STRPTR)host);
    if (a != 0xFFFFFFFFUL) {
        *addr = a;
        return TRUE;
    }
    if (!(h = gethostbyname((STRPTR)host)) || h->h_length != 4)
        return FALSE;
    memcpy(addr, h->h_addr_list[0], 4);
    return TRUE;
}

/* GDM: Plex servers on this network, as "address\tport\tname" lines. */
#define GDM_PORT 32414
#define GDM_MS   2500           /* how long answers are waited for */
/* Jellyfin's discovery: "who is JellyfinServer?" broadcast to UDP 7359;
 * each server answers with JSON (Address, Id, Name), as "address\tid\t
 * name" lines (jfjson.c reads them). */
#define JF_DISC_PORT 7359
#define JF_DISC_MS   2000
static void jf_discover(struct NetJob *j)
{
    static const char q[] = "who is JellyfinServer?";
    struct sockaddr_in sa;
    LONG s, one = 1;
    ULONG deadline;
    UBYTE *out;
    ULONG len = 0, cap = 2048;
    char buf[1025];
    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        set_err(j, _("No socket (is the TCP/IP stack running?)"));
        return;
    }
    if (!(out = AllocVec(cap + 1, MEMF_ANY | MEMF_CLEAR))) {
        CloseSocket(s);
        set_err(j, _("Not enough memory"));
        return;
    }
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    IoctlSocket(s, FIONBIO, (char *)&one);
    bind_out(s);
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = JF_DISC_PORT;             /* big-endian already */
    sa.sin_addr.s_addr = 0xFFFFFFFFUL;
    sendto(s, (APTR)q, sizeof(q) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
    deadline = time_ms() + JF_DISC_MS;
    while ((LONG)(time_ms() - deadline) < 0) {
        struct sockaddr_in from;
        LONG fl = sizeof(from), r;
        int w = wait_sock(j, s, FALSE, 1);
        char addr[80], id[40], name[64], line[200];
        if (w < 0)
            break;
        if (w == 0)
            continue;
        r = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0)
            continue;
        buf[r] = 0;
        if (!jf_discovered(buf, (unsigned long)r, addr, sizeof(addr), id, sizeof(id), name,
                           sizeof(name)))
            continue;
        utf8_to_latin1(name);
        sprintf(line, "%s\t%s\t%s\n", addr, id, name);
        if (strstr((char *)out, line))
            continue;                       /* (answered twice) */
        if (len + strlen(line) < cap) {
            strcpy((char *)out + len, line);
            len += strlen(line);
        }
    }
    CloseSocket(s);
    if (j->cancel) {
        FreeVec(out);
        set_err(j, _("Cancelled"));
        return;
    }
    if (!len) {
        FreeVec(out);
        set_err(j, _("No Jellyfin server answered on this network"));
        return;
    }
    j->body = out;
    j->len = len;
    j->status = 200;
}

static void gdm(struct NetJob *j)
{
    static const char q[] = "M-SEARCH * HTTP/1.0";
    static const ULONG to[2] = { 0xEF0000FAUL, 0xFFFFFFFFUL };  /* 239.0.0.250, broadcast */
    struct sockaddr_in sa;
    LONG s, one = 1, i;
    ULONG seen[16], deadline;
    int nseen = 0;
    UBYTE *out;
    ULONG len = 0, cap = 4096;
    char buf[1025];
    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        set_err(j, _("No socket (is the TCP/IP stack running?)"));
        return;
    }
    if (!(out = AllocVec(cap + 1, MEMF_ANY | MEMF_CLEAR))) {
        CloseSocket(s);
        set_err(j, _("Not enough memory"));
        return;
    }
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    IoctlSocket(s, FIONBIO, (char *)&one);
    bind_out(s);
    for (i = 0; i < 2; i++) {
        memset(&sa, 0, sizeof(sa));
        sa.sin_len = sizeof(sa);
        sa.sin_family = AF_INET;
        sa.sin_port = GDM_PORT;             /* big-endian already */
        memcpy(&sa.sin_addr, &to[i], 4);
        sendto(s, (APTR)q, sizeof(q) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
    }
    deadline = time_ms() + GDM_MS;
    while ((LONG)(time_ms() - deadline) < 0) {
        struct sockaddr_in from;
        LONG fl = sizeof(from), r;
        int w = wait_sock(j, s, FALSE, 1);
        ULONG addr;
        char name[80], port[12], line[128];
        const UBYTE *ab;
        if (w < 0)
            break;
        if (w == 0)
            continue;
        r = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0)
            continue;
        buf[r] = 0;
        if (!strstr(buf, "200 OK"))
            continue;
        memcpy(&addr, &from.sin_addr, 4);
        for (i = 0; i < nseen && seen[i] != addr; i++)
            ;
        if (i < nseen || nseen >= 16)
            continue;                       /* answered twice (multicast and broadcast) */
        seen[nseen++] = addr;
        name[0] = 0;
        amb_strlcpy(port, "32400", sizeof(port));
        {
            char *p = buf;
            while (p && *p) {
                char *e = strchr(p, '\n'), *v;
                if (e)
                    *e = 0;
                if ((v = strchr(p, ':'))) {
                    char *t = v + 1, *z;
                    *v = 0;
                    while (*t == ' ')
                        t++;
                    for (z = t + strlen(t); z > t && (z[-1] == '\r' || z[-1] == ' '); z--)
                        ;
                    *z = 0;
                    if (!amb_stricmp(p, "Name"))
                        amb_strlcpy(name, t, sizeof(name));
                    else if (!amb_stricmp(p, "Port"))
                        amb_strlcpy(port, t, sizeof(port));
                }
                p = e ? e + 1 : NULL;
            }
        }
        ab = (const UBYTE *)&addr;
        sprintf(line, "%lu.%lu.%lu.%lu\t%s\t%s\n", (unsigned long)ab[0], (unsigned long)ab[1],
                (unsigned long)ab[2], (unsigned long)ab[3], port, name);
        if (len + strlen(line) < cap) {
            strcpy((char *)out + len, line);
            len += strlen(line);
        }
    }
    CloseSocket(s);
    if (j->cancel) {
        FreeVec(out);
        set_err(j, _("Cancelled"));
        return;
    }
    if (!len) {
        FreeVec(out);
        set_err(j, _("No Plex server answered on this network"));
        return;
    }
    j->body = out;
    j->len = len;
    j->status = 200;
}

/* The account's servers too (signed in at plex.tv, HTTPS on: the job's
 * tls, its extra the account's headers): first, each with its token, then
 * those GDM found that are not among them. */
static void gdm_account(struct NetJob *j)
{
    static char acc[2048];              /* the network process's alone */
    UBYTE *g = j->body, *out;
    ULONG glen = j->len, len, addr;
    char err[96], loc[8];
    const char *p, *e;
    amb_strlcpy(err, j->err, sizeof(err));
    j->body = NULL;
    j->len = 0;
    j->err[0] = 0;
    acc[0] = 0;
    if (!resolve(PLEX_TV, &addr))
        set_err(j, _("That server could not be found (DNS)"));
    else if (fetch(j, addr, PLEX_TV, 443, "/api/resources?includeHttps=1&includeRelay=0", TRUE)) {
        LONG st = parse_response(j, loc, sizeof(loc));
        if (st == 200)
            plex_resource_lines(j->body, j->len, acc, sizeof(acc));
        else {
            sprintf(err, _("The server answered %ld"), (long)st);
            set_err(j, err);
        }
        FreeVec(j->body);
    }
    if (j->err[0])
        amb_log("plex: the account's servers", j->err);
    j->body = NULL;
    j->len = 0;
    if (!acc[0] || !(out = AllocVec(strlen(acc) + glen + 1, MEMF_ANY))) {
        j->body = g;                        /* GDM's alone, as they were */
        j->len = glen;
        amb_strlcpy(j->err, g ? "" : err, sizeof(j->err));
        return;
    }
    strcpy((char *)out, acc);
    len = strlen(acc);
    for (p = (const char *)g; g && p < (const char *)g + glen; p = e + 1) {
        const char *t;
        char key[72];
        LONG k;
        if (!(e = strchr(p, '\n')))
            break;
        /* "address\tport\t": already the account's? */
        if (!(t = strchr(p, '\t')) || !(t = strchr(t + 1, '\t')) || t > e ||
            (k = t + 1 - p) >= (LONG)sizeof(key) - 1)
            continue;
        key[0] = '\n';
        memcpy(key + 1, p, k);
        key[k + 1] = 0;
        if (!strncmp(acc, key + 1, k) || strstr(acc, key))
            continue;
        memcpy(out + len, p, e + 1 - p);
        len += e + 1 - p;
    }
    out[len] = 0;
    FreeVec(g);
    j->body = out;
    j->len = len;
    j->status = 200;
    j->err[0] = 0;
}

static void run(struct NetJob *j)
{
    char host[80], path[400], loc[400];
    UWORD port_no = 80;
    ULONG addr = 0;
    UBYTE secure = j->tls;
    int hop, tries;
    ULONG t0 = time_ms();
    BOOL is_api = !j->host[0];
    j->t_start = t0;
    j->status = 0;
    j->err[0] = 0;
    j->body = NULL;
    j->len = 0;
    if (!SocketBase) {
        set_err(j, "No TCP/IP stack is running (bsdsocket.library)");
        return;
    }
    if (j->jfdisc) {
        jf_discover(j);
        j->ms = time_ms() - t0;
        return;
    }
    if (j->gdm) {
        gdm(j);
        if (j->tls && use_https)
            gdm_account(j);
        j->ms = time_ms() - t0;
        return;
    }
    if (secure && !use_https) {
        set_err(j, _("HTTPS is off (Settings > Networking)"));
        return;
    }
    amb_strlcpy(path, j->path, sizeof(path));
    for (hop = 0; hop <= REDIRECTS; hop++) {
        tries = 1;
        if (is_api && hop == 0) {
            if (!find_api(j))
                return;
            tries = api.n;                  /* each server once */
        }
        while (tries-- > 0) {
            if (is_api && hop == 0) {
                addr = api.addr[api.next];
                amb_strlcpy(host, api.name[api.next], sizeof(host));
                port_no = 80;
            } else if (hop == 0) {
                amb_strlcpy(host, j->host, sizeof(host));
                port_no = j->port ? j->port : secure ? 443 : 80;
                if (!resolve(host, &addr)) {
                    set_err(j, _("That server could not be found (DNS)"));
                    return;
                }
            }
            j->err[0] = 0;
            if (fetch(j, addr, host, port_no, path, secure))
                break;
            if (j->cancel)
                return;
            if (is_api && hop == 0)         /* that server failed: the next one */
                api.next = (api.next + 1) % api.n;
        }
        if (!j->body)
            return;
        j->status = parse_response(j, loc, sizeof(loc));
        if (j->status >= 300 && j->status < 400 && loc[0]) {
            struct hostent *h;
            FreeVec(j->body);
            j->body = NULL;
            j->len = 0;
            if (loc[0] == '/') {            /* the same server, another path */
                amb_strlcpy(path, loc, sizeof(path));
                continue;
            }
            if (!amb_strnicmp(loc, "https:", 6) && !use_https) {
                set_err(j, _("The server only answers over HTTPS"));
                return;
            }
            if (!split_url(loc, host, sizeof(host), &port_no, path, sizeof(path), &secure) ||
                !(h = gethostbyname((STRPTR)host)) || h->h_length != 4) {
                set_err(j, _("The server sent us somewhere we cannot follow"));
                return;
            }
            memcpy(&addr, h->h_addr_list[0], 4);
            is_api = FALSE;
            continue;
        }
        break;
    }
    if (j->body && (j->status < 200 || j->status > 299)) {    /* 201: plex.tv's new code */
        char m[96];
        sprintf(m, _("The server answered %ld"), (long)j->status);
        set_err(j, m);
        FreeVec(j->body);
        j->body = NULL;
        j->len = 0;
    }
    if (j->body)
        amb_strlcpy(j->server, host, sizeof(j->server));
    j->ms = time_ms() - t0;
}

static void net_main(void)
{
    struct MsgPort *p = CreateMsgPort();
    port = p;
    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    Signal(parent, 1UL << start_sig);
    if (!p) {
        if (SocketBase)
            CloseLibrary(SocketBase);
        return;
    }
    for (;;) {
        struct NetJob *j;
        WaitPort(p);
        while ((j = (struct NetJob *)GetMsg(p))) {
            if (j->quit) {
                tls_close(&tls);            /* before the socket library it uses */
                if (SocketBase)
                    CloseLibrary(SocketBase);
                SocketBase = NULL;
                DeleteMsgPort(p);
                Forbid();                   /* gone before Ambiance can unload */
                ReplyMsg(&j->msg);
                return;
            }
            if (!j->cancel)
                run(j);
            if (j->parse && j->body && !j->cancel && list_init(&j->list)) {
                /* a Plex list read here, not in the UI (net priority -1:
                 * it takes the time the UI leaves) */
                ULONG t0 = time_ms();
                j->n = plex_fill(j->pkey, j->body, j->len, &j->list, &j->total);
                j->ms_read = time_ms() - t0;
                j->parsed = TRUE;
            }
            ReplyMsg(&j->msg);
        }
    }
}

BOOL net_start(void)
{
    if (running)
        return TRUE;
    parent = FindTask(NULL);
    if (!reply_port && !(reply_port = CreateMsgPort()))
        return FALSE;
    if ((start_sig = AllocSignal(-1)) < 0)
        return FALSE;
    port = NULL;
    SetSignal(0, 1UL << start_sig);
    proc = (struct Task *)CreateNewProcTags(NP_Entry, (ULONG)net_main,
                                            NP_Name, (ULONG)"Ambiance network",
                                            NP_StackSize, 65536, NP_Priority, -1,
                                            NP_WindowPtr, (ULONG)-1, TAG_END);
    if (proc)
        Wait(1UL << start_sig);
    FreeSignal(start_sig);
    start_sig = -1;
    running = proc && port;
    if (running)
        amb_log("network", "process started");
    return running;
}

ULONG net_sigmask(void)
{
    return reply_port ? 1UL << reply_port->mp_SigBit : 0;
}

void net_submit(struct NetJob *j)
{
    j->t_sub = time_ms();
    j->busy = TRUE;
    j->done = FALSE;
    j->quit = FALSE;
    if (!running && !net_start()) {
        j->busy = FALSE;
        j->done = TRUE;
        j->body = NULL;
        amb_strlcpy(j->err, _("The network process could not start"), sizeof(j->err));
        return;
    }
    out++;
    j->msg.mn_ReplyPort = reply_port;
    j->msg.mn_Length = sizeof(*j);
    PutMsg(port, &j->msg);
}

struct NetJob *net_collect(void)
{
    struct NetJob *j;
    if (!reply_port || !(j = (struct NetJob *)GetMsg(reply_port)))
        return NULL;
    out--;
    j->busy = FALSE;
    j->done = TRUE;
    return j;
}

void net_stop(void)
{
    struct NetJob q;
    if (!running)
        return;
    /* every job out back first (each gives up within a quarter second) */
    while (out > 0) {
        struct NetJob *j;
        WaitPort(reply_port);
        while ((j = net_collect())) {
            if (j->body)
                FreeVec(j->body);
            j->body = NULL;
            if (j->orphan) {
                if (j->parsed)
                    list_free(&j->list);    /* (a Plex list read for it) */
                FreeVec(j);
            }
        }
    }
    memset(&q, 0, sizeof(q));
    q.quit = TRUE;
    q.msg.mn_ReplyPort = reply_port;
    q.msg.mn_Length = sizeof(q);
    PutMsg(port, &q.msg);
    WaitPort(reply_port);
    GetMsg(reply_port);
    running = FALSE;
    proc = NULL;
    port = NULL;
    DeleteMsgPort(reply_port);
    reply_port = NULL;
    amb_log("network", "process stopped");
}

/* Every job out told to stop (net_stop then waits for them). */
void net_cancel(struct NetJob *j)
{
    if (j && j->busy)
        j->cancel = TRUE;
}

/* ---- the interfaces, for Settings > Networking. Called from the main
 * task: it opens bsdsocket.library for itself (a library base is its
 * opener's), the SocketBase below standing for the network process's. */
/* ReleaseInterfaceList, called by hand: the NDK's inline for it names a0
 * both as its argument and as clobbered, which GCC refuses. */
static void release_ifs(struct Library *base, struct List *l)
{
    register struct Library *a6 __asm("a6") = base;
    register struct List *a0 __asm("a0") = l;
    __asm volatile("jsr a6@(-456:W)"
                   : "+r"(a0)
                   : "r"(a6)
                   : "d0", "d1", "a1", "fp0", "fp1", "cc", "memory");
}

static BOOL is_loop(const char *name, ULONG ip)
{
    return (name[0] == 'l' && name[1] == 'o') || (ip >> 24) == 127;
}

int net_interfaces(struct NetIf *out, int max, const char *toward, ULONG *in_use)
{
    struct Library *SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    int n = -1;
    LONG have = 0;
    *in_use = 0;
    if (!SocketBase)
        return -1;
    if (SocketBaseTags(SBTM_GETREF(SBTC_HAVE_INTERFACE_API), (ULONG)&have, TAG_END) != 0)
        have = 0;
    if (have) {
        /* Roadshow: its list, each asked for its address, state, device */
        struct List *l = ObtainInterfaceList();
        if (l) {
            struct Node *nd;
            n = 0;
            for (nd = l->lh_Head; nd->ln_Succ && n < max; nd = nd->ln_Succ) {
                struct sockaddr_in a;
                LONG state = 0, unit = 0;
                STRPTR dev = NULL;
                struct NetIf *f = &out[n];
                memset(&a, 0, sizeof(a));
                if (!nd->ln_Name)
                    continue;
                QueryInterfaceTags((STRPTR)nd->ln_Name, IFQ_Address, (ULONG)&a, IFQ_State,
                                   (ULONG)&state, IFQ_DeviceName, (ULONG)&dev, IFQ_DeviceUnit,
                                   (ULONG)&unit, TAG_END);
                memset(f, 0, sizeof(*f));
                amb_strlcpy(f->name, nd->ln_Name, sizeof(f->name));
                memcpy(&f->ip, &a.sin_addr, 4);
                if (is_loop(f->name, f->ip))
                    continue;
                f->up = state == SM_Up;
                if (dev)
                    amb_strlcpy(f->dev, (const char *)FilePart(dev), sizeof(f->dev));
                f->unit = unit;
                n++;
            }
            release_ifs(SocketBase, l);
        }
    }
    if (n < 0) {
        /* other stacks: SIOCGIFCONF, an ifreq each (longer where an
         * address is), and SIOCGIFFLAGS for whether it is up */
        LONG s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            static UBYTE buf[1024];
            struct ifconf ifc;
            ifc.ifc_len = sizeof(buf);
            ifc.ifc_buf = (APTR)buf;
            if (IoctlSocket(s, SIOCGIFCONF, (char *)&ifc) == 0 && ifc.ifc_len > 0) {
                UBYTE *p = buf, *end = buf + ifc.ifc_len;
                n = 0;
                while (p + IFNAMSIZ + 2 <= end && n < max) {
                    struct ifreq *r = (struct ifreq *)p, fr;
                    ULONG alen = r->ifr_addr.sa_len > sizeof(struct sockaddr)
                                 ? r->ifr_addr.sa_len : sizeof(struct sockaddr);
                    struct NetIf *f = &out[n];
                    int k;
                    p += IFNAMSIZ + alen;
                    if (r->ifr_addr.sa_family != AF_INET)
                        continue;
                    memset(f, 0, sizeof(*f));
                    memcpy(f->name, r->ifr_name, IFNAMSIZ);
                    f->name[IFNAMSIZ] = 0;
                    memcpy(&f->ip, &((struct sockaddr_in *)&r->ifr_addr)->sin_addr, 4);
                    if (is_loop(f->name, f->ip))
                        continue;
                    for (k = 0; k < n && strcmp(out[k].name, f->name); k++)
                        ;
                    if (k < n)
                        continue;           /* listed once */
                    memset(&fr, 0, sizeof(fr));
                    memcpy(fr.ifr_name, r->ifr_name, IFNAMSIZ);
                    f->up = IoctlSocket(s, SIOCGIFFLAGS, (char *)&fr) == 0 &&
                            (fr.ifr_flags & IFF_UP);
                    n++;
                }
            }
            CloseSocket(s);
        }
    }
    {
        /* the address used toward the server (or any address off this
         * one): a UDP socket connected, nothing sent, then its own name */
        LONG s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct sockaddr_in sa, me;
            LONG ml = sizeof(me);
            ULONG to = toward && toward[0] ? inet_addr((STRPTR)toward) : 0xFFFFFFFFUL;
            ULONG ip = bind_ip;
            if (to == 0xFFFFFFFFUL)
                to = 0x01010101UL;          /* 1.1.1.1: off the local network */
            if (ip) {
                memset(&me, 0, sizeof(me));
                me.sin_len = sizeof(me);
                me.sin_family = AF_INET;
                memcpy(&me.sin_addr, &ip, 4);
                bind(s, (struct sockaddr *)&me, sizeof(me));
            }
            memset(&sa, 0, sizeof(sa));
            sa.sin_len = sizeof(sa);
            sa.sin_family = AF_INET;
            sa.sin_port = 9;                /* discard: never sent to */
            memcpy(&sa.sin_addr, &to, 4);
            memset(&me, 0, sizeof(me));
            if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
                getsockname(s, (struct sockaddr *)&me, &ml) == 0)
                memcpy(in_use, &me.sin_addr, 4);
            CloseSocket(s);
        }
    }
    CloseLibrary(SocketBase);
    return n;
}

/* The station-list servers, for Settings > Networking > Radio: the
 * addresses of all.api.radio-browser.info, each named (reverse lookup) and
 * timed connecting (ms; -1 no answer within a second and a half). From the
 * main task, as net_interfaces. */
int net_radio_mirrors(struct NetMirror *out, int max)
{
    struct Library *SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    struct hostent *h;
    ULONG addr[MAX_HOSTS];
    int i, n = 0;
    if (!SocketBase)
        return -1;
    if (!(h = gethostbyname((STRPTR)API_ALL)) || h->h_length != 4) {
        CloseLibrary(SocketBase);
        return -1;
    }
    for (i = 0; h->h_addr_list[i] && n < MAX_HOSTS && n < max; i++)
        memcpy(&addr[n++], h->h_addr_list[i], 4);
    for (i = 0; i < n; i++) {
        struct NetMirror *m = &out[i];
        struct hostent *r = gethostbyaddr((STRPTR)&addr[i], 4, AF_INET);
        const char *nm = r && r->h_name ? (const char *)r->h_name : "";
        LONG k = strlen(nm), s, one = 1;
        memset(m, 0, sizeof(*m));
        m->addr = addr[i];
        m->ms = -1;
        if (k < 24 || amb_stricmp(nm + k - 23, ".api.radio-browser.info"))
            nm = "";                        /* no name of its own: left out below */
        amb_strlcpy(m->name, nm, sizeof(m->name));
        if ((s = socket(AF_INET, SOCK_STREAM, 0)) >= 0) {
            struct sockaddr_in sa;
            ULONG t0 = time_ms();
            IoctlSocket(s, FIONBIO, (char *)&one);
            memset(&sa, 0, sizeof(sa));
            sa.sin_len = sizeof(sa);
            sa.sin_family = AF_INET;
            sa.sin_port = 80;
            memcpy(&sa.sin_addr, &addr[i], 4);
            if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 || Errno() == EINPROGRESS) {
                fd_set fs;
                struct timeval tv;
                LONG e = 0, el = sizeof(e);
                FD_ZERO(&fs);
                FD_SET(s, &fs);
                tv.tv_secs = 1;
                tv.tv_micro = 500000;
                if (WaitSelect(s + 1, NULL, &fs, NULL, &tv, NULL) > 0 &&
                    getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) == 0 && !e)
                    m->ms = (LONG)(time_ms() - t0);
            }
            CloseSocket(s);
        }
    }
    CloseLibrary(SocketBase);
    {
        /* the nameless dropped (the API is asked by name) */
        int k = 0;
        for (i = 0; i < n; i++)
            if (out[i].name[0])
                out[k++] = out[i];
        n = k;
    }
    return n;
}
