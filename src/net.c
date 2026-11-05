/*
 * AmigaMCE - the network process: HTTP requests off the UI
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
 * process opens it, and is the only one that uses SocketBase. Plain HTTP
 * only (HTTP/1.0, Connection: close, chunked bodies understood all the
 * same); a redirect to another http: address is followed, one to https:
 * is reported. Every wait has a timeout, and a job given up by the UI
 * (cancel) stops at the next one.
 *
 * The radio-browser.info API is found as its documentation asks: the
 * addresses of all.api.radio-browser.info, one chosen at random, its name
 * by reverse lookup (the Host the server answers to); a server that fails
 * gives way to the next.
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/filio.h>

struct Library *SocketBase;         /* the network process's alone */

#define API_ALL     "all.api.radio-browser.info"
#define CONNECT_S   10              /* seconds to connect */
#define READ_S      20              /* seconds without a byte before giving up */
#define BODY_MAX    (1536UL << 10)  /* a body larger is cut off */
#define MAX_HOSTS   8
#define REDIRECTS   3

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
        amce_strlcpy(j->err, s, sizeof(j->err));
}

static BOOL find_api(struct NetJob *j)
{
    struct hostent *h;
    int i, n = 0;
    ULONG seed = time_ms();
    if (api.n)
        return TRUE;
    if (!(h = gethostbyname((STRPTR)API_ALL)) || h->h_length != 4) {
        set_err(j, "radio-browser.info could not be found (DNS)");
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
        if (k < 24 || amce_stricmp(nm + k - 23, ".api.radio-browser.info"))
            nm = API_ALL;
        amce_strlcpy(api.name[i], nm, sizeof(api.name[i]));
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

static LONG open_conn(struct NetJob *j, ULONG addr, UWORD port_no)
{
    struct sockaddr_in sa;
    LONG s, one = 1, e = 0, el = sizeof(e);
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        set_err(j, "No socket (is the TCP/IP stack running?)");
        return -1;
    }
    IoctlSocket(s, FIONBIO, (char *)&one);
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = port_no;                  /* big-endian already: htons is nothing */
    memcpy(&sa.sin_addr, &addr, 4);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 && Errno() != EINPROGRESS) {
        CloseSocket(s);
        set_err(j, "The connection was refused");
        return -1;
    }
    if (wait_sock(j, s, TRUE, CONNECT_S) <= 0 ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
        CloseSocket(s);
        set_err(j, j->cancel ? "Cancelled" : "The server did not answer");
        return -1;
    }
    return s;
}

static BOOL send_all(struct NetJob *j, LONG s, const char *buf, LONG n)
{
    while (n > 0) {
        LONG w;
        if (wait_sock(j, s, TRUE, READ_S) <= 0)
            return FALSE;
        w = send(s, (APTR)buf, n, 0);
        if (w <= 0)
            return FALSE;
        buf += w;
        n -= w;
    }
    return TRUE;
}

