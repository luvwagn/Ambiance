/*
 * AmigaMCE - DVD-Video discs: found, and their main film played
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Finding them: the DOS list's volumes that belong to a CD/DVD filesystem
 * (its handler's name or DOS type says so: ODFileSystem, CDFileSystem,
 * a UDF handler...) and have VIDEO_TS/VIDEO_TS.IFO. Handlers cannot be
 * told apart by the disk type a volume reports (ODFileSystem reports a
 * DOS disk for UDF too), so the file decides; other volumes are not
 * looked in, so no hard disk or network share is read.
 *
 * Playing one: amcedvd.library (src/dvdlib), on the drive's sectors read
 * through its device (dvddisc.c), the volume's DOS device giving device
 * and unit. What plays: its film, when one title is most of the disc
 * (half its programmes' time or more: a film, or a "play all"); else its
 * programmes one after another, as a disc of episodes or music videos has
 * them, each a title of its own (those under a minute, logos and
 * trailers, left out). The card is fed a title's Program Stream as it
 * comes (stream.c's video_run, by dvdplay_read); at its end the player
 * starts the next (dvdplay_chapter). No menus yet: still frames and waits
 * are passed over, and a title ends when the disc leaves it (its last
 * chapter done, a menu next).
 *
 * The library is opened by dvdplay_open and closed by dvdplay_close (it
 * stays loaded until memory is wanted, as amcesql.library does). It is
 * used by one task at a time: AmigaMCE's own to open, seek and close; the
 * player's process to read, only between them.
 */
#include "amce.h"
#include "dvdplay.h"
#include "dvddisc.h"
#include "vprobe.h"

#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <libraries/amcedvd.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/amcedvd.h>
#include <string.h>
#include <stdio.h>

struct Library *AmceDvdBase;

/* ---- finding them */

static void bstr(BPTR b, char *out, LONG max)
{
    const UBYTE *s = (const UBYTE *)BADDR(b);
    LONG n;
    out[0] = 0;
    if (!s)
        return;
    n = s[0];
    if (n > max - 1)
        n = max - 1;
    memcpy(out, s + 1, n);
    out[n] = 0;
}

static BOOL has(const char *s, const char *w)
{
    LONG n = (LONG)strlen(w);
    for (; *s; s++)
        if (!amce_strnicmp(s, w, n))
            return TRUE;
    return FALSE;
}

/* A device node's filesystem is a CD/DVD one: its handler's name, or its
 * DOS type ('CD01', 'CDFS', 'UDF\0', 'ISO\0'...). Its drive in dev, unit. */
static BOOL optical(struct DeviceNode *dn, char *dev, LONG devmax, LONG *unit)
{
    char h[64];
    ULONG type = 0;
    struct FileSysStartupMsg *fssm;
    dev[0] = 0;
    *unit = 0;
    if ((ULONG)dn->dn_Startup <= 64)        /* (a number, not a startup message: not a disk) */
        return FALSE;
    fssm = (struct FileSysStartupMsg *)BADDR(dn->dn_Startup);
    bstr(fssm->fssm_Device, dev, devmax);
    *unit = (LONG)fssm->fssm_Unit;
    if (!dev[0])
        return FALSE;
    if (fssm->fssm_Environ) {
        struct DosEnvec *de = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
        if (de && de->de_TableSize >= DE_DOSTYPE)
            type = de->de_DosType;
    }
    if ((type >> 16) == 0x4344 ||           /* "CD.." */
        (type >> 8) == 0x554446 ||          /* "UDF." */
        (type >> 8) == 0x49534F)            /* "ISO." */
        return TRUE;
    bstr(dn->dn_Handler, h, sizeof(h));
    return has(h, "CD") || has(h, "DVD") || has(h, "UDF") || has(h, "ODFileSystem") ||
           has(h, "ISO9660");
}

#define VMAX 8

