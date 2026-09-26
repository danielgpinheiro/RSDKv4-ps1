/*
 * PS1 menus (see menu.hh): pack loading (phase 4.2).
 */
#include "menu.hh"
#include "gpu_chain.hh"

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
