/*
 * zzprobe - what a ZZ9000 offers this Amiga, and how fast, as Ambiance uses it
 * Copyright (c) 2026 Brian King, MIT
 *
 *   zzprobe [JPEG file] [PNG file] [WEBP file] [MP3 file] [QUICK] [NOBENCH]
 *
 *   zzprobe PATHS [SLIDE] [NODT] [NOZZ] [PICS file file ...]   (the picture paths alone: paths.c)
 *
 * Probes (the ZZ9000 SDK's headers, MIT, only for the picture paths'
 * calls to zz9k.library):
 *   - the machine, and the ZZ9000 board (Zorro II or III) by its IDs;
 *   - zz9k.library, mpega.library (the SDK's, decoding on the card, or the
 *     68k one), MHI drivers, AHI and the ZZ9000AX driver, AmiSSL (and the
 *     card's accelerated replacement), Picasso96 and CyberGraphX, the RTG
 *     display modes by monitor, the SDK's commands and ZZPlay;
 *   - the ZZ9000 datatype descriptors (active in DEVS:DataTypes, or
 *     installed inactive in SYS:Storage/DataTypes), and which datatype
 *     reads each picture and sound format here;
 *   - the card's own report: the SDK's zz9k-info and zz9k-services, run.
 * Then times what Ambiance does with them:
 *   - pictures, as one table: the datatype the system picks for each
 *     format; for JPEG also WarpJPEG and jfif.datatype (their classes used
 *     directly), and jdec built into zzprobe (Ambiance's own decoder,
 *     alone, from memory);
 *   - MP3 decoded by mpega.library, as Ambiance asks a 68k for this CPU,
 *     and at full quality (what it asks of the card's).
 * Default test files: PROGDIR:Bench/test.jpg, test.png, test.webp, test.mp3
 * (made by mkbench.py beside this file; the Bench drawer goes beside zzprobe).
 * The report goes to the Shell, and is added to PROGDIR:zzprobe.txt with
 * the date, so runs before and after a change sit side by side.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/datetime.h>
#include <dos/rdargs.h>
#include <dos/var.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <libraries/configvars.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <proto/expansion.h>
#include <proto/datatypes.h>
#include <proto/intuition.h>
#include <clib/alib_protos.h>
#include <string.h>
#include <stdio.h>
#include "../../src/jdec/jdec.h"
#include "stackcall.h"

/* used: nothing reads it, but Version finds it in the file (unused, it was dropped) */
static const char __attribute__((used)) version[] = "$VER: zzprobe 1.25 (22.11.2026)";

extern struct ExecBase *SysBase;
extern struct GfxBase *GfxBase;
struct Device *TimerBase;
struct Library *DataTypesBase;
struct ExpansionBase *ExpansionBase;
static struct Library *MPEGABase;

static struct timerequest treq;
static ULONG efreq;
static ULONG run_ms = 2000;             /* each picture test: about this long */
static ULONG mp3_secs = 30;             /* MP3: this many seconds of sound decoded */

/* ------------------------------------------------------------ the report */
static char *out;
static ULONG outn, outmax = 65536;

static void say(const char *s)
{
    ULONG n = strlen(s);
    PutStr((CONST_STRPTR)s);
    if (out && outn + n < outmax) {
        memcpy(out + outn, s, n);
        outn += n;
    }
}

static void sayf(const char *s)         /* a line */
{
    say(s);
    say("\n");
}

static void heading(const char *s)
{
    say("\n");
    sayf(s);
}

static ULONG now_us(void)
{
    struct EClockVal e;
    ReadEClock(&e);
    return (ULONG)((((unsigned long long)e.ev_hi << 32) | e.ev_lo) * 1000000ULL / efreq);
}

static void fmt_ms(char *s, ULONG us)
{
    if (us >= 10000)
        sprintf(s, "%ld ms", (long)((us + 500) / 1000));
    else
        sprintf(s, "%ld.%02ld ms", (long)(us / 1000), (long)(us % 1000 / 10));
}

/* ------------------------------------------------------------ small helpers */
static BOOL exists(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!l)
        return FALSE;
    UnLock(l);
    return TRUE;
}

/* A library's version and ID string, as a line: "name 2.29 (ID)", or
 * "name: not installed". Opening it is all that is done with it. */
static BOOL lib_line(const char *name, const char *show, char *idout, LONG idmax)
{
    char line[256];
    struct Library *l = OpenLibrary((CONST_STRPTR)name, 0);
    if (idout)
        idout[0] = 0;
    if (!l) {
        sprintf(line, "  %s: not installed", show);
        sayf(line);
        return FALSE;
    }
    sprintf(line, "  %s %ld.%ld", show, (long)l->lib_Version, (long)l->lib_Revision);
    if (l->lib_IdString) {
        char id[120];
        LONG n;
        strncpy(id, (const char *)l->lib_IdString, sizeof(id) - 1);
        id[sizeof(id) - 1] = 0;
        for (n = strlen(id); n > 0 && (id[n - 1] == '\n' || id[n - 1] == '\r'); n--)
            id[n - 1] = 0;
        sprintf(line + strlen(line), " (%s)", id);
        if (idout) {
            strncpy(idout, id, idmax - 1);
            idout[idmax - 1] = 0;
        }
    }
    CloseLibrary(l);
    sayf(line);
    return TRUE;
}

/* ------------------------------------------------------------ the machine */
static UWORD cpu_of(void)
{
    UWORD af = SysBase->AttnFlags;
    return (af & AFF_68060) ? 60 : (af & AFF_68040) ? 40 : (af & AFF_68030) ? 30 :
           (af & AFF_68020) ? 20 : (af & AFF_68010) ? 10 : 0;
}

static void machine(void)
{
    char line[200];
    UWORD af = SysBase->AttnFlags;
    BOOL uae = FALSE;
    heading("Machine");
    sprintf(line, "  CPU 680%02ld%s, Kickstart %ld.%ld, %s %s", (long)cpu_of(),
            (af & (AFF_68881 | AFF_68882 | (1 << 6))) ? " with FPU" : "",   /* bit 6: AFF_FPU40 */
            (long)SysBase->LibNode.lib_Version, (long)SysBase->SoftVer,
            (GfxBase->ChipRevBits0 & GFXF_AA_ALICE) ? "AGA" :
            (GfxBase->ChipRevBits0 & GFXF_HR_AGNUS) ? "ECS" : "OCS",
            (GfxBase->DisplayFlags & PAL) ? "PAL" : "NTSC");
    sayf(line);
    sprintf(line, "  Free memory: %ld KB chip, %ld KB fast", (long)(AvailMem(MEMF_CHIP) >> 10),
            (long)(AvailMem(MEMF_FAST) >> 10));
    sayf(line);
    if (ExpansionBase)
        uae = FindConfigDev(NULL, 2011, -1) != NULL;
    sayf(uae ? "  Emulator: UAE family (Amiberry, WinUAE, FS-UAE)" : "  Emulator: none seen");
}

/* ------------------------------------------------------------ the board */
static int zz_zorro;                    /* 0 none, 2, 3 */

