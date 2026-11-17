/* AmigaOS's basic types for host-built tests (tests/kodilite_host.c):
 * the same sizes as on the Amiga. */
#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H
#include <stdint.h>
typedef int8_t BYTE;
typedef uint8_t UBYTE;
typedef int16_t WORD;
typedef uint16_t UWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int16_t BOOL;
typedef void *APTR;
typedef char *STRPTR;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#endif
