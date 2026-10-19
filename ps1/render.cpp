// RSDKv4 (Sonic 2) PS1 renderer: replaces RSDKv4/Drawing.cpp (the PC software renderer) on PS1. From the Sonic CD
// port's ps1/render.cpp (RSDKv3-ps1 1e583ad; docs/30 phase 3.2), bound to the v4 engine.
//
// Everything a frame draws is DMA-chained (ps1/gpu_chain.hh, the Nexus port's chain): main.cpp opens the
// chain around Engine.RunFrame(), psyqo sends it after frame() returns. Sprites come from the stage's
// VRAM atlas (ps1/sprite_atlas, tools/atlas), tiles from the stage's tile pages (ps1/tileset,
// tools/tiles): nothing is decoded at runtime.
//
// Upstream Drawing.cpp functions also change game state, not just pixels (phase 2.3: the title card
// works from its draw sub). Every such side effect is ported here explicitly, marked "upstream": draw
// subs (DrawObjectList), draw order + water position (DrawStageGFX, v4: by tLayerMidPoint), the BG
// layers' autoscroll and the parallax line positions (DrawHLineScrollLayer / DrawVLineScrollLayer, v4's
// SW path; v4 advances the parallax scrollPos in Scene.cpp), the view globals (InitRenderDevice),
// DrawObjectAnimation's dispatch. Not ported: the SW path's per-line `activePalette` (each drawn line
// points it at its bank): on PS1 it stays at gfxLineBuffer[0]'s bank (SetActivePalette), as the mobile
// game's GL renderer leaves it; it only matters for SetPaletteEntry(-1) (the next zone's tile colours).
//
// Scope (docs/30 phase 3): chain + clear + palette (fades, per-line banks), atlas sprites (plain / flipped / scaled /
// rotated / rotozoom / ink), HScroll tile layers from the packed chunk table (Scene.hpp Tiles128x128::ps1Tile) and
// BG line strips, text menus (phase 4: the level select's DrawMenu, upstream's code). Not yet (counted in
// g_ps1DrawTodo): tint, VScroll layers, 3D faces (phase 7). Sonic 2 has no 3D floor / sky layer.
#include "sprite_quad.hh"
#include "RetroEngine.hpp"

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <stdio.h>
#include <string.h>

#include <psyqo/alloc.h>

#include <psyqo/gpu.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/sprites.hh>

#include "common/syscalls/syscalls.h"
#include "gpu_chain.hh"
#include "sprite_atlas.hh"
#include "tileset.hh"
#include "video.hh" // PS1Hblanks (profiling)

extern psyqo::GPU *g_ps1GPU;

// ---------------------------------------------------------------------------------------------------
// Engine globals owned by Drawing.cpp upstream

int SCREEN_XSIZE_CONFIG = DEFAULT_SCREEN_XSIZE;
int SCREEN_XSIZE        = DEFAULT_SCREEN_XSIZE; // 320 (user decision; PC 424)
int SCREEN_CENTERX      = DEFAULT_SCREEN_XSIZE / 2;

float SCREEN_XSIZE_F   = DEFAULT_SCREEN_XSIZE;
float SCREEN_CENTERX_F = DEFAULT_SCREEN_XSIZE / 2;
float SCREEN_YSIZE_F   = SCREEN_YSIZE;
float SCREEN_CENTERY_F = SCREEN_YSIZE / 2;

DrawListEntry drawListEntries[DRAWLAYER_COUNT];

int gfxDataPosition;
GFXSurface gfxSurface[SURFACE_COUNT];
byte graphicData[GFXDATA_SIZE];

// Evidence for draw-sub changes (native opcodes, tools/logic_test.sh): a hash of every DrawSprite call the scripts
// make in a frame and their count (phase 2's headless renderer; the same calls are hashed here).
volatile uint32_t g_ps1DrawHash = 0, g_ps1DrawCount = 0, g_ps1DrawHashLast = 0, g_ps1DrawCountLast = 0;

// ---------------------------------------------------------------------------------------------------
// Verification counters (GDB)

volatile uint32_t g_ps1DrawFrames     = 0; // frames drawn by the stage renderer (clear + scene)
volatile uint32_t g_ps1RepeatFrames   = 0; // frames that kept the last image (nothing or only objects drawn)
volatile uint32_t g_ps1DrawSprites    = 0; // atlas sprite pieces chained this frame
volatile uint32_t g_ps1DrawQuads      = 0; // textured quads chained this frame (flip / scale / rotation)
volatile uint32_t g_ps1LayerPrims     = 0; // tile-layer primitives chained this frame
volatile uint32_t g_ps1LayerHbl       = 0; // CPU time of this frame's tile layers (hblanks)
volatile uint32_t g_ps1StripBandCount = 0; // strip bands drawn this frame (profiling)
volatile uint32_t g_ps1TileBandCount  = 0; // tile bands drawn this frame (profiling)
volatile uint32_t g_ps1ProfObjDrawHbl = 0; // this frame: object draw subs (script VM + their Draw* calls)
volatile uint32_t g_ps1ProfFloorsHbl  = 0; // this frame: every 3D floor / sky layer
volatile uint32_t g_ps1ProfLayer[5]   = {}; // this frame: FG (uniform) layers, per-line passes, tile bands, strip bands (hblanks), HScroll calls
volatile uint32_t g_ps1ProfSlot[4]    = {}; // this frame: hblanks per draw slot (layerID 0-3)
volatile uint32_t g_ps1TileFlipRects  = 0; // flipped tiles drawn as flip-bit sprites (total, docs/43 G3)
volatile uint32_t g_ps1TileFlipped    = 0; // this frame: tiles drawn as flipped / partial quads or sprites (slow path)
volatile uint32_t g_ps1AtlasDrawMiss  = 0; // sprite texels in no atlas cluster
volatile int32_t g_ps1MissSheet = -1, g_ps1MissX = 0, g_ps1MissY = 0; // the last miss (atlas sheet, texel)
volatile uint32_t g_ps1SheetUnbound   = 0; // draws from a sheet the stage atlas doesn't have
volatile uint32_t g_ps1DrawTodo       = 0; // draw calls of later steps (ink effects, faces, text...), skipped
volatile uint32_t g_ps1SurfaceGen     = 1; // bumped by Sprite.cpp when a sheet name is (un)registered
volatile uint8_t g_ps1DebugDrawMask   = 0; // bit 0: drop object-list primitives (layers-only frames); bit 1: tile bands chain nothing (profiling)

// ---------------------------------------------------------------------------------------------------
// Palette: master CLUT (256 x 15-bit) at (0, 240), the atlas' 4-bit CLUTs (16 entries of master
// indices each) where the atlas put them. As on PC (FlipScreen converts the palette after drawing),
// the rows are chained at the start of the frame and filled again at FlipScreen with the palette as
// it stands when drawing is done.

#define PS1_CLUT_X 0
#define PS1_CLUT_Y 240
#define PS1_PAL_ENTRIES (256 + PS1_ATLAS_MAX_CLUTS * 16)

static uint16_t s_pal[PS1_PAL_ENTRIES];
struct PS1PalChunk {
    uint16_t *dst;
    uint16_t src, n;
};
static PS1PalChunk s_palChunks[128];
static int s_palChunkCount = 0;

// Every visible entry carries the STP bit (0x8000): textured primitives drawn semi-transparent then
// blend all their texels (psx-spx: only STP texels blend), opaque draws are unaffected; 0x0000 stays
// transparent (index 0).
static inline uint16_t PS1Conv(int r, int g, int b) { return (uint16_t)(0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }

// A full-screen alpha DrawRectangle that ends the frame (PC's screen fades) is applied to the CLUTs
// and the clear colour at FlipScreen: every pixel is CLUT-based, so this is PC's per-pixel blend
// c * (255 - a) + fade * a, exactly (user decision 2026-09-24). If anything is drawn after it, the
// chain hook draws it as a 4-level alpha rectangle instead (PS1FadeHook).
static bool s_fadePending = false;
static int s_fadeR, s_fadeG, s_fadeB, s_fadeA;
static psyqo::Prim::FastFill *s_clearPrim = nullptr;
volatile uint32_t g_ps1ClutFades = 0; // frames whose fade went into the CLUTs (exact)

// Direct-colour primitives of the frame (flat rectangles such as the Title's background, flat 3D faces
// such as the menu's panels and the special stage's half-pipe, the ClearScreen rectangle): the CLUT fade doesn't
// reach them, so FlipScreen applies the same blend to their colour words, still in the chain there (psyqo sends it
// after flip()). Recorded as runs: `count` primitives `stride` words apart, the colour in each one's first word
// (PS1FadedFaces' bulk runs: 128 faces an entry; single primitives chained back to back merge into one run). Before,
// the half-pipe's faces were not recorded at all and the special stage's fades left them at full colour.
struct PS1DirectRun {
    uint32_t *word;
    uint16_t count, stride;
};
#define PS1_DIRECT_RUNS 96
static PS1DirectRun s_directRuns[PS1_DIRECT_RUNS];
static int s_directCount = 0;
volatile uint32_t g_ps1FadeDirect = 0, g_ps1FadeDirectDropped = 0; // colours faded / not recorded (list full)

void PS1FadeRecordRun(uint32_t *first, int count, int stride)
{
    if (count <= 0)
        return;
    if (s_directCount) {
        PS1DirectRun &r = s_directRuns[s_directCount - 1];
        if (r.stride == stride && r.word + r.count * stride == first && r.count + count <= 0xFFFF) {
            r.count = (uint16_t)(r.count + count);
            return;
        }
    }
    if (s_directCount < PS1_DIRECT_RUNS)
        s_directRuns[s_directCount++] = PS1DirectRun{ first, (uint16_t)count, (uint16_t)stride };
    else
        g_ps1FadeDirectDropped = g_ps1FadeDirectDropped + count;
}

// One flat primitive (w[0] = GP0 command | BGR) written straight into the chain, its colour recorded.
static void PS1ChainFlatRaw(const uint32_t *w, int words)
{
    uint32_t *d = PS1ChainRawBegin(words);
    if (!d)
        return;
    for (int i = 0; i < words; ++i) d[i] = w[i];
    PS1ChainRawEnd();
    PS1FadeRecordRun(d, 1, words + 1); // + 1: the next fragment's chain tag
}

static void PS1BuildPalette(int bank)
{
    const PaletteEntry *pal = fullPalette32[bank];
    s_pal[0]                = 0x0000;                          // index 0 = transparent (tiles and sprites)
    if (s_fadePending) {
        int a = s_fadeA, na = 255 - a;
        for (int i = 1; i < 256; ++i)
            s_pal[i] = PS1Conv((pal[i].r * na + s_fadeR * a) / 255, (pal[i].g * na + s_fadeG * a) / 255, (pal[i].b * na + s_fadeB * a) / 255);
    }
    else { // from the RGB565 copy every palette writer keeps in step (Palette.hpp): one load per colour
        const ushort *p565 = fullPalette[bank];
        for (int i = 1; i < 256; ++i) {
            uint32_t c = p565[i]; // b5 | g6 << 5 | r5 << 11 -> PS1Conv's r5 | (g >> 3) << 5 | b5 << 10
            s_pal[i]   = (uint16_t)(0x8000 | (c >> 11) | ((c >> 1) & 0x3E0) | ((c & 0x1F) << 10));
        }
    }
    for (int k = 0; k < g_ps1AtlasClutCount; ++k) {
        const uint8_t *map = g_ps1AtlasCluts[k].map;
        uint16_t *dst      = &s_pal[256 + k * 16];
        dst[0]             = 0x0000;
        for (int i = 1; i < 16; ++i) dst[i] = s_pal[map[i]];
    }
}

static void PS1ReserveRow(int src, int count, int16_t x, int16_t y)
{
    uint16_t *data[32];
    uint16_t len[32];
    int n = PS1ChainReserveRow(count, x, y, data, len, 32);
    for (int i = 0; i < n && s_palChunkCount < 128; ++i) {
        s_palChunks[s_palChunkCount++] = PS1PalChunk{ data[i], (uint16_t)src, len[i] };
        src += len[i];
    }
}

#if PS1_GAME == 1
// Sonic 1 (docs/37): the converted palette of a bank (colours + the atlas / strip 4-bit CLUTs, ~5 hblanks to build) is
// kept in one of two slots (Labyrinth's water draws two banks a frame) with the RGB565 colours it came from. Each fill
// compares the bank's colours with them (word by word: every palette writer is covered) and converts only the entries
// that changed, then the CLUT maps if any did (palette cycling touches a few entries a frame); a slot built for another
// atlas load / CLUT count, or a new bank, is converted whole; a fade in progress is always converted (PS1BuildPalette).
struct PS1PalCache {
    int bank, cluts;
    uint32_t atlas;
    bool valid;
    alignas(4) uint16_t src[256];
    uint16_t out[PS1_PAL_ENTRIES];
};
static PS1PalCache s_palCache[2];
static int s_palCacheNext = 0;
volatile uint32_t g_ps1PalCacheHits = 0, g_ps1PalCacheMisses = 0, g_ps1PalCacheBuilds = 0; // GDB: same / some entries / whole
static inline uint16_t PS1Conv565(uint32_t c) // PS1BuildPalette's conversion of one RGB565 colour
{
    return (uint16_t)(0x8000 | (c >> 11) | ((c >> 1) & 0x3E0) | ((c & 0x1F) << 10));
}
static void PS1PalCacheMaps(uint16_t *out)
{
    for (int k = 0; k < g_ps1AtlasClutCount; ++k) {
        const uint8_t *map = g_ps1AtlasCluts[k].map;
        uint16_t *dst      = &out[256 + k * 16];
        dst[0]             = 0x0000;
        for (int i = 1; i < 16; ++i) dst[i] = out[map[i]];
    }
}
static const uint16_t *PS1PaletteFor(int bank)
{
    if (s_fadePending) {
        PS1BuildPalette(bank);
        return s_pal;
    }
    const ushort *now = fullPalette[bank];
    PS1PalCache *slot = nullptr;
    for (int k = 0; k < 2 && !slot; ++k)
        if (s_palCache[k].valid && s_palCache[k].bank == bank)
            slot = &s_palCache[k];
    if (slot && slot->atlas == g_ps1AtlasLoads && slot->cluts == g_ps1AtlasClutCount) {
        bool changed = false;
        if (!(reinterpret_cast<uintptr_t>(now) & 3)) {
            const uint32_t *cur = reinterpret_cast<const uint32_t *>(now);
            uint32_t *old       = reinterpret_cast<uint32_t *>(slot->src);
            for (int w = 0; w < 128; ++w) {
                if (old[w] == cur[w])
                    continue;
                old[w]  = cur[w];
                changed = true;
                for (int i = 2 * w; i < 2 * w + 2; ++i)
                    if (i)
                        slot->out[i] = PS1Conv565(now[i]); // entry 0 stays transparent
            }
        }
        else {
            for (int i = 0; i < 256; ++i) {
                if (slot->src[i] == now[i])
                    continue;
                slot->src[i] = now[i];
                changed      = true;
                if (i)
                    slot->out[i] = PS1Conv565(now[i]);
            }
        }
        if (changed) {
            PS1PalCacheMaps(slot->out);
            g_ps1PalCacheMisses = g_ps1PalCacheMisses + 1;
        }
        else
            g_ps1PalCacheHits = g_ps1PalCacheHits + 1;
        return slot->out;
    }
    if (!slot) {
        slot           = &s_palCache[s_palCacheNext];
        s_palCacheNext = s_palCacheNext ^ 1;
    }
    g_ps1PalCacheBuilds = g_ps1PalCacheBuilds + 1;
    uint16_t *out       = slot->out;
    out[0]              = 0x0000;
    for (int i = 1; i < 256; ++i) out[i] = PS1Conv565(now[i]);
    PS1PalCacheMaps(out);
    memcpy(slot->src, now, sizeof(slot->src));
    slot->bank  = bank;
    slot->atlas = g_ps1AtlasLoads;
    slot->cluts = g_ps1AtlasClutCount;
    slot->valid = true;
    return out;
}
#endif

#if PS1_GAME != 1
// Sonic 2 (docs/43: ~4 hblanks a frame converted all 255 colours): s_pal kept with the RGB565 colours it came from, as
// Sonic 1's slots but one (s_pal itself). A fill of the same bank, atlas load and CLUT count converts only the entries
// that changed and redoes the CLUT maps if any did; anything else, or a fade in progress, builds it whole.
static uint16_t s_palSrc[256];
static int s_palSrcBank = -1, s_palSrcCluts = -1;
static uint32_t s_palSrcAtlas = 0;
static uint s_palSrcVersion = 0; // g_ps1PalVersion when s_palSrc was last compared (no write since: nothing changed)
__attribute__((noinline, optimize("Os"))) static void PS1BuildPaletteCached(int bank)
{
    if (s_fadePending || bank != s_palSrcBank || g_ps1AtlasLoads != s_palSrcAtlas || g_ps1AtlasClutCount != s_palSrcCluts) {
        PS1BuildPalette(bank);
        if (s_fadePending) {
            s_palSrcBank = -1;
            return;
        }
        memcpy(s_palSrc, fullPalette[bank], sizeof(s_palSrc));
        s_palSrcBank = bank, s_palSrcAtlas = g_ps1AtlasLoads, s_palSrcCluts = g_ps1AtlasClutCount;
        s_palSrcVersion = g_ps1PalVersion;
        return;
    }
    if (s_palSrcVersion == g_ps1PalVersion) // no palette write since the last compare: s_palSrc is the palette
        return;
    s_palSrcVersion    = g_ps1PalVersion;
    const ushort *p565 = fullPalette[bank];
    bool changed       = false;
    for (int i = 1; i < 256; ++i) {
        uint32_t c = p565[i];
        if (c == s_palSrc[i])
            continue;
        s_palSrc[i] = (uint16_t)c;
        s_pal[i]    = (uint16_t)(0x8000 | (c >> 11) | ((c >> 1) & 0x3E0) | ((c & 0x1F) << 10)); // PS1BuildPalette's
        changed     = true;
    }
    if (!changed)
        return;
    for (int k = 0; k < g_ps1AtlasClutCount; ++k) {
        const uint8_t *map = g_ps1AtlasCluts[k].map;
        uint16_t *dst      = &s_pal[256 + k * 16];
        dst[0]             = 0x0000;
        for (int i = 1; i < 16; ++i) dst[i] = s_pal[map[i]];
    }
}
#endif

// Fills chunks [first, end) (end < 0: up to s_palChunkCount) with `bank`'s palette.
static void PS1FillPalette(int bank, int first = 0, int end = -1)
{
#if PS1_GAME == 1
    const uint16_t *pal = PS1PaletteFor(bank);
    if (end < 0)
        end = s_palChunkCount;
    for (int i = first; i < end; ++i) memcpy(s_palChunks[i].dst, &pal[s_palChunks[i].src], s_palChunks[i].n * sizeof(uint16_t));
#else
    PS1BuildPaletteCached(bank);
    if (end < 0)
        end = s_palChunkCount;
    for (int i = first; i < end; ++i) memcpy(s_palChunks[i].dst, &s_pal[s_palChunks[i].src], s_palChunks[i].n * sizeof(uint16_t));
#endif
}

// Master + atlas 4-bit CLUT rows, chained here (filled by PS1FillPalette); dy > 0: the bottom bank's copy (set B).
static void PS1ReservePaletteRows(int dy = 0)
{
    PS1ReserveRow(0, 256, PS1_CLUT_X, (int16_t)(PS1_CLUT_Y + dy));
    for (int k = 0; k < g_ps1AtlasClutCount;) { // runs of adjacent CLUT slots in one VRAM row
        int run = 1;
        while (k + run < g_ps1AtlasClutCount && g_ps1AtlasCluts[k + run].y == g_ps1AtlasCluts[k].y
               && g_ps1AtlasCluts[k + run].x == g_ps1AtlasCluts[k].x + 16 * run)
            ++run;
        PS1ReserveRow(256 + k * 16, run * 16, (int16_t)g_ps1AtlasCluts[k].x, (int16_t)(g_ps1AtlasCluts[k].y + dy));
        k += run;
    }
}

// ---------------------------------------------------------------------------------------------------
// Draw-time palette-bank split (docs/30 phase 3.6). Upstream's SW renderer picks each line's bank
// (gfxLineBuffer, SetActivePalette: the water) as it draws. When the lines form two runs (a top bank
// over [0, y0), a bottom bank over [y0, 240): every water frame), the bottom run is drawn with a second
// copy of the CLUTs ("set B", PS1_CLUT_B_DY rows below set A: master row 497, the atlas' CLUT rows
// 240-241 -> 497-498): primitives below y0 use set B, sprites and tile / strip bands that cross y0 are cut
// there, quads that cross it are drawn twice under a drawing area. The bank state is the one at each
// primitive's draw call, as upstream (the Player's draw sub moves the water line mid-frame). Any other
// layout (> 2 runs, a bank change under set B, atlas CLUTs past row 241) falls back to the region
// replay at FlipScreen (PS1DrawBankRegions), with both sets filled per region.
#define PS1_CLUT_B_DY 257
static int s_splitY      = 0;     // first line of the bottom bank now (0 = one bank)
static int s_splitTop    = 0;     // banks above / below s_splitY
static int s_splitBottom = 0;
static bool s_splitComplex = false; // this frame falls back to the replay
static bool s_setB         = false; // set B's rows are chained this frame
static int s_setAEnd = 0, s_setBFirst = 0, s_setBEnd = 0; // palette chunk ranges of the two sets
static uint32_t s_splitAtlas = 0xFFFFFFFF;
static bool s_splitFits      = false;
volatile uint32_t g_ps1SplitFrames = 0, g_ps1SplitCuts = 0, g_ps1SplitQuads = 0; // split frames / cut primitives / doubled quads (total)
volatile uint32_t g_ps1SplitFallbacks = 0; // frames that fell back to the replay

static bool PS1SplitFits()
{
    if (s_splitAtlas != g_ps1AtlasLoads) {
        s_splitAtlas = g_ps1AtlasLoads;
        s_splitFits  = true;
        for (int k = 0; k < g_ps1AtlasClutCount; ++k)
            if (g_ps1AtlasCluts[k].y < PS1_CLUT_Y || g_ps1AtlasCluts[k].y > PS1_CLUT_Y + 1)
                s_splitFits = false;
    }
    return s_splitFits;
}

// The split from gfxLineBuffer as it is now.
static void PS1SplitUpdate()
{
    typedef uint32_t __attribute__((may_alias)) PS1Word;
    const PS1Word *lw = (const PS1Word *)gfxLineBuffer; // aligned(4) on PS1 (Palette.cpp)
    int b0            = gfxLineBuffer[0];
    uint32_t w0       = b0 * 0x01010101u;
    int i             = 0;
    while (i < SCREEN_YSIZE / 4 && lw[i] == w0) ++i;
    if (i == SCREEN_YSIZE / 4) {
        s_splitY = 0;
        return;
    }
    int y = i * 4;
    while (gfxLineBuffer[y] == b0) ++y;
    int b1       = gfxLineBuffer[y];
    uint32_t w1  = b1 * 0x01010101u;
    bool twoRuns = true;
    for (int l = y; l < SCREEN_YSIZE && twoRuns; ++l) {
        if ((l & 3) == 0 && l + 4 <= SCREEN_YSIZE) {
            twoRuns = lw[l >> 2] == w1;
            l += 3;
        }
        else
            twoRuns = gfxLineBuffer[l] == b1;
    }
    if (!twoRuns || !PS1SplitFits() || (s_setB && (b0 != s_splitTop || b1 != s_splitBottom))) {
        s_splitComplex = true;
        s_splitY       = 0;
        return;
    }
    s_splitY      = y;
    s_splitTop    = b0;
    s_splitBottom = b1;
}

// Set B's CLUT rows, chained before the first primitive that uses them.
static void PS1EnsureSetB()
{
    if (s_setB)
        return;
    s_setB      = true;
    s_setBFirst = s_palChunkCount;
    PS1ReservePaletteRows(PS1_CLUT_B_DY);
    s_setBEnd = s_palChunkCount; // filled at FlipScreen (the chain is sent after the frame)
    PS1ChainFlushCache();
    g_ps1SplitFrames = g_ps1SplitFrames + 1;
}

// ---------------------------------------------------------------------------------------------------
// Frame: main.cpp calls PS1RenderBeginFrame / PS1RenderEndFrame around Engine.RunFrame(). The first
// draw of a frame starts it: DrawStageGFX clears; anything else (the paused stage mode draws only its
// object lists over the last image, like PC's persistent framebuffer) repeats the last frame first.

static bool s_chainOpen  = false;
static uint32_t *s_sceneMark = nullptr; // chain position after the clear and the frame-start CLUT rows
static bool s_frameOpen  = false;

// SetActivePalette (Palette.hpp) changed gfxLineBuffer: the next draw calls follow it.
void PS1PaletteLinesChanged()
{
    if (s_frameOpen && !s_splitComplex)
        PS1SplitUpdate();
}
static bool s_frameFresh = false; // this frame started with a clear

static void PS1ClearClusterCache();

static void PS1FrameStart(bool clear)
{
    if (s_frameOpen || !s_chainOpen)
        return;
    s_frameOpen      = true;
    s_frameFresh     = clear;
    g_ps1DrawSprites = 0;
    g_ps1DrawQuads   = 0;
    g_ps1LayerPrims  = 0;
    g_ps1LayerHbl    = 0;
    for (int i = 0; i < 5; ++i) g_ps1ProfLayer[i] = 0;
    g_ps1ProfObjDrawHbl = 0;
    g_ps1ProfFloorsHbl  = 0;
    for (int i = 0; i < 4; ++i) g_ps1ProfSlot[i] = 0;
    g_ps1TileFlipped = 0;
    g_ps1StripBandCount = 0;
    g_ps1TileBandCount  = 0;
    s_fadePending = false;
    s_clearPrim   = nullptr;
    s_directCount = 0;
    if (clear) {
        s_clearPrim = PS1ChainClear(0, 0, 0);
        g_ps1DrawFrames = g_ps1DrawFrames + 1;
    }
    else {
        PS1ChainRepeatLastFrame();
        g_ps1RepeatFrames = g_ps1RepeatFrames + 1;
    }
    s_palChunkCount = 0;
    PS1ReservePaletteRows(); // filled at FlipScreen, or at PS1RenderEndFrame for a frame that only repeats
    s_setAEnd = s_palChunkCount;
    s_setB         = false;
    s_splitComplex = false;
    PS1SplitUpdate();
    PS1ChainFlushCache(); // the GPU caches CLUTs
    PS1ChainInvalidateTPage();
    s_sceneMark = PS1ChainMark();
}

void PS1RenderBeginFrame(psyqo::GPU &gpu)
{
    PS1ChainBegin(gpu);
    PS1AtlasServicePictures(); // on-demand pictures the last frame asked for (nothing chained yet)
    s_chainOpen = true;
    s_frameOpen = false;
}

// The PS1 menus (ps1/menu.cpp) open their frame here: cleared (menu screens) or the last frame repeated (the pause
// menu over the frozen stage). Palette rows as any frame (the menus use their own CLUT rows).
void PS1RenderMenuFrame(bool clear)
{
    PS1FrameStart(clear);
    PS1FillPalette(gfxLineBuffer[0]); // no FlipScreen on menu frames: the stage rows as a repeated frame
}

void PS1RenderEndFrame()
{
    PS1ChainTPageNoFlip(); // the next frame / an immediate draw (loading icon, FMV) starts without flip bits (G3)
    if (!s_frameOpen) {
        PS1FrameStart(false); // nothing drawn: keep the image in both buffers
        PS1FillPalette(gfxLineBuffer[0]);
    }
    PS1ChainEnd();
    s_chainOpen = false;
}

// ---------------------------------------------------------------------------------------------------
// Sheets -> atlas sheets. gfxSurface[] holds the names the scripts registered (Sprite.cpp); the atlas
// sheet of each is looked up again whenever a name or the atlas changes.

static int16_t s_surfAtlas[SURFACE_COUNT];
static uint32_t s_boundGen = 0, s_boundAtlas = 0xFFFFFFFF;

static int PS1SurfaceSheet(int sheetID)
{
    if (s_boundGen != g_ps1SurfaceGen || s_boundAtlas != g_ps1AtlasLoads) {
        s_boundGen   = g_ps1SurfaceGen;
        s_boundAtlas = g_ps1AtlasLoads;
        for (int s = 0; s < SURFACE_COUNT; ++s)
            s_surfAtlas[s] = gfxSurface[s].fileName[0] && g_ps1AtlasSheetCount ? (int16_t)PS1AtlasFindSheet(gfxSurface[s].fileName) : (int16_t)-1;
        PS1ClearClusterCache();
    }
    if ((uint)sheetID >= SURFACE_COUNT)
        return -1;
    return s_surfAtlas[sheetID];
}

struct PS1ClusterCacheEntry {
    int16_t sheet, x, y, index;
};
static PS1ClusterCacheEntry s_clusterCache[64];
static const PS1AtlasCluster *s_faceCluster = nullptr; // PS1TexturedFace's last cluster (cleared with the cache)
static uint16_t s_faceTPage = 0, s_faceClut = 0;
// One-piece sprite frames with their GPU words (PS1DrawSprite's fast path, docs/30 / docs/37 "Mania lessons pass"):
// keyed on (surface, frame rect), the piece's texture word (u | v << 8 | CLUT << 16) and texture page as
// PS1ChainPieceSpriteRows builds them. Cleared with the cluster cache (atlas / surface list changes).
struct PS1SpriteCacheEntry {
    int16_t sheetID, sprX, sprY, w, h; // sheetID -1: empty
    uint16_t tpage;
    uint32_t tex;
};
static PS1SpriteCacheEntry s_spriteCache[64];
volatile uint32_t g_ps1SpriteCacheHits = 0, g_ps1SpriteCacheFills = 0; // GDB
volatile uint8_t g_ps1SpriteCacheOn     = 1; // GDB: 0 = every sprite through the lookups (the same-build A/B)
static void PS1ClearClusterCache()
{
    memset(s_clusterCache, 0xFF, sizeof(s_clusterCache));
    memset(s_spriteCache, 0xFF, sizeof(s_spriteCache));
    s_faceCluster = nullptr;
}

// The cluster holding texel (x, y) of an atlas sheet; nullptr if none, or if it belongs to an on-demand
// picture not in VRAM yet (queued by PS1AtlasPictureReady: drawn from the next frame on).
static const PS1AtlasCluster *PS1FindCluster(int sheet, int x, int y)
{
    PS1ClusterCacheEntry &e = s_clusterCache[((unsigned)x * 7 + (unsigned)y * 13 + (unsigned)sheet * 31) & 63];
    if (e.sheet == sheet && e.x == x && e.y == y) {
        const PS1AtlasCluster *c = &g_ps1AtlasClusters[e.index];
        return !c->picture || PS1AtlasPictureReady(c) ? c : nullptr;
    }
    const PS1AtlasSheet &sh = g_ps1AtlasSheets[sheet];
    for (int i = sh.firstCluster; i < sh.firstCluster + sh.clusterCount; ++i) {
        const PS1AtlasCluster &c = g_ps1AtlasClusters[i];
        if (x >= c.srcX && x < c.srcX + c.w && y >= c.srcY && y < c.srcY + c.h) {
            e = PS1ClusterCacheEntry{ (int16_t)sheet, (int16_t)x, (int16_t)y, (int16_t)i };
            return !c.picture || PS1AtlasPictureReady(&c) ? &c : nullptr;
        }
    }
    g_ps1AtlasDrawMiss = g_ps1AtlasDrawMiss + 1;
    g_ps1MissSheet = sheet, g_ps1MissX = x, g_ps1MissY = y;
    return nullptr;
}

static psyqo::PrimPieces::TPageAttr PS1ClusterTPage(const PS1AtlasCluster *c)
{
    psyqo::PrimPieces::TPageAttr attr;
    attr.set(c->depth == 4 ? psyqo::Prim::TPageAttr::ColorMode::Tex4Bits : psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
    attr.setPageX(c->tpageX);
    attr.setPageY(c->tpageY);
    return attr;
}

// The cluster's CLUT; setB: the bottom bank's copy (draw-time bank split).
static psyqo::PrimPieces::ClutIndex PS1ClusterClut(const PS1AtlasCluster *c, bool setB = false)
{
    int16_t dy = setB ? PS1_CLUT_B_DY : 0;
    if (c->clut == 0xFFFF)
        return psyqo::PrimPieces::ClutIndex(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = (int16_t)(PS1_CLUT_Y + dy) } });
    const PS1AtlasClut &k = g_ps1AtlasCluts[c->clut];
    return psyqo::PrimPieces::ClutIndex(psyqo::Vertex{ { .x = (int16_t)k.x, .y = (int16_t)(k.y + dy) } });
}

