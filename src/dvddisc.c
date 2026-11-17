/*
 * Ambiance - reading a DVD for dvdnav.library
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Sectors of 2048 bytes, from a drive or an image file, behind the
 * DvdReader that dvdnav.library reads the disc through (its seek and
 * read are called on the task that calls the library).
 *
 * A drive is read with SCSI commands through HD_SCSICMD, which
 * scsi.device (OS 3.2 and 3.9, ATAPI drives on the IDE port included),
 * SCSI cards' devices and the ATAPI drivers all take: READ(10) for the
 * sectors and READ CAPACITY for the size. Sectors arrive in a buffer of
 * our own (public memory, longword aligned, as DMA controllers want),
 * 16 at a time: libdvdnav asks for one at a time as a film plays, so the
 * next 15 are there already. A failed read is tried three times; a drive
 * still spinning up ("not ready") is given up to ten seconds.
 *
 * A disc may be read from any task, one at a time (the player's process
 * while a film plays, Ambiance's own between): its requests come back to
 * a port that signals whichever task sent them, with SIGF_SINGLE, exec's
 * signal for waiting on one thing.
 *
 * An image file is read with Read, positioned with Seek under 2 GB.
 * DVD images are bigger (4.7 GB a layer), and DOS's Seek is 32-bit: past
 * 2 GB the 64-bit packets MorphOS brought in are sent to the file's
 * handler (dvdiso.h), which UAE's and Amiberry's host file system answers
 * (a folder of the host's mounted as a volume). A handler that does not
 * know them is positioned in two 32-bit Seeks, from the start and then
 * on, which reaches 4 GB if it takes positions past 2 GB at all (PFS3
 * holds no file over 2 GB, FFS and SFS none over 4 GB); a bigger image
 * there cannot be read, and the log says so. An image under 2 GB is never
 * sent the packets (vamos, which runs the tests, would not know them).
 */
#include "dvddisc.h"
#include "dvdiso.h"

#include <exec/memory.h>
#include <exec/io.h>
#include <devices/scsidisk.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>
#include <string.h>

#define CHUNK 16                            /* sectors per SCSI read (32 KB) */

/* a 64-bit packet, and the message it goes in */
struct Pkt64 {
    struct Message m;
    struct DosPacket64 p;
};

enum { POS_32, POS_STEPS, POS_64 };         /* how an image is positioned */

struct DvdDisc {
    BPTR fh;                                /* an image, or */
    struct MsgPort *port;                   /* a drive (SIGB_SINGLE, any task's; an image's 64-bit packets too) */
    struct IOStdReq *io;
    BOOL dev_open;
    UBYTE *bounce;                          /* CHUNK sectors, for the drive */
    ULONG b_from, b_n;                      /* what bounce holds (b_n 0: nothing) */
    ULONG pos;                              /* the next sector */
    ULONG sectors;
    ULONG errors;
    struct Pkt64 *pk;                       /* an image's: its packet, */
    UBYTE how;                              /*   POS_, */
    BOOL told;                              /*   a sector it could not reach logged, */
    void (*log)(APTR h, LONG level, CONST_STRPTR line);
    char what[64];                          /*   how it is positioned, for the log */
};

static void put_err(char *err, LONG len, const char *a, const char *b)
{
    LONG n = 0;
    if (!err || len <= 0)
        return;
    while (*a && n < len - 1)
        err[n++] = *a++;
    while (b && *b && n < len - 1)
        err[n++] = *b++;
    err[n] = 0;
}

/* A port for replies, which signals whichever task waits on it */
static BOOL make_port(struct DvdDisc *d)
{
    if (!(d->port = AllocVec(sizeof(*d->port), MEMF_PUBLIC | MEMF_CLEAR)))
        return FALSE;
    d->port->mp_Node.ln_Type = NT_MSGPORT;
    d->port->mp_Flags = PA_SIGNAL;
    d->port->mp_SigBit = SIGB_SINGLE;
    d->port->mp_SigTask = FindTask(NULL);
    NewList(&d->port->mp_MsgList);
    return TRUE;
}

