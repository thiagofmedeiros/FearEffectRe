// High-level replacements for the PlayStation BIOS functions the game calls
// through the A0h/B0h/C0h vectors, plus syscall and break.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "Machine.h"

namespace fe {

namespace {

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

// printf against guest memory: a1..a3 then the caller's stack from sp+16.
std::string formatGuest(Machine& m, Cpu& c, u32 fmtAddr, int firstArg) {
    std::string fmt = m.cstring(fmtAddr, 1024), out;
    int argi = firstArg;
    auto next = [&]() -> u32 {
        int i = argi++;
        return i < 4 ? c.r[4 + i] : m.read(c.r[29] + 4 * static_cast<u32>(i), 4);
    };
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        std::string spec = "%";
        ++i;
        while (i < fmt.size() && std::string("-+ #0123456789.lh").find(fmt[i]) != std::string::npos) {
            if (fmt[i] != 'l' && fmt[i] != 'h') spec.push_back(fmt[i]);
            ++i;
        }
        if (i >= fmt.size()) break;
        char conv = fmt[i];
        char buf[128];
        switch (conv) {
            case '%': out.push_back('%'); break;
            case 's': out += m.cstring(next(), 512); break;
            case 'c': out.push_back(static_cast<char>(next())); break;
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p':
                spec.push_back(conv == 'p' ? 'x' : conv);
                std::snprintf(buf, sizeof buf, spec.c_str(), next());
                out += buf;
                break;
            default: out.push_back(conv); break;
        }
    }
    return out;
}

// "bu00:NAME" -> host path, empty when that slot has no card.
std::string cardPath(const Machine& m, const std::string& name) {
    size_t colon = name.find(':');
    if (m.cardDir.empty() || colon == std::string::npos || name.rfind("bu0", 0) != 0) return {};
    std::string file;
    for (char ch : name.substr(colon + 1))
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') file.push_back(ch);
    return file.empty() ? std::string{} : m.cardDir + "/" + file;
}

bool wildcard(const std::string& pattern, const std::string& name) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '*') return true;
        if (i >= name.size() || (pattern[i] != '?' && pattern[i] != name[i])) return false;
    }
    return pattern.size() == name.size();
}

u32 fillDirEntry(Machine& m, u32 entry) {
    namespace fs = std::filesystem;
    if (m.cardListPos >= m.cardListing.size()) return 0;
    const std::string& name = m.cardListing[m.cardListPos++];
    std::error_code ec;
    u32 size = static_cast<u32>(fs::file_size(fs::path(m.cardDir) / name, ec));
    for (u32 i = 0; i < 20; ++i) m.write(entry + i, 1, i < name.size() ? static_cast<u8>(name[i]) : 0);
    m.write(entry + 0x14, 4, 0x50);
    m.write(entry + 0x18, 4, size);
    m.write(entry + 0x1C, 4, 0);
    m.write(entry + 0x20, 4, 0);
    return entry;
}

void tty(Machine& m, const std::string& text) {
    for (char ch : text) {
        if (ch == '\n') {
            m.note("tty: " + m.ttyLine);
            m.ttyLine.clear();
        } else if (ch != '\r') {
            m.ttyLine.push_back(ch);
        }
    }
}

}  // namespace

void Machine::deliverEvent(Cpu& c, u32 cls, u32 spec) {
    for (auto& ev : events) {
        if (ev.status != 0x2000 || ev.cls != cls || ev.spec != spec) continue;
        if (ev.mode == 0x1000 && ev.func) {
            u32 ra = c.r[31];
            c.r[31] = 0xBFC0FFF0;
            call(c, ev.func);
            c.r[31] = ra;
        } else {
            ev.status = 0x4000;
        }
    }
}

