/*
 * PS1 menus (see menu.hh): pack loading (phase 4.2).
 */
#include "menu.hh"
#include "gpu_chain.hh"
#include "loading.hh"

#include <stdio.h>
#include <string.h>

#include <psyqo/alloc.h>
#include <psyqo/gpu.hh>

#include "common/syscalls/syscalls.h"

extern psyqo::GPU *g_ps1GPU;

PS1MenuPack g_ps1MenuResident;
PS1MenuPack g_ps1MenuScreens;
volatile uint32_t g_ps1MenuPackLoads = 0;
volatile uint32_t g_ps1MenuPackBytes = 0;
volatile uint32_t g_ps1MenuTestLoad  = 0;

namespace {

struct PackHeader {
    char magic[4];
    uint16_t images, fonts, glyphs, blocks, strings, pad;
    uint32_t dataOffset;
};
struct PackBlock {
    uint16_t x, y, w, h;
    uint32_t offset;
};
static_assert(sizeof(PackHeader) == 20, "menu pack header");
static_assert(sizeof(PackBlock) == 12, "menu pack block");

#define PS1_MENU_STAGING 4096 // bytes: VRAM upload staging (heap, only during the load)
#define PS1_MENU_MAX_BLOCKS 128

}  // namespace

void PS1MenuPackFree(PS1MenuPack &pack) {
    if (pack.mem)
        psyqo_free(pack.mem);
    pack = PS1MenuPack();
}

bool PS1MenuPackLoad(PS1MenuPack &pack, const char *path, void *buf, uint32_t bufSize) {
    PS1MenuPackFree(pack);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ramsyscall_printf("[menu] missing %s\n", path);
        return false;
    }
    PackHeader h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "PMN1", 4) && h.blocks <= PS1_MENU_MAX_BLOCKS;
    // Tables + string bytes in one block: the strings' bytes follow the block records in the file, up to dataOffset.
    uint32_t tablesEnd = sizeof(h) + h.images * sizeof(PS1MenuImage) + h.fonts * sizeof(PS1MenuFont) + h.glyphs * sizeof(PS1MenuGlyph);
    uint32_t blocksEnd = tablesEnd + h.blocks * sizeof(PackBlock);
    uint32_t strBytes  = ok && h.dataOffset > blocksEnd ? h.dataOffset - blocksEnd : 0;
    uint32_t memSize   = (tablesEnd - sizeof(h)) + h.strings * sizeof(char *) + strBytes + h.strings; // + terminators
    uint8_t *mem       = !ok ? nullptr : buf ? (memSize <= bufSize ? (uint8_t *)buf : nullptr) : (uint8_t *)psyqo_malloc(memSize);
    if (ok && buf && memSize > bufSize)
        ramsyscall_printf("[menu] %s: tables %u B > buffer %u B\n", path, memSize, bufSize);
    PackBlock blocks[PS1_MENU_MAX_BLOCKS];
    if (mem) {
        uint8_t *p   = mem;
        pack.images  = (PS1MenuImage *)p;
        p += h.images * sizeof(PS1MenuImage);
        pack.fonts   = (PS1MenuFont *)p;
        p += h.fonts * sizeof(PS1MenuFont);
        pack.glyphs  = (PS1MenuGlyph *)p;
        p += h.glyphs * sizeof(PS1MenuGlyph);
        pack.strings = (const char **)p;
        p += h.strings * sizeof(char *);
        ok = fread(pack.images, sizeof(PS1MenuImage), h.images, f) == h.images;
        ok = ok && fread(pack.fonts, sizeof(PS1MenuFont), h.fonts, f) == h.fonts;
        ok = ok && fread(pack.glyphs, sizeof(PS1MenuGlyph), h.glyphs, f) == h.glyphs;
        ok = ok && fread(blocks, sizeof(PackBlock), h.blocks, f) == h.blocks;
        for (int i = 0; ok && i < h.strings; ++i) { // u16 length + bytes -> zero-terminated in place
            uint16_t len = 0;
            ok           = fread(&len, 2, 1, f) == 1 && fread(p, 1, len, f) == len;
            p[len]       = 0;
            pack.strings[i] = (const char *)p;
            p += len + 1;
        }
    }
    else {
        ok = false;
    }
    uint8_t *staging = ok ? (uint8_t *)psyqo_malloc(PS1_MENU_STAGING) : nullptr;
    uint32_t bytes   = 0;
    if (staging) {
        PS1GpuImmediate(); // the previous frame's chain may still be texturing from VRAM
        for (int b = 0; ok && b < h.blocks; ++b) {
            const PackBlock &blk = blocks[b];
            int lines            = PS1_MENU_STAGING / (blk.w * 2);
            ok                   = lines > 0 && fseek(f, (long)(h.dataOffset + blk.offset), SEEK_SET) == 0;
            for (int line = 0; ok && line < blk.h; line += lines) {
                int n     = blk.h - line < lines ? blk.h - line : lines;
                size_t sz = (size_t)blk.w * n * 2;
                ok        = fread(staging, 1, sz, f) == sz;
                if (ok)
                    g_ps1GPU->uploadToVRAM((const uint16_t *)staging,
                                           psyqo::Rect{ { (int16_t)blk.x, (int16_t)(blk.y + line) }, { (int16_t)blk.w, (int16_t)n } });
                bytes += sz;
            }
        }
        psyqo_free(staging);
    }
    else {
        ok = false;
    }
    fclose(f);
    if (!ok) {
        ramsyscall_printf("[menu] bad or unreadable %s\n", path);
        if (mem && !buf)
            psyqo_free(mem);
        pack = PS1MenuPack();
        return false;
    }
    pack.mem         = buf ? nullptr : mem;
    pack.imageCount  = h.images;
    pack.fontCount   = h.fonts;
    pack.glyphCount  = h.glyphs;
    pack.stringCount = h.strings;
    g_ps1MenuPackLoads = g_ps1MenuPackLoads + 1;
    g_ps1MenuPackBytes = bytes;
    return true;
}

