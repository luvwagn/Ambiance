/*
 * AmigaMCE - IFF ANIM player (Videos)
 * Copyright (c) 2026 Brian King
 *
 * An ANIM plays on a screen of its own, in its own display mode (HAM,
 * extra halfbrite, interlace, overscan: what it was drawn for), opened in
 * front of AmigaMCE's, which waits behind it as it was. Its frames come
 * from the file one at a time, never the whole of it (Tron2 is 28 MB),
 * and are decoded straight into the bitplanes (animdec.c), two buffers
 * shown in turn as the format intends: a frame changes the one two back.
 *
 * Each frame is shown when its time comes (ANHD: sixtieths of a second);
 * a frame that takes longer to read and decode is shown late and the
 * rest follow it, none skipped (a delta needs the frame before). The log
 * says how it went: frames a second wanted and got, the slowest frame.
 *
 * Keys: Esc, Backspace, the right button or the pad's blue stop it; Space,
 * Return, the left button or fire pause it; Up and Down (the stick, the
 * wheel) set the volume of what plays meanwhile, a station or a module,
 * as on Now playing. A short one (under 8 s) plays
 * again until stopped; a longer one goes back to the list at its end.
 */
#include "amce.h"
#include "animdec.h"

#include <string.h>
#include <stdio.h>
#include <exec/ports.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/lowlevel.h>

extern struct GfxBase *GfxBase;
extern struct Library *LowLevelBase;

#define ID(a, b, c, d) (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (d))
#define LOOP_MS 8000            /* a whole run shorter than this plays again */

static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

static UWORD be16(const UBYTE *p)
{
    return (UWORD)((p[0] << 8) | p[1]);
}

/* One frame's FORM, as read */
struct Frame {
    const UBYTE *bmhd, *cmap, *body, *dlta, *anhd;
    ULONG cmaplen, bodylen, dltalen, camg;
    BOOL has_camg;
};

static UBYTE *fbuf;             /* the frame being read */
static ULONG fbuf_size;

/* The next FORM ILBM from fh into fbuf, its chunks found; FALSE at the
 * end (or anything that is not one). */
static BOOL read_frame(BPTR fh, struct Frame *f, ULONG *readms)
{
    UBYTE h[12];
    ULONG len, t0 = time_ms();
    const UBYTE *p, *e;
    for (;;) {
        if (Read(fh, h, 12) != 12 || be32(h) != ID('F', 'O', 'R', 'M'))
            return FALSE;
        len = be32(h + 4);
        if (len < 4)
            return FALSE;
        len -= 4;
        if (be32(h + 8) == ID('I', 'L', 'B', 'M'))
            break;
        if (Seek(fh, len + (len & 1), OFFSET_CURRENT) < 0)  /* sound and the like: passed */
            return FALSE;
    }
    if (len > fbuf_size) {
        FreeVec(fbuf);
        fbuf_size = len + 4096;
        if (!(fbuf = AllocVec(fbuf_size, MEMF_ANY))) {
            fbuf_size = 0;
            return FALSE;
        }
    }
    if (Read(fh, fbuf, len) != (LONG)len)
        return FALSE;
    if (len & 1)
        Seek(fh, 1, OFFSET_CURRENT);
    memset(f, 0, sizeof(*f));
    for (p = fbuf, e = fbuf + len; p + 8 <= e;) {
        ULONG id = be32(p), cl = be32(p + 4);
        const UBYTE *d = p + 8;
        if (cl > (ULONG)(e - d))
            cl = e - d;
        if (id == ID('B', 'M', 'H', 'D') && cl >= 20)
            f->bmhd = d;
        else if (id == ID('C', 'M', 'A', 'P')) {
            f->cmap = d;
            f->cmaplen = cl;
        } else if (id == ID('C', 'A', 'M', 'G') && cl >= 4) {
            f->camg = be32(d);
            f->has_camg = TRUE;
        } else if (id == ID('A', 'N', 'H', 'D') && cl >= 24)
            f->anhd = d;
        else if (id == ID('B', 'O', 'D', 'Y')) {
            f->body = d;
            f->bodylen = cl;
        } else if (id == ID('D', 'L', 'T', 'A')) {
            f->dlta = d;
            f->dltalen = cl;
        }
        p = d + ((cl + 1) & ~1UL);
    }
    *readms += time_ms() - t0;
    return TRUE;
}

