#include "RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_PS1
/*
 * PS1 app (docs/30 phase 2): psyqo owns the loop. start() boots the engine (Engine.Init: GameConfig,
 * global scripts, first stage) inside a bracketed load (loading icon); frame() runs one engine frame
 * (RetroEngine::RunFrame) and updates the GDB counters below. Rendering, audio and the menus arrive in
 * phases 3-5; until then the game logic runs headless (ps1/render_headless.cpp, ps1/audio_headless.cpp).
 *
 * `make BOOT=<stage folder>` (e.g. Zone01): start in that stage (Engine.startSceneFolder / ID, the same
 * fields the PC `stage=` argument sets) for the test harness, with player BOOT_PLAYER (stage.playerListPos:
 * 0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails).
 */
#include <psyqo/advancedpad.hh>
#include "../ps1/pad.hh"
#include <psyqo/alloc.h>
#include <psyqo/application.hh>
#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>

#include "../ps1/loading.hh"
#include "../ps1/stack.hh"
#include "../ps1/test_mode.hh"
#include "../ps1/version_stamp.hh"
#include "../ps1/video.hh"

extern "C" char __heap_start, __stack_start;
volatile uint32_t g_ps1PerfFrames          = 0; // frames run (engine RunFrame calls)
volatile uint32_t g_ps1PerfVsyncs          = 0; // vsyncs elapsed across those frames
volatile uint32_t g_ps1PerfLastFrameVsyncs = 0; // vsyncs taken by the last frame (1 = 60 fps)
volatile uint32_t g_ps1PerfLastHbl         = 0; // RunFrame CPU time of the last frame (hblanks, 262 = 1 frame)
volatile uint32_t g_ps1PerfMaxHbl          = 0; // longest RunFrame CPU time (hblanks)
volatile uint32_t g_ps1PerfOverHbl         = 0; // frames whose CPU time exceeded one frame
volatile uint32_t g_ps1HeapTotal           = 0; // bytes between bss end and the stack
volatile uint32_t g_ps1HeapHeadroom        = 0; // bytes never reached by the heap high-water mark
volatile uint32_t g_ps1InitDone            = 0; // Engine.Init() returned
static uint32_t s_ps1PerfLastCount         = 0;
// Measuring window (tools/perf_scene.sh, docs/30 "Mania lessons pass"): GDB sets From / To (frame numbers as
// g_ps1PerfFrames counts them; .data, so a run sets them before the game starts); frames in [From, To) are summed.
volatile uint32_t g_ps1PerfWinFrom = 0xFFFFFFFFu, g_ps1PerfWinTo = 0;
volatile uint32_t g_ps1PerfWinFrames = 0, g_ps1PerfWinHbl = 0, g_ps1PerfWinMaxHbl = 0; // frames, CPU hblanks sum / worst
volatile uint32_t g_ps1PerfWinVbl = 0, g_ps1PerfWinMaxVbl = 0, g_ps1PerfWinOver = 0;   // vblanks sum / worst, CPU > 262
#ifdef PS1_PERF_SPLIT
// `make PERF_SPLIT=1` (docs/43, harness only): the frame time besides the CPU's. psyqo sends a frame's chain right after
// the flip, so the GPU draws it while the CPU builds the next frame, and the flip waits for that chain and a new vblank.
// GpuTail: hblanks the GPU was still drawing the previous frame's chain (its DMA, up to the GPU FIFO's last 16 words)
// after this frame's CPU work ended; 0 in PCSX-Redux, which draws instantly, modelled by DuckStation. Flip: hblanks from
// the end of frame() to the next frame() (the vblank wait; the tail is already waited for). Window sums as above, plus
// the window sums of the per-frame section counters (g_ps1PerfWinSec[], order as s_ps1PerfSec).
volatile uint32_t g_ps1PerfGpuTail = 0, g_ps1PerfFlip = 0;
volatile uint32_t g_ps1PerfWinTail = 0, g_ps1PerfWinMaxTail = 0, g_ps1PerfWinTailFrames = 0; // sum / worst / frames > 0
volatile uint32_t g_ps1PerfWinFlip = 0, g_ps1PerfWinMaxFlip = 0;
static uint32_t s_ps1FrameEnd = 0;
extern volatile uint32_t g_ps1ProfObjectsHbl, g_ps1ProfDrawHbl, g_ps1ProfObjDrawHbl, g_ps1ProfFloorsHbl, g_ps1ProfLayer[5];
extern volatile uint32_t g_ps1Prof3DTransformHbl, g_ps1Prof3DSortHbl, g_ps1Prof3DDrawHbl;
// objects, draw (all of DrawStageGFX), object draw subs, 3D floors, FG layers, per-line passes, tile bands, strip bands,
// 3D transform, sort, draw (the last Draw3DScene: stale outside 3D scenes)
static volatile uint32_t *const s_ps1PerfSec[11] = {
    &g_ps1ProfObjectsHbl, &g_ps1ProfDrawHbl, &g_ps1ProfObjDrawHbl, &g_ps1ProfFloorsHbl, &g_ps1ProfLayer[0],
    &g_ps1ProfLayer[1], &g_ps1ProfLayer[2], &g_ps1ProfLayer[3], &g_ps1Prof3DTransformHbl, &g_ps1Prof3DSortHbl,
    &g_ps1Prof3DDrawHbl};
