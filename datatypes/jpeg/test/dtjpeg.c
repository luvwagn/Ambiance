/*
 * dtjpeg: load a picture through datatypes, as any program does, and say
 * which datatype read it, how long it took and what came back.
 * AmigaMCE, MIT.
 *
 *   dtjpeg FILE/A,FIT/K,TIMES/N,PPM/K
 *
 * FIT=WxH   pass amcejpeg.datatype's hint (show at most... at least WxH)
 * TIMES=n   decode n times and give the average (default 1)
 * PPM=file  the pixels read back (PDTM_READPIXELARRAY, RGB) as a PPM,
 *           to compare with jdec on another machine (test/compare.py)
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/datatypes.h>
#include <stdio.h>
#include <string.h>
#include "../amcejpeg.h"
#include "../stackcall.h"

static const char __attribute__((used)) ver[] = "$VER: dtjpeg 1.0 (12.11.2026) AmigaMCE";
struct Library *DataTypesBase;

static LONG ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ds.ds_Days * 24 * 60 * 50 + ds.ds_Minute * 60 * 50 + ds.ds_Tick;
}

static ULONG run(void *unused)
{
    LONG args[4] = { 0 };
    struct RDArgs *ra;
    ULONG fw = 0, fh = 0, times = 1, i;
    Object *o = NULL;
    LONG t0, t1, err = 0;
    int rc = RETURN_FAIL;
    (void)unused;

    if (!(ra = ReadArgs("FILE/A,FIT/K,TIMES/N,PPM/K", args, NULL))) {
        PrintFault(IoErr(), "dtjpeg");
        return RETURN_FAIL;
    }
    if (args[1] && sscanf((char *)args[1], "%lux%lu", &fw, &fh) != 2) {
        printf("dtjpeg: FIT is WxH, e.g. FIT=160x120\n");
        goto out;
    }
    if (args[2] && *(LONG *)args[2] > 0)
        times = *(LONG *)args[2];
    if (!(DataTypesBase = OpenLibrary("datatypes.library", 39))) {
        printf("dtjpeg: no datatypes.library 39\n");
        goto out;
    }

    {                                   /* which datatype it goes to */
        BPTR lock = Lock((STRPTR)args[0], ACCESS_READ);
        struct DataType *dt;
        if (!lock) {
            PrintFault(IoErr(), (STRPTR)args[0]);
            goto out;
        }
        if ((dt = ObtainDataTypeA(DTST_FILE, (APTR)lock, NULL))) {
            printf("%s: %s (class %s.datatype)\n", (char *)args[0],
                   (char *)dt->dtn_Header->dth_Name, (char *)dt->dtn_Header->dth_BaseName);
            ReleaseDataType(dt);
        }
        UnLock(lock);
    }

    t0 = ticks();
    for (i = 0; i < times; i++) {
        if (o)
            DisposeDTObject(o);
        o = NewDTObject((APTR)args[0], DTA_GroupID, GID_PICTURE, PDTA_Remap, FALSE,
                        PDTA_DestMode, PMODE_V43,
                        JDTA_FitWidth, fw, JDTA_FitHeight, fh, TAG_DONE);
        if (!o) {
            err = IoErr();
            break;
        }
    }
    t1 = ticks();
    if (!o) {
        printf("dtjpeg: not loaded: ");
        PrintFault(err, NULL);
        goto out;
    }

    {
        struct BitMapHeader *bmh = NULL;
        ULONG full_w = 0, full_h = 0, scale = 0, damaged = 0;
        GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
        /* amcejpeg.datatype's own; another datatype leaves them 0 */
        GetDTAttrs(o, JDTA_FullWidth, (ULONG)&full_w, JDTA_FullHeight, (ULONG)&full_h,
                   JDTA_Scale, (ULONG)&scale, JDTA_Damaged, (ULONG)&damaged, TAG_DONE);
        if (!bmh)
            goto out;
        printf("%u x %u, depth %u", bmh->bmh_Width, bmh->bmh_Height, bmh->bmh_Depth);
        if (scale)
            printf(" (1/%lu of %lu x %lu%s)", scale, full_w, full_h, damaged ? ", damaged" : "");
        printf("\n%lu ms a decode (%lu x, %ld ticks)\n",
               (ULONG)((t1 - t0) * 20 / (LONG)times), times, (long)(t1 - t0));

        if (args[3]) {                  /* the pixels, read back */
            ULONG w = bmh->bmh_Width, h = bmh->bmh_Height, mod = w * 3;
            UBYTE *buf = AllocVec(mod * h, MEMF_ANY);
            struct pdtBlitPixelArray m;
            BPTR f;
            if (!buf) {
                printf("dtjpeg: no memory for the PPM\n");
                goto out;
            }
            m.MethodID = PDTM_READPIXELARRAY;
            m.pbpa_PixelData = buf;
            m.pbpa_PixelFormat = PBPAFMT_RGB;
            m.pbpa_PixelArrayMod = mod;
            m.pbpa_Left = m.pbpa_Top = 0;
            m.pbpa_Width = w;
            m.pbpa_Height = h;
            DoDTMethodA(o, NULL, NULL, (Msg)&m);
            if ((f = Open((STRPTR)args[3], MODE_NEWFILE))) {
                char hd[32];
                sprintf(hd, "P6\n%lu %lu\n255\n", w, h);
                Write(f, hd, strlen(hd));
                Write(f, buf, mod * h);
                Close(f);
                printf("pixels in %s\n", (char *)args[3]);
            } else
                PrintFault(IoErr(), (STRPTR)args[3]);
            FreeVec(buf);
        }
    }
    rc = RETURN_OK;
out:
    if (o)
        DisposeDTObject(o);
    if (DataTypesBase)
        CloseLibrary(DataTypesBase);
    FreeArgs(ra);
    return (ULONG)rc;
}

/* on a stack of its own: the Shell's may be 4 KB, and a datatype,
 * picture.datatype and what they call all run on it */
int main(void)
{
    return (int)stack_call(65536, run, NULL);
}
