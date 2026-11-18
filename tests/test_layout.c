/* The layout libraries as the program finds and opens them (lhost.c), on
 * a 68020 under vamos: PROGDIR:Layouts holds showcase, horizon and wrongabi
 * (built against another layout.h, never to be used), named .library: the
 * only names vamos opens (LAYOUT_EXT). */
#ifndef LAYOUT_EXT
#define LAYOUT_EXT ".layout"
#endif
#include <stdio.h>
#include <string.h>
#include "../src/amb.h"
#include "../src/lhost.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

void amb_log(const char *what, const char *detail)
{
    printf("log %s: %s\n", what, detail);
}

/* the listed layout in file, or -1 (the drawer's order is the disk's) */
static int listed(const struct LayoutInfo *li, int n, const char *file)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(li[i].file, file))
            return i;
    return -1;
}

int main(void)
{
    struct LayoutInfo li[8];
    const struct LayoutEngine *e;
    int n = layout_list(li, 8), s = listed(li, n, "showcase" LAYOUT_EXT), h = listed(li, n, "horizon" LAYOUT_EXT);
    CHECK(n == 2);
    CHECK(s >= 0 && !strcmp(li[s].name, "Showcase") && li[s].min_bits == 15);
    CHECK(h >= 0 && !strcmp(li[h].name, "Horizon") && li[h].min_bits == 15);
    CHECK(layout_loaded() == NULL);

    e = layout_load("showcase" LAYOUT_EXT);
    CHECK(e != NULL);
    CHECK(e && e->abi == (ULONG)LAYOUT_ABI && !strcmp(e->name, "Showcase"));
    CHECK(layout_loaded() == e);
    /* listing again with one open keeps it open */
    CHECK(layout_list(li, 8) == 2);
    CHECK(layout_loaded() == e);
    /* another in its place */
    e = layout_load("horizon" LAYOUT_EXT);
    CHECK(e != NULL && !strcmp(e->name, "Horizon") && layout_loaded() == e);
    layout_unload();
    CHECK(layout_loaded() == NULL);

    CHECK(layout_load("wrongabi" LAYOUT_EXT) == NULL);
    CHECK(layout_load("missing" LAYOUT_EXT) == NULL);
    CHECK(layout_loaded() == NULL);

    printf("%d failed\n", fails);
    return fails != 0;
}
