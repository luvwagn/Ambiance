/*
 * Ambiance - iconified: an AppIcon on Workbench (iconify.c)
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef ICONIFY_H
#define ICONIFY_H

#include <exec/types.h>

enum { ICON_NONE, ICON_OPEN, ICON_QUIT };

BOOL icon_available(void);              /* workbench.library and icon.library there */
BOOL icon_add(const char *label, const char *quit_label);  /* the AppIcon, Tools > Quit */
int icon_poll(void);                    /* ICON_* */
ULONG icon_sigmask(void);
void icon_remove(void);
BOOL icon_shown(void);
void icon_end(void);                    /* at the end: everything let go */

#endif
