#!/bin/bash
# Phase 0 smoke test in PCSX-Redux (background + timeout): GDB counters of ps1/platform_test.cpp
# checked against the host (file CRC-32s), a VRAM dump during the load (loading icon) and after,
# and a Cross press through tools/pad.lua. Needs tools/smoke_disc.sh first.
cd "$(dirname "$0")/.."
. tools/redux_env.sh
OUT=${1:-/tmp/rsdkv4_smoke}
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:${REDUX_PORT}
symbol-file rsdkv4.elf
break PS1LoadingDebugHook if tick == 3
commands
silent
printf "ICON tick=%u frame=%u\n", tick, g_ps1LoadingFrame
shell python3 tools/vramshot.py ${OUT}_icon
continue
end
break PS1LoadingDoneHook
commands
silent
printf "LOAD us=%u requests=%u sectors=%u\n", us, g_ps1FsRequests, g_ps1FsSectors
continue
end
tbreak PS1ChainEnd if g_ps1PerfFrames == 120
continue
GDBEOF
cat >> "$OUT.gdb" <<'GDBEOF'
printf "RESULT done=%u files=%u spuOk=%u spuBytes=%u frames=%u vsyncs=%u\n", g_ps1TestDone, g_ps1TestFilesOk, g_ps1TestSpuOk, g_ps1TestSpuBytes, g_ps1PerfFrames, g_ps1PerfVsyncs
set $i = 0
while $i < 6
  printf "FILE %d size=%u crc=%08x\n", $i, g_ps1TestSize[$i], g_ps1TestCrc[$i]
  set $i = $i + 1
end
shell curl -s "http://localhost:${REDUX_WEB}/api/v1/lua/pad?press=Cross" >/dev/null
tbreak PS1ChainEnd if g_ps1PerfFrames == 130
continue
printf "PAD pressed=%04x\n", g_ps1TestPad
shell curl -s "http://localhost:${REDUX_WEB}/api/v1/lua/pad?release=Cross" >/dev/null
GDBEOF
echo "shell python3 tools/vramshot.py ${OUT}_f130" >> "$OUT.gdb"
echo "detach" >> "$OUT.gdb"
# Other sessions share the emulator (tools/redux_env.sh): never kill theirs, only our own PID.
redux_busy && { echo "PCSX-Redux (GDB $REDUX_PORT) is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb $REDUX_ARGS -webserver -exec "dofile('$PWD/tools/pad.lua')" \
      -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout 600 ${GDB:-tools/gdbw} --batch -x "$OUT.gdb" 2>&1 | grep -E "^(ICON|LOAD|RESULT|FILE|PAD)" | tee "$OUT.log"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
python3 - "$OUT.log" <<'PYEOF'
import re, sys, zlib
files = ['Data/Game/GameConfig.bin', 'Data/Stages/Zone01/StageConfig.bin', 'Data/Stages/Zone01/Act1.bin',
         'Data/Stages/Zone01/128x128Tiles.bin', 'Data/Stages/Zone01/CollisionMasks.bin', 'Data/Stages/Zone01/Backgrounds.bin']
ok = 0
for l in open(sys.argv[1]):
    m = re.match(r'FILE (\d) size=(\d+) crc=([0-9a-f]+)', l)
    if m:
        d = open('build/iso/' + files[int(m[1])], 'rb').read()
        good = int(m[2]) == len(d) and int(m[3], 16) == zlib.crc32(d) & 0xFFFFFFFF
        ok += good
        print('%-38s %s' % (files[int(m[1])], 'CRC OK' if good else 'MISMATCH'))
print('%d/6 files match the host' % ok)
PYEOF
