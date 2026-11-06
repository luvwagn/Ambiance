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

int main(void)
{
    struct SongInfo si;
    struct ItemList l;
    char art[300];
    LONG i, n;
    BOOL artist = FALSE, empty = FALSE, pics = FALSE, loose = FALSE;

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

    list_init(&l);
    n = songs_read_dir("Songs:", &l);
    for (i = 0; i < l.count; i++) {
        printf("  list: %s (%d)\n", l.items[i].label, l.items[i].kind);
        artist |= !strcmp(l.items[i].label, "Artist");
        loose |= !strcmp(l.items[i].label, "Loose");
        empty |= !strcmp(l.items[i].label, "Empty");
        pics |= !strcmp(l.items[i].label, "Pics");
    }
    check("folders with music listed", n == 3 && artist && loose);
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