/* ---- an image file */

/* A 64-bit packet to the image's handler: FALSE if it does not know it,
 * or it failed (dp_Res2 set: UAE's refused position has dp_Res1 -1);
 * *hi, *lo its result (dp_Res1). */
static BOOL pkt64(struct DvdDisc *d, LONG type, ULONG a_hi, ULONG a_lo, LONG mode, ULONG *hi,
                  ULONG *lo)
{
    struct FileHandle *fh = (struct FileHandle *)BADDR(d->fh);
    struct Pkt64 *k = d->pk;
    *hi = *lo = 0;
    if (!k && !(k = d->pk = AllocVec(sizeof(*k), MEMF_PUBLIC | MEMF_CLEAR)))
        return FALSE;
    if (!fh->fh_Type || (!d->port && !make_port(d)))
        return FALSE;                       /* (NIL:'s has no handler) */
    memset(&k->m, 0, sizeof(k->m));
    k->m.mn_Node.ln_Type = NT_MESSAGE;
    k->m.mn_Node.ln_Name = (char *)&k->p;
    k->m.mn_ReplyPort = d->port;
    k->m.mn_Length = sizeof(*k);
    dvdi_pkt64(&k->p, type, fh->fh_Arg1, a_hi, a_lo, mode);
    k->p.dp_Link = (ULONG)&k->m;
    k->p.dp_Port = (ULONG)d->port;
    d->port->mp_SigTask = FindTask(NULL);   /* the reply to whoever asks */
    SetSignal(0, SIGF_SINGLE);
    PutMsg(fh->fh_Type, &k->m);
    WaitPort(d->port);
    GetMsg(d->port);
    *hi = k->p.dp_Res1Hi;
    *lo = k->p.dp_Res1Lo;
    return dvdi_pkt64_known(&k->p) && !k->p.dp_Res2;
}

/* Why a sector could not be reached: logged once */
static void img_tell(struct DvdDisc *d, const char *m)
{
    if (d->told)
        return;
    d->told = TRUE;
    if (d->log)
        d->log(d, DVDLOG_ERROR, (CONST_STRPTR)m);
}

static BOOL img_to(struct DvdDisc *d, ULONG sector)
{
    ULONG hi, lo, r_hi, r_lo;
    LONG first, rest;
    if (d->how == POS_64) {
        dvdi_sector_pos(sector, &hi, &lo);
        if (pkt64(d, ACTION_CHANGE_FILE_POSITION64, hi, lo, OFFSET_BEGINNING, &r_hi, &r_lo) &&
            (r_hi | r_lo))
            return TRUE;
        img_tell(d, "image: a 64-bit position refused by its file system");
        return FALSE;
    }
    switch (dvdi_seek_steps(sector, &first, &rest)) {
    case 1:
        return Seek(d->fh, first, OFFSET_BEGINNING) != -1;
    case 2:
        if (Seek(d->fh, first, OFFSET_BEGINNING) != -1 && Seek(d->fh, rest, OFFSET_CURRENT) != -1)
            return TRUE;
        img_tell(d, "image: its file system cannot position past 2 GB "
                    "(no 64-bit packets, and Seek past 2 GB refused)");
        return FALSE;
    }
    img_tell(d, "image: a sector past 4 GB, and its file system has no 64-bit packets");
    return FALSE;
}

static LONG img_seek(APTR h, ULONG sector)
{
    struct DvdDisc *d = h;
    if (sector > d->sectors)
        return -1;
    if (!img_to(d, sector)) {
        d->errors++;
        return -1;
    }
    d->pos = sector;
    return 0;
}

