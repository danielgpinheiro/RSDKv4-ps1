// RSDKv3 (Sonic CD) PS1 renderer: replaces RSDKv3/Drawing.cpp (the PC software/GL renderer) on PS1.
//
// Everything a frame draws is DMA-chained (ps1/gpu_chain.hh, the Nexus port's chain): main.cpp opens the
// chain around Engine.RunFrame(), psyqo sends it after frame() returns. Sprites come from the stage's
// VRAM atlas (ps1/sprite_atlas, tools/atlas), tiles from the stage's tile pages (ps1/tileset,
// tools/tiles): nothing is decoded at runtime.
//
// Upstream Drawing.cpp functions also change game state, not just pixels (phase 2.3: the title card
// works from its draw sub). Every such side effect is ported here explicitly, marked "upstream":
// draw subs (DrawObjectList), draw order + water position (DrawStageGFX), the BG autoscroll and
// parallax line positions (DrawHLineScrollLayer / DrawVLineScrollLayer), the view globals
// (InitRenderDevice), DrawObjectAnimation's dispatch.
//
// Phase 3.2 scope (docs/28): chain + clear + palette, atlas sprites (plain / flipped / scaled / rotated
// / rotozoom), HScroll tile layers. Not yet (counted in g_ps1DrawTodo): ink effects and tint (3.3),
// per-line palette banks + water palette (3.4), VScroll / 3D layers / faces (3.5, 7), menu text (4).
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
#include "fmv.hh"
#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include "tileset.hh"
#include "video.hh" // PS1Hblanks (profiling)

extern psyqo::GPU *g_ps1GPU;

// ---------------------------------------------------------------------------------------------------
// Engine globals owned by Drawing.cpp upstream

int SCREEN_XSIZE        = DEFAULT_SCREEN_XSIZE; // 320 (user decision; PC 424)
int SCREEN_CENTERX      = DEFAULT_SCREEN_XSIZE / 2;
int SCREEN_XSIZE_CONFIG = DEFAULT_SCREEN_XSIZE;

DrawListEntry drawListEntries[DRAWLAYER_COUNT];

int gfxDataPosition;
GFXSurface gfxSurface[SURFACE_COUNT];
byte graphicData[GFXDATA_SIZE];

ushort gfxVertexSize       = 0;
ushort gfxVertexSizeOpaque = 0;
ushort gfxIndexSize        = 0;
ushort gfxIndexSizeOpaque  = 0;
ushort vertexSize3D        = 0;
ushort indexSize3D         = 0;
ushort tileUVArray[TILEUV_SIZE];
bool render3DEnabled = false;
byte texBufferMode   = 0;

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
    else {
        for (int i = 1; i < 256; ++i) s_pal[i] = PS1Conv(pal[i].r, pal[i].g, pal[i].b);
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

// Fills chunks [first, s_palChunkCount) with `bank`'s palette.
static void PS1FillPalette(int bank, int first = 0)
{
    PS1BuildPalette(bank);
    for (int i = first; i < s_palChunkCount; ++i) memcpy(s_palChunks[i].dst, &s_pal[s_palChunks[i].src], s_palChunks[i].n * sizeof(uint16_t));
}

// Master + atlas 4-bit CLUT rows, chained here (filled by PS1FillPalette).
static void PS1ReservePaletteRows()
{
    PS1ReserveRow(0, 256, PS1_CLUT_X, PS1_CLUT_Y);
    for (int k = 0; k < g_ps1AtlasClutCount;) { // runs of adjacent CLUT slots in one VRAM row
        int run = 1;
        while (k + run < g_ps1AtlasClutCount && g_ps1AtlasCluts[k + run].y == g_ps1AtlasCluts[k].y
               && g_ps1AtlasCluts[k + run].x == g_ps1AtlasCluts[k].x + 16 * run)
            ++run;
        PS1ReserveRow(256 + k * 16, run * 16, (int16_t)g_ps1AtlasCluts[k].x, (int16_t)g_ps1AtlasCluts[k].y);
        k += run;
    }
}

// ---------------------------------------------------------------------------------------------------
// Frame: main.cpp calls PS1RenderBeginFrame / PS1RenderEndFrame around Engine.RunFrame(). The first
// draw of a frame starts it: DrawStageGFX clears; anything else (the paused stage mode draws only its
// object lists over the last image, like PC's persistent framebuffer) repeats the last frame first.

static bool s_chainOpen  = false;
static uint32_t *s_sceneMark = nullptr; // chain position after the clear and the frame-start CLUT rows
static bool s_frameOpen  = false;
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
    PS1ReservePaletteRows();
    PS1FillPalette(gfxLineBuffer[0]);
    PS1ChainFlushCache(); // the GPU caches CLUTs
    PS1ChainInvalidateTPage();
    s_sceneMark = PS1ChainMark();
}

static bool s_fmvFrame = false; // this frame shows an FMV (ps1/fmv.cpp): FlipScreen leaves it alone

void PS1RenderBeginFrame(psyqo::GPU &gpu)
{
    PS1ChainBegin(gpu);
    PS1AtlasServicePictures(); // on-demand pictures the last frame asked for (nothing chained yet)
    PS1FmvBeginFrame();        // FMV open / close (swaps the chain arenas: nothing chained yet)
    s_chainOpen = true;
    s_frameOpen = false;
    s_fmvFrame  = false;
}

// FMV frames (ps1/fmv.cpp): 0 = black, 1 = a new picture was just written into the other buffer (the one
// shown after this frame's flip): it is copied into this frame's buffer too, so both hold it; 2 = no new
// picture: the displayed frame is copied over (the skip fade darkens it a step further each frame).
void PS1RenderFmvFrame(int mode)
{
    if (s_frameOpen || !s_chainOpen)
        return;
    s_frameOpen = true;
    s_fmvFrame  = true;
    if (mode == 1) {
        PS1ChainFillLines(0, 32, 0, 0, 0);
        PS1ChainCopyLastFrameLines(32, 208);
        PS1ChainFillLines(208, SCREEN_YSIZE, 0, 0, 0);
    } else if (mode == 2) {
        PS1ChainCopyLastFrameLines(0, SCREEN_YSIZE);
    } else {
        PS1ChainFillLines(0, SCREEN_YSIZE, 0, 0, 0);
    }
}

// FMV skip fade (upstream ProcessVideo: fadeMode += 8 per frame): the frame, a copy of the last one, is
// darkened by step / 255 with subtractive blending, so 32 frames reach black.
void PS1RenderFmvFade(int step)
{
    psyqo::Prim::Rectangle r(psyqo::Color{ { .r = (uint8_t)step, .g = (uint8_t)step, .b = (uint8_t)step } });
    r.position = psyqo::Vertex{ { .x = 0, .y = 0 } };
    r.size     = psyqo::Vertex{ { .w = (int16_t)SCREEN_XSIZE, .h = SCREEN_YSIZE } };
    psyqo::PrimPieces::TPageAttr attr; // untextured semi-transparency takes its mode from the texpage
    attr.set(psyqo::Prim::TPageAttr::SemiTrans::FullBackSubFullFront);
    PS1ChainTPage(attr);
    r.setSemiTrans();
    PS1ChainRect(r);
}

