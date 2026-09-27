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
set -eu
cd "$(dirname "$0")/.."
SRC=${SRC:-../Sonic2_Extracted}
rm -rf build/iso && mkdir -p build/iso
rsync -a --exclude '*.gif' --exclude '*.wav' --exclude '*.ogg' --exclude '.DS_Store' \
      --exclude 'Game/Menu/' --exclude 'Game/Models/' --exclude 'Stages/ZoneM/' "$SRC/Data/" build/iso/Data/
rsync -a --exclude 'ZoneM.bin' --exclude '.DS_Store' "$SRC/Bytecode/" build/iso/Bytecode/
/usr/bin/python3 tools/scripts/patch_bytecode.py build/iso/Bytecode  # hot script loops -> PS1 native opcodes
/usr/bin/python3 tools/scripts/scene3d_stages.py build/iso/Bytecode build/iso/Data  # stages that get the Scene3D arrays
make -s -C tools/rsdkmanifest
rm -rf build/manifest && mkdir -p build/manifest
for v in ":0" "_t:1" "_k:2" "_st:3"; do
    suffix=${v%%:*} player=${v#*:}
    tools/rsdkmanifest/rsdkmanifest build/iso "build/manifest/p$player" "$player" > "build/manifest/p$player.log" 2>&1 ||
        { cat "build/manifest/p$player.log"; exit 1; }
    /usr/bin/python3 tools/atlas/build_atlas.py "$SRC/Data" "build/manifest/p$player" --suffix "$suffix" \
        --out build/iso/Data/Sprites/Atlas > "build/manifest/atlas$suffix.log" || { cat "build/manifest/atlas$suffix.log"; exit 1; }
done
/usr/bin/python3 tools/tiles/convert_tiles.py "$SRC/Data" build/iso/Data
/usr/bin/python3 tools/bgstrips/build_bgstrips.py build/iso/Data > build/manifest/bgstrips.log || { cat build/manifest/bgstrips.log; exit 1; }
/usr/bin/python3 tools/menu/build_menu.py "$SRC/Data" build/iso/Data # menu art + fonts + strings (phase 4.2)
# SFX banks (phase 5.1): resident + menu + one per stage, SPU-ADPCM, from the original scripts' reachable sets
/usr/bin/python3 tools/audio/build_sfx.py "$SRC/Data" build/iso/Data "$SRC/Bytecode"
# Music (phase 5.2): 44 Ogg tracks -> Data/Music/Music.xa (CD-XA, 8 channels, loop rows from the scripts) + Music.bin
/usr/bin/python3 tools/audio/build_music.py "$SRC/Data" build/iso/Data "$SRC/Bytecode"
# docs/30 phase 7.3: the console reads 128x128Tiles.ps1 / CollisionMasks.ps1 (written by rsdkmanifest above with the
# engine's own loaders); the originals were only needed by the host tools (BG strips)
for d in build/iso/Data/Stages/*/; do
    [ -f "$d/128x128Tiles.ps1" ] && rm -f "$d/128x128Tiles.bin"
    [ -f "$d/CollisionMasks.ps1" ] && rm -f "$d/CollisionMasks.bin"
done
/usr/bin/python3 tools/disc/gen_load_order.py build/iso # files in the order the game opens them (docs/30 phase 7.3 / 9)
echo "iso root: $(find build/iso -type f | wc -l | tr -d ' ') files, $(du -sh build/iso | cut -f1)"
make -s disc
