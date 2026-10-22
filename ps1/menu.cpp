/*
 * PS1 menus (see menu.hh): pack loading (phase 4.2).
 */
#include "menu.hh"
#include "gpu_chain.hh"
#include "loading.hh"
#include "version_stamp.hh"

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
#if PS1_GAME == 1
#include "menu_pack_s1.h" // Sonic 1 (docs/37 phase 4)
#else
#include "menu_pack.h"
#endif

#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/sprites.hh>

void PS1RenderMenuFrame(bool clear); // ps1/render.cpp

volatile uint32_t g_ps1MenuScreen = 0, g_ps1MenuSel = 0;

namespace {

uint8_t s_screensMem[3072]; // PS1Menu.pmn's tables (2.1 KB): static, never on the heap (see PS1MenuResidentLoad)
uint8_t s_taMem[384];       // PS1TimeAttack.pmn's tables (240 B)
PS1MenuPack s_taPack;       // the Time Attack act pictures (VRAM page row 1 of the menu columns)

struct MenuState {
    int screen;
    int sel;       // selected item of the screen
    int saveSlot;  // -1 = no save, 0-3 = the file being started
    int player;    // player select: 0 Sonic, 1 Sonic & Tails, 2 Tails, 3 Knuckles (the native SAVESEL_* order)
    int timer;
    int fade;      // frames of fade-in left
    int delDialog; // save select: the delete dialog is open (saveDeleteFrame)
    int delSel;    // its selection: 0 YES, 1 NO
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

// An image of a pack at (x, y); semiMode >= 0 draws its STP texels blended (0: B/2 + F/2, 1: B + F, 3: B + F/4);
// level: the sprite's grey modulation (0x80 = the art's own colours).
void drawImage(const PS1MenuImage &img, int x, int y, int semiMode = 0, uint8_t level = 0x80) {
    bool semi = img.semi && semiMode >= 0;
    chainTexPage(img.tpage, semi ? semiMode : -1);
    psyqo::Prim::Sprite s(psyqo::Color{ { .r = level, .g = level, .b = level } });
    if (semi)
        s.setSemiTrans();
    psyqo::PrimPieces::ClutIndex clut;
    memcpy(&clut, &img.clut, 2);
    s.position = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)y } };
    s.size     = psyqo::Vertex{ { .w = (int16_t)img.w, .h = (int16_t)img.h } };
    s.texInfo  = psyqo::PrimPieces::TexInfo{ img.u, img.v, clut };
    PS1ChainSprite(s);
}

