/*
 * AmigaMCE - the loader: a second process that does the disk work
 * Copyright (c) 2026 Brian King
 *
 * Reading a fact sheet and decoding a screenshot take long enough on a
 * real Amiga that the list would stall if the UI did it: a key pressed
 * meanwhile waits until it is done. So the UI hands that work to this
 * process, at a lower priority, and goes on taking input; the result comes
 * back as a message. It also reads ahead: the entry after the one shown,
 * in the direction the list is moving, so a steady Down, Down, Down finds
 * each one ready.
 *
 * A job is the UI's: it fills one in, ldr_submit sends it, and it comes
 * back marked done (ldr_collect, ldr_wait). Pictures are decoded without
 * remapping on screens with a colour table, so one read ahead holds no
 * pens; it gets them when it is drawn (gfx_picture_obj).
 *
 * Without the process (it could not start) jobs run at once, in the UI.
 */
#include "amce.h"

#include <string.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

static struct MsgPort *job_port;    /* the loader's, made by the loader */
static struct MsgPort *reply_port;  /* ours */
static struct Task *parent;
static BYTE start_sig = -1;
static BOOL running;
static LONG out;                    /* jobs sent and not back */

static void run(struct LJob *j)
{
    BPTR l;
    if (j->type == LJ_ENTRY) {
        char shot[300];
        j->nlines = ags_read_txt(j->path, j->lines, LJ_FACTS);
        if (j->want_pic && !j->cancel && ags_screenshot_path(j->path, shot, sizeof(shot)))
            j->obj = pic_decode(shot, j->scr, j->remap, &j->remapped, &j->info);
        return;
    }
    if (j->type == LJ_PICTURE) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        j->size = -1;
        if (fib && (l = Lock((CONST_STRPTR)j->path, SHARED_LOCK))) {
            if (Examine(l, fib)) {
                j->size = fib->fib_Size;
                j->days = fib->fib_Date.ds_Days;
            }
            UnLock(l);
        }
        if (fib)
            FreeDosObject(DOS_FIB, fib);
        if (j->want_pic && !j->cancel)
            j->obj = pic_decode(j->path, j->scr, j->remap, &j->remapped, &j->info);
        return;
    }
    if (j->type == LJ_PICFOLDER) {
        j->np = pics_scan(j->path, &j->nd, j->first, sizeof(j->first));
        if (j->np > 0 && j->want_pic && !j->cancel)
            j->obj = pic_decode(j->first, j->scr, j->remap, &j->remapped, &j->info);
    }
}

static void loader_main(void)
{
    struct MsgPort *port = CreateMsgPort();
    job_port = port;
    Signal(parent, 1UL << start_sig);
    if (!port)
        return;
    for (;;) {
        struct LJob *j;
        WaitPort(port);
        while ((j = (struct LJob *)GetMsg(port))) {
            if (j->type == LJ_QUIT) {
                DeleteMsgPort(port);
                Forbid();               /* gone before AmigaMCE can unload */
                ReplyMsg(&j->msg);
                return;
            }
            if (!j->cancel) {
                run(j);
                j->ran = TRUE;
            }
            ReplyMsg(&j->msg);
        }
    }
}

BOOL ldr_start(void)
{
    if (running)
        return TRUE;
    parent = FindTask(NULL);
    if (!reply_port && !(reply_port = CreateMsgPort()))
        return FALSE;
    if ((start_sig = AllocSignal(-1)) < 0)
        return FALSE;
    SetSignal(0, 1UL << start_sig);
    job_port = NULL;
    if (!CreateNewProcTags(NP_Entry, (ULONG)loader_main, NP_Name, (ULONG)"AmigaMCE loader",
                           NP_StackSize, 32768, NP_Priority, -1,
                           NP_WindowPtr, (ULONG)-1, TAG_END)) {
        FreeSignal(start_sig);
        start_sig = -1;
        return FALSE;
    }
    Wait(1UL << start_sig);
    FreeSignal(start_sig);
    start_sig = -1;
    running = job_port != NULL;
    return running;
}

ULONG ldr_sigmask(void)
{
    return reply_port ? 1UL << reply_port->mp_SigBit : 0;
}

void ldr_submit(struct LJob *j)
{
    j->done = FALSE;
    j->ran = FALSE;
    if (!running) {
        run(j);                         /* no loader: now, here */
        j->ran = j->done = TRUE;
        return;
    }
    j->busy = TRUE;
    out++;
    j->msg.mn_ReplyPort = reply_port;
    j->msg.mn_Length = sizeof(*j);
    PutMsg(job_port, &j->msg);
}

struct LJob *ldr_collect(void)
{
    struct LJob *j = reply_port ? (struct LJob *)GetMsg(reply_port) : NULL;
    if (j) {
        j->busy = FALSE;
        j->done = TRUE;
        out--;
    }
    return j;
}

LONG ldr_out(void)
{
    return out;
}

void ldr_wait(struct LJob *j)
{
    while (j->busy) {
        WaitPort(reply_port);
        while (ldr_collect())
            ;
    }
}

void ldr_stop(void)
{
    static struct LJob quit;            /* static: a job is too big for the stack */
    if (running) {
        memset(&quit, 0, sizeof(quit));
        quit.type = LJ_QUIT;
        quit.msg.mn_ReplyPort = reply_port;
        quit.msg.mn_Length = sizeof(quit);
        PutMsg(job_port, &quit.msg);
        while (!quit.done) {
            WaitPort(reply_port);
            {
                struct LJob *j;
                while ((j = (struct LJob *)GetMsg(reply_port))) {
                    if (j != &quit)
                        out--;
                    j->busy = FALSE;
                    j->done = TRUE;
                }
            }
        }
        running = FALSE;
        job_port = NULL;
    }
    if (reply_port) {
        DeleteMsgPort(reply_port);
        reply_port = NULL;
    }
}