static void board(void)
{
    struct ConfigDev *cd = NULL;
    char line[200];
    int n = 0;
    heading("ZZ9000 board (MNT, manufacturer $6d6e)");
    if (!ExpansionBase) {
        sayf("  expansion.library did not open");
        return;
    }
    while ((cd = FindConfigDev(cd, 0x6d6e, -1))) {
        UBYTE p = cd->cd_Rom.er_Product;
        const char *what = p == 4 ? "ZZ9000 on Zorro III" : p == 3 ? "ZZ9000 on Zorro II"
                                                           : "another MNT board";
        sprintf(line, "  %s (product %ld) at $%08lx, %ld KB", what, (long)p,
                (unsigned long)cd->cd_BoardAddr, (long)(cd->cd_BoardSize >> 10));
        sayf(line);
        if (p == 4)
            zz_zorro = 3;
        else if (p == 3 && !zz_zorro)
            zz_zorro = 2;
        n++;
    }
    if (!n)
        sayf("  none found");
}

/* ------------------------------------------------------------ software */
static BOOL mpega_card, mpega_any, zzlib, mhizz, amissl_acc, ahi_ax;

static void mhi_drivers(void)
{
    BPTR l = Lock((CONST_STRPTR)"LIBS:MHI", SHARED_LOCK);
    struct FileInfoBlock *fib;
    char line[200];
    int n = 0;
    if (!l) {
        sayf("  MHI drivers: none (no LIBS:MHI)");
        return;
    }
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib))
            while (ExNext(l, fib)) {
                if (fib->fib_DirEntryType > 0)
                    continue;
                sprintf(line, "  MHI driver: %.100s", fib->fib_FileName);
                if (!strcmp((char *)fib->fib_FileName, "mhizz9000.library")) {
                    strcat(line, " (the ZZ9000's)");
                    mhizz = TRUE;
                }
                sayf(line);
                n++;
            }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    if (!n)
        sayf("  MHI drivers: none in LIBS:MHI");
}

/* RTG display modes, one line per monitor (the part of a mode's name
 * before the colon: "ZZ9000", "PRISM", "uaegfx"...), with how many modes. */
static void rtg_monitors(void)
{
    char names[8][32], line[200];
    LONG counts[8];
    int nn = 0, i;
    ULONG id = INVALID_ID;
    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        struct DisplayInfo di;
        struct NameInfo ni;
        char mon[32], *c;
        BOOL rtg = (id & MONITOR_ID_MASK) > DBLPAL_MONITOR_ID;
        if (!rtg && GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, id) > 0)
            rtg = (di.PropertyFlags & DIPF_IS_FOREIGN) != 0;
        if (!rtg)
            continue;
        if (GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, id) <= 0)
            strcpy(mon, "(unnamed)");
        else {
            strncpy(mon, (char *)ni.Name, sizeof(mon) - 1);
            mon[sizeof(mon) - 1] = 0;
            if ((c = strchr(mon, ':')))
                *c = 0;
        }
        for (i = 0; i < nn && strcmp(names[i], mon); i++)
            ;
        if (i == nn && nn < 8) {
            strcpy(names[nn], mon);
            counts[nn++] = 0;
        }
        if (i < nn)
            counts[i]++;
    }
    if (!nn)
        sayf("  RTG modes: none");
    for (i = 0; i < nn; i++) {
        sprintf(line, "  RTG modes: %ld from \"%.31s\"", (long)counts[i], names[i]);
        sayf(line);
    }
}

static void descriptor(const char *name)
{
    char a[80], b[80], line[160];
    sprintf(a, "DEVS:DataTypes/%s", name);
    sprintf(b, "SYS:Storage/DataTypes/%s", name);
    sprintf(line, "  %-18s %s", name,
            exists(a) ? "active (DEVS:DataTypes)"
            : exists(b) ? "installed but inactive: move it from SYS:Storage/DataTypes to DEVS:DataTypes"
            : "not installed");
    sayf(line);
}

static void software(void)
{
    char id[120];
    heading("Libraries and programs");
    zzlib = lib_line("zz9k.library", "zz9k.library (the card's services)", NULL, 0);
    mpega_any = lib_line("mpega.library", "mpega.library", id, sizeof(id));
    mpega_card = mpega_any && strstr(id, "ZZ9000") != NULL;
    if (mpega_any)
        sayf(mpega_card ? "    -> the ZZ9000 SDK's: MP3 decoded on the card"
                        : "    -> not the card's: MP3 decoded by the 68k");
    mhi_drivers();
    sayf(exists("DEVS:ahi.device") ? "  AHI: installed" : "  AHI: not installed");
    ahi_ax = exists("DEVS:AHI/zz9000ax.audio");
    sayf(ahi_ax ? "  AHI driver for the ZZ9000AX: installed (DEVS:AHI/zz9000ax.audio)"
                : "  AHI driver for the ZZ9000AX: not installed");
    if (exists("LIBS:AmiSSL/amissl_v362.library")) {
        amissl_acc = exists("LIBS:AmiSSL/amissl_v362.library.bak");
        sayf(amissl_acc ? "  AmiSSL 5.x: the ZZ9000's accelerated amissl_v362.library (original kept as .bak)"
                        : "  AmiSSL 5.x: installed, the original (not the ZZ9000's accelerated one)");
    } else {
        lib_line("amisslmaster.library", "amisslmaster.library", NULL, 0);
    }
    lib_line("Picasso96API.library", "Picasso96API.library", NULL, 0);
    lib_line("cybergraphics.library", "cybergraphics.library", NULL, 0);
    rtg_monitors();
    sayf(exists("C:zz9k-info") ? "  C:zz9k-info: installed" : "  C:zz9k-info: not installed");
    sayf(exists("C:zz9k-services") ? "  C:zz9k-services: installed"
                                   : "  C:zz9k-services: not installed");
    sayf(exists("SYS:Utilities/ZZ9000/ZZPlay") ? "  ZZPlay: installed (SYS:Utilities/ZZ9000)"
                                                : "  ZZPlay: not found in SYS:Utilities/ZZ9000");

    heading("ZZ9000 datatype descriptors");
    descriptor("ZZ9000-JPEG");
    descriptor("ZZ9000-PNG");
    descriptor("ZZ9000-WebP");
    descriptor("ZZ9000-MP3");
    descriptor("ZZ9000-FLAC");
    descriptor("ZZ9000-OggVorbis");
}

/* ------------------------------------------------------------ which datatype reads what */
static const UBYTE s_ilbm[] = { 'F','O','R','M', 0,0,0,46, 'I','L','B','M',
    'B','M','H','D', 0,0,0,20, 0,1, 0,1, 0,0, 0,0, 1, 0, 0, 0, 0,0, 10, 11, 0,1, 0,1,
    'C','M','A','P', 0,0,0,6, 0,0,0, 255,255,255, 'B','O','D','Y', 0,0,0,2, 0,0 };
static const UBYTE s_gif[] = { 'G','I','F','8','9','a', 1,0, 1,0, 0x80, 0, 0,
    0,0,0, 255,255,255, ',', 0,0, 0,0, 1,0, 1,0, 0, 2, 2, 0x44, 1, 0, ';' };
static const UBYTE s_jpeg[] = { 0xFF,0xD8, 0xFF,0xE0, 0,16, 'J','F','I','F',0, 1,1, 0, 0,1, 0,1,
    0,0, 0xFF,0xD9 };
