/*
 * Ambiance - Music's own files: what is in them (tags, format, length)
 * and their pictures
 * Copyright (c) 2026 Brian King
 *
 * Read for the details pane (the loader) and for Now playing: an MP3's
 * ID3v2 (2.2 to 2.4) or ID3v1 tag and its first frame (a Xing or Info
 * frame gives a VBR file's length), a FLAC's STREAMINFO, Vorbis comment
 * and PICTURE blocks, WAV, AIFF and 8SVX headers (pcm_header, also what
 * stream.c plays them by). A file's picture is its folder's cover file
 * (cover, folder, front: PNG before JPEG) or else the one inside it
 * (ID3's APIC, FLAC's PICTURE), copied out to RAM:Ambiance-art/ under a
 * name made of the album and its size, so the tracks of an album share it.
 */
#include "amce.h"
#include "radiocsv.h"

#include <string.h>
#include <stdio.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>


/* ---- PCM files: WAV, AIFF (and AIFC uncompressed), 8SVX */

/* An AIFF's rate: an 80-bit IEEE extended number, whole Hz. */
static ULONG ext80(const UBYTE *b)
{
    int e = ((b[0] & 0x7F) << 8 | b[1]) - 16383 - 31;
    ULONG m = amce_be32(b + 2);
    if (e > 0 || e < -31)
        return 0;                           /* past 4 GHz, or under 1 Hz */
    return m >> -e;
}

/* The header of a PCM file in b (n bytes from its start): where its
 * samples start and how they are stored. FALSE: not one of these, or a
 * kind not played (compressed). */
BOOL pcm_header(const UBYTE *b, LONG n, struct PcmFmt *f)
{
    LONG p;
    BOOL wav, aiff = FALSE, svx = FALSE, have_fmt = FALSE;
    memset(f, 0, sizeof(*f));
    if (n < 12)
        return FALSE;
    wav = !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4);
    if (!memcmp(b, "FORM", 4)) {
        aiff = !memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4);
        svx = !memcmp(b + 8, "8SVX", 4);
    }
    if (!wav && !aiff && !svx)
        return FALSE;
    f->kind = wav ? PCM_WAV : aiff ? PCM_AIFF : PCM_8SVX;
    f->big = !wav;
    f->signed8 = !wav;                      /* WAV's 8-bit samples are unsigned */
    f->channels = 1;
    for (p = 12; p + 8 <= n;) {
        const UBYTE *c = b + p;
        ULONG len = wav ? amce_le32(c + 4) : amce_be32(c + 4);
        const UBYTE *d = c + 8;
        LONG have = n - (p + 8);
        if (wav && !memcmp(c, "fmt ", 4) && have >= 16) {
            UWORD tag = amce_le16(d);
            if (tag == 0xFFFE && len >= 26 && have >= 26)
                tag = amce_le16(d + 24);         /* WAVE_FORMAT_EXTENSIBLE: its sub-format */
            if (tag != 1)
                return FALSE;               /* not PCM */
            f->channels = amce_le16(d + 2);
            f->rate = amce_le32(d + 4);
            f->bits = amce_le16(d + 14);
            have_fmt = TRUE;
        } else if (aiff && !memcmp(c, "COMM", 4) && have >= 18) {
            f->channels = amce_be16(d);
            f->frames = amce_be32(d + 2);
            f->bits = amce_be16(d + 6);
            f->rate = ext80(d + 8);
            if (!memcmp(b + 8, "AIFC", 4) && have >= 22) {
                if (!memcmp(d + 18, "sowt", 4))
                    f->big = FALSE;         /* little-endian 16-bit */
                else if (memcmp(d + 18, "NONE", 4) && memcmp(d + 18, "twos", 4))
                    return FALSE;           /* compressed */
            }
            have_fmt = TRUE;
        } else if (svx && !memcmp(c, "VHDR", 4) && have >= 20) {
            f->rate = amce_be16(d + 12);
            f->bits = 8;
            if (d[15])
                return FALSE;               /* Fibonacci-delta packed */
            have_fmt = TRUE;
        } else if (svx && !memcmp(c, "CHAN", 4) && have >= 4) {
            if (amce_be32(d) == 6)
                f->svx_stereo = TRUE;       /* left, then right: the left played */
        } else if ((wav && !memcmp(c, "data", 4)) || (svx && !memcmp(c, "BODY", 4))) {
            f->off = p + 8;
            f->bytes = len;
            break;
        } else if (aiff && !memcmp(c, "SSND", 4) && have >= 8) {
            f->off = p + 8 + 8 + amce_be32(d);   /* its offset field, then the samples */
            f->bytes = len >= 8 ? len - 8 - amce_be32(d) : 0;
            break;
        }
        if (len > (ULONG)(n - p))
            break;                          /* past what was read: the samples not found */
        p += 8 + len + (len & 1);           /* chunks are padded to even */
    }
    if (!have_fmt || !f->off || !f->rate || !f->channels || f->channels > 8 ||
        (f->bits != 8 && f->bits != 16 && f->bits != 24))
        return FALSE;
    if (f->svx_stereo)
        f->bytes /= 2;
    f->align = (UWORD)(f->channels * (f->bits / 8));
    if (!f->frames || f->kind != PCM_AIFF)
        f->frames = f->bytes / f->align;
    return TRUE;
}