void Machine::biosCall(Cpu& c, u32 vector) {
    u32 fn = c.r[9] & 0xFF;
    u32 a0 = c.r[4], a1 = c.r[5], a2 = c.r[6], a3 = c.r[7];
    u32& v0 = c.r[2];
    u32 id = (vector << 8) | fn;
    if (logBios) note("bios " + hex(id) + " a0=" + hex(a0) + " a1=" + hex(a1) + " ra=" + hex(c.r[31]));

    auto eventIndex = [&](u32 handle) -> Event* {
        u32 i = handle & 0xFFFF;
        return (handle >> 24) == 0xF1 && i < events.size() ? &events[i] : nullptr;
    };

    switch (id) {
        // ---- A0: C library ------------------------------------------------
        case 0xA013:  // setjmp
            write(a0, 4, c.r[31]);
            write(a0 + 4, 4, c.r[29]);
            write(a0 + 8, 4, c.r[30]);
            for (u32 i = 0; i < 8; ++i) write(a0 + 12 + 4 * i, 4, c.r[16 + i]);
            write(a0 + 44, 4, c.r[28]);
            v0 = 0;
            break;
        case 0xA015: {  // strcat
            u32 d = a0;
            while (read(d, 1)) ++d;
            for (u32 s = a1;; ++s, ++d) {
                u32 ch = read(s, 1);
                write(d, 1, ch);
                if (!ch) break;
            }
            v0 = a0;
            break;
        }
        case 0xA017: case 0xA018: {  // strcmp, strncmp
            u32 n = id == 0xA018 ? a2 : 0xFFFFFFFF;
            v0 = 0;
            for (u32 i = 0; i < n; ++i) {
                u32 x = read(a0 + i, 1), y = read(a1 + i, 1);
                if (x != y) {
                    v0 = x - y;
                    break;
                }
                if (!x) break;
            }
            break;
        }
        case 0xA019: {  // strcpy
            for (u32 i = 0;; ++i) {
                u32 ch = read(a1 + i, 1);
                write(a0 + i, 1, ch);
                if (!ch) break;
            }
            v0 = a0;
            break;
        }
        case 0xA01A: {  // strncpy
            bool end = false;
            for (u32 i = 0; i < a2; ++i) {
                u32 ch = end ? 0 : read(a1 + i, 1);
                if (!ch) end = true;
                write(a0 + i, 1, ch);
            }
            v0 = a0;
            break;
        }
        case 0xA01B: {  // strlen
            u32 n = 0;
            while (read(a0 + n, 1)) ++n;
            v0 = n;
            break;
        }
        case 0xA025:  // toupper
            v0 = (a0 >= 'a' && a0 <= 'z') ? a0 - 32 : a0;
            break;
        case 0xA027:  // bcopy(src, dst, len)
            for (u32 i = 0; i < a2; ++i) write(a1 + i, 1, read(a0 + i, 1));
            break;
        case 0xA028:  // bzero
            for (u32 i = 0; i < a1; ++i) write(a0 + i, 1, 0);
            break;
        case 0xA02A:  // memcpy
            for (u32 i = 0; i < a2; ++i) write(a0 + i, 1, read(a1 + i, 1));
            v0 = a0;
            break;
        case 0xA02B:  // memset
            for (u32 i = 0; i < a2; ++i) write(a0 + i, 1, a1);
            v0 = a0;
            break;
        case 0xA02E: {  // memchr
            v0 = 0;
            for (u32 i = 0; i < a2; ++i)
                if (read(a0 + i, 1) == (a1 & 0xFF)) {
                    v0 = a0 + i;
                    break;
                }
            break;
        }
        case 0xA02F:  // rand
            randSeed = randSeed * 0x41C64E6D + 0x3039;
            v0 = (randSeed >> 16) & 0x7FFF;
            break;
        case 0xA030: randSeed = a0; break;  // srand
        case 0xA039: heapBase = a0; heapSize = a1; break;  // InitHeap
        case 0xA03F: tty(*this, formatGuest(*this, c, a0, 1)); break;  // printf
        case 0xA044: break;  // FlushCache
        case 0xA049: write(0x1F801810, 4, a0); break;  // GPU_cw
        case 0xA070: break;  // _bu_init
        case 0xA072: break;  // _96_remove
        case 0xA09F: break;  // SetMem
        case 0xA0AB: case 0xA0AC:  // _card_info, _card_load: card present in slot 1 only
            deliverEvent(c, 0xF4000001, (!cardDir.empty() && (a0 & 0x10) == 0) ? 0x0004 : 0x0100);
            v0 = 1;
            break;

        // ---- B0: kernel ---------------------------------------------------
        case 0xB007: deliverEvent(c, a0, a1); break;
        case 0xB008: {  // OpenEvent
            v0 = 0xFFFFFFFF;
            for (u32 i = 0; i < events.size(); ++i) {
                if (events[i].status) continue;
                events[i] = Event{a0, a1, a2, a3, 0x1000};
                v0 = 0xF1000000 | i;
                break;
            }
            break;
        }
        case 0xB009: if (Event* e = eventIndex(a0)) e->status = 0; v0 = 1; break;
        case 0xB00A: {  // WaitEvent
            Event* e = eventIndex(a0);
            v0 = 0;
            if (!e) break;
            for (int spin = 0; spin < 200000 && e->status != 0x4000; ++spin) {
                c.cycles -= 2048;
                if (c.cycles <= 0) onSlice(c);
            }
            if (e->status == 0x4000) {
                e->status = 0x2000;
                v0 = 1;
            }
            break;
        }
        case 0xB00B: {  // TestEvent
            Event* e = eventIndex(a0);
            v0 = 0;
            if (e && e->status == 0x4000) {
                e->status = 0x2000;
                v0 = 1;
            }
            break;
        }
        case 0xB00C: if (Event* e = eventIndex(a0)) e->status = 0x2000; v0 = 1; break;
        case 0xB00D: if (Event* e = eventIndex(a0)) e->status = 0x1000; v0 = 1; break;
        case 0xB012:  // InitPAD
            padBuf[0] = a0; padLen[0] = a1; padBuf[1] = a2; padLen[1] = a3;
            v0 = 2;
            break;
        case 0xB013: padStarted = true; v0 = 2; break;
        case 0xB014: padStarted = false; break;
        case 0xB015: v0 = 0; break;  // PAD_init
        case 0xB017: throw ExceptionReturn{};
        case 0xB018: entryIntHook = 0; break;
        case 0xB019: entryIntHook = a0; break;
        case 0xB032: {  // open(name, mode)
            namespace fs = std::filesystem;
            std::string path = cardPath(*this, cstring(a0));
            v0 = 0xFFFFFFFF;
            if (path.empty()) break;
            std::error_code ec;
            if (a1 & 0x200) {  // create with (mode >> 16) blocks of 8192 bytes
                if (fs::exists(path, ec)) break;
                std::ofstream f(path, std::ios::binary);
                std::vector<char> zero(static_cast<size_t>(a1 >> 16) * 8192, 0);
                f.write(zero.data(), static_cast<std::streamsize>(zero.size()));
                if (!f) break;
            } else if (!fs::exists(path, ec)) {
                break;
            }
            size_t slot = 0;
            while (slot < cardFiles.size() && cardFiles[slot].open) ++slot;
            if (slot == cardFiles.size()) cardFiles.emplace_back();
            cardFiles[slot] = CardFile{path, 0, true};
            v0 = static_cast<u32>(slot) + 2;
            deliverEvent(c, 0xF0000011, 0x0004);
            break;
        }
        case 0xB033: {  // lseek(fd, offset, whence)
            v0 = 0xFFFFFFFF;
            if (a0 < 2 || a0 - 2 >= cardFiles.size() || !cardFiles[a0 - 2].open) break;
            CardFile& f = cardFiles[a0 - 2];
            f.pos = a2 == 1 ? f.pos + a1 : a1;
            v0 = f.pos;
            break;
        }
        case 0xB034: case 0xB035: {  // read(fd, buf, n), write(fd, buf, n)
            v0 = 0xFFFFFFFF;
            if (a0 < 2 || a0 - 2 >= cardFiles.size() || !cardFiles[a0 - 2].open) break;
            CardFile& f = cardFiles[a0 - 2];
            std::vector<char> buf(a2);
            if (id == 0xB034) {
                std::ifstream in(f.path, std::ios::binary);
                in.seekg(f.pos);
                in.read(buf.data(), static_cast<std::streamsize>(a2));
                for (u32 i = 0; i < a2; ++i) write(a1 + i, 1, static_cast<u8>(buf[i]));
            } else {
                for (u32 i = 0; i < a2; ++i) buf[i] = static_cast<char>(read(a1 + i, 1));
                std::fstream out(f.path, std::ios::binary | std::ios::in | std::ios::out);
                out.seekp(f.pos);
                out.write(buf.data(), static_cast<std::streamsize>(a2));
            }
            f.pos += a2;
            v0 = a2;
            deliverEvent(c, 0xF0000011, 0x0004);
            break;
        }
        case 0xB036:  // close
            if (a0 >= 2 && a0 - 2 < cardFiles.size()) cardFiles[a0 - 2].open = false;
            v0 = a0;
            break;
        case 0xB03F: tty(*this, cstring(a0, 1024)); break;  // puts
        case 0xB041: v0 = cardDir.empty() ? 0 : 1; break;  // format
        case 0xB045: {  // erase(name)
            std::string path = cardPath(*this, cstring(a0));
            std::error_code ec;
            v0 = !path.empty() && std::filesystem::remove(path, ec) ? 1 : 0;
            break;
        }
        case 0xB042: {  // firstfile(pattern, entry)
            namespace fs = std::filesystem;
            std::string pattern = cstring(a0);
            cardListing.clear();
            cardListPos = 0;
            v0 = 0;
            size_t colon = pattern.find(':');
            if (cardDir.empty() || colon == std::string::npos || pattern.rfind("bu0", 0) != 0) {
                deliverEvent(c, 0xF0000011, 0x0100);  // no card: timeout
                break;
            }
            pattern = pattern.substr(colon + 1);
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(cardDir, ec))
                if (e.is_regular_file() && wildcard(pattern, e.path().filename().string()))
                    cardListing.push_back(e.path().filename().string());
            std::sort(cardListing.begin(), cardListing.end());
            v0 = fillDirEntry(*this, a1);
            // the card library waits for the hardware event when nothing matched
            deliverEvent(c, 0xF0000011, 0x0004);
            break;
        }
        case 0xB043: v0 = fillDirEntry(*this, a0); break;  // nextfile(entry)
        case 0xB04A: case 0xB04B: case 0xB04C: break;  // InitCARD, StartCARD, StopCARD
        case 0xB04E: deliverEvent(c, 0xF0000011, 0x0004); v0 = 1; break;  // _card_write
        case 0xB050: v0 = 0; break;  // _new_card
        case 0xB056: v0 = 0x674; break;  // GetC0Table (libraries patch the kernel through it)
        case 0xB057: v0 = 0x874; break;  // GetB0Table
        case 0xB05B: v0 = 0; break;  // ChangeClearPad

        // ---- C0 -------------------------------------------------------------
        case 0xC002:  // SysEnqIntRP(priority, element)
            write(a1, 4, intChains[a0 & 3]);
            intChains[a0 & 3] = a1;
            v0 = 0;
            break;
        case 0xC003: {  // SysDeqIntRP
            u32 prio = a0 & 3;
            if (intChains[prio] == a1) {
                intChains[prio] = read(a1, 4);
            } else {
                for (u32 el = intChains[prio], guard = 0; el && guard < 64; ++guard) {
                    u32 next = read(el, 4);
                    if (next == a1) {
                        write(el, 4, read(a1, 4));
                        break;
                    }
                    el = next;
                }
            }
            v0 = 0;
            break;
        }
        case 0xC00A: v0 = 0; break;  // ChangeClearRCnt

        default:
            note("unimplemented bios call " + hex(id) + " a0=" + hex(a0) + " a1=" + hex(a1) + " ra=" + hex(c.r[31]));
            v0 = 0;
            break;
    }
}

