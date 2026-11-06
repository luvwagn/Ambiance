/*
 * AmigaMCE - EaglePlayer and DeliTracker players, hosted in AmigaMCE
 * Copyright (c) 2026 Brian King
 *
 * The hundreds of formats the Amiga's other music players know (OctaMED,
 * TFMX, Future Composer, Jochen Hippel, Rob Hubbard, AHX, SoundMon...) are
 * played by the player modules EaglePlayer and DeliTracker use: small
 * replayers, each in a file of its own, with one interface. AmigaMCE loads
 * the one that recognises a module and runs it itself, on its own CIA
 * timer and its own claim on the audio channels: no other program.
 *
 * Where the players are: EaglePlayer's and DeliTracker's drawers (AGS has
 * Work:Eagleplayer/Eagleplayers and Work:DeliTracker_II/DeliPlayers), or
 * PROGDIR:Eagleplayers.
 *
 * Finding the player for a module: each player's check is run on it until
 * one says yes; players named like the module's prefix or suffix ("MED.",
 * ".tfmx") first, and the one that played a module of that kind before
 * first of all (remembered in PROGDIR:Cache/EaglePlayers). Players that
 * need parts of EaglePlayer AmigaMCE does not have (amplifiers, format
 * loaders) and custom players (modules that are programs) are passed over.
 *
 * The calls made, their order and the callbacks provided follow
 * HippoPlayer's host for the same players (MIT licence, (c) 2017
 * Kari-Pekka Koljonen; see CREDITS.txt). eaglehost.s has the register
 * level.
 */
#include "amce.h"

#include <string.h>
#include <stdio.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;

/* ---- the interface: tags */
#define DTP(n) (0x80004454UL + (n))
#define EP(n)  (0x80004550UL + (n))
#define MI(n)  (0x80004D49UL + (n))
#define DTP_CustomPlayer    DTP(1)
#define DTP_PlayerVersion   DTP(4)
#define DTP_PlayerName      DTP(5)
#define DTP_Creator         DTP(6)
#define DTP_Check1          DTP(7)
#define DTP_Check2          DTP(8)
#define DTP_ExtLoad         DTP(9)
#define DTP_Interrupt       DTP(10)
#define DTP_Config          DTP(12)
#define DTP_SubSongRange    DTP(14)
#define DTP_InitPlayer      DTP(15)
#define DTP_EndPlayer       DTP(16)
#define DTP_InitSound       DTP(17)
#define DTP_EndSound        DTP(18)
#define DTP_StartInt        DTP(19)
#define DTP_StopInt         DTP(20)
#define DTP_Volume          DTP(21)
#define DTP_NextPatt        DTP(25)
#define DTP_PrevPatt        DTP(26)
#define DTP_NewSubSongRange DTP(30)
#define DTP_DeliBase        DTP(31)
#define DTP_Flags           DTP(32)
#define DTP_NoteStruct      DTP(38)
#define DTP_ModuleName      DTP(47)
#define DTP_AuthorName      DTP(49)
#define DTP_InitNote        DTP(50)
#define EP_Get_ModuleInfo   EP(0)
#define EP_Voices           EP(2)
#define EP_StructInit       EP(8)
#define EP_StructEnd        EP(9)
#define EP_GetPositionNr    EP(12)
#define EP_Flags            EP(14)
#define EP_EjectPlayer      EP(18)
#define EP_Check3           EP(20)
#define EP_NewModuleInfo    EP(22)
#define EP_PatternInit      EP(25)
#define EP_Check5           EP(28)
#define EP_InitAmplifier    EP(35)
#define EP_EagleBase        EP(42)
#define EP_Check7           EP(43)
#define EP_Check8           EP(44)
#define MI_SongName         MI(0)
#define MI_AuthorName       MI(1)
#define MI_Length           MI(5)
#define MI_Duration         MI(28)
#define MI_Soundsystem      MI(29)
#define PLYF_ANYMEM         4

/* ---- the globals (offsets: DeliPlayer.i 2.15, EaglePlayer.i 2.02) */
#define G_SIZE     500          /* EPG_SizeOf */
#define JT_SIZE    396          /* -ENPP_SizeOf: 66 entries */
#define JT_N       65
#define UPS_SIZE   80
#define SLOTS      64
#define o_DOSBase 4
#define o_IntuitionBase 8
#define o_GfxBase 12
#define o_DirArrayPtr 24
#define o_FileArrayPtr 28
#define o_PathArrayPtr 32
#define o_ChkData 36
#define o_ChkSize 40
#define o_SndNum 44
#define o_SndVol 46
#define o_SndLBal 48
#define o_SndRBal 50
#define o_LED 52
#define o_Timer 54
#define o_GetListData 56
#define o_LoadFile 60
#define o_CopyDir 64
#define o_CopyFile 68
#define o_CopyString 72
#define o_AudioAlloc 76
#define o_AudioFree 80
#define o_StartInt 84
#define o_StopInt 88
#define o_SongEnd 92
#define o_CutSuffix 96
#define o_SetTimer 100
#define o_WaitAudioDMA 104
#define o_LockScreen 108
#define o_UnlockScreen 112
#define o_NotePlayer 116
#define o_AllocListData 120
#define o_FreeListData 124
#define o_ModuleChange 176
#define o_UPS 268
#define o_ARG1 294
#define o_ARG2 298
#define o_ARG3 302
#define o_ARG4 306
#define o_ARG5 310
#define o_Voices 326
#define o_Voice1Vol 328
#define o_Volume 372
#define o_Balance 374
#define o_LeftBalance 376
#define o_RightBalance 378
/* UPS (the players' scope data) and pattern info */
#define UPS_PER(v) (6 + 18 * (v))
#define UPS_VOL(v) (8 + 18 * (v))
#define PI_Pattpos 4
#define PI_Songpos 6
#define PI_MaxSongPos 8
#define PI_BPM 10
#define PI_Speed 12