volatile uint32_t g_ps1PerfWinSec[11] = {};
// the window's frames one by one (perf_scene.sh LOG=1): CPU hblanks (up to 4095) | vsyncs (up to 15) << 12
#define PS1_PERF_LOG_N 640
volatile uint16_t g_ps1PerfLog[PS1_PERF_LOG_N];
#endif

psyqo::GPU *g_ps1GPU           = nullptr;
PS1Pad *g_ps1Pad   = nullptr;
psyqo::CDRomDevice *g_ps1CDRom = nullptr;
void PS1IrqGuardInstall(); // ps1/irq_guard.cpp
#if PS1_PC_SAMPLER
void PS1PcSamplerInstall(); // ps1/pc_sampler.cpp (make SAMPLER=1)
#endif
void PS1McPrepare();       // ps1/memcard.cpp
void PS1SaveTestService(); // RSDKv4/Userdata.cpp (GDB test hook)
void PS1RenderBeginFrame(psyqo::GPU &gpu); // ps1/render.cpp
#include "../ps1/menu.hh"
void PS1RenderEndFrame();
void PS1DetectGpu(); // ps1/render.cpp
extern volatile uint32_t g_ps1GpuType, g_ps1GpuQuery7, g_ps1FlipTest;

__attribute__((noinline)) static void PS1PerfTick(psyqo::GPU &gpu)
{
    uint32_t now = gpu.getFrameCount();
    if (g_ps1PerfFrames) {
        uint32_t d               = now - s_ps1PerfLastCount;
        g_ps1PerfVsyncs          = g_ps1PerfVsyncs + d;
        g_ps1PerfLastFrameVsyncs = d;
        uint32_t f = g_ps1PerfFrames;
        if (f >= g_ps1PerfWinFrom && f < g_ps1PerfWinTo) {
            uint32_t h          = g_ps1PerfLastHbl;
            g_ps1PerfWinFrames  = g_ps1PerfWinFrames + 1;
            g_ps1PerfWinHbl     = g_ps1PerfWinHbl + h;
            g_ps1PerfWinVbl     = g_ps1PerfWinVbl + d;
            if (h > g_ps1PerfWinMaxHbl)
                g_ps1PerfWinMaxHbl = h;
            if (d > g_ps1PerfWinMaxVbl)
                g_ps1PerfWinMaxVbl = d;
            if (h > 262)
                g_ps1PerfWinOver = g_ps1PerfWinOver + 1;
#ifdef PS1_PERF_SPLIT
            uint32_t t = g_ps1PerfGpuTail, fl = g_ps1PerfFlip;
            g_ps1PerfWinTail = g_ps1PerfWinTail + t;
            g_ps1PerfWinFlip = g_ps1PerfWinFlip + fl;
            if (t)
                g_ps1PerfWinTailFrames = g_ps1PerfWinTailFrames + 1;
            if (t > g_ps1PerfWinMaxTail)
                g_ps1PerfWinMaxTail = t;
            if (fl > g_ps1PerfWinMaxFlip)
                g_ps1PerfWinMaxFlip = fl;
            for (int k = 0; k < 11; ++k)
                g_ps1PerfWinSec[k] = g_ps1PerfWinSec[k] + *s_ps1PerfSec[k];
            if (f - g_ps1PerfWinFrom < PS1_PERF_LOG_N)
                g_ps1PerfLog[f - g_ps1PerfWinFrom] = (uint16_t)((h < 4095 ? h : 4095) | (d < 15 ? d : 15) << 12);
#endif
        }
    }
    s_ps1PerfLastCount = now;
    g_ps1PerfFrames    = g_ps1PerfFrames + 1;
    uintptr_t hs = (uintptr_t)&__heap_start, ss = (uintptr_t)&__stack_start;
    uintptr_t top = (uintptr_t)psyqo_heap_end();
    if (top < hs)
        top = hs;
    g_ps1HeapTotal    = ss - hs;
    g_ps1HeapHeadroom = ss - top;
    if (PS1StackGuardBroken()) // the main stack (ps1/stack.hh) overflowed its array
        g_ps1StackOverflow = g_ps1StackOverflow + 1;
}