/* The mounted volumes of CD/DVD filesystems, with their drives: their count. */
static LONG optical_volumes(struct DvdVol *v, LONG max, ULONG *sig)
{
    struct DosList *dl, *vl;
    LONG n = 0;
    ULONG s = 0;
    dl = LockDosList(LDF_VOLUMES | LDF_DEVICES | LDF_READ);
    vl = dl;
    while ((vl = NextDosEntry(vl, LDF_VOLUMES))) {
        struct DosList *d = dl;
        if (!vl->dol_Task)
            continue;                       /* (not in a drive now) */
        while ((d = NextDosEntry(d, LDF_DEVICES))) {
            char dev[48];
            LONG unit;
            if (d->dol_Task != vl->dol_Task)
                continue;
            if (optical((struct DeviceNode *)d, dev, sizeof(dev), &unit)) {
                char name[32];
                const char *c;
                bstr(vl->dol_Name, name, sizeof(name));
                for (c = name; *c; c++)
                    s = s * 31 + (UBYTE)*c;
                s = s * 31 + (ULONG)vl->dol_misc.dol_volume.dol_VolumeDate.ds_Tick;
                if (n < max) {
                    amce_strlcpy(v[n].vol, name, sizeof(v[n].vol));
                    amce_strlcpy(v[n].dev, dev, sizeof(v[n].dev));
                    v[n].unit = unit;
                    n++;
                }
            }
            break;
        }
    }
    UnLockDosList(LDF_VOLUMES | LDF_DEVICES | LDF_READ);
    if (sig)
        *sig = s + (ULONG)n;
    return n;
}

ULONG dvd_volumes_sig(void)
{
    struct DvdVol v[VMAX];
    ULONG s;
    optical_volumes(v, VMAX, &s);
    return s;
}

LONG dvd_volumes(struct DvdVol *out, LONG max)
{
    struct DvdVol v[VMAX];
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR was = me->pr_WindowPtr;
    LONG n = optical_volumes(v, VMAX, NULL), i, k = 0;
    me->pr_WindowPtr = (APTR)-1;            /* (no "insert volume" if one has just gone) */
    for (i = 0; i < n && k < max; i++) {
        char path[64];
        BPTR l;
        sprintf(path, "%.31s:VIDEO_TS/VIDEO_TS.IFO", v[i].vol);
        if ((l = Lock((CONST_STRPTR)path, SHARED_LOCK))) {
            UnLock(l);
            out[k++] = v[i];
        }
    }
    me->pr_WindowPtr = was;
    return k;
}

/* amcedvd.library where dvdplay_open looks for it (not opened) */
BOOL dvd_lib_there(void)
{
    static const char *const at[] = { "PROGDIR:Libs/" AMCEDVD_NAME, "LIBS:" AMCEDVD_NAME };
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR was = me->pr_WindowPtr;
    BOOL there = FALSE;
    int i;
    me->pr_WindowPtr = (APTR)-1;
    for (i = 0; i < 2 && !there; i++) {
        BPTR l = Lock((CONST_STRPTR)at[i], SHARED_LOCK);
        if (l) {
            UnLock(l);
            there = TRUE;
        }
    }
    me->pr_WindowPtr = was;
    return there;
}

/* ---- playing one */
#define TMAX 99                             /* titles a disc can have */
#define SHORT_MS 60000                      /* shorter: not a programme */

struct DvdPlay {
    struct DvdDisc *disc;
    APTR dvd;
    UBYTE list[TMAX];                       /* what plays: titles in order */
    ULONG lens[TMAX];                       /*   their lengths (ms) */
    LONG n, idx;                            /*   how many, which now */
    LONG title;                             /* list[idx] */
    LONG a_fmt;                             /* its sound chosen: DVDA_, -1 none */
    UBYTE a_id, a_sub;                      /*   its packets: stream id, and in private
                                             *   stream 1 its substream (0 none) */
    UBYTE a_ch;
    char a_lang[4];                         /*   its language ("en"), "" not given */
    LONG a_track, a_tracks;                 /*   which of how many (from 1) */
    char vol[32];
    volatile LONG chapter, chapters;        /* (dvdplay_read's, read by AmigaMCE) */
    ULONG ms;                               /* the title's length */
    BOOL pend;                              /* a pack in blk still to give */
    BOOL ended;
    char name[64];
    UBYTE blk[DVD_BLOCK];
};