// A piece of a frame: texel rect [x, x + w) x [y, y + h) of one cluster, at offset (ox, oy) in the frame.
struct PS1Piece {
    const PS1AtlasCluster *c;
    int16_t ox, oy, w, h, u, v;
};

// Splits frame (sprX, sprY, width, height) of surface sheetID into cluster pieces and calls fn on each.
// Frames past the bottom of their sheet continue into the next loaded sheet of the same width, as on
// PC where every sheet sits back to back in one buffer (Sonic CD's menu headings: MenuGfx1 -> 2).
template <typename Fn> static void PS1ForEachPieceAtlas(int sheet, int sheetID, int width, int height, int sprX, int sprY, Fn fn);
template <typename Fn> static void PS1ForEachPiece(int sheetID, int width, int height, int sprX, int sprY, Fn fn)
{
    int sheet = PS1SurfaceSheet(sheetID);
    if (sheet < 0) {
        g_ps1SheetUnbound = g_ps1SheetUnbound + 1;
        return;
    }
    PS1ForEachPieceAtlas(sheet, sheetID, width, height, sprX, sprY, fn);
}

// Same with an atlas sheet index (sheetID: its surface, for the overflow into the next sheet; -1 = none).
template <typename Fn> static void PS1ForEachPieceAtlas(int sheet, int sheetID, int width, int height, int sprX, int sprY, Fn fn)
{
    int oy = 0;
    while (height > 0) {
        int sh = g_ps1AtlasSheets[sheet].height;
        while (sprY >= sh) { // overflow into the next loaded sheet of the same width
            if (sheetID < 0)
                return;
            int w = g_ps1AtlasSheets[sheet].width, next = -1;
            for (int s = sheetID + 1; s < SURFACE_COUNT && next < 0; ++s) {
                int a = PS1SurfaceSheet(s);
                if (a >= 0 && g_ps1AtlasSheets[a].width == w) {
                    next    = a;
                    sheetID = s;
                }
            }
            if (next < 0)
                return;
            sprY -= sh;
            sheet = next;
            sh    = g_ps1AtlasSheets[sheet].height;
        }
        int part = sprY + height > sh ? sh - sprY : height;
        struct Rect16 {
            int16_t x, y, w, h;
        } stack[16];
        int sp      = 0;
        stack[sp++] = Rect16{ (int16_t)sprX, (int16_t)sprY, (int16_t)width, (int16_t)part };
        while (sp) {
            Rect16 r                 = stack[--sp];
            const PS1AtlasCluster *c = PS1FindCluster(sheet, r.x, r.y);
            if (!c)
                continue;
            int iw = c->srcX + c->w - r.x, ih = c->srcY + c->h - r.y;
            if (iw > r.w)
                iw = r.w;
            if (ih > r.h)
                ih = r.h;
            fn(PS1Piece{ c, (int16_t)(r.x - sprX), (int16_t)(oy + r.y - sprY), (int16_t)iw, (int16_t)ih, (int16_t)(c->u + r.x - c->srcX),
                         (int16_t)(c->v + r.y - c->srcY) });
            if (r.w > iw && sp < 16)
                stack[sp++] = Rect16{ (int16_t)(r.x + iw), r.y, (int16_t)(r.w - iw), (int16_t)ih };
            if (r.h > ih && sp < 16)
                stack[sp++] = Rect16{ r.x, (int16_t)(r.y + ih), r.w, (int16_t)(r.h - ih) };
        }
        oy += part;
        sprY += part;
        height -= part;
    }
}

static inline bool PS1ObjectsHidden() { return (g_ps1DebugDrawMask & 1) != 0; }

// Ink state for the next pieces: GPU semi-transparency mode (psx-spx GP0 E1h bits 5-6) and the texel
// modulation (0x80 = unchanged). Set by the ink draw calls around their piece loops.
enum PS1Ink { PS1_INK_OPAQUE, PS1_INK_HALF, PS1_INK_ADD, PS1_INK_SUB };
static uint8_t s_ink    = PS1_INK_OPAQUE;
static uint8_t s_inkMod = 0x80;
volatile uint32_t g_ps1InkPieces = 0; // semi-transparent sprite pieces chained

static inline void PS1ApplyInk(psyqo::PrimPieces::TPageAttr &attr)
{
    static const psyqo::Prim::TPageAttr::SemiTrans modes[4] = { psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront,
                                                                psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront,
                                                                psyqo::Prim::TPageAttr::SemiTrans::FullBackAndFullFront,
                                                                psyqo::Prim::TPageAttr::SemiTrans::FullBackSubFullFront };
    attr.set(modes[s_ink]);
}

// PC alpha 0-255 -> the PS1 levels (user decision 2026-09-24): 0 skip, 1 = 50 %, 2 = 75 % (two 50 %
// draws: B/4 + 3F/4), 3 = opaque.
static inline int PS1AlphaLevel(int a) { return a < 64 ? 0 : a < 160 ? 1 : a < 224 ? 2 : 3; }

// Rows [v, v + h) of a piece at (x, y) with CLUT set A or B.
static void PS1ChainPieceSpriteRows(const PS1Piece &p, int x, int y, int v, int h, bool setB)
{
    psyqo::PrimPieces::TPageAttr attr = PS1ClusterTPage(p.c);
    if (s_ink != PS1_INK_OPAQUE)
        PS1ApplyInk(attr);
    PS1ChainTPage(attr);
    psyqo::Prim::Sprite s(psyqo::Color{ { .r = s_inkMod, .g = s_inkMod, .b = s_inkMod } });
    if (s_ink != PS1_INK_OPAQUE) {
        s.setSemiTrans();
        g_ps1InkPieces = g_ps1InkPieces + 1;
    }
    s.position = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)y } };
    s.size     = psyqo::Vertex{ { .w = p.w, .h = (int16_t)h } };
    s.texInfo  = psyqo::PrimPieces::TexInfo{ (uint8_t)p.u, (uint8_t)v, PS1ClusterClut(p.c, setB) };
    PS1ChainSprite(s);
    g_ps1DrawSprites = g_ps1DrawSprites + 1;
}

// 1:1 piece at (x, y); under a bank split, cut at the split line (rows below it use set B).
static void PS1ChainPieceSprite(const PS1Piece &p, int x, int y)
{
    int y0 = s_splitY;
    if (!y0 || y + p.h <= y0) {
        PS1ChainPieceSpriteRows(p, x, y, p.v, p.h, false);
        return;
    }
    PS1EnsureSetB();
    if (y >= y0) {
        PS1ChainPieceSpriteRows(p, x, y, p.v, p.h, true);
        return;
    }
    int h1 = y0 - y;
    PS1ChainPieceSpriteRows(p, x, y, p.v, h1, false);
    PS1ChainPieceSpriteRows(p, x, y0, p.v + h1, p.h - h1, true);
    g_ps1SplitCuts = g_ps1SplitCuts + 1;
}

