/*
 * AmigaMCE - HTTPS: TLS over a socket, by AmiSSL
 * Copyright (c) 2026 Brian King
 *
 * AmiSSL (amisslmaster.library, AmiSSL 5: OpenSSL 3) is opened by each
 * process that uses it, for itself, with its own bsdsocket.library: a
 * base of AmiSSL's is its opener's, as SocketBase is. So the network
 * process and the radio process each have a TlsLib, and AmiSSL's inline
 * calls (and the inline functions of its headers) find the calling
 * process's bases by its task (tls_base), never a global shared by both.
 *
 * AmiSSL never touches the socket: it reads and writes two memory BIOs,
 * and the bytes between them and the socket are moved here, with the same
 * bsdsocket calls the plain connections use (recv, send, EWOULDBLOCK by
 * Errno). Given the socket itself (SSL_set_fd), AmiSSL's own reads failed
 * at once under Amiberry's bsdsocket.library ("system library: Device not
 * configured", errno 6) where ours, on the same socket, work.
 *
 * The sockets are non-blocking, as the plain ones are: a handshake, a read
 * or a write that would wait says so (TLS_WANT_READ, TLS_WANT_WRITE), and
 * the caller waits on the socket as it does already (its timeout, its
 * Stop), then calls again.
 *
 * OpenSSL's error queue is emptied before each handshake, read and write:
 * SSL_get_error reads it, and an error left there by anything before
 * (loading the certificates) made a read that only had to wait look like
 * a failure, every connection given up at its first wait.
 *
 * The root certificates a server's is checked against are AmigaMCE's own,
 * Certs/cacert.pem (Mozilla's, from certifi), read here and given to
 * OpenSSL from memory, the first time a check is wanted; AmiSSL's own
 * (AmiSSL:Certs) only without them, and only where that assign exists:
 * OpenSSL's default lookup stat()ed AmiSSL:Certs, which an AmiSSL put in
 * LIBS: alone (as AGS's is) does not have, and the handshake failed with
 * "system library: Device not configured".
 *
 * Certificates are checked where something
 * of the user's is sent (plex.tv: the sign-in); a radio station's stream is
 * public, and many stations' certificates would fail an Amiga's checks
 * (a clock not set, an old chain), so theirs is not.
 *
 * Only asked for when Settings > Networking > HTTPS is on (AmiSSL found).
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <libraries/bsdsocket.h>
#include <sys/errno.h>

/* AmiSSL's calls take the base of the process making them */
struct Library;
static struct Library *tls_base(int ext);
#define AMISSL_BASE_NAME    tls_base(0)
#define AMISSLEXT_BASE_NAME tls_base(1)
#define __NOLIBBASE__                       /* AmiSSLMasterBase: tls_open's own */
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <utility/tagitem.h>

/* The processes with AmiSSL open (two: the network's, the radio's). */
#define OWNERS 4
static struct TlsLib *volatile owner[OWNERS];
static struct Task *volatile owner_task[OWNERS];

static struct Library *tls_base(int ext)
{
    struct Task *me = FindTask(NULL);
    int i;
    for (i = 0; i < OWNERS; i++)
        if (owner_task[i] == me && owner[i])
            return ext ? owner[i]->ext : owner[i]->ssl;
    return NULL;
}

static BOOL own(struct TlsLib *t, BOOL on)
{
    struct Task *me = FindTask(NULL);
    int i;
    Forbid();
    for (i = 0; i < OWNERS; i++)
        if (on ? !owner[i] : owner[i] == t) {
            owner[i] = on ? t : NULL;
            owner_task[i] = on ? me : NULL;
            break;
        }
    Permit();
    return i < OWNERS;
}

#define BASES(t) (void)(t)

/* OpenSSL's random numbers seeded from what this Amiga has to hand: the
 * clock and the date, the beam's position (read again and again, each
 * read a little later), tasks and free memory. A TLS handshake needs
 * random numbers at once; AmiSSL on a 68k has no source of its own here
 * (as YAM, by AmiSSL's own author, seeds it), and without one OpenSSL
 * stopped the handshake at its first step with a "system library" error
 * ("Device not configured"). */
