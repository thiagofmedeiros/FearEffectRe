// Motion decoder (MDEC) at 1F801820h: run-length decoding, dequantisation,
// IDCT and YUV to RGB conversion of 16x16 macroblocks, fed and drained by
// DMA channels 0 and 1.
#pragma once

#include <vector>

#include "psx/Machine.h"

namespace fe {

struct Mdec {
    void writeCommand(u32 word);
    void writeControl(u32 word);
    u32 readStatus() const;
    u32 readData();
    void dmaIn(Machine& m, u32 addr, u32 words);
    void dmaOut(Machine& m, u32 addr, u32 words);

    u64 macroblocks = 0;
    bool lazyInput = false;  // read DMA input from RAM when output is collected instead of at transfer time
    std::vector<std::string> trace;  // first few commands and transfers, for the boot report

    void state(StateIO& io, Machine& m);
private:
    void decode(size_t wantBytes);
    u16 code(size_t i) const;
    size_t codeCount() const { return lazy ? lazyCodes : in.size(); }
    bool block(s16* blk, const u8* qt);

    u32 command = 0, control = 0;
    u32 need = 0;  // parameter words still expected
    std::vector<u16> in;
    // Codes sent by DMA are read from RAM when the output is collected: the
    // game keeps filling the buffer after it has started the transfer.
    const u8* lazy = nullptr;
    size_t lazyCodes = 0;
    size_t pos = 0;
    std::vector<u8> out;
    size_t outPos = 0;
    u8 qY[64]{}, qC[64]{};
    s16 scale[64]{};
};

}  // namespace fe