static void lib_log(APTR h, LONG level, CONST_STRPTR line)
{
    (void)h;
    if (level <= DVDLOG_INFO)
        amce_log("dvd", (const char *)line);
}

/* Up to the film's first pack (kept in blk, given first): what comes
 * before it (the title's start, stream and palette changes) taken in. */
static BOOL prime(struct DvdPlay *dp)
{
    struct DvdEvent ev;
    struct DvdInfo in;
    LONG k;
    dp->pend = FALSE;
    dp->ended = FALSE;
    for (k = 0; k < 4000; k++) {
        LONG e = DvdNextBlock(dp->dvd, dp->blk, &ev);
        if (e == DVDE_BLOCK || e == DVDE_NAV) {
            dp->pend = TRUE;
            if (DvdGetInfo(dp->dvd, &in)) {
                dp->chapter = in.chapter;
                dp->chapters = in.chapters;
            }
            return TRUE;
        }
        if (e == DVDE_STILL)
            DvdSkip(dp->dvd, DVDSKIP_STILL);
        else if (e == DVDE_WAIT)
            DvdSkip(dp->dvd, DVDSKIP_WAIT);
        else if (e == DVDE_STOP || e == DVDE_ERROR)
            break;
    }
    dp->ended = TRUE;
    return FALSE;
}

/* What plays, from the titles' lengths (lens[0] title 1's, 0 not one):
 * the longest alone when it is half the programmes' time or more (a film,
 * or a "play all"), else every title of a minute or more in order. Into
 * list (title numbers) and out (their lengths): how many. */
LONG dvdplay_choose(const ULONG *lens, LONG nt, UBYTE *list, ULONG *out)
{
    ULONG total = 0, best_ms = 0;
    LONG t, best = 0, n = 0;
    for (t = 1; t <= nt && t <= TMAX; t++) {
        if (lens[t - 1] >= SHORT_MS)
            total += lens[t - 1];
        if (lens[t - 1] > best_ms) {
            best_ms = lens[t - 1];
            best = t;
        }
    }
    if (best && best_ms >= SHORT_MS && best_ms * 2 < total) {
        for (t = 1; t <= nt && t <= TMAX; t++)
            if (lens[t - 1] >= SHORT_MS) {
                list[n] = (UBYTE)t;
                out[n++] = lens[t - 1];
            }
    } else if (best) {
        list[0] = (UBYTE)best;
        out[0] = best_ms;
        n = 1;
    }
    return n;
}

/* Which sound track plays, of n (si[i] the disc's track i): stereo (two
 * channels or fewer) first, whatever else there is; of those, MPEG audio
 * before AC-3 (Amiberry's emulated card plays no AC-3: the CPU then
 * decodes it, ac3.library, which stereo spares a mixing down). Then
 * multichannel MPEG, then multichannel AC-3; LPCM and DTS are not the
 * card's. Of equals, the disc's own choice (deflt), else the first. -1 if
 * none plays. */
static int rank(const struct DvdStreamInfo *si)
{
    BOOL stereo = si->channels && si->channels <= 2;
    if (!si->present)
        return 0;
    if (si->format == DVDA_MPEG1 || si->format == DVDA_MPEG2)
        return stereo ? 4 : 2;
    if (si->format == DVDA_AC3)
        return stereo ? 3 : 1;
    return 0;
}

LONG dvdplay_pick_audio(const struct DvdStreamInfo *si, LONG n, LONG deflt)
{
    LONG i, best = -1;
    int br = 0;
    for (i = 0; i < n; i++) {
        int r = rank(&si[i]);
        if (r > br || (r == br && r && i == deflt)) {
            best = i;
            br = r;
        }
    }
    return best;
}