// An image modulated by an RGB colour: the native SetRenderVertexColor tints (texel x colour / 255), here at the GPU's
// full range (texel x c / 128 with c = colour, about twice as bright): the 10 x 10 save-screen emeralds came out muted
// on the mid-grey art and hard to tell from the flat grey of the missing ones (tester 2026-10-20, V4-106).
void drawImageTint(const PS1MenuImage &img, int x, int y, uint32_t rgb) {
    chainTexPage(img.tpage, -1);
    psyqo::Prim::Sprite s(psyqo::Color{ { .r = (uint8_t)(rgb >> 16), .g = (uint8_t)(rgb >> 8), .b = (uint8_t)rgb } });
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

// align: 0 left, 1 centred on x. Lines split at '\n'. level: drawImage's.
void drawText(const PS1MenuPack &pack, int font, const char *s, int x, int y, int align = 0, uint8_t level = 0x80) {
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
                drawImage(pack.images[g->image], (x16 >> 4) + g->xoff, y + g->yoff, 0, level);
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

// A save file's character icon(s) on its 32-pixel bar (SubMenuButton: each icon centred at x -76; Sonic & Tails: Tails
// there, Sonic 8 left of it drawn on top), centred at x 23, y + 16 (V4-118)
void drawSaveIcons(const PS1MenuPack &mp, int characterID, int y) {
    static const int icons[4] = { PS1_MENU_ICON_SONIC, PS1_MENU_ICON_TAILS, PS1_MENU_ICON_KNUX, PS1_MENU_ICON_TAILS };
    const PS1MenuImage &icon = mp.images[icons[characterID & 3]];
    drawImage(icon, 23 - icon.w / 2, y + 16 - icon.h / 2);
    if ((characterID & 3) == 3) {
        const PS1MenuImage &sonic = mp.images[PS1_MENU_ICON_SONIC];
        drawImage(sonic, 15 - sonic.w / 2, y + 16 - sonic.h / 2);
    }
}

void drawBar(int y, int h, bool selected) {
    if (selected)
        drawRect(0, y, 320, h, 200, 160, 0, 0); // B/2 + F/2: the mockup's translucent yellow
    else
        drawRect(0, y, 320, h, 0, 0, 40, 0);
}

// ---- actions (native semantics) -------------------------------------------------------------------

void setGlobal(const char *name, int v) { SetGlobalVariableByName(name, v); }

// The original 2013 port's hidden options (its level select's ITEMS / MAX EMERALDS: options.shieldType, Sonic 1's
// options.superStates), which its Stage Setup wipes in save mode; on the PS1 that wipe is patched out
// (patch_bytecode.py patch_keep_options) and the Options screen keeps them in two of SaveGame's unused values (47, 48)
// with a marker bit: a card without them reads as the defaults (user 2026-10-09: enabled). Story play uses them (a new
// game, a file, no save), Time Attack the original 0.
enum { PS1_OPT_SET = 0x100 };
#if PS1_GAME == 1
constexpr int kShieldDefault = 2; // "S1+S3": Sonic 1's blue shield and invincibility, the Sonic 3 shields + insta shield
#else
// options.shieldType as Sonic 2's scripts read it (tester report 2026-10-10; the Monitor startup, the random monitor's
// tables, GlobalCode function 58): 0 Sonic 2, 1 the Sonic 3 shields as placed + the insta shield, 2 every monitor random
// from the Sonic 2 set (the Eggman monitor included), 3 random from the Sonic 3 set + the insta shield
constexpr int kShieldDefault = 1; // Sonic 3 shields + insta shield
#endif
int optShield() {
    int v = ((const SaveGame *)saveRAM)->unused[0];
    return v & PS1_OPT_SET ? v & 3 : kShieldDefault;
}
void setOptShield(int v) { ((SaveGame *)saveRAM)->unused[0] = PS1_OPT_SET | (v & 3); }
#if PS1_GAME == 1
int optSevenEmeralds() { // options.superStates: the 7th special stage (MAX EMERALDS 07)
    int v = ((const SaveGame *)saveRAM)->unused[1];
    return v & PS1_OPT_SET ? v & 1 : 1;
}
void setOptSevenEmeralds(int v) { ((SaveGame *)saveRAM)->unused[1] = PS1_OPT_SET | (v & 1); }
#endif
void applyStoryOptions() {
    setGlobal("options.shieldType", optShield());
#if PS1_GAME == 1
    setGlobal("options.superStates", optSevenEmeralds());
#endif
}

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
    applyStoryOptions();
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
    applyStoryOptions();
    if (f.stageID >= 0x80) {
        setGlobal("specialStage.nextZone", f.stageID - 0x81);
        InitStartingStage(STAGELIST_SPECIAL, f.specialStageID, f.characterID);
    }
    else {
        setGlobal("specialStage.nextZone", f.stageID - 1);
        InitStartingStage(STAGELIST_REGULAR, f.stageID - 1, f.characterID);
    }
}

#if PS1_GAME == 1
// ---- Time Attack, Sonic 1 (TimeAttack.cpp / RecordsScreen.cpp GAME_SONIC1): six zones of 3 acts, the Final Zone (1)
// and the special stages (6: records blank at 30000). Records: 3 per act (centiseconds), 60000 = the native blank.
// Pictures (PS1TimeAttack.pmn, 112 x 84): acts 0-17, the Final Zone, special stages 1-6.
enum { TA_ZONES = 8, TA_ACTS = 3, TA_PIC_W = 112, TA_PIC_H = 84 };
#else
// ---- Time Attack (TimeAttack.cpp, RecordsScreen.cpp; Sonic 2 without Egg Gauntlet: 11 zones of 2 acts, Metropolis
// 3, Wing Fortress / Boss Attack / Hidden Palace 1). Records: 3 per act (centiseconds), 60000 = the native blank.
enum { TA_ZONES = 11, TA_ACTS = 2 };
#endif
struct TAState {
    int zone, act;
    int button;   // records screen: 0 PLAY, 1 NEXT ACT
    int rank;     // the new record's place (1-3) while it flashes
    int flash;    // frames of the flash left (RECORDSSCREEN_STATE_EXITRESULTS: 2 s)
    bool pending; // a run was started from the records screen: RESETGAME comes back to it
} s_ta;

#if PS1_GAME == 1
int taActCount(int z) { return z == 6 ? 1 : z == 7 ? 6 : TA_ACTS; }            // RecordsScreen: final / special
int taOffset(int z) { return z == 6 ? TA_ACTS * 6 : z == 7 ? TA_ACTS * 6 + 1 : TA_ACTS * z; } // recordOffset
int taRecordPos(int z) { return 3 * taOffset(z); }
int taPicture(int z, int act) { return taOffset(z) + act; }                     // PS1TimeAttack.pmn order
int taName(int z, int act) { return PS1_STR_SAVE_STAGE_NAME1 + taOffset(z) + act; } // strSaveStageList[nameID + act]
#else
int taActCount(int z) { return z == 7 ? 3 : z >= 8 ? 1 : TA_ACTS; } // RecordsScreen: "metropolis sux"
int taRecordPos(int z) { return 3 * (z >= 8 ? TA_ACTS * 8 + 1 + (z - 8) : TA_ACTS * z); }
int taPicture(int z, int act) { return z < 8 ? TA_ACTS * z + act : 17 + (z - 8); } // PS1TimeAttack.pmn order
int taName(int z, int act) { // strSaveStageList[nameID + actID]
    static const int offs[3] = { TA_ACTS * 8 + 1, 22, 20 };
    int off = z < 8 ? TA_ACTS * z : offs[z - 8];
    return PS1_STR_SAVE_STAGE_NAME1 + (off == TA_ACTS * 8 + 1 ? off + 1 : off) + act;
}
#endif
int taZoneTotal(int z) {
    const SaveGame *saveGame = (const SaveGame *)saveRAM;
    int t = 0;
    for (int a = 0; a < taActCount(z); ++a) t += saveGame->records[taRecordPos(z) + 3 * a];
    return t;
}
#if PS1_GAME == 1
bool taUnlocked(int z) { // TimeAttack_Create GAME_SONIC1: past the zone's last act; Final Zone past 18, specials past 19
    const SaveGame *saveGame = (const SaveGame *)saveRAM;
    int need = z == 6 ? TA_ACTS * 6 : z == 7 ? TA_ACTS * 6 + 1 : TA_ACTS * (z + 1);
    return saveGame->unlockedActs > need && saveGame->unlockedActs < 0x80;
}
#else
bool taUnlocked(int z) { // TimeAttack_Create: reached in a save (unlockedActs), Hidden Palace by its own flag
    const SaveGame *saveGame = (const SaveGame *)saveRAM;
    if (z == 10)
        return saveGame->unlockedHPZ;
    return saveGame->unlockedActs > TA_ACTS * (z + 1) && saveGame->unlockedActs < 0x80;
}
#endif

// TimeAttack_Create: blank records -> 60000 (saved), unlockedActs = the furthest stage of the files.
void taCreate() {
    SaveGame *saveGame = (SaveGame *)saveRAM;
    bool updated       = false;
#if PS1_GAME == 1
    for (int i = 0; i < (6 * TA_ACTS + 1) * 3 + 6 * 3; ++i) { // 19 acts at 60000, then 6 special stages at 30000
        if (!saveGame->records[i]) {
            saveGame->records[i] = i < (6 * TA_ACTS + 1) * 3 ? 60000 : 30000;
            updated              = true;
        }
    }
#else
    for (int i = 0; i < TA_ZONES * TA_ACTS * 3; ++i) {
        if (!saveGame->records[i]) {
            saveGame->records[i] = 60000;
            updated              = true;
        }
    }
#endif
    if (updated)
        WriteSaveRAMData();
    int zone = saveGame->unlockedActs;
    for (int i = 0; i < 4; ++i) {
        if (saveGame->files[i].stageID > zone)
            zone = saveGame->files[i].stageID;
    }
    saveGame->unlockedActs = zone;
}

// RecordsScreen RECORDSSCREEN_STATE_LOADSTAGE (with the decomp's Wing Fortress fix): Sonic, one life.
void taPlay() {
    setGlobal("options.saveSlot", 0);
    setGlobal("options.gameMode", 2);
    setGlobal("player.lives", 1);
    setGlobal("player.score", 0);
    setGlobal("player.scoreBonus", 50000);
    setGlobal("specialStage.listPos", 0);
    setGlobal("specialStage.emeralds", 0);
    setGlobal("specialStage.nextZone", 0);
    setGlobal("lampPostID", 0);
    setGlobal("starPostID", 0);
    setGlobal("timeAttack.result", 0);
    setGlobal("options.shieldType", 0); // the original shields: records stay comparable (user 2026-10-09)
#if PS1_GAME == 1
    setGlobal("options.superStates", 0);
#endif
    int z = s_ta.zone;
#if PS1_GAME == 1
    if (z >= 7) // RECORDSSCREEN_STATE_LOADSTAGE GAME_SONIC1: the special stages
        InitStartingStage(STAGELIST_SPECIAL, s_ta.act, 0);
    else
        InitStartingStage(STAGELIST_REGULAR, z * TA_ACTS + s_ta.act, 0); // the Final Zone: 18
#else
    if (z >= 9) {
        if (z == 9)
            InitStartingStage(STAGELIST_BONUS, 1, 0);
        else
            InitStartingStage(STAGELIST_REGULAR, z * TA_ACTS, 0);
    }
    else if (z == 8)
        InitStartingStage(STAGELIST_REGULAR, 18, 0);
    else
        InitStartingStage(STAGELIST_REGULAR, z * TA_ACTS + s_ta.act, 0);
#endif
    s_ta.pending = true;
}

// RECORDSSCREEN_STATE_SHOWRESULTS: a finished run's time (timeAttack.result, set by the act's end) enters the act's
// records; a new place flashes for 2 s (EXITRESULTS) and the save is written.
void taShowResults() {
    SaveGame *saveGame = (SaveGame *)saveRAM;
    s_ta.pending       = false;
    s_ta.rank          = 0;
    int time           = GetGlobalVariableByName("timeAttack.result");
    if (time <= 0)
        return;
    int *records = &saveGame->records[taRecordPos(s_ta.zone) + 3 * s_ta.act];
    for (int r = 0; r < 3; ++r) {
        if (time < records[r]) {
            for (int s = 2; s > r; --s) records[s] = records[s - 1];
            records[r] = time;
            s_ta.rank  = r + 1;
            break;
        }
    }
    if (s_ta.rank) {
        s_ta.flash = 120;
        PlaySfxByName("Event", false);
        WriteSaveRAMData();
    }
}

// AddTimeStringToFont without its leading space: M'SS"CC
void formatTime(char *out, int time) {
    int m = time / 6000, sec = time / 100 % 60, cs = time % 100;
    if (m >= 10)
        *out++ = '0' + m / 10 % 10;
    *out++ = '0' + m % 10;
    *out++ = '\'';
    *out++ = '0' + sec / 10;
    *out++ = '0' + sec % 10;
    *out++ = '"';
    *out++ = '0' + cs / 10;
    *out++ = '0' + cs % 10;
    *out   = 0;
}

void playMainMenuMusic() { // MenuControl_Create / FadeScreen
    SetMusicTrack("MainMenu.ogg", 0, true, 106596);
    PlayMusic(0, 0);
}

void go(int screen, int sel = 0) {
    s_m.screen = screen;
    s_m.sel    = sel;
    s_m.timer  = 0;
    s_m.fade   = 8;
}

// SaveSelect.cpp SAVESELECT_STATE_DELSETUP / DIALOGWAIT (the mobile versions' DELETE button): a used file is deleted
// behind the DialogPanel's yes / no ("Delete Saved Game?", NO first like the pause dialogs). On the PS1 Select opens it on
// the selected file (tester report 2026-10-10). YES resets the file as the native does (characterID 0, 3 lives, score
// 0, bonus 50000, stage 0, no emeralds) and writes the card; the other files, the records and the options stay.
// Returns true while the dialog is open (it takes the screen's input and draws over the list).
bool saveDeleteFrame(int moveX, bool ok, bool back, bool select) {
    const PS1MenuPack &rp = g_ps1MenuResident;
    SaveGame *saveGame    = (SaveGame *)saveRAM;
    int slot              = s_m.sel - 1;
    if (!s_m.delDialog) {
        if (!select || slot < 0 || saveGame->files[slot].stageID <= 0)
            return false;
        PlaySfxByName("Menu Select", false);
        s_m.delDialog = 1;
        s_m.delSel    = 1;
        moveX         = 0;
        ok = back = false;
    }
    s_m.delSel = (s_m.delSel + moveX + 2) & 1;
    drawRect(30, 60, 260, 120, 16, 40, 150); // the pause dialogs' panel
    drawText(rp, PS1_FONT_TEXT_S, PS1MenuString(PS1_STR_DELETE_SAVED_GAME), 160, 96, 1);
    for (int i = 0; i < 2; ++i) {
        int x = i ? 200 : 120;
        if (i == s_m.delSel)
            drawRect(x - 34, 145, 68, 18, 170, 130, 0);
        drawText(rp, PS1_FONT_LABEL_S, PS1MenuString(i ? PS1_STR_NO : PS1_STR_YES), x, 148, 1);
    }
    if (ok && s_m.delSel == 0) {
        PlaySfxByName("Event", false);
        SaveFile &f      = saveGame->files[slot];
        f.characterID    = 0;
        f.lives          = 3;
        f.score          = 0;
        f.scoreBonus     = 50000;
        f.stageID        = 0;
        f.emeralds       = 0;
        f.specialStageID = 0;
        WriteSaveRAMData();
        s_m.delDialog = 0;
    }
    else if (ok || back) {
        PlaySfxByName("Menu Back", false);
        s_m.delDialog = 0;
    }
    return true;
}

// The save screen's hint on a used file's bar (right of the emeralds).
void saveDeleteHint(const PS1MenuPack &mp, int y) {
    static const char hint[] = "SELECT: DELETE";
    drawText(mp, PS1_FONT_LABEL_M, hint, 312 - textWidth(mp, PS1_FONT_LABEL_M, hint), y + 20);
}

// ---- screens -----------------------------------------------------------------------------------------

void titleScreen(bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    drawImage(mp.images[PS1_MENU_LOGO], 60, 30);
    if ((s_m.timer >> 5) & 1)
        drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(PS1_STR_PRESS_START), 160, 170, 1);
    if (ok) {
        PlaySfxByName("Menu Select", false);
        playMainMenuMusic();
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
    else if (ok && s_m.sel == 1) {
        PlaySfxByName("Menu Select", false);
        taCreate();
        go(PS1_MENU_TIMEATTACK, 0);
    }
    else if (ok && s_m.sel == 2) {
        PlaySfxByName("Menu Select", false);
        go(PS1_MENU_OPTIONS);
    }
    else if (back)
        go(PS1_MENU_TITLE);
}

#if PS1_GAME == 1
void saveScreen(int move, int moveX, bool ok, bool back, bool select) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    SaveGame *saveGame    = (SaveGame *)saveRAM;
    // SubMenuButton S1's six; the 7th (options.superStates) in its special-stage sprite's purple (Special/Objects.gif
    // 0xA440FF, brightened like the six)
    static const uint32_t emeralds[7] = { 0x8080FF, 0xFFFF00, 0xFF60C0, 0xA0FF00, 0xFF4060, 0xFFFFFF, 0xBA49FF };
    if (!s_m.delDialog)
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
        // SaveSelect.cpp: special stages SaveStageName20+, past the Final Zone "COMPLETE" (strSaveStageList[25])
        int name = f.stageID >= 0x80 ? PS1_STR_SAVE_STAGE_NAME1 + f.specialStageID + 19
                   : f.stageID - 1 > 18 ? PS1_STR_SAVE_STAGE_NAME26 : PS1_STR_SAVE_STAGE_NAME1 + f.stageID - 1;
        if (name > PS1_STR_SAVE_STAGE_NAME26)
            name = PS1_STR_SAVE_STAGE_NAME26;
        drawSaveIcons(mp, f.characterID, y);
        drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(name), 60, y + 3);
        if (i == s_m.sel && !s_m.delDialog)
            saveDeleteHint(mp, y);
        int shown = (f.emeralds >> 6) & 1 || optSevenEmeralds() ? 7 : 6; // the 7th only when it can be (or was) won
        for (int e = 0; e < shown; ++e) { // each in its colour when collected (SubMenuButton, GAME_SONIC1)
            if ((f.emeralds >> e) & 1)
                drawImageTint(mp.images[PS1_MENU_EMERALD_ON], 60 + e * 12, y + 20, emeralds[e]);
            else
                drawImage(mp.images[PS1_MENU_EMERALD_OFF], 60 + e * 12, y + 20);
        }
    }
    if (saveDeleteFrame(moveX, ok, back, select))
        return;
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

