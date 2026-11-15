/*
 * AmigaMCE - small string helpers (no C library string locale dependence),
 * and the other small helpers more than one part of AmigaMCE uses: kept
 * here once, not copied into each
 * Copyright (c) 2026 Brian King
 */
#include "amce.h"
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <string.h>

#include <stdio.h>

static UBYTE fold(UBYTE c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)   /* Latin-1 upper case */
        return c + 32;
    return c;
}

int amce_stricmp(const char *a, const char *b)
{
    const UBYTE *p = (const UBYTE *)a, *q = (const UBYTE *)b;
    for (;;) {
        UBYTE x = fold(*p++), y = fold(*q++);
        if (x != y)
            return (int)x - (int)y;
        if (!x)
            return 0;
    }
}

int amce_strnicmp(const char *a, const char *b, LONG n)
{
    const UBYTE *p = (const UBYTE *)a, *q = (const UBYTE *)b;
    while (n-- > 0) {
        UBYTE x = fold(*p++), y = fold(*q++);
        if (x != y)
            return (int)x - (int)y;
        if (!x)
            return 0;
    }
    return 0;
}

/* Cut short: a Far East character's three bytes, or a Greek letter's two
 * (cjk.c), are kept whole or left out, never split (a first byte alone
 * would show as a box). */
static void cut_whole(UBYTE *d0, UBYTE *d)
{
    UBYTE *p = d0;
    while (p < d) {
        if (*p >= 0x80 && *p <= 0x9F) {
            ULONG len = *p <= 0x81 ? 2 : 3;
            if (p + len > d) {
                *p = 0;
                return;
            }
            p += len;
        } else {
            p++;
        }
    }
}

void amce_strlcpy(char *d, const char *s, LONG n)
{
    char *d0 = d;
    if (n <= 0)
        return;
    while (--n > 0 && *s)
        *d++ = *s++;
    *d = 0;
    if (*s)
        cut_whole((UBYTE *)d0, (UBYTE *)d);
}

void amce_strlcat(char *d, const char *s, LONG n)
{
    while (n > 0 && *d) {
        d++;
        n--;
    }
    amce_strlcpy(d, s, n);
}

/* A sample rate as kHz, its decimals only as many as it has: 44100
 * "44.1kHz", 22050 "22.05kHz", 48000 "48kHz", 8363 "8.363kHz". */
const char *fmt_khz(ULONG hz, char *out)
{
    ULONG k = hz / 1000, f = hz % 1000;
    if (!f)
        sprintf(out, "%lukHz", (unsigned long)k);
    else if (!(f % 100))
        sprintf(out, "%lu.%lukHz", (unsigned long)k, (unsigned long)(f / 100));
    else if (!(f % 10))
        sprintf(out, "%lu.%02lukHz", (unsigned long)k, (unsigned long)(f / 10));
    else
        sprintf(out, "%lu.%03lukHz", (unsigned long)k, (unsigned long)f);
    return out;
}

BOOL amce_ends_with(const char *s, const char *ext)
{
    LONG a = strlen(s), b = strlen(ext);
    return a > b && amce_stricmp(s + a - b, ext) == 0;
}

BOOL amce_contains_nocase(const char *hay, const char *needle)
{
    const UBYTE *h = (const UBYTE *)hay, *n = (const UBYTE *)needle;
    LONG i, j;
    if (!n[0])
        return FALSE;
    for (i = 0; h[i]; i++) {
        for (j = 0; n[j] && h[i + j] && fold(h[i + j]) == fold(n[j]); j++)
            ;
        if (!n[j])
            return TRUE;
    }
    return FALSE;
}

LONG amce_num(const char *s)
{
    LONG v = 0;
    while (s && *s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

void amce_join_path(char *out, LONG max, const char *dir, const char *name)
{
    LONG n;
    amce_strlcpy(out, dir, max);
    n = strlen(out);
    if (n && out[n - 1] != ':' && out[n - 1] != '/')
        amce_strlcat(out, "/", max);
    amce_strlcat(out, name, max);
}

BOOL amce_exists(const char *path)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR l;
    me->pr_WindowPtr = (APTR)-1;            /* (no "insert volume" requester) */
    l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = old;
    if (l)
        UnLock(l);
    return l != 0;
}

void amce_time_text(char *out, ULONG ms)
{
    ULONG s = ms / 1000;
    sprintf(out, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* The ZZ9000 asked one thing at a time from AmigaMCE's tasks: Picasso96's
 * overlay (opened, moved, closed), the SDK's mpega.library, AHI and MHI
 * set up or fed, zz9k.library's calls. Picasso96 opening an overlay while
 * the background playback process began the sound hung the Amiga. A
 * task may take it again inside (a semaphore nests); none waits for
 * another task while holding it. Before amce_card_init (the tests), a
 * no-op. */
static struct SignalSemaphore card_sem;
static BOOL card_ready;

void amce_card_init(void)
{
    if (!card_ready) {
        InitSemaphore(&card_sem);
        card_ready = TRUE;
    }
}

void amce_card_lock(void)
{
    if (card_ready)
        ObtainSemaphore(&card_sem);
}

void amce_card_unlock(void)
{
    if (card_ready)
        ReleaseSemaphore(&card_sem);
}