#define W(p, o) (*(volatile UWORD *)((UBYTE *)(p) + (o)))
#define L(p, o) (*(volatile ULONG *)((UBYTE *)(p) + (o)))

/* eaglehost.s */
LONG eh_call(APTR fn, APTR base, LONG d0, LONG d1);
LONG eh_icall(APTR fn, APTR base, LONG d0);
extern APTR eh_enpp[];
extern void ehs_getlistdata(void), ehs_loadfile(void), ehs_copydir(void), ehs_copyfile(void),
    ehs_copystring(void), ehs_ok(void), ehs_songend(void), ehs_cutsuffix(void),
    ehs_settimer(void), ehs_waitdma(void), ehs_alloclist(void), ehs_modchange(void);
UWORD eh_master = 64;           /* PokeVol's scale (0-64) */
ULONG eh_len;                   /* GetListData's second result */
ULONG eh_d1;                    /* eh_call: what D1 and A0 were left as */
APTR eh_a0;
volatile UBYTE eh_songend;      /* SongEnd called */
volatile UBYTE eh_hit;          /* channels started (DMAMask) since last looked */
volatile UBYTE eh_vol[4];       /* the volume each channel was last given (PokeVol) */
/* the spectrum's: what PokeAdr, PokeLen and PokePer last set, and where
 * each note began (DMAMask on: eh_trig) */
volatile APTR eh_adr[4], eh_sadr[4];
volatile UWORD eh_plen[4], eh_slen[4], eh_per[4];
volatile UBYTE eh_trig;
static BOOL eh_poked;           /* a PokeAdr seen: the player says where its samples are */

/* ---- one module played */
struct Host {
    UBYTE jt[JT_SIZE];          /* JMPs, below the globals */
    UBYTE g[G_SIZE];            /* the globals: A5 */
    char dir[256];              /* the module's folder (with its / or :) */
    char file[108];             /* its file name */
    char path[512];             /* the path array players build names in */
    UBYTE ups[UPS_SIZE];
    struct { APTR adr; ULONG len, asize; BOOL chip; } slot[SLOTS];  /* 0: the module */
};

static struct {
    struct Host *h;
    BPTR seg;                   /* the player */
    const ULONG *tags;
    APTR f_interrupt, f_volume, f_voices, f_getpos, f_startint, f_stopint, f_initsound,
         f_endsound, f_nextpatt, f_prevpatt;
    APTR notestruct;
    UBYTE *pinfo, *ups;         /* the player's pattern info, scope data */
    WORD sub_min, sub_max, sub;
    WORD poslen;
    volatile WORD pos;          /* EP_GetPositionNr, asked each tick */
    BOOL own_int;               /* the player times itself (DTP_StartInt) */
    BOOL running, paused;
    char name[48], song[64], author[48];
    UWORD timer;
    BOOL vol_changed;
} ep;

/* ---------------------------------------------------------- the players */
static const char *const dirs[] = {
    "PROGDIR:Eagleplayers", "EaglePlayer2:Eagleplayers", "EaglePlayer:Eagleplayers",
    "Work:Eagleplayer/Eagleplayers", "DeliTracker:DeliPlayers", "Work:DeliTracker_II/DeliPlayers",
    NULL
};

/* Players known to hang on a check or need what AmigaMCE has not. */
static const char *const skip[] = { "Core Design", NULL };

struct Cand {
    char name[40];
    UBYTE dir;
};
static struct Cand *cands;
static LONG ncands, capcands;
static BOOL scanned;

static void cand_cb(void *ctx, const char *name, BOOL isdir)
{
    UBYTE d = (UBYTE)(LONG)ctx;
    LONG i, n = strlen(name);
    if (isdir || (n > 5 && !amce_stricmp(name + n - 5, ".info")) || n >= 40)
        return;
    for (i = 0; skip[i]; i++)
        if (!amce_stricmp(name, skip[i]))
            return;
    for (i = 0; i < ncands; i++)
        if (!amce_stricmp(cands[i].name, name))
            return;                         /* EaglePlayer's copy first */
    if (ncands == capcands) {
        struct Cand *nc = AllocVec((capcands + 128) * sizeof(*nc), MEMF_ANY);
        if (!nc)
            return;
        if (cands) {
            CopyMem(cands, nc, ncands * sizeof(*nc));
            FreeVec(cands);
        }
        cands = nc;
        capcands += 128;
    }
    amce_strlcpy(cands[ncands].name, name, sizeof(cands[ncands].name));
    cands[ncands].dir = d;
    ncands++;
}

static void scan_players(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    int d;
    if (scanned)
        return;
    scanned = TRUE;
    me->pr_WindowPtr = (APTR)-1;
    for (d = 0; dirs[d]; d++)
        dir_scan(dirs[d], cand_cb, (void *)(LONG)d);
    me->pr_WindowPtr = old;
    {
        char m[64];
        sprintf(m, "%ld players found", (long)ncands);
        amce_log("eagleplayers", m);
    }
}

