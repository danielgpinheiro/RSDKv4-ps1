# The PCSX-Redux instance the test harness drives (sourced by every emulator script, from the repo root).
# User 2026-10-09: the 16 MB PCSX-Redux copy (../pcsx-redux-16mb/redux16mb) in STOCK mode only (-2mb, 1 MB VRAM: never
# -8mb / -16mb / -2mbvram), each game on its own portable instance next to other sessions' emulators:
#   GAME=2 (Sonic 2): portable settings in ../pcsx-redux-s2perf, GDB 3342, web 8102
#   GAME=1 (Sonic 1): portable settings in ../pcsx-redux-s1perf, GDB 3343, web 8103
# (the shared install, GDB 3333, is the Mania session's; the Sonic CD session uses 3338 / 8088.) OpenBIOS, fastboot.
# REDUX_PORT / REDUX_WEB / REDUX_PORTABLE / PCSX override the choice. Scripts kill only the PID they started.
PS1_ROOT=$(cd .. && pwd)
PCSX=${PCSX:-$PS1_ROOT/pcsx-redux-16mb/redux16mb}
if [ "${GAME:-2}" = 1 ]; then
    REDUX_PORTABLE=${REDUX_PORTABLE:-$PS1_ROOT/pcsx-redux-s1perf}
    REDUX_PORT=${REDUX_PORT:-3343}
    REDUX_WEB=${REDUX_WEB:-8103}
else
    REDUX_PORTABLE=${REDUX_PORTABLE:-$PS1_ROOT/pcsx-redux-s2perf}
    REDUX_PORT=${REDUX_PORT:-3342}
    REDUX_WEB=${REDUX_WEB:-8102}
fi
export REDUX_PORT REDUX_WEB
REDUX_ARGS="-2mb -bios $PS1_ROOT/pcsx-redux-16mb/openbios.bin -gdb-port $REDUX_PORT -webserver -webserver-port $REDUX_WEB"
if [ -n "${REDUX_PORTABLE:-}" ]; then
    mkdir -p "$REDUX_PORTABLE"
    [ -f "$REDUX_PORTABLE/pcsx.json" ] || echo '{}' > "$REDUX_PORTABLE/pcsx.json"
    # Audio off for every test run (user 2026-10-09): SPU.Mute in the instance's own settings (the game's SPU state is
    # unchanged: only the host output is muted)
    /usr/bin/python3 -c "import json,sys;p=sys.argv[1];d=json.load(open(p));d.setdefault('SPU',{})['Mute']=True;json.dump(d,open(p,'w'),indent=4)" "$REDUX_PORTABLE/pcsx.json"
    REDUX_ARGS="$REDUX_ARGS -portable $REDUX_PORTABLE"
    # Freshly formatted cards for every run (docs/37): a run that saves (the menus, a finished act) must not change the
    # next run's game through the card (Sonic 1's scripts read the spin dash option from the save). HARNESS_CARDS=0: the
    # script passes its own (tools/memcard/save_test.sh).
    if [ "${HARNESS_CARDS:-1}" = 1 ]; then
        python3 tools/memcard/mcd.py format "$REDUX_PORTABLE/harness_mc1.mcd" >/dev/null
        python3 tools/memcard/mcd.py format "$REDUX_PORTABLE/harness_mc2.mcd" >/dev/null
        REDUX_ARGS="$REDUX_ARGS -memcard1 $REDUX_PORTABLE/harness_mc1.mcd -memcard2 $REDUX_PORTABLE/harness_mc2.mcd"
    fi
fi
# Busy: our portable instance's GDB port is taken, or (shared install) any PCSX-Redux runs.
redux_busy() {
    if [ -n "${REDUX_PORTABLE:-}" ]; then
        nc -z localhost "$REDUX_PORT" 2>/dev/null || nc -z localhost "$REDUX_WEB" 2>/dev/null
    else
        pgrep -x PCSX-Redux >/dev/null
    fi
}
