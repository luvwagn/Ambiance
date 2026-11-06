/*
 * AmigaMCE - Far East text: Japanese, Chinese and Korean names
 * Copyright (c) 2026 Brian King
 *
 * AmigaMCE's strings are Latin-1, as the Amiga's are. A Japanese, Chinese
 * or Korean character (the ranges in cjk_wanted, from UTF-8: radiocsv.c's
 * utf8_to_latin1) is kept in them as three bytes no Latin-1 text has:
 *
 *   0x80 | cp >> 11,  0xA0 | (cp >> 6 & 31),  0x80 | (cp & 63)
 *
 * The first is a C1 control (0x80-0x9F), the others 0x80-0xBF: none is
 * ASCII (a tab, a slash, a quote), none is NUL, and none is changed by
 * Latin-1 upper- or lower-casing, so the rest of AmigaMCE passes them
 * along untouched. Text is only read from its start, so the second and
 * third bytes need not differ from a first.
 *
 * Drawn from glyph packs (tools/mkcjk.py: Galmuri, Ark Pixel), in
 * PROGDIR:Fonts/amce-cjk: 12 (12 pixels) and 15 (Galmuri14's). A screen's
 * packs are a chain, the first that has the character drawing it: 15 then
 * 12 on screens whose lines are 20 pixels or more, otherwise 12 (with
 * Galmuri11 Bold too: one weight for all of a name's characters). Each glyph is
 * a one-pen mask stamped with BltTemplate, as the logo and icons are.
 * A pack's index (its code points and advances) is read when first
 * needed; its glyphs too if memory is plentiful, else on demand into a
 * small cache. On screens under 400 lines (12-pixel text on tall or few
 * pixels) a tag stands for each run instead: [JP], [KO] or [CJK].
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>

/* ---- the text */

int cjk_dec(const UBYTE *s, ULONG n, ULONG *cp)
{
    if (n < 3 || s[0] < 0x80 || s[0] > 0x9F || s[1] < 0xA0 || s[1] > 0xBF || s[2] < 0x80 ||
        s[2] > 0xBF)
        return 0;
    *cp = ((ULONG)(s[0] & 31) << 11) | ((ULONG)(s[1] & 31) << 6) | (s[2] & 63);
    return 3;
}

BOOL cjk_any(const char *s, ULONG n)
{
    const UBYTE *p = (const UBYTE *)s;
    while (n--) {
        if (*p >= 0x80 && *p <= 0x9F)
            return TRUE;
        p++;
    }
    return FALSE;
}

static BOOL is_kana(ULONG c)
{
    return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) ||
           (c >= 0xFF66 && c <= 0xFF9F);
}

static BOOL is_hangul(ULONG c)
{
    return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xFFA0 && c <= 0xFFDC);
}

/* The tag for s: Japanese if it has kana, Korean if Hangul, else (only
 * Chinese characters, which Japanese names use too) CJK. */
static const char *tag_of(const char *s, ULONG n)
{
    const UBYTE *p = (const UBYTE *)s;
    ULONG i = 0, cp;
    BOOL ko = FALSE;
    while (i < n) {
        int k = cjk_dec(p + i, n - i, &cp);
        if (!k) {
            i++;
            continue;
        }
        if (is_kana(cp))
            return "[JP]";
        if (is_hangul(cp))
            ko = TRUE;
        i += k;
    }
    return ko ? "[KO]" : "[CJK]";
}

/* s with each run of Far East characters (spaces between them included)
 * one tag: "宇多田 ヒカル - First Love" is "[JP] - First Love". */
ULONG cjk_tagged(const char *s, ULONG n, char *out, ULONG max)
{
    const UBYTE *p = (const UBYTE *)s;
    const char *tag = tag_of(s, n);
    ULONG i = 0, o = 0, cp, tl = strlen(tag);
    while (i < n && o + 1 < max) {
        int k = cjk_dec(p + i, n - i, &cp);
        if (!k) {
            if (p[i] >= 0x80 && p[i] <= 0x9F) {
                i++;                        /* a character cut short: nothing */
                continue;
            }
            out[o++] = (char)p[i++];
            continue;
        }
        if (o + tl + 1 > max)
            break;
        memcpy(out + o, tag, tl);
        o += tl;
        i += k;
        for (;;) {
            ULONG j = i;
            while (j < n && p[j] == ' ')
                j++;
            if ((k = cjk_dec(p + j, n - j, &cp)))
                i = j + k;
            else
                break;
        }
    }
    out[o] = 0;
    return o;
}