/* ---- tags */
static void put_text(char *out, LONG max, const UBYTE *s, LONG n, int enc)
{
    LONG i, o = 0;
    if (max <= 0)
        return;
    if (enc == 1 || enc == 2) {
        /* UTF-16 (1: with a byte order mark): as Latin-1, the rest '?' */
        BOOL le = enc == 1;
        i = 0;
        if (enc == 1 && n >= 2) {
            le = !(s[0] == 0xFE && s[1] == 0xFF);
            if ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF))
                i = 2;
        }
        for (; i + 1 < n && o < max - 1; i += 2) {
            UWORD u = le ? (UWORD)(s[i] | s[i + 1] << 8) : (UWORD)(s[i] << 8 | s[i + 1]);
            if (!u)
                break;
            out[o++] = u < 256 ? (char)u : '?';
        }
        out[o] = 0;
    } else {
        for (i = 0; i < n && s[i] && o < max - 1; i++)
            out[o++] = (char)s[i];
        out[o] = 0;
        if (enc == 3)
            utf8_to_latin1(out);
    }
    squeeze_spaces(out);
}

static void set_if_empty(char *dst, LONG max, const UBYTE *s, LONG n, int enc)
{
    if (!dst[0])
        put_text(dst, max, s, n, enc);
}

/* The size of the ID3v2 tag at b (10 bytes), 0 none. */
static ULONG id3_size(const UBYTE *b)
{
    if (memcmp(b, "ID3", 3) || b[3] < 2 || b[3] > 4)
        return 0;
    return 10 + (((ULONG)b[6] & 0x7F) << 21 | ((ULONG)b[7] & 0x7F) << 14 |
                 ((ULONG)b[8] & 0x7F) << 7 | ((ULONG)b[9] & 0x7F)) + ((b[5] & 0x10) ? 10 : 0);
}

struct Art {
    LONG off, len;              /* the picture's bytes in the file, 0 none */
    BOOL png;
};

static ULONG syncsafe(const UBYTE *b)
{
    return ((ULONG)b[0] & 0x7F) << 21 | ((ULONG)b[1] & 0x7F) << 14 | ((ULONG)b[2] & 0x7F) << 7 |
           ((ULONG)b[3] & 0x7F);
}