void PS1RenderEndFrame()
{
    if (!s_frameOpen)
        PS1FrameStart(false); // nothing drawn: keep the image in both buffers
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

static psyqo::PrimPieces::ClutIndex PS1ClusterClut(const PS1AtlasCluster *c)
{
    if (c->clut == 0xFFFF)
        return psyqo::PrimPieces::ClutIndex(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
    const PS1AtlasClut &k = g_ps1AtlasCluts[c->clut];
    return psyqo::PrimPieces::ClutIndex(psyqo::Vertex{ { .x = (int16_t)k.x, .y = (int16_t)k.y } });
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

static void PS1ChainPieceSprite(const PS1Piece &p, int x, int y)
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
    s.size     = psyqo::Vertex{ { .w = p.w, .h = p.h } };
    s.texInfo  = psyqo::PrimPieces::TexInfo{ (uint8_t)p.u, (uint8_t)p.v, PS1ClusterClut(p.c) };
    PS1ChainSprite(s);
    g_ps1DrawSprites = g_ps1DrawSprites + 1;
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
    PS1ChainQuad(q);
    g_ps1DrawQuads = g_ps1DrawQuads + 1;
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

int InitRenderDevice()
{
    // upstream InitRenderDevice / SetScreenSize: the logic's view (scroll edges, object activity border)
    SCREEN_XSIZE        = DEFAULT_SCREEN_XSIZE;
    SCREEN_CENTERX      = SCREEN_XSIZE / 2;
    SCREEN_SCROLL_LEFT  = SCREEN_CENTERX - 8;
    SCREEN_SCROLL_RIGHT = SCREEN_CENTERX + 8;
    OBJECT_BORDER_X2    = SCREEN_XSIZE + 0x80;
    return 1;
}

// PC converts the palette here, after drawing: the CLUT rows chained at the frame start get the
// palette as it is now (a SetFade / palette change from a draw sub shows in the same frame).
volatile uint32_t g_ps1BankRegions = 0; // palette-bank regions (runs of equal gfxLineBuffer) in the last frame
volatile uint32_t g_ps1BankMask    = 0; // banks used by the last frame (bit per bank)
volatile uint32_t g_ps1BankReplays = 0; // regions drawn again with their own bank (total)

// Per-line palette banks (upstream SetActivePalette / gfxLineBuffer; user decision 2026-09-24: replay
// per region). The frame's first pass gets the bank covering the most lines; every further run of lines with another bank
// gets its bank's CLUT rows (same VRAM rows), a drawing area of those lines, a fresh base (clear
// colour, or the last frame in a repeated frame) and a replay of the frame's primitives.
static int s_baseBank = 0; // bank of the frame's first pass (the one covering the most lines)

static void PS1DrawBankRegions(psyqo::Color clear)
{
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
    if (!s_frameOpen || s_fmvFrame)
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
    int frameChunks = s_palChunkCount;
    PS1DrawBankRegions(clear);
    int end         = s_palChunkCount; // the regions filled their own chunks: refill only the frame-start rows
    s_palChunkCount = frameChunks;
    PS1FillPalette(s_baseBank); // the first pass is drawn with the base bank
    s_palChunkCount = end;
    if (s_fadePending) {
        g_ps1ClutFades = g_ps1ClutFades + 1;
        if (s_clearPrim) // cleared (undrawn) pixels are black: under the fade, fade colour * a
            s_clearPrim->setColor(clear);
    }
}

void GenerateBlendLookupTable() {}
void UpdateHardwareTextures() {}

void ClearScreen(byte index)
{
    PS1FrameStart(true);
    const PaletteEntry &c = fullPalette32[gfxLineBuffer[0]][index];
    uint32_t w[3] = { 0x60000000u | c.r | (uint32_t)c.g << 8 | (uint32_t)c.b << 16, 0, (uint32_t)SCREEN_XSIZE | (uint32_t)SCREEN_YSIZE << 16 };
    PS1ChainFlatRaw(w, 3); // GP0 60h: flat rectangle
}

// upstream: draw subs are game logic too; only their Draw* calls produce primitives.
void DrawObjectList(int layer)
{
    PS1FrameStart(false);
    uint32_t t0 = PS1Hblanks();
    g_ps1ChainSkipPrims = PS1ObjectsHidden();
    int size            = drawListEntries[layer].listSize;
    for (int i = 0; i < size; ++i) {
        objectLoop = drawListEntries[layer].entityRefs[i];
        int type   = objectEntityList[objectLoop].type;
        if (type) {
            activePlayer = 0;
            if (scriptCode[objectScriptList[type].subDraw.scriptCodePtr] > 0)
                ProcessScript(objectScriptList[type].subDraw.scriptCodePtr, objectScriptList[type].subDraw.jumpTablePtr, SUB_DRAW);
        }
    }
    g_ps1ChainSkipPrims = false;
    g_ps1ProfObjDrawHbl = g_ps1ProfObjDrawHbl + PS1HblanksSince(t0);
}

void DrawHLineScrollLayer(int layerID);
void DrawVLineScrollLayer(int layerID);
static void PS1Draw3DFloor(int layerID);

static void PS1DrawLayer(int layerID)
{
    if (activeTileLayers[layerID] >= LAYER_COUNT)
        return;
    switch (stageLayouts[activeTileLayers[layerID]].type) {
        case LAYER_HSCROLL: DrawHLineScrollLayer(layerID); break;
        case LAYER_VSCROLL: DrawVLineScrollLayer(layerID); break;
        case LAYER_3DFLOOR:
        case LAYER_3DSKY: PS1Draw3DFloor(layerID); break;
        default: break;
    }
}

// upstream DrawStageGFX (SW path): water position, then layers and draw lists in upstream order
// (list 7 before 6), then the full-screen fade rectangle.
volatile uint32_t g_ps1ProfWaitHbl = 0; // profiling (g_ps1DebugDrawMask bit 2): time waiting for the previous chain
void DrawStageGFX()
{
    if (g_ps1DebugDrawMask & 4) { // profiling: draw with the GPU/DMA idle (bus contention test)
        uint32_t t0 = PS1Hblanks();
        PS1GpuImmediate();
        g_ps1ProfWaitHbl = PS1HblanksSince(t0);
    }
    PS1FrameStart(true);
    waterDrawPos = waterLevel - yScrollOffset;
    if (waterDrawPos < 0)
        waterDrawPos = 0;
    if (waterDrawPos > SCREEN_YSIZE)
        waterDrawPos = SCREEN_YSIZE;

    DrawObjectList(0);
    PS1DrawLayer(0);
    DrawObjectList(1);
    PS1DrawLayer(1);
    DrawObjectList(2);
    PS1DrawLayer(2);
    DrawObjectList(3);
    DrawObjectList(4);
    PS1DrawLayer(3);
    DrawObjectList(5);
    if (forceUseScripts || Engine.usingOrigins)
        DrawObjectList(7);
    DrawObjectList(6);
    if (fadeMode > 0)
        DrawRectangle(0, 0, SCREEN_XSIZE, SCREEN_YSIZE, fadeR, fadeG, fadeB, fadeA);
}

void DrawDebugOverlays() {}

// ---------------------------------------------------------------------------------------------------
// Tile layers. Each chunk tile's visual as one halfword (built at stage load, updated when a script
// changes a tile): bits 0-9 tile index, 10-11 flip, 12 visual plane, 13 "has pixels" (the tile page
// scan in ps1/tileset). Plus, per chunk, which of its 8 tile rows have visible tiles on each plane.

#define PS1_TV_INDEX 0x03FF
#define PS1_TV_DIR_SHIFT 10
#define PS1_TV_PLANE 0x1000
#define PS1_TV_SOLID 0x2000
static uint16_t s_tileVis[CHUNKTILE_COUNT];
static uint16_t s_chunkRowPlanes[CHUNKTILE_COUNT / 64];

static inline uint16_t PS1TileVisOf(int i)
{
    int t = tiles128x128.tileIndex[i] & PS1_TV_INDEX;
    return (uint16_t)(t | ((tiles128x128.direction[i] & 3) << PS1_TV_DIR_SHIFT) | (tiles128x128.visualPlane[i] ? PS1_TV_PLANE : 0)
                      | ((g_ps1TileSolid[t >> 3] >> (t & 7)) & 1 ? PS1_TV_SOLID : 0));
}

static void PS1UpdateChunkRows(int chunk)
{
    uint16_t m = 0;
    for (int t = 0; t < 64; ++t) {
        uint16_t tv = s_tileVis[(chunk << 6) + t];
        if (tv & PS1_TV_SOLID)
            m |= 1 << ((t >> 3) * 2 + ((tv & PS1_TV_PLANE) ? 1 : 0));
    }
    s_chunkRowPlanes[chunk] = m;
}

void PS1BuildTileVisuals()
{
    for (int i = 0; i < CHUNKTILE_COUNT; ++i) s_tileVis[i] = PS1TileVisOf(i);
    for (int c = 0; c < CHUNKTILE_COUNT / 64; ++c) PS1UpdateChunkRows(c);
}

void PS1TileChanged(int chunkTile)
{
    if ((uint)chunkTile >= CHUNKTILE_COUNT)
        return;
    s_tileVis[chunkTile] = PS1TileVisOf(chunkTile);
    PS1UpdateChunkRows(chunkTile >> 6);
}

// upstream Copy16x16Tile (Scene.hpp, SW path): tile `src`'s pixels overwrite tile `dest`'s, from now
// on. Here: a VRAM-to-VRAM copy inside the tile pages, chained where the script calls it (draws before
// it still show the old pixels, as on PC), or immediate outside a frame.
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
    if (s_chainOpen) {
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
static uint32_t s_tileClutWord = 0;

static void PS1TileSetup()
{
    if (s_tileClutWord)
        return;
    psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
    uint16_t c;
    memcpy(&c, &clut, 2);
    s_tileClutWord = (uint32_t)c << 16;
    for (int p = 0; p < 4; ++p) {
        s_tileTPage[p].set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
        s_tileTPage[p].setPageX((uint8_t)(PS1_TILE_PAGE_X + 2 * (p & 1)));
        s_tileTPage[p].setPageY((uint8_t)(PS1_TILE_PAGE_Y + (p >> 1)));
    }
}

// Textured quad over tile texels, UV edges uL..uR (exclusive, may run backwards) and vT..vB.
static void PS1ChainTileQuad(int x, int y, int w, int h, int pageX, int pageY, int uL, int uR, int vT, int vB)
{
    psyqo::Prim::TexturedQuad quad(psyqo::Color{ { .r = 0x80, .g = 0x80, .b = 0x80 } });
    quad.pointA    = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)y } };
    quad.uvA       = psyqo::PrimPieces::UVCoords{ (uint8_t)uL, (uint8_t)vT };
    quad.clutIndex = psyqo::PrimPieces::ClutIndex(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
    quad.pointB    = psyqo::Vertex{ { .x = (int16_t)(x + w), .y = (int16_t)y } };
    quad.uvB       = psyqo::PrimPieces::UVCoords{ (uint8_t)uR, (uint8_t)vT };
    quad.tpage.setPageX((uint8_t)pageX);
    quad.tpage.setPageY((uint8_t)pageY);
    quad.tpage.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
    quad.pointC = psyqo::Vertex{ { .x = (int16_t)x, .y = (int16_t)(y + h) } };
    quad.uvC    = psyqo::PrimPieces::UVCoordsPadded{ (uint8_t)uL, (uint8_t)vB, 0 };
    quad.pointD = psyqo::Vertex{ { .x = (int16_t)(x + w), .y = (int16_t)(y + h) } };
    quad.uvD    = psyqo::PrimPieces::UVCoordsPadded{ (uint8_t)uR, (uint8_t)vB, 0 };
    PS1ChainQuad(quad);
}

// Rows [ty0, ty0 + h) of a tile (rows before flipping) at screen (sx, sy): unflipped as a sprite,
// flipped as a quad with backwards UVs (the Nexus port's PS1DrawTilePiece; old GPUs have no
// rectangle flip bits, psx-spx).
static void PS1DrawTilePiece(int sx, int sy, int tileIndex, int ty0, int h, int dir)
{
    int page = tileIndex >> 8, pt = tileIndex & 0xFF;
    int u = (pt & 0xF) << 4, v = (pt >> 4) << 4;
    int pageX = PS1_TILE_PAGE_X + 2 * (page & 1), pageY = PS1_TILE_PAGE_Y + (page >> 1);
    if (dir == FLIP_NONE) {
        PS1ChainTPage(s_tileTPage[page]);
        psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
        psyqo::Prim::Sprite s(psyqo::Color{ { .r = 0x80, .g = 0x80, .b = 0x80 } });
        s.position = psyqo::Vertex{ { .x = (int16_t)sx, .y = (int16_t)sy } };
        s.texInfo  = psyqo::PrimPieces::TexInfo{ (uint8_t)u, (uint8_t)(v + ty0), clut };
        s.size     = psyqo::Vertex{ { .x = TILE_SIZE, .y = (int16_t)h } };
        PS1ChainSprite(s);
        return;
    }
    int uL = (dir & FLIP_X) ? u + 15 : u;
    int uR = (dir & FLIP_X) ? u - 1 : u + 16;
    int dv = (dir & FLIP_Y) ? -1 : 1;
    int vT = (dir & FLIP_Y) ? v + 15 - ty0 : v + ty0;
    if (uR < 0) { // u -1 / 256 don't fit a UV byte: address the tile from the neighbouring 8-bit page
        pageX -= 1, uL += 128, uR += 128;
    }
    else if (uR > 255) {
        pageX += 1, uL -= 128, uR -= 128;
    }
    int vB = vT + h * dv;
    if (vB < 0 || vB > 255) { // pageY can't shift: the last row gets its own constant-v strip
        int vLast = vT + (h - 1) * dv;
        if (h > 1)
            PS1ChainTileQuad(sx, sy, TILE_SIZE, h - 1, pageX, pageY, uL, uR, vT, vLast);
        PS1ChainTileQuad(sx, sy + h - 1, TILE_SIZE, 1, pageX, pageY, uL, uR, vLast, vLast);
        return;
    }
    PS1ChainTileQuad(sx, sy, TILE_SIZE, h, pageX, pageY, uL, uR, vT, vB);
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
    uint32_t clut  = s_tileClutWord;
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
    uint16_t want          = PS1_TV_SOLID | (above ? PS1_TV_PLANE : 0);
    uint32_t yWord         = (uint32_t)(uint16_t)sy << 16;
    uint32_t clutWord      = s_tileClutWord;
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
            const uint16_t *vis = &s_tileVis[(chunk << 6) + tileRow + first];
            int x               = sx;
            for (int k = 0; k < count && x < SCREEN_XSIZE; ++k, x += TILE_SIZE) {
                uint16_t tv = vis[k];
                if ((tv & (PS1_TV_SOLID | PS1_TV_PLANE)) != want)
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

// BG line strips (tools/bgstrips/build_bgstrips.py -> Data/Stages/<Stage>/BGStrips[_t].bin, format
// there): BG layer lines pre-rendered at build time as 8-bit rows of one horizontal period, in VRAM
// the stage's atlas leaves free. A band of lines in a strip region costs 1-3 sprites instead of ~21
// tiles (per-line parallax would otherwise make one band per line). From the Nexus port.
#define PS1_STRIP_MAX    64
#define PS1_STRIP_PIECES 9 // tools/bgstrips MAX_PIECES
struct PS1StripRegion {
    uint8_t layer, plane, pieces, pad;
    uint16_t line0, lines, period;
    uint16_t x[PS1_STRIP_PIECES], y[PS1_STRIP_PIECES], w[PS1_STRIP_PIECES]; // piece VRAM origin (hw) + width (texels)
};
static PS1StripRegion s_strips[PS1_STRIP_MAX];
static int s_stripCount = 0;
static uint8_t s_stripRow[LAYER_COUNT][2][TILELAYER_SCROLL_COUNT / TILE_SIZE]; // region + 1 per tile row
volatile uint32_t g_ps1StripRegions = 0; // regions loaded for this stage
volatile uint32_t g_ps1StripBytes   = 0; // strip texel bytes uploaded by the last load

// Loads the stage's strips (player 1 = Tails: BGStrips_t.bin, placed around the Tails atlas) into VRAM.
// Immediate uploads: call during the stage load, after PS1AtlasLoad.
void PS1LoadBGStrips(const char *folder, int player)
{
    s_stripCount = 0;
    g_ps1StripRegions = 0;
    memset(s_stripRow, 0, sizeof(s_stripRow));
    char path[96];
    snprintf(path, sizeof(path), "Data/Stages/%s/BGStrips%s.bin", folder, player == 1 ? "_t" : "");
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
        ok = ok && r.pieces <= PS1_STRIP_PIECES && r.layer < LAYER_COUNT && r.line0 + r.lines <= TILELAYER_SCROLL_COUNT;
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
    uint8_t region;
};
static PS1StripBand s_stripBands[SCREEN_YSIZE];
static short s_lineX[SCREEN_YSIZE];

// upstream DrawHLineScrollLayer: the same scroll-state update (BG autoscroll, parallax line
// positions: game state) and per-line X (parallax line, deformation above / below the water line,
// wrap), drawn as bands of lines sharing one X (a band never crosses a tile row).
void DrawHLineScrollLayer(int layerID)
{
    uint32_t t0      = PS1Hblanks();
    g_ps1ProfLayer[4] = g_ps1ProfLayer[4] + 1;
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    int layerwidth   = layer->xsize;
    int layerheight  = layer->ysize;
    byte above       = layerID >= tLayerMidPoint;
    int *deformationData, *deformationDataW;

    int yscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG Layer
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
                hParallax.scrollPos[i] += hParallax.scrollSpeed[i];
                if (hParallax.scrollPos[i] > fullLayerwidth << 16)
                    hParallax.scrollPos[i] -= fullLayerwidth << 16;
                if (hParallax.scrollPos[i] < 0)
                    hParallax.scrollPos[i] += fullLayerwidth << 16;
                hParallax.linePos[i] += hParallax.scrollPos[i] >> 16;
                hParallax.linePos[i] %= fullLayerwidth;
            }
        }
        lastXSize = layerwidth;
    }
    // (end of the upstream state update)

    if (!layerwidth || !layerheight || !s_frameOpen)
        return;
    PS1TileSetup();
    int rows  = layerheight < TILELAYER_CHUNK_H ? layerheight : TILELAYER_CHUNK_H; // PS1-sized rows (Scene.hpp)
    int fullW = layerwidth << 7;
    int fullH = rows << 7;
    int tileYPos = yscrollOffset % (layerheight << 7);
    if (tileYPos < 0)
        tileYPos += layerheight << 7;
    tileYPos %= fullH;

    // The FG without deformation scrolls every line by xScrollOffset (upstream sets every
    // hParallax.linePos to it above): one band per tile row, no per-line pass (the Nexus fast path).
    bool uniform = !activeTileLayers[layerID];
    for (int i = 0; i < PARALLAX_COUNT && uniform; ++i) uniform = !hParallax.deform[i];
    int ly = tileYPos;
    if (uniform) {
        int chunkX = xScrollOffset % fullW;
        if (chunkX < 0)
            chunkX += fullW;
        for (int y = 0; y < SCREEN_YSIZE;) {
            int h = TILE_SIZE - (ly & 0xF);
            if (h > SCREEN_YSIZE - y)
                h = SCREEN_YSIZE - y;
            PS1DrawTileBand(layer, above, y, h, ly, chunkX, fullW);
            y += h;
            ly += h;
            if (ly >= fullH)
                ly -= fullH;
        }
        g_ps1LayerHbl     = g_ps1LayerHbl + PS1HblanksSince(t0);
        g_ps1ProfLayer[0] = g_ps1ProfLayer[0] + PS1HblanksSince(t0);
        g_ps1ProfSlot[layerID & 3] = PS1HblanksSince(t0);
        return;
    }
    for (int y = 0; y < SCREEN_YSIZE; ++y) {
        byte idx   = layer->lineScroll[ly];
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

    uint32_t t1             = PS1Hblanks();
    g_ps1ProfLayer[1]       = g_ps1ProfLayer[1] + PS1HblanksSince(t0);
    uint32_t tTiles         = 0;
    int stripBands          = 0;
    int layerIdx            = activeTileLayers[layerID];
    const uint8_t *stripRow = s_stripCount && layerIdx < LAYER_COUNT ? s_stripRow[layerIdx][above] : nullptr;
    ly                      = tileYPos;
    for (int y = 0; y < SCREEN_YSIZE;) {
        int chunkX = s_lineX[y];
        int ri     = stripRow ? stripRow[ly >> 4] : 0;
        int h      = 1;
        if (ri) { // strip region: a band runs until the X scroll changes or the region ends
            const PS1StripRegion &r = s_strips[ri - 1];
            int end                 = r.line0 + r.lines;
            while (y + h < SCREEN_YSIZE && ly + h < end && s_lineX[y + h] == chunkX) ++h;
            PS1StripBand &b = s_stripBands[stripBands++]; // drawn after the loop, piece by piece
            b.sy = (short)y, b.h = (short)h, b.ly = (short)ly, b.chunkX = (short)chunkX, b.region = (uint8_t)(ri - 1);
        }
        else {
            int rowLeft = TILE_SIZE - (ly & 0xF);
            while (h < rowLeft && y + h < SCREEN_YSIZE && s_lineX[y + h] == chunkX) ++h;
            uint32_t tb = PS1Hblanks();
            PS1DrawTileBand(layer, above, y, h, ly, chunkX, fullW);
            tTiles += PS1HblanksSince(tb);
        }
        y += h;
        ly += h;
        if (ly >= fullH)
            ly -= fullH;
    }
    g_ps1ProfLayer[2] = g_ps1ProfLayer[2] + tTiles;
    g_ps1StripBandCount = g_ps1StripBandCount + stripBands;
    uint32_t t2       = PS1Hblanks();
    static uint32_t s_stripClut = 0;
    if (!s_stripClut) {
        psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{ { .x = PS1_CLUT_X, .y = PS1_CLUT_Y } });
        uint16_t c;
        memcpy(&c, &clut, 2);
        s_stripClut = (uint32_t)c << 16;
    }
    for (int i = 0; i < stripBands;) { // strip bands grouped by region, chained piece by piece
        int region = s_stripBands[i].region, j = i;
        while (j < stripBands && s_stripBands[j].region == region) ++j;
        const PS1StripRegion &r        = s_strips[region];
        int count[PS1_STRIP_PIECES]    = {};
        int pos[PS1_STRIP_PIECES], total = 0;
        s_stripWalkCount = 0;
        for (int b = i; b < j; ++b)
            PS1WalkStripBand(r, s_stripBands[b].sy, s_stripBands[b].h, s_stripBands[b].ly, s_stripBands[b].chunkX, s_stripClut, count);
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
    g_ps1ProfLayer[3] = g_ps1ProfLayer[3] + PS1HblanksSince(t2);
    (void)t1;
    g_ps1LayerHbl = g_ps1LayerHbl + PS1HblanksSince(t0);
}

// upstream DrawVLineScrollLayer's scroll-state update (incl. its `scrollPos += scrollPos << 16`);
// drawing VScroll layers is step 3.5.
void DrawVLineScrollLayer(int layerID)
{
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    int layerwidth   = layer->xsize;
    int layerheight  = layer->ysize;
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
                vParallax.scrollPos[i] += vParallax.scrollSpeed[i];
                vParallax.scrollPos[i] += vParallax.scrollPos[i] << 16;
                if (vParallax.scrollPos[i] > fullLayerheight << 16)
                    vParallax.scrollPos[i] -= fullLayerheight << 16;
                vParallax.linePos[i] += vParallax.scrollPos[i] >> 16;
                vParallax.linePos[i] %= fullLayerheight;
            }
        }
        lastYSize = layerheight;
    }
    g_ps1DrawTodo = g_ps1DrawTodo + 1;
}

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

