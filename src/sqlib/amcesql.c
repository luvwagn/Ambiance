/* amcesql.library: SQLite for AmigaMCE, read only.
 *
 * AmigaMCE reads a beets music library (a SQLite 3 database, often on a
 * file share) with it, and opens it only when one is set up, from
 * PROGDIR:Libs, so the program itself stays small. Inside is SQLite 3.34.0
 * as built for AmigaOS 3 by Steffen Gutmann (Aminet: biz/dbase/
 * sqlite-3.34.0.a-amiga; its libsqlite3.a, public domain like SQLite).
 *
 * Every database is opened read only and immutable: no locks taken and
 * nothing written, so a database on a share is never touched (the share
 * has no byte-range locks, and the port would make .lock files). That
 * holds only while beets is not writing it at the same moment.
 *
 * The API is SQLite's own prepare/step/column, through the registers:
 * the caller needs no SQLite headers. Rows come one at a time from
 * SqlStep; text columns stay valid until the next step or finalize.
 *
 * Memory: the page cache is 256 KB where there is under 8 MB of fast RAM,
 * 1 MB otherwise (SQLite's own is 2 MB), and sorting is done in memory
 * (small: a list of names). Everything comes from exec through malloc
 * below, counted (SqlMemory). SqlRelease gives back an open database's
 * cache; closed, nothing is held but the library's code, which exec
 * expunges as any library's: when memory runs short, or when AmigaMCE
 * asks (RemLibrary) before a game or a module, with no opener left.
 *
 * One AmigaMCE at a time: the library's data is shared by its openers
 * (libnix's libinit, not the per-opener libinitr: libsqlite3.a is not
 * base-relative), and SQLite is used from one process.
 *
 * (c) 2026 Brian King, MIT. SQLite: public domain. */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <time.h>
#include "stabs.h"
#include "sqlite3.h"

const char LibName[] = "amcesql.library";
const char LibIdString[] = "amcesql 1.0 (07.11.2026) SQLite 3.34.0, read only, for AmigaMCE";
const UWORD LibVersion = 1;
const UWORD LibRevision = 0;

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
/* libnix's maths and utility stubs call through these; in a program its
 * startup opens them, in a library nobody does: __UserLibInit */
extern struct Library *MathIeeeDoubBasBase, *UtilityBase;

/* ---- the C library pieces SQLite calls, the library's own: libnix's
 * need its startup, which a library does not run. Memory comes from
 * exec, a size kept in front for realloc, and counted. */
static ULONG mem_now, mem_peak;

void *malloc(size_t n)
{
    ULONG *p = AllocVec(n + 8, MEMF_ANY);
    if (!p)
        return NULL;
    p[0] = n;
    if ((mem_now += n) > mem_peak)
        mem_peak = mem_now;
    return p + 2;                           /* 8-byte aligned past the header */
}

void free(void *p)
{
    if (p) {
        mem_now -= ((ULONG *)p)[-2];
        FreeVec((ULONG *)p - 2);
    }
}

void *realloc(void *p, size_t n)
{
    void *q;
    ULONG old;
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    old = ((ULONG *)p)[-2];
    if (n <= old)
        return p;
    if (!(q = malloc(n)))
        return NULL;
    memcpy(q, p, old);
    free(p);
    return q;
}

/* The port's OS layer has a putc() left in (its debugging, quiet): a
 * stream that takes nothing, so libnix's stdio, which wants its startup,
 * stays out. */
char __sF[256];
int __swbuf(int c, void *f)
{
    (void)f;
    return c;
}

int atexit(void (*f)(void))
{
    (void)f;
    return 0;                               /* nothing runs at a library's "exit" */
}

/* Only date functions with the 'localtime' modifier ask: the Amiga keeps
 * local time already, so it is the time as given. */
struct tm *localtime(const time_t *t)
{
    static struct tm tm;
    time_t s = *t;
    LONG days = s / 86400, rem = s % 86400, y = 1970;
    static const UBYTE mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int m;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    tm.tm_hour = rem / 3600;
    tm.tm_min = rem % 3600 / 60;
    tm.tm_sec = rem % 60;
    tm.tm_wday = (int)((days + 4) % 7);
    for (;;) {
        int ylen = (y % 4 == 0 && (y % 100 || y % 400 == 0)) ? 366 : 365;
        if (days < ylen)
            break;
        days -= ylen;
        y++;
    }
    tm.tm_year = y - 1900;
    tm.tm_yday = days;
    for (m = 0; m < 12; m++) {
        int ml = mdays[m] + (m == 1 && (y % 4 == 0 && (y % 100 || y % 400 == 0)));
        if (days < ml)
            break;
        days -= ml;
    }
    tm.tm_mon = m;
    tm.tm_mday = days + 1;
    tm.tm_isdst = 0;
    return &tm;
}

