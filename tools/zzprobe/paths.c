/*
 * zzprobe - picture paths, end to end (included by zzprobe.c)
 * Copyright (c) 2026 Brian King, MIT
 *
 *   zzprobe PATHS [NODT] [NOZZ] [PICS file file ...]
 *
 * Each JPEG decoded every way Ambiance could, from the file to its pixels
 * at the size shown (fitted in the box, its shape kept), timed the same way
 * as the rest of the bench (best of up to 10 runs, mean in brackets):
 *
 *   own        jdec at the smallest of 1/1..1/8 still at least the size,
 *              then averaged down on the 68k (as Ambiance's thumbnails and
 *              viewer do): the file read, decoded, made small, in fast RAM
 *   zz9k dt    zz9k-picture.datatype's class (decoding on the card), its
 *              object from the file, the whole picture read back
 *              (PDTM_READPIXELARRAY), then averaged down on the 68k: what
 *              Ambiance's datatype route costs with the card's datatype
 *   jfif dt    the same through jfif.datatype: today's route for what jdec
 *              declines, for scale
 *   API back   zz9k.library: the file read into a shared buffer on the
 *              card, ZZ9KDecodeJpeg fitting it (libjpeg's 1/2..1/8, then the
 *              card's scaler) into a 68k-visible surface the size shown,
 *              read back into fast RAM
 *   API card   the same into a surface on the card alone: nothing read
 *              back (what drawing straight into the screen would cost,
 *              less the blit itself)
 *   API tiles  an image session in tile mode: the picture decoded on the
 *              card (whole: it does not scale in tile mode), its BGRA tiles
 *              copied out of a small shared buffer as each comes, then
 *              averaged down on the 68k. No surface: what Zorro II and the
 *              card's own datatype can use.
 *
 * Amiberry's emulated card (zz9k.library 2.33) offers no surfaces (caps:
 * max_surfaces 0, host-window heap 0: every ZZ9KAllocSurface "unsupported",
 * as the surface probe lists), so its API back and API card columns stay
 * empty there; a real card has its own numbers.
 *
 * The pixels are checked: the API's and the datatype's against own's (the
 * mean difference of each colour, 0-255), so a wrong channel order or a
 * failed decode shows. On Amiberry the card's work runs on the PC: its
 * times say nothing about a real ZZ9000's; the 68k's parts (reading back,
 * averaging) are still the emulated 68k's.
 */
#include "proto/zz9k.h"
#include "zz9k/surface.h"

struct Library *ZZ9KBase;
static BOOL pb_zzdt = TRUE;             /* the datatypes' columns (NODT: left out) */
static BOOL pb_nozz;                    /* NOZZ: zz9k.library left alone (no API columns) */

#define PB_N 6
enum { PB_OWN, PB_ZZDT, PB_JFIF, PB_APIBACK, PB_APICARD, PB_APITILE };
static const char *const pb_name[PB_N] = { "own", "zz9k dt", "jfif dt", "API back", "API card", "API tiles" };

struct Box {
    const char *name;
    UWORD w, h;
};
static const struct Box pb_box[] = { { "tile", 160, 160 }, { "screen", 640, 512 }, { "HD", 1280, 720 } };
#define PB_BOXES (sizeof(pb_box) / sizeof(pb_box[0]))

/* the picture fitted in w x h, its shape kept, never made larger */
static void pb_fit(ULONG sw, ULONG sh, ULONG w, ULONG h, ULONG *dw, ULONG *dh)
{
    if (sw <= w && sh <= h) {
        *dw = sw;
        *dh = sh;
        return;
    }
    *dw = w;
    *dh = sh * w / sw;
    if (*dh > h) {
        *dh = h;
        *dw = sw * h / sh;
    }
    if (!*dw)
        *dw = 1;
    if (!*dh)
        *dh = 1;
}

/* src (sw x sh, 0xAARRGGBB) averaged down into dst (dw x dh): each small
 * pixel the mean of the big ones under it (a box), as thumb.c does */