static const UBYTE s_png[] = { 0x89,'P','N','G',13,10,26,10, 0,0,0,13, 'I','H','D','R',
    0,0,0,1, 0,0,0,1, 8, 2, 0, 0, 0, 0x90,0x77,0x53,0xDE, 0,0,0,0, 'I','E','N','D',
    0xAE,0x42,0x60,0x82 };
static const UBYTE s_webp[] = { 'R','I','F','F', 26,0,0,0, 'W','E','B','P', 'V','P','8','L',
    13,0,0,0, 0x2F, 0,0,0, 0x10, 0x07,0x10,0x11,0x11,0x88,0x88,0xFE,0x07, 0 };
static const UBYTE s_flac[] = { 'f','L','a','C', 0x80,0,0,34, 0x10,0x00,0x10,0x00, 0,0,0, 0,0,0,
    0x0A,0xC4,0x42,0xF0, 0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0 };
static const UBYTE s_ogg[] = { 'O','g','g','S', 0, 2, 0,0,0,0,0,0,0,0, 1,0,0,0, 0,0,0,0,
    0,0,0,0, 1, 30, 1,'v','o','r','b','i','s', 0,0,0,0, 2, 0x44,0xAC,0,0, 0,0,0,0,
    0x00,0xF4,0x01,0x00, 0,0,0,0, 0xB8, 1 };

static char jpeg_cls[40];                /* the class that reads JPEGs here */

/* How a descriptor knows its files: the bytes it looks for at the start
 * (?? any), the name pattern, its flags and priority */
static void desc_detail(const struct DataTypeHeader *h)
{
    char line[200];
    int i, n = h->dth_MaskLen, o;
    o = sprintf(line, "        recognises: ");
    if (!n || !h->dth_Mask)
        o += sprintf(line + o, "(no bytes)");
    for (i = 0; i < n && i < 16; i++) {
        WORD m = h->dth_Mask[i];
        if (m < 0) {
            o += sprintf(line + o, "?? ");
        } else if (m >= 32 && m < 127) {    /* not %c: this libnix's is 16-bit */
            line[o++] = '\'';
            line[o++] = (char)m;
            line[o++] = '\'';
            line[o++] = ' ';
            line[o] = 0;
        } else {
            o += sprintf(line + o, "%02lx ", (unsigned long)(m & 0xFF));
        }
    }
    if (n > 16)
        o += sprintf(line + o, "...");
    snprintf(line + o, sizeof(line) - o, "; name %.30s; flags $%04lx; priority %ld",
             h->dth_Pattern ? (const char *)h->dth_Pattern : "-",
             (unsigned long)h->dth_Flags, (long)h->dth_Priority);
    sayf(line);
}

/* which descriptor an actual file matches */
static void file_dt(const char *fmt, const char *path)
{
    char line[200];
    BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
    struct DataType *dtn = NULL;
    if (!l)
        return;
    dtn = ObtainDataTypeA(DTST_FILE, (APTR)l, NULL);
    UnLock(l);
    if (dtn && dtn->dtn_Header) {
        snprintf(line, sizeof(line), "  %-5s %.60s: descriptor %s (%s.datatype)", fmt, path,
                 dtn->dtn_Header->dth_Name ? (const char *)dtn->dtn_Header->dth_Name : "?",
                 dtn->dtn_Header->dth_BaseName ? (const char *)dtn->dtn_Header->dth_BaseName : "?");
        sayf(line);
    } else {
        snprintf(line, sizeof(line), "  %-5s %.60s: no descriptor matches it", fmt, path);
        sayf(line);
    }
    if (dtn)
        ReleaseDataType(dtn);
}

static void which_dt(const char *fmt, const char *ext, const UBYTE *data, ULONG len, ULONG group)
{
    char file[40], line[200];
    BPTR fh, l;
    struct DataType *dtn = NULL;
    sprintf(file, "T:zzprobe.%s", ext);
    if ((fh = Open((CONST_STRPTR)file, MODE_NEWFILE))) {
        Write(fh, (APTR)data, len);
        Close(fh);
    }
    if ((l = Lock((CONST_STRPTR)file, SHARED_LOCK))) {
        dtn = ObtainDataTypeA(DTST_FILE, (APTR)l, NULL);
        UnLock(l);
    }
    DeleteFile((CONST_STRPTR)file);
    if (!dtn || !dtn->dtn_Header || dtn->dtn_Header->dth_GroupID != group) {
        sprintf(line, "  %-5s no datatype", fmt);
    } else {
        const char *base = dtn->dtn_Header->dth_BaseName ? (const char *)dtn->dtn_Header->dth_BaseName : "?";
        if (!strcmp(fmt, "JPEG")) {
            strncpy(jpeg_cls, base, sizeof(jpeg_cls) - 1);
            jpeg_cls[sizeof(jpeg_cls) - 1] = 0;
        }
        sprintf(line, "  %-5s %s.datatype (descriptor %s)%s", fmt, base,
                dtn->dtn_Header->dth_Name ? (const char *)dtn->dtn_Header->dth_Name : "?",
                !strncmp(base, "zz9k", 4) ? "  <- on the ZZ9000" : "");
    }
    sayf(line);
    if (dtn && dtn->dtn_Header && group == GID_PICTURE)
        desc_detail(dtn->dtn_Header);
    if (dtn)
        ReleaseDataType(dtn);
}

static void datatypes(const char *jpeg, const char *png, const char *webp)
{
    UBYTE *mp3;
    ULONG i;
    heading("Which datatype reads each format here");
    if (!DataTypesBase) {
        sayf("  datatypes.library did not open");
        return;
    }
    which_dt("JPEG", "jpg", s_jpeg, sizeof(s_jpeg), GID_PICTURE);
    which_dt("PNG", "png", s_png, sizeof(s_png), GID_PICTURE);
    which_dt("WebP", "webp", s_webp, sizeof(s_webp), GID_PICTURE);
    which_dt("GIF", "gif", s_gif, sizeof(s_gif), GID_PICTURE);
    which_dt("ILBM", "iff", s_ilbm, sizeof(s_ilbm), GID_PICTURE);
    /* MP3: three 128 kbit/s 44.1 kHz frames (headers, silence) */
    if ((mp3 = AllocVec(3 * 417, MEMF_ANY | MEMF_CLEAR))) {
        for (i = 0; i < 3; i++) {
            mp3[i * 417] = 0xFF;
            mp3[i * 417 + 1] = 0xFB;
            mp3[i * 417 + 2] = 0x90;
            mp3[i * 417 + 3] = 0x64;
        }
        which_dt("MP3", "mp3", mp3, 3 * 417, GID_SOUND);
        FreeVec(mp3);
    }
    which_dt("FLAC", "flac", s_flac, sizeof(s_flac), GID_SOUND);
    which_dt("Ogg", "ogg", s_ogg, sizeof(s_ogg), GID_SOUND);
    heading("Which descriptor each test file matches");
    file_dt("JPEG", jpeg);
    file_dt("PNG", png);
    file_dt("WebP", webp);
}

