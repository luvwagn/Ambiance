/*
 * Ambiance - the selected entry's details: read by the loader (loader.c)
 * once the selection rests, the entries next to it read ahead, given to
 * the view that is driven (details.h) as they come. Moved out of main.c
 * (its "preview" code) so that a layout and Classic's pane read the same
 * way and neither's drawing decides what is read.
 *
 * Each entry is two jobs, its text (fact sheet, file details) and its
 * picture, so the text is up at once and the picture follows; what the
 * view waits for goes ahead of the loader's queue. The entries next to the
 * one shown are read ahead, so a steady pace of Down finds each ready.
 * With memory to spare (det_roomy), speed comes first: two entries ahead
 * and one behind are read, and entries shown stay decoded for going back,
 * up to DET_SLOTS jobs. Short of memory, only the one shown and the next.
 * A view that makes its own pictures (a layout: lv_picture, at its size)
 * gets only the text jobs.
 *
 * Copyright (c) 2026 Brian King
 */
#include "details.h"
#include "igame.h"

#include <string.h>
#include <proto/exec.h>

#define DET_SLOTS 12
#define DET_TIGHT 4
#define SETTLE   100            /* ms the selection rests before its details are read */
#define PLEX_SETTLE 150         /* ...a Plex entry's: its picture asked for, then decoded */
#define READY     30            /* ...or shown, if they were read ahead already */

struct Det det = { NULL, -1, FALSE, FALSE, 0, 0, NULL, NULL, FALSE, FALSE, FALSE, FALSE, 1, 0, 0 };

static const struct DetView *V;
static struct LJob *slot[DET_SLOTS];
static ULONG clock_;            /* cache order */

void det_view(const struct DetView *v)
{
    V = v;
}

BOOL det_roomy(void)
{
    return AvailMem(MEMF_ANY) >= 3UL * 1024 * 1024 && AvailMem(MEMF_CHIP) >= 768UL * 1024;
}

static BOOL shots(void)
{
    return V && V->shots && V->shots();
}

int det_kind(struct Page *p, LONG idx, char *path, LONG max, UBYTE *shot)
{
    struct Item *it;
    *shot = 0;
    if (idx < 0 || idx >= p->list.count)
        return 0;
    it = &p->list.items[idx];
    if (it->flags & ITEM_GONE)
        return 0;                           /* not held now: read once it is again */
    if (p->songs && !p->plex) {
        /* a music file or folder: its details, and its picture */
        if ((it->kind != IT_SONG && it->kind != IT_DIR) || beets_page(it->path))
            return 0;                       /* a beets page: no folder to look in */
        amb_strlcpy(path, it->path, max);
        *shot = SHOT_SONG;
        return LJ_SONG;
    }
    if (p->pics) {
        amb_strlcpy(path, it->path, max);
        if (it->kind == IT_PIC || it->kind == IT_ANIM) {
            if (!(it->kind == IT_ANIM && it->sortkey && !strcmp(it->sortkey, "DVD")))
                *shot = SHOT_FILE;          /* an animation: a frame; an MPEG video: an I picture
                                             * through it (mpgthumb.c); a DVD: none */
            return LJ_PICTURE;
        }
        if (it->kind == IT_DIR) {
            if (!(it->flags & ITEM_DISABLED))
                *shot = SHOT_FOLDER;
            return LJ_PICFOLDER;
        }
        return 0;
    }
    if (it->kind != IT_RUN)
        return 0;
    if (p->igame)
        ig_shot_base(it->path, path, max);  /* <folder>/igame: igame.iff, and no .txt */
    else
        item_path(it, path, max);
    *shot = SHOT_AGS;
    return LJ_ENTRY;
}

static void free_slot(int i)
{
    struct LJob *j = slot[i];
    if (!j)
        return;
    if (j->busy)
        ldr_wait(j);
    pic_dispose(j->obj);
    thumb_free(j->thumb);
    FreeVec(j->text);
    if (det.tj == j)
        det.tj = NULL;
    if (det.sj == j)
        det.sj = NULL;
    FreeVec(j);
    slot[i] = NULL;
}

