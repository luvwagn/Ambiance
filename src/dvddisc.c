/*
 * AmigaMCE - reading a DVD for amcedvd.library
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * Sectors of 2048 bytes, from a drive or an image file, behind the
 * DvdReader that amcedvd.library reads the disc through (its seek and
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
 * while a film plays, AmigaMCE's own between): its requests come back to
 * a port that signals whichever task sent them, with SIGF_SINGLE, exec's
 * signal for waiting on one thing.
 *
 * An image file is read with Seek and Read: 32-bit, so an image of 2 GB
 * or more is refused for now (a single-layer DVD is up to 4.7 GB; most
 * of that is the films, the menus and IFOs are near the start, but a
 * film's sectors past 2 GB could not be read).
 */
#include "dvddisc.h"

#include <exec/memory.h>
#include <exec/io.h>
#include <devices/scsidisk.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>
#include <string.h>

#define CHUNK 16                            /* sectors per SCSI read (32 KB) */

struct DvdDisc {
    BPTR fh;                                /* an image, or */
    struct MsgPort *port;                   /* a drive (SIGB_SINGLE, any task's) */
    struct IOStdReq *io;
    BOOL dev_open;
    UBYTE *bounce;                          /* CHUNK sectors, for the drive */
    ULONG b_from, b_n;                      /* what bounce holds (b_n 0: nothing) */
    ULONG pos;                              /* the next sector */
    ULONG sectors;
    ULONG errors;
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

/* ---- an image file */

static LONG img_seek(APTR h, ULONG sector)
{
    struct DvdDisc *d = h;
    if (sector > d->sectors)
        return -1;
    if (Seek(d->fh, (LONG)(sector * DVD_BLOCK), OFFSET_BEGINNING) < 0) {
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

struct DvdDisc *dvdd_open_image(const char *path, char *err, LONG errlen)
{
    struct DvdDisc *d = AllocVec(sizeof(*d), MEMF_ANY | MEMF_CLEAR);
    struct FileInfoBlock *fib;
    if (!d) {
        put_err(err, errlen, "out of memory", NULL);
        return NULL;
    }
    if (!(d->fh = Open((CONST_STRPTR)path, MODE_OLDFILE))) {
        put_err(err, errlen, "cannot open ", path);
        FreeVec(d);
        return NULL;
    }
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (ExamineFH(d->fh, fib)) {
            /* fib_Size is a LONG: 2 GB and over shows negative (or 0) */
            if (fib->fib_Size > 0)
                d->sectors = (ULONG)fib->fib_Size / DVD_BLOCK;
            else
                d->sectors = 0;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    if (!d->sectors) {
        put_err(err, errlen, "image empty, or 2 GB or more (not read yet): ", path);
        dvdd_close(d);
        return NULL;
    }
    return d;
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
    if ((d->port = AllocVec(sizeof(*d->port), MEMF_PUBLIC | MEMF_CLEAR))) {
        d->port->mp_Node.ln_Type = NT_MSGPORT;
        d->port->mp_Flags = PA_SIGNAL;
        d->port->mp_SigBit = SIGB_SINGLE;
        d->port->mp_SigTask = FindTask(NULL);
        NewList(&d->port->mp_MsgList);
    }
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
