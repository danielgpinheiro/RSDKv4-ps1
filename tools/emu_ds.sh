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
# The user's settings.ini while a harness run holds DuckStation (every harness that edits it, 2026-10-23: a run killed
# before its restore left a throwaway memory card and StartPaused in the user's settings for days). A run that finds it
# at start puts it back first (no DuckStation runs then: its holder was killed); the restore removes it.
DS_HELD="$DS_SET.harness"

ds_busy() { pgrep -f "DuckStation.app/Contents/MacOS" >/dev/null; }

# ds_snapshot DIR: the current disc + ELF into DIR (prints nothing)
ds_snapshot() {
    mkdir -p "$1"
    cp build/game.bin "$1/game.bin"
    cp build/game.cue "$1/game.cue"
    cp rsdkv4.elf "$1/rsdkv4.elf"
}

ds_restore() {
    [ -n "$DS_BAK" ] || return 0
    if [ -f "$DS_HELD" ]; then cp "$DS_HELD" "$DS_SET" && rm -f "$DS_HELD"
    elif [ -f "$DS_BAK" ]; then cp "$DS_BAK" "$DS_SET"; fi
    DS_BAK=
}

# ds_start SNAPDIR: exits 2 when DuckStation already runs (another session)
ds_start() {
    ds_busy && { echo "DuckStation is already running (another session?): not starting it" >&2; exit 2; }
    if [ -f "$DS_HELD" ]; then
        echo "DuckStation settings: the user's put back from $DS_HELD (a killed run left them)" >&2
        cp "$DS_HELD" "$DS_SET"
    fi
    cp "$DS_SET" "$DS_HELD"
    DS_BAK="$1/settings.ini.bak"
    cp "$DS_SET" "$DS_BAK"
    # the exit trap stops our DuckStation before restoring: it writes its settings (the run's) when it quits
    trap ds_stop EXIT
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

# started with open -g, DuckStation is not our child (wait returns at once): TERM each second until it is gone, KILL
# after 15 s, then restore (a restore while it still quits was overwritten by its own settings)
ds_stop() {
    if [ -n "$DS_PID" ]; then
        for _ in $(seq 1 15); do kill $DS_PID 2>/dev/null || break; sleep 1; done
        kill -9 $DS_PID 2>/dev/null
        wait $DS_PID 2>/dev/null
    fi
    DS_PID=; ds_restore
}
