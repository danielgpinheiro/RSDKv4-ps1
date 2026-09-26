#!/bin/bash
# Halt the current rsdkv4.ps-exe right after game frame N (PS1PerfTick) and dump VRAM + the two
# framebuffers as PNGs (tools/vramshot.py). At that point the chain of frame N hasn't been sent yet:
# the displayed buffer holds frame N-1 complete (the other one is older). Prints renderer counters.
# Usage: tools/frame_capture.sh N[,N2,...] OUT_PREFIX   (MASK=bits FROM=frame: hold g_ps1PadTestMask from that
#        frame; GDBSET='var=value' also applied at FROM, e.g. GDBSET='waterLevel=1900'). A list of frames (increasing,
#        all after FROM) is captured in one run as OUT_PREFIX_<frame>_*.
cd "$(dirname "$0")/.."
N=$1; OUT=$2
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
FRAMES=$(echo "$N" | tr ',' ' ')
LAST=$(echo $FRAMES | awk '{print $NF}')
{
cat <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv4.elf
break PS1PerfTick if g_ps1PerfFrames == ${FROM:-0}
continue
set var g_ps1PadTestMask = ${MASK:-0}
${GDBSET:+set var $GDBSET}
delete
GDBEOF
for F in $FRAMES; do
  P=$OUT; [ "$F" != "$N" ] && P=${OUT}_$F # a list of frames: OUT_<frame>_fb0.png ...
cat <<GDBEOF
break PS1PerfTick if g_ps1PerfFrames == $F
continue
delete
printf "ACTIVE %d %d %d %d mid %d | layer1 type %d %dx%d\n", activeTileLayers[0], activeTileLayers[1], activeTileLayers[2], activeTileLayers[3], tLayerMidPoint, stageLayouts[1].type, stageLayouts[1].xsize, stageLayouts[1].ysize
printf "FRAME %u chainOverflow=%u parity=%d folder=%s | sprites=%u quads=%u layerPrims=%u layerHbl=%u lastHbl=%u | chainWords=%u high=%u overflow=%u | miss=%u unbound=%u todo=%u draws=%u repeats=%u\n", g_ps1PerfFrames, g_ps1ChainOverflow, g_ps1GPU->m_parity, stageList[activeStageList][stageListPosition].folder, g_ps1DrawSprites, g_ps1DrawQuads, g_ps1LayerPrims, g_ps1LayerHbl, g_ps1PerfLastHbl, g_ps1ChainLastWords, g_ps1ChainHighWater, g_ps1ChainOverflow, g_ps1AtlasDrawMiss, g_ps1SheetUnbound, g_ps1DrawTodo, g_ps1DrawFrames, g_ps1RepeatFrames
shell /usr/bin/python3 tools/vramshot.py ${P} >/dev/null
printf "MISS count=%u sheet=%d %s x=%d y=%d\n", g_ps1AtlasDrawMiss, g_ps1MissSheet, g_ps1MissSheet >= 0 ? g_ps1AtlasSheets[g_ps1MissSheet].name : "-", g_ps1MissX, g_ps1MissY
printf "SLOTS %u %u %u %u wait=%u slowTiles=%u prims=%u | replay hbl=%u walked=%u kept=%u | split frames=%u cuts=%u quads=%u fallbacks=%u\n", g_ps1ProfSlot[0], g_ps1ProfSlot[1], g_ps1ProfSlot[2], g_ps1ProfSlot[3], g_ps1ProfWaitHbl, g_ps1TileFlipped, g_ps1LayerPrims, g_ps1BankHbl, g_ps1ReplayWalked, g_ps1ReplayKept, g_ps1SplitFrames, g_ps1SplitCuts, g_ps1SplitQuads, g_ps1SplitFallbacks
printf "LAYERS uniform=%u lines=%u tiles=%u (hblanks) calls=%u bands=%u objects=%u drawSubs=%u | ink pieces=%u clutFades=%u fadeMode=%d fadeA=%d | banks regions=%u mask=%x water=%d | faces=%u texMiss=%u (sheet %d uv %d,%d-%d,%d) 3dlayers=%u\n", g_ps1ProfLayer[0], g_ps1ProfLayer[1], g_ps1ProfLayer[2], g_ps1ProfLayer[4], g_ps1TileBandCount, g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl, g_ps1InkPieces, g_ps1ClutFades, fadeMode, fadeA, g_ps1BankRegions, g_ps1BankMask, waterDrawPos, g_ps1Faces, g_ps1FaceTexMiss, g_ps1FaceMissSheet, g_ps1FaceMissU0, g_ps1FaceMissV0, g_ps1FaceMissU1, g_ps1FaceMissV1, g_ps1Draw3DLayer
GDBEOF
done
echo detach
} > "$OUT.gdb"
# Other sessions share this emulator (GDB port 3333): never kill theirs, only our own PID.
pgrep -x PCSX-Redux >/dev/null && { echo "PCSX-Redux is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(FRAME|LAYERS|ACTIVE|SLOTS|MISS) .*"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
