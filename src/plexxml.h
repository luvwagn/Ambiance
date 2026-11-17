/*
 * Ambiance - a Plex server's XML, read just enough (plexxml.c). No Amiga
 * headers, so the host tests use it as it is.
 */
#ifndef AMB_PLEXXML_H
#define AMB_PLEXXML_H

#define XML_MAX_ATTRS 40

long xml_scan(char *buf, unsigned long len,
              int (*el)(void *ctx, const char *name, char **attr, int n, int depth), void *ctx);
const char *xml_attr(char **attr, int n, const char *name);
void xml_unescape(char *s);

/* plex.tv's answers (XML or JSON): a value by its name (1 found); the
 * account's servers (/api/resources) as "address\tport\tname\ttoken\n"
 * lines, each by its first local address, plain HTTP where it has one. */
int plex_tv_value(const unsigned char *body, const char *name, char *out, long max);
long plex_resource_lines(unsigned char *body, unsigned long len, char *out, long max);

#endif
