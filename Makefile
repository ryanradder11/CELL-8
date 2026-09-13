#---------------------------------------------------------------------------------
# Cell-8 - minimal PSL1GHT homebrew: PPU hello world + RSX clear/flip + intro screen
#---------------------------------------------------------------------------------

TARGET	:= Cell-8

OFILES	:= source/main.o source/rsxutil.o source/font/font5x7.o source/chip8/chip8.o source/romlist/romlist.o source/sound/sound.o source/screen/screen.o source/screen/simple.vpo.o source/screen/simple.fpo.o

# -I. : picks up simple_vpo.h/simple_fpo.h, generated at the repo root by
# the %.vpo.o/%.fpo.o rules below (bin2o writes them next to the Makefile,
# not next to the .vcg/.fcg source -- see the comment by those rules).
INCLUDES := -Iinclude -I. -I$(PS3DEV)/ppu/include

# -lio: needed for io/pad.h (detecting the intro-screen button press /
# CHIP-8 keypad input)
# HDD/USB directory scanning in romlist.cpp uses standard POSIX
# -laudio: PSL1GHT audio-port API, used for the CHIP-8 sound_timer beep
LIBS	 := -lrsx -lgcm_sys -lsysutil -lio -laudio -lrt -llv2 -lm

BUILDDIR := build

# Bundles roms/ (just the 8 Timendus chip8-test-suite ROMs) into the
# .pkg's USRDIR/roms/ -- the exact same directory romlist.cpp's HDD scan
# reads from (/dev_hdd0/game/CELL80001/USRDIR/roms/), so once installed
# these show up in the menu automatically, no extra code needed.
PKGFILES := roms

TITLE	:= Cell-8
APPID	:= CELL80001
ICON0	:= ICON0.png

# Local RPCS3 install, used by the "run" target below. Override on the
# command line (make RPCS3=/other/path run) if it ever moves.
RPCS3	?= /Users/ryanradder/projects/PS3_development/rpcs3-v0.0.42-19931-de6c1b52_macos_aarch64/RPCS3.app/Contents/MacOS/rpcs3

include $(PSL1GHT)/ppu_rules

# ppu_rules' default CGCOMP is the plain arm64 cgcomp, which can't load
# NVIDIA's Cg library (arm64 process, x86_64-only library -- see CLAUDE.md's
# "Shader compilation" section). Route shader compiles through the x86_64
# build instead, via Rosetta.
CGCOMP := arch -x86_64 /usr/local/ps3dev/bin/cgcomp-x86_64

# bin2o (from base_rules) turns a compiled shader binary into a linkable
# .o plus a matching header of extern symbols (e.g. simple.vpo -> a
# simple_vpo.h declaring simple_vpo[]/simple_vpo_end[]/simple_vpo_size).
# Only data_rules defines these two patterns by default, and this project
# includes ppu_rules instead, so they're added directly here.
%.vpo.o : %.vpo
	$(VERB) echo $(notdir $<)
	$(VERB) $(bin2o)

%.fpo.o : %.fpo
	$(VERB) echo $(notdir $<)
	$(VERB) $(bin2o)

# MACHDEP is only defined by ppu_rules (included above), so CFLAGS/CXXFLAGS
# must be assigned after the include -- assigning them earlier with ":="
# would have expanded $(MACHDEP) as empty.
# -mcpu=cell, -Os: known-working PSL1GHT samples (e.g. the rsxtest sample)
# always compile with both. PSL1GHT's RSX_FUNC command macros do some
# hand-written inline-asm register/context handling (manually moving r2/r31,
# adjusting the stack) for passing the RSX context through -- exactly the
# kind of code whose correctness can depend on the compiler's optimization
# level and target tuning. Cell-8 had neither (plain -O0, generic
# powerpc64-ps3-elf tuning) until real 3D draw calls needed to work.
CFLAGS	 := $(INCLUDES) -Wall -O2 $(MACHDEP)
CXXFLAGS := $(CFLAGS)
LDFLAGS	 := $(MACHDEP)

# Apple's bundled GNU Make 3.81 has a built-in default of LD=ld, which
# defeats base_rules' "LD ?= $(PREFIX)gcc" (the ?= only fires on an
# *unset* variable, and make's own built-in default counts as set).
# Force the PPU cross-linker explicitly. Use g++ since main.cpp/rsxutil.cpp
# are C++.
LD := ppu-g++

# ppu_rules' generic %.self pattern rule also generates a legacy
# "fake self" via the fself tool, for jailbroken-console fake-signed
# loading. On this ARM64 ps3dev build, fself crashes (bus error) on
# valid ELFs -- a toolchain bug unrelated to this app, and the fake
# self isn't needed to run under RPCS3. This explicit (non-pattern)
# rule takes precedence over that pattern rule and skips the fself step.
$(TARGET).self: $(TARGET).elf
	@echo CEX self ... $(notdir $@)
	@mkdir -p $(BUILDDIR)
	@$(STRIP) $< -o $(BUILDDIR)/$(notdir $<)
	@$(SPRX) $(BUILDDIR)/$(notdir $<)
	@$(SELF) $(BUILDDIR)/$(notdir $<) $@

all: $(TARGET).self

# screen.cpp includes the headers bin2o generates as a side effect of
# building these -- without this, make has no reason to build them first.
# (Kept after "all" so "all" -- not this -- stays the default goal.)
source/screen/screen.o: source/screen/simple.vpo.o source/screen/simple.fpo.o

pkg: $(TARGET).pkg

run: $(TARGET).self
	"$(RPCS3)" "$(CURDIR)/$(TARGET).self"

clean:
	rm -rf $(TARGET).elf $(TARGET).self $(TARGET).pkg $(TARGET)*.gnpdrm.pkg $(TARGET).fake.self $(BUILDDIR) $(OFILES)
	rm -f source/screen/simple.vpo source/screen/simple.fpo simple_vpo.h simple_fpo.h
