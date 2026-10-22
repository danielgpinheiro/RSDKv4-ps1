#!/bin/bash
# The special stage entered the way a player gets there: natural build (rsdkv4.ps-exe + build/game.cue), the menus ->
# a no-save game -> the first zone, GDB loads regular stage POS at frame 2200 (-1: stay in the first zone) and then
# special stage SPECIAL at 2800 (as the scripts' warp does: activeStageList / stageListPosition / STAGEMODE_LOAD).
# The stage loads before it shape the heap: a BOOT=Special build starts on a clean one and can pass while this fails
# (2026-10-10: the chain arenas found no room after Emerald Hill, nothing drawn). Prints the state at frame 3300 and
# exits 1 if a frame had no chain arena (g_ps1ChainArenaFail) or the stage is not the special stage.
# PLAYER=n (playerListPos: 0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails) is set with POS's load (POS >= 0), so that
# zone and the special stage load that character's atlases (V4-119: Knuckles, Emerald Hill 2 -> special stage 4).
# Usage: [GAME=1] [PLAYER=n] tools/special_entry_test.sh OUT [POS] [SPECIAL]
cd "$(dirname "$0")/.."
OUT=$1; POS=${2:--1}; SP=${3:-0}
P='printf "STATE SE F%u folder=%s list=%d player=%d arenaFail=%u s3dFail=%u headroom=%u\\n", g_ps1PerfFrames, stageList[activeStageList][stageListPosition].folder, activeStageList, playerListPos, g_ps1ChainArenaFail, g_ps1Scene3DAllocFail, g_ps1HeapHeadroom'
{
  if [ "$POS" -ge 0 ]; then
    echo "2200 set var activeStageList = 1"; echo "2200 set var stageListPosition = $POS"; echo "2200 set var stageMode = 0"
    [ -n "$PLAYER" ] && echo "2200 set var playerListPos = $PLAYER"
  fi
  echo "2800 set var activeStageList = 3"; echo "2800 set var stageListPosition = $SP"; echo "2800 set var stageMode = 0"
  echo "3300 $P"
} > "$OUT.extra"
R=$(GDBEXTRA="$OUT.extra" TIMEOUT=${TIMEOUT:-900} tools/menu/menu_run.sh \
    "400:4096,410:0,520:16,530:0,640:16,650:0,740:16,750:0,1200:4096,1210:0,1400:4096,1410:0" "3300" "$OUT" 2>&1 |
    grep -o "STATE SE .*")
echo "$R"
echo "$R" | grep -q "folder=Special .*arenaFail=0 s3dFail=0 " || { echo "special_entry_test: FAILED" >&2; exit 1; }
