/*
 * Ambiance - translations (catalogs)
 * Copyright (c) 2026 Brian King
 *
 * Text the user sees is written in English in the source, as _("...")
 * (or N_("...") in a table, translated where it is used). Its catalog ID
 * is a hash of that English text (FNV-1a, 31 bits), so the source needs no
 * list of numbers: tools/catalog.py finds every _() and N_(), writes
 * catalogs/Ambiance.cd from them and builds each language's .ct into
 * Catalogs/<language>/Ambiance.catalog. A string with no translation, or
 * no catalog at all, is the English itself.
 *
 * The catalogs are locale.library's (IFF CTLG), but read here: each found
 * in PROGDIR:Catalogs or LOCALE:Catalogs is known by the language written
 * in it (its LANG chunk), not by its drawer's name, which a host folder
 * shared with an emulator can spell otherwise ("français"). The one used
 * is the Workbench's first language that has one (Locale prefs), or the
 * one chosen in Settings > General. Its strings are kept sorted by ID and
 * found by a binary search, behind a small cache by the English's address
 * (a literal, at one address for good).
 */
#include "amb.h"

#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>
#include <libraries/locale.h>

struct LocaleBase *LocaleBase;

#define MAX_CATS 16
static struct {
    char lang[32];
    char path[120];
} cats[MAX_CATS];
static int ncats;

static UBYTE *cat_buf;          /* the catalog in use, whole */
static ULONG cat_size;
static ULONG *cat_ids;          /* its strings' IDs, sorted, */
static const char **cat_strs;   /* and the strings */
static ULONG cat_n;
static char cat_lang[32];

#define TR_CACHE 512
static struct {
    const char *en, *tr;
} cache[TR_CACHE];

ULONG amb_tr_id(const char *s)
{
    ULONG h = 2166136261UL;
    const UBYTE *p = (const UBYTE *)s;
    while (*p)
        h = (h ^ *p++) * 16777619UL;
    return h & 0x7FFFFFFFUL;
}

