/*
 * PS1 DMA-chained renderer helpers — see gpu_chain.hh.
 */
#include "gpu_chain.hh"

#include <psyqo/alloc.h>

#include <string.h>

#include <new>

#include <psyqo/fragments.hh>
#include <psyqo/gpu.hh>
#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/misc.hh>

// Arena per frame parity. A frame's arena is being DMA'd while the next frame builds in the other one.
// Sonic CD (docs/28 3.6): 64 KB each, on the heap, time-shared with the CD read-ahead windows (user
// decision 2026-09-24): released at the start of a load (PS1ChainSuspend), allocated again at its end.
#define PS1_CHAIN_ARENA_WORDS (64 * 1024 / 4) // Tidal Tempest underwater frames (flipped-tile quads + region replay) reach ~12.3K words
// Every chained fragment is kept <= 14 words (psyqo GPU::c_chainThreshold / 4). psyqo links
// bigger fragments with bit 23 set so the linked-list DMA stops, then its DMA IRQ reads DMA2
// MADR to find and send the big node by hand. Real hardware leaves MADR at the failing link,
// but PCSX-Redux just treats bit 23 as end-of-list and leaves MADR at the chain start, so psyqo
// re-sends earlier nodes: primitives get drawn twice and out of order (verified: Sonic drawn
// over the front grass). Small fragments avoid that path on both.
#define PS1_CHAIN_MAX_WORDS   14
#define PS1_TILE_BATCH        4  // 4 * 3 words = 12 <= 14
#define PS1_SPRITE_BATCH      3  // 3 * 4 words = 12 <= 14
#define PS1_CLUT_CHUNK        22 // halfwords per CLUT upload chunk: 3 + 11 words = 14 // 84 * 3 words = 252 <= 255-word chained fragment limit

volatile uint32_t g_ps1ChainOverflow  = 0;
volatile uint32_t g_ps1ChainHighWater = 0;
volatile uint32_t g_ps1ChainLastWords = 0;
volatile uint32_t g_ps1ChainReplayWords = 0;

namespace {

typedef psyqo::Fragments::FixedFragment<psyqo::Prim::Sprite16x16, PS1_TILE_BATCH> TileBatch;
typedef psyqo::Fragments::FixedFragment<psyqo::Prim::Sprite, PS1_SPRITE_BATCH> SpriteBatch;

struct ClutChunkFragment {
    uintptr_t head;                 // psyqo chain link (must be uintptr_t)
    psyqo::Prim::VRAMUpload upload; // 3 words
    uint16_t data[PS1_CLUT_CHUNK];  // 11 words
    unsigned words;                 // payload size in words (not sent)
    size_t getActualFragmentSize() const { return words; }
};
static_assert(sizeof(psyqo::Prim::VRAMUpload) / 4 + PS1_CLUT_CHUNK / 2 <= PS1_CHAIN_MAX_WORDS, "CLUT chunk too big");
static_assert(PS1_TILE_BATCH * sizeof(psyqo::Prim::Sprite16x16) / 4 <= PS1_CHAIN_MAX_WORDS, "tile batch too big");
static_assert(PS1_SPRITE_BATCH * sizeof(psyqo::Prim::Sprite) / 4 <= PS1_CHAIN_MAX_WORDS, "sprite batch too big");

// A copied fragment (PS1ChainReplay): payload <= PS1_CHAIN_MAX_WORDS like every fragment here.
struct RawFragment {
    uintptr_t head;
    uint32_t data[PS1_CHAIN_MAX_WORDS];
    unsigned words;
    size_t getActualFragmentSize() const { return words; }
};

// A replayed fragment of exactly its size: the size field sits before `head` (psyqo's chain() only
// reads &head and getActualFragmentSize()), so a copy takes 2 + n arena words instead of 16.
struct VarFragment {
    unsigned words;
    uintptr_t head;
    uint32_t data[PS1_CHAIN_MAX_WORDS]; // only `words` of them are allocated
    size_t getActualFragmentSize() const { return words; }
};

uint32_t *s_arenaMem = nullptr; // 2 x s_arenaWords (heap), nullptr while suspended
uint32_t s_arenaWords = PS1_CHAIN_ARENA_WORDS; // per parity; smaller while an FMV plays (PS1ChainSetArenaWords)
void (*s_hook)() = nullptr;

inline void runHook() {
    if (s_hook) {
        auto h = s_hook;
        s_hook = nullptr;
        h();
    }
}
uint32_t *s_base    = nullptr;
uint32_t *s_cur     = nullptr;
uint32_t *s_end     = nullptr;
psyqo::GPU *s_gpu   = nullptr;
TileBatch *s_batch  = nullptr; // open tile batch (not yet chained)
SpriteBatch *s_sbatch = nullptr; // open sprite batch (not yet chained); at most one batch is open
uint16_t s_lastTPage = 0xFFFF;  // raw TPageAttr of the last chained TPage (0xFFFF = unknown)
bool s_tpageValid    = false;

VarFragment *allocVar(unsigned n) {
    if (!s_cur || s_cur + 2 + n > s_end) {
        g_ps1ChainOverflow = g_ps1ChainOverflow + 1;
        return nullptr;
    }
    auto *f = reinterpret_cast<VarFragment *>(s_cur);
    s_cur += 2 + n;
    f->words = n;
    return f;
}

template <typename T>
T *allocRaw() {
    size_t words = (sizeof(T) + 3) / 4;
    if (!s_cur || s_cur + words > s_end) {
        g_ps1ChainOverflow = g_ps1ChainOverflow + 1;
        return nullptr;
    }
    T *p = reinterpret_cast<T *>(s_cur);
    s_cur += words;
    return p;
}

void flushBatch() {
    if (s_batch) {
        if (s_batch->count)
            s_gpu->chain(*s_batch);
        s_batch = nullptr;
    }
    if (s_sbatch) {
        if (s_sbatch->count)
            s_gpu->chain(*s_sbatch);
        s_sbatch = nullptr;
    }
}

template <typename Prim>
void chainSimple(const Prim &prim) {
    runHook();
    flushBatch();
    auto *frag = allocRaw<psyqo::Fragments::SimpleFragment<Prim>>();
    if (!frag)
        return;
    new (frag) psyqo::Fragments::SimpleFragment<Prim>(prim);
    s_gpu->chain(*frag);
}

} // namespace

