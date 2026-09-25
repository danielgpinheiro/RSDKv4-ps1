/*
 * Per-stage sprite atlas loader (see sprite_atlas.hh and tools/atlas/build_atlas.py).
 */
#include "sprite_atlas.hh"
#include "gpu_chain.hh"
#include "loading.hh"

#include <stdio.h>
#include <string.h>

#include <psyqo/alloc.h>
#include <psyqo/gpu.hh>

#include "common/syscalls/syscalls.h"

extern psyqo::GPU *g_ps1GPU;

PS1AtlasSheet   g_ps1AtlasSheets[PS1_ATLAS_MAX_SHEETS];
PS1AtlasCluster g_ps1AtlasClusters[PS1_ATLAS_MAX_CLUSTERS];
PS1AtlasClut    g_ps1AtlasCluts[PS1_ATLAS_MAX_CLUTS];
int             g_ps1AtlasSheetCount = 0, g_ps1AtlasClusterCount = 0, g_ps1AtlasClutCount = 0;
uint16_t        g_ps1AtlasFmvX = 0xFFFF, g_ps1AtlasFmvY = 0xFFFF;

volatile uint32_t g_ps1AtlasLoads      = 0;
volatile uint32_t g_ps1AtlasBlockBytes = 0;
volatile uint32_t g_ps1AtlasMissing    = 0;

struct AtlasHeader {
    char magic[4];
    uint16_t sheets, clusters, cluts, blocks, fmvX, fmvY;
    uint32_t dataOffset;
};
struct AtlasBlock {
    uint16_t x, y, w, h;
    uint32_t offset;
};
static_assert(sizeof(AtlasHeader) == 20, "atlas header");
static_assert(sizeof(AtlasBlock) == 12, "atlas block record");

#define STAGING_BYTES (16 * 1024) // VRAM upload staging (heap, only during the load)
#define MAX_BLOCKS    32

// On-demand pictures (sprite_atlas.hh).
struct PicHeader {
    char magic[4];
    uint16_t slots, pictures, blocks, pad;
    uint32_t dataOffset;
};
struct PicSlot {
    uint16_t x, y, w, h;
    int16_t dTpageX, dTpageY;
};
struct PicEntry {
    uint16_t firstBlock, blockCount;
};
static_assert(sizeof(PicHeader) == 16, "pic header");
static_assert(sizeof(PicSlot) == 12, "pic slot record");

#define MAX_SLOTS      4
#define MAX_PICTURES   32
#define MAX_PIC_BLOCKS 64

volatile uint32_t g_ps1PictureLoads   = 0;
volatile uint32_t g_ps1PicturePending = 0;
volatile int32_t  g_ps1PictureSlot[MAX_SLOTS];

static char s_picPath[96];
static PicHeader s_pic;
static PicSlot s_slots[MAX_SLOTS];
static PicEntry s_pics[MAX_PICTURES];
static AtlasBlock s_picBlocks[MAX_PIC_BLOCKS];
static int8_t s_picSlot[MAX_PICTURES];            // slot holding each picture, -1 = not in VRAM
static int8_t s_picAt[MAX_PICTURES];              // slot whose position the picture's clusters hold now
static uint32_t s_slotUsed[MAX_SLOTS];            // frame stamp of the last draw from each slot
static int8_t s_queue[MAX_SLOTS];
static int s_queued = 0;
static uint32_t s_stamp = 1;

static void clearPictures() {
    s_pic.slots = s_pic.pictures = 0;
    s_queued = 0;
    for (int k = 0; k < MAX_SLOTS; ++k) g_ps1PictureSlot[k] = -1, s_slotUsed[k] = 0;
}

static void clearTables() {
    g_ps1AtlasSheetCount = g_ps1AtlasClusterCount = g_ps1AtlasClutCount = 0;
    g_ps1AtlasFmvX = g_ps1AtlasFmvY = 0xFFFF;
    clearPictures();
}

