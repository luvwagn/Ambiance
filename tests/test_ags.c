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
    char out[128], tmp[300];
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
        Printf("  [%s] -> %s\n", (ULONG)l.items[i].label, (ULONG)item_path(&l.items[i], tmp, sizeof(tmp)));
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
        check_str("A.ags second path", item_path(&l.items[1], tmp, sizeof(tmp)), "AGS:+  Games - 0-Z.ags/A.ags/Test Game");
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
        check_str("0-Z flat third path", item_path(&l.items[2], tmp, sizeof(tmp)), "AGS:+  Games - 0-Z.ags/A.ags/Test Game");
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

    /* Entries share their folder's path string; copies keep that */
    {
        struct ItemList c;
        list_init(&l);
        list_init(&c);
        n = ags_read_dir("AGS:+  Games - 0-Z.ags/A.ags", &l, FALSE);
        if (n >= 2) {
            check_long("shared folder", l.items[0].path == l.items[1].path, 1);
            list_copy(&c, &l.items[1], "Copy");
            list_copy(&c, &l.items[0], "Copy 2");
            check_str("copy path", item_path(&c.items[0], tmp, sizeof(tmp)),
                      "AGS:+  Games - 0-Z.ags/A.ags/Test Game");
            check_long("copy shares", c.items[0].path == c.items[1].path, 1);
            check_long("copy own pool", c.items[0].path != l.items[0].path, 1);
        }
        list_free(&c);
        list_free(&l);
    }

    /* Lists held a letter at a time: a letter's entries copied as they
     * are, their strings kept in the letter's own (small) pool */
    {
        struct ItemList subs, letter, whole;
        list_init(&subs);
        list_init(&whole);
        n = ags_read_dir("AGS:+  Games - 0-Z.ags", &subs, FALSE);
        check_long("letter menu", ags_letter_menu(&subs), 1);
        for (i = 0; i < subs.count; i++) {
            item_path(&subs.items[i], tmp, sizeof(tmp));
            list_init_puddle(&letter, 2048);
            ags_read_dir(tmp, &letter, FALSE);
            {
                LONG k;
                for (k = 0; k < letter.count; k++)
                    list_append(&whole, &letter.items[k]);
            }
            list_drop_items(&letter);
            check_long("dropped entries", letter.count, 0);
            if (i == 0)
                check_str("appended path", item_path(&whole.items[whole.count - 1], tmp, sizeof(tmp)),
                          "AGS:+  Games - 0-Z.ags/A.ags/Test Game");
            /* the strings outlive the entries (the pool goes with the test) */
        }
        check_long("letters appended", whole.count, 3);
        list_free(&whole);
        list_free(&subs);
        list_init(&subs);
        ags_read_dir("AGS:", &subs, FALSE);
        check_long("not a letter menu", ags_letter_menu(&subs), 0);
        list_free(&subs);
    }

    /* The index cache: written, read back letter by letter */
    {
        struct ItemList a, back;
        struct IdxOut o;
        struct Idx ix;
        struct DateStamp ds;
        LONG got;
        list_init(&a);
        list_init(&back);
        n = ags_read_dir("AGS:+  Games - 0-Z.ags/A.ags", &a, FALSE);
        check_long("idx source", n, 2);
        check_long("idx date", idx_date("AGS:+  Games - 0-Z.ags/A.ags", &ds), 1);
        check_long("idx begin", idx_begin(&o, 2), 1);
        check_long("idx letter A", idx_letter(&o, 0, &ds, "A.ags", a.count, a.items, NULL, 0), 1);
        check_long("idx letter B", idx_letter(&o, 1, &ds, "B.ags", 7, NULL, NULL, 0), 1);
        check_long("idx save", idx_save("T:test.idx", o.buf, o.len), 1);
        idx_abandon(&o);
        check_long("idx load", idx_load("T:test.idx", &ix), 1);
        check_long("idx letters", ix.n, 2);
        if (ix.n == 2) {
            check_str("idx name", ix.l[0].name, "A.ags");
            check_long("idx count A", ix.l[0].count, 2);
            check_long("idx count B", ix.l[1].count, 7);
            check_long("idx B names", ix.l[1].len, 0);
            check_long("idx same date", CompareDates(&ix.l[0].date, &ds), 0);
            got = idx_read_block("T:test.idx", ix.l[0].off, ix.l[0].len,
                                 "AGS:+  Games - 0-Z.ags/A.ags", &back);
            check_long("idx read", got, 2);
            for (i = 0; i < got && i < a.count; i++) {
                check_str("idx label", back.items[i].label, a.items[i].label);
                check_str("idx path", item_path(&back.items[i], tmp, sizeof(tmp)),
                          item_path(&a.items[i], out, sizeof(out)));
                check_long("idx kind", back.items[i].kind, a.items[i].kind);
            }
        }
        idx_free(&ix);
        check_long("idx missing", idx_load("T:none.idx", &ix), 0);
        list_free(&a);
        list_free(&back);
    }

    /* Pictures: folders first, tags, playable / greyed folders; letter
     * folders (A, Mixed/B) merged into their parent's list */
    {
        static const char *want[] = { "Art", "Empty", "Mixed", "Slides", "Agony.png",
                                      "Alien.iff", "Banshee.gif", "NoExtGif", "Top.JPG" };
        char first[160];
        LONG nd;
        list_init(&l);
        n = pics_read_dir("Images:", &l, FALSE);
        check_long("pics count", n, 9);
        for (i = 0; i < l.count && i < 9; i++)
            check_str("pics order", l.items[i].label, want[i]);
        if (l.count == 9) {
            check_long("pics Art opens", l.items[0].mark, MARK_ARROW);
            check_long("pics Empty greyed", l.items[1].flags & ITEM_DISABLED, ITEM_DISABLED);
            check_long("pics Mixed opens", l.items[2].mark, MARK_ARROW);
            check_long("pics Mixed not greyed", l.items[2].flags & ITEM_DISABLED, 0);
            check_long("pics Slides playable", l.items[3].mark, MARK_PLAY);
            check_str("pics Slides count", l.items[3].sortkey, "2");
            check_str("pics merged path", l.items[4].path, "Images:A/Agony.png");
            check_str("pics sniffed tag", l.items[7].sortkey, "GIF");
            check_long("pics file kind", l.items[8].kind, IT_PIC);
            check_str("pics file tag", l.items[8].sortkey, "JPEG");
            check_str("pics file path", l.items[8].path, "Images:Top.JPG");
        }
        list_free(&l);
        list_init(&l);
        n = pics_read_dir("Images:Mixed", &l, FALSE);
        check_long("pics Mixed entries", n, 3);
        if (n == 3) {
            check_str("pics Mixed sub", l.items[0].label, "Sub");
            check_str("pics Mixed merged", l.items[1].label, "Bee.gif");
            check_str("pics Mixed own", l.items[2].label, "one.lbm");
        }
        list_free(&l);
        n = pics_scan("Images:Mixed", &nd, first, sizeof(first));
        check_long("scan Mixed pics", n, 2);
        check_long("scan Mixed dirs", nd, 1);
        check_str("scan Mixed first", first, "Images:Mixed/B/Bee.gif");
        check_long("scan root pics", pics_scan("Images:", &nd, NULL, 0), 5);
        check_long("scan root dirs", nd, 4);
        check_long("scan missing", pics_scan("Images:Nope", &nd, NULL, 0), -1);
        list_init(&l);
        check_long("missing volume", pics_read_dir("NoSuchVolume:Images", &l, FALSE), -1);
        list_free(&l);
    }

    /* Videos: animations and folders only; an ANIM by a picture's name
     * (.iff) looked inside; a folder of pictures alone greyed. Read as
     * Pictures, a .iff is not opened (found when it is opened). */
    {
        list_init(&l);
        n = pics_read_dir("Videos:", &l, TRUE);
        check_long("vids count", n, 3);
        if (n == 3) {
            check_str("vids folder", l.items[0].label, "Clips");
            check_str("vids folder count", l.items[0].sortkey, "1");
            check_long("vids folder open", l.items[0].flags & ITEM_DISABLED, 0);
            check_str("vids pictures folder", l.items[1].label, "Logo");
            check_long("vids pictures folder greyed", l.items[1].flags & ITEM_DISABLED, ITEM_DISABLED);
            check_str("vids anim", l.items[2].label, "Spin.iff");
            check_long("vids anim kind", l.items[2].kind, IT_ANIM);
            check_str("vids anim tag", l.items[2].sortkey, "ANIM");
        }
        list_free(&l);
        list_init(&l);
        n = pics_read_dir("Videos:", &l, FALSE);
        check_long("pics of Videos count", n, 4);
        if (n == 4)
            check_long("pics .iff not opened", l.items[2].kind, IT_PIC);
        list_free(&l);
        check_long("is anim", pics_is_anim("Videos:Spin.iff"), TRUE);
        check_long("is not anim", pics_is_anim("Videos:Still.iff"), FALSE);
    }

    Printf(failures ? "%ld FAILED\n" : "all tests passed\n", (LONG)failures);
    return failures ? 10 : 0;
}