volatile uint32_t g_ps1ChainArenaFail = 0; // frames without an arena (heap full): nothing drawn
volatile uint32_t g_ps1ChainSuspends  = 0; // arenas released for a load
static bool s_suspended = false;

void PS1ChainBegin(psyqo::GPU &gpu) {
    s_gpu = &gpu;
    if (!s_arenaMem && !s_suspended)
        s_arenaMem = (uint32_t *)psyqo_malloc(2 * s_arenaWords * 4);
    if (!s_arenaMem) {
        g_ps1ChainArenaFail = g_ps1ChainArenaFail + 1;
        s_base = s_cur = s_end = nullptr;
        s_batch  = nullptr;
        s_sbatch = nullptr;
        return;
    }
    s_base       = s_arenaMem + (gpu.getParity() ? s_arenaWords : 0);
    s_cur        = s_base;
    s_end        = s_base + s_arenaWords;
    s_batch      = nullptr;
    s_sbatch     = nullptr;
    s_tpageValid = false;
    s_hook       = nullptr;
    g_ps1ChainReplayWords = 0;
}

void PS1ChainEnd() {
    flushBatch();
    uint32_t used = (uint32_t)(s_cur - s_base);
    g_ps1ChainLastWords = used;
    if (used > g_ps1ChainHighWater)
        g_ps1ChainHighWater = used;
}

void PS1ChainSuspend() {
    if (!s_arenaMem || s_suspended)
        return;
    flushBatch();
    if (s_cur != s_base) // this frame already chained something psyqo points at: keep the arenas
        return;
    PS1GpuImmediate(); // the previous frame's chain may still be DMA'd from the other arena
    psyqo_free(s_arenaMem);
    s_arenaMem  = nullptr;
    s_base = s_cur = s_end = nullptr;
    s_suspended = true;
    g_ps1ChainSuspends = g_ps1ChainSuspends + 1;
}

void PS1ChainSetArenaWords(uint32_t words) { s_arenaWords = words ? words : PS1_CHAIN_ARENA_WORDS; }

void PS1ChainResume() {
    if (!s_suspended)
        return;
    s_suspended = false;
    if (s_gpu)
        PS1ChainBegin(*s_gpu); // the current frame's chain starts over in the new arena
}

void PS1GpuImmediate() {
    if (s_gpu)
        s_gpu->waitChainIdle();
}

