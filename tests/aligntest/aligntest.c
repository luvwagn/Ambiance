/*
 * AlignTest - reproduction of the 18-byte list entry corruption. NOT part
 * of the build and NOT for running on a real machine: the 18- and 20-byte
 * variants corrupt memory by design.
 *
 * Finding: not alignment and not the emulator. With -Os -m68000, GCC turns
 * count * 18 (or * 20) into a __mulsi3 call and moves it between loading
 * CopyMem's a0/a1 and the call; __mulsi3 loads UtilityBase into a0, so
 * CopyMem copies from utility.library onto the stack. Under vamos the
 * 18-byte run stops with "AllocPooled: invalid memory pool". 16-byte
 * entries make the multiply a shift and pass. tools/check_syscalls.py
 * finds this pattern in compiled code.
 *
 * The list code is a copy of AmigaMCE's (src/ags.c list_add / list_sort),
 * instantiated once per entry layout. Results: PROGDIR:AlignTest.log and a
 * console window.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#define N      300
#define ROUNDS 20

static BPTR out_fh, log_fh;
static char line[200];

static void say(void)
{
    if (out_fh) {
        FPuts(out_fh, (CONST_STRPTR)line);
        Flush(out_fh);
    }
    if (log_fh) {
        FPuts(log_fh, (CONST_STRPTR)line);
        Flush(log_fh);
    }
}

static char *pool_strdup(APTR pool, const char *s)
{
    LONG n = strlen(s) + 1;
    char *d = AllocPooled(pool, n);
    if (d)
        CopyMem((APTR)s, d, n);
    return d;
}

/* ---- one layout per variant: the same fields, different tail */
struct E16 { char *label, *path, *sortkey; UBYTE kind, action, mark; };
struct E18 { char *label, *path, *sortkey; UBYTE kind, action, mark; UWORD width; };
struct E20 { char *label, *path, *sortkey; UBYTE kind, action, mark; UWORD width; UWORD pad; };

#define HAS_WIDTH_E16 0
#define HAS_WIDTH_E18 1
#define HAS_WIDTH_E20 1

#define DEFINE_VARIANT(T, HASW)                                                       \
struct L_##T { APTR pool; struct T *items; LONG count, cap; };                         \
static struct T *add_##T(struct L_##T *l, const char *label, const char *path,        \
                         const char *sortkey, UBYTE kind)                              \
{                                                                                      \
    struct T *it;                                                                      \
    if (l->count == l->cap) {                                                          \
        LONG ncap = l->cap ? l->cap * 2 : 64;                                          \
        struct T *n = AllocPooled(l->pool, ncap * sizeof(struct T));                   \
        if (!n)                                                                        \
            return NULL;                                                               \
        if (l->items)                                                                  \
            CopyMem(l->items, n, l->count * sizeof(struct T));                         \
        l->items = n;                                                                  \
        l->cap = ncap;                                                                 \
    }                                                                                  \
    it = &l->items[l->count];                                                          \
    it->label = pool_strdup(l->pool, label);                                           \
    it->path = pool_strdup(l->pool, path);                                             \
    it->sortkey = pool_strdup(l->pool, sortkey);                                       \
    it->kind = kind;                                                                   \
    it->action = 0;                                                                    \
    it->mark = 0;                                                                      \
    SETW_##T(it, 0);                                                                   \
    if (!it->label || !it->path || !it->sortkey)                                       \
        return NULL;                                                                   \
    l->count++;                                                                        \
    return it;                                                                         \
}                                                                                      \
static void sort_##T(struct L_##T *l)                                                  \
{                                                                                      \
    struct T *tmp, *src, *dst;                                                         \
    LONG n = l->count, w, i;                                                           \
    if (n < 2)                                                                         \
        return;                                                                        \
    tmp = AllocPooled(l->pool, n * sizeof(struct T));                                  \
    if (!tmp)                                                                          \
        return;                                                                        \
    src = l->items;                                                                    \
    dst = tmp;                                                                         \
    for (w = 1; w < n; w *= 2) {                                                       \
        for (i = 0; i < n; i += 2 * w) {                                               \
            LONG lo = i, mid = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n; \
            LONG a = lo, b = mid, k = lo;                                              \
            while (a < mid && b < hi)                                                  \
                dst[k++] = strcmp(src[a].sortkey, src[b].sortkey) <= 0 ? src[a++] : src[b++]; \
            while (a < mid)                                                            \
                dst[k++] = src[a++];                                                   \
            while (b < hi)                                                             \
                dst[k++] = src[b++];                                                   \
        }                                                                              \
        { struct T *t = src; src = dst; dst = t; }                                     \
    }                                                                                  \
    if (src != l->items)                                                               \
        CopyMem(src, l->items, n * sizeof(struct T));                                  \
}                                                                                      \
/* returns the number of bad entries over all rounds */                                \
static LONG run_##T(const char *name)                                                  \
{                                                                                      \
    LONG round, i, bad = 0, shown = 0;                                                 \
    char lab[32];                                                                      \
    for (round = 0; round < ROUNDS; round++) {                                         \
        struct L_##T l, other;                                                         \
        /* a small list made and freed first, as Settings is in AmigaMCE */            \
        other.pool = CreatePool(MEMF_ANY, 8192, 4096);                                 \
        other.items = NULL; other.count = other.cap = 0;                               \
        for (i = 0; i < 9; i++) {                                                      \
            struct T *o = add_##T(&other, "Setting", "x", "Setting", 4);               \
            if (o) o->mark = (UBYTE)(2 + (i & 3));                                     \
        }                                                                              \
        DeletePool(other.pool);                                                        \
        l.pool = CreatePool(MEMF_ANY, 8192, 4096);                                     \
        l.items = NULL; l.count = l.cap = 0;                                           \
        for (i = 0; i < N; i++) {                                                      \
            sprintf(lab, "Game %03ld", (long)(N - 1 - i));                             \
            add_##T(&l, lab, lab, lab, 2);                                             \
        }                                                                              \
        sort_##T(&l);                                                                  \
        for (i = 0; i < l.count; i++)                                                  \
            SETW_##T(&l.items[i], (UWORD)(strlen(l.items[i].label) * 7 + 1));          \
        for (i = 0; i < l.count; i++) {                                                \
            struct T *it = &l.items[i];                                                \
            sprintf(lab, "Game %03ld", (long)i);                                       \
            if (strcmp(it->label, lab) || strcmp(it->path, lab) || it->kind != 2 ||    \
                it->mark != 0 || GETW_##T(it) != (UWORD)(strlen(lab) * 7 + 1)) {       \
                bad++;                                                                 \
                if (shown++ < 3) {                                                     \
                    sprintf(line, "  %s round %ld entry %ld at $%08lx (addr mod 4 = %ld):" \
                            " kind %ld mark %ld label ptr $%08lx\\n", name, (long)round,  \
                            (long)i, (unsigned long)it, (long)((ULONG)it & 3),         \
                            (long)it->kind, (long)it->mark, (unsigned long)it->label); \
                    say();                                                             \
                }                                                                      \
            }                                                                          \
        }                                                                              \
        if (l.count != N)                                                              \
            bad += N - l.count;                                                        \
        DeletePool(l.pool);                                                            \
    }                                                                                  \
    return bad;                                                                        \
}

