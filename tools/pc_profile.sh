#!/bin/bash
# CPU profile by PC sampling (`make BOOT=<folder> SAMPLER=1`, ps1/pc_sampler.cpp): holds MASK (default 8 = Right) from
# frame 240, records the interrupted PC from frame START to END (root counter 0 IRQ at random intervals), then prints
# the share of samples per function (nm) and, with LINES=1, the hottest source lines (addr2line).
# Usage: tools/pc_profile.sh OUT [START END]   (MASK=, FROM= as tools/frame_capture.sh)
# CHASE=1: tools/perf_scene.sh's input instead (Right from 240, Right + A from 420 to 432, then Right): profile the frames
# perf_scene.sh times. The ELF is copied to OUT.elf with the samples and the report maps through that copy, so a report
# re-run after a rebuild still reads the right symbols (docs/32 9f lesson).
cd "$(dirname "$0")/.."
OUT=$1; START=${2:-300}; END=${3:-420}
. tools/redux_env.sh
cp rsdkv4.elf "$OUT.elf"
CHASE_STEPS=
if [ "${CHASE:-0}" = 1 ]; then
    MASK=8; FROM=240
    [ "$START" -lt 420 ] && CHASE_STEPS="break PS1PerfTick if g_ps1PerfFrames == 420
continue
set var g_ps1PadTestMask = 0x18
delete
break PS1PerfTick if g_ps1PerfFrames == 432
continue
set var g_ps1PadTestMask = 0x08
delete"
fi
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:${REDUX_PORT}
symbol-file $OUT.elf
break PS1PerfTick if g_ps1PerfFrames == ${FROM:-240}
continue
set var g_ps1PadTestMask = ${MASK:-8}
delete
break PS1PerfTick if g_ps1PerfFrames == $START
continue
set var g_ps1PcSampleOn = 1
delete
${CHASE_STEPS:-}
break PS1PerfTick if g_ps1PerfFrames == $END
continue
set var g_ps1PcSampleOn = 0
printf "SAMPLES %u frames $START-$END lastHbl=%u\n", g_ps1PcSampleCount, g_ps1PerfLastHbl
eval "dump binary memory ${OUT}_pcs.bin &g_ps1PcSamples[0] &g_ps1PcSamples[%u]", g_ps1PcSampleCount
detach
GDBEOF
redux_busy && { echo "PCSX-Redux (GDB $REDUX_PORT) is already running (another session?): not starting it" >&2; exit 2; }
$PCSX -fastboot -interpreter -debugger -gdb $REDUX_ARGS -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
EMU=$!
sleep 4
timeout ${TIMEOUT:-600} /opt/homebrew/bin/gdb --batch -x "$OUT.gdb" 2>&1 | grep -oE "SAMPLES .*" | tee "$OUT.txt"
kill $EMU 2>/dev/null; wait $EMU 2>/dev/null
/usr/bin/python3 - "$OUT" <<'PYEOF' | tee -a "$OUT.txt"
import bisect, collections, os, struct, subprocess, sys
out = sys.argv[1]
d = open(out + '_pcs.bin', 'rb').read()
pcs = struct.unpack('<%dI' % (len(d) // 4), d)
syms = []
for line in subprocess.run(['mipsel-none-elf-nm', '-n', '-C', out + '.elf'], capture_output=True, text=True).stdout.splitlines():
    p = line.split(' ', 2)
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
h = collections.Counter()
for pc in pcs:
    i = bisect.bisect_right(addrs, pc) - 1
    h[syms[i][1] if i >= 0 else '%08x' % pc] += 1
print('%d samples' % len(pcs))
for name, c in h.most_common(int(os.environ.get('TOP', 40))):
    print('%5.1f%% %5d  %s' % (100.0 * c / len(pcs), c, name[:120]))
if os.environ.get('LINES') == '1':
    lines = collections.Counter(pcs)
    top = [pc for pc, _ in lines.most_common(int(os.environ.get('NLINES', 40)))]
    a2l = subprocess.run(['mipsel-none-elf-addr2line', '-e', out + '.elf', '-f', '-C'] + ['%x' % p for p in top],
                         capture_output=True, text=True).stdout.splitlines()
    print('hottest PCs:')
    for k, pc in enumerate(top):
        fn, where = (a2l[2 * k], a2l[2 * k + 1]) if 2 * k + 1 < len(a2l) else ('?', '?')
        print('%5d  %08x  %s  %s' % (lines[pc], pc, where.split('/')[-1], fn[:60]))
PYEOF