LONG ep_players(void)
{
    scan_players();
    return ncands;
}

/* ---- what played a kind of module before: "p:med" (prefix), "e:mod" */
#define MAP_FILE "PROGDIR:Cache/EaglePlayers"
#define MAP_MAX 64
static struct { char key[16]; char name[40]; } map[MAP_MAX];
static int nmap;
static BOOL map_read;

static void map_load(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR fh;
    char line[80];
    if (map_read)
        return;
    map_read = TRUE;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)MAP_FILE, MODE_OLDFILE);
    me->pr_WindowPtr = old;
    if (!fh)
        return;
    while (nmap < MAP_MAX && FGets(fh, (STRPTR)line, sizeof(line))) {
        char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (!tab)
            continue;
        *tab = 0;
        amce_strlcpy(map[nmap].key, line, sizeof(map[nmap].key));
        amce_strlcpy(map[nmap].name, tab + 1, sizeof(map[nmap].name));
        nmap++;
    }
    Close(fh);
}

static void map_save(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    BPTR fh, l;
    int i;
    me->pr_WindowPtr = (APTR)-1;
    if (!(l = Lock((CONST_STRPTR)"PROGDIR:Cache", SHARED_LOCK)))
        l = CreateDir((CONST_STRPTR)"PROGDIR:Cache");
    if (l)
        UnLock(l);
    fh = Open((CONST_STRPTR)MAP_FILE, MODE_NEWFILE);
    me->pr_WindowPtr = old;
    if (!fh)
        return;
    for (i = 0; i < nmap; i++) {
        FPuts(fh, (CONST_STRPTR)map[i].key);
        FPuts(fh, (CONST_STRPTR)"\t");
        FPuts(fh, (CONST_STRPTR)map[i].name);
        FPuts(fh, (CONST_STRPTR)"\n");
    }
    Close(fh);
}

static const char *map_find(const char *key)
{
    int i;
    for (i = 0; i < nmap; i++)
        if (!strcmp(map[i].key, key))
            return map[i].name;
    return NULL;
}

static void map_learn(const char *key, const char *name)
{
    int i;
    if (!key[2])
        return;
    for (i = 0; i < nmap; i++)
        if (!strcmp(map[i].key, key)) {
            if (strcmp(map[i].name, name)) {
                amce_strlcpy(map[i].name, name, sizeof(map[i].name));
                map_save();
            }
            return;
        }
    if (nmap == MAP_MAX) {
        memmove(&map[0], &map[1], sizeof(map[0]) * (MAP_MAX - 1));
        nmap--;
    }
    amce_strlcpy(map[nmap].key, key, sizeof(map[nmap].key));
    amce_strlcpy(map[nmap].name, name, sizeof(map[nmap].name));
    nmap++;
    map_save();
}

/* The module's kind, from its name: "p:" its prefix (up to the first dot),
 * "e:" its suffix (after the last), lower case. */
static void name_keys(const char *file, char *pre, char *ext)
{
    const char *dot = strchr(file, '.'), *last = strrchr(file, '.');
    LONG i, n;
    strcpy(pre, "p:");
    strcpy(ext, "e:");
    if (dot && dot > file && dot - file < 8) {
        for (i = 0, n = dot - file; i < n; i++)
            pre[2 + i] = (char)(file[i] | 0x20);
        pre[2 + n] = 0;
    }
    if (last && last != file && strlen(last + 1) < 8) {
        for (i = 0; last[1 + i]; i++)
            ext[2 + i] = (char)(last[1 + i] | 0x20);
        ext[2 + i] = 0;
    }
}

static BOOL contains_nocase(const char *hay, const char *needle)
{
    LONG n = strlen(needle), i, j;
    if (!n)
        return FALSE;
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < n && hay[i + j] && ((hay[i + j] | 0x20) == (needle[j] | 0x20)); j++)
            ;
        if (j == n)
            return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------- tags */
static const ULONG *seg_tags(BPTR seg)
{
    const UBYTE *code;
    if (!seg)
        return NULL;
    code = (const UBYTE *)BADDR(seg) + 4;
    if (memcmp(code + 4, "EPPLAYER", 8) && memcmp(code + 4, "DELIRIUM", 8))
        return NULL;
    return *(const ULONG *const *)(code + 12);
}

static ULONG tag(const ULONG *t, ULONG id)
{
    if (!t)
        return 0;
    for (; t[0]; t += 2)
        if (t[0] == id)
            return t[1];
    return 0;
}

static BOOL has_tag(const ULONG *t, ULONG id)
{
    if (!t)
        return FALSE;
    for (; t[0]; t += 2)
        if (t[0] == id)
            return TRUE;
    return FALSE;
}

static LONG call(ULONG id)
{
    ULONG fn = tag(ep.tags, id);
    return fn ? eh_call((APTR)fn, ep.h->g, 0, 0) : 0;
}

/* ---------------------------------------------------------- the host */
static void host_free(struct Host *h)
{
    int i;
    if (!h)
        return;
    for (i = 0; i < SLOTS; i++)
        if (h->slot[i].adr) {
            if (h->slot[i].chip)
                FreeMem(h->slot[i].adr, h->slot[i].asize);
            else
                FreeVec(h->slot[i].adr);
        }
    FreeVec(h);
}

