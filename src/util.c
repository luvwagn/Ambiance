/*
 * AmigaMCE - small string helpers (no C library string locale dependence)
 * Copyright (c) 2026 Brian King
 */
#include "amce.h"

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

void amce_strlcpy(char *d, const char *s, LONG n)
{
    if (n <= 0)
        return;
    while (--n > 0 && *s)
        *d++ = *s++;
    *d = 0;
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