#ifdef PS1_COMBO_TEST
void PS1ComboTestTick(uint32_t frame); // ps1/combo_test.cpp
#endif

#ifdef PS1_BOOT_FOLDER
extern "C" const char g_ps1BootBuild[] = PS1_BOOT_FOLDER; // marks a `make BOOT=` test build (make dist refuses it)
#endif

namespace {

class RSDKv4PS1App final : public psyqo::Application {
    void prepare() override
    {
        gpu().initialize(psyqo::GPU::Configuration{}
                             .set(psyqo::GPU::Resolution::W320)
                             .set(psyqo::GPU::VideoMode::AUTO)
                             .set(psyqo::GPU::ColorMode::C15BITS)
                             .set(psyqo::GPU::Interlace::PROGRESSIVE)
                             .set(psyqo::GPU::MiscSetting::CLEAR_VRAM));
        g_ps1GPU = &gpu();
        m_cdrom.prepare();
        PS1McPrepare(); // memory card saves (RSDKv4/Userdata.cpp, docs/30 phase 8)
    }

    void start() override
    {
        m_cdrom.resetBlocking(gpu());
        g_ps1CDRom = &m_cdrom;
        PS1IrqGuardInstall(); // re-raises CD / DMA IRQ edges lost to psyqo's I_STAT read-modify-write (docs/31)
#if PS1_PC_SAMPLER
        PS1PcSamplerInstall();
#endif
        m_pad.initialize(); // psyqo AdvancedPad's driver on port 1 only (ps1/pad.hh, docs/43 C11); never SimplePad (Nexus docs/26 F14)
        g_ps1Pad = &m_pad;
        PS1ProfTicksInit(); // per-object-type CPU profile (Object.cpp g_ps1TypeTicks)
        memset(&scriptEng, 0, sizeof(scriptEng)); // the VM state in the scratchpad (Script.hpp)
#ifdef PS1_BOOT_FOLDER
        StrCopy(Engine.startSceneFolder, g_ps1BootBuild);
        StrCopy(Engine.startSceneID, PS1_BOOT_ID);
#endif
#if PS1_TEST
        PS1TestInit(); // TEST.SEL: the scenario (docs/45)
        if (PS1TestBootStage()) {
            StrCopy(Engine.startSceneFolder, PS1TestBootFolder());
            StrCopy(Engine.startSceneID, PS1TestBootId());
        }
#endif
        // The memory card is read once, here, before the boot load (RSDKv4/Userdata.cpp; the engine's ReadSaveRAMData
        // calls get the cached result): a card read inside the load left the heap split under the chain arenas
        // (47 KB less headroom at the load peak).
        ReadSaveRAMData();
        PS1DetectGpu();    // flip-bit rectangles for flipped tiles and sprites if the GPU draws them (ps1/render.cpp, docs/43 G3)
#if PS1_TEST
        PS1TestNote("gpu", g_ps1GpuType, g_ps1GpuQuery7, g_ps1FlipTest); // "gpu type q7 readback" (0 = not tested)
#endif
        PS1VersionLoad();      // VERSION.TXT (build stamp for the title screen, ps1/version_stamp.hh): next to the index
        PS1LoadingBegin(); // boot load: GameConfig, global scripts, first stage
        PS1Scene3DReadList(); // the stages that get the Scene3D block (RSDKv4/Scene3D.cpp)
        Engine.Init();
#ifdef PS1_BOOT_FOLDER
        // As a save file with this character starts a game (RetroEngine::Init, the startSave path).
        SetGlobalVariableByName("stage.player2Enabled", PS1_BOOT_PLAYER == 3);
#endif
#if PS1_TEST
        if (PS1TestBootStage())
            SetGlobalVariableByName("stage.player2Enabled", PS1TestBootPlayer() == 3);
        PS1TestAfterInit();
#endif
        PS1LoadingEnd();
        // The menus' resident fonts + strings (VRAM rows 242-255; docs/30 phase 4.2).
        PS1MenuResidentLoad();
#if !defined(PS1_BOOT_FOLDER) && PS1_MENUS
#if PS1_TEST
        if (!PS1TestBootStage())
#endif
        PS1MenuOpen(PS1_MENU_TITLE); // the mobile front end's flow (docs/30 phase 4): menus first, the scripted Title
                                     // comes when a game starts
#endif
        g_ps1InitDone = 1;
    }