// Textured quad: corners A (u0, v0), B (u1, v0), C (u0, v1), D (u1, v1). UVs are texel edges; they
// may run backwards (mirroring). A UV byte can't hold -1 or 256: those edges are pulled in by one texel
// (rare: pieces at a page edge), which drops/duplicates at most one texel column or row.
#if PS1_GAME == 1
// The quad PS1ChainPieceQuad chains (ink: the current one; plain: opaque, unmodulated), set A's CLUT.
static psyqo::Prim::TexturedQuad PS1PieceQuad(const PS1Piece &p, const int16_t *xs, const int16_t *ys, int u0, int u1, int v0, int v1,
                                              bool ink = true)
{
    auto clampUV  = [](int t) { return (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t); };
    uint8_t mod   = ink ? s_inkMod : 0x80;
    psyqo::Prim::TexturedQuad q(psyqo::Color{ { .r = mod, .g = mod, .b = mod } });
    q.pointA    = psyqo::Vertex{ { .x = xs[0], .y = ys[0] } };
    q.pointB    = psyqo::Vertex{ { .x = xs[1], .y = ys[1] } };
    q.pointC    = psyqo::Vertex{ { .x = xs[2], .y = ys[2] } };
    q.pointD    = psyqo::Vertex{ { .x = xs[3], .y = ys[3] } };
    q.uvA       = psyqo::PrimPieces::UVCoords{ clampUV(u0), clampUV(v0) };
    q.uvB       = psyqo::PrimPieces::UVCoords{ clampUV(u1), clampUV(v0) };
    q.uvC       = psyqo::PrimPieces::UVCoordsPadded{ clampUV(u0), clampUV(v1), 0 };
    q.uvD       = psyqo::PrimPieces::UVCoordsPadded{ clampUV(u1), clampUV(v1), 0 };
    q.clutIndex = PS1ClusterClut(p.c);
    q.tpage     = PS1ClusterTPage(p.c);
    if (ink && s_ink != PS1_INK_OPAQUE) {
        PS1ApplyInk(q.tpage);
        q.setSemiTrans();
    }
    return q;
}
static void PS1ChainPieceQuad(const PS1Piece &p, const int16_t *xs, const int16_t *ys, int u0, int u1, int v0, int v1)
{
    psyqo::Prim::TexturedQuad q = PS1PieceQuad(p, xs, ys, u0, u1, v0, v1);
    if (s_ink != PS1_INK_OPAQUE)
        g_ps1InkPieces = g_ps1InkPieces + 1;
#else
static void PS1ChainPieceQuad(const PS1Piece &p, const int16_t *xs, const int16_t *ys, int u0, int u1, int v0, int v1)
{
    auto clampUV = [](int t) { return (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t); };
    psyqo::Prim::TexturedQuad q(psyqo::Color{ { .r = s_inkMod, .g = s_inkMod, .b = s_inkMod } });
    q.pointA    = psyqo::Vertex{ { .x = xs[0], .y = ys[0] } };
    q.pointB    = psyqo::Vertex{ { .x = xs[1], .y = ys[1] } };
    q.pointC    = psyqo::Vertex{ { .x = xs[2], .y = ys[2] } };
    q.pointD    = psyqo::Vertex{ { .x = xs[3], .y = ys[3] } };
    q.uvA       = psyqo::PrimPieces::UVCoords{ clampUV(u0), clampUV(v0) };
    q.uvB       = psyqo::PrimPieces::UVCoords{ clampUV(u1), clampUV(v0) };
    q.uvC       = psyqo::PrimPieces::UVCoordsPadded{ clampUV(u0), clampUV(v1), 0 };
    q.uvD       = psyqo::PrimPieces::UVCoordsPadded{ clampUV(u1), clampUV(v1), 0 };
    q.clutIndex = PS1ClusterClut(p.c);
    q.tpage     = PS1ClusterTPage(p.c);
    if (s_ink != PS1_INK_OPAQUE) {
        PS1ApplyInk(q.tpage);
        q.setSemiTrans();
        g_ps1InkPieces = g_ps1InkPieces + 1;
    }
#endif
    g_ps1DrawQuads = g_ps1DrawQuads + 1;
    int y0 = s_splitY;
    if (y0) { // bank split: set B below the line, both halves under a drawing area when the quad crosses it
        int ymin = ys[0], ymax = ys[0];
        for (int k = 1; k < 4; ++k) {
            ymin = ys[k] < ymin ? ys[k] : ymin;
            ymax = ys[k] > ymax ? ys[k] : ymax;
        }
        if (ymax > y0) {
            PS1EnsureSetB();
            if (ymin < y0) {
                PS1ChainDrawArea(0, 0, SCREEN_XSIZE, y0);
                PS1ChainQuad(q);
                PS1ChainDrawArea(0, y0, SCREEN_XSIZE, SCREEN_YSIZE);
                g_ps1SplitQuads = g_ps1SplitQuads + 1;
            }
            q.clutIndex = PS1ClusterClut(p.c, true);
            PS1ChainQuad(q);
            if (ymin < y0)
                PS1ChainDrawAreaReset();
            return;
        }
    }
    PS1ChainQuad(q);
}

// Axis-aligned piece at screen rect (x, y, w, h) with optional mirroring. The GPU samples a quad's texels from its
// top-left corner and leaves out the right / bottom edge, so a texture axis running backwards on screen must not start
// at its far edge (u + w: the atlas's next piece, a dark line along a mirrored sprite). 1:1 mirrors sample texel
// u + w - 1 - k at column k (UV edges u + w - 1 .. u - 1, exact); scaled mirrors take u + w - 1 .. u (one texel
// narrower: a scaled edge has no exact integer UV); unmirrored scaled pieces the full [u, u + w) range.
static void PS1ChainPieceRect(const PS1Piece &p, int x, int y, int w, int h, bool flipX, bool flipY)
{
    if (!flipX && !flipY && w == p.w && h == p.h) {
        PS1ChainPieceSprite(p, x, y);
        return;
    }
    PS1QuadUV uv  = PS1RectUV(p.u, p.v, p.w, p.h, w, h, flipX, flipY); // ps1/sprite_quad.hh
    int16_t xs[4] = { (int16_t)x, (int16_t)(x + w), (int16_t)x, (int16_t)(x + w) };
    int16_t ys[4] = { (int16_t)y, (int16_t)y, (int16_t)(y + h), (int16_t)(y + h) };
    PS1ChainPieceQuad(p, xs, ys, uv.u0, uv.u1, uv.v0, uv.v1);
}

// GPU limits (psx-spx GPU "Vertex"): coordinates are 11-bit signed (-1024..1023, beyond that they wrap)
// and a polygon spanning more than 1023 x 511 pixels is not drawn at all. Upstream's software renderer
// clips everything to the screen; here primitives beyond the limits are clipped to it first (a UFO
// next to the special-stage camera is drawn up to ~96x its size; the menu background's 3D panels
// project to several screens).
volatile uint32_t g_ps1GpuClipped = 0; // primitives clipped to the screen because they were beyond the limits
volatile uint32_t g_ps1GpuLimit   = 0; // primitives still beyond the limits (drawn as they are: GDB check)
volatile uint32_t g_ps1ClipRects = 0, g_ps1ClipQuads = 0, g_ps1ClipBillboards = 0; // ... of them: scaled pieces, rotozoom pieces, 3D billboards
// Billboards / rotozoom pieces beyond the limits and wholly off screen: before the fix they were chained as they were and
// their wrapped coordinates drew them across the screen (the gate / text flicker). g_ps1LimitFix = 0 (GDB) restores that
// for the A/B.
volatile uint32_t g_ps1WrapCulled = 0, g_ps1LimitFix = 1;

static bool PS1BeyondGpuLimits(int x0, int y0, int x1, int y1)
{
    return x0 < -1024 || y0 < -1024 || x1 > 1023 || y1 > 1023 || x1 - x0 > 1023 || y1 - y0 > 511;
}

// PS1ChainPieceRect for any size: a piece beyond the GPU limits is cut to the screen, its texel edges
// moved in proportion (mirrored pieces from the far edge).
static void PS1ChainPieceRectClipped(const PS1Piece &p, int x, int y, int w, int h, bool flipX, bool flipY)
{
    if (!PS1BeyondGpuLimits(x, y, x + w, y + h)) {
        PS1ChainPieceRect(p, x, y, w, h, flipX, flipY);
        return;
    }
    g_ps1GpuClipped = g_ps1GpuClipped + 1;
    g_ps1ClipRects = g_ps1ClipRects + 1;
    int cx0 = x < 0 ? 0 : x, cx1 = x + w > SCREEN_XSIZE ? SCREEN_XSIZE : x + w;
    int cy0 = y < 0 ? 0 : y, cy1 = y + h > SCREEN_YSIZE ? SCREEN_YSIZE : y + h;
    if (cx0 >= cx1 || cy0 >= cy1)
        return;
    PS1QuadUV uv  = PS1ClipRectUV(p.u, p.v, p.w, p.h, x, y, w, h, cx0, cx1, cy0, cy1, flipX, flipY); // ps1/sprite_quad.hh
    int16_t xs[4] = { (int16_t)cx0, (int16_t)cx1, (int16_t)cx0, (int16_t)cx1 };
    int16_t ys[4] = { (int16_t)cy0, (int16_t)cy0, (int16_t)cy1, (int16_t)cy1 };
    PS1ChainPieceQuad(p, xs, ys, uv.u0, uv.u1, uv.v0, uv.v1);
}

// A convex polygon (screen coordinates, any size) cut to the screen (Sutherland-Hodgman) and chained as
// a fan of flat triangles: cmd = GP0 20h (| 02000000h semi-transparent) with the BGR colour.
static void PS1ChainFlatPolygonClipped(const int *px, const int *py, int n, uint32_t cmd, int repeat)
{
    int ax[12], ay[12], bx[12], by[12];
    for (int i = 0; i < n; ++i) ax[i] = px[i], ay[i] = py[i];
    for (int side = 0; side < 4 && n > 0; ++side) {
        // side 0: x >= 0, 1: x <= W, 2: y >= 0, 3: y <= H
        auto inside = [&](int x, int y) {
            switch (side) {
                case 0: return x >= 0;
                case 1: return x <= SCREEN_XSIZE;
                case 2: return y >= 0;
                default: return y <= SCREEN_YSIZE;
            }
        };
        auto cross = [&](int x0, int y0, int x1, int y1, int *ox, int *oy) {
            if (side < 2) {
                int e = side == 0 ? 0 : SCREEN_XSIZE;
                *ox   = e;
                *oy   = y0 + (int)((int64_t)(y1 - y0) * (e - x0) / (x1 - x0));
            }
            else {
                int e = side == 2 ? 0 : SCREEN_YSIZE;
                *oy   = e;
                *ox   = x0 + (int)((int64_t)(x1 - x0) * (e - y0) / (y1 - y0));
            }
        };
        int m = 0;
        for (int i = 0; i < n && m < 10; ++i) {
            int j = (i + 1) % n;
            bool in0 = inside(ax[i], ay[i]), in1 = inside(ax[j], ay[j]);
            if (in0)
                bx[m] = ax[i], by[m] = ay[i], ++m;
            if (in0 != in1) {
                cross(ax[i], ay[i], ax[j], ay[j], &bx[m], &by[m]);
                ++m;
            }
        }
        n = m;
        for (int i = 0; i < n; ++i) ax[i] = bx[i], ay[i] = by[i];
    }
    for (int i = 1; i + 1 < n; ++i) {
        uint32_t w[4] = { cmd, (uint32_t)(uint16_t)ax[0] | ((uint32_t)(uint16_t)ay[0] << 16),
                          (uint32_t)(uint16_t)ax[i] | ((uint32_t)(uint16_t)ay[i] << 16),
                          (uint32_t)(uint16_t)ax[i + 1] | ((uint32_t)(uint16_t)ay[i + 1] << 16) };
        for (int r = 0; r < repeat; ++r) PS1ChainFlatRaw(w, 4);
    }
}

// A textured piece quad (corners A B C D and texel-edge UVs as PS1ChainPieceQuad) beyond the GPU limits: cut to the
// screen (Sutherland-Hodgman, the UVs carried with 8 fractional bits) and chained as a fan of textured triangles
// (GP0 24h / 26h), ink and bank split as PS1ChainPieceQuad. Rotozoomed sprites passing the screen (the special stage's
// messages zooming in: the Sonic CD UFO's bug on the rotation path) wrapped or were dropped by the GPU before.
static void PS1ChainPieceQuadClipped(const PS1Piece &p, const int *qx, const int *qy, int u0, int u1, int v0, int v1)
{
    int ax[12] = { qx[0], qx[1], qx[3], qx[2] }, ay[12] = { qy[0], qy[1], qy[3], qy[2] }; // A B D C: around the outline
    int au[12] = { u0 << 8, u1 << 8, u1 << 8, u0 << 8 }, av[12] = { v0 << 8, v0 << 8, v1 << 8, v1 << 8 };
    int bx[12], by[12], bu[12], bv[12];
    int n = 4;
    for (int side = 0; side < 4 && n > 0; ++side) {
        auto dist = [side](int x, int y) { // >= 0: inside this screen edge
            return side == 0 ? x : side == 1 ? SCREEN_XSIZE - x : side == 2 ? y : SCREEN_YSIZE - y;
        };
        int m = 0;
        for (int i = 0; i < n && m < 10; ++i) {
            int j = (i + 1) % n, d0 = dist(ax[i], ay[i]), d1 = dist(ax[j], ay[j]);
            if (d0 >= 0)
                bx[m] = ax[i], by[m] = ay[i], bu[m] = au[i], bv[m] = av[i], ++m;
            if ((d0 >= 0) != (d1 >= 0)) { // the crossing: i + (j - i) * d0 / (d0 - d1)
                int64_t num = d0, den = (int64_t)d0 - d1;
                auto lerp = [&](int a, int b) { return a + (int)((int64_t)(b - a) * num / den); };
                bx[m] = side == 0 ? 0 : side == 1 ? SCREEN_XSIZE : lerp(ax[i], ax[j]);
                by[m] = side == 2 ? 0 : side == 3 ? SCREEN_YSIZE : lerp(ay[i], ay[j]);
                bu[m] = lerp(au[i], au[j]), bv[m] = lerp(av[i], av[j]);
                ++m;
            }
        }
        n = m;
        for (int i = 0; i < n; ++i) ax[i] = bx[i], ay[i] = by[i], au[i] = bu[i], av[i] = bv[i];
    }
    if (n < 3)
        return;
    g_ps1GpuClipped = g_ps1GpuClipped + 1;
    g_ps1ClipQuads = g_ps1ClipQuads + 1;
    psyqo::PrimPieces::TPageAttr tpa = PS1ClusterTPage(p.c);
    bool semi                        = s_ink != PS1_INK_OPAQUE;
    if (semi) {
        PS1ApplyInk(tpa);
        g_ps1InkPieces = g_ps1InkPieces + 1;
    }
    uint16_t tpage, clutA, clutB;
    psyqo::PrimPieces::ClutIndex ca = PS1ClusterClut(p.c);
    __builtin_memcpy(&tpage, &tpa, 2);
    __builtin_memcpy(&clutA, &ca, 2);
    const uint32_t cmd = (semi ? 0x26000000u : 0x24000000u) | s_inkMod | (uint32_t)s_inkMod << 8 | (uint32_t)s_inkMod << 16;
    auto xy = [&](int k) { return (uint32_t)(uint16_t)ax[k] | ((uint32_t)(uint16_t)ay[k] << 16); };
    auto uv = [&](int k) {
        int u = (au[k] + 128) >> 8, v = (av[k] + 128) >> 8;
        return (uint32_t)(u < 0 ? 0 : u > 255 ? 255 : u) | (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << 8;
    };
    auto emit = [&](uint16_t clut) {
        for (int i = 1; i + 1 < n; ++i) {
            uint32_t w[7] = { cmd, xy(0), uv(0) | (uint32_t)clut << 16, xy(i), uv(i) | (uint32_t)tpage << 16, xy(i + 1), uv(i + 1) };
            PS1ChainPrimsRawN(w, 7, 1);
        }
    };
    int ymin = ay[0], ymax = ay[0];
    for (int k = 1; k < n; ++k) {
        ymin = ay[k] < ymin ? ay[k] : ymin;
        ymax = ay[k] > ymax ? ay[k] : ymax;
    }
    int y0 = s_splitY;
    if (y0 && ymax > y0) { // bank split (PS1ChainPieceQuad): set B below the line
        PS1EnsureSetB();
        psyqo::PrimPieces::ClutIndex cb = PS1ClusterClut(p.c, true);
        __builtin_memcpy(&clutB, &cb, 2);
        if (ymin < y0) {
            PS1ChainDrawArea(0, 0, SCREEN_XSIZE, y0);
            emit(clutA);
            PS1ChainDrawArea(0, y0, SCREEN_XSIZE, SCREEN_YSIZE);
            g_ps1SplitQuads = g_ps1SplitQuads + 1;
        }
        emit(clutB);
        if (ymin < y0)
            PS1ChainDrawAreaReset();
        return;
    }
    emit(clutA);
}

// ---------------------------------------------------------------------------------------------------
// Upstream API

// upstream InitRenderDevice -> SetScreenDimensions -> SetScreenSize: the screen width and what depends on it
// (scroll edges, object activity borders). Left at their static values, every bounds-priority object would be
// judged against a 424 px screen.
int InitRenderDevice()
{
    SCREEN_XSIZE        = DEFAULT_SCREEN_XSIZE;
    SCREEN_CENTERX      = SCREEN_XSIZE / 2;
    SCREEN_XSIZE_F      = SCREEN_XSIZE;
    SCREEN_CENTERX_F    = SCREEN_CENTERX;
    SCREEN_SCROLL_LEFT  = SCREEN_CENTERX - 8;
    SCREEN_SCROLL_RIGHT = SCREEN_CENTERX + 8;
    OBJECT_BORDER_X2    = SCREEN_XSIZE + 0x80;
    OBJECT_BORDER_X4    = SCREEN_XSIZE + 0x20;
    return 1;
}

// PC converts the palette here, after drawing: the CLUT rows chained at the frame start get the
// palette as it is now (a SetFade / palette change from a draw sub shows in the same frame).
volatile uint32_t g_ps1BankRegions = 0; // palette-bank regions (runs of equal gfxLineBuffer) in the last frame
volatile uint32_t g_ps1BankMask    = 0; // banks used by the last frame (bit per bank)
volatile uint32_t g_ps1BankReplays = 0; // regions drawn again with their own bank (total)
volatile uint32_t g_ps1BankHbl     = 0; // CPU time of the last frame's bank regions (replays), hblanks
extern volatile uint32_t g_ps1ReplayWalked, g_ps1ReplayKept; // ps1/gpu_chain.cpp

// Per-line palette banks (upstream SetActivePalette / gfxLineBuffer; user decision 2026-09-24: replay
// per region). The frame's first pass gets the bank covering the most lines; every further run of lines with another bank
// gets its bank's CLUT rows (same VRAM rows), a drawing area of those lines, a fresh base (clear
// colour, or the last frame in a repeated frame) and a replay of the frame's primitives.
static int s_baseBank = 0; // bank of the frame's first pass (the one covering the most lines)

static void PS1DrawBankRegions(psyqo::Color clear)
{
    // Nearly every frame uses one bank on every line: 60 word compares instead of the region scan.
    typedef uint32_t __attribute__((may_alias)) PS1Word;
    static_assert(SCREEN_YSIZE % 4 == 0, "gfxLineBuffer words");
    const PS1Word *lw = (const PS1Word *)gfxLineBuffer; // aligned(4) on PS1 (Palette.cpp)
    uint32_t w0       = gfxLineBuffer[0] * 0x01010101u;
    int i             = 0;
    while (i < SCREEN_YSIZE / 4 && lw[i] == w0) ++i;
    if (i == SCREEN_YSIZE / 4) {
        s_baseBank       = gfxLineBuffer[0] & (PALETTE_COUNT - 1);
        g_ps1BankRegions = 1;
        g_ps1BankMask    = 1u << s_baseBank;
        return;
    }
    uint32_t *sceneEnd = PS1ChainMark();
    int lines[PALETTE_COUNT] = {};
    for (int y = 0; y < SCREEN_YSIZE; ++y) lines[gfxLineBuffer[y] & (PALETTE_COUNT - 1)]++;
    s_baseBank = gfxLineBuffer[0] & (PALETTE_COUNT - 1);
    for (int b = 0; b < PALETTE_COUNT; ++b)
        if (lines[b] > lines[s_baseBank])
            s_baseBank = b; // replays then cover at most half the screen
    int regions = 0, y0 = 0;
    uint32_t mask = 0;
    for (int y = 1; y <= SCREEN_YSIZE; ++y) {
        if (y < SCREEN_YSIZE && gfxLineBuffer[y] == gfxLineBuffer[y0])
            continue;
        int bank = gfxLineBuffer[y0] & (PALETTE_COUNT - 1);
        ++regions;
        mask |= 1u << bank;
        if (bank != s_baseBank) { // this run of lines again, with its own bank
            int first = s_palChunkCount;
            PS1ReservePaletteRows();
            if (s_setB) // set B's rows too: primitives drawn before the frame fell back use them
                PS1ReservePaletteRows(PS1_CLUT_B_DY);
            PS1FillPalette(bank, first);
            PS1ChainFlushCache();
            PS1ChainDrawArea(0, y0, 320, y);
            if (s_frameFresh)
                PS1ChainFillLines(y0, y, clear.r, clear.g, clear.b);
            else
                PS1ChainCopyLastFrameLines(y0, y);
            PS1ChainReplayLines(s_sceneMark, sceneEnd, y0, y);
            g_ps1BankReplays = g_ps1BankReplays + 1;
        }
        y0 = y;
    }
    if (regions > 1) {
        PS1ChainDrawAreaReset();
        PS1ChainInvalidateTPage();
    }
    g_ps1BankRegions = regions;
    g_ps1BankMask    = mask;
}

// PC converts the palette here, after drawing: the CLUT rows chained at the frame start get the
// palette as it is now (a SetFade / palette change from a draw sub shows in the same frame).
void FlipScreen()
{
    if (!s_frameOpen)
        return;
    PS1ChainSetHook(nullptr); // nothing else comes: a pending fade stays a CLUT fade (the hook would draw it)
    psyqo::Color clear = { { .r = 0, .g = 0, .b = 0 } };
    if (s_fadePending)
        clear = psyqo::Color{ { .r = (uint8_t)(s_fadeR * s_fadeA / 255), .g = (uint8_t)(s_fadeG * s_fadeA / 255),
                                .b = (uint8_t)(s_fadeB * s_fadeA / 255) } };
    if (s_fadePending) { // before the bank regions replay (copy) the frame's fragments
        // c * (255 - a) + fade * a, / 255 as (n + 1 + (n >> 8)) >> 8 (exact for n <= 65025: the CLUT fade's result)
        const uint32_t a = s_fadeA, na = 255 - a, fr = s_fadeR * a, fg = s_fadeG * a, fb = s_fadeB * a;
        auto div255 = [](uint32_t n) { return (n + 1 + (n >> 8)) >> 8; };
        uint32_t faded = 0;
        for (int i = 0; i < s_directCount; ++i) {
            const PS1DirectRun &run = s_directRuns[i];
            uint32_t *w = run.word;
            for (int k = 0; k < run.count; ++k, w += run.stride) {
                uint32_t c = *w;
                uint32_t r = div255((c & 0xFF) * na + fr), g = div255((c >> 8 & 0xFF) * na + fg), b = div255((c >> 16 & 0xFF) * na + fb);
                *w         = (c & 0xFF000000) | r | g << 8 | b << 16;
            }
            faded += run.count;
        }
        g_ps1FadeDirect = g_ps1FadeDirect + faded;
    }
    uint32_t tb       = PS1Hblanks();
    g_ps1ReplayWalked = 0;
    g_ps1ReplayKept   = 0;
    if (!s_splitComplex)
        PS1SplitUpdate(); // the final layout (a change after the last draw call)
    if (!s_splitComplex) { // one bank, or the draw-time split: set A = the top bank, set B = the bottom one
        s_baseBank = (s_splitY ? s_splitTop : gfxLineBuffer[0]) & (PALETTE_COUNT - 1);
        PS1FillPalette(s_baseBank, 0, s_setAEnd);
        if (s_setB)
            PS1FillPalette(s_splitBottom, s_setBFirst, s_setBEnd);
        g_ps1BankRegions = s_splitY ? 2 : 1;
        g_ps1BankMask    = (1u << s_baseBank) | (s_splitY ? 1u << (s_splitBottom & (PALETTE_COUNT - 1)) : 0);
    }
    else { // region replay (both CLUT sets hold the base bank in the first pass)
        g_ps1SplitFallbacks = g_ps1SplitFallbacks + 1;
        int frameChunks     = s_palChunkCount;
        PS1DrawBankRegions(clear);
        int end         = s_palChunkCount; // the regions filled their own chunks: refill only the frame's rows
        s_palChunkCount = frameChunks;
        PS1FillPalette(s_baseBank); // the first pass is drawn with the base bank
        s_palChunkCount = end;
    }
    g_ps1BankHbl = PS1HblanksSince(tb);
    if (s_fadePending) {
        g_ps1ClutFades = g_ps1ClutFades + 1;
        if (s_clearPrim) // cleared (undrawn) pixels are black: under the fade, fade colour * a
            s_clearPrim->setColor(clear);
    }
}

void GenerateBlendLookupTable() {}

void ClearScreen(byte index)
{
    PS1FrameStart(true);
    const PaletteEntry &c = fullPalette32[gfxLineBuffer[0]][index];
    uint32_t w[3] = { 0x60000000u | c.r | (uint32_t)c.g << 8 | (uint32_t)c.b << 16, 0, (uint32_t)SCREEN_XSIZE | (uint32_t)SCREEN_YSIZE << 16 };
    PS1ChainFlatRaw(w, 3); // GP0 60h: flat rectangle
}

// upstream: draw subs are game logic too; only their Draw* calls produce primitives.
extern uint32_t g_ps1TypeDrawTicks[OBJECT_COUNT], g_ps1TypeDrawCalls[OBJECT_COUNT]; // Object.cpp profile
#if PS1_GAME == 1
bool PS1S1TopSub(int ptr); // Script.cpp
#endif
void DrawObjectList(int layer)
{
    PS1FrameStart(false);
    uint32_t t0 = PS1Hblanks();
    g_ps1ChainSkipPrims = PS1ObjectsHidden();
    int size            = drawListEntries[layer].listSize;
    for (int i = 0; i < size; ++i) {
        objectEntityPos = drawListEntries[layer].entityRefs[i];
        int type        = objectEntityList[objectEntityPos].type;
#if PS1_GAME == 1
        // Sonic 1 (docs/37 phase 7): a draw sub that is one native opcode runs without ProcessScript (Script.cpp
        // PS1S1TopSub); the per-type draw ticks only in PROFILE=1 builds (two counter reads a sub).
        if (type) {
#if PS1_PROFILE
            uint32_t tt = PS1ProfTicks();
#endif
            int ptr = objectScriptList[type].eventDraw.scriptCodePtr;
            if (scriptCode[ptr] > 0 && !PS1S1TopSub(ptr))
                ProcessScript(ptr, objectScriptList[type].eventDraw.jumpTablePtr, EVENT_DRAW);
#if PS1_PROFILE
            g_ps1TypeDrawTicks[type] += (PS1ProfTicks() - tt) & 0xFFFF;
            g_ps1TypeDrawCalls[type]++;
#endif
        }
#else
        if (type) {
            uint32_t tt = PS1ProfTicks();
            if (scriptCode[objectScriptList[type].eventDraw.scriptCodePtr] > 0)
                ProcessScript(objectScriptList[type].eventDraw.scriptCodePtr, objectScriptList[type].eventDraw.jumpTablePtr, EVENT_DRAW);
            g_ps1TypeDrawTicks[type] += (PS1ProfTicks() - tt) & 0xFFFF;
            g_ps1TypeDrawCalls[type]++;
        }
#endif
    }
    g_ps1ChainSkipPrims = false;
    g_ps1ProfObjDrawHbl = g_ps1ProfObjDrawHbl + PS1HblanksSince(t0);
}

void DrawHLineScrollLayer(int layerID);
void DrawVLineScrollLayer(int layerID);

static void PS1DrawLayer(int layerID)
{
    if (activeTileLayers[layerID] >= LAYER_COUNT)
        return;
    switch (stageLayouts[activeTileLayers[layerID]].type) {
        case LAYER_HSCROLL: DrawHLineScrollLayer(layerID); break;
        case LAYER_VSCROLL: DrawVLineScrollLayer(layerID); break;
        case LAYER_3DFLOOR: Draw3DFloorLayer(layerID); break;
        case LAYER_3DSKY: Draw3DSkyLayer(layerID); break;
        default: break;
    }
}

// upstream DrawStageGFX (v4 SW path): water position, then layers and draw lists in upstream order by
// tLayerMidPoint (list 7 before 6; nothing when it is >= 6), then the full-screen fade rectangle.
volatile uint32_t g_ps1ProfWaitHbl = 0; // profiling (g_ps1DebugDrawMask bit 2): time waiting for the previous chain
void DrawStageGFX()
{
    if (g_ps1DebugDrawMask & 4) { // profiling: draw with the GPU/DMA idle (bus contention test)
        uint32_t t0 = PS1Hblanks();
        PS1GpuImmediate();
        g_ps1ProfWaitHbl = PS1HblanksSince(t0);
    }
    PS1FrameStart(true);
    memset(g_ps1TypeDrawTicks, 0, sizeof(g_ps1TypeDrawTicks));
    memset(g_ps1TypeDrawCalls, 0, sizeof(g_ps1TypeDrawCalls));
    g_ps1DrawHashLast  = g_ps1DrawHash; // the previous frame's
    g_ps1DrawCountLast = g_ps1DrawCount;
    g_ps1DrawHash      = 0;
    g_ps1DrawCount     = 0;
    waterDrawPos = waterLevel - yScrollOffset;
    if (waterDrawPos < 0)
        waterDrawPos = 0;
    if (waterDrawPos > SCREEN_YSIZE)
        waterDrawPos = SCREEN_YSIZE;

    if (tLayerMidPoint < 3) {
        DrawObjectList(0);
        PS1DrawLayer(0);
        DrawObjectList(1);
        PS1DrawLayer(1);
        DrawObjectList(2);
        DrawObjectList(3);
        DrawObjectList(4);
        PS1DrawLayer(2);
    }
    else if (tLayerMidPoint < 6) {
        DrawObjectList(0);
        PS1DrawLayer(0);
        DrawObjectList(1);
        PS1DrawLayer(1);
        DrawObjectList(2);
        PS1DrawLayer(2);
        DrawObjectList(3);
        DrawObjectList(4);
    }
    if (tLayerMidPoint < 6) {
        PS1DrawLayer(3);
        DrawObjectList(5);
#if RETRO_REV03
        // Upstream draws layer 7 only for Origins data (its fix for the mobile Tails Object in the special stages).
        // The mobile data's only layer-7 objects are the Sonic 3 shields' (Sonic 2's Fire Shield; Sonic 1's Fire
        // Shield and Lightning Spark), which the PS1 Options turn on (user 2026-10-09): drawn outside the special stages.
        if (forceUseScripts || Engine.usingOrigins || activeStageList != STAGELIST_SPECIAL)
            DrawObjectList(7);
#endif
        DrawObjectList(6);
    }
    if (fadeMode > 0)
        DrawRectangle(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, fadeR, fadeG, fadeB, fadeA);
}

void DrawDebugOverlays() {}

// ---------------------------------------------------------------------------------------------------
// Tile layers. The chunk tiles' visuals are the engine's packed table itself (Scene.hpp, phase 2): per
// chunk tile one halfword, bits 0-9 tile index, 10-11 flip, 12-15 visual plane (upstream draws plane 0
// below the mid point, 1 above; other values never). Empty tiles (no texel, ps1/tileset
// g_ps1TileSolid) are skipped. Per chunk: which of its 8 tile rows have visible tiles on each plane
// (rebuilt at stage load and when a script changes a tile or Copy16x16Tile fills an empty tile).

#define PS1_TV_INDEX 0x03FF
#define PS1_TV_DIR_SHIFT 10
#define PS1_TV_PLANE_SHIFT 12
static uint16_t s_chunkRowPlanes[CHUNKTILE_COUNT / 64];

static inline bool PS1TileSolid(int t) { return (g_ps1TileSolid[t >> 3] >> (t & 7)) & 1; }

// Copy16x16Tile destinations made solid at least once since the stage load ("sticky"): their chunk rows stay marked
// whatever the tile shows later (docs/30 / docs/37 "Mania lessons pass"). The row masks are a superset of the rows
// with a visible tile -- PS1DrawTileBand still checks each tile's solidity -- so pictures don't change; a tile an
// animation keeps turning empty and solid costs one rebuild per stage instead of one per copy. A rebuild is also
// deferred to the next tile layer draw: a frame of animated-tile copies rebuilds once (Sonic 1's special stage: ~30
// rebuilds in one frame, a 39-vblank freeze).
static uint8_t s_tileSticky[1024 / 8];
static bool s_tileVisualsDirty = false;
static inline bool PS1TileMaybeSolid(int t) { return ((g_ps1TileSolid[t >> 3] | s_tileSticky[t >> 3]) >> (t & 7)) & 1; }

static void PS1UpdateChunkRows(int chunk)
{
    uint16_t m          = 0;
    const ushort *tiles = &tiles128x128.ps1Tile[chunk << 6];
    for (int t = 0; t < 64; ++t) {
        uint16_t tv = tiles[t];
        int plane   = tv >> PS1_TV_PLANE_SHIFT;
        if (plane <= 1 && PS1TileMaybeSolid(tv & PS1_TV_INDEX))
            m |= 1 << ((t >> 3) * 2 + plane);
    }
    s_chunkRowPlanes[chunk] = m;
}

static void PS1RebuildTileVisuals()
{
    // PS1UpdateChunkRows for every chunk, from a byte per tile (the bit tables unpacked once): ~half the instructions
    uint8_t maybe[1024];
    for (int t = 0; t < 1024; ++t) maybe[t] = PS1TileMaybeSolid(t);
    const ushort *tiles = &tiles128x128.ps1Tile[0];
    for (int c = 0; c < CHUNKTILE_COUNT / 64; ++c) {
        uint32_t m = 0;
        for (int row = 0; row < 8; ++row) {
            uint32_t bit = 1u << (row * 2);
            for (int k = 0; k < 8; ++k) {
                uint32_t tv = *tiles++, plane = tv >> PS1_TV_PLANE_SHIFT;
                if (plane <= 1 && maybe[tv & PS1_TV_INDEX])
                    m |= bit << plane;
            }
        }
        s_chunkRowPlanes[c] = (uint16_t)m;
    }
    s_tileVisualsDirty = false;
}

void PS1BuildTileVisuals() // a stage load: no copies yet
{
    memset(s_tileSticky, 0, sizeof(s_tileSticky));
    PS1RebuildTileVisuals();
}

void PS1TileChanged(int chunkTile)
{
    if ((uint)chunkTile >= CHUNKTILE_COUNT)
        return;
    PS1UpdateChunkRows(chunkTile >> 6);
}

// upstream Copy16x16Tile (Scene.hpp, SW path): tile `src`'s pixels overwrite tile `dest`'s, from now
// on. Here: a VRAM-to-VRAM copy inside the tile pages, chained where the script calls it (draws before
// it still show the old pixels, as on PC), or immediate outside a frame and during a load (startups call
// it while the chain arenas are lent to the CD read: Chemical Plant).
volatile uint32_t g_ps1TileCopies  = 0; // Copy16x16Tile calls
volatile uint32_t g_ps1TileRebuild = 0; // copies that made an empty tile visible (packed table rebuilt)
static void PS1TileVramXY(int t, int &x, int &y)
{
    int p = (t >> 8) & 3, n = t & 0xFF;
    x     = (PS1_TILE_PAGE_X + 2 * (p & 1)) * 64 + (n & 15) * 8;
    y     = (PS1_TILE_PAGE_Y + (p >> 1)) * 256 + (n >> 4) * 16;
}

void PS1Copy16x16Tile(int dest, int src)
{
    if ((uint)dest >= 1024 || (uint)src >= 1024)
        return;
    int sx, sy, dx, dy;
    PS1TileVramXY(src, sx, sy);
    PS1TileVramXY(dest, dx, dy);
    if (s_chainOpen && PS1ChainHasArena()) {
        PS1ChainVramCopy(sx, sy, dx, dy, 8, 16);
        PS1ChainFlushCache();
    }
    else if (g_ps1GPU) {
        struct {
            uint32_t command = 0x80000000;
            psyqo::Vertex src, dst, size;
        } c;
        c.src  = psyqo::Vertex{ { .x = (int16_t)sx, .y = (int16_t)sy } };
        c.dst  = psyqo::Vertex{ { .x = (int16_t)dx, .y = (int16_t)dy } };
        c.size = psyqo::Vertex{ { .w = 8, .h = 16 } };
        PS1GpuImmediate();
        g_ps1GPU->sendPrimitive(c);
    }
    g_ps1TileCopies = g_ps1TileCopies + 1;
    bool wasSolid   = (g_ps1TileSolid[dest >> 3] >> (dest & 7)) & 1;
    bool solid      = (g_ps1TileSolid[src >> 3] >> (src & 7)) & 1;
    if (solid)
        g_ps1TileSolid[dest >> 3] |= (uint8_t)(1 << (dest & 7));
    else
        g_ps1TileSolid[dest >> 3] &= (uint8_t)~(1 << (dest & 7));
    if (solid && !wasSolid && !((s_tileSticky[dest >> 3] >> (dest & 7)) & 1)) { // chunk tiles showing `dest` were skipped
        s_tileSticky[dest >> 3] |= (uint8_t)(1 << (dest & 7));                      // as empty: rebuilt before the next draw
        s_tileVisualsDirty = true;
        g_ps1TileRebuild   = g_ps1TileRebuild + 1;
    }
}

static psyqo::PrimPieces::TPageAttr s_tileTPage[4];
static uint32_t s_tileClutWord = 0, s_tileClutWordB = 0; // master CLUT: set A / set B (bank split)
static bool s_tileSetB = false;                          // the band being drawn uses set B

static void PS1TileSetup()
{
    if (s_tileClutWord)
        return;
    psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
    uint16_t c;
    memcpy(&c, &clut, 2);
    s_tileClutWord = (uint32_t)c << 16;
    psyqo::PrimPieces::ClutIndex clutB(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y + PS1_CLUT_B_DY } });
    memcpy(&c, &clutB, 2);
    s_tileClutWordB = (uint32_t)c << 16;
    for (int p = 0; p < 4; ++p) {
        s_tileTPage[p].set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
        s_tileTPage[p].setPageX((uint8_t)(PS1_TILE_PAGE_X + 2 * (p & 1)));
        s_tileTPage[p].setPageY((uint8_t)(PS1_TILE_PAGE_Y + (p >> 1)));
    }
}

