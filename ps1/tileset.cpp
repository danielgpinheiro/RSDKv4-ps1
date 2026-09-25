/*
 * Stage tile set loader (see tileset.hh and tools/tiles/convert_tiles.py).
 */
#include "tileset.hh"
#include "gpu_chain.hh"

#include <stdio.h>

#include <psyqo/alloc.h>
#include <psyqo/gpu.hh>

#include "common/syscalls/syscalls.h"

extern psyqo::GPU *g_ps1GPU;

uint8_t g_ps1TileSolid[1024 / 8];
volatile uint32_t g_ps1TileSetLoads = 0;
volatile uint32_t g_ps1TileSetBytes = 0;

#define PAGES         4
#define PAGE_HW       128 // an 8-bit 256-texel page = 128 halfwords wide
#define STAGING_LINES 64  // 16 KB staging (heap, only during the load)

bool PS1TileSetLoad(const char *folder, uint8_t *pal768) {
    char path[96];
    snprintf(path, sizeof(path), "Data/Stages/%s/16x16Tiles.vram", folder);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ramsyscall_printf("[tiles] missing %s\n", path);
        return false;
    }
    uint8_t hdr[4];
    bool ok = fread(hdr, 1, 4, f) == 4 && hdr[0] == 1 && hdr[1] == 0 && hdr[2] == 4 && hdr[3] == 0; // BE 256 x 1024
    ok = ok && fread(pal768, 1, 768, f) == 768;
    uint16_t *staging = ok ? (uint16_t *)psyqo_malloc(STAGING_LINES * PAGE_HW * 2) : nullptr;
    uint32_t bytes    = 0;
    for (int i = 0; i < 1024 / 8; ++i) g_ps1TileSolid[i] = 0;
    if (staging) {
        PS1GpuImmediate(); // the previous frame's chain may still be texturing from VRAM
        for (int p = 0; ok && p < PAGES; ++p) {
            int16_t x = (int16_t)((PS1_TILE_PAGE_X + 2 * (p & 1)) * 64);
            int16_t y = (int16_t)((PS1_TILE_PAGE_Y + (p >> 1)) * 256);
            for (int line = 0; ok && line < 256; line += STAGING_LINES) {
                size_t sz = STAGING_LINES * PAGE_HW * 2;
                ok = fread(staging, 1, sz, f) == sz;
                if (ok) { // mark the tiles of these 4 tile rows that have a non-zero texel
                    const uint8_t *px = (const uint8_t *)staging;
                    for (int tr = 0; tr < STAGING_LINES / 16; ++tr)
                        for (int tc = 0; tc < 16; ++tc) {
                            int t      = p * 256 + ((line >> 4) + tr) * 16 + tc;
                            bool solid = false;
                            for (int r = 0; r < 16 && !solid; ++r) {
                                const uint32_t *w = (const uint32_t *)&px[(tr * 16 + r) * 256 + tc * 16];
                                solid             = (w[0] | w[1] | w[2] | w[3]) != 0;
                            }
                            if (solid)
                                g_ps1TileSolid[t >> 3] |= (uint8_t)(1 << (t & 7));
                        }
                }
                if (ok)
                    g_ps1GPU->uploadToVRAM(staging, psyqo::Rect{{x, (int16_t)(y + line)}, {PAGE_HW, STAGING_LINES}});
                bytes += sz;
            }
        }
        psyqo_free(staging);
    } else {
        ok = false;
    }
    fclose(f);
    if (!ok) {
        ramsyscall_printf("[tiles] bad or unreadable %s\n", path);
        return false;
    }
    g_ps1TileSetBytes = bytes;
    g_ps1TileSetLoads = g_ps1TileSetLoads + 1;
    return true;
}

int g_ps1FloorTexW = 0, g_ps1FloorTexH = 0, g_ps1FloorScale = 16;

bool PS1FloorLoad(const char *folder) {
    g_ps1FloorTexW = g_ps1FloorTexH = 0;
    char path[96];
    snprintf(path, sizeof(path), "Data/Stages/%s/Floor.vram", folder);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false; // not a special stage
    uint8_t hdr[12];
    bool ok = fread(hdr, 1, 12, f) == 12 && hdr[0] == 'F' && hdr[1] == 'L' && hdr[2] == 'R' && hdr[3] == '1';
    int w = hdr[4] | (hdr[5] << 8), h = hdr[6] | (hdr[7] << 8), scale = hdr[8] | (hdr[9] << 8);
    ok = ok && w > 0 && w <= 256 && !(w & 1) && h > 0 && h <= 256 && scale > 0;
    uint16_t *staging = ok ? (uint16_t *)psyqo_malloc(STAGING_LINES * PAGE_HW * 2) : nullptr;
    if (staging) {
        PS1GpuImmediate();
        for (int y = 0; ok && y < h; y += STAGING_LINES) {
            int n     = h - y < STAGING_LINES ? h - y : STAGING_LINES;
            size_t sz = (size_t)w * n;
            ok        = fread(staging, 1, sz, f) == sz;
            if (ok)
                g_ps1GPU->uploadToVRAM(staging, psyqo::Rect{{(int16_t)(PS1_FLOOR_PAGE_X * 64), (int16_t)(PS1_FLOOR_PAGE_Y * 256 + y)},
                                                            {(int16_t)(w / 2), (int16_t)n}});
        }
        psyqo_free(staging);
    } else {
        ok = false;
    }
    fclose(f);
    if (ok) {
        g_ps1FloorTexW  = w;
        g_ps1FloorTexH  = h;
        g_ps1FloorScale = scale;
    }
    return ok;
}
