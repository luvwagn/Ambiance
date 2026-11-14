/*
 * dvdplay.c run under vamos on the test DVD's image (tests/mkdvd.sh),
 * through amcedvd.library: the film chosen (the longest title: title 1,
 * two chapters, 4 s), what the player is told of it, its packs read to
 * its end, a seek and the chapters.
 */
#include "../src/amce.h"
#include "../src/dvdplay.h"
#include "../src/vprobe.h"
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>

/* what dvdplay.c uses from the rest of AmigaMCE */
const char *amce_tr(const char *s) { return s; }
void amce_log(const char *what, const char *m) { printf("  log: %s: %s\n", what, m); }

static int failed, passed;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (c)                                          \
            passed++;                                   \
        else {                                          \
            failed++;                                   \
            printf("FAIL line %d: ", __LINE__);         \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static UBYTE buf[32768];

/* Packs to the end (or max of them): their count, each checked to begin
 * with a pack header; *nav: NAV packs among them. */
static LONG read_all(struct DvdPlay *dp, LONG max, LONG *nav, int *bad)
{
    LONG packs = 0, n, i;
    *nav = 0;
    *bad = 0;
    while (packs < max && (n = dvdplay_read(dp, buf, sizeof(buf))) > 0) {
        if (n % 2048)
            (*bad)++;
        for (i = 0; i + 2048 <= n; i += 2048) {
            UBYTE *p = buf + i;
            if (p[0] || p[1] || p[2] != 1 || p[3] != 0xBA)
                (*bad)++;
            else if (p[14 + (p[13] & 7) + 3] == 0xBB && p[38 + 3] == 0xBF)
                (*nav)++;                   /* system header, then private stream 2 */
            packs++;
        }
    }
    return n < 0 ? -1 : packs;
}

int main(int argc, char **argv)
{
    struct VProbe v;
    struct DvdPlay *dp;
    char err[200];
    LONG packs, nav;
    ULONG at;
    int bad;
    if (argc < 2) {
        printf("usage: dvdplay_68k image\n");
        return 20;
    }
    CHECK(!dvdplay_open_image("nothere.iso", &v, err, sizeof(err)) && err[0], "a missing image");
    dp = dvdplay_open_image(argv[1], &v, err, sizeof(err));
    CHECK(dp != NULL, "open: %s", err);
    if (!dp)
        goto done;
    printf("%s\n", dvdplay_name(dp));
    CHECK(!strcmp(dvdplay_name(dp), "test.iso: title 1"), "name %s", dvdplay_name(dp));
    CHECK(v.w == 720 && v.h == 576 && v.aspect == 2 && v.mpeg2 && v.ps && v.fps_milli == 25000,
          "video %ldx%ld aspect %ld", (long)v.w, (long)v.h, (long)v.aspect);
    CHECK(v.ac3 && !v.mp2 && !v.a_other, "sound: ac3 %ld mp2 %ld", (long)v.ac3, (long)v.mp2);
    CHECK(v.ms >= 3800 && v.ms <= 4200, "length %lu ms", (unsigned long)v.ms);
    CHECK(dvdplay_can_chapter(dp, 1) && dvdplay_can_chapter(dp, -1), "chapters at the start");

    /* the whole film: title 1's two VOBs (138 sectors each), then its end */
    packs = read_all(dp, 100000, &nav, &bad);
    printf("film: %ld packs, %ld NAV\n", (long)packs, (long)nav);
    CHECK(packs >= 260 && packs <= 290, "%ld packs", (long)packs);
    CHECK(nav >= 6, "%ld NAV packs", (long)nav);
    CHECK(!bad, "%d packs not starting with a pack header", bad);
    CHECK(dvdplay_read(dp, buf, sizeof(buf)) == 0, "more after the end");
    CHECK(!dvdplay_can_chapter(dp, 1), "a next chapter at the end");

    /* from its start again, then the next chapter: half way */
    CHECK(dvdplay_seek(dp, 0, &at) && at == 0, "seek 0: %lu", (unsigned long)at);
    packs = read_all(dp, 30, &nav, &bad);
    CHECK(packs >= 30 && !bad, "from the start: %ld packs", (long)packs);
    CHECK(dvdplay_chapter(dp, 1, &at), "next chapter");
    printf("chapter 2 at %lu ms\n", (unsigned long)at);
    CHECK(at >= 1800 && at <= 2200, "chapter 2 at %lu ms", (unsigned long)at);
    packs = read_all(dp, 100000, &nav, &bad);
    CHECK(packs >= 120 && packs <= 150 && !bad, "chapter 2 to the end: %ld packs", (long)packs);
    CHECK(!dvdplay_can_chapter(dp, 1), "a chapter after the last");

    /* a seek into the middle, and back a chapter from there */
    CHECK(dvdplay_seek(dp, 1000, &at) && at >= 400 && at <= 1600, "seek 1 s: %lu", (unsigned long)at);
    packs = read_all(dp, 100000, &nav, &bad);
    CHECK(packs >= 180 && packs <= 260 && !bad, "from 1 s: %ld packs", (long)packs);
    CHECK(dvdplay_chapter(dp, -1, &at) && at <= 200, "previous chapter: %lu", (unsigned long)at);
    dvdplay_close(dp);

done:
    printf("%d passed, %d failed\n", passed, failed);
    if (!failed)
        printf("dvdplay tests passed\n");
    return failed ? 20 : 0;
}
