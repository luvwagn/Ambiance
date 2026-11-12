# amcejpeg.datatype

JPEG pictures for every Amiga program that loads them through datatypes
(MultiView, Workbench, AmigaMCE, ...), written for 68020+ CPUs without
an FPU. MIT licence.

- **Fast on a 68020:** 16-bit multiplies only, and some hand-written 68020
  code. A 160×160 cover decodes in about 0.3 s on a 50 MHz 68030.
- **Scaled decoding:** a program that shows a picture smaller than its own
  size can ask for a 1/2, 1/4 or 1/8 decode with the tags in
  `amcejpeg.h`. Thumbnails come out several times faster that way.
- **Low memory:** it works a band of rows at a time, so memory use stays at
  a few MCU rows whatever the picture's size (picture.datatype keeps its
  own copy of the picture).
- **Safe on damaged files:** a damaged or cut-off file decodes to grey
  where its data ends.

It needs picture.datatype 43 or newer (OS 3.5, 3.9, 3.2, or an updated
picture.datatype on 3.0/3.1).

## Installing

    Copy amcejpeg.datatype SYS:Classes/DataTypes/
    Copy AmigaMCE-JPEG DEVS:DataTypes/
    AddDataTypes DEVS:DataTypes/AmigaMCE-JPEG

Use `AddDataTypes REFRESH`, or reboot, if another JPEG datatype was
already in use. Descriptors are tried highest priority first; this one is
priority 5. If another JPEG descriptor in DEVS:DataTypes has a higher
priority (the ZZ9000's has 10, and is meant to win when it is active),
that one reads JPEGs instead. `dtjpeg` shows which one does:

    dtjpeg Pictures/cover.jpg TIMES=10
    dtjpeg Pictures/cover.jpg FIT=160x120 TIMES=10
    dtjpeg Pictures/cover.jpg PPM=RAM:cover.ppm

## Not yet

These files fail to load (IoErr: unknown compression), and datatypes do
not fall back to another datatype:

- progressive JPEGs
- CMYK/YCCK JPEGs
- 12-bit and arithmetic-coded JPEGs

## Source

`jdec.c`/`jdec.h` are the decoder, and `amcejpeg.c` is the class library.
The tests are in `test/`:

- `compare.py`: compares against libjpeg at every scale.
- `fuzz.py`: fuzzing with sanitizers.
- `cyc/`: 68020 cycle counts and profiles under Musashi.
