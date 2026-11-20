# Ambiance Makefile for GCC (m68k-amigaos), modelled on xSysInfo
# Copyright (c) 2026 Brian King
#
#   make            build Ambiance and the LHA archive (with its installer)
#   make test       run the AGS parser tests under vamos
#   make art        regenerate src/art.c (logo, icons, palette)
#   make fonts      regenerate the bitmap fonts (amb-pixel 12/24/36, Galmuri)
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
          -Werror=format-overflow=2 -Werror=format-truncation=2 -Isrc/sqlib/include -Iextras/dvdnav/include -Iextras/ac3/include -Iextras/mpegv/include \
          -Ibuild -DAMB_VERSION="\"$(FULL_VERSION)\"" -DAMB_DATE="\"$(ADATE)\""
# The layouts' interface (src/layout.h, src/lkcore.h) as a number: a
# layout library built against other versions of them is never used
# (lhost.c, src/layouts/layoutlib.c)
LAYOUT_ABI := $(shell cat src/layout.h src/lkcore.h | cksum | cut -d' ' -f1)
CFLAGS  += -DLAYOUT_ABI=$(LAYOUT_ABI)UL
LDFLAGS = -noixemul $(CPU)
LIBS    = -lamiga -lm

# A library's version for its Resident (LIB_RT_VERSION, src/sqlib/libinit.c)
# from its source's own LibVersion, so the two cannot differ: ramlib turns a
# library away whose Resident is older than the version asked for
# (tools/check_libs.py checks every library built).
libver = $(shell sed -n 's/^const UWORD LibVersion = \([0-9]*\);.*/\1/p' $(1))

SRCS = src/main.c src/mpgthumb.c src/details.c src/lvhost.c src/gfx.c src/view.c src/thumb.c src/ilbm.c src/pics.c src/loader.c src/dirscan.c src/index.c src/smooth.c src/textbox.c src/mods.c src/modplay.c src/ptcore.c src/ptmix.c src/medtime.c src/tls.c src/unpack.c src/eagle.c src/mixplay.c src/ags.c src/caps.c src/art.c src/util.c \
       src/log.c src/debug.c src/net.c src/radio.c src/radiocsv.c src/stream.c src/plex.c src/plexxml.c src/songinfo.c src/nfo.c src/cjk.c src/lang.c \
       src/vis.c src/visflow.c src/amphost.c src/vistab.c src/cpucheck.c src/anim.c src/animdec.c src/animdraw.c src/beets.c \
       src/jpgthumb.c src/iconify.c src/zz9k.c src/vprobe.c src/video.c src/tspack.c \
       src/dvdplay.c src/dvddisc.c src/dvdiso.c src/glyph.c src/ownlib.c src/spu.c src/igame.c src/igamecsv.c src/unarc.c src/whdinst.c src/lkcore.c src/lkit.c src/lhost.c src/copper.c
OBJS = $(patsubst src/%.c,build/%.o,$(SRCS)) build/eaglehost.o build/hfft.o build/animdraw68k.o build/c2p68k.o build/jdec.o
# AmiSSL 6 as well as 5: src/tls.c compiled again against AmiSSL 6.1's SDK
# headers (extras/amissl6, Apache 2.0), its functions tls6_; tls.o opens
# AmiSSL 6 where it is installed, else 5 (nothing of AmiSSL linked in)
AMISSL6_INC = extras/amissl6/include
ifneq ($(wildcard $(AMISSL6_INC)/libraries/amisslmaster.h),)
OBJS += build/tls6.o
build/tls.o: CFLAGS += -DTLS_HAVE_V6
endif
VASM ?= vasmm68k_mot

TARGET     = Ambiance
LHA_DIR    = build/lha/Ambiance
LHA_NAME   = ambiance-$(FULL_VERSION).lha
LHA_OPTS  ?=

FONT_FILES = assets/Fonts/amb-pixel.font assets/Fonts/amb-pixel/12 \
             assets/Fonts/amb-pixel/24 assets/Fonts/amb-pixel/36
# The laced screens' Galmuri fonts (tools/mkbdf.py): name and line height
GALMURI    = amb-galmuri11:16 amb-galmuri11b:16 amb-galmuri14:21 amb-galmuri14b:21
FONT_FILES += $(foreach f,$(GALMURI),assets/Fonts/$(word 1,$(subst :, ,$(f))).font \
              assets/Fonts/$(subst :,/,$(f)))
# Smooth style, one file per RTG screen height (tools/mksmooth.py)
SMOOTH_FILES = assets/Fonts/amb-smooth/384 assets/Fonts/amb-smooth/480 assets/Fonts/amb-smooth/600 \
               assets/Fonts/amb-smooth/720 assets/Fonts/amb-smooth/768 assets/Fonts/amb-smooth/1024 \
               assets/Fonts/amb-smooth/1080 assets/Fonts/amb-smooth/1200 assets/Fonts/amb-smooth/logo1 \
               assets/Fonts/amb-smooth/logo2 assets/Fonts/amb-smooth/logo3
# Japanese, Chinese and Korean glyphs (tools/mkcjk.py: Galmuri, Ark Pixel),
# and Greek (tools/mkgreek.py: Galmuri11)
CJK_FILES  = assets/Fonts/amb-cjk/12 assets/Fonts/amb-cjk/15 assets/Fonts/amb-cjk/greek
ICONS      = docs/Ambiance.info docs/drawer.info docs/readme.info docs/appicon.info
LICENCES   = assets/fonts-src/OFL-PixelifySans.txt assets/fonts-src/OFL-Rubik.txt \
             assets/fonts-src/OFL-Galmuri.txt assets/fonts-src/OFL-ArkPixel.txt \
             assets/fonts-src/OFL-Geologica.txt CREDITS.txt LICENSE

.PHONY: test-igame test-showcase test-horizon all clean leakcheck check-libs test-spu test test-replayer test-animdraw test-beets dvdlib test-dvd test-dvd-host test-dvdiso ac3lib test-ac3 mpgalib test-mpga mpegvlib test-mpegv anim-bench mpeg-prof check-formats check-syscalls art fonts smooth icons preview lha catalogs FORCE_VERSION

all: $(TARGET) layouts lha

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
build/c2p68k.o: src/c2p68k.s
	@mkdir -p build
	@echo "  ASM   $@"
	@$(VASM) -Fhunk -m68020 -quiet -o $@ $<

src/c2p68k.s: tools/c2pgen/c2pgen.py
	@$(PYTHON) tools/c2pgen/c2pgen.py $@

