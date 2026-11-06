# AmigaMCE Makefile for GCC (m68k-amigaos), modelled on xSysInfo
# Copyright (c) 2026 Brian King
#
#   make            build AmigaMCE, the LHA archive and the ADF (holding the LHA)
#   make test       run the AGS parser tests under vamos
#   make art        regenerate src/art.c (logo, icons, palette)
#   make fonts      regenerate the bitmap fonts (amce-pixel 12/24/36, Galmuri)
#   make smooth     regenerate the smooth-style artwork for RTG screens
#   make icons      regenerate the Workbench .info icons
#   make preview    render true-aspect PNG previews of the Home Screen

ADATE   := $(shell date '+%-d.%-m.%Y')
# FULL_VERSION is 0.1.0, or 0.1.0-3-gabcdef-dirty between tags
FULL_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null | sed -E 's/^v//')
ifeq ($(strip $(FULL_VERSION)),)
FULL_VERSION := 0.1.0
endif
FULL_VERSION := $(FULL_VERSION)
VERSION_STAMP = build/version

CC     = m68k-amigaos-gcc
STRIP  = m68k-amigaos-strip
PYTHON ?= python3

# A 68020 or better (its libraries from the toolchain's m68020-60
# multilib); src/cpucheck.c alone is built for the 68000, to say so there.
CPU     = -m68020-60
CFLAGS  = -Os $(CPU) -noixemul -Wall -Wextra -Wno-pointer-sign \
          -DAMCE_VERSION="\"$(FULL_VERSION)\"" -DAMCE_DATE="\"$(ADATE)\""
LDFLAGS = -noixemul $(CPU)
LIBS    = -lamiga

SRCS = src/main.c src/gfx.c src/view.c src/thumb.c src/ilbm.c src/pics.c src/loader.c src/dirscan.c src/index.c src/smooth.c src/textbox.c src/mods.c src/modplay.c src/ptcore.c src/unpack.c src/eagle.c src/mixplay.c src/ags.c src/caps.c src/art.c src/util.c \
       src/log.c src/debug.c src/net.c src/radio.c src/radiocsv.c src/stream.c src/plex.c src/plexxml.c src/songinfo.c src/cjk.c src/lang.c \
       src/vis.c src/vistab.c src/cpucheck.c
OBJS = $(patsubst src/%.c,build/%.o,$(SRCS)) build/eaglehost.o build/hfft.o
VASM ?= vasmm68k_mot

TARGET     = AmigaMCE
LHA_DIR    = build/lha/AmigaMCE
LHA_NAME   = amigamce-$(FULL_VERSION).lha
LHA_OPTS  ?=
DISK       = amigamce-$(FULL_VERSION).adf
DISK_TITLE = AmigaMCE

FONT_FILES = assets/Fonts/amce-pixel.font assets/Fonts/amce-pixel/12 \
             assets/Fonts/amce-pixel/24 assets/Fonts/amce-pixel/36
# The laced screens' Galmuri fonts (tools/mkbdf.py): name and line height
GALMURI    = amce-galmuri11:16 amce-galmuri11b:16 amce-galmuri14:21 amce-galmuri14b:21
FONT_FILES += $(foreach f,$(GALMURI),assets/Fonts/$(word 1,$(subst :, ,$(f))).font \
              assets/Fonts/$(subst :,/,$(f)))
# Smooth style, one file per RTG screen height (tools/mksmooth.py)
SMOOTH_FILES = assets/Fonts/amce-smooth/480 assets/Fonts/amce-smooth/600 \
               assets/Fonts/amce-smooth/768 assets/Fonts/amce-smooth/logo1 \
               assets/Fonts/amce-smooth/logo2 assets/Fonts/amce-smooth/logo3
# Japanese, Chinese and Korean glyphs (tools/mkcjk.py: Galmuri, Ark Pixel)
CJK_FILES  = assets/Fonts/amce-cjk/12 assets/Fonts/amce-cjk/15
ICONS      = docs/AmigaMCE.info docs/drawer.info docs/Disk.info docs/readme.info
LICENCES   = assets/fonts-src/OFL-PixelifySans.txt assets/fonts-src/OFL-Rubik.txt \
             assets/fonts-src/OFL-Galmuri.txt assets/fonts-src/OFL-ArkPixel.txt CREDITS.txt LICENSE

.PHONY: all clean test test-replayer check-formats check-syscalls art fonts smooth icons preview lha disk catalogs FORCE_VERSION

all: $(TARGET) lha disk