psyqo::Prim::FastFill *PS1ChainClear(uint8_t r, uint8_t g, uint8_t b) {
    flushBatch();
    auto *frag = allocRaw<psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill>>();
    if (!frag)
        return nullptr;
    new (frag) psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill>();
    s_gpu->getNextClear(frag->primitive, psyqo::Color{{.r = r, .g = g, .b = b}});
    s_gpu->chain(*frag);
    return &frag->primitive;
}

void PS1ChainUploadRow(const uint16_t *data, int count, int16_t x, int16_t y) {
    runHook();
    flushBatch();
    for (int off = 0; off < count; off += PS1_CLUT_CHUNK) {
        int n    = (count - off < PS1_CLUT_CHUNK) ? (count - off) : PS1_CLUT_CHUNK; // 256 = 22 x 11 + 14
        auto *fr = allocRaw<ClutChunkFragment>();
        if (!fr)
            return;
        new (&fr->upload) psyqo::Prim::VRAMUpload();
        fr->upload.region = psyqo::Rect{{(int16_t)(x + off), y}, {(int16_t)n, 1}};
        memcpy(fr->data, data + off, n * sizeof(uint16_t));
        fr->words = (unsigned)(sizeof(psyqo::Prim::VRAMUpload) / 4 + n / 2); // n is even
        s_gpu->chain(*fr);
    }
}

int PS1ChainReserveRow(int count, int16_t x, int16_t y, uint16_t **chunkData, uint16_t *chunkLen, int maxChunks) {
    runHook();
    flushBatch();
    int chunks = 0;
    for (int off = 0; off < count && chunks < maxChunks; off += PS1_CLUT_CHUNK) {
        int n    = (count - off < PS1_CLUT_CHUNK) ? (count - off) : PS1_CLUT_CHUNK;
        auto *fr = allocRaw<ClutChunkFragment>();
        if (!fr)
            return 0;
        new (&fr->upload) psyqo::Prim::VRAMUpload();
        fr->upload.region = psyqo::Rect{{(int16_t)(x + off), y}, {(int16_t)n, 1}};
        fr->words         = (unsigned)(sizeof(psyqo::Prim::VRAMUpload) / 4 + n / 2); // n is even
        s_gpu->chain(*fr);
        chunkData[chunks] = fr->data;
        chunkLen[chunks]  = (uint16_t)n;
        ++chunks;
    }
    return chunks;
}

namespace {
// GP0(80h) VRAM-to-VRAM rectangle copy (no psyqo primitive for it).
struct VRAMCopy {
    uint32_t command = 0x80000000;
    psyqo::Vertex src, dst, size;
};
static_assert(sizeof(VRAMCopy) == 16, "VRAMCopy is 4 words");
} // namespace

void PS1ChainRepeatLastFrame() {
    psyqo::Prim::FastFill next; // getNextClear() tells which buffer this frame's chain draws into
    s_gpu->getNextClear(next, psyqo::Color{{.r = 0, .g = 0, .b = 0}});
    VRAMCopy c;
    c.dst  = next.rect.pos;
    c.src  = psyqo::Vertex{{.x = next.rect.pos.x, .y = (int16_t)(next.rect.pos.y ? 0 : 256)}};
    c.size = next.rect.size;
    chainSimple(c);
}

void PS1ChainFlushCache() {
    flushBatch();
    chainSimple(psyqo::Prim::FlushCache());
}

void PS1ChainUploadCLUT(const uint16_t *clut256, int16_t x, int16_t y) {
    PS1ChainUploadRow(clut256, 256, x, y);
    // The GPU caches the CLUT: flush after changing it at the same VRAM location.
    PS1ChainFlushCache();
}

void PS1ChainTPage(psyqo::PrimPieces::TPageAttr attr) {
    runHook();
    uint16_t raw;
    static_assert(sizeof(attr) == sizeof(raw), "TPageAttr is 16 bits");
    memcpy(&raw, &attr, sizeof(raw));
    if (s_tpageValid && raw == s_lastTPage)
        return;
    psyqo::Prim::TPage tp;
    tp.attr = attr;
    chainSimple(tp);
    s_lastTPage  = raw;
    s_tpageValid = true;
}

void PS1ChainInvalidateTPage() { s_tpageValid = false; }

bool g_ps1ChainSkipPrims = false;

void PS1ChainSprite16(const psyqo::Prim::Sprite16x16 &s) {
    if (g_ps1ChainSkipPrims)
        return;
    runHook();
    if (!s_batch || s_batch->count >= PS1_TILE_BATCH) {
        flushBatch(); // also closes an open sprite batch (keeps the draw order)
        s_batch = allocRaw<TileBatch>();
        if (!s_batch)
            return;
        new (s_batch) TileBatch();
        s_batch->count = 0;
    }
    s_batch->primitives[s_batch->count++] = s;
}

