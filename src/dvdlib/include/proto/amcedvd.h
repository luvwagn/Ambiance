/* Written in the form sfdc 1.13 makes from amcedvd_lib.sfd. MIT, (c) 2026 Brian King. */

#ifndef PROTO_AMCEDVD_H
#define PROTO_AMCEDVD_H

#include <clib/amcedvd_protos.h>

#if defined(_CONST_BASES)
# ifndef __CONSTLIBBASEDECL__
# define __CONSTLIBBASEDECL__ const
# endif /* __CONSTLIBBASEDECL__ */
# ifndef __SEGMENTLIBBASEDECL__
# define __SEGMENTLIBBASEDECL__  __attribute__((__section__(".data")))
# endif /* __SEGMENTLIBBASEDECL__ */
#endif /* _CONST_BASES */
#ifdef __amigaos4__
# include <interfaces/amcedvd.h>
# ifndef __NOGLOBALIFACE__
   extern struct AmceDvdIFace *IAmceDvd;
# endif /* __NOGLOBALIFACE__*/
#endif /* !__amigaos4__ */
#ifndef __NOLIBBASE__
  extern struct Library *
# ifdef __CONSTLIBBASEDECL__
   __CONSTLIBBASEDECL__
# endif /* __CONSTLIBBASEDECL__ */
  AmceDvdBase
# ifdef __SEGMENTLIBBASEDECL__
 __SEGMENTLIBBASEDECL__
# endif /* __SEGMENTLIBBASEDECL__ */
;
#endif /* !__NOLIBBASE__ */

#ifndef _NO_INLINE
# if defined(__GNUC__)
#  ifdef __AROS__
#   include <defines/amcedvd.h>
#  else
#   include <inline/amcedvd.h>
#  endif
# else
#  include <pragmas/amcedvd_pragmas.h>
# endif
#endif /* _NO_INLINE */

#endif /* !PROTO_AMCEDVD_H */
