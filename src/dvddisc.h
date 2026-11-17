/*
 * Ambiance - reading a DVD for dvdnav.library (dvddisc.c)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * A disc is read in 2048-byte sectors, from a drive (any exec device
 * that takes HD_SCSICMD: scsi.device, an ATAPI driver, a SCSI card's) or
 * from an image file. dvdd_reader() fills the DvdReader dvdnav.library
 * reads through.
 */
#ifndef AMB_DVDDISC_H
#define AMB_DVDDISC_H

#include <exec/types.h>
#include <libraries/dvdnav.h>

struct DvdDisc;

/* An image file (a .iso of a DVD-Video disc). NULL, with why in err, if
 * it cannot be read: past 2 GB its file system must answer the 64-bit
 * packets (UAE's does), or take Seek past 2 GB (up to 4 GB then). */
struct DvdDisc *dvdd_open_image(const char *path, char *err, LONG errlen);
/* What an image file holds (dvdiso.h's DVDI_: a DVD-Video disc's, a disc's
 * of UDF alone, of ISO 9660, none), a few KB of it read; *sectors its
 * size. -1, with why in err, if it cannot be read. */
LONG dvdd_image_kind(const char *path, ULONG *sectors, char *err, LONG errlen);
const char *dvdd_how(struct DvdDisc *d);    /* how it is read, for the log */
/* A drive: device and unit as in its mountlist or DEVS:DOSDrivers entry.
 * NULL, with why in err, if there is no disc in it or it is not a
 * 2048-byte-sector drive. */
struct DvdDisc *dvdd_open_drive(const char *device, LONG unit, char *err, LONG errlen);
void dvdd_close(struct DvdDisc *d);

ULONG dvdd_sectors(struct DvdDisc *d);      /* its size, 0 if not known */
ULONG dvdd_errors(struct DvdDisc *d);       /* reads that failed (retries included) */
void dvdd_reader(struct DvdDisc *d, struct DvdReader *rd);  /* rd->log left NULL */
/* Where to say that an image's sector could not be reached (once) */
void dvdd_log(struct DvdDisc *d, void (*log)(APTR h, LONG level, CONST_STRPTR line));

#endif
