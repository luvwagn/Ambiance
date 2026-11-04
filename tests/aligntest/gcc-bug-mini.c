/* m68k-amigaos-gcc -Os -m68000 -mtune=68020-60 -S mini.c */
typedef unsigned long size_t;

struct Entry { char *a, *b, *c; unsigned char k, x, m; unsigned short w; };  /* 18 bytes */
struct Vec   { void *pool; struct Entry *items; long count, cap; };

extern void *SysBase;

/* CopyMem(a0 = source, a1 = dest, d0 = size), as in the NDK's inline/exec.h */
static inline void copy_mem(void *source, void *dest, unsigned long size)
{
    register void *a6 __asm("a6") = SysBase;
    register void *a0 __asm("a0") = source;
    register void *a1 __asm("a1") = dest;
    register unsigned long d0 __asm("d0") = size;
    __asm volatile ("jsr a6@(-624:W)"
                    : "+a" (a0), "+a" (a1), "+d" (d0)
                    : "a" (a6)
                    : "d1", "cc", "memory");
}

extern void *alloc(void *pool, unsigned long size);

struct Entry *add(struct Vec *v, char *s)
{
    struct Entry *e;
    if (v->count == v->cap) {
        long ncap = v->cap ? v->cap * 2 : 64;
        struct Entry *n = alloc(v->pool, ncap * sizeof(struct Entry));
        if (!n)
            return 0;
        if (v->items)
            copy_mem(v->items, n, v->count * sizeof(struct Entry));
        v->items = n;
        v->cap = ncap;
    }
    e = &v->items[v->count];
    e->a = s;
    v->count++;
    return e;
}
