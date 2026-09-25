#!/bin/bash
# Halt the current rsdkv4.ps-exe right after game frame N (PS1PerfTick) and dump VRAM + the two
# framebuffers as PNGs (tools/vramshot.py). At that point the chain of frame N hasn't been sent yet:
# the displayed buffer holds frame N-1 complete (the other one is older). Prints renderer counters.
# Usage: tools/frame_capture.sh N OUT_PREFIX   (MASK=bits FROM=frame: hold g_ps1PadTestMask from that frame;
#        GDBSET='var=value' also applied at FROM, e.g. GDBSET='waterLevel=1900')
cd "$(dirname "$0")/.."
N=$1; OUT=$2
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv4.elf
break PS1PerfTick if g_ps1PerfFrames == ${FROM:-0}
continue
set var g_ps1PadTestMask = ${MASK:-0}
${GDBSET:+set var $GDBSET}
delete
break PS1PerfTick if g_ps1PerfFrames == $N
continue
printf "ACTIVE %d %d %d %d mid %d | layer1 type %d %dx%d\n", activeTileLayers[0], activeTileLayers[1], activeTileLayers[2], activeTileLayers[3], tLayerMidPoint, stageLayouts[1].type, stageLayouts[1].xsize, stageLayouts[1].ysize
printf "FRAME %u chainOverflow=%u parity=%d folder=%s | sprites=%u quads=%u layerPrims=%u layerHbl=%u lastHbl=%u | chainWords=%u high=%u overflow=%u | miss=%u unbound=%u todo=%u draws=%u repeats=%u\n", g_ps1PerfFrames, g_ps1ChainOverflow, g_ps1GPU->m_parity, stageList[activeStageList][stageListPosition].folder, g_ps1DrawSprites, g_ps1DrawQuads, g_ps1LayerPrims, g_ps1LayerHbl, g_ps1PerfLastHbl, g_ps1ChainLastWords, g_ps1ChainHighWater, g_ps1ChainOverflow, g_ps1AtlasDrawMiss, g_ps1SheetUnbound, g_ps1DrawTodo, g_ps1DrawFrames, g_ps1RepeatFrames
shell python3 tools/vramshot.py ${OUT} >/dev/null
printf "FLOOR quads=%u lines=%u hbl=%u nearY=%d cand=%u\n", g_ps1FloorQuads, g_ps1FloorLines, g_ps1FloorHbl, g_ps1FloorNearY, g_ps1FloorCand
printf "FACES drawn=%u texMiss=%u | glyph fallback=%u scale=%u char=%u tables=%u\n", g_ps1Faces, g_ps1FaceTexMiss, g_ps1GlyphFallback, g_ps1GlyphFallbackScale, g_ps1GlyphFallbackChar, g_ps1GlyphTables
printf "MISS count=%u sheet=%d %s x=%d y=%d\n", g_ps1AtlasDrawMiss, g_ps1MissSheet, g_ps1MissSheet >= 0 ? g_ps1AtlasSheets[g_ps1MissSheet].name : "-", g_ps1MissX, g_ps1MissY
printf "SLOTS %u %u %u %u wait=%u slowTiles=%u prims=%u\n", g_ps1ProfSlot[0], g_ps1ProfSlot[1], g_ps1ProfSlot[2], g_ps1ProfSlot[3], g_ps1ProfWaitHbl, g_ps1TileFlipped, g_ps1LayerPrims
printf "LAYERS uniform=%u lines=%u tiles=%u strips=%u (hblanks) calls=%u bands tiles=%u strips=%u stripRegions=%u objects=%u drawSubs=%u | ink pieces=%u clutFades=%u fadeMode=%d fadeA=%d | banks regions=%u mask=%x water=%d\n", g_ps1ProfLayer[0], g_ps1ProfLayer[1], g_ps1ProfLayer[2], g_ps1ProfLayer[3], g_ps1ProfLayer[4], g_ps1TileBandCount, g_ps1StripBandCount, g_ps1StripRegions, g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl, g_ps1InkPieces, g_ps1ClutFades, fadeMode, fadeA, g_ps1BankRegions, g_ps1BankMask, waterDrawPos
detach
GDBEOF
# Other sessions share this emulator (GDB port 3333): never kill theirs, only our own PID.
pgrep -x PCSX-Redux >/dev/null && { echo "PCSX-Redux is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(FRAME|LAYERS|ACTIVE|SLOTS|MISS|FACES|FLOOR) .*"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
