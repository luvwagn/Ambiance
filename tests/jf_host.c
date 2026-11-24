/*
 * jf_host.c - jfjson.c on the host: the six saved answers of Brian's
 * Jellyfin server (tests/fixtures/jellyfin) read item by item and compared
 * with what Python's own JSON decoder reads from them (expected.txt, by
 * expected.py), and the tokenizer's corners: escapes, surrogate pairs, bad
 * JSON, a sign-in's answer, a discovery packet, a walk stopped early.
 * Copyright (c) 2026 Brian King, MIT
 */
#define _POSIX_C_SOURCE 200809L   /* (open_memstream) */
#include "../src/jfjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static char *slurp(const char *path, unsigned long *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    b[n] = 0;
    *len = (unsigned long)n;
    return b;
}

/* a field as expected.txt writes it */
static void esc(FILE *o, const char *s)
{
    for (; *s; s++) {
        if (*s == '\\')
            fputs("\\\\", o);
        else if (*s == '\t')
            fputs("\\t", o);
        else if (*s == '\n')
            fputs("\\n", o);
        else if (*s == '\r')
            fputs("\\r", o);
        else
            fputc(*s, o);
    }
}

static int row(void *ctx, const struct JfItem *it)
{
    FILE *o = ctx;
    const char *f[] = { it->name, it->id, it->type, it->coll, it->media, it->album, it->album_id,
                        it->album_artist, it->artist, it->artist_id, it->year, it->ticks,
                        it->index, it->disc, it->children, it->container, it->image,
                        it->album_image, it->width, it->height, it->date, it->rating,
                        it->overview };
    unsigned i;
    for (i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        esc(o, f[i]);
        fputc('\t', o);
    }
    fprintf(o, "%d\n", it->folder);
    return 1;
}

static int count_only(void *ctx, const struct JfItem *it)
{
    (void)it;
    return ++*(int *)ctx < 3;           /* stop after the third */
}

