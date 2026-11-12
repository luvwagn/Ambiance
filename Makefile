# AmigaMCE Makefile for GCC (m68k-amigaos), modelled on xSysInfo
# Copyright (c) 2026 Brian King
#
#   make            build AmigaMCE and the LHA archive (with its installer)
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
# a sprintf that may write past its buffer (numbers at their widest) is
# an error, not a warning that scrolls past
CFLAGS  = -Os $(CPU) -noixemul -Wall -Wextra -Wno-pointer-sign \
          -Werror=format-overflow=2 -Werror=format-truncation=2 -Isrc/sqlib/include \
          -DAMCE_VERSION="\"$(FULL_VERSION)\"" -DAMCE_DATE="\"$(ADATE)\""
LDFLAGS = -noixemul $(CPU)
LIBS    = -lamiga

SRCS = src/main.c src/gfx.c src/view.c src/thumb.c src/ilbm.c src/pics.c src/loader.c src/dirscan.c src/index.c src/smooth.c src/textbox.c src/mods.c src/modplay.c src/ptcore.c src/ptmix.c src/unpack.c src/eagle.c src/mixplay.c src/ags.c src/caps.c src/art.c src/util.c \
       src/log.c src/debug.c src/net.c src/radio.c src/radiocsv.c src/stream.c src/plex.c src/plexxml.c src/songinfo.c src/cjk.c src/lang.c \
       src/vis.c src/vistab.c src/cpucheck.c src/anim.c src/animdec.c src/animdraw.c src/beets.c
OBJS = $(patsubst src/%.c,build/%.o,$(SRCS)) build/eaglehost.o build/hfft.o build/animdraw68k.o
VASM ?= vasmm68k_mot

TARGET     = AmigaMCE
LHA_DIR    = build/lha/AmigaMCE
LHA_NAME   = amigamce-$(FULL_VERSION).lha
LHA_OPTS  ?=

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
# Japanese, Chinese and Korean glyphs (tools/mkcjk.py: Galmuri, Ark Pixel),
# and Greek (tools/mkgreek.py: Galmuri11)
CJK_FILES  = assets/Fonts/amce-cjk/12 assets/Fonts/amce-cjk/15 assets/Fonts/amce-cjk/greek
ICONS      = docs/AmigaMCE.info docs/drawer.info docs/readme.info
LICENCES   = assets/fonts-src/OFL-PixelifySans.txt assets/fonts-src/OFL-Rubik.txt \
             assets/fonts-src/OFL-Galmuri.txt assets/fonts-src/OFL-ArkPixel.txt \
             assets/fonts-src/OFL-Geologica.txt CREDITS.txt LICENSE

.PHONY: all clean test test-replayer test-animdraw test-beets anim-bench check-formats check-syscalls art fonts smooth icons preview lha catalogs FORCE_VERSION

all: $(TARGET) lha

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
build/animdraw68k.o: src/animdraw68k.s
	@mkdir -p build
	@echo "  ASM   $@"
	@$(VASM) -Fhunk -m68020 -quiet -o $@ $<

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

# an animation's frames, decoded and drawn each frame: for speed, not size
# (-O2 decodes Tron2's frames in 55% of the cycles -Os does)
build/animdec.o build/animdraw.o: CFLAGS += -O2
build/animdraw.o: CFLAGS += -DAD_ASM

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

# rtgbench: AmigaMCE's drawing timed on RTG screens, 8-bit against 16-bit
build/rtgbench: tools/rtgbench/rtgbench.c src/animdraw.c src/animdraw.h build/animdraw68k.o
	@mkdir -p build
	@echo "  CC    $<"
	@$(CC) -O2 -m68020 -noixemul -Wall -Wno-pointer-sign -DAD_ASM -s -o $@ $< src/animdraw.c build/animdraw68k.o -lamiga
rtgbench: build/rtgbench

test: check-formats check-syscalls build/test_ags build/test_songs build/test_fft test-replayer test-radio test-plex test-animdraw test-beets
	@# git keeps no empty folders: the fixtures' own (empty AGS menus) made again
	@while IFS= read -r d; do mkdir -p "$$d"; done < tests/fixtures/empty-dirs.txt
	@rm -rf build/vt && mkdir -p build/vt
	@cd build && vamos -V AGS:$(abspath tests/fixtures/AGS2) -V Images:$(abspath tests/fixtures/Images) -V Videos:$(abspath tests/fixtures/Videos) -V T:$(abspath build/vt) test_ags
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

# 5-8 channels mixed onto Paula's four (ptmix.c), against a plain model of
# eight Paulas.
build/ptmix_host: tests/ptmix_host.c src/ptmix.c src/ptmix.h src/ptcore.c src/ptcore.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/ptmix_host.c src/ptmix.c src/ptcore.c

