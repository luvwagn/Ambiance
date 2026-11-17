/*
 * Ambiance - the playback controls' glyphs (glyph.c): one set for Now
 * playing, the mini player, and the anim and video controls
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef AMB_GLYPH_H
#define AMB_GLYPH_H

#include <exec/types.h>

struct RastPort;
struct Screen;

enum {
    GL_PREV,        /* |<  the previous track or chapter, or from the start */
    GL_NEXT,        /* >|  the next */
    GL_BACK,        /* <<  back a step */
    GL_FWD,         /* >>  on a step */
    GL_PLAY,        /* >   */
    GL_PAUSE,       /* ||  */
    GL_STOP,        /* a square */
    GL_LOOP,        /* a loop over a straight arrow (GLF_ON: looping) */
    GL_MUTE,        /* a speaker (GLF_ON: muted, crossed) */
    GL_VDOWN,       /* -   */
    GL_VUP,         /* +   */
    GL_FULL,        /* four corners at the edges: full screen */
    GL_UNFULL,      /* four corners by the middle: back from it */
    GL_MENU         /* three bars: a DVD's menu */
};
#define GLF_ON 1

/* A glyph's rows for a button h rows high: odd (each triangle a one-row
 * tip), about three fifths of it (a half when small, lowres). */
WORD glyph_rows(WORD h);
/* Pixels across for one down on scr, so a glyph looks square: 2 where
 * pixels are tall (hires, not laced), 4 superhires, else 1. */
WORD glyph_sx(struct Screen *scr);
WORD glyph_sx_mode(ULONG id);       /* the same for a display mode */
/* Glyph kind, gh rows (glyph_rows), centred on cx, cy, sx across for one
 * down; in pen fg (pens as the RastPort's), dim the loop's part not in
 * effect. */
void glyph_draw(struct RastPort *rp, int kind, WORD cx, WORD cy, WORD gh, WORD sx, UBYTE fg,
                UBYTE dim, UWORD flags);
/* The speaker's width (GL_MUTE), to place it. */
WORD glyph_speaker_w(WORD gh, WORD sx);

#endif