void PS1ChainSprite16Raw(uint32_t xy, uint32_t tex) {
    static const uint32_t cmd = [] {
        psyqo::Prim::Sprite16x16 s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
        uint32_t w;
        memcpy(&w, &s, 4);
        return w;
    }();
    if (g_ps1ChainSkipPrims)
        return;
    runHook();
    if (!s_batch || s_batch->count >= PS1_TILE_BATCH) {
        flushBatch();
        s_batch = allocRaw<TileBatch>();
        if (!s_batch)
            return;
        new (s_batch) TileBatch();
        s_batch->count = 0;
    }
    uint32_t *w = reinterpret_cast<uint32_t *>(&s_batch->primitives[s_batch->count++]);
    w[0]        = cmd;
    w[1]        = xy;
    w[2]        = tex;
}

void PS1ChainSprite16RawN(const uint32_t *xy, const uint32_t *tex, int n) {
    static uint32_t cmd = 0;
    if (!cmd) {
        psyqo::Prim::Sprite16x16 s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
        memcpy(&cmd, &s, 4);
    }
    if (g_ps1ChainSkipPrims || n <= 0)
        return;
    runHook();
    flushBatch();
    while (n > 0) {
        int k = n < PS1_TILE_BATCH ? n : PS1_TILE_BATCH;
        VarFragment *f = allocVar(3 * k);
        if (!f)
            return;
        uint32_t *w = f->data;
        for (int i = 0; i < k; ++i) {
            w[0] = cmd;
            w[1] = *xy++;
            w[2] = *tex++;
            w += 3;
        }
        s_gpu->chain(*f);
        n -= k;
    }
}

void PS1ChainSpriteRawN(const uint32_t *words, int n) {
    static uint32_t cmd = 0;
    if (!cmd) {
        psyqo::Prim::Sprite s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
        memcpy(&cmd, &s, 4);
    }
    if (g_ps1ChainSkipPrims || n <= 0)
        return;
    runHook();
    flushBatch();
    while (n > 0) {
        int k = n < PS1_SPRITE_BATCH ? n : PS1_SPRITE_BATCH;
        VarFragment *f = allocVar(4 * k);
        if (!f)
            return;
        uint32_t *w = f->data;
        for (int i = 0; i < k; ++i) {
            w[0] = cmd;
            w[1] = words[0];
            w[2] = words[1];
            w[3] = words[2];
            w += 4;
            words += 3;
        }
        s_gpu->chain(*f);
        n -= k;
    }
}

static VarFragment *s_rawFrag = nullptr;
uint32_t *PS1ChainRawBegin(int words) {
    if (g_ps1ChainSkipPrims || words <= 0 || words > PS1_CHAIN_MAX_WORDS)
        return nullptr;
    runHook();
    flushBatch();
    s_rawFrag = allocVar(words);
    return s_rawFrag ? s_rawFrag->data : nullptr;
}

void PS1ChainRawEnd() {
    if (s_rawFrag)
        s_gpu->chain(*s_rawFrag);
    s_rawFrag    = nullptr;
    s_tpageValid = false;
}

static uint32_t *s_bulk = nullptr;
static int s_bulkWpp = 0;
// A bulk primitive as psyqo's chain(first, last) sees it: the DMA header word, then the words.
struct BulkFrag {
    uintptr_t head;
    size_t getActualFragmentSize() const { return (size_t)s_bulkWpp; }
};

uint32_t *PS1ChainBulkBegin(int n, int wpp) {
    s_bulk = nullptr;
    if (g_ps1ChainSkipPrims || n <= 0 || wpp <= 0 || wpp > PS1_CHAIN_MAX_WORDS)
        return nullptr;
    runHook();
    flushBatch();
    if (!s_cur || s_cur + n * (wpp + 1) > s_end) {
        g_ps1ChainOverflow = g_ps1ChainOverflow + 1;
        return nullptr;
    }
    s_bulk    = s_cur;
    s_bulkWpp = wpp;
    return s_cur + 1;
}

