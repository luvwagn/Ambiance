/*
 * AmigaMCE - archives unpacked (unarc.c): by xadmaster.library where it is
 * installed (LhA, LZX, Zip and the rest it knows), else by the LhA
 * command in C:. AmigaMCE carries no unpacker of its own.
 */
#ifndef AMCE_UNARC_H
#define AMCE_UNARC_H

#include "amce.h"

enum { UNARC_NONE, UNARC_XAD, UNARC_LHA };

int unarc_how(void);            /* what would unpack an archive here, now */
const char *unarc_name(int how);    /* "xadmaster.library", "C:LhA", "" */

/* Every file of archive into dest (made if need be), its drawers kept.
 * An entry naming a place outside dest (a device, a leading '/', '//')
 * is left out. why: what went wrong; FALSE then. */
BOOL unarc_extract(const char *archive, const char *dest, char *why, LONG wmax);

/* A name from an archive safe to put under a drawer: not empty, no ':',
 * no leading '/', no "//" (AmigaDOS's parent), no "/" alone at its end
 * that would leave nothing. */
BOOL unarc_safe_name(const char *name);

#endif
