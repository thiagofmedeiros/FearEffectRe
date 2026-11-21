// BIN/CUE (2352-byte raw sectors) and ISO (2048-byte sectors) disc images
// with the ISO9660 file system PlayStation discs use.
#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fe {

struct DiscFile {
    std::string path;  // upper case, '/' separated, no ";1"
    std::uint32_t lba = 0;
    std::uint32_t size = 0;
    bool directory = false;
    bool form2 = false;  // XA audio/video sectors
};

class DiscImage {
public:
    // Accepts a .cue, .bin, .img or .iso path. Throws std::runtime_error.
    explicit DiscImage(const std::string& path);

    bool raw() const { return sectorSize_ == 2352; }
    std::uint32_t sectorCount() const { return sectors_; }
    const std::string& volumeId() const { return volumeId_; }
    const std::vector<DiscFile>& files() const { return files_; }

    // 2352 bytes; synthesised header for 2048-byte images.
    bool readRawSector(std::uint32_t lba, std::uint8_t* out);
    // 2048 bytes of user data.
    bool readSector(std::uint32_t lba, std::uint8_t* out);
    std::vector<std::uint8_t> readFile(const DiscFile& file);
    const DiscFile* find(const std::string& path) const;
    // File named by BOOT= in SYSTEM.CNF.
    std::optional<DiscFile> bootExecutable();

private:
    void walk(std::uint32_t lba, std::uint32_t size, const std::string& prefix, int depth);

    std::ifstream file_;
    std::uint32_t sectorSize_ = 2352;
    std::uint32_t sectors_ = 0;
    std::string volumeId_;
    std::vector<DiscFile> files_;
};

}  // namespace fe
