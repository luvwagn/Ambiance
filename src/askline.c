/*
 * Ambiance - a line typed in: a small window over Ambiance's screen
 * Copyright (c) 2026 Brian King
 *
 * Ambiance is driven by keys and the joystick, with nothing typed, until
 * Jellyfin: a server's address, a user name and a password. A GadTools
 * string gadget shows what is typed, so a password would show; this
 * window takes keys itself (IDCMP_VANILLAKEY) and draws them, a password
 * as dots. Return keeps the line, Esc (or the close gadget) gives up.
 * The caller stops double buffering first, as for ASL's requesters.
 */
#include "amb.h"

#include <string.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

static void draw_field(struct Window *w, struct DrawInfo *di, WORD x, WORD y, WORD fw, WORD fh,
                       const char *line, BOOL secret)
{
    struct RastPort *rp = w->RPort;
    char shown[130];
    LONG n = (LONG)strlen(line), i, fit;
    UWORD tp = di->dri_Pens[TEXTPEN], bp = di->dri_Pens[BACKGROUNDPEN];
    if (secret) {
        for (i = 0; i < n && i < (LONG)sizeof(shown) - 1; i++)
            shown[i] = '*';
        shown[i] = 0;
    } else {
        amb_strlcpy(shown, line, sizeof(shown));
    }
    /* the end of it shown, with a cursor, where it is longer than the field */
    amb_strlcat(shown, "_", sizeof(shown));
    n = (LONG)strlen(shown);
    for (fit = 0; fit < n && TextLength(rp, (STRPTR)shown + n - fit - 1, fit + 1) < fw - 8; fit++)
        ;
    SetAPen(rp, bp);
    RectFill(rp, x + 2, y + 2, x + fw - 3, y + fh - 3);
    SetAPen(rp, di->dri_Pens[SHADOWPEN]);
    Move(rp, x, y + fh - 1);
    Draw(rp, x, y);
    Draw(rp, x + fw - 1, y);
    SetAPen(rp, di->dri_Pens[SHINEPEN]);
    Draw(rp, x + fw - 1, y + fh - 1);
    Draw(rp, x, y + fh - 1);
    SetAPen(rp, tp);
    SetBPen(rp, bp);
    SetDrMd(rp, JAM2);
    Move(rp, x + 4, y + (fh - rp->TxHeight) / 2 + rp->TxBaseline);
    Text(rp, (STRPTR)shown + n - fit, fit);
}

BOOL ask_line(struct Screen *scr, const char *title, const char *prompt, char *buf, LONG max,
              BOOL secret)
{
    struct Window *w;
    struct DrawInfo *di;
    struct RastPort *rp;
    char line[128];
    WORD fh, ww, wh, x, y, fw, th;
    BOOL ok = FALSE, done = FALSE;
    if (!scr || !(di = GetScreenDrawInfo(scr)))
        return FALSE;
    amb_strlcpy(line, buf, (LONG)sizeof(line) < max ? (LONG)sizeof(line) : max);
    th = scr->Font ? scr->Font->ta_YSize : 8;
    fh = th + 8;
    ww = scr->Width / 2 < 300 ? (scr->Width < 320 ? scr->Width : 300) : scr->Width / 2;
    if (ww > 560)
        ww = 560;
    wh = scr->BarHeight + 1 + th * 3 + fh + 24;
    w = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)scr,
                       WA_Left, (scr->Width - ww) / 2, WA_Top, (scr->Height - wh) / 3,
                       WA_Width, ww, WA_Height, wh, WA_Title, (ULONG)title,
                       WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
                       WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_NoCareRefresh, TRUE,
                       WA_SmartRefresh, TRUE,
                       WA_IDCMP, IDCMP_VANILLAKEY | IDCMP_CLOSEWINDOW, TAG_DONE);
    if (!w) {
        FreeScreenDrawInfo(scr, di);
        return FALSE;
    }
    rp = w->RPort;
    SetFont(rp, di->dri_Font);
    th = rp->TxHeight;
    x = w->BorderLeft + 10;
    fw = w->Width - w->BorderLeft - w->BorderRight - 20;
    y = w->BorderTop + 6;
    SetAPen(rp, di->dri_Pens[TEXTPEN]);
    SetDrMd(rp, JAM1);
    Move(rp, x, y + rp->TxBaseline);
    Text(rp, (STRPTR)prompt, strlen(prompt));
    y += th + 6;
    draw_field(w, di, x, y, fw, fh, line, secret);
    {
        const char *hint = _("Return: OK   Esc: cancel");
        SetAPen(rp, di->dri_Pens[TEXTPEN]);
        SetDrMd(rp, JAM1);
        Move(rp, x, y + fh + 6 + rp->TxBaseline);
        Text(rp, (STRPTR)hint, strlen(hint));
    }
    while (!done) {
        struct IntuiMessage *m;
        WaitPort(w->UserPort);
        while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) {
            ULONG cl = m->Class;
            UWORD code = m->Code;
            ReplyMsg((struct Message *)m);
            if (cl == IDCMP_CLOSEWINDOW) {
                done = TRUE;
            } else if (cl == IDCMP_VANILLAKEY) {
                LONG n = (LONG)strlen(line);
                if (code == 13) {
                    ok = done = TRUE;
                } else if (code == 27) {
                    done = TRUE;
                } else if (code == 8) {
                    if (n)
                        line[n - 1] = 0;
                } else if (code == 24) {
                    line[0] = 0;            /* (Ctrl-X: all of it) */
                } else if (code >= 32 && code != 127 && n < max - 1 && n < (LONG)sizeof(line) - 1) {
                    line[n] = (char)code;
                    line[n + 1] = 0;
                }
                if (!done)
                    draw_field(w, di, x, y, fw, fh, line, secret);
            }
        }
    }
    CloseWindow(w);
    FreeScreenDrawInfo(scr, di);
    if (ok)
        amb_strlcpy(buf, line, max);
    memset(line, 0, sizeof(line));          /* (a password not left about) */
    return ok;
}
