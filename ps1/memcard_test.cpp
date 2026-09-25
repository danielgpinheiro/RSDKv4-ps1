/*
 * Memory card self-test (phase 8): `make MEMCARD_TEST=1`, run with tools/memcard/selftest.sh
 * (port 1 = a formatted temporary card, port 2 = an unformatted one; never the player's cards).
 * Results are GDB counters; PS1McTestHook(step) runs after every step so the host can inspect the
 * card image (PCSX-Redux writes each sector to the file immediately).
 */
#include <string.h>

#include "common/util/sjis-title-encoder.hh"
#include "memcard.hh"
#include "memcard_icons.h"

volatile uint32_t g_ps1McTestSteps     = 0; // steps run
volatile uint32_t g_ps1McTestPassed    = 0;
volatile uint32_t g_ps1McTestFirstFail = 0; // step number + 1 of the first failure, 0 = none
volatile uint32_t g_ps1McTestLastError = 0; // psyqo error code at that failure
volatile uint32_t g_ps1McTestDone      = 0;
volatile uint32_t g_ps1McTestErr[20];       // error code returned by each step

extern "C" __attribute__((noinline)) void PS1McTestHook(uint32_t step) { __asm__ volatile("" ::"r"(step)); }

namespace {

using MC = psyqo::MemoryCard;
constexpr auto P1 = MC::Port::Port0, P2 = MC::Port::Port1;
constexpr const char *TITLE = "RSDK SAVE TEST";

uint8_t s_data[9000], s_back[9000];

void fill(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) s_data[i] = (uint8_t)(i * 7 + 3); // tools/memcard/mcd.py --pattern
}

void step(uint32_t n, PS1McError e, bool ok) {
    g_ps1McTestErr[n] = (uint32_t)e;
    g_ps1McTestSteps  = g_ps1McTestSteps + 1;
    if (ok)
        g_ps1McTestPassed = g_ps1McTestPassed + 1;
    else if (!g_ps1McTestFirstFail) {
        g_ps1McTestFirstFail = n + 1;
        g_ps1McTestLastError = (uint32_t)e;
    }
    PS1McTestHook(n);
}

bool iconEq(const PS1McIcon &a, const PS1McIcon &b) {
    return a.frameCount == b.frameCount && !memcmp(a.clut, b.clut, sizeof(a.clut)) &&
           !memcmp(a.pixels[0], b.pixels[0], sizeof(a.pixels[0]));
}

} // namespace

void PS1McSelfTest() {
    char file[21];
    bool nameOk = PS1McFileName(file, 'A', "HBRW-00000", "TEST") && !strcmp(file, "BAHBRW-00000TEST");
    uint32_t freeBlocks = 0, len = 0, count = 0;
    uint16_t ver = 0;
    bool exists = true;
    PS1McError e;

    e = PS1McState(P1);
    step(0, e, e == PS1McError::OK && nameOk);
    e = PS1McFreeBlocks(P1, &freeBlocks);
    step(1, e, e == PS1McError::OK && freeBlocks == 15);
    e = PS1McExists(P1, file, &exists);
    step(2, e, e == PS1McError::OK && !exists);

    fill(3000);
    e = PS1McSave(P1, file, TITLE, s_ps1McIconSonicCD, s_data, 3000, 1);
    step(3, e, e == PS1McError::OK); // host: mcd.py check --icon s_ps1McIconSonicCD --pattern 3000
    e = PS1McExists(P1, file, &exists);
    step(4, e, e == PS1McError::OK && exists);

    static psyqo::MemoryCardFileSystem::FileInfo info;
    uint8_t title[64] = {};
    Sjis::g_titleEncoder(title, sizeof(title), TITLE); // what psyqo wrote
    e = PS1McInfo(P1, file, &info);
    step(5, e, e == PS1McError::OK && !memcmp(info.title, title, 64) && iconEq(info.icon, s_ps1McIconSonicCD));

    memset(s_back, 0, sizeof(s_back));
    e = PS1McLoad(P1, file, s_back, sizeof(s_back), &len, &ver);
    step(6, e, e == PS1McError::OK && len == 3000 && ver == 1 && !memcmp(s_back, s_data, 3000));

    static psyqo::MemoryCardFileSystem::FileEntry list[15];
    e = PS1McList(P1, list, 15, &count);
    step(7, e, e == PS1McError::OK && count == 1 && !strcmp(list[0].name, file) && list[0].sizeInBlocks == 1);
    e = PS1McFreeBlocks(P1, &freeBlocks);
    step(8, e, e == PS1McError::OK && freeBlocks == 14);

    // Replace it with a 2-block save (header 4 frames + 16 + 9000 > 8192 bytes), another icon.
    fill(9000);
    e = PS1McSave(P1, file, TITLE, s_ps1McIconSonic2, s_data, 9000, 2);
    step(9, e, e == PS1McError::OK); // host: mcd.py check --icon s_ps1McIconSonic2 --pattern 9000
    memset(s_back, 0, sizeof(s_back));
    e = PS1McLoad(P1, file, s_back, sizeof(s_back), &len, &ver);
    step(10, e, e == PS1McError::OK && len == 9000 && ver == 2 && !memcmp(s_back, s_data, 9000));
    e = PS1McFreeBlocks(P1, &freeBlocks);
    step(11, e, e == PS1McError::OK && freeBlocks == 13);
    e = PS1McLoad(P1, file, s_back, 100, &len, &ver);
    step(12, e, e == PS1McError::SerializeOverflow && len == 0);

    e = PS1McDelete(P1, file);
    step(13, e, e == PS1McError::OK);
    e = PS1McExists(P1, file, &exists);
    step(14, e, e == PS1McError::OK && !exists);
    e = PS1McFreeBlocks(P1, &freeBlocks);
    step(15, e, e == PS1McError::OK && freeBlocks == 15);
    e = PS1McLoad(P1, file, s_back, sizeof(s_back), &len, &ver);
    step(16, e, e == PS1McError::FileNotFound);

    // Port 2: an unformatted card is reported, never formatted or written.
    e = PS1McState(P2);
    step(17, e, e == PS1McError::NotFormatted);
    e = PS1McSave(P2, file, TITLE, s_ps1McIconSonicCD, s_data, 100, 1);
    step(18, e, e == PS1McError::NotFormatted);

    g_ps1McTestDone = 1;
}