static struct Host *host_new(const char *path, UBYTE *mod, ULONG len, ULONG asize, BOOL chip)
{
    struct Host *h = AllocVec(sizeof(*h), MEMF_ANY | MEMF_CLEAR);
    UBYTE *g;
    int i;
    if (!h)
        return NULL;
    g = h->g;
    /* the module's name, its folder, the path array */
    amce_strlcpy(h->file, (const char *)FilePart((STRPTR)path), sizeof(h->file));
    amce_strlcpy(h->dir, path, sizeof(h->dir));
    h->dir[(const char *)FilePart((STRPTR)path) - path] = 0;
    amce_strlcpy(h->path, path, sizeof(h->path));
    L(g, o_DirArrayPtr) = (ULONG)h->dir;
    L(g, o_FileArrayPtr) = (ULONG)h->file;
    L(g, o_PathArrayPtr) = (ULONG)h->path;
    L(g, o_DOSBase) = (ULONG)DOSBase;
    L(g, o_IntuitionBase) = (ULONG)IntuitionBase;
    L(g, o_GfxBase) = (ULONG)GfxBase;
    L(g, o_ChkData) = (ULONG)mod;
    L(g, o_ChkSize) = len;
    W(g, o_SndVol) = 64;
    W(g, o_SndLBal) = 64;
    W(g, o_SndRBal) = 64;
    W(g, o_Timer) = 28419 / 2;              /* the Sonix player needs a default */
    W(g, o_Voices) = 0x0F;
    for (i = 0; i < 4; i++)
        W(g, o_Voice1Vol + 2 * i) = 64;
    W(g, o_Volume) = 255;
    W(g, o_Balance) = 255;
    W(g, o_LeftBalance) = 255;
    W(g, o_RightBalance) = 255;
    L(g, o_UPS) = (ULONG)h->ups;
    L(g, o_GetListData) = (ULONG)ehs_getlistdata;
    L(g, o_LoadFile) = (ULONG)ehs_loadfile;
    L(g, o_CopyDir) = (ULONG)ehs_copydir;
    L(g, o_CopyFile) = (ULONG)ehs_copyfile;
    L(g, o_CopyString) = (ULONG)ehs_copystring;
    L(g, o_AudioAlloc) = (ULONG)ehs_ok;
    L(g, o_AudioFree) = (ULONG)ehs_ok;
    L(g, o_StartInt) = (ULONG)ehs_ok;
    L(g, o_StopInt) = (ULONG)ehs_ok;
    L(g, o_SongEnd) = (ULONG)ehs_songend;
    L(g, o_CutSuffix) = (ULONG)ehs_cutsuffix;
    L(g, o_SetTimer) = (ULONG)ehs_settimer;
    L(g, o_WaitAudioDMA) = (ULONG)ehs_waitdma;
    L(g, o_LockScreen) = (ULONG)ehs_ok;
    L(g, o_UnlockScreen) = (ULONG)ehs_ok;
    L(g, o_NotePlayer) = (ULONG)ehs_ok;
    L(g, o_AllocListData) = (ULONG)ehs_alloclist;
    L(g, o_FreeListData) = (ULONG)ehs_ok;
    L(g, o_ModuleChange) = (ULONG)ehs_modchange;
    /* the jump table under the globals: JMP to each, -6 first */
    for (i = 0; i < JT_N && eh_enpp[i]; i++) {
        UBYTE *e = g - 6 * (i + 1);
        *(UWORD *)e = 0x4EF9;
        *(ULONG *)(e + 2) = (ULONG)eh_enpp[i];
    }
    h->slot[0].adr = mod;
    h->slot[0].len = len;
    h->slot[0].asize = asize;
    h->slot[0].chip = chip;
    CacheClearU();
    return h;
}

/* ---- the callbacks eaglehost.s passes on */
ULONG ehc_getlistdata(ULONG n)
{
    struct Host *h = ep.h;
    if (!h || n >= SLOTS) {
        eh_len = 0;
        return 0;
    }
    eh_len = h->slot[n].len;
    return (ULONG)h->slot[n].adr;
}

static LONG free_slot(struct Host *h)
{
    LONG i;
    for (i = 1; i < SLOTS; i++)
        if (!h->slot[i].adr)
            return i;
    return -1;
}

/* A file a player asked for (instruments, samples): whole, unpacked, in
 * chip RAM. Tried as named, then with its file part in lower case (the
 * names players add are guesses). */
LONG ehc_loadfile(APTR base)
{
    struct Host *h = ep.h;
    char *path = (char *)L(base, o_PathArrayPtr), alt[512];
    UBYTE *buf, *chip;
    ULONG len = 0;
    LONG k;
    char packer[32];
    const char *why;
    if (!h || (k = free_slot(h)) < 0)
        return -1;
    buf = unpack_read_whole(path, &len);
    if (!buf) {
        char *f;
        amce_strlcpy(alt, path, sizeof(alt));
        for (f = (char *)FilePart((STRPTR)alt); *f; f++)
            if (*f >= 'A' && *f <= 'Z')
                *f |= 0x20;
        buf = unpack_read_whole(alt, &len);
    }
    if (!buf)
        return -1;
    buf = unpack_buffer(buf, &len, packer, sizeof(packer), &why);
    chip = AllocMem(len + 16, MEMF_CHIP | MEMF_CLEAR);
    if (chip) {
        CopyMem(buf, chip, len);
        FreeVec(buf);
        h->slot[k].adr = chip;
        h->slot[k].asize = len + 16;
        h->slot[k].chip = TRUE;
    } else {
        h->slot[k].adr = buf;
        h->slot[k].chip = FALSE;
    }
    h->slot[k].len = len;
    return 0;
}