void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID)
{
    if (width <= 0 || height <= 0 || !s_frameOpen || PS1ObjectsHidden())
        return;
    PS1ForEachPiece(sheetID, width, height, sprX, sprY, [&](const PS1Piece &p) { PS1ChainPieceSprite(p, XPos + p.ox, YPos + p.oy); });
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
        DrawSprite(XPos - pivotX, YPos - pivotY, width, height, sprX, sprY, sheetID); // callers pass -frame pivot
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
    for (int i = 0; i < passes; ++i) DrawSprite(XPos, YPos, width, height, sprX, sprY, sheetID);
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
        DrawSprite(XPos, YPos, width, height, sprX, sprY, sheetID);
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

// upstream DrawObjectAnimation, verbatim: picks the draw call from the animation's rotation style.
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
                                  frame->height, 0x200 - ((532 - entity->rotation) >> 6 << 6), frame->sheetID);
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
// Pre-scaled glyphs (tools/font/prescale_font.py, user decision 2026-09-24): the font's table,
// loaded with it (LoadFontFile -> PS1LoadFontTable); entries per scale, drawn 1:1 from the atlas sheet.
struct PS1Glyph {
    uint16_t index, x, y, w, h;
    int16_t dx, dy;
};
#define PS1_GLYPH_SCALES 4
static PS1Glyph *s_glyphs = nullptr; // heap, exactly the table's size (Japanese Help: 1,436 entries, 20 KB)
static uint16_t s_glyphScale[PS1_GLYPH_SCALES], s_glyphFirst[PS1_GLYPH_SCALES], s_glyphCount[PS1_GLYPH_SCALES];
static int s_glyphScales = 0;
static char s_glyphSheet[64];
volatile uint32_t g_ps1GlyphTables = 0; // pre-scaled glyph tables loaded
volatile uint32_t g_ps1GlyphFallback = 0, g_ps1GlyphFallbackScale = 0, g_ps1GlyphFallbackChar = 0; // chars drawn scaled (no pre-scaled glyph)

