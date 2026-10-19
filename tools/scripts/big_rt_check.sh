#!/bin/bash
# Host check (docs/43): Sonic 2's two-run big-value table (RSDKv4/Script.cpp, PS1_BIG_RT: runtime insertions in a run at
# the array's top) == the one sorted table Sonic 1 keeps (the same code with PS1_GAME 1): random loads (ascending
# stores, reloads at a lower position that truncate), runtime writes (small / big, new / existing slots, up to the
# capacity) and reads; every read, the overflow count and the code slots must match.
set -eu
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
python3 - "$T" <<'PY'
import sys
s = open('RSDKv4/Script.cpp').read()
open(sys.argv[1] + '/big.inc', 'w').write(s[s.index('struct PS1ScriptBig {'):s.index('// The bytecode marks unused subs/functions')])
PY
cat > "$T/t.cpp" <<'CPP'
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
typedef unsigned int uint;
typedef unsigned short ushort;
#define SCRIPTCODE_COUNT    (0x30000)
#define PS1_SCRIPTBIG_COUNT (0x1400)
#define PS1_SCRIPT_BIG      (-0x8000)
struct Code {
    short slot[SCRIPTCODE_COUNT];
};
namespace A { // Sonic 1: one table
volatile int g_ps1ScriptOverflow, g_ps1ScriptBigCount;
Code scriptCode;
#define PS1_GAME 1
#include "big.inc"
#undef PS1_GAME
#undef PS1_BIG_TOTAL
int val(int i) { return scriptCode.slot[i] != PS1_SCRIPT_BIG ? scriptCode.slot[i] : PS1ScriptBigValue(i); }
void reset() { memset(&scriptCode, 0, sizeof(scriptCode)), s_ps1ScriptBigCount = 0, PS1ScriptBigChanged(); }
} // namespace A
namespace B { // Sonic 2: the runtime run
volatile int g_ps1ScriptOverflow, g_ps1ScriptBigCount;
Code scriptCode;
#define PS1_GAME 2
#include "big.inc"
#undef PS1_GAME
int val(int i) { return scriptCode.slot[i] != PS1_SCRIPT_BIG ? scriptCode.slot[i] : PS1ScriptBigValue(i); }
void reset() { memset(&scriptCode, 0, sizeof(scriptCode)), s_ps1ScriptBigCount = 0, s_ps1BigRtCount = 0, PS1ScriptBigChanged(); }
} // namespace B
static int rv()
{
    int k = rand() % 4;
    return k == 0 ? (rand() % 65536) - 32768 : k == 1 ? rand() * 7919 : k == 2 ? -(rand() * 31) : rand() % 100;
}
int main()
{
    long bad = 0, reads = 0, loads = 0;
    for (int run = 0; run < 40; ++run) {
        srand(run + 1);
        const int range = run % 2 ? 4000 : SCRIPTCODE_COUNT, writes = run % 4 < 2 ? 45 : 3;
        auto load = [&](int from, int n) { // LoadBytecode: ascending stores from `from`
            ++loads;
            for (int k = 0, p = from; k < n && p < SCRIPTCODE_COUNT; ++k, p += 1 + rand() % 8) {
                int v = rv();
                if (A::PS1ScriptStore(p, v) != B::PS1ScriptStore(p, v))
                    ++bad;
            }
        };
        A::reset(), B::reset();
        load(0, run % 3 == 0 ? 6000 : 2000);
        for (int step = 0; step < 200000; ++step) {
            int op = rand() % 100, p = rand() % range;
            if (op < writes) {
                int v = rv();
                A::PS1ScriptWrite(p, v), B::PS1ScriptWrite(p, v);
            }
            else if (op >= 98 && rand() % 40 == 0)
                load(rand() % range, 500);
            else
                ++reads, bad += A::val(p) != B::val(p);
            if (A::g_ps1ScriptOverflow != B::g_ps1ScriptOverflow || A::g_ps1ScriptBigCount != B::g_ps1ScriptBigCount) {
                printf("run %d step %d: overflow %d / %d, entries %d / %d\n", run, step, A::g_ps1ScriptOverflow,
                       B::g_ps1ScriptOverflow, A::g_ps1ScriptBigCount, B::g_ps1ScriptBigCount);
                return 1;
            }
        }
        for (int p = 0; p < SCRIPTCODE_COUNT; ++p) ++reads, bad += A::val(p) != B::val(p);
        bad += memcmp(&A::scriptCode, &B::scriptCode, sizeof(Code)) != 0;
    }
    printf("big-value table: %ld reads, %ld loads, %ld differ from the one-table code\n", reads, loads, bad);
    return bad != 0;
}
CPP
c++ -std=c++17 -O2 -I"$T" -o "$T/t" "$T/t.cpp"
"$T/t"
rm -rf "$T"
