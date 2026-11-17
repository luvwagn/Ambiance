# Album cover decode times: JPEG baseline

Taken from Ambiance.log on 2026-11-05, before covers were asked for as PNG.
Machine: Amiberry A1200, 68030 (cycle-exact), plex tier 0; 640 x 512 PAL hires laced.
Each row is one decode (repeats of the same result, a picture read ahead and
shown again, left out). `decode` is the datatype's (WarpJPEG; a few covers the
server sent as PNG though asked for JPEG, WarpPNG), `reduce` making the
picture its colours for the screen, `draw` remapping and putting it on screen.

| build | cover | asked | got | datatype | decode ms | reduce ms | draw ms | colours |
|---|---|---|---|---|---:|---:|---:|---:|
| 0.4.1-31-gd639692 | 32363F0F | 262x217 | 262x262 | WarpJPEG | 1563 | 1591 | 430 | 90 |
| 0.4.1-31-gd639692 | D745BC85 | 262x217 | 262x262 | WarpPNG | 971 | 1473 | 428 | 90 |
| 0.4.1-31-gd639692 | F1D493F1 | 262x217 | 262x262 | WarpJPEG | 1213 | 1427 | 291 | 32 |
| 0.4.1-31-gd639692 | AB69BE90 | 262x217 | 262x262 | WarpJPEG | 1482 | 1679 | 429 | 90 |
| 0.4.1-33-g95e9514 | AB69BE90 | 160x160 | 160x160 | WarpJPEG | 666 | 931 | 230 | 48 |
| 0.4.1-33-g95e9514 | 42DC24AF | 160x160 | 164x160 | WarpJPEG | 645 | 900 | 246 | 48 |
| 0.4.1-34-g00053f9 | F1D493F1 | 160x160 | 160x160 | WarpJPEG | 612 | 693 | 193 | 32 |
| 0.4.1-34-g00053f9 | AB69BE90 | 160x160 | 160x160 | WarpJPEG | 810 | 871 | 231 | 48 |
| 0.4.1-34-g00053f9 | 42DC24AF | 160x160 | 164x160 | WarpJPEG | 1339 | 1746 | 385 | 48 |
| 0.4.1-35-gd32baa5 | AB69BE90 | 160x160 | 160x160 | WarpJPEG | 701 | 813 | 231 | 48 |
| 0.4.1-35-gd32baa5 | 3F97665D | 160x160 | 160x160 | WarpPNG | 525 | 787 | 232 | 48 |
| 0.4.1-35-gd32baa5 | 6293ADC7 | 160x160 | 160x160 | WarpJPEG | 759 | 778 | 229 | 48 |
| 0.4.1-35-gd32baa5 | DEEB677B | 160x160 | 160x160 | WarpJPEG | 589 | 768 | 232 | 48 |
| 0.4.1-35-gd32baa5 | FBFDBE22 | 160x160 | 160x160 | WarpJPEG | 606 | 723 | 199 | 32 |
| 0.4.1-35-gd32baa5 | 24188B44 | 160x160 | 160x160 | WarpJPEG | 779 | 834 | 227 | 48 |
| 0.4.1-35-gd32baa5 | 2B384922 | 160x160 | 165x160 | WarpJPEG | 785 | 783 | 245 | 48 |
| 0.4.1-35-gd32baa5 | 9644C971 | 160x160 | 160x160 | WarpJPEG | 966 | 1612 | 229 | 48 |
| 0.4.1-35-gd32baa5 | 90524421 | 160x160 | 160x160 | WarpPNG | 456 | 668 | 230 | 48 |
| 0.4.1-35-gd32baa5 | 5BAE4D15 | 160x160 | 160x160 | WarpJPEG | 457 | 654 | 191 | 32 |
| 0.4.1-35-gd32baa5 | 2148BB1B | 160x160 | 160x160 | WarpJPEG | 776 | 826 | 233 | 48 |
| 0.4.1-35-gd32baa5 | 7C24BA5E | 160x160 | 160x160 | WarpJPEG | 1109 | 1330 | 375 | 48 |
| 0.4.1-39-g0401b8e | 3F97665D | 160x160 | 160x160 | WarpPNG | 511 | 784 | 231 | 48 |
| 0.4.1-39-g0401b8e | 831B785B | 160x160 | 177x160 | WarpJPEG | 702 | 627 | 253 | 48 |
| 0.4.1-39-g0401b8e | 9644C971 | 160x160 | 160x160 | WarpJPEG | 567 | 696 | 230 | 48 |
| 0.4.1-39-g0401b8e | 07FEABB5 | 160x160 | 160x160 | WarpJPEG | 1435 | 1312 | 377 | 48 |
| 0.4.1-41-g8939880 | 24188B44 | 160x160 | 160x160 | WarpJPEG | 822 | 870 | 231 | 48 |
| 0.4.1-41-g8939880 | 2B384922 | 160x160 | 165x160 | WarpJPEG | 885 | 823 | 249 | 48 |
| 0.4.1-41-g8939880 | 9644C971 | 160x160 | 160x160 | WarpJPEG | 539 | 773 | 233 | 48 |
| 0.4.1-43-ge608c2a | 2B384922 | 160x160 | 165x160 | WarpJPEG | 797 | 857 | 243 | 48 |
| 0.4.1-43-ge608c2a | 07FEABB5 | 160x160 | 160x160 | WarpJPEG | 1394 | 1580 | 381 | 48 |
| 0.4.1-43-ge608c2a | 946B37E8 | 160x160 | 161x160 | WarpJPEG | 1157 | 1757 | 384 | 48 |
| 0.4.1-43-ge608c2a | 4CB889D6 | 160x160 | 160x279 | WarpJPEG | 2052 | 1680 | 288 | 48 |