/* ------------------------------------------------------------ the card's own report */
static void run_tool(const char *cmd, const char *title)
{
    BPTR fh;
    LONG rc;
    char line[200];
    heading(title);
    if (!(fh = Open((CONST_STRPTR)"T:zzprobe.out", MODE_NEWFILE))) {
        sayf("  T: could not be written");
        return;
    }
    rc = SystemTags((CONST_STRPTR)cmd, SYS_Output, fh, SYS_Input, 0, TAG_END);
    Close(fh);
    if (rc == -1) {
        sayf("  could not be run");
    } else if ((fh = Open((CONST_STRPTR)"T:zzprobe.out", MODE_OLDFILE))) {
        int n = 0;
        while (FGets(fh, (STRPTR)line + 2, sizeof(line) - 3) && n < 120) {
            line[0] = line[1] = ' ';
            say(line);
            if (line[strlen(line) - 1] != '\n')
                say("\n");
            n++;
        }
        if (!n)
            sayf("  (no output)");
        Close(fh);
        sprintf(line, "  (return code %ld)", (long)rc);
        sayf(line);
    }
    DeleteFile((CONST_STRPTR)"T:zzprobe.out");
}

/* ------------------------------------------------------------ bench: pictures, as a table */
/* Each format's picture decoded every way there is here, one row a way:
 *   - the datatype the system picks for it (NewDTObject, from the file);
 *   - JPEG: WarpJPEG and jfif.datatype, each class made into objects
 *     directly from the file, so each is timed whichever the system picks;
 *   - JPEG: jdec built into zzprobe, from memory: the decoder alone.
 * Times: best of up to 10 runs (mean); "first" is the first load apart,
 * with the datatype's loading (and any requester it shows). */
#define NSC 4
struct Row {
    const char *fmt;
    char dec[48];
    ULONG first;                        /* us; 0: not timed */
    ULONG best[NSC], mean[NSC];         /* us at 1/1 .. 1/8; 0: not run */
    char note[120];                     /* instead of times */
};
static struct Row rows[12];
static int nrows;

/* Where the time goes, the fastest run's parts (us): JPEG through the
 * system's datatype at 1/1 (sysph) and through each class */
enum { PH_READ, PH_NEW, PH_LAYOUT, PH_DISPOSE, PH_TOTAL, PH_N };
static ULONG sysph[PH_N];
static BOOL have_sysph;
static ULONG sys_mode = (ULONG)INVALID_ID;
/* the JPEG classes benched side by side, each made into objects directly */
enum { CL_WARP, CL_JFIF, CL_N };
static ULONG clph[CL_N][NSC][PH_N];       /* the fastest run's parts, at each scale */
static ULONG cl_mode[CL_N];               /* the display mode each one's object ends with */
static char notes[8][160];
static int nnotes;

static struct Row *row_new(const char *fmt, const char *dec)
{
    struct Row *r = &rows[nrows < 11 ? nrows++ : 11];
    memset(r, 0, sizeof(*r));
    r->fmt = fmt;
    strncpy(r->dec, dec, sizeof(r->dec) - 1);
    return r;
}

static void note_add(const char *s)
{
    if (nnotes < 8) {
        strncpy(notes[nnotes], s, sizeof(notes[0]) - 1);
        nnotes++;
    }
}

/* why an object was not made, in words */
static void why(LONG err, char *out, LONG max)
{
    char t[100];
    t[0] = 0;
    if (err >= DTERROR_UNKNOWN_DATATYPE && err < DTERROR_UNKNOWN_DATATYPE + 100) {
        CONST_STRPTR s = GetDTString((ULONG)err);
        char *p;
        if (s)
            strncpy(t, (const char *)s, sizeof(t) - 1);
        t[sizeof(t) - 1] = 0;
        if ((p = strstr(t, " for %s")) || (p = strstr(t, " %s")))
            *p = 0;                     /* its file name slot: the row says which */
    } else if (err > 0) {
        Fault(err, NULL, (STRPTR)t, sizeof(t));
    }
    if (!t[0])
        strcpy(t, "no reason given");
    snprintf(out, max, "failed: %s", t);
}

typedef LONG (*Once)(void *ctx);        /* one decode: 0, or why not */

/* first (if wanted), then up to 10 runs within run_ms */
/* Ctrl-C in its Shell: everything still to time is skipped (once seen,
 * it stays seen), and the rest of the run ends quickly */
static BOOL brk;

static BOOL brk_check(void)
{
    if (!brk && (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)) {
        brk = TRUE;
        sayf("*** Break (Ctrl-C): zzprobe stops after this step");
    }
    return brk;
}

static LONG timed(Once f, void *ctx, ULONG *first, ULONG *best, ULONG *mean)
{
    ULONG start, runs = 0, sum = 0, b = 0xFFFFFFFFUL, t0, dt;
    LONG err;
    if (brk_check())
        return -2;
    if (first) {
        t0 = now_us();
        if ((err = f(ctx)))
            return err;
        *first = now_us() - t0;
        if (!*first)
            *first = 1;
    }
    start = now_us();
    do {
        t0 = now_us();
        if ((err = f(ctx)))
            return err;
        dt = now_us() - t0;
        if (dt < b)
            b = dt;
        sum += dt;
        runs++;
    } while (runs < 10 && now_us() - start < run_ms * 1000 && !brk_check());
    *best = b ? b : 1;
    *mean = sum / runs ? sum / runs : 1;
    return 0;
}

static LONG file_size(const char *path)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG n;
    if (!fh)
        return -1;
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_BEGINNING);
    Close(fh);
    return n;
}

static BOOL file_read(const char *path, UBYTE *buf, LONG n)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    BOOL ok;
    if (!fh)
        return FALSE;
    ok = Read(fh, buf, n) == n;
    Close(fh);
    return ok;
}

/* -- the system's datatype, through NewDTObject */
struct DtRun {
    const char *path;
    ULONG w, h;
    char base[40];
    ULONG *ph;                          /* the fastest run's parts, or NULL */
};

static LONG dt_once(void *p)
{
    struct DtRun *c = p;
    struct BitMapHeader *bmh = NULL;
    struct DataType *dtn = NULL;
    ULONG t0 = now_us(), t1, t2, t3;
    Object *o = NewDTObject((APTR)c->path, DTA_GroupID, GID_PICTURE, PDTA_Remap, FALSE,
                            PDTA_DestMode, PMODE_V43, TAG_END);
    if (!o) {
        LONG e = IoErr();
        return e ? e : -1;
    }
    t1 = now_us();
    DoDTMethod(o, NULL, NULL, DTM_PROCLAYOUT, NULL, (ULONG)TRUE);
    t2 = now_us();
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, DTA_DataType, (ULONG)&dtn, TAG_END);
    if (c->ph)
        GetDTAttrs(o, PDTA_ModeID, (ULONG)&sys_mode, TAG_END);
    if (bmh) {
        c->w = bmh->bmh_Width;
        c->h = bmh->bmh_Height;
    }
    if (dtn && dtn->dtn_Header && dtn->dtn_Header->dth_BaseName) {
        strncpy(c->base, (const char *)dtn->dtn_Header->dth_BaseName, sizeof(c->base) - 1);
        c->base[sizeof(c->base) - 1] = 0;
    }
    DisposeDTObject(o);
    t3 = now_us();
    if (c->ph && (!c->ph[PH_TOTAL] || t3 - t0 < c->ph[PH_TOTAL])) {
        c->ph[PH_NEW] = t1 - t0;        /* the file read, and the decode, inside */
        c->ph[PH_LAYOUT] = t2 - t1;
        c->ph[PH_DISPOSE] = t3 - t2;
        c->ph[PH_TOTAL] = t3 - t0;
    }
    return 0;
}

