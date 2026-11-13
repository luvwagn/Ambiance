/*
 * AmigaMCE - medtime.c on the build machine: a MED module's length.
 *   medtime_host <module> [timer E-clocks a tick at its first tempo]
 */
#include <stdio.h>
#include <stdlib.h>
#include "../src/medtime.h"

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *m;
    long n;
    struct MedTime t;
    unsigned long cnt = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    if (argc < 2 || !(f = fopen(argv[1], "rb")))
        return 2;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    m = malloc(n);
    if (!m || fread(m, 1, n, f) != (size_t)n)
        return 2;
    fclose(f);
    med_measure(m, (unsigned long)n, &t);
    printf("units16 %lu (%lu ticks), %d positions, tempo %u", t.units16, t.units16 / 16,
           t.positions, t.tempo);
    if (!cnt && t.tempo > 10)
        cnt = 474326UL / t.tempo;           /* MED's SPD timer */
    if (cnt)
        printf(", %.1f s at %lu E-clocks a tick", t.units16 / 16.0 * cnt / 709379.0, cnt);
    printf("\n");
    return t.units16 ? 0 : 1;
}
