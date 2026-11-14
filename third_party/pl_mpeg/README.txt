pl_mpeg: MPEG-1 video decoder, MP2 audio decoder and MPEG-PS demuxer, by
Dominic Szablewski (https://github.com/phoboslab/pl_mpeg), a single header,
at c871f2be022ece7ef4f64230b4fb8e1fb9eb6023 (30 December 2025), changed by
AmigaMCE for speed on a 68020 without an FPU (each change marked
"AmigaMCE:" in pl_mpeg.h), the pictures it decodes the same, bit for bit:

- bits read a longword at a time, and the video VLCs by tables of their
  first bits made from pl_mpeg's own trees; a DCT coefficient's code read
  with its sign from the same longword
- the IDCT only where a block has coefficients, by its even and odd halves
  (each rounding within itself; a half of zeros gives zeros), its rows
  straight into the picture, the rows' rounding carried in through the
  first column; block_data zeroed only where it was set
- x * 362 and x * 196 by shifts and adds, the dequantising by 16-bit
  multiplies (the same products)
- motion compensation and a block's one added value four pixels at a
  time, each byte's average or clamp as pl_mpeg's
- start codes looked for through what the buffer holds at once

Checked with tests/mpeg_bench.c (each picture's hash, against the
original's, built for this computer and for the 68020 under vamos) and
timed with tests/mpeg_prof.py (make mpeg-prof).

Licence: MIT (SPDX-License-Identifier: MIT, in pl_mpeg.h), as AmigaMCE's.

AmigaMCE builds it in src/plmpeg.c and uses its demuxer and video decoder
to play MPEG-1 video where there is no ZZ9000 (src/stream.c: swv_run); the
sound goes to mpega.library, not pl_mpeg's MP2 decoder (floating point,
slow on a 68k without an FPU).