/* -- a JPEG datatype's class, its objects made directly from the file */
struct ClsRun {
    Class *cl;
    struct DataType *dtn;
    const char *path;
    ULONG w, h;
    ULONG *ph;                          /* the fastest run's parts */
    ULONG *mode_out;                    /* where to put the mode it ends with */
};

static LONG cls_once(void *p)
{
    struct ClsRun *c = p;
    struct BitMapHeader *bmh = NULL;
    ULONG t0 = now_us(), t1, t2, t3, t4;
    Object *o;
    /* the file locked here and the lock given to the object, as
     * NewDTObject does (DTA_Handle for DTST_FILE is a lock: datatypesclass
     * Examine()s it and opens the file from it; a file handle crashed, see
     * paths.c); the object's from then on, so it is not unlocked here */
    BPTR fh = Lock((CONST_STRPTR)c->path, SHARED_LOCK);
    if (!fh)
        return IoErr() ? IoErr() : -1;
    t1 = now_us();
    o = NewObject(c->cl, NULL, DTA_SourceType, DTST_FILE, DTA_Handle, (ULONG)fh,
                  DTA_DataType, (ULONG)c->dtn, DTA_Name, (ULONG)c->path,
                  PDTA_Remap, FALSE, PDTA_DestMode, PMODE_V43, TAG_END);
    if (!o) {
        LONG e = IoErr();
        return e ? e : -1;
    }
    t2 = now_us();
    DoDTMethod(o, NULL, NULL, DTM_PROCLAYOUT, NULL, (ULONG)TRUE);
    t3 = now_us();
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_END);
    if (c->ph && c->mode_out)
        GetDTAttrs(o, PDTA_ModeID, (ULONG)c->mode_out, TAG_END);
    if (bmh) {
        c->w = bmh->bmh_Width;
        c->h = bmh->bmh_Height;
    }
    {
        ULONG d0 = now_us(), tot;       /* the GetDTAttrs above left out */
        DisposeObject(o);
        t4 = now_us() - d0;             /* disposing */
        tot = (t3 - t0) + t4;
        if (c->ph && (!c->ph[PH_TOTAL] || tot < c->ph[PH_TOTAL])) {
            c->ph[PH_READ] = t1 - t0;   /* opening it: the reading is the class's, in OM_NEW */
            c->ph[PH_NEW] = t2 - t1;    /* picture.datatype's OM_NEW, and the class's with the decode */
            c->ph[PH_LAYOUT] = t3 - t2;
            c->ph[PH_DISPOSE] = t4;
            c->ph[PH_TOTAL] = tot;
        }
    }
    return 0;
}

/* -- jdec built in, from memory */
struct MemSrc {
    const UBYTE *p;
    ULONG n, pos;
};

static int32_t mem_read(void *ctx, uint8_t *buf, int32_t n)
{
    struct MemSrc *m = ctx;
    if ((ULONG)n > m->n - m->pos)
        n = (int32_t)(m->n - m->pos);
    memcpy(buf, m->p + m->pos, (size_t)n);
    m->pos += (ULONG)n;
    return n;
}

static void *mem_alloc(void *ctx, uint32_t n)
{
    (void)ctx;
    return AllocVec(n, MEMF_ANY);
}

static void mem_free(void *ctx, void *p)
{
    (void)ctx;
    FreeVec(p);
}

static int rows_drop(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride)
{
    (void)ctx; (void)rgb; (void)y; (void)n; (void)stride;
    return 0;                           /* decoded, then nowhere to go: the decoder alone */
}

static const char *jd_why(int r)
{
    switch (r) {
    case JD_ENOMEM: return "out of memory";
    case JD_EFORMAT: return "not a JPEG, or damaged";
    case JD_EPROGRESSIVE: return "progressive JPEG (not decoded yet)";
    case JD_ECMYK: return "CMYK JPEG (not decoded yet)";
    case JD_EUNSUPPORTED: return "a kind of JPEG not decoded";
    case JD_ETOOBIG: return "too big";
    default: return "error";
    }
}

struct JdRun {
    UBYTE *data;
    LONG len;
    int scale;
    int err;
};

static LONG jd_once(void *p)
{
    struct JdRun *c = p;
    struct MemSrc m;
    JDecIO io;
    JDecInfo info;
    JDec *d;
    int r;
    m.p = c->data;
    m.n = (ULONG)c->len;
    m.pos = 0;
    io.ctx = &m;
    io.read = mem_read;
    io.alloc = mem_alloc;
    io.free = mem_free;
    io.rows = rows_drop;
    if (!(r = jd_open(&d, &io, &info))) {
        r = jd_decode(d, c->scale);
        jd_close(d);
    }
    c->err = r;
    return r ? -1 : 0;
}

/* A WebP that strict decoders refuse: chunks after the image without
 * the extended header (VP8X) that allows them (C2PA, EXIF added later) */
static BOOL webp_odd(const char *path)
{
    UBYTE h[20];
    LONG n = file_size(path);
    ULONG c;
    if (n < 20 || !file_read(path, h, 20))
        return FALSE;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WEBP", 4) ||
        (memcmp(h + 12, "VP8 ", 4) && memcmp(h + 12, "VP8L", 4)))
        return FALSE;
    c = h[16] | (h[17] << 8) | ((ULONG)h[18] << 16) | ((ULONG)h[19] << 24);
    return (ULONG)n > 20 + c + (c & 1);
}

/* A library left in memory by an earlier run, opened by nobody, put out,
 * so the file on disk is what is tested */
static void expunge_lib(const char *name)
{
    struct Library *l;
    Forbid();
    if ((l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)name)) && !l->lib_OpenCnt)
        RemLibrary(l);
    Permit();
}

/* jfif.datatype (Henryk Richter's) decodes smaller only through its
 * prefs (SCALE=0..3 here, read when it is loaded, for every program) */
#define JFIF_PREFS "classes/datatypes/jfif44.prefs"

static void bench_system(const char *fmt, const char *path, ULONG *pw, ULONG *ph)
{
    struct DtRun c;
    struct Row *r;
    LONG err;
    char name[64];
    memset(&c, 0, sizeof(c));
    c.path = path;
    if (!exists(path)) {
        r = row_new(fmt, "-");
        snprintf(r->note, sizeof(r->note), "no test file (%.60s)", path);
        return;
    }
    r = row_new(fmt, "the system's datatype");
    if (!strcmp(fmt, "JPEG")) {
        memset(sysph, 0, sizeof(sysph));
        c.ph = sysph;
    }
    if ((err = timed(dt_once, &c, &r->first, &r->best[0], &r->mean[0]))) {
        why(err, r->note, sizeof(r->note));
        return;
    }
    if (c.ph) {
        have_sysph = TRUE;
        c.ph = NULL;
    }
    snprintf(name, sizeof(name), "%.40s.datatype", c.base[0] ? c.base : "?");
    strncpy(r->dec, name, sizeof(r->dec) - 1);
    if (pw)
        *pw = c.w;
    if (ph)
        *ph = c.h;
}