static void seed(struct TlsLib *t)
{
    ULONG pool[48];
    struct DateStamp ds;
    volatile UWORD *vhpos = (volatile UWORD *)0xDFF006;
    int i, k = 0;
    BASES(t);
    DateStamp(&ds);
    pool[k++] = (ULONG)ds.ds_Days;
    pool[k++] = (ULONG)ds.ds_Minute;
    pool[k++] = (ULONG)ds.ds_Tick;
    pool[k++] = time_ms();
    pool[k++] = (ULONG)FindTask(NULL);
    pool[k++] = AvailMem(MEMF_ANY);
    pool[k++] = AvailMem(MEMF_CHIP);
    pool[k++] = (ULONG)SysBase->DispCount;
    pool[k++] = (ULONG)SysBase->IdleCount;
    pool[k++] = (ULONG)t ^ (ULONG)t->ctx;
    for (i = 0; k < 48; i++) {
        ULONG v = ((ULONG)*vhpos << 16) ^ time_ms() ^ ((ULONG)i * 2654435761UL);
        LONG d;
        for (d = 0; d < 50 + (LONG)(v & 63); d++)
            v = v * 1103515245UL + 12345UL + *vhpos;    /* a little later each time */
        pool[k++] = v;
    }
    RAND_seed(pool, sizeof(pool));
}

BOOL tls_open(struct TlsLib *t, struct Library *socketbase)
{
    struct Library *AmiSSLMasterBase, *ssl = NULL, *ext = NULL;
    SSL_CTX *ctx;
    if (t->ctx)
        return TRUE;
    if (t->tried)
        return FALSE;                       /* once a process: not there is not there */
    t->tried = TRUE;
    t->sock = socketbase;                   /* the process's: the socket's calls here */
    if (!socketbase ||
        !(AmiSSLMasterBase = OpenLibrary((CONST_STRPTR)"amisslmaster.library",
                                         AMISSLMASTER_MIN_VERSION))) {
        amce_strlcpy(t->why, "AmiSSL 5 is not installed (amisslmaster.library)", sizeof(t->why));
        return FALSE;
    }
    if (OpenAmiSSLTags(AMISSL_V3xx,
                       AmiSSL_UsesOpenSSLStructs, FALSE,
                       AmiSSL_InitAmiSSL, TRUE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&ssl,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&ext,
                       AmiSSL_SocketBase, (ULONG)socketbase,
                       AmiSSL_ErrNoPtr, (ULONG)&t->err,
                       TAG_DONE) != 0 || !ssl) {
        CloseLibrary(AmiSSLMasterBase);
        amce_strlcpy(t->why, "AmiSSL could not be opened (AmiSSL 5 needed)", sizeof(t->why));
        return FALSE;
    }
    t->master = AmiSSLMasterBase;
    t->ssl = ssl;
    t->ext = ext;
    if (!own(t, TRUE) || !(ctx = SSL_CTX_new(TLS_client_method()))) {
        own(t, FALSE);
        CloseAmiSSL();
        CloseLibrary(AmiSSLMasterBase);
        t->master = t->ssl = t->ext = NULL;
        amce_strlcpy(t->why, "AmiSSL could not make a TLS context", sizeof(t->why));
        return FALSE;
    }
    ERR_clear_error();                      /* (SSL_get_error needs it empty) */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    t->ctx = ctx;
    {
        int before = RAND_status();
        char m[80];
        seed(t);
        sprintf(m, "random numbers %s before seeding, %s after",
                before == 1 ? "ready" : "not ready", RAND_status() == 1 ? "ready" : "not ready");
        amce_log("https", m);
        ERR_clear_error();
    }
    sprintf(t->ver, "%.40s", OpenSSL_version(OPENSSL_VERSION));
    t->why[0] = 0;
    amce_log("https: AmiSSL opened", t->ver);
    return TRUE;
}

void tls_close(struct TlsLib *t)
{
    struct Library *AmiSSLMasterBase = t->master;
    BASES(t);
    if (t->ctx) {
        SSL_CTX_free((SSL_CTX *)t->ctx);
        t->ctx = NULL;
        CloseAmiSSL();
        CloseLibrary(AmiSSLMasterBase);
        own(t, FALSE);
    }
    t->master = t->ssl = t->ext = NULL;
    t->tried = FALSE;
}

/* A connection: AmiSSL's end of it (two memory BIOs) and the socket's,
 * with what is still to be sent. */
#define OUTBUF 16384
struct TlsConn {
    SSL *ssl;
    BIO *rb, *wb;                       /* AmiSSL reads rb, writes wb */
    LONG sock;
    BOOL eof;                           /* the other end closed */
    BOOL starved;                       /* what came is not a whole record: wait for more */
    BOOL failed;                        /* the socket failed (not only closed) */
    ULONG out_len, out_pos;
    UBYTE out[OUTBUF];                  /* from wb, not sent yet */
    UBYTE in[4096];
};

