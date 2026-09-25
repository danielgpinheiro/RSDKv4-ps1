/*
 * CD-XA music — see xa_music.hh. Action pattern from ps1/cdrom_stream.cpp; XA command sequence
 * from PSn00bSDK examples/cdrom/cdxa and psx-spx cdromdrive.md (Setfilter, Setmode, ReadS).
 */
#include "xa_music.hh"

#include <EASTL/atomic.h>
#include <stdio.h>
#include <string.h>

#include "common/hardware/dma.h"
#include "psyqo/cdrom-device.hh"
#include "psyqo/gpu.hh"
#include "psyqo/hardware/cdrom.hh"
#include "psyqo/hardware/sbus.hh"
#include "psyqo/kernel.hh"
#include "psyqo/msf.hh"

extern psyqo::CDRomDevice *g_ps1CDRom;
extern psyqo::GPU *g_ps1GPU;
extern volatile uint32_t g_ps1PerfFrames;
bool PS1FsFind(const char *path, uint32_t *lba, uint32_t *size); // cdrom_fs.cpp
void PS1FsStopStream();                                          // cdrom_fs.cpp: one CD action at a time

volatile uint32_t g_ps1XaStarts      = 0;
volatile uint32_t g_ps1XaLoops       = 0;
volatile uint32_t g_ps1XaEnds        = 0;
volatile uint32_t g_ps1XaDataSectors = 0;
volatile uint32_t g_ps1XaForcedStops = 0;
volatile uint32_t g_ps1XaLoopFrame   = 0;
volatile uint32_t g_ps1XaStartFrame  = 0;
volatile int32_t g_ps1XaChannel      = -1;
volatile int32_t g_ps1XaTrack        = -1;

using psyqo::Hardware::CDRom::CDL;