void ehc_cutsuffix(APTR base)
{
    char *p = (char *)L(base, o_PathArrayPtr);
    LONG n = strlen(p);
    if (n > 3 && !amce_stricmp(p + n - 3, ".pp"))
        p[n - 3] = 0;
    else if (n > 4 && !amce_stricmp(p + n - 4, ".xpk"))
        p[n - 4] = 0;
}

void ehc_settimer(ULONG t)
{
    ep.timer = (UWORD)t;
    if (!ep.own_int)
        mp_tick_count((UWORD)t);
}

ULONG ehc_alloclist(ULONG size, ULONG flags)
{
    struct Host *h = ep.h;
    LONG k;
    APTR m;
    if (!h || (k = free_slot(h)) < 0)
        return 0;
    if (!(m = AllocMem(size + 16, flags | MEMF_CLEAR)))
        return 0;
    h->slot[k].adr = m;
    h->slot[k].len = size;
    h->slot[k].asize = size + 16;
    h->slot[k].chip = TRUE;                 /* AllocMem'd: freed with FreeMem */
    return (ULONG)m;
}

/* ModuleChange: a player patching the replay code inside its module (a
 * table of code to find, its length in words, and the patch's offset), as
 * HippoPlayer does it: the one kind of call players make. */
void ehc_modchange(APTR base)
{
    UBYTE *data = (UBYTE *)L(base, o_ARG1);
    ULONG len = L(base, o_ARG2);
    const UWORD *tab = (const UWORD *)L(base, o_ARG3);
    UBYTE *end;
    if (L(base, o_ARG4) != 1 || (LONG)L(base, o_ARG5) != -2 || !data || !len || !tab)
        return;
    if (ep.h && len > ep.h->slot[0].len)
        len = ep.h->slot[0].len;
    end = data + (len & ~1UL);
    for (; tab[0]; tab += 3) {
        const UWORD *find = (const UWORD *)((const UBYTE *)L(base, o_ARG3) + tab[0]);
        UWORD words = (UWORD)(tab[1] + 1);
        UBYTE *patch = (UBYTE *)L(base, o_ARG3) + tab[2];
        UWORD *p;
        for (p = (UWORD *)data; (UBYTE *)(p + words) <= end; p++) {
            UWORD k;
            for (k = 0; k < words && p[k] == find[k]; k++)
                ;
            if (k == words) {
                /* JSR patch, then NOPs over the rest */
                p[0] = 0x4EB9;
                *(ULONG *)(p + 1) = (ULONG)patch;
                for (k = 3; k < words; k++)
                    p[k] = 0x4E71;
                p += words - 1;
            }
        }
    }
    CacheClearU();
}

void ehc_clearcache(void)
{
    CacheClearU();
}

/* ---------------------------------------------------------- choosing */
static BOOL try_player(struct Host *h, const char *dir, const char *name, BPTR *seg_out,
                       const ULONG **tags_out)
{
    char path[300];
    BPTR seg;
    const ULONG *t;
    ULONG fn, *var;
    LONG rc = -1;
    amce_strlcpy(path, dir, sizeof(path));
    AddPart((STRPTR)path, (CONST_STRPTR)name, sizeof(path));
    if (!(seg = LoadSeg((CONST_STRPTR)path)))
        return FALSE;
    t = seg_tags(seg);
    if (!t || has_tag(t, DTP_CustomPlayer) || has_tag(t, EP_InitAmplifier) ||
        has_tag(t, EP_Check7) || has_tag(t, EP_Check8)) {
        UnLoadSeg(seg);
        return FALSE;
    }
    CacheClearU();
    if ((var = (ULONG *)tag(t, DTP_DeliBase)))
        *var = (ULONG)h->g;
    if ((var = (ULONG *)tag(t, EP_EagleBase)))
        *var = (ULONG)h->g;
    if ((fn = tag(t, DTP_Check2)))
        rc = eh_call((APTR)fn, h->g, 0, 0);
    else if ((fn = tag(t, DTP_Check1)))
        rc = eh_call((APTR)fn, h->g, 0, 0);
    else if ((fn = tag(t, EP_Check3)))
        rc = eh_call((APTR)fn, h->g, 0, 0);
    else if ((fn = tag(t, EP_Check5)))
        rc = eh_call((APTR)fn, h->g, 0, 0);
    if (rc != 0) {
        UnLoadSeg(seg);
        return FALSE;
    }
    *seg_out = seg;
    *tags_out = t;
    return TRUE;
}