/* An ID3v2 tag's frames: the text wanted, and where its picture is. */
static void read_id3(BPTR fh, const UBYTE *h, struct SongInfo *si, struct Art *art)
{
    UBYTE ver = h[3], fl = h[5];
    LONG end = (LONG)id3_size(h) - ((fl & 0x10) ? 10 : 0), pos = 10;
    UBYTE fb[10], buf[260];
    if (fl & 0x80)
        return;                             /* unsynchronised: rare, left alone */
    if ((fl & 0x40) && ver >= 3) {
        /* an extended header: passed over */
        Seek(fh, 10, OFFSET_BEGINNING);
        if (Read(fh, fb, 4) != 4)
            return;
        pos += ver == 4 ? (LONG)syncsafe(fb) : (LONG)amce_be32(fb) + 4;
    }
    while (pos + (ver == 2 ? 6 : 10) <= end) {
        char id[5];
        LONG flen, hl = ver == 2 ? 6 : 10, skip = 0;
        UWORD ffl = 0;
        if (Seek(fh, pos, OFFSET_BEGINNING) < 0 || Read(fh, fb, hl) != hl || !fb[0])
            break;                          /* padding */
        if (ver == 2) {
            memcpy(id, fb, 3);
            id[3] = 0;
            flen = (LONG)fb[3] << 16 | (LONG)fb[4] << 8 | fb[5];
        } else {
            memcpy(id, fb, 4);
            id[4] = 0;
            flen = ver == 4 ? (LONG)syncsafe(fb + 4) : (LONG)amce_be32(fb + 4);
            ffl = amce_be16(fb + 8);
        }
        if (flen <= 0 || pos + hl + flen > end)
            break;
        if (ver == 4) {
            if (ffl & 0x000E)
                goto next;                  /* compressed, encrypted, unsynchronised */
            if (ffl & 0x0040)
                skip++;                     /* its group */
            if (ffl & 0x0001)
                skip += 4;                  /* its length again */
        } else if (ver == 3 && (ffl & 0x00C0)) {
            goto next;                      /* compressed or encrypted */
        }
        if (id[0] == 'T' || !strcmp(id, "COMM")) {
            LONG k = flen - skip > (LONG)sizeof(buf) ? (LONG)sizeof(buf) : flen - skip;
            char *dst = NULL;
            LONG max = 0;
            if (Read(fh, buf, skip) != skip || k <= 1 || Read(fh, buf, k) != k)
                goto next;
            if (!strcmp(id, "TIT2") || !strcmp(id, "TT2"))
                dst = si->title, max = sizeof(si->title);
            else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1"))
                dst = si->artist, max = sizeof(si->artist);
            else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2"))
                dst = si->band, max = sizeof(si->band);
            else if (!strcmp(id, "TALB") || !strcmp(id, "TAL"))
                dst = si->album, max = sizeof(si->album);
            else if (!strcmp(id, "TYER") || !strcmp(id, "TYE") || !strcmp(id, "TDRC"))
                dst = si->year, max = sizeof(si->year);
            else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK"))
                dst = si->track, max = sizeof(si->track);
            else if (!strcmp(id, "TCON") || !strcmp(id, "TCO"))
                dst = si->genre, max = sizeof(si->genre);
            if (dst)
                set_if_empty(dst, max, buf + 1, k - 1, buf[0]);
        } else if ((!strcmp(id, "APIC") || !strcmp(id, "PIC")) && !art->len) {
            /* encoding, its kind (a MIME type; v2.2 three letters), the
             * picture's type, a description, then the picture */
            LONG k = flen - skip > (LONG)sizeof(buf) ? (LONG)sizeof(buf) : flen - skip, i;
            int enc;
            if (Read(fh, buf, skip) != skip || Read(fh, buf, k) != k || k < 6)
                goto next;
            enc = buf[0];
            if (ver == 2) {
                art->png = !memcmp(buf + 1, "PNG", 3);
                i = 5;
            } else {
                for (i = 1; i < k && buf[i]; i++)
                    ;
                art->png = strstr((char *)buf + 1, "png") != NULL ||
                           strstr((char *)buf + 1, "PNG") != NULL;
                i += 2;                     /* its end, the picture's type */
            }
            if (enc == 1 || enc == 2) {
                while (i + 1 < k && (buf[i] || buf[i + 1]))
                    i += 2;
                i += 2;
            } else {
                while (i < k && buf[i])
                    i++;
                i++;
            }
            if (i < k && flen - skip - i > 64) {
                art->off = pos + hl + skip + i;
                art->len = flen - skip - i;
            }
        }
    next:
        pos += hl + flen;
    }
}

static void read_id3v1(BPTR fh, LONG size, struct SongInfo *si)
{
    UBYTE t[128];
    if (size < 128 || Seek(fh, size - 128, OFFSET_BEGINNING) < 0 || Read(fh, t, 128) != 128 ||
        memcmp(t, "TAG", 3))
        return;
    set_if_empty(si->title, sizeof(si->title), t + 3, 30, 0);
    set_if_empty(si->artist, sizeof(si->artist), t + 33, 30, 0);
    set_if_empty(si->album, sizeof(si->album), t + 63, 30, 0);
    set_if_empty(si->year, sizeof(si->year), t + 93, 4, 0);
}

/* An MP3's first frame (after its tag): its rate, channels, bitrate and,
 * with a Xing or Info header, its length. */
