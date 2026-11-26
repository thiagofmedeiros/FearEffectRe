// Save state contents of each part of the machine.
#include "psx/State.h"

#include <algorithm>

#include "psx/Cdrom.h"
#include "psx/Gpu.h"
#include "psx/Mdec.h"
#include "psx/Spu.h"

namespace fe {

namespace {
constexpr u32 kTag = 0x46455354;  // "FEST"
}

void Machine::state(StateIO& io) {
    io.tag(kTag + 1);
    io(cpu.r);
    io(cpu.hi);
    io(cpu.lo);
    io(cpu.pc);
    io(cpu.cop0);
    io(cpu.gteData);
    io(cpu.gteCtrl);
    io.bytes(ram.get(), kRamSize);
    io(scratch);
    io(ioShadow);
    io(clock);
    io(nextVblank);
    io(frame);
    io(iStat);
    io(iMask);
    io(entryIntHook);
    io(intChains);
    io(events);
    io(padBuf);
    io(padLen);
    io(padStarted);
    io(heapBase);
    io(heapSize);
    io(randSeed);
    u64 files = cardFiles.size();
    io(files);
    if (io.loading()) cardFiles.resize(static_cast<size_t>(files));
    for (CardFile& f : cardFiles) {
        io(f.path);
        io(f.pos);
        io(f.open);
        io(f.noWait);
    }
    io(cardListing);
    io(cardListPos);
    io(cardPending);
    io(ttyLine);
    io(audioClock);
    io(sio);
    io(dma);
    io(dpcr);
    io(dicr);
    io(gpuRead);
    io(gp0Words);
    io(gp1Words);
    io(timers);
    io.tag(kTag + 2);
    cd->state(io);
    io.tag(kTag + 3);
    gpu->state(io);
    io.tag(kTag + 4);
    mdec->state(io, *this);
    io.tag(kTag + 5);
    spu->state(io);
    io.tag(kTag + 6);
}

std::vector<u8> Machine::saveState() {
    StateIO io;
    state(io);
    return io.data();
}

void Machine::loadState(const std::vector<u8>& data) {
    StateIO io(data);
    state(io);
    // resume with an empty slice: the first instruction ends it, and the
    // machine then schedules the next one from the restored clock
    cpu.cycles = 0;
    sliceLen = 0;
    inException = false;
    hostFrames = 0;
    safePointWanted = false;
    audio.clear();
}

void Cdrom::state(StateIO& io) {
    io(index);
    io(ifl);
    io(ie);
    io(stat);
    io(mode);
    io(filterFile);
    io(filterChan);
    io(busy);
    io(reading);
    io(setlocPending);
    io(setloc);
    io(lba);
    io(nextSector);
    io(params);
    io(response);
    io(responsePos);
    u64 n = queue.size();
    io(n);
    if (io.loading()) queue.resize(static_cast<size_t>(n));
    for (Pending& p : queue) {
        io(p.time);
        io(p.irq);
        io(p.bytes);
    }
    io(sectorBuf);
    io(data);
    io(dataPos);
    io(fresh);
    io(lidOpenUntil);
    io(lidWasOpen);
    io(header);
}

void Gpu::state(StateIO& io) {
    io(vram);
    io(displayEnabled);
    io(dispX);
    io(dispY);
    io(displayMode);
    io(dmaDirection);
    io(cmd);
    io(cmdNeed);
    io(polyline);
    io(tx);
    io(ty);
    io(tw);
    io(th);
    io(tcx);
    io(tcy);
    io(loading);
    io(storing);
    io(readLatch);
    io(texpage);
    io(tpx);
    io(tpy);
    io(blend);
    io(depth);
    io(clutX);
    io(clutY);
    io(twMaskX);
    io(twMaskY);
    io(twOffX);
    io(twOffY);
    io(areaX1);
    io(areaY1);
    io(areaX2);
    io(areaY2);
    io(offX);
    io(offY);
    io(setMask);
    io(checkMask);
    io(hRange);
    io(vRange);
    int fb[4] = {fbX1, fbY1, fbX2, fbY2};
    io(fb);
    if (!io.loading()) return;
    // The high-resolution copy is not saved: it is rebuilt from video memory
    // (enlarged pixel by pixel), and the game redraws it within a frame.
    // Enlarged movie pictures are made again when next uploaded.
    fbX1 = 1024;
    fbY1 = 512;
    fbX2 = fbY2 = -1;
    if (scale > 1) growFrameBuffer(fb[0], fb[1], fb[2], fb[3]);
    std::fill(videoState.begin(), videoState.end(), u8{0});
    videoDirty.clear();
    videoBreak = true;
}

void Mdec::state(StateIO& io, Machine& m) {
    io(command);
    io(control);
    io(need);
    io(in);
    s64 lazyAt = lazy ? lazy - m.ram.get() : -1;
    io(lazyAt);
    if (io.loading()) lazy = lazyAt >= 0 ? m.ram.get() + lazyAt : nullptr;
    io(lazyCodes);
    io(pos);
    io(out);
    io(outPos);
    io(qY);
    io(qC);
    io(scale);
}

void Spu::state(StateIO& io) {
    io(ram);
    io(regs);
    io(voices);
    io(transferAddr);
    io(endx);
    io(irqFlag);
    io(mainL);
    io(mainR);
    io(xa);
    io(xaHist);
    io(xaLast);
    io(xaPhase);
}

}  // namespace fe
