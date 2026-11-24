/*
 * Ambiance - a Jellyfin server's JSON, read just enough (jfjson.c). No
 * Amiga headers, so the host tests use it as it is.
 */
#ifndef AMB_JFJSON_H
#define AMB_JFJSON_H

/* The tokenizer: buf (len bytes, changed: strings are decoded where they
 * are, \uXXXX to UTF-8) walked once. For each value, val() gets its
 * nesting depth (the top object's members are depth 1), its member name
 * (NULL in an array), its kind and, for a string, number, true, false or
 * null, its text (strings decoded, NUL-ended). '{' and '[' come first as
 * kinds, then their contents, then end() with the same depth. A callback
 * returning 0 stops the walk. Returns 0, or -1 if buf is not JSON. */
enum { JF_STR = 's', JF_NUM = 'n', JF_TRUE = 't', JF_FALSE = 'f', JF_NULL = 'z',
       JF_OBJ = '{', JF_ARR = '[' };
typedef int (*jf_val_fn)(void *ctx, int depth, const char *name, int kind, const char *text);
typedef int (*jf_end_fn)(void *ctx, int depth, int kind);
int jf_walk(char *buf, unsigned long len, jf_val_fn val, jf_end_fn end, void *ctx);

/* An item of an answer's "Items" (or a sign-in's "User"), the members
 * Ambiance shows: strings UTF-8 as the server sent them (cut to fit),
 * numbers as text ("" not sent). */
struct JfItem {
    char name[160], id[36], type[24], coll[16], media[12];
    char album[120], album_id[36], album_artist[120], artist[120], artist_id[36];
    char year[8], ticks[16], index[8], disc[8], children[8], container[16];
    char image[36];             /* ImageTags.Primary */
    char album_image[36];       /* AlbumPrimaryImageTag (a track's album's cover) */
    char width[8], height[8], date[24], rating[12], overview[300];
    int folder;                 /* IsFolder: 1 true, 0 false or not sent */
};

/* The items of an answer (buf, len: changed). item() gets each in turn
 * (and may stop the walk returning 0); *total: TotalRecordCount (else the
 * items' count). Returns the items read, -1 if not JSON. */
long jf_items(char *buf, unsigned long len, int (*item)(void *ctx, const struct JfItem *it),
              void *ctx, long *total);

/* One member of the answer's top object, or of an object one level down
 * ("User.Id": dotted), as text into out (max bytes). 1 found, 0 not. */
int jf_value(const char *buf, unsigned long len, const char *name, char *out, long max);

/* Discovery's answer (a UDP packet, buf len bytes, changed): its
 * "Address" (http://host:port), "Id" and "Name" into the three outs.
 * 1 if it had an address. */
int jf_discovered(char *buf, unsigned long len, char *address, long amax, char *id, long imax,
                  char *name, long nmax);

#endif
