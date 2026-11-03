# AmigaMCE

A fast, full-screen media centre for the Amiga, in the spirit of Kodi,
RetroArch and Windows Media Center. AmigaMCE is a drop-in companion to
[Amiga Game Selector (AGS)](https://github.com/MrV2K/Amiga-Game-Selector):
copy the drawer onto an AGS installation and double-click it.

- Kickstart 3.1 or newer; OCS/ECS (320-wide lowres) or AGA/RTG (640-wide hires).
- Reads the AGS menu tree, fact sheets and screenshots directly; launches titles
  through their `.run` scripts and returns to AmigaMCE afterwards.
- Pictures are decoded through the installed datatypes.
- Text pages scroll smoothly without flicker, with a scroll bar when needed.
- Keyboard, mouse and joystick/CD32 pad.

Mods, Videos, Pictures, Music and Internet Radio are on the menu but not yet
implemented.

## Building

Builds with the `m68k-amigaos-gcc` 13.4 toolchain, the same one used by
[xSysInfo](https://github.com/reinauer/xSysInfo). The quickest way is its
container:

    docker run --rm -v $PWD:/src -w /src stefanreinauer/amiga-gcc:gcc-v13.4 make

| Target         | Result                                                    |
|----------------|-----------------------------------------------------------|
| `make`         | `AmigaMCE`, `amigamce-<version>.lha`, `amigamce-<version>.adf` |
| `make test`    | AGS parser tests, run under vamos                         |
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