/* ---- the packs */

#define NPACKS 2
#define CACHE  256
static const char *pack_name[NPACKS] = { "12", "15" };

struct Pack {
    BOOL tried, ok;
    BPTR fh;                        /* open while its glyphs are read on demand */
    ULONG count, data_at;
    UBYTE cw, ch, asc;
    UWORD *codes;
    UBYTE *adv;
    UWORD *rows;                    /* all its glyphs, if read whole */
    ULONG alloc;                    /* the index's size (codes + adv) */
};

static struct Pack packs[NPACKS];
static int chain[2], nchain;
static BOOL tags;
static struct {
    UWORD pack, idx1;               /* idx1: glyph index + 1, 0 empty */
    UWORD rows[17];
} cache[CACHE];
static UWORD *chip;                 /* one glyph for BltTemplate (chip RAM) */
static WORD qmark_w;                /* "?" for a character no pack has */

static BOOL pack_load(int k)
{
    struct Pack *p = &packs[k];
    char path[64];
    UBYTE hdr[14];
    ULONG n, idx;
    if (p->tried)
        return p->ok;
    p->tried = TRUE;
    sprintf(path, "PROGDIR:Fonts/amce-cjk/%s", pack_name[k]);
    if (!(p->fh = Open((STRPTR)path, MODE_OLDFILE)))
        return FALSE;
    if (Read(p->fh, hdr, 14) != 14 || memcmp(hdr, "ACJK", 4) || hdr[5] != 1 || hdr[7] > 17 ||
        hdr[6] > 16)
        goto bad;
    p->cw = hdr[6];
    p->ch = hdr[7];
    p->asc = hdr[8];
    n = p->count = ((ULONG)hdr[10] << 24) | ((ULONG)hdr[11] << 16) | ((ULONG)hdr[12] << 8) | hdr[13];
    if (!n || n > 40000)
        goto bad;
    idx = n * 3 + (n & 1);
    p->alloc = idx;
    if (!(p->codes = AllocVec(idx, MEMF_ANY)))
        goto bad;
    if (Read(p->fh, p->codes, idx) != (LONG)idx)
        goto bad;
    p->adv = (UBYTE *)(p->codes + n);
    p->data_at = 14 + idx;
    {
        ULONG size = n * p->ch * 2;
        /* plenty of memory: all of it now, no reading as it is drawn */
        if (AvailMem(MEMF_ANY) > size + 4000000UL && (p->rows = AllocVec(size, MEMF_ANY))) {
            if (Read(p->fh, p->rows, size) != (LONG)size) {
                FreeVec(p->rows);
                p->rows = NULL;
            } else {
                Close(p->fh);
                p->fh = 0;
            }
        }
    }
    {
        char m[120];
        sprintf(m, "%s: %lu glyphs, %lu x %lu%s", pack_name[k], (unsigned long)n,
                (unsigned long)p->cw, (unsigned long)p->ch, p->rows ? ", all read" : ", read as drawn");
        amce_log("far east font", m);
    }
    p->ok = TRUE;
    return TRUE;
bad:
    amce_log("far east font: not usable", path);
    if (p->codes)
        FreeVec(p->codes);
    p->codes = NULL;
    if (p->fh)
        Close(p->fh);
    p->fh = 0;
    return FALSE;
}

