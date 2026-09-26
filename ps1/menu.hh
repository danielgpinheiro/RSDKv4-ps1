/*
 * PS1 menus (docs/30 phase 4): the mobile menus' flow rebuilt in 2D. Art and fonts come pre-converted from
 * tools/menu/build_menu.py (format there): Data/Game/Menu/PS1Fonts.pmn (resident: the pause menu's fonts + the
 * strings, VRAM rows 242-255, uploaded at boot) and PS1Menu.pmn (the menu screens' art, uploaded when they open).
 * Ids: ps1/menu_pack.h (generated).
 */
#pragma once

#include <stdint.h>

struct PS1MenuImage {
    uint16_t w, h;
    uint8_t u, v;
    uint16_t tpage; // GP0 E1h bits 0-8
    uint16_t clut;  // CLUT word
    uint8_t semi;   // has 50 % texels (STP entries): draw semi-transparent
    uint8_t pad;
};
struct PS1MenuFont {
    uint16_t firstGlyph, glyphCount;
    uint8_t lineHeight, base;
    uint16_t pad;
};
struct PS1MenuGlyph {
    uint16_t code, image; // image 0xFFFF = none (space)
    int8_t xoff, yoff;
    uint16_t advance; // 1/16 px
};
static_assert(sizeof(PS1MenuImage) == 12, "menu image record");
static_assert(sizeof(PS1MenuFont) == 8, "menu font record");
static_assert(sizeof(PS1MenuGlyph) == 8, "menu glyph record");

struct PS1MenuPack {
    PS1MenuImage *images = nullptr;
    PS1MenuFont *fonts   = nullptr;
    PS1MenuGlyph *glyphs = nullptr;
    const char **strings = nullptr;
    uint16_t imageCount = 0, fontCount = 0, glyphCount = 0, stringCount = 0;
    void *mem = nullptr; // one heap block for the tables (nullptr when they sit in a caller buffer)
};

extern PS1MenuPack g_ps1MenuResident; // PS1Fonts.pmn
extern PS1MenuPack g_ps1MenuScreens;  // PS1Menu.pmn (while the menus are open)

// Loads a pack: tables into `buf` (bufSize bytes) or, if null, one heap block; pixel blocks straight to VRAM
// (immediate: waits for the chain in flight). False (pack left empty) if the file is missing, bad or too big.
// The resident pack uses a static buffer: a small heap block allocated at boot would split the free heap the two
// 64 KB chain arenas need (the first frame could not allocate them).
bool PS1MenuPackLoad(PS1MenuPack &pack, const char *path, void *buf = nullptr, uint32_t bufSize = 0);
bool PS1MenuResidentLoad(); // PS1Fonts.pmn into its static buffer (boot)
void PS1MenuPackFree(PS1MenuPack &pack);
const char *PS1MenuString(int id); // the resident pack's strings ("" if none)

extern volatile uint32_t g_ps1MenuPackLoads;  // packs loaded (GDB)
extern volatile uint32_t g_ps1MenuPackBytes;  // texel + CLUT bytes uploaded by the last load
extern volatile uint32_t g_ps1MenuTestLoad;   // GDB: 1 = load PS1Menu.pmn at the next frame (tools/menu_vram_test.sh)
void PS1MenuTestService();                    // main.cpp frame(): services g_ps1MenuTestLoad