// Reads <atlas>.pic's tables (the pixels stay on the disc until a picture is needed).
static bool loadPictureTables(const char *atlasPath) {
    int n = snprintf(s_picPath, sizeof(s_picPath), "%s", atlasPath);
    memcpy(s_picPath + n - 4, ".pic", 4);
    FILE *f = fopen(s_picPath, "rb");
    if (!f)
        return false;
    bool ok = fread(&s_pic, sizeof(s_pic), 1, f) == 1 && !memcmp(s_pic.magic, "PIC1", 4) && s_pic.slots <= MAX_SLOTS &&
              s_pic.pictures <= MAX_PICTURES && s_pic.blocks <= MAX_PIC_BLOCKS;
    ok = ok && fread(s_slots, sizeof(PicSlot), s_pic.slots, f) == s_pic.slots;
    ok = ok && fread(s_pics, sizeof(PicEntry), s_pic.pictures, f) == s_pic.pictures;
    ok = ok && fread(s_picBlocks, sizeof(AtlasBlock), s_pic.blocks, f) == s_pic.blocks;
    fclose(f);
    if (!ok) {
        s_pic.slots = s_pic.pictures = 0;
        return false;
    }
    for (int p = 0; p < s_pic.pictures; ++p) s_picSlot[p] = -1, s_picAt[p] = 0;
    return true;
}

// Streams one block (VRAM halfword rect at x, y) from file offset `offset` through the staging buffer.
static bool uploadBlock(FILE *f, uint32_t offset, int x, int y, int w, int h, uint16_t *staging, uint32_t &bytes) {
    int linesPerChunk = STAGING_BYTES / (w * 2);
    bool ok           = fseek(f, (long)offset, SEEK_SET) == 0;
    for (int line = 0; ok && line < h; line += linesPerChunk) {
        int n     = h - line < linesPerChunk ? h - line : linesPerChunk;
        size_t sz = (size_t)w * n * 2;
        ok        = fread(staging, 1, sz, f) == sz;
        if (ok)
            g_ps1GPU->uploadToVRAM(staging, psyqo::Rect{{(int16_t)x, (int16_t)(y + line)}, {(int16_t)w, (int16_t)n}});
        bytes += sz;
    }
    return ok;
}

bool PS1AtlasLoad(const char *folder, int player) {
    clearTables();
    char path[96];
    // Per-player atlases (docs/28 3.2): <Stage>.atl = Sonic, <Stage>_t.atl = Tails.
    snprintf(path, sizeof(path), "Data/Sprites/Atlas/%s%s.atl", folder, player == 1 ? "_t" : "");
    FILE *f = fopen(path, "rb");
    if (!f) {
        ramsyscall_printf("[atlas] missing %s: no sprites in this stage\n", path);
        return false;
    }
    AtlasHeader h;
    AtlasBlock blocks[MAX_BLOCKS];
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "ATL1", 4) && h.sheets <= PS1_ATLAS_MAX_SHEETS &&
              h.clusters <= PS1_ATLAS_MAX_CLUSTERS && h.cluts <= PS1_ATLAS_MAX_CLUTS && h.blocks <= MAX_BLOCKS;
    ok = ok && fread(g_ps1AtlasSheets, sizeof(PS1AtlasSheet), h.sheets, f) == h.sheets;
    ok = ok && fread(g_ps1AtlasClusters, sizeof(PS1AtlasCluster), h.clusters, f) == h.clusters;
    ok = ok && fread(g_ps1AtlasCluts, sizeof(PS1AtlasClut), h.cluts, f) == h.cluts;
    ok = ok && fread(blocks, sizeof(AtlasBlock), h.blocks, f) == h.blocks;
    uint16_t *staging = ok ? (uint16_t *)psyqo_malloc(STAGING_BYTES) : nullptr;
    uint32_t bytes    = 0;
    if (staging) {
        PS1GpuImmediate(); // the previous frame's chain may still be texturing from VRAM
        for (int b = 0; ok && b < h.blocks; ++b) {
            const AtlasBlock &blk = blocks[b];
            ok = uploadBlock(f, h.dataOffset + blk.offset, blk.x, blk.y, blk.w, blk.h, staging, bytes);
        }
        psyqo_free(staging);
    } else {
        ok = false;
    }
    fclose(f);
    if (!ok) {
        ramsyscall_printf("[atlas] bad or unreadable %s\n", path);
        clearTables();
        return false;
    }
    g_ps1AtlasSheetCount   = h.sheets;
    g_ps1AtlasClusterCount = h.clusters;
    g_ps1AtlasClutCount    = h.cluts;
    g_ps1AtlasFmvX         = h.fmvX;
    g_ps1AtlasFmvY         = h.fmvY;
    g_ps1AtlasBlockBytes   = bytes;
    g_ps1AtlasLoads        = g_ps1AtlasLoads + 1;
    for (int i = 0; i < h.clusters; ++i) {
        if (g_ps1AtlasClusters[i].picture) { // the stage streams pictures: their tables now, pixels on demand
            if (!loadPictureTables(path))
                ramsyscall_printf("[atlas] missing or bad pictures file for %s\n", path);
            break;
        }
    }
    return true;
}

