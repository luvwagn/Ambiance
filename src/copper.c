/*
 * Ambiance - the screen's user copper list (AGA), built from requests
 * Copyright (c) 2026 Brian King
 *
 * One user copper list on a screen's ViewPort, made from what is asked of
 * it: from line y, colour register p is rgb (24 bits: BPLCON3's LOCT, the
 * high nibbles written, then the low). What it is for:
 *
 *   - palette bands: from a line down, some registers other colours (a
 *     picture viewer's controls under a picture with every register of its
 *     own; a layout's rows, each with a palette of its own);
 *   - colours changed line by line (a layout's background gradient).
 *
 * The system's copper list loads the ColorMap's colours at the top of each
 * frame, so what is written here holds from its line to the bottom of the
 * screen, every frame. A line's writes are made in the order asked (the
 * first ones fall in the line's blanking, before its pixels: about 13 of
 * them), grouped by register bank where they can be.
 *
 * Lines are the screen's, even laced: MrgCop gives each field the waits of
 * its own lines, so on a laced screen a band's writes are made at its line
 * and the one after (each field its own). BPLCON3's other bits are the
 * system's, read from the copper list it shows.
 */
#include "amb.h"
#include "copper.h"

#include <string.h>
#include <stdio.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/copper.h>
#include <graphics/view.h>
#include <graphics/gfxmacros.h>
#include <hardware/custom.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

extern struct GfxBase *GfxBase;

#define CP_CUSTOM(off) ((APTR)(0xDFF000UL + (off)))
#define CP_BPLCON3 0x106
#define CP_COLOR(n) (0x180 + (n) * 2)

struct CopWrite {
    LONG y;
    UBYTE pen;
    ULONG rgb;
};

struct CopPlan {
    struct Screen *scr;
    struct CopWrite *w;
    LONG n, max;
    BOOL early;                 /* each line's writes begun at the end of the line before
                                 * in its field (after its pixels): its whole blanking */
};

/* The end of a line's pixels, in the copper's horizontal positions (colour
 * clocks): a standard display window's right edge (0x1C1 lores pixels) */
#define CP_LINE_END 0xE0

