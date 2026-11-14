ZZ9000 SDK headers (BlitterStudio's zz9000-firmware, sdk/), for testing
AmigaMCE's ZZ9000 video player (src/zz9k.c, src/video.c) only.

  amiga-include/  sdk/amiga/include   (zz9k.library's proto, clib, inline)
  include/        sdk/include         (zz9k/abi.h and the SDK's types)
  host-include/   sdk/host/include    (zz9k/request.h, reply.h: the media
                                       session's request builders and
                                       reply decoders)

From https://github.com/BlitterStudio/zz9000-firmware at
97a5938e07b8d0653e3d6facb79f654d135c990d (14 November 2026).

LICENCE: these files are GPL-3.0-or-later. AmigaMCE is MIT. The SDK's
author has agreed to add a GPL-3 section 7 exception for programs that only
call zz9k.library; until that exception is published, a build containing
this code must not be distributed. This branch (zz9k-video) is for testing.
