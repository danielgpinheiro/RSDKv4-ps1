# RSDKv4 (Sonic 2) PS1 port — Makefile
#
# Builds a .ps-exe via the psyqo toolchain (mipsel-none-elf, C++20), with the Sonic CD port's flags
# (../RSDKv3-ps1/Makefile). Plan: ../docs/30.
#
# Phase 0: the PS1 platform layer (ps1/, copied from RSDKv3-ps1 at 1e583ad) + a smoke-test app
# (ps1/platform_test.cpp). The RSDKv4 engine sources join SRCS in phase 2; the engine-bound layer files
# (render, tileset, sprite_atlas, fmv) join with the renderer (phase 3).
TARGET = rsdkv4
TYPE = ps-exe

SRCS = \
ps1/platform_test.cpp \
ps1/libc/string.cpp \
ps1/libc/stdlib.cpp \
ps1/libc/math.cpp \
ps1/libc/stdio.cpp \
ps1/cdrom_fs.cpp \
ps1/cdrom_stream.cpp \
ps1/gpu_chain.cpp \
ps1/spu.cpp \
ps1/xa_music.cpp \
ps1/mdec.cpp \
ps1/video.cpp \
ps1/loading.cpp \
ps1/memcard.cpp \

# ps1/libc comes FIRST: its C/C++ shim headers (<string>, <cmath>, <stdio.h>, ...) shadow any
# system headers the freestanding toolchain might otherwise surface.
CPPFLAGS = -I ps1/libc -I RSDKv4 -DRETRO_USE_MOD_LOADER=0

# `make MEMCARD_TEST=1`: memory card self-test (ps1/memcard_test.cpp).
ifeq ($(MEMCARD_TEST),1)
CPPFLAGS += -DPS1_MEMCARD_TEST=1
SRCS += ps1/memcard_test.cpp
endif

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

# The FMV Huffman (VLC) decoder is the hot loop of video playback: optimise it for speed.
ps1/mdec.o: CPPFLAGS += -O2
ps1/gpu_chain.o: CPPFLAGS += -O2

# Cancel make's built-in `%: %.o` link rule: the extensionless libc shim headers
# (ps1/libc/string, vector, ...) listed in .dep files must not be rebuilt from `string.o`.
%: %.o

# Test disc (phase 0): a hand-picked subset of ../Sonic2_Extracted/Data (tools/smoke_disc.sh), then
# mkpsxiso + the byte-level verifier. The real asset pipeline arrives in phase 1.
disc:
	/usr/bin/python3 tools/disc/build_disc.py build/iso rsdkv4.ps-exe none build/game
	/usr/bin/python3 tools/disc/verify_disc.py build/game.bin build/iso rsdkv4.ps-exe none

.PHONY: disc
