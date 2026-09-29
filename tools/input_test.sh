#!/bin/bash
# Phase 4.1: every pad-1 button through the real controller path (tools/pad.lua: SIO0 override -> psyqo
# AdvancedPad -> ReadInputDevice) lands on its engine input (g_ps1PadButtons, bit = INPUT_* index) and nothing else.
# Mapping (RSDKv4/Input.cpp): Up Down Left Right, Cross = A, Circle = B, Square = C, Triangle = X, L2 = Y, R2 = Z,
# L1 = L, R1 = R, Start, Select. Uses the current rsdkv4.ps-exe (any build) and build/game.cue.
# Usage: tools/input_test.sh [OUT_PREFIX] [FIRST_FRAME=200]
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/rsdkv4_input}; F=${2:-200}
. tools/redux_env.sh
BUTTONS="Up Down Left Right Cross Circle Square Triangle L2 R2 L1 R1 Start Select"
{
cat <<GDBEOF
set pagination off
set confirm off
target remote localhost:${REDUX_PORT}
symbol-file rsdkv4.elf
GDBEOF
k=0
for b in $BUTTONS; do
  f0=$((F + k * 6)); f1=$((f0 + 2)); f2=$((f0 + 4))
cat <<GDBEOF
break PS1PerfTick if g_ps1PerfFrames == $f0
continue
delete
shell curl -s 'http://localhost:${REDUX_WEB}/api/v1/lua/pad?press=$b' >/dev/null
break PS1PerfTick if g_ps1PerfFrames == $f1
continue
delete
printf "BUTTON $b expect=%d buttons=0x%04x connected=%d\\n", $k, g_ps1PadButtons, g_ps1PadConnected
shell curl -s 'http://localhost:${REDUX_WEB}/api/v1/lua/pad?release=$b' >/dev/null
break PS1PerfTick if g_ps1PerfFrames == $f2
continue
delete
printf "RELEASED $b buttons=0x%04x\\n", g_ps1PadButtons
GDBEOF
  k=$((k + 1))
done
echo detach
} > "$OUT.gdb"
# Other sessions share the emulator (tools/redux_env.sh): never kill theirs, only our own PID.
redux_busy && { echo "PCSX-Redux (GDB $REDUX_PORT) is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb $REDUX_ARGS -webserver -exec "dofile('$PWD/tools/pad.lua')" \
      -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "(BUTTON|RELEASED) .*" | tee "$OUT.txt"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
/usr/bin/python3 - "$OUT.txt" <<'PYEOF'
import re, sys
ok, n = True, 0
for l in open(sys.argv[1]):
    m = re.match(r'BUTTON (\S+) expect=(\d+) buttons=0x([0-9a-f]+) connected=(\d)', l)
    if m:
        n += 1
        good = int(m[3], 16) == 1 << int(m[2]) and m[4] == '1'
        ok &= good
        print('%-8s -> input %2d: %s' % (m[1], int(m[2]), 'OK' if good else 'WRONG (0x%s)' % m[3]))
    m = re.match(r'RELEASED (\S+) buttons=0x([0-9a-f]+)', l)
    if m and int(m[2], 16):
        ok = False
        print('%-8s not released (0x%s)' % (m[1], m[2]))
print('%d buttons, %s' % (n, 'all mapped and released' if ok and n == 14 else 'FAIL'))
PYEOF
