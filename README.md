# AmigaMCE

**A fast, full-screen media centre for the Amiga.** Games and demos from
[Amiga Game Selector (AGS)](https://github.com/MrV2K/Amiga-Game-Selector),
tracker music, internet radio, your Plex music library and your pictures,
all in one place, driven by keyboard, mouse or joystick, in the spirit of
Kodi and Windows Media Center.

AmigaMCE is a drop-in companion to AGS 3: copy its drawer onto an AGS
installation and double-click it. Everything plays inside AmigaMCE, on the
Amiga's own screen and sound; it never switches you out to another program.

It runs on Kickstart 3.1 and up with a 68020 or better, from a plain A1200
to an accelerated or RTG machine, on OCS, ECS or AGA, and scales what it asks
of the Amiga to how fast it really is.

## What it does

### Games and demos (AGS)

- Browses the AGS menu tree (Games, Demos, Disk Magazines, Favourites) by
  category, with the title's screenshot and fact sheet in a pane beside
  the list on screens 640 pixels wide or more.
- Launches titles through their AGS `.run` scripts, honouring the AGS
  Expert, ExpertPerm and Adult settings. While a game runs AmigaMCE gives
  back everything it can, and comes back on the same entry afterwards.
- Long lists such as All Games (thousands of entries) are kept in an index,
  so they open at once; only letters that changed are read again.
  Screenshots and fact sheets are read by a background process, so the list
  never waits for the disk.

### Music

- **Mods:** ProTracker modules played by AmigaMCE's own replayer, after the
  ProTracker 2.3D play routine and timed by a CIA timer. Hundreds of other
  formats (OctaMED, TFMX, Future Composer, Hippel, AHX, SID...) play through
  EaglePlayer and DeliTracker player modules where they are installed (AGS
  has them). XM, S3M, IT and multichannel MODs are mixed by modplug.library
  on a 68020 or better. Packed modules are unpacked with xfdmaster.library.
- **Radio:** internet radio stations from
  [radio-browser.info](https://www.radio-browser.info), by top voted, most
  listened to, trending, chiptune, demoscene, genre or country (A to Z,
  your country first), showing only stations an Amiga can play: MP3,
  working, and no faster than this Amiga copes with. Shows the station's
  current song titles.
- **Plex:** the music libraries of a
  [Plex Media Server](https://www.plex.tv) on your network, by artist,
  album or recently added, with covers and details beside the list. Plex
  converts each track to an MP3 this Amiga can decode: 22 kHz mono on a
  68020/030, 24 kHz stereo on a 68040/060 or faster; a fast Amiga with
  flacdec.library plays MP3 and FLAC tracks as stored. Plain HTTP on your
  own network, so no AmiSSL is needed.
- **Music files:** MP3 and FLAC files on the Amiga itself, under Music next
  to Plex: the `MUSIC:` assign if you have one, and up to eight folders
  chosen in Settings > Sound > Music folders. A folder's files play in
  order, with seeking.
- **Now Playing:** title, artist, album and cover; a level meter per channel;
  a position bar you can click to seek; previous, back, play/pause, stop,
  forward, next and volume. The music plays on while you browse, and the
  list you came from marks what is playing.

### Pictures

- Browses `Media:Images` with a preview of the selected picture or folder,
  shows pictures full screen on AmigaMCE's own screen (no mode switch), and
  plays folders as slideshows. Pictures are decoded by the installed
  datatypes and shown in their own colours where the screen allows it.

### Screens and looks

- Native screens: 320x200/256 lowres, 640x200/256 hires and 640x400/512
  laced on OCS, ECS and AGA. RTG: 640x480, 800x600 and 1024x768.
- Laced screens choose from several pixel fonts; big RTG screens scale the
  whole layout up, in a pixel look or a smooth, anti-aliased one.
- Flicker-free scrolling text and lists, with a scroll bar for the mouse.

## Requirements

| | |
|---|---|
| **Needed** | Kickstart 3.1 or later and a 68020 or better, on OCS, ECS, AGA or RTG |
| **For Games and Demos** | Amiga Game Selector 3 (AmigaMCE finds it through the `AGS:` assign) |
| **For Radio and Plex** | A running TCP/IP stack (bsdsocket.library: Roadshow, for example, or an emulator's) |
| **For MP3 and FLAC** | mpega.library for MP3; [flacdec.library](https://aminet.net/package/util/libs/flacdec) for FLAC (its 68060 build on a 68060) |
| **Optional** | EaglePlayer/DeliTracker players, modplug.library and xfdmaster.library for more module formats; picture datatypes for the formats you have; AmiSSL to list HTTPS radio stations |

AHI and MHI are detected and shown in Settings, but AmigaMCE plays through
Paula for now.

## Installing

1. Copy the `AmigaMCE` drawer from the archive to your hard drive. Keep its
   `Fonts` drawer next to the program.
2. If AGS boots straight into its own menu, quit AGS to Workbench first.
3. Double-click AmigaMCE.

From the ADF, unpack the archive on it first, for example
`lha x DF0:amigamce.lha Work:`.

## Using it

| Key or button | What it does |
|---|---|
| Up/Down, wheel | Move; held, it speeds up. Shift+Up/Down pages |
| Fire, Return | Play a game, module, station or track; open a folder or setting |
| Right | Open a folder; on a game, its details |
| Left, Esc, right mouse | Back. Esc twice at the menu quits |
| Mouse | One click opens a folder or setting; a game or picture is selected by one click and played by two |
| A-Z | Jump to a letter; CD32 shoulder buttons step letters |
| Now Playing | Left/Right choose a button, Fire presses it; Space play/pause; S stop; N/P next/previous; `,` and `.` back/forward; Up/Down volume |

A Now playing row at the top of the music lists takes you back to what is
playing.

### Setting up Plex

1. Settings > Networking > Plex: turn Plex on and choose your server under
   Plex server. Servers on your network are found automatically.
2. If your server asks for a sign-in, give AmigaMCE your Plex token from the
   Shell: `SetEnv SAVE AmigaMCE/PlexToken <token>`. About > Plex in AmigaMCE
   explains where to find it.
3. Your music libraries appear under Music.

## Settings

| Page | What it holds |
|---|---|
| General | Jump scrolling (very long lists step a letter at a time), Debug |
| Screen | Screen mode, Style (fonts and looks), Double buffering (RTG) |
| Sound | What plays the sound and decodes MP3 and FLAC (Paula, mpega.library and flacdec.library for now); Music folders (the `MUSIC:` assign on or off, folders to add or take out) |
| Networking | Radio (HTTPS stations, the highest bitrate listed, which radio-browser.info server to use), Plex (on/off, server, lower quality audio), Stream buffer, Network interface |

Settings are saved in `ENVARC:AmigaMCE/`. Debug shows the CPU load and
memory use in the header and writes a timing log to `AmigaMCE.log` next to
the program, which is the first thing to look at if something misbehaves.

## Not there yet

- Videos is on the menu but arrives in a later version.
- HTTPS radio streams are listed (with AmiSSL) but do not play yet.
- AHI and MHI output are planned; with AHI, other sample rates than Paula's
  become possible.
- Plex sign-in through plex.tv needs HTTPS, so the token is set by hand for
  now.

## Building

AmigaMCE builds with the `m68k-amigaos-gcc` 16.2 toolchain (16.2-rc23 or
later), for the 68020 and up (`-m68020-60`); `src/cpucheck.c` alone is built
for the 68000, so a 68000 or 68010 gets a requester saying so. The quickest way is the
[amiga-gcc container](https://github.com/reinauer/container-amiga-gcc):

    docker run --rm -v $PWD:/src -w /src stefanreinauer/amiga-gcc:16.2-rc23 make

GCC 13.4 is not recommended: it can drop the register arguments of an
inline OS call when a multiply helper call ends up between them (see
`tests/aligntest/gcc-register-asm-bug-report.txt`).

| Target | Result |
|---|---|
| `make` | `AmigaMCE`, `amigamce-<version>.lha` and `amigamce-<version>.adf` (a data disk holding the archive) |
| `make test` | The AGS and Pictures folder readers under vamos; the replayer, radio CSV and Plex XML readers on the host |
| `make art` | Regenerates `src/art.c` (logo, menu icons, palette) |
| `make fonts` | Regenerates the bitmap fonts: `amce-pixel` from Pixelify Sans, `amce-galmuri*` from Galmuri |
| `make icons` | Regenerates the Workbench icons in `docs/` |

Every push to `main` is built and tested by GitHub Actions.

### Versions and releases

The version comes from `git describe --tags`. The minor number counts the
library views: it goes up by one each time a new one joins the menu (Mods
brought 0.4, Music from a Plex server 0.5). Builds between tags are named
after the last one (`0.5.0-12-gabcdef`); a patch release gets its own tag
(`v0.5.1`).

Pushing a `v*` tag builds a draft GitHub release with the archive and ADF
attached. Publishing that release uploads it to Aminet.

### Layout

    src/            the program (main, gfx, ags, modplay, stream, plex, net, ...)
    tests/          vamos and host tests, and a synthetic AGS tree
    tools/          host-side generators for the art, fonts and icons
    assets/         the Amiga fonts, their sources and their licences
    docs/           Workbench icons
    notes/          measurements kept for later work

## Licence and credits

AmigaMCE © 2026 Brian King, under the [MIT licence](LICENSE).

The fonts (Pixelify Sans, Galmuri and Rubik, and the Amiga fonts made from
them) are under the SIL Open Font Licence 1.1 and are not covered by the MIT
licence; see `assets/fonts-src/`. The EaglePlayer host follows HippoPlayer's
(MIT, © 2017 Kari-Pekka Koljonen). FLAC is decoded by flacdec.library
(© 2026 Renaud Schweingruber, with dr_flac by David Reid, public domain or
MIT-0) where it is installed; it is not included, as it may only be passed
on in its own archive. [CREDITS.txt](CREDITS.txt) has the details.

Amiga is a trademark of Amiga Corporation. AGS, WHDLoad, Plex and other
names belong to their owners; AmigaMCE is not affiliated with any of them.
