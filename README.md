# AmigaMCE

A fast, full-screen media centre for the Amiga, in the spirit of Kodi,
RetroArch and Windows Media Center. AmigaMCE is a drop-in companion to
[Amiga Game Selector (AGS)](https://github.com/MrV2K/Amiga-Game-Selector):
copy the drawer onto an AGS installation and double-click it.

- Kickstart 3.1 or newer. Screen mode is chosen in Settings: 320x200/256
  lowres, 640x200/256 hires and 640x400/512 laced on AGA, 640x480, 800x600
  and 1024x768 on RTG.
- Reads the AGS menu tree, fact sheets and screenshots directly; launches titles
  through their `.run` scripts and returns to AmigaMCE afterwards.
- Pictures are decoded through the installed datatypes. The Pictures
  section browses `Media:Images`, plays folders as slideshows and shows
  pictures full screen on AmigaMCE's own screen (no mode switch), in the
  picture's own palette on native and 256-colour screens.
- Text pages scroll smoothly without flicker, with a scroll bar when needed.
- Keyboard, mouse and joystick/CD32 pad.

Mods, Videos, Music and Radio are on the menu but not yet implemented.

## Building

Builds with the `m68k-amigaos-gcc` 16.2 toolchain (16.2-rc23 or later).
The quickest way is the
[amiga-gcc container](https://github.com/reinauer/container-amiga-gcc):

    docker run --rm -v $PWD:/src -w /src stefanreinauer/amiga-gcc:16.2-rc23 make

GCC 13.4 is not recommended: it can drop the register arguments of an
inline OS call when a multiply helper call ends up between them (see
`tests/aligntest/gcc-register-asm-bug-report.txt`).

| Target         | Result                                                    |
|----------------|-----------------------------------------------------------|
| `make`         | `AmigaMCE`, `amigamce-<version>.lha`, `amigamce-<version>.adf` (a data disk holding the LHA) |
| `make test`    | AGS and Pictures folder reader tests, run under vamos      |
| `make preview` | True-aspect PNG previews of the Home Screen in `build/`   |
| `make art`     | Regenerates `src/art.c` (logo, menu icons, palette)       |
| `make fonts`   | Regenerates the `amce-pixel` bitmap fonts from Pixelify Sans |
| `make icons`   | Regenerates the Workbench icons in `docs/`                |

The version comes from `git describe --tags`; tag `vX.Y.Z` to release.
Pushing a `v*` tag builds a draft GitHub release; publishing it uploads to
Aminet.

## Layout

    src/            program sources (main, gfx, ags, caps, art, util)
    tests/          vamos unit tests and a synthetic AGS tree
    tools/          host-side generators for art (mkart, hiicons), fonts, icons and previews
    assets/         generated Amiga fonts, plus the source TTFs and their licences
    docs/           Workbench icons

## Credits

AmigaMCE © 2026 Brian King. Fonts: Pixelify Sans and Rubik, both under the
SIL Open Font Licence 1.1 (see `assets/fonts-src/`). Amiga is a trademark of
Amiga Corporation.

AmigaMCE's own licence has not been chosen yet.
