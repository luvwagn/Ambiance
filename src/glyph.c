/*
 * AmigaMCE - the playback controls' glyphs
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * One set, drawn by Now playing's buttons, the mini player's, the controls
 * beneath an animation or video (and a DVD's), and the bar over an ANIM
 * on a screen of its own: so they look the same, and a change is made
 * once. Each is measured from its height in rows (odd, so a triangle has
 * a one-row tip) and drawn sx pixels across for each one down, so it is
 * as wide as tall on screen where pixels are tall (hires, not laced);
 * lowres, hires, laced and RTG alike. Drawn in pixels with RectFill: no
 * lines, so the same at every size.
 */
#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/displayinfo.h>
#include <intuition/screens.h>
#include <proto/graphics.h>
#include "glyph.h"

static void fill(struct RastPort *rp, WORD x0, WORD y0, WORD x1, WORD y1, UBYTE pen)
{
    WORD t;
    if (x1 < x0) {
        t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        t = y0;
        y0 = y1;
        y1 = t;
    }
    SetAPen(rp, pen);
    RectFill(rp, x0, y0, x1, y1);
}

/* A triangle pointing right (dir > 0) or left, h rows (odd: one row for
 * its tip), each row sx pixels wider than the last to the middle: clean
 * 45-degree edges in pixels, as wide as it is tall on screen. Its width:
 * (h + 1) / 2 * sx. */
static void tri(struct RastPort *rp, WORD x, WORD y, WORD h, WORD sx, int dir, UBYTE pen)
{
    WORD r, w = (h + 1) / 2 * sx;
    for (r = 0; r < h; r++) {
        WORD d = r < h - 1 - r ? r : h - 1 - r, len = (d + 1) * sx;
        if (dir > 0)
            fill(rp, x, y + r, x + len - 1, y + r, pen);
        else
            fill(rp, x + w - len, y + r, x + w - 1, y + r, pen);
    }
}

WORD glyph_rows(WORD h)
{
    WORD gh = (h * 3 / 5) | 1;
    if (h <= 16)
        gh = (h / 2) | 1;                   /* small (lowres): more room round it */
    if (gh < (h <= 16 ? 5 : 7))
        gh = h <= 16 ? 5 : 7;
    return gh;
}

WORD glyph_sx(struct Screen *scr)
{
    return scr ? glyph_sx_mode(GetVPModeID(&scr->ViewPort)) : 1;
}

WORD glyph_sx_mode(ULONG id)
{
    struct DisplayInfo di;
    WORD sx;
    if (id == (ULONG)INVALID_ID || !GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, id) ||
        !di.Resolution.x || !di.Resolution.y)
        return 1;
    /* (its pixel's size in ticks: as tall as wide on RTG and laced hires) */
    sx = (WORD)((di.Resolution.y + di.Resolution.x / 2) / di.Resolution.x);
    return sx < 1 ? 1 : sx > 4 ? 4 : sx;
}

/* The speaker: its parts' sizes for gh rows. */
static void speaker_parts(WORD gh, WORD sx, WORD *bh, WORD *bw, WORD *cw, WORD *t)
{
    *bh = gh / 3 > 2 ? gh / 3 : 2;
    *bw = (gh / 4 > 1 ? gh / 4 : 1) * sx;
    *cw = (gh / 3 > 1 ? gh / 3 : 1) * sx;
    *t = gh / 7 > 1 ? gh / 7 : 1;
}

WORD glyph_speaker_w(WORD gh, WORD sx)
{
    WORD bh, bw, cw, t, xs = gh / 2 | 1;
    speaker_parts(gh, sx, &bh, &bw, &cw, &t);
    return bw + cw + 2 * sx + (xs + t - 1) * sx;
}

/* A speaker, its left at x, centred on row cy: a cross after it when
 * muted, two waves when not. */
static void speaker(struct RastPort *rp, WORD x, WORD cy, WORD gh, WORD sx, BOOL muted, UBYTE fg)
{
    WORD bh, bw, cw, t, c, rx;
    speaker_parts(gh, sx, &bh, &bw, &cw, &t);
    fill(rp, x, cy - bh / 2, x + bw - 1, cy - bh / 2 + bh - 1, fg);
    for (c = 0; c < cw; c++) {              /* the cone, widening */
        WORD hh = bh + (WORD)((LONG)(gh - bh) * (c + 1) / cw);
        fill(rp, x + bw + c, cy - hh / 2, x + bw + c, cy - hh / 2 + hh - 1, fg);
    }
    rx = x + bw + cw + 2 * sx;
    if (muted) {
        WORD xs = gh / 2 | 1, d;
        for (d = 0; d < xs; d++) {
            WORD y0 = cy - xs / 2 + d;
            fill(rp, rx + d * sx, y0, rx + (d + t) * sx - 1, y0, fg);
            fill(rp, rx + (xs - 1 - d) * sx, y0, rx + (xs - 1 - d + t) * sx - 1, y0, fg);
        }
    } else {
        WORD h1 = gh / 3 | 1, h2 = (gh * 2 / 3) | 1;
        fill(rp, rx, cy - h1 / 2, rx + t * sx - 1, cy - h1 / 2 + h1 - 1, fg);
        fill(rp, rx + (t + 2) * sx, cy - h2 / 2, rx + (t * 2 + 2) * sx - 1, cy - h2 / 2 + h2 - 1, fg);
    }
}

