# AmigaMCE Makefile for GCC (m68k-amigaos), modelled on xSysInfo
# Copyright (c) 2026 Brian King
#
#   make            build AmigaMCE, the LHA archive and the ADF
#   make test       run the AGS parser tests under vamos
#   make art        regenerate src/art.c (logo, icons, palette)
#   make fonts      regenerate the amce-pixel bitmap fonts
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

CFLAGS  = -Os -m68000 -mtune=68020-60 -noixemul -Wall -Wextra -Wno-pointer-sign \
          -DAMCE_VERSION="\"$(FULL_VERSION)\"" -DAMCE_DATE="\"$(ADATE)\""
LDFLAGS = -noixemul
LIBS    = -lamiga

SRCS = src/main.c src/gfx.c src/textbox.c src/ags.c src/caps.c src/art.c src/util.c src/log.c
OBJS = $(patsubst src/%.c,build/%.o,$(SRCS))

TARGET     = AmigaMCE
LHA_DIR    = build/lha/AmigaMCE
LHA_NAME   = amigamce-$(FULL_VERSION).lha
LHA_OPTS  ?=
DISK       = amigamce-$(FULL_VERSION).adf
DISK_TITLE = AmigaMCE

FONT_FILES = assets/Fonts/amce-pixel.font assets/Fonts/amce-pixel/12 \
             assets/Fonts/amce-pixel-hi.font assets/Fonts/amce-pixel-hi/12
ICONS      = docs/AmigaMCE.info docs/drawer.info docs/Disk.info docs/readme.info
LICENCES   = assets/fonts-src/OFL-PixelifySans.txt assets/fonts-src/OFL-Rubik.txt CREDITS.txt

.PHONY: all clean test check-formats art fonts icons preview lha disk FORCE_VERSION

all: $(TARGET) lha disk

# Compiler flags are not file dependencies: rebuild only when the
# embedded version or date actually changes.
$(VERSION_STAMP): FORCE_VERSION
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(FULL_VERSION)' '$(ADATE)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

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
build/test_ags: tests/test_ags.c src/ags.c src/util.c src/amce.h
	@echo "  CC    $@"
	@$(CC) -O1 -m68000 -noixemul -Wall -Wno-pointer-sign -o $@ tests/test_ags.c src/ags.c src/util.c

# amiga.lib's sprintf is exec's RawDoFmt, where %d/%u/%x/%c read 16 bits.
# Every numeric conversion must carry the l modifier with a long argument.
check-formats:
	@if grep -nE 'printf *\(.*%[-+ 0#]*[0-9]*(\.[0-9]+)?[diuxXc]' src/*.c; then \
		echo "error: use %l with a long argument (RawDoFmt reads %d/%u/%x/%c as 16-bit)"; exit 1; fi
	@echo "  CHECK formats ok"

test: check-formats build/test_ags
	@rm -rf build/vt && mkdir -p build/vt
	@cd build && vamos -V AGS:$(abspath tests/fixtures/AGS2) -V T:$(abspath build/vt) test_ags

# ------------------------------------------------------------------ assets
art:
	$(PYTHON) tools/mkart.py assets/fonts-src/Rubik-Bold.ttf src/art.c

fonts:
	@mkdir -p build/fontgen
	$(PYTHON) tools/extract.py assets/fonts-src/PixelifySans-Regular.ttf build/fontgen/glyphs.json
	$(PYTHON) tools/amigafont.py build/fontgen/glyphs.json build/fontgen
	cp -R build/fontgen/FONTS/. assets/Fonts/

icons:
	$(PYTHON) tools/mkicon.py docs

preview: src/art.c $(FONT_FILES)
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-hires.png hires pal
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-lores.png lores pal
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-ntsc.png lores ntsc
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-about-hires.png hires pal about 0
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-laced.png laced pal games
	$(PYTHON) tools/preview_screen.py src/art.c assets/Fonts build/preview-rtg.png rtg pal games

# ------------------------------------------------------------------ release
lha: $(TARGET) AmigaMCE.readme $(ICONS) $(FONT_FILES) $(LICENCES)
	@echo "  LHA   $(LHA_NAME)"
	@rm -rf build/lha
	@mkdir -p $(LHA_DIR)/Fonts/amce-pixel $(LHA_DIR)/Fonts/amce-pixel-hi $(LHA_DIR)/Licences
	@cp $(TARGET) $(LHA_DIR)/
	@cp docs/AmigaMCE.info $(LHA_DIR)/
	@cp assets/Fonts/amce-pixel.font assets/Fonts/amce-pixel-hi.font $(LHA_DIR)/Fonts/
	@cp assets/Fonts/amce-pixel/12 $(LHA_DIR)/Fonts/amce-pixel/
	@cp assets/Fonts/amce-pixel-hi/12 $(LHA_DIR)/Fonts/amce-pixel-hi/
	@cp $(LICENCES) $(LHA_DIR)/Licences/
	@awk -v version="$(FULL_VERSION)" '{ gsub(/\[VERSION\]/, version); print }' \
		AmigaMCE.readme > $(LHA_DIR)/readme.txt
	@cp docs/readme.info $(LHA_DIR)/readme.txt.info
	@cp docs/drawer.info build/lha/AmigaMCE.info
	@rm -f $(LHA_NAME)
	@cd build/lha && lha aqo5 $(LHA_OPTS) ../../$(LHA_NAME) AmigaMCE AmigaMCE.info
	@echo "Created $(LHA_NAME)"

# A bootable floppy: works from DF0: on a booted system, or boots straight
# into AmigaMCE (no AGS menu then, so it shows the media modules only).
disk: lha
	@echo "  DISK  $(DISK)"
	@rm -f $(DISK)
	@xdftool $(DISK) format "$(DISK_TITLE)"
	@xdftool $(DISK) write $(LHA_DIR)/AmigaMCE AmigaMCE
	@xdftool $(DISK) write docs/AmigaMCE.info AmigaMCE.info
	@xdftool $(DISK) write docs/Disk.info Disk.info
	@xdftool $(DISK) write $(LHA_DIR)/readme.txt readme.txt
	@xdftool $(DISK) write docs/readme.info readme.txt.info
	@xdftool $(DISK) makedir Fonts
	@xdftool $(DISK) write $(LHA_DIR)/Fonts/amce-pixel.font Fonts/amce-pixel.font
	@xdftool $(DISK) write $(LHA_DIR)/Fonts/amce-pixel-hi.font Fonts/amce-pixel-hi.font
	@xdftool $(DISK) makedir Fonts/amce-pixel
	@xdftool $(DISK) write $(LHA_DIR)/Fonts/amce-pixel/12 Fonts/amce-pixel/12
	@xdftool $(DISK) makedir Fonts/amce-pixel-hi
	@xdftool $(DISK) write $(LHA_DIR)/Fonts/amce-pixel-hi/12 Fonts/amce-pixel-hi/12
	@xdftool $(DISK) makedir Licences
	@set -e; for f in $(notdir $(LICENCES)); do \
		xdftool $(DISK) write $(LHA_DIR)/Licences/$$f Licences/$$f; done
	@xdftool $(DISK) makedir S
	@xdftool $(DISK) write Startup-Sequence S/Startup-Sequence
	@xdftool $(DISK) boot install
	@xdftool $(DISK) info

clean:
	rm -rf build/*.o build/lha build/vt build/test_ags $(VERSION_STAMP) $(TARGET) \
		amigamce-*.lha amigamce-*.adf