build/c2p_test: tests/c2p_test.c build/c2p68k.o
	@$(CC) -O2 $(CPU) -noixemul -Wall -o $@ tests/c2p_test.c build/c2p68k.o

# the assembler chunky to planar (AGA video) against a plain one, on a 68020
test-c2p: build/c2p_test
	@cd build && vamos -C 68020 -m 8192 c2p_test > test-c2p.txt && grep -q " 0 failed" test-c2p.txt \
		&& echo "  CHECK chunky to planar (68020 assembler) ok" || (cat test-c2p.txt; exit 1)

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

# The ZZ9000 SDK's headers, for zz9k.c only (MIT: third_party/zz9k-sdk.txt):
# fetched from BlitterStudio's zz9000-firmware at the commit pinned here,
# only sdk/'s three header folders and the licence texts (a sparse, shallow
# fetch, about 600 KB), into build/zz9k-sdk, kept by make clean. To move to
# a newer SDK, change ZZ9K_SDK_COMMIT (the next build fetches it).
ZZ9K_SDK_REPO   = https://github.com/BlitterStudio/zz9000-firmware.git
ZZ9K_SDK_COMMIT = 6de721a01349756211133aaffc6679b1d1596c51
ZZ9K_SDK        = build/zz9k-sdk
ZZ9K_SDK_STAMP  = $(ZZ9K_SDK)/.fetched-$(ZZ9K_SDK_COMMIT)
ZZ9K_CFLAGS = -I$(ZZ9K_SDK)/sdk/amiga/include -I$(ZZ9K_SDK)/sdk/include -I$(ZZ9K_SDK)/sdk/host/include \
              -Wno-unused-function -Wno-sign-compare
build/zz9k.o: CFLAGS += $(ZZ9K_CFLAGS)
build/zz9k.o: $(ZZ9K_SDK_STAMP)

$(ZZ9K_SDK_STAMP):
	@echo "  FETCH ZZ9000 SDK headers ($(ZZ9K_SDK_COMMIT))"
	@rm -rf $(ZZ9K_SDK) && mkdir -p $(ZZ9K_SDK)
	@cd $(ZZ9K_SDK) && git init -q && git remote add origin $(ZZ9K_SDK_REPO) \
		&& git sparse-checkout set --no-cone /sdk/amiga/include/ /sdk/include/ /sdk/host/include/ \
		     /sdk/LICENSES/MIT.txt /LICENSING.md \
		&& git fetch -q --depth 1 --filter=blob:none origin $(ZZ9K_SDK_COMMIT) \
		&& git checkout -q FETCH_HEAD
	@test -f $(ZZ9K_SDK)/sdk/amiga/include/proto/zz9k.h || { echo "ZZ9000 SDK headers not fetched"; exit 1; }
	@touch $@

# four frames of AC-3 (5.1, 448 kbit/s: a DVD's kind) that stream.c times
# each AC-3 library on, to use the faster here (an FPU's build is not
# always: an emulator without its JIT's FPU)
build/ac3probe.h: extras/ac3/tests/fixtures/5.1-448k.ac3
	@mkdir -p build
	@echo "  GEN   $@"
	@$(PYTHON) -c "import sys; d=open(sys.argv[1],'rb').read()[:4*1792]; \
		print('static const UBYTE ac3probe[%d] = {' % len(d)); \
		print(',\n'.join(','.join('0x%02x' % b for b in d[i:i+16]) for i in range(0, len(d), 16))); \
		print('};')" $< > $@
build/stream.o: build/ac3probe.h

build/%.o: src/%.c src/amb.h $(VERSION_STAMP)
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) -c -o $@ $<

build/lkit.o: src/lkit.h src/lkcore.h
build/view.o build/copper.o build/anim.o: src/copper.h
build/anim.o build/mpgthumb.o build/stream.o build/tspack.o: src/tspack.h
build/anim.o build/mpgthumb.o build/video.o build/vprobe.o: src/vprobe.h
# (LAYOUT_ABI is the checksum of these: the objects that check or use it)
build/lhost.o build/main.o: src/layout.h src/lkcore.h
build/main.o build/details.o build/lvhost.o: src/app.h src/details.h src/lvhost.h src/layout.h src/lkcore.h
build/lkcore.o: src/lkcore.h
build/lhost.o: src/lhost.h src/layout.h src/lkcore.h

# The layouts: PROGDIR:Layouts/<name>.layout, each an Exec library with
# libnix's libinit (as ambsql.library's), src/layouts/layoutlib.c (its
# GetLayout) and the layout's own source. They draw only through the
# program's table: no C library linked in.
LAYOUT_NAMES = showcase horizon
LAYOUTS      = $(patsubst %,build/layouts/%.layout,$(LAYOUT_NAMES))
LAYOUT_CC    = $(CC) -O2 $(CPU) -noixemul -nostartfiles -fomit-frame-pointer -Wall -Wextra \
               -Wno-pointer-sign -Isrc -Isrc/sqlib
LAYOUT_HDR   = src/layout.h src/lkcore.h
build/layouts/%/libinit.o: src/sqlib/libinit.c src/sqlib/libinit.h src/layouts/%.c Makefile
	@mkdir -p $(dir $@)
	@$(LAYOUT_CC) -DLIB_RT_VERSION=$(call libver,src/layouts/$*.c) -c -o $@ $<
build/layouts/%/layoutlib.o: src/layouts/layoutlib.c $(LAYOUT_HDR) Makefile
	@mkdir -p $(dir $@)
	@$(LAYOUT_CC) -DLAYOUT_ABI=$(LAYOUT_ABI)UL -c -o $@ $<
build/layouts/%/layout.o: src/layouts/%.c $(LAYOUT_HDR) Makefile
	@mkdir -p $(dir $@)
	@echo "  CC    $< (layout)"
	@$(LAYOUT_CC) -DLAYOUT_ABI=$(LAYOUT_ABI)UL -c -o $@ $<
build/layouts/%.layout: build/layouts/%/libinit.o build/layouts/%/layoutlib.o build/layouts/%/layout.o
	@echo "  LD    $@"
	@$(LAYOUT_CC) -o $@.dbg $^ -lamiga
	@$(STRIP) -o $@ $@.dbg
.PRECIOUS: build/layouts/%/libinit.o build/layouts/%/layoutlib.o build/layouts/%/layout.o
layouts: $(LAYOUTS)

