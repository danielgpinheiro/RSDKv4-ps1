/*

MIT License

Copyright (c) 2025 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

// psyqo AdvancedPad::readPad for port 1 only (ps1/pad.hh, docs/43 C11): the same SIO0 sequence, delays, ACK timeout and
// multitap handling, without the port loop, the second port and the event callbacks.
#include "pad.hh"

#include <psyqo/hardware/cpu.hh>
#include <psyqo/hardware/sio.hh>
#include <psyqo/kernel.hh>
#include <psyqo/sio0-bus.hh>

using namespace psyqo::Hardware;

namespace {
enum PadType : uint8_t { None = 0xff, Multitap = 0x80 };
inline void busyLoop(unsigned delay) {
    unsigned cycles = 0;
    while (++cycles < delay) asm("");
}
inline void flushRxBuffer() {
    while (SIO::Stat & SIO::Status::STAT_RXRDY) SIO::Data.throwAway();
}
inline void configurePort(uint8_t port) {
    SIO::Ctrl = (port * SIO::Control::CTRL_PORTSEL) | SIO::Control::CTRL_DTR;
    SIO::Baud = 0x88; // 250kHz
    flushRxBuffer();
    SIO::Ctrl |= (SIO::Control::CTRL_TXEN | SIO::Control::CTRL_ACKIRQEN);
    busyLoop(100); // AdvancedPad: about 23us before the first clock pulse
}
inline uint8_t outputDefault(unsigned ticks) { return ticks == 0 || ticks == 2 ? 0x01 : ticks == 1 ? 0x42 : 0x00; }
inline uint8_t outputMultitap(unsigned ticks) {
    uint8_t dataOut = 0x00;
    switch (ticks) {
        case 0:
        case 2: dataOut = 0x01; break;
        case 1:
        case 3:
        case 11:
        case 19:
        case 27: dataOut = 0x42; break;
    }
    switch (ticks) {
        case 2:
        case 11: SIO::Baud = 0x22; break; // 1MHz burst
        case 3: SIO::Baud = 0x88; break;
    }
    return dataOut;
}
inline uint8_t transceive(uint8_t dataOut) {
    SIO::Ctrl |= SIO::Control::CTRL_ERRRES;
    CPU::IReg.clear(CPU::IRQ::Controller);
    SIO::Data = dataOut;
    while (!(SIO::Stat & SIO::Status::STAT_RXRDY));
    return SIO::Data;
}
inline bool waitForAck() {
    int cyclesWaited = 0;
    static constexpr int ackTimeout = 0x137; // ~105us
    while (!(CPU::IReg.isSet(CPU::IRQ::Controller)) && ++cyclesWaited < ackTimeout);
    return cyclesWaited < ackTimeout;
}
} // namespace

void PS1Pad::initialize() {
    __builtin_memset(m_padData, 0xff, sizeof(m_padData));
    SIO::Ctrl = SIO::Control::CTRL_IR;
    SIO::Baud = 0x88;
    SIO::Mode = 0xd;
    SIO::Ctrl = 0;
    psyqo::Kernel::Internal::addOnFrame([this]() { readPad(); });
}

void PS1Pad::readPad() {
    if (psyqo::SIO0Bus::owned()) return; // a memory card transfer owns SIO0 (AdvancedPad's rule)
    static constexpr unsigned padDataWidth = sizeof(PadData);
    uint8_t dataIn, dataOut, devType = PadType::None;
    configurePort(0);
    uint8_t *padData = reinterpret_cast<uint8_t *>(&m_padData[0].packed[0]);
    __builtin_memset(padData, 0xff, sizeof(m_padData[0]));
    for (unsigned ticks = 0, maxTicks = 5; ticks < maxTicks; ticks++) {
        dataOut = devType == PadType::Multitap ? outputMultitap(ticks) : outputDefault(ticks);
        dataIn  = transceive(dataOut);
        if (ticks == 2) {
            if (dataIn == 0x5a || dataIn == 0x00) {
                maxTicks = devType & 0x0f;
                if (!maxTicks) maxTicks = 0x10;
                maxTicks   = (maxTicks * 2) + ticks + 1;
                padData[0] = 0;
            } else {
                maxTicks   = ticks;
                padData[0] = 0xff;
                padData[1] = PadType::None;
            }
        }
        if (devType == PadType::Multitap) {
            const unsigned padIndex = ticks >= 11 ? (ticks - 3) / 8 : 0;
            switch (ticks) {
                case 2: break;
                case 4:
                case 12:
                case 20:
                case 28: padData[(padDataWidth * padIndex) + 0] = dataIn != 0x5a; break;
                case 3:
                case 11:
                case 19:
                case 27: padData[(padDataWidth * padIndex) + 1] = dataIn; break;
                default: padData[(padDataWidth * padIndex) + ((ticks - 3) % 8)] = dataIn;
            }
        } else {
            switch (ticks) {
                case 0: break;
                case 1:
                    devType = dataIn;
                    if (dataIn != PadType::Multitap) padData[1] = dataIn;
                    break;
                case 2: padData[0] = dataIn != 0x5a; break;
                default: padData[ticks - 1] = dataIn;
            }
        }
        if (ticks < (maxTicks - 1)) {
            if (!waitForAck()) {
                for (int padIndex = 0; padIndex < 4; padIndex++) padData[(padDataWidth * padIndex)] = 0x01;
                break;
            }
            while (SIO::Stat & SIO::Status::STAT_ACK);
        }
    }
    SIO::Ctrl = 0;
}
