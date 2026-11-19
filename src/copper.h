/* Ambiance - the screen's user copper list (AGA), built from requests
 * (copper.c): palette bands, colours changed line by line. */
#ifndef AMB_COPPER_H
#define AMB_COPPER_H

#include <exec/types.h>

struct Screen;
struct CopPlan;

/* An AGA screen whose copper list can be added to */
BOOL cop_ok(struct Screen *scr);
/* A plan of at most max writes; NULL: none to be had (not AGA, memory) */
struct CopPlan *cop_plan(struct Screen *scr, LONG max);
/* From screen line y down, register pen is rgb (0xRRGGBB). A line's
 * writes are made in the order asked: the first in its blanking. */
void cop_color(struct CopPlan *p, LONG y, UBYTE pen, ULONG rgb);
/* Made the screen's user copper list (replacing any), and shown */
BOOL cop_commit(struct CopPlan *p);
void cop_plan_free(struct CopPlan *p);
/* The screen's user copper list gone */
void cop_off(struct Screen *scr);
/* Screen lines nw writes made at one line take to be done, at per_line
 * copper moves a scanline (0: a cautious guess) */
LONG cop_lines_for(struct Screen *scr, LONG nw, LONG per_line);

#endif