# their finding and opening (lhost.c) on a 68020 under vamos, with
# showcase.layout and one built against another layout.h (named .library
# there: vamos opens no other names)
LT_DIR = build/layouttest
$(LT_DIR)/Layouts/wrongabi.library: src/sqlib/libinit.c src/layouts/layoutlib.c tests/layout_wrong.c $(LAYOUT_HDR)
	@mkdir -p $(LT_DIR)/Layouts $(LT_DIR)/obj
	@$(LAYOUT_CC) -DLIB_RT_VERSION=1 -c -o $(LT_DIR)/obj/libinit.o src/sqlib/libinit.c
	@$(LAYOUT_CC) -DLAYOUT_ABI=12345UL -c -o $(LT_DIR)/obj/layoutlib.o src/layouts/layoutlib.c
	@$(LAYOUT_CC) -DLAYOUT_ABI=12345UL -c -o $(LT_DIR)/obj/wrong.o tests/layout_wrong.c
	@$(LAYOUT_CC) -o $@ $(LT_DIR)/obj/libinit.o $(LT_DIR)/obj/layoutlib.o $(LT_DIR)/obj/wrong.o -lamiga
$(LT_DIR)/test_layout: tests/test_layout.c src/lhost.c src/lhost.h src/util.c $(LAYOUT_HDR) $(LAYOUTS) \
                       $(LT_DIR)/Layouts/wrongabi.library
	@mkdir -p $(LT_DIR)/Layouts
	@for l in $(LAYOUT_NAMES); do cp build/layouts/$$l.layout $(LT_DIR)/Layouts/$$l.library; done
	@$(CC) -O1 $(CPU) -noixemul -Wall -Wno-pointer-sign -DAMB_NO_EXPUNGE -DLAYOUT_ABI=$(LAYOUT_ABI)UL \
		'-DLAYOUT_EXT=".library"' '-DLAYOUTS_DIR="Layouts"' \
		-o $@ tests/test_layout.c src/lhost.c src/util.c
test-layout: $(LT_DIR)/test_layout
	@cd $(LT_DIR) && vamos -C 68020 test_layout > ../test-layout.txt && grep -q "^0 failed" ../test-layout.txt \
		&& echo "  CHECK layout libraries found, opened, the wrong one refused ok" || (cat ../test-layout.txt; exit 1)

build/tls6.o: src/tls.c src/amb.h $(VERSION_STAMP)
	@echo "  CC    $@ (AmiSSL 6)"
	@$(CC) -I$(AMISSL6_INC) $(CFLAGS) -DTLS_V6 -c -o $@ $<

# an animation's frames, decoded and drawn each frame: for speed, not size
# (-O2 decodes Tron2's frames in 55% of the cycles -Os does)
build/animdec.o build/animdraw.o: CFLAGS += -O2
build/animdraw.o: CFLAGS += -DAD_ASM

$(TARGET): $(OBJS)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@cp $@ build/Ambiance.dbg       # (its symbols: Debug's "unseen" offsets read against it)
	@echo "  STRIP $@"
	@$(STRIP) $@
	@wc -c < "$@" | awk '{printf "$@ successfully compiled (%s bytes)\n", $$1}'

# ------------------------------------------------------------------ tests
build/test_ags: tests/test_ags.c src/ags.c src/pics.c src/dirscan.c src/index.c src/util.c src/amb.h
	@echo "  CC    $@"
	@$(CC) -O1 -m68000 -noixemul -Wall -Wno-pointer-sign -o $@ tests/test_ags.c src/ags.c src/pics.c src/dirscan.c src/index.c src/util.c

# iGame's list (igame.c) under vamos: iGame: the fixture drawer.
IGTEST_SRC = tests/test_igame.c src/igame.c src/igamecsv.c src/ags.c src/dirscan.c src/util.c src/lang.c
build/test_igame: $(IGTEST_SRC) src/igame.h src/igamecsv.h src/amb.h
	@echo "  CC    $@"
	@$(CC) -O1 -m68000 -noixemul -Wall -Wno-pointer-sign -o $@ $(IGTEST_SRC)

# WHDLoad put in C: from an unpacked package (whdinst.c), archive names checked (unarc.c).
WHDTEST_SRC = tests/test_whdinst.c src/whdinst.c src/unarc.c src/dirscan.c src/util.c src/lang.c
build/test_whdinst: $(WHDTEST_SRC) src/whdinst.h src/unarc.h src/amb.h
	@echo "  CC    $@"
	@$(CC) -O1 -m68000 -noixemul -Wall -Wno-pointer-sign -o $@ $(WHDTEST_SRC)

build/test_songs: tests/test_songs.c src/songinfo.c src/nfo.c src/plexxml.c src/mods.c src/ags.c src/pics.c src/dirscan.c src/index.c src/util.c src/radiocsv.c src/lang.c src/amb.h
	@echo "  CC    $@"
	@$(CC) -O1 -m68000 -noixemul -Wall -Wno-pointer-sign -o $@ tests/test_songs.c src/songinfo.c src/nfo.c src/plexxml.c src/mods.c src/ags.c src/pics.c src/dirscan.c src/index.c src/util.c src/radiocsv.c src/lang.c

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
# every library and datatype built, as AmigaOS will see it: its Resident's
# version, LibVersion.LibRevision and $VER agree, and Ambiance asks for no
# newer (tools/check_libs.py)
hdrver = $(shell sed -n 's/^\#define $(2) *\([0-9]*\).*/\1/p' $(1))
check-libs: $(DVDLIB) $(AC3LIB) $(AC3FPULIB) $(MPGALIB) $(SQLIB) $(MPEGVLIB) $(LAYOUTS)
	@$(PYTHON) tools/check_libs.py \
		$(DVDLIB)=extras/dvdnav/src/dvdnavlib.c=$(call hdrver,extras/dvdnav/include/libraries/dvdnav.h,DVDNAVLIB_VERSION) \
		$(AC3LIB)=extras/ac3/src/ac3lib.c=$(call hdrver,extras/ac3/include/libraries/ac3.h,AC3VERSION) \
		$(AC3FPULIB)=extras/ac3/src/ac3lib.c=$(call hdrver,extras/ac3/include/libraries/ac3.h,AC3VERSION) \
		$(MPGALIB)=extras/mpga/src/mpga.c=$(call hdrver,extras/mpga/include/libraries/mpega.h,AMBMPEGAVERSION) \
		$(SQLIB)=src/sqlib/ambsql.c=1 \
		$(MPEGVLIB)=extras/mpegv/src/mpegv.c=$(call hdrver,extras/mpegv/include/libraries/mpegv.h,MPEGVVERSION) \
		$(foreach l,$(LAYOUT_NAMES),build/layouts/$(l).layout=src/layouts/$(l).c)

