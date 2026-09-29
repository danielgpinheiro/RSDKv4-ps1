#!/bin/bash
# CPU profile by PC sampling (`make BOOT=<folder> SAMPLER=1`, ps1/pc_sampler.cpp): holds MASK (default 8 = Right) from
# frame 240, records the interrupted PC from frame START to END (root counter 0 IRQ at random intervals), then prints
# the share of samples per function (nm) and, with LINES=1, the hottest source lines (addr2line).
# Usage: tools/pc_profile.sh OUT [START END]   (MASK=, FROM= as tools/frame_capture.sh)
cd "$(dirname "$0")/.."
OUT=$1; START=${2:-300}; END=${3:-420}
. tools/redux_env.sh
cat > "$OUT.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:${REDUX_PORT}
symbol-file rsdkv4.elf
break PS1PerfTick if g_ps1PerfFrames == ${FROM:-240}
continue
set var g_ps1PadTestMask = ${MASK:-8}
delete
break PS1PerfTick if g_ps1PerfFrames == $START
continue
set var g_ps1PcSampleOn = 1
delete
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
for line in subprocess.run(['mipsel-none-elf-nm', '-n', '-C', 'rsdkv4.elf'], capture_output=True, text=True).stdout.splitlines():
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
    top = [pc for pc, _ in lines.most_common(40)]
    a2l = subprocess.run(['mipsel-none-elf-addr2line', '-e', 'rsdkv4.elf', '-f', '-C', '-i'] + ['%x' % p for p in top],
                         capture_output=True, text=True).stdout.splitlines()
    print('hottest PCs:')
    for pc in top:
        print('%5d  %08x' % (lines[pc], pc))
PYEOF
