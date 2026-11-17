/* vamos test for igame.c: iGame found by its assign, gameslist.csv read
 * (hidden and gone entries left out, demos apart), its pages, an entry's
 * facts, and the launch script. Run in build/ with iGame: the fixture
 * drawer tests/fixtures/iGame and T: a scratch folder. vamos has no
 * icon.library, so the script is the slave alone (no icon's tooltypes). */
#include "../src/igame.h"
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Library *IconBase;

static int fails;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

void amce_log(const char *what, const char *detail)
{
    printf("  log: %s: %s\n", what, detail);
}

ULONG time_ms(void)
{
    return 0;
}

/* unpack.c's, without the unpacking (the lists are plain text) */
UBYTE *unpack_read_whole(const char *path, ULONG *len)
{
    UBYTE *buf = NULL;
    LONG size;
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return NULL;
    if (Seek(fh, 0, OFFSET_END) >= 0 && (size = Seek(fh, 0, OFFSET_BEGINNING)) > 0 &&
        (buf = AllocVec(size + 16, MEMF_ANY | MEMF_CLEAR))) {
        if (Read(fh, buf, size) != size) {
            FreeVec(buf);
            buf = NULL;
        } else
            *len = (ULONG)size;
    }
    Close(fh);
    return buf;
}

static LONG find(const struct ItemList *l, const char *label)
{
    LONG i;
    for (i = 0; i < l->count; i++)
        if (!strcmp(l->items[i].label, label))
            return i;
    return -1;
}

static void show(const char *what, const struct ItemList *l)
{
    LONG i;
    printf("  %s:", what);
    for (i = 0; i < l->count; i++)
        printf(" [%s]", l->items[i].label);
    printf("\n");
}

int main(void)
{
    struct IgCat c;
    struct ItemList l;
    char dir[256], path[200], lines[24][96], why[100], script[600];
    const struct IgGame *gm;
    BOOL ordered;
    LONG n, i;

    memset(&c, 0, sizeof(c));
    check("found by the iGame: assign", ig_discover(dir, sizeof(dir), FALSE) && !strcmp(dir, "iGame:"));
    check("its drawer has a list", ig_folder_ok(dir));
    check("a drawer without one has none", !ig_folder_ok("iGame:WHD"));

    n = ig_load(&c, dir);
    printf("  %ld entries: %ld games, %ld demos\n", (long)n, (long)c.games, (long)c.demos);
    check("hidden and gone left out", n == 3);
    check("two games, one demo", c.games == 2 && c.demos == 1);

    /* Games' first page */
    list_init(&l);
    ig_page_path(path, sizeof(path), FALSE, "root");
    check("a page's path is iGame's", ig_page(path) && !ig_page("Work:Games"));
    n = ig_read(&c, path, &l, &ordered);
    show("games root", &l);
    check("games root: favourites, all, most played, genre, chipset, year",
          n == 6 && find(&l, "Favourites") == 0 && find(&l, "All games") == 1 &&
          find(&l, "Most played") == 2 && find(&l, "By genre") == 3 &&
          find(&l, "By chipset") == 4 && find(&l, "By year") == 5);
    check("its rows are folders of iGame's", l.items[1].kind == IT_DIR && ig_page(l.items[1].path));
    list_free(&l);

    /* All games, by title; the user's own title used */
    list_init(&l);
    ig_page_path(path, sizeof(path), FALSE, "all");
    n = ig_read(&c, path, &l, &ordered);
    show("all games", &l);
    check("all games: two, in title order", n == 2 && !ordered &&
          !strcmp(l.items[0].label, "Turrican II") && !strcmp(l.items[1].label, "Zool AGA"));
    check("an entry's path is its slave", l.items[0].kind == IT_RUN &&
          !strcmp(l.items[0].path, "iGame:WHD/Games/Turrican2/Turrican2.slave"));
    list_free(&l);

    /* Most played first */
    list_init(&l);
    ig_page_path(path, sizeof(path), FALSE, "most");
    n = ig_read(&c, path, &l, &ordered);
    show("most played", &l);
    check("most played: 7 before 2, not in title order", n == 2 && ordered &&
          !strcmp(l.items[0].label, "Turrican II"));
    list_free(&l);

    /* By chipset, then one chipset */
    list_init(&l);
    ig_page_path(path, sizeof(path), FALSE, "chips");
    n = ig_read(&c, path, &l, &ordered);
    show("by chipset", &l);
    check("games' chipsets: AGA, OCS", n == 2 && !strcmp(l.items[0].label, "AGA") &&
          !strcmp(l.items[1].label, "OCS"));
    i = find(&l, "AGA");
    if (i >= 0) {
        struct ItemList m;
        list_init(&m);
        n = ig_read(&c, l.items[i].path, &m, &ordered);
        show("AGA", &m);
        check("AGA games: Zool only (the demo is under Demos)", n == 1 &&
              !strcmp(m.items[0].label, "Zool AGA"));
        list_free(&m);
    }
    list_free(&l);

    /* Demos */
    list_init(&l);
    ig_page_path(path, sizeof(path), TRUE, "root");
    n = ig_read(&c, path, &l, &ordered);
    show("demos root", &l);
    check("demos root: all demos, chipset, year (one genre, none played)",
          n == 3 && find(&l, "All demos") == 0 && find(&l, "By genre") < 0 &&
          find(&l, "Most played") < 0);
    list_free(&l);

    /* an entry: its facts, its screenshot's base */
    gm = ig_lookup(&c, "iGame:WHD/Games/Turrican2/Turrican2.slave");
    check("looked up by its path", gm && !strcmp(gm->title, "Turrican II"));
    n = ig_facts(gm, lines, 24);
    for (i = 0; i < n; i++)
        printf("  fact: %s\n", lines[i]);
    check("facts: title, by (igame.data), genre, year, played, favourite, slave",
          n >= 8 && !strcmp(lines[0], "Turrican II") && !strcmp(lines[1], "By Factor 5") &&
          !strcmp(lines[3], "Genre: Shoot'em up") && !strcmp(lines[4], "Year: 1991") &&
          !strcmp(lines[7], "Played 7 times") && !strcmp(lines[n - 1], "WHDLoad: Turrican2.slave"));
    ig_shot_base(gm ? gm->path : "", path, sizeof(path));
    check("screenshot: igame.iff in its folder", !strcmp(path, "iGame:WHD/Games/Turrican2/igame"));

    /* the launch script */
    check("script written", gm && ig_write_launch_script(gm, "T:run", why, sizeof(why)));
    {
        BPTR fh = Open((CONST_STRPTR)"T:run", MODE_OLDFILE);
        LONG k = fh ? Read(fh, script, sizeof(script) - 1) : 0;
        if (fh)
            Close(fh);
        script[k > 0 ? k : 0] = 0;
        printf("%s", script);
        check("script: into its folder, WHDLoad with the slave",
              strstr(script, "CD \"iGame:WHD/Games/Turrican2\"\n") &&
              strstr(script, "WHDLoad SLAVE=\"Turrican2.slave\"\n"));
    }
    {
        struct IgGame gone = *gm;
        gone.path = (char *)"iGame:WHD/Games/Gone/Gone.slave";
        check("a slave no longer there: not started", !ig_write_launch_script(&gone, "T:run2", why,
                                                                               sizeof(why)) && why[0]);
    }

    ig_free(&c);
    check("freed", c.n == 0 && !c.pool);
    printf("%s\n", fails ? "SOME TESTS FAILED" : "all iGame tests passed");
    return fails ? 20 : 0;
}
