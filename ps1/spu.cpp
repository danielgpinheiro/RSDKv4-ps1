/*
 * PS1 SPU driver (phase 5). Sequence from ps1-bare-metal src/common/spu.c (initSPU,
 * sendSPURAMData / receiveSPURAMData, resetAllSPUChannels, playSample); register names from
 * nugget common/hardware/spu.h. See spu.hh.
 */
#include "spu.hh"

#include "common/hardware/dma.h"
#include "common/hardware/hwregs.h"
#include "common/hardware/spu.h"

// SPU_CTRL (psx-spx "SPU Control Register"): 15 enable, 14 unmute (DAC), 5-4 transfer mode,
// 0 CD audio enable. SPU_STATUS 5-0 mirror the control bits once applied.
#define CTRL_ENABLE    0x8000
#define CTRL_DAC       0x4000
#define CTRL_XFER_MASK 0x0030
#define CTRL_XFER_DMAW 0x0020
#define CTRL_XFER_DMAR 0x0030
#define CTRL_CD_AUDIO  0x0001

#define DMA_CHUNK_WORDS 16
#define DUMMY_ADDR      (0x80000u - 32u)
#define REVERB_ADDR     (0x80000u - 16u)

volatile uint32_t g_ps1SpuDmaBytes = 0;

namespace {

alignas(4) const uint8_t s_dummyBlock[16] = {0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; // mute + loop

void delayUs(unsigned us) {
    for (volatile unsigned i = 0; i < us * 10; ++i) {
    }
}

void waitDmaDone() {
    while (DMA_CTRL[DMA_SPU].CHCR & (1u << 24)) {
    }
    delayUs(2 * DMA_CHUNK_WORDS); // SPU FIFO drain (~2 us per word)
}

void setTransferMode(uint16_t mode) {
    uint16_t ctrl = SPU_CTRL & ~CTRL_XFER_MASK;
    SPU_CTRL      = ctrl;
    while (SPU_STATUS & CTRL_XFER_MASK) {
    }
    SPU_CTRL = ctrl | mode;
    while ((SPU_STATUS & CTRL_XFER_MASK) != mode) {
    }
}

void startDma(uint32_t addr, const void *data, uint32_t len, bool write) {
    waitDmaDone();
    uint32_t words = (len + 3) / 4;
    uint32_t chunk = words < DMA_CHUNK_WORDS ? words : DMA_CHUNK_WORDS;
    uint32_t count = words < DMA_CHUNK_WORDS ? 1 : words / DMA_CHUNK_WORDS;
    setTransferMode(0);
    SPU_RAM_DTA = (uint16_t)(addr / 8);
    setTransferMode(write ? CTRL_XFER_DMAW : CTRL_XFER_DMAR);
    // DMA waitstates: 0 for writes, 2 for reads (ps1-bare-metal BIU_CTRL_DMA_DELAY).
    SBUS_DEV4_CTRL = (SBUS_DEV4_CTRL & ~(0xfu << 24)) | ((write ? 0u : 2u) << 24);
    DPCR = (DPCR & ~(7u << 16)) | (4u << 16) | (1u << 19);
    DMA_CTRL[DMA_SPU].MADR = (uintptr_t)data;
    DMA_CTRL[DMA_SPU].BCR  = chunk | (count << 16);
    DMA_CTRL[DMA_SPU].CHCR = (write ? 1u : 0u) | (1u << 9) | (1u << 24); // direction, slice mode, start
}

} // namespace

void PS1SpuWrite(uint32_t addr, const void *data, uint32_t len) {
    startDma(addr, data, len, true);
    waitDmaDone();
    g_ps1SpuDmaBytes = g_ps1SpuDmaBytes + len;
}

void PS1SpuRead(uint32_t addr, void *data, uint32_t len) {
    startDma(addr, data, len, false);
    waitDmaDone();
}

void PS1SpuInit() {
    // psyqo's kernel already configured the bus (16-bit, DEV4) and enabled the SPU.
    SPU_CTRL = CTRL_ENABLE;
    while (SPU_STATUS & CTRL_XFER_MASK) {
    }
    SPU_VOL_MAIN_LEFT  = 0x3fff;
    SPU_VOL_MAIN_RIGHT = 0x3fff;
    SPU_REVERB_LEFT    = 0;
    SPU_REVERB_RIGHT   = 0;
    SPU_REVERB_ADDR    = (uint16_t)(REVERB_ADDR / 8);
    SPU_VOL_CD_LEFT    = 0;
    SPU_VOL_CD_RIGHT   = 0;
    SPU_VOL_EXT_LEFT   = 0;
    SPU_VOL_EXT_RIGHT  = 0;

    PS1SpuWrite(DUMMY_ADDR, s_dummyBlock, sizeof(s_dummyBlock));

    for (int v = 0; v < 24; ++v) { // park every voice on the silent looping dummy block
        SPU_VOICES[v].volumeLeft      = 0;
        SPU_VOICES[v].volumeRight     = 0;
        SPU_VOICES[v].sampleRate      = PS1_SPU_PITCH_UNIT;
        SPU_VOICES[v].sampleStartAddr = (uint16_t)(DUMMY_ADDR / 8);
    }
    SPU_PITCH_MOD_LOW  = 0;
    SPU_PITCH_MOD_HIGH = 0;
    SPU_NOISE_EN_LOW   = 0;
    SPU_NOISE_EN_HIGH  = 0;
    SPU_REVERB_EN_LOW  = 0;
    SPU_REVERB_EN_HIGH = 0;
    SPU_KEY_ON_LOW     = 0xffff;
    SPU_KEY_ON_HIGH    = 0x00ff;

    SPU_CTRL = CTRL_ENABLE | CTRL_DAC | CTRL_CD_AUDIO;
}

void PS1SpuVoicePlay(int voice, uint32_t addr, uint32_t rate, uint16_t volL, uint16_t volR) {
    volatile SPUVoice &v = SPU_VOICES[voice];
    v.volumeLeft         = volL & 0x3fff; // bit 15 clear = fixed volume
    v.volumeRight        = volR & 0x3fff;
    v.sampleRate         = (uint16_t)((rate * PS1_SPU_PITCH_UNIT) / 44100);
    v.sampleStartAddr    = (uint16_t)(addr / 8);
    // psx-spx ADSR: 1F801C08h = sustain level 15, decay shift 15, attack shift 0 (instant);
    // 1F801C0Ah = release/sustain rates 0 (ps1-bare-metal playSample).
    v.adsrLo        = 0x00ff;
    v.adsrHi        = 0x0000;
    v.currentVolume = 0x7fff;
    if (voice < 16)
        SPU_KEY_ON_LOW = (uint16_t)(1u << voice);
    else
        SPU_KEY_ON_HIGH = (uint16_t)(1u << (voice - 16));
}

void PS1SpuVoiceStop(uint32_t voiceMask) {
    SPU_KEY_OFF_LOW  = (uint16_t)(voiceMask & 0xffff);
    SPU_KEY_OFF_HIGH = (uint16_t)(voiceMask >> 16);
}

uint16_t PS1SpuVoiceEnvelope(int voice) { return SPU_VOICES[voice].currentVolume; }

void PS1SpuSetCdVolume(uint16_t left, uint16_t right) {
    SPU_VOL_CD_LEFT  = left & 0x7fff;
    SPU_VOL_CD_RIGHT = right & 0x7fff;
}
