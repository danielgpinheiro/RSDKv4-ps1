/*
 * Lost-IRQ guard — see irq_guard.hh.
 */
#include "irq_guard.hh"

#include "common/psxlibc/handlers.h"
#include "common/syscalls/syscalls.h"
#include "psyqo/cdrom-device.hh"
#include "psyqo/gpu.hh"
#include "psyqo/hardware/cdrom.hh"
#include "psyqo/hardware/cpu.hh"

extern psyqo::CDRomDevice *g_ps1CDRom;
extern psyqo::GPU *g_ps1GPU;

volatile uint32_t g_ps1CdIrqKicks  = 0;
volatile uint32_t g_ps1DmaIrqKicks = 0;
volatile uint32_t g_ps1CdIrqStale  = 0;
volatile uint32_t g_ps1IrqTestStaleCd = 0; // GDB test hook: > 0 -> make that many stale CD latches (checks cdIrqFilter)

#define GUARD_PERIOD_US 10000 // a lost edge costs at most ~10 ms (plus the wait for the next pump)
#define CD_HINTMSK      0x1f  // psyqo's value (cdrom-device-reset.cpp)
#define DICR_MASTER_EN  0x00800000u
#define DICR_MASTER_IRQ 0x80000000u

// I_MASK as a plain register (psyqo's IRQReg hides the base class's assignment).
static void setIMask(uint32_t v) { psyqo::Hardware::CPU::IMask.access() = v; }

void PS1IrqGuardCheck() {
    using namespace psyqo::Hardware;
    uint32_t mask = CPU::IMask.access();
    // CD-ROM: HINTSTS bits 0-2 = the pending response (psx-spx cdromdrive.md); the drive raises its IRQ
    // while (HINTMSK & HINTSTS) != 0, so turning HINTMSK off and on gives I_STAT a new edge.
    // Only while an action is registered: its handler needs somewhere to deliver the response.
    if (g_ps1CDRom && !g_ps1CDRom->isIdle()) {
        setIMask(mask & ~static_cast<uint32_t>(CPU::IRQ::CDRom));
        CPU::flushWriteQueue();
        if ((CDRom::Cause & 7) && !CPU::IReg.isSet(CPU::IRQ::CDRom)) {
            CDRom::CauseMask = 0;
            CDRom::CauseMask = CD_HINTMSK;
            g_ps1CdIrqKicks  = g_ps1CdIrqKicks + 1;
        }
        setIMask(mask); // as found: a blocking CD action keeps the IRQ masked and polls I_STAT itself
    }
    // Test: let the next response latch while the CD IRQ is masked, consume it here (one sector lost;
    // the stream resyncs) and leave the latch: without cdIrqFilter, psyqo's irq() would then abort.
    if (g_ps1IrqTestStaleCd && g_ps1CDRom && !g_ps1CDRom->isIdle()) {
        setIMask(mask & ~static_cast<uint32_t>(CPU::IRQ::CDRom));
        CPU::flushWriteQueue();
        for (int i = 0; i < 200000 && !CPU::IReg.isSet(CPU::IRQ::CDRom); i++) {
        }
        if (CPU::IReg.isSet(CPU::IRQ::CDRom) && (CDRom::Cause & 7)) {
            CDRom::Cause         = 7;
            g_ps1IrqTestStaleCd = g_ps1IrqTestStaleCd - 1;
        }
        setIMask(mask);
    }
    // DMA: DICR bit 31 is recomputed on every DICR write and sets I_STAT bit 3 on 0 -> 1 (psx-spx
    // dmachannels.md). Writing 0 to the channel flags (bits 24-30) leaves them alone.
    setIMask(mask & ~static_cast<uint32_t>(CPU::IRQ::DMA));
    CPU::flushWriteQueue();
    uint32_t dicr = CPU::DICR;
    if ((dicr & DICR_MASTER_IRQ) && (dicr & DICR_MASTER_EN) && !CPU::IReg.isSet(CPU::IRQ::DMA)) {
        uint32_t keep = dicr & 0x00ffffffu;
        CPU::DICR = keep & ~DICR_MASTER_EN; // bit 31 -> 0
        CPU::DICR = keep;                   // bit 31 -> 1: the edge
        g_ps1DmaIrqKicks = g_ps1DmaIrqKicks + 1;
    }
    setIMask(mask);
}

// Stale CD latches. psyqo's CDRomDevice::irq() loops until the drive has no response left, then
// returns; an interrupt that arrived meanwhile was handled by that loop, but its edge still latched
// I_STAT, so the next exception enters irq() with nothing pending, and irq() aborts ("invoked with
// unknown cause": a hang with the XA audio still playing). A queued response presented right after an
// ack does exactly that, and so does a kick above. This runs first on every exception (BIOS IRQ chain,
// priority 0, before the default handler that delivers psyqo's CD event) and acknowledges such a latch
// the correct way (write-only), so psyqo never sees it. It touches the drive only when its I_STAT bit
// is set (when psyqo's handler would touch it anyway) and puts the drive's register bank back.
static int cdIrqFilter() {
    using namespace psyqo::Hardware;
    constexpr uint32_t cd = static_cast<uint32_t>(CPU::IRQ::CDRom);
    if (!(CPU::IReg.access() & CPU::IMask.access() & cd))
        return 0;
    uint8_t bank    = CDRom::Ctrl & 3;
    uint8_t hintsts = CDRom::Cause;
    if ((hintsts & 7) == 0) {
        if (hintsts & 0x18)
            CDRom::Cause = 0x18; // HCLRCTL: BFEMPT / BFWRDY, which psyqo's irq() doesn't expect alone either
        CPU::IReg.access() = ~cd; // 0 acknowledges, 1 leaves the other bits alone
        g_ps1CdIrqStale = g_ps1CdIrqStale + 1;
    }
    CDRom::Ctrl = bank;
    return 0; // never runs a second function; the chain goes on
}
static HandlerInfo s_cdIrqFilter = {nullptr, nullptr, cdIrqFilter, 0};

void PS1IrqGuardInstall() {
    syscall_sysEnqIntRP(0, &s_cdIrqFilter);
    g_ps1GPU->armPeriodicTimer(GUARD_PERIOD_US, [](uint32_t) { PS1IrqGuardCheck(); });
}
