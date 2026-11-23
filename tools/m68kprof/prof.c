/*
 * m68kprof - runs m68kprof's 68k program (t68k: jdec and shrink.c, built
 * as Ambiance builds them) under Musashi and counts, per function:
 * instructions, an estimate of 68060 issue cycles (its two pipelines
 * modelled roughly: simple instructions pair when independent), branch
 * mispredictions (a 2-bit counter per branch, as the 060's branch cache
 * keeps), and data and instruction cache misses (8 KB each, 4-way, 16-byte
 * lines, LRU, the data cache copyback with write allocation).
 *
 *   m68kprof t68k photo.jpg box_w box_h [out.rgb]
 *
 * Musashi (c) Karl Stenerud, MIT. Copyright (c) 2026 Brian King, MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "m68k.h"

#define MEM_SIZE (96u << 20)
#define BOX 0x400
#define MARK 0x500
#define END 0x504
#define LOAD_AT 0x10000
static uint8_t *mem;
static int done;

/* ---------------------------------------------------------------- symbols */
struct Sym {
    uint32_t addr;
    char name[64];
};
static struct Sym *syms;
static int nsyms;
static uint32_t code_lo, code_hi;   /* the code hunk */
static uint32_t entry;              /* _start (else the code's first byte) */

static int sym_cmp(const void *a, const void *b)
{
    uint32_t x = ((const struct Sym *)a)->addr, y = ((const struct Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

static int sym_of(uint32_t pc)
{
    int lo = 0, hi = nsyms - 1, r = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (syms[m].addr <= pc) {
            r = m;
            lo = m + 1;
        } else {
            hi = m - 1;
        }
    }
    return r;
}

/* ---------------------------------------------------------------- hunks */
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void w32(uint32_t a, uint32_t v)
{
    mem[a] = v >> 24;
    mem[a + 1] = v >> 16;
    mem[a + 2] = v >> 8;
    mem[a + 3] = v;
}

static uint32_t r32(uint32_t a)
{
    return be32(mem + a);
}

static uint32_t load_hunks(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    uint32_t pos = 0, nh, first, last, i, base[64], size[64], at = LOAD_AT;
    int cur = -1;
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n)
        exit(1);
    fclose(f);
#define RD() (pos += 4, be32(b + pos - 4))
    if ((RD() & 0x3FFFFFFF) != 0x3F3) {
        fprintf(stderr, "not a hunk file\n");
        exit(1);
    }
    while (RD())                         /* resident library names: none */
        ;
    nh = RD();
    first = RD();
    last = RD();
    (void)nh;
    for (i = first; i <= last; i++) {
        size[i] = (RD() & 0x3FFFFFFF) * 4;
        base[i] = at;
        at = (at + size[i] + 15) & ~15u;
    }
    while (pos < (uint32_t)n) {
        uint32_t t = RD() & 0x3FFFFFFF, k;
        switch (t) {
        case 0x3E9:                     /* CODE */
        case 0x3EA:                     /* DATA */
            cur = cur < 0 ? (int)first : cur;
            k = RD() * 4;
            memcpy(mem + base[cur], b + pos, k);
            if (t == 0x3E9) {
                code_lo = base[cur];
                code_hi = base[cur] + size[cur];
            }
            pos += k;
            break;
        case 0x3EB:                     /* BSS */
            cur = cur < 0 ? (int)first : cur;
            RD();
            break;
        case 0x3EC:                     /* RELOC32 */
            for (;;) {
                uint32_t cnt = RD(), h;
                if (!cnt)
                    break;
                h = RD();
                while (cnt--) {
                    uint32_t off = RD();
                    w32(base[cur] + off, r32(base[cur] + off) + base[h]);
                }
            }
            break;
        case 0x3FC:                     /* RELOC32SHORT */
        case 0x3F7: {
            uint32_t p2 = pos;
            for (;;) {
                uint32_t cnt = (uint32_t)b[p2] << 8 | b[p2 + 1], h;
                p2 += 2;
                if (!cnt)
                    break;
                h = (uint32_t)b[p2] << 8 | b[p2 + 1];
                p2 += 2;
                while (cnt--) {
                    uint32_t off = (uint32_t)b[p2] << 8 | b[p2 + 1];
                    p2 += 2;
                    w32(base[cur] + off, r32(base[cur] + off) + base[h]);
                }
            }
            pos = (p2 + 3) & ~3u;
            break;
        }
        case 0x3F0:                     /* SYMBOL */
            for (;;) {
                uint32_t nl = RD() & 0xFFFFFF;
                if (!nl)
                    break;
                syms = realloc(syms, sizeof(*syms) * (size_t)(nsyms + 1));
                memset(&syms[nsyms], 0, sizeof(*syms));
                memcpy(syms[nsyms].name, b + pos, nl * 4 < 63 ? nl * 4 : 63);
                pos += nl * 4;
                syms[nsyms].addr = base[cur] + RD();
                if (!strcmp(syms[nsyms].name, "_start"))
                    entry = syms[nsyms].addr;
                if (cur >= 0 && base[cur] == code_lo)
                    nsyms++;
            }
            break;
        case 0x3F1: {                   /* DEBUG */
            uint32_t k2 = RD();
            pos += k2 * 4;
            break;
        }
        case 0x3F2:                     /* END */
            cur++;
            break;
        default:
            fprintf(stderr, "hunk %x at %u: not known\n", t, pos - 4);
            exit(1);
        }
    }
    free(b);
    qsort(syms, (size_t)nsyms, sizeof(*syms), sym_cmp);
    return at;
}

/* ---------------------------------------------------------------- caches */
#define WAYS 4
#define SETS 128                        /* 8 KB / 16 / 4 */
struct Cache {
    uint32_t tag[SETS][WAYS];
    uint8_t valid[SETS][WAYS], dirty[SETS][WAYS], age[SETS][WAYS];
};
static struct Cache dc, ic;

/* 1: a miss; *wb: a dirty line written back */
static int cache_touch(struct Cache *c, uint32_t a, int write, int *wb)
{
    uint32_t line = a >> 4, set = line & (SETS - 1), tag = line >> 7;
    int w, hit = -1, victim = 0;
    for (w = 0; w < WAYS; w++)
        if (c->valid[set][w] && c->tag[set][w] == tag)
            hit = w;
    if (hit < 0) {
        for (w = 0; w < WAYS; w++) {
            if (!c->valid[set][w]) {
                victim = w;
                break;
            }
            if (c->age[set][w] > c->age[set][victim])
                victim = w;
        }
        if (wb)
            *wb = c->valid[set][victim] && c->dirty[set][victim];
        c->valid[set][victim] = 1;
        c->dirty[set][victim] = 0;
        c->tag[set][victim] = tag;
        hit = victim;
        for (w = 0; w < WAYS; w++)
            if (c->age[set][w] < 255)
                c->age[set][w]++;
        c->age[set][hit] = 0;
        if (write)
            c->dirty[set][hit] = 1;
        return 1;
    }
    for (w = 0; w < WAYS; w++)
        if (c->age[set][w] < c->age[set][hit])
            c->age[set][w]++;
    c->age[set][hit] = 0;
    if (write)
        c->dirty[set][hit] = 1;
    return 0;
}

/* ---------------------------------------------------------------- per PC */
enum { K_SIMPLE, K_BRANCH, K_UNCOND, K_HEAVY };
struct Ins {
    uint8_t known, kind, len, mem, cost;
    uint16_t wr, rd;                    /* registers written, read: D0-7 bits 0-7, A0-7 bits 8-15 */
    uint8_t pred;                       /* a branch's 2-bit counter */
};
struct PcCount {
    uint64_t n, cyc, mp, dmiss, dwb, imiss, dacc;
};
static struct Ins *ins;
static struct PcCount *pcc;

static uint16_t regs_in(const char *s)
{
    uint16_t m = 0;
    for (; *s; s++) {
        if ((s[0] == 'D' || s[0] == 'A') && s[1] >= '0' && s[1] <= '7' && !isalnum((unsigned char)s[2]) &&
            (s == 0 || !isalnum((unsigned char)s[-1])))
            m |= (uint16_t)(1u << ((s[0] == 'A' ? 8 : 0) + (s[1] - '0')));
        if (s[0] == 'S' && s[1] == 'P' && !isalnum((unsigned char)s[2]))
            m |= 1u << 15;
    }
    return m;
}

static int movem_count(const char *s)
{
    int n = 0, a, b;
    char c;
    while (*s) {
        if ((s[0] == 'D' || s[0] == 'A') && isdigit((unsigned char)s[1])) {
            c = s[0];
            a = s[1] - '0';
            s += 2;
            if (s[0] == '-' && (s[1] == 'D' || s[1] == 'A') && isdigit((unsigned char)s[2])) {
                b = s[2] - '0';
                n += (s[1] == c ? b - a : (7 - a) + 1 + b) + 1;
                s += 3;
            } else {
                n++;
            }
        } else {
            s++;
        }
    }
    return n ? n : 1;
}

static void classify(uint32_t pc)
{
    struct Ins *in = &ins[(pc - code_lo) >> 1];
    char buf[256], mn[32], *ops, *last;
    int i;
    in->len = (uint8_t)m68k_disassemble(buf, pc, M68K_CPU_TYPE_68040);
    for (i = 0; buf[i] && buf[i] != ' ' && i < 31; i++)
        mn[i] = (char)tolower((unsigned char)buf[i]);
    mn[i] = 0;
    ops = buf + i;
    while (*ops == ' ')
        ops++;
    last = strrchr(ops, ',');
    {
        /* the last operand, not a comma inside brackets */
        int depth = 0;
        char *p;
        last = NULL;
        for (p = ops; *p; p++) {
            if (*p == '(' || *p == '[')
                depth++;
            else if (*p == ')' || *p == ']')
                depth--;
            else if (*p == ',' && !depth)
                last = p + 1;
        }
        if (!last)
            last = ops;
        while (*last == ' ')
            last++;
    }
    in->mem = strchr(ops, '(') != NULL;
    in->rd = regs_in(ops);
    in->wr = 0;
    /* written: a plain register destination, or an address register stepped */
    if ((last[0] == 'D' || last[0] == 'A') && isdigit((unsigned char)last[1]) && !last[2])
        in->wr |= (uint16_t)(1u << ((last[0] == 'A' ? 8 : 0) + (last[1] - '0')));
    {
        char *p;
        for (p = ops; (p = strchr(p, '(')); p++)
            if ((p[1] == 'A') && isdigit((unsigned char)p[2]) && p[3] == ')' && (p[4] == '+' || (p > ops && p[-1] == '-')))
                in->wr |= (uint16_t)(1u << (8 + (p[2] - '0')));
    }
    if (!strncmp(mn, "move.", 5) || !strncmp(mn, "movea", 5)) {
        /* a move does not read its register destination */
        if ((last[0] == 'D' || last[0] == 'A') && isdigit((unsigned char)last[1]) && !last[2]) {
            uint16_t lastbit = (uint16_t)(1u << ((last[0] == 'A' ? 8 : 0) + (last[1] - '0')));
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%.*s", (int)(last - ops), ops);
            in->rd = regs_in(tmp);
            (void)lastbit;
        }
    }
    in->kind = K_SIMPLE;
    in->cost = 1;
    if (mn[0] == 'b' && strcmp(mn, "bra") && strncmp(mn, "bra.", 4) && strncmp(mn, "bset", 4) &&
        strncmp(mn, "bclr", 4) && strncmp(mn, "btst", 4) && strncmp(mn, "bchg", 4) && strncmp(mn, "bfext", 5) &&
        strncmp(mn, "bf", 2) && strncmp(mn, "bsr", 3))
        in->kind = K_BRANCH;
    else if (!strncmp(mn, "db", 2))
        in->kind = K_BRANCH;
    else if (!strncmp(mn, "bra", 3))
        in->kind = K_UNCOND;
    else if (!strncmp(mn, "jsr", 3) || !strncmp(mn, "bsr", 3) || !strncmp(mn, "rts", 3) ||
             !strncmp(mn, "jmp", 3)) {
        in->kind = K_HEAVY;
        in->cost = 2;
    } else if (!strncmp(mn, "mul", 3)) {
        in->kind = K_HEAVY;
        in->cost = 2;
    } else if (!strncmp(mn, "div", 3)) {
        in->kind = K_HEAVY;
        in->cost = strstr(mn, ".l") ? 38 : 22;
    } else if (!strncmp(mn, "movem", 5)) {
        in->kind = K_HEAVY;
        in->cost = (uint8_t)movem_count(ops);
    } else if (!strncmp(mn, "bf", 2)) {
        in->kind = K_HEAVY;             /* (bit fields: pOEP only, several cycles on a 68060) */
        in->cost = 6;
    } else if (!strncmp(mn, "link", 4) || !strncmp(mn, "unlk", 4) ||
               !strncmp(mn, "movep", 5) || !strncmp(mn, "trap", 4) || !strncmp(mn, "chk", 3)) {
        in->kind = K_HEAVY;
        in->cost = 2;
    }
    in->pred = 1;                       /* weakly not taken */
    in->known = 1;
}

/* the stream's state */
static uint32_t prev_pc;
static int prev_valid, prev_paired;
static uint64_t phase_n[8][7];
static int phase = 0;
static uint32_t cur_pc;

void prof_hook(unsigned int pc)
{
    struct Ins *in;
    struct PcCount *c;
    uint32_t cost;
    if (pc < code_lo || pc >= code_hi)
        return;
    in = &ins[(pc - code_lo) >> 1];
    if (!in->known)
        classify(pc);
    c = &pcc[(pc - code_lo) >> 1];
    c->n++;
    cur_pc = pc;
    /* the last instruction a branch: taken or not, predicted or not */
    if (prev_valid) {
        struct Ins *p = &ins[(prev_pc - code_lo) >> 1];
        if (p->kind == K_BRANCH) {
            int taken = pc != prev_pc + p->len, guess = p->pred >= 2;
            if (taken != guess)
                pcc[(prev_pc - code_lo) >> 1].mp++;
            if (taken && p->pred < 3)
                p->pred++;
            else if (!taken && p->pred > 0)
                p->pred--;
        }
    }
    /* issue: a simple instruction after an unpaired simple one, using none of
     * its results and not both at memory, goes in the same cycle */
    cost = in->cost;
    if (prev_valid && !prev_paired && (in->kind == K_SIMPLE || in->kind == K_BRANCH)) {
        struct Ins *p = &ins[(prev_pc - code_lo) >> 1];
        if (p->kind == K_SIMPLE && !(p->wr & in->rd) && !(p->wr & in->wr) && !(p->mem && in->mem)) {
            cost = 0;
            prev_paired = 1;
        } else {
            prev_paired = 0;
        }
    } else {
        prev_paired = 0;
    }
    c->cyc += cost;
    /* the instruction cache: its first line (and a second if it spans one) */
    if (cache_touch(&ic, pc, 0, NULL))
        c->imiss++;
    if (((pc + in->len - 1) >> 4) != (pc >> 4) && cache_touch(&ic, pc + in->len - 1, 0, NULL))
        c->imiss++;
    prev_pc = pc;
    prev_valid = 1;
}

static void data_access(uint32_t a, int write)
{
    int wb = 0;
    struct PcCount *c;
    if (!cur_pc)
        return;
    c = &pcc[(cur_pc - code_lo) >> 1];
    c->dacc++;
    if (cache_touch(&dc, a, write, &wb))
        c->dmiss++;
    if (wb)
        c->dwb++;
}

/* ---------------------------------------------------------------- Musashi's memory */
unsigned int m68k_read_memory_8(unsigned int a)
{
    if (a >= MEM_SIZE)
        return 0;
    data_access(a, 0);
    return mem[a];
}
unsigned int m68k_read_memory_16(unsigned int a)
{
    if (a + 1 >= MEM_SIZE)
        return 0;
    data_access(a, 0);
    return (unsigned)mem[a] << 8 | mem[a + 1];
}
unsigned int m68k_read_memory_32(unsigned int a)
{
    if (a + 3 >= MEM_SIZE)
        return 0;
    data_access(a, 0);
    if ((a & 15) > 12)
        data_access(a + 3, 0);
    return r32(a);
}
unsigned int m68k_read_immediate_16(unsigned int a)
{
    return (unsigned)mem[a] << 8 | mem[a + 1];
}
unsigned int m68k_read_immediate_32(unsigned int a)
{
    return r32(a);
}
unsigned int m68k_read_pcrelative_8(unsigned int a)
{
    data_access(a, 0);
    return mem[a];
}
unsigned int m68k_read_pcrelative_16(unsigned int a)
{
    data_access(a, 0);
    return (unsigned)mem[a] << 8 | mem[a + 1];
}
unsigned int m68k_read_pcrelative_32(unsigned int a)
{
    data_access(a, 0);
    return r32(a);
}
unsigned int m68k_read_disassembler_8(unsigned int a)
{
    return mem[a];
}
unsigned int m68k_read_disassembler_16(unsigned int a)
{
    return (unsigned)mem[a] << 8 | mem[a + 1];
}
unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return r32(a);
}
static void marked(uint32_t v);
void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    if (a >= MEM_SIZE)
        return;
    data_access(a, 1);
    mem[a] = (uint8_t)v;
}
void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    if (a + 1 >= MEM_SIZE)
        return;
    data_access(a, 1);
    mem[a] = (uint8_t)(v >> 8);
    mem[a + 1] = (uint8_t)v;
}
void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    if (a + 3 >= MEM_SIZE)
        return;
    if (a == MARK) {
        marked(v);
        return;
    }
    if (a == END) {
        done = 1;
        m68k_end_timeslice();
        return;
    }
    data_access(a, 1);
    if ((a & 15) > 12)
        data_access(a + 3, 1);
    w32(a, v);
}