static uint8_t s_residentMem[3072]; // PS1Fonts.pmn's tables + strings (2.7 KB today)
bool PS1MenuResidentLoad() {
    return PS1MenuPackLoad(g_ps1MenuResident, "Data/Game/Menu/PS1Fonts.pmn", s_residentMem, sizeof(s_residentMem));
}

const char *PS1MenuString(int id) {
    return id >= 0 && id < g_ps1MenuResident.stringCount ? g_ps1MenuResident.strings[id] : "";
}

void PS1MenuTestService() {
    if (!g_ps1MenuTestLoad)
        return;
    g_ps1MenuTestLoad = 0;
    PS1MenuPackLoad(g_ps1MenuScreens, "Data/Game/Menu/PS1Menu.pmn");
}

// ---------------------------------------------------------------------------------------------------
// Menu screens (phase 4.3): the mobile menus' flow (RSDKv4/NativeObjects: MenuControl, SaveSelect,
// PlayerSelectScreen) in 2D, with the approved look (docs/30 "Menu look"). Every action sets the same globals and
// calls the same InitStartingStage as the native code (quoted per action).

#include "RetroEngine.hpp"
#include "menu_pack.h"

#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/sprites.hh>

void PS1RenderMenuFrame(bool clear); // ps1/render.cpp

volatile uint32_t g_ps1MenuScreen = 0, g_ps1MenuSel = 0;

namespace {

uint8_t s_screensMem[3072]; // PS1Menu.pmn's tables (2.1 KB): static, never on the heap (see PS1MenuResidentLoad)

struct MenuState {
    int screen;
    int sel;       // selected item of the screen
    int saveSlot;  // -1 = no save, 0-3 = the file being started
    int player;    // player select: 0 Sonic, 1 Sonic & Tails, 2 Tails, 3 Knuckles (the native SAVESEL_* order)
    int timer;
    int fade;      // frames of fade-in left
} s_m;

const int kPlayerChar[4] = { 0, 3, 1, 2 }; // SAVESEL_SONIC / ST / TAILS / KNUX -> characterID
const int kPlayerName[4] = { PS1_STR_SONIC, PS1_STR_SONIC_AND_TAILS, PS1_STR_TAILS, PS1_STR_KNUCKLES };

// ---- drawing -----------------------------------------------------------------------------------

void chainTexPage(uint16_t tpage, int semiMode) {
    psyqo::PrimPieces::TPageAttr attr;
    memcpy(&attr, &tpage, 2);
    if (semiMode >= 0)
        attr.set((psyqo::Prim::TPageAttr::SemiTrans)semiMode);
    PS1ChainTPage(attr);
}

// An image of a pack at (x, y); semiMode >= 0 draws its STP texels blended (0: B/2 + F/2, 1: B + F, 3: B + F/4).
void drawImage(const PS1MenuImage &img, int x, int y, int semiMode = 0) {
    bool semi = img.semi && semiMode >= 0;
    chainTexPage(img.tpage, semi ? semiMode : -1);
    psyqo::Prim::Sprite s(psyqo::Color{ { .r = 0x80, .g = 0x80, .b = 0x80 } });
    if (semi)
        s.setSemiTrans();
    psyqo::PrimPieces::ClutIndex clut;
    memcpy(&clut, &img.clut, 2);
    s.position = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)y } };
    s.size     = psyqo::Vertex{ { .w = (int16_t)img.w, .h = (int16_t)img.h } };
    s.texInfo  = psyqo::PrimPieces::TexInfo{ img.u, img.v, clut };
    PS1ChainSprite(s);
}