#define ObtainEngine(base) LP0(30, Class *, ObtainEngine, , base)

/* One JPEG class, its objects made directly from the file at each scale
 * it can do: jfif.datatype through its prefs, WarpJPEG at 1/1. Installed
 * (datatypes/), or beside zzprobe. */
static const char *const cl_name[CL_N] = { "WarpJPEG.datatype", "jfif.datatype" };
static const char *const cl_local[CL_N] = { NULL, "PROGDIR:jfif/jfif.datatype" };

static struct Library *cl_open(int k, BOOL *local)
{
    char name[64];
    struct Library *lib;
    snprintf(name, sizeof(name), "datatypes/%s", cl_name[k]);
    *local = FALSE;
    lib = OpenLibrary((CONST_STRPTR)name, 0);
    if (!lib && cl_local[k] && (lib = OpenLibrary((CONST_STRPTR)cl_local[k], 0)))
        *local = TRUE;
    return lib;
}

static void bench_class(int k, const char *path, ULONG w, ULONG h)
{
    struct Library *lib;
    struct ClsRun c;
    struct Row *r;
    char label[48], old[200], val[220];
    BPTR lock;
    LONG err, had = 0;
    int s, ns = k == CL_WARP ? 1 : NSC;
    BOOL local;
    expunge_lib(cl_name[k]);            /* the file on disk, not one an earlier run left */
    lib = cl_open(k, &local);
    snprintf(label, sizeof(label), "%s%s", cl_name[k], local ? " (*)" : "");
    r = row_new("JPEG", label);
    if (!lib) {
        snprintf(r->note, sizeof(r->note), "not installed%s", cl_local[k] ? ", nor beside zzprobe" : "");
        return;
    }
    if (local)
        note_add("(*) not installed: loaded from beside zzprobe for this test");
    memset(&c, 0, sizeof(c));
    c.path = path;
    c.cl = ObtainEngine(lib);
    if ((lock = Lock((CONST_STRPTR)path, SHARED_LOCK))) {
        c.dtn = ObtainDataTypeA(DTST_FILE, (APTR)lock, NULL);
        UnLock(lock);
    }
    memset(clph[k], 0, sizeof(clph[k]));
    cl_mode[k] = (ULONG)INVALID_ID;
    if (k == CL_JFIF)
        had = GetVar((STRPTR)JFIF_PREFS, (STRPTR)old, sizeof(old), GVF_GLOBAL_ONLY);
    for (s = 0; s < ns && c.cl; s++) {
        if (s && k == CL_JFIF) {        /* its prefs, read when it is loaded */
            snprintf(val, sizeof(val), "%s%sSCALE=%ld", had > 0 ? old : "", had > 0 ? " " : "", (long)s);
            SetVar((STRPTR)JFIF_PREFS, (STRPTR)val, -1, GVF_GLOBAL_ONLY);
            CloseLibrary(lib);
            expunge_lib(cl_name[k]);
            if (!(lib = cl_open(k, &local)) || !(c.cl = ObtainEngine(lib))) {
                strcpy(r->note, "could not be loaded again");
                break;
            }
        }
        c.ph = NULL;
        c.mode_out = NULL;
        if (!s) {                       /* the first load apart */
            ULONG t0 = now_us();
            if ((err = cls_once(&c))) {
                why(err, r->note, sizeof(r->note));
                break;
            }
            r->first = now_us() - t0;
            if (!r->first)
                r->first = 1;
        }
        c.ph = clph[k][s];
        c.mode_out = s ? NULL : &cl_mode[k];
        if ((err = timed(cls_once, &c, NULL, &r->best[s], &r->mean[s]))) {
            why(err, r->note, sizeof(r->note));
            break;
        }
    }
    if (k == CL_JFIF) {
        if (had > 0)
            SetVar((STRPTR)JFIF_PREFS, (STRPTR)old, -1, GVF_GLOBAL_ONLY);
        else
            DeleteVar((STRPTR)JFIF_PREFS, GVF_GLOBAL_ONLY);
        note_add("jfif.datatype at 1/2 to 1/8: its own SCALE prefs, set for the test and put back");
    }
    if (c.dtn)
        ReleaseDataType(c.dtn);
    if (lib)
        CloseLibrary(lib);
    if (k == CL_JFIF)
        expunge_lib(cl_name[k]);        /* its prefs as they were, next time */
}

static void ph_cell(char *out, ULONG us, BOOL have)
{
    if (!have)
        strcpy(out, "-");
    else if (us >= 10000)
        sprintf(out, "%ld", (long)((us + 500) / 1000));
    else
        sprintf(out, "%ld.%ld", (long)(us / 1000), (long)(us % 1000 / 100));
}

/* the parts of the fastest runs, side by side */
static void time_parts(void)
{
    static const char *const name[PH_N] = {
        "open the file",
        "make the object (OM_NEW, the decode in it)",
        "layout (DTM_PROCLAYOUT)",
        "dispose of the object",
        "total",
    };
    char line[200], c[3][16];
    int i;
    BOOL have[CL_N];
    have[CL_WARP] = clph[CL_WARP][0][PH_TOTAL] != 0;
    have[CL_JFIF] = clph[CL_JFIF][0][PH_TOTAL] != 0;
    if (!have[CL_WARP] && !have[CL_JFIF] && !have_sysph)
        return;
    heading("Where a JPEG's time goes (ms, the parts of the fastest run, 1/1)");
    snprintf(line, sizeof(line), "  %-44s %-8s %-8s %-8s", "", "system", "WarpJPEG", "jfif");
    sayf(line);
    for (i = 0; i < PH_N; i++) {
        ph_cell(c[0], sysph[i], have_sysph && i != PH_READ);
        ph_cell(c[1], clph[CL_WARP][0][i], have[CL_WARP]);
        ph_cell(c[2], clph[CL_JFIF][0][i], have[CL_JFIF]);
        snprintf(line, sizeof(line), "  %-44s %-8s %-8s %-8s", name[i], c[0], c[1], c[2]);
        sayf(line);
    }
    snprintf(line, sizeof(line), "  Display mode after layout: the system's $%08lx, WarpJPEG $%08lx, jfif $%08lx",
             (unsigned long)sys_mode, (unsigned long)cl_mode[CL_WARP], (unsigned long)cl_mode[CL_JFIF]);
    sayf(line);
    sayf("  system: NewDTObject from the file (finding the datatype too); the classes'");
    sayf("  objects made directly. OM_NEW is the class's decode and picture.datatype's work.");
}

static void bench_builtin_jpeg(const char *path)
{
    struct Row *r = row_new("JPEG", "built in (jdec, from memory)");
    struct JdRun c;
    int s;
    memset(&c, 0, sizeof(c));
    c.len = file_size(path);
    if (c.len <= 0 || !(c.data = AllocVec((ULONG)c.len, MEMF_ANY)) || !file_read(path, c.data, c.len)) {
        strcpy(r->note, "the test file could not be read into memory");
        FreeVec(c.data);
        return;
    }
    for (s = 0; s < NSC; s++) {
        c.scale = s;
        if (timed(jd_once, &c, NULL, &r->best[s], &r->mean[s])) {
            snprintf(r->note, sizeof(r->note), "failed: %s", jd_why(c.err));
            break;
        }
    }
    FreeVec(c.data);
}