/* ---------------------------------------------------------------- phases and totals */
static void totals(uint64_t t[7])
{
    uint32_t i, n = (code_hi - code_lo) / 2;
    memset(t, 0, sizeof(uint64_t) * 7);
    for (i = 0; i < n; i++) {
        t[0] += pcc[i].n;
        t[1] += pcc[i].cyc;
        t[2] += pcc[i].mp;
        t[3] += pcc[i].dmiss;
        t[4] += pcc[i].dwb;
        t[5] += pcc[i].imiss;
        t[6] += pcc[i].dacc;
    }
}

static void marked(uint32_t v)
{
    if (v < 8) {
        totals(phase_n[v]);
        phase = (int)v;
    }
}

struct Fn {
    int sym;
    uint64_t t[7];
};
static int fn_cmp(const void *a, const void *b)
{
    uint64_t x = ((const struct Fn *)a)->t[1], y = ((const struct Fn *)b)->t[1];
    return x < y ? 1 : x > y ? -1 : 0;
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    uint32_t top, in_at, heap, i, nfn = 0;
    struct Fn *fn;
    uint64_t all[7];
    double mp_pen = 7, miss_pen = 20, wb_pen = 10;
    if (argc < 5) {
        fprintf(stderr, "m68kprof t68k photo.jpg box_w box_h [out.rgb]\n");
        return 1;
    }
    if (getenv("PROF_MISS"))
        miss_pen = atof(getenv("PROF_MISS"));
    if (getenv("PROF_MP"))
        mp_pen = atof(getenv("PROF_MP"));
    wb_pen = miss_pen / 2;
    mem = calloc(1, MEM_SIZE);
    top = load_hunks(argv[1]);
    if (getenv("PROF_SYMS")) {
        /* every function, locals too (syms.py): offsets in the code hunk */
        FILE *sf = fopen(getenv("PROF_SYMS"), "r");
        char line[128];
        nsyms = 0;
        while (sf && fgets(line, sizeof(line), sf)) {
            unsigned off;
            char name[64];
            if (sscanf(line, "%x %63s", &off, name) == 2) {
                syms = realloc(syms, sizeof(*syms) * (size_t)(nsyms + 1));
                syms[nsyms].addr = code_lo + off;
                snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "%s", name);
                nsyms++;
            }
        }
        if (sf)
            fclose(sf);
        qsort(syms, (size_t)nsyms, sizeof(*syms), sym_cmp);
    }
    ins = calloc((code_hi - code_lo) / 2 + 1, sizeof(*ins));
    pcc = calloc((code_hi - code_lo) / 2 + 1, sizeof(*pcc));
    if (!(f = fopen(argv[2], "rb"))) {
        perror(argv[2]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    in_at = (top + 4095) & ~4095u;
    if (fread(mem + in_at, 1, (size_t)n, f) != (size_t)n)
        return 1;
    fclose(f);
    heap = (in_at + (uint32_t)n + 4095) & ~4095u;
    w32(BOX + 0, in_at);
    w32(BOX + 4, (uint32_t)n);
    w32(BOX + 8, (uint32_t)atoi(argv[3]));
    w32(BOX + 12, (uint32_t)atoi(argv[4]));
    w32(BOX + 44, heap);
    w32(BOX + 48, MEM_SIZE - (1u << 20));
    /* the reset vectors: the stack at the top, the program at its start */
    w32(0, MEM_SIZE - 64);
    w32(4, entry ? entry : code_lo);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68040);
    m68k_pulse_reset();
    while (!done)
        m68k_execute(1000000);
    totals(all);
    printf("result %d: %ux%u box %ux%u, jdec 1/%d to %ux%u, made %ux%u\n", (int)r32(BOX + 40), r32(BOX + 8),
           r32(BOX + 12), r32(BOX + 8), r32(BOX + 12), 1 << r32(BOX + 32), r32(BOX + 24), r32(BOX + 28), r32(BOX + 16),
           r32(BOX + 20));
    printf("instructions %llu, issue cycles %llu, mispredicts %llu, D misses %llu (writebacks %llu) of %llu accesses, "
           "I misses %llu\n",
           (unsigned long long)all[0], (unsigned long long)all[1], (unsigned long long)all[2],
           (unsigned long long)all[3], (unsigned long long)all[4], (unsigned long long)all[6],
           (unsigned long long)all[5]);
    printf("est. cycles (mispredict %.0f, miss %.0f, writeback %.0f): %.0f\n", mp_pen, miss_pen, wb_pen,
           all[1] + all[2] * mp_pen + (all[3] + all[5]) * miss_pen + all[4] * wb_pen);
    {
        /* the phases: 1 open, 2 decode (rows made small as they come), 3 the end */
        static const char *const pn[] = { "start", "open", "decode", "after" };
        int p;
        for (p = 1; p <= 3; p++) {
            uint64_t *a = phase_n[p], *b = p < 3 ? phase_n[p + 1] : all, d[7];
            int k;
            for (k = 0; k < 7; k++)
                d[k] = b[k] - a[k];
            printf("phase %-7s instr %12llu issue %12llu mispred %9llu D miss %9llu I miss %8llu est %12.0f\n",
                   pn[p], (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2],
                   (unsigned long long)d[3], (unsigned long long)d[5],
                   d[1] + d[2] * mp_pen + (d[3] + d[5]) * miss_pen + d[4] * wb_pen);
        }
    }
    /* per function */
    fn = calloc((size_t)nsyms + 1, sizeof(*fn));
    for (i = 0; i < (code_hi - code_lo) / 2; i++) {
        int s;
        uint32_t k;
        if (!pcc[i].n && !pcc[i].dacc)
            continue;
        s = sym_of(code_lo + i * 2);
        for (k = 0; k < nfn && fn[k].sym != s; k++)
            ;
        if (k == nfn)
            fn[nfn++].sym = s;
        fn[k].t[0] += pcc[i].n;
        fn[k].t[1] += pcc[i].cyc;
        fn[k].t[2] += pcc[i].mp;
        fn[k].t[3] += pcc[i].dmiss;
        fn[k].t[4] += pcc[i].dwb;
        fn[k].t[5] += pcc[i].imiss;
        fn[k].t[6] += pcc[i].dacc;
    }
    qsort(fn, nfn, sizeof(*fn), fn_cmp);
    printf("%-22s %12s %12s %10s %10s %9s %9s %12s\n", "function", "instr", "issue cyc", "mispred", "D miss",
           "D wb", "I miss", "est. cycles");
    for (i = 0; i < nfn && i < (getenv("PROF_ALL") ? nfn : 24u); i++) {
        double est = fn[i].t[1] + fn[i].t[2] * mp_pen + (fn[i].t[3] + fn[i].t[5]) * miss_pen + fn[i].t[4] * wb_pen;
        printf("%-22.22s %12llu %12llu %10llu %10llu %9llu %9llu %12.0f\n",
               fn[i].sym >= 0 ? syms[fn[i].sym].name : "?", (unsigned long long)fn[i].t[0],
               (unsigned long long)fn[i].t[1], (unsigned long long)fn[i].t[2], (unsigned long long)fn[i].t[3],
               (unsigned long long)fn[i].t[4], (unsigned long long)fn[i].t[5], est);
    }
    if (getenv("PROF_HOT")) {
        /* the hottest instructions, disassembled */
        uint32_t want = (uint32_t)atoi(getenv("PROF_HOT")), k;
        for (k = 0; k < want; k++) {
            uint32_t best = 0, bi = 0;
            char buf[256];
            for (i = 0; i < (code_hi - code_lo) / 2; i++)
                if (pcc[i].n > best) {
                    best = (uint32_t)pcc[i].n;
                    bi = i;
                }
            if (!best)
                break;
            m68k_disassemble(buf, code_lo + bi * 2, M68K_CPU_TYPE_68040);
            printf("  %08x %-14.14s %10u  %s\n", code_lo + bi * 2, syms[sym_of(code_lo + bi * 2)].name, best, buf);
            pcc[bi].n = 0;
        }
    }
    if (argc > 5) {
        uint32_t o = r32(BOX + 36), sz = r32(BOX + 16) * r32(BOX + 20) * 3;
        FILE *g = fopen(argv[5], "wb");
        if (g && o) {
            fwrite(mem + o, 1, sz, g);
            fclose(g);
        }
    }
    return 0;
}
