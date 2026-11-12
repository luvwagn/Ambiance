/*
 * amcejpeg.datatype: JPEG pictures for 68020+ Amigas, a picture.datatype
 * (V43+) subclass on jdec (jdec.c). Copyright (c) 2026 Brian King, MIT
 *
 * A shared library like any class library: its one function,
 * ObtainEngine(), gives datatypes.library the class. The picture is
 * decoded in OM_NEW, a band of rows at a time, each given to
 * picture.datatype as RGB (PDTM_WRITEPIXELARRAY); picture.datatype then
 * does what it does with any deep picture (remapping, dithering, RTG).
 *
 * The tags in amcejpeg.h ask for a smaller decode (1/2, 1/4, 1/8) where
 * the picture will be shown smaller anyway: thumbnails, mainly.
 *
 * Built with libnix's libinit (src/sqlib/libinit.c), as amcesql.library
 * is: the library's data is shared by every opener, so nothing here is
 * kept between calls but the library bases and the class. Each object's
 * decoding has its own memory (jdec allocates through AllocVec), so any
 * number of tasks can load pictures at once.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <devices/timer.h>
#include <intuition/intuitionbase.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/datatypes.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>
#include <string.h>
#include "stabs.h"
#include "jdec.h"
#include "amcejpeg.h"
#include "stackcall.h"

#ifndef DTERROR_UNKNOWN_COMPRESSION
#define DTERROR_UNKNOWN_COMPRESSION DTERROR_INVALID_DATA
#endif

#ifndef DT_DATE
#define DT_DATE "12.11.2026"
#endif

const char LibName[] = "amcejpeg.datatype";
const char LibIdString[] = "amcejpeg.datatype 46.0 (" DT_DATE ") JPEG for 68020+, MIT";
const UWORD LibVersion = 46;
const UWORD LibRevision = 0;
static const char __attribute__((used)) verstag[] =
    "$VER: amcejpeg.datatype 46.0 (" DT_DATE ") JPEG for 68020+, MIT";

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
struct Library *DataTypesBase;
static struct Library *PictureBase;     /* held open: our superclass */
static Class *jclass;
struct Device *TimerBase;               /* the E-clock, for JDTA_Time*; NULL: none */
static struct timerequest treq;
static ULONG efreq_k;                   /* E-clock ticks a millisecond */

static ULONG eclock(void)
{
    struct EClockVal e;
    if (!TimerBase)
        return 0;
    ReadEClock(&e);
    return e.ev_lo;
}

static ULONG ticks_us(ULONG t)          /* to 4 s or so */
{
    return efreq_k ? t * 1000 / efreq_k : 0;
}

/* each object's own */
struct Inst {
    ULONG fullw, fullh;                 /* the picture's size */
    ULONG scale;                        /* 1, 2, 4, 8 */
    ULONG damaged;
    ULONG us_load, us_write;            /* JDTA_TimeLoad, JDTA_TimeWrite */
    ULONG us_headers, us_setattrs;      /* JDTA_TimeHeaders, JDTA_TimeSetAttrs */
    ULONG us_nominal, us_read;          /* JDTA_TimeNominal, JDTA_TimeRead */
};

/* ---- jdec's input, memory and output */

struct Src {
    Class *cl;
    Object *o;
    BPTR fh;                            /* a file (DTST_FILE), or */
    const UBYTE *mem;                   /* memory (DTST_MEMORY) */
    ULONG memlen, mempos;
    LONG ioerr;
    ULONG w;                            /* rows' width */
    ULONG t_write;                      /* E-clock ticks in WRITEPIXELARRAY */
    UBYTE *whole;                       /* the file read whole, to free */
};

static int32_t src_read(void *ctx, uint8_t *buf, int32_t n)
{
    struct Src *s = ctx;
    LONG r;
    if (s->mem) {
        ULONG left = s->memlen - s->mempos;
        if ((ULONG)n > left)
            n = (int32_t)left;
        if (n > 0)
            CopyMem((APTR)(s->mem + s->mempos), buf, (ULONG)n);
        s->mempos += (ULONG)n;
        return n;
    }
    r = Read(s->fh, buf, n);
    if (r < 0) {
        s->ioerr = IoErr();
        return -1;
    }
    return (int32_t)r;
}

static void *src_alloc(void *ctx, uint32_t n)
{
    (void)ctx;
    return AllocVec(n, MEMF_ANY);
}