void det_flush(void)
{
    int i;
    for (i = 0; i < DET_SLOTS; i++)
        free_slot(i);
    det.tj = det.sj = NULL;
}

/* Let go without waiting: one still out is orphaned (one not begun never
 * runs, one begun is freed when it ends), if orphan (it holds nothing of
 * the screen's: a colour-table screen decodes unremapped, no pens).
 * Waiting could take minutes: every job queued ran first. */
void det_let_go(BOOL orphan)
{
    int i;
    for (i = 0; i < DET_SLOTS; i++) {
        struct LJob *j = slot[i];
        if (j && j->busy && orphan) {
            ldr_orphan(j);
            slot[i] = NULL;
        } else {
            free_slot(i);
        }
    }
    det.tj = det.sj = NULL;
}

static BOOL current(const struct LJob *j)
{
    return j == det.tj || j == det.sj;
}

struct LJob *det_request(int type, UBYTE mode, const char *path, BOOL ahead, BOOL first)
{
    int i, free_i = -1;
    struct LJob *j;
    int limit = det_roomy() ? DET_SLOTS : DET_TIGHT, used = 0;
    for (i = 0; i < DET_SLOTS; i++) {
        j = slot[i];
        if (j && j->type == type && j->mode == mode && strcmp(j->path, path) == 0) {
            if (!j->busy && (j->spent || !j->ran)) {
                free_slot(i);           /* its picture went to the screen, or it was
                                         * skipped: read it again */
                break;
            }
            j->cancel = FALSE;
            j->stamp = ++clock_;
            if (first && j->busy)
                ldr_submit_first(j);    /* not started yet: to the front */
            return j;
        }
    }
    for (i = 0; i < DET_SLOTS; i++)
        if (slot[i])
            used++;
    for (i = 0; i < DET_SLOTS && free_i < 0 && used < limit; i++)
        if (!slot[i])
            free_i = i;
    while (free_i < 0) {
        /* the longest unused one that is back */
        int old = -1;
        for (i = 0; i < DET_SLOTS; i++)
            if (slot[i] && !slot[i]->busy && !current(slot[i]) &&
                (old < 0 || slot[i]->stamp < slot[old]->stamp))
                old = i;
        if (old < 0)
            break;
        free_slot(old);
        if (--used < limit)
            free_i = old;
    }
    if (free_i < 0) {
        /* every one is still out: a read-ahead waits its turn; the one
         * asked for goes past the limit (never waited for here) */
        if (ahead)
            return NULL;
        for (i = 0; i < DET_SLOTS && free_i < 0; i++)
            if (!slot[i])
                free_i = i;
        if (free_i < 0)
            return NULL;
    }
    if (!(j = AllocVec(sizeof(*j), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    j->type = type;
    j->mode = mode;
    amb_strlcpy(j->path, path, sizeof(j->path));
    j->want_pic = FALSE;                    /* pictures are LJ_SHOT jobs of their own */
    if (V && V->job)
        V->job(j);                          /* the screen's (LJ_SHOT: made at the view's size) */
    j->stamp = ++clock_;
    slot[free_i] = j;
    if (first)
        ldr_submit_first(j);
    else
        ldr_submit(j);
    return j;
}

void det_release(struct LJob *j)
{
    int i;
    for (i = 0; i < DET_SLOTS; i++)
        if (j && slot[i] == j)
            free_slot(i);
}

void det_first(UBYTE shot, const char *path, char *key)
{
    int i;
    key[0] = 0;
    for (i = 0; i < DET_SLOTS; i++) {
        struct LJob *j = slot[i];
        if (j && j->done && j->first[0] && !strcmp(j->path, path) &&
            (j->type == LJ_PICFOLDER || j->type == LJ_SONG || (j->type == LJ_SHOT && j->mode == shot))) {
            amb_strlcpy(key, j->first, 300);
            return;
        }
    }
}

/* The entries around this one read ahead: the next one the way the list
 * moves; with memory to spare, the one after it and the one behind too */
static void read_ahead(struct Page *p)
{
    static const LONG near[] = { 1, 2, -1 };
    char path[300];
    int i, n = det_roomy() ? 3 : 1;
    UBYTE shot;
    for (i = 0; i < n; i++) {
        int k = det_kind(p, p->sel + near[i] * det.dir, path, sizeof(path), &shot);
        if (!k || !det_request(k, 0, path, TRUE, FALSE))
            break;
        if (shot && shots() && !det_request(LJ_SHOT, shot, path, TRUE, FALSE))
            break;
    }
}

void det_progress(struct Page *p)
{
    char path[300];
    UBYTE shot;
    int k;
    if (det.tj && det.tj->done && !det.tj->ran) {      /* skipped: ask again */
        k = det_kind(p, p->sel, path, sizeof(path), &shot);
        det_release(det.tj);
        det.tj = k ? det_request(k, 0, path, FALSE, TRUE) : NULL;
    }
    if (det.sj && det.sj->done && !det.sj->ran) {
        k = det_kind(p, p->sel, path, sizeof(path), &shot);
        det_release(det.sj);
        det.sj = shot ? det_request(LJ_SHOT, shot, path, FALSE, TRUE) : NULL;
    }
    if (det.tj && det.tj->done && !det.text_shown) {
        /* an entry without a screenshot, an empty folder: no picture */
        if (det.has_box && det.tj->type != LJ_PICTURE && det.tj->np <= 0) {
            det.has_box = FALSE;
            if (det.sj)
                det.sj->cancel = TRUE;
            det.sj = NULL;
            if (V->no_picture)
                V->no_picture(p);
        }
        det.text_shown = TRUE;
        if (V->text)
            V->text(p);
    }
    if (det.text_shown && det.sj && det.sj->done && !det.pic_shown) {
        det.pic_shown = TRUE;
        if (V->picture)
            V->picture(p);
    }
    if (det.text_shown && (det.pic_shown || !det.has_box)) {
        if (!det_roomy()) {
            struct LJob *t = det.tj, *s = det.sj;
            det.tj = det.sj = NULL;         /* short of memory: keep only what is ahead */
            det_release(t);
            det_release(s);
        }
        read_ahead(p);
    }
}

void det_begin(struct Page *p)
{
    char path[300];
    UBYTE shot = 0;
    int k;
    BOOL keep, was_shown = det.shown && det.page == p;
    det.pending = FALSE;
    det.page = p;
    det.sel = p->sel;
    det.shown = TRUE;
    det.tj = det.sj = NULL;
    det.text_shown = det.pic_shown = FALSE;
    if (V->start)
        V->start(p);
    if (p->plex) {
        if (V->plex)
            V->plex(p, was_shown);          /* the server's details */
        return;
    }
    k = p->list.count ? det_kind(p, p->sel, path, sizeof(path), &shot) : 0;
    if (!shots())
        shot = 0;                           /* (the view's own pictures, at its own sizes) */
    det.has_box = shot != 0;
    keep = shot && V->keeps && V->keeps(p, p->sel);    /* the picture on screen is this one's */
    if (!det.since)
        det.since = time_ms();
    if (k) {
        if (shot && !keep)
            det.sj = det_request(LJ_SHOT, shot, path, FALSE, TRUE);
        det.tj = det_request(k, 0, path, FALSE, TRUE);     /* last in: first out */
    }
    if (keep && det.tj)
        det.pic_shown = TRUE;
    if (!det.tj) {
        if (V->empty)
            V->empty(p);
        return;
    }
    if (det.tj->done && (!det.sj || det.sj->done))
        det.ready++;
    else
        det.waited++;
    if (V->begin)
        V->begin(p, keep);
    det_progress(p);
    if (V->tick)
        V->tick(p);                         /* waited a while already: the spinner now */
}

void det_touch(struct Page *p)
{
    char path[300];
    UBYTE shot;
    int k, i, found = 0, want;
    ULONG settle = SETTLE;
    if (!V || !V->has_pane(p) || (det.shown && det.page == p && det.sel == p->sel))
        return;                             /* already showing this one */
    if (p->style_pv) {
        det.page = p;                       /* no reading: shown at once */
        det.sel = p->sel;
        det.shown = TRUE;
        det.pending = FALSE;
        if (V->style)
            V->style(p);
        return;
    }
    if (p->plex)
        settle = app_plex_art_kept(p, p->sel) ? READY   /* fetched ahead: only a decode */
                                              : PLEX_SETTLE;    /* asked of the server: rest first */
    /* read ahead and back already: showing it is only a copy, so sooner */
    if ((k = det_kind(p, p->sel, path, sizeof(path), &shot))) {
        want = shot && shots() ? 2 : 1;
        for (i = 0; i < DET_SLOTS; i++) {
            struct LJob *j = slot[i];
            if (j && j->done && j->ran && !j->spent && strcmp(j->path, path) == 0 &&
                ((j->type == k && !j->mode) || (j->type == LJ_SHOT && j->mode == shot)))
                found++;
        }
        if (found >= want)
            settle = READY;
    }
    if (!det.shown && p->plex && app_plex_movable(p, p->sel))
        settle = READY;                     /* its cover is on screen: only moved */
    /* chosen by a click: not read until a second click could have come
     * (reading a big file, the emulator's Amiga stops a moment, and a
     * click made meanwhile is lost: double-clicks did nothing) */
    if (det.clicked && settle < app_dc_time())
        settle = app_dc_time();
    else if (!det.shown && V->clear_picture && !(V->keeps && V->keeps(p, p->sel)))
        /* a page just shown (opened, or back to it): a picture left from
         * the page before goes now, unless it is this entry's too */
        V->clear_picture(p);
    if (det.shown && det.page == p && !app_moving() && !(V->keeps && V->keeps(p, p->sel))) {
        /* the entry before's picture goes now, not when this one's comes:
         * it never stands next to this one's text */
        if (V->clear_picture)
            V->clear_picture(p);
        if (settle != READY) {
            /* not read yet: its text goes now too */
            if (V->clear_all)
                V->clear_all(p);
            det.shown = FALSE;
            det.tj = det.sj = NULL;
        }
    }
    det.pending = TRUE;
    det.since = time_ms();
    det.due = det.since + settle;
}

void det_tick(void)
{
    struct Page *p = app_cur();
    if (!det.pending) {
        if (p)
            app_plex_ahead(p);              /* at rest: the neighbours' pictures */
        if (p && det.shown && det.page == p && det.sel == p->sel && !det.tj && !det.sj && !p->plex &&
            !p->style_pv && V && V->has_pane(p)) {
            /* shown as having none: has it some now? (a list come back to
             * at the start: its entries from the index, not yet held - the
             * restored one was given none, and nothing asked again until a
             * move) */
            char path[300];
            UBYTE shot;
            if (det_kind(p, p->sel, path, sizeof(path), &shot)) {
                det.shown = FALSE;
                det_touch(p);
            }
        }
        return;
    }
    if (!p || !V || !V->has_pane(p) || app_pane_pending()) {
        det.pending = FALSE;
        return;
    }
    if (app_moving() || (LONG)(time_ms() - det.due) < 0)
        return;                             /* still moving, or the letter is up */
    det_begin(p);
}

BOOL det_due(void)
{
    return (det.tj && det.tj->done && !det.text_shown) || (det.sj && det.sj->done && !det.pic_shown);
}

void det_arrived(struct Page *p)
{
    if (!det.tj && !det.sj)
        return;
    if (!((det.tj && det.tj->done && !det.text_shown) || (det.sj && det.sj->done && !det.pic_shown) ||
          (det.tj && det.tj->done && !det.tj->ran)))
        return;
    if (p && p == det.page && p->sel == det.sel && V && V->has_pane(p) && !app_pane_pending() &&
        !app_moving())
        det_progress(p);
    else if (p && (p != det.page || p->sel != det.sel)) {
        det.shown = FALSE;                  /* moved on: read again when it rests */
        det.tj = det.sj = NULL;
        det_touch(p);
    }
}