/* A CMAP as a LoadRGB32 table (n colours of the screen's, the rest black). */
static void cmap_table(const UBYTE *c, ULONG len, ULONG ncol, ULONG *tab)
{
    ULONG i, n = len / 3, lo = 0;
    if (n > ncol)
        n = ncol;
    for (i = 0; i < n * 3; i++)
        lo |= c[i] & 0x0F;
    tab[0] = ncol << 16;
    for (i = 0; i < ncol * 3; i++) {
        ULONG v = i < n * 3 ? c[i] : 0;
        if (!lo)
            v |= v >> 4;                    /* old 4-bit colours, 0xF0: widened */
        tab[1 + i] = v * 0x01010101UL;
    }
    tab[1 + ncol * 3] = 0;
}

/* The display mode for the ANIM's CAMG: as it is if this Amiga has it,
 * else on the default monitor (an NTSC one on a PAL Amiga), else the best
 * one there is for its size, depth and kind. INVALID_ID if none. */
static ULONG pick_mode(ULONG camg, BOOL has_camg, UWORD w, UWORD h, UBYTE depth)
{
    ULONG keys, id, need = 0;
    if (!has_camg)                          /* none said: by its size */
        camg = (w > 400 ? HIRES_KEY : 0) | (h > 300 ? LACE : 0);
    camg &= ~(ULONG)(SPRITES | VP_HIDE | GENLOCK_AUDIO | GENLOCK_VIDEO);
    if (!(camg & 0xFFFF0000UL))
        camg &= ~(ULONG)EXTENDED_MODE;      /* an old 16-bit one */
    keys = camg & (HAM_KEY | EXTRAHALFBRITE_KEY | LACE | HIRES | SUPERHIRES);
    if (!ModeNotAvailable(camg))
        return camg;
    if (!ModeNotAvailable(keys))
        return keys;
    if (camg & HAM_KEY)
        need = DIPF_IS_HAM;
    else if (camg & EXTRAHALFBRITE_KEY)
        need = DIPF_IS_EXTRAHALFBRITE;
    id = BestModeID(BIDTAG_NominalWidth, w, BIDTAG_NominalHeight, h, BIDTAG_Depth, depth,
                    BIDTAG_DIPFMustHave, need,
                    BIDTAG_DIPFMustNotHave, (SPECIAL_FLAGS & ~need) | DIPF_IS_FOREIGN,
                    TAG_DONE);
    return id;
}

/* The mode's deepest bitmap (AGA's 8 planes, ECS's 5 or 6) */
static UWORD mode_depth(ULONG id)
{
    struct DimensionInfo dim;
    if (!GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, id))
        return 0;
    return dim.MaxDepth;
}

/* Its place: centred on the mode's normal (text) area, the display clip
 * as far as the mode's overscan goes. */
static void place(ULONG id, UWORD w, UWORD h, WORD *left, WORD *top, struct Rectangle *clip)
{
    struct Rectangle txt, max;
    WORD tw, th;
    if (!QueryOverscan(id, &txt, OSCAN_TEXT) || !QueryOverscan(id, &max, OSCAN_MAX)) {
        *left = *top = 0;
        clip->MinX = clip->MinY = 0;
        clip->MaxX = w - 1;
        clip->MaxY = h - 1;
        return;
    }
    tw = txt.MaxX - txt.MinX + 1;
    th = txt.MaxY - txt.MinY + 1;
    *left = txt.MinX + (tw - (WORD)w) / 2;
    *top = txt.MinY + (th - (WORD)h) / 2;
    clip->MinX = *left > max.MinX ? *left : max.MinX;
    clip->MinY = *top > max.MinY ? *top : max.MinY;
    clip->MaxX = *left + (WORD)w - 1 < max.MaxX ? *left + (WORD)w - 1 : max.MaxX;
    clip->MaxY = *top + (WORD)h - 1 < max.MaxY ? *top + (WORD)h - 1 : max.MaxY;
}

static void planes_of(struct BitMap *bm, UWORD w, UWORD h, UBYTE depth, struct AnimPlanes *pl)
{
    int i;
    for (i = 0; i < 8; i++)
        pl->plane[i] = i < depth ? (unsigned char *)bm->Planes[i] : NULL;
    pl->stride = bm->BytesPerRow;
    pl->depth = depth;
    pl->rb = ((w + 15) >> 4) << 1;
    if (pl->rb > bm->BytesPerRow)
        pl->rb = bm->BytesPerRow;
    pl->h = h < bm->Rows ? h : bm->Rows;
}

