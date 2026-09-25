#!/bin/bash
# Phase 2 headless boot test in PCSX-Redux (background + timeouts, emulator killed at the end).
# GDB (breakpoints set before the first continue; PCSX started without -run):
#   LOAD  every outermost load (ps1/loading PS1LoadingDoneHook): time, vblanks since GPU init, CD
#         sectors, stage now set. `us` (psyqo GPU::now) only advances while callbacks are pumped:
#         CPU-bound work without CD waits under-counts it; the vblank count (IRQ) is exact.
#   FRAME at frame counts F1 F2 (PS1PerfTick): stage, engine mode, perf, heap, VM/array guards
# Needs rsdkv4.ps-exe + build/game.cue (tools/data_disc.sh).
# Usage: tools/boot_test.sh [OUT_PREFIX] [F1=60] [F2=600]   (frame 1 = the first stage load is also sampled)
# Frame CPU time is in hblanks (16-bit root counter 1: a frame longer than ~4 s wraps; use LOAD us).
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/rsdkv4_boot}; F1=${2:-60}; F2=${3:-600}
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv4.elf
break PS1LoadingDoneHook
commands
silent
printf "LOAD us=%u vbl=%u sectors=%u init=%u list=%d pos=%d folder=%s stageMode=%d\n", us, g_ps1GPU->m_frameCount, g_ps1FsSectors, g_ps1InitDone, activeStageList, stageListPosition, stageList[activeStageList][stageListPosition].folder, stageMode
continue
end
break PS1PerfTick if g_ps1PerfFrames == 1 || g_ps1PerfFrames == $F1 || g_ps1PerfFrames == $F2
commands
silent
if g_ps1PerfFrames == 1
  dump binary memory ${OUT}_atan.bin &arcTan256LookupTable[0] &arcTan256LookupTable[0x10000]
end
printf "FRAME n=%u vsyncs=%u lastHbl=%u maxHbl=%u overHbl=%u | mode=%d list=%d pos=%d folder=%s stageMode=%d\n", g_ps1PerfFrames, g_ps1PerfVsyncs, g_ps1PerfLastHbl, g_ps1PerfMaxHbl, g_ps1PerfOverHbl, Engine.gameMode, activeStageList, stageListPosition, stageList[activeStageList][stageListPosition].folder, stageMode
printf "STATE scriptFrames=%d aniFiles=%d sfx=%d+%d bytecode=%s mode=%d | heap total=%u headroom=%u\n", scriptFrameCount, animationFileCount, globalSFXCount, stageSFXCount, Engine.usingBytecode ? "yes" : "no", Engine.bytecodeMode, g_ps1HeapTotal, g_ps1HeapHeadroom
printf "CHAIN arenaFail=%u suspends=%u high=%u overflow=%u fallbackReads=%u\n", g_ps1ChainArenaFail, g_ps1ChainSuspends, g_ps1ChainHighWater, g_ps1ChainOverflow, g_ps1FsFallbackReads
printf "PROF objects=%u drawSubs=%u | 3D transform=%u sort=%u draw=%u (hblanks, last frame / last Draw3DScene) vertices=%d faces=%d\n", g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl, g_ps1Prof3DTransformHbl, g_ps1Prof3DSortHbl, g_ps1Prof3DDrawHbl, vertexCount, faceCount
printf "GUARDS scriptOverflow=%u badPtr=%u divZero=%u badOpcode=%u rowsClamped=%u stageListClamped=%u scene3D overflow=%u maxVertex=%d maxFace=%d\n", g_ps1ScriptOverflow, g_ps1ScriptBadPtr, g_ps1ScriptDivZero, g_ps1ScriptBadOpcode, g_ps1LayerRowsClamped, g_ps1StageListClamped, g_ps1Scene3DOverflow, g_ps1Scene3DMaxVertex, g_ps1Scene3DMaxFace
if g_ps1PerfFrames == $F2
  detach
else
  continue
end
end
continue
GDBEOF
pkill -f PCSX-Redux 2>/dev/null; sleep 1
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -iso build/game.cue -exe rsdkv4.ps-exe \
      -stdout -logfile "$OUT.log" >/dev/null 2>&1 &
sleep 4
timeout ${TIMEOUT:-240} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(LOAD|FRAME|STATE|PROF|GUARDS|CHAIN) .*|rror.*" | tee "$OUT.txt"
pkill -f PCSX-Redux