/* The title's sound tracks looked at, one chosen and given to the disc
 * (DvdSetStream), its packets noted for dvdplay_read */
static void choose_audio(struct DvdPlay *dp)
{
    struct DvdStreamInfo si[8];
    struct DvdInfo in;
    LONG i, k;
    char m[160];
    dp->a_fmt = -1;
    dp->a_id = dp->a_sub = dp->a_ch = 0;
    if (!DvdGetInfo(dp->dvd, &in))
        return;
    for (i = 0; i < 8; i++)
        DvdStreamInfo(dp->dvd, DVDS_AUDIO, i, &si[i]);
    k = dvdplay_pick_audio(si, 8, in.audio);
    if (k < 0) {
        sprintf(m, "title %ld: no sound the card plays (%ld tracks)", (long)dp->title,
                (long)in.audio_count);
        amce_log("dvd", m);
        return;
    }
    if (k != in.audio)
        DvdSetStream(dp->dvd, DVDS_AUDIO, k);
    dp->a_fmt = si[k].format;
    dp->a_ch = (UBYTE)si[k].channels;
    memcpy(dp->a_lang, si[k].lang, 3);
    dp->a_lang[3] = 0;
    dp->a_track = k + 1;
    dp->a_tracks = in.audio_count;
    if (si[k].format == DVDA_AC3) {
        dp->a_id = 0xBD;
        dp->a_sub = (UBYTE)(0x80 + si[k].physical);
    } else
        dp->a_id = (UBYTE)(0xC0 + si[k].physical);
    sprintf(m, "title %ld: sound track %ld of %ld (%s, %ld channels, \"%s\")", (long)dp->title,
            (long)(k + 1), (long)in.audio_count, dp->a_fmt == DVDA_AC3 ? "AC-3" : "MPEG",
            (long)dp->a_ch, si[k].lang);
    amce_log("dvd", m);
}

/* A pack's sound not the one chosen (or any, with none chosen): its
 * packet made a padding packet (stream BE), so the card sees one track
 * and the pack's clock stays */
static void other_sound_out(struct DvdPlay *dp, UBYTE *b)
{
    UBYTE *p, *e = b + DVD_BLOCK;
    if (b[0] || b[1] || b[2] != 1 || b[3] != 0xBA)
        return;
    p = b + 14 + (b[13] & 7);
    while (p + 9 < e && !p[0] && !p[1] && p[2] == 1) {
        UBYTE id = p[3];
        LONG len = ((LONG)p[4] << 8) | p[5];
        BOOL sound = FALSE, keep = FALSE;
        if (id >= 0xC0 && id <= 0xDF) {
            sound = TRUE;
            keep = id == dp->a_id;
        } else if (id == 0xBD && p + 9 + p[8] < e) {
            UBYTE sub = p[9 + p[8]];
            if (sub >= 0x80 && sub <= 0xAF) {   /* AC-3, DTS, LPCM (0x20-0x3F: pictures) */
                sound = TRUE;
                keep = dp->a_id == 0xBD && sub == dp->a_sub;
            }
        }
        if (sound && !keep)
            p[3] = 0xBE;
        p += 6 + len;
    }
}

/* Programme k of the list, from chapter (0: its start) */
static BOOL start(struct DvdPlay *dp, LONG k, LONG chapter)
{
    if (k < 0 || k >= dp->n || !DvdPlay(dp->dvd, dp->list[k], chapter > 0 ? chapter : 0))
        return FALSE;
    dp->idx = k;
    dp->title = dp->list[k];
    dp->ms = dp->lens[k];
    if (dp->n > 1)
        sprintf(dp->name, "%.31s: %ld / %ld", dp->vol, (long)(k + 1), (long)dp->n);
    else
        sprintf(dp->name, "%.31s", dp->vol);
    if (!prime(dp))
        return FALSE;
    choose_audio(dp);
    other_sound_out(dp, dp->blk);           /* (the pack prime kept) */
    return TRUE;
}