// Player select, Sonic 1 (PlayerSelectScreen.cpp): the original locks Tails and Knuckles until the Ending (the save's
// flags); on the PS1 every character is there from the start (user 2026-10-09): Sonic, Sonic & Tails, Tails, Knuckles.
void playerScreen(int moveX, bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    const int count       = 4;
    if (count > 1)
        s_m.sel = (s_m.sel + moveX + count) % count;
    else
        s_m.sel = 0;
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
    if (count > 1) {
        drawText(mp, PS1_FONT_LABEL_L, "<", 40, 110);
        drawText(mp, PS1_FONT_LABEL_L, ">", 265, 110);
    }
    drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(kPlayerName[s_m.sel]), 160, 206, 1);
    if (ok) {
        PlaySfxByName("Menu Select", false);
        startNewGame(s_m.saveSlot, kPlayerChar[s_m.sel]);
    }
    else if (back)
        go(PS1_MENU_SAVE, s_m.saveSlot + 1);
}

// Options, Sonic 1 (SettingsScreen.cpp GAME_SONIC1): music / SFX volume, the spin dash (on / off: saveGame's value 35,
// which Sonic 1's player script reads), the shields and the emerald count (the original's hidden options:
// applyStoryOptions), the staff credits.
void optionsScreen(int moveY, int moveX, bool ok, bool back) {
    enum { ROW_SHIELDS = 3, ROW_EMERALDS = 4, ROW_CREDITS = 5, ROWS = 6 };
    const PS1MenuPack &mp = g_ps1MenuScreens;
    SaveGame *saveGame    = (SaveGame *)saveRAM;
    s_m.sel               = (s_m.sel + moveY + ROWS) % ROWS;
    drawText(mp, PS1_FONT_HEADING_L, PS1MenuString(PS1_STR_SETTINGS), 160, 6, 1);
    static const int items[ROWS]  = { PS1_STR_MUSIC,   PS1_STR_SOUND_F_X,    PS1_STR_SPIN_DASH,
                                      PS1_STR_SHIELDS, PS1_STR_MAX_EMERALDS, PS1_STR_STAFF_CREDITS };
    static const int shields[4]   = { PS1_STR_SHIELD_SONIC1, PS1_STR_SHIELD_SONIC2, PS1_STR_SHIELD_SONIC1_S3,
                                      PS1_STR_SHIELD_SONIC2_S3 }; // the level select's S1 / S2 / S1+S3 / S2+S3
    for (int i = 0; i < ROWS; ++i) {
        int y = 46 + i * 31;
        drawBar(y - 4, 26, i == s_m.sel);
        drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(items[i]), 40, y);
        if (i < 2) { // volume: 5 notches
            int vol = i == 0 ? saveGame->musVolume : saveGame->sfxVolume;
            for (int k = 0; k < 5; ++k) {
                bool on = vol > k * (MAX_VOLUME / 5);
                drawRect(200 + k * 18, y + 2, 12, 12, on ? 255 : 40, on ? 210 : 40, on ? 0 : 80);
            }
        }
        else if (i == 2) { // ON / OFF, the chosen one lit (the native buttons' bgColor)
            bool on = saveGame->spindashEnabled;
            drawRect(196, y - 2, 42, 18, on ? 0 : 0, on ? 160 : 96, on ? 72 : 32);
            drawRect(244, y - 2, 42, 18, 0, !on ? 160 : 96, !on ? 72 : 32);
            drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_ON), 217, y + 1, 1);
            drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_OFF), 265, y + 1, 1);
        }
        else if (i == ROW_SHIELDS)
            drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(shields[optShield()]), 246, y + 1, 1);
        else if (i == ROW_EMERALDS) { // 6 / 7, the chosen one lit
            bool seven = optSevenEmeralds();
            drawRect(214, y - 2, 30, 18, 0, !seven ? 160 : 96, !seven ? 72 : 32);
            drawRect(250, y - 2, 30, 18, 0, seven ? 160 : 96, seven ? 72 : 32);
            drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_SIX), 229, y + 1, 1);
            drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_SEVEN), 265, y + 1, 1);
        }
    }
    if (moveX && s_m.sel < 2) { // SettingsScreen.cpp SETTINGSSCREEN_SEL_MUSVOL / SFXVOL
        PlaySfxByName("Menu Move", false);
        int &vol = s_m.sel == 0 ? saveGame->musVolume : saveGame->sfxVolume;
        if (moveX < 0) {
            if (vol > 0)
                vol -= (MAX_VOLUME / 5);
            SetGameVolumes(saveGame->musVolume, saveGame->sfxVolume);
            if (s_m.sel == 0 && !saveGame->musVolume)
                musicEnabled = false;
        }
        else {
            if (vol < MAX_VOLUME)
                vol += (MAX_VOLUME / 5);
            if (s_m.sel == 0 && !musicEnabled) {
                musicEnabled = true;
                PlayMusic(0, 0);
            }
            SetGameVolumes(saveGame->musVolume, saveGame->sfxVolume);
        }
    }
    else if (moveX && s_m.sel == 2) { // SETTINGSSCREEN_SEL_SPINDASH: left / right flip it
        PlaySfxByName("Menu Move", false);
        saveGame->spindashEnabled = !saveGame->spindashEnabled;
    }
    else if (moveX && s_m.sel == ROW_SHIELDS) { // the level select's ITEMS: left / right cycle the four
        PlaySfxByName("Menu Move", false);
        setOptShield((optShield() + moveX + 4) & 3);
    }
    else if (moveX && s_m.sel == ROW_EMERALDS) { // MAX EMERALDS 06 / 07
        PlaySfxByName("Menu Move", false);
        setOptSevenEmeralds(!optSevenEmeralds());
    }
    if (ok && s_m.sel == ROW_CREDITS) {
        PlaySfxByName("Menu Select", false);
        InitStartingStage(STAGELIST_PRESENTATION, 2, 0); // the scripted Staff Credits; its end returns to the menus
    }
    else if (back) {
        WriteSaveRAMData();
        go(PS1_MENU_MAIN, 2);
    }
}

