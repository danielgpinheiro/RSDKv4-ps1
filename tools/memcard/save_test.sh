#!/bin/bash
# Save test (docs/30 phase 8) on temporary card images: never the player's cards. PCSX-Redux stores -memcard1 /
# -memcard2 in ~/.config/pcsx-redux/pcsx.json, so it is backed up and restored on exit.
# Part 1 (GDB, natural build): the boot read of a fresh formatted card (no save, no write) -> saveRAM gets two markers
#   -> write -> card 1 lists the file (tools/memcard/mcd.py, CRC ok) -> RAM scrambled -> re-read -> the markers are back.
# Part 2 (two boots on a fresh card): New Game in file 1 with Sonic writes the card; the next boot's save select
#   shows that file (saveRAM[4] = stage 1, character 0) read from the card.
# Other sessions share the emulator: it refuses to start when one runs and kills only its own PID.
# Usage: tools/memcard/save_test.sh [WORKDIR]
set -u
cd "$(dirname "$0")/../.."
W=${1:-/tmp/rsdkv4_save_test}; mkdir -p "$W"
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
CFG="$HOME/.config/pcsx-redux"
MCD="python3 tools/memcard/mcd.py"
FILE=BAHBRW-00004SONIC2
pgrep -x PCSX-Redux >/dev/null && { echo "PCSX-Redux is already running (another session?): not starting it" >&2; exit 2; }
cp -p "$CFG/pcsx.json" "$W/pcsx.json.bak"
shasum "$CFG"/memcard*.mcd > "$W/cards.sha" 2>/dev/null
restore() { cp -p "$W/pcsx.json.bak" "$CFG/pcsx.json"; }
trap restore EXIT

run() { # run CARD1 GDBFILE: one emulator session
    $PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver -memcard1 "$1" -memcard2 "$W/mc2.mcd" \
          -iso build/game.cue -exe rsdkv4.ps-exe >/dev/null 2>&1 &
    local emu=$!
    sleep 4
    timeout ${TIMEOUT:-300} /opt/homebrew/bin/gdb --batch -x "$2" 2>&1 | grep -E "^(READ|WRITE|BOOT|FILE)"
    kill $emu 2>/dev/null; wait $emu 2>/dev/null
}
at() { printf 'break PS1PerfTick if g_ps1PerfFrames == %d\ncontinue\ndelete\n' "$1"; }
head_gdb() { printf 'set pagination off\nset confirm off\ntarget remote localhost:3333\nsymbol-file rsdkv4.elf\n'; }

echo "== part 1: write / re-read"
$MCD format "$W/mc1.mcd" >/dev/null && $MCD raw "$W/mc2.mcd" >/dev/null
{
head_gdb
at 100
printf 'printf "BOOT reads=%%u writes=%%u err=%%d state=%%d\\n", g_ps1SaveReads, g_ps1SaveWrites, g_ps1SaveLastError, s_ps1SaveState\n'
printf 'set var saveRAM[3] = 0x12345678\nset var saveRAM[200] = 777\nset var g_ps1SaveTest = 1\n'
at 200
printf 'printf "WRITE writes=%%u err=%%d\\n", g_ps1SaveWrites, g_ps1SaveLastError\n'
printf 'shell cp %s/mc1.mcd %s/mc1_saved.mcd\n' "$W" "$W"
printf 'set var saveRAM[3] = 0\nset var saveRAM[200] = 0\nset var g_ps1SaveTest = 2\n'
at 300
printf 'printf "READ2 reads=%%u err=%%d saveRAM[3]=%%x saveRAM[200]=%%d | heap never reached %%u\\n", g_ps1SaveReads, g_ps1SaveLastError, saveRAM[3], saveRAM[200], g_ps1HeapHeadroom\n'
printf 'detach\n'
} > "$W/part1.gdb"
run "$W/mc1.mcd" "$W/part1.gdb"
echo "card 1 after the write:"; $MCD list "$W/mc1_saved.mcd"; $MCD check "$W/mc1_saved.mcd" $FILE
echo "card 2 (blank, slot 2) untouched:"; $MCD list "$W/mc2.mcd" 2>&1 | head -2

echo "== part 2: two boots"
$MCD format "$W/mc3.mcd" >/dev/null
{
head_gdb
for f in 60:0x1000 61:0 80:0x10 81:0 100:0x2 101:0 104:0x10 105:0 130:0x10 131:0; do
    at "${f%%:*}"; printf 'set var g_ps1PadTestMask = %s\n' "${f#*:}"
done
at 150
printf 'printf "WRITE boot1 writes=%%u err=%%d file0 char=%%d stage=%%d lives=%%d\\n", g_ps1SaveWrites, g_ps1SaveLastError, saveRAM[0], saveRAM[4], saveRAM[1]\n'
printf 'detach\n'
} > "$W/part2a.gdb"
run "$W/mc3.mcd" "$W/part2a.gdb"
{
head_gdb
at 100
printf 'printf "FILE boot2 reads=%%u err=%%d state=%%d file0 char=%%d stage=%%d lives=%%d\\n", g_ps1SaveReads, g_ps1SaveLastError, s_ps1SaveState, saveRAM[0], saveRAM[4], saveRAM[1]\n'
printf 'detach\n'
} > "$W/part2b.gdb"
run "$W/mc3.mcd" "$W/part2b.gdb"
$MCD list "$W/mc3.mcd"
shasum "$CFG"/memcard*.mcd 2>/dev/null | diff -q - "$W/cards.sha" >/dev/null && echo "player cards unchanged" || echo "player cards CHANGED"