static void cell(char *out, ULONG best, ULONG mean)
{
    char b[16], m[16];
    if (!best) {
        strcpy(out, "-");
        return;
    }
    if (best >= 10000)
        sprintf(b, "%ld", (long)((best + 500) / 1000));
    else
        sprintf(b, "%ld.%ld", (long)(best / 1000), (long)(best % 1000 / 100));
    if (mean >= 10000)
        sprintf(m, "%ld", (long)((mean + 500) / 1000));
    else
        sprintf(m, "%ld.%ld", (long)(mean / 1000), (long)(mean % 1000 / 100));
    sprintf(out, "%s (%s)", b, m);
}

static void bench_pictures(const char *jpeg, const char *png, const char *webp)
{
    char line[200], c[NSC][24], f[16];
    ULONG jw = 0, jh = 0;
    int i, s;
    nrows = nnotes = 0;
    have_sysph = FALSE;
    sys_mode = (ULONG)INVALID_ID;
    memset(clph, 0, sizeof(clph));
    bench_system("JPEG", jpeg, &jw, &jh);
    if (exists(jpeg)) {                 /* the JPEG datatypes here, side by side */
        int k;
        for (k = 0; k < CL_N; k++)
            bench_class(k, jpeg, jw, jh);
    }
    if (exists(jpeg))
        bench_builtin_jpeg(jpeg);
    bench_system("PNG", png, NULL, NULL);
    strcpy(row_new("PNG", "Ambiance's own")->note, "not written yet");
    bench_system("WebP", webp, NULL, NULL);
    strcpy(row_new("WebP", "Ambiance's own")->note, "not written yet");
    if (exists(webp) && webp_odd(webp))
        note_add("test.webp has chunks after its image without a VP8X header (C2PA or EXIF "
                 "added later?): strict decoders refuse it");

    heading("Bench: pictures (ms, best of up to 10 runs, mean in brackets)");
    sayf("  Format Decoder                         first      1/1          1/2          1/4          1/8");
    for (i = 0; i < nrows; i++) {
        struct Row *r = &rows[i];
        if (r->note[0] && !r->best[0]) {
            snprintf(line, sizeof(line), "  %-6s %-30s %s", r->fmt, r->dec, r->note);
            sayf(line);
            continue;
        }
        if (r->first)
            sprintf(f, "%ld", (long)((r->first + 500) / 1000));
        else
            strcpy(f, "-");
        for (s = 0; s < NSC; s++)
            cell(c[s], r->best[s], r->mean[s]);
        snprintf(line, sizeof(line), "  %-6s %-30s %7s  %-12s %-12s %-12s %-12s",
                 r->fmt, r->dec, f, c[0], c[1], c[2], c[3]);
        sayf(line);
        if (r->note[0]) {
            snprintf(line, sizeof(line), "  %-6s %-30s %s", "", "", r->note);
            sayf(line);
        }
    }
    if (jw && jh) {
        snprintf(line, sizeof(line), "  Test picture %ld x %ld. 1/2 to 1/8: jfif.datatype's SCALE prefs; "
                 "jdec's own scales.", (long)jw, (long)jh);
        sayf(line);
    }
    sayf("  first: the first load, with loading the datatype (and any requester it shows).");
    sayf("  Datatypes: from the file, with the layout; built in: decoding alone, from memory.");
    for (i = 0; i < nnotes; i++) {
        snprintf(line, sizeof(line), "  %s", notes[i]);
        sayf(line);
    }
    time_parts();
}

/* ------------------------------------------------------------ bench: picture paths */
#include "paths.c"

/* ------------------------------------------------------------ bench: MP3 by mpega.library */
/* mpega.library has no C header here; its structures and calls as in
 * Ambiance's stream.c, from its documentation. */
typedef struct { WORD freq_div, quality; LONG freq_max; } MPEGA_OUTPUT;
typedef struct { WORD force_mono; MPEGA_OUTPUT mono, stereo; } MPEGA_LAYER;
typedef struct {
    struct Hook *bs_access;
    MPEGA_LAYER layer_1_2, layer_3;
    WORD check_mpeg;
    LONG stream_buffer_size;
} MPEGA_CTRL;
typedef struct {
    WORD norm, layer, mode, bitrate;
    LONG frequency;
    WORD channels;
    ULONG ms_duration;
    WORD private_bit, copyright, original;
    WORD dec_channels, dec_quality;
    LONG dec_frequency;
    APTR handle;
} MPEGA_STREAM;
#define MPEGA_PCM_SIZE 1152
#define MPEGA_open(n, c) \
    LP2(30, MPEGA_STREAM *, MPEGA_open, char *, n, a0, MPEGA_CTRL *, c, a1, , MPEGABase)
#define MPEGA_close(s) LP1NR(36, MPEGA_close, MPEGA_STREAM *, s, a0, , MPEGABase)
#define MPEGA_decode_frame(s, p) \
    LP2(42, LONG, MPEGA_decode_frame, MPEGA_STREAM *, s, a0, WORD **, p, a1, , MPEGABase)

static void bench_mp3_one(const char *path, const char *label, WORD q, WORD mono, LONG fmax)
{
    static WORD pcm0[MPEGA_PCM_SIZE], pcm1[MPEGA_PCM_SIZE];
    WORD *pcm[2];
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *ms;
    ULONG t0, us, samples = 0, want;
    char line[320], t[24];
    int k;
    pcm[0] = pcm0;
    pcm[1] = pcm1;
    memset(&ctrl, 0, sizeof(ctrl));
    for (k = 0; k < 2; k++) {
        MPEGA_LAYER *ly = k ? &ctrl.layer_3 : &ctrl.layer_1_2;
        ly->force_mono = mono;
        ly->mono.freq_div = 1;
        ly->mono.quality = q;
        ly->mono.freq_max = fmax;
        ly->stereo = ly->mono;
    }
    if (!(ms = MPEGA_open((char *)path, &ctrl))) {
        sprintf(line, "  %s: mpega.library could not open %.100s", label, path);
        sayf(line);
        return;
    }
    want = (ULONG)ms->dec_frequency * mp3_secs;
    t0 = now_us();
    while (samples < want) {
        LONG n = MPEGA_decode_frame(ms, pcm);
        if (n == -2)
            continue;                   /* a bad frame: the next */
        if (n < 0)
            break;                      /* the end */
        samples += n;
    }
    us = now_us() - t0;
    if (samples && ms->dec_frequency) {
        ULONG audio_ms = (ULONG)((unsigned long long)samples * 1000 / (ULONG)ms->dec_frequency);
        fmt_ms(t, audio_ms ? us / audio_ms * 1000 : 0);
        sprintf(line, "  %s: %ld kbit/s %ld Hz %s, decoded at %ld Hz %s (quality %ld): "
                      "%s per second of sound, %ld.%ldx real time (%ld s decoded)",
                label, (long)ms->bitrate, (long)ms->frequency, ms->channels == 2 ? "stereo" : "mono",
                (long)ms->dec_frequency, ms->dec_channels == 2 ? "stereo" : "mono",
                (long)ms->dec_quality, t,
                (long)(us ? (unsigned long long)audio_ms * 1000 / us : 0),
                (long)(us ? (unsigned long long)audio_ms * 10000 / us % 10 : 0),
                (long)(audio_ms / 1000));
    } else {
        sprintf(line, "  %s: nothing decoded", label);
    }
    sayf(line);
    MPEGA_close(ms);
}

