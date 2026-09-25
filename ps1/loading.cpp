/*
 * Loading indicator — see loading.hh. Icon data: ps1/loading_icon.h (tools/loading/build_icon.py).
 */
#include "loading.hh"

#include <psyqo/gpu.hh>
#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/sprites.hh>

#include <psyqo/alloc.h>

#include "gpu_chain.hh"
#include "loading_icon.h"

extern psyqo::GPU *g_ps1GPU;
extern volatile uint32_t g_ps1PerfFrames;

volatile uint32_t g_ps1LoadingLoads  = 0;
volatile uint32_t g_ps1LoadingShown  = 0;
volatile uint32_t g_ps1LoadingTicks  = 0;
volatile uint32_t g_ps1LoadingFrame  = 0;
volatile uint32_t g_ps1LoadingLastUs = 0;
volatile uint32_t g_ps1LoadingHeadroom = 0; // heap never reached, read at the end of the last load
extern "C" char __heap_start, __stack_start;
// GDB evidence aids: PS1LoadingDebugHook stops right after an icon frame is drawn,
// PS1LoadingDoneHook at the end of every outermost load (tools/load_profile.sh).
extern "C" __attribute__((noinline)) void PS1LoadingDebugHook(uint32_t tick) { __asm__ volatile("" ::"r"(tick)); }
extern "C" __attribute__((noinline)) void PS1LoadingDoneHook(uint32_t us) { __asm__ volatile("" ::"r"(us)); }

namespace {

// Screen position (bottom-right, 8-px margin) and scratch layout inside the hidden buffer:
// texture at (0, 0) as a 4-bit page (pageX 0), CLUT at (64, 96), saved background at (128, 0).
constexpr int16_t ICON_X = 320 - PS1_LOADING_ICON_SIZE - 8;
constexpr int16_t ICON_Y = 240 - PS1_LOADING_ICON_SIZE - 8;
constexpr int16_t CLUT_X = 64, CLUT_Y = PS1_LOADING_ICON_TEX_H;
constexpr int16_t SAVE_X = 128, SAVE_Y = 0;

struct VRAMCopy { // GP0(80h) VRAM-to-VRAM copy
    uint32_t command = 0x80000000;
    uint32_t src, dst, size;
};

int s_depth      = 0;
unsigned s_timer = 0;
uint32_t s_start = 0;
bool s_shown     = false;
int s_frame      = 0;
int16_t s_dispY  = 0; // displayed buffer (Y 0 or 256) while the icon is up

// psyqo (gpu.cpp flip): GPU::initialize shows Y 0; each flip toggles the parity and shows Y 0 for
// parity 1, Y 256 for parity 0, while the chain built in the frame before draws the other buffer.
// So inside frame() the displayed buffer is getNextScissor's (the next chain is only sent after
// the flip), and before the first flip (start()) it is Y 0. g_ps1PerfFrames = frames completed =
// flips done (PS1PerfTick ends every frame()).
int16_t displayedY() { return (g_ps1PerfFrames == 0 || g_ps1GPU->getParity()) ? 0 : 256; }

void copy(int16_t sx, int16_t sy, int16_t dx, int16_t dy, int16_t w, int16_t h) {
    VRAMCopy c;
    c.src  = (uint32_t)(uint16_t)sx | ((uint32_t)(uint16_t)sy << 16);
    c.dst  = (uint32_t)(uint16_t)dx | ((uint32_t)(uint16_t)dy << 16);
    c.size = (uint32_t)(uint16_t)w | ((uint32_t)(uint16_t)h << 16);
    g_ps1GPU->sendPrimitive(c);
}

void restoreBackground() {
    int16_t hid = (int16_t)(256 - s_dispY);
    copy(SAVE_X, (int16_t)(hid + SAVE_Y), ICON_X, (int16_t)(s_dispY + ICON_Y), PS1_LOADING_ICON_SIZE, PS1_LOADING_ICON_SIZE);
}

volatile uint32_t s_reentered = 0; // GDB: ticks skipped because one was already running

void tickBody();

// The blocking uploadToVRAM in the first tick pumps GPU callbacks while it waits, and a periodic
// timer that fell behind during a long blocking CD read fires again right there: without this
// guard the nested tick starts a second DMA ("Only one GPU DMA transfer at a time", psyqo abort;
// seen on Sonic CD's boot load, phase 2.2).
void tick() {
    static bool s_inTick = false;
    if (s_inTick) {
        s_reentered = s_reentered + 1;
        return;
    }
    s_inTick = true;
    tickBody();
    s_inTick = false;
}

void tickBody() {
    if (g_ps1GPU->now() - s_start < PS1_LOADING_DELAY_US)
        return;
    if (g_ps1GPU->isChainTransferring())
        return; // the previous frame's chain is still drawing: next tick
    if (!s_shown) {
        s_dispY     = displayedY();
        int16_t hid = (int16_t)(256 - s_dispY);
        g_ps1GPU->uploadToVRAM(s_ps1LoadingIconTex,
                               psyqo::Rect{{0, hid}, {PS1_LOADING_ICON_TEX_HW, PS1_LOADING_ICON_TEX_H}});
        g_ps1GPU->uploadToVRAM(s_ps1LoadingIconClut, psyqo::Rect{{CLUT_X, (int16_t)(hid + CLUT_Y)}, {16, 1}});
        copy(ICON_X, (int16_t)(s_dispY + ICON_Y), SAVE_X, (int16_t)(hid + SAVE_Y), PS1_LOADING_ICON_SIZE, PS1_LOADING_ICON_SIZE);
        s_shown          = true;
        g_ps1LoadingShown = g_ps1LoadingShown + 1;
    } else {
        restoreBackground(); // the icon's transparent texels must show the frame, not the last icon
    }
    int16_t hid = (int16_t)(256 - s_dispY);
    // Draw into the displayed buffer, then give psyqo its next-buffer scissor back.
    psyqo::Prim::Scissor scissor;
    scissor.start  = psyqo::Prim::DrawingAreaStart(psyqo::Vertex{{.x = 0, .y = s_dispY}});
    scissor.end    = psyqo::Prim::DrawingAreaEnd(psyqo::Vertex{{.x = 320, .y = (int16_t)(s_dispY + 240)}});
    scissor.offset = psyqo::Prim::DrawingOffset(psyqo::Vertex{{.x = 0, .y = s_dispY}});
    g_ps1GPU->sendPrimitive(scissor);
    psyqo::Prim::TPage tpage;
    tpage.attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex4Bits);
    tpage.attr.setPageX(0);
    tpage.attr.setPageY((uint8_t)(hid >> 8));
    g_ps1GPU->sendPrimitive(tpage);
    psyqo::Prim::Sprite s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
    s.position = psyqo::Vertex{{.x = ICON_X, .y = ICON_Y}};
    s.size     = psyqo::Vertex{{.w = PS1_LOADING_ICON_SIZE, .h = PS1_LOADING_ICON_SIZE}};
    s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)((s_frame % PS1_LOADING_ICON_PER_ROW) * PS1_LOADING_ICON_SIZE),
                                           (uint8_t)((s_frame / PS1_LOADING_ICON_PER_ROW) * PS1_LOADING_ICON_SIZE),
                                           psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = CLUT_X, .y = (int16_t)(hid + CLUT_Y)}})};
    g_ps1GPU->sendPrimitive(s);
    g_ps1GPU->getNextScissor(scissor);
    g_ps1GPU->sendPrimitive(scissor);
    PS1ChainInvalidateTPage(); // the GPU's texpage changed under the chain renderer
    g_ps1LoadingFrame = (uint32_t)s_frame;
    g_ps1LoadingTicks = g_ps1LoadingTicks + 1;
    s_frame           = (s_frame + 1) % PS1_LOADING_ICON_FRAMES;
    PS1LoadingDebugHook(g_ps1LoadingTicks);
}

} // namespace