static void src_free(void *ctx, void *p)
{
    (void)ctx;
    FreeVec(p);
}

static int src_rows(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride)
{
    struct Src *s = ctx;
    struct pdtBlitPixelArray m;
    m.MethodID = PDTM_WRITEPIXELARRAY;
    m.pbpa_PixelData = (APTR)rgb;
    m.pbpa_PixelFormat = PBPAFMT_RGB;
    m.pbpa_PixelArrayMod = (ULONG)stride;
    m.pbpa_Left = 0;
    m.pbpa_Top = (ULONG)y;
    m.pbpa_Width = s->w;
    m.pbpa_Height = (ULONG)n;
    {
        ULONG t0 = eclock();
        DoSuperMethodA(s->cl, s->o, (Msg)&m);
        s->t_write += eclock() - t0;
    }
    return 0;
}

/* A tag's value in a list (utility.library's GetTagData, without it) */
static ULONG tag_data(struct TagItem *t, Tag id, ULONG def)
{
    while (t) {
        switch (t->ti_Tag) {
        case TAG_DONE:
            return def;
        case TAG_MORE:
            t = (struct TagItem *)t->ti_Data;
            continue;
        case TAG_IGNORE:
            break;
        case TAG_SKIP:
            t += t->ti_Data;
            break;
        default:
            if (t->ti_Tag == id)
                return t->ti_Data;
        }
        t++;
    }
    return def;
}

/* jdec's error as DOS's (IoErr) */
static LONG dos_err(int r, const struct Src *s)
{
    switch (r) {
    case JD_ENOMEM:
    case JD_ETOOBIG:
        return ERROR_NO_FREE_STORE;
    case JD_EREAD:
        return s->ioerr ? s->ioerr : DTERROR_NOT_ENOUGH_DATA;
    case JD_EPROGRESSIVE:
    case JD_ECMYK:
    case JD_EUNSUPPORTED:
        return DTERROR_UNKNOWN_COMPRESSION;
    default:
        return DTERROR_INVALID_DATA;
    }
}