static void read_mp3(BPTR fh, LONG from, LONG size, struct SongInfo *si)
{
    static const UWORD br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224,
                                   256, 320, 0 };
    static const UWORD br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144,
                                   160, 0 };
    static const UWORD sr[3] = { 44100, 48000, 32000 };
    static UBYTE b[4096];
    LONG n, i;
    if (Seek(fh, from, OFFSET_BEGINNING) < 0 || (n = Read(fh, b, sizeof(b))) < 64)
        return;
    for (i = 0; i + 40 < n; i++) {
        UBYTE v, layer, bi, si_, mode;
        ULONG rate, kbps, frames;
        LONG x;
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0)
            continue;
        v = (b[i + 1] >> 3) & 3;            /* 3: MPEG 1, 2: 2, 0: 2.5 */
        layer = (b[i + 1] >> 1) & 3;        /* 1: layer III */
        bi = b[i + 2] >> 4;
        si_ = (b[i + 2] >> 2) & 3;
        mode = b[i + 3] >> 6;
        if (v == 1 || layer != 1 || bi == 0 || bi == 15 || si_ == 3)
            continue;
        rate = sr[si_] >> (v == 3 ? 0 : v == 2 ? 1 : 2);
        kbps = v == 3 ? br1[bi] : br2[bi];
        si->rate = rate;
        si->channels = mode == 3 ? 1 : 2;
        si->kbps = kbps;
        /* a Xing (VBR) or Info (CBR) frame: the number of frames */
        x = i + 4 + (v == 3 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17));
        if (x + 12 < n && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4)) &&
            (b[x + 7] & 1) && (frames = amce_be32(b + x + 8))) {
            ULONG spf = v == 3 ? 1152 : 576;
            si->ms = (ULONG)(((unsigned long long)frames * spf * 1000) / rate);
            if (si->ms)
                si->kbps = (ULONG)(((unsigned long long)(size - from) * 8) / si->ms);
        } else if (kbps) {
            si->ms = (ULONG)(size - from) / kbps * 8;
        }
        return;
    }
}

/* A FLAC's metadata blocks: STREAMINFO, the Vorbis comment, PICTURE. */
static void read_flac(BPTR fh, LONG size, struct SongInfo *si, struct Art *art)
{
    UBYTE h[4], b[300];
    LONG pos = 4;
    BOOL last = FALSE;
    while (!last && pos + 4 <= size) {
        LONG len;
        UBYTE type;
        if (Seek(fh, pos, OFFSET_BEGINNING) < 0 || Read(fh, h, 4) != 4)
            return;
        last = (h[0] & 0x80) != 0;
        type = h[0] & 0x7F;
        len = (LONG)h[1] << 16 | (LONG)h[2] << 8 | h[3];
        if (type == 0 && len >= 18 && Read(fh, b, 18) == 18) {
            ULONG rate = (ULONG)b[10] << 12 | (ULONG)b[11] << 4 | b[12] >> 4;
            unsigned long long total = ((unsigned long long)(b[13] & 0x0F) << 32) | amce_be32(b + 14);
            si->rate = rate;
            si->channels = ((b[12] >> 1) & 7) + 1;
            si->bits = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
            if (rate)
                si->ms = (ULONG)(total * 1000 / rate);
            if (si->ms)
                si->kbps = (ULONG)(((unsigned long long)size * 8) / si->ms);
        } else if (type == 4 && len >= 8) {
            /* the vendor, then "NAME=value" comments, lengths little-endian */
            LONG p = pos + 4, end = pos + 4 + len, cnt, i;
            if (Read(fh, h, 4) != 4)
                return;
            p += 4 + amce_le32(h);
            if (Seek(fh, p, OFFSET_BEGINNING) < 0 || Read(fh, h, 4) != 4)
                return;
            cnt = amce_le32(h);
            p += 4;
            for (i = 0; i < cnt && p + 4 <= end; i++) {
                LONG cl, k;
                char *eq;
                if (Seek(fh, p, OFFSET_BEGINNING) < 0 || Read(fh, h, 4) != 4)
                    return;
                cl = amce_le32(h);
                k = cl < (LONG)sizeof(b) - 1 ? cl : (LONG)sizeof(b) - 1;
                if (cl < 0 || Read(fh, b, k) != k)
                    return;
                b[k] = 0;
                if ((eq = strchr((char *)b, '='))) {
                    char *v = eq + 1;
                    LONG vn = k - (v - (char *)b);
                    *eq = 0;
                    if (!amce_stricmp((char *)b, "TITLE"))
                        set_if_empty(si->title, sizeof(si->title), (UBYTE *)v, vn, 3);
                    else if (!amce_stricmp((char *)b, "ARTIST"))
                        set_if_empty(si->artist, sizeof(si->artist), (UBYTE *)v, vn, 3);
                    else if (!amce_stricmp((char *)b, "ALBUMARTIST"))
                        set_if_empty(si->band, sizeof(si->band), (UBYTE *)v, vn, 3);
                    else if (!amce_stricmp((char *)b, "ALBUM"))
                        set_if_empty(si->album, sizeof(si->album), (UBYTE *)v, vn, 3);
                    else if (!amce_stricmp((char *)b, "DATE"))
                        set_if_empty(si->year, sizeof(si->year), (UBYTE *)v, vn, 3);
                    else if (!amce_stricmp((char *)b, "TRACKNUMBER"))
                        set_if_empty(si->track, sizeof(si->track), (UBYTE *)v, vn, 3);
                    else if (!amce_stricmp((char *)b, "GENRE"))
                        set_if_empty(si->genre, sizeof(si->genre), (UBYTE *)v, vn, 3);
                }
                p += 4 + cl;
            }
        } else if (type == 6 && len >= 32 && !art->len) {
            /* its type, a MIME type, a description, its size and colours,
             * then the picture */
            LONG p = pos + 4, ml, dl;
            if (Read(fh, b, 8) != 8)
                return;
            ml = amce_be32(b + 4);
            if (ml < 0 || ml > 64 || Read(fh, b, ml + 4) != ml + 4)
                return;
            b[ml] = 0;
            art->png = strstr((char *)b, "png") != NULL;
            dl = amce_be32(b + ml);
            p += 8 + ml + 4 + dl + 16;
            if (Seek(fh, p, OFFSET_BEGINNING) < 0 || Read(fh, h, 4) != 4)
                return;
            if (amce_be32(h) > 64 && p + 4 + (LONG)amce_be32(h) <= pos + 4 + len) {
                art->off = p + 4;
                art->len = amce_be32(h);
            }
        }
        pos += 4 + len;
    }
}

