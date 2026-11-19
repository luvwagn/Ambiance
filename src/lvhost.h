/* The layouts' drawing calls (lvhost.c): the host's side of struct
 * LayoutHost that draws, apart from main.c */
#ifndef LVHOST_H
#define LVHOST_H
#include "layout.h"
#include "lkcore.h"

struct LvClip { BOOL on; WORD x0, y0, x1, y1; };   /* drawing kept inside (a viewer's controls) */

struct LvHost {
    struct Gfx *g;                              /* the screen's (its fonts, size) */
    struct LkOut *out;                          /* the canvas drawn on */
    struct LvClip *ctl;                         /* the clip a layout's own can't leave */
    const struct BigLetter *(*letter)(void);    /* the big letter up, NULL none */
    BOOL (*own)(const struct LImage *im);       /* a picture the host made for the layout */
    BOOL *sp_up;                                /* set: the letter is up by sprites */
    int (*icon_of)(int section);                /* a section's icon (ICON_*) */
};
extern struct LvHost lvh;

/* the drawing calls into h (clip ... plexmark); main sets the rest */
void lvh_calls(struct LayoutHost *h);
void lvh_free(void);            /* what the calls made for themselves (the logo, Plex's mark) */

#endif
