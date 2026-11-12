/*
 * amcejpeg.datatype: JPEG pictures for 68020+ Amigas (picture.datatype V43+)
 * Copyright (c) 2026 Brian King, MIT
 *
 * Every program that loads pictures through datatypes gets it as it is.
 * A program that shows a picture smaller than its own size (a thumbnail,
 * a picture fitted to a screen) can say so with the tags below, given to
 * NewDTObject(): the datatype then decodes at 1/2, 1/4 or 1/8 of the
 * size where that is still at least as big as asked, which is several
 * times faster than decoding it whole and scaling it down. The object's
 * bitmap header then has the size decoded; JDTA_FullWidth/Height give the
 * picture's own. Other JPEG datatypes ignore the tags, so a program can
 * always pass them.
 */
#ifndef AMCEJPEG_H
#define AMCEJPEG_H

#include <utility/tagitem.h>

#define JDTA_Dummy      (TAG_USER + 0x4A440000UL)   /* 'JD' */

/* (ULONG) NewDTObject(): the width and height it will be shown at, in
 * pixels. The datatype decodes no smaller than that, keeping the
 * picture's shape. 0 or left out: no limit that way. */
#define JDTA_FitWidth   (JDTA_Dummy + 1)
#define JDTA_FitHeight  (JDTA_Dummy + 2)

/* (ULONG *) GetDTAttrs(): the picture's own size, whatever was decoded */
#define JDTA_FullWidth  (JDTA_Dummy + 3)
#define JDTA_FullHeight (JDTA_Dummy + 4)

/* (ULONG *) GetDTAttrs(): the scale decoded at: 1, 2, 4 or 8 (1/n) */
#define JDTA_Scale      (JDTA_Dummy + 5)

/* (ULONG *) GetDTAttrs(): TRUE if the file ended or was damaged before
 * the picture did (what is missing is grey) */
#define JDTA_Damaged    (JDTA_Dummy + 6)

/* (ULONG *) GetDTAttrs(): where the decode's time went, in microseconds
 * (0 where timer.device's E-clock was not to be had): all of it, from
 * the headers to the last row; and of that, handing the rows over to
 * picture.datatype (PDTM_WRITEPIXELARRAY). For zzprobe. */
#define JDTA_TimeLoad   (JDTA_Dummy + 7)
#define JDTA_TimeWrite  (JDTA_Dummy + 8)
/* ...and of it: the headers read (jd_open), and the picture's size and
 * mode given to picture.datatype (SetDTAttrs) */
#define JDTA_TimeHeaders (JDTA_Dummy + 9)
#define JDTA_TimeSetAttrs (JDTA_Dummy + 10)
/* ...the nominal size set (OM_SET to picture.datatype), and the whole
 * file read into memory before decoding (0 when it was streamed) */
#define JDTA_TimeNominal (JDTA_Dummy + 11)
#define JDTA_TimeRead   (JDTA_Dummy + 12)

/* (ULONG) NewDTObject(), for zzprobe only: how picture.datatype is given
 * PMODE_V43 and the nominal size (0 the default; see amcejpeg.c) */
#define JDTA_Variant    (JDTA_Dummy + 20)

#endif
