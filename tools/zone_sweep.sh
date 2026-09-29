#!/bin/bash
# Phase 2 sweep (docs/37): for each "FOLDER[:PLAYER[:ID]]" builds `make BOOT=FOLDER BOOT_PLAYER=PLAYER [BOOT_ID=ID]` for
# the current GAME and runs tools/logic_test.sh on it (run right, jump, land; guards, heap, frame hblanks), then prints
# one summary line per run. The natural build is rebuilt at the end (make dist / the exports refuse BOOT builds).
# Usage: GAME=1 tools/zone_sweep.sh OUT_DIR Zone01:0 Zone01:1 Zone02 ...
cd "$(dirname "$0")/.."
OUT=$1; shift
mkdir -p "$OUT"
GAME=${GAME:-2}
for spec in "$@"; do
    IFS=: read -r folder player id <<<"$spec"
    player=${player:-0}
    tag=${folder}_p${player}${id:+_id$id}
    make -j8 GAME="$GAME" BOOT="$folder" BOOT_PLAYER="$player" ${id:+BOOT_ID=$id} >"$OUT/$tag.build.log" 2>&1 ||
        { echo "$tag: BUILD FAILED"; continue; }
    rm -f "$OUT/${tag}_guards.bin"
    GAME=$GAME tools/logic_test.sh "$OUT/$tag" >"$OUT/$tag.stdout" 2>&1
    f=$OUT/$tag.txt
    # guards from the dump (logic_test.sh: GDB printf lines can be lost); no dump -> unknown, never 0
    bad=$(/usr/bin/python3 -c "
import struct, sys
n = 'scriptOverflow badPtr divZero badOpcode entityOOR rowsClamped lineScrollClamped textMenuClamped aniOverflow pause scene3D saveRAMClamped'.split()
try:
    v = struct.unpack('<12I', open(sys.argv[1], 'rb').read())
except Exception:
    print('? (no guard dump)'); sys.exit()
print(' '.join('%s=%d' % kv for kv in zip(n, v) if kv[1]) or '0')" "$OUT/${tag}_guards.bin")
    heap=$(grep -oE "headroom=[0-9]+" "$f" | head -1)
    hbl=$(grep -oE "^F(240|420|600) .*lastHbl=[0-9]+ maxHbl=[0-9]+" "$f" | sed -E 's/^F([0-9]+) .*lastHbl=([0-9]+) maxHbl=([0-9]+)/F\1:\2/' | tr '\n' ' ')
    x0=$(grep -oE "^F240 .*P0 type=[0-9]+ x=-?[0-9]+" "$f" | grep -oE "x=-?[0-9]+$" | cut -d= -f2)
    x1=$(grep -oE "^F600 .*P0 type=[0-9]+ x=-?[0-9]+" "$f" | grep -oE "x=-?[0-9]+$" | cut -d= -f2)
    tm=$(grep -oE "^F600 .*time [0-9]+:[0-9]+\.[0-9]+" "$f" | grep -oE "[0-9]+:[0-9]+\.[0-9]+$")
    ents=$(grep -oE "ENTITIES [0-9]+ active" "$OUT/$tag.stdout" 2>/dev/null | grep -oE "[0-9]+" | head -1)
    printf '%-18s %s | player x %s -> %s | timer %s | hblanks %s| entities %s | guards %s\n' "$tag" "$heap" "$x0" "$x1" \
        "$tm" "$hbl" "$ents" "$bad"
done
make -j8 GAME="$GAME" >"$OUT/natural.build.log" 2>&1 && echo "natural GAME=$GAME build restored"
