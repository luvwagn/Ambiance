/* vamos test for whdinst.c and unarc.c's checks: WHDLoad found (or not),
 * installed from an unpacked package whose layout is not assumed, its
 * other programs put beside it (one already there kept), WHDLoad.prf into
 * S:, Devs:Kickstarts made, the leftovers deleted; and the names an
 * archive may not use. Run in build/ with C:, S:, DEVS: and T: scratch
 * folders (T:AmigaMCE-WHDLoad made by the Makefile as a package would be). */
#include "../src/whdinst.h"
#include "../src/unarc.h"
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>

static int fails;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

void amce_log(const char *what, const char *detail)
{
    printf("  log: %s: %s\n", what, detail);
}

static LONG size_of(const char *f)
{
    BPTR fh = Open((CONST_STRPTR)f, MODE_OLDFILE);
    LONG n = -1;
    if (fh) {
        Seek(fh, 0, OFFSET_END);
        n = Seek(fh, 0, OFFSET_BEGINNING);
        Close(fh);
    }
    return n;
}

int main(void)
{
    char where[64], why[120], done[120];

    check("names: a plain one", unarc_safe_name("WHDLoad/C/WHDLoad"));
    check("names: a device", !unarc_safe_name("SYS:C/WHDLoad"));
    check("names: from the root", !unarc_safe_name("/C/WHDLoad"));
    check("names: a parent", !unarc_safe_name("WHDLoad//S/Startup-Sequence"));
    check("names: none", !unarc_safe_name(""));

    check("not installed yet", !whd_present(where, sizeof(where)) && !where[0]);
    check("installed from the package", whd_install_from(WHD_DIR, why, sizeof(why), done, sizeof(done)));
    printf("  done: %s; why: %s\n", done, why);
    check("C:WHDLoad there, as packed", size_of("C:WHDLoad") == 5);
    check("found now", whd_present(where, sizeof(where)) && !strcmp(where, "C:WHDLoad"));
    check("DIC beside it", size_of("C:DIC") == 3);
    check("an icon not copied", size_of("C:WHDLoad.info") < 0);
    check("C:Patcher already there kept", size_of("C:Patcher") == 1);
    check("S:WHDLoad.prf copied", size_of("S:WHDLoad.prf") == 4);
    check("DEVS:Kickstarts made", amce_exists("DEVS:Kickstarts"));
    check("said", strstr(done, "WHDLoad and 1 more") != NULL);

    whd_cleanup();
    check("the unpacked package deleted", !amce_exists(WHD_DIR));

    check("a package without WHDLoad: not installed", !whd_install_from("T:", why, sizeof(why), done,
                                                                         sizeof(done)) && why[0]);
    printf("%s\n", fails ? "SOME TESTS FAILED" : "all WHDLoad tests passed");
    return fails ? 20 : 0;
}
