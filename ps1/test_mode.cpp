/*
 * Regression test mode — see test_mode.hh and ../docs/45. Compiled only with `make TEST=1`.
 *
 * Engine-agnostic on purpose (the same file serves the other ports): the scenario names memory by address (the host
 * resolves symbols and struct offsets from this build's ELF), so the target only reads, hashes, pokes and prints.
 * Scenario layout: ../tests/tools/scn.py (the two must agree: version below).
 */
#include "test_mode.hh"

#if PS1_TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psyqo/alloc.h>
#include <psyqo/gpu.hh>
#include "common/psxlibc/handlers.h"
#include "common/syscalls/syscalls.h"

extern volatile uint32_t g_ps1PerfFrames, g_ps1PerfLastHbl, g_ps1PerfLastFrameVsyncs;
extern volatile uint16_t g_ps1PadTestMask, g_ps1PadButtons;
extern psyqo::GPU *g_ps1GPU;
bool PS1LoadingActive(); // loading.cpp

extern "C" const char g_ps1TestBuild[] = "TEST"; // marks a TEST=1 build (make dist / export_builder refuse it)

#define SCN_MAGIC   0x4C455354u // "TSEL"
#define SCN_VERSION 1
enum : uint16_t { F_RECORD = 1, F_NOHASH = 2, F_SELFTEST_CRASH = 4, F_SELFTEST_HANG = 8 };
enum : uint32_t { C_WATCH = 1, C_ARRAYS = 2, C_DUMPS = 4, C_STACK = 32 };

struct ScnHeader {
    uint32_t magic;
    uint16_t version, flags;
    uint32_t seed, endFrame;
    char folder[16];
    char id[8];
    int8_t player;
    uint8_t gameMode, hangLimitSec, loadLimitSec;
    uint16_t nInput, nCheck, nPoke, nWatchF, nWatchC, nHash, nArr, nDump;
    uint32_t bodyBytes;
    char name[32];
};
static_assert(sizeof(ScnHeader) == 96, "scn.py header layout");
struct ScnInput { uint32_t frame, mask; };
struct ScnCheck { uint32_t frame, kinds; };
struct ScnPoke { uint32_t frame, addr, value; uint8_t size, pad[3]; };
struct ScnWatch { uint32_t addr; uint8_t size, sign, pad[2]; };
struct ScnRange { uint32_t addr, len; };
struct ScnArrField { uint16_t off; uint8_t size, pad; };
struct ScnArr { uint32_t base; uint16_t stride, count, filterOff; uint8_t filterSize, nFields; };

struct PS1TestScenario {
    ScnHeader h;
    const ScnInput *input;
    const ScnCheck *check;
    const ScnPoke *poke;
    const ScnWatch *watchF, *watchC;
    const ScnRange *hash, *dump;
    const uint8_t *arr;
    uint16_t inPos, chkPos, pokePos;
    uint16_t lastPad;
    bool ended;
};
PS1TestScenario *g_ps1Test = nullptr;