/* ID3's genres by number ("(12)", "12"): the first 80, ID3v1's own. */
static const char *const genres[80] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz",
    "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno",
    "Industrial", "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno",
    "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
    "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock", "Bass", "Soul",
    "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer",
    "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
    "Hard Rock"
};

static void genre_name(char *g, LONG max)
{
    const char *s = g;
    ULONG v = 0;
    BOOL paren = *s == '(';
    if (paren)
        s++;
    if (*s < '0' || *s > '9')
        return;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    if (paren && *s == ')')
        s++;
    if (*s && *s != ' ') {
        if (paren)
            memmove(g, s, strlen(s) + 1);   /* "(12)Other": the name after it */
        return;
    }
    if (v < 80)
        amce_strlcpy(g, genres[v], max);
    else
        g[0] = 0;
}

/* ---- pictures */
static void join(char *out, LONG max, const char *dir, const char *name)
{
    LONG n;
    amce_strlcpy(out, dir, max);
    n = strlen(out);
    if (n && out[n - 1] != ':' && out[n - 1] != '/')
        amce_strlcat(out, "/", max);
    amce_strlcat(out, name, max);
}

/* A folder's cover file into out: TRUE if it has one. */
BOOL song_folder_art(const char *dir, char *out, LONG max)
{
    static const char *const names[] = { "cover.png", "folder.png", "front.png", "cover.jpg",
                                         "folder.jpg", "front.jpg", "cover.jpeg", "folder.jpeg",
                                         "AlbumArtSmall.jpg", "albumart.jpg", NULL };
    int i;
    for (i = 0; names[i]; i++) {
        join(out, max, dir, names[i]);
        if (amce_exists(out))
            return TRUE;
    }
    out[0] = 0;
    return FALSE;
}

/* The picture in a file copied out to RAM: (as its name: what the tracks of
 * its album share), or there already. */
static BOOL art_out(BPTR fh, const char *path, const struct SongInfo *si, const struct Art *a,
                    char *out, LONG max, BOOL extract)
{
    ULONG hash = 5381;
    const char *s;
    BPTR o, l;
    static UBYTE buf[8192];
    LONG left;
    for (s = si->album[0] ? si->album : path; *s; s++)
        hash = hash * 33 + (UBYTE)*s;
    for (s = si->band[0] ? si->band : si->artist; *s; s++)
        hash = hash * 33 + (UBYTE)*s;
    hash = hash * 33 + (ULONG)a->len;
    sprintf(out, "RAM:Ambiance-art/s%08lx.%s", (unsigned long)hash, a->png ? "png" : "jpg");
    (void)max;
    if (amce_exists(out))
        return TRUE;
    if (!extract)
        return TRUE;                        /* where it would be */
    if ((l = Lock((CONST_STRPTR)"RAM:Ambiance-art", SHARED_LOCK)))
        UnLock(l);
    else if ((l = CreateDir((CONST_STRPTR)"RAM:Ambiance-art")))
        UnLock(l);
    if (a->len > 4L << 20 || Seek(fh, a->off, OFFSET_BEGINNING) < 0 ||
        !(o = Open((CONST_STRPTR)out, MODE_NEWFILE))) {
        out[0] = 0;
        return FALSE;
    }
    for (left = a->len; left > 0;) {
        LONG k = left > (LONG)sizeof(buf) ? (LONG)sizeof(buf) : left;
        if (Read(fh, buf, k) != k || Write(o, buf, k) != k)
            break;
        left -= k;
    }
    Close(o);
    if (left > 0) {
        DeleteFile((CONST_STRPTR)out);
        out[0] = 0;
        return FALSE;
    }
    return TRUE;
}