bool PS1AtlasPictureReady(const PS1AtlasCluster *c) {
    int p = c->picture - 1;
    if (p < 0)
        return true;
    if (p >= s_pic.pictures)
        return false;
    int slot = s_picSlot[p];
    if (slot >= 0) {
        s_slotUsed[slot] = s_stamp;
        return true;
    }
    g_ps1PicturePending = g_ps1PicturePending + 1;
    for (int i = 0; i < s_queued; ++i)
        if (s_queue[i] == p)
            return false;
    if (s_queued < s_pic.slots)
        s_queue[s_queued++] = (int8_t)p;
    return false;
}

// Moves picture p's clusters (tpage) from the slot they point at to `slot` (same shape, whole-page offset).
static void retarget(int p, int slot) {
    int dx = s_slots[slot].dTpageX - s_slots[s_picAt[p]].dTpageX;
    int dy = s_slots[slot].dTpageY - s_slots[s_picAt[p]].dTpageY;
    for (int i = 0; i < g_ps1AtlasClusterCount; ++i) {
        PS1AtlasCluster &c = g_ps1AtlasClusters[i];
        if (c.picture == p + 1) {
            c.tpageX = (uint8_t)(c.tpageX + dx);
            c.tpageY = (uint8_t)(c.tpageY + dy);
        }
    }
    s_picAt[p] = (int8_t)slot;
}

void PS1AtlasServicePictures() {
    uint32_t stamp = s_stamp++;
    if (!s_queued)
        return;
    int queued = s_queued;
    s_queued   = 0;
    PS1LoadingBegin(); // chain arenas -> CD windows, loading icon if it takes long (as a stage load)
    FILE *f           = fopen(s_picPath, "rb");
    uint16_t *staging = f ? (uint16_t *)psyqo_malloc(STAGING_BYTES) : nullptr;
    if (staging) {
        PS1GpuImmediate(); // the previous frame's chain may still be texturing from the slot
        for (int q = 0; q < queued; ++q) {
            int p = s_queue[q];
            // Slot: an empty one, else the least recently drawn from (never one loaded in this batch).
            int slot = -1;
            for (int k = 0; k < s_pic.slots && slot < 0; ++k)
                if (g_ps1PictureSlot[k] < 0)
                    slot = k;
            if (slot < 0) {
                for (int k = 0; k < s_pic.slots; ++k)
                    if (s_slotUsed[k] <= stamp && (slot < 0 || s_slotUsed[k] < s_slotUsed[slot]))
                        slot = k;
            }
            if (slot < 0)
                break;
            if (g_ps1PictureSlot[slot] >= 0)
                s_picSlot[g_ps1PictureSlot[slot]] = -1;
            const PicEntry &e = s_pics[p];
            const PicSlot &sl = s_slots[slot];
            uint32_t bytes    = 0;
            bool ok           = true;
            for (int b = e.firstBlock; ok && b < e.firstBlock + e.blockCount; ++b) {
                const AtlasBlock &blk = s_picBlocks[b];
                ok = uploadBlock(f, s_pic.dataOffset + blk.offset, blk.x + sl.dTpageX * 64, blk.y + sl.dTpageY * 256, blk.w,
                                 blk.h, staging, bytes);
            }
            g_ps1PictureSlot[slot] = ok ? p : -1;
            s_slotUsed[slot]       = stamp + 1; // counts as drawn this frame: not replaced by the next in the batch
            if (ok) {
                retarget(p, slot);
                s_picSlot[p]     = (int8_t)slot;
                g_ps1PictureLoads = g_ps1PictureLoads + 1;
            }
        }
        psyqo_free(staging);
    }
    if (f)
        fclose(f);
    PS1LoadingEnd();
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c; }

int PS1AtlasFindSheet(const char *path) {
    static const char prefix[] = "data/sprites/";
    const char *p = path;
    int i = 0;
    while (prefix[i] && lower(p[i]) == prefix[i]) ++i;
    if (!prefix[i])
        p += i; // strip "Data/Sprites/"
    for (int s = 0; s < g_ps1AtlasSheetCount; ++s) {
        const char *n = g_ps1AtlasSheets[s].name;
        int k = 0;
        while (n[k] && lower(p[k]) == n[k]) ++k;
        if (!n[k] && !p[k])
            return s;
    }
    g_ps1AtlasMissing = g_ps1AtlasMissing + 1;
    return -1;
}
