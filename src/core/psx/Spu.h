// Sound processing unit: 24 ADPCM voices with ADSR envelopes, 512 KiB of
// sound RAM filled through the transfer FIFO or DMA channel 4, and the
// CD-XA audio input. Reverb, noise and pitch modulation are not implemented.
#pragma once

#include <deque>
#include <vector>

#include "psx/Machine.h"

namespace fe {

struct Spu {
    Spu();

    u16 read(u32 offset) const;                 // offset from 1F801C00h
    void write(Machine& m, u32 offset, u16 value);
    void dma(Machine& m, u32 addr, u32 words, bool toSpu);
    // Appends `frames` stereo sample pairs at 44100 Hz to `out`.
    void generate(Machine& m, size_t frames, std::vector<s16>& out);
    // One 2352-byte CD-XA audio sector.
    void xaSector(const u8* raw);

    u64 keyOns = 0;

private:
    struct Voice {
        u32 addr = 0, repeat = 0;   // in bytes
        u32 counter = 0;            // 12-bit fraction
        s16 block[28]{};
        int pos = 28;               // sample index in block, 28 = need a new block
        s32 hist1 = 0, hist2 = 0;
        s16 prev = 0, cur = 0;
        s32 level = 0;              // envelope 0..7FFF
        int phase = 0;              // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
        u32 wait = 0;
        s32 volL = 0, volR = 0;
    };
    void decodeBlock(Machine& m, int v);
    void envelope(int v);
    u16& reg(u32 offset) { return regs[(offset & 0x3FF) >> 1]; }
    u16 regc(u32 offset) const { return regs[(offset & 0x3FF) >> 1]; }

    std::vector<u8> ram;
    u16 regs[0x200]{};
    Voice voices[24];
    u32 transferAddr = 0;
    u32 endx = 0;
    bool irqFlag = false;
    s32 mainL = 0, mainR = 0;
    std::deque<s16> xa;             // stereo pairs at 44100 Hz
    s32 xaHist[2][2]{};
    s16 xaLast[2]{};
    u32 xaPhase = 0;
};

}  // namespace fe