# radio-browser.info's CSV, UTF-8 to Latin-1, URL encoding (radiocsv.c).
build/radio_host: tests/radio_host.c src/radiocsv.c src/radiocsv.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/radio_host.c src/radiocsv.c

# a Plex server's XML (plexxml.c).
build/animdraw_host: tests/animdraw_host.c src/animdraw.c src/animdraw.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/animdraw_host.c src/animdraw.c

build/animdraw_68k: tests/animdraw_host.c src/animdraw.c src/animdraw.h build/animdraw68k.o
	@$(CC) -O2 $(CPU) -noixemul -Wall -DAD_ASM -o $@ tests/animdraw_host.c src/animdraw.c build/animdraw68k.o

# the drawing checked on the host (C), and on a 68020 with its assembler
test-animdraw: build/animdraw_host build/animdraw_68k
	@build/animdraw_host > build/test-animdraw.txt && echo "  CHECK animation drawing ok" || (cat build/test-animdraw.txt; exit 1)
	@cd build && vamos -C 68020 -m 8192 -s 256 animdraw_68k > test-animdraw68k.txt && grep -q " 0 failed" test-animdraw68k.txt \
		&& echo "  CHECK animation drawing (68020 assembler) ok" || (cat test-animdraw68k.txt; exit 1)

# an ANIM's decoding and drawing timed on a 68020 (vamos counts its cycles):
#   make anim-bench ANIM=file.anim
anim-bench:
	@mkdir -p build/bench
	@$(CC) -O2 $(CPU) -noixemul -w -o build/bench/anim_bench_c tests/anim_bench.c src/animdec.c src/animdraw.c
	@$(CC) -O2 $(CPU) -noixemul -w -DAD_ASM -o build/bench/anim_bench_asm tests/anim_bench.c src/animdec.c src/animdraw.c build/animdraw68k.o
	@for b in c asm; do for m in "0 100" "1 100" "1 200"; do printf "%-3s mode/scale %s: " $$b "$$m"; \
		vamos -C 68020 -m 8192 -l machine:info build/bench/anim_bench_$$b $(ANIM) $$m 2>&1 | grep "runtime end" | grep -o "total_cycles=[0-9]*"; done; done

# a beets library (beets.c) read on a 68020 through amcesql.library, from
# a small library made for it (tests/mkbeets.py)
build/beets_68k: tests/beets_68k.c src/beets.c src/util.c src/radiocsv.c src/amce.h $(SQLIB)
	@mkdir -p build/beets/prog/Libs build/beets/prog/Cache
	@$(CC) -O2 $(CPU) -noixemul -Wall -Isrc/sqlib/include '-DIDX_FILE="prog:Cache/beets.idx"' \
		-o $@ tests/beets_68k.c src/beets.c src/util.c src/radiocsv.c
	@cp $(SQLIB) build/beets/prog/Libs/

test-beets: build/beets_68k
	@$(PYTHON) -I tests/mkbeets.py build/beets
	@rm -f build/beets/prog/Cache/beets.idx
	@cp build/beets_68k build/beets/prog/
	@cd build/beets && vamos -C 68020 -m 8192 -s 256 -V music:$(abspath build/beets/music) \
		-V prog:$(abspath build/beets/prog) prog:beets_68k > ../test-beets.txt 2>&1 \
		&& grep -q "beets tests passed" ../test-beets.txt \
		&& echo "  CHECK beets library ok" || (cat ../test-beets.txt; exit 1)

build/plex_host: tests/plex_host.c src/plexxml.c src/plexxml.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/plex_host.c src/plexxml.c

test-plex: build/plex_host
	@build/plex_host > build/test-plex.txt && echo "  CHECK plex XML ok" || (cat build/test-plex.txt; exit 1)

test-radio: build/radio_host
	@build/radio_host > build/test-radio.txt && echo "  CHECK radio CSV ok" || (cat build/test-radio.txt; exit 1)

