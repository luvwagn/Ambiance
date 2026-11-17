/*
 * Ambiance - DVD images: what one holds, and the 64-bit DOS packet that
 * positions in one (dvdiso.c)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * No OS calls here (dvddisc.c makes them): tested on the host
 * (tests/dvdiso_host.c).
 */
#ifndef AMCE_DVDISO_H
#define AMCE_DVDISO_H

#include <exec/types.h>

/* ---- 64-bit file positions
 *
 * A DVD image is 4.7 to 8.5 GB; DOS's Seek and fib_Size are 32-bit. The
 * packets MorphOS brought in for it, which UAE's (and Amiberry's) host
 * file system answers: a DosPacket64, its 64-bit fields in two longwords,
 * high first, on 8-byte boundaries (the offsets are filesys.cpp's). A
 * handler that knows the packet keeps dp_Res0 DP64_INIT; one that does not
 * replies DOSFALSE into it (where an ordinary packet's dp_Res1 is). */
#define ACTION_CHANGE_FILE_POSITION64 8001  /* Arg1 fh_Arg1, Arg2 position, Arg3 OFFSET_ mode */
#define ACTION_GET_FILE_SIZE64        8004  /* Arg1 fh_Arg1; Res1 the size (-1: an error) */
#define DP64_INIT                     (-3L)

struct DosPacket64 {
    ULONG dp_Link;              /*  0: its Message */
    ULONG dp_Port;              /*  4: the reply port */
    LONG  dp_Type;              /*  8 */
    LONG  dp_Res0;              /* 12: DP64_INIT */
    LONG  dp_Res2;              /* 16 */
    LONG  dp_Pad0;
    ULONG dp_Res1Hi, dp_Res1Lo; /* 24 */
    LONG  dp_Arg1;              /* 32 */
    LONG  dp_Pad1;
    ULONG dp_Arg2Hi, dp_Arg2Lo; /* 40 */
    LONG  dp_Arg3;              /* 48 */
    LONG  dp_Arg4;              /* 52 */
    ULONG dp_Arg5Hi, dp_Arg5Lo; /* 56 */
};

/* A packet made ready (link and port left to the sender): type, fh_Arg1,
 * a 64-bit argument (hi, lo), a mode. */
void dvdi_pkt64(struct DosPacket64 *p, LONG type, LONG arg1, ULONG hi, ULONG lo, LONG arg3);
/* Back from the handler: TRUE if it knew the packet */
BOOL dvdi_pkt64_known(const struct DosPacket64 *p);

/* A sector's byte position, as two longwords */
void dvdi_sector_pos(ULONG sector, ULONG *hi, ULONG *lo);
/* A byte size, in whole sectors (an image of 8 TB or more: 0) */
ULONG dvdi_size_sectors(ULONG hi, ULONG lo);

/* Without the packets, a 32-bit Seek: to a sector under 2 GB at once
 * (*first its position, *rest 0; 1), else, under 4 GB, in two steps: to
 * *first from the start, then *rest further on (2). 0 past that. */
LONG dvdi_seek_steps(ULONG sector, LONG *first, LONG *rest);

/* ---- what an image holds (its sectors 16 on, where volume descriptors
 * are, and its root folder: a few KB read) */
enum { DVDI_NONE, DVDI_ISO, DVDI_UDF, DVDI_VIDEO };

/* Sectors 16 on (n of them): ISO 9660's root folder (its first sector and
 * length in bytes) if a primary volume descriptor is there; *udf: UDF's
 * descriptors there too (a DVD has both: the "UDF bridge"). FALSE if no
 * ISO 9660. */
BOOL dvdi_volume(const UBYTE *s, LONG n, ULONG *root, ULONG *len, BOOL *udf);
/* An ISO 9660 folder's records (len bytes): a folder called name in it
 * (VIDEO_TS: a DVD-Video disc). */
BOOL dvdi_has_dir(const UBYTE *d, ULONG len, const char *name);

#endif
