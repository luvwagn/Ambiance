/*
 * AmigaMCE - a Plex server's XML, read just enough (plexxml.c). No Amiga
 * headers, so the host tests use it as it is.
 */
#ifndef AMCE_PLEXXML_H
#define AMCE_PLEXXML_H

#define XML_MAX_ATTRS 40

long xml_scan(char *buf, unsigned long len,
              int (*el)(void *ctx, const char *name, char **attr, int n, int depth), void *ctx);
const char *xml_attr(char **attr, int n, const char *name);
void xml_unescape(char *s);

#endif