static void bench_mp3(const char *path)
{
    UWORD cpu = cpu_of();
    WORD q;
    LONG fmax;
    char line[320];
    if (!exists(path)) {
        sprintf(line, "  MP3: no test file (%.100s): copy the Bench drawer beside zzprobe, or give MP3=<file>", path);
        sayf(line);
        return;
    }
    if (!(MPEGABase = OpenLibrary((CONST_STRPTR)"mpega.library", 2))) {
        sayf("  MP3: mpega.library is not installed");
        return;
    }
    /* what Ambiance asks of a 68k mpega for this CPU (Paula: up to 28 kHz) */
    q = cpu >= 40 ? 1 : 0;
    fmax = cpu >= 40 ? 28000 : cpu >= 30 ? 24000 : 12000;
    bench_mp3_one(path, "as Ambiance asks a 68k", q, cpu < 40, fmax);
    /* what it asks of the card's: stereo, best quality, the whole rate (AHI) */
    bench_mp3_one(path, "full quality (as Ambiance asks the card's)", 2, 0, 48000);
    if (mpega_card)
        sayf("  (the card decodes: these times are the 68k waiting on the card, not working)");
    CloseLibrary(MPEGABase);
    MPEGABase = NULL;
}

/* ------------------------------------------------------------ summary */
static void summary(void)
{
    heading("What Ambiance will use");
    sayf(zz_zorro ? (zz_zorro == 3 ? "  ZZ9000 found, on Zorro III" : "  ZZ9000 found, on Zorro II")
                  : "  No ZZ9000 found");
    sayf(mpega_card ? "  MP3: decoded on the card (its mpega.library), stereo, best quality"
         : mhizz ? "  MP3: the card's MHI driver can decode it (Settings > Sound > MP3: MHI, mhizz9000.library)"
         : mpega_any ? "  MP3: decoded by the 68k (install the ZZ9000 SDK's mpega.library for the card)"
         : "  MP3: no decoder");
    sayf(amissl_acc ? "  HTTPS (when Ambiance offers it): through the card's accelerated AmiSSL"
                    : "  HTTPS (when Ambiance offers it): AmiSSL as installed");
    sayf("  JPEG thumbnails: Ambiance's own decoder (jdec), at 1/2 to 1/8 when that is enough");
    if (jpeg_cls[0]) {
        char l[120];
        sprintf(l, "  JPEG at full size, and any jdec declines: %.40s.datatype", jpeg_cls);
        sayf(l);
    }
    sayf("  Pictures: whichever datatype reads them above; JPEG, PNG and WebP are decoded");
    sayf("    on the card only when the ZZ9000-JPEG/PNG/WebP descriptors are active");
    if (zz_zorro && !zzlib)
        sayf("  Note: zz9k.library is not installed: the card's SDK services are not set up");
}

/* ------------------------------------------------------------ main */
static ULONG run(void *unused)
{
    static const char tmpl[] = "JPEG/K,PNG/K,WEBP/K,MP3/K,QUICK/S,NOBENCH/S,PATHS/S,PICS/M,NODT/S,NOZZ/S,SLIDE/S";
    LONG args[11] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *ra = ReadArgs((CONST_STRPTR)tmpl, args, NULL);
    const char *jpeg = "PROGDIR:Bench/test.jpg", *png = "PROGDIR:Bench/test.png",
               *webp = "PROGDIR:Bench/test.webp", *mp3 = "PROGDIR:Bench/test.mp3";
    char line[200], date[LEN_DATSTRING], clock[LEN_DATSTRING];
    struct DateTime dt;
    BPTR fh;

    (void)version;
    (void)unused;
    if (ra) {
        if (args[0])
            jpeg = (const char *)args[0];
        if (args[1])
            png = (const char *)args[1];
        if (args[2])
            webp = (const char *)args[2];
        if (args[3])
            mp3 = (const char *)args[3];
        if (args[4]) {
            run_ms = 500;
            mp3_secs = 10;
        }
    }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&treq, 0)) {
        PutStr((CONST_STRPTR)"zzprobe: timer.device did not open\n");
        if (ra)
            FreeArgs(ra);
        return 20;
    }
    TimerBase = treq.tr_node.io_Device;
    {
        struct EClockVal e;
        efreq = ReadEClock(&e);
    }
    out = AllocVec(outmax, MEMF_ANY);
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 36);
    DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
    {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR oldwin = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;    /* no "insert volume" requesters */

        DateStamp(&dt.dat_Stamp);
        dt.dat_Format = FORMAT_INT;      /* yy-mm-dd: this NDK has no FORMAT_ISO */
        dt.dat_Flags = 0;
        dt.dat_StrDay = NULL;
        dt.dat_StrDate = (STRPTR)date;
        dt.dat_StrTime = (STRPTR)clock;
        date[0] = clock[0] = 0;
        DateToStr(&dt);
        sprintf(line, "======== zzprobe 1.25, %s %s ========", date, clock);
        sayf(line);
        machine();
        board();
        if (ra && args[6]) {            /* PATHS: the picture paths alone */
            const char *const *pics = (const char *const *)args[7];
            int n = 0;
            if (pics)
                while (pics[n])
                    n++;
            pb_zzdt = args[8] == 0;
            pb_nozz = args[9] != 0;
            if (args[10])
                pb_box = pb_box_slide;  /* SLIDE: full-screen slideshow sizes */
            if (n)
                bench_paths(pics, n);
            else
                bench_paths(&jpeg, 1);
            goto done;
        }
        software();
        datatypes(jpeg, png, webp);
        if (exists("C:zz9k-info"))
            run_tool("C:zz9k-info", "The card's own report (zz9k-info)");
        if (exists("C:zz9k-services"))
            run_tool("C:zz9k-services --all", "The card's services (zz9k-services --all)");
        if (!(ra && args[5])) {
            bench_pictures(jpeg, png, webp);
            heading("Bench: MP3 through mpega.library");
            bench_mp3(mp3);
        }
        summary();
    done:
        me->pr_WindowPtr = oldwin;
    }
    if (out && (fh = Open((CONST_STRPTR)"PROGDIR:zzprobe.txt", MODE_READWRITE))) {
        Seek(fh, 0, OFFSET_END);
        Write(fh, out, outn);
        Write(fh, (APTR)"\n", 1);
        Close(fh);
        PutStr((CONST_STRPTR)"\nAdded to zzprobe.txt beside the program\n");
    }
    if (DataTypesBase)
        CloseLibrary(DataTypesBase);
    if (ExpansionBase)
        CloseLibrary((struct Library *)ExpansionBase);
    FreeVec(out);
    CloseDevice((struct IORequest *)&treq);
    if (ra)
        FreeArgs(ra);
    return 0;
}

/* on a stack of its own: the Shell's may be 4 KB, and the datatypes
 * benched, picture.datatype and what they call all run on it */
int main(void)
{
    return (int)stack_call(65536, run, NULL);
}