# Compiler flags are not file dependencies: rebuild only when the
# embedded version or date actually changes.
$(VERSION_STAMP): FORCE_VERSION
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(FULL_VERSION)' '$(ADATE)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

build/eaglehost.o: src/eaglehost.s
	@mkdir -p build
	@echo "  ASM   $@"
	@$(VASM) -Fhunk -m68000 -quiet -o $@ $<

# HippoPlayer's FFT (the spectrum), as HippoPlayer assembles it
build/hfft.o: src/hfft.s
	@mkdir -p build
	@echo "  ASM   $@"
	@$(VASM) -Fhunk -m68000 -quiet -o $@ $<

build/cpucheck.o: src/cpucheck.c
	@mkdir -p build
	@echo "  CC    $@ (68000)"
	@$(CC) $(filter-out $(CPU),$(CFLAGS)) -m68000 -c -o $@ $<

build/%.o: src/%.c src/amce.h $(VERSION_STAMP)
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@echo "  STRIP $@"
	@$(STRIP) $@
	@wc -c < "$@" | awk '{printf "$@ successfully compiled (%s bytes)\n", $$1}'

# ------------------------------------------------------------------ tests
build/test_ags: tests/test_ags.c src/ags.c src/pics.c src/dirscan.c src/index.c src/util.c src/amce.h
	@echo "  CC    $@"
	@$(CC) -O1 -m68000 -noixemul -Wall -Wno-pointer-sign -o $@ tests/test_ags.c src/ags.c src/pics.c src/dirscan.c src/index.c src/util.c

build/test_songs: tests/test_songs.c src/songinfo.c src/mods.c src/ags.c src/pics.c src/dirscan.c src/index.c src/util.c src/radiocsv.c src/lang.c src/amce.h
	@echo "  CC    $@"
	@$(CC) -O1 -m68000 -noixemul -Wall -Wno-pointer-sign -o $@ tests/test_songs.c src/songinfo.c src/mods.c src/ags.c src/pics.c src/dirscan.c src/index.c src/util.c src/radiocsv.c src/lang.c

build/test_fft: tests/test_fft.c src/vistab.c build/hfft.o
	@echo "  CC    $@"
	@$(CC) -O1 -m68020 -noixemul -Wall -o $@ tests/test_fft.c src/vistab.c build/hfft.o