/* The last buffer change's message in: the buffer it let go is free. */
static void safe_done(struct MsgPort *safe, BOOL *wait)
{
    while (safe && (wait[0] || wait[1])) {
        if (GetMsg(safe))
            wait[0] = wait[1] = FALSE;
        else
            Wait(1UL << safe->mp_SigBit);
    }
}

#define VOL_STEP    4           /* of 64, as Now playing's */
#define VOL_DELAY 300           /* ms the stick is held before it repeats */
#define VOL_REPEAT 100          /* ...and then a step every */

static void volume(int d)
{
    int v = mp_volume() + d;
    mp_set_volume(v < 0 ? 0 : v > 64 ? 64 : v);
}

/* Input while it plays: 1 stop, 2 pause or go on, 0 nothing; the volume
 * set here. */
static int input(struct Window *win, ULONG *jlast)
{
    static ULONG held_since, held_step;
    struct IntuiMessage *im;
    int r = 0;
    while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        ULONG cl = im->Class;
        UWORD code = im->Code;
        ReplyMsg((struct Message *)im);
        if (cl == IDCMP_RAWKEY) {
            if (code == 0x45 || code == 0x41 || code == 0x10)   /* Esc, Backspace, Q */
                r = 1;
            else if ((code == 0x40 || code == 0x44 || code == 0x43) && !r)  /* Space, Return */
                r = 2;
            else if (code == 0x4C || code == 0x7A)  /* Up (held: it repeats), the wheel */
                volume(VOL_STEP);
            else if (code == 0x4D || code == 0x7B)
                volume(-VOL_STEP);
        } else if (cl == IDCMP_MOUSEBUTTONS) {
            if (code == MENUDOWN)
                r = 1;
            else if (code == SELECTDOWN && !r)
                r = 2;
        }
    }
    if (LowLevelBase) {
        ULONG st = ReadJoyPort(1), pressed = st & ~*jlast;
        if ((st & JP_TYPE_MASK) == JP_TYPE_JOYSTK || (st & JP_TYPE_MASK) == JP_TYPE_GAMECTLR) {
            if (pressed & JPF_BUTTON_BLUE)
                r = 1;
            else if ((pressed & (JPF_BUTTON_RED | JPF_BUTTON_PLAY)) && !r)
                r = 2;
            if (st & (JPF_JOY_UP | JPF_JOY_DOWN)) {
                ULONG now = time_ms();
                int d = st & JPF_JOY_UP ? VOL_STEP : -VOL_STEP;
                if (pressed & (JPF_JOY_UP | JPF_JOY_DOWN)) {
                    volume(d);
                    held_since = held_step = now;
                } else if (now - held_since >= VOL_DELAY && now - held_step >= VOL_REPEAT) {
                    volume(d);
                    held_step = now;
                }
            }
        }
        *jlast = st;
    }
    return r;
}