test-replayer: build/pt_host build/ptmix_host
	@$(PYTHON) -I tests/mkmod.py build/test.mod
	@build/pt_host build/test.mod - > build/test-replayer.txt
	@grep -q "length 0:19.440" build/test-replayer.txt && grep -q "channels heard 1111" build/test-replayer.txt \
		&& echo "  CHECK replayer ok" || (cat build/test-replayer.txt; echo "replayer test failed"; exit 1)
	@for k in 8CHN FLT8; do \
		$(PYTHON) -I tests/mkmod.py build/test-$$k.mod $$k && \
		build/pt_host build/test-$$k.mod - > build/test-replayer-$$k.txt && \
		grep -q "length 0:19.440" build/test-replayer-$$k.txt && \
		grep -q "channels heard 11111111" build/test-replayer-$$k.txt || \
		{ cat build/test-replayer-$$k.txt; echo "replayer test ($$k) failed"; exit 1; }; \
	done; echo "  CHECK replayer, 8 channels (8CHN, FLT8) ok"
	@for k in 8CHN FLT8; do \
		build/ptmix_host build/test-$$k.mod > build/test-mix-$$k.txt || \
		{ cat build/test-mix-$$k.txt; echo "mixer test ($$k) failed"; exit 1; }; \
	done; echo "  CHECK mixer, 8 channels onto 4 ok"

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
	$(PYTHON) tools/mkgreek.py assets/fonts-src/Galmuri11-greek.bdf assets/Fonts/amce-cjk/greek

smooth:
	$(PYTHON) tools/mksmooth.py assets/fonts-src/Rubik-VF.ttf assets/Fonts/amce-smooth 480 600 768 \
		--greek assets/fonts-src/Geologica-VF.ttf

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
# chosen (the Workbench's ticked).
# Aminet's own archives, as they are there: modplug.library (public domain),
# mpega.library and xfdmaster.library (free to pass on, not for profit,
# all files together and unchanged)
# amcesql.library: SQLite (Aminet's 68k libsqlite3.a) for beets libraries,
# loaded by AmigaMCE only when one is set up (PROGDIR:Libs)
SQLIB     = build/sqlib/amcesql.library
SQLIB_CC  = $(CC) -O2 -m68020 -noixemul -nostartfiles -fomit-frame-pointer -Wall -Wno-array-bounds
build/sqlib/libinit.o: src/sqlib/libinit.c src/sqlib/libinit.h
	@mkdir -p build/sqlib
	@echo "  CC    $<"
	@$(SQLIB_CC) -Isrc/sqlib -c -o $@ $<
$(SQLIB): build/sqlib/libinit.o src/sqlib/amcesql.c extras/sqlite/libsqlite3.a extras/sqlite/sqlite3.h
	@echo "  LD    $@"
	@$(SQLIB_CC) -Isrc/sqlib -Iextras/sqlite -o $@.dbg build/sqlib/libinit.o src/sqlib/amcesql.c \
		extras/sqlite/libsqlite3.a -lm -lamiga
	@$(STRIP) -o $@ $@.dbg

EXTRAS = extras/aminet/modplug-68k.lha extras/aminet/mpega_library.lha extras/aminet/xfdmaster.lha
lha: $(TARGET) AmigaMCE.readme $(ICONS) docs/install.info docs/Install-AmigaMCE $(FONT_FILES) \
     $(SMOOTH_FILES) $(CJK_FILES) $(LICENCES) build/catalogs/.built $(EXTRAS) $(SQLIB)
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
	@mkdir -p $(LHA_DIR)/Libs
	@cp $(SQLIB) $(LHA_DIR)/Libs/
	@cp extras/sqlite/LICENSE.md $(LHA_DIR)/Licences/SQLite.txt
	@awk -v version="$(FULL_VERSION)" '{ gsub(/\[VERSION\]/, version); print }' \
		AmigaMCE.readme > $(LHA_DIR)/readme.txt
	@cp docs/readme.info $(LHA_DIR)/readme.txt.info
	@cp docs/drawer.info build/lha/AmigaMCE.info
	@mkdir -p $(LHA_DIR)/Catalogs
	@cp -R build/catalogs/* $(LHA_DIR)/Catalogs/
	@cp docs/Install-AmigaMCE build/lha/Install-AmigaMCE
	@# Aminet's libraries for more of the music, each archive's files whole
	@# and unchanged (their terms: kept together, not changed), for the
	@# installer to offer where they are missing or older (never on AGS)
	@mkdir -p build/lha/Extras
	@for a in $(EXTRAS); do (cd build/lha/Extras && lha xq ../../../$$a); done
	@cp docs/install.info build/lha/Install-AmigaMCE.info
	@rm -f $(LHA_NAME)
	@cd build/lha && lha aqo5 $(LHA_OPTS) ../../$(LHA_NAME) AmigaMCE AmigaMCE.info \
		Install-AmigaMCE Install-AmigaMCE.info Extras
	@echo "Created $(LHA_NAME)"

clean:
	rm -rf build/*.o build/lha build/sqlib build/beets build/beets_68k build/catalogs build/vt build/test_ags build/test_songs build/songs build/ram $(VERSION_STAMP) $(TARGET) \
		amigamce-*.lha
