/*
 * zzprobe - what a ZZ9000 offers this Amiga, and how fast, as AmigaMCE uses it
 * Copyright (c) 2026 Brian King, MIT
 *
 *   zzprobe [JPEG file] [PNG file] [WEBP file] [MP3 file] [QUICK] [NOBENCH]
 *
 * Probes, without the ZZ9000 SDK's headers (they are GPL-3; nothing of
 * theirs is compiled in):
 *   - the machine, and the ZZ9000 board (Zorro II or III) by its IDs;
 *   - zz9k.library, mpega.library (the SDK's, decoding on the card, or the
 *     68k one), MHI drivers, AHI and the ZZ9000AX driver, AmiSSL (and the
 *     card's accelerated replacement), Picasso96 and CyberGraphX, the RTG
 *     display modes by monitor, the SDK's commands and ZZPlay;
 *   - the ZZ9000 datatype descriptors (active in DEVS:DataTypes, or
 *     installed inactive in SYS:Storage/DataTypes), and which datatype
 *     reads each picture and sound format here;
 *   - the card's own report: the SDK's zz9k-info and zz9k-services, run.
 * Then times what AmigaMCE does with them:
 *   - pictures decoded through datatypes (whichever reads them now), and
 *     their pixels read back, as AmigaMCE's previews do;
 *   - MP3 decoded by mpega.library, as AmigaMCE asks a 68k for this CPU,
 *     and at full quality (what it asks of the card's).
 * Default test files: PROGDIR:Bench/test.jpg, test.png, test.webp, test.mp3.
 * The report goes to the Shell, and is added to PROGDIR:zzprobe.txt with
 * the date, so runs before and after a change sit side by side.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/datetime.h>
#include <dos/rdargs.h>
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
#include <clib/alib_protos.h>
#include <string.h>
#include <stdio.h>

static const char version[] = "$VER: zzprobe 1.0 (11.11.2026)";

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
                sprintf(line, "  MHI driver: %s", fib->fib_FileName);
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
        sprintf(line, "  RTG modes: %ld from \"%s\"", (long)counts[i], names[i]);
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
        sprintf(line, "  %-5s %s.datatype (descriptor %s)%s", fmt, base,
                dtn->dtn_Header->dth_Name ? (const char *)dtn->dtn_Header->dth_Name : "?",
                !strncmp(base, "zz9k", 4) ? "  <- on the ZZ9000" : "");
    }
    if (dtn)
        ReleaseDataType(dtn);
    sayf(line);
}

static void datatypes(void)
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

/* ------------------------------------------------------------ bench: pictures */
static void bench_picture(const char *path, const char *fmt)
{
    char line[220], t1[24], t2[24], t3[24];
    ULONG best = 0xFFFFFFFF, worst = 0, sum = 0, runs = 0, start, rb_best = 0xFFFFFFFF;
    ULONG w = 0, h = 0;
    UWORD depth = 0;
    char base[40];
    base[0] = 0;
    if (!exists(path)) {
        sprintf(line, "  %s: no test file (%s)", fmt, path);
        sayf(line);
        return;
    }
    start = now_us();
    do {
        ULONG t0 = now_us(), dt;
        Object *o = NewDTObject((APTR)path, DTA_GroupID, GID_PICTURE, PDTA_Remap, FALSE,
                                PDTA_DestMode, PMODE_V43, TAG_END);
        struct BitMapHeader *bmh = NULL;
        struct DataType *dtn = NULL;
        if (!o) {
            sprintf(line, "  %s: %s could not be read by any datatype", fmt, path);
            sayf(line);
            return;
        }
        DoDTMethod(o, NULL, NULL, DTM_PROCLAYOUT, NULL, (ULONG)TRUE);
        dt = now_us() - t0;
        GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, DTA_DataType, (ULONG)&dtn, TAG_END);
        if (bmh) {
            w = bmh->bmh_Width;
            h = bmh->bmh_Height;
            depth = bmh->bmh_Depth;
        }
        if (dtn && dtn->dtn_Header && dtn->dtn_Header->dth_BaseName) {
            strncpy(base, (const char *)dtn->dtn_Header->dth_BaseName, sizeof(base) - 1);
            base[sizeof(base) - 1] = 0;
        }
        /* its pixels read back as RGB, as AmigaMCE's previews read them */
        if (w && h) {
            UBYTE *buf = AllocVec(w * 3 * 16, MEMF_ANY);
            if (buf) {
                ULONG r0 = now_us(), y;
                struct pdtBlitPixelArray pb;
                for (y = 0; y < h; y += 16) {
                    pb.MethodID = PDTM_READPIXELARRAY;
                    pb.pbpa_PixelData = buf;
                    pb.pbpa_PixelFormat = PBPAFMT_RGB;
                    pb.pbpa_PixelArrayMod = w * 3;
                    pb.pbpa_Left = 0;
                    pb.pbpa_Top = y;
                    pb.pbpa_Width = w;
                    pb.pbpa_Height = h - y < 16 ? h - y : 16;
                    DoMethodA(o, (Msg)&pb);
                }
                r0 = now_us() - r0;
                if (r0 < rb_best)
                    rb_best = r0;
                FreeVec(buf);
            }
        }
        DisposeDTObject(o);
        if (dt < best)
            best = dt;
        if (dt > worst)
            worst = dt;
        sum += dt;
        runs++;
    } while (runs < 10 && now_us() - start < run_ms * 1000);
    fmt_ms(t1, best);
    fmt_ms(t2, sum / runs);
    fmt_ms(t3, rb_best == 0xFFFFFFFF ? 0 : rb_best);
    sprintf(line, "  %s %ld x %ld x %ld by %s.datatype%s: decode best %s, mean %s (%ld runs); "
                  "pixels read back %s", fmt, (long)w, (long)h, (long)depth, base[0] ? base : "?",
            !strncmp(base, "zz9k", 4) ? " (ZZ9000)" : "", t1, t2, (long)runs, t3);
    sayf(line);
}

