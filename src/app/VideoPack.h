// The HD cache stored as H.264 video. The enlarged movie and background
// frames are kept in chunks of up to a few hundred frames, in the order the
// disc holds them, each chunk an H.264 stream with a keyframe every 30 frames
// (about a hundredth of the raw pictures' size). The cache builder encodes
// with ffmpeg (x264); the game decodes with the H.264 decoder that comes with
// Windows (Media Foundation), so playing needs nothing extra.
//
// File: records one after the other, each "FEHV", the body's size (4 bytes)
// and the body: width, height, frame count (4 bytes each), per frame its
// picture hash (8 bytes), its size in the stream (4) and whether it is a
// keyframe (4), then the frames' H.264 data (Annex B, one access unit each).
// A record cut short (the program was stopped while writing) is dropped.
#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace app {

class VideoPack {
public:
    VideoPack();
    ~VideoPack();
    VideoPack(const VideoPack&) = delete;
    VideoPack& operator=(const VideoPack&) = delete;

    bool open(const std::filesystem::path& file, std::string& error);
    size_t size();
    bool has(uint64_t hash);
    // Every picture, as {hash, width, height}, in the order they are stored.
    struct Entry {
        uint64_t hash;
        int width, height;
    };
    std::vector<Entry> entries();

    // Decodes the picture with this hash into width*height*3 bytes of RGB
    // (width and height of the enlarged picture). Frames are decoded from the
    // last keyframe, or on from the previous call when the game plays on.
    bool read(uint64_t hash, int width, int height, std::vector<uint8_t>& rgb);

    struct Frame {
        uint64_t hash = 0;
        std::vector<uint8_t> data;  // one access unit
        bool key = false;
    };
    bool append(int width, int height, const std::vector<Frame>& frames);

private:
    struct Chunk {
        long long offset = 0;  // of the first frame's data
        int width = 0, height = 0;
        std::vector<uint32_t> sizes;
        std::vector<uint64_t> hashes;
        std::vector<long long> starts;  // offset of each frame from `offset`
        std::vector<uint8_t> key;
    };
    struct Where {
        uint32_t chunk, frame;
    };
    struct Decoder;
    // Two decoders, so a picture looked up elsewhere (one shared by two
    // movies) does not lose the place in the movie playing; each decodes
    // the next frame ahead on another thread while the game runs.
    struct Slot {
        std::unique_ptr<Decoder> dec;
        long long loaded = -1;  // chunk whose data is in `data`
        std::vector<uint8_t> data;
        long long chunk = -1, frame = -1;  // where the decoder is (frame -1: needs a keyframe)
        long long lastFrame = -1;          // picture in lastRgb
        std::vector<uint8_t> lastRgb;
        long long aheadFrame = -1;  // picture being decoded ahead into aheadRgb
        std::vector<uint8_t> aheadRgb;
        std::future<bool> ahead;
        uint64_t used = 0;
    };
    bool decode(Slot& s, uint32_t chunk, uint32_t from, uint32_t to, std::vector<uint8_t>& rgb);
    void decodeAhead(Slot& s, uint32_t chunk);

    std::filesystem::path path_;
    FILE* file_ = nullptr;
    std::mutex m_;
    std::vector<Chunk> chunks_;
    std::unordered_map<uint64_t, Where> index_;
    long long end_ = 0;
    Slot slots_[2];
    uint64_t clock_ = 0;

    // The pictures decoded last (YUV 4:2:0, as the decoder gives them), so
    // that frames shown again soon (played backwards, a step back, a picture
    // shared by several movies) cost no decoding.
    static constexpr size_t kRecent = 100;
    std::mutex recentMutex_;
    std::list<std::pair<uint64_t, std::vector<uint8_t>>> recent_;  // most recent first
    std::unordered_map<uint64_t, decltype(recent_)::iterator> recentIndex_;
    // pictures reached by a jump (shared by several movies, shown again and
    // again): kept apart so that a long run of other frames does not push them out
    static constexpr size_t kJumped = 32;
    std::list<std::pair<uint64_t, std::vector<uint8_t>>> jumped_;
    std::unordered_map<uint64_t, decltype(jumped_)::iterator> jumpedIndex_;
    void remember(uint64_t hash, std::vector<uint8_t>&& yuv, bool jump = false);
    bool recall(uint64_t hash, int width, int height, std::vector<uint8_t>& rgb);
};

// Encodes one chunk with ffmpeg and appends it to a VideoPack.
class VideoChunkWriter {
public:
    // ffmpeg.exe from the ai folder or the PATH; empty when there is none.
    static std::filesystem::path findFfmpeg(const std::filesystem::path& aiDir);

    VideoChunkWriter(VideoPack& pack, std::filesystem::path ffmpeg);
    ~VideoChunkWriter();

    size_t count() const { return hashes_.size(); }
    int width() const { return width_; }
    int height() const { return height_; }
    bool add(uint64_t hash, int width, int height, const uint8_t* rgb);  // starts a chunk if none is open
    bool finish();  // encodes and appends the open chunk (no-op when none is)

private:
    void stop();
    VideoPack& pack_;
    std::filesystem::path ffmpeg_;
    int width_ = 0, height_ = 0;
    std::vector<uint64_t> hashes_;
    struct Process;
    std::unique_ptr<Process> proc_;
};

}  // namespace app
