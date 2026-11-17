/*
 * Ambiance - packed files unpacked, with xfdmaster.library
 * Copyright (c) 2026 Brian King
 *
 * Module collections are full of packed files: PowerPacker ("PP20", the
 * mod.name.pp of old), XPK (through xfd's xPK slave and xpkmaster), and
 * the dozens of crunchers xfdmaster's slaves know. When a file is not
 * something Ambiance can read as it is, it is read whole and given to
 * xfdmaster to recognise and unpack, a few layers deep if need be. Without
 * xfdmaster.library nothing is unpacked, and the file is taken as it is.
 *
 * The library is called through the inline LP macros: there is no C
 * header for it in the NDK, so its two structures and calls are here,
 * from xfdmaster.i (37.1).
 */
#include "amb.h"

#include <string.h>
#include <exec/libraries.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define XFDOBJ_BUFFERINFO   1
#define XFDFF_RECOGEXTERN   1       /* the external slaves too (xPK, ...) */

struct xfdBufferInfo {
    APTR  SourceBuffer;
    ULONG SourceBufLen;
    APTR  Slave;
    STRPTR PackerName;
    UWORD PackerFlags;
    UWORD Error;
    APTR  TargetBuffer;
    ULONG TargetBufMemType;
    ULONG TargetBufLen;
    ULONG TargetBufSaveLen;
    ULONG DecrAddress;
    ULONG JmpAddress;
    APTR  Special;
    UWORD Flags;
};

#define xfdAllocObject(t) \
    LP1(114, APTR, xfdAllocObject, ULONG, t, d0, , xfd)
#define xfdFreeObject(o) \
    LP1NR(120, xfdFreeObject, APTR, o, a1, , xfd)
#define xfdRecogBuffer(bi) \
    LP1(54, BOOL, xfdRecogBuffer, struct xfdBufferInfo *, bi, a0, , xfd)
#define xfdDecrunchBuffer(bi) \
    LP1(60, BOOL, xfdDecrunchBuffer, struct xfdBufferInfo *, bi, a0, , xfd)

#define LAYERS     4                /* packed inside packed... */
#define UNPACK_MAX (16UL << 20)     /* files larger are not read whole */

/* The whole file in memory (AllocVec), NULL if it cannot be. */
UBYTE *unpack_read_whole(const char *path, ULONG *len)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    UBYTE *buf = NULL;
    LONG size;
    BPTR fh;
    me->pr_WindowPtr = (APTR)-1;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = oldwin;
    if (!fh)
        return NULL;
    if (Seek(fh, 0, OFFSET_END) >= 0 && (size = Seek(fh, 0, OFFSET_BEGINNING)) > 0 &&
        (ULONG)size <= UNPACK_MAX && (buf = AllocVec(size + 16, MEMF_ANY | MEMF_CLEAR))) {
        if (Read(fh, buf, size) != size) {
            FreeVec(buf);
            buf = NULL;
        } else {
            *len = (ULONG)size;
        }
    }
    Close(fh);
    return buf;
}

/* buf (len bytes, AllocVec) unpacked if it is packed: the unpacked data
 * (AllocVec, 16 bytes spare at the end) with buf freed, or buf itself if
 * it is not packed (or cannot be unpacked: then *why says why). packer:
 * the outermost packer's name ("" if none). */
UBYTE *unpack_buffer(UBYTE *buf, ULONG *len, char *packer, LONG pmax, const char **why)
{
    struct Library *xfd;
    int layer;
    *why = NULL;
    if (packer && pmax)
        packer[0] = 0;
    if (!(xfd = OpenLibrary((CONST_STRPTR)"xfdmaster.library", 37)))
        return buf;
    for (layer = 0; layer < LAYERS; layer++) {
        struct xfdBufferInfo *bi = xfdAllocObject(XFDOBJ_BUFFERINFO);
        UBYTE *out;
        if (!bi)
            break;
        bi->SourceBuffer = buf;
        bi->SourceBufLen = *len;
        bi->Flags = XFDFF_RECOGEXTERN;
        if (!xfdRecogBuffer(bi)) {
            xfdFreeObject(bi);
            break;                          /* not packed (any more) */
        }
        if (packer && pmax && !packer[0] && bi->PackerName)
            amb_strlcpy(packer, (const char *)bi->PackerName, pmax);
        if (bi->PackerFlags & (1 << 4) || bi->PackerFlags & (3 << 6)) {
            *why = "it is packed with a password";
            xfdFreeObject(bi);
            break;
        }
        bi->TargetBufMemType = MEMF_ANY;
        if (!xfdDecrunchBuffer(bi)) {
            *why = bi->Error == 1 ? "not enough memory to unpack it"
                                  : bi->Error == 9 ? "a library its packer needs is missing"
                                                   : "it could not be unpacked";
            xfdFreeObject(bi);
            break;
        }
        /* into memory of our own, so every buffer here is freed the same way */
        out = AllocVec(bi->TargetBufSaveLen + 16, MEMF_ANY | MEMF_CLEAR);
        if (out)
            CopyMem(bi->TargetBuffer, out, bi->TargetBufSaveLen);
        FreeMem(bi->TargetBuffer, bi->TargetBufLen);
        if (!out) {
            *why = "not enough memory to unpack it";
            xfdFreeObject(bi);
            break;
        }
        FreeVec(buf);
        buf = out;
        *len = bi->TargetBufSaveLen;
        xfdFreeObject(bi);
    }
    CloseLibrary(xfd);
    return buf;
}