// Time Attack, Sonic 1: the six zones, the Final Zone and the special stages in a list; the selected one's first
// picture (112 x 84) with its total time, and the total of every entry.
void timeAttackScreen(int moveY, bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    char buf[16];
    s_m.sel = (s_m.sel + moveY + TA_ZONES) % TA_ZONES;
    drawText(mp, PS1_FONT_HEADING_L, PS1MenuString(PS1_STR_TIME_ATTACK), 160, 6, 1);
    int total = 0;
    for (int z = 0; z < TA_ZONES; ++z) {
        int y = 46 + z * 22;
        total += taZoneTotal(z);
        if (z == s_m.sel)
            drawRect(0, y - 4, 180, 18, 200, 160, 0, 0);
        drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_STAGE_NAME1 + z), 10, y);
        if (!taUnlocked(z)) {
            drawRect(0, y - 4, 180, 18, 16, 40, 140, 0); // dimmed
            drawImage(mp.images[PS1_MENU_LOCK], 160, y - 3);
        }
    }
    int z = s_m.sel;
    drawRect(190, 48, TA_PIC_W + 4, TA_PIC_H + 4, 128, 128, 128); // the native grey frame
    if (s_taPack.imageCount)
        drawImage(s_taPack.images[taPicture(z, 0)], 192, 50);
    if (!taUnlocked(z)) {
        drawRect(192, 50, TA_PIC_W, TA_PIC_H, 0, 0, 0, 0);
        drawImage(mp.images[PS1_MENU_LOCK], 192 + TA_PIC_W / 2 - 8, 50 + TA_PIC_H / 2 - 8);
    }
    formatTime(buf, taZoneTotal(z));
    drawText(mp, PS1_FONT_LABEL_M, buf, 248, 142, 1);
    drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_TOTAL_TIME), 248, 188, 1);
    formatTime(buf, total);
    drawText(mp, PS1_FONT_LABEL_M, buf, 248, 206, 1);
    if (ok && taUnlocked(z)) {
        PlaySfxByName("Menu Select", false);
        s_ta.zone   = z;
        s_ta.act    = 0;
        s_ta.button = 0;
        s_ta.rank   = 0;
        s_ta.flash  = 0;
        debugMode   = false; // RecordsScreen_Create
        go(PS1_MENU_RECORDS);
    }
    else if (back)
        go(PS1_MENU_MAIN, 1);
}