// Texture flip bits (docs/43 G3; nocash psx-spx "GPU Render Rectangle Commands" / "GPU Versions"): GP0(E1h) bits 12-13
// mirror rectangles on every GPU but the old 160-pin CXD8514Q (PU-7 / EARLY-PU-8 boards; every PSone has them), which is
// why flipped tiles were 9-word textured quads (~3x a rectangle's GPU time, "GPU Rendering Timings"). Emulators differ
// (PCSX-Redux and DuckStation before 2026-10-16 ignore the bits; DuckStation b9b8366 draws them), so the GPU is tested at
// boot instead of trusting GP1(10h) index 7: two texels uploaded off screen, a 2 x 1 X-flipped rect drawn from them and read
// back by GP0(C0h). Only when they come back mirrored (texcoord u0, then u0 - 1: the rule the quads' backwards UVs give)
// are flipped tiles sprites under a flipped texpage (X flip starts at the tile's last texel column, u + 15).
// g_ps1FlipRectsOn = 0 (GDB) forces the quads; g_ps1FlipRectsForce = 1 forces flip rects (harness: CPU cost on emulators
// that don't draw them).
static bool s_flipRects = false;
volatile uint32_t g_ps1GpuVersion = 0, g_ps1FlipRectsOn = 1, g_ps1FlipTest = 0; // FlipTest: the readback word
volatile uint32_t g_ps1FlipRectsForce = 0;
void PS1DetectGpuVersion() // main.cpp start(), before anything is drawn
{
    using namespace psyqo::Hardware;
    auto ready = []() { while (!(GPU::Ctrl & (1 << 26))) {} };
    auto query = [](uint32_t index) { GPU::Ctrl = 0x10000000 | index; return (uint32_t)GPU::Data; };
    ready();
    const uint32_t area0 = query(3), area1 = query(4), offset = query(5), stat = GPU::Ctrl;
    g_ps1GpuVersion = query(7); // 2 on the new GPU (DuckStation >= a9c01a6 answers; PCSX-Redux doesn't)
    // the corner of VRAM (x 1016-1023, y 511): texels u 60-63 of the 15-bit page at x 960, y 256; free at boot
    ready();
    GPU::Data = 0xA0000000;                  // CPU -> VRAM
    GPU::Data = (511u << 16) | 1020u;
    GPU::Data = (1u << 16) | 4u;
    GPU::Data = 0x56781234u;                 // u 60 = 1234h, u 61 = 5678h
    GPU::Data = 0x0F0F0F0Fu;                 // u 62, 63
    ready();
    GPU::Data = 0xE3000000;                  // drawing area: all of VRAM, no offset
    GPU::Data = 0xE4000000 | (511u << 10) | 1023u;
    GPU::Data = 0xE5000000;
    GPU::Data = 0xE1000000 | 0x1000 | 0x400 | 0x100 | 0x10 | 15; // X flip, draw to display, 15-bit, page (960, 256)
    GPU::Data = 0x01000000;                  // clear the texture cache
    GPU::Data = 0x65808080;                  // textured raw rect, variable size
    GPU::Data = (511u << 16) | 1016u;
    GPU::Data = (255u << 8) | 61u;           // u0 61, v 255
    GPU::Data = (1u << 16) | 2u;
    ready();
    GPU::Data = 0x01000000;
    GPU::Data = 0xC0000000;                  // VRAM -> CPU: the two drawn pixels
    GPU::Data = (511u << 16) | 1016u;
    GPU::Data = (1u << 16) | 2u;
    while (!(GPU::Ctrl & (1 << 27))) {}
    g_ps1FlipTest = (uint32_t)GPU::Data;
    s_flipRects   = g_ps1FlipTest == 0x12345678u; // pixel 1016 = u 61, 1017 = u 60: mirrored
    ready();
    GPU::Data = 0xE1000000 | (stat & 0x7FF); // the texpage as it was (GPUSTAT bits 0-10), flip bits clear
    GPU::Data = 0xE3000000 | (area0 & 0xFFFFF);
    GPU::Data = 0xE4000000 | (area1 & 0xFFFFF);
    GPU::Data = 0xE5000000 | (offset & 0x3FFFFF);
}

// Screen lines [sy, sy + h) showing layer lines [ly, ly + h) (inside one tile row) scrolled to chunkX.
// Bands touching a screen edge are extended to whole tile rows (the GPU clips).
// Raw textured quad(s) for rows [ty0, ty0 + h) of a tile at screen (sx, sy), flipped per `dir`: GP0 2Ch,
// corners A B C D (top-left, top-right, bottom-left, bottom-right), UVs running backwards for flips
// (the Nexus PS1DrawTilePiece math: u -1 / 256 addressed from the neighbouring 8-bit page; a v edge
// past the page gets the last row as its own constant-v strip). Returns words written (9 or 18).
static int PS1TileQuadWords(uint32_t *w, int sx, int sy, int tileIndex, int ty0, int h, int dir)
{
    int page = tileIndex >> 8, pt = tileIndex & 0xFF;
    int u = (pt & 0xF) << 4, v = (pt >> 4) << 4;
    int pageX = PS1_TILE_PAGE_X + 2 * (page & 1), pageY = PS1_TILE_PAGE_Y + (page >> 1);
    int uL = (dir & FLIP_X) ? u + 15 : u;
    int uR = (dir & FLIP_X) ? u - 1 : u + 16;
    int dv = (dir & FLIP_Y) ? -1 : 1;
    int vT = (dir & FLIP_Y) ? v + 15 - ty0 : v + ty0;
    if (uR < 0)
        pageX -= 1, uL += 128, uR += 128;
    else if (uR > 255)
        pageX += 1, uL -= 128, uR -= 128;
    uint32_t tpage = (uint32_t)(pageX | (pageY << 4) | (1 << 7)); // GP0 E1h bits: page X / Y, 8-bit
    uint32_t clut  = s_tileSetB ? s_tileClutWordB : s_tileClutWord;
    auto quad = [&](uint32_t *q, int y, int hh, int v0, int v1) {
        uint32_t xl = (uint16_t)sx, xr = (uint16_t)(sx + TILE_SIZE);
        uint32_t yt = (uint32_t)(uint16_t)y << 16, yb = (uint32_t)(uint16_t)(y + hh) << 16;
        q[0] = 0x2C808080;
        q[1] = yt | xl;
        q[2] = clut | ((uint32_t)(uint8_t)v0 << 8) | (uint8_t)uL;
        q[3] = yt | xr;
        q[4] = (tpage << 16) | ((uint32_t)(uint8_t)v0 << 8) | (uint8_t)uR;
        q[5] = yb | xl;
        q[6] = ((uint32_t)(uint8_t)v1 << 8) | (uint8_t)uL;
        q[7] = yb | xr;
        q[8] = ((uint32_t)(uint8_t)v1 << 8) | (uint8_t)uR;
    };
    int vB = vT + h * dv;
    if (vB < 0 || vB > 255) {
        int vLast = vT + (h - 1) * dv;
        int n     = 0;
        if (h > 1) {
            quad(w, sy, h - 1, vT, vLast);
            n = 9;
        }
        quad(w + n, sy + h - 1, 1, vLast, vLast);
        return n + 9;
    }
    quad(w, sy, h, vT, vB);
    return 9;
}

