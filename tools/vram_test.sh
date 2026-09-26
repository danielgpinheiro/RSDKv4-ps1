#!/bin/bash
# Phase 3.1 check (after the Sonic CD port's tools/vram_test.sh): the stage's tile pages + sprite atlas in VRAM and
# the tile palette entries equal the build-time files (tools/check_vram.py). Boots the current rsdkv4.ps-exe (e.g.
# `make BOOT=Zone01 BOOT_PLAYER=3`), dumps activePalette32 right after the stage load and VRAM at frame F
# (PCSX-Redux web server).
# Usage: [INJECT="gdb cmds"] tools/vram_test.sh STAGE[_t|_k|_st] [OUT_PREFIX] [F=10]   (the suffix names the player
#        variant's atlas; INJECT runs once, at the first PS1TileSetLoad, before the stage load picks the atlas)
cd "$(dirname "$0")/.."
STAGE=$1; OUT=${2:-/tmp/rsdkv4_vram}; F=${3:-10}
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv4.elf
break PS1LoadingDoneHook if g_ps1InitDone
commands
silent
dump binary memory ${OUT}_pal.bin &activePalette32[0] &activePalette32[256]
printf "LOADED %s player=%d tiles=%u (%u B) atlas=%u (%u B) clusters=%d\n", stageList[activeStageList][stageListPosition].folder, playerListPos, g_ps1TileSetLoads, g_ps1TileSetBytes, g_ps1AtlasLoads, g_ps1AtlasBlockBytes, g_ps1AtlasClusterCount
continue
end
set \$inj = 0
break PS1TileSetLoad if \$inj == 0
commands
silent
set \$inj = 1
${INJECT:-echo}
printf "INJECT at load: playerListPos=%d\\n", playerListPos
continue
end
break PS1PerfTick if g_ps1PerfFrames == $F
commands
silent
shell /usr/bin/python3 tools/vramshot.py ${OUT} >/dev/null
printf "FRAME %u heap total=%u headroom=%u\n", g_ps1PerfFrames, g_ps1HeapTotal, g_ps1HeapHeadroom
detach
end
continue
GDBEOF
# Other sessions share this emulator (GDB port 3333): never kill theirs, only our own PID.
pgrep -x PCSX-Redux >/dev/null && { echo "PCSX-Redux is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver -iso build/game.cue -exe rsdkv4.ps-exe \
      >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-240} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(INJECT|LOADED|FRAME) .*"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
/usr/bin/python3 tools/check_vram.py "${OUT}_vram.bin" build/iso/Data "$STAGE" "${OUT}_pal.bin"