void PS1ChainBulkEnd(int used) {
    if (!s_bulk || used <= 0) {
        s_bulk = nullptr;
        return;
    }
    uint32_t stride = (uint32_t)s_bulkWpp + 1, size = (uint32_t)s_bulkWpp << 24;
    uint32_t *h = s_bulk;
    for (int k = 0; k < used - 1; ++k, h += stride) // DMA linked-list headers: size << 24 | next packet address
        *h = size | (reinterpret_cast<uintptr_t>(h + stride) & 0xffffff);
    s_gpu->chain(reinterpret_cast<BulkFrag *>(s_bulk), reinterpret_cast<BulkFrag *>(h));
    s_cur        = s_bulk + used * stride;
    s_bulk       = nullptr;
    s_tpageValid = false;
}

void PS1ChainPrimsRawN(const uint32_t *words, int wpp, int n) {
    if (g_ps1ChainSkipPrims || n <= 0 || wpp <= 0 || wpp > PS1_CHAIN_MAX_WORDS)
        return;
    runHook();
    flushBatch();
    int per = PS1_CHAIN_MAX_WORDS / wpp;
    while (n > 0) {
        int k = n < per ? n : per;
        VarFragment *f = allocVar(wpp * k);
        if (!f)
            return;
        memcpy(f->data, words, wpp * k * 4);
        s_gpu->chain(*f);
        words += wpp * k;
        n -= k;
    }
    s_tpageValid = false;
}

void PS1ChainSprite(const psyqo::Prim::Sprite &s) {
    if (g_ps1ChainSkipPrims)
        return;
    runHook();
    if (!s_sbatch || s_sbatch->count >= PS1_SPRITE_BATCH) {
        flushBatch();
        s_sbatch = allocRaw<SpriteBatch>();
        if (!s_sbatch)
            return;
        new (s_sbatch) SpriteBatch();
        s_sbatch->count = 0;
    }
    s_sbatch->primitives[s_sbatch->count++] = s;
}

void PS1ChainRect(const psyqo::Prim::Rectangle &r) {
    if (!g_ps1ChainSkipPrims)
        chainSimple(r);
}

void PS1ChainQuad(const psyqo::Prim::TexturedQuad &q) {
    if (g_ps1ChainSkipPrims)
        return;
    chainSimple(q);
    s_tpageValid = false; // a textured polygon's texpage attribute changes the GPU draw mode
}

uint32_t *PS1ChainMark() {
    flushBatch();
    return s_cur;
}

int PS1ChainReplay(uint32_t *from, uint32_t *to) {
    flushBatch();
    int words = 0;
    uint32_t *p = from;
    for (int guard = 0; p && p != to && p >= s_base && p < s_end && guard < 4096; ++guard) {
        uint32_t head = *p;
        unsigned n    = head >> 24;
        uint32_t *next = reinterpret_cast<uint32_t *>(0x80000000u | (head & 0xffffff));
        if (n && n <= PS1_CHAIN_MAX_WORDS && (p[1] >> 24) != 0xA0) { // skip CPU->VRAM uploads (CLUT rows)
            auto *f = allocVar(n);
            if (!f)
                break;
            memcpy(f->data, p + 1, n * 4);
            s_gpu->chain(*f);
            words += n + 1;
        }
        p = next;
    }
    g_ps1ChainReplayWords = g_ps1ChainReplayWords + words;
    s_tpageValid = false;
    return words;
}

// Screen Y extent of the GP0 packets in n words (psx-spx "GPU Render Polygon / Rectangle Commands").
// Returns false when the words hold no drawing packet (state commands only): always replayed.
static bool packetsYRange(const uint32_t *w, unsigned n, int &ymin, int &ymax) {
    bool drawn = false;
    ymin = 0x7FFF, ymax = -0x8000;
    auto yOf = [](uint32_t xy) { return (int)(int16_t)(xy >> 16); };
    unsigned i = 0;
    while (i < n) {
        uint32_t cmd = w[i] >> 24;
        if (cmd >= 0x20 && cmd < 0x40) { // polygon: 3/4 vertices, [color], xy, [uv]
            bool quad = cmd & 0x08, tex = cmd & 0x04, gouraud = cmd & 0x10;
            unsigned verts = quad ? 4 : 3, k = i + 1;
            for (unsigned v = 0; v < verts && k < n; ++v) {
                if (v && gouraud)
                    ++k; // vertex colour
                int y = yOf(w[k++]);
                ymin = y < ymin ? y : ymin;
                ymax = y > ymax ? y : ymax;
                if (tex)
                    ++k;
            }
            drawn = true;
            i = k;
        } else if (cmd >= 0x60 && cmd < 0x80) { // rectangle: color, xy, [uv], [wh]
            bool tex = cmd & 0x04;
            unsigned size = (cmd >> 3) & 3, k = i + 1;
            int y = yOf(w[k++]);
            if (tex)
                ++k;
            int h = size == 0 ? (k < n ? (int)(w[k++] >> 16) : 0) : size == 1 ? 1 : size == 2 ? 8 : 16;
            ymin = y < ymin ? y : ymin;
            ymax = y + h - 1 > ymax ? y + h - 1 : ymax;
            drawn = true;
            i = k;
        } else {
            return drawn ? true : false; // state / other packets end the scan (kept)
        }
    }
    return drawn;
}

