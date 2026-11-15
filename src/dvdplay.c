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
 * and unit; or an image file's (a .iso in Videos), read through its file
 * system. What plays: its film, when one title is most of the disc
 * (half its programmes' time or more: a film, or a "play all"); else its
 * programmes one after another, as a disc of episodes or music videos has
 * them, each a title of its own (those under a minute, logos and
 * trailers, left out). The card is fed a title's Program Stream as it
 * comes (stream.c's video_run, by dvdplay_read); at its end the player
 * starts the next (dvdplay_chapter). Still frames and waits are passed
 * over, and a title ends when the disc leaves it (its last chapter done, a
 * menu next).
 *
 * Or, decoded by the CPU, a disc with menus (dvdplay_nav): from its first
 * play as a DVD player reads it, its virtual machine deciding what comes
 * next; reading stops at what the player must deal with (a jump, a still,
 * a wait: dvdplay_pending), and what the viewer asks of its menus (a
 * button, its menu, a seek) is queued by the UI (dvdplay_ask) and done by
 * the playback process (dvdplay_service), the one task using the library
 * while it plays. Its subpictures' stream, colours and highlighted button
 * are kept for the player to draw (spu.c).
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
#include "spu.h"

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
    BOOL pend_nav;                          /*   (a NAV pack) */
    BOOL ended;
    char name[64];
    /* the disc's own navigation (dvdplay_nav): from its first play, its
     * menus and whatever they lead to; the playback process reads, and
     * carries out what the UI asks (asks[], under Forbid) */
    BOOL nav;
    ULONG menus;                            /* DvdMenus' bits (0 none, or no 1.2 library) */
    volatile int stop_ev;                   /* DVDP_: what reading stopped for */
    volatile LONG still_s;
    volatile BOOL ev_user;                  /*   a hop the viewer asked for (a button, Menu) */
    BOOL user_req;                          /* (asked: the next hop is the viewer's) */
    struct { UBYTE what; LONG a, b; } asks[8];
    volatile UBYTE ask_in, ask_out;
    volatile LONG domain, vts;
    volatile LONG spu;                      /* the subpicture stream (0-31), -1 none */
    volatile BOOL spu_forced;               /*   only its forced ones (subtitles off) */
    ULONG clut[16];
    volatile ULONG clut_seq;
    struct SpuHl hl;
    volatile ULONG hl_seq;
    volatile LONG hl_button;
    volatile ULONG at_ms, forbidden;
    volatile BOOL wide;
    /* its times: the last VOBU's end (90 kHz), to see them jump back (a
     * menu going round) or on, with no jump of the disc's to say so */
    BOOL vobu_known;
    ULONG vobu_end, n_restart;
    /* the disc's reading (the library's, its CSS library's): through
     * timed_read, counted and timed, for the log */
    LONG (*r_seek)(APTR h, ULONG sector);
    LONG (*r_read)(APTR h, APTR buf, LONG n);
    ULONG r_calls, r_sectors, r_slow, r_hz, r_slowest;
    unsigned long long r_ticks;
    UBYTE blk[DVD_BLOCK];
};

static LONG timed_seek(APTR h, ULONG sector)
{
    struct DvdPlay *dp = h;
    return dp->r_seek(dp->disc, sector);
}

static LONG timed_read(APTR h, APTR buf, LONG n)
{
    struct DvdPlay *dp = h;
    ULONG hz = 0, t = time_ticks(&hz), dt;
    LONG got = dp->r_read(dp->disc, buf, n);
    dt = time_ticks(&hz) - t;
    dp->r_hz = hz;
    dp->r_ticks += dt;
    dp->r_calls++;
    if (got > 0)
        dp->r_sectors += (ULONG)got;
    if (dt > dp->r_slowest)
        dp->r_slowest = dt;
    if (hz && dt > hz / 20)
        dp->r_slow++;                       /* (over 50 ms) */
    return got;
}

/* The disc's reader, through timed_read */
static void reader(struct DvdPlay *dp, struct DvdReader *rd)
{
    dvdd_reader(dp->disc, rd);
    dp->r_seek = rd->seek;
    dp->r_read = rd->read;
    rd->handle = dp;
    rd->seek = timed_seek;
    rd->read = timed_read;
}

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
    if (dp->nav && in.audio >= 0 && in.audio < 8 && rank(&si[in.audio]))
        k = in.audio;                       /* (its own navigation: the disc's choice, its
                                             * audio menu's, if it plays) */
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
    if (dp->nav && in.domain != DVDD_TITLE)
        sprintf(m, "a menu: its sound (%s, %ld channels)", si[k].format == DVDA_AC3 ? "AC-3" : "MPEG",
                (long)si[k].channels);
    else
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
    char m[360];
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
    reader(dp, &rd);
    rd.log = lib_log;
    dvdd_log(dp->disc, lib_log);            /* (an image's sector out of reach) */
    if (!(dp->dvd = DvdOpen(&rd, 0))) {
        sprintf(m, _("Not a DVD-Video disc AmigaMCE can read: %.120s"), (const char *)DvdError(NULL));
        fail(err, errlen, m);
        dvdplay_close(dp);
        return NULL;
    }
    /* its menus (amcedvd.library 1.2 on) */
    if (AmceDvdBase->lib_Version > 1 || AmceDvdBase->lib_Revision >= 2)
        dp->menus = DvdMenus(dp->dvd);
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
    sprintf(m, "%.31s (%.110s): %s; title %ld of %ld, %ld chapters, %lu s, %ld x %ld %s, sound %s",
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
    char name[32], from[120];
    LONG n;
    err[0] = 0;
    if (!(disc = dvdd_open_image(path, err, errlen))) {
        amce_log("dvd", err);
        return NULL;
    }
    /* its name without .iso, as a disc's volume is named */
    amce_strlcpy(name, (const char *)FilePart((STRPTR)path), sizeof(name));
    if ((n = (LONG)strlen(name)) > 4 && !amce_stricmp(name + n - 4, ".iso"))
        name[n - 4] = 0;
    sprintf(from, "%.60s, %lu MB, %.40s", path, (unsigned long)(dvdd_sectors(disc) / 512),
            dvdd_how(disc));
    return open_disc(disc, name, from, v, err, errlen);
}

const char *dvdplay_name(struct DvdPlay *dp)
{
    return dp->name;
}

/* ---- the disc's own navigation (its menus) */

/* The highlighted button, as the playback process will draw it */
static void hl_fetch(struct DvdPlay *dp)
{
    struct DvdHighlight h;
    struct SpuHl n;
    LONG b = DvdHighlight(dp->dvd, &h, DVDH_SELECTED), i;
    memset(&n, 0, sizeof(n));
    if (b > 0) {
        n.on = TRUE;
        n.x0 = h.x0, n.y0 = h.y0, n.x1 = h.x1, n.y1 = h.y1;
        for (i = 0; i < 4; i++) {
            n.col[i] = h.colour[i];
            n.alpha[i] = h.alpha[i];
        }
    }
    dp->hl_button = b > 0 ? b : 0;
    if (memcmp(&n, &dp->hl, sizeof(n))) {
        Forbid();
        dp->hl = n;
        dp->hl_seq++;
        Permit();
    }
}

/* Where it is now: its domain, title, time, what the disc forbids */
static void now_fetch(struct DvdPlay *dp)
{
    struct DvdInfo in;
    if (!DvdGetInfo(dp->dvd, &in))
        return;
    dp->domain = in.domain;
    dp->at_ms = in.domain == DVDD_TITLE ? in.ms : 0;
    dp->forbidden = in.forbidden;
    dp->wide = in.wide;
    dp->chapter = in.chapter;
    dp->chapters = in.chapters;
    if (in.domain == DVDD_TITLE && in.title != dp->title) {
        ULONG ms = 0;
        dp->title = in.title;
        DvdTitleLength(dp->dvd, in.title, &ms);
        dp->ms = ms;
    }
}

ULONG dvdplay_menus(struct DvdPlay *dp)
{
    return dp->menus;
}

BOOL dvdplay_is_nav(struct DvdPlay *dp)
{
    return dp->nav;
}

/* Reading from the disc's start, its first play: the disc opened afresh
 * (its machine started again), its first pack found as prime does,
 * passing over the jumps and changes before it. */
BOOL dvdplay_nav(struct DvdPlay *dp)
{
    struct DvdReader rd;
    struct DvdEvent ev;
    LONG k;
    char m[120];
    if (!dp->menus)
        return FALSE;
    DvdClose(dp->dvd);
    reader(dp, &rd);
    rd.log = lib_log;
    if (!(dp->dvd = DvdOpen(&rd, 0))) {
        amce_log("dvd", "opened again for its menus: could not be");
        dp->ended = TRUE;
        return FALSE;
    }
    dp->nav = TRUE;
    dp->ended = dp->pend = dp->pend_nav = dp->vobu_known = FALSE;
    dp->stop_ev = DVDP_NONE;
    dp->spu = -1;
    dp->title = 0;
    dp->ms = 0;
    dp->a_fmt = -1;
    sprintf(dp->name, "%.31s", dp->vol);
    for (k = 0; k < 4000; k++) {
        LONG e = DvdNextBlock(dp->dvd, dp->blk, &ev);
        if (e == DVDE_BLOCK || e == DVDE_NAV) {
            dp->pend = TRUE;
            dp->pend_nav = e == DVDE_NAV;
            if (e == DVDE_BLOCK)
                other_sound_out(dp, dp->blk);
            now_fetch(dp);
            sprintf(m, "from its start: its menus (%lx)", (unsigned long)dp->menus);
            amce_log("dvd", m);
            return TRUE;
        }
        if (e == DVDE_DOMAIN) {
            dp->domain = ev.domain;
            dp->vts = ev.vts;
            choose_audio(dp);
        } else if (e == DVDE_SPU_STREAM) {
            dp->spu = ev.spu_wide >= 0 ? (ev.spu_wide & 31) : -1;
            dp->spu_forced = ev.spu_wide >= 0 && (ev.spu_wide & 0x80);
        } else if (e == DVDE_CLUT) {
            memcpy(dp->clut, ev.clut, sizeof(dp->clut));
            dp->clut_seq++;
        } else if (e == DVDE_AUDIO_STREAM)
            choose_audio(dp);
        else if (e == DVDE_STILL)
            DvdSkip(dp->dvd, DVDSKIP_STILL);    /* (a still before any picture) */
        else if (e == DVDE_WAIT)
            DvdSkip(dp->dvd, DVDSKIP_WAIT);
        else if (e == DVDE_STOP || e == DVDE_ERROR)
            break;
    }
    amce_log("dvd", "from its start: nothing to play: its film instead");
    dp->nav = FALSE;
    start(dp, 0, 0);
    return FALSE;
}

/* One of the disc's events taken in (not a pack): what reading stops for */
static void nav_event(struct DvdPlay *dp, LONG e, const struct DvdEvent *ev)
{
    switch (e) {
    case DVDE_HOP:
        dp->vobu_known = FALSE;             /* (its times start afresh) */
        dp->ev_user = dp->user_req;
        dp->user_req = FALSE;
        dp->stop_ev = DVDP_HOP;
        break;
    case DVDE_STILL:
        dp->still_s = ev->still;
        dp->stop_ev = DVDP_STILL;
        break;
    case DVDE_WAIT:
        dp->stop_ev = DVDP_WAIT;
        break;
    case DVDE_STOP: {
        char m[160];
        sprintf(m, "the disc stopped%s (in %s)", dp->title ? "" : " before any title of its own",
                dp->domain == DVDD_TITLE ? "a title" : dp->domain == DVDD_FIRSTPLAY ? "its first play"
                                                                                      : "a menu");
        amce_log("dvd", m);
        dp->ended = TRUE;
        dp->stop_ev = DVDP_END;
        break;
    }
    case DVDE_DOMAIN: {
        char m[80];
        dp->domain = ev->domain;
        dp->vts = ev->vts;
        sprintf(m, "now %s (title set %ld)",
                ev->domain == DVDD_TITLE ? "a title" : ev->domain == DVDD_FIRSTPLAY ? "its first play"
                                                                                      : "a menu",
                (long)ev->vts);
        amce_log("dvd", m);
        now_fetch(dp);
        choose_audio(dp);
        hl_fetch(dp);
        break;
    }
    case DVDE_AUDIO_STREAM:
        choose_audio(dp);
        break;
    case DVDE_SPU_STREAM:
        dp->spu = ev->spu_wide >= 0 ? (ev->spu_wide & 31) : -1;
        dp->spu_forced = ev->spu_wide >= 0 && (ev->spu_wide & 0x80);
        break;
    case DVDE_CLUT:
        Forbid();
        memcpy(dp->clut, ev->clut, sizeof(dp->clut));
        dp->clut_seq++;
        Permit();
        break;
    case DVDE_HIGHLIGHT:
        hl_fetch(dp);
        break;
    case DVDE_CELL:
        now_fetch(dp);
        hl_fetch(dp);
        break;
    default:
        break;
    }
}

/* A NAV pack's VOBU: TRUE if its times do not follow on from the last's
 * (back, or more than a second on), with no jump to say so. */
static BOOL nav_restarts(struct DvdPlay *dp)
{
    const UBYTE *b = dp->blk;
    ULONG s, e;
    BOOL known = dp->vobu_known;
    if (b[38] || b[39] || b[40] != 1 || b[41] != 0xBF || b[44] != 0)
        return FALSE;                       /* (not the PCI where it should be) */
    s = ((ULONG)b[57] << 24) | ((ULONG)b[58] << 16) | ((ULONG)b[59] << 8) | b[60];
    e = ((ULONG)b[61] << 24) | ((ULONG)b[62] << 16) | ((ULONG)b[63] << 8) | b[64];
    dp->vobu_known = TRUE;
    if (known && (s + 9000 < dp->vobu_end || s > dp->vobu_end + 90000)) {
        dp->vobu_known = FALSE;             /* (this pack again after: taken as it is) */
        if (++dp->n_restart <= 3) {
            char m[100];
            sprintf(m, "its times start again (%lu ms, from %lu): played out, then on",
                    (unsigned long)(s / 90), (unsigned long)(dp->vobu_end / 90));
            amce_log("dvd", m);
        }
        return TRUE;
    }
    dp->vobu_end = e;
    return FALSE;
}

/* The disc read on through its menus and titles: packs into dst until
 * something it must stop for (dvdplay_pending), then 0 until dvdplay_go. */
static LONG nav_read(struct DvdPlay *dp, volatile UBYTE *dst, LONG max)
{
    LONG got = 0;
    struct DvdEvent ev;
    while (dp->stop_ev == DVDP_NONE && got + DVD_BLOCK <= max) {
        LONG e;
        if (dp->pend) {
            dp->pend = FALSE;
            e = dp->pend_nav ? DVDE_NAV : DVDE_BLOCK;
            dp->pend_nav = FALSE;
        } else
            e = DvdNextBlock(dp->dvd, dp->blk, &ev);
        if (e == DVDE_NAV && nav_restarts(dp)) {
            /* its times start again (a menu going round to its start): what
             * came before played out first, then on from this pack, timed
             * afresh, as after a jump of the disc's own */
            dp->pend = dp->pend_nav = TRUE;
            dp->ev_user = FALSE;
            dp->stop_ev = DVDP_HOP;
            break;
        }
        if (e == DVDE_BLOCK || e == DVDE_NAV) {
            if (e == DVDE_BLOCK)
                other_sound_out(dp, dp->blk);
            else {
                now_fetch(dp);              /* (a new VOBU: its time; its buttons) */
                hl_fetch(dp);
            }
            CopyMem(dp->blk, (APTR)(dst + got), DVD_BLOCK);
            got += DVD_BLOCK;
        } else if (e == DVDE_ERROR) {
            char m[200];
            sprintf(m, "read: %.150s", (const char *)DvdError(dp->dvd));
            amce_log("dvd", m);
            dp->ended = TRUE;
            dp->stop_ev = DVDP_END;
            return got ? got : -1;
        } else
            nav_event(dp, e, &ev);
    }
    return got;
}

int dvdplay_pending(struct DvdPlay *dp, LONG *still_s, BOOL *user)
{
    if (still_s)
        *still_s = dp->still_s;
    if (user)
        *user = dp->ev_user;
    return dp->nav ? dp->stop_ev : DVDP_NONE;
}

void dvdplay_go(struct DvdPlay *dp)
{
    if (dp->stop_ev == DVDP_STILL)
        DvdSkip(dp->dvd, DVDSKIP_STILL);
    else if (dp->stop_ev == DVDP_WAIT)
        DvdSkip(dp->dvd, DVDSKIP_WAIT);
    if (dp->stop_ev != DVDP_END)
        dp->stop_ev = DVDP_NONE;
    dp->ev_user = FALSE;
}

void dvdplay_ask(struct DvdPlay *dp, int what, LONG a, LONG b)
{
    UBYTE i;
    if (!dp || !dp->nav)
        return;
    Forbid();
    i = dp->ask_in;
    if ((UBYTE)(i - dp->ask_out) < 8) {
        dp->asks[i & 7].what = (UBYTE)what;
        dp->asks[i & 7].a = a;
        dp->asks[i & 7].b = b;
        dp->ask_in = (UBYTE)(i + 1);
    }
    Permit();
}

void dvdplay_service(struct DvdPlay *dp)
{
    char m[100];
    while (dp->nav && dp->ask_out != dp->ask_in) {
        UBYTE what;
        LONG a, b, ok = TRUE;
        Forbid();
        what = dp->asks[dp->ask_out & 7].what;
        a = dp->asks[dp->ask_out & 7].a;
        b = dp->asks[dp->ask_out & 7].b;
        dp->ask_out++;
        Permit();
        switch (what) {
        case DVDQ_UP:
            ok = DvdButton(dp->dvd, DVDB_UP, 0);
            break;
        case DVDQ_DOWN:
            ok = DvdButton(dp->dvd, DVDB_DOWN, 0);
            break;
        case DVDQ_LEFT:
            ok = DvdButton(dp->dvd, DVDB_LEFT, 0);
            break;
        case DVDQ_RIGHT:
            ok = DvdButton(dp->dvd, DVDB_RIGHT, 0);
            break;
        case DVDQ_ACTIVATE:
            dp->user_req = TRUE;
            ok = DvdButton(dp->dvd, DVDB_ACTIVATE, 0);
            if (ok) {
                sprintf(m, "its menu's button %ld pressed", (long)dp->hl_button);
                amce_log("dvd", m);
            }
            break;
        case DVDQ_POINT:
            DvdPoint(dp->dvd, a, b, FALSE);     /* (off its buttons: nothing) */
            break;
        case DVDQ_CLICK: {
            struct DvdHighlight h;
            LONG was = DvdHighlight(dp->dvd, &h, DVDH_SELECTED);
            dp->user_req = TRUE;
            ok = DvdPoint(dp->dvd, a, b, TRUE);
            sprintf(m, "its menu clicked at %ld, %ld: %s (button %ld before)", (long)a, (long)b,
                    ok ? "a button pressed" : "no button there", (long)was);
            amce_log("dvd", m);
            if (!ok) {
                dp->user_req = FALSE;
                ok = TRUE;                  /* (logged) */
            }
            break;
        }
        case DVDQ_MENU:
            dp->user_req = TRUE;
            if (dp->domain == DVDD_TITLE)
                ok = DvdMenu(dp->dvd, DVDM_ROOT) || DvdMenu(dp->dvd, DVDM_TITLE);
            else
                ok = DvdMenu(dp->dvd, DVDM_ESCAPE);     /* (from a menu: back to the title) */
            break;
        case DVDQ_SEEK:
            dp->user_req = TRUE;
            ok = DvdSeekTime(dp->dvd, (ULONG)a);
            break;
        case DVDQ_CHAPTER:
            dp->user_req = TRUE;
            ok = DvdChapter(dp->dvd, a > 0 ? DVDC_NEXT : DVDC_PREV);
            break;
        }
        if (!ok) {
            dp->user_req = FALSE;
            sprintf(m, "asked %ld: %.60s", (long)what, (const char *)DvdError(dp->dvd));
            amce_log("dvd", m);
        }
        hl_fetch(dp);                       /* (a button moved to: drawn now, a still too) */
    }
}

void dvdplay_poll(struct DvdPlay *dp)
{
    struct DvdEvent ev;
    int k;
    dvdplay_service(dp);
    if (dp->stop_ev != DVDP_STILL && dp->stop_ev != DVDP_WAIT)
        return;
    /* the disc's events while it stands still: a button changed, a jump */
    for (k = 0; k < 16; k++) {
        LONG e = DvdNextBlock(dp->dvd, dp->blk, &ev);
        if (e == DVDE_STILL || e == DVDE_WAIT || e == DVDE_NOP)
            break;
        if (e == DVDE_BLOCK || e == DVDE_NAV) {
            dp->pend = TRUE;                /* (moved on by itself: given next) */
            dp->pend_nav = e == DVDE_NAV;
            if (e == DVDE_BLOCK)
                other_sound_out(dp, dp->blk);
            dp->stop_ev = DVDP_HOP;
            break;
        }
        if (e == DVDE_ERROR)
            break;
        nav_event(dp, e, &ev);
        if (dp->stop_ev == DVDP_HOP || dp->stop_ev == DVDP_END)
            break;
    }
}

void dvdplay_now(struct DvdPlay *dp, struct DvdNow *n)
{
    n->menu = dp->nav && dp->domain != DVDD_TITLE;
    n->buttons = dp->nav && dp->hl_button > 0;
    n->title_ms = dp->nav && dp->domain == DVDD_TITLE ? dp->ms : 0;
    n->at_ms = dp->at_ms;
    n->forbidden = dp->forbidden;
    n->title = dp->title;
}

LONG dvdplay_spu(struct DvdPlay *dp, BOOL *forced_only)
{
    *forced_only = dp->spu_forced;
    return dp->nav ? dp->spu : -1;
}

const ULONG *dvdplay_clut(struct DvdPlay *dp, ULONG *seq)
{
    *seq = dp->clut_seq;
    return dp->clut;
}

ULONG dvdplay_highlight(struct DvdPlay *dp, struct SpuHl *hl)
{
    ULONG q;
    Forbid();
    *hl = dp->hl;
    q = dp->hl_seq;
    Permit();
    return q;
}

LONG dvdplay_read(struct DvdPlay *dp, volatile UBYTE *dst, LONG max)
{
    LONG got = 0;
    struct DvdEvent ev;
    struct DvdInfo in;
    if (dp->nav)
        return nav_read(dp, dst, max);
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
    if (dp->nav && !ms)
        return dvdplay_nav(dp);             /* (its menus: the disc from its start again) */
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
    if (dp->nav)
        return dp->domain == DVDD_TITLE && dp->stop_ev != DVDP_END &&
               !(dp->forbidden & (dir > 0 ? DVDU_NEXT_CHAPTER : DVDU_PREV_CHAPTER)) &&
               (dir < 0 || dp->chapter < dp->chapters);
    return dir > 0 ? (!dp->ended && dp->chapter < dp->chapters) || dp->idx + 1 < dp->n : TRUE;
}

BOOL dvdplay_next_title(struct DvdPlay *dp)
{
    if (dp->nav)
        return FALSE;                       /* (the disc's own navigation goes on) */
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
    if (dp->r_calls && dp->r_hz) {
        char m[160];
        sprintf(m, "the disc read: %lu MB in %lu calls, %lu ms in all (%lu KB/s), the slowest %lu ms, "
                   "%lu over 50 ms",
                (unsigned long)(dp->r_sectors / 512), (unsigned long)dp->r_calls,
                (unsigned long)(dp->r_ticks * 1000 / dp->r_hz),
                (unsigned long)(dp->r_ticks ? (unsigned long long)dp->r_sectors * 2 * dp->r_hz / dp->r_ticks : 0),
                (unsigned long)((unsigned long long)dp->r_slowest * 1000 / dp->r_hz),
                (unsigned long)dp->r_slow);
        amce_log("dvd", m);
    }
    if (dp->dvd)
        DvdClose(dp->dvd);
    if (dp->disc)
        dvdd_close(dp->disc);
    FreeVec(dp);
    amce_close_own_lib(&AmceDvdBase);
}