/* ---- the root certificates */
static char certs_path[256];            /* set by the main task, read here */

void tls_set_certs(const char *path)
{
    amce_strlcpy(certs_path, path ? path : "", sizeof(certs_path));
}

/* A file whole into memory (AllocVec'd, 0 after it): NULL if not there. */
static char *read_whole(const char *path, ULONG *len)
{
    BPTR fh;
    LONG size, got;
    char *b = NULL;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    fh = path[0] ? Open((CONST_STRPTR)path, MODE_OLDFILE) : 0;
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return NULL;
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size > 0 && size < 2L << 20 && (b = AllocVec(size + 1, MEMF_ANY))) {
        got = Read(fh, b, size);
        if (got != size) {
            FreeVec(b);
            b = NULL;
        } else {
            b[size] = 0;
            *len = (ULONG)size;
        }
    }
    Close(fh);
    return b;
}

/* The root certificates into the context's store, once: TRUE if there
 * are some. */
static BOOL certs_load(struct TlsLib *t)
{
    char m[200], *pem;
    ULONG len = 0, t0 = time_ms();
    LONG n = 0;
    BASES(t);
    if (t->certs)
        return t->certs > 0;
    t->certs = -1;
    if ((pem = read_whole(certs_path, &len))) {
        BIO *b = BIO_new_mem_buf(pem, (int)len);
        X509_STORE *st = SSL_CTX_get_cert_store((SSL_CTX *)t->ctx);
        X509 *x;
        while (b && st && (x = PEM_read_bio_X509(b, NULL, NULL, NULL))) {
            if (X509_STORE_add_cert(st, x))
                n++;
            X509_free(x);
        }
        if (b)
            BIO_free(b);
        FreeVec(pem);
        ERR_clear_error();                  /* (the end of the file: "no start line") */
    }
    if (n > 0) {
        sprintf(m, "%ld root certificates from %.120s in %ld ms", (long)n, certs_path,
                (long)(time_ms() - t0));
        amce_log("https", m);
        t->certs = 1;
        return TRUE;
    }
    {
        /* AmigaMCE's not there: AmiSSL's, if its assign is */
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR oldwin = me->pr_WindowPtr;
        BPTR l;
        me->pr_WindowPtr = (APTR)-1;
        l = Lock((CONST_STRPTR)"AmiSSL:Certs", SHARED_LOCK);
        me->pr_WindowPtr = oldwin;
        if (l) {
            UnLock(l);
            if (SSL_CTX_set_default_verify_paths((SSL_CTX *)t->ctx)) {
                ERR_clear_error();
                amce_log("https", "root certificates: AmiSSL's (AmiSSL:Certs)");
                t->certs = 1;
                return TRUE;
            }
            ERR_clear_error();
        }
    }
    sprintf(m, "no root certificates: %.120s not read, no AmiSSL:Certs",
            certs_path[0] ? certs_path : "Certs/cacert.pem");
    amce_log("https", m);
    return FALSE;
}

/* ---- the time a certificate is checked at
 * A certificate is valid between two dates, checked against the Amiga's
 * clock. One that has no battery clock, or whose emulator does not set it,
 * starts in 1978, and every certificate is "not yet valid". AmigaMCE was
 * built on AMCE_DATE, so now is no earlier than that: if the clock says
 * otherwise, the check is made as of that day (the certificate still
 * checked against the root certificates, its name against the host's). */
static ULONG days_from_civil(LONG y, LONG m, LONG d)    /* days since 1.1.1970 */
{
    LONG era, yoe, doy, doe;
    y -= m <= 2;
    era = y / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (ULONG)(era * 146097 + doe - 719468);
}

static ULONG build_secs(void)          /* AMCE_DATE ("12.11.2026"), seconds since 1970 */
{
    const char *p = AMCE_DATE;
    LONG v[3] = { 0, 0, 0 };
    int i;
    for (i = 0; i < 3 && *p; i++) {
        while (*p >= '0' && *p <= '9')
            v[i] = v[i] * 10 + (*p++ - '0');
        if (*p == '.')
            p++;
    }
    if (v[2] < 2000 || v[1] < 1 || v[1] > 12 || v[0] < 1)
        return 0;
    return days_from_civil(v[2], v[1], v[0]) * 86400UL;
}

