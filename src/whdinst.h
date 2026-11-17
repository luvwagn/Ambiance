/*
 * Ambiance - WHDLoad itself, found or installed (whdinst.c). Games and
 * Demos offer to fetch its user package from whdload.de where the network
 * is up and WHDLoad is not installed (and AGS, which carries its own, is
 * not there).
 */
#ifndef AMCE_WHDINST_H
#define AMCE_WHDINST_H

#include "amce.h"

#define WHD_HOST    "www.whdload.de"
#define WHD_PATH    "/whdload/WHDLoad_usr_small.lha"    /* the user package, fewer doc languages */
#define WHD_ARCHIVE "T:Ambiance-WHDLoad.lha"
#define WHD_DIR     "T:Ambiance-WHDLoad"

/* Ambiance/WHDLoadOffer: "no" once Not now was chosen (Games stops offering;
 * Settings > Folders > WHDLoad still does). */
#define WHD_OFFER_VAR "Ambiance/WHDLoadOffer"

BOOL whd_present(char *where, LONG max);    /* WHDLoad in C: (or SYS:C): where */

/* From the unpacked package in dir: WHDLoad and the programs beside it
 * into C: (those already there kept), WHDLoad.prf into S: if it has none,
 * Devs:Kickstarts made. done: what was put where. */
BOOL whd_install_from(const char *dir, char *why, LONG wmax, char *done, LONG dmax);

void whd_cleanup(void);         /* the download and what it unpacked, deleted */

#endif
