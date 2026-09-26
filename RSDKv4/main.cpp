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
#include <psyqo/alloc.h>
#include <psyqo/application.hh>
#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>

#include "../ps1/loading.hh"
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

psyqo::GPU *g_ps1GPU           = nullptr;
psyqo::AdvancedPad *g_ps1Pad   = nullptr;
psyqo::CDRomDevice *g_ps1CDRom = nullptr;
void PS1IrqGuardInstall(); // ps1/irq_guard.cpp
void PS1RenderBeginFrame(psyqo::GPU &gpu); // ps1/render.cpp
#include "../ps1/menu.hh"
void PS1RenderEndFrame();

__attribute__((noinline)) static void PS1PerfTick(psyqo::GPU &gpu)
{
    uint32_t now = gpu.getFrameCount();
    if (g_ps1PerfFrames) {
        uint32_t d               = now - s_ps1PerfLastCount;
        g_ps1PerfVsyncs          = g_ps1PerfVsyncs + d;
        g_ps1PerfLastFrameVsyncs = d;
    }
    s_ps1PerfLastCount = now;
    g_ps1PerfFrames    = g_ps1PerfFrames + 1;
    uintptr_t hs = (uintptr_t)&__heap_start, ss = (uintptr_t)&__stack_start;
    uintptr_t top = (uintptr_t)psyqo_heap_end();
    if (top < hs)
        top = hs;
    g_ps1HeapTotal    = ss - hs;
    g_ps1HeapHeadroom = ss - top;
}

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
    }

    void start() override
    {
        m_cdrom.resetBlocking(gpu());
        g_ps1CDRom = &m_cdrom;
        PS1IrqGuardInstall(); // re-raises CD / DMA IRQ edges lost to psyqo's I_STAT read-modify-write (docs/31)
        m_pad.initialize(psyqo::AdvancedPad::PollingMode::Fast); // never SimplePad (Nexus docs/26 F14)
        g_ps1Pad = &m_pad;
        PS1ProfTicksInit(); // per-object-type CPU profile (Object.cpp g_ps1TypeTicks)
        memset(&scriptEng, 0, sizeof(scriptEng)); // the VM state in the scratchpad (Script.hpp)
#ifdef PS1_BOOT_FOLDER
        StrCopy(Engine.startSceneFolder, g_ps1BootBuild);
        StrCopy(Engine.startSceneID, PS1_BOOT_ID);
#endif
        PS1LoadingBegin(); // boot load: GameConfig, global scripts, first stage
        Engine.Init();
#ifdef PS1_BOOT_FOLDER
        // As a save file with this character starts a game (RetroEngine::Init, the startSave path).
        SetGlobalVariableByName("stage.player2Enabled", PS1_BOOT_PLAYER == 3);
#endif
        PS1LoadingEnd();
        // The menus' resident fonts + strings (VRAM rows 242-255; docs/30 phase 4.2).
        PS1MenuResidentLoad();
#ifndef PS1_BOOT_FOLDER
        PS1MenuOpen(PS1_MENU_TITLE); // the mobile front end's flow (docs/30 phase 4): menus first, the scripted Title
                                     // comes when a game starts
#endif
        g_ps1InitDone = 1;
    }

    void frame() override
    {
        uint32_t t0 = PS1Hblanks();
        PS1MenuTestService(); // GDB: g_ps1MenuTestLoad
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
        PS1PerfTick(gpu());
    }

    psyqo::CDRomDevice m_cdrom;
    psyqo::AdvancedPad m_pad;
};

RSDKv4PS1App s_app;

} // namespace

int main() { return s_app.run(); }

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
