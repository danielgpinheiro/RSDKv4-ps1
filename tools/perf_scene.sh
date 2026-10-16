#!/bin/bash
# Scene timing over a measured window (docs/30 / docs/37 "Mania lessons pass"): a `make BOOT=<folder> [BOOT_PLAYER=n]`
# build (and, for EMU=ds, its disc: `make disc`) runs the scripted chase -- Right from frame 240, Right + A (jump) from
# 420 to 432, then Right to the end -- and the window counters (RSDKv4/main.cpp g_ps1PerfWin*) sum frames [FROM, TO):
# CPU hblanks a frame (avg / worst; 262 = one NTSC frame), vblanks a frame (avg / worst: 1.00 = 60 fps), frames whose
# CPU time is over 262. The counters go to OUT_win.bin (6 u32; GDB printf lines get lost) and one line to stdout.
# Usage: [EMU=redux|ds] [PRE='gdb commands'] [JUMP=0] tools/perf_scene.sh OUT [FROM=300] [TO=900]
#   JUMP=0: Right only, no jump (Sonic 1's special stage: the jump ends it early; window 300-1500 there).
#   EMU=redux (default): the stock-mode redux16mb instance of tools/redux_env.sh, EXE given with -exe.
#   EMU=ds: DuckStation (tools/emu_ds.sh) booted from a snapshot of build/game.cue + rsdkv4.elf in OUT_ds/; only when no
#   other session runs it (exit 2 otherwise). PRE runs before the game starts: on DuckStation that is before the EXE is
#   read from the disc (a .data switch set there is overwritten), so FROM must be > 240 (the window is set at frame 240).
#   GPU="f1 f2 ...": at these frames dump RAM + the frame's chain (psyqo's head / tail) and price it with
#   tools/gpu_cost.py (docs/43: DuckStation's GPU model) -> OUT_gpu_F.txt, one "GPUCOST" line each.
#   A `make PERF_SPLIT=1` build adds a "SPLIT" line: GPU tail (GPU still drawing the previous chain after the CPU's work;
#   only DuckStation models it), flip gap, and the section averages, hblanks a frame over the window (RSDKv4/main.cpp).
cd "$(dirname "$0")/.."
OUT=$1; FROM=${2:-300}; TO=${3:-900}
EMU=${EMU:-redux}
JM=0x18; [ "${JUMP:-1}" = 0 ] && JM=0x08
ELF=rsdkv4.elf
if [ "$EMU" = ds ]; then
    . tools/emu_ds.sh
    SNAP=${OUT}_ds; ds_busy && { echo "DuckStation is busy (another session): not starting it" >&2; exit 2; }
    ds_snapshot "$SNAP"; ELF=$SNAP/rsdkv4.elf
    HDR="file $ELF
set architecture mips:3000
target remote localhost:2345"
else
    . tools/redux_env.sh
    HDR="target remote localhost:${REDUX_PORT}
symbol-file $ELF"
fi
printf "%s\n%s\n" "$HDR" "${PRE:-}" > "$OUT.hdr"
SPLIT=0; mipsel-none-elf-nm "$ELF" 2>/dev/null | grep -q g_ps1PerfGpuTail && SPLIT=1
# The GDB script: one conditional break per event frame, in order (PS1PerfTick runs at the frame's end, after its chain is
# complete): the window and pad at 240 / 420 / 432, the GPU dumps, the window's counters at TO + 1.
/usr/bin/python3 - "$OUT" "$FROM" "$TO" "$JM" "$SPLIT" "${GPU:-}" > "$OUT.gdb" <<'PYEOF'
import sys
out, frm, to, jm, split, gpu = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5] == '1', sys.argv[6].split()
ev = {}
def add(f, *lines): ev.setdefault(f, []).extend(lines)
add(240, 'set var g_ps1PerfWinFrom = %d' % frm, 'set var g_ps1PerfWinTo = %d' % to, 'set var g_ps1PadTestMask = 0x08')
add(420, 'set var g_ps1PadTestMask = %s' % jm)
add(432, 'set var g_ps1PadTestMask = 0x08')
for f in map(int, gpu):
    add(f, 'dump binary memory %s_gpu_%d.ram 0x80000000 0x80200000' % (out, f),
        'printf "GPUHEAD %u %08x %08x %08x %d\\n", g_ps1PerfFrames, (unsigned)g_ps1GPU->m_chainHead, '
        '(unsigned)g_ps1GPU->m_chainTail, (unsigned)g_ps1GPU->m_chainTailCount, (int)g_ps1GPU->m_chainHeadOversized')