/* ------------------------------------------------------------ bench: MP3 by mpega.library */
/* mpega.library has no C header here; its structures and calls as in
 * AmigaMCE's stream.c, from its documentation. */
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
        sprintf(line, "  %s: mpega.library could not open %s", label, path);
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
    char line[200];
    if (!exists(path)) {
        sprintf(line, "  MP3: no test file (%s)", path);
        sayf(line);
        return;
    }
    if (!(MPEGABase = OpenLibrary((CONST_STRPTR)"mpega.library", 2))) {
        sayf("  MP3: mpega.library is not installed");
        return;
    }
    /* what AmigaMCE asks of a 68k mpega for this CPU (Paula: up to 28 kHz) */
    q = cpu >= 40 ? 1 : 0;
    fmax = cpu >= 40 ? 28000 : cpu >= 30 ? 24000 : 12000;
    bench_mp3_one(path, "as AmigaMCE asks a 68k", q, cpu < 40, fmax);
    /* what it asks of the card's: stereo, best quality, the whole rate (AHI) */
    bench_mp3_one(path, "full quality (as AmigaMCE asks the card's)", 2, 0, 48000);
    if (mpega_card)
        sayf("  (the card decodes: these times are the 68k waiting on the card, not working)");
    CloseLibrary(MPEGABase);
    MPEGABase = NULL;
}

/* ------------------------------------------------------------ summary */
static void summary(void)
{
    heading("What AmigaMCE will use");
    sayf(zz_zorro ? (zz_zorro == 3 ? "  ZZ9000 found, on Zorro III" : "  ZZ9000 found, on Zorro II")
                  : "  No ZZ9000 found");
    sayf(mpega_card ? "  MP3: decoded on the card (its mpega.library), stereo, best quality"
         : mhizz ? "  MP3: the card's MHI driver can decode it (Settings > Sound > MP3: MHI, mhizz9000.library)"
         : mpega_any ? "  MP3: decoded by the 68k (install the ZZ9000 SDK's mpega.library for the card)"
         : "  MP3: no decoder");
    sayf(amissl_acc ? "  HTTPS (when AmigaMCE offers it): through the card's accelerated AmiSSL"
                    : "  HTTPS (when AmigaMCE offers it): AmiSSL as installed");
    sayf("  Pictures: whichever datatype reads them above; JPEG, PNG and WebP are decoded");
    sayf("    on the card only when the ZZ9000-JPEG/PNG/WebP descriptors are active");
    if (zz_zorro && !zzlib)
        sayf("  Note: zz9k.library is not installed: the card's SDK services are not set up");
}

/* ------------------------------------------------------------ main */
int main(void)
{
    static const char tmpl[] = "JPEG/K,PNG/K,WEBP/K,MP3/K,QUICK/S,NOBENCH/S";
    LONG args[6] = { 0, 0, 0, 0, 0, 0 };
    struct RDArgs *ra = ReadArgs((CONST_STRPTR)tmpl, args, NULL);
    const char *jpeg = "PROGDIR:Bench/test.jpg", *png = "PROGDIR:Bench/test.png",
               *webp = "PROGDIR:Bench/test.webp", *mp3 = "PROGDIR:Bench/test.mp3";
    char line[200], date[LEN_DATSTRING], clock[LEN_DATSTRING];
    struct DateTime dt;
    BPTR fh;

    (void)version;
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
        dt.dat_Format = FORMAT_ISO;
        dt.dat_Flags = 0;
        dt.dat_StrDay = NULL;
        dt.dat_StrDate = (STRPTR)date;
        dt.dat_StrTime = (STRPTR)clock;
        date[0] = clock[0] = 0;
        DateToStr(&dt);
        sprintf(line, "======== zzprobe 1.0, %s %s ========", date, clock);
        sayf(line);
        machine();
        board();
        software();
        datatypes();
        if (exists("C:zz9k-info"))
            run_tool("C:zz9k-info", "The card's own report (zz9k-info)");
        if (exists("C:zz9k-services"))
            run_tool("C:zz9k-services --all", "The card's services (zz9k-services --all)");
        if (!(ra && args[5])) {
            heading("Bench: pictures through datatypes (decode, then pixels read back as RGB)");
            bench_picture(jpeg, "JPEG");
            bench_picture(png, "PNG");
            bench_picture(webp, "WebP");
            heading("Bench: MP3 through mpega.library");
            bench_mp3(mp3);
        }
        summary();
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