// Screen lines [sy, sy + h) showing layer lines [ly, ly + h) (inside one tile row) scrolled to chunkX.
// Bands touching a screen edge are extended to whole tile rows (the GPU clips). Tiles of one band never
// overlap, so they are grouped: whole unflipped tiles per texture page (texpage + bulk 16x16 sprites),
// then flipped / partial tiles as raw quads (each carries its texpage).
static void PS1ChainFlipTiles(const uint32_t *f, int n, int sy, int ty0, int h);
// FLIP (docs/43 G3): flipped tiles recorded for PS1ChainFlipTiles' flip rects; else textured quads written in the loop,
// as before (two instances: the quad path keeps its registers).
template <bool FLIP>
static void PS1DrawTileBandT(const TileLayer *layer, byte above, int sy, int h, int ly, int chunkX, int fullW)
{
    g_ps1TileBandCount = g_ps1TileBandCount + 1;
    int ty0            = ly & 0xF;
    if (sy == 0) {
        sy -= ty0, h += ty0, ty0 = 0;
    }
    if (sy + h == SCREEN_YSIZE)
        h = TILE_SIZE - ty0;
    const ushort *chunkRow = &layer->tiles[(ly >> 7) * TILELAYER_CHUNK_W];
    int tileRow            = ((ly & 0x7F) >> 4) << 3;
    uint16_t want          = above ? 1 : 0; // visual plane
    uint32_t yWord         = (uint32_t)(uint16_t)sy << 16;
    uint32_t clutWord      = s_tileSetB ? s_tileClutWordB : s_tileClutWord;
    bool whole             = h == TILE_SIZE;
    int prims              = 0;
    uint16_t rowBit        = 1 << ((tileRow >> 3) * 2 + (above ? 1 : 0));
    uint32_t pageXY[4][24], pageTex[4][24]; // one band: <= 21 tiles across 320 px
    int pageN[4] = { 0, 0, 0, 0 };
    uint32_t partial[4][24 * 3]; // unflipped partial rows: variable sprites {xy, uv|clut, wh} per page
    int partialN[4] = { 0, 0, 0, 0 };
    uint32_t whWord = (uint32_t)TILE_SIZE | ((uint32_t)h << 16);
    uint32_t flipped[FLIP ? 24 : 1]; // FLIP: flipped tiles, x | tile index << 16 | dir << 28
    uint32_t quads[FLIP ? 1 : 24 * 18];
    int quadWords = 0, quadN = 0;
    int px = chunkX & ~0xF;
    int sx = -(chunkX & 0xF);
    // Chunk by chunk: one chunk-row lookup, then its tiles' packed visuals read in sequence.
    while (sx < SCREEN_XSIZE) {
        int first = (px & 0x7F) >> 4, count = 8 - first;
        int chunk = chunkRow[px >> 7];
        if (s_chunkRowPlanes[chunk] & rowBit) {
            const ushort *vis = &tiles128x128.ps1Tile[(chunk << 6) + tileRow + first];
            int x             = sx;
            for (int k = 0; k < count && x < SCREEN_XSIZE; ++k, x += TILE_SIZE) {
                uint16_t tv = vis[k];
                if ((tv >> PS1_TV_PLANE_SHIFT) != want || !PS1TileSolid(tv & PS1_TV_INDEX))
                    continue;
                ++prims;
                int dir       = (tv >> PS1_TV_DIR_SHIFT) & 3;
                int tileIndex = tv & PS1_TV_INDEX;
                if (dir == FLIP_NONE) {
                    int page = tileIndex >> 8, pt = tileIndex & 0xFF;
                    if (whole) {
                        int n            = pageN[page]++;
                        pageXY[page][n]  = yWord | (uint16_t)x;
                        pageTex[page][n] = (uint32_t)(((pt & 0xF) << 4) | ((pt & 0xF0) << 8)) | clutWord;
                    }
                    else { // rows [ty0, ty0 + h): a sprite (4 words) instead of a quad (9)
                        uint32_t *w = &partial[page][partialN[page]++ * 3];
                        w[0]        = yWord | (uint16_t)x;
                        w[1]        = (uint32_t)(((pt & 0xF) << 4) | ((((pt & 0xF0)) + ty0) << 8)) | clutWord;
                        w[2]        = whWord;
                    }
                }
                else if constexpr (FLIP) { // flipped: drawn after the loop as flip rects (PS1ChainFlipTiles)
                    flipped[quadN++] = (uint32_t)(uint16_t)x | (uint32_t)tileIndex << 16 | (uint32_t)dir << 28;
                }
                else {
                    quadWords += PS1TileQuadWords(&quads[quadWords], x, sy, tileIndex, ty0, h, dir);
                    ++quadN;
                }
            }
        }
        sx += count * TILE_SIZE;
        px += count * TILE_SIZE;
        if (px >= fullW)
            px -= fullW;
    }
    g_ps1TileFlipped = g_ps1TileFlipped + quadN;
    if (!(g_ps1DebugDrawMask & 2)) {
        for (int p = 0; p < 4; ++p)
            if (pageN[p] || partialN[p]) {
                PS1ChainTPage(s_tileTPage[p]);
                PS1ChainSprite16RawN(pageXY[p], pageTex[p], pageN[p]);
                PS1ChainSpriteRawN(partial[p], partialN[p]);
            }
        if constexpr (FLIP) {
            if (quadN)
                PS1ChainFlipTiles(flipped, quadN, sy, ty0, h);
        }
        else
            PS1ChainPrimsRawN(quads, 9, quadWords / 9);
    }
    g_ps1LayerPrims = g_ps1LayerPrims + prims;
}
static void PS1DrawTileBand(const TileLayer *layer, byte above, int sy, int h, int ly, int chunkX, int fullW)
{
    if ((s_flipRects || g_ps1FlipRectsForce) && g_ps1FlipRectsOn)
        PS1DrawTileBandT<true>(layer, above, sy, h, ly, chunkX, fullW);
    else
        PS1DrawTileBandT<false>(layer, above, sy, h, ly, chunkX, fullW);
}
// PS1DrawTileBand's flipped tiles (n entries x | tile index << 16 | dir << 28, rows [ty0, ty0 + h) at screen line sy):
// as flip rects (docs/43 G3): per tile page and direction the page's texpage with the flip bits, then the sprites (16 x 16,
// or variable for a partial band) with the texcoords of their first texel (u + 15 for an X flip, v + 15 - ty0 for a Y flip).
static __attribute__((noinline)) void PS1ChainFlipTiles(const uint32_t *f, int n, int sy, int ty0, int h)
{
    const uint32_t yWord = (uint32_t)(uint16_t)sy << 16, whWord = (uint32_t)TILE_SIZE | ((uint32_t)h << 16);
    int keyN[12] = {}, start[12], fill[12], pos = 0;
    for (int i = 0; i < n; ++i) ++keyN[((f[i] >> 24) & 3) * 3 + (f[i] >> 28) - 1]; // page * 3 + dir - 1
    for (int k = 0; k < 12; ++k) start[k] = fill[k] = pos, pos += keyN[k];
    uint32_t w[24 * 3];
    const bool whole = h == TILE_SIZE;
    for (int i = 0; i < n; ++i) {
        int tileIndex = (f[i] >> 16) & 0xFFF, dir = f[i] >> 28, page = tileIndex >> 8, pt = tileIndex & 0xFF;
        int u = ((pt & 0xF) << 4) + ((dir & FLIP_X) ? 15 : 0), v = (pt & 0xF0) + ((dir & FLIP_Y) ? 15 - ty0 : ty0);
        int at = fill[page * 3 + dir - 1]++;
        uint32_t xy = yWord | (f[i] & 0xFFFF), tex = (uint32_t)(u | (v << 8)) | (s_tileSetB ? s_tileClutWordB : s_tileClutWord);
        if (whole)
            w[at] = xy, w[24 + at] = tex; // Sprite16RawN: xy and texinfo arrays
        else
            w[3 * at] = xy, w[3 * at + 1] = tex, w[3 * at + 2] = whWord; // SpriteRawN: {xy, texinfo, wh}
    }
    for (int k = 0; k < 12; ++k) {
        if (!keyN[k])
            continue;
        int page = k / 3, dir = k % 3 + 1;
        uint16_t raw;
        __builtin_memcpy(&raw, &s_tileTPage[page], 2);
        raw = (uint16_t)(raw | (dir << 12)); // GP0(E1h) bit 12 X flip, bit 13 Y flip (RSDK FLIP_X 1, FLIP_Y 2)
        psyqo::PrimPieces::TPageAttr attr;
        __builtin_memcpy(&attr, &raw, 2);
        PS1ChainTPage(attr);
        if (whole)
            PS1ChainSprite16RawN(&w[start[k]], &w[24 + start[k]], keyN[k]);
        else
            PS1ChainSpriteRawN(&w[3 * start[k]], keyN[k]);
    }
    g_ps1TileFlipRects = g_ps1TileFlipRects + n;
}

// A tile band under the bank split: set B below the split line, cut there when it crosses it.
static void PS1DrawTileBandSplit(const TileLayer *layer, byte above, int sy, int h, int ly, int chunkX, int fullW)
{
    int y0 = s_splitY;
    if (!y0 || sy + h <= y0) {
        PS1DrawTileBand(layer, above, sy, h, ly, chunkX, fullW);
        return;
    }
    PS1EnsureSetB();
    s_tileSetB = true;
    if (sy >= y0) {
        PS1DrawTileBand(layer, above, sy, h, ly, chunkX, fullW);
    }
    else {
        int h1     = y0 - sy;
        s_tileSetB = false;
        PS1DrawTileBand(layer, above, sy, h1, ly, chunkX, fullW);
        s_tileSetB = true;
        PS1DrawTileBand(layer, above, y0, h - h1, ly + h1, chunkX, fullW);
        g_ps1SplitCuts = g_ps1SplitCuts + 1;
    }
    s_tileSetB = false;
}

// BG line strips (tools/bgstrips/build_bgstrips.py -> Data/Stages/<Stage>/BGStrips<suffix>.bin, format
// there; docs/30 phase 3.5): BG layer lines pre-rendered at build time as 8-bit rows of one horizontal period, in
// VRAM the stage's atlas leaves free. A band of lines in a strip region costs 1-3 sprites instead of ~21
// tiles (per-line parallax would otherwise make one band per line). From the Nexus / Sonic CD ports.
#define PS1_STRIP_MAX    32 // Sonic 2: 18 at most (Wing Fortress with Tails); tools/bgstrips MAX_REGIONS
#if PS1_GAME == 1
// Sonic 1: 'BGS3' strips (tools/bgstrips build_stage_seg, docs/37 phase 3): a region's period is a map of up to 32
// 256-texel pieces, pieces with the same content share one VRAM spot, and rows of <= 15 colours are 4-bit with their
// own CLUT (appended to the atlas' CLUTs, filled with the palette like them). Green Hill's water: ~100 tile bands a
// frame -> strip bands.
#define PS1_STRIP_PIECES 32 // tools/bgstrips SEG_PIECES
#define PS1_STRIP_MAGIC  "BGS3"
#else
#define PS1_STRIP_PIECES 9 // tools/bgstrips MAX_PIECES
#define PS1_STRIP_MAGIC  "BGS2"
#endif
struct PS1StripRegion {
    uint8_t layer, plane, pieces, pad; // pad (BGS3): flags, bit 0 = 4-bit
    uint16_t line0, lines, period;
    uint16_t x[PS1_STRIP_PIECES], y[PS1_STRIP_PIECES], w[PS1_STRIP_PIECES]; // piece VRAM origin (hw) + width (texels)
#if PS1_GAME == 1
    uint32_t clut[2]; // 4-bit regions: the CLUT word for set A / set B (bank split)
#endif
};
static PS1StripRegion s_strips[PS1_STRIP_MAX];
static int s_stripCount = 0;
static uint8_t s_stripRow[LAYER_COUNT][2][TILELAYER_LINESCROLL_COUNT / TILE_SIZE]; // region + 1 per tile row
volatile uint32_t g_ps1StripRegions = 0; // regions loaded for this stage
volatile uint32_t g_ps1StripBytes   = 0; // strip texel bytes uploaded by the last load

// Loads the stage's strips for the player variant (the atlas' one: BGStrips<suffix>.bin sits around that atlas) into
// VRAM. Immediate uploads: call during the stage load, after PS1AtlasLoad.
void PS1LoadBGStrips(const char *folder, int player)
{
    s_stripCount = 0;
    g_ps1StripRegions = 0;
    memset(s_stripRow, 0, sizeof(s_stripRow));
    char path[96];
    static const char *const suffix[4] = { "", "_t", "_k", "_st" };
    snprintf(path, sizeof(path), "Data/Stages/%s/BGStrips%s.bin", folder, player >= 0 && player < 4 ? suffix[player] : "");
    FILE *f = fopen(path, "rb");
    if (!f)
        return; // no HScroll BG strips for this stage: tiles only
    uint8_t head[8];
    int count = 0;
    bool ok   = fread(head, 1, 8, f) == 8 && !memcmp(head, PS1_STRIP_MAGIC, 4);
    if (ok) {
        count = head[4] | (head[5] << 8);
        ok    = count <= PS1_STRIP_MAX;
    }
#if PS1_GAME == 1
    // the file's CLUTs go after the atlas' (PS1BuildPalette / PS1ReservePaletteRows fill and chain them)
    int clutBase = g_ps1AtlasClutCount, clutCount = ok ? head[6] | (head[7] << 8) : 0;
    ok = ok && clutBase + clutCount <= PS1_ATLAS_MAX_CLUTS;
    for (int k = 0; ok && k < clutCount; ++k) {
        uint8_t c[20];
        ok = fread(c, 1, 20, f) == 20;
        PS1AtlasClut &dst = g_ps1AtlasCluts[clutBase + k];
        dst.x = c[0] | (c[1] << 8);
        dst.y = c[2] | (c[3] << 8);
        memcpy(dst.map, c + 4, 16);
    }
#endif
    for (int i = 0; ok && i < count; ++i) {
        uint8_t e[10 + 6 * PS1_STRIP_PIECES];
        ok = fread(e, 1, sizeof(e), f) == sizeof(e);
        PS1StripRegion &r = s_strips[i];
        r.layer  = e[0];
        r.plane  = e[1] & 1;
        r.line0  = e[2] | (e[3] << 8);
        r.lines  = e[4] | (e[5] << 8);
        r.period = e[6] | (e[7] << 8);
        r.pieces = e[8];
        for (int k = 0; k < PS1_STRIP_PIECES; ++k) {
            r.x[k] = e[10 + 6 * k] | (e[11 + 6 * k] << 8);
            r.y[k] = e[12 + 6 * k] | (e[13 + 6 * k] << 8);
            r.w[k] = e[14 + 6 * k] | (e[15 + 6 * k] << 8);
        }
        ok = ok && r.pieces <= PS1_STRIP_PIECES && r.layer < LAYER_COUNT && r.line0 + r.lines <= TILELAYER_LINESCROLL_COUNT;
#if PS1_GAME == 1
        r.pad = e[9];
        if (r.pad & 1) {
            int k = r.pad >> 1;
            ok    = ok && k < clutCount;
            if (ok) {
                const PS1AtlasClut &c = g_ps1AtlasCluts[clutBase + k];
                for (int s = 0; s < 2; ++s) {
                    psyqo::PrimPieces::ClutIndex ci(psyqo::Vertex{ { .x = (int16_t)c.x, .y = (int16_t)(c.y + (s ? PS1_CLUT_B_DY : 0)) } });
                    uint16_t cv;
                    memcpy(&cv, &ci, 2);
                    r.clut[s] = (uint32_t)cv << 16;
                }
            }
        }
#endif
    }
    const int stagingBytes = 16 * 1024;
    uint8_t *staging       = ok ? (uint8_t *)psyqo_malloc(stagingBytes) : nullptr;
    uint32_t bytes         = 0;
    if (staging) {
        PS1GpuImmediate();
        for (int i = 0; ok && i < count; ++i) {
            PS1StripRegion &r = s_strips[i];
            for (int k = 0; ok && k < r.pieces; ++k) {
#if PS1_GAME == 1
                bool shared = false; // an earlier piece's spot: stored once
                for (int j = 0; j < k; ++j) shared = shared || (r.x[j] == r.x[k] && r.y[j] == r.y[k]);
                if (shared)
                    continue;
                int rowBytes     = (r.pad & 1) ? r.w[k] / 2 : r.w[k]; // 4-bit: two texels a byte
                int rowsPerChunk = stagingBytes / rowBytes;
                for (int row = 0; ok && row < r.lines; row += rowsPerChunk) {
                    int n = r.lines - row < rowsPerChunk ? r.lines - row : rowsPerChunk;
                    ok    = fread(staging, 1, n * rowBytes, f) == (size_t)(n * rowBytes);
                    if (ok)
                        g_ps1GPU->uploadToVRAM((const uint16_t *)staging,
                                               psyqo::Rect{ { (int16_t)r.x[k], (int16_t)(r.y[k] + row) }, { (int16_t)(rowBytes / 2), (int16_t)n } });
                    bytes += n * rowBytes;
                }
#else
                int rowsPerChunk = stagingBytes / r.w[k];
                for (int row = 0; ok && row < r.lines; row += rowsPerChunk) {
                    int n = r.lines - row < rowsPerChunk ? r.lines - row : rowsPerChunk;
                    ok    = fread(staging, 1, n * r.w[k], f) == (size_t)(n * r.w[k]);
                    if (ok)
                        g_ps1GPU->uploadToVRAM((const uint16_t *)staging,
                                               psyqo::Rect{ { (int16_t)r.x[k], (int16_t)(r.y[k] + row) }, { (int16_t)(r.w[k] / 2), (int16_t)n } });
                    bytes += n * r.w[k];
                }
#endif
            }
            for (int row = r.line0 >> 4; row < (r.line0 + r.lines) >> 4; ++row) s_stripRow[r.layer][r.plane][row] = (uint8_t)(i + 1);
        }
        psyqo_free(staging);
    }
    else {
        ok = false;
    }
    fclose(f);
    if (!ok) {
        ramsyscall_printf("[strips] bad or unreadable %s: tiles only\n", path);
        memset(s_stripRow, 0, sizeof(s_stripRow));
        return;
    }
    s_stripCount      = count;
    g_ps1StripRegions = count;
    g_ps1StripBytes   = bytes;
#if PS1_GAME == 1
    g_ps1AtlasClutCount = clutBase + clutCount;
#endif
}

// Strip sprites of one region, grouped by piece: one walk writes the runs in band order with their
// piece, a scatter pass groups them by piece (counting sort), then each piece is chained as one texpage
// + bulk sprites. {xy, u|v<<8|clut<<16, w|h<<16} words; a band spans <= 3 runs, so 768 runs cover 240
// bands (2 x 9 KB: user decision 2026-09-24, 60 fps over RAM).
#define PS1_STRIP_RUNS 768
static uint32_t s_stripRuns[PS1_STRIP_RUNS * 3];   // grouped by piece
static uint32_t s_stripWalk[PS1_STRIP_RUNS * 3];   // band order
static uint8_t s_stripWalkPiece[PS1_STRIP_RUNS];
static int s_stripWalkCount = 0;

// Walks a band of lines [ly, ly + h) in a strip region scrolled to chunkX (period from chunkX % period
// across 320 px; a run <= 256 texels = one piece = one page): runs appended in band order, counted per piece.
static void PS1WalkStripBand(const PS1StripRegion &r, int sy, int h, int ly, int chunkX, uint32_t clut, int *count)
{
    int row = ly - r.line0;
    int t   = chunkX % r.period;
    int k = 0, s0 = 0;
    while (t >= s0 + r.w[k]) s0 += r.w[k++];
    for (int x = 0; x < SCREEN_XSIZE;) {
        int u   = t - s0;
        int run = r.w[k] - u;
        if (run > SCREEN_XSIZE - x)
            run = SCREEN_XSIZE - x;
        int n = s_stripWalkCount;
        if (n < PS1_STRIP_RUNS) {
            uint32_t *w         = &s_stripWalk[n * 3];
            w[0]                = (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)sy << 16);
            w[1]                = (uint32_t)u | ((uint32_t)((r.y[k] & 0xFF) + row) << 8) | clut;
            w[2]                = (uint32_t)run | ((uint32_t)h << 16);
            s_stripWalkPiece[n] = (uint8_t)k;
            s_stripWalkCount    = n + 1;
            ++count[k];
        }
        x += run;
        t += run;
        s0 += r.w[k];
        if (++k == r.pieces || t >= r.period) { // wrap to the period start
            if (t >= r.period)
                t -= r.period;
            k = 0, s0 = 0;
            while (t >= s0 + r.w[k]) s0 += r.w[k++];
        }
    }
}

struct PS1StripBand {
    short sy, h, ly, chunkX;
    uint8_t region, setB; // setB: below the bank split line
};
static PS1StripBand s_stripBands[SCREEN_YSIZE + 1]; // + the band cut at the bank split line
static short s_lineX[SCREEN_YSIZE];

