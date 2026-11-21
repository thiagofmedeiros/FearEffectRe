#include "Cdrom.h"

#include "Spu.h"

#include <algorithm>
#include <cstdio>

namespace fe {

namespace {
void trace(Machine& m, int& left, const char* what, u32 a, u32 b) {
    if (left <= 0) return;
    --left;
    char text[96];
    std::snprintf(text, sizeof text, "cd %-8s %02x %02x  t=%llu pc=%08x istat=%x sr=%x", what, a, b,
                  static_cast<unsigned long long>(m.now()), m.cpu.pc, m.iStat, m.cpu.cop0[12]);
    m.log.push_back(text);
}
constexpr u64 kAckDelay = 50401;            // typical first response
constexpr u64 kCompleteDelay = 4 * 50401;   // second response of short commands
constexpr u64 kSeekDelay = kCpuHz / 60;     // not modelled by distance yet

u8 bcd(u32 v) { return static_cast<u8>((v / 10) * 16 + v % 10); }
u32 unbcd(u8 v) { return (v >> 4) * 10u + (v & 15); }
}  // namespace

u64 Cdrom::nextEvent() const {
    if (ifl) return ~0ull;  // nothing can be delivered until the game acknowledges
    u64 t = ~0ull;
    if (!queue.empty()) t = queue.front().time;
    else if (reading) t = nextSector;
    return t;
}

void Cdrom::deliver(Machine& m, u8 irq, std::vector<u8> bytes) {
    trace(m, traceLeft, "deliver", irq, bytes.empty() ? 0 : bytes[0]);
    response = std::move(bytes);
    responsePos = 0;
    ifl = irq;
    if (ie & ifl) m.raise(kIrqCdrom);
}

void Cdrom::tick(Machine& m) {
    if (ifl) return;
    if (!queue.empty()) {
        if (queue.front().time > m.clock) return;
        Pending p = std::move(queue.front());
        queue.pop_front();
        if (p.irq == 3 || p.irq == 5) busy = false;
        deliver(m, p.irq, std::move(p.bytes));
        return;
    }
    if (!reading || nextSector > m.clock) return;

    u8 raw[2352];
    u64 interval = kCpuHz / ((mode & 0x80) ? 150 : 75);
    nextSector = std::max(nextSector + interval, m.clock + interval / 4);
    if (!disc->readRawSector(lba, raw)) {
        reading = false;
        stat &= ~0x20;
        deliver(m, 5, {static_cast<u8>(stat | 0x04), 0x04});
        m.note("cdrom: read past end of disc at lba " + std::to_string(lba));
        return;
    }
    ++lba;
    ++sectorsRead;
    std::copy(raw + 12, raw + 20, header);
    // XA-ADPCM sectors go to the audio decoder instead of the data path.
    u8 submode = raw[18];
    if ((mode & 0x40) && (submode & 0x44) == 0x44) {
        bool match = !(mode & 0x08) || (raw[16] == filterFile && raw[17] == filterChan);
        if (match) {
            ++xaSectors;
            m.spu->xaSector(raw);
        }
        return;
    }
    if ((mode & 0x08) && (submode & 0x44) == 0x44) return;
    if (mode & 0x20) sectorBuf.assign(raw + 12, raw + 12 + 2340);
    else sectorBuf.assign(raw + 24, raw + 24 + 2048);
    fresh = true;
    deliver(m, 1, {stat});
}

void Cdrom::insert(Machine& m, std::shared_ptr<DiscImage> image) {
    disc = std::move(image);
    lidOpenUntil = m.now() + 2ull * kCpuHz;
    lidWasOpen = true;
    reading = false;
    stat = 0x10;
    queue.clear();
}

void Cdrom::command(Machine& m, u8 cmd) {
    ++commands;
    trace(m, traceLeft, "command", cmd, params.empty() ? 0 : params[0]);
    u64 now = m.now();
    busy = true;
    // a new command replaces responses of the previous one that are still due
    queue.clear();
    auto ack = [&](std::vector<u8> bytes) { push(now + kAckDelay, 3, std::move(bytes)); };
    auto complete = [&](u64 delay, std::vector<u8> bytes) { push(now + kAckDelay + delay, 2, std::move(bytes)); };
    if (lidWasOpen) {
        bool open = now < lidOpenUntil;
        if (cmd == 0x01) {  // Getstat reports the open lid once, then the motor spins up
            ack({static_cast<u8>(open ? 0x10 : 0x12)});
            if (!open) {
                lidWasOpen = false;
                stat = 0x02;
            }
            params.clear();
            m.requestSlice(m.cpu);
            return;
        }
        if (open && cmd != 0x19) {  // drive door open
            push(now + kAckDelay, 5, {0x11, 0x08});
            params.clear();
            m.requestSlice(m.cpu);
            return;
        }
        if (!open) {
            lidWasOpen = false;
            stat = 0x02;
        }
    }

    switch (cmd) {
        case 0x01: ack({stat}); break;  // Getstat
        case 0x02:                      // Setloc
            if (params.size() >= 3) {
                u32 msf = unbcd(params[0]) * 60 * 75 + unbcd(params[1]) * 75 + unbcd(params[2]);
                setloc = msf >= 150 ? msf - 150 : 0;
                setlocPending = true;
            }
            ack({stat});
            break;
        case 0x06: case 0x1B:  // ReadN, ReadS
            if (setlocPending) {
                lba = setloc;
                setlocPending = false;
            }
            reading = true;
            stat = 0x22;
            nextSector = now + kAckDelay + kSeekDelay;
            ack({stat});
            break;
        case 0x07: stat |= 0x02; ack({stat}); complete(kCompleteDelay, {stat}); break;  // MotorOn
        case 0x08:  // Stop
            reading = false;
            ack({stat});
            stat = 0x00;
            complete(kCompleteDelay, {stat});
            break;
        case 0x09:  // Pause
            ack({stat});
            reading = false;
            stat = 0x02;
            complete(kCompleteDelay, {stat});
            break;
        case 0x0A:  // Init
            reading = false;
            mode = 0x20;
            stat = 0x02;
            ack({stat});
            complete(kCompleteDelay, {stat});
            break;
        case 0x0B: case 0x0C: ack({stat}); break;  // Mute, Demute
        case 0x0D:  // Setfilter
            if (params.size() >= 2) {
                filterFile = params[0];
                filterChan = params[1];
            }
            ack({stat});
            break;
        case 0x0E:  // Setmode
            if (!params.empty()) mode = params[0];
            ack({stat});
            break;
        case 0x0F: ack({stat, mode, 0, filterFile, filterChan}); break;  // Getparam
        case 0x10: ack(std::vector<u8>(header, header + 8)); break;      // GetlocL
        case 0x11: {  // GetlocP
            u32 rel = lba, abs = lba + 150;
            ack({1, 1, bcd(rel / 4500), bcd(rel / 75 % 60), bcd(rel % 75), bcd(abs / 4500), bcd(abs / 75 % 60),
                 bcd(abs % 75)});
            break;
        }
        case 0x13: ack({stat, 1, 1}); break;  // GetTN: one data track
        case 0x14: {                          // GetTD
            u32 pos = (!params.empty() && params[0] == 0) ? disc->sectorCount() + 150 : 150;
            ack({stat, bcd(pos / 4500), bcd(pos / 75 % 60)});
            break;
        }
        case 0x15: case 0x16:  // SeekL, SeekP
            if (setlocPending) {
                lba = setloc;
                setlocPending = false;
            }
            reading = false;
            stat = 0x02;
            ack({stat});
            complete(kSeekDelay, {stat});
            break;
        case 0x19:  // Test
            if (!params.empty() && params[0] == 0x20) ack({0x94, 0x09, 0x19, 0xC0});
            else ack({stat});
            break;
        case 0x1A:  // GetID: licensed NTSC-U disc
            ack({stat});
            complete(kCompleteDelay, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'});
            break;
        case 0x1E: ack({stat}); complete(kCpuHz / 2, {stat}); break;  // ReadTOC
        default: {
            char text[48];
            std::snprintf(text, sizeof text, "cdrom: unimplemented command %02x", cmd);
            m.note(text);
            push(now + kAckDelay, 5, {static_cast<u8>(stat | 0x01), 0x40});
            break;
        }
    }
    params.clear();
    m.requestSlice(m.cpu);
}

u8 Cdrom::read(Machine& m, u32 port) {
    (void)m;
    switch (port) {
        case 0: {
            u8 v = index;
            if (params.empty()) v |= 0x08;
            if (params.size() < 16) v |= 0x10;
            if (responsePos < response.size()) v |= 0x20;
            if (dataPos < data.size()) v |= 0x40;
            if (busy) v |= 0x80;
            return v;
        }
        case 1: return responsePos < response.size() ? response[responsePos++] : 0;
        case 2: return dataPos < data.size() ? data[dataPos++] : 0;
        default: return static_cast<u8>(0xE0 | ((index & 1) ? ifl : ie));
    }
}

void Cdrom::write(Machine& m, u32 port, u8 value) {
    if (port) trace(m, traceLeft, "write", port * 4 + index, value);
    switch (port * 4 + index) {
        case 0: case 1: case 2: case 3: index = value & 3; break;
        case 4: command(m, value); break;
        case 8: if (params.size() < 16) params.push_back(value); break;
        case 9:
            ie = value & 0x1F;
            if (ie & ifl) m.raise(kIrqCdrom);
            break;
        case 12:  // request register
            if (value & 0x80) {
                if (fresh || dataPos >= data.size()) {
                    data = sectorBuf;
                    dataPos = 0;
                    fresh = false;
                }
            } else {
                data.clear();
                dataPos = 0;
            }
            break;
        case 13:  // interrupt flag: writing 1 acknowledges
            ifl &= ~(value & 0x1F);
            if (value & 0x40) params.clear();
            if (!ifl) {
                // leave the game a moment between consecutive interrupts
                if (!queue.empty()) queue.front().time = std::max(queue.front().time, m.now() + 2000);
                m.requestSlice(m.cpu);
            }
            break;
        default: break;  // audio volume and sound map registers
    }
}

void Cdrom::dmaRead(Machine& m, u32 addr, u32 bytes) {
    if (traceLeft > 0) {
        char text[96];
        std::snprintf(text, sizeof text, "cd dma      addr=%06x bytes=%u fifo=%zu/%zu dicr=%08x t=%llu", addr, bytes, dataPos, data.size(), m.dicr,
                      static_cast<unsigned long long>(m.now()));
        m.log.push_back(text);
        --traceLeft;
    }
    for (u32 i = 0; i < bytes; ++i) {
        u8 v = dataPos < data.size() ? data[dataPos++] : 0;
        m.ram[(addr + i) & (kRamSize - 1)] = v;
    }
}

}  // namespace fe
