#!/bin/bash
# Per-object-type CPU profile of a `make BOOT=<folder>` build (headless): holds Right from frame 240 and dumps the
# last frame's per-type ticks (root counter 2, sysclock/8 = 0.24 us; Object.cpp g_ps1TypeTicks / DrawCalls) at
# frames F (default 420 600), then prints the top types by name (GameConfig + StageConfig order).
# Build with `make BOOT=<folder> PROFILE=1` (VM instruction counts; without it the ops column is 0).
# Usage: tools/obj_profile.sh FOLDER [OUT_PREFIX] [F1 F2]
cd "$(dirname "$0")/.."
STAGE=$1; OUT=${2:-/tmp/rsdkv4_prof}; F1=${3:-420}; F2=${4:-600}
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv4.elf
break PS1PerfTick if g_ps1PerfFrames == 240 || g_ps1PerfFrames == $F1 || g_ps1PerfFrames == $F2
commands
silent
if g_ps1PerfFrames == 240
  set var g_ps1PadTestMask = 0x08
else
  printf "FRAME %u lastHbl=%u objects=%u drawSubs=%u\n", g_ps1PerfFrames, g_ps1PerfLastHbl, g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl
  eval "dump binary memory ${OUT}_f%u_upd.bin &g_ps1TypeTicks[0] &g_ps1TypeTicks[256]", g_ps1PerfFrames
  eval "dump binary memory ${OUT}_f%u_updn.bin &g_ps1TypeCalls[0] &g_ps1TypeCalls[256]", g_ps1PerfFrames
  eval "dump binary memory ${OUT}_f%u_ops.bin &g_ps1TypeOps[0] &g_ps1TypeOps[256]", g_ps1PerfFrames
  eval "dump binary memory ${OUT}_f%u_drw.bin &g_ps1TypeDrawTicks[0] &g_ps1TypeDrawTicks[256]", g_ps1PerfFrames
  eval "dump binary memory ${OUT}_f%u_drwn.bin &g_ps1TypeDrawCalls[0] &g_ps1TypeDrawCalls[256]", g_ps1PerfFrames
end
if g_ps1PerfFrames == $F2
  detach
else
  continue
end
end
continue
GDBEOF
# Other sessions share this emulator (GDB port 3333): never kill theirs, only our own PID.
pgrep -x PCSX-Redux >/dev/null && { echo "PCSX-Redux is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -iso build/game.cue -exe rsdkv4.ps-exe \
      -stdout -logfile "$OUT.log" >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "FRAME .*" | tee "$OUT.txt"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
/usr/bin/python3 - "$STAGE" "$OUT" $F1 $F2 <<'PYEOF'
import os, struct, sys
sys.path.insert(0, 'tools/atlas')
import object_sheets as o
stage, out = sys.argv[1], sys.argv[2]
D = '../Sonic2_Extracted/Data'
g, _, sn = o.stage_config(D, stage)
names = ['blank'] + (o.game_config(D)[0] if g else []) + sn
TPH = 33868800 / 8 / 15734
for f in sys.argv[3:]:
    p = '%s_f%s' % (out, f)
    if not os.path.exists(p + '_upd.bin'):
        continue
    rd = lambda s: struct.unpack('<256I', open(p + s, 'rb').read())
    up, un, dr, dn, ops = rd('_upd.bin'), rd('_updn.bin'), rd('_drw.bin'), rd('_drwn.bin'), rd('_ops.bin')
    rows = sorted(((up[t] + dr[t], t) for t in range(256) if up[t] + dr[t]), reverse=True)
    print('frame %s: update %.1f hbl, draw %.1f hbl (%d / %d calls)' % (f, sum(up) / TPH, sum(dr) / TPH, sum(un), sum(dn)))
    for tot, t in rows[:15]:
        print('  %-20s upd %6.1f hbl x%-3d %6d ops (%4d cyc/op)  draw %6.1f hbl x%-3d' % (names[t] if t < len(names) else t, up[t] / TPH, un[t],
              ops[t], up[t] * 8 / ops[t] if ops[t] else 0, dr[t] / TPH, dn[t]))
PYEOF