LONG anim_play(struct Gfx *g, const char *path, char *err, LONG errmax)
{
    BPTR fh;
    UBYTE head[12];
    struct Frame f;
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    struct ScreenBuffer *sb[2] = { NULL, NULL };
    struct MsgPort *safe = NULL;
    BOOL safe_wait[2] = { FALSE, FALSE };
    struct AnimPlanes pl[2];
    ULONG *tab = NULL, id, camg, ncol, jlast = 0, readms = 0, decms = 0, worst = 0, lates = 0;
    ULONG t0, due, jif, runs = 0, start_ms, loop_pos;
    UWORD w, h, *blank = NULL;
    UBYTE depth, masking, comp;
    WORD left, top;
    struct Rectangle clip;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    LONG result = -1;
    int shown = 0, n, k, quit = 0;
    BOOL paused = FALSE, loop = FALSE;
    char m[200];

    err[0] = 0;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh) {
        amce_strlcpy(err, _("The animation could not be opened"), errmax);
        return -1;
    }
    if (Read(fh, head, 12) != 12 || be32(head) != ID('F', 'O', 'R', 'M') ||
        be32(head + 8) != ID('A', 'N', 'I', 'M') || !read_frame(fh, &f, &readms) ||
        !f.bmhd || !f.body) {
        amce_strlcpy(err, _("Not an IFF ANIM this can play"), errmax);
        goto out;
    }
    loop_pos = 12;
    w = be16(f.bmhd);
    h = be16(f.bmhd + 2);
    depth = f.bmhd[8];
    masking = f.bmhd[9];
    comp = f.bmhd[10];
    camg = f.camg;
    if (!w || !h || !depth || depth > 8 || comp > 1) {
        amce_strlcpy(err, _("Not an IFF ANIM this can play"), errmax);
        goto out;
    }
    if ((id = pick_mode(camg, f.has_camg, w, h, depth)) == (ULONG)INVALID_ID || mode_depth(id) < depth) {
        (void)errmax;
        sprintf(err, _("This Amiga has no screen mode for it (%ld colours, %s)"),
                 (long)(1L << depth), (camg & HAM_KEY) ? "HAM" : (camg & EXTRAHALFBRITE_KEY) ? "EHB" :
                 depth > 6 ? "AGA" : "ECS");
        goto out;
    }
    ncol = 1UL << depth;
    if ((camg & EXTRAHALFBRITE_KEY) && depth == 6)
        ncol = 32;                          /* the other 32 are these at half */
    if (!(tab = AllocVec((ncol * 3 + 2) * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR)) ||
        !(blank = AllocMem(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR)))
        goto nomem;
    if (f.cmap)
        cmap_table(f.cmap, f.cmaplen, ncol, tab);
    else
        tab[0] = ncol << 16;
    place(id, w, h, &left, &top, &clip);
    scr = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, w, SA_Height, h, SA_Depth, depth,
                         SA_Left, left, SA_Top, top, SA_DClip, (ULONG)&clip,
                         SA_Colors32, (ULONG)tab, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
                         SA_Behind, TRUE, SA_Draggable, FALSE, SA_Exclusive, TRUE,
                         SA_Title, (ULONG)"AmigaMCE", TAG_DONE);
    if (!scr || !(sb[0] = AllocScreenBuffer(scr, NULL, SB_SCREEN_BITMAP)) ||
        !(sb[1] = AllocScreenBuffer(scr, NULL, SB_COPY_BITMAP)) || !(safe = CreateMsgPort()))
        goto nomem;
    sb[0]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe;
    sb[1]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe;
    if (!(GetBitMapAttr(sb[0]->sb_BitMap, BMA_FLAGS) & BMF_STANDARD) ||
        !(GetBitMapAttr(sb[1]->sb_BitMap, BMA_FLAGS) & BMF_STANDARD) ||
        GetBitMapAttr(sb[0]->sb_BitMap, BMA_DEPTH) < depth) {
        amce_strlcpy(err, _("Animations play on the Amiga's own screens, and none was found"), errmax);
        goto out;                           /* a graphics card's: no planes to draw into */
    }
    win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, w, WA_Height, h, WA_Backdrop, TRUE, WA_Borderless, TRUE,
                         WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_SimpleRefresh, TRUE,
                         WA_NoCareRefresh, TRUE,
                         WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS, TAG_DONE);
    if (!win)
        goto nomem;
    SetPointer(win, blank, 1, 16, 0, 0);
    planes_of(sb[0]->sb_BitMap, w, h, depth, &pl[0]);
    planes_of(sb[1]->sb_BitMap, w, h, depth, &pl[1]);

    sprintf(m, "%s: %lux%lux%lu, mode %08lx (CAMG %05lx), op %ld", FilePart((STRPTR)path),
            (unsigned long)w, (unsigned long)h, (unsigned long)depth, (unsigned long)id, (unsigned long)camg,
            f.anhd ? (long)f.anhd[0] : 0L);
    amce_log("anim", m);
    if (LowLevelBase)
        jlast = ReadJoyPort(1);

