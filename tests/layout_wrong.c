/* A layout built against another layout.h (its LAYOUT_ABI not the
 * program's): tests/test_layout.c checks it is never used. */
#include <exec/types.h>
#include "../src/layout.h"

const char LibName[] = "wrongabi.layout";
const char LibIdString[] = "wrongabi.layout 1.0 (17.11.2026) for AmigaMCE's tests";
static const char __attribute__((used)) LibVerTag[] = "$VER: wrongabi.layout 1.0 (17.11.2026)";
const UWORD LibVersion = 1;
const UWORD LibRevision = 0;

static BOOL w_open(const struct LayoutHost *h) { (void)h; return TRUE; }
static void w_close(void) { }

const struct LayoutEngine layout_engine = {
    LAYOUT_ABI, "Wrong", 15, 480, 0, w_open, w_close, 0, 0, 0, 0, 0
};
