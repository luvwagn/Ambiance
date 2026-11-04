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
#include <graphics/modeid.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* Two loader processes: one for pictures (decoding a big JPEG takes
 * seconds on a 68020), one for everything else (folders, fact sheets),
 * so a listing or a fact sheet never waits behind a picture being
 * decoded. Each runs a job the UI waits for (ldr_submit_first) at
 * PRI_URGENT, and reading ahead below that: the other one's reading ahead
 * (All Games' letters, the next pictures) never holds up what is wanted
 * now. The picture one reads ahead lowest.
 *
 * A picture the UI waits for goes to a third process of its own (W_NOW):
 * a picture being read ahead cannot be stopped once begun (a JPEG takes
 * seconds), and the one wanted now no longer waits for it to end. At
 * PRI_URGENT it simply runs, the reading ahead paused below it. */
#define PRI_URGENT (-1)
enum { W_QUICK, W_PICS, W_NOW, WORKERS };
static struct Worker {
    struct MsgPort *port;           /* its own, made by it */
    struct Task *task;
    const char *name;
    LONG pri;
} workers[WORKERS] = {
    { NULL, NULL, "AmigaMCE loader", -2 },
    { NULL, NULL, "AmigaMCE pictures", -3 },
    { NULL, NULL, "AmigaMCE picture now", PRI_URGENT },
};
static struct Worker *starting;     /* the one being started (its process reads it) */
static struct MsgPort *reply_port;  /* ours */
static struct Task *parent;
static BYTE start_sig = -1;
static BOOL running;
struct Task *ldr_task, *ldr_task2, *ldr_task3;     /* the processes (log: which task
                                                     * a guru names) */
static LONG out;                    /* jobs sent and not back */

/* Which process does job j: pictures to a picture one, the one wanted
 * now (urgent) to its own. */
static BOOL is_picture(const struct LJob *j)
{
    return j->type == LJ_SHOT || (j->want_pic && (j->type == LJ_PICTURE || j->type == LJ_ENTRY ||
                                                  j->type == LJ_PICFOLDER));
}

static struct Worker *worker_for(const struct LJob *j)
{
    if (is_picture(j))
        return &workers[j->urgent ? W_NOW : W_PICS];
    return &workers[W_QUICK];
}

/* Decode picture src for job j. With tw (a colour-table screen), it is
 * decoded as it is and made small in colours of its own (thumb.c): the
 * pane's picture always, the viewer's one only when its colours do not
 * fit the screen's (else it is shown in them, register for register). */
static void decode(struct LJob *j, const char *src)
{
    ULONG t0 = time_ms(), dw, dh;
    if (j->tw && !j->viewfit &&
        (j->thumb = ilbm_thumb(src, j->tw, j->th, j->tall, TRUE, j->maxcol, &j->info))) {
        j->ms_decode = time_ms() - t0;      /* an ILBM: read here, no datatype */
        j->own = TRUE;
        return;
    }
    j->obj = pic_decode(src, j->tw ? NULL : j->scr, j->remap, &j->remapped, &j->info);
    j->ms_decode = time_ms() - t0;
    if (!j->obj || !j->tw || j->remapped || j->cancel)
        return;
    if (j->viewfit) {
        if (j->info.depth <= 8 && !(j->info.mode & HAM_KEY) &&
            (1UL << j->info.depth) <= (ULONG)j->maxcol + 1)
            return;                     /* fits as it is */
        view_fit(j->tw, j->th, j->tall, &j->info, &dw, &dh, NULL, 0);
    } else {
        pic_fit(j->info.w, j->info.h, j->tall, j->tw, j->th, TRUE, &dw, &dh);
    }
    t0 = time_ms();
    j->thumb = thumb_make(j->obj, dw, dh, j->maxcol);
    j->ms_thumb = time_ms() - t0;
    if (j->thumb || (!j->viewfit && j->info.depth > 8)) {
        pic_dispose(j->obj);            /* a deep one is no use to the pane without */
        j->obj = NULL;
    }
}

static void run(struct LJob *j)
{
    BPTR l;
    if (j->type == LJ_ENTRY) {
        char shot[300];
        j->nlines = ags_read_txt(j->path, j->lines, LJ_FACTS);
        j->np = ags_screenshot_path(j->path, shot, sizeof(shot));  /* has one (LJ_SHOT) */
        if (j->want_pic && !j->cancel && j->np)
            decode(j, shot);
        return;
    }
    if (j->type == LJ_SHOT) {
        /* the picture alone: an entry's screenshot, a picture, or a
         * folder's first picture (its text is a job of its own) */
        char shot[300];
        const char *src = j->path;
        LONG nd;
        if (j->mode == SHOT_AGS) {
            if (!ags_screenshot_path(j->path, shot, sizeof(shot)))
                return;
            src = shot;
        } else if (j->mode == SHOT_FOLDER) {
            if (pics_scan(j->path, &nd, shot, sizeof(shot)) <= 0)
                return;
            src = shot;
        }
        if (!j->cancel)
            decode(j, src);
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
            decode(j, j->path);
        return;
    }
    if (j->type == LJ_FOLDER) {
        /* small puddles: a letter's names, kept apart so they can be let go */
        if (!list_init_puddle(&j->list, 2048)) {
            j->np = -1;
            return;
        }
        j->np = -1;
        if (j->cache[0]) {
            j->np = idx_read_block(j->cache, j->off, j->len, j->path, &j->list);
            if (j->np >= 0) {
                list_sort(&j->list);
                return;
            }
            list_free(&j->list);        /* the cache let us down: the folder, then */
            j->cache[0] = 0;
            if (!list_init_puddle(&j->list, 2048))
                return;
        }
        if (!idx_date(j->path, &j->date))
            memset(&j->date, 0, sizeof(j->date));
        j->np = ags_read_dir(j->path, &j->list, j->adult);
        return;
    }
    if (j->type == LJ_PICDIR) {
        j->np = list_init(&j->list) ? pics_read_dir(j->path, &j->list) : -1;
        return;
    }
    if (j->type == LJ_WRITE) {
        j->np = idx_save(j->path, j->buf, j->len) ? 1 : -1;
    }
    if (j->type == LJ_PICFOLDER) {
        j->np = pics_scan(j->path, &j->nd, j->first, sizeof(j->first));
        if (j->np > 0 && j->want_pic && !j->cancel)
            decode(j, j->first);
    }
}

static void loader_main(void)
{
    struct Worker *w = starting;
    struct MsgPort *port = CreateMsgPort();
    w->port = port;
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
                SetTaskPri((struct Task *)FindTask(NULL), j->urgent ? PRI_URGENT : w->pri);
                j->t_begun = time_ms();
                run(j);
                j->ran = TRUE;
            }
            ReplyMsg(&j->msg);
        }
    }
}