end = ['dump binary value %s_win.bin g_ps1PerfWinFrames' % out] + ['append binary value %s_win.bin %s' % (out, v) for v in
       ('g_ps1PerfWinHbl', 'g_ps1PerfWinMaxHbl', 'g_ps1PerfWinVbl', 'g_ps1PerfWinMaxVbl', 'g_ps1PerfWinOver')]
if split:
    end += ['dump binary value %s_split.bin g_ps1PerfWinTail' % out] + ['append binary value %s_split.bin %s' % (out, v) for v in
            ('g_ps1PerfWinMaxTail', 'g_ps1PerfWinTailFrames', 'g_ps1PerfWinFlip', 'g_ps1PerfWinMaxFlip', 'g_ps1PerfWinSec')]
end.append('printf "X P0 x=%d y=%d folder=%s headroom=%u\\n", objectEntityList[0].xpos >> 16, objectEntityList[0].ypos >> 16, '
           'stageList[activeStageList][stageListPosition].folder, g_ps1HeapHeadroom')
add(to + 1, *end)
print('set pagination off\nset confirm off')
print(open(out + '.hdr').read(), end='')
for f in sorted(ev):
    print('break PS1PerfTick if g_ps1PerfFrames == %d\ncontinue\ndelete' % f)
    print('\n'.join(ev[f]))
print('detach')
PYEOF
rm -f "${OUT}_win.bin" "${OUT}_split.bin"
if [ "$EMU" = ds ]; then
    ds_start "$SNAP"
else
    redux_busy && { echo "PCSX-Redux (GDB $REDUX_PORT) is already running: not starting it" >&2; exit 2; }
    $PCSX -fastboot -interpreter -debugger -gdb $REDUX_ARGS -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
    EMU_PID=$!
    sleep 4
fi
timeout -k 10 ${TIMEOUT:-900} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" > "$OUT.gdblog" 2>&1
if [ "$EMU" = ds ]; then ds_stop; else kill $EMU_PID 2>/dev/null; wait $EMU_PID 2>/dev/null; fi
X=$(grep -oE "X P0 .*" "$OUT.gdblog")
/usr/bin/python3 - "$OUT" "$EMU" "$X" <<'PYEOF' | tee "$OUT.txt"
import os, struct, sys
out, emu, x = sys.argv[1:4]
p = out + '_win.bin'
if not os.path.exists(p) or os.path.getsize(p) != 24:
    print('%s %s: no window dump (see %s.gdblog)' % (os.path.basename(out), emu, out)); sys.exit(1)
n, hbl, mhbl, vbl, mvbl, over = struct.unpack('<6I', open(p, 'rb').read())
n = max(n, 1)
print('%s %s frames=%d cpu avg=%.1f worst=%d | vbl avg=%.3f worst=%d | over262=%d | %s' %
      (os.path.basename(out), emu, n, hbl / n, mhbl, vbl / n, mvbl, over, x))
q = out + '_split.bin'
if os.path.exists(q) and os.path.getsize(q) == 16 * 4:
    v = struct.unpack('<16I', open(q, 'rb').read())
    names = ('objects', 'draw', 'objDraw', 'floors', 'fg', 'perLine', 'tiles', 'strips', '3dT', '3dS', '3dD')
    print('SPLIT tail=%.1f maxTail=%d tailFrames=%d flip=%.1f maxFlip=%d | %s' % (v[0] / n, v[1], v[2], v[3] / n, v[4],
          ' '.join('%s=%.1f' % (k, v[5 + i] / n) for i, k in enumerate(names))))
PYEOF
for c in ${GPU:-}; do
    h=$(grep -E "^GPUHEAD $c " "$OUT.gdblog" | head -1 | awk '{print "0x" $3, "0x" $4, "0x" $5, $6}')
    [ -n "$h" ] && [ -f "${OUT}_gpu_$c.ram" ] || { echo "GPUCOST $c none" | tee -a "$OUT.txt"; continue; }
    /usr/bin/python3 tools/gpu_cost.py "${OUT}_gpu_$c.ram" $h --top 12 > "${OUT}_gpu_$c.txt"
    echo "GPUCOST $c $(grep '^SUM ' "${OUT}_gpu_$c.txt" | cut -c5-)" | tee -a "$OUT.txt"
    rm -f "${OUT}_gpu_$c.ram"
done