void PS1LoadFontTable(const char *fontPath)
{
    s_glyphScales = 0;
    psyqo_free(s_glyphs);
    s_glyphs = nullptr;
    char path[96];
    int n = 0;
    while (fontPath[n] && n < 80) path[n] = fontPath[n], ++n;
    if (n > 4 && path[n - 4] == '.')
        n -= 4;
    memcpy(&path[n], "_ps1.bin", 9);
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char magic[4];
    uint16_t scales = 0;
    bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "PSF1", 4) && fread(&scales, 2, 1, f) == 1 && fread(s_glyphSheet, 1, 64, f) == 64;
    int total = 0;
    long body = ftell(f);
    for (int k = 0; ok && k < scales && k < PS1_GLYPH_SCALES; ++k) { // pass 1: sizes
        uint16_t hdr[2];
        ok = fread(hdr, 2, 2, f) == 2 && fseek(f, hdr[1] * (long)sizeof(PS1Glyph), SEEK_CUR) == 0;
        total += hdr[1];
    }
    ok = ok && total > 0 && (s_glyphs = (PS1Glyph *)psyqo_malloc(total * sizeof(PS1Glyph))) != nullptr && fseek(f, body, SEEK_SET) == 0;
    total = 0;
    for (int k = 0; ok && k < scales && k < PS1_GLYPH_SCALES; ++k) { // pass 2: entries
        uint16_t hdr[2];
        ok = fread(hdr, 2, 2, f) == 2;
        if (ok) {
            s_glyphScale[k] = hdr[0], s_glyphFirst[k] = (uint16_t)total, s_glyphCount[k] = hdr[1];
            ok = fread(&s_glyphs[total], sizeof(PS1Glyph), hdr[1], f) == hdr[1];
            total += hdr[1];
            s_glyphScales = k + 1;
        }
    }
    fclose(f);
    s_glyphSheet[63] = 0;
    if (!ok)
        s_glyphScales = 0;
    g_ps1GlyphTables = g_ps1GlyphTables + 1;
}