static BOOL start_worker(struct Worker *w)
{
    w->port = NULL;
    starting = w;
    SetSignal(0, 1UL << start_sig);
    if (!(w->task = (struct Task *)CreateNewProcTags(NP_Entry, (ULONG)loader_main,
                                                      NP_Name, (ULONG)w->name,
                                                      NP_StackSize, 65536, NP_Priority, w->pri,
                                                      NP_WindowPtr, (ULONG)-1, TAG_END)))
        return FALSE;
    Wait(1UL << start_sig);
    return w->port != NULL;
}

static void stop_worker(struct Worker *w);

BOOL ldr_start(void)
{
    if (running)
        return TRUE;
    parent = FindTask(NULL);
    if (!reply_port && !(reply_port = CreateMsgPort()))
        return FALSE;
    if ((start_sig = AllocSignal(-1)) < 0)
        return FALSE;
    {
        int i;
        running = TRUE;
        for (i = 0; i < WORKERS && running; i++)
            if (!start_worker(&workers[i])) {
                while (--i >= 0)
                    stop_worker(&workers[i]);
                running = FALSE;
            }
    }
    FreeSignal(start_sig);
    start_sig = -1;
    ldr_task = workers[W_QUICK].task;
    ldr_task2 = workers[W_PICS].task;
    ldr_task3 = workers[W_NOW].task;
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
    j->urgent = FALSE;
    j->t_sent = time_ms();
    if (!running) {
        run(j);                         /* no loader: now, here */
        j->ran = j->done = TRUE;
        return;
    }
    j->busy = TRUE;
    out++;
    j->msg.mn_ReplyPort = reply_port;
    j->msg.mn_Length = sizeof(*j);
    PutMsg(worker_for(j)->port, &j->msg);
}

