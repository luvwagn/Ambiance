/*
 * AmigaMCE - a MED/OctaMED song's length (medtime.c)
 * Copyright (c) 2026 Brian King, MIT licence
 */
#ifndef MEDTIME_H
#define MEDTIME_H

struct MedTime {
    unsigned long units16;      /* its length: ticks at its first tempo, x 16 */
    int positions;              /* in its order */
    unsigned tempo;             /* its first tempo */
};

/* m: the module as in its file (len bytes). Its length, as units16; 0 if
 * it is not an MMD0-MMD3 module, or has nothing to walk. */
unsigned long med_measure(const unsigned char *m, unsigned long len, struct MedTime *out);

#endif
