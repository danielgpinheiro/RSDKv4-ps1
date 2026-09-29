#!/bin/bash
# Phase 2 headless boot test in PCSX-Redux (background + timeouts, emulator killed at the end), from the Sonic CD
# port's harness. GDB (breakpoints set before the first continue; PCSX started without -run):
#   LOAD  every outermost load (ps1/loading PS1LoadingDoneHook): time, vblanks since GPU init, CD sectors, stage
#         now set. `us` (psyqo GPU::now) only advances while callbacks are pumped: CPU-bound work without CD
#         waits under-counts it; the vblank count (IRQ) is exact.
#   FRAME at frame counts 1, F1, F2 (PS1PerfTick): stage, engine mode, perf, heap, VM/array guards.
# Needs rsdkv4.ps-exe + build/game.cue (tools/data_disc.sh).
# Usage: tools/boot_test.sh [OUT_PREFIX] [F1=60] [F2=600]
# Frame CPU time is in hblanks (16-bit root counter 1: a frame longer than ~4 s wraps; use LOAD us).
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/rsdkv4_boot}; F1=${2:-60}; F2=${3:-600}
. tools/redux_env.sh
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:${REDUX_PORT}
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
printf "FRAME n=%u sectors=%u fsReq=%u vbl=%u vsyncs=%u lastHbl=%u maxHbl=%u overHbl=%u | mode=%d list=%d pos=%d folder=%s stageMode=%d player=%d\n", g_ps1PerfFrames, g_ps1FsSectors, g_ps1FsRequests, g_ps1GPU->m_frameCount, g_ps1PerfVsyncs, g_ps1PerfLastHbl, g_ps1PerfMaxHbl, g_ps1PerfOverHbl, Engine.gameMode, activeStageList, stageListPosition, stageList[activeStageList][stageListPosition].folder, stageMode, playerListPos
printf "STATE scriptFrames=%d aniFiles=%d anims=%d aniFrames=%d sfx=%d+%d bytecode=%d scriptCodePos=%d jumpPos=%d big=%u | heap total=%u headroom=%u | scene3D block allocs=%u frees=%u fail=%u\n", scriptFrameCount, animationFileCount, animationCount, animFrameCount, globalSFXCount, stageSFXCount, Engine.usingBytecode, scriptCodePos, jumpTablePos, g_ps1ScriptBigCount, g_ps1HeapTotal, g_ps1HeapHeadroom, g_ps1Scene3DAllocs, g_ps1Scene3DFrees, g_ps1Scene3DAllocFail
printf "PROF objects=%u drawSubs=%u | 3D transform=%u sort=%u draw=%u (hblanks) vertices=%d faces=%d\n", g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl, g_ps1Prof3DTransformHbl, g_ps1Prof3DSortHbl, g_ps1Prof3DDrawHbl, vertexCount, faceCount
printf "GUARDS scriptOverflow=%u badPtr=%u divZero=%u badOpcode=%u entityOOR=%u rowsClamped=%u lineScrollClamped=%u stageListClamped=%u textMenuClamped=%u aniOverflow=%u pause=%u copyTile=%u scene3D overflow=%u maxVertex=%d maxFace=%d\n", g_ps1ScriptOverflow, g_ps1ScriptBadPtr, g_ps1ScriptDivZero, g_ps1ScriptBadOpcode, g_ps1EntityOutOfRange, g_ps1LayerRowsClamped, g_ps1LineScrollClamped, g_ps1StageListClamped, g_ps1TextMenuClamped, g_ps1AnimFrameOverflow, g_ps1PauseRequests, g_ps1Copy16x16Tile, g_ps1Scene3DOverflow, g_ps1Scene3DMaxVertex, g_ps1Scene3DMaxFace
if g_ps1PerfFrames == $F2
  detach
else
  continue
end
end
continue
GDBEOF
# Other sessions share the emulator (tools/redux_env.sh): never kill theirs, only our own PID.
redux_busy && { echo "PCSX-Redux (GDB $REDUX_PORT) is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb $REDUX_ARGS -iso build/game.cue -exe rsdkv4.ps-exe \
      -stdout -logfile "$OUT.log" >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-240} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(LOAD|FRAME|STATE|PROF|GUARDS) .*|rror.*" | tee "$OUT.txt"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