static void fail(char *err, LONG errlen, const char *why)
{
    amce_strlcpy(err, why, errlen);
    amce_log("dvd", why);
}

/* The disc in d (its drive or image, opened): its longest title started */
static struct DvdPlay *open_disc(struct DvdDisc *disc, const char *name, const char *from,
                                 struct VProbe *v, char *err, LONG errlen)
{
    struct DvdPlay *dp;
    struct DvdReader rd;
    struct DvdInfo in;
    char m[300];
    LONG t, best;
    ULONG best_ms, ms;
    if (!(dp = AllocVec(sizeof(*dp), MEMF_ANY | MEMF_CLEAR))) {
        dvdd_close(disc);
        fail(err, errlen, _("Not enough memory"));
        return NULL;
    }
    dp->disc = disc;
    if (!AmceDvdBase && !(AmceDvdBase = amce_open_own_lib(AMCEDVD_NAME, AMCEDVD_VERSION))) {
        fail(err, errlen, _("DVDs need amcedvd.library (in AmigaMCE's Libs)"));
        dvdplay_close(dp);
        return NULL;
    }
    dvdd_reader(dp->disc, &rd);
    rd.log = lib_log;
    if (!(dp->dvd = DvdOpen(&rd, 0))) {
        sprintf(m, _("Not a DVD-Video disc AmigaMCE can read: %.120s"), (const char *)DvdError(NULL));
        fail(err, errlen, m);
        dvdplay_close(dp);
        return NULL;
    }
    /* what plays: the film, or the programmes in order */
    DvdGetInfo(dp->dvd, &in);
    {
        ULONG lens[TMAX];
        LONG nt = in.titles < TMAX ? in.titles : TMAX;
        for (t = 1; t <= nt; t++)
            if (DvdTitleLength(dp->dvd, t, &ms) <= 0 || !(lens[t - 1] = ms))
                lens[t - 1] = 0;
        dp->n = dvdplay_choose(lens, nt, dp->list, dp->lens);
    }
    amce_strlcpy(dp->vol, name, sizeof(dp->vol));
    if (!dp->n || !start(dp, 0, 0)) {
        fail(err, errlen, _("The DVD's film could not be played"));
        dvdplay_close(dp);
        return NULL;
    }
    best = dp->title;
    best_ms = dp->ms;
    DvdGetInfo(dp->dvd, &in);
    memset(v, 0, sizeof(*v));
    dvdplay_probe(dp, v);
    sprintf(m, "%.31s (%.60s): %s; title %ld of %ld, %ld chapters, %lu s, %ld x %ld %s, sound %s",
            name, from, dp->n > 1 ? "its programmes in order" : "its film", (long)best,
            (long)in.titles, (long)dp->chapters,
            (unsigned long)(best_ms / 1000), (long)v->w, (long)v->h, in.wide ? "16:9" : "4:3",
            v->ac3 ? "AC-3" : v->mp2 ? "MPEG" : "none the card plays");
    amce_log("dvd", m);
    return dp;
}

struct DvdPlay *dvdplay_open(const char *vol, struct VProbe *v, char *err, LONG errlen)
{
    struct DvdVol dv[VMAX];
    struct DvdDisc *disc;
    char name[32], m[200];
    LONG n, i;
    err[0] = 0;
    amce_strlcpy(name, vol, sizeof(name));
    if ((n = (LONG)strlen(name)) && name[n - 1] == ':')
        name[n - 1] = 0;
    n = optical_volumes(dv, VMAX, NULL);
    for (i = 0; i < n; i++)
        if (!amce_stricmp(dv[i].vol, name))
            break;
    if (i == n) {
        fail(err, errlen, _("The DVD is no longer in its drive"));
        return NULL;
    }
    if (!(disc = dvdd_open_drive(dv[i].dev, dv[i].unit, m, sizeof(m)))) {
        fail(err, errlen, m);
        return NULL;
    }
    sprintf(m, "%.40s unit %ld", dv[i].dev, (long)dv[i].unit);
    return open_disc(disc, name, m, v, err, errlen);
}