static LONG pack_find(struct Pack *p, ULONG cp)
{
    LONG lo = 0, hi = (LONG)p->count - 1;
    while (lo <= hi) {
        LONG mid = (lo + hi) >> 1;
        if (p->codes[mid] == cp)
            return mid;
        if (p->codes[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* The chain's pack and glyph for cp: FALSE none. */
static BOOL find(ULONG cp, int *pk, LONG *gi)
{
    int i;
    for (i = 0; i < nchain; i++) {
        struct Pack *p = &packs[chain[i]];
        LONG k;
        if (!pack_load(chain[i]))
            continue;
        if ((k = pack_find(p, cp)) >= 0) {
            *pk = chain[i];
            *gi = k;
            return TRUE;
        }
    }
    return FALSE;
}

static const UWORD *glyph_rows(int pk, LONG gi)
{
    struct Pack *p = &packs[pk];
    UWORD slot;
    if (p->rows)
        return p->rows + (ULONG)gi * p->ch;
    slot = (UWORD)((gi * 7 + pk) & (CACHE - 1));
    if (cache[slot].idx1 != gi + 1 || cache[slot].pack != pk) {
        if (!p->fh || Seek(p->fh, p->data_at + (ULONG)gi * p->ch * 2, OFFSET_BEGINNING) < 0 ||
            Read(p->fh, cache[slot].rows, p->ch * 2) != p->ch * 2) {
            cache[slot].idx1 = 0;
            return NULL;
        }
        cache[slot].idx1 = (UWORD)(gi + 1);
        cache[slot].pack = (UWORD)pk;
    }
    return cache[slot].rows;
}

void cjk_open(struct Gfx *g)
{
    nchain = 0;
    tags = g->h < 400;
    if (tags)
        return;
    if (g->font_h >= 20)
        chain[nchain++] = 1;
    chain[nchain++] = 0;
    if (!chip)
        chip = AllocMem(17 * 2, MEMF_CHIP | MEMF_CLEAR);
    qmark_w = 0;
}

void cjk_close(void)
{
    int k;
    for (k = 0; k < NPACKS; k++) {
        struct Pack *p = &packs[k];
        if (p->fh)
            Close(p->fh);
        if (p->codes)
            FreeVec(p->codes);
        if (p->rows)
            FreeVec(p->rows);
        memset(p, 0, sizeof(*p));
    }
    memset(cache, 0, sizeof(cache));
    if (chip)
        FreeMem(chip, 17 * 2);
    chip = NULL;
    nchain = 0;
}

BOOL cjk_tags(void)
{
    return tags;
}

/* ---- measuring and drawing mixed text (gfx_text, gfx_textlen) */

static WORD latin_len(struct Gfx *g, struct RastPort *rp, const char *s, ULONG n, int face)
{
    if (!n)
        return 0;
    if (g->sm)
        return sm_textlen(g->sm, s, n, face);
    return TextLength(rp, (CONST_STRPTR)s, n);
}

static WORD char_w(struct Gfx *g, struct RastPort *rp, ULONG cp, int face)
{
    int pk;
    LONG gi;
    if (find(cp, &pk, &gi))
        return packs[pk].adv[gi];
    if (!qmark_w)
        qmark_w = latin_len(g, rp, "?", 1, face);
    return qmark_w;
}

/* The length of the Latin-1 run at s (up to a Far East character). */
static ULONG latin_run(const UBYTE *s, ULONG n)
{
    ULONG i = 0;
    while (i < n && !(s[i] >= 0x80 && s[i] <= 0x9F))
        i++;
    return i;
}

WORD cjk_textlen(struct Gfx *g, struct RastPort *rp, const char *s, ULONG n, int face)
{
    const UBYTE *p = (const UBYTE *)s;
    ULONG i = 0, cp;
    LONG w = 0;
    while (i < n) {
        ULONG r = latin_run(p + i, n - i);
        int k;
        w += latin_len(g, rp, s + i, r, face);
        i += r;
        if (i >= n)
            break;
        if ((k = cjk_dec(p + i, n - i, &cp))) {
            w += char_w(g, rp, cp, face);
            i += k;
        } else {
            i++;                            /* cut short: nothing */
        }
    }
    return (WORD)w;
}

/* How many bytes of s fit in maxw pixels (whole characters). */
ULONG cjk_fit(struct Gfx *g, struct RastPort *rp, const char *s, ULONG n, int face, WORD maxw)
{
    const UBYTE *p = (const UBYTE *)s;
    ULONG i = 0, cp;
    LONG w = 0;
    while (i < n) {
        ULONG r = latin_run(p + i, n - i);
        int k;
        if (r) {
            WORD rw = latin_len(g, rp, s + i, r, face);
            if (w + rw > maxw) {
                ULONG f;
                if (g->sm) {
                    f = sm_fit(g->sm, s + i, r, face, (WORD)(maxw - w));
                } else {
                    struct TextExtent te;
                    f = TextFit(rp, (CONST_STRPTR)(s + i), r, &te, NULL, 1, (WORD)(maxw - w),
                                g->font_h + 1);
                }
                return i + f;
            }
            w += rw;
            i += r;
        }
        if (i >= n)
            break;
        if ((k = cjk_dec(p + i, n - i, &cp))) {
            WORD cw = char_w(g, rp, cp, face);
            if (w + cw > maxw)
                return i;
            w += cw;
            i += k;
        } else {
            i++;
        }
    }
    return n;
}

static void draw_char(struct Gfx *g, struct RastPort *rp, WORD x, WORD y, ULONG cp, UBYTE fg,
                      UBYTE bg, BOOL opaque, int face, WORD *adv)
{
    int pk;
    LONG gi;
    const UWORD *rows;
    struct Pack *p;
    WORD top;
    if (!find(cp, &pk, &gi) || !chip || !(rows = glyph_rows(pk, gi))) {
        /* none: a "?" */
        *adv = char_w(g, rp, cp, face);
        if (g->sm) {
            sm_text(g->sm, g, x, y, "?", 1, face, fg, bg, opaque);
        } else {
            SetAPen(rp, fg);
            Move(rp, x, y + g->baseline);
            Text(rp, (CONST_STRPTR) "?", 1);
        }
        return;
    }
    p = &packs[pk];
    *adv = p->adv[gi];
    if (opaque) {
        SetAPen(rp, bg);
        RectFill(rp, x, y, x + *adv - 1, y + g->font_h - 1);
    }
    /* on the text's baseline, kept within a pixel of its line */
    top = y + (WORD)g->baseline - p->asc;
    if (top < y - 1)
        top = y - 1;
    CopyMem((APTR)rows, chip, p->ch * 2);
    SetAPen(rp, fg);
    SetDrMd(rp, JAM1);
    BltTemplate((PLANEPTR)chip, 0, 2, rp, x, top, p->cw, p->ch);
}

/* Draw n bytes of s at x, y (the line's top), fitting in maxw (0: any);
 * the width drawn. */
WORD cjk_text(struct Gfx *g, struct RastPort *rp, WORD x, WORD y, const char *s, ULONG n,
              UBYTE fg, UBYTE bg, BOOL opaque, int face, WORD maxw)
{
    const UBYTE *p = (const UBYTE *)s;
    ULONG i = 0, cp;
    WORD x0 = x;
    if (maxw > 0)
        n = cjk_fit(g, rp, s, n, face, maxw);
    while (i < n) {
        ULONG r = latin_run(p + i, n - i);
        int k;
        if (r) {
            if (g->sm) {
                sm_text(g->sm, g, x, y, s + i, r, face, fg, bg, opaque);
            } else {
                SetAPen(rp, fg);
                if (opaque) {
                    SetBPen(rp, bg);
                    SetDrMd(rp, JAM2);
                }
                Move(rp, x, y + g->baseline);
                Text(rp, (CONST_STRPTR)(s + i), r);
                if (opaque)
                    SetDrMd(rp, JAM1);
            }
            x += latin_len(g, rp, s + i, r, face);
            i += r;
        }
        if (i >= n)
            break;
        if ((k = cjk_dec(p + i, n - i, &cp))) {
            WORD adv;
            draw_char(g, rp, x, y, cp, fg, bg, opaque, face, &adv);
            x += adv;
            i += k;
        } else {
            i++;
        }
    }
    return x - x0;
}
