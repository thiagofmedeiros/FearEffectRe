#include "DiscImage.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace fe {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

std::filesystem::path binFromCue(const std::filesystem::path& cue) {
    std::ifstream in(cue);
    std::string line;
    while (std::getline(in, line)) {
        if (upper(trim(line)).rfind("FILE", 0) != 0) continue;
        size_t a = line.find('"'), b = line.rfind('"');
        if (a == std::string::npos || b <= a) break;
        return cue.parent_path() / line.substr(a + 1, b - a - 1);
    }
    throw std::runtime_error("no FILE entry in " + cue.string());
}

}  // namespace

DiscImage::DiscImage(const std::string& path) {
    std::filesystem::path p(path);
    if (upper(p.extension().string()) == ".CUE") p = binFromCue(p);
    file_.open(p, std::ios::binary);
    if (!file_) throw std::runtime_error("cannot open " + p.string());

    static const std::uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    std::uint8_t head[12]{};
    file_.read(reinterpret_cast<char*>(head), sizeof head);
    sectorSize_ = std::memcmp(head, sync, sizeof sync) == 0 ? 2352 : 2048;
    sectors_ = static_cast<std::uint32_t>(std::filesystem::file_size(p) / sectorSize_);

    std::uint8_t pvd[2048];
    if (!readSector(16, pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0)
        throw std::runtime_error("no ISO9660 volume in " + p.string());
    volumeId_ = trim(std::string(reinterpret_cast<char*>(pvd + 40), 32));
    walk(le32(pvd + 156 + 2), le32(pvd + 156 + 10), "", 0);
}

bool DiscImage::readRawSector(std::uint32_t lba, std::uint8_t* out) {
    if (lba >= sectors_) return false;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(lba) * sectorSize_);
    if (sectorSize_ == 2352) {
        file_.read(reinterpret_cast<char*>(out), 2352);
        return static_cast<bool>(file_);
    }
    std::memset(out, 0, 2352);
    std::memset(out + 1, 0xFF, 10);
    std::uint32_t msf = lba + 150;
    auto bcd = [](std::uint32_t v) { return static_cast<std::uint8_t>((v / 10) * 16 + v % 10); };
    out[12] = bcd(msf / (60 * 75));
    out[13] = bcd(msf / 75 % 60);
    out[14] = bcd(msf % 75);
    out[15] = 2;
    out[18] = out[22] = 0x08;  // data, form 1
    file_.read(reinterpret_cast<char*>(out + 24), 2048);
    return static_cast<bool>(file_);
}

bool DiscImage::readSector(std::uint32_t lba, std::uint8_t* out) {
    if (lba >= sectors_) return false;
    file_.clear();
    if (sectorSize_ == 2048) {
        file_.seekg(static_cast<std::streamoff>(lba) * 2048);
    } else {
        std::uint8_t mode = 0;
        file_.seekg(static_cast<std::streamoff>(lba) * 2352 + 15);
        file_.read(reinterpret_cast<char*>(&mode), 1);
        file_.seekg(static_cast<std::streamoff>(lba) * 2352 + (mode == 2 ? 24 : 16));
    }
    file_.read(reinterpret_cast<char*>(out), 2048);
    return static_cast<bool>(file_);
}

std::vector<std::uint8_t> DiscImage::readFile(const DiscFile& f) {
    std::uint32_t count = (f.size + 2047) / 2048;
    std::vector<std::uint8_t> data(static_cast<size_t>(count) * 2048);
    for (std::uint32_t i = 0; i < count; ++i)
        if (!readSector(f.lba + i, data.data() + static_cast<size_t>(i) * 2048))
            throw std::runtime_error("read error in " + f.path);
    data.resize(f.size);
    return data;
}

void DiscImage::walk(std::uint32_t lba, std::uint32_t size, const std::string& prefix, int depth) {
    if (depth > 8) return;
    std::uint32_t count = (size + 2047) / 2048;
    std::vector<std::uint8_t> dir(static_cast<size_t>(count) * 2048);
    for (std::uint32_t i = 0; i < count; ++i) readSector(lba + i, dir.data() + static_cast<size_t>(i) * 2048);

    size_t pos = 0;
    while (pos < size) {
        std::uint8_t len = dir[pos];
        if (len == 0) {  // records never straddle a sector
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        if (pos + len > dir.size() || len < 34) break;
        const std::uint8_t* rec = dir.data() + pos;
        pos += len;
        std::uint8_t nameLen = rec[32];
        if (nameLen == 1 && (rec[33] == 0 || rec[33] == 1)) continue;

        DiscFile f;
        std::string name(reinterpret_cast<const char*>(rec + 33), nameLen);
        f.path = prefix + upper(name.substr(0, name.find(';')));
        f.lba = le32(rec + 2);
        f.size = le32(rec + 10);
        f.directory = rec[25] & 2;
        size_t su = 33 + nameLen + (nameLen % 2 ? 0 : 1);
        if (su + 14 <= len && rec[su + 6] == 'X' && rec[su + 7] == 'A') {
            std::uint16_t attr = static_cast<std::uint16_t>((rec[su + 4] << 8) | rec[su + 5]);
            f.form2 = attr & 0x5000;
        }
        files_.push_back(f);
        if (f.directory) walk(f.lba, f.size, f.path + "/", depth + 1);
    }
}

const DiscFile* DiscImage::find(const std::string& path) const {
    std::string want = upper(path);
    std::replace(want.begin(), want.end(), '\\', '/');
    while (!want.empty() && want.front() == '/') want.erase(want.begin());
    if (size_t semi = want.find(';'); semi != std::string::npos) want.resize(semi);
    for (const auto& f : files_)
        if (f.path == want) return &f;
    return nullptr;
}

std::optional<DiscFile> DiscImage::bootExecutable() {
    const DiscFile* cnf = find("SYSTEM.CNF");
    if (!cnf) return std::nullopt;
    auto data = readFile(*cnf);
    std::string text(data.begin(), data.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = trim(text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = eol == std::string::npos ? text.size() : eol + 1;
        std::string key = upper(line);
        key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
        if (key.rfind("BOOT=", 0) != 0) continue;
        std::string value = trim(line.substr(line.find('=') + 1));
        if (size_t colon = value.find(':'); colon != std::string::npos) value = value.substr(colon + 1);
        if (const DiscFile* f = find(value)) return *f;
    }
    return std::nullopt;
}

}  // namespace fe
