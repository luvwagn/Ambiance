/* vamos test for Music's own files: songinfo.c's tags and headers, and
 * mods.c's lists (folders without music left out). Run in build/ with
 * Songs: the folder tests/mksongs.py made, RAM: a scratch folder. */
#include "../src/amce.h"
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>

static int fails;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

/* lang.c logs which language it opened: not needed here */
void amce_log(const char *what, const char *detail)
{
    (void)what;
    (void)detail;
}

int main(void)
{
    struct SongInfo si;
    struct ItemList l;
    char art[300];
    LONG i, n;
    BOOL artist = FALSE, empty = FALSE, pics = FALSE, loose = FALSE, band = FALSE;

    songs_can_play(TRUE, TRUE);
    check("mp3 read", song_info("Songs:Artist/Album/01 song.mp3", &si, art, sizeof(art), TRUE));
    printf("  mp3: '%s' '%s' '%s' %s track %s, %lu Hz %lu ch %lu kbit/s %lu ms, art %s\n",
           si.title, si.artist, si.album, si.year, si.track, si.rate, si.channels, si.kbps, si.ms,
           art);
    check("mp3 title", !strcmp(si.title, "Song One"));
    check("mp3 artist (UTF-16)", !strcmp(si.artist, "Art"));
    check("mp3 album (UTF-8)", !strcmp(si.album, "Alb\xe9"));
    check("mp3 year, track", !strcmp(si.year, "1999") && !strcmp(si.track, "3/9"));
    check("mp3 frame", si.rate == 44100 && si.channels == 2);
    check("mp3 Xing length", si.ms == 26122);
    check("folder cover first", strstr(art, "cover.png") != NULL);

    check("mp3 alone read", song_info("Songs:Solo/solo.mp3", &si, art, sizeof(art), TRUE));
    {
        BPTR fh = Open((CONST_STRPTR)art, MODE_OLDFILE);
        UBYTE b[4] = { 0, 0, 0, 0 };
        LONG sz = 0;
        if (fh) {
            Read(fh, b, 4);
            Seek(fh, 0, OFFSET_END);
            sz = Seek(fh, 0, OFFSET_BEGINNING);
            Close(fh);
        }
        printf("  mp3 alone: art %s, %ld bytes\n", art, (long)sz);
        check("APIC copied out", strstr(art, ".png") && b[0] == 0x89 && b[1] == 'P' && sz == 208);
    }
    check("flac read", song_info("Songs:Artist/Other/x.flac", &si, art, sizeof(art), TRUE));
    printf("  flac: '%s' '%s' '%s' %s, %lu Hz %lu ch %lu-bit %lu ms, art %s\n", si.title,
           si.artist, si.album, si.year, si.rate, si.channels, si.bits, si.ms, art);
    check("flac tags", !strcmp(si.title, "Flac Song") && !strcmp(si.artist, "Flac Artist") &&
                       !strcmp(si.album, "Flac Album") && !strcmp(si.year, "2001"));
    check("flac stream", si.rate == 44100 && si.channels == 2 && si.bits == 16 && si.ms == 10000);
    check("flac picture copied out", !strncmp(art, "RAM:AmigaMCE-art/s", 18) &&
                                     strstr(art, ".jpg") != NULL);
    {
        BPTR fh = Open((CONST_STRPTR)art, MODE_OLDFILE);
        UBYTE b[2] = { 0, 0 };
        LONG sz = 0;
        if (fh) {
            Read(fh, b, 2);
            Seek(fh, 0, OFFSET_END);
            sz = Seek(fh, 0, OFFSET_BEGINNING);
            Close(fh);
        }
        check("flac picture's bytes", b[0] == 0xFF && b[1] == 0xD8 && sz == 300);
    }

    check("wav read", song_info("Songs:Loose/tone.wav", &si, NULL, 0, FALSE));
    printf("  wav: %s %lu Hz %lu ch %lu-bit %lu ms\n", si.fmt, si.rate, si.channels, si.bits, si.ms);
    check("wav", !strcmp(si.fmt, "WAV") && si.rate == 44100 && si.channels == 2 && si.bits == 16 &&
                 si.ms == 1000);
    check("aiff read", song_info("Songs:Loose/beep.aiff", &si, NULL, 0, FALSE));
    printf("  aiff: '%s' %lu Hz %lu ch %lu-bit %lu ms\n", si.title, si.rate, si.channels, si.bits,
           si.ms);
    check("aiff", !strcmp(si.fmt, "AIFF") && si.rate == 22050 && si.channels == 1 &&
                  si.bits == 8 && si.ms == 500 && !strcmp(si.title, "Aiff Song"));
    check("8svx read", song_info("Songs:Loose/sample.8svx", &si, NULL, 0, FALSE));
    check("8svx", !strcmp(si.fmt, "8SVX") && si.rate == 8363 && si.ms == 1000 &&
                  !strcmp(si.title, "Sample"));


    /* Kodi's .nfo files (nfo.c) */
    {
        struct Nfo nf;
        char lines[24][96], *text = NULL;
        int k, j;
        check("album.nfo read", nfo_album_of("Songs:Artist/Album", &nf));
        printf("  album.nfo: '%s' by '%s' %s, '%s', genres '%s', styles '%s', rating %s/%d\n",
               nf.title, nf.artist, nf.year, nf.label, nf.genres, nf.styles, nf.rating,
               (int)nf.rating_max);
        printf("  review: '%s'\n", nf.text ? nf.text : "(none)");
        check("album.nfo fields", !strcmp(nf.title, "Copper Lists") &&
              !strcmp(nf.artist, "The Paula Channels") && !strcmp(nf.year, "1993") &&
              !strcmp(nf.label, "Copper Records"));
        check("album.nfo lists", !strcmp(nf.genres, "Synth-pop, Chiptune") &&
              !strcmp(nf.styles, "Chiptune, Electronic"));
        check("album.nfo rating", !strcmp(nf.rating, "8.5") && nf.rating_max == 10);
        check("album.nfo review, paragraphs, entity", nf.text &&
              !strcmp(nf.text, "Their second album, and the one where the limits became the point.\n"
                               "Every song fits in 512K of chip memory & the stereo is hard left and right."));
        check("a track's title not the album's", strcmp(nf.title, "Wait for the Beam") != 0);
        nfo_free(&nf);

        check("artist.nfo read", nfo_artist_of("Songs:Artist/Album", TRUE, &nf));
        printf("  artist.nfo: '%s' %s, formed %s-%s, '%s' '%s'\n", nf.title, nf.type, nf.formed,
               nf.ended, nf.genres, nf.styles);
        printf("  biography: '%s'\n", nf.text ? nf.text : "(none)");
        check("artist.nfo fields", !strcmp(nf.title, "The Paula Channels") &&
              !strcmp(nf.type, "Group") && !strcmp(nf.formed, "1988") && !strcmp(nf.ended, "1996"));
        check("artist.nfo CDATA biography", nf.text &&
              !strcmp(nf.text, "A four-piece from Lancaster.\nThey never used more than four channels <really>."));
        nfo_free(&nf);
        check("artist.nfo from the artist's own folder", nfo_artist_of("Songs:Artist", FALSE, &nf));
        nfo_free(&nf);

        check("disc folders", nfo_disc_folder("CD1") && nfo_disc_folder("CD 2") &&
              nfo_disc_folder("Disc 3 - Live") && nfo_disc_folder("disk2") &&
              !nfo_disc_folder("CDs") && !nfo_disc_folder("Discography") && !nfo_disc_folder("CD"));
        check("album.nfo of a disc's folder", nfo_album_of("Songs:Band/Live/CD2", &nf));
        printf("  disc's album: '%s' %s, rating %s/%d, review '%s'\n", nf.title, nf.year, nf.rating,
               (int)nf.rating_max, nf.text ? nf.text : "");
        check("its fields (UTF-8 to Latin-1, old rating out of 5)",
              !strcmp(nf.title, "Live at the Caf\xe9") && !strcmp(nf.year, "1995") &&
              !strcmp(nf.rating, "4") && nf.rating_max == 5);
        nfo_free(&nf);
        check("no album.nfo", !nfo_album_of("Songs:Loose", &nf));

        check("fanart from the album's folder", nfo_art("Songs:Artist/Album", NFO_ART_FANART, FALSE,
                                                       art, sizeof(art)) &&
              !strcmp(art, "Songs:Artist/fanart.jpg"));
        check("logo from the artist's folder", nfo_art("Songs:Artist", NFO_ART_LOGO, TRUE, art,
                                                      sizeof(art)) &&
              !strcmp(art, "Songs:Artist/logo.png"));
        check("disc art", nfo_art("Songs:Artist/Album", NFO_ART_DISC, FALSE, art, sizeof(art)) &&
              !strcmp(art, "Songs:Artist/Album/discart.png"));
        check("no fanart", !nfo_art("Songs:Band/Live/CD1", NFO_ART_FANART, FALSE, art, sizeof(art)));

        k = song_details("Songs:Artist/Album/01 song.mp3", lines, 24, art, sizeof(art), &text);
        for (j = 0; j < k; j++)
            printf("  song pane: '%s'\n", lines[j]);
        check("a song's pane: label, genres, rating, review", k > 3 &&
              !strcmp(lines[k - 3], "Copper Records") &&
              !strcmp(lines[k - 2], "Synth-pop, Chiptune, Electronic") &&
              !strcmp(lines[k - 1], "Rating 8.5/10") && text != NULL);
        FreeVec(text);
        k = song_details("Songs:Artist/Album", lines, 24, art, sizeof(art), &text);
        for (j = 0; j < k; j++)
            printf("  album pane: '%s'\n", lines[j]);
        check("an album folder's pane", k >= 6 && !strcmp(lines[3], "The Paula Channels") &&
              !strcmp(lines[4], "Copper Lists (1993)") && text != NULL);
        FreeVec(text);
        k = song_details("Songs:Artist", lines, 24, art, sizeof(art), &text);
        for (j = 0; j < k; j++)
            printf("  artist pane: '%s'\n", lines[j]);
        check("an artist folder's pane", k >= 4 && !strcmp(lines[k - 1], "Formed 1988 - 1996") &&
              text != NULL && !strncmp(text, "A four-piece", 12));
        FreeVec(text);
    }

    list_init(&l);
    n = songs_read_dir("Songs:", &l);
    for (i = 0; i < l.count; i++) {
        printf("  list: %s (%d)\n", l.items[i].label, l.items[i].kind);
        artist |= !strcmp(l.items[i].label, "Artist");
        band |= !strcmp(l.items[i].label, "Band");
        loose |= !strcmp(l.items[i].label, "Loose");
        empty |= !strcmp(l.items[i].label, "Empty");
        pics |= !strcmp(l.items[i].label, "Pics");
    }
    check("folders with music listed", n == 4 && artist && loose && band);
    check("folders without music left out", !empty && !pics);
    list_free(&l);
    list_init(&l);
    n = songs_read_dir("Songs:Loose", &l);
    check("wav, aiff, 8svx listed", n == 3);
    list_free(&l);
    songs_can_play(TRUE, FALSE);
    list_init(&l);
    n = songs_read_dir("Songs:Artist", &l);
    check("no flacdec: a folder of FLAC left out", n == 1);
    list_free(&l);
    printf(fails ? "%d FAILED\n" : "all song tests passed\n", fails);
    return fails ? 20 : 0;
}
