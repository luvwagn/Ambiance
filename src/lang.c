/*
 * AmigaMCE - translations (locale.library catalogs)
 * Copyright (c) 2026 Brian King
 *
 * Text the user sees is written in English in the source, as _("...")
 * (or N_("...") in a table, translated where it is used). Its catalog ID
 * is a hash of that English text (FNV-1a, 31 bits), so the source needs no
 * list of numbers: tools/catalog.py finds every _() and N_(), writes
 * catalogs/AmigaMCE.cd from them and builds each language's .ct into
 * Catalogs/<language>/AmigaMCE.catalog. A string with no translation, or
 * no catalog at all, is the English itself.
 *
 * The catalog is the Workbench's language (Locale prefs), or the one
 * chosen in Settings > General. Lookups go through a small cache by the
 * string's address: the English is a literal, at one address for good.
 */
#include "amce.h"

#include <string.h>
#include <proto/exec.h>
#include <proto/locale.h>
#include <libraries/locale.h>

struct LocaleBase *LocaleBase;
static struct Catalog *cat;

#define TR_CACHE 512
static struct {
    const char *en, *tr;
} cache[TR_CACHE];

ULONG amce_tr_id(const char *s)
{
    ULONG h = 2166136261UL;
    const UBYTE *p = (const UBYTE *)s;
    while (*p)
        h = (h ^ *p++) * 16777619UL;
    return h & 0x7FFFFFFFUL;
}

const char *amce_tr(const char *s)
{
    ULONG k;
    if (!cat || !s || !*s)
        return s;
    k = ((ULONG)s >> 1) & (TR_CACHE - 1);
    /* the network and radio processes ask too: one at a time at the cache */
    Forbid();
    if (cache[k].en != s) {
        cache[k].en = s;
        cache[k].tr = (const char *)GetCatalogStr(cat, (LONG)amce_tr_id(s), (STRPTR)s);
    }
    s = cache[k].tr;
    Permit();
    return s;
}

/* The catalog for lang (an Amiga language name: "deutsch"), or the
 * Workbench's (NULL or ""), or none ("english"). */
void amce_lang_open(const char *lang)
{
    char m[80];
    amce_lang_close();
    if (!LocaleBase && !(LocaleBase = (struct LocaleBase *)OpenLibrary((CONST_STRPTR) "locale.library", 38)))
        return;
    if (lang && !strcmp(lang, "english"))
        return;
    cat = OpenCatalog(NULL, (STRPTR) "AmigaMCE.catalog", OC_BuiltInLanguage, (ULONG) "english",
                      lang && *lang ? OC_Language : TAG_IGNORE, (ULONG)lang, TAG_END);
    if (cat) {
        amce_strlcpy(m, (const char *)cat->cat_Language, sizeof(m));
        amce_log("language", m);
    } else {
        amce_log("language", lang && *lang ? "no catalog for it: English" : "English");
    }
}

void amce_lang_close(void)
{
    if (cat && LocaleBase)
        CloseCatalog(cat);
    cat = NULL;
    memset(cache, 0, sizeof(cache));
}

void amce_lang_quit(void)
{
    amce_lang_close();
    if (LocaleBase)
        CloseLibrary((struct Library *)LocaleBase);
    LocaleBase = NULL;
}

/* The catalog's language (its name as Locale knows it), "" if English. */
const char *amce_lang_now(void)
{
    return cat ? (const char *)cat->cat_Language : "";
}
