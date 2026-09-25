/*
 * Continuous CD-ROM sector streaming — see cdrom_stream.hh.
 * State machine modelled on nugget/psyqo/src/cdrom-device-readsectors.cpp.
 */
#include "cdrom_stream.hh"

#include <EASTL/atomic.h>

#include "common/hardware/dma.h"
#include "psyqo/cdrom-device.hh"
#include "psyqo/gpu.hh"
#include "psyqo/hardware/cdrom.hh"
#include "psyqo/hardware/sbus.hh"
#include "psyqo/kernel.hh"
#include "psyqo/msf.hh"

extern psyqo::CDRomDevice *g_ps1CDRom;
extern psyqo::GPU *g_ps1GPU;
void PS1FsStopStream(); // cdrom_fs.cpp: one CD action at a time

#define CD_SECTOR 2048

volatile uint32_t g_ps1StreamRestarts  = 0;
volatile uint32_t g_ps1StreamDiscarded = 0;

namespace {

enum class StreamState : uint8_t { IDLE, SETLOC, SETFILTER, SETMODE, READ, READ_ACK, PAUSE, PAUSE_ACK };

// Stream bookkeeping. m_written is advanced by the IRQ, m_consumed by the main loop.
struct Stream {
    uint8_t *ring          = nullptr;
    uint32_t ringSectors   = 0;
    uint32_t startLBA      = 0;
    uint32_t total         = 0;
    volatile uint32_t written  = 0; // sectors delivered into the ring
    volatile uint32_t consumed = 0; // sectors released by the consumer
    volatile bool running      = false;
    volatile bool error        = false;
    volatile bool stopping     = false;
    bool open                  = false;
    bool xa                    = false; // STR with XA audio: filter + XA-ADPCM, ReadS, never pause when full
    uint8_t xaFile = 0, xaChannel = 0;
};
Stream s_stream;

class StreamAction : public psyqo::CDRomDevice::Action<StreamState> {
  public:
    StreamAction() : Action("PS1StreamAction") {}
    void start(psyqo::CDRomDevice *device, uint32_t lba) {
        registerMe(device);
        setCallback([](bool ok) {
            if (!ok)
                s_stream.error = true;
            s_stream.running = false;
            eastl::atomic_signal_fence(eastl::memory_order_release);
        });
        setState(StreamState::SETLOC);
        psyqo::MSF msf(lba + 150);
        uint8_t bcd[3];
        msf.toBCD(bcd);
        psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::SETLOC, bcd[0], bcd[1], bcd[2]);
    }
    bool dataReady(const psyqo::CDRomDevice::Response &) override {
        StreamState st = getState();
        bool room = (s_stream.written - s_stream.consumed) < s_stream.ringSectors;
        if (s_stream.xa && st == StreamState::READ_ACK && !room && s_stream.written < s_stream.total && !s_stream.stopping) {
            // XA: keep the drive (and the music) running; this sector is skipped like a late one,
            // the reader resyncs to the next frame. It still counts toward the end of the stream.
            g_ps1StreamDiscarded = g_ps1StreamDiscarded + 1;
            s_stream.total = s_stream.total - 1;
            return false;
        }
        if (st != StreamState::READ_ACK || !room || s_stream.written >= s_stream.total) {
            // Late sector while pausing (or no room): drop it, it will be re-read after SETLOC.
            g_ps1StreamDiscarded = g_ps1StreamDiscarded + 1;
            if (st == StreamState::READ_ACK) {
                setState(StreamState::PAUSE);
                psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::PAUSE);
            }
            return false;
        }
        psyqo::Hardware::CDRom::Ctrl.throwAway();
        psyqo::Hardware::CDRom::DataRequest = 0;
        psyqo::Hardware::CDRom::InterruptControl.throwAway();
        psyqo::Hardware::CDRom::DataRequest = 0x80;
        psyqo::Hardware::SBus::Dev5Ctrl = 0x20943;
        psyqo::Hardware::SBus::ComCtrl  = 0x132c;
        eastl::atomic_signal_fence(eastl::memory_order_acquire);
        uint8_t *dst = s_stream.ring + (s_stream.written % s_stream.ringSectors) * CD_SECTOR;
        DMA_CTRL[DMA_CDROM].MADR = reinterpret_cast<uintptr_t>(dst);
        DMA_CTRL[DMA_CDROM].BCR  = 512 | 0x10000;
        DMA_CTRL[DMA_CDROM].CHCR = 0x11000000; // burst: completes before the CPU continues
        s_stream.written = s_stream.written + 1;
        bool full = (s_stream.written - s_stream.consumed) >= s_stream.ringSectors;
        if ((full && !s_stream.xa) || s_stream.written >= s_stream.total || s_stream.stopping) {
            setState(StreamState::PAUSE);
            psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::PAUSE);
        }
        eastl::atomic_signal_fence(eastl::memory_order_release);
        return false;
    }
    bool complete(const psyqo::CDRomDevice::Response &) override {
        if (getState() != StreamState::PAUSE_ACK)
            return false;
        setSuccess(true);
        return true;
    }
    bool acknowledge(const psyqo::CDRomDevice::Response &) override {
        switch (getState()) {
            case StreamState::SETLOC:
                if (s_stream.xa) {
                    setState(StreamState::SETFILTER);
                    psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::SETFILTER, s_stream.xaFile, s_stream.xaChannel);
                    break;
                }
                setState(StreamState::SETMODE);
                psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::SETMODE, 0x80); // 2x, data
                break;
            case StreamState::SETFILTER:
                setState(StreamState::SETMODE);
                // 2x | XA-ADPCM (audio sectors to the SPU) | XA filter; data sectors stay 2048 bytes
                psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::SETMODE, 0xC8);
                break;
            case StreamState::SETMODE:
                setState(StreamState::READ);
                psyqo::Hardware::CDRom::Command.send(s_stream.xa ? psyqo::Hardware::CDRom::CDL::READS
                                                                 : psyqo::Hardware::CDRom::CDL::READN);
                break;
            case StreamState::READ:
                setState(StreamState::READ_ACK);
                break;
            case StreamState::PAUSE:
                setState(StreamState::PAUSE_ACK);
                break;
            default:
                break;
        }
        return false;
    }
};