static const PS1Glyph *PS1FindGlyph(int scale, int index)
{
    for (int k = 0; k < s_glyphScales; ++k) {
        if (s_glyphScale[k] != scale)
            continue;
        int lo = s_glyphFirst[k], hi = lo + s_glyphCount[k] - 1; // sorted by font index (prescale_font)
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            if (s_glyphs[mid].index == index)
                return &s_glyphs[mid];
            if (s_glyphs[mid].index < index)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        return nullptr;
    }
    return nullptr;
}

// upstream DrawBitmapText (SW path): each character is a scaled sprite of the font's glyph rect on
// textMenuSurfaceNo; with a pre-scaled table for this scale the glyph is drawn 1:1 instead (same place).
void DrawBitmapText(void *menu, int XPos, int YPos, int scale, int spacing, int rowStart, int rowCount)
{
    TextMenu *tMenu = (TextMenu *)menu;
    int Y           = YPos << 9;
    if (rowCount < 0)
        rowCount = tMenu->rowCount;
    if (rowStart + rowCount > tMenu->rowCount)
        rowCount = tMenu->rowCount - rowStart;
    int psheet = -1;
    for (int k = 0; k < s_glyphScales; ++k)
        if (s_glyphScale[k] == scale && g_ps1AtlasSheetCount)
            psheet = PS1AtlasFindSheet(s_glyphSheet);
    while (rowCount > 0) {
        int X = XPos << 9;
        for (int i = 0; i < tMenu->entrySize[rowStart]; ++i) {
            ushort c             = tMenu->textData[tMenu->entryStart[rowStart] + i];
            FontCharacter *fChar = &fontCharacterList[c];
            const PS1Glyph *g    = psheet >= 0 ? PS1FindGlyph(scale, c) : nullptr;
            if (g) {
                if (g->w && g->h && s_frameOpen && !PS1ObjectsHidden()) { // 0 x 0: too small at this scale
                    int x = (X >> 9) + g->dx, y = (Y >> 9) + g->dy;
                    PS1ForEachPieceAtlas(psheet, -1, g->w, g->h, g->x, g->y, [&](const PS1Piece &p) { PS1ChainPieceSprite(p, x + p.ox, y + p.oy); });
                }
            }
            else {
                g_ps1GlyphFallback = g_ps1GlyphFallback + 1, g_ps1GlyphFallbackScale = scale, g_ps1GlyphFallbackChar = c;
                DrawSpriteScaled(FLIP_NONE, X >> 9, Y >> 9, -fChar->pivotX, -fChar->pivotY, scale, scale, fChar->width, fChar->height, fChar->srcX,
                                 fChar->srcY, textMenuSurfaceNo);
            }
            X += fChar->xAdvance * scale;
        }
        Y += spacing * scale;
        rowStart++;
        rowCount--;
    }
}
void DrawTextMenu(void *menu, int XPos, int YPos) { g_ps1DrawTodo = g_ps1DrawTodo + 1; }