void PS1FsReleaseWindows(); // cdrom_fs.cpp

bool PS1LoadingActive() { return s_depth > 0; }

void PS1LoadingBegin() {
    if (!g_ps1GPU)
        return;
    if (s_depth++ > 0)
        return;
    g_ps1LoadingLoads = g_ps1LoadingLoads + 1;
    PS1ChainSuspend(); // the chain arenas give their heap to the CD read-ahead windows (ps1/gpu_chain)
    s_start = g_ps1GPU->now();
    s_shown = false;
    s_frame = 0;
    s_timer = g_ps1GPU->armPeriodicTimer(PS1_LOADING_ICON_FRAME_US, [](uint32_t) { tick(); });
}

void PS1LoadingEnd() {
    if (!g_ps1GPU || s_depth == 0)
        return;
    if (--s_depth > 0)
        return;
    g_ps1GPU->cancelTimer(s_timer);
    PS1FsReleaseWindows(); // the read-ahead windows live for one load
    PS1ChainResume();      // and the chain arenas come back
    g_ps1LoadingLastUs = g_ps1GPU->now() - s_start;
    uintptr_t top = (uintptr_t)psyqo_heap_end();
    if (top < (uintptr_t)&__heap_start)
        top = (uintptr_t)&__heap_start;
    g_ps1LoadingHeadroom = (uint32_t)((uintptr_t)&__stack_start - top);
    PS1LoadingDoneHook(g_ps1LoadingLastUs);
    if (s_shown) {
        g_ps1GPU->waitChainIdle();
        restoreBackground();
        // The hidden buffer (scratch) is the frame the next flip shows: replace it with the frame
        // that stayed on screen during the load, so no scratch data is ever displayed.
        copy(0, s_dispY, 0, (int16_t)(256 - s_dispY), 320, 240);
    }
    s_shown = false;
}