    void frame() override
    {
        uint32_t t0 = PS1Hblanks();
#ifdef PS1_PERF_SPLIT
        if (s_ps1FrameEnd)
            g_ps1PerfFlip = (t0 - s_ps1FrameEnd) & 0xffff;
#endif
        PS1MenuTestService(); // GDB: g_ps1MenuTestLoad
        PS1SaveTestService(); // GDB: g_ps1SaveTest
#if PS1_TEST
        PS1TestPreFrame(); // input track, pokes
#endif
        PS1RenderBeginFrame(gpu()); // all drawing is DMA-chained; psyqo sends it after frame() returns
        Engine.RunFrame();
        PS1RenderEndFrame();
        PS1AudioUpdate(); // ps1/audio.cpp: ended SFX channels, music volume
        uint32_t cpu     = PS1HblanksSince(t0);
        g_ps1PerfLastHbl = cpu;
        if (cpu > g_ps1PerfMaxHbl)
            g_ps1PerfMaxHbl = cpu;
        if (cpu > 262)
            g_ps1PerfOverHbl = g_ps1PerfOverHbl + 1;
#ifdef PS1_PERF_SPLIT
        uint32_t tw = PS1Hblanks();
        gpu().waitChainIdle(); // psyqo's flip waits for it too
        g_ps1PerfGpuTail = PS1HblanksSince(tw);
#endif
        PS1PerfTick(gpu());
#if PS1_TEST
        PS1TestPostFrame(); // telemetry, checkpoints, end of the run
#endif
#ifdef PS1_COMBO_TEST
        PS1ComboTestTick(g_ps1PerfFrames);
#endif
#ifdef PS1_PERF_SPLIT
        s_ps1FrameEnd = PS1Hblanks();
#endif
    }

    psyqo::CDRomDevice m_cdrom;
    PS1Pad m_pad;
};

RSDKv4PS1App s_app;

int ps1AppMain() { return s_app.run(); }

} // namespace

// The app runs on its own .bss stack, out of the heap's way (ps1/stack.hh, V4-102).
int main() {
    PS1StackPaint();
    return PS1RunOnStack(ps1AppMain, g_ps1Stack + PS1_STACK_BYTES / 4);
}

#else // PC and other platforms: the upstream main

#if !RETRO_USE_ORIGINAL_CODE

#if RETRO_PLATFORM == RETRO_WIN
#include "Windows.h"
#endif

void parseArguments(int argc, char *argv[])
{
    for (int a = 0; a < argc; ++a) {
        const char *find = "";

        find = strstr(argv[a], "stage=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneFolder[b++] = find[c++];
            Engine.startSceneFolder[b] = 0;
        }

        find = strstr(argv[a], "scene=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneID[b++] = find[c++];
            Engine.startSceneID[b] = 0;
        }

        find = strstr(argv[a], "console=true");
        if (find) {
            engineDebugMode       = true;
            Engine.devMenu        = true;
            Engine.consoleEnabled = true;
#if RETRO_PLATFORM == RETRO_WIN
            AllocConsole();
            freopen_s((FILE **)stdin, "CONIN$", "w", stdin);
            freopen_s((FILE **)stdout, "CONOUT$", "w", stdout);
            freopen_s((FILE **)stderr, "CONOUT$", "w", stderr);
#endif
        }

        find = strstr(argv[a], "usingCWD=true");
        if (find) {
            usingCWD = true;
        }
    }
}
#endif

int main(int argc, char *argv[])
{
#if !RETRO_USE_ORIGINAL_CODE
    parseArguments(argc, argv);
#endif

    SDL_SetHint(SDL_HINT_WINRT_HANDLE_BACK_BUTTON, "1");
    Engine.Init();
    Engine.Run();

#if !RETRO_USE_ORIGINAL_CODE
    if (Engine.consoleEnabled) {
#if RETRO_PLATFORM == RETRO_WIN
        FreeConsole();
#endif
    }
#endif

    return 0;
}

#if RETRO_PLATFORM == RETRO_UWP
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return SDL_WinRTRunApp(main, NULL); }
#endif
#endif // RETRO_PLATFORM == RETRO_PS1