/* ---- the library */
void __UserLibCleanup(void);
int __UserLibInit(struct Library *lib)
{
    (void)lib;
    SysBase = *(struct ExecBase *volatile *)4L;
    if (!(DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37)) ||
        !(MathIeeeDoubBasBase = OpenLibrary((CONST_STRPTR)"mathieeedoubbas.library", 34)) ||
        !(UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 37))) {
        __UserLibCleanup();
        return 1;
    }
    return 0;
}

void __UserLibCleanup(void)
{
    if (DOSBase)
        sqlite3_shutdown();
    if (UtilityBase)
        CloseLibrary(UtilityBase);
    if (MathIeeeDoubBasBase)
        CloseLibrary(MathIeeeDoubBasBase);
    if (DOSBase)
        CloseLibrary((struct Library *)DOSBase);
    UtilityBase = MathIeeeDoubBasBase = NULL;
    DOSBase = NULL;
}

/* A path as a URI's: what a URI would read otherwise (% ? #) escaped. */
static char *uri_for(const char *path)
{
    size_t n = strlen(path), i, o = 0;
    char *u = malloc(n * 3 + 32);
    static const char hex[] = "0123456789ABCDEF";
    if (!u)
        return NULL;
    memcpy(u, "file:", 5);
    o = 5;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '%' || c == '?' || c == '#') {
            u[o++] = '%';
            u[o++] = hex[c >> 4];
            u[o++] = hex[c & 15];
        } else
            u[o++] = (char)c;
    }
    strcpy(u + o, "?immutable=1");
    return u;
}

static char lasterr[160];

APTR SqlOpen(STRPTR path)
{
    sqlite3 *db = NULL;
    char *uri;
    int rc;
    lasterr[0] = 0;
    if (sqlite3_initialize() != SQLITE_OK || !(uri = uri_for((const char *)path)))
        return NULL;
    rc = sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
    free(uri);
    if (rc != SQLITE_OK) {
        strncpy(lasterr, db ? sqlite3_errmsg(db) : "out of memory", sizeof(lasterr) - 1);
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_exec(db, AvailMem(MEMF_FAST | MEMF_TOTAL) < 8L << 20 ?
                 "PRAGMA cache_size=-256; PRAGMA temp_store=MEMORY" :
                 "PRAGMA cache_size=-1024; PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    return db;
}

VOID SqlClose(APTR db)
{
    sqlite3_close_v2((sqlite3 *)db);
}

APTR SqlPrepare(APTR db, STRPTR sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2((sqlite3 *)db, (const char *)sql, -1, &st, NULL) != SQLITE_OK)
        return NULL;
    return st;
}

LONG SqlBindText(APTR stmt, LONG index, STRPTR text)
{
    return sqlite3_bind_text((sqlite3_stmt *)stmt, (int)index, (const char *)text, -1,
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

/* 1: a row, 0: no more, -1: an error (SqlError says which) */
LONG SqlStep(APTR stmt)
{
    int rc = sqlite3_step((sqlite3_stmt *)stmt);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

LONG SqlColumns(APTR stmt)
{
    return sqlite3_column_count((sqlite3_stmt *)stmt);
}

STRPTR SqlText(APTR stmt, LONG column)
{
    return (STRPTR)sqlite3_column_text((sqlite3_stmt *)stmt, (int)column);
}

LONG SqlInt(APTR stmt, LONG column)
{
    return (LONG)sqlite3_column_int((sqlite3_stmt *)stmt, (int)column);
}

VOID SqlFinalize(APTR stmt)
{
    sqlite3_finalize((sqlite3_stmt *)stmt);
}

STRPTR SqlError(APTR db)
{
    return (STRPTR)(db ? sqlite3_errmsg((sqlite3 *)db) : lasterr);
}

STRPTR SqlVersion(void)
{
    return (STRPTR)sqlite3_libversion();
}

/* Bytes held now, or (peak) at the most so far. */
ULONG SqlMemory(LONG peak)
{
    return peak ? mem_peak : mem_now;
}

/* An open database's cache given back (it fills again as it is read). */
VOID SqlRelease(APTR db)
{
    if (db)
        sqlite3_db_release_memory((sqlite3 *)db);
}

/* the jump table, in the .sfd's order (from bias 30) */
ADDTABL_1(SqlOpen, a0);
ADDTABL_1(SqlClose, a0);
ADDTABL_2(SqlPrepare, a0, a1);
ADDTABL_3(SqlBindText, a0, d0, a1);
ADDTABL_1(SqlStep, a0);
ADDTABL_1(SqlColumns, a0);
ADDTABL_2(SqlText, a0, d0);
ADDTABL_2(SqlInt, a0, d0);
ADDTABL_1(SqlFinalize, a0);
ADDTABL_1(SqlError, a0);
ADDTABL_0(SqlVersion);
ADDTABL_1(SqlMemory, d0);
ADDTABL_1(SqlRelease, a0);
ADDTABL_END();