void opSyscall(Cpu& c, u32 pc) {
    Machine& m = *c.m;
    switch (c.r[4]) {
        case 0: break;
        case 1:  // EnterCriticalSection
            // the BIOS clears IEp/IM2 in the saved SR; after its rfe that is IEc/IM2
            c.r[2] = (c.cop0[12] & 0x401) == 0x401;
            c.cop0[12] &= ~0x401u;
            break;
        case 2:  // ExitCriticalSection
            c.cop0[12] |= 0x401;
            m.requestSlice(c);
            break;
        default:
            m.note("unimplemented syscall " + hex(c.r[4]) + " at " + hex(pc));
            break;
    }
}

void opBreak(Cpu& c, u32 pc, u32 word) {
    (void)c;
    throw Stop{"break " + hex((word >> 6) & 0xFFFFF) + " at " + hex(pc)};
}

u32 mfc0(Cpu& c, u32 reg) { return c.cop0[reg]; }

void mtc0(Cpu& c, u32 reg, u32 value) {
    c.cop0[reg] = value;
    if (reg == 12) c.m->requestSlice(c);
}

void rfe(Cpu& c) { c.cop0[12] = (c.cop0[12] & ~0xFu) | ((c.cop0[12] >> 2) & 0xF); }

}  // namespace fe
