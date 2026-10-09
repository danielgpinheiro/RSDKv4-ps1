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
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
$HDR
${PRE:-}
break PS1PerfTick if g_ps1PerfFrames == 240
continue
delete
set var g_ps1PerfWinFrom = $FROM
set var g_ps1PerfWinTo = $TO
set var g_ps1PadTestMask = 0x08
break PS1PerfTick if g_ps1PerfFrames == 420
continue
delete
set var g_ps1PadTestMask = $JM
break PS1PerfTick if g_ps1PerfFrames == 432
continue
delete
set var g_ps1PadTestMask = 0x08
break PS1PerfTick if g_ps1PerfFrames == $((TO + 1))
continue
delete
dump binary value ${OUT}_win.bin g_ps1PerfWinFrames
append binary value ${OUT}_win.bin g_ps1PerfWinHbl
append binary value ${OUT}_win.bin g_ps1PerfWinMaxHbl
append binary value ${OUT}_win.bin g_ps1PerfWinVbl
append binary value ${OUT}_win.bin g_ps1PerfWinMaxVbl
append binary value ${OUT}_win.bin g_ps1PerfWinOver
printf "X P0 x=%d y=%d folder=%s headroom=%u\n", objectEntityList[0].xpos >> 16, objectEntityList[0].ypos >> 16, stageList[activeStageList][stageListPosition].folder, g_ps1HeapHeadroom
detach
GDBEOF
rm -f "${OUT}_win.bin"
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
PYEOF