static const char *lookup(ULONG id)
{
    LONG lo = 0, hi = (LONG)cat_n - 1;
    while (lo <= hi) {
        LONG mid = (lo + hi) >> 1;
        if (cat_ids[mid] == id)
            return cat_strs[mid];
        if (cat_ids[mid] < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

const char *amb_tr(const char *s)
{
    ULONG k;
    const char *t;
    if (!cat_n || !s || !*s)
        return s;
    k = ((ULONG)s >> 1) & (TR_CACHE - 1);
    /* the network and playback processes ask too: one at a time at the cache */
    Forbid();
    if (cache[k].en != s) {
        cache[k].en = s;
        t = lookup(amb_tr_id(s));
        /* at most twice its English and 16 bytes more: what the buffers
         * text is formatted into are sized for (a Japanese character is
         * three bytes); a longer one is left in English */
        if (t && strlen(t) > 2 * strlen(s) + 16)
            t = NULL;
        cache[k].tr = t ? t : s;
    }
    s = cache[k].tr;
    Permit();
    return s;
}

/* The language written in a catalog file (its LANG chunk), into lang. */
static BOOL cat_language(const char *path, char *lang)
{
    UBYTE b[256];
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG n, i;
    lang[0] = 0;
    if (!fh)
        return FALSE;
    n = Read(fh, b, sizeof(b));
    Close(fh);
    if (n < 20 || memcmp(b, "FORM", 4) || memcmp(b + 8, "CTLG", 4))
        return FALSE;
    for (i = 12; i + 8 <= n;) {
        ULONG len = amb_be32(b + i + 4);
        if (!memcmp(b + i, "LANG", 4)) {
            ULONG k;
            for (k = 0; k < len && k < 31 && i + 8 + (LONG)k < n && b[i + 8 + k]; k++)
                lang[k] = (char)b[i + 8 + k];
            lang[k] = 0;
            return lang[0] != 0;
        }
        i += 8 + len + (len & 1);
    }
    return FALSE;
}

static void scan_dir(const char *dir)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR l;
    if (!fib)
        return;
    if ((l = Lock((CONST_STRPTR)dir, SHARED_LOCK))) {
        if (Examine(l, fib))
            while (ExNext(l, fib) && ncats < MAX_CATS) {
                char path[120], lang[32];
                int i;
                if (fib->fib_DirEntryType <= 0)
                    continue;
                sprintf(path, "%s/%.40s/Ambiance.catalog", dir, (char *)fib->fib_FileName);
                if (!cat_language(path, lang)) {
                    /* LOCALE:'s drawers are every program's: most have no
                     * Ambiance.catalog, as they should not. Ours (PROGDIR's)
                     * should: said, once a run each (this is scanned again
                     * whenever Settings > General is drawn) */
                    static char said[MAX_CATS][32];
                    static int nsaid;
                    int k;
                    if (strncmp(dir, "PROGDIR:", 8))
                        continue;
                    for (k = 0; k < nsaid && strcmp(said[k], (char *)fib->fib_FileName); k++)
                        ;
                    if (k < nsaid || nsaid >= MAX_CATS)
                        continue;
                    amb_strlcpy(said[nsaid++], (char *)fib->fib_FileName, sizeof(said[0]));
                    /* say what the drawer is called, byte by byte where not
                     * ASCII (a host folder can name it otherwise) */
                    char m[160], *o = m;
                    const UBYTE *q = (const UBYTE *)fib->fib_FileName;
                    for (; *q && o < m + 100; q++)
                        if (*q < 0x80)
                            *o++ = (char)*q;
                        else
                            o += sprintf(o, "<%02lx>", (unsigned long)*q);
                    strcpy(o, ": no catalog read from it");
                    amb_log("language drawer", m);
                    continue;
                }
                for (i = 0; i < ncats && amb_stricmp(cats[i].lang, lang); i++)
                    ;
                if (i < ncats)
                    continue;                   /* PROGDIR's first */
                amb_strlcpy(cats[ncats].lang, lang, sizeof(cats[0].lang));
                amb_strlcpy(cats[ncats].path, path, sizeof(cats[0].path));
                ncats++;
            }
        UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
}

/* The catalogs there are, by their languages. */
void amb_lang_scan(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR old = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;            /* no LOCALE: is no requester */
    ncats = 0;
    scan_dir("PROGDIR:Catalogs");
    scan_dir("LOCALE:Catalogs");
    me->pr_WindowPtr = old;
}

int amb_lang_count(void)
{
    return ncats;
}

const char *amb_lang_name(int i)
{
    return i >= 0 && i < ncats ? cats[i].lang : "";
}

/* The Workbench's languages (Locale prefs), first first: n of them. */
static int wb_languages(char out[][32], int max)
{
    struct Locale *loc;
    int n = 0;
    if (!LocaleBase &&
        !(LocaleBase = (struct LocaleBase *)OpenLibrary((CONST_STRPTR) "locale.library", 38)))
        return 0;
    if ((loc = OpenLocale(NULL))) {
        for (; n < max && n < 10 && loc->loc_PrefLanguages[n]; n++)
            amb_strlcpy(out[n], (const char *)loc->loc_PrefLanguages[n], 32);
        CloseLocale(loc);
    }
    return n;
}

const char *amb_lang_wb(void)
{
    static char l[4][32];
    return wb_languages(l, 4) ? l[0] : "";
}

/* The same language by another name: Greek's catalog is "greek" (as
 * Workbench 3.x's Greek localisations call it), which some systems'
 * Locale prefs call "ellinika". */
static BOOL same_lang(const char *a, const char *b)
{
    static const char *const greek[] = { "greek", "ellinika" };
    int i, ia = -1, ib = -1;
    if (!amb_stricmp(a, b))
        return TRUE;
    for (i = 0; i < 2; i++) {
        if (!amb_stricmp(a, greek[i]))
            ia = i;
        if (!amb_stricmp(b, greek[i]))
            ib = i;
    }
    return ia >= 0 && ib >= 0;
}

static BOOL load(int k)
{
    BPTR fh;
    ULONG i, n = 0, cap;
    if (!(fh = Open((CONST_STRPTR)cats[k].path, MODE_OLDFILE)))
        return FALSE;
    Seek(fh, 0, OFFSET_END);
    cat_size = (ULONG)Seek(fh, 0, OFFSET_BEGINNING);
    if (cat_size < 20 || cat_size > 400000 || !(cat_buf = AllocVec(cat_size + 1, MEMF_ANY)) ||
        Read(fh, cat_buf, cat_size) != (LONG)cat_size) {
        Close(fh);
        amb_lang_close();
        return FALSE;
    }
    Close(fh);
    /* the STRS chunk: ID, length (its NUL counted as written), the string,
     * padded to 4 */
    for (i = 12; i + 8 <= cat_size;) {
        ULONG len = amb_be32(cat_buf + i + 4);
        if (!memcmp(cat_buf + i, "STRS", 4) && i + 8 + len <= cat_size) {
            UBYTE *p = cat_buf + i + 8, *e = p + len;
            cap = len / 12 + 1;
            cat_ids = AllocVec(cap * sizeof(ULONG), MEMF_ANY);
            cat_strs = AllocVec(cap * sizeof(char *), MEMF_ANY);
            if (!cat_ids || !cat_strs) {
                amb_lang_close();
                return FALSE;
            }
            while (p + 8 <= e && n < cap) {
                ULONG id = amb_be32(p), sl = amb_be32(p + 4), j;
                const char *s = (const char *)(p + 8);
                if (p + 8 + sl > e)
                    break;
                /* kept in ID order (written so, but any order is read) */
                for (j = n; j > 0 && cat_ids[j - 1] > id; j--) {
                    cat_ids[j] = cat_ids[j - 1];
                    cat_strs[j] = cat_strs[j - 1];
                }
                cat_ids[j] = id;
                cat_strs[j] = s;
                n++;
                p += 8 + ((sl + 3) & ~3UL);
            }
            break;
        }
        i += 8 + len + (len & 1);
    }
    cat_n = n;
    amb_strlcpy(cat_lang, cats[k].lang, sizeof(cat_lang));
    return n > 0;
}

/* The catalog for lang (a language as Locale names it: "deutsch"), or the
 * Workbench's first that has one (NULL or ""), or none ("english"). */
void amb_lang_open(const char *lang)
{
    char m[200];
    int i, k = -1;
    amb_lang_close();
    if (lang && !amb_stricmp(lang, "english")) {
        amb_log("language", "English, as chosen");
        return;
    }
    if (lang && *lang) {
        for (i = 0; i < ncats && k < 0; i++)
            if (same_lang(cats[i].lang, lang))
                k = i;
    } else {
        char wb[10][32];
        int n = wb_languages(wb, 10), j;
        for (j = 0; j < n && k < 0; j++) {
            if (!amb_stricmp(wb[j], "english"))
                break;                      /* English comes first there */
            for (i = 0; i < ncats && k < 0; i++)
                if (same_lang(cats[i].lang, wb[j]))
                    k = i;
        }
    }
    if (k >= 0 && load(k)) {
        sprintf(m, "%.31s (%lu strings, %.100s)", cat_lang, (unsigned long)cat_n, cats[k].path);
        amb_log("language", m);
    } else {
        amb_log("language", lang && *lang ? "no catalog for it: English" : "English");
    }
}

void amb_lang_close(void)
{
    if (cat_ids)
        FreeVec(cat_ids);
    if (cat_strs)
        FreeVec(cat_strs);
    if (cat_buf)
        FreeVec(cat_buf);
    cat_ids = NULL;
    cat_strs = NULL;
    cat_buf = NULL;
    cat_n = 0;
    cat_lang[0] = 0;
    memset(cache, 0, sizeof(cache));
}

void amb_lang_quit(void)
{
    amb_lang_close();
    if (LocaleBase)
        CloseLibrary((struct Library *)LocaleBase);
    LocaleBase = NULL;
}

/* The catalog's language, "" if English. */
const char *amb_lang_now(void)
{
    return cat_n ? cat_lang : "";
}

/* The language shown, as an ISO 639-1 code ("de"): for servers that can
 * answer in it (Plex's X-Plex-Language). */
const char *amb_lang_iso(void)
{
    static const struct { const char *name, *iso; } map[] = {
        { "deutsch", "de" }, { "fran\347ais", "fr" }, { "italiano", "it" },
        { "espa\361ol", "es" }, { "svenska", "sv" }, { "suomi", "fi" }, { "dansk", "da" },
        { "norsk", "nb" }, { "nederlands", "nl" }, { "portugu\352s", "pt" },
        { "polski", "pl" }, { "nihongo", "ja" }, { "greek", "el" }, { "ellinika", "el" },
        { "korean", "ko" },
    };
    unsigned i;
    if (!cat_n)
        return "en";
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!amb_stricmp(cat_lang, map[i].name))
            return map[i].iso;
    return "en";
}
