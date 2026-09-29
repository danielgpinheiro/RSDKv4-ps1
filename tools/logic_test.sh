#!/bin/bash
# Phase 2.3 gameplay-logic test (headless), from the Sonic CD port's harness: boots a `make BOOT=<folder>
# [BOOT_PLAYER=n]` build, drives the player with g_ps1PadTestMask (bit = INPUT_* index: 0 up 1 down 2 left
# 3 right 4 A 5 B 6 C ... 12 start) and samples player 1 (entity slot 0: v4 players are script objects),
# the stage timer, perf, heap and guards at fixed frames (PS1PerfTick). Entity types are dumped at frame S0
# (build/..._ents.bin) and counted.
#   S0  settle (title card)        -> hold Right
#   S1  running                    -> Right + A (jump)
#   S2  airborne                   -> release all
#   S3  landed / stopped           -> end
# The guard counters at S3 are also dumped to OUT_guards.bin (12 u32, the GUARDS order without copyTile / nested):
# GDB printf output is sometimes lost, a dump is not (tools/zone_sweep.sh reads it). OUT_audio.bin: 6 u32, the SFX
# key-ons, PlaySfx of unloaded ids, bank errors, no free channel, frames of silent music, banks uploaded.
# Usage: make BOOT=Zone01 && tools/logic_test.sh [OUT_PREFIX] [S0=240] [S1=420] [S2=432] [S3=600]
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/rsdkv4_logic}; S0=${2:-240}; S1=${3:-420}; S2=${4:-432}; S3=${5:-600}
# HOLD_RIGHT=1: hold Right from S0 to the end, no jump (the obj_profile.sh input: runs into enemies in Emerald Hill).
M1=0x18; M2=0
# INJECT_S0="gdb commands": run at frame S0 (tests: e.g. move the player next to an object whose branch must run).
INJECT_S0=${INJECT_S0:-}
# INJECT_AT=frame + INJECT="gdb commands": a second injection at that frame (e.g. onto an object once it has moved).
# PRE="gdb commands": before the game runs (e.g. 'set var g_ps1FsFastLoad = 0': a .data switch, crt0 keeps it).
INJECT_AT=${INJECT_AT:-0}; INJECT=${INJECT:-}
# AT_END="gdb commands": run at frame S3 after the dumps (e.g. print a test build's branch counters).
AT_END=${AT_END:-}
if [ "${HOLD_RIGHT:-0}" = 1 ]; then M1=0x08; M2=0x08; fi
. tools/redux_env.sh
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:${REDUX_PORT}
symbol-file rsdkv4.elf
${PRE:-}
break PS1LoadingDoneHook
commands
silent
printf "LOAD vbl=%u sectors=%u list=%d pos=%d folder=%s player=%d\n", g_ps1GPU->m_frameCount, g_ps1FsSectors, activeStageList, stageListPosition, stageList[activeStageList][stageListPosition].folder, playerListPos
continue
end
break PS1PerfTick if g_ps1PerfFrames == 1 || g_ps1PerfFrames == $INJECT_AT || g_ps1PerfFrames == $S0 || g_ps1PerfFrames == $S1 || g_ps1PerfFrames == $S2 || g_ps1PerfFrames == $S3
commands
silent
printf "DRAW F%u hash=%08x sprites=%u\n", g_ps1PerfFrames, g_ps1DrawHashLast, g_ps1DrawCountLast
printf "F%u vbl=%u mask=%04x P0 type=%d x=%d y=%d xv=%d yv=%d speed=%d gravity=%d ctrl=%d | P1 type=%d | time %d:%02d.%02d enabled=%d stageMode=%d | lastHbl=%u maxHbl=%u objects=%u drawSubs=%u\n", g_ps1PerfFrames, g_ps1GPU->m_frameCount, g_ps1PadButtons, objectEntityList[0].type, objectEntityList[0].xpos >> 16, objectEntityList[0].ypos >> 16, objectEntityList[0].xvel, objectEntityList[0].yvel, objectEntityList[0].speed, objectEntityList[0].gravity, objectEntityList[0].controlMode, objectEntityList[1].type, stageMinutes, stageSeconds, stageMilliseconds, timeEnabled, stageMode, g_ps1PerfLastHbl, g_ps1PerfMaxHbl, g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl
if g_ps1PerfFrames == $S0
  printf "ENTSIZE %d\n", sizeof(Entity)
  dump binary memory ${OUT}_ents.bin &objectEntityList[0] &objectEntityList[0x380]
  dump binary memory ${OUT}_tile.bin &tiles128x128.ps1Tile[0] &tiles128x128.ps1Tile[0x8000]
  dump binary memory ${OUT}_tcol.bin &tiles128x128.ps1Collision[0] &tiles128x128.ps1Collision[0x8000]
  dump binary memory ${OUT}_code.bin &scriptCode.slot[0] &scriptCode.slot[scriptCodePos]
  dump binary memory ${OUT}_big.bin &'Script.cpp'::s_ps1ScriptBig[0] &'Script.cpp'::s_ps1ScriptBig['Script.cpp'::s_ps1ScriptBigCount]
  set var g_ps1PadTestMask = 0x08
  $INJECT_S0
