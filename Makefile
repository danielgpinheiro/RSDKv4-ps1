# RSDKv4 (Sonic 2) PS1 port — Makefile
#
# Builds a .ps-exe via the psyqo toolchain (mipsel-none-elf, C++20), with the Sonic CD port's flags
# (../RSDKv3-ps1/Makefile). Plan: ../docs/30.
#
# The RSDKv4 engine (RSDKv4/, PS1 app in RSDKv4/main.cpp) on the PS1 platform layer (ps1/, copied from
# RSDKv3-ps1 at 1e583ad). Phase 3: the GPU renderer (ps1/render.cpp, 3.2) over the tile pages and sprite atlas
# uploaded at stage load (ps1/tileset, ps1/sprite_atlas, 3.1); audio still headless (ps1/audio_headless.cpp,
# phase 5). Not built
# on PS1: Drawing.cpp, Renderer.cpp, Audio.cpp, Networking.cpp and the native menu objects
# (docs/30: the scripted Title + 2D PS1 menus in phase 4).
# `make PLATFORM_TEST=1`: the phase 0 smoke test (ps1/platform_test.cpp) instead.
TARGET = rsdkv4
TYPE = ps-exe

SRCS = \
RSDKv4/Animation.cpp \
RSDKv4/Collision.cpp \
RSDKv4/Debug.cpp \
RSDKv4/Ini.cpp \
RSDKv4/Input.cpp \
RSDKv4/Math.cpp \
RSDKv4/ModAPI.cpp \
RSDKv4/Object.cpp \
RSDKv4/Palette.cpp \
RSDKv4/Reader.cpp \
RSDKv4/RetroEngine.cpp \
RSDKv4/Scene.cpp \
RSDKv4/Scene3D.cpp \
RSDKv4/Script.cpp \
RSDKv4/Sprite.cpp \
RSDKv4/String.cpp \
RSDKv4/Text.cpp \
RSDKv4/Userdata.cpp \
RSDKv4/main.cpp \
ps1/render.cpp \
ps1/audio_headless.cpp \
ps1/tileset.cpp \
ps1/sprite_atlas.cpp \
ps1/menu.cpp \
ps1/libc/string.cpp \
ps1/libc/stdlib.cpp \
ps1/libc/math.cpp \
ps1/libc/stdio.cpp \
ps1/cdrom_fs.cpp \
ps1/cdrom_stream.cpp \
ps1/irq_guard.cpp \
ps1/gpu_chain.cpp \
ps1/spu.cpp \
ps1/xa_music.cpp \
ps1/mdec.cpp \
ps1/video.cpp \
ps1/loading.cpp \
ps1/memcard.cpp \

ifeq ($(PLATFORM_TEST),1)
SRCS := ps1/platform_test.cpp $(filter-out RSDKv4/% ps1/%_headless.cpp ps1/render.cpp ps1/tileset.cpp ps1/sprite_atlas.cpp ps1/menu.cpp,$(SRCS))
endif

# ps1/libc comes FIRST: its C/C++ shim headers (<string>, <cmath>, <stdio.h>, ...) shadow any
# system headers the freestanding toolchain might otherwise surface.
CPPFLAGS = -I ps1/libc -I RSDKv4 -I RSDKv4/NativeObjects -DRETRO_USE_MOD_LOADER=0 -DRETRO_USE_NETWORKING=0

# `make BOOT=<stage folder>` (e.g. Zone01): start in that stage (test harness): its first GameConfig entry
# unless BOOT_ID is given, with player BOOT_PLAYER (stage.playerListPos: 0 Sonic, 1 Tails, 2 Knuckles,
# 3 Sonic & Tails). Rebuild RSDKv4/main.cpp and RetroEngine.cpp when switching (touch them).
# `make PROFILE=1`: count VM instructions per object type (tools/obj_profile.sh; costs ~1-2 % CPU).
ifeq ($(PROFILE),1)
CPPFLAGS += -DPS1_PROFILE=1
endif

ifneq ($(BOOT),)
CPPFLAGS += -DPS1_BOOT_FOLDER='"$(BOOT)"' -DPS1_BOOT_ID='"$(BOOT_ID)"' -DPS1_BOOT_PLAYER=$(or $(BOOT_PLAYER),0)
endif

# `make MEMCARD_TEST=1`: memory card self-test (ps1/memcard_test.cpp).
ifeq ($(MEMCARD_TEST),1)
CPPFLAGS += -DPS1_MEMCARD_TEST=1
SRCS += ps1/memcard_test.cpp
endif

# Build-flag stamp: every object depends on build/flags.txt, rewritten only when PROFILE / BOOT / BOOT_ID /
# BOOT_PLAYER change, so switching them rebuilds everything (make only sees source timestamps otherwise: a
# PROFILE=1 build once kept a non-profile Object.o).
BUILD_FLAGS := PROFILE=$(PROFILE) BOOT=$(BOOT) BOOT_ID=$(BOOT_ID) BOOT_PLAYER=$(BOOT_PLAYER)
$(shell mkdir -p build; echo '$(BUILD_FLAGS)' | cmp -s - build/flags.txt || echo '$(BUILD_FLAGS)' > build/flags.txt)
EXTRA_DEPS += build/flags.txt

# C++20 is the minimum required by psyqo.
CXXFLAGS = -std=c++20

# This activates the PSYQo library and the rest of the toolchain.
include ../nugget/psyqo/psyqo.mk

# The R3000A has no FPU: soft-float + libgcc's helpers. Must come AFTER the include so it overrides
# common.mk's -mfp32 ARCHFLAGS.
CPPFLAGS += -msoft-float
LDFLAGS   += -msoft-float
LDFLAGS   += -lgcc
# Retail PS1 = 2 MB RAM (EXE at 0x80010000, ~1.94 MB usable). Never raise this for a release.
LDFLAGS   += -Wl,--defsym,RAM_SIZE=2M
# nugget's hardware-measured memcpy/memset (8-word / 4-word blocks).
LDFLAGS   += -Wl,-wrap,memcpy -Wl,-wrap,memset

# Trig tables computed at build time on the host (tools/trig/gen_trig.c), included by Math.cpp.
CPPFLAGS += -I build/gen
build/gen/trig_tables.inc: tools/trig/gen_trig.c
	mkdir -p build/gen
	cc -O2 -o build/gen/gen_trig $<
	build/gen/gen_trig > $@
RSDKv4/Math.o RSDKv4/Math.dep: build/gen/trig_tables.inc

# The FMV Huffman (VLC) decoder is the hot loop of video playback: optimise it for speed.
ps1/mdec.o: CPPFLAGS += -O2
ps1/gpu_chain.o: CPPFLAGS += -O2
# The script VM and the object / collision loops are the CPU hot path (as in the Sonic CD port).
RSDKv4/Script.o RSDKv4/Object.o RSDKv4/Collision.o: CPPFLAGS += -O2

$(OBJS): build/flags.txt

# Cancel make's built-in `%: %.o` link rule: the extensionless libc shim headers
# (ps1/libc/string, vector, ...) listed in .dep files must not be rebuilt from `string.o`.
%: %.o

# Test disc (phase 0): a hand-picked subset of ../Sonic2_Extracted/Data (tools/smoke_disc.sh), then
# mkpsxiso + the byte-level verifier. The real asset pipeline arrives in phase 1.
disc:
	/usr/bin/python3 tools/disc/build_disc.py build/iso rsdkv4.ps-exe none build/game
	/usr/bin/python3 tools/disc/verify_disc.py build/game.bin build/iso rsdkv4.ps-exe none

.PHONY: disc
