/*
 * Full-screen FMV — see fmv.hh.
 */
#include "fmv.hh"
#include "gpu_chain.hh"
#include "loading.hh"
#include "video.hh"
#include "xa_music.hh"

#include <stdio.h>
#include <string.h>

void PS1RenderFmvFrame(int mode); // ps1/render.cpp: 0 = black, 1 = new picture (bars only), 2 = repeat
void PS1RenderFmvFade(int step);  // ps1/render.cpp: darken the whole frame by step / 255

volatile uint32_t g_ps1FmvPlays   = 0;
volatile uint32_t g_ps1FmvFails   = 0;
volatile uint32_t g_ps1FmvFrames  = 0;
volatile uint32_t g_ps1FmvSkipped = 0;

#define FMV_ARENA_WORDS 256 // per parity while a video plays: fills, a copy and the fade
#define FMV_Y           32  // 176 lines letterboxed in 240
#define FMV_FADE_STEP   8   // as upstream ProcessVideo: fadeMode += 8 per frame after a skip

namespace {
enum class State : uint8_t { IDLE, REQUESTED, PLAYING, CLOSING, CLOSED };
State s_state = State::IDLE;
char s_path[64];
int s_audio      = 0;
bool s_picture   = false; // a picture has been shown (else frames stay black)
bool s_skipped   = false;
int s_fade       = 0;

void swapArenas(uint32_t words) {
    PS1ChainSuspend(); // nothing chained yet this frame: the arenas go back to the heap
    PS1ChainSetArenaWords(words);
}
} // namespace

bool PS1FmvRequest(const char *name, int soundtrack) {
    snprintf(s_path, sizeof(s_path), "Data/Videos/%s.str", name);
    s_audio   = soundtrack ? 1 : 0;
    s_state   = State::REQUESTED;
    s_picture = false;
    s_skipped = false;
    s_fade    = 0;
    return true;
}

void PS1FmvBeginFrame() {
    if (s_state == State::REQUESTED) {
        PS1XaStop(); // the video owns the CD
        swapArenas(FMV_ARENA_WORDS);
        static bool s_mdecReady = false;
        if (!s_mdecReady) // MDEC reset + its DMA channels + tables, once
            s_mdecReady = PS1VideoInit();
        PS1LoadingBegin(); // opening reads the sidecar + directory: no loading icon needed, but the CD windows
        bool ok = PS1VideoOpen(s_path, s_audio);
        PS1LoadingEnd(); // (resumes the chain with the small arenas)
        if (ok) {
            g_ps1FmvPlays = g_ps1FmvPlays + 1;
            s_state       = State::PLAYING;
        } else {
            g_ps1FmvFails = g_ps1FmvFails + 1;
            s_state       = State::CLOSING;
        }
    }
    if (s_state == State::CLOSING) {
        PS1VideoClose();
        swapArenas(0);
        PS1ChainResume();
        s_state = State::CLOSED;
    }
}

int PS1FmvUpdate(bool skip) {
    if (s_state == State::CLOSED) {
        s_state = State::IDLE;
        return 1;
    }
    if (s_state == State::IDLE)
        return 1;
    g_ps1FmvFrames = g_ps1FmvFrames + 1;
    if (s_state != State::PLAYING) {
        PS1RenderFmvFrame(0);
        return 0;
    }
    if (skip && !s_skipped) {
        s_skipped     = true;
        g_ps1FmvSkipped = g_ps1FmvSkipped + 1;
    }
    int r = 0;
    if (!s_skipped)
        // Into the buffer shown after this frame's flip: psyqo sends the chain after flip(), so during the
        // frame PS1ChainBufferY() is the buffer on screen (an upload there showed the new picture, then the
        // old one for a frame). The frame's chain copies it into its own buffer (PS1RenderFmvFrame mode 1).
        r = PS1VideoDecodeTo(PS1Hblanks(), 0, (256 - PS1ChainBufferY()) + FMV_Y);
    if (r > 0)
        s_picture = true;
    PS1RenderFmvFrame(r > 0 ? 1 : s_picture ? 2 : 0);
    if (s_skipped) { // fade the last picture out, then close
        s_fade += FMV_FADE_STEP;
        PS1RenderFmvFade(FMV_FADE_STEP);
        if (s_fade >= 0xFF)
            s_state = State::CLOSING;
    } else if (r < 0) {
        s_state = State::CLOSING;
    }
    return 0;
}

bool PS1FmvActive() { return s_state == State::PLAYING; }
