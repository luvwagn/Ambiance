ZZ9000 SDK headers (BlitterStudio's zz9000-firmware, sdk/), for AmigaMCE's
ZZ9000 video player (src/zz9k.c, src/video.c), unchanged:

  amiga-include/  sdk/amiga/include   (zz9k.library's and mpega.library's
                                       proto, clib, inline, pragmas)
  include/        sdk/include         (zz9k/abi.h and the SDK's types;
                                       module.h and service_registry.h,
                                       which stay GPL, left out: unused)
  host-include/   sdk/host/include    (zz9k/request.h, reply.h: the media
                                       session's request builders and
                                       reply decoders)

From https://github.com/BlitterStudio/zz9000-firmware at
6de721a01349756211133aaffc6679b1d1596c51 (17 November 2026).

LICENCE: MIT (LICENSE-MIT.txt; each file's SPDX line). zz9k.library and
mpega.library themselves are GPL-3.0-or-later, with an additional permission
under GPL-3 section 7 that lets any program, whatever its licence, open
them with OpenLibrary() and call their public functions through these MIT
headers (LICENSING.md, the SDK's own, copied here). AmigaMCE compiles in
only these headers and opens zz9k.library at run time; no library source or
object code (zz9k_host.o included) is built in.