StreamAction s_action;

void startRead() {
    s_stream.running = true;
    eastl::atomic_signal_fence(eastl::memory_order_release);
    s_action.start(g_ps1CDRom, s_stream.startLBA + s_stream.written);
}

} // namespace

static bool openStream(uint32_t lba, uint32_t totalSectors, uint8_t *ring, uint32_t ringSectors, bool xa, uint8_t xaFile,
                       uint8_t xaChannel);

bool PS1StreamOpenXA(uint32_t lba, uint32_t totalSectors, uint8_t *ring, uint32_t ringSectors, uint8_t xaFile, uint8_t xaChannel) {
    return openStream(lba, totalSectors, ring, ringSectors, true, xaFile, xaChannel);
}

bool PS1StreamOpen(uint32_t lba, uint32_t totalSectors, uint8_t *ring, uint32_t ringSectors) {
    return openStream(lba, totalSectors, ring, ringSectors, false, 0, 0);
}

static bool openStream(uint32_t lba, uint32_t totalSectors, uint8_t *ring, uint32_t ringSectors, bool xa, uint8_t xaFile,
                       uint8_t xaChannel) {
    if (!g_ps1CDRom || s_stream.running || !ring || !ringSectors)
        return false;
    PS1FsStopStream();
    s_stream.xa        = xa;
    s_stream.xaFile    = xaFile;
    s_stream.xaChannel = xaChannel;
    s_stream.ring        = ring;
    s_stream.ringSectors = ringSectors;
    s_stream.startLBA    = lba;
    s_stream.total       = totalSectors;
    s_stream.written     = 0;
    s_stream.consumed    = 0;
    s_stream.error       = false;
    s_stream.stopping    = false;
    s_stream.open        = true;
    startRead();
    return true;
}

void PS1StreamPump() {
    eastl::atomic_signal_fence(eastl::memory_order_acquire);
    if (!s_stream.open || s_stream.running || s_stream.stopping)
        return;
    if (s_stream.written >= s_stream.total)
        return;
    // Restart once at least half the ring is free, so each seek buys a long read.
    uint32_t used = s_stream.written - s_stream.consumed;
    if (used <= s_stream.ringSectors / 2) {
        g_ps1StreamRestarts = g_ps1StreamRestarts + 1;
        startRead();
    }
}

uint32_t PS1StreamAvailable() {
    eastl::atomic_signal_fence(eastl::memory_order_acquire);
    return s_stream.written - s_stream.consumed;
}

const uint8_t *PS1StreamSector(uint32_t i) {
    return s_stream.ring + ((s_stream.consumed + i) % s_stream.ringSectors) * CD_SECTOR;
}

void PS1StreamConsume(uint32_t n) {
    uint32_t avail = PS1StreamAvailable();
    if (n > avail)
        n = avail;
    s_stream.consumed = s_stream.consumed + n;
    eastl::atomic_signal_fence(eastl::memory_order_release);
}

bool PS1StreamEnded() {
    eastl::atomic_signal_fence(eastl::memory_order_acquire);
    return s_stream.consumed >= s_stream.total;
}

bool PS1StreamError() { return s_stream.error; }

void PS1StreamClose() {
    s_stream.stopping = true;
    eastl::atomic_signal_fence(eastl::memory_order_release);
    // A running action pauses on its next sector (dataReady checks `stopping`); wait for it.
    while (s_stream.running && g_ps1GPU)
        g_ps1GPU->pumpCallbacks();
    s_stream.open = false;
}