struct DvdPlay *dvdplay_open_image(const char *path, struct VProbe *v, char *err, LONG errlen)
{
    struct DvdDisc *disc;
    err[0] = 0;
    if (!(disc = dvdd_open_image(path, err, errlen))) {
        amce_log("dvd", err);
        return NULL;
    }
    return open_disc(disc, (const char *)FilePart((STRPTR)path), path, v, err, errlen);
}

const char *dvdplay_name(struct DvdPlay *dp)
{
    return dp->name;
}

LONG dvdplay_read(struct DvdPlay *dp, volatile UBYTE *dst, LONG max)
{
    LONG got = 0;
    struct DvdEvent ev;
    struct DvdInfo in;
    while (!dp->ended && got + DVD_BLOCK <= max) {
        LONG e;
        if (dp->pend) {
            dp->pend = FALSE;
            e = DVDE_BLOCK;
        } else
            e = DvdNextBlock(dp->dvd, dp->blk, &ev);
        switch (e) {
        case DVDE_BLOCK:
            if (e == DVDE_BLOCK)
                other_sound_out(dp, dp->blk);
            /* fall through */
        case DVDE_NAV:                      /* (the NAV pack carries the system header) */
            CopyMem(dp->blk, (APTR)(dst + got), DVD_BLOCK);
            got += DVD_BLOCK;
            break;
        case DVDE_STILL:
            DvdSkip(dp->dvd, DVDSKIP_STILL);
            break;
        case DVDE_WAIT:
            DvdSkip(dp->dvd, DVDSKIP_WAIT);
            break;
        case DVDE_DOMAIN:
            if (ev.domain != DVDD_TITLE)
                dp->ended = TRUE;           /* to a menu: the film is over */
            break;
        case DVDE_CELL:
            if (DvdGetInfo(dp->dvd, &in)) {
                if (in.title != dp->title)
                    dp->ended = TRUE;       /* on to another title */
                dp->chapter = in.chapter;
                dp->chapters = in.chapters;
            }
            break;
        case DVDE_STOP:
            dp->ended = TRUE;
            break;
        case DVDE_ERROR: {
            char m[200];
            sprintf(m, "read: %.150s", (const char *)DvdError(dp->dvd));
            amce_log("dvd", m);
            return got ? got : -1;
        }
        default:
            break;
        }
    }
    return got;
}

static ULONG where(struct DvdPlay *dp)
{
    struct DvdInfo in;
    return DvdGetInfo(dp->dvd, &in) ? in.ms : 0;
}

BOOL dvdplay_seek(struct DvdPlay *dp, ULONG ms, ULONG *at)
{
    *at = 0;
    if (!ms)
        return DvdPlay(dp->dvd, dp->title, 0) && prime(dp);
    /* (after its end the disc has gone on to a menu: back in the film first) */
    if (dp->ended && !(DvdPlay(dp->dvd, dp->title, 0) && prime(dp)))
        return FALSE;
    {
        /* there, or if the disc will not seek to that time (a cell it
         * cannot find, an angle block), the nearest that it will: a few
         * seconds either side, then further */
        static const LONG near_s[] = { 0, -2, 2, -5, 5, -10, 10, -30, 30, -60, 60 };
        int i;
        char m[100];
        for (i = 0; i < (int)(sizeof(near_s) / sizeof(near_s[0])); i++) {
            LONG t = (LONG)ms + near_s[i] * 1000;
            if (t < 0 || (dp->ms && (ULONG)t >= dp->ms))
                continue;
            if (DvdSeekTime(dp->dvd, (ULONG)t)) {
                if (i) {
                    sprintf(m, "a seek to %lu ms: not there, %ld s %s", (unsigned long)ms,
                            (long)(near_s[i] < 0 ? -near_s[i] : near_s[i]),
                            near_s[i] < 0 ? "before" : "after");
                    amce_log("dvd", m);
                }
                if (!prime(dp))
                    return FALSE;
                *at = where(dp);
                return TRUE;
            }
        }
        sprintf(m, "a seek to %lu ms: not there or within a minute of it", (unsigned long)ms);
        amce_log("dvd", m);
    }
    return FALSE;
}