#define SETW_E16(it, v) ((void)0)
#define GETW_E16(it) ((UWORD)(strlen((it)->label) * 7 + 1))
#define SETW_E18(it, v) ((it)->width = (v))
#define GETW_E18(it) ((it)->width)
#define SETW_E20(it, v) ((it)->width = (v))
#define GETW_E20(it) ((it)->width)

DEFINE_VARIANT(E16, 0)
DEFINE_VARIANT(E18, 1)
DEFINE_VARIANT(E20, 1)

int main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    LONG b16, b18, b20;
    BOOL own_con = FALSE;

    if (me->pr_CLI)
        out_fh = Output();
    else if ((out_fh = Open((CONST_STRPTR)"CON:40/30/560/180/AlignTest/CLOSE/WAIT", MODE_NEWFILE)))
        own_con = TRUE;
    log_fh = Open((CONST_STRPTR)"PROGDIR:AlignTest.log", MODE_NEWFILE);

    sprintf(line, "AlignTest: %ld entries, %ld rounds per layout\n", (long)N, (long)ROUNDS);
    say();
    sprintf(line, "sizes: 16-byte layout %ld, 18-byte layout %ld, 20-byte layout %ld\n",
            (long)sizeof(struct E16), (long)sizeof(struct E18), (long)sizeof(struct E20));
    say();
    sprintf(line, "CPU flags $%04lx, exec %ld.%ld\n", (unsigned long)SysBase->AttnFlags,
            (long)SysBase->LibNode.lib_Version, (long)SysBase->SoftVer);
    say();
    b16 = run_E16("16");
    sprintf(line, "16-byte entries: %s (%ld bad)\n", b16 ? "FAIL" : "ok", (long)b16);
    say();
    b18 = run_E18("18");
    sprintf(line, "18-byte entries: %s (%ld bad)\n", b18 ? "FAIL" : "ok", (long)b18);
    say();
    b20 = run_E20("20");
    sprintf(line, "20-byte entries: %s (%ld bad)\n", b20 ? "FAIL" : "ok", (long)b20);
    say();
    sprintf(line, "done\n");
    say();
    if (log_fh)
        Close(log_fh);
    if (own_con)
        Close(out_fh);
    return (b16 || b18 || b20) ? 5 : 0;
}