const PS1MenuGlyph *findGlyph(const PS1MenuPack &pack, int font, int code) {
    if (font >= pack.fontCount)
        return nullptr;
    const PS1MenuFont &f = pack.fonts[font];
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < f.glyphCount; ++i)
            if (pack.glyphs[f.firstGlyph + i].code == code)
                return &pack.glyphs[f.firstGlyph + i];
        if (code >= 'a' && code <= 'z')
            code -= 32; // fonts without lower case: upper case
        else
            break;
    }
    return nullptr;
}

int textWidth(const PS1MenuPack &pack, int font, const char *s) { // pixels
    int w = 0;
    for (; *s && *s != '\n'; ++s) {
        const PS1MenuGlyph *g = findGlyph(pack, font, (uint8_t)*s);
        if (g)
            w += g->advance;
    }
    return w >> 4;
}

// align: 0 left, 1 centred on x. Lines split at '\n'.
void drawText(const PS1MenuPack &pack, int font, const char *s, int x, int y, int align = 0) {
    if (font >= pack.fontCount)
        return;
    int lineH = pack.fonts[font].lineHeight;
    while (*s) {
        int x16 = (align ? x - textWidth(pack, font, s) / 2 : x) << 4;
        for (; *s && *s != '\n'; ++s) {
            const PS1MenuGlyph *g = findGlyph(pack, font, (uint8_t)*s);
            if (!g)
                continue;
            if (g->image < pack.imageCount)
                drawImage(pack.images[g->image], (x16 >> 4) + g->xoff, y + g->yoff);
            x16 += g->advance;
        }
        if (*s == '\n')
            ++s;
        y += lineH;
    }
}

// Flat rectangle; semiMode >= 0: semi-transparent with that GPU mode.
void drawRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int semiMode = -1) {
    if (semiMode >= 0)
        chainTexPage(0, semiMode);
    uint32_t *wd = PS1ChainRawBegin(3);
    if (!wd)
        return;
    wd[0] = (semiMode >= 0 ? 0x62000000u : 0x60000000u) | r | (uint32_t)g << 8 | (uint32_t)b << 16;
    wd[1] = (uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16;
    wd[2] = (uint32_t)(uint16_t)w | (uint32_t)(uint16_t)h << 16;
    PS1ChainRawEnd();
}

// The menus' background: a vertical gradient (Gouraud quad) + the line-art watermark at +25 %.
void drawBackground() {
    uint32_t *w = PS1ChainRawBegin(8);
    if (w) {
        const uint32_t top = 0x8C2810, bot = 0xBE4624; // BGR: (16, 40, 140) -> (36, 70, 190)
        w[0] = 0x38000000u | top;
        w[1] = 0;
        w[2] = top;
        w[3] = 320;
        w[4] = bot;
        w[5] = 240u << 16;
        w[6] = bot;
        w[7] = 320u | 240u << 16;
        PS1ChainRawEnd();
    }
    PS1ChainInvalidateTPage();
    drawImage(g_ps1MenuScreens.images[PS1_MENU_WATERMARK], 110, 10, 3);
}

void drawBar(int y, int h, bool selected) {
    if (selected)
        drawRect(0, y, 320, h, 200, 160, 0, 0); // B/2 + F/2: the mockup's translucent yellow
    else
        drawRect(0, y, 320, h, 0, 0, 40, 0);
}

// ---- actions (native semantics) -------------------------------------------------------------------

void setGlobal(const char *name, int v) { SetGlobalVariableByName(name, v); }

// PlayerSelectScreen.cpp (PLAYERSELECTSCREEN_STATE_... after the fade): a new game (no save / a new file).
void startNewGame(int saveSlot, int charID) {
    SaveGame *saveGame = (SaveGame *)saveRAM;
    if (saveSlot < 0) {
        setGlobal("options.saveSlot", 0);
        setGlobal("options.gameMode", 0);
    }
    else {
        setGlobal("options.saveSlot", saveSlot);
        setGlobal("options.gameMode", 1);
        SaveFile &f      = saveGame->files[saveSlot];
        f.characterID    = charID;
        f.lives          = 3;
        f.score          = 0;
        f.scoreBonus     = 50000;
        f.stageID        = 1;
        f.emeralds       = 0;
        f.specialStageID = 0;
        WriteSaveRAMData();
    }
    setGlobal("options.stageSelectFlag", 0);
    setGlobal("player.lives", 3);
    setGlobal("player.score", 0);
    setGlobal("player.scoreBonus", 50000);
    setGlobal("specialStage.listPos", 0);
    setGlobal("specialStage.emeralds", 0);
    setGlobal("specialStage.nextZone", 0);
    setGlobal("timeAttack.result", 0);
    setGlobal("lampPostID", 0);
    setGlobal("starPostID", 0);
    debugMode = false;
    InitStartingStage(STAGELIST_PRESENTATION, 0, charID);
}

// SaveSelect.cpp (SAVESELECT_STATE_... load): continue a saved file.
void loadSavedGame(int slot) {
    SaveFile &f = ((SaveGame *)saveRAM)->files[slot];
    setGlobal("options.saveSlot", slot);
    setGlobal("options.gameMode", 1);
    setGlobal("options.stageSelectFlag", 0);
    setGlobal("player.lives", f.lives);
    setGlobal("player.score", f.score);
    setGlobal("player.scoreBonus", f.scoreBonus);
    setGlobal("specialStage.listPos", f.specialStageID);
    setGlobal("specialStage.emeralds", f.emeralds);
    setGlobal("lampPostID", 0);
    setGlobal("starPostID", 0);
    if (f.stageID >= 0x80) {
        setGlobal("specialStage.nextZone", f.stageID - 0x81);
        InitStartingStage(STAGELIST_SPECIAL, f.specialStageID, f.characterID);
    }
    else {
        setGlobal("specialStage.nextZone", f.stageID - 1);
        InitStartingStage(STAGELIST_REGULAR, f.stageID - 1, f.characterID);
    }
}

void go(int screen, int sel = 0) {
    s_m.screen = screen;
    s_m.sel    = sel;
    s_m.timer  = 0;
    s_m.fade   = 8;
}

// ---- screens -----------------------------------------------------------------------------------------

void titleScreen(bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    drawImage(mp.images[PS1_MENU_LOGO], 60, 30);
    if ((s_m.timer >> 5) & 1)
        drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(PS1_STR_PRESS_START), 160, 170, 1);
    if (ok) {
        PlaySfxByName("Menu Select", false);
        go(PS1_MENU_MAIN);
    }
}