check-syscalls: $(SRCS) src/amb.h build/ac3probe.h $(ZZ9K_SDK_STAMP)
	@mkdir -p $(ASM_DIR)
	@set -e; for f in $(SRCS); do \
		$(CC) $(CFLAGS) $$( [ $$f = src/zz9k.c ] && echo '$(ZZ9K_CFLAGS)' ) \
		  -S -o $(ASM_DIR)/$$(basename $$f .c).s $$f; done
	@$(PYTHON) tools/check_syscalls.py $(patsubst src/%.c,$(ASM_DIR)/%.s,$(SRCS))
	@echo "  CHECK OS-call arguments ok"

# a DVD's subpictures, on the host (stand-in Amiga headers: tests/yuvdraw)
build/spu_host: src/spu.c src/spu.h tests/spu/spu_host.c
	@mkdir -p build
	@cc -O2 -w -Itests/yuvdraw -Isrc -o $@ tests/spu/spu_host.c src/spu.c
test-spu: build/spu_host
	@build/spu_host > /dev/null && echo "  CHECK a DVD's subpictures decoded and blended (host) ok"

# leakcheck: clang's static analyser over Ambiance's own sources, the NDK's
# acquire/release pairs (AllocVec/FreeVec, Open/Close, Lock/UnLock,
# OpenLibrary/CloseLibrary, AllocBitMap/FreeBitMap...) declared to it as
# ownership (tools/leakcheck/mkshim.py): a path that drops what it acquired
# is reported. A report, not a gate: AssignLock taking its lock (ags.c) is
# not known to it, and memory AllocVec cleared reads to it as unset.
# tls.c, net.c and zz9k.c are left out (their own inline calls do not parse).
CLANG    ?= clang
TOOLROOT  = $(abspath $(dir $(shell command -v $(CC)))..)
LC_DIR    = build/leakcheck
LC_SRCS   = $(filter-out src/tls.c src/net.c src/zz9k.c,$(SRCS))
LC_FLAGS  = --target=m68k-unknown-linux-gnu -mcpu=68020 -fpack-struct=2 -nostdinc -w --analyze \
            -Wno-error=implicit-function-declaration -Wno-error=int-conversion \
            -Wno-error=incompatible-function-pointer-types \
            -Xanalyzer -analyzer-config -Xanalyzer unix.DynamicMemoryModeling:Optimistic=true \
            -Xanalyzer -analyzer-disable-checker=deadcode \
            -D__AMIGA__ -Damigaos -D__mc68020__ \
            -include $(LC_DIR)/shim.h -I$(LC_DIR)/ov -Isrc -Ibuild -Isrc/sqlib/include -Iextras/dvdnav/include \
            -Iextras/ac3/include -Iextras/mpegv/include \
            -isystem $(TOOLROOT)/m68k-amigaos/libnix/include \
            -isystem $(wildcard $(TOOLROOT)/lib/gcc/m68k-amigaos/*/include) \
            -isystem $(TOOLROOT)/m68k-amigaos/ndk-include -isystem $(TOOLROOT)/m68k-amigaos/sys-include \
            -isystem $(TOOLROOT)/m68k-amigaos/include
leakcheck: build/ac3probe.h
	@mkdir -p $(LC_DIR)
	@$(PYTHON) tools/leakcheck/mkshim.py $(TOOLROOT)/m68k-amigaos/ndk-include $(LC_DIR)
	@echo "  CLANG --analyze ($(words $(LC_SRCS)) files)"
	@printf '%s\n' $(LC_SRCS) | xargs -P 8 -I{} sh -c \
		'$(CLANG) $(LC_FLAGS) -o /dev/null {} > $(LC_DIR)/$$(basename {}).txt 2>&1 || true'
	@cat $(LC_DIR)/*.c.txt | grep -E "warning: .*(leak|freed|free |released)" || echo "  CHECK no leak found"

# rtgbench: Ambiance's drawing timed on RTG screens, 8-bit against 16-bit
build/rtgbench: tools/rtgbench/rtgbench.c src/animdraw.c src/animdraw.h build/animdraw68k.o
	@mkdir -p build
	@echo "  CC    $<"
	@$(CC) -O2 -m68020 -noixemul -Wall -Wno-pointer-sign -DAD_ASM -s -o $@ $< src/animdraw.c build/animdraw68k.o -lamiga
rtgbench: build/rtgbench

# zzprobe: what a ZZ9000 offers this Amiga (board, libraries, datatypes,
# the card's own report) and how fast Ambiance's uses of it are
# (with jdec built in, to time the JPEG decoder alone)
build/zzprobe: tools/zzprobe/zzprobe.c tools/zzprobe/stackcall.h src/jdec/jdec.c src/jdec/jdec.h
	@mkdir -p build
	@echo "  CC    $<"
	@$(CC) -O2 -m68020-60 -noixemul -Wall -Wno-pointer-sign -s -o $@ $< src/jdec/jdec.c -lamiga
zzprobe: build/zzprobe

test: check-formats check-syscalls check-libs test-spu build/test_ags build/test_igame build/test_whdinst build/test_songs build/test_fft test-replayer test-radio test-igame test-plex test-lkit test-showcase test-horizon test-layout test-animdraw test-beets test-dvd-host test-dvdiso test-tspack test-dvd test-ac3 test-mpga test-mpegv
	@# git keeps no empty folders: the fixtures' own (empty AGS menus) made again
	@while IFS= read -r d; do mkdir -p "$$d"; done < tests/fixtures/empty-dirs.txt
	@rm -rf build/vt && mkdir -p build/vt
	@cd build && vamos -V AGS:$(abspath tests/fixtures/AGS2) -V Images:$(abspath tests/fixtures/Images) -V Videos:$(abspath tests/fixtures/Videos) -V T:$(abspath build/vt) test_ags
	@rm -rf build/vt && mkdir -p build/vt
	@cd build && vamos -V iGame:$(abspath tests/fixtures/iGame) -V T:$(abspath build/vt) test_igame > test-igame68k.txt 2>&1 \
		&& grep -q "all iGame tests passed" test-igame68k.txt \
		&& echo "  CHECK iGame's list (68000, igame.c) ok" || (grep -v WARNING test-igame68k.txt; exit 1)
	@# a package as unpacked: WHDLoad, DIC, an icon, Patcher (one already in C: kept), WHDLoad.prf
	@rm -rf build/vw && mkdir -p build/vw/C build/vw/S build/vw/DEVS build/vw/T/Ambiance-WHDLoad/WHDLoad/C \
		build/vw/T/Ambiance-WHDLoad/WHDLoad/S
	@printf WHDLD > build/vw/T/Ambiance-WHDLoad/WHDLoad/C/WHDLoad && printf DIC > build/vw/T/Ambiance-WHDLoad/WHDLoad/C/DIC \
		&& printf NEW > build/vw/T/Ambiance-WHDLoad/WHDLoad/C/Patcher && printf icon > build/vw/T/Ambiance-WHDLoad/WHDLoad/C/WHDLoad.info \
		&& printf PREF > build/vw/T/Ambiance-WHDLoad/WHDLoad/S/WHDLoad.prf && printf O > build/vw/C/Patcher
	@cd build && vamos -V VW:$(abspath build/vw) -a C:VW:C -a S:VW:S -a DEVS:VW:DEVS -a T:VW:T test_whdinst > test-whdinst.txt 2>&1 \
		&& grep -q "all WHDLoad tests passed" test-whdinst.txt \
		&& echo "  CHECK WHDLoad installed from its package (68000, whdinst.c)" || (grep -v WARNING test-whdinst.txt; exit 1)
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

# The layouts' drawing kit (lkcore.c): drawn into a picture in memory that
# keeps what a 15, 16 or 24-bit screen keeps, checked; build/lkit-*.ppm to
# look at.
build/lkit_host: tests/lkit_host.c src/lkcore.c src/lkcore.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -Wextra -o $@ tests/lkit_host.c src/lkcore.c
test-lkit: build/lkit_host
	@build/lkit_host > build/test-lkit.txt && echo "  CHECK layout drawing kit ok" || (cat build/test-lkit.txt; exit 1)

# Showcase's screens drawn on the host by the kit with the smooth fonts'
# own glyphs, from made-up models: build/showcase-<screen>-<w>x<h>.ppm to
# look at (checked only for drawing off the screen, or not all of it).
build/showcase_host: tests/showcase_host.c src/lkcore.c src/layouts/showcase.c $(LAYOUT_HDR) tests/shim/exec/types.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -Wextra -Itests/shim -DLAYOUT_ABI=1UL -o $@ tests/showcase_host.c src/lkcore.c src/layouts/showcase.c
test-showcase: build/showcase_host
	@build/showcase_host > build/test-showcase.txt && echo "  CHECK Showcase drawn" || (cat build/test-showcase.txt; exit 1)

# Horizon's, the same way: build/horizon-<screen>-<w>x<h>.ppm
build/horizon_host: tests/showcase_host.c src/lkcore.c src/layouts/horizon.c $(LAYOUT_HDR) tests/shim/exec/types.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -Wextra -Itests/shim -DLAYOUT_ABI=1UL -DHORIZON '-DLNAME="horizon"' '-DLTITLE="Horizon"' -o $@ tests/showcase_host.c src/lkcore.c src/layouts/horizon.c
test-horizon: build/horizon_host
	@build/horizon_host > build/test-horizon.txt && echo "  CHECK Horizon drawn" || (cat build/test-horizon.txt; exit 1)

# radio-browser.info's CSV, UTF-8 to Latin-1, URL encoding (radiocsv.c).
build/radio_host: tests/radio_host.c src/radiocsv.c src/radiocsv.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/radio_host.c src/radiocsv.c

# iGame's gameslist.csv, tooltypes as WHDLoad arguments, demos (igamecsv.c).
build/igame_host: tests/igame_host.c src/igamecsv.c src/igamecsv.h
	@mkdir -p build
	@$(HOSTCC) -O2 -Wall -o $@ tests/igame_host.c src/igamecsv.c

test-igame: build/igame_host
	@build/igame_host > build/test-igame.txt && echo "  CHECK iGame's list ok" || (cat build/test-igame.txt; exit 1)

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

# mpegv.library 2: MPEG-1 and MPEG-2 video decoded by the CPU (libmpeg2
# inside, made faster for a 68k), its own project (extras/mpegv, a git
# submodule: github.com/luvwagn/mpegv.library, GPL-3: its source there,
# not in the archive), opened at run time where there is no ZZ9000 (or it
# is not to be used). Built by its own Makefile; its tests (its pictures
# against libmpeg2's own C, bit for bit, on the host and on a 68020 under
# vamos) are part of make test; where a 68020's cycles go, by stage and
# function, for the first N pictures of a file:
#   make mpeg-prof MPG=file.mpg [N=60]
MPEGVLIB = extras/mpegv/build/mpegv.library
MPEGV_SRC = $(wildcard extras/mpegv/src/*.c extras/mpegv/src/*.h extras/mpegv/src/*.s \
              extras/mpegv/include/*/*.h extras/mpegv/third_party/libmpeg2/libmpeg2/*.[chs] \
              extras/mpegv/third_party/libmpeg2/include/*.h) extras/mpegv/Makefile
$(MPEGVLIB): $(MPEGV_SRC)
	@$(MAKE) --no-print-directory -C extras/mpegv build/mpegv.library
mpegvlib: $(MPEGVLIB)
test-mpegv: $(MPEGVLIB)
	@$(MAKE) --no-print-directory -C extras/mpegv test
N ?= 60
mpeg-prof:
	@$(MAKE) --no-print-directory -C extras/mpegv prof MPG=$(abspath $(MPG)) N=$(N)

# an ANIM's decoding and drawing timed on a 68020 (vamos counts its cycles):
#   make anim-bench ANIM=file.anim
anim-bench:
	@mkdir -p build/bench
	@$(CC) -O2 $(CPU) -noixemul -w -o build/bench/anim_bench_c tests/anim_bench.c src/animdec.c src/animdraw.c
	@$(CC) -O2 $(CPU) -noixemul -w -DAD_ASM -o build/bench/anim_bench_asm tests/anim_bench.c src/animdec.c src/animdraw.c build/animdraw68k.o
	@for b in c asm; do for m in "0 100" "1 100" "1 200"; do printf "%-3s mode/scale %s: " $$b "$$m"; \
		vamos -C 68020 -m 8192 -l machine:info build/bench/anim_bench_$$b $(ANIM) $$m 2>&1 | grep "runtime end" | grep -o "total_cycles=[0-9]*"; done; done

# dvdnav.library: DVD-Video navigation, libdvdread 7.1.1 and libdvdnav
# 7.0.0 behind an Amiga interface of its own: its own project (extras/dvdnav,
# a git submodule: github.com/luvwagn/dvdnav.library, GPL-3: its source
# there, not in the archive; its interface, include/ and dvdnav_lib.sfd,
# MIT). Opened at run time only, like ambsql.library. Formerly
# amcedvd.library, here (before 0.9.1.1).
DVDLIB     = extras/dvdnav/build/dvdnav.library
DVDLIB_INC = extras/dvdnav/include
DVDLIB_DEPS = $(wildcard extras/dvdnav/src/*.c extras/dvdnav/src/*.h extras/dvdnav/src/port/*.h \
               extras/dvdnav/src/port/*/*.h extras/dvdnav/include/*/*.h \
               extras/dvdnav/third_party/*/src/*.[ch] extras/dvdnav/third_party/*/src/*/*.[ch]) \
              extras/dvdnav/Makefile
$(DVDLIB): $(DVDLIB_DEPS)
	@$(MAKE) --no-print-directory -C extras/dvdnav build/dvdnav.library
dvdlib: $(DVDLIB)

# its tests (extras/dvdnav/tests/dvd_test.c) on its DVD image
# (extras/dvdnav/tests/mkdvd.sh): its own make test (on the host with its
# sources linked in, then through the library on a 68020 under vamos), and
# here once more on a 68020 with Ambiance's reader (src/dvddisc.c) as well
DVD_ISO = build/dvd/test.iso
$(DVD_ISO): extras/dvdnav/tests/fixtures/dvd/test.iso.xz
	@mkdir -p build/dvd
	@$(PYTHON) -I -c "import lzma, sys; open(sys.argv[2], 'wb').write(lzma.open(sys.argv[1]).read())" $< $@
test-dvd-host: $(DVDLIB)
	@$(MAKE) --no-print-directory -C extras/dvdnav test
# transport streams packed as Program Streams (src/tspack.c), on the host,
# against ffmpeg's own reading of streams it makes (needs ffmpeg, ffprobe)
build/tspack/tspack_host: tests/tspack_host.c src/tspack.c src/tspack.h tests/host/exec/types.h
	@mkdir -p build/tspack
	@$(HOSTCC) -O2 -Wall -Itests/host -Isrc -o $@ tests/tspack_host.c src/tspack.c
test-tspack: build/tspack/tspack_host
	@sh tests/tspack_test.sh build/tspack $(abspath build/tspack/tspack_host) $(abspath extras/mpegv/build/host/mpv_test) \
		> build/test-tspack.txt 2>&1 \
		&& grep -q "tspack tests passed" build/test-tspack.txt \
		&& echo "  CHECK transport streams packed as Program Streams, two at once (host, against ffmpeg) ok" \
		|| (cat build/test-tspack.txt; exit 1)
# a DVD image's reading past 2 GB and what one holds (src/dvdiso.c), on the
# host: the 64-bit packet as UAE's file system reads it, the test image
build/dvd/dvdiso_host: tests/dvdiso_host.c src/dvdiso.c src/dvdiso.h tests/host/exec/types.h
	@mkdir -p build/dvd
	@$(HOSTCC) -O2 -Wall -Itests/host -Isrc -o $@ tests/dvdiso_host.c src/dvdiso.c
test-dvdiso: build/dvd/dvdiso_host $(DVD_ISO)
	@build/dvd/dvdiso_host $(DVD_ISO) > build/test-dvdiso.txt 2>&1 \
		&& grep -q "dvdiso tests passed" build/test-dvdiso.txt \
		&& echo "  CHECK DVD images: 64-bit positions, what one holds (host) ok" || (cat build/test-dvdiso.txt; exit 1)
build/dvd/dvd_68k: extras/dvdnav/tests/dvd_test.c src/dvddisc.c src/dvddisc.h src/dvdiso.c src/dvdiso.h \
                   $(DVDLIB_INC)/libraries/dvdnav.h $(DVDLIB_INC)/inline/dvdnav.h $(DVDLIB)
	@mkdir -p build/dvd/prog/Libs
	@echo "  CC    $@"
	@$(CC) -O2 $(CPU) -noixemul -Wall -Wno-pointer-sign -I$(DVDLIB_INC) -Isrc -DDVD_TEST_DVDDISC -o $@ \
		extras/dvdnav/tests/dvd_test.c src/dvddisc.c src/dvdiso.c -lamiga
	@$(CC) -Os $(CPU) -noixemul -I$(DVDLIB_INC) -S -o build/dvd/dvddisc.s src/dvddisc.c
	@$(PYTHON) tools/check_syscalls.py build/dvd/dvddisc.s
	@cp $(DVDLIB) build/dvd/prog/Libs/
	@cp $@ build/dvd/prog/
# Ambiance's side (src/dvdplay.c): the film chosen and read as the player reads it
build/dvd/dvdplay_68k: tests/dvdplay_68k.c src/dvdplay.c src/dvdplay.h src/dvddisc.c src/dvddisc.h src/spu.h \
                       src/dvdiso.c src/dvdiso.h src/util.c src/ownlib.c src/amb.h $(DVDLIB)
	@mkdir -p build/dvd/prog
	@echo "  CC    $@"
	@$(CC) -O2 $(CPU) -noixemul -Wall -Wno-pointer-sign -I$(DVDLIB_INC) -DAMB_NO_EXPUNGE -o $@ \
		tests/dvdplay_68k.c src/dvdplay.c src/dvddisc.c src/dvdiso.c src/util.c src/ownlib.c -lamiga
	@cp $@ build/dvd/prog/
test-dvd: build/dvd/dvd_68k build/dvd/dvdplay_68k $(DVD_ISO)
	@cp $(DVD_ISO) build/dvd/prog/test.iso
	@cd build/dvd && vamos -C 68020 -m 8192 -s 48 -V prog:$(abspath build/dvd/prog) \
		prog:dvd_68k prog:test.iso > ../test-dvd.txt 2>&1 \
		&& grep -q "dvd tests passed" ../test-dvd.txt \
		&& echo "  CHECK DVD navigation (68020, dvdnav.library, Ambiance's reader) ok" || (cat ../test-dvd.txt; exit 1)
	@cd build/dvd && vamos -C 68020 -m 8192 -s 48 -V prog:$(abspath build/dvd/prog) \
		prog:dvdplay_68k prog:test.iso > ../test-dvdplay.txt 2>&1 \
		&& grep -q "dvdplay tests passed" ../test-dvdplay.txt \
		&& echo "  CHECK a DVD's film (68020, dvdplay.c) ok" || (cat ../test-dvdplay.txt; exit 1)

# ac3.library and ac3fpu.library: A/52 (Dolby Digital, AC-3) audio decoded
# to 16-bit PCM by liba52 0.8.0, for any program to open at run time, as
# mpega.library is: their own project (extras/ac3, a git submodule:
# github.com/luvwagn/ac3.library, GPL-3: its source there, not in the
# archive). ac3.library is liba52's integer build (68020 and up, its
# multiplies one MULS.L each); ac3fpu.library its floating-point build for
# a 68040 or 68060's FPU. Built by their own Makefile; its test (the
# fixtures' CRCs on the host and through ac3.library on a 68020 under
# vamos) is part of make test.
AC3LIB     = extras/ac3/build/ac3.library
AC3FPULIB  = extras/ac3/build/ac3fpu.library
AC3_DEPS   = $(wildcard extras/ac3/src/*.c extras/ac3/src/*.h extras/ac3/src/port/*.h \
               extras/ac3/include/*/*.h extras/ac3/third_party/liba52/liba52/*.[ch] \
               extras/ac3/third_party/liba52/include/*.h) extras/ac3/Makefile
$(AC3LIB) $(AC3FPULIB) &: $(AC3_DEPS)
	@$(MAKE) --no-print-directory -C extras/ac3 build/ac3.library build/ac3fpu.library
ac3lib: $(AC3LIB) $(AC3FPULIB)
test-ac3: $(AC3LIB) $(AC3FPULIB)
	@$(MAKE) --no-print-directory -C extras/ac3 test

# ambmpega.library: MPEG audio (MP3, MP2) decoded by the CPU, libmad behind
# mpega.library's functions under a name of its own, so it never clashes
# with LIBS:mpega.library (Tavenard's, a clone, or the ZZ9000's): Settings >
# Sound chooses. Its own project (extras/mpga, a git submodule:
# github.com/luvwagn/mpga.library, GPL-3: its source there, not in the
# archive); built by its own Makefile, its test part of make test.
MPGALIB   = extras/mpga/build/ambmpega.library
MPGA_DEPS = $(wildcard extras/mpga/src/*.c extras/mpga/src/*.h extras/mpga/src/*.s \
              extras/mpga/src/port/*.h extras/mpga/include/*/*.h \
              extras/mpga/third_party/libmad/*.[ch]) extras/mpga/Makefile
$(MPGALIB): $(MPGA_DEPS)
	@$(MAKE) --no-print-directory -C extras/mpga build/ambmpega.library
mpgalib: $(MPGALIB)
test-mpga: $(MPGALIB)
	@$(MAKE) --no-print-directory -C extras/mpga test

# a beets library (beets.c) read on a 68020 through ambsql.library, from
# a small library made for it (tests/mkbeets.py)
build/beets_68k: tests/beets_68k.c src/beets.c src/util.c src/ownlib.c src/radiocsv.c src/amb.h $(SQLIB)
	@mkdir -p build/beets/prog/Libs build/beets/prog/Cache
	@$(CC) -O2 $(CPU) -noixemul -Wall -Isrc/sqlib/include '-DIDX_FILE="prog:Cache/beets.idx"' \
		-DAMB_NO_EXPUNGE -o $@ tests/beets_68k.c src/beets.c src/util.c src/ownlib.c src/radiocsv.c
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
	$(PYTHON) tools/mkbdf.py assets/fonts-src/Galmuri11-latin1.bdf assets/Fonts amb-galmuri11
	$(PYTHON) tools/mkbdf.py assets/fonts-src/Galmuri11-Bold-latin1.bdf assets/Fonts amb-galmuri11b
	$(PYTHON) tools/mkbdf.py assets/fonts-src/Galmuri14-latin1.bdf assets/Fonts amb-galmuri14
	$(PYTHON) tools/mkbdf.py assets/fonts-src/Galmuri14-latin1.bdf assets/Fonts amb-galmuri14b --bold
	$(PYTHON) tools/mkgreek.py assets/fonts-src/Galmuri11-greek.bdf assets/Fonts/amb-cjk/greek

smooth:
	$(PYTHON) tools/mksmooth.py assets/fonts-src/Rubik-VF.ttf assets/Fonts/amb-smooth 384 480 600 720 768 1024 1080 1200 \
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
# Ambiance.catalog (tools/catalog.py checks each keeps its English's %
# formats). After changing _() strings in src/: python3 tools/catalog.py
# scan src catalogs, then translate what is new in each .ct.
CT_FILES   = $(wildcard catalogs/*.ct)
build/catalogs/.built: $(CT_FILES) catalogs/Ambiance.cd tools/catalog.py
	@echo "  CAT   $(words $(CT_FILES)) languages"
	@rm -rf build/catalogs
	@$(PYTHON) tools/catalog.py build catalogs build/catalogs > /dev/null
	@touch $@

catalogs: build/catalogs/.built

# The release archive: the Ambiance drawer with every language in it, and
# an Installer script beside it that copies the drawer and the languages
# chosen (the Workbench's ticked).
# Aminet's own archives, as they are there: modplug.library (public domain),
# mpega.library and xfdmaster.library (free to pass on, not for profit,
# all files together and unchanged)
# ambsql.library: SQLite (Aminet's 68k libsqlite3.a) for beets libraries,
# loaded by Ambiance only when one is set up (PROGDIR:Libs)
SQLIB     = build/sqlib/ambsql.library
SQLIB_CC  = $(CC) -O2 -m68020 -noixemul -nostartfiles -fomit-frame-pointer -Wall -Wno-array-bounds
build/sqlib/libinit.o: src/sqlib/libinit.c src/sqlib/libinit.h src/sqlib/ambsql.c Makefile
	@mkdir -p build/sqlib
	@echo "  CC    $<"
	@$(SQLIB_CC) -Isrc/sqlib -DLIB_RT_VERSION=$(call libver,src/sqlib/ambsql.c) -c -o $@ $<
$(SQLIB): build/sqlib/libinit.o src/sqlib/ambsql.c extras/sqlite/libsqlite3.a extras/sqlite/sqlite3.h
	@echo "  LD    $@"
	@$(SQLIB_CC) -Isrc/sqlib -Iextras/sqlite -o $@.dbg build/sqlib/libinit.o src/sqlib/ambsql.c \
		extras/sqlite/libsqlite3.a -lm -lamiga
	@$(STRIP) -o $@ $@.dbg

# jdec, Ambiance's JPEG decoder (src/jdec), built in for JPEG thumbnails
# (src/jpgthumb.c); -O2: it is the hot path
build/jdec.o: src/jdec/jdec.c src/jdec/jdec.h
	@mkdir -p build
	@echo "  CC    $<"
	@$(CC) -O2 $(CPU) -noixemul -Wall -Wextra -c -o $@ $<

EXTRAS = extras/aminet/modplug-68k.lha extras/aminet/mpega_library.lha extras/aminet/xfdmaster.lha
lha: $(TARGET) Ambiance.readme $(ICONS) docs/install.info docs/Install-Ambiance $(FONT_FILES) \
     $(SMOOTH_FILES) $(CJK_FILES) $(LICENCES) build/catalogs/.built $(EXTRAS) $(SQLIB) $(DVDLIB) $(AC3LIB) $(AC3FPULIB) $(MPGALIB) $(MPEGVLIB) \
     assets/certs/cacert.pem
	@echo "  LHA   $(LHA_NAME)"
	@rm -rf build/lha
	@mkdir -p $(LHA_DIR)/Fonts/amb-pixel $(LHA_DIR)/Fonts/amb-smooth $(LHA_DIR)/Fonts/amb-cjk \
		$(LHA_DIR)/Licences $(LHA_DIR)/Visuals
	@cp docs/Visuals-ReadMe.txt $(LHA_DIR)/Visuals/ReadMe.txt
	@cp $(TARGET) $(LHA_DIR)/
	@cp docs/Ambiance.info $(LHA_DIR)/
	@# the AppIcon shown while iconified (iconify.c): no drawer icon, unseen
	@mkdir -p $(LHA_DIR)/Icons
	@cp docs/appicon.info $(LHA_DIR)/Icons/AppIcon.info
	@cp assets/Fonts/amb-pixel.font $(LHA_DIR)/Fonts/
	@cp assets/Fonts/amb-pixel/12 assets/Fonts/amb-pixel/24 assets/Fonts/amb-pixel/36 \
		$(LHA_DIR)/Fonts/amb-pixel/
	@for f in $(GALMURI); do n=$${f%%:*}; mkdir -p $(LHA_DIR)/Fonts/$$n; \
		cp assets/Fonts/$$n.font $(LHA_DIR)/Fonts/; \
		cp assets/Fonts/$$n/$${f##*:} $(LHA_DIR)/Fonts/$$n/; done
	@cp $(SMOOTH_FILES) $(LHA_DIR)/Fonts/amb-smooth/
	@cp $(CJK_FILES) $(LHA_DIR)/Fonts/amb-cjk/
	@cp $(LICENCES) $(LHA_DIR)/Licences/
	@mkdir -p $(LHA_DIR)/Layouts
	@cp $(LAYOUTS) $(LHA_DIR)/Layouts/
	@mkdir -p $(LHA_DIR)/Libs
	@cp $(SQLIB) $(LHA_DIR)/Libs/
	@# dvdnav.library (DVDs: libdvdread and libdvdnav, GPL-3; its source
	@# at github.com/luvwagn/dvdnav.library), extras/dvdnav
	@cp $(DVDLIB) $(LHA_DIR)/Libs/
	@cp extras/dvdnav/LICENSE $(LHA_DIR)/Licences/dvdnav.library.txt
	@# ac3.library and ac3fpu.library (AC-3 sound: liba52, GPL-3; their
	@# source at github.com/luvwagn/ac3.library), extras/ac3, for a video's
	@# AC3 where the ZZ9000 will not decode it
	@cp $(AC3LIB) $(AC3FPULIB) $(LHA_DIR)/Libs/
	@cp extras/ac3/LICENSE $(LHA_DIR)/Licences/ac3.library.txt
	@# ambmpega.library (MP3 and MP2 by the CPU: libmad, GPL-3; its source
	@# at github.com/luvwagn/mpga.library), extras/mpga
	@cp $(MPGALIB) $(LHA_DIR)/Libs/
	@cp extras/mpga/LICENSE $(LHA_DIR)/Licences/ambmpega.library.txt
	@# mpegv.library (MPEG-1 and MPEG-2 by the CPU: libmpeg2, GPL-3; its source
	@# at github.com/luvwagn/mpegv.library), extras/mpegv
	@cp $(MPEGVLIB) $(LHA_DIR)/Libs/
	@cp extras/mpegv/LICENSE $(LHA_DIR)/Licences/mpegv.library.txt
	@cp extras/sqlite/LICENSE.md $(LHA_DIR)/Licences/SQLite.txt
	@# Mozilla's root certificates (certifi's cacert.pem, MPL 2.0): what
	@# HTTPS checks a server's certificate with (tls.c), AmiSSL's own or not
	@mkdir -p $(LHA_DIR)/Certs
	@cp assets/certs/cacert.pem $(LHA_DIR)/Certs/
	@cp assets/certs/LICENSE-certifi.txt $(LHA_DIR)/Licences/Mozilla-CA-certificates.txt
	@awk -v version="$(FULL_VERSION)" '{ gsub(/\[VERSION\]/, version); print }' \
		Ambiance.readme > $(LHA_DIR)/readme.txt
	@cp docs/readme.info $(LHA_DIR)/readme.txt.info
	@cp docs/drawer.info build/lha/Ambiance.info
	@mkdir -p $(LHA_DIR)/Catalogs
	@cp -R build/catalogs/* $(LHA_DIR)/Catalogs/
	@cp docs/Install-Ambiance build/lha/Install-Ambiance
	@# Aminet's libraries for more of the music, each archive's files whole
	@# and unchanged (their terms: kept together, not changed), for the
	@# installer to offer where they are missing or older (never on AGS)
	@mkdir -p build/lha/Extras
	@for a in $(EXTRAS); do (cd build/lha/Extras && lha xq ../../../$$a); done
	@cp docs/install.info build/lha/Install-Ambiance.info
	@rm -f $(LHA_NAME)
	@cd build/lha && lha aqo5 $(LHA_OPTS) ../../$(LHA_NAME) Ambiance Ambiance.info \
		Install-Ambiance Install-Ambiance.info Extras
	@echo "Created $(LHA_NAME)"

clean:
	rm -rf build/*.o build/lha build/sqlib build/dvd build/beets build/beets_68k build/catalogs build/vt build/test_ags build/test_songs build/songs build/ram $(VERSION_STAMP) $(TARGET) \
		ambiance-*.lha
