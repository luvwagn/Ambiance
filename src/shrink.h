/* shrink.c: a picture made small as its rows come (jdec's), the whole
 * decoded picture never held; deep_thumb's box averages, into dw x dh RGB */
#ifndef AMB_SHRINK_H
#define AMB_SHRINK_H
#ifdef SHRINK_HOST
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned char UBYTE;
#endif
struct Shrink;
struct Shrink *shrink_new(ULONG sw, ULONG sh, ULONG dw, ULONG dh);
void shrink_rows(struct Shrink *k, const UBYTE *rgb, LONG y, LONG n, LONG stride);
UBYTE *shrink_done(struct Shrink *k);
void shrink_free(struct Shrink *k);
#endif