void mainScreen(int move, bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    static const int items[3] = { PS1_STR_START_GAME, PS1_STR_TIME_ATTACK, PS1_STR_OPTIONS };
    s_m.sel = (s_m.sel + move + 3) % 3;
    drawImage(mp.images[PS1_MENU_LOGO], 60, 8);
    for (int i = 0; i < 3; ++i) {
        int y = 128 + i * 28;
        if (i == s_m.sel)
            drawBar(y - 3, 24, true);
        drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(items[i]), 160, y, 1);
    }
    if (ok && s_m.sel == 0) { // MenuControl.cpp: Start Game
        PlaySfxByName("Menu Select", false);
        setGlobal("options.vsMode", 0);
        go(PS1_MENU_SAVE);
    }
    else if (back)
        go(PS1_MENU_TITLE);
}

void saveScreen(int move, bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    SaveGame *saveGame    = (SaveGame *)saveRAM;
    static const int icons[4] = { PS1_MENU_ICON_SONIC, PS1_MENU_ICON_TAILS, PS1_MENU_ICON_KNUX, PS1_MENU_ICON_SONIC };
    s_m.sel = (s_m.sel + move + 5) % 5;
    drawText(mp, PS1_FONT_HEADING_L, PS1MenuString(PS1_STR_SAVE_SELECT), 160, 6, 1);
    for (int i = 0; i < 5; ++i) {
        int y = 44 + i * 38;
        drawBar(y, 32, i == s_m.sel);
        if (!i) {
            drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(PS1_STR_NO_SAVE), 60, y + 8);
            continue;
        }
        const SaveFile &f = saveGame->files[i - 1];
        if (f.stageID <= 0) {
            drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(PS1_STR_NEW_GAME), 60, y + 8);
            continue;
        }
        // SaveSelect.cpp: the file's stage (special stages: SaveStageName20+), character, emeralds
        int name = f.stageID >= 0x80 ? PS1_STR_SAVE_STAGE_NAME1 + f.specialStageID + 19 : PS1_STR_SAVE_STAGE_NAME1 + f.stageID - 1;
        if (name > PS1_STR_SAVE_STAGE_NAME25)
            name = PS1_STR_SAVE_STAGE_NAME25;
        const PS1MenuImage &icon = mp.images[icons[f.characterID & 3]];
        drawImage(icon, 14, y + 2);
        if ((f.characterID & 3) == 3)
            drawImage(mp.images[PS1_MENU_ICON_TAILS], 28, y + 2);
        drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(name), 60, y + 3);
        for (int e = 0; e < 7; ++e)
            drawImage(mp.images[(f.emeralds >> e) & 1 ? PS1_MENU_EMERALD_ON : PS1_MENU_EMERALD_OFF], 60 + e * 12, y + 20);
    }
    if (ok) {
        PlaySfxByName("Menu Select", false);
        int slot = s_m.sel - 1;
        if (slot >= 0 && saveGame->files[slot].stageID > 0)
            loadSavedGame(slot);
        else {
            s_m.saveSlot = slot;
            go(PS1_MENU_PLAYER, 0);
        }
    }
    else if (back)
        go(PS1_MENU_MAIN, 0);
}

