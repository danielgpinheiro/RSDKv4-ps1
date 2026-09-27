#!/bin/bash
# Game disc from the extracted Sonic 2 data (phase 2): Data/ + Bytecode/ into build/iso without the PC source
# assets (tools/disc/build_disc.py SOURCE_ONLY: .gif .wav .ogg), the native menus' art and meshes
# (Data/Game/Menu, Data/Game/Models: docs/30, 2D PS1 menus in phase 4), Egg Gauntlet (ZoneM: left out,
# user decision 2026-09-25) and macOS metadata, then `make disc` (mkpsxiso + verify_disc, no license).
# SRC = the extracted tree holding Data/ and Bytecode/: ../Sonic2_Extracted, or tools/extract_rsdk.py's output.
# The bytecode gets its PS1 edits (tools/scripts/patch_bytecode.py). Sprite atlases (phase 3.1): the engine's own
# manifests per player variant (tools/rsdkmanifest runs every stage's startups on the disc tree), packed by
# tools/atlas/build_atlas.py from the GIFs into Data/Sprites/Atlas/<Stage><suffix>.atl ('' Sonic, _t Tails,
# _k Knuckles, _st Sonic & Tails). Tile pages: tools/tiles/convert_tiles.py -> Data/Stages/<Stage>/16x16Tiles.vram.
# BG line strips (phase 3.5, after both: they fill the VRAM each atlas leaves free): tools/bgstrips ->
# Data/Stages/<Stage>/BGStrips<suffix>.bin.
# SFX (phase 5.1): tools/audio/build_sfx.py -> Data/Game/PS1SfxResident.bin, PS1SfxMenu.bin,
# Data/Stages/<Stage>/PS1Sfx.bin. Music (5.2): tools/audio/build_music.py -> Data/Music/Music.xa + Music.bin.
# Since phase 11 the steps live in tools/build_assets.py (shared with the public builder); this runs it on SRC.
set -eu
cd "$(dirname "$0")/.."
SRC=${SRC:-../Sonic2_Extracted}
# one pipeline for the port and the public builder (docs/30 phase 11): tools/build_assets.py
/usr/bin/python3 tools/build_assets.py --src "$SRC"
make -s disc