/* What a music file is (si), and its picture into art ("" none): its
 * folder's cover, or the one inside it (extract: copied out to RAM:; else
 * only named). FALSE if the file cannot be read. */
BOOL song_info(const char *path, struct SongInfo *si, char *art, LONG artmax, BOOL extract)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    UBYTE h[16];
    BPTR fh;
    LONG size, tag;
    struct Art a = { 0, 0, FALSE };
    char dir[300];
    memset(si, 0, sizeof(*si));
    if (art)
        art[0] = 0;
    me->pr_WindowPtr = (APTR)-1;
    if (!(fh = Open((CONST_STRPTR)path, MODE_OLDFILE))) {
        me->pr_WindowPtr = old;
        return FALSE;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    si->size = size > 0 ? (ULONG)size : 0;
    if (Read(fh, h, 16) != 16) {
        Close(fh);
        me->pr_WindowPtr = old;
        return FALSE;
    }
    if (!memcmp(h, "fLaC", 4)) {
        amce_strlcpy(si->fmt, "FLAC", sizeof(si->fmt));
        read_flac(fh, size, si, &a);
    } else {
        static UBYTE pb[2048];
        struct PcmFmt f;
        LONG n;
        Seek(fh, 0, OFFSET_BEGINNING);
        n = Read(fh, pb, sizeof(pb));
        if (n > 0 && pcm_header(pb, n, &f)) {
            if (size > 0 && f.off + f.bytes > (ULONG)size) {
                f.bytes = (ULONG)size > f.off ? (ULONG)size - f.off : 0;
                f.frames = f.bytes / f.align;   /* a WAV written as it streamed */
            }
            amce_strlcpy(si->fmt, f.kind == PCM_WAV ? "WAV" : f.kind == PCM_AIFF ? "AIFF" : "8SVX",
                         sizeof(si->fmt));
            si->rate = f.rate;
            si->channels = f.svx_stereo ? 1 : f.channels;
            si->bits = f.bits;
            if (f.rate)
                si->ms = (f.frames / f.rate) * 1000 + (f.frames % f.rate) * 1000 / f.rate;
            si->kbps = (f.rate * f.align * 8 + 500) / 1000;
            {
                /* NAME and AUTH (AIFF, 8SVX): its title and author */
                LONG p = 12;
                while (f.big && p + 8 <= n) {
                    ULONG len = amce_be32(pb + p + 4);
                    if (len > (ULONG)n)
                        break;
                    if (!memcmp(pb + p, "NAME", 4))
                        set_if_empty(si->title, sizeof(si->title), pb + p + 8,
                                     (LONG)len < n - p - 8 ? (LONG)len : n - p - 8, 0);
                    else if (!memcmp(pb + p, "AUTH", 4))
                        set_if_empty(si->artist, sizeof(si->artist), pb + p + 8,
                                     (LONG)len < n - p - 8 ? (LONG)len : n - p - 8, 0);
                    p += 8 + len + (len & 1);
                }
            }
        } else {
            amce_strlcpy(si->fmt, "MP3", sizeof(si->fmt));
            tag = id3_size(h);
            if (tag)
                read_id3(fh, h, si, &a);
            if (!si->title[0] || !si->artist[0])
                read_id3v1(fh, size, si);
            read_mp3(fh, tag, size, si);
        }
    }
    if (art) {
        LONG k;
        amce_strlcpy(dir, path, sizeof(dir));
        k = FilePart((STRPTR)dir) - (STRPTR)dir;
        dir[k] = 0;
        if (!song_folder_art(dir, art, artmax) && a.len)
            art_out(fh, path, si, &a, art, artmax, extract);
    }
    si->art = art && art[0];
    if (si->genre[0])
        genre_name(si->genre, sizeof(si->genre));     /* "(12)": Other */
    Close(fh);
    me->pr_WindowPtr = old;
    return TRUE;
}

