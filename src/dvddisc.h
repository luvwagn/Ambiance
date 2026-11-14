/*
 * AmigaMCE - reading a DVD for amcedvd.library (dvddisc.c)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * A disc is read in 2048-byte sectors, from a drive (any exec device
 * that takes HD_SCSICMD: scsi.device, an ATAPI driver, a SCSI card's) or
 * from an image file. dvdd_reader() fills the DvdReader amcedvd.library
 * reads through.
 */
#ifndef AMCE_DVDDISC_H
#define AMCE_DVDDISC_H

#include <exec/types.h>
#include <libraries/amcedvd.h>

struct DvdDisc;

/* An image file (a .iso of a DVD-Video disc). NULL, with why in err, if
 * it cannot be read; images of 2 GB or more are not read yet (32-bit
 * Seek). */
struct DvdDisc *dvdd_open_image(const char *path, char *err, LONG errlen);
/* A drive: device and unit as in its mountlist or DEVS:DOSDrivers entry.
 * NULL, with why in err, if there is no disc in it or it is not a
 * 2048-byte-sector drive. */
struct DvdDisc *dvdd_open_drive(const char *device, LONG unit, char *err, LONG errlen);
void dvdd_close(struct DvdDisc *d);

ULONG dvdd_sectors(struct DvdDisc *d);      /* its size, 0 if not known */
ULONG dvdd_errors(struct DvdDisc *d);       /* reads that failed (retries included) */
void dvdd_reader(struct DvdDisc *d, struct DvdReader *rd);  /* rd->log left NULL */

#endif
