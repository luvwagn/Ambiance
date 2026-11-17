/*
 * Ambiance - radio-browser.info's answers: CSV, UTF-8, URL encoding
 * (radiocsv.c). No Amiga headers, so the host tests use it as it is.
 */
#ifndef AMB_RADIOCSV_H
#define AMB_RADIOCSV_H

#define CSV_MAX_FIELDS 40
typedef int BOOLISH;

long csv_parse(char *buf, unsigned long len, int (*row)(void *ctx, char **f, int n), void *ctx);
int csv_col(char **hdr, int n, const char *name);
void utf8_to_latin1(char *s);
void url_encode(const char *in, char *out, long max);
void squeeze_spaces(char *s);

#endif
