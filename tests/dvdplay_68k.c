/*
 * dvdplay.c run under vamos on the test DVD's image (tests/mkdvd.sh),
 * through amcedvd.library: the film chosen (the longest title: title 1,
 * two chapters, 4 s), what the player is told of it, its packs read to
 * its end, a seek and the chapters; first what the image holds, as Videos'
 * preview reads it.
 */
#include "../src/amce.h"
#include "../src/dvdplay.h"
#include "../src/dvddisc.h"
#include "../src/dvdiso.h"
#include "../src/vprobe.h"
#include "../src/spu.h"
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <libraries/amcedvd.h>

/* what dvdplay.c uses from the rest of AmigaMCE */
const char *amce_tr(const char *s) { return s; }
void amce_log(const char *what, const char *m) { printf("  log: %s: %s\n", what, m); }
static ULONG fake_ticks;
ULONG time_ticks(ULONG *hz) { if (hz) *hz = 1000; return fake_ticks += 1; }

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
 * with a pack header; *nav: NAV packs among them; mpa, ac3: the packs of
 * each sound track's (MPEG audio C0, AC-3 80) the card was given. */
static LONG mpa, ac3;

static LONG read_all(struct DvdPlay *dp, LONG max, LONG *nav, int *bad)
{
    LONG packs = 0, n, i;
    *nav = 0;
    *bad = 0;
    mpa = ac3 = 0;
    while (packs < max && (n = dvdplay_read(dp, buf, sizeof(buf))) > 0) {
        if (n % 2048)
            (*bad)++;
        for (i = 0; i + 2048 <= n; i += 2048) {
            UBYTE *p = buf + i;
            if (p[0] || p[1] || p[2] != 1 || p[3] != 0xBA)
                (*bad)++;
            else if (p[14 + (p[13] & 7) + 3] == 0xBB && p[38 + 3] == 0xBF)
                (*nav)++;                   /* system header, then private stream 2 */
            else {
                UBYTE *e = p + 14 + (p[13] & 7);
                if (e[3] == 0xC0)
                    mpa++;
                else if (e[3] == 0xBD && e[9 + e[8]] == 0x80)
                    ac3++;
            }
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
    /* what plays: a disc of episodes (C11's lengths), a film with extras,
     * a "play all" beside its parts, the test disc (all under a minute) */
    {
        static const ULONG c11[18] = { 404000, 419000, 431000, 490000, 426000, 453000, 417000,
                                       443000, 445000, 511000, 513000, 403000, 597000, 379000,
                                       20000, 10000, 17000, 9000 };
        static const ULONG film[4] = { 30000, 6120000, 900000, 600000 };
        static const ULONG all[4] = { 1200000, 1300000, 2500000, 15000 };
        static const ULONG tiny[2] = { 4000, 2000 };
        UBYTE l[99];
        ULONG o[99];
        LONG k = dvdplay_choose(c11, 18, l, o);
        CHECK(k == 14 && l[0] == 1 && l[13] == 14 && o[12] == 597000, "episodes: %ld", (long)k);
        k = dvdplay_choose(film, 4, l, o);
        CHECK(k == 1 && l[0] == 2, "film: %ld, title %ld", (long)k, (long)l[0]);
        k = dvdplay_choose(all, 4, l, o);
        CHECK(k == 1 && l[0] == 3, "play all: %ld, title %ld", (long)k, (long)l[0]);
        k = dvdplay_choose(tiny, 2, l, o);
        CHECK(k == 1 && l[0] == 1, "short titles: %ld, title %ld", (long)k, (long)l[0]);
    }
    /* which sound track: stereo first (MPEG, then AC-3), then
     * multichannel (MPEG, then AC-3); of equals the disc's own; none of
     * LPCM, DTS */
    {
        struct DvdStreamInfo t[3];
        memset(t, 0, sizeof(t));
        t[0].present = t[1].present = 1;
        t[0].format = DVDA_AC3, t[0].channels = 6;
        t[1].format = DVDA_MPEG1, t[1].channels = 2;
        CHECK(dvdplay_pick_audio(t, 3, 0) == 1, "MPEG over AC-3");
        t[1].format = DVDA_AC3, t[1].channels = 2;
        CHECK(dvdplay_pick_audio(t, 3, 0) == 1, "AC-3 stereo over 5.1");
        t[0].format = DVDA_MPEG2;
        CHECK(dvdplay_pick_audio(t, 3, 0) == 1, "AC-3 stereo over multichannel MPEG");
        t[0].format = DVDA_AC3;
        t[1].channels = 6;
        CHECK(dvdplay_pick_audio(t, 3, 1) == 1 && dvdplay_pick_audio(t, 3, -1) == 0,
              "of equals, the disc's own, else the first");
        t[0].format = DVDA_LPCM, t[1].format = DVDA_DTS;
        CHECK(dvdplay_pick_audio(t, 3, 0) == -1, "LPCM and DTS: none");
        t[2].format = DVDA_MPEG1, t[2].channels = 2;
        CHECK(dvdplay_pick_audio(t, 3, 0) == -1, "a track not there chosen");
    }
    if (argc < 2) {
        printf("usage: dvdplay_68k image\n");
        return 20;
    }
    /* what an image holds, as Videos' preview asks (under 2 GB: Seek, no
     * 64-bit packets, which vamos would not know) */
    {
        ULONG sectors;
        LONG k = dvdd_image_kind(argv[1], &sectors, err, sizeof(err));
        CHECK(k == DVDI_VIDEO && sectors > 400, "the image: kind %ld, %lu sectors (%s)", (long)k,
              (unsigned long)sectors, k < 0 ? err : "");
        err[0] = 0;
        k = dvdd_image_kind("nothere.iso", &sectors, err, sizeof(err));
        CHECK(k == -1 && err[0] && !sectors, "a missing image: kind %ld", (long)k);
        k = dvdd_image_kind("prog:dvdplay_68k", &sectors, err, sizeof(err));
        CHECK(k == DVDI_NONE, "a program taken for a disc: kind %ld", (long)k);
    }
    CHECK(!dvdplay_open_image("nothere.iso", &v, err, sizeof(err)) && err[0], "a missing image");
    dp = dvdplay_open_image(argv[1], &v, err, sizeof(err));
    CHECK(dp != NULL, "open: %s", err);
    if (!dp)
        goto done;
    printf("%s\n", dvdplay_name(dp));
    CHECK(!strcmp(dvdplay_name(dp), "test"), "name %s (test.iso, without .iso)", dvdplay_name(dp));
    CHECK(v.w == 720 && v.h == 576 && v.aspect == 2 && v.mpeg2 && v.ps && v.fps_milli == 25000,
          "video %ldx%ld aspect %ld", (long)v.w, (long)v.h, (long)v.aspect);
    /* two tracks, AC-3 5.1 first: the MPEG one chosen */
    CHECK(!v.ac3 && v.mp2 && v.audio_id == 0xC0 && v.a_chans == 2, "sound: ac3 %ld mp2 %ld id %lx",
          (long)v.ac3, (long)v.mp2, (unsigned long)v.audio_id);
    CHECK(v.ms >= 3800 && v.ms <= 4200, "length %lu ms", (unsigned long)v.ms);
    CHECK(dvdplay_can_chapter(dp, 1) && dvdplay_can_chapter(dp, -1), "chapters at the start");

    /* the whole film: title 1's two VOBs (162 sectors each), then its end */
    packs = read_all(dp, 100000, &nav, &bad);
    printf("film: %ld packs, %ld NAV\n", (long)packs, (long)nav);
    CHECK(packs >= 300 && packs <= 340, "%ld packs", (long)packs);
    CHECK(nav >= 6, "%ld NAV packs", (long)nav);
    CHECK(!bad, "%d packs not starting with a pack header", bad);
    CHECK(mpa > 10 && ac3 == 0, "sound given: %ld MPEG, %ld AC-3 packs", (long)mpa, (long)ac3);
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
    CHECK(packs >= 145 && packs <= 180 && !bad, "chapter 2 to the end: %ld packs", (long)packs);
    CHECK(!dvdplay_can_chapter(dp, 1), "a chapter after the last");

    /* a seek into the middle, and back a chapter from there */
    CHECK(dvdplay_seek(dp, 1000, &at) && at >= 400 && at <= 1600, "seek 1 s: %lu", (unsigned long)at);
    packs = read_all(dp, 100000, &nav, &bad);
    CHECK(packs >= 200 && packs <= 300 && !bad, "from 1 s: %ld packs", (long)packs);
    CHECK(dvdplay_chapter(dp, -1, &at) && at <= 200, "previous chapter: %lu", (unsigned long)at);

    /* its menus: from its first play to the root menu's still, its two
     * buttons (the first highlighted), right to the second, pressed: title
     * 2 plays, then back to the menu */
    {
        ULONG mb = dvdplay_menus(dp), seq0, seq1, cseq;
        struct SpuHl hl;
        struct DvdNow now;
        LONG still = 0, sub = 0, spu_packs = 0, k, hops0 = 0;
        BOOL user = 0, forced;
        int ev = DVDP_NONE;
        CHECK(mb & DVDMB_TITLE, "menus %lx", (unsigned long)mb);
        CHECK(dvdplay_nav(dp) && dvdplay_is_nav(dp), "from its first play");
        /* the menu's packs, to its still */
        for (k = 0; k < 400 && ((ev = dvdplay_pending(dp, &still, &user)) == DVDP_NONE || ev == DVDP_WAIT); k++) {
            LONG n, i;
            if (ev == DVDP_WAIT) {
                dvdplay_go(dp);             /* (shown what it had: on to the still) */
                continue;
            }
            n = dvdplay_read(dp, buf, sizeof(buf));
            for (i = 0; i + 2048 <= n; i += 2048) {
                UBYTE *e = buf + i + 14 + (buf[i + 13] & 7);
                if (e[3] == 0xBB)
                    e += 6 + ((e[4] << 8) | e[5]);
                if (e[3] == 0xBD && (e[9 + e[8]] & 0xE0) == 0x20)
                    spu_packs++;
            }
        }
        dvdplay_now(dp, &now);
        printf("menu: event %d, still %ld, %ld subpicture packs\n", ev, (long)still, (long)spu_packs);
        CHECK(ev == DVDP_STILL && still == 255, "the menu's still: %d %ld", ev, (long)still);
        CHECK(now.menu && now.buttons && !now.title_ms, "in a menu: %d %d %lu", now.menu, now.buttons,
              (unsigned long)now.title_ms);
        CHECK(spu_packs >= 1, "subpicture packs %ld", (long)spu_packs);
        sub = dvdplay_spu(dp, &forced);
        CHECK(sub == 0, "its subpicture stream %ld", (long)sub);
        dvdplay_clut(dp, &cseq);
        CHECK(cseq >= 1, "no colour table");
        seq0 = dvdplay_highlight(dp, &hl);
        CHECK(hl.on && hl.x0 == 100 && hl.y0 == 200 && hl.x1 >= 318 && hl.x1 <= 320, "button 1: %d %d %d %d",
              hl.on, hl.x0, hl.y0, hl.x1);
        dvdplay_ask(dp, DVDQ_RIGHT, 0, 0);
        dvdplay_poll(dp);
        seq1 = dvdplay_highlight(dp, &hl);
        CHECK(seq1 != seq0 && hl.on && hl.x0 == 400, "button 2: %d", hl.x0);
        CHECK(dvdplay_pending(dp, NULL, NULL) == DVDP_STILL, "still after moving");
        dvdplay_ask(dp, DVDQ_ACTIVATE, 0, 0);
        for (k = 0; k < 5 && (ev = dvdplay_pending(dp, NULL, &user)) == DVDP_STILL; k++)
            dvdplay_poll(dp);
        CHECK(ev == DVDP_HOP && user, "pressed: %d %d", ev, user);
        dvdplay_go(dp);
        /* title 2: its packs, then its end goes back to the menu (the jump
         * acted on when pressed: no second for it) */
        packs = 0;
        for (k = 0; k < 400 && (ev = dvdplay_pending(dp, NULL, NULL)) != DVDP_STILL; k++) {
            LONG n;
            if (ev == DVDP_HOP && !packs)
                hops0++;
            if (ev == DVDP_HOP || ev == DVDP_WAIT) {
                dvdplay_now(dp, &now);
                dvdplay_go(dp);
                continue;
            }
            if (ev == DVDP_END)
                break;
            n = dvdplay_read(dp, buf, sizeof(buf));
            if (n > 0) {
                packs += n / 2048;
                dvdplay_now(dp, &now);
                if (now.title == 2 && !now.menu)
                    mb |= 0x100;            /* (seen playing title 2) */
            }
        }
        printf("title 2: %ld packs, then event %d\n", (long)packs, ev);
        CHECK(mb & 0x100, "title 2 not played");
        CHECK(!hops0, "the press's jump given again: %ld", (long)hops0);
        CHECK(packs >= 140, "title 2: %ld packs", (long)packs);
        CHECK(ev == DVDP_STILL, "back at the menu: %d", ev);
    }
    dvdplay_close(dp);

done:
    printf("%d passed, %d failed\n", passed, failed);
    if (!failed)
        printf("dvdplay tests passed\n");
    return failed ? 20 : 0;
}