// Records, Sonic 1: as Sonic 2's (the act's picture, name, 3 records, PLAY / NEXT ACT), 112 x 84 pictures.
void recordsScreen(int moveX, bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    const SaveGame *saveGame = (const SaveGame *)saveRAM;
    char buf[24];
    int z = s_ta.zone, n = taActCount(z);
    if (s_ta.flash > 0 && !--s_ta.flash)
        s_ta.rank = 0;
    if (!s_ta.flash) { // RECORDSSCREEN_STATE_MAIN (EXITRESULTS takes no input)
        if (n > 1 && moveX < 0 && --s_ta.button < 0) {
            s_ta.button = 1;
            s_ta.act    = (s_ta.act + n - 1) % n;
        }
        else if (n > 1 && moveX > 0 && ++s_ta.button >= 2) {
            s_ta.button = 0;
            s_ta.act    = (s_ta.act + 1) % n;
        }
    }
    drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(taName(z, s_ta.act)), 160, 8, 1);
    drawRect(158 - TA_PIC_W / 2, 32, TA_PIC_W + 4, TA_PIC_H + 4, 128, 128, 128);
    if (s_taPack.imageCount)
        drawImage(s_taPack.images[taPicture(z, s_ta.act)], 160 - TA_PIC_W / 2, 34);
    drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_RECORDS), 160, 132, 1);
    const int *rec = &saveGame->records[taRecordPos(z) + 3 * s_ta.act];
    for (int r = 0; r < 3; ++r) {
        if (s_ta.rank == r + 1 && s_ta.flash % 6 >= 3) // the new record blinks (0.1 s period)
            continue;
        buf[0] = '1' + r;
        buf[1] = '.';
        buf[2] = ' ';
        formatTime(buf + 3, rec[r]);
        drawText(mp, PS1_FONT_LABEL_M, buf, 124, 150 + r * 16);
    }
    int px = n > 1 ? 100 : 160;
    drawRect(px - 44, 208, 88, 18, s_ta.button == 0 ? 200 : 0, s_ta.button == 0 ? 160 : 0, s_ta.button == 0 ? 0 : 40, 0);
    drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_PLAY), px, 212, 1);
    if (n > 1) {
        drawRect(176, 208, 88, 18, s_ta.button == 1 ? 200 : 0, s_ta.button == 1 ? 160 : 0, s_ta.button == 1 ? 0 : 40, 0);
        drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_NEXT_ACT), 220, 212, 1);
    }
    if (s_ta.flash)
        return;
    if (ok && s_ta.button == 1) {
        PlaySfxByName("Menu Move", false);
        s_ta.act = (s_ta.act + 1) % n;
    }
    else if (ok) {
        PlaySfxByName("Menu Select", false);
        taPlay();
    }
    else if (back) {
        PlaySfxByName("Menu Back", false);
        go(PS1_MENU_TIMEATTACK, z);
    }
}