/* Lines for the details pane (up to max of 96 bytes): the title, artist,
 * album and year, then the track, format, length and size. */
int song_lines(const char *path, const struct SongInfo *si, char lines[][96], int max)
{
    int n = 0;
    char t[96], *dot;
    if (max < 6)
        return 0;
    if (si->title[0]) {
        amce_strlcpy(lines[n++], si->title, 96);
    } else {
        amce_strlcpy(t, FilePart((STRPTR)path), sizeof(t));
        if ((dot = strrchr(t, '.')))
            *dot = 0;
        amce_strlcpy(lines[n++], t, 96);
    }
    if (si->artist[0] || si->band[0])
        amce_strlcpy(lines[n++], si->artist[0] ? si->artist : si->band, 96);
    if (si->album[0]) {
        if (si->year[0])
            sprintf(lines[n++], "%.80s (%.4s)", si->album, si->year);
        else
            amce_strlcpy(lines[n++], si->album, 96);
    } else if (si->year[0]) {
        amce_strlcpy(lines[n++], si->year, 96);
    }
    lines[n++][0] = 0;
    t[0] = 0;
    if (si->track[0])
        sprintf(t, _("Track %.10s   "), si->track);
    if (si->ms)
        sprintf(t + strlen(t), "%lu:%02lu", (unsigned long)(si->ms / 60000),
                (unsigned long)(si->ms / 1000 % 60));
    if (t[0])
        amce_strlcpy(lines[n++], t, 96);
    sprintf(t, "%s", si->fmt);
    if (si->kbps)
        sprintf(t + strlen(t), " %lukbps", (unsigned long)si->kbps);
    if (si->rate) {
        char k[16];
        sprintf(t + strlen(t), ", %s", fmt_khz(si->rate, k));
    }
    if (si->channels)
        sprintf(t + strlen(t), " %s", si->channels == 1 ? _("mono") : si->channels == 2 ? _("stereo")
                                                                                     : _("multi"));
    if (si->bits && strcmp(si->fmt, "MP3"))
        sprintf(t + strlen(t), ", %lu-bit", (unsigned long)si->bits);
    amce_strlcpy(lines[n++], t, 96);
    if (n < max) {
        if (si->size >= 1048576)
            sprintf(lines[n++], "%lu.%luMB", (unsigned long)(si->size >> 20),
                    (unsigned long)((si->size & 0xFFFFF) * 10 >> 20));
        else
            sprintf(lines[n++], "%luKB", (unsigned long)((si->size + 512) >> 10));
    }
    if (si->genre[0] && n < max)
        amce_strlcpy(lines[n++], si->genre, 96);
    return n;
}

/* A folder in a music list: how many music files and folders it holds,
 * and its picture (its cover file, else its first file's own). */
struct dir_ctx {
    LONG files, dirs;
    char first[108];
};

static void dir_cb(void *ctx, const char *name, BOOL isdir)
{
    struct dir_ctx *c = ctx;
    if (isdir) {
        c->dirs++;
    } else if (song_name_ok(name)) {
        if (!c->files || strcmp(name, c->first) < 0)
            if (strlen(name) < sizeof(c->first))
                strcpy(c->first, name);     /* the first by name: its track 1, mostly */
        c->files++;
    }
}

/* A music folder's lines; text (not NULL): its .nfo looked for too, the
 * review or biography put there (album.nfo: an album's or disc's folder;
 * artist.nfo: an artist's). */