restart:
    /* the first frame, into both buffers: the next changes the second */
    k = shown ^ (runs ? 1 : 0);
    safe_done(safe, safe_wait);
    animdec_body(f.body, f.bodylen, &pl[k], comp, masking);
    if (runs && f.cmap) {                   /* its first colours again */
        cmap_table(f.cmap, f.cmaplen, ncol, tab);
        LoadRGB32(&scr->ViewPort, tab);
    }
    if (runs) {
        while (!ChangeScreenBuffer(scr, sb[k]))
            WaitTOF();
        safe_wait[shown] = TRUE;
        shown = k;
        safe_done(safe, safe_wait);
    }
    BltBitMap(sb[k]->sb_BitMap, 0, 0, sb[k ^ 1]->sb_BitMap, 0, 0, w, h, 0xC0, 0xFF, NULL);
    if (!runs) {
        ScreenToFront(scr);
        shown = 0;
    }
    WaitBlit();
    start_ms = t0 = time_ms();
    jif = f.anhd ? be32(f.anhd + 14) : 0;
    due = 0;
    n = 1;
    for (;;) {
        ULONG td, il, op, bits, dt;
        int r = input(win, &jlast);
        if (r == 1) {
            quit = 1;
            break;
        }
        if (r == 2)
            paused = !paused;
        if (paused) {
            WaitTOF();
            t0 = time_ms() - due;           /* the clock stopped meanwhile */
            continue;
        }
        if (!read_frame(fh, &f, &readms))
            break;                          /* the end */
        op = f.anhd ? f.anhd[0] : 0;
        il = f.anhd ? f.anhd[18] : 0;
        bits = f.anhd ? be32(f.anhd + 20) : 0;
        jif = f.anhd ? be32(f.anhd + 14) : 1;
        k = shown ^ 1;                      /* the buffer not shown: two frames back */
        safe_done(safe, safe_wait);
        td = time_ms();
        if (il == 1)                        /* this one changes the frame before */
            BltBitMap(sb[shown]->sb_BitMap, 0, 0, sb[k]->sb_BitMap, 0, 0, w, h, 0xC0, 0xFF, NULL),
                WaitBlit();
        if (op == 0 && f.body)
            animdec_body(f.body, f.bodylen, &pl[k], comp, masking);
        else if (f.dlta && animdec_delta(f.dlta, f.dltalen, &pl[k], op, bits) == -2) {
            sprintf(err, _("ANIM-%ld compression is not supported"), (long)op);
            quit = 1;
            break;
        }
        dt = time_ms() - td;
        decms += dt;
        if (dt > worst)
            worst = dt;
        /* its time: sixtieths of a second after the last (none: a frame) */
        due += (jif ? jif : 1) * 1000 / 60;
        if ((LONG)(time_ms() - t0) > (LONG)due + 20)
            lates++;
        while ((LONG)(time_ms() - t0) < (LONG)due - 10)
            WaitTOF();
        if (f.cmap) {
            cmap_table(f.cmap, f.cmaplen, ncol, tab);
            LoadRGB32(&scr->ViewPort, tab);
        }
        while (!ChangeScreenBuffer(scr, sb[k]))
            WaitTOF();                      /* the last change not taken yet */
        safe_wait[shown] = TRUE;
        shown = k;
        n++;
    }
    runs++;
    if (!quit && !err[0]) {
        ULONG ms = time_ms() - start_ms;
        if (runs == 1)
            loop = ms < LOOP_MS;
        if (loop && Seek(fh, loop_pos, OFFSET_BEGINNING) >= 0 && read_frame(fh, &f, &readms) &&
            f.body)
            goto restart;
    }
    {
        ULONG ms = time_ms() - start_ms;
        sprintf(m, "%ld frames in %ld ms (%ld a second), decoding %ld ms (%ld at the most), "
                "reading %ld ms, %ld late; played %ld time%s",
                (long)n, (long)ms, ms ? (long)(n * 1000 / ms) : 0L, (long)decms, (long)worst,
                (long)readms, (long)lates, (long)runs, runs == 1 ? "" : "s");
        amce_log("anim", m);
    }
    result = 0;
    goto out;
nomem:
    amce_strlcpy(err, _("Not enough memory for the animation's screen"), errmax);
out:
    if (scr) {
        safe_done(safe, safe_wait);         /* nothing left in flight */
        if (shown != 0 && sb[0]) {          /* the screen's own bitmap back before it closes */
            while (!ChangeScreenBuffer(scr, sb[0]))
                WaitTOF();
            safe_wait[1] = TRUE;
            safe_done(safe, safe_wait);
        }
        if (win)
            CloseWindow(win);
        if (sb[1])
            FreeScreenBuffer(scr, sb[1]);
        if (sb[0])
            FreeScreenBuffer(scr, sb[0]);
        CloseScreen(scr);
        if (g && g->scr)
            ScreenToFront(g->scr);
        if (g && g->win)
            ActivateWindow(g->win);
    }
    if (safe) {
        while (GetMsg(safe))
            ;
        DeleteMsgPort(safe);
    }
    FreeVec(tab);
    if (blank)
        FreeMem(blank, 6 * sizeof(UWORD));
    FreeVec(fbuf);
    fbuf = NULL;
    fbuf_size = 0;
    Close(fh);
    return result;
}