static void pb_shrink(const ULONG *src, ULONG sw, ULONG sh, ULONG *dst, ULONG dw, ULONG dh)
{
    ULONG x, y, i, j;
    for (y = 0; y < dh; y++) {
        ULONG y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (x = 0; x < dw; x++) {
            ULONG x0 = x * sw / dw, x1 = (x + 1) * sw / dw, r = 0, g = 0, b = 0, n;
            if (x1 <= x0)
                x1 = x0 + 1;
            for (j = y0; j < y1; j++) {
                const ULONG *p = src + j * sw;
                for (i = x0; i < x1; i++) {
                    ULONG c = p[i];
                    r += (c >> 16) & 255;
                    g += (c >> 8) & 255;
                    b += c & 255;
                }
            }
            n = (x1 - x0) * (y1 - y0);
            dst[y * dw + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
}

/* ---- own: jdec */
struct PbOwn {
    const char *path;
    ULONG w, h;                 /* the box */
    ULONG *out;                 /* dw x dh */
    ULONG dw, dh, iw, ih;       /* made; the picture's own size */
    ULONG *big;                 /* decoded, at its scale */
    ULONG bw, bh;
    int err;
};

/* jdec's one context: its reader's (the file in memory) and the rows' */
struct PbOwnIo {
    struct MemSrc m;
    struct PbOwn *c;
};

static int32_t pb_mread(void *ctx, uint8_t *buf, int32_t n)
{
    return mem_read(&((struct PbOwnIo *)ctx)->m, buf, n);
}

static int pb_rows(void *ctx, const uint8_t *rgb, int32_t y, int32_t n, int32_t stride)
{
    struct PbOwn *c = ((struct PbOwnIo *)ctx)->c;
    int32_t i;
    ULONG x;
    for (i = 0; i < n && (ULONG)(y + i) < c->bh; i++) {
        const uint8_t *s = rgb + i * stride;
        ULONG *d = c->big + (ULONG)(y + i) * c->bw;
        for (x = 0; x < c->bw; x++, s += 3)
            d[x] = ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
    }
    return 0;
}

static LONG pb_own_once(void *p)
{
    struct PbOwn *c = p;
    struct PbOwnIo m;
    JDecIO io;
    JDecInfo info;
    JDec *d = NULL;
    LONG len = file_size(c->path);
    UBYTE *file;
    int32_t ow, oh;
    int r, s;
    if (len <= 0 || !(file = AllocVec((ULONG)len, MEMF_ANY)))
        return -1;
    if (!file_read(c->path, file, len)) {
        FreeVec(file);
        return -1;
    }
    m.m.p = file;
    m.m.n = (ULONG)len;
    m.m.pos = 0;
    m.c = c;
    io.ctx = &m;
    io.read = pb_mread;
    io.alloc = mem_alloc;
    io.free = mem_free;
    io.rows = pb_rows;
    if ((r = jd_open(&d, &io, &info))) {
        c->err = r;
        FreeVec(file);
        return -1;
    }
    c->iw = (ULONG)info.width;
    c->ih = (ULONG)info.height;
    pb_fit(c->iw, c->ih, c->w, c->h, &c->dw, &c->dh);
    s = jd_scale_for(&info, (int32_t)c->dw, (int32_t)c->dh);
    jd_out_size(&info, s, &ow, &oh);
    c->bw = (ULONG)ow;
    c->bh = (ULONG)oh;
    if (!(c->big = AllocVec(c->bw * c->bh * 4, MEMF_ANY))) {
        jd_close(d);
        FreeVec(file);
        return -1;
    }
    r = jd_decode(d, s);
    jd_close(d);
    FreeVec(file);
    if (r) {
        c->err = r;
        FreeVec(c->big);
        c->big = NULL;
        return -1;
    }
    FreeVec(c->out);
    if ((c->out = AllocVec(c->dw * c->dh * 4, MEMF_ANY)))
        pb_shrink(c->big, c->bw, c->bh, c->out, c->dw, c->dh);
    FreeVec(c->big);
    c->big = NULL;
    return c->out ? 0 : -1;
}

/* ---- a datatype's class: its object from the file, read back whole, made small */
struct PbDt {
    Class *cl;
    struct DataType *dtn;
    const char *path;
    ULONG w, h;
    ULONG *out, dw, dh, iw, ih;
    LONG err;
};

static LONG pb_dt_once(void *p)
{
    struct PbDt *c = p;
    struct BitMapHeader *bmh = NULL;
    struct pdtBlitPixelArray pa;
    ULONG *big;
    Object *o;
    /* DTA_Handle for DTST_FILE is a lock (datatypesclass Examine()s it and
     * opens the file from it), as NewDTObject passes, and the object's
     * from then on. A file handle here (as cls_once had it) was Examine()d
     * as a lock: DOS sent its packet to a nonsense port, PutMsg "called"
     * the port (PA 3) into the illegal-instruction vector, and SmartCrash's
     * trap handler, run in user mode, faulted (Exception 8, privilege
     * violation): every "hang" was SmartCrash's requester waiting */
    BPTR fh = Lock((CONST_STRPTR)c->path, SHARED_LOCK);
    if (!fh)
        return IoErr() ? IoErr() : -1;
    o = NewObject(c->cl, NULL, DTA_SourceType, DTST_FILE, DTA_Handle, (ULONG)fh,
                  DTA_DataType, (ULONG)c->dtn, DTA_Name, (ULONG)c->path,
                  PDTA_Remap, FALSE, PDTA_DestMode, PMODE_V43, TAG_END);
    if (!o) {
        LONG e = IoErr();
        return e ? e : -1;
    }
    /* no layout: a deep picture's pixels are read from the object as RGB,
     * as Ambiance's thumbnails do (gfx.c pic_decode_fit); laid out, PMODE_V43,
     * READPIXELARRAY failed */
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_END);

    if (!bmh || !bmh->bmh_Width || !bmh->bmh_Height) {
        DisposeObject(o);
        return -1;
    }
    c->iw = bmh->bmh_Width;
    c->ih = bmh->bmh_Height;
    pb_fit(c->iw, c->ih, c->w, c->h, &c->dw, &c->dh);
    if (!(big = AllocVec(c->iw * c->ih * 4, MEMF_ANY))) {
        DisposeObject(o);
        return ERROR_NO_FREE_STORE;
    }
    {
        /* as Ambiance's thumb.c reads them: RGB, a band of rows at a time
         * (as thumb.c's read_rows) */
        ULONG top, n, x, band = 32;
        UBYTE *rgb = AllocVec(c->iw * 3 * band, MEMF_ANY);
        if (!rgb) {
            FreeVec(big);
            DisposeObject(o);
            return ERROR_NO_FREE_STORE;
        }
        for (top = 0; top < c->ih; top += n) {
            ULONG k;
            n = c->ih - top < band ? c->ih - top : band;
            memset(&pa, 0, sizeof(pa));
            pa.MethodID = PDTM_READPIXELARRAY;
            pa.pbpa_PixelData = rgb;
            pa.pbpa_PixelFormat = PBPAFMT_RGB;
            pa.pbpa_PixelArrayMod = c->iw * 3;
            pa.pbpa_Left = 0;
            pa.pbpa_Top = top;
            pa.pbpa_Width = c->iw;
            pa.pbpa_Height = n;
            if (!DoMethodA(o, (Msg)&pa)) {
                FreeVec(rgb);
                FreeVec(big);
                DisposeObject(o);
                return -1;
            }
            for (k = 0; k < n; k++) {
                const UBYTE *s = rgb + k * c->iw * 3;
                ULONG *d = big + (top + k) * c->iw;
                for (x = 0; x < c->iw; x++, s += 3)
                    d[x] = ((ULONG)s[0] << 16) | ((ULONG)s[1] << 8) | s[2];
            }
        }
        FreeVec(rgb);
    }
    DisposeObject(o);
    FreeVec(c->out);
    if ((c->out = AllocVec(c->dw * c->dh * 4, MEMF_ANY)))
        pb_shrink(big, c->iw, c->ih, c->out, c->dw, c->dh);
    FreeVec(big);
    return c->out ? 0 : ERROR_NO_FREE_STORE;
}

/* ---- zz9k.library: ZZ9KDecodeJpeg, fitted on the card */
struct PbApi {
    const char *path;
    ULONG w, h, dw, dh;         /* the box; the size shown (from own's) */
    BOOL back;                  /* read back into fast RAM */
    ULONG *out;
    int status;
    const char *stage;          /* the call that failed */
    ULONG fmt;                  /* the surface's format: ARGB8888, else BGRA8888 (the card's own) */
};

static LONG pb_api_once(void *p)
{
    struct PbApi *c = p;
    ZZ9KSharedBuffer src;
    ZZ9KSurface surf;
    ZZ9KImageDecodeDesc d;
    ZZ9KImageDecodeResult res;
    LONG len = file_size(c->path), ok = -1;
    BPTR fh;
    if (len <= 0)
        return -1;
    memset(&src, 0, sizeof(src));
    memset(&surf, 0, sizeof(surf));
    c->stage = "ZZ9KAllocShared";
    if ((c->status = ZZ9KAllocShared((ULONG)len, 16, ZZ9K_ALLOC_HOST_WINDOW, &src)) != ZZ9K_STATUS_OK)
        return -1;
    if (!c->fmt)
        c->fmt = ZZ9K_SURFACE_FORMAT_ARGB8888;
    if (!(fh = Open((CONST_STRPTR)c->path, MODE_OLDFILE)) || Read(fh, (APTR)src.data, len) != len) {
        if (fh)
            Close(fh);
        ZZ9KFreeShared(src.handle);
        return -1;
    }
    Close(fh);
    c->stage = "ZZ9KAllocSurface";
    if ((c->status = ZZ9KAllocSurface(c->dw, c->dh, c->fmt,
                                      c->back ? ZZ9K_SURFACE_FLAG_CPU_VISIBLE : ZZ9K_SURFACE_FLAG_ARM_LOCAL,
                                      &surf)) != ZZ9K_STATUS_OK) {
        ZZ9KFreeShared(src.handle);
        return -1;
    }
    memset(&d, 0, sizeof(d));
    memset(&res, 0, sizeof(res));
    d.src_handle = src.handle;
    d.src_offset = 0;
    d.src_length = (ULONG)len;
    d.dst_surface = surf.handle;
    d.dst_width = c->dw;
    d.dst_height = c->dh;
    d.output_format = c->fmt;
    d.flags = ZZ9K_IMAGE_DECODE_FLAG_FIT | ZZ9K_IMAGE_DECODE_FLAG_PRESERVE_ASPECT;
    c->stage = "ZZ9KDecodeJpeg";
    if ((c->status = ZZ9KDecodeJpeg(&d, &res)) == ZZ9K_STATUS_OK) {
        ok = 0;
        c->stage = NULL;
        if (c->back) {
            ULONG y, row = c->dw * 4;
            FreeVec(c->out);
            if (surf.data && (c->out = AllocVec(c->dw * c->dh * 4, MEMF_ANY))) {
                for (y = 0; y < c->dh; y++)
                    CopyMem((APTR)((volatile UBYTE *)surf.data + y * surf.pitch), c->out + y * c->dw, row);
                if (c->fmt == ZZ9K_SURFACE_FORMAT_BGRA8888) {
                    ULONG i, n = c->dw * c->dh;     /* B G R A bytes: to 0xAARRGGBB */
                    for (i = 0; i < n; i++) {
                        ULONG v = c->out[i];
                        c->out[i] = ((v & 0xFF) << 24) | ((v >> 8) & 0xFF) << 16 | ((v >> 16) & 0xFF) << 8 |
                                    (v >> 24);
                    }
                }
            } else {
                ok = -1;
            }
        }
    }
    ZZ9KFreeSurface(surf.handle);
    ZZ9KFreeShared(src.handle);
    return ok;
}

/* each step's start, into PROGDIR:paths-progress.txt (closed each time:
 * a run that hangs shows where) */
static void pb_step(const char *file, const char *box, const char *path)
{
    char l[160];
    BPTR fh = Open((CONST_STRPTR)"PROGDIR:paths-progress.txt", MODE_READWRITE);
    if (!fh)
        return;
    Seek(fh, 0, OFFSET_END);
    snprintf(l, sizeof(l), "%lu %.50s %s %s\n", (unsigned long)(now_us() / 1000), FilePart((STRPTR)file), box,
             path);
    Write(fh, l, strlen(l));
    Close(fh);
}

static void pb_say(const char *s)
{
    BPTR fh;
    sayf(s);
    if ((fh = Open((CONST_STRPTR)"PROGDIR:paths-progress.txt", MODE_READWRITE))) {
        Seek(fh, 0, OFFSET_END);
        Write(fh, (APTR)s, strlen(s));
        Write(fh, (APTR)"\n", 1);
        Close(fh);
    }
}

static const char *zz_status_name(int s)
{
    static const char *const n[] = { "OK", "queued", "busy", "unsupported", "bad request", "bad handle",
                                     "no memory", "timeout", "cancelled", "I/O error", "not found",
                                     "module missing", "module mismatch" };
    return s >= 0 && s < (int)(sizeof(n) / sizeof(n[0])) ? n[s] : "error";
}

/* ---- zz9k.library: an image session, its output in tiles (BGRA) into a
 * shared buffer, copied out as each comes (no surface: what the card's
 * datatype and Zorro II can use). The size shown asked for (dst): if the
 * card scales (RESULT_SCALED) the tiles are that size, else the whole
 * picture's, averaged down on the 68k */
#define PB_TSTRIDE (4 * 4096)
#define PB_TROWS 4
struct PbTile {
    const char *path;
    ULONG dw, dh;
    ULONG *out;
    ULONG iw, ih, ow, oh, ntiles, flags;
    int status;
    const char *stage;
    BOOL said;
};

static LONG pb_tile_once(void *p)
{
    struct PbTile *c = p;
    ZZ9KSharedBuffer src, tb;
    ZZ9KImageSessionBeginDesc bd;
    ZZ9KImageSessionFeedDesc fd;
    ZZ9KImageSessionResult r;
    ULONG *canvas = NULL, cw = 0, ch = 0, off = 0, guard = 0;
    LONG len = file_size(c->path), ok = -1;
    BOOL eof = FALSE, open = FALSE;
    BPTR fh;
    if (len <= 0)
        return -1;
    memset(&src, 0, sizeof(src));
    memset(&tb, 0, sizeof(tb));
    c->stage = "ZZ9KAllocShared (file)";
    if ((c->status = ZZ9KAllocShared((ULONG)len, 16, ZZ9K_ALLOC_HOST_WINDOW, &src)) != ZZ9K_STATUS_OK)
        return -1;
    if (!(fh = Open((CONST_STRPTR)c->path, MODE_OLDFILE)) || Read(fh, (APTR)src.data, len) != len) {
        if (fh)
            Close(fh);
        goto out;
    }
    Close(fh);
    c->stage = "ZZ9KAllocShared (tiles)";
    if ((c->status = ZZ9KAllocShared(PB_TSTRIDE * PB_TROWS, 16, ZZ9K_ALLOC_HOST_WINDOW, &tb)) != ZZ9K_STATUS_OK)
        goto out;
    memset(&bd, 0, sizeof(bd));
    bd.codec = ZZ9K_IMAGE_CODEC_JPEG;
    bd.output_mode = ZZ9K_IMAGE_OUTPUT_TILE_BUFFER;
    bd.output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
    bd.tile_handle = tb.handle;
    bd.tile_stride = PB_TSTRIDE;
    bd.tile_rows = PB_TROWS;
    bd.dst_width = c->dw;                   /* (the size shown: scaled to it, if the card does) */
    bd.dst_height = c->dh;
    memset(&r, 0, sizeof(r));
    c->stage = "ZZ9KImageSessionBegin";
    if ((c->status = ZZ9KImageSessionBegin(&bd, &r)) != ZZ9K_STATUS_OK)
        goto out;
    open = TRUE;
    c->ntiles = 0;
    c->flags = 0;
    while (guard++ < 100000) {
        memset(&fd, 0, sizeof(fd));
        fd.session = r.session;
        fd.src_handle = src.handle;
        fd.src_offset = off;
        fd.src_length = (ULONG)len - off;
        fd.flags = off >= (ULONG)len ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0;
        if (fd.flags)
            eof = TRUE;
        c->stage = "ZZ9KImageSessionFeed";
        if ((c->status = ZZ9KImageSessionFeed(&fd, &r)) != ZZ9K_STATUS_OK)
            goto out;
        off += r.bytes_consumed;
        c->flags |= r.flags;
        if (r.image_width && !c->iw) {
            c->iw = r.image_width;
            c->ih = r.image_height;
        }
        if (r.state == ZZ9K_IMAGE_SESSION_STATE_TILE_READY && r.tile_width && r.tile_height) {
            ULONG y, x;
            if (!canvas) {
                /* the output's size: the size asked for if scaled, else the picture's */
                cw = (r.flags & ZZ9K_IMAGE_SESSION_RESULT_SCALED) ? c->dw : c->iw;
                ch = (r.flags & ZZ9K_IMAGE_SESSION_RESULT_SCALED) ? c->dh : c->ih;
                if (!cw || !ch || !(canvas = AllocVec(cw * ch * 4, MEMF_ANY | MEMF_CLEAR))) {
                    c->stage = "the canvas";
                    goto out;
                }
            }
            c->ntiles++;
            for (y = 0; y < r.tile_height && r.tile_y + y < ch; y++) {
                const ULONG *s = (const ULONG *)((volatile UBYTE *)tb.data + y * PB_TSTRIDE);
                ULONG *d = canvas + (r.tile_y + y) * cw + r.tile_x;
                for (x = 0; x < r.tile_width && r.tile_x + x < cw; x++) {
                    ULONG v = s[x];             /* B G R A bytes: to 0x00RRGGBB */
                    d[x] = ((v >> 8) & 0xFF) << 16 | ((v >> 16) & 0xFF) << 8 | (v >> 24);
                }
            }
        } else if (r.state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
            ok = 0;
            break;
        } else if (r.state == ZZ9K_IMAGE_SESSION_STATE_ERROR) {
            c->stage = "the session (state ERROR)";
            goto out;
        } else if (r.state == ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT && eof) {
            c->stage = "the session (wants input after EOF)";
            goto out;
        }
    }
    if (ok == 0 && canvas) {
        c->ow = cw;
        c->oh = ch;
        FreeVec(c->out);
        if ((c->out = AllocVec(c->dw * c->dh * 4, MEMF_ANY))) {
            if (cw == c->dw && ch == c->dh)
                CopyMem(canvas, c->out, cw * ch * 4);
            else
                pb_shrink(canvas, cw, ch, c->out, c->dw, c->dh);
        } else {
            ok = -1;
        }
    } else if (ok == 0) {
        c->stage = "the session (no tiles)";
        ok = -1;
    }
    if (!ok)
        c->stage = NULL;
out:
    if (open)
        ZZ9KImageSessionClose(r.session, 0);
    FreeVec(canvas);
    if (tb.handle)
        ZZ9KFreeShared(tb.handle);
    ZZ9KFreeShared(src.handle);
    return ok;
}

/* what the card says about surfaces, and which allocations it takes */
static void pb_surface_probe(void)
{
    static const ULONG fmts[] = { ZZ9K_SURFACE_FORMAT_ARGB8888, ZZ9K_SURFACE_FORMAT_BGRA8888,
                                  ZZ9K_SURFACE_FORMAT_RGB565 };
    static const char *const fname[] = { "ARGB8888", "BGRA8888", "RGB565" };
    static const ULONG flags[] = { 0, ZZ9K_SURFACE_FLAG_CPU_VISIBLE, ZZ9K_SURFACE_FLAG_ARM_LOCAL,
                                   ZZ9K_SURFACE_FLAG_CPU_VISIBLE | ZZ9K_SURFACE_FLAG_SHARED_BUFFER };
    static const char *const flname[] = { "none", "cpu-visible", "arm-local", "cpu-visible+shared" };
    ZZ9KCaps caps;
    ZZ9KServiceInfo sv;
    char l[200];
    int f, g, st;
    memset(&caps, 0, sizeof(caps));
    st = ZZ9KQueryCaps(&caps);
    snprintf(l, sizeof(l), "  caps (%s): bits $%08lx, surfaces %s, max %lu, image decode %s, scale %s; "
             "host-window heap %lu",
             zz_status_name(st), (unsigned long)caps.capability_bits,
             (caps.capability_bits & ZZ9K_CAP_SURFACES) ? "yes" : "no", (unsigned long)caps.max_surfaces,
             (caps.capability_bits & ZZ9K_CAP_IMAGE_DECODE) ? "yes" : "no",
             (caps.capability_bits & ZZ9K_CAP_IMAGE_SCALE) ? "yes" : "no",
             (unsigned long)caps.host_window_heap_size);
    pb_say(l);
    memset(&sv, 0, sizeof(sv));
    st = ZZ9KQueryService(ZZ9K_SERVICE_SURFACE, &sv);
    snprintf(l, sizeof(l), "  surface service (%s): version %lu, flags $%08lx, %.20s", zz_status_name(st),
             (unsigned long)sv.version, (unsigned long)sv.flags, sv.name);
    pb_say(l);
    for (f = 0; f < 3; f++)
        for (g = 0; g < 4; g++) {
            ZZ9KSurface su;
            int a, b2;
            ULONG pitch = 0, len = 0;
            memset(&su, 0, sizeof(su));
            a = ZZ9KAllocSurface(160, 120, fmts[f], flags[g], &su);
            if (a == ZZ9K_STATUS_OK)
                ZZ9KFreeSurface(su.handle);
            zz9k_surface_layout(160, 120, fmts[f], (uint32_t *)&pitch, (uint32_t *)&len);
            memset(&su, 0, sizeof(su));
            b2 = ZZ9KAllocSurfaceEx(160, 120, fmts[f], flags[g], pitch, &su);
            snprintf(l, sizeof(l), "  surface 160x120 %-8s %-18s: AllocSurface %s, Ex(pitch %lu) %s%s",
                     fname[f], flname[g], zz_status_name(a), (unsigned long)pitch, zz_status_name(b2),
                     b2 == ZZ9K_STATUS_OK && su.data ? " (68k can read it)" : "");
            if (b2 == ZZ9K_STATUS_OK)
                ZZ9KFreeSurface(su.handle);
            pb_say(l);
        }
}

/* the mean difference of each colour, 0-255 x 10 */
static void pb_diff(const ULONG *a, const ULONG *b, ULONG n, ULONG *dr, ULONG *dg, ULONG *db)
{
    ULONG i, r = 0, g = 0, bl = 0;
    for (i = 0; i < n; i++) {
        LONG x = (LONG)((a[i] >> 16) & 255) - (LONG)((b[i] >> 16) & 255);
        LONG y = (LONG)((a[i] >> 8) & 255) - (LONG)((b[i] >> 8) & 255);
        LONG z = (LONG)(a[i] & 255) - (LONG)(b[i] & 255);
        r += x < 0 ? -x : x;
        g += y < 0 ? -y : y;
        bl += z < 0 ? -z : z;
    }
    *dr = n ? r * 10 / n : 0;
    *dg = n ? g * 10 / n : 0;
    *db = n ? bl * 10 / n : 0;
}

static struct Library *pb_class(const char *name, Class **cl)
{
    char path[64];
    struct Library *lib;
    snprintf(path, sizeof(path), "datatypes/%s", name);
    *cl = NULL;
    if ((lib = OpenLibrary((CONST_STRPTR)path, 0)))
        *cl = ObtainEngine(lib);
    if (lib && !*cl) {
        CloseLibrary(lib);
        lib = NULL;
    }
    return lib;
}

static void pb_check(char *out, LONG max, const char *what, const ULONG *a, const ULONG *own, ULONG n)
{
    ULONG r, g, b;
    if (!a || !own) {
        snprintf(out, max, "%s -", what);
        return;
    }
    pb_diff(a, own, n, &r, &g, &b);
    snprintf(out, max, "%s %ld.%ld/%ld.%ld/%ld.%ld", what, (long)(r / 10), (long)(r % 10), (long)(g / 10),
             (long)(g % 10), (long)(b / 10), (long)(b % 10));
}

static void bench_paths(const char *const *files, int nfiles)
{
    struct Library *zzlib = NULL, *zzdtl, *jfifl;
    Class *zzcl, *jfifcl;
    ZZ9KServiceInfo svc;
    BOOL api = FALSE;
    char line[240], c[PB_N][24];
    int fi;
    ULONG b;

    heading("Bench: picture paths, file to pixels at the size shown (ms, best of up to 10, mean)");
    if (!pb_nozz && (zzlib = OpenLibrary((CONST_STRPTR)"zz9k.library", 2))) {
        ZZ9KBase = zzlib;
        memset(&svc, 0, sizeof(svc));
        if (zzlib->lib_Version > 2 || zzlib->lib_Revision >= ZZ9K_LIBRARY_MIN_REVISION_TYPED_IMAGE_DECODE) {
            if (ZZ9KQueryService(ZZ9K_SERVICE_IMAGE, &svc) == ZZ9K_STATUS_OK &&
                (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_JPEG_BASELINE))
                api = TRUE;
            snprintf(line, sizeof(line), "  zz9k.library %ld.%ld; image service %s, flags $%08lx (JPEG scaling %s, "
                     "progressive %s, WebP %s, to the screen %s)",
                     (long)zzlib->lib_Version, (long)zzlib->lib_Revision, api ? "there" : "not there",
                     (unsigned long)svc.flags, (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_JPEG_SCALING) ? "yes" : "no",
                     (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_JPEG_PROGRESSIVE) ? "yes" : "no",
                     (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_WEBP) ? "yes" : "no",
                     (svc.flags & ZZ9K_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT) ? "yes" : "no");
        } else {
            snprintf(line, sizeof(line), "  zz9k.library %ld.%ld: too old for ZZ9KDecodeJpeg", (long)zzlib->lib_Version,
                     (long)zzlib->lib_Revision);
        }
        pb_say(line);
        if (api)
            pb_surface_probe();
    } else {
        pb_say("  zz9k.library 2 not there: the API's columns are left out");
    }
    zzdtl = NULL;
    zzcl = NULL;
    if (pb_zzdt)
        zzdtl = pb_class("zz9k-picture.datatype", &zzcl);
    jfifl = pb_class("jfif.datatype", &jfifcl);
    if (!zzdtl && pb_zzdt)
        pb_say("  zz9k-picture.datatype not there: its column is left out");
    if (!pb_zzdt)
        pb_say("  The datatypes' columns left out (NODT)");

    for (fi = 0; fi < nfiles; fi++) {
        const char *path = files[fi];
        struct DataType *dtn = NULL;
        BPTR lock;
        if (!exists(path)) {
            snprintf(line, sizeof(line), "  %.60s: not found", path);
            pb_say(line);
            continue;
        }
        if ((lock = Lock((CONST_STRPTR)path, SHARED_LOCK))) {
            dtn = ObtainDataTypeA(DTST_FILE, (APTR)lock, NULL);
            UnLock(lock);
        }
        for (b = 0; b < PB_BOXES; b++) {
            struct PbOwn own;
            struct PbDt zd, jd;
            struct PbApi ab, ac;
            struct PbTile at;
            ULONG best[PB_N], mean[PB_N];
            char chk[4][48];
            int k;
            memset(best, 0, sizeof(best));
            memset(mean, 0, sizeof(mean));
            memset(&own, 0, sizeof(own));
            own.path = path;
            own.w = pb_box[b].w;
            own.h = pb_box[b].h;
            pb_step(path, pb_box[b].name, "own");
            if (timed(pb_own_once, &own, NULL, &best[PB_OWN], &mean[PB_OWN]))
                best[PB_OWN] = 0;
            memset(&zd, 0, sizeof(zd));
            memset(&jd, 0, sizeof(jd));
            zd.path = jd.path = path;
            zd.w = jd.w = pb_box[b].w;
            zd.h = jd.h = pb_box[b].h;
            zd.dtn = jd.dtn = dtn;
            zd.cl = zzcl;
            jd.cl = jfifcl;
            pb_step(path, pb_box[b].name, "jfif dt");
            if (jfifcl && pb_zzdt && (jd.err = timed(pb_dt_once, &jd, NULL, &best[PB_JFIF], &mean[PB_JFIF])))
                best[PB_JFIF] = 0;
            memset(&ab, 0, sizeof(ab));
            memset(&ac, 0, sizeof(ac));
            memset(&at, 0, sizeof(at));
            if (api && own.dw) {
                ab.path = ac.path = path;
                ab.dw = ac.dw = own.dw;
                ab.dh = ac.dh = own.dh;
                ab.back = TRUE;
                pb_step(path, pb_box[b].name, "API back");
                if (timed(pb_api_once, &ab, NULL, &best[PB_APIBACK], &mean[PB_APIBACK])) {
                    char w[120];
                    snprintf(w, sizeof(w), "      API in ARGB8888: %s said %s; again in BGRA8888",
                             ab.stage ? ab.stage : "read back", zz_status_name(ab.status));
                    pb_say(w);
                    ab.fmt = ac.fmt = ZZ9K_SURFACE_FORMAT_BGRA8888;
                    FreeVec(ab.out);
                    ab.out = NULL;
                    if (timed(pb_api_once, &ab, NULL, &best[PB_APIBACK], &mean[PB_APIBACK]))
                        best[PB_APIBACK] = 0;
                }
                pb_step(path, pb_box[b].name, "API card");
                if (timed(pb_api_once, &ac, NULL, &best[PB_APICARD], &mean[PB_APICARD]))
                    best[PB_APICARD] = 0;
                memset(&at, 0, sizeof(at));
                at.path = path;
                at.dw = own.dw;
                at.dh = own.dh;
                pb_step(path, pb_box[b].name, "API tiles");
                if (timed(pb_tile_once, &at, NULL, &best[PB_APITILE], &mean[PB_APITILE]))
                    best[PB_APITILE] = 0;
                snprintf(line, sizeof(line), "      API tiles: %s%s%s; picture %ldx%ld, made %ldx%ld on the card (%s), "
                         "%ld tiles", at.stage ? at.stage : "ok", at.stage ? " said " : "",
                         at.stage ? zz_status_name(at.status) : "", (long)at.iw, (long)at.ih, (long)at.ow,
                         (long)at.oh, (at.flags & ZZ9K_IMAGE_SESSION_RESULT_SCALED) ? "scaled" : "whole",
                         (long)at.ntiles);
                pb_say(line);
            }
            pb_step(path, pb_box[b].name, "zz9k dt");
            if (zzcl && pb_zzdt && (zd.err = timed(pb_dt_once, &zd, NULL, &best[PB_ZZDT], &mean[PB_ZZDT])))
                best[PB_ZZDT] = 0;
            if (b == 0) {
                snprintf(line, sizeof(line), "  %.50s, %ld x %ld", FilePart((STRPTR)path),
                         (long)(own.iw ? own.iw : zd.iw), (long)(own.ih ? own.ih : zd.ih));
                pb_say(line);
                snprintf(line, sizeof(line), "    %-16s %-12s %-12s %-12s %-12s %-12s %-12s", "shown at",
                         pb_name[0], pb_name[1], pb_name[2], pb_name[3], pb_name[4], pb_name[5]);
                pb_say(line);
            }
            for (k = 0; k < PB_N; k++)
                cell(c[k], best[k], mean[k]);
            {
                char atx[24];
                snprintf(atx, sizeof(atx), "%s %ldx%ld", pb_box[b].name, (long)own.dw, (long)own.dh);
                snprintf(line, sizeof(line), "    %-16s %-12s %-12s %-12s %-12s %-12s %-12s", atx, c[0], c[1], c[2],
                         c[3], c[4], c[5]);
                pb_say(line);
            }
            if (!best[PB_OWN] && own.err) {
                snprintf(line, sizeof(line), "      own: %s", jd_why(own.err));
                pb_say(line);
            }
            if (api && !best[PB_APIBACK]) {
                snprintf(line, sizeof(line), "      API: %s said %s", ab.stage ? ab.stage : "read back",
                         zz_status_name(ab.status));
                pb_say(line);
            }
            if (zzcl && pb_zzdt && !best[PB_ZZDT]) {
                why(zd.err, chk[0], sizeof(chk[0]));
                snprintf(line, sizeof(line), "      zz9k dt: %s", chk[0]);
                pb_say(line);
            }
            /* the pixels against own's (when both are the same size) */
            pb_check(chk[0], sizeof(chk[0]), "zz9k dt",
                     zd.dw == own.dw && zd.dh == own.dh ? zd.out : NULL, own.out, own.dw * own.dh);
            pb_check(chk[1], sizeof(chk[1]), "jfif dt",
                     jd.dw == own.dw && jd.dh == own.dh ? jd.out : NULL, own.out, own.dw * own.dh);
            pb_check(chk[2], sizeof(chk[2]), "API", ab.out, own.out, own.dw * own.dh);
            pb_check(chk[3], sizeof(chk[3]), "tiles", at.out, own.out, own.dw * own.dh);
            snprintf(line, sizeof(line), "      differs from own's, R/G/B 0-255: %s; %s; %s; %s", chk[0], chk[1], chk[2],
                     chk[3]);
            pb_say(line);
            FreeVec(own.out);
            FreeVec(zd.out);
            FreeVec(jd.out);
            FreeVec(ab.out);
            FreeVec(at.out);
        }
        if (dtn)
            ReleaseDataType(dtn);
    }
    pb_say("  own: jdec at 1/1..1/8, then averaged on the 68k. dt: the class's object from the file,");
    pb_say("  read back whole, averaged on the 68k. API back: decoded and fitted on the card, read");
    pb_say("  back; API card: the same left on the card (straight-to-screen's cost, less the blit).");
    pb_say("  Every column includes reading the file. On Amiberry the card's own work runs on the PC.");
    if (zzdtl)
        CloseLibrary(zzdtl);
    if (jfifl)
        CloseLibrary(jfifl);
    if (zzlib)
        CloseLibrary(zzlib);
    ZZ9KBase = NULL;
}
