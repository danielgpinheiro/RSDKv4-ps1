/*
 * Phase 0 smoke test (docs/28): proves the PS1 platform layer copied from RSDKv2-ps1 works in this
 * repo before the RSDKv3 engine is ported (phase 2 replaces this app with RSDKv3/main.cpp).
 *
 * - GPU: 320x240, a test pattern drawn through the DMA chain (ps1/gpu_chain) every frame.
 * - CD: every test file read through ps1/cdrom_fs (read-ahead windows) inside one bracketed load,
 *   so the loading icon (ps1/loading) shows; CRC-32 of each file in g_ps1TestCrc[].
 * - SPU: Data/SoundFX/Global/Jump.vag (built by tools/smoke_disc.sh) uploaded, played, read back.
 * - Pad: psyqo::AdvancedPad buttons in g_ps1TestPad.
 * Everything is checked from GDB (tools/smoke_test.sh).
 */
#include <psyqo/advancedpad.hh>
#include <psyqo/application.hh>
#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/scene.hh>
#include <stdio.h>
#include <string.h>

#include "gpu_chain.hh"
#include "loading.hh"
#include "spu.hh"

psyqo::GPU *g_ps1GPU            = nullptr;
psyqo::CDRomDevice *g_ps1CDRom  = nullptr;
volatile uint32_t g_ps1PerfFrames = 0; // frames run (loading.cpp, xa_music.cpp)
volatile uint32_t g_ps1PerfVsyncs = 0;

#define TEST_FILES 6
static const char *const s_files[TEST_FILES] = {
    "Data/Game/GameConfig.bin",           "Data/Stages/Zone01/StageConfig.bin",   "Data/Stages/Zone01/Act1.bin",
    "Data/Stages/Zone01/128x128Tiles.bin",  "Data/Stages/Zone01/CollisionMasks.bin", "Data/Stages/Zone01/Backgrounds.bin",
};
volatile uint32_t g_ps1TestCrc[TEST_FILES];  // CRC-32 of each file as read from the disc
volatile uint32_t g_ps1TestSize[TEST_FILES]; // bytes read
volatile uint32_t g_ps1TestFilesOk  = 0;     // files opened and read completely
volatile uint32_t g_ps1TestSpuOk    = 0;     // 1 = SPU RAM read back == the .vag payload
volatile uint32_t g_ps1TestSpuBytes = 0;
volatile uint32_t g_ps1TestPad      = 0;     // AdvancedPad pad 1 buttons (bit = psyqo Button)
volatile uint32_t g_ps1TestDone     = 0;     // start() finished

static uint32_t crc32(const uint8_t *p, uint32_t n, uint32_t c = 0) { // zlib CRC-32
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static uint8_t s_buf[64 * 1024];

namespace {

class PlatformTest final : public psyqo::Application {
    void prepare() override {
        gpu().initialize(psyqo::GPU::Configuration{}
                             .set(psyqo::GPU::Resolution::W320)
                             .set(psyqo::GPU::VideoMode::AUTO)
                             .set(psyqo::GPU::ColorMode::C15BITS)
                             .set(psyqo::GPU::Interlace::PROGRESSIVE)
                             .set(psyqo::GPU::MiscSetting::CLEAR_VRAM));
        g_ps1GPU = &gpu();
        m_cdrom.prepare();
    }

    void start() override {
        m_cdrom.resetBlocking(gpu());
        g_ps1CDRom = &m_cdrom;
        m_pad.initialize(psyqo::AdvancedPad::PollingMode::Fast);

        PS1LoadingBegin(); // one load: every test file, through the read-ahead windows
        for (int i = 0; i < TEST_FILES; ++i) {
            FILE *f = fopen(s_files[i], "rb");
            if (!f)
                continue;
            uint32_t crc = 0, total = 0;
            size_t n;
            while ((n = fread(s_buf, 1, sizeof(s_buf), f)) > 0) {
                crc = crc32(s_buf, (uint32_t)n, crc);
                total += (uint32_t)n;
            }
            fclose(f);
            g_ps1TestCrc[i]  = crc;
            g_ps1TestSize[i] = total;
            g_ps1TestFilesOk = g_ps1TestFilesOk + 1;
        }
        PS1LoadingEnd();

        // SPU: a .vag = 48-byte header (big-endian size at 0x0C, rate at 0x10) + SPU-ADPCM data.
        PS1SpuInit();
        if (FILE *f = fopen("Data/SoundFX/Global/Jump.vag", "rb")) {
            size_t n = fread(s_buf, 1, sizeof(s_buf), f);
            fclose(f);
            if (n > 48) {
                uint32_t size = (uint32_t)s_buf[12] << 24 | s_buf[13] << 16 | s_buf[14] << 8 | s_buf[15];
                uint32_t rate = (uint32_t)s_buf[16] << 24 | s_buf[17] << 16 | s_buf[18] << 8 | s_buf[19];
                uint32_t len  = (size + 63) & ~63u; // DMA in 64-byte blocks
                if (48 + len <= sizeof(s_buf)) {
                    memset(s_buf + 48 + size, 0, len - size);
                    PS1SpuWrite(0x1000, s_buf + 48, len);
                    static uint8_t back[32 * 1024];
                    uint32_t cmp = len < sizeof(back) ? len : sizeof(back);
                    PS1SpuRead(0x1000, back, cmp);
                    g_ps1TestSpuOk    = memcmp(back, s_buf + 48, cmp) == 0;
                    g_ps1TestSpuBytes = cmp;
                    PS1SpuVoicePlay(1, 0x1000, rate, 0x3FFF, 0x3FFF);
                }
            }
        }
        g_ps1TestDone = 1;
    }

    void frame() override {
        uint32_t now = gpu().getFrameCount();
        if (g_ps1PerfFrames)
            g_ps1PerfVsyncs = g_ps1PerfVsyncs + (now - m_lastCount);
        m_lastCount = now;
        uint32_t b = 0;
        for (int k = 0; k < 16; ++k)
            if (m_pad.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, (psyqo::AdvancedPad::Button)k))
                b |= 1u << k;
        g_ps1TestPad = b;

        // Test pattern: 8 colour bars + a bar that moves one pixel per frame.
        PS1ChainBegin(gpu());
        PS1ChainClear(0, 0, 0);
        static const uint8_t col[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                          {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {40, 40, 40}};
        for (int i = 0; i < 8; ++i) {
            psyqo::Prim::Rectangle r(psyqo::Color{{.r = col[i][0], .g = col[i][1], .b = col[i][2]}});
            r.position = psyqo::Vertex{{.x = (int16_t)(i * 40), .y = 40}};
            r.size     = psyqo::Vertex{{.w = 40, .h = 120}};
            PS1ChainRect(r);
        }
        psyqo::Prim::Rectangle m(psyqo::Color{{.r = 255, .g = 128, .b = 0}});
        m.position = psyqo::Vertex{{.x = (int16_t)(g_ps1PerfFrames % 312), .y = 180}};
        m.size     = psyqo::Vertex{{.w = 8, .h = 20}};
        PS1ChainRect(m);
        PS1ChainEnd();
        g_ps1PerfFrames = g_ps1PerfFrames + 1;
    }

    psyqo::CDRomDevice m_cdrom;
    psyqo::AdvancedPad m_pad;
    uint32_t m_lastCount = 0;
};

PlatformTest s_app;

} // namespace

int main() { return s_app.run(); }