void playerScreen(int moveX, bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    s_m.sel = (s_m.sel + moveX + 4) % 4;
    drawText(mp, PS1_FONT_HEADING_L, PS1MenuString(PS1_STR_PLAYER_SELECT), 160, 6, 1);
    drawImage(mp.images[PS1_MENU_PLATE], 85, 50);
    switch (s_m.sel) {
        case 0: drawImage(mp.images[PS1_MENU_PORTRAIT_SONIC], 90, 52); break;
        case 1:
            drawImage(mp.images[PS1_MENU_PORTRAIT_TAILS], 118, 58);
            drawImage(mp.images[PS1_MENU_PORTRAIT_SONIC], 62, 52);
            break;
        case 2: drawImage(mp.images[PS1_MENU_PORTRAIT_TAILS], 90, 52); break;
        default: drawImage(mp.images[PS1_MENU_PORTRAIT_KNUX], 90, 52); break;
    }
    drawText(mp, PS1_FONT_LABEL_L, "<", 40, 110);
    drawText(mp, PS1_FONT_LABEL_L, ">", 265, 110);
    drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(kPlayerName[s_m.sel]), 160, 206, 1);
    if (ok) {
        PlaySfxByName("Menu Select", false);
        startNewGame(s_m.saveSlot, kPlayerChar[s_m.sel]);
    }
    else if (back)
        go(PS1_MENU_SAVE, s_m.saveSlot + 1);
}

}  // namespace

void PS1MenuOpen(int screen) {
    if (!g_ps1MenuScreens.imageCount) {
        PS1LoadingBegin();
        PS1MenuPackLoad(g_ps1MenuScreens, "Data/Game/Menu/PS1Menu.pmn", s_screensMem, sizeof(s_screensMem));
        PS1LoadingEnd();
    }
    s_m          = MenuState();
    s_m.saveSlot = -1;
    go(screen);
    Engine.gameMode = ENGINE_PS1MENU;
}

void PS1MenuFrame() {
    CheckKeyPress(&keyPress);
    if (Engine.gameMode != ENGINE_PS1MENU || !g_ps1MenuScreens.imageCount)
        return;
    int moveY = keyPress.down - keyPress.up, moveX = keyPress.right - keyPress.left;
    bool ok = keyPress.start || keyPress.A, back = keyPress.B || keyPress.X;
    if (moveY || moveX)
        PlaySfxByName("Menu Move", false);
    ++s_m.timer;
    PS1RenderMenuFrame(true);
    drawBackground();
    int screen = s_m.screen;
    switch (screen) {
        case PS1_MENU_TITLE: titleScreen(ok, back); break;
        case PS1_MENU_MAIN: mainScreen(moveY, ok, back); break;
        case PS1_MENU_SAVE: saveScreen(moveY, ok, back); break;
        case PS1_MENU_PLAYER: playerScreen(moveX, ok, back); break;
    }
    if (Engine.gameMode != ENGINE_PS1MENU) { // a game started: the stage load replaces the menu art in VRAM
        g_ps1MenuScreens = PS1MenuPack();
        return;
    }
    if (s_m.fade > 0) { // fade in after a screen change: black at 75 / 50 % (two B/2 + F/2 passes = 25 %)
        int passes = s_m.fade > 4 ? 2 : 1;
        for (int i = 0; i < passes; ++i) drawRect(0, 0, 320, 240, 0, 0, 0, 0);
        --s_m.fade;
    }
    g_ps1MenuScreen = s_m.screen;
    g_ps1MenuSel    = s_m.sel;
}