// ---------------------------------------------------------------------------------------------------
// 3D floor (docs/28 phase 7): the special stages' floor, upstream Draw3DFloorLayer / Draw3DSkyLayer (the sky
// variant is the same floor at the PC's double resolution). Upstream walks the floor per screen row y =
// 128 + i (i = 4..111): the row sees the floor at forward distance f = K / i (K = YPos * 3 / 2048, px) and
// steps f / 192 floor px per screen pixel along (-cos, sin). So a floor point at forward f and lateral l
// (px, camera frame) is on screen at x = 160 + 192 l / f, y = 128 + K / f. Here:
//  - far rows (more than PS1_FLOOR_NEAR px ahead): one textured line each over the far-floor texture
//    (tools/floor/build_floor.py: 1 texel per 16x16 tile, tile page 3), its ends computed as upstream's
//    line walk and clipped to the floor; a row is affine in floor space, so GPU interpolation is exact;
//  - near rows: every visible 16x16 floor tile as one textured quad through the projection above
//    (vertices shared per grid row), clipped to those rows by the drawing area. PS1 textures are affine
//    per quad; at 16 px per tile the warp stays small.
#define PS1_FLOOR_NEAR      320 // px ahead drawn as tile quads (~350 at the special stages' camera height; farther rows: the far texture)
#define PS1_FLOOR_MAX_QUADS 900
#define PS1_FLOOR_GRID      120 // vertices per row buffer (2 rows x 120 x 4 bytes in the 1 KB scratchpad)
#define PS1_FLOOR_S         8   // GTE input scale (1/8 px): |dx| <= 4095 px fits a 16-bit vertex
#define PS1_FLOOR_SZMIN     97  // SZ below H / 2 (96) saturates the GTE divide: corner nearer than 12 px

volatile uint32_t g_ps1FloorQuads = 0; // tile quads of the last floor
volatile uint32_t g_ps1FloorLines = 0; // far textured lines of the last floor
volatile uint32_t g_ps1FloorHbl   = 0; // CPU time of the last floor (hblanks)
volatile int32_t  g_ps1FloorNearY = 0; // first screen row drawn with tile quads
volatile uint32_t g_ps1FloorCand  = 0; // tiles visited for the last floor

// Parameter interval [*t0, *t1] (screen x steps) where p0 + t * dp stays inside [0, max).
static void PS1FloorClipAxis(int p0, int dp, int max, int *t0, int *t1)
{
    if (dp == 0) {
        if (p0 < 0 || p0 >= max)
            *t0 = 1, *t1 = 0;
        return;
    }
    // t with p0 + t dp >= 0 and <= max - 1, rounded inward.
    int lo, hi;
    if (dp > 0) {
        lo = p0 >= 0 ? 0 : (-p0 + dp - 1) / dp;
        hi = (max - 1 - p0) >= 0 ? (max - 1 - p0) / dp : -1;
    } else {
        int n = -dp;
        lo = p0 <= max - 1 ? 0 : (p0 - (max - 1) + n - 1) / n;
        hi = p0 >= 0 ? p0 / n : -1;
    }
    if (lo > *t0)
        *t0 = lo;
    if (hi < *t1)
        *t1 = hi;
}

