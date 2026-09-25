#!/bin/bash
# Game disc from the extracted Sonic 2 data (phase 2): Data/ + Bytecode/ into build/iso without the PC source
# assets (tools/disc/build_disc.py SOURCE_ONLY: .gif .wav .ogg), the native menus' art and meshes
# (Data/Game/Menu, Data/Game/Models: docs/30, 2D PS1 menus in phase 4), Egg Gauntlet (ZoneM: left out,
# user decision 2026-09-25) and macOS metadata, then `make disc` (mkpsxiso + verify_disc, no license).
# SRC = the extracted tree holding Data/ and Bytecode/: ../Sonic2_Extracted, or tools/extract_rsdk.py's output.
# Converted assets (atlases, tiles, SFX, music) join in later phases.
set -eu
cd "$(dirname "$0")/.."
SRC=${SRC:-../Sonic2_Extracted}
rm -rf build/iso && mkdir -p build/iso
rsync -a --exclude '*.gif' --exclude '*.wav' --exclude '*.ogg' --exclude '.DS_Store' \
      --exclude 'Game/Menu/' --exclude 'Game/Models/' --exclude 'Stages/ZoneM/' "$SRC/Data/" build/iso/Data/
rsync -a --exclude 'ZoneM.bin' --exclude '.DS_Store' "$SRC/Bytecode/" build/iso/Bytecode/
echo "iso root: $(find build/iso -type f | wc -l | tr -d ' ') files, $(du -sh build/iso | cut -f1)"
make -s disc
