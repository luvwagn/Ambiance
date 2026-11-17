/*
 * AmigaMCE - Kodi lite, the first layout (layout.h)
 * Copyright (c) 2026 Brian King
 *
 * After Kodi's look, without its fanart: a menu down the left, solid
 * panels, a light blue focus and a big clock, on a dark blue-grey
 * gradient. Nothing is blended and nothing large is decoded (see
 * claude/layout-engines.md, and the mockups' Kodi lite page).
 */
#include <exec/types.h>
#include "../layout.h"

const char LibName[] = "kodilite.layout";
const char LibIdString[] = "kodilite.layout 1.0 (17.11.2026) Kodi lite, for AmigaMCE";
static const char __attribute__((used)) LibVerTag[] =
    "$VER: kodilite.layout 1.0 (17.11.2026) Kodi lite, for AmigaMCE";
const UWORD LibVersion = 1;
const UWORD LibRevision = 0;

static const struct LayoutHost *H;

static BOOL kl_open(const struct LayoutHost *host)
{
    H = host;
    return TRUE;
}

static void kl_close(void)
{
    H = NULL;
}

static void kl_draw(int screen)
{
    (void)screen;
}

static void kl_changed(int screen, ULONG what)
{
    (void)screen;
    (void)what;
}

static void kl_tick(ULONG ms)
{
    (void)ms;
}

static int kl_input(int screen, int action, LONG arg)
{
    (void)screen;
    (void)action;
    (void)arg;
    return LR_UNUSED;
}

static int kl_pointer(int screen, WORD x, WORD y, BOOL click)
{
    (void)screen;
    (void)x;
    (void)y;
    (void)click;
    return LR_UNUSED;
}

const struct LayoutEngine layout_engine = {
    LAYOUT_ABI, "Kodi lite", 15, 480, 0,
    kl_open, kl_close, kl_draw, kl_changed, kl_tick, kl_input, kl_pointer
};
