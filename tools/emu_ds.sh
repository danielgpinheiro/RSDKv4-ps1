# DuckStation for the harness (sourced; from RSDKv5-ps1 tools/emu.sh, docs/31 §6): retail SCPH-101 BIOS (its
# settings.ini), GDB stub on 2345, booted from a snapshot of the disc (ds_snapshot: build/game.bin/.cue + rsdkv4.elf
# copied, so a rebuild in the tree never changes the disc or the symbols under a run).
# DuckStation is shared with other sessions (user 2026-10-09: only when none of them runs it): ds_start refuses to
# start when one runs, ds_stop kills only our PID, settings.ini is backed up and restored. For the run: slot 1 a
# non-persistent card (never the user's), audio muted (user 2026-10-09), 2 MB RAM.
DS_APP=/Applications/DuckStation.app/Contents/MacOS/DuckStation
DS_SET="$HOME/Library/Application Support/DuckStation/settings.ini"
DS_PID=
DS_BAK=

ds_busy() { pgrep -f "DuckStation.app/Contents/MacOS" >/dev/null; }

# ds_snapshot DIR: the current disc + ELF into DIR (prints nothing)
ds_snapshot() {
    mkdir -p "$1"
    cp build/game.bin "$1/game.bin"
    cp build/game.cue "$1/game.cue"
    cp rsdkv4.elf "$1/rsdkv4.elf"
}

ds_restore() { [ -n "$DS_BAK" ] && [ -f "$DS_BAK" ] && cp "$DS_BAK" "$DS_SET"; }

# ds_start SNAPDIR: exits 2 when DuckStation already runs (another session)
ds_start() {
    ds_busy && { echo "DuckStation is already running (another session?): not starting it" >&2; exit 2; }
    DS_BAK="$1/settings.ini.bak"
    cp "$DS_SET" "$DS_BAK"
    trap ds_restore EXIT
    /usr/bin/python3 - "$DS_SET" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
def section(s, name, keys):
    m = re.search(r'^\[%s\]\n((?:(?!\[).*\n)*)' % re.escape(name), s, re.M)
    if not m:
        return s + '\n[%s]\n' % name + ''.join('%s = %s\n' % kv for kv in keys.items())
    body = [l for l in m.group(1).splitlines() if l.split(' = ')[0] not in keys]
    body = ['%s = %s' % kv for kv in keys.items()] + body
    return s[:m.start(1)] + '\n'.join(body) + '\n' + s[m.end(1):]
s = section(s, 'MemoryCards', {'Card1Type': 'NonPersistent'})
s = section(s, 'Audio', {'OutputMuted': 'true'})
s = section(s, 'Console', {'Enable8MBRAM': 'false'})
s = section(s, 'Debug', {'EnableGDBServer': 'true'})
s = section(s, 'Main', {'SaveStateOnExit': 'false', 'ConfirmPowerOff': 'false', 'StartPaused': 'true'})  # GDB's first continue starts it
s = section(s, 'CPU', {'ExecutionMode': 'Interpreter', 'OverclockEnable': 'false'})
open(p, 'w').write(s)
PYEOF
    "$DS_APP" -batch -fastboot -nofullscreen -- "$1/game.cue" >/dev/null 2>&1 &
    DS_PID=$!
    sleep 4
    for _ in $(seq 1 26); do nc -z localhost 2345 2>/dev/null && break; sleep 1; done
}

ds_stop() { [ -n "$DS_PID" ] && { kill $DS_PID 2>/dev/null; wait $DS_PID 2>/dev/null; }; DS_PID=; ds_restore; }
