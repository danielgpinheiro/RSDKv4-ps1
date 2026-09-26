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
// BG line strips. Not yet (counted in g_ps1DrawTodo): tint, VScroll layers, 3D faces (phase 7), menu text
// (phase 4). Sonic 2 has no 3D floor / sky layer.
#include "RetroEngine.hpp"

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
// such as the menu's panels, the ClearScreen rectangle): the CLUT fade doesn't reach them, so FlipScreen
// applies the same blend to their colour words, still in the chain there (psyqo sends it after flip()).
struct PS1DirectColour {
    uint32_t *word;
    uint8_t r, g, b;
};
#define PS1_DIRECT_COLOURS 96
static PS1DirectColour s_directColours[PS1_DIRECT_COLOURS];
static int s_directCount = 0;
volatile uint32_t g_ps1FadeDirect = 0, g_ps1FadeDirectDropped = 0; // colours faded / not recorded (list full)

// One flat primitive (w[0] = GP0 command | BGR) written straight into the chain, its colour recorded.
static void PS1ChainFlatRaw(const uint32_t *w, int words)
{
    uint32_t *d = PS1ChainRawBegin(words);
    if (!d)
        return;
    for (int i = 0; i < words; ++i) d[i] = w[i];
    PS1ChainRawEnd();
    if (s_directCount < PS1_DIRECT_COLOURS)
        s_directColours[s_directCount++] = PS1DirectColour{ d, (uint8_t)(w[0] & 0xFF), (uint8_t)(w[0] >> 8), (uint8_t)(w[0] >> 16) };
    else
        g_ps1FadeDirectDropped = g_ps1FadeDirectDropped + 1;
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

// Fills chunks [first, end) (end < 0: up to s_palChunkCount) with `bank`'s palette.
static void PS1FillPalette(int bank, int first = 0, int end = -1)
{
    PS1BuildPalette(bank);
    if (end < 0)
        end = s_palChunkCount;
    for (int i = first; i < end; ++i) memcpy(s_palChunks[i].dst, &s_pal[s_palChunks[i].src], s_palChunks[i].n * sizeof(uint16_t));
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
void PS1RenderMenuFrame(bool clear) { PS1FrameStart(clear); }

void PS1RenderEndFrame()
{
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
static void PS1ClearClusterCache() { memset(s_clusterCache, 0xFF, sizeof(s_clusterCache)); }

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

// Axis-aligned piece at screen rect (x, y, w, h) with optional mirroring: 1:1 mirrors sample texel
// u + w - 1 - k at column k (UV edges u + w - 1 .. u - 1), scaled ones the full [u, u + w) range.
static void PS1ChainPieceRect(const PS1Piece &p, int x, int y, int w, int h, bool flipX, bool flipY)
{
    if (!flipX && !flipY && w == p.w && h == p.h) {
        PS1ChainPieceSprite(p, x, y);
        return;
    }
    bool unit = w == p.w && h == p.h;
    int u0 = p.u, u1 = p.u + p.w, v0 = p.v, v1 = p.v + p.h;
    if (flipX) {
        u0 = unit ? p.u + p.w - 1 : p.u + p.w;
        u1 = unit ? p.u - 1 : p.u;
    }
    if (flipY) {
        v0 = unit ? p.v + p.h - 1 : p.v + p.h;
        v1 = unit ? p.v - 1 : p.v;
    }
    int16_t xs[4] = { (int16_t)x, (int16_t)(x + w), (int16_t)x, (int16_t)(x + w) };
    int16_t ys[4] = { (int16_t)y, (int16_t)y, (int16_t)(y + h), (int16_t)(y + h) };
    PS1ChainPieceQuad(p, xs, ys, u0, u1, v0, v1);
}

// GPU limits (psx-spx GPU "Vertex"): coordinates are 11-bit signed (-1024..1023, beyond that they wrap)
// and a polygon spanning more than 1023 x 511 pixels is not drawn at all. Upstream's software renderer
// clips everything to the screen; here primitives beyond the limits are clipped to it first (a UFO
// next to the special-stage camera is drawn up to ~96x its size; the menu background's 3D panels
// project to several screens).
volatile uint32_t g_ps1GpuClipped = 0; // primitives clipped to the screen because they were beyond the limits
volatile uint32_t g_ps1GpuLimit   = 0; // primitives still beyond the limits (drawn as they are: GDB check)

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
    int cx0 = x < 0 ? 0 : x, cx1 = x + w > SCREEN_XSIZE ? SCREEN_XSIZE : x + w;
    int cy0 = y < 0 ? 0 : y, cy1 = y + h > SCREEN_YSIZE ? SCREEN_YSIZE : y + h;
    if (cx0 >= cx1 || cy0 >= cy1)
        return;
    auto edge = [](int base, int texels, int pos, int len, bool flip) {
        int t = (int)((int64_t)pos * texels / len);
        return flip ? base + texels - t : base + t;
    };
    int u0 = edge(p.u, p.w, cx0 - x, w, flipX), u1 = edge(p.u, p.w, cx1 - x, w, flipX);
    int v0 = edge(p.v, p.h, cy0 - y, h, flipY), v1 = edge(p.v, p.h, cy1 - y, h, flipY);
    int16_t xs[4] = { (int16_t)cx0, (int16_t)cx1, (int16_t)cx0, (int16_t)cx1 };
    int16_t ys[4] = { (int16_t)cy0, (int16_t)cy0, (int16_t)cy1, (int16_t)cy1 };
    PS1ChainPieceQuad(p, xs, ys, u0, u1, v0, v1);
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
        int a = s_fadeA, na = 255 - a;
        for (int i = 0; i < s_directCount; ++i) {
            const PS1DirectColour &c = s_directColours[i];
            uint32_t r = (c.r * na + s_fadeR * a) / 255, g = (c.g * na + s_fadeG * a) / 255, b = (c.b * na + s_fadeB * a) / 255;
            *c.word    = (*c.word & 0xFF000000) | r | g << 8 | b << 16;
        }
        g_ps1FadeDirect = g_ps1FadeDirect + s_directCount;
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
void DrawObjectList(int layer)
{
    PS1FrameStart(false);
    uint32_t t0 = PS1Hblanks();
    g_ps1ChainSkipPrims = PS1ObjectsHidden();
    int size            = drawListEntries[layer].listSize;
    for (int i = 0; i < size; ++i) {
        objectEntityPos = drawListEntries[layer].entityRefs[i];
        int type        = objectEntityList[objectEntityPos].type;
        if (type) {
            uint32_t tt = PS1ProfTicks();
            if (scriptCode[objectScriptList[type].eventDraw.scriptCodePtr] > 0)
                ProcessScript(objectScriptList[type].eventDraw.scriptCodePtr, objectScriptList[type].eventDraw.jumpTablePtr, EVENT_DRAW);
            g_ps1TypeDrawTicks[type] += (PS1ProfTicks() - tt) & 0xFFFF;
            g_ps1TypeDrawCalls[type]++;
        }
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
        if (forceUseScripts || Engine.usingOrigins)
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

static void PS1UpdateChunkRows(int chunk)
{
    uint16_t m          = 0;
    const ushort *tiles = &tiles128x128.ps1Tile[chunk << 6];
    for (int t = 0; t < 64; ++t) {
        uint16_t tv = tiles[t];
        int plane   = tv >> PS1_TV_PLANE_SHIFT;
        if (plane <= 1 && PS1TileSolid(tv & PS1_TV_INDEX))
            m |= 1 << ((t >> 3) * 2 + plane);
    }
    s_chunkRowPlanes[chunk] = m;
}

void PS1BuildTileVisuals()
{
    for (int c = 0; c < CHUNKTILE_COUNT / 64; ++c) PS1UpdateChunkRows(c);
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
    if (solid && !wasSolid) { // chunk tiles showing `dest` were skipped as empty
        PS1BuildTileVisuals();
        g_ps1TileRebuild = g_ps1TileRebuild + 1;
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
static void PS1DrawTileBand(const TileLayer *layer, byte above, int sy, int h, int ly, int chunkX, int fullW)
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
    uint32_t quads[24 * 18];
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
        PS1ChainPrimsRawN(quads, 9, quadWords / 9);
    }
    g_ps1LayerPrims = g_ps1LayerPrims + prims;
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
#define PS1_STRIP_PIECES 9 // tools/bgstrips MAX_PIECES
struct PS1StripRegion {
    uint8_t layer, plane, pieces, pad;
    uint16_t line0, lines, period;
    uint16_t x[PS1_STRIP_PIECES], y[PS1_STRIP_PIECES], w[PS1_STRIP_PIECES]; // piece VRAM origin (hw) + width (texels)
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
    bool ok   = fread(head, 1, 8, f) == 8 && !memcmp(head, "BGS2", 4);
    if (ok) {
        count = head[4] | (head[5] << 8);
        ok    = count <= PS1_STRIP_MAX;
    }
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
    }
    const int stagingBytes = 16 * 1024;
    uint8_t *staging       = ok ? (uint8_t *)psyqo_malloc(stagingBytes) : nullptr;
    uint32_t bytes         = 0;
    if (staging) {
        PS1GpuImmediate();
        for (int i = 0; ok && i < count; ++i) {
            PS1StripRegion &r = s_strips[i];
            for (int k = 0; ok && k < r.pieces; ++k) {
                int rowsPerChunk = stagingBytes / r.w[k];
                for (int row = 0; ok && row < r.lines; row += rowsPerChunk) {
                    int n = r.lines - row < rowsPerChunk ? r.lines - row : rowsPerChunk;
                    ok    = fread(staging, 1, n * r.w[k], f) == (size_t)(n * r.w[k]);
                    if (ok)
                        g_ps1GPU->uploadToVRAM((const uint16_t *)staging,
                                               psyqo::Rect{ { (int16_t)r.x[k], (int16_t)(r.y[k] + row) }, { (int16_t)(r.w[k] / 2), (int16_t)n } });
                    bytes += n * r.w[k];
                }
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
void DrawHLineScrollLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;
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
        for (int b = i; b < j; ++b)
            PS1WalkStripBand(r, s_stripBands[b].sy, s_stripBands[b].h, s_stripBands[b].ly, s_stripBands[b].chunkX,
                             stripClut[s_stripBands[b].setB], count);
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
            attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
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

static void PS1DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    if (width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) { PS1ChainPieceSprite(p, XPos + p.ox, YPos + p.oy); });
}

// The engine's DrawSprite calls are hashed (g_ps1DrawHash, as phase 2's headless renderer did); the renderer's
// own uses (unrotated DrawSpriteRotated, ink passes) call PS1DrawSprite.
void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    uint32_t h = g_ps1DrawHash;
    for (int a : { XPos, YPos, width, height, sprX, sprY, sheetID }) h = h * 31 + (uint32_t)a;
    g_ps1DrawHash  = h;
    g_ps1DrawCount = g_ps1DrawCount + 1;
    PS1DrawSprite(XPos, YPos, width, height, sprX, sprY, sheetID);
}

void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID)
{
    if (width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden())
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
    int sn = sin512LookupTable[rotation] * scale >> 9;
    int cs = cos512LookupTable[rotation] * scale >> 9;
    bool fx = direction != FLIP_NONE;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) {
        int16_t xs[4], ys[4];
        const int lx[4] = { p.ox, p.ox + p.w, p.ox, p.ox + p.w };
        const int ly[4] = { p.oy, p.oy, p.oy + p.h, p.oy + p.h };
        for (int k = 0; k < 4; ++k) {
            int x = fx ? pivotX - lx[k] : lx[k] - pivotX;
            int y = ly[k] - pivotY;
            xs[k] = (int16_t)(XPos + ((x * cs + y * sn) >> 9));
            ys[k] = (int16_t)(YPos + ((y * cs - x * sn) >> 9));
        }
        PS1ChainPieceQuad(p, xs, ys, p.u, p.u + p.w, p.v, p.v + p.h);
    });
}

void DrawSpriteRotated(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID)
{
    if (!(rotation & 0x1FF) && direction == FLIP_NONE) { // no rotation: 1:1 sprite (pixel exact)
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

void DrawFace(void *v, uint colour)
{
    if (!s_frameOpen || PS1ObjectsHidden())
        return;
    const Vertex *q = (const Vertex *)v;
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

void DrawTexturedFace(void *v, byte sheetID)
{
    if (!s_frameOpen || PS1ObjectsHidden())
        return;
    const Vertex *q = (const Vertex *)v;
    int sheet       = PS1SurfaceSheet(sheetID);
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
        g_ps1FaceTexMiss = g_ps1FaceTexMiss + 1;
        return;
    }
    psyqo::PrimPieces::TPageAttr tp = PS1ClusterTPage(c);
    uint16_t tpage, clut;
    memcpy(&tpage, &tp, 2);
    psyqo::PrimPieces::ClutIndex ci = PS1ClusterClut(c);
    memcpy(&clut, &ci, 2);
    auto uv = [&](int k) {
        int u = c->u + q[k].u - c->srcX, vv = c->v + q[k].v - c->srcY;
        return (uint32_t)(uint8_t)(u > 255 ? 255 : u) | ((uint32_t)(uint8_t)(vv > 255 ? 255 : vv) << 8);
    };
    uint32_t w[9];
    w[0] = 0x2C808080; // GP0 2Ch textured quad, colour 0x80 (texels unchanged)
    for (int k = 0; k < 4; ++k) w[1 + 2 * k] = (uint32_t)(uint16_t)q[k].x | ((uint32_t)(uint16_t)q[k].y << 16);
    w[2] = uv(0) | ((uint32_t)clut << 16);
    w[4] = uv(1) | ((uint32_t)tpage << 16);
    w[6] = uv(2);
    w[8] = uv(3);
    PS1ChainPrimsRawN(w, 9, 1);
    g_ps1Faces = g_ps1Faces + 1;
}
void DrawFadedFace(void *v, uint color, uint fogColor, int alpha) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }  // special stage: phase 7
void DrawTexturedFaceBlended(void *v, byte sheetID) { g_ps1DrawTodo = g_ps1DrawTodo + 1; } // special stage: phase 7

// Text menus (the scripts' DrawMenu: level select, results): phase 4 (2D menus). Counted.
void DrawTextMenu(void *menu, int XPos, int YPos) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }
