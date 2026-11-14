pl_mpeg: MPEG-1 video decoder, MP2 audio decoder and MPEG-PS demuxer, by
Dominic Szablewski (https://github.com/phoboslab/pl_mpeg), a single header,
at c871f2be022ece7ef4f64230b4fb8e1fb9eb6023 (30 December 2025), unchanged.

Licence: MIT (SPDX-License-Identifier: MIT, in pl_mpeg.h), as AmigaMCE's.

AmigaMCE builds it in src/plmpeg.c and uses its demuxer and video decoder
to play MPEG-1 video where there is no ZZ9000 (src/stream.c: swv_run); the
sound goes to mpega.library, not pl_mpeg's MP2 decoder (floating point,
slow on a 68k without an FPU).
