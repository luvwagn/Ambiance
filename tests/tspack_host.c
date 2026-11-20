/*
 * tspack.c on the host: a transport stream packed as a Program Stream
 * (tests/tspack_test.sh makes the streams with ffmpeg and compares what
 * comes out with ffmpeg's own reading of the transport stream).
 *
 *   tspack_host IN.ts OUT.mpg SIZE VPID APID MPEG2 [IN2.ts OUT2.mpg]
 *
 * SIZE 188 or 192; APID 8191 for none. TSP_SKIP=n (and TSP_HEAD=m) in
 * the environment: m bytes of its Program Stream from its start, then the
 * rest from byte n on (a packet's start) by tsp_seek_in, as after a seek. With a second stream, the two are
 * packed at once by two instances of their own (tsp_begin_in), a read of
 * one, then the other's, as a picture is made while another video plays;
 * the first is also packed alone by tspack.c's own (tsp_begin), and the
 * two outputs must be the same.
 * Copyright (c) 2026 Brian King, MIT licence
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tspack.h"

static LONG rd(void *ctx, UBYTE *b, LONG n)
{
    size_t got = fread(b, 1, (size_t)n, (FILE *)ctx);
    if (ferror((FILE *)ctx))
        return -1;
    return (LONG)got;
}

#define OUT_MAX (64 * 1024)

/* All of in packed into out (by t, or tspack.c's own when t is NULL) */
static long pack_all(struct Tsp *t, FILE *in, FILE *out, UBYTE size, UWORD vpid, UWORD apid, BOOL mpeg2)
{
    static UBYTE b[OUT_MAX];
    long total = 0;
    LONG n;
    if (t)
        tsp_begin_in(t, size, vpid, apid, FALSE, mpeg2);
    else
        tsp_begin(size, vpid, apid, FALSE, mpeg2);
    while ((n = t ? tsp_pack_in(t, rd, in, b, OUT_MAX) : tsp_pack(rd, in, b, OUT_MAX)) > 0) {
        fwrite(b, 1, (size_t)n, out);
        total += n;
    }
    return n < 0 ? -1 : total;
}

int main(int argc, char **argv)
{
    UBYTE size;
    UWORD vpid, apid;
    BOOL mpeg2;
    FILE *in, *out;
    if (argc != 7 && argc != 9) {
        fprintf(stderr, "usage: tspack_host IN OUT SIZE VPID APID MPEG2 [IN2 OUT2]\n");
        return 2;
    }
    size = (UBYTE)atoi(argv[3]);
    vpid = (UWORD)atoi(argv[4]);
    apid = (UWORD)atoi(argv[5]);
    mpeg2 = (BOOL)atoi(argv[6]);
    if (!(in = fopen(argv[1], "rb")) || !(out = fopen(argv[2], "wb"))) {
        perror("open");
        return 2;
    }
    if (argc == 7) {
        long n;
        if (getenv("TSP_SKIP")) {
            /* its start (TSP_HEAD bytes of it packed: the decoder's sequence
             * header), then from the middle (a seek, as a picture's maker
             * does): on a packet's start, by tsp_seek_in, all of the rest */
            static UBYTE b[OUT_MAX];
            struct Tsp *t = malloc(tsp_size());
            long at = atol(getenv("TSP_SKIP")), head = getenv("TSP_HEAD") ? atol(getenv("TSP_HEAD")) : 0;
            LONG k;
            n = 0;
            tsp_begin_in(t, size, vpid, apid, FALSE, mpeg2);
            while (n < head && (k = tsp_pack_in(t, rd, in, b, OUT_MAX)) > 0) {
                fwrite(b, 1, (size_t)k, out);
                n += k;
            }
            fseek(in, at / size * size, SEEK_SET);
            if (head)
                tsp_seek_in(t);             /* (its decoder has its sequence header) */
            else
                tsp_begin_in(t, size, vpid, apid, FALSE, mpeg2);   /* (cold: to the next one) */
            while ((k = tsp_pack_in(t, rd, in, b, OUT_MAX)) > 0) {
                fwrite(b, 1, (size_t)k, out);
                n += k;
            }
            free(t);
        } else
            n = pack_all(NULL, in, out, size, vpid, apid, mpeg2);
        printf("packed %ld bytes\n", n);
        fclose(in);
        fclose(out);
        return n > 0 ? 0 : 1;
    }
    {
        /* two at once, each by its own, a read about */
        struct Tsp *a = malloc(tsp_size()), *b = malloc(tsp_size());
        FILE *in2 = fopen(argv[7], "rb"), *out2 = fopen(argv[8], "wb");
        static UBYTE buf[OUT_MAX];
        LONG na = 1, nb = 1;
        long ta = 0, tb = 0;
        if (!a || !b || !in2 || !out2) {
            perror("two");
            return 2;
        }
        tsp_begin_in(a, size, vpid, apid, FALSE, mpeg2);
        tsp_begin_in(b, size, vpid, apid, FALSE, mpeg2);
        while (na > 0 || nb > 0) {
            if (na > 0 && (na = tsp_pack_in(a, rd, in, buf, 4096 + 2 * 2100)) > 0) {
                fwrite(buf, 1, (size_t)na, out);
                ta += na;
            }
            if (nb > 0 && (nb = tsp_pack_in(b, rd, in2, buf, 8192 + 2 * 2100)) > 0) {
                fwrite(buf, 1, (size_t)nb, out2);
                tb += nb;
            }
        }
        printf("packed at once: %ld and %ld bytes\n", ta, tb);
        fclose(in);
        fclose(in2);
        fclose(out);
        fclose(out2);
        free(a);
        free(b);
        return ta > 0 && tb > 0 && na == 0 && nb == 0 ? 0 : 1;
    }
}
