/* Ambiance - the screen's user copper list (AGA), built from requests
 * (copper.c): palette bands, colours changed line by line, other
 * registers (a half size video's rows shown twice). */
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
/* From screen line y down, register reg (its offset from 0xDFF000:
 * 0x108 BPL1MOD) is val: in the line's writes, in the order asked; its
 * line's wait at the line's start (not cop_early's) */
void cop_reg(struct CopPlan *p, LONG y, UWORD reg, UWORD val);
/* A register as the system's copper list sets it for the screen (FALSE:
 * not found) */
BOOL cop_sysreg(struct Screen *scr, UWORD reg, UWORD *val);
/* A line's writes begun at the end of the line before it in its field
 * (once its pixels are done: the whole blanking, about twice as many
 * writes before the line's pixels). For bands under a picture whose last
 * lines' right edge is the display window's. */
void cop_early(struct CopPlan *p, BOOL on);
/* Made the screen's user copper list (replacing any), and shown */
BOOL cop_commit(struct CopPlan *p);
void cop_plan_free(struct CopPlan *p);
/* The screen's user copper list gone */
void cop_off(struct Screen *scr);
/* Screen lines nw writes made at one line take to be done, at per_line
 * copper moves a scanline (0: a cautious guess) */
LONG cop_lines_for(struct Screen *scr, LONG nw, LONG per_line);

#endif
