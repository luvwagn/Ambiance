/*
 * dvdiso.c on the host: the 64-bit packet as UAE's file system
 * (filesys.cpp) reads it out of the Amiga's memory, positions past 2 and
 * 4 GB, and what the test DVD's image holds (tests/mkdvd.sh: ISO 9660 and
 * UDF, VIDEO_TS in its root).
 *
 *   dvdiso_host test.iso
 */
#include "dvdiso.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

static int failed, passed;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (c)                                          \
            passed++;                                   \
        else {                                          \
            failed++;                                   \
            printf("FAIL line %d: ", __LINE__);         \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

/* The packet as the Amiga holds it (big-endian, its longwords in order) */
static void amiga_bytes(const struct DosPacket64 *p, unsigned char *b)
{
    const ULONG *l = (const ULONG *)p;
    unsigned i;
    for (i = 0; i < sizeof(*p) / 4; i++) {
        b[i * 4] = (unsigned char)(l[i] >> 24);
        b[i * 4 + 1] = (unsigned char)(l[i] >> 16);
        b[i * 4 + 2] = (unsigned char)(l[i] >> 8);
        b[i * 4 + 3] = (unsigned char)l[i];
    }
}

static unsigned long long be(const unsigned char *b, int n)
{
    unsigned long long v = 0;
    while (n--)
        v = v << 8 | *b++;
    return v;
}

static unsigned char img[8 * 2048];

int main(int argc, char **argv)
{
    struct DosPacket64 p;
    unsigned char b[64];
    ULONG hi, lo;
    LONG first, rest;
    BOOL udf;
    ULONG root, len;

    /* ---- the packet: where filesys.cpp's GET_PCK64_ARG1..3 and
     * PUT_PCK64_RES0..2 are (dp64_Arg1 32, Arg2 40, Arg3 48, Res0 12,
     * Res2 16, Res1 24) */
    CHECK(sizeof(p) == 64 && offsetof(struct DosPacket64, dp_Type) == 8 &&
          offsetof(struct DosPacket64, dp_Res0) == 12 && offsetof(struct DosPacket64, dp_Res2) == 16 &&
          offsetof(struct DosPacket64, dp_Res1Hi) == 24 && offsetof(struct DosPacket64, dp_Arg1) == 32 &&
          offsetof(struct DosPacket64, dp_Arg2Hi) == 40 && offsetof(struct DosPacket64, dp_Arg3) == 48,
          "DosPacket64 layout");
    /* sector 3,500,000 of a 7.16 GB image: 7,168,000,000 bytes in */
    dvdi_sector_pos(3500000, &hi, &lo);
    dvdi_pkt64(&p, ACTION_CHANGE_FILE_POSITION64, 0x1234, hi, lo, -1);
    amiga_bytes(&p, b);
    CHECK(be(b + 8, 4) == 8001, "type %llu", be(b + 8, 4));
    CHECK(be(b + 32, 4) == 0x1234, "Arg1 (fh_Arg1) %llx", be(b + 32, 4));
    CHECK(be(b + 40, 8) == 7168000000ULL, "Arg2 (the position) %llu", be(b + 40, 8));
    CHECK(be(b + 48, 4) == 0xFFFFFFFFULL, "Arg3 (OFFSET_BEGINNING) %llx", be(b + 48, 4));
    CHECK((LONG)be(b + 12, 4) == DP64_INIT, "Res0 sent as DP64_INIT");
    /* a handler that knows it keeps DP64_INIT; one that does not replies
     * DOSFALSE, ERROR_ACTION_NOT_KNOWN where an old packet's Res1, Res2 are */
    p.dp_Res1Hi = 0, p.dp_Res1Lo = 1;
    CHECK(dvdi_pkt64_known(&p), "answered");
    p.dp_Res0 = 0, p.dp_Res2 = 209;
    CHECK(!dvdi_pkt64_known(&p), "not known taken as known");
    dvdi_pkt64(&p, ACTION_GET_FILE_SIZE64, 1, 0, 0, 0);
    p.dp_Res2 = 209;
    CHECK(!dvdi_pkt64_known(&p), "not known (Res0 untouched) taken as known");
    /* a size back: 7,688,961,024 bytes (3,754,375 sectors) */
    CHECK(dvdi_size_sectors(1, 7688961024ULL & 0xFFFFFFFF) == 3754375, "size %lu",
          (unsigned long)dvdi_size_sectors(1, 7688961024ULL & 0xFFFFFFFF));
    CHECK(dvdi_size_sectors(0x200000, 0) == 0, "8 TB taken");
    dvdi_sector_pos(0x1FFFFF, &hi, &lo);
    CHECK(hi == 0 && lo == 0xFFFFF800UL, "4 GB less a sector: %lx %lx", (unsigned long)hi, (unsigned long)lo);
    dvdi_sector_pos(0x200000, &hi, &lo);
    CHECK(hi == 1 && lo == 0, "4 GB: %lx %lx", (unsigned long)hi, (unsigned long)lo);

    /* ---- 32-bit Seeks: once under 2 GB, twice under 4 GB, never past */
    CHECK(dvdi_seek_steps(16, &first, &rest) == 1 && first == 32768 && rest == 0, "sector 16");
    CHECK(dvdi_seek_steps(0xFFFFF, &first, &rest) == 1 && first == 0x7FFFF800L, "2 GB less a sector");
    CHECK(dvdi_seek_steps(0x100000, &first, &rest) == 2 && first == 0x7FFFF800L && rest == 2048,
          "2 GB: %ld + %ld", (long)first, (long)rest);
    CHECK(dvdi_seek_steps(0x1FFFFE, &first, &rest) == 2 && first == 0x7FFFF800L && rest == 0x7FFFF800L,
          "4 GB less two sectors: %ld + %ld", (long)first, (long)rest);
    CHECK(dvdi_seek_steps(0x1FFFFF, &first, &rest) == 0, "4 GB less a sector");
    CHECK(dvdi_seek_steps(3500000, &first, &rest) == 0, "7 GB");

    /* ---- what an image holds: UDF alone, nothing, a folder over two sectors */
    memset(img, 0, sizeof(img));
    memcpy(img + 1, "BEA01", 5);
    memcpy(img + 2048 + 1, "NSR02", 5);
    memcpy(img + 4096 + 1, "TEA01", 5);
    CHECK(!dvdi_volume(img, 5, &root, &len, &udf) && udf, "UDF alone");
    memset(img, 0, sizeof(img));
    CHECK(!dvdi_volume(img, 5, &root, &len, &udf) && !udf, "nothing");
    {
        /* records: "." and ".." filling the first sector's start, the rest
         * of it unused, then AUDIO_TS (a file) and VIDEO_TS (a folder) */
        static const char *names[] = { "\0", "\1", "AUDIO_TS;1", "video_ts" };
        LONG at[] = { 0, 34, 2048, 2048 + 44 }, i;
        for (i = 0; i < 4; i++) {
            unsigned char *r = img + at[i];
            LONG nl = i < 2 ? 1 : (LONG)strlen(names[i]);
            r[0] = (unsigned char)(33 + nl + (nl & 1 ? 0 : 1));
            r[25] = i == 2 ? 0 : 2;
            r[32] = (unsigned char)nl;
            memcpy(r + 33, names[i], nl);
        }
        CHECK(dvdi_has_dir(img, 4096, "VIDEO_TS"), "VIDEO_TS in the second sector (any case)");
        CHECK(!dvdi_has_dir(img, 4096, "AUDIO_TS"), "a file taken for a folder");
        CHECK(!dvdi_has_dir(img, 2048, "VIDEO_TS"), "read past its length");
        CHECK(!dvdi_has_dir(img, 4096, "VIDEO"), "a name's start taken for it");
    }

    /* ---- the test DVD's image */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        size_t n = 0;
        if (f && !fseek(f, 16 * 2048, SEEK_SET))
            n = fread(img, 2048, 5, f);
        CHECK(n == 5, "%s: sectors 16-20", argv[1]);
        if (n == 5) {
            CHECK(dvdi_volume(img, 5, &root, &len, &udf) && udf && root > 16 && len >= 2048,
                  "ISO 9660 and UDF: root %lu, %lu bytes, udf %d", (unsigned long)root,
                  (unsigned long)len, udf);
            if (len > 2 * 2048)
                len = 2 * 2048;
            n = 0;
            if (!fseek(f, (long)root * 2048, SEEK_SET))
                n = fread(img, 1, len, f);
            CHECK(n == len && dvdi_has_dir(img, len, "VIDEO_TS"), "VIDEO_TS in its root");
            CHECK(!dvdi_has_dir(img, len, "VIDEO_TS.IFO"), "a folder by a longer name");
        }
        if (f)
            fclose(f);
    }

    printf("%d passed, %d failed\n", passed, failed);
    if (!failed)
        printf("dvdiso tests passed\n");
    return failed ? 1 : 0;
}