end
if g_ps1PerfFrames == $INJECT_AT
  $INJECT
end
if g_ps1PerfFrames == $S1
  set var g_ps1PadTestMask = $M1
end
if g_ps1PerfFrames == $S2
  set var g_ps1PadTestMask = $M2
end
if g_ps1PerfFrames == $S3
  dump binary memory ${OUT}_ents_end.bin &objectEntityList[0] &objectEntityList[0x380]
  dump binary memory ${OUT}_code_end.bin &scriptCode.slot[0] &scriptCode.slot[scriptCodePos]
  dump binary memory ${OUT}_glob_end.bin &globalVariables[0] &globalVariables[256]
  printf "STATE scriptCodePos=%d jumpPos=%d big=%u scriptFrames=%d aniFrames=%d anims=%d | heap total=%u headroom=%u | 3D maxVertex=%d maxFace=%d\n", scriptCodePos, jumpTablePos, g_ps1ScriptBigCount, scriptFrameCount, animFrameCount, animationCount, g_ps1HeapTotal, g_ps1HeapHeadroom, g_ps1Scene3DMaxVertex, g_ps1Scene3DMaxFace
  printf "GUARDS scriptOverflow=%u badPtr=%u divZero=%u badOpcode=%u entityOOR=%u rowsClamped=%u lineScrollClamped=%u textMenuClamped=%u aniOverflow=%u pause=%u copyTile=%u scene3D=%u nested=%u saveRAMClamped=%u\n", g_ps1ScriptOverflow, g_ps1ScriptBadPtr, g_ps1ScriptDivZero, g_ps1ScriptBadOpcode, g_ps1EntityOutOfRange, g_ps1LayerRowsClamped, g_ps1LineScrollClamped, g_ps1TextMenuClamped, g_ps1AnimFrameOverflow, g_ps1PauseRequests, g_ps1Copy16x16Tile, g_ps1Scene3DOverflow, g_ps1ScriptNestedCalls, g_ps1SaveRAMClamped
  printf "OOR last index=%d max=%d from slot=%d type=%d\n", g_ps1EntityOORIndex, g_ps1EntityOORMax, g_ps1EntityOORFrom, g_ps1EntityOORType
  $AT_END
  dump binary value ${OUT}_guards.bin g_ps1ScriptOverflow
  append binary value ${OUT}_guards.bin g_ps1ScriptBadPtr
  append binary value ${OUT}_guards.bin g_ps1ScriptDivZero
  append binary value ${OUT}_guards.bin g_ps1ScriptBadOpcode
  append binary value ${OUT}_guards.bin g_ps1EntityOutOfRange
  append binary value ${OUT}_guards.bin g_ps1LayerRowsClamped
  append binary value ${OUT}_guards.bin g_ps1LineScrollClamped
  append binary value ${OUT}_guards.bin g_ps1TextMenuClamped
  append binary value ${OUT}_guards.bin g_ps1AnimFrameOverflow
  append binary value ${OUT}_guards.bin g_ps1PauseRequests
  append binary value ${OUT}_guards.bin g_ps1Scene3DOverflow
  append binary value ${OUT}_guards.bin g_ps1SaveRAMClamped
  dump binary value ${OUT}_audio.bin g_ps1SfxPlays
  append binary value ${OUT}_audio.bin g_ps1SfxUnloaded
  append binary value ${OUT}_audio.bin g_ps1SfxBankErrors
  append binary value ${OUT}_audio.bin g_ps1SfxNoChannel
  append binary value ${OUT}_audio.bin g_ps1MusicSilentFrames
  append binary value ${OUT}_audio.bin g_ps1SfxBanks
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
timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(LOAD|F[0-9]+|GUARDS|STATE|ENTSIZE|OOR|DRAW) .*" | tee "$OUT.txt"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
ES=$(grep -oE "ENTSIZE [0-9]+" "$OUT.txt" | awk '{print $2}')
[ -f "${OUT}_ents.bin" ] && [ -n "$ES" ] && /usr/bin/python3 - "${OUT}_ents.bin" "$ES" <<'PYEOF'
import sys, collections
d, es = open(sys.argv[1], 'rb').read(), int(sys.argv[2])
off = 5 * 4 + 48 * 4 + 9 * 4 + 2  # RSDKv4/Object.hpp Entity: 5 ints, values[48], 9 ints, ushort groupID, byte type
types = [d[i * es + off] for i in range(len(d) // es)]
c = collections.Counter(t for t in types if t)
print('ENTITIES %d active (of %d), %d types; most common: %s' % (sum(c.values()), len(types), len(c),
      ', '.join('%d x%d' % kv for kv in c.most_common(8))))
PYEOF