/* N: the next chapter, or at the title's last the next programme; P: the
 * previous chapter, or at its first the previous programme (the first's
 * start again). After a title's end, from where it ended. */
BOOL dvdplay_chapter(struct DvdPlay *dp, int dir, ULONG *at)
{
    *at = 0;
    if (dp->ended && dp->chapters > 1 && !start(dp, dp->idx, dp->chapter))
        return FALSE;                       /* (back in it at the chapter it ended in) */
    if (dir > 0) {
        if (!dp->ended && dp->chapter < dp->chapters) {
            if (!DvdChapter(dp->dvd, DVDC_NEXT) || !prime(dp))
                return FALSE;
            *at = where(dp);
            return TRUE;
        }
        return start(dp, dp->idx + 1, 0);
    }
    if (!dp->ended && dp->chapter > 1) {
        if (!DvdChapter(dp->dvd, DVDC_PREV) || !prime(dp))
            return FALSE;
        *at = where(dp);
        return TRUE;
    }
    return start(dp, dp->idx > 0 ? dp->idx - 1 : 0, 0);
}

void dvdplay_where(struct DvdPlay *dp, struct DvdWhere *w)
{
    w->title = dp->title;
    w->programme = dp->idx + 1;
    w->programmes = dp->n;
    w->chapter = dp->chapter;
    w->chapters = dp->chapters;
    w->a_track = dp->a_fmt >= 0 ? dp->a_track : 0;
    w->a_tracks = dp->a_tracks;
    w->a_ch = dp->a_ch;
    w->a_ac3 = dp->a_fmt == DVDA_AC3;
    memcpy(w->a_lang, dp->a_lang, sizeof(w->a_lang));
}

BOOL dvdplay_can_chapter(struct DvdPlay *dp, int dir)
{
    return dir > 0 ? (!dp->ended && dp->chapter < dp->chapters) || dp->idx + 1 < dp->n : TRUE;
}

BOOL dvdplay_next_title(struct DvdPlay *dp)
{
    return dp->idx + 1 < dp->n;
}

/* What the player needs of the title now: size, aspect, length */
void dvdplay_probe(struct DvdPlay *dp, struct VProbe *v)
{
    struct DvdInfo in;
    if (!DvdGetInfo(dp->dvd, &in))
        in.width = in.height = in.wide = 0;
    v->w = (UWORD)(in.width ? in.width : 720);
    v->h = (UWORD)(in.height ? in.height : 576);
    v->aspect = in.wide ? 3 : 2;            /* (MPEG-2's codes: 16:9, 4:3) */
    v->fps_milli = v->h == 576 ? 25000 : 29970;
    v->ps = TRUE;
    v->mpeg2 = TRUE;
    v->ms = dp->ms;
    v->ac3 = dp->a_fmt == DVDA_AC3;
    v->mp2 = dp->a_fmt == DVDA_MPEG1 || dp->a_fmt == DVDA_MPEG2;
    v->audio_id = v->mp2 ? dp->a_id : 0;
    v->a_chans = dp->a_ch;
}

void dvdplay_close(struct DvdPlay *dp)
{
    if (!dp)
        return;
    if (dp->dvd)
        DvdClose(dp->dvd);
    if (dp->disc)
        dvdd_close(dp->disc);
    FreeVec(dp);
    if (AmceDvdBase) {
        CloseLibrary(AmceDvdBase);          /* stays loaded until memory is wanted */
        AmceDvdBase = NULL;
    }
}