#else
void saveScreen(int move, int moveX, bool ok, bool back, bool select) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    SaveGame *saveGame    = (SaveGame *)saveRAM;
    // SubMenuButton emeraldColorsS2
    static const uint32_t emeralds[7] = { 0x00C0FF, 0x8000C0, 0xFF0000, 0xFF60FF, 0xFFC000, 0x60C000, 0xFFFFFF };
    if (!s_m.delDialog)
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
        drawSaveIcons(mp, f.characterID, y);
        drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(name), 60, y + 3);
        if (i == s_m.sel && !s_m.delDialog)
            saveDeleteHint(mp, y);
        for (int e = 0; e < 7; ++e) { // each in its colour when collected (SubMenuButton emeraldColorsS2)
            if ((f.emeralds >> e) & 1)
                drawImageTint(mp.images[PS1_MENU_EMERALD_ON], 60 + e * 12, y + 20, emeralds[e]);
            else
                drawImage(mp.images[PS1_MENU_EMERALD_OFF], 60 + e * 12, y + 20);
        }
    }
    if (saveDeleteFrame(moveX, ok, back, select))
        return;
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

// Options (OptionsMenu / SettingsScreen): music and SFX volume in the native 5 steps (SetGameVolumes; heard from
// phase 5), the shields (the original's hidden option: applyStoryOptions), the staff credits (the scripted Credits
// stage, presentation list 2). Sonic 2's scripts never read the spin dash setting (a Sonic 1 option) and the box art
// only changes the mobile title: not shown.
void optionsScreen(int moveY, int moveX, bool ok, bool back) {
    enum { ROW_SHIELDS = 2, ROW_CREDITS = 3, ROWS = 4 };
    const PS1MenuPack &mp = g_ps1MenuScreens;
    SaveGame *saveGame    = (SaveGame *)saveRAM;
    s_m.sel               = (s_m.sel + moveY + ROWS) % ROWS;
    drawText(mp, PS1_FONT_HEADING_L, PS1MenuString(PS1_STR_SETTINGS), 160, 6, 1);
    static const int items[ROWS] = { PS1_STR_MUSIC, PS1_STR_SOUND_F_X, PS1_STR_SHIELDS, PS1_STR_STAFF_CREDITS };
    static const int shields[4]  = { PS1_STR_SHIELD_SONIC2, PS1_STR_SHIELD_SONIC3_INSTA, PS1_STR_SHIELD_RANDOM,
                                     PS1_STR_SHIELD_RANDOM_INSTA }; // options.shieldType's values (kShieldDefault)
    for (int i = 0; i < ROWS; ++i) {
        int y = 56 + i * 36;
        drawBar(y - 4, 28, i == s_m.sel);
        drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(items[i]), 40, y);
        if (i < 2) { // volume: 5 notches
            int vol = i == 0 ? saveGame->musVolume : saveGame->sfxVolume;
            for (int k = 0; k < 5; ++k) {
                bool on = vol > k * (MAX_VOLUME / 5);
                drawRect(200 + k * 18, y + 2, 12, 12, on ? 255 : 40, on ? 210 : 40, on ? 0 : 80);
            }
        }
        else if (i == ROW_SHIELDS)
            drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(shields[optShield()]), 246, y + 1, 1);
    }
    if (moveX && s_m.sel < 2) { // SettingsScreen.cpp SETTINGSSCREEN_SEL_MUSVOL / SFXVOL
        PlaySfxByName("Menu Move", false);
        int &vol = s_m.sel == 0 ? saveGame->musVolume : saveGame->sfxVolume;
        if (moveX < 0) {
            if (vol > 0)
                vol -= (MAX_VOLUME / 5);
            SetGameVolumes(saveGame->musVolume, saveGame->sfxVolume);
            if (s_m.sel == 0 && !saveGame->musVolume)
                musicEnabled = false;
        }
        else {
            if (vol < MAX_VOLUME)
                vol += (MAX_VOLUME / 5);
            if (s_m.sel == 0 && !musicEnabled) {
                musicEnabled = true;
                PlayMusic(0, 0);
            }
            SetGameVolumes(saveGame->musVolume, saveGame->sfxVolume);
        }
    }
    else if (moveX && s_m.sel == ROW_SHIELDS) { // the level select's shield setting: left / right cycle the four
        PlaySfxByName("Menu Move", false);
        setOptShield((optShield() + moveX + 4) & 3);
    }
    if (ok && s_m.sel == ROW_CREDITS) {
        PlaySfxByName("Menu Select", false);
        InitStartingStage(STAGELIST_PRESENTATION, 2, 0); // the scripted Staff Credits; its end returns to the menus
    }
    else if (back) {
        WriteSaveRAMData();
        go(PS1_MENU_MAIN, 2);
    }
}

// Time Attack: the zones in a list (locked ones dimmed with the lock), the selected zone's act 1 picture with its
// total time (the native zone buttons), and the total of every zone (the native "TOTAL TIME:" button).
void timeAttackScreen(int moveY, bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    char buf[16];
    s_m.sel = (s_m.sel + moveY + TA_ZONES) % TA_ZONES;
    drawText(mp, PS1_FONT_HEADING_L, PS1MenuString(PS1_STR_TIME_ATTACK), 160, 6, 1);
    int total = 0;
    for (int z = 0; z < TA_ZONES; ++z) {
        int y = 42 + z * 17;
        total += taZoneTotal(z);
        if (z == s_m.sel)
            drawRect(0, y - 3, 176, 16, 200, 160, 0, 0);
        drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_STAGE_NAME1 + z), 12, y);
        if (!taUnlocked(z)) {
            drawRect(0, y - 3, 176, 16, 16, 40, 140, 0); // dimmed
            drawImage(mp.images[PS1_MENU_LOCK], 156, y - 3);
        }
    }
    int z = s_m.sel;
    drawRect(184, 44, 132, 100, 128, 128, 128); // the native grey frame (RenderRect 128 x 98)
    if (s_taPack.imageCount)
        drawImage(s_taPack.images[taPicture(z, 0)], 186, 46);
    if (!taUnlocked(z)) {
        drawRect(186, 46, 128, 96, 0, 0, 0, 0);
        drawImage(mp.images[PS1_MENU_LOCK], 242, 86);
    }
    formatTime(buf, taZoneTotal(z));
    drawText(mp, PS1_FONT_LABEL_M, buf, 250, 152, 1);
    drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_TOTAL_TIME), 250, 188, 1);
    formatTime(buf, total);
    drawText(mp, PS1_FONT_LABEL_M, buf, 250, 206, 1);
    if (ok && taUnlocked(z)) {
        PlaySfxByName("Menu Select", false);
        s_ta.zone   = z;
        s_ta.act    = 0;
        s_ta.button = 0;
        s_ta.rank   = 0;
        s_ta.flash  = 0;
        debugMode   = false; // RecordsScreen_Create
        go(PS1_MENU_RECORDS);
    }
    else if (back)
        go(PS1_MENU_MAIN, 1);
}