/* As ldr_submit, but ahead of every job not started yet: what the UI is
 * waiting to show goes before what it reads ahead. Also for a job already
 * sent: it moves to the front if it has not been started. */
void ldr_submit_first(struct LJob *j)
{
    struct Node *n;
    struct MsgPort *job_port;
    if (!running) {
        ldr_submit(j);
        return;
    }
    if (j->busy) {
        /* sent already: to the front of the queue of the process for what
         * is wanted now, or, begun, hurried where it is */
        struct Worker *was = worker_for(j), *now;
        BOOL queued = FALSE;
        Forbid();
        for (n = was->port->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ)
            if (n == &j->msg.mn_Node) {
                Remove(n);
                queued = TRUE;
                break;
            }
        j->urgent = TRUE;
        now = worker_for(j);
        if (queued) {
            AddHead(&now->port->mp_MsgList, &j->msg.mn_Node);
            Signal(now->port->mp_SigTask, 1UL << now->port->mp_SigBit);
        } else if (!j->done && was->task) {
            SetTaskPri(was->task, PRI_URGENT);  /* begun: hurry it */
        }
        Permit();
        return;
    }
    j->urgent = TRUE;
    job_port = worker_for(j)->port;
    j->done = FALSE;
    j->ran = FALSE;
    j->t_sent = time_ms();
    j->busy = TRUE;
    out++;
    j->msg.mn_ReplyPort = reply_port;
    j->msg.mn_Length = sizeof(*j);
    j->msg.mn_Node.ln_Type = NT_MESSAGE;
    Forbid();
    AddHead(&job_port->mp_MsgList, &j->msg.mn_Node);
    Permit();
    Signal(job_port->mp_SigTask, 1UL << job_port->mp_SigBit);
}

/* A job nobody waits for any more (ldr_orphan), back: freed with all it
 * brought. */
static void orphan_free(struct LJob *j)
{
    if (j->list.pool)
        list_free(&j->list);
    pic_dispose(j->obj);
    thumb_free(j->thumb);
    FreeVec(j->buf);
    FreeVec(j);
}

void ldr_orphan(struct LJob *j)
{
    if (!j)
        return;
    if (!j->busy) {
        orphan_free(j);
        return;
    }
    j->orphan = TRUE;
    j->cancel = TRUE;                   /* if it has not started, it never will */
}

struct LJob *ldr_collect(void)
{
    struct LJob *j;
    while (reply_port && (j = (struct LJob *)GetMsg(reply_port))) {
        j->busy = FALSE;
        j->done = TRUE;
        out--;
        if (j->orphan) {
            orphan_free(j);
            continue;
        }
        return j;
    }
    return NULL;
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

static void stop_worker(struct Worker *w)
{
    static struct LJob quit;            /* static: a job is too big for the stack */
    if (!w->port)
        return;
    memset(&quit, 0, sizeof(quit));
    quit.type = LJ_QUIT;
    quit.msg.mn_ReplyPort = reply_port;
    quit.msg.mn_Length = sizeof(quit);
    PutMsg(w->port, &quit.msg);
    while (!quit.done) {
        struct LJob *j;
        WaitPort(reply_port);
        while ((j = (struct LJob *)GetMsg(reply_port))) {
            if (j != &quit)
                out--;
            j->busy = FALSE;
            j->done = TRUE;
            if (j->orphan)
                orphan_free(j);
        }
    }
    w->port = NULL;
    w->task = NULL;
}

void ldr_stop(void)
{
    if (running) {
        int i;
        for (i = WORKERS - 1; i >= 0; i--)
            stop_worker(&workers[i]);
        running = FALSE;
    }
    if (reply_port) {
        DeleteMsgPort(reply_port);
        reply_port = NULL;
    }
}