/* The picture into a new object; 0, or why not (for IoErr) */
static LONG load(Class *cl, Object *o, struct TagItem *attrs)
{
    struct Inst *in = INST_DATA(cl, o);
    struct BitMapHeader *bmh = NULL;
    ULONG st = DTST_FILE, xa, ya;
    struct Src s;
    JDecIO io;
    JDecInfo info;
    JDec *d;
    int32_t w, h;
    int r, scale;
    ULONG t0 = eclock();

    memset(&s, 0, sizeof(s));
    s.cl = cl;
    s.o = o;
    GetDTAttrs(o, DTA_SourceType, (ULONG)&st, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
    if (!bmh)
        return ERROR_NO_FREE_STORE;
    if (st == DTST_FILE) {
        LONG n;
        GetDTAttrs(o, DTA_Handle, (ULONG)&s.fh, TAG_DONE);
        if (!s.fh)
            return ERROR_REQUIRED_ARG_MISSING;
        /* a file up to 4 MB read whole, in one Read: much less filesystem
         * work than jdec's 4 KB at a time. Bigger ones, or without the
         * memory, streamed. */
        Seek(s.fh, 0, OFFSET_END);
        n = Seek(s.fh, 0, OFFSET_BEGINNING);
        if (n > 0 && n <= 4L << 20 && (s.whole = AllocVec((ULONG)n, MEMF_ANY))) {
            ULONG tr = eclock();
            if (Read(s.fh, s.whole, n) == n) {
                s.mem = s.whole;
                s.memlen = (ULONG)n;
            } else {
                Seek(s.fh, 0, OFFSET_BEGINNING);
                FreeVec(s.whole);
                s.whole = NULL;
            }
            in->us_read = ticks_us(eclock() - tr);
        }
#if defined(DTST_MEMORY) && defined(DTA_SourceAddress) && defined(DTA_SourceSize)
    } else if (st == DTST_MEMORY) {
        APTR a = NULL;
        ULONG n = 0;
        GetDTAttrs(o, DTA_SourceAddress, (ULONG)&a, DTA_SourceSize, (ULONG)&n, TAG_DONE);
        if (!a || !n)
            return ERROR_REQUIRED_ARG_MISSING;
        s.mem = a;
        s.memlen = n;
#endif
    } else {
        return ERROR_OBJECT_WRONG_TYPE;  /* clipboard: no JPEGs there */
    }

    io.ctx = &s;
    io.read = src_read;
    io.alloc = src_alloc;
    io.free = src_free;
    io.rows = src_rows;
    {
        ULONG th = eclock();
        r = jd_open(&d, &io, &info);
        in->us_headers = ticks_us(eclock() - th);
    }
    if (r) {
        FreeVec(s.whole);
        return dos_err(r, &s);
    }

    scale = jd_scale_for(&info, (int32_t)tag_data(attrs, JDTA_FitWidth, 0),
                         (int32_t)tag_data(attrs, JDTA_FitHeight, 0));
    jd_out_size(&info, scale, &w, &h);
    s.w = (ULONG)w;

    xa = (ULONG)info.xdensity;          /* pixel shape, from JFIF */
    ya = (ULONG)info.ydensity;
    if (!xa || !ya || xa == ya)
        xa = ya = 1;
    while (xa > 255 || ya > 255) {
        xa = (xa + 1) >> 1;
        ya = (ya + 1) >> 1;
    }
    bmh->bmh_Left = bmh->bmh_Top = 0;
    bmh->bmh_Width = bmh->bmh_PageWidth = (UWORD)w;
    bmh->bmh_Height = bmh->bmh_PageHeight = (UWORD)h;
    bmh->bmh_Depth = 24;
    bmh->bmh_XAspect = (UBYTE)xa;
    bmh->bmh_YAspect = (UBYTE)ya;
    {
        /* PMODE_V43 was given with OM_NEW (dispatch); set here only if
         * picture.datatype did not take it there. The nominal size by
         * OM_SET to picture.datatype itself, not SetDTAttrs: that went
         * through datatypes.library and took 8-10 ms on OS 3.2 (68030). */
        ULONG mode = 0, ts = eclock();
        struct TagItem nt[3];
        struct opSet os;
        GetDTAttrs(o, PDTA_SourceMode, (ULONG)&mode, TAG_DONE);
        if (mode != PMODE_V43)
            SetDTAttrs(o, NULL, NULL, PDTA_SourceMode, PMODE_V43, TAG_DONE);
        in->us_setattrs = ticks_us(eclock() - ts);
        ts = eclock();
        nt[0].ti_Tag = DTA_NominalHoriz;
        nt[0].ti_Data = (ULONG)w;
        nt[1].ti_Tag = DTA_NominalVert;
        nt[1].ti_Data = (ULONG)h;
        nt[2].ti_Tag = TAG_DONE;
        os.MethodID = OM_SET;
        os.ops_AttrList = nt;
        os.ops_GInfo = NULL;
        DoSuperMethodA(cl, o, (Msg)&os);
        in->us_nominal = ticks_us(eclock() - ts);
    }

    r = jd_decode(d, scale);
    in->damaged = jd_damaged(d) ? TRUE : FALSE;
    jd_close(d);
    FreeVec(s.whole);
    in->us_load = ticks_us(eclock() - t0);
    in->us_write = ticks_us(s.t_write);
    if (r)
        return dos_err(r, &s);
    in->fullw = (ULONG)info.width;
    in->fullh = (ULONG)info.height;
    in->scale = 1UL << scale;
    return 0;
}

/* load() on a stack of its own: a datatype runs on its caller's, which
 * may be a Shell's 4 KB, and picture.datatype is called from inside */
#define LOAD_STACK 24576
struct LoadArgs {
    Class *cl;
    Object *o;
    struct TagItem *attrs;
};

static ULONG load_entry(void *p)
{
    struct LoadArgs *a = p;
    return (ULONG)load(a->cl, a->o, a->attrs);
}

static ULONG get(Class *cl, Object *o, struct opGet *m)
{
    struct Inst *in = INST_DATA(cl, o);
    ULONG v;
    switch (m->opg_AttrID) {
    case JDTA_FullWidth: v = in->fullw; break;
    case JDTA_FullHeight: v = in->fullh; break;
    case JDTA_Scale: v = in->scale; break;
    case JDTA_Damaged: v = in->damaged; break;
    case JDTA_TimeLoad: v = in->us_load; break;
    case JDTA_TimeWrite: v = in->us_write; break;
    case JDTA_TimeHeaders: v = in->us_headers; break;
    case JDTA_TimeSetAttrs: v = in->us_setattrs; break;
    case JDTA_TimeNominal: v = in->us_nominal; break;
    case JDTA_TimeRead: v = in->us_read; break;
    default:
        return DoSuperMethodA(cl, o, (Msg)m);
    }
    *m->opg_Storage = v;
    return TRUE;
}

static ULONG dispatch(register Class *cl __asm("a0"), register Object *o __asm("a2"),
                      register Msg msg __asm("a1"))
{
    switch (msg->MethodID) {
    case OM_NEW: {
        /* picture.datatype told PMODE_V43 at once, with the caller's tags */
        struct TagItem t[2];
        struct opSet ns = *(struct opSet *)msg;
        Object *obj;
        t[0].ti_Tag = PDTA_SourceMode;
        t[0].ti_Data = PMODE_V43;
        t[1].ti_Tag = TAG_MORE;
        t[1].ti_Data = (ULONG)((struct opSet *)msg)->ops_AttrList;
        ns.ops_AttrList = t;
        obj = (Object *)DoSuperMethodA(cl, o, (Msg)&ns);
        if (obj) {
            struct LoadArgs la;
            LONG err;
            la.cl = cl;
            la.o = obj;
            la.attrs = ((struct opSet *)msg)->ops_AttrList;
            err = (LONG)stack_call(LOAD_STACK, load_entry, &la);
            if (err) {
                ULONG dispose = OM_DISPOSE;
                CoerceMethodA(cl, obj, (Msg)&dispose);
                SetIoErr(err);
                obj = NULL;
            }
        }
        return (ULONG)obj;
    }
    case OM_GET:
        return get(cl, o, (struct opGet *)msg);
    }
    return DoSuperMethodA(cl, o, msg);
}

/* ---- the library */

Class *ObtainEngine(void)
{
    return jclass;
}

void __UserLibCleanup(void);

/* Why the library would not start, in ENV:amcejpeg.datatype (zzprobe
 * shows it): a library has nowhere else to say so. Gone once it starts. */
#define WHY_VAR "amcejpeg.datatype"
static void why_not(const char *s)
{
    if (DOSBase)
        SetVar((STRPTR)WHY_VAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY);
}

int __UserLibInit(struct Library *lib)
{
    const char *why = NULL;
    (void)lib;
    SysBase = *(struct ExecBase *volatile *)4L;
    if (!(DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39)))
        why = "no dos.library 39";
    else if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39)))
        why = "no intuition.library 39";
    else if (!(DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39)))
        why = "no datatypes.library 39";
    /* V43: deep pictures (PMODE_V43, PDTM_WRITEPIXELARRAY) */
    else if (!(PictureBase = OpenLibrary((CONST_STRPTR)"datatypes/picture.datatype", 43)))
        why = "no datatypes/picture.datatype 43 or newer";
    else if (!(jclass = MakeClass((ClassID)LibName, (ClassID)PICTUREDTCLASS, NULL, sizeof(struct Inst), 0)))
        why = "MakeClass failed (no public picture.datatype class?)";
    if (why) {
        why_not(why);
        __UserLibCleanup();
        return 1;
    }
    DeleteVar((STRPTR)WHY_VAR, GVF_GLOBAL_ONLY);
    if (!OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&treq, 0)) {
        struct EClockVal e;
        TimerBase = treq.tr_node.io_Device;
        efreq_k = ReadEClock(&e) / 1000;
    }
    jclass->cl_Dispatcher.h_Entry = (HOOKFUNC)dispatch;
    jclass->cl_UserData = (ULONG)lib;
    AddClass(jclass);
    return 0;
}

void __UserLibCleanup(void)
{
    if (TimerBase) {
        CloseDevice((struct IORequest *)&treq);
        TimerBase = NULL;
    }
    if (jclass) {
        RemoveClass(jclass);
        FreeClass(jclass);
        jclass = NULL;
    }
    if (PictureBase)
        CloseLibrary(PictureBase);
    if (DataTypesBase)
        CloseLibrary(DataTypesBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
    PictureBase = DataTypesBase = NULL;
    IntuitionBase = NULL;
    DOSBase = NULL;
}

/* the jump table: ObtainEngine at -30, as every datatype class library */
ADDTABL_0(ObtainEngine);
ADDTABL_END();
