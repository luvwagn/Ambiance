/*
 * AmigaMCE - DVD images: what one holds, and the 64-bit DOS packet that
 * positions in one
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * What an image holds: ISO 9660's primary volume descriptor at sector 16
 * names the root folder, and a DVD-Video disc has VIDEO_TS in it. DVDs
 * are UDF, with ISO 9660 beside it on the same files (the "UDF bridge"
 * every DVD player's disc has); an image of UDF alone is only said to be
 * UDF here (dvdnav.library reads UDF, and says when it plays).
 *
 * Positions past 2 GB: see dvdiso.h.
 */
#include "dvdiso.h"

#include <stddef.h>

#define BLOCK 2048

/* the packet's fields where UAE's filesys.cpp looks for them */
typedef char dp64_layout[offsetof(struct DosPacket64, dp_Res0) == 12 &&
                         offsetof(struct DosPacket64, dp_Res2) == 16 &&
                         offsetof(struct DosPacket64, dp_Res1Hi) == 24 &&
                         offsetof(struct DosPacket64, dp_Arg1) == 32 &&
                         offsetof(struct DosPacket64, dp_Arg2Hi) == 40 &&
                         offsetof(struct DosPacket64, dp_Arg3) == 48 &&
                         sizeof(struct DosPacket64) == 64 ? 1 : -1];

void dvdi_pkt64(struct DosPacket64 *p, LONG type, LONG arg1, ULONG hi, ULONG lo, LONG arg3)
{
    p->dp_Type = type;
    p->dp_Res0 = DP64_INIT;
    p->dp_Res2 = 0;
    p->dp_Pad0 = 0;
    p->dp_Res1Hi = p->dp_Res1Lo = 0;
    p->dp_Arg1 = arg1;
    p->dp_Pad1 = 0;
    p->dp_Arg2Hi = hi;
    p->dp_Arg2Lo = lo;
    p->dp_Arg3 = arg3;
    p->dp_Arg4 = 0;
    p->dp_Arg5Hi = p->dp_Arg5Lo = 0;
}

BOOL dvdi_pkt64_known(const struct DosPacket64 *p)
{
    return p->dp_Res0 == DP64_INIT && p->dp_Res2 != 209;  /* (ERROR_ACTION_NOT_KNOWN) */
}

void dvdi_sector_pos(ULONG sector, ULONG *hi, ULONG *lo)
{
    *hi = sector >> 21;
    *lo = sector << 11;
}

ULONG dvdi_size_sectors(ULONG hi, ULONG lo)
{
    if (hi >> 21)
        return 0;
    return hi << 21 | lo >> 11;
}

LONG dvdi_seek_steps(ULONG sector, LONG *first, LONG *rest)
{
    if (sector < 0x100000UL) {              /* under 2 GB */
        *first = (LONG)(sector * BLOCK);
        *rest = 0;
        return 1;
    }
    if (sector < 0x1FFFFFUL) {              /* under 4 GB: 2 GB less a sector, then on */
        *first = 0x7FFFF800L;
        *rest = (LONG)((sector - 0xFFFFFUL) * BLOCK);
        return 2;
    }
    return 0;
}

static ULONG le32(const UBYTE *b)
{
    return (ULONG)b[0] | (ULONG)b[1] << 8 | (ULONG)b[2] << 16 | (ULONG)b[3] << 24;
}

static BOOL is(const UBYTE *s, const char *id)
{
    LONG i;
    for (i = 0; i < 5; i++)
        if (s[1 + i] != (UBYTE)id[i])
            return FALSE;
    return TRUE;
}

BOOL dvdi_volume(const UBYTE *s, LONG n, ULONG *root, ULONG *len, BOOL *udf)
{
    BOOL iso = FALSE, term = FALSE;
    LONG k;
    *udf = FALSE;
    *root = *len = 0;
    for (k = 0; k < n; k++, s += BLOCK) {
        if (is(s, "BEA01") || is(s, "NSR02") || is(s, "NSR03"))
            *udf = TRUE;
        else if (is(s, "CD001") && !term) {
            if (s[0] == 1 && !iso) {        /* the primary one */
                *root = le32(s + 156 + 2);
                *len = le32(s + 156 + 10);
                iso = *len > 0;
            } else if (s[0] == 255)
                term = TRUE;                /* (the set's end: UDF's follow) */
        } else if (!is(s, "TEA01") && !is(s, "BOOT2") && !is(s, "CDW02"))
            break;                          /* nothing more of either */
    }
    return iso;
}

static UBYTE up(UBYTE c)
{
    return c >= 'a' && c <= 'z' ? (UBYTE)(c - 32) : c;
}

BOOL dvdi_has_dir(const UBYTE *d, ULONG len, const char *name)
{
    ULONG i = 0;
    while (i < len) {
        const UBYTE *r = d + i;
        ULONG rl = r[0], nl, k;
        if (!rl) {                          /* the rest of this sector unused */
            i = (i / BLOCK + 1) * BLOCK;
            continue;
        }
        if (rl < 34 || i + rl > len)
            break;
        nl = r[32];
        if ((r[25] & 2) && 33 + nl <= rl) { /* a folder */
            for (k = 0; k < nl && name[k] && up(r[33 + k]) == up((UBYTE)name[k]); k++)
                ;
            if (!name[k] && (k == nl || r[33 + k] == ';' || r[33 + k] == '.'))
                return TRUE;
        }
        i += rl;
    }
    return FALSE;
}