/* BPLCON3 as the system's copper list sets it (bank and LOCT ours) */
static BOOL cop_bplcon3(UWORD *base)
{
    struct View *v = GfxBase->ActiView;
    UWORD *c;
    LONG i;
    if (!v || !v->LOFCprList || !(c = v->LOFCprList->start))
        return FALSE;
    for (i = 0; i < v->LOFCprList->MaxCount && i < 8000; i++, c += 2) {
        if (c[0] == 0xFFFF && c[1] == 0xFFFE)
            break;
        if (c[0] == CP_BPLCON3) {
            *base = c[1] & (UWORD)~0xE200;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL cop_ok(struct Screen *scr)
{
    UWORD base;
    return scr && (GfxBase->ChipRevBits0 & GFXF_AA_ALICE) && GfxBase->LibNode.lib_Version >= 39 &&
           cop_bplcon3(&base);
}

struct CopPlan *cop_plan(struct Screen *scr, LONG max)
{
    struct CopPlan *p;
    if (!cop_ok(scr) || max <= 0 || !(p = AllocVec(sizeof(*p), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    if (!(p->w = AllocVec((ULONG)max * sizeof(struct CopWrite), MEMF_ANY))) {
        FreeVec(p);
        return NULL;
    }
    p->scr = scr;
    p->max = max;
    return p;
}

void cop_color(struct CopPlan *p, LONG y, UBYTE pen, ULONG rgb)
{
    if (!p || p->n >= p->max || y < 0)
        return;
    p->w[p->n].y = y;
    p->w[p->n].pen = pen;
    p->w[p->n].rgb = rgb & 0xFFFFFF;
    p->n++;
}

void cop_early(struct CopPlan *p, BOOL on)
{
    if (p)
        p->early = on;
}

void cop_plan_free(struct CopPlan *p)
{
    if (p) {
        FreeVec(p->w);
        FreeVec(p);
    }
}

/* Moves a line's writes need, at most: per run of one bank, BPLCON3 and a
 * register for the high nibbles, the same for the low, then BPLCON3 back */
static LONG cop_moves(LONG nw)
{
    return nw * 4 + 2;
}

LONG cop_lines_for(struct Screen *scr, LONG nw, LONG per_line)
{
    LONG moves = nw * 2 + (nw / 32 + 2) * 2 + 1, lines;   /* (the runs of a bank or two) */
    if (per_line <= 0)
        per_line = 16;
    lines = (moves + per_line - 1) / per_line + 1;
    return scr && (scr->ViewPort.Modes & LACE) ? lines * 2 : lines;
}

/* One wait and its writes: in the order given, consecutive ones of one
 * bank as a run (their high nibbles, then their low) */
static void cop_emit(struct UCopList *ucl, LONG y, const struct CopWrite *w, LONG n, UWORD base, LONG before)
{
    LONG i = 0, j, k;
    if (before > 0 && y - before >= 0)
        CWait(ucl, y - before, CP_LINE_END);    /* (the line before's end: all its blanking) */
    else
        CWait(ucl, y, 0);
    CBump(ucl);
    while (i < n) {
        UWORD bank = (UWORD)((w[i].pen >> 5) << 13);
        for (j = i + 1; j < n && (UWORD)((w[j].pen >> 5) << 13) == bank; j++)
            ;
        CMove(ucl, CP_CUSTOM(CP_BPLCON3), base | bank);
        CBump(ucl);
        for (k = i; k < j; k++) {
            ULONG c = w[k].rgb;
            CMove(ucl, CP_CUSTOM(CP_COLOR(w[k].pen & 31)),
                  (UWORD)((((c >> 20) & 15) << 8) | (((c >> 12) & 15) << 4) | ((c >> 4) & 15)));
            CBump(ucl);
        }
        CMove(ucl, CP_CUSTOM(CP_BPLCON3), base | bank | 0x0200);
        CBump(ucl);
        for (k = i; k < j; k++) {
            ULONG c = w[k].rgb;
            CMove(ucl, CP_CUSTOM(CP_COLOR(w[k].pen & 31)),
                  (UWORD)((((c >> 16) & 15) << 8) | (((c >> 8) & 15) << 4) | (c & 15)));
            CBump(ucl);
        }
        i = j;
    }
    CMove(ucl, CP_CUSTOM(CP_BPLCON3), base);    /* (the system's again) */
    CBump(ucl);
}

BOOL cop_commit(struct CopPlan *p)
{
    struct ViewPort *vp;
    struct UCopList *ucl;
    UWORD base;
    LONG i, j, nlines = 0, size;
    BOOL lace;
    char m[100];
    if (!p || !p->n || !cop_bplcon3(&base))
        return FALSE;
    vp = &p->scr->ViewPort;
    lace = (vp->Modes & LACE) != 0;
    /* (sorted by line, keeping each line's order: a stable insertion sort) */
    for (i = 1; i < p->n; i++) {
        struct CopWrite x = p->w[i];
        for (j = i - 1; j >= 0 && p->w[j].y > x.y; j--)
            p->w[j + 1] = p->w[j];
        p->w[j + 1] = x;
    }
    size = 4;
    for (i = 0; i < p->n; i = j) {
        for (j = i; j < p->n && p->w[j].y == p->w[i].y; j++)
            ;
        size += (2 + cop_moves(j - i)) * (lace ? 2 : 1);
        nlines++;
    }
    if (!(ucl = AllocMem(sizeof(*ucl), MEMF_PUBLIC | MEMF_CLEAR)))
        return FALSE;
    if (!UCopperListInit(ucl, size)) {
        FreeMem(ucl, sizeof(*ucl));
        return FALSE;
    }
    for (i = 0; i < p->n; i = j) {
        for (j = i; j < p->n && p->w[j].y == p->w[i].y; j++)
            ;
        cop_emit(ucl, p->w[i].y, p->w + i, j - i, base, p->early ? (lace ? 2 : 1) : 0);
        if (lace)                           /* (the other field's line too) */
            cop_emit(ucl, p->w[i].y + 1, p->w + i, j - i, base, p->early ? 2 : 0);
    }
    CWait(ucl, 10000, 255);                 /* (CEND) */
    CBump(ucl);
    if (vp->UCopIns)
        FreeVPortCopLists(vp);              /* (one list a screen: this replaces any) */
    Forbid();
    vp->UCopIns = ucl;
    Permit();
    MakeScreen(p->scr);
    RethinkDisplay();
    sprintf(m, "%ld writes on %ld lines%s", (long)p->n, (long)nlines, lace ? " (each field)" : "");
    amb_log("copper", m);
    return TRUE;
}

void cop_off(struct Screen *scr)
{
    if (scr && scr->ViewPort.UCopIns) {
        FreeVPortCopLists(&scr->ViewPort);
        RemakeDisplay();
    }
}