static LONG img_read(APTR h, APTR buf, LONG n)
{
    struct DvdDisc *d = h;
    LONG got;
    if (n <= 0 || d->pos >= d->sectors)
        return 0;
    if ((ULONG)n > d->sectors - d->pos)
        n = (LONG)(d->sectors - d->pos);
    got = Read(d->fh, buf, n * DVD_BLOCK);
    if (got < 0) {
        d->errors++;
        return -1;
    }
    got /= DVD_BLOCK;
    d->pos += (ULONG)got;
    return got;
}

/* Its size, and how it is positioned: under 2 GB by Seek (fib_Size, a
 * LONG, positive); else by the 64-bit packets if its handler knows them
 * (its size asked for, then a position tried), else by Seek in two steps
 * under 4 GB (fib_Size taken as a ULONG). FALSE, with why in err, if it
 * cannot be read. */
static BOOL img_size(struct DvdDisc *d, const char *path, char *err, LONG errlen)
{
    struct FileInfoBlock *fib;
    ULONG size = 0, hi, lo, r_hi, r_lo;
    BOOL known = FALSE;
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (ExamineFH(d->fh, fib)) {
            size = (ULONG)fib->fib_Size;
            known = TRUE;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    if (known && size && size < 0x80000000UL) {
        d->how = POS_32;
        strcpy(d->what, "positioned by Seek");
        if ((d->sectors = size / DVD_BLOCK))
            return TRUE;
        put_err(err, errlen, "image too small: ", path);
        return FALSE;
    }
    /* 2 GB or more (or no size said): its handler's 64-bit packets */
    if (pkt64(d, ACTION_GET_FILE_SIZE64, 0, 0, 0, &hi, &lo) && !(hi & 0x80000000UL) &&
        (d->sectors = dvdi_size_sectors(hi, lo)) &&
        pkt64(d, ACTION_CHANGE_FILE_POSITION64, 0, 0, OFFSET_BEGINNING, &r_hi, &r_lo) &&
        (r_hi | r_lo)) {
        d->how = POS_64;
        strcpy(d->what, "positioned by 64-bit packets");
        return TRUE;
    }
    d->sectors = 0;
    if (!known || !size) {
        put_err(err, errlen, "image empty, or its size not to be had: ", path);
        return FALSE;
    }
    if (size == 0xFFFFFFFFUL) {
        /* (UAE's size for 4 GB or more; its packets were not answered) */
        put_err(err, errlen, "image of 4 GB or more, and its file system cannot position "
                "past 4 GB (no 64-bit packets): ", path);
        return FALSE;
    }
    d->sectors = size / DVD_BLOCK;
    d->how = POS_STEPS;
    strcpy(d->what, "positioned by Seek, in two steps past 2 GB");
    return TRUE;
}

struct DvdDisc *dvdd_open_image(const char *path, char *err, LONG errlen)
{
    struct DvdDisc *d = AllocVec(sizeof(*d), MEMF_ANY | MEMF_CLEAR);
    if (!d) {
        put_err(err, errlen, "out of memory", NULL);
        return NULL;
    }
    if (!(d->fh = Open((CONST_STRPTR)path, MODE_OLDFILE))) {
        put_err(err, errlen, "cannot open ", path);
        FreeVec(d);
        return NULL;
    }
    if (!img_size(d, path, err, errlen)) {
        dvdd_close(d);
        return NULL;
    }
    return d;
}

LONG dvdd_image_kind(const char *path, ULONG *sectors, char *err, LONG errlen)
{
    struct DvdDisc *d = dvdd_open_image(path, err, errlen);
    UBYTE *b;
    ULONG root, len;
    LONG kind = DVDI_NONE, n;
    BOOL udf;
    *sectors = 0;
    if (!d)
        return -1;
    *sectors = d->sectors;
    if (!(b = AllocVec(5 * DVD_BLOCK, MEMF_ANY))) {
        put_err(err, errlen, "out of memory", NULL);
        dvdd_close(d);
        return -1;
    }
    /* the volume descriptors, from sector 16: ISO 9660's, then UDF's */
    if (img_seek(d, 16) == 0 && (n = img_read(d, b, 5)) > 0) {
        if (dvdi_volume(b, n, &root, &len, &udf)) {
            if (len > 2 * DVD_BLOCK)
                len = 2 * DVD_BLOCK;        /* (a disc's root folder holds a few names) */
            n = (LONG)((len + DVD_BLOCK - 1) / DVD_BLOCK);
            kind = img_seek(d, root) == 0 && img_read(d, b, n) == n &&
                   dvdi_has_dir(b, len, "VIDEO_TS") ? DVDI_VIDEO : DVDI_ISO;
        } else if (udf)
            kind = DVDI_UDF;
    }
    FreeVec(b);
    dvdd_close(d);
    return kind;
}

const char *dvdd_how(struct DvdDisc *d)
{
    return d->fh ? d->what : "a drive";
}

/* ---- a drive, through HD_SCSICMD */

static LONG scsi(struct DvdDisc *d, UBYTE *cmd, UWORD cmdlen, APTR data, ULONG len, UBYTE *sense_key)
{
    struct SCSICmd sc;
    UBYTE sense[18];
    memset(&sc, 0, sizeof(sc));
    memset(sense, 0, sizeof(sense));
    sc.scsi_Data = (UWORD *)data;
    sc.scsi_Length = len;
    sc.scsi_Command = cmd;
    sc.scsi_CmdLength = cmdlen;
    sc.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    sc.scsi_SenseData = sense;
    sc.scsi_SenseLength = sizeof(sense);
    d->io->io_Command = HD_SCSICMD;
    d->io->io_Data = &sc;
    d->io->io_Length = sizeof(sc);
    d->port->mp_SigTask = FindTask(NULL);   /* the reply to whoever asks */
    SetSignal(0, SIGF_SINGLE);
    DoIO((struct IORequest *)d->io);
    *sense_key = sc.scsi_SenseActual >= 3 ? (UBYTE)(sense[2] & 15) : 0;
    if (d->io->io_Error || sc.scsi_Status)
        return -1;
    return (LONG)sc.scsi_Actual;
}

/* READ(10) of n sectors from lba into the bounce buffer, with retries */
static BOOL drive_read10(struct DvdDisc *d, ULONG lba, UWORD n)
{
    UBYTE cmd[10], key;
    LONG tries = 0, waits = 0;
    for (;;) {
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x28;
        cmd[2] = (UBYTE)(lba >> 24);
        cmd[3] = (UBYTE)(lba >> 16);
        cmd[4] = (UBYTE)(lba >> 8);
        cmd[5] = (UBYTE)lba;
        cmd[7] = (UBYTE)(n >> 8);
        cmd[8] = (UBYTE)n;
        if (scsi(d, cmd, 10, d->bounce, (ULONG)n * DVD_BLOCK, &key) == (LONG)n * DVD_BLOCK)
            return TRUE;
        d->errors++;
        if (key == 2 && waits < 20) {       /* not ready: spinning up */
            Delay(25);
            waits++;
            continue;
        }
        if (++tries >= 3)
            return FALSE;
    }
}

static LONG drive_seek(APTR h, ULONG sector)
{
    struct DvdDisc *d = h;
    if (d->sectors && sector > d->sectors)
        return -1;
    d->pos = sector;
    return 0;
}

static LONG drive_read(APTR h, APTR buf, LONG n)
{
    struct DvdDisc *d = h;
    UBYTE *out = buf;
    LONG done = 0;
    if (d->sectors && d->pos >= d->sectors)
        return 0;
    if (d->sectors && (ULONG)n > d->sectors - d->pos)
        n = (LONG)(d->sectors - d->pos);
    while (done < n) {
        ULONG c;
        if (!d->b_n || d->pos < d->b_from || d->pos >= d->b_from + d->b_n) {
            /* not read ahead already: CHUNK sectors from here */
            UWORD k = CHUNK;
            if (d->sectors && d->sectors - d->pos < k)
                k = (UWORD)(d->sectors - d->pos);
            d->b_n = 0;
            if (!drive_read10(d, d->pos, k))
                return done ? done : -1;
            d->b_from = d->pos;
            d->b_n = k;
        }
        c = d->b_from + d->b_n - d->pos;
        if (c > (ULONG)(n - done))
            c = (ULONG)(n - done);
        CopyMem(d->bounce + (d->pos - d->b_from) * DVD_BLOCK, out, c * DVD_BLOCK);
        out += c * DVD_BLOCK;
        d->pos += c;
        done += (LONG)c;
    }
    return done;
}

struct DvdDisc *dvdd_open_drive(const char *device, LONG unit, char *err, LONG errlen)
{
    struct DvdDisc *d = AllocVec(sizeof(*d), MEMF_ANY | MEMF_CLEAR);
    UBYTE cmd[10], cap[8], key;
    LONG tries;
    if (!d) {
        put_err(err, errlen, "out of memory", NULL);
        return NULL;
    }
    make_port(d);
    if (!(d->bounce = AllocVec(CHUNK * DVD_BLOCK, MEMF_PUBLIC)) || !d->port ||
        !(d->io = (struct IOStdReq *)CreateIORequest(d->port, sizeof(struct IOStdReq)))) {
        put_err(err, errlen, "out of memory", NULL);
        dvdd_close(d);
        return NULL;
    }
    if (OpenDevice((CONST_STRPTR)device, (ULONG)unit, (struct IORequest *)d->io, 0)) {
        put_err(err, errlen, "cannot open ", device);
        dvdd_close(d);
        return NULL;
    }
    d->dev_open = TRUE;
    /* READ CAPACITY: the last sector and the sector size (tried again
     * while the drive spins up, a few seconds at most) */
    for (tries = 0;; tries++) {
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x25;
        if (scsi(d, cmd, 10, cap, sizeof(cap), &key) == (LONG)sizeof(cap))
            break;
        if (key != 2 && key != 6) {         /* not "not ready" or "unit attention" */
            put_err(err, errlen, "no disc in the drive, or not a CD/DVD drive: ", device);
            dvdd_close(d);
            return NULL;
        }
        if (tries >= 40) {
            put_err(err, errlen, "the drive is not ready: ", device);
            dvdd_close(d);
            return NULL;
        }
        Delay(25);
    }
    if (((ULONG)cap[4] << 24 | (ULONG)cap[5] << 16 | (ULONG)cap[6] << 8 | cap[7]) != DVD_BLOCK) {
        put_err(err, errlen, "not a CD/DVD (sectors are not 2048 bytes): ", device);
        dvdd_close(d);
        return NULL;
    }
    d->sectors = ((ULONG)cap[0] << 24 | (ULONG)cap[1] << 16 | (ULONG)cap[2] << 8 | cap[3]) + 1;
    return d;
}

void dvdd_close(struct DvdDisc *d)
{
    if (!d)
        return;
    if (d->fh)
        Close(d->fh);
    if (d->dev_open)
        CloseDevice((struct IORequest *)d->io);
    if (d->io)
        DeleteIORequest((struct IORequest *)d->io);
    if (d->port)
        FreeVec(d->port);                   /* (no signal of its own to free) */
    if (d->bounce)
        FreeVec(d->bounce);
    if (d->pk)
        FreeVec(d->pk);
    FreeVec(d);
}

ULONG dvdd_sectors(struct DvdDisc *d)
{
    return d->sectors;
}

ULONG dvdd_errors(struct DvdDisc *d)
{
    return d->errors;
}

void dvdd_reader(struct DvdDisc *d, struct DvdReader *rd)
{
    rd->handle = d;
    rd->seek = d->fh ? img_seek : drive_seek;
    rd->read = d->fh ? img_read : drive_read;
    rd->log = NULL;
}

void dvdd_log(struct DvdDisc *d, void (*log)(APTR h, LONG level, CONST_STRPTR line))
{
    d->log = log;
}