## Summary

**160 x 160, JPEG (WarpJPEG)** (25 decodes): decode median 779 ms (range 457-2052), reduce median 834 ms (range 627-1757), draw median 233 ms (range 191-385).

**160 x 160, PNG sent anyway (WarpPNG)** (3 decodes): decode median 511 ms (range 456-525), reduce median 784 ms (range 668-787), draw median 231 ms (range 230-232).

**262 x 217 asked, before the square request and the tier caps, JPEG** (3 decodes): decode median 1482 ms (range 1213-1563), reduce median 1591 ms (range 1427-1679), draw median 429 ms (range 291-430).

Draw times fall in two groups, about 230 ms and about 380 ms; the slower are probably
with music playing.

## PNG (0.4.1-44-g4e2c486, format=png asked for)

Same machine, 2026-11-05 12:07. Three covers that are in the JPEG table too:

| cover | JPEG decode ms | JPEG reduce ms | PNG decode ms | PNG reduce ms | PNG draw ms |
|---|---:|---:|---:|---:|---:|
| 24188B44 | 779, 822 | 834, 870 | 541 | 877 | 233 |
| 9644C971 | 966, 567, 539 | 1612, 696, 773 | 437 | 792 | 243 |
| 07FEABB5 | 1435, 1394 | 1312, 1580 | 476 | 791 | 233 |

PNG decodes in 437-541 ms against JPEG's 539-1435 for the same covers: about a
third faster on a plain cover, three times faster on a detailed one
(07FEABB5), and steadier (PNG's time hardly depends on the picture). The reduce
to 48 colours (about 790-880 ms) does not change and is now the larger part.

## PNG, colours chosen over the cells in use (0.4.1-46-gedf7d72)

Same machine, 2026-11-05 12:17. `made` is the whole of making the thumbnail;
after it, the log's split of it.

| cover | got | decode ms | made ms | reading | counting | choosing | mapping |
|---|---|---:|---:|---:|---:|---:|---:|
| 9644C971 | 160x160 | 447 | 111 | 7 | 74 | 16 | 12 |
| 07FEABB5 | 160x160 | 530 | 186 | 8 | 91 | 73 | 13 |
| 946B37E8 | 161x160 | 907 | 467 | 14 | 198 | 182 | 73 |
| 4CB889D6 | 160x279 to 91x160 | 1375 | 1036 | 25 | 829 | 174 | 7 |

A cover at the size shown now takes 111-186 ms to make, against 790-880
before: decode plus make about 560-720 ms, against 1.3-1.7 s as PNG with the
old reduction, 1.6-2.8 s as JPEG. 946B37E8 was made with music playing (its
draw, 388 ms, is the slower group), so everything about it took about twice as
long. 4CB889D6, a tall cover, came 160 x 279 (minSize=1 makes the shorter side
160) and was averaged down: from 0.4.1-47 covers are asked for within the
square (minSize=0), so they come at the size shown.
