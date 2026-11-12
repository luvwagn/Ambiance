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
 * The sockets are non-blocking, as the plain ones are: a handshake, a read
 * or a write that would wait says so (TLS_WANT_READ, TLS_WANT_WRITE), and
 * the caller waits on the socket as it does already (its timeout, its
 * Stop), then calls again.
 *
 * Certificates are checked (AmiSSL's own, in AmiSSL:Certs) where something
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

BOOL tls_open(struct TlsLib *t, struct Library *socketbase)
{
    struct Library *AmiSSLMasterBase, *ssl = NULL, *ext = NULL;
    SSL_CTX *ctx;
    if (t->ctx)
        return TRUE;
    if (t->tried)
        return FALSE;                       /* once a process: not there is not there */
    t->tried = TRUE;
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
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
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
    SSL_CTX_set_default_verify_paths(ctx);  /* AmiSSL:Certs */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    t->ctx = ctx;
    sprintf(t->ver, "%.40s", OpenSSL_version(OPENSSL_VERSION));
    t->why[0] = 0;
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

APTR tls_new(struct TlsLib *t, LONG sock, const char *host, BOOL verify)
{
    SSL *s;
    BASES(t);
    if (!t->ctx || !(s = SSL_new((SSL_CTX *)t->ctx)))
        return NULL;
    SSL_set_fd(s, (int)sock);
    SSL_set_tlsext_host_name(s, (char *)host);     /* SNI: one address, many sites */
    if (verify) {
        SSL_set1_host(s, host);             /* the certificate must be the host's */
        SSL_set_verify(s, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_set_verify(s, SSL_VERIFY_NONE, NULL);
    }
    SSL_set_connect_state(s);
    return s;
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
        unsigned long q = ERR_get_error();
        if (q && ERR_GET_REASON(q) == SSL_R_CERTIFICATE_VERIFY_FAILED && v != X509_V_OK)
            sprintf(t->why, "certificate: %.80s", X509_verify_cert_error_string(v));
        else if (q)
            ERR_error_string_n(q, t->why, sizeof(t->why));
        else
            sprintf(t->why, "TLS error %ld", (long)e);
    }
    return -1;
}

int tls_handshake(struct TlsLib *t, APTR c)
{
    LONG r;
    BASES(t);
    r = result(t, (SSL *)c, SSL_connect((SSL *)c));
    if (r == 0)
        amce_strlcpy(t->why, "the server closed the connection", sizeof(t->why));
    return r > 0 ? 1 : r == 0 ? -1 : (int)r;
}

LONG tls_read(struct TlsLib *t, APTR c, APTR buf, LONG n)
{
    BASES(t);
    return result(t, (SSL *)c, SSL_read((SSL *)c, buf, (int)n));
}

LONG tls_write(struct TlsLib *t, APTR c, const void *buf, LONG n)
{
    BASES(t);
    return result(t, (SSL *)c, SSL_write((SSL *)c, buf, (int)n));
}

LONG tls_pending(struct TlsLib *t, APTR c)
{
    BASES(t);
    return c ? SSL_pending((SSL *)c) : 0;
}

void tls_free(struct TlsLib *t, APTR c)
{
    BASES(t);
    if (c)
        SSL_free((SSL *)c);                 /* no goodbye sent: the socket closes next */
}
