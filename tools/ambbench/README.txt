ambbench 1.1 - times Ambiance's 68k code on a real Amiga

Copy the whole ambbench drawer (ambbench, Libs, Bench) to the Amiga and,
from a Shell in that drawer:

    ambbench                 everything (a few minutes on a 68030)
    ambbench QUICK           fewer passes and frames
    ambbench ONLY=C2P        one part: MEM, C2P, PENS, MP3 or VIDEO
    ambbench VIDEO=dh1:Films/clip.mpg dh1:dvd/VTS_01_1.VOB
                             your own video files (their first 6 MB)
    ambbench MP3=dh1:Music/song.mp3

CTRL-C stops it between tests. Each run is added to ambbench.txt beside
the program, dated: please send that file, with the CPU's clock (50 MHz,
28 MHz, ...), which the program cannot tell.

What it times:
  MEM    longwords written to and read from chip RAM and fast RAM
  C2P    a 320 x 256 frame of pens to 8 bitplanes, into chip RAM and fast
         RAM: the old C, Ambiance's 68020 assembler, the 68040/060 store
         buffer's, and half size's two ways (the planes checked the same)
  PENS   AGA video: a 320 x 256 picture made bytes for its planes, as
         pens of a palette (as today for a file) and as HAM8, once and
         twice across (lowres and hires screens)
  MP3    Libs/ambmpega060.library (68060 only) and ambmpega.library:
         high, medium, 22 kHz mono, as Ambiance asks for them
  VIDEO  Libs/mpegv.library: every picture, B pictures passed over, half
         size (Bench/pal.m2v: DVD-like 720 x 576; Bench/ps.mpg: a tiny
         MPEG-1 clip; a real VCD or DVD file of your own is better)

The libraries in Libs are Ambiance 0.9.6's. Any already in memory are
dropped first (if nothing has them open), so these are the ones timed.
