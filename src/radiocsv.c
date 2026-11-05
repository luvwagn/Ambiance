/*
 * AmigaMCE - radio-browser.info's answers: CSV, UTF-8, URL encoding
 * Copyright (c) 2026 Brian King
 *
 * Plain C and nothing of the Amiga's, so the tests build it on the host
 * (tests/radio_host.c). The API answers in CSV when asked for it (one row
 * a station, the first row the column names, RFC 4180 quoting): far less
 * to parse than JSON on a 68000. Names come in UTF-8; the screen's fonts
 * are Latin-1, so they are converted (what Latin-1 lacks becomes '?', and
 * a few common punctuation marks their nearest ASCII).
 */
#include "radiocsv.h"

#include <string.h>

/* Every row of buf (len bytes, changed in place: fields end in 0) to
 * row(ctx, fields, n); it returns 0 to stop. Rows with more than
 * CSV_MAX_FIELDS fields keep the first ones. Returns the rows seen. */
long csv_parse(char *buf, unsigned long len, int (*row)(void *ctx, char **f, int n), void *ctx)
{
    char *f[CSV_MAX_FIELDS];
    char *p = buf, *end = buf + len, *o;
    long rows = 0;
    while (p < end) {
        int n = 0;
        BOOLISH last = 0;
        if (*p == '\r' || *p == '\n') {     /* an empty line */
            p++;
            continue;
        }
        while (!last) {
            char *start = p;
            if (p < end && *p == '"') {
                /* quoted: "" is a quote, and commas and line ends are text */
                o = p;
                start = p;
                p++;
                while (p < end) {
                    if (*p == '"') {
                        if (p + 1 < end && p[1] == '"') {
                            *o++ = '"';
                            p += 2;
                            continue;
                        }
                        p++;
                        break;
                    }
                    *o++ = *p++;
                }
                while (p < end && *p != ',' && *p != '\n' && *p != '\r')
                    p++;                    /* anything after the closing quote */
                if (p >= end || *p != ',')
                    last = 1;
                if (p < end)
                    p++;
                if (last && p < end && p[-1] == '\r' && *p == '\n')
                    p++;
                *o = 0;
            } else {
                while (p < end && *p != ',' && *p != '\n' && *p != '\r')
                    p++;
                if (p >= end || *p != ',')
                    last = 1;
                if (p < end) {
                    if (*p == '\r' && p + 1 < end && p[1] == '\n')
                        *p++ = 0;
                    *p++ = 0;
                } else {
                    *end = 0;               /* the caller left a byte for it */
                }
            }
            if (n < CSV_MAX_FIELDS)
                f[n++] = start;
        }
        rows++;
        if (!row(ctx, f, n))
            break;
    }
    return rows;
}

/* The column called name in the header row, or -1. */
int csv_col(char **hdr, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(hdr[i], name))
            return i;
    return -1;
}

/* UTF-8 to Latin-1, in place (it only gets shorter). */
void utf8_to_latin1(char *s)
{
    unsigned char *p = (unsigned char *)s, *o = (unsigned char *)s;
    while (*p) {
        unsigned long c = *p;
        int more = 0;
        if (c < 0x80) {
            *o++ = *p++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            c &= 0x1F;
            more = 1;
        } else if ((c & 0xF0) == 0xE0) {
            c &= 0x0F;
            more = 2;
        } else if ((c & 0xF8) == 0xF0) {
            c &= 0x07;
            more = 3;
        } else {
            *o++ = (unsigned char)c;        /* not UTF-8 after all: as it is */
            p++;
            continue;
        }
        p++;
        while (more && (*p & 0xC0) == 0x80) {
            c = (c << 6) | (*p++ & 0x3F);
            more--;
        }
        if (more) {
            *o++ = '?';
            continue;
        }
        if (c >= 0xA0 && c <= 0xFF)
            *o++ = (unsigned char)c;
        else if (c == 0x2018 || c == 0x2019 || c == 0x2032)
            *o++ = '\'';
        else if (c == 0x201C || c == 0x201D || c == 0x2033)
            *o++ = '"';
        else if (c == 0x2013 || c == 0x2014 || c == 0x2212)
            *o++ = '-';
        else if (c == 0x2026) {
            *o++ = '.';                     /* fits: the source was 3 bytes */
            *o++ = '.';
            *o++ = '.';
        } else if (c == 0x2022 || c == 0x00B7)
            *o++ = 0xB7;
        else if (c >= 0xFE00 && c <= 0xFE0F)
            ;                               /* variation selectors: nothing */
        else if (c == 0x200B || c == 0x200D || c == 0xFEFF)
            ;                               /* zero width */
        else if (c < 0x20 || (c >= 0x80 && c < 0xA0))
            *o++ = ' ';
        else
            *o++ = '?';
    }
    *o = 0;
}

/* in, made safe for a URL's query (out max bytes). */
void url_encode(const char *in, char *out, long max)
{
    static const char hex[] = "0123456789ABCDEF";
    long n = 0;
    for (; *in && n < max - 4; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
}

/* Spaces at both ends gone, and runs of them inside made one. */
void squeeze_spaces(char *s)
{
    char *p = s, *o = s;
    int sp = 1;
    for (; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (!sp)
                *o++ = ' ';
            sp = 1;
        } else {
            *o++ = *p;
            sp = 0;
        }
    }
    if (o > s && o[-1] == ' ')
        o--;
    *o = 0;
}