// ---- TTY (PCSX expansion region: putc at 1F802080h, exit at 1F802082h) -------------------------------------------
static inline void ttyc(char c) { *(volatile char *)0xBF802080 = c; }
static void tty(const char *s) { while (*s) ttyc(*s++); }
static void ttyx(uint32_t v) {
    char b[9];
    int n = 0;
    do {
        b[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    while (n) ttyc(b[--n]);
}
static void ttysx(uint32_t v) { ttyc(' '); ttyx(v); }
static void testExit(uint32_t status) {
    *(volatile int16_t *)0xBF802082 = (int16_t)status;
    for (;;) {
    }
}

static uint32_t readMem(uint32_t addr, uint8_t size, uint8_t sign) {
    switch (size) {
        case 1: return sign ? (uint32_t)(int32_t) * (volatile int8_t *)addr : *(volatile uint8_t *)addr;
        case 2: return sign ? (uint32_t)(int32_t) * (volatile int16_t *)addr : *(volatile uint16_t *)addr;
        default: return *(volatile uint32_t *)addr;
    }
}

// ---- watchdog + crash ---------------------------------------------------------------------------------------------
// Runs first on every exception (BIOS IRQ chain, priority 0, as irq_guard's filter): counts the vblanks since the last
// logic frame. A main loop stuck for hangLimitSec (loadLimitSec while a load is bracketed) prints HANG with the
// interrupted thread's PC / RA / SP (current TCB: [[108h]], psx-spx kernelbios.md "Thread Control Blocks") and leaves the
// machine as it is: the runner attaches GDB (registers, backtrace) and then stops the emulator.
static volatile uint32_t s_lastVbl = 0, s_lastFrame = 0, s_loadStart = 0;
static volatile uint8_t s_phase = 0;
static volatile bool s_wasLoading = false;
static uint32_t *curTcb() { return *(uint32_t **)(*(uint32_t *)0x80000108); }
static int watchdog() {
    if (!g_ps1Test || !g_ps1GPU || g_ps1Test->ended)
        return 0;
    uint32_t now  = g_ps1GPU->getFrameCount();
    bool loading  = PS1LoadingActive();
    if (loading && !s_wasLoading)
        s_loadStart = now; // a load starts: timed on its own limit
    else if (!loading && s_wasLoading)
        s_lastVbl = now;   // a load ended: counts as progress (a frame may hold several loads)
    s_wasLoading = loading;
    uint32_t since = loading ? now - s_loadStart : now - s_lastVbl;
    uint32_t lim   = (loading ? g_ps1Test->h.loadLimitSec : g_ps1Test->h.hangLimitSec) * 60u;
    if (lim && since > lim) {
        g_ps1Test->ended = true;
        uint32_t *t = curTcb();
        tty("HANG");
        ttysx(s_lastFrame);
        ttysx(since);
        ttysx(loading);
        ttysx(s_phase);
        tty(" epc");
        ttysx(t[0x88 / 4]);
        tty(" ra");
        ttysx(t[(0x08 + 31 * 4) / 4]);
        tty(" sp");
        ttysx(t[(0x08 + 29 * 4) / 4]);
        ttyc('\n');
    }
    return 0;
}
static HandlerInfo s_watchdog = {nullptr, nullptr, watchdog, 0};

// A(40h) SystemErrorUnresolvedException (the BIOS's dead end for exceptions nobody handles): print and exit instead.
extern "C" void PS1TestUnresolved() {
    uint32_t *t = curTcb();
    uint32_t badv;
    __asm__ volatile("mfc0 %0, $8" : "=r"(badv));
    tty("CRASH cause");
    ttysx(t[0x98 / 4]);
    tty(" epc");
    ttysx(t[0x88 / 4]);
    tty(" badv");
    ttysx(badv);
    tty(" ra");
    ttysx(t[(0x08 + 31 * 4) / 4]);
    tty(" sp");
    ttysx(t[(0x08 + 29 * 4) / 4]);
    tty(" frame");
    ttysx(g_ps1PerfFrames);
    ttyc('\n');
    testExit(4);
}

// ---- scenario -----------------------------------------------------------------------------------------------------
void PS1TestPhase(uint8_t p) { s_phase = p; }

void PS1TestInit() {
    tty("T test-build v1\n");
    FILE *f = fopen("Data/TEST.SEL", "rb");
    ScnHeader h;
    if (!f || fread(&h, sizeof(h), 1, f) != 1 || h.magic != SCN_MAGIC) {
        if (f)
            fclose(f);
        tty("T no-scenario\n");
        return;
    }
    if (h.version != SCN_VERSION) {
        fclose(f);
        tty("T bad-version");
        ttysx(h.version);
        ttyc('\n');
        testExit(2);
    }
    // One block: the header copy + the body (input track, lists), at the bottom of the fresh heap (it stays for the run).
    uint8_t *mem = (uint8_t *)psyqo_malloc(sizeof(PS1TestScenario) + h.bodyBytes);
    PS1TestScenario *s = (PS1TestScenario *)mem;
    memset(s, 0, sizeof(*s));
    s->h = h;
    uint8_t *b = mem + sizeof(PS1TestScenario);
    if (fread(b, 1, h.bodyBytes, f) != h.bodyBytes) {
        tty("T short-read\n");
        testExit(2);
    }
    fclose(f);
    s->input  = (const ScnInput *)b;  b += h.nInput * sizeof(ScnInput);
    s->check  = (const ScnCheck *)b;  b += h.nCheck * sizeof(ScnCheck);
    s->poke   = (const ScnPoke *)b;   b += h.nPoke * sizeof(ScnPoke);
    s->watchF = (const ScnWatch *)b;  b += h.nWatchF * sizeof(ScnWatch);
    s->watchC = (const ScnWatch *)b;  b += h.nWatchC * sizeof(ScnWatch);
    s->hash   = (const ScnRange *)b;  b += h.nHash * sizeof(ScnRange);
    s->dump   = (const ScnRange *)b;  b += h.nDump * sizeof(ScnRange);
    s->arr    = b;
    g_ps1Test = s;
    tty("S ");
    tty(h.name);
    ttysx(h.seed);
    ttysx(h.endFrame);
    ttyc('\n');
    s_lastVbl = g_ps1GPU ? g_ps1GPU->getFrameCount() : 0;
    syscall_sysEnqIntRP(0, &s_watchdog);
    ((uint32_t *)0x80000200)[0x40] = (uint32_t)&PS1TestUnresolved; // A0 table (RAM, 200h)
    if (h.flags & F_SELFTEST_CRASH)
        *(volatile uint32_t *)0x00000001 = 0; // unaligned store: address error -> CRASH path
}

bool PS1TestBootStage() { return g_ps1Test && g_ps1Test->h.folder[0]; }
const char *PS1TestBootFolder() { return g_ps1Test->h.folder; }
const char *PS1TestBootId() { return g_ps1Test->h.id; }
int PS1TestBootPlayer() { return g_ps1Test ? g_ps1Test->h.player : -1; }
int PS1TestBootMode() { return g_ps1Test ? g_ps1Test->h.gameMode : 0; }

void PS1TestAfterInit() {
    if (!g_ps1Test)
        return;
    srand(g_ps1Test->h.seed);
    s_lastVbl = g_ps1GPU->getFrameCount();
}

void PS1TestPreFrame() {
    PS1TestScenario *s = g_ps1Test;
    if (!s)
        return;
    uint32_t fr = g_ps1PerfFrames; // the frame RunFrame is about to run (PS1PerfTick counts it after)
    while (s->inPos < s->h.nInput && s->input[s->inPos].frame <= fr) {
        g_ps1PadTestMask = (uint16_t)s->input[s->inPos].mask;
        s->inPos++;
    }
    while (s->pokePos < s->h.nPoke && s->poke[s->pokePos].frame <= fr) {
        const ScnPoke &p = s->poke[s->pokePos++];
        if (p.size == 1)
            *(volatile uint8_t *)p.addr = (uint8_t)p.value;
        else if (p.size == 2)
            *(volatile uint16_t *)p.addr = (uint16_t)p.value;
        else
            *(volatile uint32_t *)p.addr = p.value;
    }
    if ((s->h.flags & F_SELFTEST_HANG) && fr == 60)
        for (;;) {
        }
}

static void checkpoint(PS1TestScenario *s, uint32_t fr, uint32_t kinds) {
    if (kinds & C_WATCH) {
        tty("C ");
        ttyx(fr);
        tty(" W");
        for (int i = 0; i < s->h.nWatchC; i++) ttysx(readMem(s->watchC[i].addr, s->watchC[i].size, s->watchC[i].sign));
        ttyc('\n');
    }
    if (kinds & C_ARRAYS) {
        const uint8_t *a = s->arr;
        for (int i = 0; i < s->h.nArr; i++) {
            const ScnArr *d = (const ScnArr *)a;
            const ScnArrField *fl = (const ScnArrField *)(a + sizeof(ScnArr));
            for (int e = 0; e < d->count; e++) {
                uint32_t base = d->base + (uint32_t)e * d->stride;
                if (d->filterSize && !readMem(base + d->filterOff, d->filterSize, 0))
                    continue;
                tty("C ");
                ttyx(fr);
                tty(" A");
                ttyx(i);
                ttysx(e);
                for (int k = 0; k < d->nFields; k++) ttysx(readMem(base + fl[k].off, fl[k].size, 0));
                ttyc('\n');
            }
            a += sizeof(ScnArr) + d->nFields * sizeof(ScnArrField);
            a += (4 - ((uintptr_t)a & 3)) & 3;
        }
    }
    if (kinds & C_DUMPS) {
        for (int i = 0; i < s->h.nDump; i++) {
            const uint8_t *p = (const uint8_t *)s->dump[i].addr;
            for (uint32_t o = 0; o < s->dump[i].len; o += 32) {
                tty("C ");
                ttyx(fr);
                tty(" D");
                ttyx(i);
                ttysx(o);
                ttyc(' ');
                for (uint32_t k = o; k < o + 32 && k < s->dump[i].len; k++) {
                    ttyc("0123456789abcdef"[p[k] >> 4]);
                    ttyc("0123456789abcdef"[p[k] & 15]);
                }
                ttyc('\n');
            }
        }
    }
}

void PS1TestPostFrame() {
    PS1TestScenario *s = g_ps1Test;
    if (!s || s->ended)
        return;
    uint32_t fr = g_ps1PerfFrames - 1; // the frame RunFrame just ran
    s_lastFrame = fr;
    s_lastVbl   = g_ps1GPU->getFrameCount();
    uint32_t h  = 5381;
    if (!(s->h.flags & F_NOHASH))
        for (int i = 0; i < s->h.nHash; i++) {
            const uint32_t *p = (const uint32_t *)s->hash[i].addr;
            for (uint32_t n = s->hash[i].len >> 2; n; n--) h = ((h << 5) + h) ^ *p++;
        }
    tty("F ");
    ttyx(fr);
    ttysx(h);
    ttysx(g_ps1PerfLastHbl);
    ttysx(g_ps1PerfLastFrameVsyncs);
    for (int i = 0; i < s->h.nWatchF; i++) ttysx(readMem(s->watchF[i].addr, s->watchF[i].size, s->watchF[i].sign));
    ttyc('\n');
    if ((s->h.flags & F_RECORD) && g_ps1PadButtons != s->lastPad) {
        s->lastPad = g_ps1PadButtons;
        tty("R ");
        ttyx(fr);
        ttysx(s->lastPad);
        ttyc('\n');
    }
    while (s->chkPos < s->h.nCheck && s->check[s->chkPos].frame <= fr) checkpoint(s, fr, s->check[s->chkPos++].kinds);
    if (fr >= s->h.endFrame) {
        s->ended = true;
        tty("E 0 ");
        ttyx(fr);
        ttyc('\n');
        testExit(0);
    }
}
#endif