/* The player for the module in h: tried in order of likelihood. */
static BOOL find_player(struct Host *h, char *found, LONG fmax)
{
    char pre[16], ext[16];
    const char *want[2];
    UBYTE *tried;
    LONG i, k, pass;
    BOOL ok = FALSE;
    scan_players();
    map_load();
    if (!ncands)
        return FALSE;
    if (!(tried = AllocVec(ncands, MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    name_keys(h->file, pre, ext);
    want[0] = map_find(pre);
    want[1] = map_find(ext);
    /* 0: what played this kind before; 1: named like its prefix or suffix;
     * 2: the rest */
    for (pass = 0; pass < 3 && !ok; pass++) {
        for (i = 0; i < ncands && !ok; i++) {
            struct Cand *c = &cands[i];
            if (tried[i])
                continue;
            if (pass == 0) {
                for (k = 0; k < 2; k++)
                    if (want[k] && !amce_stricmp(want[k], c->name))
                        break;
                if (k == 2)
                    continue;
            } else if (pass == 1) {
                if (!(pre[2] && contains_nocase(c->name, pre + 2)) &&
                    !(ext[2] && contains_nocase(c->name, ext + 2)))
                    continue;
            }
            tried[i] = 1;
            if (try_player(h, dirs[c->dir], c->name, &ep.seg, &ep.tags)) {
                amce_strlcpy(found, c->name, fmax);
                map_learn(pre, c->name);
                map_learn(ext, c->name);
                ok = TRUE;
            }
        }
    }
    FreeVec(tried);
    return ok;
}

/* ---------------------------------------------------------- module info */
static const ULONG *module_info(void)
{
    ULONG fn = tag(ep.tags, EP_Get_ModuleInfo);
    if (fn) {
        eh_call((APTR)fn, ep.h->g, 0, 0);
        return (const ULONG *)eh_a0;
    }
    return (const ULONG *)tag(ep.tags, EP_NewModuleInfo);
}

static LONG info_value(const ULONG *mi, ULONG id, BOOL *found)
{
    *found = FALSE;
    if (!mi)
        return 0;
    for (; mi[0]; mi += 2)
        if (mi[0] == id) {
            *found = TRUE;
            return (LONG)mi[1];
        }
    return 0;
}

static void copy_text(char *out, LONG max, const char *s)
{
    LONG i;
    out[0] = 0;
    if (!s || (ULONG)s < 0x400)
        return;
    for (i = 0; i < max - 1 && s[i]; i++)
        out[i] = (s[i] >= 32 && (UBYTE)s[i] != 0x7F) ? s[i] : ' ';
    out[i] = 0;
    while (i > 0 && out[i - 1] == ' ')
        out[--i] = 0;
}

/* ---------------------------------------------------------- playing */
/* The module (len bytes; AllocVec, given to this) at path: its player
 * found and started up, ready to play. FALSE and why if not. */
BOOL ep_load(const char *path, UBYTE *mod, ULONG len, char *err, LONG errmax)
{
    UBYTE *chip;
    BOOL ischip = FALSE;
    ULONG asize = (len < 1024 ? 1024 : len) + 16;   /* checks may look at 1 KB */
    char found[40];
    ULONG fn;
    const ULONG *mi;
    BOOL got;
    LONG v;
    char m[160];

    ep_unload();
    memset(&ep, 0, sizeof(ep));
    /* most players need their module in chip RAM */
    if ((chip = AllocMem(asize, MEMF_CHIP | MEMF_CLEAR))) {
        CopyMem(mod, chip, len);
        FreeVec(mod);
        mod = chip;
        ischip = TRUE;
    }
    if (!(ep.h = host_new(path, mod, len, asize, ischip))) {
        if (ischip)
            FreeMem(mod, asize);
        else
            FreeVec(mod);
        amce_strlcpy(err, _("Not enough memory"), errmax);
        return FALSE;
    }
    eh_songend = 0;
    eh_trig = 0;
    eh_poked = FALSE;
    {
        int i;
        for (i = 0; i < 4; i++) {
            eh_adr[i] = eh_sadr[i] = NULL;
            eh_plen[i] = eh_slen[i] = eh_per[i] = 0;
        }
    }
    if (!find_player(ep.h, found, sizeof(found))) {
        host_free(ep.h);
        ep.h = NULL;
        amce_strlcpy(err, ncands ? _("No player AmigaMCE has knows this module")
                                 : _("Not a module AmigaMCE can play, and no EaglePlayer "
                                     "players were found"),
                     errmax);
        return FALSE;
    }
    copy_text(ep.name, sizeof(ep.name), (const char *)tag(ep.tags, DTP_PlayerName));
    if (!ep.name[0])
        amce_strlcpy(ep.name, found, sizeof(ep.name));

    /* as DeliTracker and HippoPlayer: config, the checks (done), ... */
    call(DTP_Config);
    call(DTP_InitNote);
    CacheClearU();
    if ((fn = tag(ep.tags, EP_PatternInit))) {
        eh_call((APTR)fn, ep.h->g, 0, 0);
        ep.pinfo = (UBYTE *)eh_a0;
    }
    if (has_tag(ep.tags, DTP_ExtLoad) && call(DTP_ExtLoad) != 0) {
        sprintf(m, _("%s could not load the module's other files"), ep.name);
        amce_strlcpy(err, m, errmax);
        ep_unload();
        return FALSE;
    }
    if ((fn = tag(ep.tags, EP_StructInit))) {
        eh_call((APTR)fn, ep.h->g, 0, 0);
        ep.ups = (UBYTE *)eh_a0;
    }
    if (call(DTP_InitPlayer) != 0) {
        sprintf(m, _("%s could not start this module"), ep.name);
        amce_strlcpy(err, m, errmax);
        /* nothing to end: the player did not start */
        UnLoadSeg(ep.seg);
        ep.seg = 0;
        host_free(ep.h);
        ep.h = NULL;
        return FALSE;
    }
    ep.running = TRUE;                      /* initialised: ep_unload ends it */
    CacheClearU();
    /* subsongs */
    if ((fn = tag(ep.tags, DTP_SubSongRange))) {
        ep.sub_min = (WORD)eh_call((APTR)fn, ep.h->g, 0, 0);
        ep.sub_max = (WORD)eh_d1;
    } else if ((fn = tag(ep.tags, DTP_NewSubSongRange))) {
        const UWORD *r = (const UWORD *)fn;
        ep.sub = (WORD)r[0];
        ep.sub_min = (WORD)r[1];
        ep.sub_max = (WORD)r[2];
    }
    if (ep.sub_max < ep.sub_min)
        ep.sub_max = ep.sub_min;
    if (ep.sub < ep.sub_min || ep.sub > ep.sub_max)
        ep.sub = ep.sub_min;
    W(ep.h->g, o_SndNum) = (UWORD)ep.sub;
    ep.f_volume = (APTR)tag(ep.tags, DTP_Volume);
    ep.f_voices = (APTR)tag(ep.tags, EP_Voices);
    ep.f_interrupt = (APTR)tag(ep.tags, DTP_Interrupt);
    ep.f_startint = (APTR)tag(ep.tags, DTP_StartInt);
    ep.f_stopint = (APTR)tag(ep.tags, DTP_StopInt);
    ep.f_initsound = (APTR)tag(ep.tags, DTP_InitSound);
    ep.f_endsound = (APTR)tag(ep.tags, DTP_EndSound);
    ep.f_nextpatt = (APTR)tag(ep.tags, DTP_NextPatt);
    ep.f_prevpatt = (APTR)tag(ep.tags, DTP_PrevPatt);
    ep.f_getpos = (APTR)tag(ep.tags, EP_GetPositionNr);
    ep.pos = -1;
    if ((fn = tag(ep.tags, DTP_NoteStruct)))
        ep.notestruct = *(APTR *)fn;
    ep.own_int = ep.f_startint != NULL;
    /* the names it knows */
    mi = module_info();
    copy_text(ep.song, sizeof(ep.song), (const char *)info_value(mi, MI_SongName, &got));
    copy_text(ep.author, sizeof(ep.author), (const char *)info_value(mi, MI_AuthorName, &got));
    if (!ep.song[0] && (fn = tag(ep.tags, DTP_ModuleName)))
        copy_text(ep.song, sizeof(ep.song), *(const char **)fn);
    if (!ep.author[0] && (fn = tag(ep.tags, DTP_AuthorName)))
        copy_text(ep.author, sizeof(ep.author), *(const char **)fn);
    v = info_value(mi, MI_Length, &got);
    ep.poslen = got && v > 0 && v < 10000 ? (WORD)v : 0;
    ep.timer = W(ep.h->g, o_Timer);
    sprintf(m, "%s: %s%s%s, %ld subsongs, %s interrupt", FilePart((STRPTR)path), ep.name,
            ep.song[0] ? ", " : "", ep.song, (long)(ep.sub_max - ep.sub_min + 1),
            ep.own_int ? "its own" : ep.f_interrupt ? "AmigaMCE's" : "no");
    amce_log("eagleplayer", m);
    return TRUE;
}

/* Sound started (the channels are AmigaMCE's by now): TRUE if AmigaMCE's
 * timer is to call ep_tick, *count its CIA count. */
BOOL ep_start(UWORD *count)
{
    if (!ep.h)
        return FALSE;
    eh_master = (UWORD)mp_volume();
    W(ep.h->g, o_SndVol) = eh_master;
    if (ep.f_initsound)
        eh_call(ep.f_initsound, ep.h->g, 0, 0);
    CacheClearU();
    ep.timer = W(ep.h->g, o_Timer);
    ep.paused = FALSE;
    if (ep.own_int) {
        eh_call(ep.f_startint, ep.h->g, 0, 0);
        return FALSE;
    }
    *count = ep.timer ? ep.timer : 14209;
    return ep.f_interrupt != NULL;
}

/* AmigaMCE's timer tick (an interrupt): the player's routine. */
void ep_tick(void)
{
    struct Host *h = ep.h;
    if (!h || ep.paused || ep.own_int)
        return;
    W(h->g, o_SndVol) = eh_master;
    if (ep.f_voices)
        eh_icall(ep.f_voices, h->g, 0x0F);
    if (ep.f_interrupt)
        eh_icall(ep.f_interrupt, h->g, 0);
    if (ep.vol_changed && ep.f_volume) {
        ep.vol_changed = FALSE;
        eh_icall(ep.f_volume, h->g, 0);
    }
    if (ep.f_getpos) {
        LONG p = eh_icall(ep.f_getpos, h->g, 0);
        if (p >= 0 && p < 10000)
            ep.pos = (WORD)p;
    }
}

static void paula_quiet(void)
{
    volatile UWORD *c = (volatile UWORD *)0xDFF000;
    c[0x96 / 2] = 0x000F;
    c[0xA8 / 2] = 0;
    c[0xB8 / 2] = 0;
    c[0xC8 / 2] = 0;
    c[0xD8 / 2] = 0;
}

/* Sound off (stop; before a different subsong or the end). */
void ep_stop(void)
{
    if (!ep.h)
        return;
    if (ep.own_int && ep.f_stopint)
        eh_call(ep.f_stopint, ep.h->g, 0, 0);
    if (ep.f_endsound)
        eh_call(ep.f_endsound, ep.h->g, 0, 0);
    paula_quiet();
}

void ep_pause(BOOL on)
{
    if (!ep.h || on == ep.paused)
        return;
    ep.paused = on;
    if (ep.own_int) {
        if (on && ep.f_stopint)
            eh_call(ep.f_stopint, ep.h->g, 0, 0);
        else if (!on)
            eh_call(ep.f_startint, ep.h->g, 0, 0);
    }
    if (on)
        paula_quiet();
    else
        *(volatile UWORD *)0xDFF096 = 0x800F;
}

/* The player ended and let go; the module freed. */
void ep_unload(void)
{
    if (ep.h && ep.running) {
        ep_stop();
        call(DTP_EndPlayer);
        call(EP_StructEnd);
        paula_quiet();
        call(EP_EjectPlayer);
    }
    ep.running = FALSE;
    if (ep.seg)
        UnLoadSeg(ep.seg);
    ep.seg = 0;
    ep.tags = NULL;
    host_free(ep.h);
    ep.h = NULL;
}

BOOL ep_loaded(void) { return ep.h != NULL; }
BOOL ep_own_timer(void) { return ep.own_int; }

BOOL ep_song_ended(void)
{
    if (!eh_songend)
        return FALSE;
    eh_songend = 0;
    return TRUE;
}

void ep_set_volume(int v)
{
    eh_master = (UWORD)v;
    ep.vol_changed = TRUE;
    if (ep.own_int && ep.h && ep.f_volume) {
        W(ep.h->g, o_SndVol) = eh_master;
        eh_call(ep.f_volume, ep.h->g, 0, 0);
    }
}

BOOL ep_step_position(int d)
{
    APTR f = d > 0 ? ep.f_nextpatt : ep.f_prevpatt;
    if (!ep.h || !f)
        return FALSE;
    eh_call(f, ep.h->g, 0, 0);
    return TRUE;
}

/* Whether the player can move through its song (d +1, -1). */
BOOL ep_can_step(int d)
{
    return ep.h && (d > 0 ? ep.f_nextpatt : ep.f_prevpatt) != NULL;
}

/* Another subsong (from its first: min). FALSE if out of range. */
BOOL ep_subsong(int n)
{
    UWORD count;
    if (!ep.h || n < ep.sub_min || n > ep.sub_max)
        return FALSE;
    mp_tick_run(FALSE);
    ep_stop();
    ep.sub = (WORD)n;
    W(ep.h->g, o_SndNum) = (UWORD)n;
    eh_songend = 0;
    if (ep_start(&count))
        mp_tick_count(count), mp_tick_run(TRUE);
    return TRUE;
}

void ep_info(struct EpInfo *in)
{
    memset(in, 0, sizeof(*in));
    if (!ep.h)
        return;
    in->player = ep.name;
    in->song = ep.song;
    in->author = ep.author;
    in->sub = ep.sub - ep.sub_min + 1;
    in->subs = ep.sub_max - ep.sub_min + 1;
    in->sub_raw = ep.sub;
    in->sub_min = ep.sub_min;
    in->sub_max = ep.sub_max;
    in->poslen = ep.poslen;
    in->pos = -1;
    in->row = -1;
    if (ep.pinfo) {
        in->pos = (WORD)W(ep.pinfo, PI_Songpos);
        in->row = (WORD)W(ep.pinfo, PI_Pattpos);
        if (W(ep.pinfo, PI_MaxSongPos))
            in->poslen = (WORD)W(ep.pinfo, PI_MaxSongPos);
    } else if (ep.f_getpos) {
        in->pos = ep.pos;
    }
}

/* The meters: each channel's volume when it last started a note. And
 * for the spectrum (v): where each voice's sample is, as HippoPlayer
 * follows them (puu016.s, scopeinterrupt): from the player's UPS where it
 * fills one, else from what it pokes into Paula through AmigaMCE. */
BOOL ep_levels(UBYTE *hit, UBYTE *vol, struct EpVoice *v, UBYTE *trig)
{
    int i;
    UBYTE t;
    *hit = eh_hit;
    eh_hit = 0;
    Disable();
    t = eh_trig;
    eh_trig = 0;
    if (v)
        for (i = 0; i < 4; i++)
            if (t & (1 << i)) {
                v[i].adr = (const BYTE *)eh_sadr[i];
                v[i].len = eh_slen[i];
            }
    Enable();
    for (i = 0; i < 4; i++) {
        vol[i] = eh_vol[i];
        if (eh_adr[i])
            eh_poked = TRUE;
        if (v && !ep.ups) {
            v[i].loop = (const BYTE *)eh_adr[i];        /* what plays after the start */
            v[i].replen = eh_plen[i];
            v[i].per = eh_per[i];
            v[i].vol = eh_vol[i];
        }
    }
    if (ep.ups) {
        t = 0;
        for (i = 0; i < 4; i++) {
            UWORD per = W(ep.ups, UPS_PER(i));
            if (per) {
                *hit |= (UBYTE)(1 << i);
                vol[i] = (UBYTE)W(ep.ups, UPS_VOL(i));
                W(ep.ups, UPS_PER(i)) = 0;
                if (v) {
                    t |= (UBYTE)(1 << i);
                    v[i].adr = (const BYTE *)L(ep.ups, 18 * i);
                    v[i].len = W(ep.ups, 4 + 18 * i);
                    v[i].per = per;
                    /* UPS_VoiceXRepeat: the sample plays once, then nothing */
                    v[i].loop = W(ep.ups, 16 + 18 * i) ? NULL : v[i].adr;
                    v[i].replen = W(ep.ups, 16 + 18 * i) ? 0 : v[i].len;
                }
            }
            if (v)
                v[i].vol = (UBYTE)W(ep.ups, UPS_VOL(i));
        }
    }
    if (trig)
        *trig = t;
    return ep.ups != NULL || eh_poked;
}