// upstream DrawHLineScrollLayer (v4 SW path): the same scroll-state update (BG autoscroll, parallax line
// positions: game state) and per-line X (parallax line, deformation above / below the water line, wrap),
// drawn as bands of lines sharing one X (a band never crosses a tile row).
// docs/43 C9: the per-line loops at -O2 (the rest of this file is -Os; the tile band itself measured slower at -O2:
// Hidden Palace S&T tiles 109.6 -> 116.9)
__attribute__((optimize("O2"))) void DrawHLineScrollLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;
    if (s_tileVisualsDirty) // Copy16x16Tile made tiles solid since the last draw (PS1Copy16x16Tile)
        PS1RebuildTileVisuals();
    uint32_t t0       = PS1Hblanks();
    g_ps1ProfLayer[4] = g_ps1ProfLayer[4] + 1;
    int layerwidth    = layer->xsize;
    int layerheight   = layer->ysize;
    byte above        = layerID >= tLayerMidPoint;
    bool fg           = !activeTileLayers[layerID];
    int *deformationData, *deformationDataW;

    int yscrollOffset = 0;
    if (!fg) { // BG Layer
        int yScroll    = yScrollOffset * layer->parallaxFactor >> 8;
        int fullheight = layerheight << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullheight << 16)
            layer->scrollPos -= fullheight << 16;
        yscrollOffset    = (yScroll + (layer->scrollPos >> 16)) % fullheight;
        layerheight      = fullheight >> 7;
        deformationData  = &bgDeformationData2[(byte)(yscrollOffset + layer->deformationOffset)];
        deformationDataW = &bgDeformationData3[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }
    else { // FG Layer
        lastXSize     = layer->xsize;
        yscrollOffset = yScrollOffset;
        for (int i = 0; i < PARALLAX_COUNT; ++i) hParallax.linePos[i] = xScrollOffset;
        deformationData  = &bgDeformationData0[(byte)(yscrollOffset + layer->deformationOffset)];
        deformationDataW = &bgDeformationData1[(byte)(yscrollOffset + waterDrawPos + layer->deformationOffsetW)];
    }

    if (layer->type == LAYER_HSCROLL) {
        if (lastXSize != layerwidth) {
            int fullLayerwidth = layerwidth << 7;
            for (int i = 0; i < hParallax.entryCount; ++i) {
                hParallax.linePos[i] = xScrollOffset * hParallax.parallaxFactor[i] >> 8;
                if (hParallax.scrollPos[i] > fullLayerwidth << 16)
                    hParallax.scrollPos[i] -= fullLayerwidth << 16;
                if (hParallax.scrollPos[i] < 0)
                    hParallax.scrollPos[i] += fullLayerwidth << 16;
                hParallax.linePos[i] += hParallax.scrollPos[i] >> 16;
                hParallax.linePos[i] %= fullLayerwidth;
            }
        }
        lastXSize = fg ? -1 : layerwidth;
    }
    // (end of the upstream state update)

    if (!s_frameOpen)
        return;
    PS1TileSetup();
    // PS1-sized layer buffers (Scene.hpp): the FG holds PS1_LAYER0_CHUNK_H chunk rows, the BG layers
    // TILELAYER_CHUNK_H; their line scroll covers the BG heights only (the FG's is all zero upstream: index 0).
    int rows  = fg ? PS1_LAYER0_CHUNK_H : TILELAYER_CHUNK_H;
    rows      = layerheight < rows ? layerheight : rows;
    int fullW = layerwidth << 7;
    int fullH = rows << 7;
    int tileYPos = yscrollOffset % (layerheight << 7);
    if (tileYPos < 0)
        tileYPos += layerheight << 7;
    tileYPos %= fullH;

    // The FG without deformation scrolls every line by xScrollOffset (upstream sets every
    // hParallax.linePos to it above): one band per tile row, no per-line pass (the Nexus fast path). Its line
    // scroll is all zero (Scene.cpp), so only parallax entry 0's deform flag matters (BG entries' ripples don't).
    bool uniform = fg && !hParallax.deform[0];
    int ly = tileYPos;
    if (uniform) {
        int chunkX = xScrollOffset % fullW;
        if (chunkX < 0)
            chunkX += fullW;
        for (int y = 0; y < SCREEN_YSIZE;) {
            int h = TILE_SIZE - (ly & 0xF);
            if (h > SCREEN_YSIZE - y)
                h = SCREEN_YSIZE - y;
            PS1DrawTileBandSplit(layer, above, y, h, ly, chunkX, fullW);
            y += h;
            ly += h;
            if (ly >= fullH)
                ly -= fullH;
        }
        g_ps1LayerHbl              = g_ps1LayerHbl + PS1HblanksSince(t0);
        g_ps1ProfLayer[0]          = g_ps1ProfLayer[0] + PS1HblanksSince(t0);
        g_ps1ProfSlot[layerID & 3] = PS1HblanksSince(t0);
        return;
    }
    for (int y = 0; y < SCREEN_YSIZE; ++y) {
        byte idx   = fg || ly >= TILELAYER_LINESCROLL_COUNT ? 0 : layer->lineScroll[ly];
        int chunkX = hParallax.linePos[idx];
        if (y < waterDrawPos) {
            if (hParallax.deform[idx])
                chunkX += *deformationData;
            ++deformationData;
        }
        else {
            if (hParallax.deform[idx])
                chunkX += *deformationDataW;
            ++deformationDataW;
        }
        if (chunkX < 0)
            chunkX += fullW;
        if (chunkX >= fullW)
            chunkX -= fullW;
        s_lineX[y] = (short)chunkX;
        if (++ly >= fullH)
            ly = 0;
    }
    g_ps1ProfLayer[1]       = g_ps1ProfLayer[1] + PS1HblanksSince(t0);
    uint32_t tTiles         = 0;
    int stripBands          = 0;
    int layerIdx            = activeTileLayers[layerID];
    const uint8_t *stripRow = s_stripCount && !fg ? s_stripRow[layerIdx][above] : nullptr;
    ly                      = tileYPos;
    for (int y = 0; y < SCREEN_YSIZE;) {
        int chunkX = s_lineX[y];
        int ri     = stripRow ? stripRow[ly >> 4] : 0;
        int h      = 1;
        if (ri) { // strip region: a band runs until the X scroll changes or the region ends
            const PS1StripRegion &r = s_strips[ri - 1];
            int end                 = r.line0 + r.lines;
            while (y + h < SCREEN_YSIZE && ly + h < end && s_lineX[y + h] == chunkX) ++h;
            int y0 = s_splitY, h1 = h;
            if (y0 > y && y0 < y + h) // cut at the bank split line
                h1 = y0 - y;
            PS1StripBand &b = s_stripBands[stripBands++]; // drawn after the loop, piece by piece
            b.sy = (short)y, b.h = (short)h1, b.ly = (short)ly, b.chunkX = (short)chunkX, b.region = (uint8_t)(ri - 1);
            b.setB = y0 && y >= y0;
            if (h1 < h) {
                PS1StripBand &c = s_stripBands[stripBands++];
                c    = b;
                c.sy = (short)y0, c.h = (short)(h - h1), c.ly = (short)(ly + h1), c.setB = 1;
                g_ps1SplitCuts = g_ps1SplitCuts + 1;
            }
            if (y0 && y + h > y0)
                PS1EnsureSetB();
        }
        else {
            int rowLeft = TILE_SIZE - (ly & 0xF);
            while (h < rowLeft && y + h < SCREEN_YSIZE && s_lineX[y + h] == chunkX) ++h;
            uint32_t tb = PS1Hblanks();
            PS1DrawTileBandSplit(layer, above, y, h, ly, chunkX, fullW);
            tTiles += PS1HblanksSince(tb);
        }
        y += h;
        ly += h;
        if (ly >= fullH)
            ly -= fullH;
    }
    g_ps1ProfLayer[2]   = g_ps1ProfLayer[2] + tTiles;
    g_ps1StripBandCount = g_ps1StripBandCount + stripBands;
    uint32_t t2         = PS1Hblanks();
    uint32_t stripClut[2] = { s_tileClutWord, s_tileClutWordB }; // master CLUT, set A / B (PS1TileSetup)
    for (int i = 0; i < stripBands;) { // strip bands grouped by region, chained piece by piece
        int region = s_stripBands[i].region, j = i;
        while (j < stripBands && s_stripBands[j].region == region) ++j;
        const PS1StripRegion &r          = s_strips[region];
        int count[PS1_STRIP_PIECES]      = {};
        int pos[PS1_STRIP_PIECES], total = 0;
        s_stripWalkCount                 = 0;
#if PS1_GAME == 1
        const uint32_t *clut = (r.pad & 1) ? r.clut : stripClut;
        auto mode = (r.pad & 1) ? psyqo::Prim::TPageAttr::ColorMode::Tex4Bits : psyqo::Prim::TPageAttr::ColorMode::Tex8Bits;
        for (int b = i; b < j; ++b)
            PS1WalkStripBand(r, s_stripBands[b].sy, s_stripBands[b].h, s_stripBands[b].ly, s_stripBands[b].chunkX,
                             clut[s_stripBands[b].setB], count);
#else
        for (int b = i; b < j; ++b)
            PS1WalkStripBand(r, s_stripBands[b].sy, s_stripBands[b].h, s_stripBands[b].ly, s_stripBands[b].chunkX,
                             stripClut[s_stripBands[b].setB], count);
#endif
        for (int k = 0; k < r.pieces; ++k) {
            pos[k] = total;
            total += count[k];
        }
        for (int q = 0; q < s_stripWalkCount; ++q) { // scatter: group by piece, band order kept
            uint32_t *d = &s_stripRuns[pos[s_stripWalkPiece[q]]++ * 3];
            uint32_t *w = &s_stripWalk[q * 3];
            d[0] = w[0], d[1] = w[1], d[2] = w[2];
        }
        for (int k = 0, start = 0; k < r.pieces; start += count[k++]) {
            int n = count[k];
            if (start + n > PS1_STRIP_RUNS)
                n = PS1_STRIP_RUNS - start;
            if (n <= 0)
                continue;
            psyqo::PrimPieces::TPageAttr attr;
#if PS1_GAME == 1
            attr.set(mode);
#else
            attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
#endif
            attr.setPageX((uint8_t)(r.x[k] >> 6));
            attr.setPageY((uint8_t)(r.y[k] >> 8));
            PS1ChainTPage(attr);
            PS1ChainSpriteRawN(&s_stripRuns[start * 3], n);
            g_ps1LayerPrims = g_ps1LayerPrims + n;
        }
        i = j;
    }
    g_ps1ProfLayer[3]          = g_ps1ProfLayer[3] + PS1HblanksSince(t2);
    g_ps1LayerHbl              = g_ps1LayerHbl + PS1HblanksSince(t0);
    g_ps1ProfSlot[layerID & 3] = PS1HblanksSince(t0);
}

// upstream DrawVLineScrollLayer's scroll-state update (v4 SW path, incl. its `scrollPos += scrollPos << 16`);
// drawing VScroll layers: none in Sonic 2 so far (counted).
void DrawVLineScrollLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;
    int layerwidth  = layer->xsize;
    int layerheight = layer->ysize;
    if (activeTileLayers[layerID]) { // BG Layer
        int fullLayerwidth = layerwidth << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullLayerwidth << 16)
            layer->scrollPos -= fullLayerwidth << 16;
    }
    else { // FG Layer
        lastYSize            = layer->ysize;
        vParallax.linePos[0] = yScrollOffset;
        vParallax.deform[0]  = true;
    }
    if (layer->type == LAYER_VSCROLL) {
        if (lastYSize != layerheight) {
            int fullLayerheight = layerheight << 7;
            for (int i = 0; i < vParallax.entryCount; ++i) {
                vParallax.linePos[i] = yScrollOffset * vParallax.parallaxFactor[i] >> 8;
                vParallax.scrollPos[i] += vParallax.scrollPos[i] << 16;
                if (vParallax.scrollPos[i] > fullLayerheight << 16)
                    vParallax.scrollPos[i] -= fullLayerheight << 16;
                vParallax.linePos[i] += vParallax.scrollPos[i] >> 16;
                vParallax.linePos[i] %= fullLayerheight;
            }
            layerheight = fullLayerheight >> 7;
        }
        lastYSize = layerheight;
    }
    g_ps1DrawTodo = g_ps1DrawTodo + 1;
}

// Sonic 2 has no 3D floor / sky layer (docs/30 phase 1): counted if one ever comes.
volatile uint32_t g_ps1Draw3DLayer = 0;
void Draw3DFloorLayer(int layerID) { g_ps1Draw3DLayer = g_ps1Draw3DLayer + 1; }
void Draw3DSkyLayer(int layerID) { g_ps1Draw3DLayer = g_ps1Draw3DLayer + 1; }

// ---------------------------------------------------------------------------------------------------
// Shapes

// Rectangle at a PS1 alpha level (see PS1AlphaLevel): 50 % = B/2 + F/2, 75 % = twice.
static void PS1ChainAlphaRect(int XPos, int YPos, int width, int height, int R, int G, int B, int level)
{
    if (g_ps1ChainSkipPrims)
        return;
    uint32_t w[3] = { 0x60000000u | (uint32_t)(R & 0xFF) | (uint32_t)(G & 0xFF) << 8 | (uint32_t)(B & 0xFF) << 16,
                      (uint32_t)(uint16_t)XPos | (uint32_t)(uint16_t)YPos << 16, (uint32_t)(uint16_t)width | (uint32_t)(uint16_t)height << 16 };
    if (level >= 3) {
        PS1ChainFlatRaw(w, 3); // GP0 60h: flat rectangle
        return;
    }
    psyqo::PrimPieces::TPageAttr attr; // untextured semi-transparency takes its mode from the texpage
    attr.set(psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront);
    PS1ChainTPage(attr);
    w[0] |= 0x02000000; // semi-transparent
    PS1ChainFlatRaw(w, 3);
    if (level == 2)
        PS1ChainFlatRaw(w, 3);
}

// The pending full-screen fade turned out not to end the frame: draw it now (4 levels).
static void PS1FadeHook()
{
    if (!s_fadePending)
        return;
    s_fadePending = false;
    int level     = PS1AlphaLevel(s_fadeA);
    if (level)
        PS1ChainAlphaRect(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, s_fadeR, s_fadeG, s_fadeB, level);
}

void DrawRectangle(int XPos, int YPos, int width, int height, int R, int G, int B, int A)
{
    if (A <= 0 || width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    if (A > 0xFF)
        A = 0xFF;
    PS1FadeHook(); // an earlier pending fade is followed by this: draw it first
    // Only in a frame that started with a clear: a repeated frame's copied pixels aren't CLUT-based
    // any more, so a CLUT fade wouldn't reach them (paused stage mode, PC's persistent framebuffer).
    if (A < 0xFF && s_frameFresh && XPos <= 0 && YPos <= 0 && XPos + width >= SCREEN_XSIZE && YPos + height >= SCREEN_YSIZE) {
        s_fadePending = true; // exact CLUT fade at FlipScreen unless something is drawn after it
        s_fadeR = R, s_fadeG = G, s_fadeB = B, s_fadeA = A;
        PS1ChainSetHook(PS1FadeHook);
        return;
    }
    int level = PS1AlphaLevel(A == 0xFF ? 0xFF : A);
    if (level)
        PS1ChainAlphaRect(XPos, YPos, width, height, R, G, B, level);
}

void DrawTintRectangle(int XPos, int YPos, int width, int height) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }
void DrawScaledTintMask(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                        int sheetID)
{
    g_ps1DrawTodo = g_ps1DrawTodo + 1;
}

// ---------------------------------------------------------------------------------------------------
// Sprites

// A sprite wholly outside the 320 x 240 drawing area draws nothing: skipped before its atlas pieces are looked up
// (the special stage's background draws twelve 256 x 256 frames around the screen; docs/30 7.2b). Sonic 2's atlases
// have no on-demand pictures, so no lookup side effect is lost.
volatile uint32_t g_ps1SpriteCull = 1; // GDB: 0 = draw off-screen sprites too (the A/B of this cull)
static inline bool PS1SpriteOffScreen(int x, int y, int w, int h)
{
    return g_ps1SpriteCull && (x >= SCREEN_XSIZE || y >= SCREEN_YSIZE || x + w <= 0 || y + h <= 0);
}
static void PS1DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    if (width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden() || PS1SpriteOffScreen(XPos, YPos, width, height))
        return;
    // the cache: opaque, unmodulated, one CLUT set (no bank split), a frame of one resident piece
    const bool plain = s_ink == PS1_INK_OPAQUE && s_inkMod == 0x80 && !s_splitY && g_ps1SpriteCacheOn && PS1SurfaceSheet(sheetID) >= 0;
    PS1SpriteCacheEntry *e = nullptr;
    if (plain) {
        uint32_t h = (uint32_t)sheetID * 31 + (uint32_t)sprX * 7 + (uint32_t)sprY * 13 + (uint32_t)width * 3 + (uint32_t)height;
        e          = &s_spriteCache[(h ^ (h >> 6)) & 63];
        if (e->sheetID == sheetID && e->sprX == sprX && e->sprY == sprY && e->w == width && e->h == height) {
            psyqo::PrimPieces::TPageAttr attr;
            memcpy(&attr, &e->tpage, 2);
            PS1ChainTPage(attr);
            PS1ChainSpriteWords((uint32_t)(uint16_t)XPos | (uint32_t)(uint16_t)YPos << 16, e->tex,
                                (uint32_t)(uint16_t)width | (uint32_t)(uint16_t)height << 16);
            g_ps1DrawSprites    = g_ps1DrawSprites + 1;
            g_ps1SpriteCacheHits = g_ps1SpriteCacheHits + 1;
            return;
        }
    }
    int pieces = 0;
    const PS1AtlasCluster *one = nullptr;
    int16_t u = 0, v = 0;
    uint32_t misses = g_ps1AtlasDrawMiss;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        PS1ChainPieceSprite(p, XPos + p.ox, YPos + p.oy);
        if (!pieces++ && p.ox == 0 && p.oy == 0 && p.w == width && p.h == height)
            one = p.c, u = p.u, v = p.v;
    });
    if (e && pieces == 1 && one && !one->picture && g_ps1AtlasDrawMiss == misses
        && (int16_t)width == width && (int16_t)height == height && (int16_t)sprX == sprX && (int16_t)sprY == sprY) {
        psyqo::PrimPieces::TPageAttr attr = PS1ClusterTPage(one); // PS1ChainPieceSpriteRows' words for this piece
        psyqo::PrimPieces::TexInfo ti{ (uint8_t)u, (uint8_t)v, PS1ClusterClut(one, false) };
        static_assert(sizeof(ti) == 4 && sizeof(attr) == 2, "sprite words");
        memcpy(&e->tpage, &attr, 2);
        memcpy(&e->tex, &ti, 4);
        e->sheetID = (int16_t)sheetID, e->sprX = (int16_t)sprX, e->sprY = (int16_t)sprY, e->w = (int16_t)width, e->h = (int16_t)height;
        g_ps1SpriteCacheFills = g_ps1SpriteCacheFills + 1;
    }
}

// The engine's DrawSprite calls are hashed (g_ps1DrawHash, as phase 2's headless renderer did); the renderer's
// own uses (unrotated DrawSpriteRotated, ink passes) call PS1DrawSprite.
void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    uint32_t h = g_ps1DrawHash;
#if PS1_GAME == 1 // the same hash, unrolled (the loop kept the seven arguments in a stack array)
    h = h * 31 + (uint32_t)XPos;
    h = h * 31 + (uint32_t)YPos;
    h = h * 31 + (uint32_t)width;
    h = h * 31 + (uint32_t)height;
    h = h * 31 + (uint32_t)sprX;
    h = h * 31 + (uint32_t)sprY;
    h = h * 31 + (uint32_t)sheetID;
#else
    for (int a : { XPos, YPos, width, height, sprX, sprY, sheetID }) h = h * 31 + (uint32_t)a;
#endif
    g_ps1DrawHash  = h;
    g_ps1DrawCount = g_ps1DrawCount + 1;
    PS1DrawSprite(XPos, YPos, width, height, sprX, sprY, sheetID);
}

void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID)
{
    if (width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden() || PS1SpriteOffScreen(XPos, YPos, width, height))
        return;
    bool fx = direction & FLIP_X, fy = direction & FLIP_Y;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        int x = fx ? XPos + width - p.ox - p.w : XPos + p.ox;
        int y = fy ? YPos + height - p.oy - p.h : YPos + p.oy;
        PS1ChainPieceRect(p, x, y, p.w, p.h, fx, fy);
    });
}

// upstream (HW path): scale 0x200 = 1.0; the frame's top-left moves by pivot * scale.
void DrawSpriteScaled(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID)
{
    if (width <= 0 || height <= 0 || scaleX <= 0 || scaleY <= 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    int sx = scaleX << 2, sy = scaleY << 2; // 2048 = 1.0
    int x0 = XPos - (pivotX * sx >> 11), y0 = YPos - (pivotY * sy >> 11);
    int W = width * sx >> 11;
    bool fx = direction == FLIP_X;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        int a = p.ox * sx >> 11, b = (p.ox + p.w) * sx >> 11;
        int t = p.oy * sy >> 11, d = (p.oy + p.h) * sy >> 11;
        if (b <= a || d <= t)
            return;
        int x = fx ? x0 + W - b : x0 + a;
        PS1ChainPieceRectClipped(p, x, y0 + t, b - a, d - t, fx, false);
    });
}

void DrawScaledChar(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                    int sheetID)
{
    DrawSpriteScaled(direction, XPos, YPos, pivotX, pivotY, scaleX, scaleY, width, height, sprX, sprY, sheetID);
}

// upstream (HW path) corners: (XPos, YPos) + R * (x, y), x = +-(local x - pivotX), y = local y - pivotY,
// cos/sin from the 512-step tables (scaled by `scale` / 0x200 for rotozoom).
#if PS1_GAME == 1
// Sonic 1 (docs/37 phase 7): the special stage draws ~220 rotated sprites a frame from a handful of frames, all at the
// maze's one rotation. A frame's pieces with their rotated corner offsets, bounds and UV edges -- everything but the
// screen position, which the corners add after the shift as before -- are kept per (frame, pivot, rotation, scale,
// flip, sheet) in a small direct-mapped cache (dropped when the atlas or the surface list changes). Frames of more
// than 4 pieces, on-demand pictures or atlas misses (their lookups have side effects) are never cached. The quads,
// culling and GPU-limit clipping are the same as the uncached path, so the output is identical.
struct PS1AffinePiece {
    PS1Piece p;
    int dx[4], dy[4];
    int xmin, xmax, ymin, ymax;
    int u0, u1, v0, v1;
    uint32_t qw[9]; // the opaque quad's GPU words (PS1ChainPieceQuad's) with the corner positions left 0
};
struct PS1AffineEntry {
    int key[10];
    uint32_t gen, atlas;
    int n; // pieces; 0 = empty
    PS1AffinePiece pc[4];
};
static PS1AffineEntry s_affine[32];
volatile uint32_t g_ps1AffineHits = 0, g_ps1AffineMisses = 0, g_ps1AffineCulled = 0; // GDB
volatile uint8_t g_ps1AffineCache = 1; // initialised data: GDB sets 0 for the uncached A/B (same build)

static void PS1AffinePieceMake(PS1AffinePiece &a, const PS1Piece &p, bool fx, int pivotX, int pivotY, int cs, int sn, bool right)
{
    a.p              = p;
    PS1QuadCorners c = PS1AffineCorners(0, 0, pivotX, pivotY, sn, cs, fx, p.ox, p.oy, p.w, p.h); // ps1/sprite_quad.hh
    a.xmin = 0x7FFFFFFF, a.xmax = -0x7FFFFFFF, a.ymin = 0x7FFFFFFF, a.ymax = -0x7FFFFFFF;
    for (int k = 0; k < 4; ++k) {
        a.dx[k] = c.x[k];
        a.dy[k] = c.y[k];
        a.xmin = a.dx[k] < a.xmin ? a.dx[k] : a.xmin, a.xmax = a.dx[k] > a.xmax ? a.dx[k] : a.xmax;
        a.ymin = a.dy[k] < a.ymin ? a.dy[k] : a.ymin, a.ymax = a.dy[k] > a.ymax ? a.dy[k] : a.ymax;
    }
    PS1QuadUV uv = PS1AffineUV(c, p.u, p.v, p.w, p.h, right); // texel edges (0a3f95f)
    a.u0 = uv.u0, a.u1 = uv.u1, a.v0 = uv.v0, a.v1 = uv.v1;
    static const int16_t zero[4] = { 0, 0, 0, 0 };
    psyqo::Prim::TexturedQuad q = PS1PieceQuad(p, zero, zero, a.u0, a.u1, a.v0, a.v1, false);
    static_assert(sizeof(q) == sizeof(a.qw), "quad words");
    memcpy(a.qw, &q, sizeof(q));
}

static void PS1AffinePieceDraw(const PS1AffinePiece &a, int XPos, int YPos)
{
    int qx[4], qy[4];
    for (int k = 0; k < 4; ++k) {
        qx[k] = XPos + a.dx[k];
        qy[k] = YPos + a.dy[k];
    }
    int xmin = XPos + a.xmin, xmax = XPos + a.xmax, ymin = YPos + a.ymin, ymax = YPos + a.ymax;
    bool beyond = PS1BeyondGpuLimits(xmin, ymin, xmax, ymax);
    if (xmax <= 0 || ymax <= 0 || xmin >= SCREEN_XSIZE || ymin >= SCREEN_YSIZE) {
        if (beyond)
            g_ps1WrapCulled = g_ps1WrapCulled + 1;
        if (g_ps1LimitFix)
            return; // wholly off screen
    }
    if (beyond && g_ps1LimitFix) { // zoomed past the GPU limits: cut to the screen
        PS1ChainPieceQuadClipped(a.p, qx, qy, a.u0, a.u1, a.v0, a.v1);
        return;
    }
    if (s_ink == PS1_INK_OPAQUE && s_inkMod == 0x80 && !s_splitY) { // PS1ChainPieceQuad's words, positions filled in
        if (uint32_t *w = PS1ChainRawBegin(9)) {
            memcpy(w, a.qw, sizeof(a.qw));
            for (int k = 0; k < 4; ++k) w[1 + 2 * k] = (uint32_t)(uint16_t)qx[k] | (uint32_t)(uint16_t)qy[k] << 16;
            PS1ChainRawEnd();
            g_ps1DrawQuads = g_ps1DrawQuads + 1;
        }
        return;
    }
    int16_t xs[4] = { (int16_t)qx[0], (int16_t)qx[1], (int16_t)qx[2], (int16_t)qx[3] };
    int16_t ys[4] = { (int16_t)qy[0], (int16_t)qy[1], (int16_t)qy[2], (int16_t)qy[3] };
    PS1ChainPieceQuad(a.p, xs, ys, a.u0, a.u1, a.v0, a.v1);
}

