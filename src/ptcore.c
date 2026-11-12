/*
 * AmigaMCE - a ProTracker replayer: the song, without the hardware
 * Copyright (c) 2026 Brian King
 *
 * This follows the ProTracker 2.3D play routine step by step, quirks and
 * all, so modules sound as they did in the tracker they were written in:
 * the same order of work in a tick, the same period table (with its 37th
 * entry of each finetune row, a 0), the same rounding in vibrato and
 * tremolo, sample offsets that stick, tremolo's square wave reading the
 * vibrato position, and so on. The period table is the one from the
 * ProTracker 2.1A play routine.
 *
 * The end: the song is over when a change of position (a jump, a break,
 * the end of a pattern or of the song) lands on a row it has played
 * before, or on F00. Rows a pattern loop (E6x) plays again do not count.
 * It plays on regardless; the player decides what to do next.
 */
#include "ptcore.h"

#define NOTES 37    /* periods a finetune row: 36 and a 0 */

static const pt_s16 periods[16 * NOTES + 16] = {
    856,808,762,720,678,640,604,570,538,508,480,453,428,404,381,360,339,320,
    302,285,269,254,240,226,214,202,190,180,170,160,151,143,135,127,120,113,0,
    850,802,757,715,674,637,601,567,535,505,477,450,425,401,379,357,337,318,
    300,284,268,253,239,225,213,201,189,179,169,159,150,142,134,126,119,113,0,
    844,796,752,709,670,632,597,563,532,502,474,447,422,398,376,355,335,316,
    298,282,266,251,237,224,211,199,188,177,167,158,149,141,133,125,118,112,0,
    838,791,746,704,665,628,592,559,528,498,470,444,419,395,373,352,332,314,
    296,280,264,249,235,222,209,198,187,176,166,157,148,140,132,125,118,111,0,
    832,785,741,699,660,623,588,555,524,495,467,441,416,392,370,350,330,312,
    294,278,262,247,233,220,208,196,185,175,165,156,147,139,131,124,117,110,0,
    826,779,736,694,655,619,584,551,520,491,463,437,413,390,368,347,328,309,
    292,276,260,245,232,219,206,195,184,174,164,155,146,138,130,123,116,109,0,
    820,774,730,689,651,614,580,547,516,487,460,434,410,387,365,345,325,307,
    290,274,258,244,230,217,205,193,183,172,163,154,145,137,129,122,115,109,0,
    814,768,725,684,646,610,575,543,513,484,457,431,407,384,363,342,323,305,
    288,272,256,242,228,216,204,192,181,171,161,152,144,136,128,121,114,108,0,
    907,856,808,762,720,678,640,604,570,538,508,480,453,428,404,381,360,339,
    320,302,285,269,254,240,226,214,202,190,180,170,160,151,143,135,127,120,0,
    900,850,802,757,715,675,636,601,567,535,505,477,450,425,401,379,357,337,
    318,300,284,268,253,238,225,212,200,189,179,169,159,150,142,134,126,119,0,
    894,844,796,752,709,670,632,597,563,532,502,474,447,422,398,376,355,335,
    316,298,282,266,251,237,223,211,199,188,177,167,158,149,141,133,125,118,0,
    887,838,791,746,704,665,628,592,559,528,498,470,444,419,395,373,352,332,
    314,296,280,264,249,235,222,209,198,187,176,166,157,148,140,132,125,118,0,
    881,832,785,741,699,660,623,588,555,524,494,467,441,416,392,370,350,330,
    312,294,278,262,247,233,220,208,196,185,175,165,156,147,139,131,123,117,0,
    875,826,779,736,694,655,619,584,551,520,491,463,437,413,390,368,347,328,
    309,292,276,260,245,232,219,206,195,184,174,164,155,146,138,130,123,116,0,
    868,820,774,730,689,651,614,580,547,516,487,460,434,410,387,365,345,325,
    307,290,274,258,244,230,217,205,193,183,172,163,154,145,137,129,122,115,0,
    862,814,768,725,684,646,610,575,543,513,484,457,431,407,384,363,342,323,
    305,288,272,256,242,228,216,203,192,181,171,161,152,144,136,128,121,114,0,
    /* past the last row: what an arpeggio reading off the end finds */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const pt_u8 sine[32] = {
    0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24
};

static const pt_u8 funk_table[16] = {
    0, 5, 6, 7, 8, 10, 11, 13, 16, 19, 22, 26, 32, 43, 64, 128
};

static pt_u16 be16(const pt_u8 *p) { return (pt_u16)((p[0] << 8) | p[1]); }

/* ------------------------------------------------------------ the file */
static int tag_channels(const pt_u8 *t)
{
    static const char *const four[] = { "M.K.", "M!K!", "M&K!", "FLT4", "4CHN", "N.T.", "LARD",
                                        "NSMS", 0 };
    int i;
    for (i = 0; four[i]; i++)
        if (t[0] == four[i][0] && t[1] == four[i][1] && t[2] == four[i][2] &&
            t[3] == four[i][3])
            return 4;
    /* other trackers' tags: "6CHN", "8CHN", "16CH", "FLT8", "TDZ6"... */
    if (t[1] == 'C' && t[2] == 'H' && t[3] == 'N' && t[0] >= '1' && t[0] <= '9')
        return t[0] - '0';
    if (t[2] == 'C' && t[3] == 'H' && t[0] >= '0' && t[0] <= '9' && t[1] >= '0' && t[1] <= '9')
        return (t[0] - '0') * 10 + t[1] - '0';
    if ((t[0] == 'F' && t[1] == 'L' && t[2] == 'T') || (t[0] == 'T' && t[1] == 'D' && t[2] == 'Z'))
        if (t[3] >= '1' && t[3] <= '9')
            return t[3] - '0';
    if (t[0] == 'O' && t[1] == 'C' && t[2] == 'T' && t[3] == 'A')
        return 8;
    return 0;
}

static int printable_name(const pt_u8 *p, int n)
{
    int i;
    for (i = 0; i < n && p[i]; i++)
        if (p[i] < 32 && p[i] != 0)
            return 0;
    return 1;
}

int pt_layout(const pt_u8 *hdr, pt_u32 hlen, pt_u32 filelen, struct PtLayout *L, int *channels)
{
    int i, nsmp, chans;
    pt_u32 sum = 0, pos;
    pt_u8 maxpat = 0, maxused = 0;
    *channels = 0;
    if (hlen < 600 || filelen < 600)
        return PT_ERR_SHORT;
    chans = hlen >= 1084 ? tag_channels(hdr + 1080) : 0;
    L->flt8 = 0;
    if (chans) {
        *channels = chans;
        if (chans < 4 || chans > PT_CHANS)
            return PT_ERR_CHANNELS;
        /* Startrekker's 8 channels: each pattern stored as two of 4 */
        L->flt8 = (pt_u8)(chans == 8 && hdr[1080] == 'F' && hdr[1081] == 'L' && hdr[1082] == 'T');
        nsmp = 31;
        L->tag[0] = hdr[1080];
        L->tag[1] = hdr[1081];
        L->tag[2] = hdr[1082];
        L->tag[3] = hdr[1083];
    } else {
        /* no tag: an Ultimate Soundtracker style module of 15 samples, if
         * everything in it makes sense */
        nsmp = 15;
        if (!printable_name(hdr, 20))
            return PT_ERR_NOTMOD;
        for (i = 0; i < 15; i++) {
            const pt_u8 *s = hdr + 20 + 30 * i;
            if (s[24] > 15 || s[25] > 64)
                return PT_ERR_NOTMOD;
            if (be16(s + 22) > 32768)
                return PT_ERR_NOTMOD;
        }
        L->tag[0] = 'S'; L->tag[1] = 'T'; L->tag[2] = '1'; L->tag[3] = '5';
        *channels = chans = 4;
    }
    L->chans = (pt_u8)chans;
    L->tag[4] = 0;
    L->nsmp = (pt_u8)nsmp;
    pos = 20 + 30 * nsmp;               /* song length, then the restart byte, then 128 orders */
    L->songlen = hdr[pos];
    if (L->songlen == 0 || L->songlen > 128)
        return PT_ERR_NOTMOD;
    for (i = 0; i < 128; i++) {
        pt_u8 o = hdr[pos + 2 + i];
        if (o > 127)
            return PT_ERR_NOTMOD;
        if (o > maxpat)
            maxpat = o;
        if (i < L->songlen && o > maxused)
            maxused = o;
    }
    if (nsmp == 15) {
        /* the start of the first pattern must look like notes: periods
         * ProTracker knows (or none), samples 1-15 */
        int bad = 0;
        const pt_u8 *c;
        for (c = hdr + 600; c + 4 <= hdr + hlen && c < hdr + 1084; c += 4) {
            pt_u16 per = (pt_u16)(((c[0] & 0x0F) << 8) | c[1]);
            if ((c[0] & 0xF0) || (per && (per < 100 || per > 1000)))
                bad++;
        }
        if (bad > 4)
            return PT_ERR_NOTMOD;
        maxpat = maxused;               /* the orders past the song are often junk */
    }
    L->hdr_len = pos + 2 + 128 + (nsmp == 31 ? 4 : 0);
    if (L->flt8) {
        /* an order names the first of its pattern's two halves (even) */
        maxpat |= 1;
        maxused |= 1;
    }
    /* every pattern the orders name; if the file has not got them all,
     * those the song uses */
    if (filelen < L->hdr_len + (pt_u32)(maxpat + 1) * (L->flt8 ? 1024 : 256UL * chans))
        maxpat = maxused;
    L->npat = (pt_u8)(maxpat + 1);
    L->pat_len = (pt_u32)L->npat * (L->flt8 ? 1024 : 256UL * chans);
    for (i = 0; i < nsmp; i++)
        sum += (pt_u32)be16(hdr + 20 + 30 * i + 22) * 2;
    L->smp_len = sum;
    if (filelen < L->hdr_len + L->pat_len)
        return nsmp == 15 ? PT_ERR_NOTMOD : PT_ERR_SHORT;
    if (nsmp == 15 && filelen < L->hdr_len + L->pat_len + sum / 2)
        return PT_ERR_NOTMOD;           /* far too short for its samples: not one */
    return PT_OK;
}

void pt_init(struct Pt *pt, const struct PtLayout *L, const pt_u8 *hdr, const pt_u8 *pat,
             pt_s8 *smp, pt_s8 *silence)
{
    int i, j;
    pt_u8 *z = (pt_u8 *)pt;
    for (i = 0; i < (int)sizeof(*pt); i++)
        z[i] = 0;
    for (i = 0; i < 20 && hdr[i]; i++)
        pt->title[i] = (char)(hdr[i] >= 32 ? hdr[i] : ' ');
    pt->title[i] = 0;
    pt->pat = pat;
    pt->npat = L->npat;
    pt->nsmp = L->nsmp;
    pt->nch = L->chans >= 4 && L->chans <= PT_CHANS ? L->chans : 4;
    pt->flt8 = L->flt8;
    pt->songlen = L->songlen;
    pt->silence = silence;
    for (i = 0; i < 128; i++)
        pt->orders[i] = hdr[20 + 30 * L->nsmp + 2 + i];
    for (i = 0; i < L->nsmp; i++) {
        const pt_u8 *s = hdr + 20 + 30 * i;
        struct PtSample *S = &pt->smp[i];
        for (j = 0; j < 22 && s[j]; j++)
            S->name[j] = (char)(s[j] >= 32 ? s[j] : ' ');
        S->name[j] = 0;
        S->len = be16(s + 22);
        S->fine = s[24] & 0x0F;
        S->vol = s[25] > 64 ? 64 : s[25];
        S->rep = be16(s + 26);
        S->replen = be16(s + 28);
        S->data = smp;
        smp += (pt_u32)S->len * 2;
        if (L->nsmp == 15) {
            /* Soundtracker's loop start was in bytes */
            S->rep /= 2;
            S->fine = 0;
        }
        if (S->replen == 0)
            S->replen = 1;
        /* a loop past the end: cut it to the sample, as players do */
        if ((pt_u32)S->rep + S->replen > S->len) {
            if (S->rep >= S->len) {
                S->rep = 0;
                S->replen = 1;
            } else {
                S->replen = (pt_u16)(S->len - S->rep);
            }
        }
        if (S->len && S->replen <= 1 && S->rep == 0) {
            S->data[0] = 0;         /* what plays after the sample: silence */
            if (S->len > 0)
                S->data[1] = 0;
        }
    }
    pt_restart(pt, 0, 6, 125);
}

void pt_restart(struct Pt *pt, int pos, pt_u8 speed, pt_u8 bpm)
{
    int i;
    for (i = 0; i < PT_CHANS; i++) {
        struct PtChan *c = &pt->ch[i];
        pt_u8 *z = (pt_u8 *)c;
        int j;
        for (j = 0; j < (int)sizeof(*c); j++)
            z[j] = 0;
        c->n_dmabit = (pt_u16)(1 << i);
        c->n_start = c->n_loopstart = c->n_wavestart = pt->silence;
        c->n_length = c->n_replen = 1;
        pt->hwvol[i] = 0;
        pt->hwper[i] = 0;
        pt->hit[i] = 0;
    }
    pt->speed = speed ? speed : 6;
    pt->bpm = bpm >= 32 ? bpm : 125;
    pt->tick_us = 2500000UL / pt->bpm;
    pt->counter = pt->speed;            /* the first tick reads a row */
    pt->pattdeltime = pt->pattdeltime2 = 0;
    pt->pbreakpos = pt->pbreakflag = pt->posjumpflag = 0;
    pt->lowmask = 0xFF;
    pt->order = (pt_s16)(pos >= 0 && pos < pt->songlen ? pos : 0);
    pt->row = 0;
    pt->dmacontemp = pt->loopmask = 0;
    pt->ended = 0;
    for (i = 0; i < (int)sizeof(pt->visited); i++)
        pt->visited[i] = 0;
}

/* ------------------------------------------------------------ hardware */
#define Q (pt->quiet)

static void hw_period(struct Pt *pt, int ch, pt_u16 p)
{
    pt->hwper[ch] = p;
    if (!Q)
        pt_hw_period(ch, p);
}

static void hw_volume(struct Pt *pt, int ch, int v)
{
    pt->hwvol[ch] = (pt_u8)v;
    if (!Q)
        pt_hw_volume(ch, (pt_u8)v);
}

/* ------------------------------------------------------------ effects */
#define CH(c) ((int)((c) - pt->ch))
#define PARAM(c) ((c)->n_cmd & 0xFF)

static void per_nop(struct Pt *pt, struct PtChan *c)
{
    hw_period(pt, CH(c), (pt_u16)c->n_period);
}

static void update_funk(struct Pt *pt, struct PtChan *c)
{
    pt_u8 speed = c->n_glissfunk >> 4;
    if (!speed)
        return;
    c->n_funkoffset = (pt_u8)(c->n_funkoffset + funk_table[speed]);
    if (c->n_funkoffset & 0x80) {
        c->n_funkoffset = 0;
        if (c->n_loopstart && c->n_wavestart && c->n_loopstart != pt->silence) {
            if (++c->n_wavestart >= c->n_loopstart + (pt_u32)c->n_replen * 2)
                c->n_wavestart = c->n_loopstart;
            if (!Q)
                *c->n_wavestart = (pt_s8)(-1 - *c->n_wavestart);
        }
    }
}

static void arpeggio(struct Pt *pt, struct PtChan *c)
{
    int i, arp, t = pt->counter % 3;
    const pt_s16 *row = &periods[c->n_finetune * NOTES];
    if (t == 0) {
        hw_period(pt, CH(c), (pt_u16)c->n_period);
        return;
    }
    arp = t == 1 ? (PARAM(c) >> 4) : (PARAM(c) & 0x0F);
    for (i = 0; i < NOTES; i++)
        if (c->n_period >= row[i]) {
            hw_period(pt, CH(c), (pt_u16)row[i + arp]);
            return;
        }
}

static void porta_up(struct Pt *pt, struct PtChan *c)
{
    c->n_period -= PARAM(c) & pt->lowmask;
    pt->lowmask = 0xFF;
    if ((c->n_period & 0x0FFF) < 113)
        c->n_period = (pt_s16)((c->n_period & 0xF000) | 113);
    hw_period(pt, CH(c), (pt_u16)(c->n_period & 0x0FFF));
}

static void porta_down(struct Pt *pt, struct PtChan *c)
{
    c->n_period += PARAM(c) & pt->lowmask;
    pt->lowmask = 0xFF;
    if ((c->n_period & 0x0FFF) > 856)
        c->n_period = (pt_s16)((c->n_period & 0xF000) | 856);
    hw_period(pt, CH(c), (pt_u16)(c->n_period & 0x0FFF));
}

static void set_tone_porta(struct Pt *pt, struct PtChan *c)
{
    pt_u16 note = c->n_note & 0x0FFF;
    const pt_s16 *row = &periods[c->n_finetune * NOTES];
    int i = 0;
    (void)pt;
    for (;;) {
        if ((pt_s16)note >= row[i])
            break;
        if (++i >= NOTES) {
            i = NOTES - 2;
            break;
        }
    }
    if ((c->n_finetune & 8) && i > 0)
        i--;
    c->n_wantedperiod = row[i];
    c->n_toneportdirec = 0;
    if (c->n_period == c->n_wantedperiod)
        c->n_wantedperiod = 0;
    else if (c->n_period > c->n_wantedperiod)
        c->n_toneportdirec = 1;
}

static void tone_no_change(struct Pt *pt, struct PtChan *c)
{
    if (c->n_wantedperiod <= 0)
        return;
    if (c->n_toneportdirec) {
        c->n_period -= c->n_toneportspeed;
        if (c->n_period <= c->n_wantedperiod) {
            c->n_period = c->n_wantedperiod;
            c->n_wantedperiod = 0;
        }
    } else {
        c->n_period += c->n_toneportspeed;
        if (c->n_period >= c->n_wantedperiod) {
            c->n_period = c->n_wantedperiod;
            c->n_wantedperiod = 0;
        }
    }
    if ((c->n_glissfunk & 0x0F) == 0) {
        hw_period(pt, CH(c), (pt_u16)c->n_period);
    } else {
        /* glissando: to the semitone */
        const pt_s16 *row = &periods[c->n_finetune * NOTES];
        int i = 0;
        for (;;) {
            if (c->n_period >= row[i])
                break;
            if (++i >= NOTES) {
                i = NOTES - 2;
                break;
            }
        }
        hw_period(pt, CH(c), (pt_u16)row[i]);
    }
}

static void tone_porta(struct Pt *pt, struct PtChan *c)
{
    if (PARAM(c)) {
        c->n_toneportspeed = (pt_u8)PARAM(c);
        c->n_cmd &= 0xFF00;
    }
    tone_no_change(pt, c);
}

static void vibrato2(struct Pt *pt, struct PtChan *c)
{
    pt_u8 pos = (c->n_vibratopos >> 2) & 0x1F;
    pt_u16 d;
    switch (c->n_wavecontrol & 3) {
    case 0: d = sine[pos]; break;
    case 1: d = c->n_vibratopos < 128 ? (pt_u16)(pos << 3) : (pt_u16)(255 - (pos << 3)); break;
    default: d = 255; break;
    }
    d = (pt_u16)((d * (c->n_vibratocmd & 0x0F)) >> 7);
    if (c->n_vibratopos < 128)
        hw_period(pt, CH(c), (pt_u16)(c->n_period + d));
    else
        hw_period(pt, CH(c), (pt_u16)(c->n_period - d));
    c->n_vibratopos = (pt_u8)(c->n_vibratopos + ((c->n_vibratocmd >> 2) & 0x3C));
}

static void vibrato(struct Pt *pt, struct PtChan *c)
{
    if (PARAM(c)) {
        if (PARAM(c) & 0x0F)
            c->n_vibratocmd = (pt_u8)((c->n_vibratocmd & 0xF0) | (PARAM(c) & 0x0F));
        if (PARAM(c) & 0xF0)
            c->n_vibratocmd = (pt_u8)((PARAM(c) & 0xF0) | (c->n_vibratocmd & 0x0F));
    }
    vibrato2(pt, c);
}

static void volume_slide(struct Pt *pt, struct PtChan *c)
{
    pt_u8 p = (pt_u8)PARAM(c);
    (void)pt;
    if ((p & 0xF0) == 0) {
        c->n_volume = (pt_s8)(c->n_volume - (p & 0x0F));
        if (c->n_volume < 0)
            c->n_volume = 0;
    } else {
        c->n_volume = (pt_s8)(c->n_volume + (p >> 4));
        if (c->n_volume > 64)
            c->n_volume = 64;
    }
}

static void tremolo(struct Pt *pt, struct PtChan *c)
{
    pt_u8 pos;
    pt_s16 d;
    if (PARAM(c)) {
        if (PARAM(c) & 0x0F)
            c->n_tremolocmd = (pt_u8)((c->n_tremolocmd & 0xF0) | (PARAM(c) & 0x0F));
        if (PARAM(c) & 0xF0)
            c->n_tremolocmd = (pt_u8)((PARAM(c) & 0xF0) | (c->n_tremolocmd & 0x0F));
    }
    pos = (c->n_tremolopos >> 2) & 0x1F;
    switch ((c->n_wavecontrol >> 4) & 3) {
    case 0: d = sine[pos]; break;
    /* ProTracker's square... ramp looks at the vibrato's position */
    case 1: d = c->n_vibratopos < 128 ? (pt_s16)(pos << 3) : (pt_s16)(255 - (pos << 3)); break;
    default: d = 255; break;
    }
    d = (pt_s16)(((pt_u16)d * (c->n_tremolocmd & 0x0F)) >> 6);
    if (c->n_tremolopos < 128) {
        d = (pt_s16)(c->n_volume + d);
        if (d > 64)
            d = 64;
    } else {
        d = (pt_s16)(c->n_volume - d);
        if (d < 0)
            d = 0;
    }
    hw_volume(pt, CH(c), d);
    c->n_tremolopos = (pt_u8)(c->n_tremolopos + ((c->n_tremolocmd >> 2) & 0x3C));
}

static void sample_offset(struct Pt *pt, struct PtChan *c)
{
    pt_u16 off;
    (void)pt;
    if (PARAM(c))
        c->n_sampleoffset = (pt_u8)PARAM(c);
    off = (pt_u16)(c->n_sampleoffset << 7);
    if ((pt_s16)off < (pt_s16)c->n_length) {
        c->n_length = (pt_u16)(c->n_length - off);
        c->n_start += (pt_u32)off * 2;
    } else {
        c->n_length = 1;
    }
}

/* Start the channel's sample again from its start, now (note delay,
 * retrigger): Paula is told after the tick, as for a new note. */
static void retrig(struct Pt *pt, struct PtChan *c)
{
    int ch = CH(c);
    if (!Q) {
        pt_hw_dma_off(c->n_dmabit);
        pt_hw_data(ch, c->n_start);
        pt_hw_length(ch, c->n_length);
    }
    hw_period(pt, ch, (pt_u16)c->n_period);
    pt->dmacontemp |= c->n_dmabit;
    pt->loopmask |= c->n_dmabit;
    pt->loop_ptr[ch] = c->n_loopstart;
    pt->loop_len[ch] = c->n_replen;
    pt->hit[ch] = (pt_u8)(c->n_volume > 0 ? c->n_volume : 1);
}

static void next_position(struct Pt *pt);

static void e_commands(struct Pt *pt, struct PtChan *c)
{
    pt_u8 p = (pt_u8)(PARAM(c) & 0x0F);
    switch (PARAM(c) >> 4) {
    case 0x0:                           /* the filter: left as it is */
        break;
    case 0x1:
        if (pt->counter == 0) {
            pt->lowmask = 0x0F;
            porta_up(pt, c);
        }
        break;
    case 0x2:
        if (pt->counter == 0) {
            pt->lowmask = 0x0F;
            porta_down(pt, c);
        }
        break;
    case 0x3:
        c->n_glissfunk = (pt_u8)((c->n_glissfunk & 0xF0) | p);
        break;
    case 0x4:
        c->n_wavecontrol = (pt_u8)((c->n_wavecontrol & 0xF0) | p);
        break;
    case 0x5:
        c->n_finetune = p;
        break;
    case 0x6:
        if (pt->counter != 0)
            break;
        if (p == 0) {
            c->n_pattpos = (pt_u8)pt->row;
        } else {
            if (c->n_loopcount == 0)
                c->n_loopcount = p;
            else if (--c->n_loopcount == 0)
                break;
            pt->pbreakpos = c->n_pattpos;
            pt->pbreakflag = 1;
        }
        break;
    case 0x7:
        c->n_wavecontrol = (pt_u8)((p << 4) | (c->n_wavecontrol & 0x0F));
        break;
    case 0x8:                           /* Karplus-Strong in 2.3D: rarely used, left out */
        break;
    case 0x9:
        if (p) {
            if (pt->counter == 0 && (c->n_note & 0x0FFF))
                break;
            if (pt->counter % p == 0)
                retrig(pt, c);
        }
        break;
    case 0xA:
        if (pt->counter == 0) {
            c->n_volume = (pt_s8)(c->n_volume + p);
            if (c->n_volume > 64)
                c->n_volume = 64;
        }
        break;
    case 0xB:
        if (pt->counter == 0) {
            c->n_volume = (pt_s8)(c->n_volume - p);
            if (c->n_volume < 0)
                c->n_volume = 0;
        }
        break;
    case 0xC:
        if (pt->counter == p)
            c->n_volume = 0;
        break;
    case 0xD:
        if (pt->counter == p && (c->n_note & 0x0FFF))
            retrig(pt, c);
        break;
    case 0xE:
        if (pt->counter == 0 && pt->pattdeltime2 == 0)
            pt->pattdeltime = (pt_u8)(p + 1);
        break;
    case 0xF:
        if (pt->counter == 0) {
            c->n_glissfunk = (pt_u8)((p << 4) | (c->n_glissfunk & 0x0F));
            if (c->n_glissfunk & 0xF0)
                update_funk(pt, c);
        }
        break;
    }
}

static void set_speed(struct Pt *pt, struct PtChan *c)
{
    pt_u8 p = (pt_u8)PARAM(c);
    if (p == 0) {
        pt->ended = 2;                  /* F00: the end, there and then */
        return;
    }
    if (p < 32) {
        pt->speed = p;
        pt->counter = 0;
    } else {
        pt->bpm = p;
        pt->tick_us = 2500000UL / p;
    }
}

static void check_more_effects(struct Pt *pt, struct PtChan *c)
{
    switch ((c->n_cmd >> 8) & 0x0F) {
    case 0x9:
        sample_offset(pt, c);
        return;
    case 0xB:
        pt->order = (pt_s16)(PARAM(c) - 1);
        pt->pbreakpos = 0;
        pt->posjumpflag = 1;
        break;
    case 0xD:
        pt->pbreakpos = (pt_u8)((PARAM(c) >> 4) * 10 + (PARAM(c) & 0x0F));
        if (pt->pbreakpos > 63)
            pt->pbreakpos = 0;
        pt->posjumpflag = 1;
        break;
    case 0xE:
        e_commands(pt, c);
        break;
    case 0xF:
        set_speed(pt, c);
        break;
    case 0xC:
        c->n_volume = (pt_s8)(PARAM(c) > 64 ? 64 : PARAM(c));
        hw_volume(pt, CH(c), c->n_volume);
        break;
    default:
        per_nop(pt, c);
        break;
    }
}

/* The effects between rows (and through a pattern delay). */
static void check_effects(struct Pt *pt, struct PtChan *c)
{
    pt_u8 fx = (pt_u8)((c->n_cmd >> 8) & 0x0F);
    update_funk(pt, c);
    if (c->n_cmd & 0x0FFF) {
        switch (fx) {
        case 0x0: arpeggio(pt, c); break;
        case 0x1: porta_up(pt, c); break;
        case 0x2: porta_down(pt, c); break;
        case 0x3: tone_porta(pt, c); break;
        case 0x4: vibrato(pt, c); break;
        case 0x5: tone_no_change(pt, c); volume_slide(pt, c); break;
        case 0x6: vibrato2(pt, c); volume_slide(pt, c); break;
        case 0xE: e_commands(pt, c); break;
        case 0x7: per_nop(pt, c); tremolo(pt, c); break;
        case 0xA: per_nop(pt, c); volume_slide(pt, c); break;
        default: per_nop(pt, c); break;
        }
    }
    if (fx != 0x7)
        hw_volume(pt, CH(c), c->n_volume);
}

static void set_period(struct Pt *pt, struct PtChan *c)
{
    pt_u16 note = c->n_note & 0x0FFF;
    int i, ch = CH(c);
    for (i = 0; i < NOTES; i++)
        if ((pt_s16)note >= periods[i])
            break;
    c->n_period = periods[c->n_finetune * NOTES + i];
    if ((c->n_cmd & 0x0FF0) != 0x0ED0) {   /* not a note delay */
        if (!(c->n_wavecontrol & 0x04))
            c->n_vibratopos = 0;
        if (!(c->n_wavecontrol & 0x40))
            c->n_tremolopos = 0;
        if (!Q) {
            pt_hw_dma_off(c->n_dmabit);
            pt_hw_length(ch, c->n_length);
            pt_hw_data(ch, c->n_start);
        }
        hw_period(pt, ch, (pt_u16)c->n_period);
        pt->dmacontemp |= c->n_dmabit;
        pt->hit[ch] = (pt_u8)(c->n_volume > 0 ? c->n_volume : 1);
    }
    check_more_effects(pt, c);
}

static void play_voice(struct Pt *pt, struct PtChan *c, const pt_u8 *d)
{
    int s;
    if (c->n_note == 0 && c->n_cmd == 0)
        per_nop(pt, c);
    c->n_note = (pt_u16)((d[0] << 8) | d[1]);
    c->n_cmd = (pt_u16)((d[2] << 8) | d[3]);
    s = (d[0] & 0xF0) | (d[2] >> 4);
    if (s >= 1 && s <= pt->nsmp) {
        struct PtSample *S = &pt->smp[s - 1];
        c->n_sample = (pt_u8)s;
        c->n_start = S->len ? S->data : pt->silence;
        c->n_finetune = S->fine;
        c->n_volume = (pt_s8)S->vol;
        c->n_length = S->len ? S->len : 1;
        c->n_replen = S->replen;
        if (S->rep) {
            c->n_loopstart = c->n_start + (pt_u32)S->rep * 2;
            c->n_wavestart = c->n_loopstart;
            c->n_length = (pt_u16)(S->rep + S->replen);
        } else {
            c->n_loopstart = c->n_start;
            c->n_wavestart = c->n_start;
        }
        if (!S->len) {
            c->n_loopstart = c->n_wavestart = pt->silence;
            c->n_replen = 1;
        }
        /* the sample's volume at once (ProTracker sets it on the row) */
        hw_volume(pt, CH(c), c->n_volume);
    }
    if (c->n_note & 0x0FFF) {
        if ((c->n_cmd & 0x0FF0) == 0x0E50) {
            c->n_finetune = (pt_u8)(c->n_cmd & 0x0F);
            set_period(pt, c);
        } else {
            pt_u8 fx = (pt_u8)((c->n_cmd >> 8) & 0x0F);
            if (fx == 3 || fx == 5) {
                set_tone_porta(pt, c);
                check_more_effects(pt, c);
            } else if (fx == 9) {
                check_more_effects(pt, c);
                set_period(pt, c);
            } else {
                set_period(pt, c);
            }
        }
    } else {
        check_more_effects(pt, c);
    }
}

/* A change of position landed on a row: played before, the song is over. */
static void mark_visit(struct Pt *pt)
{
    unsigned k = ((unsigned)(pt->order & 0x7F) << 6) | (unsigned)(pt->row & 63);
    if (pt->visited[k >> 3] & (1 << (k & 7)))
        pt->ended = 1;
}

static void next_position(struct Pt *pt)
{
    pt->row = pt->pbreakpos;
    pt->pbreakpos = 0;
    pt->posjumpflag = 0;
    pt->order = (pt_s16)((pt->order + 1) & 0x7F);
    if (pt->order >= pt->songlen) {
        pt->order = 0;
        pt->ended = 1;
    }
    mark_visit(pt);
}

void pt_tick(struct Pt *pt)
{
    int i;
    pt->dmacontemp = 0;
    pt->loopmask = 0;
    pt->elapsed_us += pt->tick_us;
    if (++pt->counter >= pt->speed) {
        pt->counter = 0;
        if (pt->pattdeltime2 == 0) {
            const pt_u8 *row;
            pt_u8 pat = pt->orders[pt->order & 0x7F];
            if (pat >= pt->npat)
                pat = 0;
            if (pt->flt8) {
                pat &= 0xFE;
                if (pat + 1 >= pt->npat)
                    pat = 0;
                row = pt->pat + (pt_u32)pat * 1024 + (pt_u32)pt->row * 16;
            } else {
                row = pt->pat + (pt_u32)pat * 256 * pt->nch + (pt_u32)pt->row * 4 * pt->nch;
            }
            {
                unsigned k = ((unsigned)(pt->order & 0x7F) << 6) | (unsigned)(pt->row & 63);
                pt->visited[k >> 3] |= (pt_u8)(1 << (k & 7));
            }
            for (i = 0; i < pt->nch; i++)
                play_voice(pt, &pt->ch[i],
                           pt->flt8 && i >= 4 ? row + 1024 + (i - 4) * 4 : row + i * 4);
            /* after the DMA starts: every channel's loop, as 2.3D does */
            pt->loopmask = (pt_u16)((1 << pt->nch) - 1);
            for (i = 0; i < pt->nch; i++) {
                pt->loop_ptr[i] = pt->ch[i].n_loopstart;
                pt->loop_len[i] = pt->ch[i].n_replen;
            }
        } else {
            for (i = 0; i < pt->nch; i++)
                check_effects(pt, &pt->ch[i]);
        }
        pt->row++;
        if (pt->pattdeltime) {
            pt->pattdeltime2 = pt->pattdeltime;
            pt->pattdeltime = 0;
        }
        if (pt->pattdeltime2) {
            if (--pt->pattdeltime2)
                pt->row--;
        }
        if (pt->pbreakflag) {
            pt->pbreakflag = 0;
            pt->row = pt->pbreakpos;
            pt->pbreakpos = 0;
        }
        if (pt->row >= 64)
            next_position(pt);
    } else {
        for (i = 0; i < pt->nch; i++)
            check_effects(pt, &pt->ch[i]);
    }
    if (pt->posjumpflag)
        next_position(pt);
}

/* ------------------------------------------------------------ length */
void pt_measure(struct Pt *pt, struct PtTimes *t)
{
    pt_u32 us = 0, ms = 0;
    int i;
    pt_s16 last = -1;
    for (i = 0; i < 128; i++) {
        t->at_ms[i] = 0xFFFFFFFFUL;
        t->speed[i] = 6;
        t->bpm[i] = 125;
    }
    pt->quiet = 1;
    pt_restart(pt, 0, 6, 125);
    /* at most an hour of song, then it is called over */
    while (!pt->ended && ms < 3600UL * 1000) {
        if (pt->order != last) {
            last = pt->order;
            if (t->at_ms[last] == 0xFFFFFFFFUL) {
                t->at_ms[last] = ms;
                t->speed[last] = pt->speed;
                t->bpm[last] = pt->bpm;
            }
        }
        pt_tick(pt);
        us += pt->tick_us;
        ms += us / 1000;
        us %= 1000;
    }
    /* the last row was read on the tick the song ended: its other ticks */
    if (pt->ended == 1)
        ms += (pt_u32)(pt->speed - 1) * pt->tick_us / 1000;
    t->total_ms = ms;
    pt->quiet = 0;
    pt_restart(pt, 0, 6, 125);
    pt->elapsed_us = 0;
}
