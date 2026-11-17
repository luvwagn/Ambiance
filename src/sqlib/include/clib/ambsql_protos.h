/* Automatically generated header (sfdc 1.13)! Do not edit! */

#ifndef CLIB_AMBSQL_PROTOS_H
#define CLIB_AMBSQL_PROTOS_H

/*
**   $VER: ambsql_protos.h 1.1 (07.11.2026)
**
**   C prototypes. For use with 32 bit integers only.
**
**   Copyright (c) 2001 Amiga, Inc.
**       All Rights Reserved
*/

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* ambsql.library: SQLite 3.34.0, read only, for Ambiance (beets music */
/* libraries). Loaded only when one is set up: PROGDIR:Libs/ambsql.library. */
APTR SqlOpen(STRPTR path);
VOID SqlClose(APTR db);
APTR SqlPrepare(APTR db, STRPTR sql);
LONG SqlBindText(APTR stmt, LONG index, STRPTR text);
LONG SqlStep(APTR stmt);
LONG SqlColumns(APTR stmt);
STRPTR SqlText(APTR stmt, LONG column);
LONG SqlInt(APTR stmt, LONG column);
VOID SqlFinalize(APTR stmt);
STRPTR SqlError(APTR db);
STRPTR SqlVersion(void);
ULONG SqlMemory(LONG peak);
VOID SqlRelease(APTR db);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLIB_AMBSQL_PROTOS_H */