// Records (RecordsScreen): the act's picture, name and 3 records; PLAY / NEXT ACT (left / right past either end
// flips the act, as the native physical controls).
void recordsScreen(int moveX, bool ok, bool back) {
    const PS1MenuPack &mp = g_ps1MenuScreens;
    const SaveGame *saveGame = (const SaveGame *)saveRAM;
    char buf[24];
    int z = s_ta.zone, n = taActCount(z);
    if (s_ta.flash > 0 && !--s_ta.flash)
        s_ta.rank = 0;
    if (!s_ta.flash) { // RECORDSSCREEN_STATE_MAIN (EXITRESULTS takes no input)
        if (n > 1 && moveX < 0 && --s_ta.button < 0) {
            s_ta.button = 1;
            s_ta.act    = (s_ta.act + n - 1) % n;
        }
        else if (n > 1 && moveX > 0 && ++s_ta.button >= 2) {
            s_ta.button = 0;
            s_ta.act    = (s_ta.act + 1) % n;
        }
    }
    drawText(mp, PS1_FONT_LABEL_L, PS1MenuString(taName(z, s_ta.act)), 160, 8, 1);
    drawRect(94, 30, 132, 100, 128, 128, 128);
    if (s_taPack.imageCount)
        drawImage(s_taPack.images[taPicture(z, s_ta.act)], 96, 32);
    drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_RECORDS), 160, 138, 1);
    const int *rec = &saveGame->records[taRecordPos(z) + 3 * s_ta.act];
    for (int r = 0; r < 3; ++r) {
        if (s_ta.rank == r + 1 && s_ta.flash % 6 >= 3) // the new record blinks (0.1 s period)
            continue;
        buf[0] = '1' + r;
        buf[1] = '.';
        buf[2] = ' ';
        formatTime(buf + 3, rec[r]);
        drawText(mp, PS1_FONT_LABEL_M, buf, 124, 154 + r * 16);
    }
    int px = n > 1 ? 100 : 160;
    drawRect(px - 44, 208, 88, 18, s_ta.button == 0 ? 200 : 0, s_ta.button == 0 ? 160 : 0, s_ta.button == 0 ? 0 : 40, 0);
    drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_PLAY), px, 212, 1);
    if (n > 1) {
        drawRect(176, 208, 88, 18, s_ta.button == 1 ? 200 : 0, s_ta.button == 1 ? 160 : 0, s_ta.button == 1 ? 0 : 40, 0);
        drawText(mp, PS1_FONT_LABEL_M, PS1MenuString(PS1_STR_NEXT_ACT), 220, 212, 1);
    }
    if (s_ta.flash)
        return;
    if (ok && s_ta.button == 1) {
        PlaySfxByName("Menu Move", false);
        s_ta.act = (s_ta.act + 1) % n;
    }
    else if (ok) {
        PlaySfxByName("Menu Select", false);
        taPlay();
    }
    else if (back) {
        PlaySfxByName("Menu Back", false);
        go(PS1_MENU_TIMEATTACK, z);
    }
}

#endif
}  // namespace

volatile uint32_t g_ps1TAZone = 0, g_ps1TAAct = 0, g_ps1TARank = 0;

void PS1MenuResetGame() {
    if (s_ta.pending) {
        PS1MenuOpen(PS1_MENU_RECORDS);
        s_ta.button = 0;
        taShowResults();
        g_ps1TARank = s_ta.rank;
    }
    else
        PS1MenuOpen(PS1_MENU_MAIN);
}

void PS1MenuOpen(int screen) {
    if (!g_ps1MenuScreens.imageCount) {
        StopMusic(true); // the game's track (ENDGAME / RESETGAME): the menus play theirs after the load
        PS1LoadingBegin();
        PS1MenuPackLoad(g_ps1MenuScreens, "Data/Game/Menu/PS1Menu.pmn", s_screensMem, sizeof(s_screensMem));
        PS1SfxLoadMenuBank(); // the menus' sounds into the stage's SPU area (no stage runs under the menus)
        PS1LoadingEnd();
    }
    s_m          = MenuState();
    s_m.saveSlot = -1;
    go(screen);
    Engine.gameMode = ENGINE_PS1MENU;
    if (screen == PS1_MENU_TITLE) { // TitleScreen.cpp: the intro jingle once
        SetMusicTrack("MenuIntro.ogg", 0, false, 0);
        SetMusicTrack("MainMenu.ogg", 1, true, 106596);
        PlayMusic(0, 0);
    }
    else
        playMainMenuMusic(); // FadeScreen MENUFADEIN: back from a game
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
    if ((s_m.screen == PS1_MENU_TIMEATTACK || s_m.screen == PS1_MENU_RECORDS) && !s_taPack.imageCount) {
        PS1MusicHold(); // one CD action at a time: the menu track resumes at its row after the load
        PS1LoadingBegin(); // 256 KB of act pictures: the loading indicator while they upload
        PS1MenuPackLoad(s_taPack, "Data/Game/Menu/PS1TimeAttack.pmn", s_taMem, sizeof(s_taMem));
        PS1LoadingEnd();
        PS1MusicRestore();
    }
    PS1RenderMenuFrame(true);
    drawBackground();
    int screen = s_m.screen;
    switch (screen) {
        case PS1_MENU_TITLE: titleScreen(ok, back); break;
        case PS1_MENU_MAIN: mainScreen(moveY, ok, back); break;
        case PS1_MENU_SAVE: saveScreen(moveY, moveX, ok, back, keyPress.select); break;
        case PS1_MENU_PLAYER: playerScreen(moveX, ok, back); break;
        case PS1_MENU_OPTIONS: optionsScreen(moveY, moveX, ok, back); break;
        case PS1_MENU_TIMEATTACK: timeAttackScreen(moveY, ok, back); break;
        case PS1_MENU_RECORDS: recordsScreen(moveX, ok, back); break;
    }
    g_ps1TAZone = s_ta.zone;
    g_ps1TAAct  = s_ta.act;
    if (Engine.gameMode != ENGINE_PS1MENU) { // a game started: the stage load replaces the menu art in VRAM
        g_ps1MenuScreens = PS1MenuPack();
        s_taPack         = PS1MenuPack();
        return;
    }
    if (screen == PS1_MENU_TITLE) // build stamp (ps1/version_stamp.hh): the title screen only, bottom-left, under the fade
        drawText(g_ps1MenuScreens, PS1_FONT_LABEL_M, PS1VersionText(), 8, 240 - 8 - 15, 0, 0x50); // light grey
    if (s_m.fade > 0) { // fade in after a screen change: black at 75 / 50 % (two B/2 + F/2 passes = 25 %)
        int passes = s_m.fade > 4 ? 2 : 1;
        for (int i = 0; i < passes; ++i) drawRect(0, 0, 320, 240, 0, 0, 0, 0);
        --s_m.fade;
    }
    g_ps1MenuScreen = s_m.screen;
    g_ps1MenuSel    = s_m.sel;
}