namespace {

#define XA_FILE       1
#define XA_MODE       0xE8 // 2x speed | XA-ADPCM | 2340-byte sectors | XA filter
#define XA_SECTOR     2340
#define XA_MAX_TRACK  80
#define XA_INTERLEAVE 8

struct Track { // Music.bin XAM2 entry (tools/audio/build_music.py), 48 bytes
    char path[32];
    uint8_t channel, pad;
    uint16_t id;
    uint32_t startRow, loopRow, rows;
};
static_assert(sizeof(Track) == 48, "Music.bin entry");
Track s_tracks[XA_MAX_TRACK];
int s_trackCount    = 0;
uint32_t s_fileLba  = 0;
bool s_ready        = false;

enum class XaState : uint8_t { IDLE, SETLOC, SETFILTER, SETMODE, READS, PLAYING, LOOP_SETLOC, LOOP_READS, PAUSE, PAUSE_ACK };

struct Xa {
    volatile bool running = false; // an action is registered
    volatile bool ended   = false; // non-looping track reached its end marker
    volatile bool paused  = false; // PauseSound: resumable
    int channel           = -1;
    int track             = -1;
    bool loop             = false;
};
Xa s_xa;

alignas(4) uint8_t s_sector[(XA_SECTOR + 3) & ~3];

void sendSetloc(uint32_t lba) {
    psyqo::MSF msf(lba + 150);
    uint8_t bcd[3];
    msf.toBCD(bcd);
    psyqo::Hardware::CDRom::Command.send(CDL::SETLOC, bcd[0], bcd[1], bcd[2]);
}

// Physical sector of row r of the current channel (row r of channel k = file sector r * 8 + k).
uint32_t rowLba(uint32_t row) { return s_fileLba + row * XA_INTERLEAVE + (uint32_t)s_xa.channel; }

class XaAction : public psyqo::CDRomDevice::Action<XaState> {
  public:
    XaAction() : Action("PS1XaAction") {}
    void start(bool resume) {
        registerMe(g_ps1CDRom);
        setCallback([](bool) {
            s_xa.running = false;
            eastl::atomic_signal_fence(eastl::memory_order_release);
        });
        if (resume) { // the drive keeps mode, filter and position across a Pause
            setState(XaState::READS);
            psyqo::Hardware::CDRom::Command.send(CDL::READS);
        } else {
            setState(XaState::SETLOC);
            sendSetloc(rowLba(s_tracks[s_xa.track].startRow));
        }
    }
    // From the main loop: pause if playing (the IRQ may be mid-sequence: then on its next ack).
    void requestPause() {
        psyqo::Kernel::fastEnterCriticalSection();
        m_pauseRequested = true;
        if (getState() == XaState::PLAYING) {
            setState(XaState::PAUSE);
            psyqo::Hardware::CDRom::Command.send(CDL::PAUSE);
        }
        psyqo::Kernel::fastLeaveCriticalSection();
    }
    void clearPauseRequest() { m_pauseRequested = false; }
    bool dataReady(const psyqo::CDRomDevice::Response &) override {
        // Only data sectors reach the CPU in XA mode: read it (header + subheader + data) and
        // look for our channel's end marker.
        psyqo::Hardware::CDRom::Ctrl.throwAway();
        psyqo::Hardware::CDRom::DataRequest = 0;
        psyqo::Hardware::CDRom::InterruptControl.throwAway();
        psyqo::Hardware::CDRom::DataRequest = 0x80;
        psyqo::Hardware::SBus::Dev5Ctrl = 0x20943;
        psyqo::Hardware::SBus::ComCtrl  = 0x132c;
        eastl::atomic_signal_fence(eastl::memory_order_acquire);
        DMA_CTRL[DMA_CDROM].MADR = reinterpret_cast<uintptr_t>(s_sector);
        DMA_CTRL[DMA_CDROM].BCR  = (XA_SECTOR / 4) | 0x10000;
        DMA_CTRL[DMA_CDROM].CHCR = 0x11000000; // burst: completes before the CPU continues
        g_ps1XaDataSectors = g_ps1XaDataSectors + 1;
        // 2340-byte layout: 4-byte header (MSF, mode), 8-byte subheader (file, channel, submode,
        // coding, repeated), then the 2048 data bytes: 'RSDKXAEND', channel, u16 track id.
        const Track &t = s_tracks[s_xa.track];
        bool marker    = s_sector[4] == XA_FILE && s_sector[5] == s_xa.channel && (s_sector[6] & 0x08)
                      && !memcmp(s_sector + 12, "RSDKXAEND", 9) && s_sector[21] == s_xa.channel
                      && (s_sector[22] | (s_sector[23] << 8)) == t.id;
        if (marker && getState() == XaState::PLAYING) {
            if (s_xa.loop && !m_pauseRequested) {
                g_ps1XaLoops     = g_ps1XaLoops + 1;
                g_ps1XaLoopFrame = g_ps1PerfFrames;
                setState(XaState::LOOP_SETLOC);
                sendSetloc(rowLba(t.loopRow));
            } else {
                g_ps1XaEnds = g_ps1XaEnds + 1;
                s_xa.ended  = true;
                setState(XaState::PAUSE);
                psyqo::Hardware::CDRom::Command.send(CDL::PAUSE);
            }
        }
        eastl::atomic_signal_fence(eastl::memory_order_release);
        return false;
    }
    bool complete(const psyqo::CDRomDevice::Response &) override {
        if (getState() != XaState::PAUSE_ACK)
            return false;
        setSuccess(true);
        return true;
    }
    bool acknowledge(const psyqo::CDRomDevice::Response &) override {
        switch (getState()) {
            case XaState::SETLOC:
                setState(XaState::SETFILTER);
                psyqo::Hardware::CDRom::Command.send(CDL::SETFILTER, XA_FILE, (uint8_t)s_xa.channel);
                break;
            case XaState::SETFILTER:
                setState(XaState::SETMODE);
                psyqo::Hardware::CDRom::Command.send(CDL::SETMODE, XA_MODE);
                break;
            case XaState::SETMODE:
                setState(XaState::READS);
                psyqo::Hardware::CDRom::Command.send(CDL::READS);
                break;
            case XaState::LOOP_SETLOC:
                setState(XaState::LOOP_READS);
                psyqo::Hardware::CDRom::Command.send(CDL::READS);
                break;
            case XaState::READS:
            case XaState::LOOP_READS:
                if (m_pauseRequested) {
                    setState(XaState::PAUSE);
                    psyqo::Hardware::CDRom::Command.send(CDL::PAUSE);
                } else {
                    setState(XaState::PLAYING);
                }
                break;
            case XaState::PAUSE:
                setState(XaState::PAUSE_ACK);
                break;
            default:
                break;
        }
        return false;
    }