int main(int argc, char **argv)
{
    static const char *const names[] = { "views", "albums", "artists", "tracks", "movies",
                                          "photos" };
    const char *dir = argc > 1 ? argv[1] : "tests/fixtures/jellyfin";
    char path[512], *exp, *got;
    unsigned long elen, glen;
    FILE *o;
    unsigned i;

    /* the six answers against Python's reading of them */
    {
        size_t olen = 0, rlen = 0;
        char *rows = NULL;
        FILE *all = open_memstream(&got, &olen);
        for (i = 0; i < 6; i++) {
            unsigned long len;
            long total = 0, n;
            char *b;
            sprintf(path, "%s/%s.json", dir, names[i]);
            if (!(b = slurp(path, &len))) {
                printf("FAIL: %s not read\n", path);
                return 1;
            }
            o = open_memstream(&rows, &rlen);
            n = jf_items(b, len, row, o, &total);
            fclose(o);
            fprintf(all, "== %s %ld %ld\n%s", names[i], n, total, rows);
            free(rows);
            rows = NULL;
            free(b);
        }
        fclose(all);
        glen = (unsigned long)olen;
    }
    sprintf(path, "%s/expected.txt", dir);
    if (!(exp = slurp(path, &elen))) {
        printf("FAIL: %s not read\n", path);
        return 1;
    }
    if (glen != elen || memcmp(got, exp, elen)) {
        /* the first line that differs */
        char *a = got, *b = exp;
        int line = 1;
        while (*a && *a == *b) {
            if (*a == '\n')
                line++;
            a++;
            b++;
        }
        printf("FAIL: the answers' items differ from expected.txt at line %d\n", line);
        printf("  got:      %.200s\n  expected: %.200s\n", a, b);
        fails++;
    } else {
        int lines = 0;
        for (i = 0; i < elen; i++)
            lines += exp[i] == '\n';
        printf("jf_host: 6 answers, %d lines, as Python's JSON reads them\n", lines);
    }

    /* escapes and surrogate pairs */
    {
        char b[] = "{\"Items\":[{\"Name\":\"A\\u00e9\\\"\\\\\\/\\n\\uD83C\\uDFB5\\uD800x\\u0000\","
                   "\"Id\":\"1\"}],\"TotalRecordCount\":1}";
        struct JfItem *it;
        static struct JfItem keep;
        long total;
        int got1(void *ctx, const struct JfItem *i2);
        CHECK(jf_items(b, strlen(b), got1, &keep, &total) == 1, "escapes: one item");
        it = &keep;
        CHECK(!strcmp(it->name, "A\xC3\xA9\"\\/\n\xF0\x9F\x8E\xB5?x?"), "escapes decoded");
        CHECK(total == 1, "escapes: total");
    }
    /* bad JSON */
    {
        char b1[] = "{\"Items\":[{\"Name\":\"x\"}";     /* not ended */
        char b2[] = "{\"Items\":[{\"Name\":\"x}]}";       /* a string not ended */
        char b3[] = "<html>no</html>";
        char b4[] = "{\"a\":tru}";
        CHECK(jf_items(b1, strlen(b1), NULL, NULL, NULL) < 0, "bad: not ended");
        CHECK(jf_items(b2, strlen(b2), NULL, NULL, NULL) < 0, "bad: string not ended");
        CHECK(jf_items(b3, strlen(b3), NULL, NULL, NULL) < 0, "bad: HTML");
        CHECK(jf_items(b4, strlen(b4), NULL, NULL, NULL) < 0, "bad: literal");
    }
    /* a sign-in's answer, and dotted values */
    {
        char b[] = "{\"User\":{\"Name\":\"Ambiance\",\"ServerId\":\"s1\",\"Id\":\"u42\","
                   "\"Policy\":{\"IsAdministrator\":false,\"Id\":\"no\"}},"
                   "\"SessionInfo\":{\"Id\":\"sess\"},\"AccessToken\":\"tok123\",\"ServerId\":\"s1\"}";
        char v[64];
        static struct JfItem keep;
        int got1(void *ctx, const struct JfItem *i2);
        CHECK(jf_value(b, strlen(b), "AccessToken", v, sizeof(v)) && !strcmp(v, "tok123"), "token");
        CHECK(jf_value(b, strlen(b), "User.Id", v, sizeof(v)) && !strcmp(v, "u42"), "User.Id");
        CHECK(jf_value(b, strlen(b), "SessionInfo.Id", v, sizeof(v)) && !strcmp(v, "sess"), "dotted");
        CHECK(!jf_value(b, strlen(b), "Nothing", v, sizeof(v)) && !v[0], "not there");
        CHECK(jf_value(b, 8, "AccessToken", v, sizeof(v)) == 0, "cut short");
        /* the answer as it was (jf_value works on a copy) */
        CHECK(strstr(b, "\"AccessToken\":\"tok123\"") != NULL, "jf_value left it");
        CHECK(jf_items(b, strlen(b), got1, &keep, NULL) == 1 && !strcmp(keep.id, "u42") &&
              !strcmp(keep.name, "Ambiance"), "the User as an item");
    }
    /* discovery's answer */
    {
        char b[] = "{\"Address\":\"http://192.168.4.78:8096\",\"Id\":\"3bd1a611\","
                   "\"Name\":\"DS918\",\"EndpointAddress\":null}";
        char a[80], id[40], nm[40];
        CHECK(jf_discovered(b, strlen(b), a, sizeof(a), id, sizeof(id), nm, sizeof(nm)) &&
              !strcmp(a, "http://192.168.4.78:8096") && !strcmp(id, "3bd1a611") &&
              !strcmp(nm, "DS918"), "discovery");
    }
    /* a walk stopped by the item callback */
    {
        unsigned long len;
        int n = 0;
        char *b;
        sprintf(path, "%s/albums.json", dir);
        b = slurp(path, &len);
        CHECK(b && jf_items(b, len, count_only, &n, NULL) == 3 && n == 3, "stopped after 3");
        free(b);
    }
    /* long names cut at a character, not in the middle of one */
    {
        char b[800];
        static struct JfItem keep;
        int got1(void *ctx, const struct JfItem *i2);
        int k;
        strcpy(b, "{\"Items\":[{\"Name\":\"");
        for (k = 0; k < 100; k++)
            strcat(b, "\\u00e9");           /* 200 bytes of UTF-8 */
        strcat(b, "\"}]}");
        CHECK(jf_items(b, strlen(b), got1, &keep, NULL) == 1 && strlen(keep.name) == 158,
              "cut at a character");
    }
    printf(fails ? "jf_host: %d failed\n" : "jf_host: all passed\n", fails);
    free(exp);
    free(got);
    return fails != 0;
}

int got1(void *ctx, const struct JfItem *i2)
{
    memcpy(ctx, i2, sizeof(*i2));
    return 1;
}