static void PS1Draw3DFloor(int layerID)
{
    uint32_t t0      = PS1Hblanks();
    TileLayer *layer = &stageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize || layer->YPos <= 0)
        return;
    PS1TileSetup();
    int a      = layer->angle & 0x1FF;
    int sn     = sinMLookupTable[a], cs = cosMLookupTable[a];
    int floorW = layer->xsize << 7, floorH = layer->ysize << 7; // px
    int camX   = layer->XPos >> 4, camZ = layer->ZPos >> 4; // 1/4096 px (upstream layerXPos, ZBuffer)
    int yPos   = layer->YPos;
    int K      = (int)((int64_t)yPos * 3 / 2048); // f = K / i (px)
    if (K <= 0)
        return;
    int iNear = K / PS1_FLOOR_NEAR;
    if (iNear < 4)
        iNear = 4;
    if (iNear > 112)
        iNear = 112;
    g_ps1FloorNearY = 128 + iNear;

    // Far rows: upstream's line walk, one textured line per row over the far-floor texture.
    int lines = 0;
    if (g_ps1FloorTexW && g_ps1FloorScale == 16) {
        uint32_t tpage = (uint32_t)(PS1_FLOOR_PAGE_X | (PS1_FLOOR_PAGE_Y << 4) | (1 << 7));
        uint32_t clut  = s_tileClutWord;
        uint32_t buf[16 * 9];
        int n = 0;
        PS1ChainDrawArea(0, 132, SCREEN_XSIZE, 128 + iNear);
        for (int i = 4; i < iNear; ++i) {
            int d  = yPos / (i << 9);
            int xb = d * -cs >> 8, zb = sn * d >> 8;
            int x0 = camX + (3 * sn * d >> 2) - xb * SCREEN_CENTERX;
            int z0 = camZ + (3 * cs * d >> 2) - zb * SCREEN_CENTERX;
            int tA = 0, tB = SCREEN_XSIZE - 1;
            PS1FloorClipAxis(x0, xb, floorW << 12, &tA, &tB);
            PS1FloorClipAxis(z0, zb, floorH << 12, &tA, &tB);
            if (tA > tB)
                continue;
            int uA = (x0 + tA * xb) >> 16, vA = (z0 + tA * zb) >> 16;
            int uB = (x0 + (tB + 1) * xb) >> 16, vB = (z0 + (tB + 1) * zb) >> 16;
            uA = uA < 0 ? 0 : uA > 255 ? 255 : uA, vA = vA < 0 ? 0 : vA > 255 ? 255 : vA;
            uB = uB < 0 ? 0 : uB > 255 ? 255 : uB, vB = vB < 0 ? 0 : vB > 255 ? 255 : vB;
            uint32_t y = 128 + i, xl = (uint32_t)tA, xr = (uint32_t)(tB + 1);
            uint32_t *q = &buf[n * 9];
            q[0] = 0x2C808080;
            q[1] = (y << 16) | xl;
            q[2] = clut | ((uint32_t)vA << 8) | (uint32_t)uA;
            q[3] = (y << 16) | xr;
            q[4] = (tpage << 16) | ((uint32_t)vB << 8) | (uint32_t)uB;
            q[5] = ((y + 1) << 16) | xl;
            q[6] = ((uint32_t)vA << 8) | (uint32_t)uA;
            q[7] = ((y + 1) << 16) | xr;
            q[8] = ((uint32_t)vB << 8) | (uint32_t)uB;
            ++lines;
            if (++n == 16) {
                PS1ChainPrimsRawN(buf, 9, n);
                n = 0;
            }
        }
        if (n)
            PS1ChainPrimsRawN(buf, 9, n);
    }
    g_ps1FloorLines = lines;

    // Near rows: projected tile quads over the floor region they see: the convex quadrilateral of the
    // world points under the screen corners (rows 128 + iNear and 240, x = 0 and 320). Per 16-px tile row,
    // only the tiles overlapping it are visited; their corners are projected by the GTE (RTPT) into two
    // vertex rows in the scratchpad, indexed from the region's first tile column.
    int px[4], pz[4];
    for (int k = 0; k < 2; ++k) {
        int i  = k ? 112 : iNear;
        int d  = yPos / (i << 9);
        int xb = d * -cs >> 8, zb = sn * d >> 8;
        int x0 = camX + (3 * sn * d >> 2) - xb * SCREEN_CENTERX;
        int z0 = camZ + (3 * cs * d >> 2) - zb * SCREEN_CENTERX;
        for (int e = 0; e < 2; ++e) { // corner order around the quadrilateral: near-left, near-right, bottom-right, bottom-left
            int c = k ? 3 - e : e;
            px[c] = (x0 + e * SCREEN_XSIZE * xb) >> 12, pz[c] = (z0 + e * SCREEN_XSIZE * zb) >> 12;
        }
    }
    int minX = px[0], maxX = px[0], minZ = pz[0], maxZ = pz[0];
    for (int c = 1; c < 4; ++c) {
        minX = px[c] < minX ? px[c] : minX, maxX = px[c] > maxX ? px[c] : maxX;
        minZ = pz[c] < minZ ? pz[c] : minZ, maxZ = pz[c] > maxZ ? pz[c] : maxZ;
    }
    int tilesW = floorW >> 4, tilesH = floorH >> 4;
    int tz0 = (minZ >> 4) - 1, tz1 = (maxZ >> 4) + 1;
    tz0 = tz0 < 0 ? 0 : tz0, tz1 = tz1 > tilesH - 1 ? tilesH - 1 : tz1;
    // x-range of the region inside world band [za, zb] (edge crossings + corners inside), in tiles
    auto rowRange = [&](int za, int zb, int &ta, int &tb) {
        int lo = 0x7FFFFFFF, hi = -0x7FFFFFFF;
        for (int c = 0; c < 4; ++c) {
            int n = (c + 1) & 3;
            if (pz[c] >= za && pz[c] <= zb)
                lo = px[c] < lo ? px[c] : lo, hi = px[c] > hi ? px[c] : hi;
            int zlo = pz[c] < pz[n] ? pz[c] : pz[n], zhi = pz[c] < pz[n] ? pz[n] : pz[c];
            if (zlo == zhi)
                continue;
            for (int zz = za; ; zz = zb) {
                if (zz >= zlo && zz <= zhi) {
                    // 32-bit: floor coordinates are px within the 4096-px floor (|products| < 2^25);
                    // a 64-bit divide is a slow libgcc call on the R3000A
                    int x = px[c] + (zz - pz[c]) * (px[n] - px[c]) / (pz[n] - pz[c]);
                    lo = x < lo ? x : lo, hi = x > hi ? x : hi;
                }
                if (zz == zb)
                    break;
            }
        }
        ta = (lo >> 4) - 1, tb = (hi >> 4) + 1;
        ta = ta < 0 ? 0 : ta, tb = tb > tilesW - 1 ? tilesW - 1 : tb;
    };
    int quads = 0, cand = 0;
    if (tz0 <= tz1 && minX < floorW && maxX >= 0) {
        int gx0 = (minX >> 4) - 2;
        gx0     = gx0 < 0 ? 0 : gx0;
        // Vertex rows in the scratchpad (1 KB, unused elsewhere): 2 x PS1_FLOOR_GRID x (int16 x, y); x = 0x7FFF
        // marks a corner too near for the GTE divide.
        uint32_t *rowA = reinterpret_cast<uint32_t *>(0x1F800000), *rowB = rowA + PS1_FLOOR_GRID;
        uint32_t *prev = rowA, *cur = rowB;
        using namespace psyqo::GTE;
        write<Register::R11R12, Unsafe>((uint32_t)(uint16_t)(int16_t)-cs);
        write<Register::R13R21, Unsafe>((uint32_t)(uint16_t)(int16_t)sn);
        clear<Register::R22R23, Unsafe>();
        write<Register::R31R32, Unsafe>((uint32_t)(uint16_t)(int16_t)sn);
        write<Register::R33, Unsafe>((uint32_t)(uint16_t)(int16_t)cs);
        clear<Register::TRX, Unsafe>();
        write<Register::TRY, Unsafe>((uint32_t)(K * PS1_FLOOR_S / 192));
        clear<Register::TRZ, Unsafe>();
        write<Register::OFX, Unsafe>((uint32_t)(SCREEN_CENTERX << 16));
        write<Register::OFY, Unsafe>((uint32_t)(128 << 16));
        write<Register::H, Safe>(192);
        int camXs = camX >> 9, camZs = camZ >> 9; // 1/8 px
        // GTE (psx-spx "GTE Coordinate Calculation Commands", RTPT): camera frame = rotation by the floor
        // angle, lateral l along (-cos, sin), forward f along (sin, cos); vertex (dx, 0, dz) * S, TRY = K S / 192,
        // H = 192, offsets (160, 128): SX = 160 + l * 192 / f, SY = 128 + (K / 192) * 192 / f = 128 + K / f.
        auto project = [&](uint32_t *row, int wz, int ta, int tb) {
            int dz = wz * PS1_FLOOR_S - camZs;
            dz     = dz < -32767 ? -32767 : dz > 32767 ? 32767 : dz;
            for (int tx = ta; tx <= tb + 1; tx += 3) {
                uint32_t xy[3];
                for (int j = 0; j < 3; ++j) {
                    int dx = ((tx + j) << 4) * PS1_FLOOR_S - camXs;
                    dx     = dx < -32767 ? -32767 : dx > 32767 ? 32767 : dx;
                    xy[j]  = (uint16_t)dx; // VY = 0
                }
                write<Register::VXY0, Unsafe>(xy[0]);
                write<Register::VZ0, Unsafe>((uint32_t)dz);
                write<Register::VXY1, Unsafe>(xy[1]);
                write<Register::VZ1, Unsafe>((uint32_t)dz);
                write<Register::VXY2, Unsafe>(xy[2]);
                write<Register::VZ2, Safe>((uint32_t)dz);
                Kernels::rtpt();
                uint32_t s0 = readRaw<Register::SXY0, Safe>(), s1 = readRaw<Register::SXY1, Safe>(), s2 = readRaw<Register::SXY2, Safe>();
                uint32_t z1 = readRaw<Register::SZ1, Safe>(), z2 = readRaw<Register::SZ2, Safe>(), z3 = readRaw<Register::SZ3, Safe>();
                uint32_t *v = &row[tx - gx0];
                v[0] = (z1 & 0xFFFF) < PS1_FLOOR_SZMIN ? 0x7FFF : s0;
                v[1] = (z2 & 0xFFFF) < PS1_FLOOR_SZMIN ? 0x7FFF : s1;
                v[2] = (z3 & 0xFFFF) < PS1_FLOOR_SZMIN ? 0x7FFF : s2;
            }
        };
        PS1ChainDrawArea(0, 128 + iNear, SCREEN_XSIZE, SCREEN_YSIZE);
        int yTop = 128 + iNear;
        int taPrev, tbPrev, ta, tb, taNext, tbNext;
        rowRange(tz0 << 4, (tz0 << 4) + 16, ta, tb);
        rowRange((tz0 << 4) - 16, tz0 << 4, taPrev, tbPrev);
        // top vertex row of the first tile row: covers this and the (empty) previous tile row
        project(prev, tz0 << 4, ta < taPrev ? ta : taPrev, tb > tbPrev ? tb : tbPrev);
        for (int tz = tz0; tz <= tz1 && quads < PS1_FLOOR_MAX_QUADS; ++tz) {
            rowRange((tz + 1) << 4, ((tz + 1) << 4) + 16, taNext, tbNext);
            int ua = ta < taNext ? ta : taNext, ub = tb > tbNext ? tb : tbNext;
            if (ub + 2 - gx0 > PS1_FLOOR_GRID)
                ub = gx0 + PS1_FLOOR_GRID - 2;
            project(cur, (tz + 1) << 4, ua, ub);
            const ushort *chunkRow = &layer->tiles[(tz >> 3) << 8];
            int te = tb + 1 - gx0 > PS1_FLOOR_GRID - 1 ? gx0 + PS1_FLOOR_GRID - 2 : tb;
            if (te - ta + 1 > PS1_FLOOR_MAX_QUADS - quads)
                te = ta + PS1_FLOOR_MAX_QUADS - quads - 1;
            // The row's quads go straight into the arena, one fragment each, linked once (PS1ChainBulkBegin).
            uint32_t *q = nullptr;
            if (te >= ta && !(q = PS1ChainBulkBegin(te - ta + 1, 9)))
                break; // the arena is full
            int rowQuads = 0;
            for (int tx = ta; tx <= te; ++tx) {
                ++cand;
                uint32_t A = prev[tx - gx0], B = prev[tx - gx0 + 1], C = cur[tx - gx0], D = cur[tx - gx0 + 1];
                if (A == 0x7FFF || B == 0x7FFF || C == 0x7FFF || D == 0x7FFF)
                    continue; // a corner under 12 px ahead: the whole tile is below the screen
                int ax = (int16_t)A, bx = (int16_t)B, cx = (int16_t)C, dx = (int16_t)D;
                int ay = (int32_t)A >> 16, by = (int32_t)B >> 16, cy = (int32_t)C >> 16, dy = (int32_t)D >> 16;
                int x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax, y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
                x0 = cx < x0 ? cx : x0, x1 = cx > x1 ? cx : x1, y0 = cy < y0 ? cy : y0, y1 = cy > y1 ? cy : y1;
                x0 = dx < x0 ? dx : x0, x1 = dx > x1 ? dx : x1, y0 = dy < y0 ? dy : y0, y1 = dy > y1 ? dy : y1;
                if (x1 < 0 || x0 >= SCREEN_XSIZE || y1 < yTop || y0 >= SCREEN_YSIZE || x1 - x0 > 1023 || y1 - y0 > 511)
                    continue;
                uint16_t tv = s_tileVis[(chunkRow[tx >> 3] << 6) + (tx & 7) + ((tz & 7) << 3)];
                if (!(tv & PS1_TV_SOLID))
                    continue;
                int t = tv & PS1_TV_INDEX, dir = (tv >> PS1_TV_DIR_SHIFT) & 3;
                int page = t >> 8, pt = t & 0xFF;
                int u = (pt & 0xF) << 4, v = (pt >> 4) << 4;
                int uL = u, uR = u + 15, vT = v, vB = v + 15;
                if (dir & FLIP_X)
                    uL = u + 15, uR = u;
                if (dir & FLIP_Y)
                    vT = v + 15, vB = v;
                uint32_t tpage = (uint32_t)((PS1_TILE_PAGE_X + 2 * (page & 1)) | ((PS1_TILE_PAGE_Y + (page >> 1)) << 4) | (1 << 7));
                q[0] = 0x2C808080;
                q[1] = A;
                q[2] = s_tileClutWord | ((uint32_t)vT << 8) | (uint32_t)uL;
                q[3] = B;
                q[4] = (tpage << 16) | ((uint32_t)vT << 8) | (uint32_t)uR;
                q[5] = C;
                q[6] = ((uint32_t)vB << 8) | (uint32_t)uL;
                q[7] = D;
                q[8] = ((uint32_t)vB << 8) | (uint32_t)uR;
                q += 10;
                ++rowQuads;
            }
            if (q)
                PS1ChainBulkEnd(rowQuads);
            quads += rowQuads;
            uint32_t *tmp = prev;
            prev          = cur;
            cur           = tmp;
            ta = taNext, tb = tbNext;
        }
    }
    g_ps1FloorCand = cand;
    PS1ChainDrawAreaReset();
    g_ps1FloorQuads = quads;
    g_ps1FloorHbl   = PS1HblanksSince(t0);
    g_ps1ProfFloorsHbl = g_ps1ProfFloorsHbl + g_ps1FloorHbl;
}

