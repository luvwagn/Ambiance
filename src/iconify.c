/*
 * AmigaMCE - iconified: an AppIcon on Workbench while the window is away
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * main.c closes the window (or puts its screen behind Workbench) and the
 * music plays on; here, the icon on Workbench's desktop that brings it
 * back (double-clicked, or its Open menu item on OS 3.5 on) and an item in
 * Workbench's Tools menu that quits. The icon's picture: PROGDIR:Icons/
 * AppIcon.info (a musical note, tools/mkicon.py), else the program's own,
 * else Workbench's default for a tool.
 *
 * workbench.library and icon.library opened here, under names of their
 * own: gfx.c's WorkbenchBase (V45, its hook) comes and goes as it needs.
 */
#include <exec/types.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
static struct Library *ic_wb, *ic_icon;
#define WB_BASE_NAME ic_wb
#define ICON_BASE_NAME ic_icon
#include <proto/wb.h>
#include <proto/icon.h>
#include <stdio.h>
#include <string.h>
#include "amce.h"
#include "iconify.h"

static struct MsgPort *ic_port;
static struct AppIcon *ic_app;
static struct AppMenuItem *ic_item;
static struct DiskObject *ic_dob;
static BOOL ic_tried;

static BOOL ic_libs(void)
{
    if (!ic_tried) {
        ic_tried = TRUE;
        ic_wb = OpenLibrary((CONST_STRPTR)"workbench.library", 37);
        ic_icon = OpenLibrary((CONST_STRPTR)"icon.library", 37);
        if (!ic_wb || !ic_icon)
            amce_log("iconify", "no workbench.library or icon.library: not offered");
    }
    return ic_wb && ic_icon;
}

BOOL icon_available(void)
{
    return ic_libs();
}

static struct DiskObject *ic_picture(const char **from)
{
    static const char *const names[] = { "PROGDIR:Icons/AppIcon", "PROGDIR:AmigaMCE" };
    struct DiskObject *d;
    int i;
    for (i = 0; i < 2; i++)
        if ((d = GetDiskObject((CONST_STRPTR)names[i]))) {
            *from = names[i];
            return d;
        }
    *from = "Workbench's default";
    return GetDefDiskObject(WBTOOL);
}

BOOL icon_add(const char *label, const char *quit_label)
{
    const char *from = "";
    char m[120];
    if (ic_app)
        return TRUE;
    if (!ic_libs())
        return FALSE;
    if (!ic_port && !(ic_port = CreateMsgPort()))
        return FALSE;
    if (!ic_dob && !(ic_dob = ic_picture(&from))) {
        amce_log("iconify", "no icon to show");
        return FALSE;
    }
    ic_dob->do_Type = 0;                    /* (an AppIcon's: none) */
    ic_dob->do_CurrentX = NO_ICON_POSITION; /* Workbench finds it a place */
    ic_dob->do_CurrentY = NO_ICON_POSITION;
    {
        struct TagItem t[2];
        t[0].ti_Tag = WBAPPICONA_SupportsOpen;  /* (OS 3.5 on: its Open menu item) */
        t[0].ti_Data = TRUE;
        t[1].ti_Tag = TAG_DONE;
        ic_app = AddAppIconA(0, 0, (CONST_STRPTR)label, ic_port, 0, ic_dob, t);
    }
    if (!ic_app) {
        amce_log("iconify", "AddAppIcon failed (Workbench not running?)");
        return FALSE;
    }
    ic_item = AddAppMenuItemA(1, 0, (CONST_STRPTR)quit_label, ic_port, NULL);
    sprintf(m, "the AppIcon on Workbench (its picture %.60s)%s", from,
            ic_item ? ", Tools > Quit there" : "");
    amce_log("iconify", m);
    return TRUE;
}

/* What Workbench asked: ICON_OPEN (the icon double-clicked, or Open),
 * ICON_QUIT (Tools > Quit AmigaMCE), ICON_NONE. Everything else (files
 * dropped on it, for now) answered and let go. */
int icon_poll(void)
{
    struct AppMessage *am;
    int r = ICON_NONE;
    if (!ic_port)
        return r;
    while ((am = (struct AppMessage *)GetMsg(ic_port))) {
        if (am->am_Type == AMTYPE_APPMENUITEM && am->am_ID == 1)
            r = ICON_QUIT;
        else if (am->am_Type == AMTYPE_APPICON && r == ICON_NONE &&
                 (am->am_NumArgs == 0 ||
                  (am->am_Version >= AM_VERSION && am->am_Class == AMCLASSICON_Open)))
            r = ICON_OPEN;
        ReplyMsg((struct Message *)am);
    }
    return r;
}

ULONG icon_sigmask(void)
{
    return ic_app && ic_port ? 1UL << ic_port->mp_SigBit : 0;
}

void icon_remove(void)
{
    if (ic_item)
        RemoveAppMenuItem(ic_item);
    ic_item = NULL;
    if (ic_app)
        RemoveAppIcon(ic_app);
    ic_app = NULL;
    if (ic_port) {
        struct Message *msg;
        while ((msg = GetMsg(ic_port)))     /* (any that came meanwhile) */
            ReplyMsg(msg);
    }
}

BOOL icon_shown(void)
{
    return ic_app != NULL;
}

void icon_end(void)
{
    icon_remove();
    if (ic_dob)
        FreeDiskObject(ic_dob);
    ic_dob = NULL;
    if (ic_port)
        DeleteMsgPort(ic_port);
    ic_port = NULL;
    if (ic_icon)
        CloseLibrary(ic_icon);
    if (ic_wb)
        CloseLibrary(ic_wb);
    ic_icon = ic_wb = NULL;
    ic_tried = FALSE;
}