/* The loop: a loop over a straight arrow, the one that happens in lp
 * (looping: the loop; else the arrow, played through), the other in ap. */
static void loop(struct RastPort *rp, WORD cx, WORD gy, WORD gh, WORD sx, WORD t, UBYTE lp, UBYTE ap)
{
    WORD lw = gh * sx + 2 * sx, x0 = cx - lw / 2, y0 = gy, tt = t > 2 ? 2 : t, txs = tt * sx;
    WORD lh = gh * 3 / 5, hd = (lh / 2) | 1, ay = gy + gh - 1 - tt / 2, ahd = (gh / 3) | 1;
    if (sx == 1 && gh >= 11) {
        /* not lowres: finer, as the solid glyphs beside it are; lines of
         * one pixel (two from 19 rows), rounded corners, the head at the
         * top's middle, in a gap of its own */
        WORD lt = gh < 19 ? 1 : 2, x1, y1, ah, hx, bh;
        lw = gh + gh / 3;
        if (!(lw & 1))
            lw--;
        x0 = cx - lw / 2;
        x1 = x0 + lw - 1;
        lh = gh * 5 / 9;
        y1 = y0 + lh - 1;
        ah = (lh / 2 + lt) | 1;
        hx = cx - (ah + 1) / 4;
        fill(rp, x0 + lt, y0, hx - lt - 1, y0 + lt - 1, lp);
        fill(rp, hx + (ah + 1) / 2 + 1, y0, x1 - lt, y0 + lt - 1, lp);
        fill(rp, x0 + lt, y1 - lt + 1, x1 - lt, y1, lp);
        fill(rp, x0, y0 + lt, x0 + lt - 1, y1 - lt, lp);
        fill(rp, x1 - lt + 1, y0 + lt, x1, y1 - lt, lp);
        if (lt > 1) {
            fill(rp, x0 + 1, y0 + 1, x0 + 1, y0 + 1, lp);
            fill(rp, x1 - 1, y0 + 1, x1 - 1, y0 + 1, lp);
            fill(rp, x0 + 1, y1 - 1, x0 + 1, y1 - 1, lp);
            fill(rp, x1 - 1, y1 - 1, x1 - 1, y1 - 1, lp);
        }
        tri(rp, hx, y0 + lt / 2 - ah / 2, ah, 1, 1, lp);
        bh = ((gh - lh) / 2) | 1;
        if (bh < ah)
            bh = ah;
        ay = gy + gh - 1 - bh / 2;
        fill(rp, x0, ay - (lt - 1) / 2, x1 - (bh + 1) / 2, ay - (lt - 1) / 2 + lt - 1, ap);
        tri(rp, x1 - (bh + 1) / 2 + 1, ay - bh / 2, bh, 1, 1, ap);
        return;
    }
    /* lowres (or tall pixels): a frame, an arrowhead on its top running
     * right; the arrow under it, straight through */
    fill(rp, x0 + txs, y0, x0 + lw - 1 - txs - (hd + 1) / 2 * sx, y0 + tt - 1, lp);
    fill(rp, x0 + txs, y0 + lh - tt, x0 + lw - 1 - txs, y0 + lh - 1, lp);
    fill(rp, x0, y0 + tt, x0 + txs - 1, y0 + lh - tt - 1, lp);
    fill(rp, x0 + lw - txs, y0 + tt, x0 + lw - 1, y0 + lh - tt - 1, lp);
    tri(rp, x0 + lw - txs - (hd + 1) / 2 * sx, y0 + tt / 2 - hd / 2, hd, sx, 1, lp);
    fill(rp, x0, ay - tt / 2, x0 + lw - 1 - (ahd + 1) / 2 * sx, ay - tt / 2 + tt - 1, ap);
    tri(rp, x0 + lw - (ahd + 1) / 2 * sx, ay - ahd / 2, ahd, sx, 1, ap);
}

/* Full screen: four corners, each two arms th thick; at the glyph's
 * corners, arms along its edges (in: by its middle, arms outwards). */
