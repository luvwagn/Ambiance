# AmigaMCE

**A fast, full-screen media centre for the Amiga.** Games and demos from
[Amiga Game Selector (AGS)](https://github.com/MrV2K/Amiga-Game-Selector)
or [iGame](https://github.com/walkero-gr/iGame),
tracker music, internet radio, your Plex music and photo libraries and your pictures,
all in one place, driven by keyboard, mouse or joystick, in the spirit of
Kodi and Windows Media Center.

Copy its drawer anywhere and double-click it. On an AGS 3 installation it
is a drop-in companion: AGS's games, demos, modules and pictures are found
by themselves. On any other Amiga, Settings > Folders says where your
music, modules and pictures are; games and demos come from iGame's list
where iGame is installed. Everything plays inside AmigaMCE, on the
Amiga's own screen and sound; it never switches you out to another program.

It runs on Kickstart 3.1 and up with a 68020 or better, from a plain A1200
to an accelerated or RTG machine, on OCS, ECS or AGA, and scales what it asks
of the Amiga to how fast it really is.

## What it does

### Games and demos (AGS, iGame)

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
- Without AGS, or beside it (an "iGame" folder at the end of Games and
  Demos), the games and demos of [iGame](https://github.com/walkero-gr/iGame)'s
  list (`gameslist.csv` in iGame's drawer): Favourites, All, Most played,
  By genre, By chipset and By year, with iGame's `igame.iff` screenshot and
  the entry's genre, year, players, chipset and times played. AmigaMCE looks
  for iGame by itself the first time (the `iGame:` assign, the usual
  drawers, then the hard disks), and keeps the drawer it finds; Settings >
  Folders > iGame chooses another, or None. A title starts as iGame starts
  it: WHDLoad with its icon's tooltypes, from its own drawer. iGame's list
  is only read.
- Without AGS or WHDLoad, on an Amiga with its network up, Games offers to
  install WHDLoad: its user package fetched from whdload.de, unpacked by
  xadmaster.library (or LhA in C:), and WHDLoad and its tools put in C:.
  Settings > Folders > WHDLoad offers it too.

### Music

- **Mods:** ProTracker modules played by AmigaMCE's own replayer, after the
  ProTracker 2.3D play routine and timed by a CIA timer. Hundreds of other
  formats (OctaMED, TFMX, Future Composer, Hippel, AHX, SID...) play through
  EaglePlayer and DeliTracker player modules where they are installed (AGS
  has them). 6- and 8-channel MODs (6CHN, 8CHN, FLT8...) play on the same
  replayer, channels 5 to 8 mixed onto Paula's four the way ProTracker 4.1
  does (its mixer, by Andreas Wendorf). XM, S3M and IT are mixed by
  modplug.library on a 68020 or better. Packed modules are unpacked with
  xfdmaster.library.
  Modules come from the `MODS:` assign and the folders of Settings > Folders
  > Modules, and on AGS from `ST-00:Modules`, `ST-00:C64Music` and `AGS:Mods`.
- **Radio:** internet radio stations from
  [radio-browser.info](https://www.radio-browser.info), by top voted, most
  listened to, trending, chiptune, demoscene, genre or country (A to Z,
  your country first), showing only stations an Amiga can play: MP3,
  working, and no faster than this Amiga copes with. Shows the station's
  current song titles. With AmiSSL 5 and Settings > Networking > HTTPS on,
  stations that stream over HTTPS are listed and play too, marked HTTPS in
  the lists and on Now Playing.
- **Plex:** the music libraries of a
  [Plex Media Server](https://www.plex.tv) on your network, by artist,
  album or recently added, with covers and details beside the list. Plex
  converts each track to an MP3 this Amiga can decode: 22 kHz mono on a
  68020/030, 24 kHz stereo on a 68040/060 or faster; a fast Amiga with
  flacdec.library plays MP3 and FLAC tracks as stored. Plain HTTP on your
  own network, so no AmiSSL is needed; with it, you can sign in at plex.tv
  instead of setting a token by hand, and your account's servers are
  listed too.
- **Music files:** MP3, FLAC, WAV, AIFF and 8SVX files on the Amiga
  itself, under Music next to Plex: the `MUSIC:` assign if you have one,
  and up to eight folders chosen in Settings > Folders > Music. Only
  folders with music somewhere inside are listed (in a folder of more than
  64 folders, all of them). Beside the list: the
  file's tags (ID3, FLAC's Vorbis comments, AIFF and 8SVX names), format,
  length and cover (the folder's cover file, or the picture inside the
  file), which Now Playing shows too. A folder's files play in order, with
  seeking.
- **A beets library:** with a [beets](https://beets.io) database in
  `MUSIC:` (`musiclibrary.db` or `library.db`), Music gets *Music library
  (beets)*: Artists, Albums and Recently added, then an artist's albums and
  an album's tracks, straight from the database instead of the folders
  (`src/beets.c`). The database is only read, never written, through
  AmigaMCE's own `Libs/amcesql.library` (SQLite 3.34, `src/sqlib`). Its
  albums are read once and kept in `Cache/beets.idx` until the database
  changes; an album's tracks come from it each time. beets' Windows paths
  are mapped to `MUSIC:` by finding which part of the first track's path
  is there. Formats AmigaMCE cannot play (WMA, AAC) and file names Latin-1
  cannot spell are left out.
- **Now Playing:** title, artist, album and cover; a level meter per channel;
  a position bar you can click to seek; previous, back, play/pause, stop,
  forward, next, loop (the module, track or file again at its end, until
  turned off; kept) and volume. The music plays on while you browse, and the
  list you came from marks what is playing.

### Pictures

- Browses the `PICTURES:` assign, the folders of Settings > Folders >
  Pictures and AGS's `Media:Images`, with a preview of the selected picture or folder,
  shows pictures full screen on AmigaMCE's own screen (no mode switch), and
  plays folders as slideshows. Pictures are decoded by the installed
  datatypes and shown in their own colours where the screen allows it.
  Moving the mouse shows the pointer and buttons for the previous picture,
  the slideshow (play or pause) and the next. One picture gives way to
  the next as Settings > Display > Slideshow transition says: at once, a
  fade through black, a cross-fade, tiles at random, or tiles swept
  across. On a true-colour screen the fades are mixed by the CPU in as
  many steps as fit in 0.8 s, and an Amiga too slow for that finishes in
  tiles; on a screen with a colour table each is a fade of its colours.
- **Plex photos:** the photo libraries of your Plex server, at the top of
  Pictures: albums and photos with a preview beside the list, each photo
  full screen and slideshows. The server makes each photo the size of the
  screen (PNG; JPEG above 640x512), so the Amiga decodes no more than it
  shows; the next one is fetched while you look at this one, and a
  slideshow waits for a slow one rather than skipping it.

### Videos

- Plays IFF ANIMs (ANIM-5, 7 and 8) from the `VIDEOS:` assign, the folders
  of Settings > Folders > Videos and AGS's `Media:Animations`, with a frame
  of the selected one beside the list (a later one when it starts blank),
  its size, colours, frame count and length.
- Each plays on a screen in its own display mode (HAM, extra halfbrite,
  interlace, overscan), read from the file a frame at a time and decoded
  straight into the bitplanes, two buffers shown in turn; so a 28 MB one
  plays on a 2 MB Amiga.
- Its sound (8SVX samples and SCTL commands) plays in step with the
  pictures, by AHI when that is the sound output, else on Paula; what was
  playing waits meanwhile (a module paused, a station or a track stopped
  and started again afterwards).
- Moving the mouse brings up controls over the picture: from the start,
  play/pause, stop, loop, volume (when it has sound), the time and the
  length; a click on the line of how far it has got goes there. At its end
  it goes back to its first frame and waits there, stopped, unless loop is
  on.
- In a Workbench window it is made as large as the window holds, larger or
  smaller; the full-screen button shows it on the whole display, on a
  screen in Workbench's own mode (no mode switch), and back.
- On a ZZ9000 it also plays MPEG video: `.mpg`, `.mpeg`, `.vob` (MPEG-1 or
  MPEG-2 Program Streams, and bare MPEG-1 video), and transport streams
  (`.ts`, `.m2ts`, `.mts`) with MPEG-2 video, which AmigaMCE repacks as it
  reads them. The card decodes the pictures and shows them on its overlay,
  freely scaled to fill the picture's place, with the same controls, keys
  and full screen as an ANIM; its sound (MPEG audio, or AC3) comes back
  from the card and plays as music does (AHI or Paula), the pictures kept
  in time with it. Where the card will not decode AC3 (Amiberry's
  emulated ZZ9000), a 68040 or 68060 decodes it instead with
  `Libs/ac3.library` or `Libs/ac3fpu.library` (liba52), the card showing
  the pictures. Clicking the line of how far it has got goes there.
  It needs the card's firmware with media sessions, Picasso96, and
  AmigaMCE's screen or window on the card (16-bit or more). H.264, HEVC
  and AAC are not the card's: the preview says so (AAC: played without
  its sound).
- Without a ZZ9000 (or with Settings > Display > MPEG video on the ZZ9000
  off), MPEG-1 is decoded by the CPU, its sound by mpega.library. Its
  pictures go to the graphics card's YUV overlay where the screen has one
  (Picasso96's PIP), which scales them; otherwise (or with Settings >
  Display > Use overlay when available off) they are made RGB and drawn,
  on an RTG screen of 16 bits or more. When the CPU falls behind, the
  video's B pictures (most of a VCD's) are passed over, not decoded,
  until it has caught up.
- DVDs: while a DVD-Video disc is mounted in a drive (any CD/DVD
  filesystem: ODFileSystem, CDFileSystem, a UDF handler), its volume is
  at the top of Videos with a disc beside it, within a second of going
  in. Fire plays it on the ZZ9000: its film when one title is most of
  the disc, else its programmes one after another (a disc of episodes or
  music videos). N and P, or the controls' |<< and >>| (in place of from
  the start and loop), go to the previous and next chapter or programme.
  Of its sound tracks the one the card is likeliest to play is chosen
  (MPEG audio, then AC-3 in stereo, then any AC-3), and only that one is
  sent. The disc is read through its drive's device (`HD_SCSICMD`) by
  `Libs/amcedvd.library` (libdvdread and libdvdnav, `src/dvdlib`). No
  encrypted (CSS) discs.
- DVD images: a `.iso` in Videos' folders is listed with a disc beside
  it, and Fire plays it as a disc in a drive is played (its menus too,
  decoded by the CPU). Its preview says what it holds (a DVD-Video disc:
  `VIDEO_TS` in its ISO 9660 root folder) and its size, a few KB of it
  read. DOS's `Seek` is 32-bit, so past 2 GB an image is positioned with
  the 64-bit packets MorphOS brought in (`ACTION_GET_FILE_SIZE64`,
  `ACTION_CHANGE_FILE_POSITION64`), which UAE's and Amiberry's host file
  system answers: a folder of the host's mounted as a volume plays an
  image of any size. A file system that does not answer them is
  positioned by `Seek` in two steps, which reaches 4 GB at most if it
  takes positions past 2 GB at all; `AmigaMCE.log` says when an image
  cannot be read.
- DVD menus: when the CPU decodes a DVD (Settings > Video: MPEG on the
  ZZ9000 off, or no ZZ9000) and the disc has menus, Fire plays it from
  its start as a DVD player does: its intro, then its menus. Their
  buttons are drawn over the picture (on the overlay or drawn alike, at
  the picture's own size); the arrows, Return or the stick and fire move
  between and press them, the pointer selects the one under it and a
  click presses it. M, a CD32 pad's green button or the controls' menu
  button (three bars) bring its menu back while a title plays, and from
  a menu go back to the title. Forced subtitles show over the film. The
  disc's own rules are kept: what it forbids at a point is greyed. On
  the ZZ9000 a DVD plays its film as before (the card's pictures cannot
  be drawn over).

### Screens and looks

- Native screens: 320x200/256 lowres in 32 colours on OCS and ECS; on AGA,
  256 colours and also 640x200/256 hires and 640x400/512 laced. RTG:
  640x480, 800x600 and 1024x768.
- Laced screens choose from several pixel fonts; big RTG screens scale the
  whole layout up, in a pixel look or a smooth, anti-aliased one.
- Or a window on Workbench, when Workbench is on a graphics card at 15 bits
  or more: Settings > Display > Workbench window, or the program icon's `WINDOWED`
  tooltype (shipped commented out as `(WINDOWED)`; take the brackets away),
  or `AmigaMCE WINDOWED` from a Shell. A choice made in Settings comes first.
  Its text is Small, Medium or Large (the window 512 x 384, 640 x 480 or
  800 x 600 at first, as far as Workbench has room); in the pixel style the
  font is the size. Resize it by the grip in its bottom right corner and
  the layout follows, the text staying its size; it opens again where and
  as large as it was left. Its Project menu (the right button over its
  title bar) has About, Settings, Full screen (Amiga-F), Iconify (Amiga-I)
  and Quit (Amiga-Q); a double-click on its title bar goes full screen too.
  In the viewer and a video, Amiga-F (as F) is their own full screen.
  Iconify (its title bar's gadget on OS 3.2, the menu, or Amiga-I, full
  screen too) closes the window, or puts the full screen behind
  Workbench's, and leaves an AppIcon with a musical note on Workbench: the
  music plays on, a video stops and the viewer closes. Double-click the
  icon (or its Open) to bring AmigaMCE back where it was; Workbench's
  Tools > Quit AmigaMCE quits. Its zoom gadget swaps the window for a mini
  player: the cover, what plays and where it is in its album or list, the
  time and its bar (click it to go there), Previous, Play, Stop, Next,
  mute, the volume and a meter, in the pixel font at 1x, 2x or 3x by
  Workbench's size (360 x 100 at 1x). Its own zoom gadget, a click on the
  cover, Esc or Return bring the window back. Space plays and pauses,
  Left/Right seek, Up/Down the volume, M mutes, N and P skip. Pictures and videos go full screen from it (their button, F or
  Amiga-F) on a screen in Workbench's own mode, so the monitor does not
  change mode. When Workbench's screen mode is changed (ScreenMode prefs),
  the window closes as Workbench does, so no "Intuition is attempting to
  reset the Workbench screen" requester, and opens again on the new
  screen, or full screen if a window cannot be there now. AmigaMCE watches
  the ScreenMode, Overscan and Font prefs in ENV:Sys as IPrefs does, and
  closes its window as soon as one changes (Intuition asks for windows to
  be closed before Workbench is told anything). Full screen because
  Workbench could not take a window, it goes back into one when Workbench's
  mode changes to one that can; on OS 3.5, 3.9 and 3.2
  Workbench's setup/cleanup hook covers its other ways of closing too.
- Flicker-free scrolling text and lists. A long list always shows a scroll bar
  (how long it is, and where you are in it), which the mouse can drag or click.

## Requirements

| | |
|---|---|
| **Needed** | Kickstart 3.1 or later and a 68020 or better, on OCS, ECS, AGA or RTG |
| **For Games and Demos** | Amiga Game Selector 3 (AmigaMCE finds it through the `AGS:` assign), or iGame's list (its `gameslist.csv`) with WHDLoad; to install WHDLoad from Games, xadmaster.library or LhA in C: |
| **For Radio and Plex** | A running TCP/IP stack (bsdsocket.library: Roadshow, for example, or an emulator's) |
| **For MP3 and FLAC** | mpega.library for MP3 (the installer offers it); [flacdec.library](https://aminet.net/package/util/libs/flacdec) for FLAC (its 68060 build on a 68060). Whether this Amiga is fast enough for FLAC is measured at each start and shown in About > System information |
| **Optional** | EaglePlayer/DeliTracker players, modplug.library and xfdmaster.library for more module formats; picture datatypes for the formats you have; AHI for a sound card; AmiSSL 5 for HTTPS (secure radio stations, signing in at plex.tv) |

Radio, Plex and music files can play through AHI (16-bit, at AHI's own
mode and rate) instead of Paula, and their MP3 can be decoded and played by
an MHI driver instead of mpega.library: Settings > Sound. Modules play on
Paula.

## Installing

1. Double-click `Install-AmigaMCE` in the archive. It asks where AmigaMCE
   should go and which languages to install (your Workbench's is ticked).
   Or copy the `AmigaMCE` drawer to your hard drive yourself, all languages
   included; keep its `Fonts` drawer next to the program.
2. On AGS, if it boots straight into its own menu, quit AGS to Workbench first.
3. Double-click AmigaMCE.

## Languages

English, Deutsch, Français, Italiano, Español, Svenska, Suomi, Dansk,
Norsk, Nederlands, Português, 日本語 (Japanese), Ελληνικά (Greek) and 한국어
(Korean). AmigaMCE follows the
Workbench's language (Locale prefs) when its catalog is installed;
Settings › General › Language picks another, and is greyed out showing the
one in use when no other is installed. Workbench 3.x has no Japanese or
Korean locale, so they are only ever picked there (their catalogs'
languages are `nihongo`, a name of AmigaMCE's own, and `korean`); AmigaMCE
draws them with its own glyph packs and needs nothing from the system for
them. Korean keeps to the 2,350 Hangul syllables of KS X 1001, all of which
the 15-pixel pack has. Greek likewise needs no
Greek fonts, keymap or ReplaceTopaz: its catalog's language is `greek`, and
a Workbench whose Locale prefs call it `greek` or `ellinika` gets it by
itself.

Text in the source is written in English as `_("...")`, or `N_("...")` in a
table. A string's catalog ID is a hash of its English, so nothing else
needs numbering. `catalogs/` holds `AmigaMCE.cd`, made from the source,
and one `.ct` per language in FlexCat's format, kept in UTF-8. Catalogs
are read by AmigaMCE itself and known by the language written in them, so
their drawers have plain ASCII names (`Catalogs/francais`). Japanese,
Chinese and Korean text can go in a catalog too, drawn from the glyph packs
in `Fonts/amce-cjk`; in Japanese and Korean the clock and file dates are written year,
month, day, and wrapped text keeps 、。」 and small kana off the start of a
line. Greek is kept as two bytes a letter (`src/cjk.c`), drawn from the
smooth style's own Greek letters (Geologica, sized to Rubik) or from
`Fonts/amce-cjk/greek` (Galmuri11's, at 12 pixels), which is always installed.
Greek takes more bytes than English, and a translation may be at most
twice its English's bytes and 16 more, so the Greek is written to fit.

| Command | What it does |
|---|---|
| `python3 tools/catalog.py scan src catalogs` | After changing `_()` strings: the `.cd` again, and each `.ct` given the new strings untranslated |
| `python3 tools/catalog.py check catalogs` | How much of each language is translated |
| `make catalogs` | Builds `build/catalogs/<language>/AmigaMCE.catalog`, refusing a translation whose `%` formats differ from its English |
| `python3 tools/textfit.py catalogs assets/Fonts` | Measures each translation in AmigaMCE's fonts on 320×256, 640×256 and 640×512: the menu column, the footer hints, text wider than the pane |

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

A Now playing row at the top of the lists (Games, Demos, Mods, Music,
Pictures, Videos, Radio and Plex's) takes you back to what is playing.
What was playing when AmigaMCE quit comes back at its next start, loaded
and stopped, with its playlist and the row: Play goes on from where it
was (a station from now). It is kept in `ENVARC:AmigaMCE/Resume`.

### Setting up Plex

1. Settings > Networking > Plex: turn Plex on and choose your server under
   Plex server. Servers on your network are found automatically.
2. If your server asks for a sign-in: with AmiSSL 5 installed, turn on
   Settings > Networking > HTTPS, then choose Settings > Networking > Plex >
   Sign in at plex.tv. It shows a short code: enter it at
   [plex.tv/link](https://plex.tv/link) on a phone or computer. Your
   account's servers then appear under Plex server, each with its own token.
   Without AmiSSL, give AmigaMCE your Plex token from the Shell:
   `SetEnv SAVE AmigaMCE/PlexToken <token>`. About > Plex in AmigaMCE
   explains where to find it.
3. Your music libraries appear under Music, your photo libraries at the
   top of Pictures.

## Settings

| Page | What it holds |
|---|---|
| General | Language, Debug |
| Display | Workbench window (on: a window on Workbench, where it can take one; off: full screen), Screen mode (full screen) or Text size (a window), Style (fonts and looks), Double buffering (RTG screens), Visualisation, Jump scrolling (very long lists step a letter at a time), Slideshow transition (Off, Fade, Cross-fade, Tiles or Wipe; a screen with a colour table fades), Videos on a screen of their own (off: on AmigaMCE's screen or window, where it is 16-bit RTG), MPEG video on the ZZ9000 (where there is one; off: MPEG-1 decoded by the CPU), Use overlay when available (RTG; MPEG decoded by the CPU shown on the graphics card's YUV overlay, which scales it; off: drawn; greyed without a graphics card) |
| Sound | Paula or AHI for radio, Plex and music files; MP3 by mpega.library or an MHI driver |
| Folders | Where Music, Mods, Pictures and Videos find their files: the `MUSIC:`, `MODS:`, `PICTURES:` and `VIDEOS:` assigns on or off, and up to eight folders each to add or take out (AGS's own drawers are found by themselves); iGame (its drawer, found by itself at first, or None); WHDLoad (installed, or installing it from whdload.de) |
| Networking | HTTPS (with AmiSSL 5: secure radio stations, signing in at plex.tv), Radio (the highest bitrate listed, which radio-browser.info server to use), Plex (on/off, server, signing in at plex.tv, lower quality audio), Stream buffer, Network interface |

Settings are saved in `ENVARC:AmigaMCE/`. Debug shows the CPU load and
memory use in the header and writes a timing log to `AmigaMCE.log` next to
the program, which is the first thing to look at if something misbehaves.

## Not there yet

- DVD menus when the ZZ9000 decodes (its pictures cannot be drawn over:
  the CPU's decoding plays them); choosing subtitles (forced ones show);
  encrypted (CSS) discs; DVD images over 4 GB on a file system without
  64-bit packets.
- MPEG video without a ZZ9000: on RTG screens for now; AGA screens come
  next.
- Modules play on Paula only; AHI for them comes later.

## Building

AmigaMCE builds with [AmigaPorts'
`m68k-amigaos-gcc`](https://github.com/AmigaPorts/m68k-amigaos-gcc) 16.2
(16.2-rc24 or later; the continuation of bebbo's amiga-gcc) and its libnix,
for the 68020 and up (`-m68020-60`); `src/cpucheck.c` alone is built for the
68000, so a 68000 or 68010 gets a requester saying so. mpegv.library and
ac3.library are submodules (`git clone --recursive`, or
`git submodule update --init`). The
[amiga-gcc container](https://github.com/reinauer/container-amiga-gcc) has
the other tools the build and tests use (vamos, lha); its newest image is
still 16.2-rc23, so put rc24's
[release](https://github.com/AmigaPorts/m68k-amigaos-gcc/releases/tag/v16.2-rc24)
ahead of it on the `PATH`, as `.github/workflows/ci-build.yml` does.

GCC 13.4 is not recommended: it can drop the register arguments of an
inline OS call when a multiply helper call ends up between them (see
`tests/aligntest/gcc-register-asm-bug-report.txt`).

| Target | Result |
|---|---|
| `make` | `AmigaMCE` and `amigamce-<version>.lha` (with the languages and `Install-AmigaMCE`) |
| `make test` | The AGS and Pictures folder readers under vamos; iGame's list (on the host and under vamos) and WHDLoad's installation from its package (under vamos); the replayer, radio CSV and Plex XML readers on the host; DVD navigation on a test disc (`tests/mkdvd.sh`), on the host and through `amcedvd.library` under vamos; MPEG-1 and MPEG-2 decoding, `mpegv.library`'s pictures against libmpeg2's own C, on the host and under vamos |
| `make dvdlib` | `build/dvdlib/amcedvd.library` (also part of `make`, into the archive's `Libs`) |
| `make mpegvlib` | `extras/mpegv/build/mpegv.library` (also part of `make`, into the archive's `Libs`; needs the submodule: `git submodule update --init`) |
| `make mpeg-prof MPG=file.mpg [N=60]` | A 68020's cycles for MPEG-1 pictures under vamos: through `mpegv.library`, and by function (its profiler) |
| `make art` | Regenerates `src/art.c` (logo, menu icons, palette) |
| `make fonts` | Regenerates the bitmap fonts: `amce-pixel` from Pixelify Sans, `amce-galmuri*` from Galmuri |
| `python3 tools/mkcjk.py <galmuri-dist> <ark-pixel-font> assets/Fonts/amce-cjk` | Regenerates the Japanese, Chinese and Korean glyph packs from Galmuri's BDFs and Ark Pixel Font's 12px glyphs |
| `python3 tools/mkgreek.py assets/fonts-src/Galmuri11-greek.bdf assets/Fonts/amce-cjk/greek` | Regenerates the Greek glyph pack (also part of `make fonts`) |
| `make icons` | Regenerates the Workbench icons in `docs/` |

Every push to `main` is built and tested by GitHub Actions.

### Versions and releases

The version comes from `git describe --tags`. The minor number counts the
library views: it goes up by one each time a new one joins the menu (Mods
brought 0.4, Music from a Plex server 0.5, Plex photos 0.6, Videos 0.7). Builds between tags are named
after the last one (`0.5.0-12-gabcdef`); a patch release gets its own tag
(`v0.5.1`).

Pushing a `v*` tag builds a draft GitHub release with the archive
attached. Publishing that release uploads it to Aminet.

### Layout

    src/            the program (main, gfx, ags, modplay, stream, plex, net, ...)
    tests/          vamos and host tests, and a synthetic AGS tree
    tools/          host-side generators for the art, fonts and icons
    assets/         the Amiga fonts, their sources and their licences
    docs/           Workbench icons and the Installer script
    catalogs/       the translations (AmigaMCE.cd and a .ct per language)
    notes/          measurements kept for later work

## Licence and credits

AmigaMCE © 2026 Brian King, under the [MIT licence](LICENSE).

The fonts (Pixelify Sans, Galmuri, Ark Pixel Font, Rubik and Geologica, and the Amiga fonts made from
them) are under the SIL Open Font Licence 1.1 and are not covered by the MIT
licence; see `assets/fonts-src/`. The EaglePlayer host follows HippoPlayer's,
and Now Playing's spectrum analyser is HippoPlayer's (`src/vis.c`, its FFT
`src/hfft.s`; MIT, © 2017 Kari-Pekka Koljonen). FLAC is decoded by flacdec.library
(© 2026 Renaud Schweingruber, with dr_flac by David Reid, public domain or
MIT-0) where it is installed; it is not included, as it may only be passed
on in its own archive. [CREDITS.txt](CREDITS.txt) has the details.

MPEG-1 and MPEG-2 video without a ZZ9000 (files, transport streams and
DVDs) is decoded by `mpegv.library` 2, a project of its own (`extras/mpegv`,
a git submodule: [mpegv.library](https://github.com/luvwagn/mpegv.library),
GPL-3.0-or-later, its source there): libmpeg2 0.5.1 (GPL-2.0-or-later),
made faster on a 68k, behind an interface for any Amiga program. AmigaMCE opens it at run time from its Libs. Clone
with `git clone --recursive`, or `git submodule update --init` after.

DVDs are read by `amcedvd.library`: libdvdread 7.1.1 and libdvdnav 7.0.0
(VideoLAN, `extras/libdvd/`, three lines changed: `extras/libdvd/CHANGES`)
built as an Exec library (`src/dvdlib/`, `make dvdlib`). It is
GPL-2.0-or-later; AmigaMCE (MIT) opens it at run time through an
interface of its own (`src/dvdlib/include`, MIT) and compiles none of its
code. Whoever is given the library must be able to get its complete
source: `src/dvdlib`, `extras/libdvd` and the Makefile's dvdlib part go
with any distributed archive that holds it. See `src/dvdlib/README`.

AC-3 sound the ZZ9000 will not decode is decoded by `ac3.library` or
`ac3fpu.library`: liba52 0.8.0 (VideoLAN) built as Exec libraries, for
any program, as mpega.library is. They are their own project
(`extras/ac3`, a git submodule:
[luvwagn/ac3.library](https://github.com/luvwagn/ac3.library),
GPL-3.0-or-later, its source there, not in the archive), built by its
own Makefile (`make ac3lib`, tested by `make test`), and opened at run
time through an interface of their own (`extras/ac3/include`, MIT).

MP3 (and MP2) is decoded by `mpega.library` from `LIBS:` (Stéphane
Tavenard's, a clone, or the ZZ9000's, which decodes on the card) or, as
Settings > Sound chooses, by **`amcempega.library`**: libmad behind
mpega's functions under a name of its own, so it never clashes with the
one in `LIBS:`, with one more of its own that writes each frame straight
into AmigaMCE's FIFO. It is its own project (`extras/mpga`, a git
submodule: [luvwagn/mpga.library](https://github.com/luvwagn/mpga.library),
GPL-3.0-or-later), built by its own Makefile (`make mpgalib`, tested by
`make test`). Unless Settings > Sound says otherwise, MP3 is decoded by
it, except where `LIBS:mpega.library` is the ZZ9000's.

The ZZ9000 video player (`src/zz9k.c`) is built with the ZZ9000 SDK's
headers (BlitterStudio's zz9000-firmware, fetched by the Makefile at a
pinned commit into `build/zz9k-sdk`), which are MIT. zz9k.library itself
is GPL-3.0-or-later, with a published section 7 permission for any program
that opens it and calls it through those headers, so AmigaMCE builds may
be distributed. See `third_party/zz9k-sdk.txt` and the SDK's own
`LICENSING.md`.

Amiga is a trademark of Amiga Corporation. AGS, WHDLoad, Plex and other
names belong to their owners; AmigaMCE is not affiliated with any of them.