  private:
    volatile bool m_pauseRequested = false;
};

XaAction s_action;

// Demute (psx-spx: "Demute - Command 0Ch --> INT3(stat)"). psyqo's CDRomDevice::unmute() waits for
// an INT2 (complete) that never comes and aborts on the INT3 ("acknowledge() not implemented"),
// so the port sends it itself.
enum class DemuteState : uint8_t { IDLE, DEMUTE };
class DemuteAction : public psyqo::CDRomDevice::Action<DemuteState> {
  public:
    DemuteAction() : Action("PS1DemuteAction") {}
    void start(volatile bool *done) {
        registerMe(g_ps1CDRom);
        m_done = done;
        setCallback([this](bool) { *m_done = true; });
        setState(DemuteState::DEMUTE);
        psyqo::Hardware::CDRom::Command.send(CDL::UNMUTE);
    }
    bool acknowledge(const psyqo::CDRomDevice::Response &) override {
        setSuccess(true);
        return true;
    }

  private:
    volatile bool *m_done = nullptr;
};
DemuteAction s_demute;

void waitIdle() {
    while (s_xa.running && g_ps1GPU)
        g_ps1GPU->pumpCallbacks();
}

void startAction(bool resume) {
    PS1FsStopStream();
    s_xa.running = true;
    s_action.clearPauseRequest();
    eastl::atomic_signal_fence(eastl::memory_order_release);
    s_action.start(resume);
}

bool pathEquals(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        char y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

} // namespace

bool PS1XaInit() {
    uint32_t size = 0;
    if (!PS1FsFind("Data/Music/Music.xa", &s_fileLba, &size))
        return false;
    FILE *f = fopen("Data/Music/Music.bin", "rb");
    if (!f)
        return false;
    uint8_t head[12];
    if (fread(head, 1, 12, f) != 12 || memcmp(head, "XAM2", 4) || (head[6] | (head[7] << 8)) != XA_INTERLEAVE) {
        fclose(f);
        return false;
    }
    int count    = head[4] | (head[5] << 8);
    s_trackCount = 0;
    while (s_trackCount < count && s_trackCount < XA_MAX_TRACK && fread(&s_tracks[s_trackCount], sizeof(Track), 1, f) == 1) {
        s_tracks[s_trackCount].path[31] = 0;
        ++s_trackCount;
    }
    fclose(f);
    // psyqo's reset never demutes the CD audio output (CD-DA / XA).
    static volatile bool s_demuted;
    s_demuted = false;
    PS1FsStopStream();
    s_demute.start(&s_demuted);
    while (!s_demuted)
        g_ps1GPU->pumpCallbacks();
    s_ready = true;
    return true;
}

int PS1XaFindTrack(const char *path) {
    const char *prefix = "Data/Music/";
    size_t n           = strlen(prefix);
    if (!strncmp(path, prefix, n))
        path += n;
    for (int i = 0; i < s_trackCount; ++i)
        if (pathEquals(s_tracks[i].path, path))
            return i;
    return -1;
}

void PS1XaPlay(int track, bool loop) {
    if (!s_ready || !g_ps1CDRom || track < 0 || track >= s_trackCount)
        return;
    PS1XaStop();
    s_xa.track   = track;
    s_xa.channel = s_tracks[track].channel;
    s_xa.loop    = loop;
    s_xa.ended   = false;
    s_xa.paused  = false;
    g_ps1XaStarts     = g_ps1XaStarts + 1;
    g_ps1XaStartFrame = g_ps1PerfFrames;
    g_ps1XaChannel    = s_xa.channel;
    g_ps1XaTrack      = track;
    startAction(false);
}

void PS1XaStop() {
    if (s_xa.running) {
        s_action.requestPause();
        waitIdle();
    }
    s_xa.paused = false;
}

void PS1XaPause() {
    if (s_xa.running && !s_xa.ended) {
        s_action.requestPause();
        waitIdle();
        s_xa.paused = true;
    }
}

void PS1XaResume() {
    if (s_xa.paused && !s_xa.running) {
        s_xa.paused = false;
        startAction(true);
    }
}

bool PS1XaPlaying() { return s_xa.running && !s_xa.ended; }
bool PS1XaEnded() { return s_xa.ended; }

// cdrom_fs safety net: a file read while music plays (one CD action at a time).
bool PS1XaStopForFileRead() {
    if (!s_xa.running)
        return false;
    g_ps1XaForcedStops = g_ps1XaForcedStops + 1;
    PS1XaStop();
    return true;
}