/* The whole response into j->body (AllocVec, 0-terminated): headers and all. */
static BOOL read_all(struct NetJob *j, LONG s)
{
    ULONG cap = 32768, len = 0;
    UBYTE *b = AllocVec(cap + 1, MEMF_ANY);
    if (!b) {
        set_err(j, "Not enough memory");
        return FALSE;
    }
    for (;;) {
        LONG r;
        int w = wait_sock(j, s, FALSE, READ_S);
        if (w <= 0) {
            FreeVec(b);
            set_err(j, w < 0 ? "Cancelled" : "The server stopped answering");
            return FALSE;
        }
        if (len == cap) {
            UBYTE *nb;
            if (cap >= BODY_MAX)
                break;                      /* enough: the rest is cut off */
            if (!(nb = AllocVec(cap * 2 + 1, MEMF_ANY))) {
                FreeVec(b);
                set_err(j, "Not enough memory");
                return FALSE;
            }
            CopyMem(b, nb, len);
            FreeVec(b);
            b = nb;
            cap *= 2;
        }
        r = recv(s, b + len, cap - len, 0);
        if (r < 0 && Errno() == EWOULDBLOCK)
            continue;
        if (r <= 0)
            break;                          /* closed: all there */
        len += r;
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
    return !amce_strnicmp(line, name, n) && line[n] == ':';
}

/* Status and headers taken off j->body; the body left in their place.
 * Location, if any, into loc. */
static LONG parse_response(struct NetJob *j, char *loc, LONG locmax)
{
    char *p = (char *)j->body, *end = p + j->len, *hdr_end;
    LONG status = 0;
    BOOL chunked = FALSE;
    loc[0] = 0;
    if (j->len < 12 || amce_strnicmp(p, "HTTP/", 5))
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
            amce_strlcpy(loc, v, locmax);
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

/* host:port and path from an http: URL; FALSE for https: or nonsense. */
static BOOL split_url(const char *url, char *host, LONG hmax, UWORD *port_no, char *path,
                      LONG pmax)
{
    const char *h, *slash, *colon;
    LONG n;
    if (amce_strnicmp(url, "http://", 7))
        return FALSE;
    h = url + 7;
    slash = strchr(h, '/');
    n = slash ? slash - h : (LONG)strlen(h);
    colon = memchr(h, ':', n);
    *port_no = 80;
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
    amce_strlcpy(path, slash ? slash : "/", pmax);
    return TRUE;
}

static BOOL fetch(struct NetJob *j, ULONG addr, const char *host, UWORD port_no,
                  const char *path)
{
    char req[700];
    LONG s;
    if ((s = open_conn(j, addr, port_no)) < 0)
        return FALSE;
    sprintf(req,
            "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: AmigaMCE/" AMCE_VERSION
            "\r\nAccept: */*\r\nConnection: close\r\n\r\n",
            path, host);
    if (!send_all(j, s, req, strlen(req))) {
        CloseSocket(s);
        set_err(j, j->cancel ? "Cancelled" : "The request could not be sent");
        return FALSE;
    }
    if (!read_all(j, s)) {
        CloseSocket(s);
        return FALSE;
    }
    CloseSocket(s);
    return TRUE;
}

static void run(struct NetJob *j)
{
    char host[80], path[400], loc[400];
    UWORD port_no = 80;
    ULONG addr = 0;
    int hop, tries;
    ULONG t0 = time_ms();
    BOOL is_api = !j->host[0];
    j->status = 0;
    j->err[0] = 0;
    j->body = NULL;
    j->len = 0;
    if (!SocketBase) {
        set_err(j, "No TCP/IP stack is running (bsdsocket.library)");
        return;
    }
    amce_strlcpy(path, j->path, sizeof(path));
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
                amce_strlcpy(host, api.name[api.next], sizeof(host));
                port_no = 80;
            } else if (hop == 0) {
                struct hostent *h;
                amce_strlcpy(host, j->host, sizeof(host));
                port_no = j->port ? j->port : 80;
                if (!(h = gethostbyname((STRPTR)host)) || h->h_length != 4) {
                    set_err(j, "That server could not be found (DNS)");
                    return;
                }
                memcpy(&addr, h->h_addr_list[0], 4);
            }
            j->err[0] = 0;
            if (fetch(j, addr, host, port_no, path))
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
                amce_strlcpy(path, loc, sizeof(path));
                continue;
            }
            if (!amce_strnicmp(loc, "https:", 6)) {
                set_err(j, "The server only answers over HTTPS");
                return;
            }
            if (!split_url(loc, host, sizeof(host), &port_no, path, sizeof(path)) ||
                !(h = gethostbyname((STRPTR)host)) || h->h_length != 4) {
                set_err(j, "The server sent us somewhere we cannot follow");
                return;
            }
            memcpy(&addr, h->h_addr_list[0], 4);
            is_api = FALSE;
            continue;
        }
        break;
    }
    if (j->body && j->status != 200) {
        char m[64];
        sprintf(m, "The server answered %ld", (long)j->status);
        set_err(j, m);
        FreeVec(j->body);
        j->body = NULL;
        j->len = 0;
    }
    if (j->body)
        amce_strlcpy(j->server, host, sizeof(j->server));
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
                if (SocketBase)
                    CloseLibrary(SocketBase);
                SocketBase = NULL;
                DeleteMsgPort(p);
                Forbid();                   /* gone before AmigaMCE can unload */
                ReplyMsg(&j->msg);
                return;
            }
            if (!j->cancel)
                run(j);
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
                                            NP_Name, (ULONG)"AmigaMCE network",
                                            NP_StackSize, 32768, NP_Priority, -1,
                                            NP_WindowPtr, (ULONG)-1, TAG_END);
    if (proc)
        Wait(1UL << start_sig);
    FreeSignal(start_sig);
    start_sig = -1;
    running = proc && port;
    if (running)
        amce_log("network", "process started");
    return running;
}

ULONG net_sigmask(void)
{
    return reply_port ? 1UL << reply_port->mp_SigBit : 0;
}

void net_submit(struct NetJob *j)
{
    j->busy = TRUE;
    j->done = FALSE;
    j->quit = FALSE;
    if (!running && !net_start()) {
        j->busy = FALSE;
        j->done = TRUE;
        j->body = NULL;
        amce_strlcpy(j->err, "The network process could not start", sizeof(j->err));
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
            if (j->orphan)
                FreeVec(j);
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
    amce_log("network", "process stopped");
}

/* Every job out told to stop (net_stop then waits for them). */
void net_cancel(struct NetJob *j)
{
    if (j && j->busy)
        j->cancel = TRUE;
}
