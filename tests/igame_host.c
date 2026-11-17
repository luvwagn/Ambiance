/* Host test for igamecsv.c: iGame's gameslist.csv lines (2.3.1 and later,
 * and older), tooltypes made into WHDLoad arguments, demos told from games,
 * paths split. */
#include <stdio.h>
#include <string.h>
#include "../src/igamecsv.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int tt(const char *in, const char *want, int wantret)
{
    char out[160];
    int r = ig_tooltype_arg(in, out, sizeof(out));
    if (r != wantret || (r == 1 && strcmp(out, want))) {
        printf("FAIL tooltype \"%s\": %d \"%s\", wanted %d \"%s\"\n", in, r, out, wantret, want);
        fails++;
        return 0;
    }
    return 1;
}

int main(void)
{
    struct IgRow r;
    char line[512], dir[128], name[64];

    /* iGame 2.3.1 on: strings quoted, 13 columns */
    strcpy(line, "0;\"Turrican II\";\"Shoot'em up\";\"Games:WHD/T/Turrican2/Turrican2.slave\";1;7;1;0;0;"
                 "\"\";1991;1;\"OCS\"\n");
    CHECK(ig_parse_line(line, &r) == 1);
    CHECK(!strcmp(r.title, "Turrican II"));
    CHECK(!strcmp(r.genre, "Shoot'em up"));
    CHECK(!strcmp(r.path, "Games:WHD/T/Turrican2/Turrican2.slave"));
    CHECK(r.favourite == 1 && r.times_played == 7 && r.last_played == 1);
    CHECK(r.hidden == 0 && r.deleted == 0);
    CHECK(r.year == 1991 && r.players == 1);
    CHECK(!strcmp(r.chipset, "OCS"));
    CHECK(!strcmp(ig_row_title(&r), "Turrican II"));

    /* a user's own title, and a ';' inside quotes */
    strcpy(line, "2;\"Lotus; the Ultimate\";\"Racing\";\"DH1:Games/Lotus/Lotus.slave\";0;0;0;0;0;"
                 "\"Lotus Esprit\";0;2;\"\"");
    CHECK(ig_parse_line(line, &r) == 1);
    CHECK(!strcmp(r.title, "Lotus; the Ultimate"));
    CHECK(!strcmp(r.path, "DH1:Games/Lotus/Lotus.slave"));
    CHECK(!strcmp(ig_row_title(&r), "Lotus Esprit"));
    CHECK(r.players == 2 && r.chipset[0] == 0);

    /* before 2.3.1: no quotes, nine columns */
    strcpy(line, "0;Zool;Platform;Work:WHD/Zool/Zool.slave;0;3;0;1;0\r\n");
    CHECK(ig_parse_line(line, &r) == 1);
    CHECK(!strcmp(r.title, "Zool") && !strcmp(r.genre, "Platform"));
    CHECK(!strcmp(r.path, "Work:WHD/Zool/Zool.slave"));
    CHECK(r.times_played == 3 && r.hidden == 1);
    CHECK(r.year == 0 && r.user_title[0] == 0 && r.chipset[0] == 0);

    /* not entries */
    strcpy(line, "\n");
    CHECK(ig_parse_line(line, &r) == 0);
    strcpy(line, "0;\"x\";\"y\";\"\";0;0;0;0;0");
    CHECK(ig_parse_line(line, &r) == 0);

    /* a title missing: the path stands in */
    strcpy(line, "0;\"\";\"\";\"Games:Foo/Foo.slave\";0;0;0;0;0");
    CHECK(ig_parse_line(line, &r) == 1);
    CHECK(!strcmp(ig_row_title(&r), "Games:Foo/Foo.slave"));

    /* demos */
    CHECK(ig_is_demo("Demo", "Games:X/X.slave"));
    CHECK(ig_is_demo("demos", NULL));
    CHECK(ig_is_demo("Intro", NULL));
    CHECK(ig_is_demo("Diskmag", NULL));
    CHECK(ig_is_demo("", "WHD:Demos/State of the Art/SotA.slave"));
    CHECK(ig_is_demo("", "WHD:Demos_AGA/Lapsus/Lapsus.slave"));
    CHECK(!ig_is_demo("Action", "WHD:Games/DemolitionMan/DM.slave"));
    CHECK(ig_is_demo("", "WHD:Games/Demo/Demo.slave"));         /* a folder "Demo" counts */
    CHECK(!ig_is_demo("Platform", "WHD:Games/Zool/Demos.slave"));  /* the file, not a folder */
    CHECK(!ig_is_demo("Demolition", NULL));

    /* tooltypes */
    tt("SLAVE=Turrican2.slave", "SLAVE=\"Turrican2.slave\"", 1);
    tt("PRELOAD", "PRELOAD", 1);
    tt("  NoVBRMove  ", "NoVBRMove", 1);
    tt("Custom1=1", "Custom1=1", 1);
    tt("Custom2=-3", "Custom2=-3", 1);
    tt("QuitKey=$59", "QuitKey=89", 1);
    tt("Data=\"data\"", "Data=\"data\"", 1);
    tt("(Custom3=1)", "", 0);
    tt(";PRELOAD", "", 0);
    tt("*** DON'T EDIT THE FOLLOWING LINES!! ***", "", -1);
    tt("IM1=abc", "", 0);
    tt("DONOTWAIT", "", 0);
    tt("TOOLPRI=-1", "", 0);
    tt("", "", 0);
    tt("Bad Key=1", "", 0);
    tt("Custom=has\"quote", "", 0);

    /* paths */
    ig_split_path("Games:WHD/T/Turrican2.slave", dir, sizeof(dir), name, sizeof(name));
    CHECK(!strcmp(dir, "Games:WHD/T") && !strcmp(name, "Turrican2.slave"));
    ig_split_path("DH0:Game", dir, sizeof(dir), name, sizeof(name));
    CHECK(!strcmp(dir, "DH0:") && !strcmp(name, "Game"));

    printf("%d failed\n", fails);
    return fails != 0;
}