static int dir_lines(const char *dir, char lines[][96], int max, char *art, LONG artmax,
                     BOOL extract, char **text)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    struct dir_ctx c;
    struct Nfo nfo;
    BOOL album = FALSE, artist = FALSE;
    int n = 0;
    if (art)
        art[0] = 0;
    if (text)
        *text = NULL;
    if (max < 3)
        return 0;
    memset(&c, 0, sizeof(c));
    me->pr_WindowPtr = (APTR)-1;
    dir_scan(dir, dir_cb, &c);
    me->pr_WindowPtr = old;
    amce_strlcpy(lines[n++], FilePart((STRPTR)dir), 96);
    lines[n][0] = 0;
    if (c.files == 1)
        strcpy(lines[n], _("1 music file"));
    else if (c.files)
        sprintf(lines[n], _("%ld music files"), (long)c.files);
    if (c.dirs)
        sprintf(lines[n] + strlen(lines[n]), c.dirs == 1 ? _("%s%ld folder") : _("%s%ld folders"),
                c.files ? ", " : "", (long)c.dirs);
    n++;
    if (text) {
        if (c.files && nfo_album_of(dir, &nfo))
            album = TRUE;
        else if (c.dirs && nfo_artist_of(dir, FALSE, &nfo))
            artist = TRUE;
    }
    if (album && n + 3 < max) {
        lines[n++][0] = 0;
        if (nfo.artist[0])
            amce_strlcpy(lines[n++], nfo.artist, 96);
        if (nfo.title[0]) {
            if (nfo.year[0])
                sprintf(lines[n++], "%.80s (%.4s)", nfo.title, nfo.year);
            else
                amce_strlcpy(lines[n++], nfo.title, 96);
        }
        n += nfo_album_lines(&nfo, lines + n, max - n);
    } else if (artist && n + 2 < max) {
        lines[n++][0] = 0;
        if (nfo.title[0] && amce_stricmp(nfo.title, lines[0]))
            amce_strlcpy(lines[n++], nfo.title, 96);
        n += nfo_artist_lines(&nfo, lines + n, max - n);
    }
    if (album || artist) {
        *text = nfo.text;                   /* the caller's now */
        nfo.text = NULL;
        nfo_free(&nfo);
    }
    if (art && !song_folder_art(dir, art, artmax) && c.files) {
        struct SongInfo si;
        char path[300];
        join(path, sizeof(path), dir, c.first);
        song_info(path, &si, art, artmax, extract);
        if (si.album[0] && !album && n < max) {
            lines[n++][0] = 0;
            if ((si.artist[0] || si.band[0]) && n < max)
                amce_strlcpy(lines[n++], si.band[0] ? si.band : si.artist, 96);
            if (n < max)
                amce_strlcpy(lines[n++], si.album, 96);
        }
    }
    return n;
}

int song_dir_lines(const char *dir, char lines[][96], int max, char *art, LONG artmax,
                   BOOL extract)
{
    return dir_lines(dir, lines, max, art, artmax, extract, NULL);
}

static BOOL is_dir(const char *path)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l;
    BOOL d = FALSE;
    if (!fib)
        return FALSE;
    if ((l = Lock((CONST_STRPTR)path, SHARED_LOCK))) {
        if (Examine(l, fib))
            d = fib->fib_DirEntryType > 0;
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    return d;
}

/* The details pane's lines for a music file or folder (the loader), and
 * its picture's file ("" none; not copied out yet). */
int song_details(const char *path, char lines[][96], int max, char *art, LONG artmax,
                 char **text)
{
    struct SongInfo si;
    struct Nfo nfo;
    char dir[300];
    int n;
    if (text)
        *text = NULL;
    if (is_dir(path))
        return dir_lines(path, lines, max, art, artmax, FALSE, text);
    if (!song_info(path, &si, art, artmax, FALSE)) {
        amce_strlcpy(lines[0], _("Could not be read"), 96);
        return 1;
    }
    /* its album's album.nfo: what the tags lack, then its label, genres and
     * styles, rating, and review */
    amce_strlcpy(dir, path, sizeof(dir));
    *PathPart((STRPTR)dir) = 0;
    if (text && nfo_album_of(dir, &nfo)) {
        if (!si.album[0])
            amce_strlcpy(si.album, nfo.title, sizeof(si.album));
        if (!si.year[0])
            amce_strlcpy(si.year, nfo.year, sizeof(si.year));
        if (!si.artist[0] && !si.band[0])
            amce_strlcpy(si.band, nfo.artist, sizeof(si.band));
        n = song_lines(path, &si, lines, max);
        if (n + 2 < max) {
            int k;
            lines[n][0] = 0;
            if ((k = nfo_album_lines(&nfo, lines + n + 1, max - n - 1)))
                n += 1 + k;
        }
        *text = nfo.text;
        nfo.text = NULL;
        nfo_free(&nfo);
        return n;
    }
    return song_lines(path, &si, lines, max);
}

/* A music file's or folder's picture, copied out to RAM: if it is inside
 * the file (extract). FALSE: none. */
BOOL song_art_of(const char *path, char *art, LONG artmax, BOOL extract)
{
    struct SongInfo si;
    char lines[6][96];
    art[0] = 0;
    if (is_dir(path))
        song_dir_lines(path, lines, 6, art, artmax, extract);
    else
        song_info(path, &si, art, artmax, extract);
    return art[0] != 0;
}