# amiga.lib's sprintf is exec's RawDoFmt, where %d/%u/%x/%c read 16 bits.
# Every numeric conversion must carry the l modifier with a long argument.
check-formats:
	@if grep -nE 'printf *\(.*%[-+ 0#]*[0-9]*(\.[0-9]+)?[diuxXc]' src/*.c; then \
		echo "error: use %l with a long argument (RawDoFmt reads %d/%u/%x/%c as 16-bit)"; exit 1; fi
	@echo "  CHECK formats ok"

# GCC 13.4 could move a helper call (__mulsi3 for a 32-bit multiply on the
# 68000) between loading an inline OS call's register arguments and the
# call, which then got clobbered arguments: with 18-byte list entries
# CopyMem copied from UtilityBase onto the stack. Fixed in GCC 16.2-rc23
# (see tests/aligntest/gcc-register-asm-bug-report.txt); the scan of the
# generated code stays as a guard against a regression.
ASM_DIR = build/asm
check-syscalls: $(SRCS) src/amce.h
	@mkdir -p $(ASM_DIR)
	@set -e; for f in $(SRCS); do \
		$(CC) $(CFLAGS) -S -o $(ASM_DIR)/$$(basename $$f .c).s $$f; done
	@$(PYTHON) tools/check_syscalls.py $(patsubst src/%.c,$(ASM_DIR)/%.s,$(SRCS))
	@echo "  CHECK OS-call arguments ok"

test: check-formats check-syscalls build/test_ags build/test_songs build/test_fft test-replayer test-radio test-plex
	@# git keeps no empty folders: the fixtures' own (empty AGS menus) made again
	@while IFS= read -r d; do mkdir -p "$$d"; done < tests/fixtures/empty-dirs.txt
	@rm -rf build/vt && mkdir -p build/vt
	@cd build && vamos -V AGS:$(abspath tests/fixtures/AGS2) -V Images:$(abspath tests/fixtures/Images) -V T:$(abspath build/vt) test_ags
	@rm -rf build/songs build/ram && mkdir -p build/ram
	@$(PYTHON) -I tests/mksongs.py build/songs
	@cd build && vamos -V Songs:$(abspath build/songs) -V RAM:$(abspath build/ram) test_songs > test-songs.txt \
		&& echo "  CHECK music files ok" || (cat test-songs.txt; exit 1)
	@cd build && vamos -C 68020 test_fft > test-fft.txt && grep -q "sine at 3: loudest 3 " test-fft.txt \
		&& grep -q "sine at 10: loudest 10 " test-fft.txt && grep -q "sine at 31: loudest 31 " test-fft.txt \
		&& grep -q "sine at 50: loudest 50 " test-fft.txt \
		&& echo "  CHECK spectrum FFT ok" || (cat test-fft.txt; echo "FFT test failed"; exit 1)

# The ProTracker replayer on the build machine: a generated module played
# through a model of Paula; its length and its four channels checked.
HOSTCC ?= cc
build/pt_host: tests/pt_host.c src/ptcore.c src/ptcore.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/pt_host.c src/ptcore.c

# radio-browser.info's CSV, UTF-8 to Latin-1, URL encoding (radiocsv.c).
build/radio_host: tests/radio_host.c src/radiocsv.c src/radiocsv.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/radio_host.c src/radiocsv.c

# a Plex server's XML (plexxml.c).
build/plex_host: tests/plex_host.c src/plexxml.c src/plexxml.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/plex_host.c src/plexxml.c

test-plex: build/plex_host
	@build/plex_host > build/test-plex.txt && echo "  CHECK plex XML ok" || (cat build/test-plex.txt; exit 1)

test-radio: build/radio_host
	@build/radio_host > build/test-radio.txt && echo "  CHECK radio CSV ok" || (cat build/test-radio.txt; exit 1)

test-replayer: build/pt_host
	@$(PYTHON) -I tests/mkmod.py build/test.mod
	@build/pt_host build/test.mod - > build/test-replayer.txt
	@grep -q "length 0:19.440" build/test-replayer.txt && grep -q "channels heard 1111" build/test-replayer.txt \
		&& echo "  CHECK replayer ok" || (cat build/test-replayer.txt; echo "replayer test failed"; exit 1)

# ------------------------------------------------------------------ assets
art:
	$(PYTHON) tools/mkart.py assets/fonts-src/Rubik-Bold.ttf src/art.c

fonts:
	@mkdir -p build/fontgen
	$(PYTHON) tools/extract.py assets/fonts-src/PixelifySans-Regular.ttf build/fontgen/glyphs.json
	$(PYTHON) tools/amigafont.py build/fontgen/glyphs.json build/fontgen
	cp -R build/fontgen/FONTS/. assets/Fonts/
	$(PYTHON) tools/mkbdf.py assets/fonts-src/Galmuri11-latin1.bdf assets/Fonts amce-galmuri11
	$(PYTHON) tools/mkbdf.py assets/fonts-src/Galmuri11-Bold-latin1.bdf assets/Fonts amce-galmuri11b
	$(PYTHON) tools/mkbdf.py assets/fonts-src/Galmuri14-latin1.bdf assets/Fonts amce-galmuri14
	$(PYTHON) tools/mkbdf.py assets/fonts-src/Galmuri14-latin1.bdf assets/Fonts amce-galmuri14b --bold

smooth:
	$(PYTHON) tools/mksmooth.py assets/fonts-src/Rubik-VF.ttf assets/Fonts/amce-smooth 480 600 768

icons:
	$(PYTHON) tools/mkicon.py docs

preview: src/art.c $(FONT_FILES)
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-hires.png hires pal
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-lores.png lores pal
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-ntsc.png lores ntsc
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-about-hires.png hires pal about 0
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-laced.png laced pal games
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-rtg.png rtg pal games
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-glide.png hires pal glide

# ------------------------------------------------------------------ release
# The translations: catalogs/*.ct built into build/catalogs/<language>/
# AmigaMCE.catalog (tools/catalog.py checks each keeps its English's %
# formats). After changing _() strings in src/: python3 tools/catalog.py
# scan src catalogs, then translate what is new in each .ct.
CT_FILES   = $(wildcard catalogs/*.ct)
build/catalogs/.built: $(CT_FILES) catalogs/AmigaMCE.cd tools/catalog.py
	@echo "  CAT   $(words $(CT_FILES)) languages"
	@rm -rf build/catalogs
	@$(PYTHON) tools/catalog.py build catalogs build/catalogs > /dev/null
	@touch $@

catalogs: build/catalogs/.built

# The release archive: the AmigaMCE drawer with every language in it, and
# an Installer script beside it that copies the drawer and the languages
# chosen (the Workbench's ticked). LHA_EN_NAME is the same without the
# languages or the installer: the floppy's, as there is no room on it.
LHA_EN_DIR  = build/lha-en
LHA_EN_NAME = build/amigamce-en.lha
lha: $(TARGET) AmigaMCE.readme $(ICONS) docs/install.info docs/Install-AmigaMCE $(FONT_FILES) \
     $(SMOOTH_FILES) $(CJK_FILES) $(LICENCES) build/catalogs/.built
	@echo "  LHA   $(LHA_NAME)"
	@rm -rf build/lha
	@mkdir -p $(LHA_DIR)/Fonts/amce-pixel $(LHA_DIR)/Fonts/amce-smooth $(LHA_DIR)/Fonts/amce-cjk \
		$(LHA_DIR)/Licences
	@cp $(TARGET) $(LHA_DIR)/
	@cp docs/AmigaMCE.info $(LHA_DIR)/
	@cp assets/Fonts/amce-pixel.font $(LHA_DIR)/Fonts/
	@cp assets/Fonts/amce-pixel/12 assets/Fonts/amce-pixel/24 assets/Fonts/amce-pixel/36 \
		$(LHA_DIR)/Fonts/amce-pixel/
	@for f in $(GALMURI); do n=$${f%%:*}; mkdir -p $(LHA_DIR)/Fonts/$$n; \
		cp assets/Fonts/$$n.font $(LHA_DIR)/Fonts/; \
		cp assets/Fonts/$$n/$${f##*:} $(LHA_DIR)/Fonts/$$n/; done
	@cp $(SMOOTH_FILES) $(LHA_DIR)/Fonts/amce-smooth/
	@cp $(CJK_FILES) $(LHA_DIR)/Fonts/amce-cjk/
	@cp $(LICENCES) $(LHA_DIR)/Licences/
	@awk -v version="$(FULL_VERSION)" '{ gsub(/\[VERSION\]/, version); print }' \
		AmigaMCE.readme > $(LHA_DIR)/readme.txt
	@cp docs/readme.info $(LHA_DIR)/readme.txt.info
	@cp docs/drawer.info build/lha/AmigaMCE.info
	@rm -rf $(LHA_EN_DIR) && cp -R build/lha $(LHA_EN_DIR)
	@# the floppy's has no room for the 15-pixel Far East glyphs: 12 everywhere
	@rm -f $(LHA_EN_DIR)/AmigaMCE/Fonts/amce-cjk/15
	@rm -f $(LHA_EN_NAME)
	@cd $(LHA_EN_DIR) && lha aqo5 $(LHA_OPTS) ../../$(LHA_EN_NAME) AmigaMCE AmigaMCE.info
	@mkdir -p $(LHA_DIR)/Catalogs
	@cp -R build/catalogs/* $(LHA_DIR)/Catalogs/
	@cp docs/Install-AmigaMCE build/lha/Install-AmigaMCE
	@cp docs/install.info build/lha/Install-AmigaMCE.info
	@rm -f $(LHA_NAME)
	@cd build/lha && lha aqo5 $(LHA_OPTS) ../../$(LHA_NAME) AmigaMCE AmigaMCE.info \
		Install-AmigaMCE Install-AmigaMCE.info
	@echo "Created $(LHA_NAME) (and, English only, $(LHA_EN_NAME) for the floppy)"

# A data floppy carrying the English-only archive and its readme: unpack it
# to a hard drive (lha x DF0:amigamce.lha). Not bootable. The archive is
# plain amigamce.lha on the disk: with its version the name can pass the 30
# characters a file name may have there. The languages are in the full
# archive (Aminet's).
disk: lha
	@echo "  DISK  $(DISK)"
	@rm -f $(DISK)
	@xdftool $(DISK) format "$(DISK_TITLE)" ffs
	@xdftool $(DISK) write $(LHA_EN_NAME) amigamce.lha
	@xdftool $(DISK) write $(LHA_DIR)/readme.txt readme.txt
	@xdftool $(DISK) write docs/readme.info readme.txt.info
	@xdftool $(DISK) write docs/Disk.info Disk.info
	@xdftool $(DISK) info

clean:
	rm -rf build/*.o build/lha build/lha-en build/catalogs build/vt build/test_ags build/test_songs build/songs build/ram $(VERSION_STAMP) $(TARGET) \
		amigamce-*.lha amigamce-*.adf
