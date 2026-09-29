#!/bin/bash
# Load-time profile (docs/30 phase 9.1), from the Sonic CD port's tool: one emulator session over GDB logs
#   REQ   every file-system CD request (PS1FsProfileHook: a blocking read, or one load-stream session = one seek)
#   OPEN  every fopen, LOAD every outermost load end (PS1LoadingDoneHook), GOTO every stage switch below,
# all stamped with the vblank count (exact; psyqo's `us` under-counts CPU-bound work), then tools/load_profile.py
# prints a table per load: vblanks, requests, stream starts, sectors, heap headroom, and where the time went.
# SCHEDULE="frame:list:pos ...": at that game frame set the stage (list 1 = regular, 3 = special) and start a load
# (stageMode = LOAD; the same stage again = a restart: only its act reloads). Default: the phase 9 baseline on a
# BOOT=Zone01 build (Zone01 act 2, a restart, every other zone's first act, Special, back to Zone01).
# PRE="gdb commands": before the game runs (e.g. 'set var g_ps1FsFastLoad = 0' for an A/B).
# Usage: make BOOT=Zone01 && make disc && tools/load_profile.sh [OUT_PREFIX] [STOP_FRAME]
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/rsdkv4_load}
DEFAULT="60:1:1 100:1:1 140:1:2 180:1:4 220:1:6 260:1:8 300:1:10 340:1:12 380:1:14 420:1:17 460:1:18 500:1:19 540:1:20 580:3:0 620:1:0"
SCHEDULE=${SCHEDULE:-$DEFAULT}
LAST=$(echo $SCHEDULE | awk '{print $NF}' | cut -d: -f1)
STOP=${2:-$((LAST + 40))}
. tools/redux_env.sh
{
cat <<'GDBEOF'
set pagination off
set confirm off
GDBEOF
echo "target remote localhost:$REDUX_PORT"
cat <<'GDBEOF'
symbol-file rsdkv4.elf
@PRE@
break PS1FsProfileHook
commands
silent
printf "REQ lba=%u n=%u us=%u vbl=%u\n", lba, count, us, g_ps1GPU->m_frameCount
continue
end
break fopen
commands
silent
printf "OPEN vbl=%u %s\n", g_ps1GPU->m_frameCount, path
continue
end
break PS1LoadingDoneHook
commands
silent
printf "LOAD vbl=%u us=%u frame=%u list=%d pos=%d folder=%s requests=%u sectors=%u streams=%u headroom=%u\n", g_ps1GPU->m_frameCount, us, g_ps1PerfFrames, activeStageList, stageListPosition, stageList[activeStageList][stageListPosition].folder, g_ps1FsRequests, g_ps1FsSectors, g_ps1FsStreamStarts, g_ps1LoadingHeadroom
continue
end
GDBEOF
for s in $SCHEDULE; do
  F=${s%%:*}; r=${s#*:}; L=${r%%:*}; P=${r#*:}
  cat <<GDBEOF
break PS1PerfTick if g_ps1PerfFrames == $F
commands
silent
set var activeStageList = $L
set var stageListPosition = $P
set var stageMode = 0
printf "GOTO vbl=%u frame=%u list=$L pos=$P requests=%u streams=%u\n", g_ps1GPU->m_frameCount, g_ps1PerfFrames, g_ps1FsRequests, g_ps1FsStreamStarts
continue
end
GDBEOF
done
cat <<GDBEOF
break PS1PerfTick if g_ps1PerfFrames == $STOP
commands
silent
printf "DONE vbl=%u frame=%u requests=%u sectors=%u streams=%u stale=%u skipped=%u errors=%u fallback=%u\n", g_ps1GPU->m_frameCount, g_ps1PerfFrames, g_ps1FsRequests, g_ps1FsSectors, g_ps1FsStreamStarts, g_ps1FsStreamStale, g_ps1FsStreamSkipped, g_ps1FsStreamErrors, g_ps1FsFallbackReads
printf "GUARDS scriptOverflow=%u badPtr=%u entityOOR=%u scene3D=%u s3dFail=%u arenaFail=%u\n", g_ps1ScriptOverflow, g_ps1ScriptBadPtr, g_ps1EntityOutOfRange, g_ps1Scene3DOverflow, g_ps1Scene3DAllocFail, g_ps1ChainArenaFail
detach
end
continue
GDBEOF
} | sed "s|@PRE@|${PRE:-}|" > "$OUT.gdb"
# Other sessions share the emulator (tools/redux_env.sh): never kill theirs, only our own PID.
redux_busy && { echo "PCSX-Redux (GDB $REDUX_PORT) is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb $REDUX_ARGS -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-1800} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -E "^(REQ|OPEN|LOAD|GOTO|DONE|GUARDS) " > "$OUT.log"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
/usr/bin/python3 tools/load_profile.py "$OUT.log" build/game.bin | tee "$OUT.txt"
