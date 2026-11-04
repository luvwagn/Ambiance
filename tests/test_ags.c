/* Host-run unit tests for the AGS reader, executed under vamos. */
#include "../src/amce.h"
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>

static int failures;

static void check_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        Printf("FAIL %s: got [%s] want [%s]\n", (ULONG)what, (ULONG)got, (ULONG)want);
        failures++;
    }
}

static void check_long(const char *what, LONG got, LONG want)
{
    if (got != want) {
        Printf("FAIL %s: got %ld want %ld\n", (ULONG)what, got, want);
        failures++;
    }
}

int main(void)
{
    char out[128];
    struct ItemList l;
    char lines[24][96];
    struct Ags a;
    LONG n, i;

    /* whdlsaves as AGS stores it, and other shapes */
    {
        char u[64];
        ags_unquote_path("\"AGS:WHDSaves/\"", u, sizeof(u));
        check_str("unquote AGS whdlsaves", u, "AGS:WHDSaves");
        ags_unquote_path("Work:Saves", u, sizeof(u));
        check_str("unquote plain", u, "Work:Saves");
        ags_unquote_path("\"WHDLoad:\"", u, sizeof(u));
        check_str("unquote volume", u, "WHDLoad:");
        ags_unquote_path("", u, sizeof(u));
        check_str("unquote empty", u, "");
    }

    ags_clean_name("+  Games - By Genre.ags", out, sizeof(out));
    check_str("clean +", out, "Games - By Genre");
    ags_clean_name("-  Demos - 0-Z.ags", out, sizeof(out));
    check_str("clean -", out, "Demos - 0-Z");
    ags_clean_name("      Welcome To AGS!.ags", out, sizeof(out));
    check_str("clean spaces", out, "Welcome To AGS!");
    ags_clean_name("-- -----------------------.ags", out, sizeof(out));
    check_long("separator --", ags_is_separator(out), 1);
    ags_clean_name(" -------------------------.ags", out, sizeof(out));
    check_long("separator space", ags_is_separator(out), 1);
    ags_clean_name("A-10 Tank Killer 1.5.run", out, sizeof(out));
    check_str("clean run", out, "A-10 Tank Killer 1.5");
    check_long("not separator", ags_is_separator("Games - 0-Z"), 0);

    /* top level, adult hidden */
    list_init(&l);
    n = ags_read_dir("AGS:", &l, FALSE);
    Printf("top level (%ld):\n", n);
    for (i = 0; i < l.count; i++)
        Printf("  [%s] -> %s\n", (ULONG)l.items[i].label, (ULONG)l.items[i].path);
    check_long("top count, adult hidden", n, 8);  /* Welcome and Options are filtered by the menu, not the reader */
    list_free(&l);

    list_init(&l);
    n = ags_read_dir("AGS:", &l, TRUE);
    check_long("top count, adult shown", n, 9);
    list_free(&l);

    /* letter folder: .run entries only, sorted, .rub skipped */
    list_init(&l);
    n = ags_read_dir("AGS:+  Games - 0-Z.ags/A.ags", &l, FALSE);
    check_long("A.ags count", n, 2);
    if (n == 2) {
        check_str("A.ags first", l.items[0].label, "Another");
        check_str("A.ags second path", l.items[1].path, "AGS:+  Games - 0-Z.ags/A.ags/Test Game");
        check_long("A.ags kind", l.items[1].kind, IT_RUN);
    }
    list_free(&l);

    /* All Games: the letter folders as one sorted list */
    list_init(&l);
    n = ags_read_dir_flat("AGS:+  Games - 0-Z.ags", &l, FALSE);
    check_long("0-Z flat count", n, 3);
    if (n == 3) {
        check_str("0-Z flat first", l.items[0].label, "Another");
        check_str("0-Z flat second", l.items[1].label, "Banshee");
        check_str("0-Z flat third path", l.items[2].path, "AGS:+  Games - 0-Z.ags/A.ags/Test Game");
        check_long("0-Z flat kind", l.items[1].kind, IT_RUN);
    }
    list_free(&l);

    /* a menu split A-Z opens flat; one with named folders keeps them */
    list_init(&l);
    n = ags_read_menu("AGS:+  Games - 0-Z.ags", &l, FALSE);
    check_long("A-Z menu flat count", n, 3);
    if (n == 3)
        check_long("A-Z menu flat kind", l.items[0].kind, IT_RUN);
    list_free(&l);
    list_init(&l);
    n = ags_read_menu("AGS:+  Games - By Genre.ags", &l, FALSE);
    check_long("genre menu keeps folders", n, 1);
    if (n == 1) {
        check_str("genre folder", l.items[0].label, "Action FPS");
        check_long("genre folder kind", l.items[0].kind, IT_DIR);
    }
    list_free(&l);

    /* Favourites: option entries in brackets hidden */
    list_init(&l);
    n = ags_read_dir("AGS:+  Favourites.ags", &l, FALSE);
    check_long("favourites hides [options]", n, 0);
    list_free(&l);

    n = ags_read_txt("AGS:+  Games - 0-Z.ags/A.ags/Test Game", lines, 24);
    Printf("txt lines (%ld):\n", n);
    for (i = 0; i < n; i++)
        Printf("  |%s|\n", (ULONG)lines[i]);
    check_long("txt lines", n, 10);
    if (n >= 4) {
        check_str("txt title", lines[0], "Test Game");
        check_str("txt blank collapsed", lines[3], "");
        check_str("txt genre", lines[4], "Genre : Action / Shooter / Horizontal");
    }

    check_long("screenshot -1", ags_screenshot_path("AGS:+  Games - 0-Z.ags/A.ags/Test Game", out, sizeof(out)), 1);
    check_str("screenshot path", out, "AGS:+  Games - 0-Z.ags/A.ags/Test Game-1.iff");
    check_long("no screenshot", ags_screenshot_path("AGS:+  Games - 0-Z.ags/A.ags/Another", out, sizeof(out)), 0);

    memset(&a, 0, sizeof(a));
    ags_write_launch_script(&a, "AGS:+  Games - 0-Z.ags/A.ags/Test Game", "T:launch");
    {
        BPTR fh = Open((CONST_STRPTR)"T:launch", MODE_OLDFILE);
        char buf[512];
        LONG len = fh ? Read(fh, buf, sizeof(buf) - 1) : 0;
        if (fh)
            Close(fh);
        buf[len > 0 ? len : 0] = 0;
        Printf("launch script:\n%s", (ULONG)buf);
        check_long("script sets Expert 0", strstr(buf, "Set Expert 0\n") != NULL, 1);
        check_long("script runs entry", strstr(buf, "Execute \"AGS:+  Games - 0-Z.ags/A.ags/Test Game.run\"") != NULL, 1);
    }
    amce_strlcpy(a.expertperm, "1", sizeof(a.expertperm));
    ags_write_launch_script(&a, "AGS:x", "T:launch2");
    {
        BPTR fh = Open((CONST_STRPTR)"T:launch2", MODE_OLDFILE);
        char buf[512];
        LONG len = fh ? Read(fh, buf, sizeof(buf) - 1) : 0;
        if (fh)
            Close(fh);
        buf[len > 0 ? len : 0] = 0;
        check_long("permanent expert kept", strstr(buf, "Set Expert 0") == NULL, 1);
    }

    /* Pictures: folders first, tags, playable / greyed folders */
    {
        static const char *want[] = { "A", "Art", "Empty", "Mixed", "Top.JPG" };
        char first[64];
        LONG nd;
        list_init(&l);
        n = pics_read_dir("Images:", &l);
        check_long("pics count", n, 5);
        for (i = 0; i < l.count && i < 5; i++)
            check_str("pics order", l.items[i].label, want[i]);
        if (l.count == 5) {
            check_long("pics A playable", l.items[0].mark, MARK_PLAY);
            check_str("pics A count", l.items[0].sortkey, "4");
            check_long("pics Art opens", l.items[1].mark, MARK_ARROW);
            check_long("pics Empty greyed", l.items[2].flags & ITEM_DISABLED, ITEM_DISABLED);
            check_long("pics Mixed opens", l.items[3].mark, MARK_ARROW);
            check_long("pics Mixed not greyed", l.items[3].flags & ITEM_DISABLED, 0);
            check_long("pics file kind", l.items[4].kind, IT_PIC);
            check_str("pics file tag", l.items[4].sortkey, "JPEG");
            check_str("pics file path", l.items[4].path, "Images:Top.JPG");
        }
        list_free(&l);
        list_init(&l);
        n = pics_read_dir("Images:A", &l);
        check_long("pics A entries", n, 4);
        if (n == 4) {
            check_str("pics A first", l.items[0].label, "Agony.png");
            check_str("pics sniffed", l.items[3].label, "NoExtGif");
            check_str("pics sniffed tag", l.items[3].sortkey, "GIF");
            check_str("pics A path", l.items[0].path, "Images:A/Agony.png");
        }
        list_free(&l);
        n = pics_scan("Images:Mixed", &nd, first, sizeof(first));
        check_long("scan Mixed pics", n, 1);
        check_long("scan Mixed dirs", nd, 1);
        check_str("scan Mixed first", first, "one.lbm");
        check_long("scan missing", pics_scan("Images:Nope", &nd, NULL, 0), -1);
        check_long("missing volume", pics_read_dir("NoSuchVolume:Images", &l), -1);
    }

    Printf(failures ? "%ld FAILED\n" : "all tests passed\n", (LONG)failures);
    return failures ? 10 : 0;
}