int PS1ChainReplayLines(uint32_t *from, uint32_t *to, int y0, int y1) {
    flushBatch();
    int words = 0;
    uint32_t *p = from;
    for (int guard = 0; p && p != to && p >= s_base && p < s_end && guard < 4096; ++guard) {
        uint32_t head = *p;
        unsigned n    = head >> 24;
        uint32_t *next = reinterpret_cast<uint32_t *>(0x80000000u | (head & 0xffffff));
        int ymin, ymax;
        bool keep = n && n <= PS1_CHAIN_MAX_WORDS && (p[1] >> 24) != 0xA0 // not a CPU->VRAM upload (CLUT rows)
                    && (!packetsYRange(p + 1, n, ymin, ymax) || (ymax >= y0 && ymin < y1));
        if (keep) {
            auto *f = allocVar(n);
            if (!f)
                break;
            memcpy(f->data, p + 1, n * 4);
            s_gpu->chain(*f);
            words += n + 1;
        }
        p = next;
    }
    g_ps1ChainReplayWords = g_ps1ChainReplayWords + words;
    s_tpageValid = false;
    return words;
}

static int16_t chainBufferY() { // the buffer this frame's chain draws into (psyqo getNextScissor)
    return s_gpu->getParity() ? 0 : 256;
}

int PS1ChainBufferY() { return s_gpu ? chainBufferY() : 0; }

void PS1ChainDrawArea(int x0, int y0, int x1, int y1) {
    int16_t by = chainBufferY();
    chainSimple(psyqo::Prim::DrawingAreaStart(psyqo::Vertex{{.x = (int16_t)x0, .y = (int16_t)(by + y0)}}));
    chainSimple(psyqo::Prim::DrawingAreaEnd(psyqo::Vertex{{.x = (int16_t)x1, .y = (int16_t)(by + y1)}}));
}

void PS1ChainDrawAreaReset() { PS1ChainDrawArea(0, 0, 320, 240); }

void PS1ChainFillLines(int y0, int y1, uint8_t r, uint8_t g, uint8_t b) {
    psyqo::Prim::FastFill f(psyqo::Color{{.r = r, .g = g, .b = b}});
    f.rect = psyqo::Rect{psyqo::Vertex{{.x = 0, .y = (int16_t)(chainBufferY() + y0)}}, psyqo::Vertex{{.w = 320, .h = (int16_t)(y1 - y0)}}};
    chainSimple(f);
}

void PS1ChainVramCopy(int sx, int sy, int dx, int dy, int w, int h) {
    VRAMCopy c;
    c.src  = psyqo::Vertex{{.x = (int16_t)sx, .y = (int16_t)sy}};
    c.dst  = psyqo::Vertex{{.x = (int16_t)dx, .y = (int16_t)dy}};
    c.size = psyqo::Vertex{{.w = (int16_t)w, .h = (int16_t)h}};
    chainSimple(c);
}

void PS1ChainCopyLastFrameLines(int y0, int y1) {
    int16_t by = chainBufferY();
    VRAMCopy c;
    c.dst  = psyqo::Vertex{{.x = 0, .y = (int16_t)(by + y0)}};
    c.src  = psyqo::Vertex{{.x = 0, .y = (int16_t)((by ? 0 : 256) + y0)}};
    c.size = psyqo::Vertex{{.w = 320, .h = (int16_t)(y1 - y0)}};
    chainSimple(c);
}

void PS1ChainMaskControl(bool forceSet, bool test) {
    chainSimple(psyqo::Prim::MaskControl(forceSet ? psyqo::Prim::MaskControl::ForceSet : psyqo::Prim::MaskControl::FromSource,
                                         test ? psyqo::Prim::MaskControl::Test::Yes : psyqo::Prim::MaskControl::Test::No));
}

void PS1ChainSetHook(void (*fn)()) { s_hook = fn; }
void PS1ChainRunHook() { runHook(); }
