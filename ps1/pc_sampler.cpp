/*
 * PC sampler (`make SAMPLER=1`, harness only): root counter 0 counts system clocks and raises its IRQ at a target that
 * changes pseudo-randomly each time (6,000-14,191 cycles, ~2.8-6.6 hblanks); the handler (BIOS IRQ chain, priority 0,
 * as irq_guard.cpp's filter) records the interrupted PC (COP0 EPC) while g_ps1PcSampleOn is set. A GDB script dumps
 * g_ps1PcSamples; tools/pc_profile.sh maps the PCs to functions. Samples are uniform over CPU time (unlike a debugger
 * break, which PCSX-Redux honours only at exception entry). The IRQ costs ~100 cycles a sample (~2 % of the CPU).
 */
#include <stdint.h>

#include "common/hardware/counters.h"
#include "common/psxlibc/handlers.h"
#include "common/syscalls/syscalls.h"

#define PS1_PC_SAMPLES 8192
volatile uint32_t g_ps1PcSamples[PS1_PC_SAMPLES];
volatile uint32_t g_ps1PcSampleCount = 0;
volatile uint32_t g_ps1PcSampleOn    = 0; // GDB: 1 = record (from a frame the script picks)

static uint32_t s_rng = 0x1234567;
static int pcSampler() {
    volatile uint32_t *istat = reinterpret_cast<volatile uint32_t *>(0xBF801070);
    if (!(*istat & 0x10))
        return 0;
    *istat = ~0x10u; // I_STAT bit 4 = timer 0: 0 acknowledges, 1 leaves the other bits alone
    uint32_t epc;
    __asm__ volatile("mfc0 %0, $14" : "=r"(epc));
    uint32_t n = g_ps1PcSampleCount;
    if (g_ps1PcSampleOn && n < PS1_PC_SAMPLES) {
        g_ps1PcSamples[n]   = epc;
        g_ps1PcSampleCount = n + 1;
    }
    s_rng                = s_rng * 1103515245u + 12345u;
    COUNTERS[0].target = 6000 + ((s_rng >> 16) & 0x1FFF);
    return 0; // the chain goes on
}
static HandlerInfo s_pcSampler = {nullptr, nullptr, pcSampler, 0};

void PS1PcSamplerInstall() {
    syscall_sysEnqIntRP(0, &s_pcSampler);
    COUNTERS[0].target = 8000;
    COUNTERS[0].mode   = (1 << 3) | (1 << 4) | (1 << 6); // reset at target, IRQ at target, repeatedly; system clock
    *reinterpret_cast<volatile uint32_t *>(0xBF801074) |= 0x10; // I_MASK: timer 0
}