static ULONG amiga_secs(void)          /* the clock, seconds since 1970 */
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ((ULONG)ds.ds_Days + 2922UL) * 86400UL + (ULONG)ds.ds_Minute * 60UL +
           (ULONG)ds.ds_Tick / TICKS_PER_SECOND;
}

APTR tls_new(struct TlsLib *t, LONG sock, const char *host, BOOL verify)
{
    struct TlsConn *c;
    BASES(t);
    t->why[0] = 0;
    if (verify && t->ctx && !certs_load(t)) {
        amce_strlcpy(t->why, "no root certificates to check the server's with", sizeof(t->why));
        return NULL;
    }
    if (!t->ctx || !(c = AllocVec(sizeof(*c), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    c->sock = sock;
    if (!(c->ssl = SSL_new((SSL_CTX *)t->ctx)) || !(c->rb = BIO_new(BIO_s_mem())) ||
        !(c->wb = BIO_new(BIO_s_mem()))) {
        if (c->rb)
            BIO_free(c->rb);
        if (c->ssl)
            SSL_free(c->ssl);
        FreeVec(c);
        return NULL;
    }
    BIO_set_mem_eof_return(c->rb, -1);      /* empty: "would wait", not the end */
    SSL_set_bio(c->ssl, c->rb, c->wb);      /* the SSL owns them now */
    SSL_set_tlsext_host_name(c->ssl, (char *)host);    /* SNI: one address, many sites */
    if (verify) {
        ULONG now = amiga_secs(), built = build_secs();
        SSL_set1_host(c->ssl, host);        /* the certificate must be the host's */
        SSL_set_verify(c->ssl, SSL_VERIFY_PEER, NULL);
        if (built && now < built) {
            static BOOL said;
            X509_VERIFY_PARAM_set_time(SSL_get0_param(c->ssl), (time_t)built);
            if (!said) {
                char m[160];
                said = TRUE;
                sprintf(m, "the Amiga's clock says day %lu after 1.1.1970, before AmigaMCE was "
                           "built (" AMCE_DATE "): certificates checked as of then",
                        (unsigned long)(now / 86400UL));
                amce_log("https", m);
            }
        }
    } else {
        SSL_set_verify(c->ssl, SSL_VERIFY_NONE, NULL);
    }
    SSL_set_connect_state(c->ssl);
    return c;
}

/* What AmiSSL has written, onto the socket: TRUE all of it sent, FALSE
 * some left (the socket full: wait to write); t->why set and c->eof if
 * the socket failed. */
static BOOL flush_out(struct TlsLib *t, struct TlsConn *c)
{
    struct Library *SocketBase = t->sock;
    BASES(t);
    for (;;) {
        LONG w;
        if (c->out_pos == c->out_len) {
            int n = BIO_read(c->wb, c->out, OUTBUF);
            c->out_pos = c->out_len = 0;
            if (n <= 0)
                return TRUE;                /* nothing more to send */
            c->out_len = (ULONG)n;
        }
        w = send(c->sock, (APTR)(c->out + c->out_pos), (LONG)(c->out_len - c->out_pos), 0);
        if (w < 0) {
            LONG e = Errno();
            if (e == EWOULDBLOCK || e == EINTR)
                return FALSE;
            sprintf(t->why, "the connection failed (sending: errno %ld)", (long)e);
            c->eof = c->failed = TRUE;
            return FALSE;
        }
        c->out_pos += (ULONG)w;
    }
}

/* What the socket has, into AmiSSL: 1 some came, 0 none yet (wait to
 * read), -1 the other end closed or the socket failed. */
static int fill_in(struct TlsLib *t, struct TlsConn *c)
{
    struct Library *SocketBase = t->sock;
    LONG r;
    BASES(t);
    if (c->eof)
        return -1;
    r = recv(c->sock, c->in, sizeof(c->in), 0);
    if (r > 0) {
        BIO_write(c->rb, c->in, (int)r);
        c->starved = FALSE;
        return 1;
    }
    if (r < 0) {
        LONG e = Errno();
        if (e == EWOULDBLOCK || e == EINTR)
            return 0;
        sprintf(t->why, "the connection failed (receiving: errno %ld)", (long)e);
        c->failed = TRUE;
    }
    c->eof = TRUE;
    BIO_set_mem_eof_return(c->rb, 0);       /* drained: the end */
    return -1;
}

/* What an SSL call's result means: n (> 0), 0 the other end closed,
 * TLS_WANT_*, or -1 failed (why kept). */
static LONG result(struct TlsLib *t, SSL *s, int r)
{
    int e;
    BASES(t);
    if (r > 0)
        return r;
    e = SSL_get_error(s, r);
    if (e == SSL_ERROR_WANT_READ)
        return TLS_WANT_READ;
    if (e == SSL_ERROR_WANT_WRITE)
        return TLS_WANT_WRITE;
    if (e == SSL_ERROR_ZERO_RETURN)
        return 0;
    if (e == SSL_ERROR_SYSCALL && !ERR_peek_error())
        return 0;                           /* closed without a goodbye: many servers */
    {
        long v = SSL_get_verify_result(s);
        const char *file = NULL, *data = NULL;
        int line = 0, flags = 0;
        unsigned long q = ERR_get_error_line_data(&file, &line, &data, &flags);
        if (q && ERR_GET_REASON(q) == SSL_R_CERTIFICATE_VERIFY_FAILED && v != X509_V_OK)
            sprintf(t->why, "certificate: %.80s", X509_verify_cert_error_string(v));
        else if (q) {
            char w[100];
            ERR_error_string_n(q, w, sizeof(w));
            /* where in OpenSSL it came from, for the log */
            sprintf(t->why, "%.70s (%.24s:%ld%s%.16s)", w, file ? (const char *)FilePart((STRPTR)file) : "?",
                    (long)line, data && (flags & ERR_TXT_STRING) ? " " : "",
                    data && (flags & ERR_TXT_STRING) ? data : "");
        } else
            sprintf(t->why, "TLS error %ld", (long)e);
        ERR_clear_error();
    }
    return -1;
}

/* One SSL call (op: 0 handshake, 1 read, 2 write), the bytes moved for it
 * until it is done or the socket would have to be waited for. */
static LONG step(struct TlsLib *t, struct TlsConn *c, int op, APTR buf, LONG n)
{
    BOOL drained = FALSE;
    BASES(t);
    for (;;) {
        LONG r;
        if (!flush_out(t, c)) {
            if (c->eof)
                return -1;
            return TLS_WANT_WRITE;          /* the socket full: what is pending first */
        }
        ERR_clear_error();                  /* (SSL_get_error: only this call's) */
        r = op == 0 ? SSL_connect(c->ssl) : op == 1 ? SSL_read(c->ssl, buf, (int)n)
                    : SSL_write(c->ssl, buf, (int)n);
        r = result(t, c->ssl, (int)r);
        if (!flush_out(t, c) && c->eof)
            return -1;                      /* (its answer, if any, out first) */
        if (r != TLS_WANT_READ && r != TLS_WANT_WRITE)
            return r;
        if (r == TLS_WANT_WRITE)
            continue;                       /* (memory: never full, but flushed) */
        switch (fill_in(t, c)) {
        case 1:
            continue;                       /* more came: the call again */
        case 0:
            c->starved = TRUE;              /* (tls_pending: nothing to be had now) */
            return c->out_pos < c->out_len ? TLS_WANT_WRITE : TLS_WANT_READ;
        default:
            if (c->failed)
                return -1;
            if (!drained) {
                drained = TRUE;             /* closed: what came before it, read */
                continue;
            }
            return 0;
        }
    }
}

int tls_handshake(struct TlsLib *t, APTR c)
{
    LONG r;
    t->why[0] = 0;
    r = step(t, (struct TlsConn *)c, 0, NULL, 0);
    if (r == 0 && !t->why[0])
        amce_strlcpy(t->why, "the server closed the connection", sizeof(t->why));
    return r > 0 ? 1 : r == 0 ? -1 : (int)r;
}

LONG tls_read(struct TlsLib *t, APTR c, APTR buf, LONG n)
{
    return step(t, (struct TlsConn *)c, 1, buf, n);
}

LONG tls_write(struct TlsLib *t, APTR c, const void *buf, LONG n)
{
    return step(t, (struct TlsConn *)c, 2, (APTR)buf, n);
}

/* Bytes to be had at once: decrypted already, or come and not read yet. */
LONG tls_pending(struct TlsLib *t, APTR c)
{
    struct TlsConn *k = c;
    BASES(t);
    if (!k)
        return 0;
    return SSL_pending(k->ssl) + (k->starved ? 0 : (LONG)BIO_ctrl_pending(k->rb));
}

void tls_free(struct TlsLib *t, APTR c)
{
    struct TlsConn *k = c;
    BASES(t);
    if (!k)
        return;
    SSL_free(k->ssl);                       /* and its BIOs; no goodbye: the socket closes next */
    FreeVec(k);
}
