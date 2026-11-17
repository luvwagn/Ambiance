# jdec

Ambiance's own JPEG decoder, built into Ambiance for JPEG thumbnails and
cover art (src/jpgthumb.c). Written for 68020+ CPUs without an FPU. MIT
licence.

- **Fast on a 68020:** 16-bit multiplies only, and some hand-written 68020
  code. A 160×160 cover decodes in about 0.3 s on a 50 MHz 68030.
- **Scaled decoding:** 1/2, 1/4 or 1/8 straight from the DCT, so a
  thumbnail costs a fraction of a full decode.
- **Low memory:** it works a band of rows at a time, so memory use stays at
  a few MCU rows whatever the picture's size.
- **Safe on damaged files:** a damaged or cut-off file decodes to grey
  where its data ends.

What it does not decode goes to the system's JPEG datatype (WarpJPEG,
jfif.datatype or whichever is installed) through datatypes.library:

- progressive JPEGs
- CMYK/YCCK JPEGs
- 12-bit and arithmetic-coded JPEGs

## Source

`jdec.c`/`jdec.h` are the decoder. The tests are in `test/`:

- `jtest.c`: jdec on the host, a JPEG to a PPM.
- `compare.py`: compares against libjpeg at every scale.
- `fuzz.py`: fuzzing with sanitizers.
- `cyc/`: 68020 cycle counts and profiles under Musashi.

zzprobe (tools/zzprobe) times jdec beside the installed JPEG datatypes.

## Credits

jdec contains no code from libjpeg or libjpeg-turbo. It follows the
methods they use and document, so that its pictures match theirs: the
Arai-Agui-Nakajima fast IDCT (as in the IJG's `jidctfst.c`), libjpeg's
"fancy" chroma upsampling, and its YCbCr-to-RGB rounding (JFIF, ITU-T
T.871). It is tested against libjpeg through Pillow. With thanks to the
Independent JPEG Group and the libjpeg-turbo Project (see CREDITS.txt).