__attribute__((always_inline)) static inline void PS1DrawSpriteAffine(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                                int scale, int sheetID)
{
    if (width <= 0 || height <= 0 || scale == 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    rotation -= rotation >> 9 << 9;
    if (rotation < 0)
        rotation += 0x200;
    if (rotation != 0)
        rotation = 0x200 - rotation;
    // Wholly off screen: every piece corner is (dx, dy) from (XPos, YPos) with |dx|, |dy| <= R (|cos|, |sin| <= |scale|,
    // the piece corners inside the frame, the shift's rounding), so when that square is off screen and within the GPU
    // limits every piece below would be culled without counting anything: skip the key, the cache and the lookups
    // (Sonic 1 has no on-demand pictures, so no lookup side effect is lost; PS1DrawSprite's cull, above).
    if (g_ps1LimitFix) {
        int ax = pivotX < 0 ? -pivotX : pivotX, bx = width - pivotX, ay = pivotY < 0 ? -pivotY : pivotY, by = height - pivotY;
        bx = bx < 0 ? -bx : bx, by = by < 0 ? -by : by;
        int R = (((ax > bx ? ax : bx) + (ay > by ? ay : by)) * (scale < 0 ? -scale : scale) >> 9) + 1;
        if ((XPos + R <= 0 || YPos + R <= 0 || XPos - R >= SCREEN_XSIZE || YPos - R >= SCREEN_YSIZE)
            && !PS1BeyondGpuLimits(XPos - R, YPos - R, XPos + R, YPos + R)) {
            g_ps1AffineCulled = g_ps1AffineCulled + 1;
            return;
        }
    }
    const int key[10] = { direction, pivotX, pivotY, sprX, sprY, width, height, rotation, scale, sheetID };
    uint32_t h = 0;
    for (int k = 0; k < 10; ++k) h = h * 31 + (uint32_t)key[k];
    PS1AffineEntry &e = s_affine[(h ^ (h >> 5) ^ (h >> 10)) & 31];
    bool same = e.n && g_ps1AffineCache && e.gen == g_ps1SurfaceGen && e.atlas == g_ps1AtlasLoads;
    for (int k = 0; k < 10 && same; ++k) same = e.key[k] == key[k]; // not memcmp: the libc one compares bytes (~450 instructions)
    if (same) {
        g_ps1AffineHits = g_ps1AffineHits + 1;
        for (int i = 0; i < e.n; ++i) PS1AffinePieceDraw(e.pc[i], XPos, YPos);
        return;
    }
    g_ps1AffineMisses = g_ps1AffineMisses + 1;
    int sn = sin512LookupTable[rotation] * scale >> 9;
    int cs = cos512LookupTable[rotation] * scale >> 9;
    bool fx = direction == FLIP_X; // upstream (software path) mirrors only for FLIP_X (V4-103)
    // Texel edges: see the Sonic 2 version below (PS1ChainPieceRect's rule at right angles and scale 1).
    const bool right  = scale == 0x200 && !(rotation & 0x7F);
    uint32_t misses   = g_ps1AtlasDrawMiss;
    int n             = 0;
    bool cacheable    = true;
    PS1AffinePiece tmp;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        PS1AffinePieceMake(tmp, p, fx, pivotX, pivotY, cs, sn, right);
        PS1AffinePieceDraw(tmp, XPos, YPos);
        if (n < 4 && !p.c->picture)
            e.pc[n] = tmp;
        else
            cacheable = false;
        ++n;
    });
    if (cacheable && n && g_ps1AtlasDrawMiss == misses) {
        memcpy(e.key, key, sizeof(key));
        e.gen   = g_ps1SurfaceGen;
        e.atlas = g_ps1AtlasLoads;
        e.n     = n;
    }
    else
        e.n = 0;
}
#else
volatile uint32_t g_ps1AffineCulled = 0; // GDB: rotated / zoomed sprites culled whole (off screen)
static void PS1DrawSpriteAffine(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                                int scale, int sheetID)
{
    if (width <= 0 || height <= 0 || scale == 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    rotation -= rotation >> 9 << 9;
    if (rotation < 0)
        rotation += 0x200;
    if (rotation != 0)
        rotation = 0x200 - rotation;
    // Wholly off screen (as Sonic 1's version above, docs/43: the special stage's message letters fly off at ~10x): each
    // corner PS1AffineCorners makes is within R + 2 of (XPos, YPos) (|sn|, |cs| <= |scale|; the shifts' floors, and the
    // fourth corner q1 + q2 - q0 adds up three of them). When that square is off screen every piece below is culled
    // (with g_ps1LimitFix), and Sonic 2's atlases have no on-demand pictures (PS1DrawSprite's cull): nothing drawn is lost
    // by not looking the pieces up. (The diagnostic g_ps1WrapCulled then counts only the pieces of sprites on screen.)
    if (g_ps1LimitFix) {
        int ax = pivotX < 0 ? -pivotX : pivotX, bx = width - pivotX, ay = pivotY < 0 ? -pivotY : pivotY, by = height - pivotY;
        bx = bx < 0 ? -bx : bx, by = by < 0 ? -by : by;
        const int R = (((ax > bx ? ax : bx) + (ay > by ? ay : by)) * (scale < 0 ? -scale : scale) >> 9) + 3;
        if (XPos + R <= 0 || YPos + R <= 0 || XPos - R >= SCREEN_XSIZE || YPos - R >= SCREEN_YSIZE) {
            g_ps1AffineCulled = g_ps1AffineCulled + 1;
            return;
        }
    }
    int sn = sin512LookupTable[rotation] * scale >> 9;
    int cs = cos512LookupTable[rotation] * scale >> 9;
    bool fx = direction == FLIP_X; // upstream (software path) mirrors only for FLIP_X (V4-103)
    // Texel edges (PS1ChainPieceRect): at right angles and scale 1 (Sonic's static-frame rotations 0 / 90 / 180 / 270,
    // mirrored when he faces left) the quad is axis-aligned and exact when a texture axis running left, or up, on
    // screen has its edges pulled back one texel; before, a mirrored frame read the atlas's next column along its left
    // edge (a dark line). Other angles / scales can't be exact with integer UVs: the far edges come one texel in, so
    // no pixel reads a neighbouring piece.
    const bool right = scale == 0x200 && !(rotation & 0x7F);
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        PS1QuadCorners q = PS1AffineCorners(XPos, YPos, pivotX, pivotY, sn, cs, fx, p.ox, p.oy, p.w, p.h); // ps1/sprite_quad.hh
        const int *qx = q.x, *qy = q.y;
        int xmin = 0x7FFFFFFF, xmax = -0x7FFFFFFF, ymin = 0x7FFFFFFF, ymax = -0x7FFFFFFF;
        for (int k = 0; k < 4; ++k) {
            xmin = qx[k] < xmin ? qx[k] : xmin, xmax = qx[k] > xmax ? qx[k] : xmax;
            ymin = qy[k] < ymin ? qy[k] : ymin, ymax = qy[k] > ymax ? qy[k] : ymax;
        }
        bool beyond = PS1BeyondGpuLimits(xmin, ymin, xmax, ymax);
        if (xmax <= 0 || ymax <= 0 || xmin >= SCREEN_XSIZE || ymin >= SCREEN_YSIZE) {
            if (beyond)
                g_ps1WrapCulled = g_ps1WrapCulled + 1;
            if (g_ps1LimitFix)
                return; // wholly off screen
        }
        PS1QuadUV uv = PS1AffineUV(q, p.u, p.v, p.w, p.h, right); // texel edges (0a3f95f)
        int u0 = uv.u0, u1 = uv.u1, v0 = uv.v0, v1 = uv.v1;
        if (beyond && g_ps1LimitFix) { // zoomed past the GPU limits: cut to the screen
            PS1ChainPieceQuadClipped(p, qx, qy, u0, u1, v0, v1);
            return;
        }
        int16_t xs[4] = { (int16_t)qx[0], (int16_t)qx[1], (int16_t)qx[2], (int16_t)qx[3] };
        int16_t ys[4] = { (int16_t)qy[0], (int16_t)qy[1], (int16_t)qy[2], (int16_t)qy[3] };
        PS1ChainPieceQuad(p, xs, ys, u0, u1, v0, v1);
    });
}

#endif

void DrawSpriteRotated(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID)
{
    if (!(rotation & 0x1FF) && direction != FLIP_X) { // no rotation, not mirrored (upstream mirrors only FLIP_X): 1:1 sprite
        PS1DrawSprite(XPos - pivotX, YPos - pivotY, width, height, sprX, sprY, sheetID); // callers pass -frame pivot
        return;
    }
    PS1DrawSpriteAffine(direction, XPos, YPos, pivotX, pivotY, sprX, sprY, width, height, rotation, 0x200, sheetID);
}

void DrawSpriteRotozoom(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation, int scale,
                        int sheetID)
{
    PS1DrawSpriteAffine(direction, XPos, YPos, pivotX, pivotY, sprX, sprY, width, height, rotation, scale, sheetID);
}

// Ink effects (user decision 2026-09-24): INK_BLEND = 50 % (exact), INK_ALPHA = 4 levels, INK_ADD /
// INK_SUB = GPU add / subtract with the texels scaled by alpha (exact up to 5-bit colour).
static void PS1DrawInkSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID, uint8_t ink, uint8_t mod, int passes)
{
    s_ink    = ink;
    s_inkMod = mod;
    for (int i = 0; i < passes; ++i) PS1DrawSprite(XPos, YPos, width, height, sprX, sprY, sheetID);
    s_ink    = PS1_INK_OPAQUE;
    s_inkMod = 0x80;
}

void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    PS1DrawInkSprite(XPos, YPos, width, height, sprX, sprY, sheetID, PS1_INK_HALF, 0x80, 1);
}

void DrawAlphaBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    int level = PS1AlphaLevel(alpha > 0xFF ? 0xFF : alpha);
    if (level == 3)
        PS1DrawSprite(XPos, YPos, width, height, sprX, sprY, sheetID);
    else if (level)
        PS1DrawInkSprite(XPos, YPos, width, height, sprX, sprY, sheetID, PS1_INK_HALF, 0x80, level);
}

void DrawAdditiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
    if (alpha > 0)
        PS1DrawInkSprite(XPos, YPos, width, height, sprX, sprY, sheetID, PS1_INK_ADD, (uint8_t)((alpha + 1) >> 1), 1);
}

void DrawSubtractiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
    if (alpha > 0)
        PS1DrawInkSprite(XPos, YPos, width, height, sprX, sprY, sheetID, PS1_INK_SUB, (uint8_t)((alpha + 1) >> 1), 1);
}

// upstream DrawObjectAnimation (v4), verbatim: picks the draw call from the animation's rotation style.
void DrawObjectAnimation(void *objScr, void *ent, int XPos, int YPos)
{
    ObjectScript *objectScript = (ObjectScript *)objScr;
    Entity *entity             = (Entity *)ent;
    SpriteAnimation *sprAnim   = &animationList[objectScript->animFile->aniListOffset + entity->animation];
    SpriteFrame *frame         = &animFrames[sprAnim->frameListOffset + entity->frame];
    int rotation               = 0;

    switch (sprAnim->rotationStyle) {
        case ROTSTYLE_NONE:
            switch (entity->direction) {
                case FLIP_NONE:
                    DrawSpriteFlipped(frame->pivotX + XPos, frame->pivotY + YPos, frame->width, frame->height, frame->sprX, frame->sprY, FLIP_NONE,
                                      frame->sheetID);
                    break;

                case FLIP_X:
                    DrawSpriteFlipped(XPos - frame->width - frame->pivotX, frame->pivotY + YPos, frame->width, frame->height, frame->sprX,
                                      frame->sprY, FLIP_X, frame->sheetID);
                    break;
                case FLIP_Y:

                    DrawSpriteFlipped(frame->pivotX + XPos, YPos - frame->height - frame->pivotY, frame->width, frame->height, frame->sprX,
                                      frame->sprY, FLIP_Y, frame->sheetID);
                    break;

                case FLIP_XY:
                    DrawSpriteFlipped(XPos - frame->width - frame->pivotX, YPos - frame->height - frame->pivotY, frame->width, frame->height,
                                      frame->sprX, frame->sprY, FLIP_XY, frame->sheetID);
                    break;

                default: break;
            }
            break;

        case ROTSTYLE_FULL:
            DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width, frame->height,
                              entity->rotation, frame->sheetID);
            break;

        case ROTSTYLE_45DEG:
            if (entity->rotation >= 0x100)
                DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
                                  frame->height, 0x200 - ((0x214 - entity->rotation) >> 6 << 6), frame->sheetID);
            else
                DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
                                  frame->height, (entity->rotation + 20) >> 6 << 6, frame->sheetID);
            break;

        case ROTSTYLE_STATICFRAMES: {
            if (entity->rotation >= 0x100)
                rotation = 8 - ((532 - entity->rotation) >> 6);
            else
                rotation = (entity->rotation + 20) >> 6;
            int frameID = entity->frame;
            switch (rotation) {
                case 0: // 0 deg
                case 8: // 360 deg
                    rotation = 0x00;
                    break;

                case 1: // 45 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0;
                    else
                        rotation = 0x80;
                    break;

                case 2: // 90 deg
                    rotation = 0x80;
                    break;

                case 3: // 135 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0x80;
                    else
                        rotation = 0x100;
                    break;

                case 4: // 180 deg
                    rotation = 0x100;
                    break;

                case 5: // 225 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 0x100;
                    else
                        rotation = 384;
                    break;

                case 6: // 270 deg
                    rotation = 384;
                    break;

                case 7: // 315 deg
                    frameID += sprAnim->frameCount;
                    if (entity->direction)
                        rotation = 384;
                    else
                        rotation = 0;
                    break;

                default: break;
            }

            frame = &animFrames[sprAnim->frameListOffset + frameID];
            DrawSpriteRotated(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width, frame->height,
                              rotation, frame->sheetID);
            // DrawSpriteRotozoom(entity->direction, XPos, YPos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY, frame->width,
            // frame->height,
            //                  rotation, entity->scale, frame->sheetID);
            break;
        }

        default: break;
    }
}

// Scene3D faces (upstream Draw3DScene: title, menu, special stages), already projected to screen
// by the engine. Vertex order = upstream's HW quads (top-left, top-right, bottom-left, bottom-right) =
// the PS1 quad order. Phase 7 moves the transform to the GTE.
volatile uint32_t g_ps1Faces        = 0; // faces chained (flat + textured), total
volatile uint32_t g_ps1FaceTexMiss  = 0; // textured faces whose UVs span no single atlas cluster (skipped)

// The special stage's per-face / per-vertex functions are built -O2 (the rest of the file stays -Os: -O2 for all of it
// costs 7.3 KB of the 2 MB heap; docs/30 phase 7.2b).
#define PS1_HOT3D __attribute__((optimize("O2")))
// Upstream's software rasterizer fills the convex hull of a face's 4 corners, in any order (its scan edges join every
// pair); the GPU's quad is a triangle strip (0 1 2, 1 2 3). Strip order: the diagonal (1-2) is the corner pair whose
// line has the other two corners on opposite sides.
PS1_HOT3D static void PS1QuadStrip(const Vertex *in, Vertex *out)
{
    int cx[4], cy[4]; // clamped for the side test only (32-bit products; far corners keep their direction)
    for (int k = 0; k < 4; ++k) {
        cx[k] = in[k].x < -8191 ? -8191 : in[k].x > 8191 ? 8191 : in[k].x;
        cy[k] = in[k].y < -8191 ? -8191 : in[k].y > 8191 ? 8191 : in[k].y;
    }
    auto side = [&](int i, int j, int k) {
        int d = (cx[j] - cx[i]) * (cy[k] - cy[i]) - (cy[j] - cy[i]) * (cx[k] - cx[i]);
        return d > 0 ? 1 : d < 0 ? -1 : 0;
    };
    static const int pairs[3][4] = { { 1, 2, 0, 3 }, { 0, 3, 1, 2 }, { 0, 2, 1, 3 } }; // diagonal i j, others k l
    for (const int *p : pairs) {
        if (side(p[0], p[1], p[2]) * side(p[0], p[1], p[3]) < 0) {
            out[0] = in[p[2]], out[1] = in[p[0]], out[2] = in[p[1]], out[3] = in[p[3]];
            return;
        }
    }
    for (int k = 0; k < 4; ++k) out[k] = in[k]; // degenerate (collinear corners): as given
}

// Draw3DScene's per-vertex GTE pass and faded-face runs live in RSDKv4/Scene3D.cpp (built -O2 whole-file: the
// -Os file's optimize("O2") functions still synthesise constant multiplies with mult). They draw only while a frame
// is open and objects are shown:
bool PS1Draw3DOpen() { return s_frameOpen && !PS1ObjectsHidden(); }

void DrawFace(void *v, uint colour)
{
    if (!s_frameOpen || PS1ObjectsHidden())
        return;
    Vertex strip[4];
    PS1QuadStrip((const Vertex *)v, strip);
    const Vertex *q = strip;
    int alpha       = (int)((colour & 0x7F000000) >> 23); // upstream: 0-254, 0xFE = opaque
    int level       = alpha >= 0xFE ? 3 : PS1AlphaLevel(alpha);
    if (!level)
        return;
    uint32_t bgr = (colour & 0xFF) << 16 | (colour & 0xFF00) | ((colour >> 16) & 0xFF);
    uint32_t semi = 0;
    if (level < 3) { // semi-transparent: B/2 + F/2 (the texpage's mode), twice for 75 %
        psyqo::PrimPieces::TPageAttr attr;
        attr.set(psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront);
        PS1ChainTPage(attr);
        semi = 0x02000000;
    }
    int repeat = level == 2 ? 2 : 1;
    int x0 = q[0].x, x1 = q[0].x, y0 = q[0].y, y1 = q[0].y;
    for (int k = 1; k < 4; ++k) {
        x0 = q[k].x < x0 ? q[k].x : x0, x1 = q[k].x > x1 ? q[k].x : x1;
        y0 = q[k].y < y0 ? q[k].y : y0, y1 = q[k].y > y1 ? q[k].y : y1;
    }
    if (PS1BeyondGpuLimits(x0, y0, x1, y1)) { // the quad as a polygon (corners A B D C), cut to the screen
        g_ps1GpuClipped = g_ps1GpuClipped + 1;
        int px[4] = { q[0].x, q[1].x, q[3].x, q[2].x }, py[4] = { q[0].y, q[1].y, q[3].y, q[2].y };
        PS1ChainFlatPolygonClipped(px, py, 4, 0x20000000 | semi | bgr, repeat);
        g_ps1Faces = g_ps1Faces + 1;
        return;
    }
    uint32_t w[5];
    w[0] = 0x28000000 | semi | bgr; // GP0 28h flat quad, BGR
    for (int k = 0; k < 4; ++k) w[1 + k] = (uint32_t)(uint16_t)q[k].x | ((uint32_t)(uint16_t)q[k].y << 16);
    for (int i = 0; i < repeat; ++i) PS1ChainFlatRaw(w, 5);
    g_ps1Faces = g_ps1Faces + 1;
}

volatile int32_t g_ps1FaceMissSheet = -1, g_ps1FaceMissU0 = 0, g_ps1FaceMissV0 = 0, g_ps1FaceMissU1 = 0, g_ps1FaceMissV1 = 0;

