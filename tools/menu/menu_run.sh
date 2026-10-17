#!/bin/bash
# Phase 4 menu runs: boots the current rsdkv4.ps-exe, sets g_ps1PadTestMask at chosen frames and captures chosen frames
# (both framebuffers, tools/vramshot.py) with the menu / engine state.
# Usage: [GDBEXTRA=FILE] tools/menu/menu_run.sh "F:MASK,F:MASK,..." "F,F,..." OUT_PREFIX
#   masks: bit = INPUT_* (Up 0x1, Down 0x2, Left 0x4, Right 0x8, A 0x10, B 0x20, C 0x40, X 0x80, Start 0x1000)
cd "$(dirname "$0")/../.."
EVENTS=$1; CAPS=$2; OUT=$3
. tools/redux_env.sh
# GDBEXTRA=FILE: lines "FRAME gdb-command" run at that frame (after the pad mask), e.g. saveRAM / global injections.
EXTRA=${GDBEXTRA:-/dev/null}
ALL=$( (echo "$EVENTS" | tr ',' '\n' | sed 's/:.*//'; echo "$CAPS" | tr ',' '\n'; sed 's/ .*//' "$EXTRA") | grep -v '^$' | sort -n -u)
{
cat <<GDBEOF
set pagination off
set confirm off
target remote localhost:${REDUX_PORT}
symbol-file rsdkv4.elf
GDBEOF
for F in $ALL; do
  echo "break PS1PerfTick if g_ps1PerfFrames == $F"
  echo "continue"
  echo "delete"
  M=$(echo "$EVENTS" | tr ',' '\n' | grep "^$F:" | sed 's/.*://')
  [ -n "$M" ] && echo "set var g_ps1PadTestMask = $M"
  [ -n "$GDBSET" ] && [ "$F" = "${GDBAT:-0}" ] && echo "$GDBSET"
  grep "^$F " "$EXTRA" | sed 's/^[0-9]* //'
  if echo ",$CAPS," | grep -q ",$F,"; then
    echo "shell /usr/bin/python3 tools/vramshot.py ${OUT}_$F >/dev/null"
    echo "printf \"STATE $F screen=%u sel=%u gameMode=%d stageMode=%d folder=%s list=%d pos=%d player=%d p2=%d gm=%d slot=%d lives=%d star=%d pause=%u/%u/%u vol=%d/%d ta=%u/%u/%u | overflow=%u miss=%u\\n\", g_ps1MenuScreen, g_ps1MenuSel, Engine.gameMode, stageMode, stageList[activeStageList][stageListPosition].folder, activeStageList, stageListPosition, playerListPos, globalVariables[50], globalVariables[0], globalVariables[2], globalVariables[25], globalVariables[38], g_ps1PauseRequests, g_ps1PauseSel, g_ps1PauseDialog, saveRAM[33], saveRAM[34], g_ps1TAZone, g_ps1TAAct, g_ps1TARank, g_ps1ChainOverflow, g_ps1AtlasDrawMiss"
  fi
done
echo detach
} > "$OUT.gdb"
# Other sessions share the emulator (tools/redux_env.sh): never kill theirs, only our own PID.
redux_busy && { echo "PCSX-Redux (GDB $REDUX_PORT) is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb $REDUX_ARGS -webserver -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-400} ${GDB:-tools/gdbw} --batch -x "$OUT.gdb" 2>&1 | grep -oE "STATE .*|rror.*"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