static void corners(struct RastPort *rp, WORD cx, WORD cy, WORD gh, WORD sx, WORD th, BOOL in,
                    UBYTE fg)
{
    WORD hw = gh * sx / 2, left = cx - hw, right = left + 2 * hw - 1, top = cy - gh / 2,
         bottom = top + gh - 1, g = gh / 8 + 1, L = in ? gh / 2 - g + 1 : gh * 2 / 5, c;
    if (L < 3)
        L = 3;
    for (c = 0; c < 4; c++) {
        WORD dx = c & 1 ? 1 : -1, dy = c & 2 ? 1 : -1, px, py, ex, ey;
        if (!in) {
            px = dx < 0 ? left : right;
            py = dy < 0 ? top : bottom;
            ex = -dx;
            ey = -dy;
        } else {
            px = cx + dx * g * sx;
            py = cy + dy * g;
            ex = dx;
            ey = dy;
        }
        fill(rp, px, py, px + ex * (L * sx - 1), py + ey * (th - 1), fg);
        fill(rp, px, py, px + ex * (th * sx - 1), py + ey * (L - 1), fg);
    }
}

void glyph_draw(struct RastPort *rp, int kind, WORD cx, WORD cy, WORD gh, WORD sx, UBYTE fg,
                UBYTE dim, UWORD flags)
{
    WORD tw = (gh + 1) / 2 * sx;            /* a triangle's width */
    WORD t = gh / 5 > 2 ? gh / 5 : 2;       /* a bar's thickness, in rows */
    WORD gy = cy - gh / 2;
    if (sx < 1)
        sx = 1;
    switch (kind) {
    case GL_PREV: {
        WORD bw = t * sx, x0 = cx - (bw + tw) / 2;
        fill(rp, x0, gy, x0 + bw - 1, gy + gh - 1, fg);
        tri(rp, x0 + bw, gy, gh, sx, -1, fg);
        break;
    }
    case GL_NEXT: {
        WORD bw = t * sx, x0 = cx - (bw + tw) / 2;
        tri(rp, x0, gy, gh, sx, 1, fg);
        fill(rp, x0 + tw, gy, x0 + tw + bw - 1, gy + gh - 1, fg);
        break;
    }
    case GL_BACK:
        tri(rp, cx - tw, gy, gh, sx, -1, fg);
        tri(rp, cx, gy, gh, sx, -1, fg);
        break;
    case GL_FWD:
        tri(rp, cx - tw, gy, gh, sx, 1, fg);
        tri(rp, cx, gy, gh, sx, 1, fg);
        break;
    case GL_PLAY:
        tri(rp, cx - tw / 2, gy, gh, sx, 1, fg);
        break;
    case GL_PAUSE: {
        /* two bars a bar's width apart, a row short of the triangle's
         * height at each end (as tall as Stop's square) */
        WORD bw = (gh / 4 > 2 ? gh / 4 : 2) * sx, x0 = cx - (bw * 3) / 2, sh = gh - 2;
        fill(rp, x0, cy - sh / 2, x0 + bw - 1, cy - sh / 2 + sh - 1, fg);
        fill(rp, x0 + bw * 2, cy - sh / 2, x0 + bw * 3 - 1, cy - sh / 2 + sh - 1, fg);
        break;
    }
    case GL_STOP: {
        WORD sh = gh - 2, sw = sh * sx;     /* a square, a little under the triangles */
        fill(rp, cx - sw / 2, cy - sh / 2, cx - sw / 2 + sw - 1, cy - sh / 2 + sh - 1, fg);
        break;
    }
    case GL_LOOP:
        loop(rp, cx, gy, gh, sx, t, flags & GLF_ON ? fg : dim, flags & GLF_ON ? dim : fg);
        break;
    case GL_MUTE:
        speaker(rp, cx - glyph_speaker_w(gh, sx) / 2, cy, gh, sx, (flags & GLF_ON) != 0, fg);
        break;
    case GL_VDOWN:
    case GL_VUP: {
        WORD len = gh, lw = len * sx, th = t > 2 ? t - 1 : 2, tx = th * sx;
        fill(rp, cx - lw / 2, cy - th / 2, cx - lw / 2 + lw - 1, cy - th / 2 + th - 1, fg);
        if (kind == GL_VUP)
            fill(rp, cx - tx / 2, cy - len / 2, cx - tx / 2 + tx - 1, cy - len / 2 + len - 1, fg);
        break;
    }
    case GL_FULL:
    case GL_UNFULL:
        corners(rp, cx, cy, gh, sx, gh < 11 ? 1 : t > 2 ? t - 1 : 2, kind == GL_UNFULL, fg);
        break;
    case GL_MENU: {
        /* three bars as wide as the glyph is tall, a bar's height apart
         * (thinner than Pause's: a list, not a button) */
        WORD th = t > 2 ? t - 1 : 2, lw = gh * sx, gap = (gh - 3 * th) / 2, k;
        for (k = 0; k < 3; k++) {
            WORD y0 = gy + k * (th + gap);
            fill(rp, cx - lw / 2, y0, cx - lw / 2 + lw - 1, y0 + th - 1, fg);
        }
        break;
    }
    }
}