// Textured quads (Draw3DScene's TEXTURED_* faces); blended: B/2 + F/2 on the texels (upstream DrawTexturedFaceBlended:
// (texel & 0xF7BC) >> 1 + (pixel & 0xF7BC) >> 1), the PS1's semi-transparency mode 0.
PS1_HOT3D static void PS1TexturedFace(const Vertex *q, byte sheetID, bool blended)
{
    if (!s_frameOpen || PS1ObjectsHidden())
        return;
    int sheet = PS1SurfaceSheet(sheetID);
    if (sheet < 0) {
        g_ps1SheetUnbound = g_ps1SheetUnbound + 1;
        return;
    }
    int u0 = q[0].u, v0 = q[0].v, u1 = u0, v1 = v0;
    for (int k = 1; k < 4; ++k) {
        u0 = q[k].u < u0 ? q[k].u : u0, u1 = q[k].u > u1 ? q[k].u : u1;
        v0 = q[k].v < v0 ? q[k].v : v0, v1 = q[k].v > v1 ? q[k].v : v1;
    }
    {
        int x0 = q[0].x, x1 = q[0].x, y0 = q[0].y, y1 = q[0].y;
        for (int k = 1; k < 4; ++k) {
            x0 = q[k].x < x0 ? q[k].x : x0, x1 = q[k].x > x1 ? q[k].x : x1;
            y0 = q[k].y < y0 ? q[k].y : y0, y1 = q[k].y > y1 ? q[k].y : y1;
        }
        if (PS1BeyondGpuLimits(x0, y0, x1, y1))
            g_ps1GpuLimit = g_ps1GpuLimit + 1;
    }
    const PS1AtlasCluster *c = PS1FindCluster(sheet, u0, v0);
    // UV edges may reach the cluster's far edge (u1 == srcX + w): texel u1 itself is never sampled.
    if (!c || u1 > c->srcX + c->w || v1 > c->srcY + c->h) {
        g_ps1FaceTexMiss   = g_ps1FaceTexMiss + 1;
        g_ps1FaceMissSheet = sheet;
        g_ps1FaceMissU0 = u0, g_ps1FaceMissV0 = v0, g_ps1FaceMissU1 = u1, g_ps1FaceMissV1 = v1;
        return;
    }
    // the cluster's texpage / CLUT words, cached for the last (non-picture) cluster: the special stage's rings and
    // shadows share one; __builtin_memcpy is inlined even with -fno-builtin (the library call cost more than the face)
    if (c != s_faceCluster) {
        psyqo::PrimPieces::TPageAttr tp = PS1ClusterTPage(c);
        __builtin_memcpy(&s_faceTPage, &tp, 2);
        psyqo::PrimPieces::ClutIndex ci = PS1ClusterClut(c);
        __builtin_memcpy(&s_faceClut, &ci, 2);
        s_faceCluster = c->picture ? nullptr : c; // picture clusters move between VRAM slots: never cached
    }
    uint16_t tpage = s_faceTPage, clut = s_faceClut;
    auto uv = [&](int k) {
        int u = c->u + q[k].u - c->srcX, vv = c->v + q[k].v - c->srcY;
        return (uint32_t)(uint8_t)(u > 255 ? 255 : u) | ((uint32_t)(uint8_t)(vv > 255 ? 255 : vv) << 8);
    };
    if (blended) {
        psyqo::PrimPieces::TPageAttr attr;
        __builtin_memcpy(&attr, &tpage, 2);
        attr.set(psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront);
        __builtin_memcpy(&tpage, &attr, 2);
    }
    uint32_t w[9];
    w[0] = (blended ? 0x2E808080u : 0x2C808080u); // GP0 2Ch / 2Eh textured quad (semi-transparent), colour 0x80
    for (int k = 0; k < 4; ++k) w[1 + 2 * k] = (uint32_t)(uint16_t)q[k].x | ((uint32_t)(uint16_t)q[k].y << 16);
    w[2] = uv(0) | ((uint32_t)clut << 16);
    w[4] = uv(1) | ((uint32_t)tpage << 16);
    w[6] = uv(2);
    w[8] = uv(3);
    PS1ChainPrimsRawN(w, 9, 1);
    g_ps1Faces = g_ps1Faces + 1;
}
// Draw3DScene's TEXTURED_C / _BLEND faces (the special stage's rings and shadows: ~45 a frame) are axis-aligned
// rectangles: corners (x0, y0) (x1, y0) (x0, y1) (x1, y1) with texels (u0, v0) (u1, v0) (u0, v1) (u1, v1). The same
// words as PS1TexturedFace for that quad, without its per-corner loops (docs/30 phase 7.2b).
// PS1TexturedRectWords: the same quad's 9 words into w (false: nothing to draw), for Sonic 2's ordering table, which
// writes them straight into its arena (RSDKv4/Scene3D.cpp PS1Draw3DSceneOT, docs/43 C1).
PS1_HOT3D bool PS1TexturedRectWords(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, byte sheetID, bool blended, uint32_t *w)
{
    if (!s_frameOpen || PS1ObjectsHidden())
        return false;
    int sheet = PS1SurfaceSheet(sheetID);
    if (sheet < 0) {
        g_ps1SheetUnbound = g_ps1SheetUnbound + 1;
        return false;
    }
    int umin = u0 < u1 ? u0 : u1, umax = u0 < u1 ? u1 : u0, vmin = v0 < v1 ? v0 : v1, vmax = v0 < v1 ? v1 : v0;
    int xmin = x0 < x1 ? x0 : x1, xmax = x0 < x1 ? x1 : x0, ymin = y0 < y1 ? y0 : y1, ymax = y0 < y1 ? y1 : y0;
    bool beyond = PS1BeyondGpuLimits(xmin, ymin, xmax, ymax);
    if (xmax <= 0 || ymax <= 0 || xmin >= SCREEN_XSIZE || ymin >= SCREEN_YSIZE || xmin == xmax || ymin == ymax) {
        if (beyond)
            g_ps1WrapCulled = g_ps1WrapCulled + 1;
        if (g_ps1LimitFix)
            return false; // nothing on screen
    }
    // A billboard passing the camera (the special stage's checkpoint gate balls, rings) projects far beyond the GPU
    // limits (psx-spx GPU "Vertex": 11-bit coordinates wrap, a polygon over 1023 x 511 is not drawn): it flickered.
    // Cut to the screen with the UVs moved in proportion, as the Sonic CD port's UFO fix (PS1ChainPieceRectClipped).
    if (beyond && g_ps1LimitFix) {
        g_ps1GpuClipped = g_ps1GpuClipped + 1;
        g_ps1ClipBillboards = g_ps1ClipBillboards + 1;
        auto cut = [](int &a0, int &a1, int &t0, int &t1, int lim) {
            int n0 = a0 < 0 ? 0 : a0 > lim ? lim : a0, n1 = a1 < 0 ? 0 : a1 > lim ? lim : a1;
            int64_t span = a1 - a0, dt = t1 - t0;
            int m0 = t0 + (int)(dt * (n0 - a0) / span), m1 = t0 + (int)(dt * (n1 - a0) / span);
            a0 = n0, a1 = n1, t0 = m0, t1 = m1;
        };
        cut(x0, x1, u0, u1, SCREEN_XSIZE);
        cut(y0, y1, v0, v1, SCREEN_YSIZE);
    }
    const PS1AtlasCluster *c = PS1FindCluster(sheet, umin, vmin);
    if (!c || umax > c->srcX + c->w || vmax > c->srcY + c->h) {
        g_ps1FaceTexMiss   = g_ps1FaceTexMiss + 1;
        g_ps1FaceMissSheet = sheet;
        g_ps1FaceMissU0 = umin, g_ps1FaceMissV0 = vmin, g_ps1FaceMissU1 = umax, g_ps1FaceMissV1 = vmax;
        return false;
    }
    if (c != s_faceCluster) {
        psyqo::PrimPieces::TPageAttr tp = PS1ClusterTPage(c);
        __builtin_memcpy(&s_faceTPage, &tp, 2);
        psyqo::PrimPieces::ClutIndex ci = PS1ClusterClut(c);
        __builtin_memcpy(&s_faceClut, &ci, 2);
        s_faceCluster = c->picture ? nullptr : c;
    }
    uint16_t tpage = s_faceTPage;
    if (blended) {
        psyqo::PrimPieces::TPageAttr attr;
        __builtin_memcpy(&attr, &tpage, 2);
        attr.set(psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront);
        __builtin_memcpy(&tpage, &attr, 2);
    }
    const int du = c->u - c->srcX, dv = c->v - c->srcY;
    auto tu = [&](int u) { return (uint32_t)(uint8_t)(u + du > 255 ? 255 : u + du); };
    auto tv = [&](int v) { return (uint32_t)(uint8_t)(v + dv > 255 ? 255 : v + dv) << 8; };
    auto xy = [](int x, int y) { return (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16); };
    w[0] = blended ? 0x2E808080u : 0x2C808080u;
    w[1] = xy(x0, y0);
    w[2] = tu(u0) | tv(v0) | ((uint32_t)s_faceClut << 16);
    w[3] = xy(x1, y0);
    w[4] = tu(u1) | tv(v0) | ((uint32_t)tpage << 16);
    w[5] = xy(x0, y1);
    w[6] = tu(u0) | tv(v1);
    w[7] = xy(x1, y1);
    w[8] = tu(u1) | tv(v1);
    return true;
}
#if PS1_GAME != 1 // Sonic 1 has no 3D stage (its EXE stays as it was)
// PS1TexturedRectWords for the special stage's ring / bomb / shadow quads (docs/43 C1: 45-70 a frame): the texture part
// of the words (command, CLUT / texpage, UVs) depends only on the sprite's UV box, the sheet and the blend, so it is
// kept for the frame in a small cache; the position tests and words are the full function's. A quad beyond the GPU
// limits (its UVs are cut with it) or a picture cluster takes the full function.
struct PS1TexWordCache {
    uint32_t surf, atlas, cluts, key; // the atlas state it was made in; key: sheetID | blended << 8 | 1 << 16 (0: empty)
    int u0, v0, u1, v1;
    uint32_t w0, a, b, c, d;
};
#define PS1_TEXWORD_N 64 // the rings' and bombs' animation frames and the shadows: 8 entries thrashed (docs/43)
static PS1TexWordCache s_texWordCache[PS1_TEXWORD_N];
extern volatile uint32_t g_ps1PerfFrames;
// The words depend only on the UV box, the sheet, the blend and the atlas state they were made in (the sheet binding
// g_ps1SurfaceGen, the atlas load, its CLUT count; picture clusters are never cached): entries stay valid across frames
// (docs/43: keyed on the frame, every box missed once a frame and took the full path)
__attribute__((always_inline)) static inline bool PS1TexWordCurrent(const PS1TexWordCache &e)
{
    return e.surf == g_ps1SurfaceGen && e.atlas == g_ps1AtlasLoads && e.cluts == (uint32_t)g_ps1AtlasClutCount;
}
__attribute__((optimize("Os"))) bool PS1TexturedRectFast(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, byte sheetID, bool blended, uint32_t *w)
{
    if (!s_frameOpen || PS1ObjectsHidden())
        return false;
    int xmin = x0 < x1 ? x0 : x1, xmax = x0 < x1 ? x1 : x0, ymin = y0 < y1 ? y0 : y1, ymax = y0 < y1 ? y1 : y0;
    bool beyond = PS1BeyondGpuLimits(xmin, ymin, xmax, ymax);
    if (beyond || xmax <= 0 || ymax <= 0 || xmin >= SCREEN_XSIZE || ymin >= SCREEN_YSIZE || xmin == xmax || ymin == ymax)
        return PS1TexturedRectWords(x0, y0, x1, y1, u0, v0, u1, v1, sheetID, blended, w); // its counters and clipping
    const uint32_t key = (uint32_t)sheetID | (uint32_t)blended << 8 | 1u << 16;
    PS1TexWordCache &e = s_texWordCache[((uint32_t)u0 * 7 + (uint32_t)v0 * 13 + (uint32_t)u1 * 3 + (uint32_t)v1 + key) & (PS1_TEXWORD_N - 1)];
    if (e.key != key || e.u0 != u0 || e.v0 != v0 || e.u1 != u1 || e.v1 != v1 || !PS1TexWordCurrent(e)) {
        uint32_t t[9];
        if (!PS1TexturedRectWords(x0, y0, x1, y1, u0, v0, u1, v1, sheetID, blended, t))
            return false;
        int umin = u0 < u1 ? u0 : u1, vmin = v0 < v1 ? v0 : v1;
        const PS1AtlasCluster *c = PS1FindCluster(PS1SurfaceSheet(sheetID), umin, vmin); // the one the words came from
        for (int k = 0; k < 9; ++k) w[k] = t[k];
        if (c && !c->picture) {
            e.surf = g_ps1SurfaceGen, e.atlas = g_ps1AtlasLoads, e.cluts = (uint32_t)g_ps1AtlasClutCount;
            e.key = key, e.u0 = u0, e.v0 = v0, e.u1 = u1, e.v1 = v1;
            e.w0 = t[0], e.a = t[2], e.b = t[4], e.c = t[6], e.d = t[8];
        }
        return true;
    }
    auto xy = [](int x, int y) { return (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16); };
    w[0] = e.w0;
    w[1] = xy(x0, y0);
    w[2] = e.a;
    w[3] = xy(x1, y0);
    w[4] = e.b;
    w[5] = xy(x0, y1);
    w[6] = e.c;
    w[7] = xy(x1, y1);
    w[8] = e.d;
    return true;
}
// PS1TexturedRectFast + PS1ChainOTRaw(9) for the ordering table's textured quads (docs/43): on a cache hit the words go
// straight into the arena (no copy); the same tests, cache and words. Null: nothing drawn, or no room.
uint32_t *PS1TexturedRectOT(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, byte sheetID, bool blended)
{
    if (!s_frameOpen || PS1ObjectsHidden())
        return nullptr;
    const int xmin = x0 < x1 ? x0 : x1, xmax = x0 < x1 ? x1 : x0, ymin = y0 < y1 ? y0 : y1, ymax = y0 < y1 ? y1 : y0;
    const uint32_t key = (uint32_t)sheetID | (uint32_t)blended << 8 | 1u << 16;
    PS1TexWordCache *e = nullptr;
    if (!PS1BeyondGpuLimits(xmin, ymin, xmax, ymax) && xmax > 0 && ymax > 0 && xmin < SCREEN_XSIZE && ymin < SCREEN_YSIZE
        && xmin != xmax && ymin != ymax) {
        e = &s_texWordCache[((uint32_t)u0 * 7 + (uint32_t)v0 * 13 + (uint32_t)u1 * 3 + (uint32_t)v1 + key) & (PS1_TEXWORD_N - 1)];
        if (e->key != key || e->u0 != u0 || e->v0 != v0 || e->u1 != u1 || e->v1 != v1 || !PS1TexWordCurrent(*e))
            e = nullptr;
        else {
            uint32_t *h = PS1ChainOTRaw(9);
            if (!h)
                return nullptr;
            auto xy = [](int x, int y) { return (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16); };
            h[1] = e->w0, h[2] = xy(x0, y0), h[3] = e->a, h[4] = xy(x1, y0), h[5] = e->b;
            h[6] = xy(x0, y1), h[7] = e->c, h[8] = xy(x1, y1), h[9] = e->d;
            return h;
        }
    }
    uint32_t w[9]; // a miss, beyond the limits, off screen: PS1TexturedRectFast as it is
    if (!PS1TexturedRectFast(x0, y0, x1, y1, u0, v0, u1, v1, sheetID, blended, w))
        return nullptr;
    uint32_t *h = PS1ChainOTRaw(9);
    if (!h)
        return nullptr;
    for (int k = 0; k < 9; ++k) h[1 + k] = w[k];
    return h;
}
#endif
PS1_HOT3D void PS1TexturedRect(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, byte sheetID, bool blended)
{
    uint32_t w[9];
    if (!PS1TexturedRectWords(x0, y0, x1, y1, u0, v0, u1, v1, sheetID, blended, w))
        return;
    PS1ChainPrimsRawN(w, 9, 1);
    g_ps1Faces = g_ps1Faces + 1;
}
void DrawTexturedFace(void *v, byte sheetID) { PS1TexturedFace((const Vertex *)v, sheetID, false); }
void DrawTexturedFaceBlended(void *v, byte sheetID) { PS1TexturedFace((const Vertex *)v, sheetID, true); }

// Upstream DrawFadedFace (the special stage's half-pipe): an opaque flat quad of the colour faded toward fogColor, per
// 5-bit channel fog * (255 - alpha) >> 8 + colour * alpha >> 8 (its blendLookupTable), then DrawFace's flat quad.
void DrawFadedFace(void *v, uint color, uint fogColor, int alpha)
{
    const Vertex *q = (const Vertex *)v;
    if (alpha > 0xFF)
        alpha = 0xFF;
    if (alpha < 1)
        return;
    if ((q[0].x < 0 && q[1].x < 0 && q[2].x < 0 && q[3].x < 0) || (q[0].x > SCREEN_XSIZE && q[1].x > SCREEN_XSIZE && q[2].x > SCREEN_XSIZE && q[3].x > SCREEN_XSIZE)
        || (q[0].y < 0 && q[1].y < 0 && q[2].y < 0 && q[3].y < 0) || (q[0].y > SCREEN_YSIZE && q[1].y > SCREEN_YSIZE && q[2].y > SCREEN_YSIZE && q[3].y > SCREEN_YSIZE))
        return;
    if ((q[0].x == q[1].x && q[1].x == q[2].x && q[2].x == q[3].x) || (q[0].y == q[1].y && q[1].y == q[2].y && q[2].y == q[3].y))
        return;
    auto ch = [&](int shift) {
        int c5 = ((color >> shift) & 0xFF) >> 3, f5 = ((fogColor >> shift) & 0xFF) >> 3;
        return (uint32_t)((f5 * (0xFF - alpha) >> 8) + (c5 * alpha >> 8)) << 3;
    };
    DrawFace(v, 0x7F000000u | ch(16) << 16 | ch(8) << 8 | ch(0)); // alpha 0xFE: opaque
}

#if PS1_GAME != 1 // Sonic 1 has no 3D stage (its EXE stays as it was)
// DrawFadedFace + DrawFace as words for the ordering table (docs/43 C1: the special stage's faded faces with a corner left
// to the CPU, ~30-200 a frame, no longer captured through the general path): the same early-outs, strip order, colour
// and limits test. 0: nothing drawn; 1: w[0..4] = the opaque flat quad; 2: beyond the GPU limits (the caller draws it
// through DrawFadedFace: the clipped polygon).
__attribute__((optimize("Os"))) int PS1FadedFaceWords(const Vertex *q, uint color, uint fogColor, int alpha, uint32_t *w)
{
    if (alpha > 0xFF)
        alpha = 0xFF;
    if (alpha < 1)
        return 0;
    if ((q[0].x < 0 && q[1].x < 0 && q[2].x < 0 && q[3].x < 0) || (q[0].x > SCREEN_XSIZE && q[1].x > SCREEN_XSIZE && q[2].x > SCREEN_XSIZE && q[3].x > SCREEN_XSIZE)
        || (q[0].y < 0 && q[1].y < 0 && q[2].y < 0 && q[3].y < 0) || (q[0].y > SCREEN_YSIZE && q[1].y > SCREEN_YSIZE && q[2].y > SCREEN_YSIZE && q[3].y > SCREEN_YSIZE))
        return 0;
    if ((q[0].x == q[1].x && q[1].x == q[2].x && q[2].x == q[3].x) || (q[0].y == q[1].y && q[1].y == q[2].y && q[2].y == q[3].y))
        return 0;
    if (!s_frameOpen || PS1ObjectsHidden()) // DrawFace's
        return 0;
    auto ch = [&](int shift) {
        int c5 = ((color >> shift) & 0xFF) >> 3, f5 = ((fogColor >> shift) & 0xFF) >> 3;
        return (uint32_t)((f5 * (0xFF - alpha) >> 8) + (c5 * alpha >> 8)) << 3;
    };
    Vertex strip[4];
    PS1QuadStrip(q, strip);
    int x0 = strip[0].x, x1 = strip[0].x, y0 = strip[0].y, y1 = strip[0].y;
    for (int k = 1; k < 4; ++k) {
        x0 = strip[k].x < x0 ? strip[k].x : x0, x1 = strip[k].x > x1 ? strip[k].x : x1;
        y0 = strip[k].y < y0 ? strip[k].y : y0, y1 = strip[k].y > y1 ? strip[k].y : y1;
    }
    if (PS1BeyondGpuLimits(x0, y0, x1, y1))
        return 2;
    // DrawFace's colour word from DrawFadedFace's 0x7F000000 | R << 16 | G << 8 | B: BGR, opaque
    w[0] = 0x28000000u | ch(0) << 16 | ch(8) << 8 | ch(16);
    for (int k = 0; k < 4; ++k) w[1 + k] = (uint32_t)(uint16_t)strip[k].x | ((uint32_t)(uint16_t)strip[k].y << 16);
    return 1;
}
#endif

// Text menus (the scripts' DrawMenu: the level select and its results screens): upstream's code as is. Each character is
// an 8 x 8 DrawSprite of the object's sheet (LevelSelect/Text.gif: the ASCII grid, the highlighted copy 128 lines
// below), which the atlas holds whole (tools/atlas/runtime_frames.py, 'Menu Control').
void DrawTextMenuEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        DrawSprite(XPos + (i << 3) - (((tMenu->entrySize[rowID] % 2) & (tMenu->alignment == 2)) * 4), YPos, 8, 8, ((tMenu->textData[id] & 0xF) << 3),
                   ((tMenu->textData[id] >> 4) << 3) + textHighlight, textMenuSurfaceNo);
        id++;
    }
}
void DrawStageTextEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        if (i == tMenu->entrySize[rowID] - 1) {
            DrawSprite(XPos + (i << 3), YPos, 8, 8, ((tMenu->textData[id] & 0xF) << 3), ((tMenu->textData[id] >> 4) << 3), textMenuSurfaceNo);
        }
        else {
            DrawSprite(XPos + (i << 3), YPos, 8, 8, ((tMenu->textData[id] & 0xF) << 3), ((tMenu->textData[id] >> 4) << 3) + textHighlight,
                       textMenuSurfaceNo);
        }
        id++;
    }
}
void DrawBlendedTextMenuEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        DrawBlendedSprite(XPos + (i << 3), YPos, 8, 8, ((tMenu->textData[id] & 0xF) << 3), ((tMenu->textData[id] >> 4) << 3) + textHighlight,
                          textMenuSurfaceNo);
        id++;
    }
}
void DrawTextMenu(void *menu, int XPos, int YPos)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int cnt         = 0;

    if (tMenu->visibleRowCount > 0) {
        cnt = (int)(tMenu->visibleRowCount + tMenu->visibleRowOffset);
    }
    else {
        tMenu->visibleRowOffset = 0;
        cnt                     = (int)tMenu->rowCount;
    }

    if (tMenu->selectionCount == 3) {
        tMenu->selection2 = -1;
        for (int i = 0; i <= tMenu->selection1; ++i) {
            if (tMenu->entryHighlight[i]) {
                tMenu->selection2 = i;
            }
        }
    }

    switch (tMenu->alignment) {
        case 0:
            for (int i = (int)tMenu->visibleRowOffset; i < cnt; ++i) {
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);
                        break;

                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);
                        break;

                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);

                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, XPos, YPos, 128);
                        break;
                }
                YPos += 8;
            }
            break;

        case 1:
            for (int i = (int)tMenu->visibleRowOffset; i < cnt; ++i) {
                int entryX = XPos - (tMenu->entrySize[i] << 3);
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);
                        break;

                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);
                        break;

                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);

                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, entryX, YPos, 128);
                        break;
                }
                YPos += 8;
            }
            break;

        case 2:
            for (int i = (int)tMenu->visibleRowOffset; i < cnt; ++i) {
                int entryX = XPos - (tMenu->entrySize[i] >> 1 << 3);
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);
                        break;
                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);
                        break;
                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 128);
                        else
                            DrawTextMenuEntry(tMenu, i, entryX, YPos, 0);

                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, entryX, YPos, 128);
                        break;
                }
                YPos += 8;
            }
            break;

        default: break;
    }
}
