/*
 * AmigaMCE - the program's side of the layout engines (layout.h)
 * Copyright (c) 2026 Brian King
 */
#ifndef LHOST_H
#define LHOST_H

#include "layout.h"

struct LayoutInfo {
    char file[32];              /* "kodilite.layout" */
    char name[32];              /* "Kodi lite" */
    UWORD min_bits, min_h;
};

/* The layouts in PROGDIR:Layouts that this program can use (each opened,
 * its table asked for with this build's LAYOUT_ABI, closed again): up to
 * max of them into out; how many. */
int layout_list(struct LayoutInfo *out, int max);

/* PROGDIR:Layouts/<file> opened and its table taken (NULL: not there,
 * or built against another layout.h; the log says which). Kept open till
 * layout_unload. */
const struct LayoutEngine *layout_load(const char *file);
void layout_unload(void);                       /* closed, out of memory */
const struct LayoutEngine *layout_loaded(void); /* the one open, or NULL */

#endif