// ---------------------------------------------------------------------------------------------------
// Pause menu (PauseMenu.cpp): Continue, Restart, Exit, the last two behind a yes / no dialog (DialogPanel), over the
// frozen stage. Only the resident pack (fonts in VRAM rows 242-255) and flat rectangles: every stage's VRAM stays as
// it is. The first frame dims the stage once (half brightness); later frames repeat it and redraw the opaque panel.

volatile uint32_t g_ps1PauseSel = 0, g_ps1PauseDialog = 0;

namespace {
struct PauseState {
    int sel, dialog, dialogSel, frames;
    bool restartOff;
} s_p;
enum { PAUSE_DLG_NONE, PAUSE_DLG_RESTART, PAUSE_DLG_EXIT };
// One panel for the menu and the dialogs: each frame repeats the last one (PS1ChainRepeatLastFrame), so the panel
// always covers what the previous frame drew over the dimmed stage.
enum { PANEL_X = 30, PANEL_Y = 60, PANEL_W = 260, PANEL_H = 120 };
}  // namespace

void PS1PauseOpen() {
    PauseSound();
    s_p = PauseState();
    // PauseMenu.cpp: Restart greyed on the last life (no save / save), outside the regular lists, in attract / VS mode
    s_p.restartOff = (GetGlobalVariableByName("player.lives") <= 1 && GetGlobalVariableByName("options.gameMode") <= 1)
                     || !activeStageList || GetGlobalVariableByName("options.attractMode") == 1
                     || GetGlobalVariableByName("options.vsMode") == 1;
    Engine.gameMode = ENGINE_PS1PAUSE;
}

void PS1PauseFrame() {
    CheckKeyPress(&keyPress);
    const PS1MenuPack &rp = g_ps1MenuResident;
    int moveY = keyPress.down - keyPress.up, moveX = keyPress.right - keyPress.left;
    bool ok = keyPress.start || keyPress.A, back = keyPress.B || keyPress.X;
    PS1RenderMenuFrame(false); // the frozen stage (the last frame repeated)
    if (!s_p.frames++)
        drawRect(0, 0, 320, 240, 0, 0, 0, 0); // dim it once: B/2
    if (s_p.dialog == PAUSE_DLG_NONE) {
        if (moveY)
            PlaySfxByName("Menu Move", false);
        s_p.sel = (s_p.sel + moveY + 3) % 3;
        drawRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 16, 40, 150);
        drawText(rp, PS1_FONT_HEADING_S, PS1MenuString(PS1_STR_PAUSE), 160, 70, 1);
        static const int items[3] = { PS1_STR_CONTINUE, PS1_STR_RESTART, PS1_STR_EXIT };
        for (int i = 0; i < 3; ++i) {
            int y = 102 + i * 22;
            if (i == s_p.sel)
                drawRect(90, y - 3, 140, 18, 170, 130, 0);
            drawText(rp, PS1_FONT_LABEL_S, PS1MenuString(items[i]), 160, y, 1);
            if (i == 1 && s_p.restartOff) // greyed (the native tints it 0x80): the text dimmed
                drawRect(90, y - 3, 140, 18, 16, 40, 150, 0);
        }
        if (ok) {
            if (s_p.sel == 0) { // PAUSEMENU_STATE_CONTINUE
                PlaySfxByName("Resume", false);
                ResumeSound();
                Engine.gameMode = ENGINE_MAINGAME;
            }
            else if (s_p.sel == 1 && !s_p.restartOff) {
                PlaySfxByName("Menu Select", false);
                s_p.dialog    = PAUSE_DLG_RESTART;
                s_p.dialogSel = 1;
            }
            else if (s_p.sel == 2) {
                PlaySfxByName("Menu Select", false);
                s_p.dialog    = PAUSE_DLG_EXIT;
                s_p.dialogSel = 1;
            }
        }
        else if (back) {
            PlaySfxByName("Resume", false);
            ResumeSound();
            Engine.gameMode = ENGINE_MAINGAME;
        }
    }
    else {
        if (moveX)
            PlaySfxByName("Menu Move", false);
        s_p.dialogSel = (s_p.dialogSel + moveX + 2) & 1;
        drawRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 16, 40, 150);
        drawText(rp, PS1_FONT_TEXT_S, PS1MenuString(s_p.dialog == PAUSE_DLG_RESTART ? PS1_STR_RESTART_MESSAGE : PS1_STR_EXIT_MESSAGE),
                 160, 80, 1);
        for (int i = 0; i < 2; ++i) {
            int x = i ? 200 : 120;
            if (i == s_p.dialogSel)
                drawRect(x - 34, 145, 68, 18, 170, 130, 0);
            drawText(rp, PS1_FONT_LABEL_S, PS1MenuString(i ? PS1_STR_NO : PS1_STR_YES), x, 148, 1);
        }
        if (ok && s_p.dialogSel == 0) {
            PlaySfxByName("Menu Select", false);
            if (s_p.dialog == PAUSE_DLG_RESTART) { // PAUSEMENU_STATE_RESTART, DLG_YES
                Engine.gameMode = ENGINE_EXITPAUSE;
                stageMode       = STAGEMODE_LOAD;
                if (GetGlobalVariableByName("options.gameMode") <= 1)
                    SetGlobalVariableByName("player.lives", GetGlobalVariableByName("player.lives") - 1);
                if (activeStageList != STAGELIST_SPECIAL) {
                    SetGlobalVariableByName("lampPostID", 0);
                    SetGlobalVariableByName("starPostID", 0);
                }
                StopMusic(true);
            }
            else // PAUSEMENU_STATE_EXIT, DLG_YES (the menu path, not skipStartMenu)
                Engine.gameMode = (GetGlobalVariableByName("options.gameMode") > 1) + ENGINE_ENDGAME;
        }
        else if ((ok && s_p.dialogSel == 1) || back) {
            PlaySfxByName("Menu Select", false);
            s_p.dialog = PAUSE_DLG_NONE;
        }
    }
    g_ps1PauseSel    = s_p.sel;
    g_ps1PauseDialog = s_p.dialog;
}
