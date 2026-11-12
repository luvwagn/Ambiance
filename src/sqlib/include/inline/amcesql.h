/* Automatically generated header (sfdc 1.13)! Do not edit! */

#ifndef _INLINE_AMCESQL_H
#define _INLINE_AMCESQL_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef AMCESQL_BASE_NAME
#define AMCESQL_BASE_NAME AmceSqlBase
#endif /* !AMCESQL_BASE_NAME */

#define SqlOpen(___path) \
      LP1(0x1e, APTR, SqlOpen , STRPTR, ___path, a0,\
      , AMCESQL_BASE_NAME)

#define SqlClose(___db) \
      LP1NR(0x24, SqlClose , APTR, ___db, a0,\
      , AMCESQL_BASE_NAME)

#define SqlPrepare(___db, ___sql) \
      LP2(0x2a, APTR, SqlPrepare , APTR, ___db, a0, STRPTR, ___sql, a1,\
      , AMCESQL_BASE_NAME)

#define SqlBindText(___stmt, ___index, ___text) \
      LP3(0x30, LONG, SqlBindText , APTR, ___stmt, a0, LONG, ___index, d0, STRPTR, ___text, a1,\
      , AMCESQL_BASE_NAME)

#define SqlStep(___stmt) \
      LP1(0x36, LONG, SqlStep , APTR, ___stmt, a0,\
      , AMCESQL_BASE_NAME)

#define SqlColumns(___stmt) \
      LP1(0x3c, LONG, SqlColumns , APTR, ___stmt, a0,\
      , AMCESQL_BASE_NAME)

#define SqlText(___stmt, ___column) \
      LP2(0x42, STRPTR, SqlText , APTR, ___stmt, a0, LONG, ___column, d0,\
      , AMCESQL_BASE_NAME)

#define SqlInt(___stmt, ___column) \
      LP2(0x48, LONG, SqlInt , APTR, ___stmt, a0, LONG, ___column, d0,\
      , AMCESQL_BASE_NAME)

#define SqlFinalize(___stmt) \
      LP1NR(0x4e, SqlFinalize , APTR, ___stmt, a0,\
      , AMCESQL_BASE_NAME)

#define SqlError(___db) \
      LP1(0x54, STRPTR, SqlError , APTR, ___db, a0,\
      , AMCESQL_BASE_NAME)

#define SqlVersion() \
      LP0(0x5a, STRPTR, SqlVersion ,\
      , AMCESQL_BASE_NAME)

#define SqlMemory(___peak) \
      LP1(0x60, ULONG, SqlMemory , LONG, ___peak, d0,\
      , AMCESQL_BASE_NAME)

#define SqlRelease(___db) \
      LP1NR(0x66, SqlRelease , APTR, ___db, a0,\
      , AMCESQL_BASE_NAME)

#endif /* !_INLINE_AMCESQL_H */
