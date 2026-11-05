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
- Mods: ProTracker modules by AmigaMCE's own replayer (`src/ptcore.c`,
  after the ProTracker 2.3D play routine) on Paula, timed by a CIA-B timer
  interrupt; other Amiga formats by EaglePlayer/DeliTracker player modules
  hosted in AmigaMCE (`src/eagle.c`, `src/eaglehost.s`); XM/S3M/IT and
  multichannel MODs mixed by modplug.library and streamed to Paula
  (`src/mixplay.c`); packed files unpacked with xfdmaster.library.
- Radio: stations browsed on the Amiga from radio-browser.info
  (`src/radio.c`, CSV over plain HTTP from a network process, `src/net.c`),
  filtered to what an Amiga can play (MP3, working, http: unless Settings >
  Allow HTTPS streams, which needs AmiSSL), and played (`src/stream.c`: the
  stream read with its ICY titles, decoded by mpega.library through a
  bitstream hook, played on Paula from a double buffer). HTTPS streams do
  not play yet.
- Music: a Plex Media Server's music (`src/plex.c`; its XML read by
  `src/plexxml.c`, which the host tests build), asked over plain HTTP by
  the network process, servers found by GDM (UDP to 239.0.0.250:32414);
  MP3 tracks played by the radio's player with an end (`src/stream.c`,
  their ID3v2 tag read past). The token from ENVARC:AmigaMCE/PlexToken.
- Keyboard, mouse and joystick/CD32 pad.

Videos is on the menu but not yet implemented.

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
| `make fonts`   | Regenerates the bitmap fonts: `amce-pixel` from Pixelify Sans, `amce-galmuri*` from Galmuri's BDFs |
| `make icons`   | Regenerates the Workbench icons in `docs/`                |

The version comes from `git describe --tags`; tag `vX.Y.Z` to release.
The minor number counts the libraries: it goes up by one (0.4, 0.5, ...)
each time a new library view joins the menu (Mods brought 0.4). Fixes and
smaller features between those are the builds after the tag
(`0.4.0-12-gabcdef`), or a patch tag (`v0.4.1`) when one is released.
Pushing a `v*` tag builds a draft GitHub release; publishing it uploads to
Aminet.

## Layout

    src/            program sources (main, gfx, ags, caps, art, util)
    tests/          vamos unit tests and a synthetic AGS tree
    tools/          host-side generators for art (mkart, hiicons), fonts, icons and previews
    assets/         generated Amiga fonts, plus the source TTFs and their licences
    docs/           Workbench icons

## Credits

AmigaMCE © 2026 Brian King. Fonts: Pixelify Sans, Galmuri and Rubik, all under the
SIL Open Font Licence 1.1 (see `assets/fonts-src/`). Amiga is a trademark of
Amiga Corporation.

AmigaMCE's own licence has not been chosen yet.
