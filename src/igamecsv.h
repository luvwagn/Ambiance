/*
 * Ambiance - iGame's files read: a gameslist.csv line, an icon's tooltype
 * as a WHDLoad argument, games told from demos (igamecsv.c). No Amiga
 * headers, so the host tests use it as it is.
 */
#ifndef AMB_IGAMECSV_H
#define AMB_IGAMECSV_H

/* One line of gameslist.csv, its fields pointing into the line (changed
 * in place). iGame 2.3.1 and later quote the strings and add user_title,
 * year, players and chipset; older files have neither the quotes nor the
 * last columns, which are then "" and 0. */
struct IgRow {
    long instance;
    char *title, *genre, *path;
    long favourite, times_played, last_played, hidden, deleted;
    char *user_title;
    long year, players;
    char *chipset;
};

/* 1: a line read into r; 0: not an entry (blank, or no path). */
int ig_parse_line(char *line, struct IgRow *r);

/* What the list shows: the user's own title when there is one. */
const char *ig_row_title(const struct IgRow *r);

/* A demo rather than a game: genre "Demo" (iGame's own), an intro or a
 * disk magazine, or a path through a folder named Demos. */
int ig_is_demo(const char *genre, const char *path);

/* One tooltype of a WHDLoad icon made into an argument for WHDLoad's
 * command line, as iGame does it: KEY=$hex in decimal, KEY=number as it
 * is, KEY=text quoted, a switch as it is. Into out: 1 an argument, 0 a
 * tooltype to leave out (commented, an icon's image data), -1 the
 * NewIcons image lines begin (nothing after it is a tooltype). */
int ig_tooltype_arg(const char *tt, char *out, long max);

/* The folder of a path ("Games:Foo/Foo.slave" -> "Games:Foo"), and its
 * file name ("Foo.slave"). */
void ig_split_path(const char *path, char *dir, long dmax, char *name, long nmax);

#endif
