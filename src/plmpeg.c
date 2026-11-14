/*
 * AmigaMCE - pl_mpeg (third_party/pl_mpeg, MIT, Dominic Szablewski) built
 * here: its demuxer and MPEG-1 video decoder, for MPEG video where there
 * is no ZZ9000 (stream.c: swv_run). Built for speed (the Makefile's -O2
 * for this file), its own warnings left to it.
 */
#include <stdlib.h>
#include <string.h>

#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "../third_party/pl_mpeg/pl_mpeg.h"
