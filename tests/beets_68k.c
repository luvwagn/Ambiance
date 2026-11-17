/*
 * beets.c run under vamos against a beets database (read only), with a
 * MUSIC: holding its files (tests/mkbeets.py): the pages it makes, and
 * the mapping of beets' Windows paths to MUSIC:.
 */
#include "../src/amce.h"
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>

/* what beets.c uses from the rest of Ambiance */
void (*dir_tick)(void);
struct Task *dir_tick_task;
const char *amce_tr(const char *s) { return s; }
void amce_log(const char *what, const char *m) { printf("  log: %s: %s\n", what, m); }
ULONG time_ms(void) { return 0; }
BOOL song_name_ok(const char *name)
{
    const char *d = strrchr(name, '.');
    return d && (!amce_stricmp(d, ".mp3") || !amce_stricmp(d, ".flac"));
}
static struct { char label[200], path[300]; UBYTE kind; } rows[100];
struct Item *list_add(struct ItemList *l, const char *label, const char *path, const char *key,
                      UBYTE kind)
{
    static struct Item it;
    (void)key;
    if (l->count < 100) {
        amce_strlcpy(rows[l->count].label, label, 200);
        amce_strlcpy(rows[l->count].path, path, 300);
        rows[l->count].kind = kind;
    }
    l->count++;
    return &it;
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static LONG page(const char *path)
{
    struct ItemList l;
    LONG n, i;
    memset(&l, 0, sizeof(l));
    n = beets_read(path, &l);
    printf("%s: %ld%s%s\n", path, (long)n, n < 0 ? ", " : "", n < 0 ? beets_error() : "");
    for (i = 0; i < l.count && i < 4; i++)
        printf("  %s  [%s]\n", rows[i].label, rows[i].path);
    return n;
}

int main(void)
{
    LONG n, k;
    CHECK(beets_there());
    CHECK(page("beets:") == 3);
    n = page("beets:artists");
    CHECK(n == 5);                          /* ABBA and abba: one artist */
    CHECK(!strcmp(rows[0].label, "*NSYNC") && !strcmp(rows[1].label, "ABBA"));
    CHECK(!strcmp(rows[2].label, "Beyonc\xe9") && !strcmp(rows[3].label, "Various Artists"));
    CHECK((UBYTE)rows[4].label[0] == 0x80);  /* Greek last: cjk.c's two bytes */
    n = page("beets:artist/ABBA");
    CHECK(n == 2 && !strcmp(rows[0].label, "Waterloo (1974)") &&
          !strcmp(rows[1].label, "Arrival (1976)") && !strcmp(rows[1].path, "beets:album/5"));
    n = page("beets:album/1");              /* the WMA left out; other artists named */
    CHECK(n == 2 && rows[0].kind == IT_SONG && !strcmp(rows[0].label, "Sun - Alpha") &&
          !strcmp(rows[0].path, "MUSIC:Various Artists/Summer Hits/01 Sun.mp3") &&
          !strcmp(rows[1].label, "Stars - Gamma"));
    n = page("beets:album/2");              /* * as beets wrote it (_), FLAC kept */
    CHECK(n == 2 && !strcmp(rows[1].path, "MUSIC:_NSYNC/No Strings Attached/02 It's Gonna Be Me.flac"));
    n = page("beets:album/3");
    CHECK(n == 1 && !strcmp(rows[0].path, "MUSIC:Beyonc\xe9/Dangerously in Love/01 Crazy in Love.mp3"));
    n = page("beets:album/4");              /* Greek file names: not reachable */
    CHECK(n == 1 && rows[0].kind == IT_TEXT);
    n = page("beets:albums");
    CHECK(n == 6 && !strcmp(rows[0].label, "Arrival - abba") &&
          !strcmp(rows[4].label, "Waterloo - ABBA"));
    n = page("beets:recent");
    CHECK(n == 6 && !strcmp(rows[0].label, "Waterloo - ABBA") && !strcmp(rows[5].label, "Summer Hits - Various Artists"));
    beets_forget();
    n = page("beets:artists");              /* again: from the cache file */
    CHECK(n == 5 && !strcmp(rows[1].label, "ABBA"));
    k = page("beets:nothing");
    CHECK(k < 0);
    beets_forget();
    if (!fails)
        printf("beets tests passed\n");
    return fails != 0;
}
