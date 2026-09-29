# The PCSX-Redux instance the test harness drives (sourced by every emulator script, from the repo root).
# GAME=2 (default, Sonic 2): the shared install (~/.config/pcsx-redux, GDB 3333, web 8080), started only when no
#   PCSX-Redux runs at all (other sessions use it: never touch theirs).
# GAME=1 (Sonic 1, docs/37 choice 4): its own instance, portable settings in ../pcsx-redux-s1/pcsx.json, GDB 3336,
#   web 8086: it runs next to other sessions' emulators without touching them; started only when ours isn't running.
# REDUX_PORT / REDUX_WEB / REDUX_PORTABLE override the choice. Scripts kill only the PID they started.
PCSX=${PCSX:-/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux}
if [ "${GAME:-2}" = 1 ]; then
    REDUX_PORTABLE=${REDUX_PORTABLE:-$(cd .. && pwd)/pcsx-redux-s1}
    REDUX_PORT=${REDUX_PORT:-3336}
    REDUX_WEB=${REDUX_WEB:-8086}
else
    REDUX_PORT=${REDUX_PORT:-3333}
    REDUX_WEB=${REDUX_WEB:-8080}
fi
export REDUX_PORT REDUX_WEB
REDUX_ARGS="-gdb-port $REDUX_PORT -webserver-port $REDUX_WEB"
if [ -n "${REDUX_PORTABLE:-}" ]; then
    mkdir -p "$REDUX_PORTABLE"
    [ -f "$REDUX_PORTABLE/pcsx.json" ] || echo '{}' > "$REDUX_PORTABLE/pcsx.json"
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
