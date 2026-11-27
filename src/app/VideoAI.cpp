#include "VideoAI.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <fstream>
#include <functional>
#include <set>
#include <iterator>
#include <map>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#include "StrVideo.h"
#include "VideoPack.h"
#include "onnxruntime_c_api.h"

namespace app {

const char* videoModeId(VideoMode m) {
    switch (m) {
        case VideoMode::AiFast: return "fast";
        case VideoMode::AiBest: return "best";
        case VideoMode::AiSeedVR2: return "seedvr2";
        default: return "original";
    }
}

bool parseVideoMode(const std::string& id, VideoMode& m) {
    if (id == "original" || id == "off") m = VideoMode::Original;
    else if (id == "fast") m = VideoMode::AiFast;
    else if (id == "best") m = VideoMode::AiBest;
    else if (id == "seedvr2") m = VideoMode::AiSeedVR2;
    else return false;
    return true;
}

namespace {

namespace fs = std::filesystem;
constexpr int K = fe::VideoUpscaler::kFactor;

// One ONNX model: float NCHW RGB 0..1 in, 4x larger out.
class Model {
public:
    ~Model() {
        if (session_) api_->ReleaseSession(session_);
        if (memory_) api_->ReleaseMemoryInfo(memory_);
        if (env_) api_->ReleaseEnv(env_);
    }

    bool load(const fs::path& aiDir, const fs::path& model, std::string& error) {
        HMODULE dll = LoadLibraryExW((aiDir / L"onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) {
            error = "onnxruntime.dll not found in " + aiDir.string();
            return false;
        }
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        auto base = reinterpret_cast<GetApiBase>(reinterpret_cast<void*>(GetProcAddress(dll, "OrtGetApiBase")));
        if (!base || !(api_ = base()->GetApi(ORT_API_VERSION))) {
            error = "onnxruntime.dll is not version 1.20 or newer";
            return false;
        }
        if (!ok(api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "FearEffectRe", &env_), error)) return false;
        OrtSessionOptions* opts = nullptr;
        if (!ok(api_->CreateSessionOptions(&opts), error)) return false;
        // DirectML needs these two; failures show up when the session is created
        (void)ok(api_->DisableMemPattern(opts), error);
        (void)ok(api_->SetSessionExecutionMode(opts, ORT_SEQUENTIAL), error);
        (void)ok(api_->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL), error);
        using AppendDml = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
        auto dml = reinterpret_cast<AppendDml>(
            reinterpret_cast<void*>(GetProcAddress(dll, "OrtSessionOptionsAppendExecutionProvider_DML")));
        OrtStatus* st = dml ? dml(opts, 0) : nullptr;
        gpu_ = dml && !st;
        if (st) api_->ReleaseStatus(st);
        if (!gpu_) std::printf("video   DirectML is not available: the upscaler runs on the CPU (slow)\n");
        bool loaded = ok(api_->CreateSession(env_, model.c_str(), opts, &session_), error);
        api_->ReleaseSessionOptions(opts);
        if (!loaded) return false;
        return ok(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_), error);
    }

    // Pads the picture by repeating its edges so the model does not darken
    // them, enlarges it and crops the padding off again.
    bool run(const fe::u8* rgb, int w, int h, std::vector<fe::u8>& out) {
        constexpr int P = 8;
        const int pw = w + 2 * P, ph = h + 2 * P;
        padded_.resize(static_cast<size_t>(pw) * ph * 3);
        for (int y = 0; y < ph; ++y) {
            const int sy = std::clamp(y - P, 0, h - 1);
            for (int x = 0; x < pw; ++x) {
                const int sx = std::clamp(x - P, 0, w - 1);
                std::memcpy(&padded_[(static_cast<size_t>(y) * pw + x) * 3], &rgb[(static_cast<size_t>(sy) * w + sx) * 3], 3);
            }
        }
        auto t0 = std::chrono::steady_clock::now();
        if (!runRaw(padded_.data(), pw, ph, big_)) return false;
        seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ++runs_;
        out.resize(static_cast<size_t>(w) * K * h * K * 3);
        const size_t row = static_cast<size_t>(w) * K * 3;
        for (int y = 0; y < h * K; ++y)
            std::memcpy(&out[y * row], &big_[((static_cast<size_t>(y + P * K) * pw * K) + P * K) * 3], row);
        return true;
    }

    void stats(const char* name) const {
        if (runs_) std::printf("video   %s model: %u pictures, %.1f ms each\n", name, runs_, seconds_ * 1000 / runs_);
    }

    bool runRaw(const fe::u8* rgb, int w, int h, std::vector<fe::u8>& out) {
        const size_t plane = static_cast<size_t>(w) * h;
        in_.resize(plane * 3);
        for (size_t i = 0; i < plane; ++i)
            for (int c = 0; c < 3; ++c) in_[c * plane + i] = rgb[i * 3 + c] * (1.0f / 255.0f);
        const int64_t shape[4] = {1, 3, h, w};
        OrtValue* input = nullptr;
        OrtValue* output = nullptr;
        std::string error;
        if (!ok(api_->CreateTensorWithDataAsOrtValue(memory_, in_.data(), in_.size() * sizeof(float), shape, 4,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input),
                error))
            return report(error);
        const char* inName = "input";
        const char* outName = "output";
        bool good = ok(api_->Run(session_, nullptr, &inName, &input, 1, &outName, 1, &output), error);
        api_->ReleaseValue(input);
        if (!good) return report(error);
        float* data = nullptr;
        good = ok(api_->GetTensorMutableData(output, reinterpret_cast<void**>(&data)), error);
        if (good) {
            const size_t oplane = plane * K * K;
            out.resize(oplane * 3);
            for (size_t i = 0; i < oplane; ++i)
                for (int c = 0; c < 3; ++c) {
                    float v = data[c * oplane + i] * 255.0f + 0.5f;
                    out[i * 3 + c] = static_cast<fe::u8>(std::clamp(v, 0.0f, 255.0f));
                }
        }
        api_->ReleaseValue(output);
        return good || report(error);
    }

    bool gpu() const { return gpu_; }


private:
    bool ok(OrtStatus* st, std::string& error) {
        if (!st) return true;
        error = api_->GetErrorMessage(st);
        api_->ReleaseStatus(st);
        return false;
    }
    bool report(const std::string& error) {
        if (!reported_) std::printf("video   upscaler failed: %s\n", error.c_str());
        reported_ = true;
        return false;
    }

    const OrtApi* api_ = nullptr;
    OrtEnv* env_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* memory_ = nullptr;
    std::vector<float> in_;
    std::vector<fe::u8> padded_, big_;
    double seconds_ = 0;
    unsigned runs_ = 0;
    bool gpu_ = false, reported_ = false;
};

// JPEG in memory through the Windows Imaging Component.
class Jpeg {
public:
    Jpeg() {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_));
    }
    ~Jpeg() {
        if (factory_) factory_->Release();
        if (com_) CoUninitialize();
    }
    Jpeg(const Jpeg&) = delete;
    Jpeg& operator=(const Jpeg&) = delete;

    bool decode(const std::vector<fe::u8>& data, int w, int h, std::vector<fe::u8>& rgb) {
        if (!factory_ || data.empty()) return false;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        bool good = false;
        UINT fw = 0, fh = 0;
        if (SUCCEEDED(factory_->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), static_cast<DWORD>(data.size()))) &&
            SUCCEEDED(factory_->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&fw, &fh)) &&
            static_cast<int>(fw) == w && static_cast<int>(fh) == h && SUCCEEDED(factory_->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            rgb.resize(static_cast<size_t>(w) * h * 3);
            good = SUCCEEDED(conv->CopyPixels(nullptr, static_cast<UINT>(w * 3), static_cast<UINT>(rgb.size()), rgb.data()));
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (stream) stream->Release();
        return good;
    }

    bool encode(int w, int h, const std::vector<fe::u8>& rgb, std::vector<fe::u8>& data) {
        if (!factory_) return false;
        // WIC writes BGR; swap a copy
        std::vector<fe::u8> bgr(rgb);
        for (size_t i = 0; i + 2 < bgr.size(); i += 3) std::swap(bgr[i], bgr[i + 2]);
        IStream* mem = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* bag = nullptr;
        bool good = false;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) &&
            SUCCEEDED(factory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(mem, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, &bag)) &&
            setJpegOptions(bag) && SUCCEEDED(frame->Initialize(bag)) &&
            SUCCEEDED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h))) &&
            SUCCEEDED(frame->SetPixelFormat(&fmt)) && fmt == GUID_WICPixelFormat24bppBGR &&
            SUCCEEDED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 3), static_cast<UINT>(bgr.size()),
                                         bgr.data())) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
            STATSTG st{};
            LARGE_INTEGER zero{};
            if (SUCCEEDED(mem->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(mem->Seek(zero, STREAM_SEEK_SET, nullptr))) {
                data.resize(static_cast<size_t>(st.cbSize.QuadPart));
                ULONG got = 0;
                good = SUCCEEDED(mem->Read(data.data(), static_cast<ULONG>(data.size()), &got)) && got == data.size();
            }
        }
        if (bag) bag->Release();
        if (frame) frame->Release();
        if (enc) enc->Release();
        if (mem) mem->Release();
        return good;
    }

private:
    // quality 98 without colour subsampling: no visible loss, a fifth of a PNG's size
    static bool setJpegOptions(IPropertyBag2* bag) {
        PROPBAG2 opt[2]{};
        VARIANT val[2];
        opt[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VariantInit(&val[0]);
        val[0].vt = VT_R4;
        val[0].fltVal = 0.98f;
        opt[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
        VariantInit(&val[1]);
        val[1].vt = VT_UI1;
        val[1].bVal = WICJpegYCrCbSubsampling444;
        return SUCCEEDED(bag->Write(2, opt, val));
    }
    IWICImagingFactory* factory_ = nullptr;
    bool com_ = false;
};

class FastUpscaler : public fe::VideoUpscaler {
public:
    Model model;
    bool upscale(const fe::u8* rgb, int w, int h, std::vector<fe::u8>& out) override { return model.run(rgb, w, h, out); }
    ~FastUpscaler() override { model.stats("fast"); }
};

uint64_t pictureHash(const fe::u8* rgb, int w, int h) {
    uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&](const void* p, size_t n) {
        auto* b = static_cast<const fe::u8*>(p);
        for (size_t i = 0; i < n; ++i) hash = (hash ^ b[i]) * 0x100000001b3ull;
    };
    mix(&w, sizeof w);
    mix(&h, sizeof h);
    mix(rgb, static_cast<size_t>(w) * h * 3);
    return hash;
}

// The HD cache: one file, hdcache.pack, of records appended one after the
// other: a tag, the picture's hash (8 bytes), the data's size (4 bytes), the
// data. "FEHR" records hold the 4x picture as raw RGB (fast to read, which
// is what matters); "FEHD" records, from earlier versions, a JPEG. A later
// record for the same picture replaces an earlier one. The index is rebuilt
// by reading the record headers when the file is opened; a record cut short
// (the program was stopped while writing) is dropped.
class Pack {
public:
    bool open(const fs::path& dir, std::string& error, const wchar_t* name = L"hdcache.pack") {
        std::error_code ec;
        fs::create_directories(dir, ec);
        path_ = dir / name;
        file_ = _wfopen(path_.c_str(), L"a+b");
        if (!file_) {
            error = "cannot open " + path_.string();
            return false;
        }
        std::lock_guard<std::mutex> lock(m_);
        std::fseek(file_, 0, SEEK_SET);
        long long at = 0;
        for (;;) {
            unsigned char head[16];
            if (std::fread(head, 1, 16, file_) != 16) break;
            const bool raw = std::memcmp(head, "FEHR", 4) == 0;
            if (!raw && std::memcmp(head, "FEHD", 4) != 0) break;
            uint64_t hash;
            uint32_t size;
            std::memcpy(&hash, head + 4, 8);
            std::memcpy(&size, head + 12, 4);
            if (_fseeki64(file_, size, SEEK_CUR) != 0) break;
            const long long next = at + 16 + size;
            if (_ftelli64(file_) != next || next > fs::file_size(path_, ec)) break;
            index_[hash] = {at + 16, size, raw};
            at = next;
        }
        end_ = at;
        if (fs::file_size(path_, ec) != static_cast<uintmax_t>(at)) {  // a cut-short record at the end
            std::fclose(file_);
            fs::resize_file(path_, static_cast<uintmax_t>(at), ec);
            file_ = _wfopen(path_.c_str(), L"a+b");
            if (!file_) {
                error = "cannot open " + path_.string();
                return false;
            }
        }
        return true;
    }
    ~Pack() {
        if (file_) std::fclose(file_);
    }

    size_t size() const { return index_.size(); }
    void forEachHash(const std::function<void(uint64_t)>& f) {
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& e : index_) f(e.first);
    }
    bool has(uint64_t hash) {
        std::lock_guard<std::mutex> lock(m_);
        return index_.count(hash) != 0;
    }
    // raw pictures in the order they were written (that of the disc, mostly)
    std::vector<uint64_t> rawInOrder() {
        std::lock_guard<std::mutex> lock(m_);
        std::vector<std::pair<long long, uint64_t>> all;
        for (const auto& e : index_)
            if (e.second.raw) all.push_back({e.second.offset, e.first});
        std::sort(all.begin(), all.end());
        std::vector<uint64_t> out;
        for (const auto& a : all) out.push_back(a.second);
        return out;
    }
    bool hasRaw(uint64_t hash) {
        std::lock_guard<std::mutex> lock(m_);
        auto it = index_.find(hash);
        return it != index_.end() && it->second.raw;
    }
    bool read(uint64_t hash, std::vector<fe::u8>& data, bool& raw) {
        std::lock_guard<std::mutex> lock(m_);
        auto it = index_.find(hash);
        if (it == index_.end()) return false;
        raw = it->second.raw;
        data.resize(it->second.size);
        return _fseeki64(file_, it->second.offset, SEEK_SET) == 0 &&
               std::fread(data.data(), 1, data.size(), file_) == data.size();
    }
    bool append(uint64_t hash, const std::vector<fe::u8>& data, bool raw) {
        std::lock_guard<std::mutex> lock(m_);
        auto it = index_.find(hash);
        if (it != index_.end() && (it->second.raw || !raw)) return true;
        unsigned char head[16];
        std::memcpy(head, raw ? "FEHR" : "FEHD", 4);
        std::memcpy(head + 4, &hash, 8);
        const uint32_t size = static_cast<uint32_t>(data.size());
        std::memcpy(head + 12, &size, 4);
        // "a" mode writes at the end; the seek is required between a read and a write
        if (_fseeki64(file_, 0, SEEK_END) != 0 || std::fwrite(head, 1, 16, file_) != 16 || std::fwrite(data.data(), 1, data.size(), file_) != data.size() ||
            std::fflush(file_) != 0)
            return false;
        index_[hash] = {end_ + 16, size, raw};
        end_ += 16 + static_cast<long long>(size);
        return true;
    }

private:
    struct Entry {
        long long offset;
        uint32_t size;
        bool raw;
    };
    fs::path path_;
    FILE* file_ = nullptr;
    std::mutex m_;
    std::unordered_map<uint64_t, Entry> index_;
    long long end_ = 0;
};

// Pictures cached as separate JPEG files by earlier versions (hdcache/xx/*.jpg)
// are moved into the pack.
void importLooseFiles(const fs::path& dir, Pack& pack) {
    std::error_code ec;
    unsigned moved = 0;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        bool empty = true;
        for (auto f = fs::directory_iterator(it->path(), ec); !ec && f != fs::directory_iterator(); f.increment(ec)) {
            const fs::path p = f->path();
            const std::string stem = p.stem().string();
            if (p.extension() != ".jpg" || stem.size() != 16) {
                empty = false;
                continue;
            }
            std::ifstream in(p, std::ios::binary);
            std::vector<fe::u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            if (data.empty() || !pack.append(std::stoull(stem, nullptr, 16), data, false)) {
                empty = false;
                continue;
            }
            fs::remove(p, ec);
            ++moved;
        }
        if (empty) fs::remove(it->path(), ec);
    }
    if (moved) std::printf("hdcache moved %u pictures from separate files into hdcache.pack\n", moved);
}

// The large model is too slow to keep up with the game, so each picture it
// enlarges is kept in the cache under the picture's hash. A picture missing
// from it is enlarged on the spot (the game pauses for it); building the
// cache beforehand from the discs (--build-hd-cache) avoids the pauses.
class BestUpscaler : public fe::VideoUpscaler {
public:
    Model model;
    VideoPack video;  // the cache built beforehand, as H.264
    Pack pack;        // pictures enlarged while playing, and caches from earlier versions
    unsigned hits = 0, misses = 0;

    bool upscale(const fe::u8* rgb, int w, int h, std::vector<fe::u8>& out) override {
        const uint64_t hash = pictureHash(rgb, w, h);
        const size_t want = static_cast<size_t>(w) * K * h * K * 3;
        if (video.read(hash, w * K, h * K, out)) {
            ++hits;
            return true;
        }
        bool raw = false;
        if (pack.read(hash, out, raw)) {
            if (raw && out.size() == want) {
                ++hits;
                return true;
            }
            // a JPEG from an earlier version: decode it once and keep it raw
            thread_local Jpeg jpeg;  // COM objects stay on the thread that made them
            std::vector<fe::u8> data;
            data.swap(out);
            if (jpeg.decode(data, w * K, h * K, out)) {
                pack.append(hash, out, true);
                ++hits;
                return true;
            }
        }
        // not in the cache: the game waits for the large model (about half a second)
        ++misses;
        if (!model.run(rgb, w, h, out)) return false;
        pack.append(hash, out, true);
        return true;
    }
    ~BestUpscaler() override {
        model.stats("best");
        if (hits + misses) std::printf("video   HD cache: %u pictures found, %u enlarged and added\n", hits, misses);
    }
};

// SeedVR2 is far too slow to run while playing: only pictures from its own
// cache are shown, never another model's. A picture missing from it (its disc
// has not been built yet) is shown as on the console.
class SeedVR2Upscaler : public fe::VideoUpscaler {
public:
    VideoPack video;
    std::unique_ptr<Pack> pack;  // raw pictures from earlier versions, until converted
    unsigned hits = 0, misses = 0;
    bool upscale(const fe::u8* rgb, int w, int h, std::vector<fe::u8>& out) override {
        const uint64_t hash = pictureHash(rgb, w, h);
        if (video.read(hash, w * K, h * K, out)) {
            ++hits;
            return true;
        }
        bool raw = false;
        if (pack && pack->read(hash, out, raw) && raw && out.size() == static_cast<size_t>(w) * K * h * K * 3) {
            ++hits;
            return true;
        }
        ++misses;
        return false;
    }
    ~SeedVR2Upscaler() override {
        if (hits + misses) std::printf("video   SeedVR2 cache: %u pictures found, %u missing (shown as on the console)\n", hits, misses);
    }
};

// A disc whose SeedVR2 cache is complete has a marker file named after its volume.
fs::path seedvr2Marker(const fs::path& cacheDir, const std::string& volumeId) {
    return cacheDir / ("seedvr2-" + volumeId + ".done");
}

std::shared_ptr<BestUpscaler> createBest(const fs::path& aiDir, const fs::path& cacheDir, std::string& error) {
    auto u = std::make_shared<BestUpscaler>();
    if (!u->model.load(aiDir, aiDir / L"best_x4.onnx", error)) return nullptr;
    if (!u->video.open(cacheDir / L"hdcache.vpack", error) || !u->pack.open(cacheDir, error)) return nullptr;
    importLooseFiles(cacheDir, u->pack);
    return u;
}

}  // namespace

std::shared_ptr<fe::VideoUpscaler> createAiUpscaler(VideoMode mode, const fs::path& aiDir, const fs::path& cacheDir,
                                                    std::string& error) {
    if (mode == VideoMode::Original) return nullptr;
    if (mode == VideoMode::AiFast) {
        auto u = std::make_shared<FastUpscaler>();
        if (!u->model.load(aiDir, aiDir / L"fast_x4.onnx", error)) return nullptr;
        return u;
    }
    if (mode == VideoMode::AiSeedVR2) {
        auto u = std::make_shared<SeedVR2Upscaler>();
        if (!u->video.open(cacheDir / L"seedvr2.vpack", error)) return nullptr;
        std::error_code ec;
        if (fs::exists(cacheDir / L"seedvr2.pack", ec)) {
            u->pack = std::make_unique<Pack>();
            if (!u->pack->open(cacheDir, error, L"seedvr2.pack")) u->pack.reset();
        }
        return u;
    }
    return createBest(aiDir, cacheDir, error);
}

namespace {

// A small bounded queue between the stages of the cache builder.
template <typename T>
class Queue {
public:
    explicit Queue(size_t limit) : limit_(limit) {}
    void push(T v) {
        std::unique_lock<std::mutex> lock(m_);
        notFull_.wait(lock, [&] { return items_.size() < limit_; });
        items_.push_back(std::move(v));
        notEmpty_.notify_one();
    }
    bool pop(T& v) {  // false once closed and empty
        std::unique_lock<std::mutex> lock(m_);
        notEmpty_.wait(lock, [&] { return !items_.empty() || closed_; });
        if (items_.empty()) return false;
        v = std::move(items_.front());
        items_.pop_front();
        notFull_.notify_one();
        return true;
    }
    void close() {
        std::lock_guard<std::mutex> lock(m_);
        closed_ = true;
        notEmpty_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<T> items_;
    size_t limit_;
    bool closed_ = false;
};

struct Job {
    uint64_t seq = 0, hash = 0;
    int width = 0, height = 0;
    bool newChunk = false;  // the first picture of a movie or background file
    bool enlarged = false;  // rgb already holds the 4x picture (from the old raw cache)
    std::vector<fe::u8> rgb;
};

// Movie and background files of a disc, in disc order.
template <typename F>
void forEachVideoFile(fe::DiscImage& disc, F f) {
    for (const auto& file : disc.files())
        if (file.form2 && !file.directory) f(file.path);
}

bool openDisc(const std::string& path, std::unique_ptr<fe::DiscImage>& disc, const char* tag) {
    try {
        disc = std::make_unique<fe::DiscImage>(path);
        return true;
    } catch (const std::exception& e) {
        std::printf("%s skipping %s: %s\n", tag, path.c_str(), e.what());
        return false;
    }
}

fs::path needFfmpeg(const fs::path& aiDir, const char* tag) {
    fs::path ffmpeg = VideoChunkWriter::findFfmpeg(aiDir);
    if (ffmpeg.empty())
        std::printf("%s cannot start: the cache is stored as H.264 video, encoded by ffmpeg.exe; put it in %s or on the PATH\n",
                    tag, aiDir.string().c_str());
    return ffmpeg;
}

constexpr size_t kChunkFrames = 300;  // pictures per H.264 chunk at most

}  // namespace

int buildHdCache(const std::vector<std::string>& discs, const fs::path& aiDir, const fs::path& cacheDir) {
    // Three stages run at the same time so the graphics card is never waiting:
    // reading and decoding the disc, the model (two sessions, so one is being
    // fed while the other computes) and the H.264 encoder, which puts the
    // pictures back in disc order and encodes them in chunks.
    constexpr int kModels = 2;
    const fs::path ffmpeg = needFfmpeg(aiDir, "hdcache");
    if (ffmpeg.empty()) return 1;
    std::vector<std::unique_ptr<Model>> models;
    for (int i = 0; i < kModels; ++i) {
        std::string error;
        auto m = std::make_unique<Model>();
        if (!m->load(aiDir, aiDir / L"best_x4.onnx", error)) {
            std::printf("hdcache cannot start: %s\n", error.c_str());
            return 1;
        }
        models.push_back(std::move(m));
    }
    VideoPack video;
    Pack pack;  // pictures enlarged earlier and kept raw: encoded without running the model again
    {
        std::string error;
        if (!video.open(cacheDir / L"hdcache.vpack", error) || !pack.open(cacheDir, error)) {
            std::printf("hdcache cannot start: %s\n", error.c_str());
            return 1;
        }
    }
    importLooseFiles(cacheDir, pack);

    Queue<Job> toModel(8), toWriter(8);
    std::atomic<unsigned> added{0}, converted{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < kModels; ++i)
        workers.emplace_back([&, i] {
            Job job;
            while (toModel.pop(job)) {
                if (!job.enlarged) {
                    std::vector<fe::u8> out;
                    if (models[static_cast<size_t>(i)]->run(job.rgb.data(), job.width, job.height, out)) job.rgb = std::move(out);
                    else job.rgb.clear();  // failed: the encoder skips it
                }
                toWriter.push(std::move(job));
            }
        });
    std::thread writer([&] {
        VideoChunkWriter chunk(video, ffmpeg);
        std::map<uint64_t, Job> waiting;  // the models may finish out of order
        uint64_t next = 0;
        Job job;
        auto finish = [&] {
            const size_t n = chunk.count();
            if (n && chunk.finish()) std::printf("hdcache chunk of %zu pictures encoded, %zu in the cache\n", n, video.size());
            std::fflush(stdout);
        };
        while (toWriter.pop(job)) {
            waiting.emplace(job.seq, std::move(job));
            for (auto it = waiting.find(next); it != waiting.end(); it = waiting.find(++next)) {
                Job& j = it->second;
                if (!j.rgb.empty()) {
                    const int w = j.width * K, h = j.height * K;
                    if (chunk.count() && (j.newChunk || chunk.count() >= kChunkFrames || w != chunk.width() || h != chunk.height()))
                        finish();
                    if (chunk.add(j.hash, w, h, j.rgb.data())) ++(j.enlarged ? converted : added);
                }
                waiting.erase(it);
            }
        }
        finish();
    });

    unsigned frames = 0, present = 0;
    uint64_t seq = 0;
    int failed = 0;
    std::unordered_set<uint64_t> seen;  // pictures repeat (still frames, loops): enlarge each once
    for (const std::string& path : discs) {
        std::unique_ptr<fe::DiscImage> disc;
        if (!openDisc(path, disc, "hdcache")) continue;
        std::printf("hdcache %s\n", path.c_str());
        std::fflush(stdout);
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t sectorsDone = 0, sectorsTotal = 1;
        unsigned queuedHere = 0;
        forEachVideoFile(*disc, [&](const std::string& file) {
            bool first = true;
            failed += forEachStrFrame(
                *disc,
                [&](const StrFrame& f) {
                    ++frames;
                    const uint64_t hash = pictureHash(f.rgb.data(), f.width, f.height);
                    if (!seen.insert(hash).second) return;
                    if (video.has(hash)) {
                        ++present;
                        return;
                    }
                    Job job{seq++, hash, f.width, f.height, first, false, {}};
                    first = false;
                    bool raw = false;
                    if (pack.read(hash, job.rgb, raw) && raw &&
                        job.rgb.size() == static_cast<size_t>(f.width) * K * f.height * K * 3)
                        job.enlarged = true;
                    else
                        job.rgb = f.rgb;
                    toModel.push(std::move(job));
                    if (++queuedHere % 50 == 1) {
                        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                        const double part = static_cast<double>(sectorsDone) / static_cast<double>(sectorsTotal);
                        const double left = part > 0.001 ? s / part - s : 0;
                        std::printf("hdcache %5.1f%% of this disc, %u pictures enlarged, %u taken from the old cache, "
                                    "%u already there, about %.0f min left\n",
                                    100.0 * part, added.load(), converted.load(), present, left / 60);
                        std::fflush(stdout);
                    }
                },
                [&](std::uint64_t done, std::uint64_t total) {
                    sectorsDone = done;
                    sectorsTotal = total ? total : 1;
                },
                file);
        });
    }
    toModel.close();
    for (auto& t : workers) t.join();
    toWriter.close();
    writer.join();
    for (auto& m : models) m->stats("best");
    std::printf("hdcache done: %u frames read, %u different pictures, %u enlarged, %u taken from the old cache, "
                "%u were there already, %d frames not decoded\n",
                frames, static_cast<unsigned>(seen.size()), added.load(), converted.load(), present, failed);
    if (converted)
        std::printf("hdcache the pictures of hdcache.pack are now in hdcache.vpack too; hdcache.pack can be deleted "
                    "(it is made again, small, for pictures enlarged while playing)\n");
    return 0;
}

int convertHdCache(const fs::path& aiDir, const fs::path& cacheDir) {
    const fs::path ffmpeg = needFfmpeg(aiDir, "convert");
    if (ffmpeg.empty()) return 1;
    int bad = 0;
    for (const wchar_t* name : {L"hdcache", L"seedvr2"}) {
        std::error_code ec;
        const fs::path rp = cacheDir / (std::wstring(name) + L".pack");
        if (!fs::exists(rp, ec)) continue;
        VideoPack video;
        Pack raw;
        std::string error;
        if (!video.open(cacheDir / (std::wstring(name) + L".vpack"), error) || !raw.open(cacheDir, error, rp.filename().c_str())) {
            std::printf("convert %s\n", error.c_str());
            return 1;
        }
        VideoChunkWriter chunk(video, ffmpeg);
        unsigned added = 0, skipped = 0, there = 0;
        std::vector<fe::u8> pic;
        for (uint64_t h : raw.rawInOrder()) {
            if (video.has(h)) {
                ++there;
                continue;
            }
            bool isRaw = false;
            // the movies and backgrounds are 320x160: 1280x640 enlarged; the size of others is not recorded
            if (!raw.read(h, pic, isRaw) || pic.size() != 1280u * 640 * 3) {
                ++skipped;
                continue;
            }
            if (chunk.count() >= kChunkFrames && !chunk.finish()) ++bad;
            if (chunk.add(h, 1280, 640, pic.data())) ++added;
        }
        if (!chunk.finish()) ++bad;
        std::printf("convert %ls.pack: %u pictures encoded into %ls.vpack, %u were there already, %u of another size left out\n",
                    name, added, name, there, skipped);
    }
    return bad ? 1 : 0;
}

int checkHdCache(const fs::path& cacheDir) {
    int bad = 0;
    for (const wchar_t* name : {L"hdcache", L"seedvr2"}) {
        std::error_code ec;
        const fs::path vp = cacheDir / (std::wstring(name) + L".vpack"), rp = cacheDir / (std::wstring(name) + L".pack");
        if (!fs::exists(vp, ec)) continue;
        VideoPack video;
        Pack raw;
        std::string error;
        if (!video.open(vp, error)) {
            std::printf("check   %s\n", error.c_str());
            return 1;
        }
        const bool haveRaw = fs::exists(rp, ec) && raw.open(cacheDir, error, rp.filename().c_str());
        auto all = video.entries();
        const size_t count = all.size();
        unsigned decoded = 0, compared = 0, failed = 0;
        double worst = 1e9, sum = 0;
        double passMs[2] = {0, 0};
        for (int pass = 0; pass < 2; ++pass) {
            if (pass) {  // jumping around: a sample in random order
                std::shuffle(all.begin(), all.end(), std::mt19937(1));
                if (all.size() > 300) all.resize(300);
            }
            double& ms = passMs[pass];
            for (const auto& e : all) {
                std::vector<fe::u8> pic, ref;
                const auto t1 = std::chrono::steady_clock::now();
                const bool got = video.read(e.hash, e.width, e.height, pic);
                ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
                if (!got) {
                    ++failed;
                    continue;
                }
                ++decoded;
                bool isRaw = false;
                if (pass || !haveRaw || !raw.read(e.hash, ref, isRaw) || !isRaw || ref.size() != pic.size()) continue;
                double se = 0;
                for (size_t i = 0; i < pic.size(); ++i) {
                    const double d = static_cast<double>(pic[i]) - ref[i];
                    se += d * d;
                }
                const double psnr = se ? 10 * std::log10(255.0 * 255.0 * static_cast<double>(pic.size()) / se) : 99;
                worst = std::min(worst, psnr);
                sum += psnr;
                ++compared;
            }
        }
        std::printf("check   %ls.vpack: %zu pictures, %u decoded (%.1f ms each in order, %.1f ms jumping), %u failed", name,
                    count, decoded, passMs[0] / std::max<size_t>(count, 1), passMs[1] / std::max<size_t>(all.size(), 1),
                    failed);
        if (compared) std::printf(", against the raw cache: PSNR %.1f dB on average, %.1f dB at worst", sum / compared, worst);
        std::printf("\n");
        bad += failed;
    }
    return bad ? 1 : 0;
}

bool seedvr2DiscReady(const fs::path& cacheDir, const std::string& volumeId) {
    std::error_code ec;
    return fs::exists(seedvr2Marker(cacheDir, volumeId), ec);
}

int buildSeedVR2Cache(const std::vector<std::string>& discs, const fs::path& aiDir, const fs::path& cacheDir) {
    const fs::path seedvr = aiDir / L"seedvr2";
    // the portable Python from setup_seedvr2.ps1, or a virtual environment set up by hand
    std::error_code ec;
    const fs::path python = fs::exists(seedvr / L"python" / L"python.exe", ec)
                                ? seedvr / L"python" / L"python.exe"
                                : seedvr / L"venv" / L"Scripts" / L"python.exe";
    const fs::path script = aiDir / L"seedvr2_chunk.py";
    const bool installed =
        fs::exists(python, ec) && fs::exists(seedvr / L"repo" / L"inference_cli.py", ec) && fs::exists(script, ec);
    const fs::path ffmpeg = needFfmpeg(aiDir, "seedvr2");
    if (ffmpeg.empty()) return 1;
    VideoPack video;
    std::unique_ptr<Pack> old;  // seedvr2.pack from earlier versions: raw pictures, encoded as they are
    {
        std::string error;
        if (!video.open(cacheDir / L"seedvr2.vpack", error)) {
            std::printf("seedvr2 cannot start: %s\n", error.c_str());
            return 1;
        }
        if (fs::exists(cacheDir / L"seedvr2.pack", ec)) {
            old = std::make_unique<Pack>();
            if (!old->open(cacheDir, error, L"seedvr2.pack")) old.reset();
        }
    }
    const fs::path work = cacheDir / L"seedvr2-work";
    const fs::path result = work / L"result.rgb";

    // Frames are gathered in disc order, a chunk of up to kChunkFrames per
    // file; the new ones go through SeedVR2 together (as a video, which keeps
    // them steady), then the whole chunk is encoded.
    struct Entry {
        uint64_t hash;
        std::vector<fe::u8> rgb;  // the disc picture when SeedVR2 has to enlarge it, else empty
    };
    std::vector<Entry> entries;
    int width = 0, height = 0;
    unsigned chunks = 0, added = 0, converted = 0;
    bool failed = false;
    VideoChunkWriter writer(video, ffmpeg);
    const auto start = std::chrono::steady_clock::now();
    auto flush = [&]() {
        if (entries.empty() || failed) return;
        size_t fresh = 0;
        for (const Entry& e : entries) fresh += !e.rgb.empty();
        if (fresh) {
            if (!installed) {
                std::printf("seedvr2 not installed: needs %s, %s and %s (see tools/ai/README.md)\n", python.string().c_str(),
                            (seedvr / L"repo").string().c_str(), script.string().c_str());
                failed = true;
                return;
            }
            fs::remove_all(work, ec);
            fs::create_directories(work, ec);
            std::ofstream list(work / L"hashes.txt");
            size_t n = 0;
            for (const Entry& e : entries) {
                if (e.rgb.empty()) continue;
                char name[32];
                std::snprintf(name, sizeof name, "%05zu.ppm", n++);
                std::ofstream out(work / name, std::ios::binary);
                out << "P6\n" << width << " " << height << "\n255\n";
                out.write(reinterpret_cast<const char*>(e.rgb.data()), static_cast<std::streamsize>(e.rgb.size()));
                char line[24];
                std::snprintf(line, sizeof line, "%016llx\n", static_cast<unsigned long long>(e.hash));
                list << line;
            }
            list.close();
            std::wstring cmd = L"\"" + python.wstring() + L"\" \"" + script.wstring() + L"\" \"" + work.wstring() + L"\" \"" +
                               result.wstring() + L"\" \"" + seedvr.wstring() + L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            DWORD code = 1;
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            if (code != 0) {
                std::printf("seedvr2 a chunk failed; its frames are kept in %s\n", work.string().c_str());
                failed = true;
                return;
            }
        }
        // the chunk in order: new pictures from SeedVR2's result, the others from the old cache
        std::ifstream in(result, std::ios::binary);
        const size_t big = static_cast<size_t>(width) * K * height * K * 3;
        std::vector<fe::u8> pic;
        for (const Entry& e : entries) {
            bool ok = false;
            if (!e.rgb.empty()) {
                pic.resize(big);
                ok = static_cast<bool>(in.read(reinterpret_cast<char*>(pic.data()), static_cast<std::streamsize>(big)));
            } else {
                bool raw = false;
                ok = old && old->read(e.hash, pic, raw) && raw && pic.size() == big;
            }
            if (!ok || !writer.add(e.hash, width * K, height * K, pic.data())) {
                failed = true;
                break;
            }
        }
        in.close();
        if (failed || !writer.finish()) {
            std::printf("seedvr2 encoding a chunk failed%s\n", fresh ? "; SeedVR2's pictures are kept in seedvr2-work" : "");
            failed = true;
            return;
        }
        added += static_cast<unsigned>(fresh);
        converted += static_cast<unsigned>(entries.size() - fresh);
        ++chunks;
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("seedvr2 %u pictures added", added);
        if (added) std::printf(" (%.2f s each)", s / added);
        if (converted) std::printf(", %u taken from seedvr2.pack", converted);
        std::printf("\n");
        std::fflush(stdout);
        entries.clear();
        fs::remove_all(work, ec);
    };

    for (const std::string& path : discs) {
        if (failed) break;
        std::unique_ptr<fe::DiscImage> disc;
        if (!openDisc(path, disc, "seedvr2")) continue;
        forEachVideoFile(*disc, [&](const std::string& file) {
            if (failed) return;
            std::printf("seedvr2 %s %s\n", path.c_str(), file.c_str());
            std::fflush(stdout);
            std::uint64_t sectorsDone = 0, sectorsTotal = 1;
            std::set<uint64_t> queued;
            forEachStrFrame(
                *disc,
                [&](const StrFrame& f) {
                    if (failed) return;
                    const uint64_t h = pictureHash(f.rgb.data(), f.width, f.height);
                    if (video.has(h) || queued.count(h)) return;
                    if (!entries.empty() && (f.width != width || f.height != height)) flush();
                    width = f.width;
                    height = f.height;
                    queued.insert(h);
                    const bool have = old && old->hasRaw(h);
                    entries.push_back({h, have ? std::vector<fe::u8>{} : f.rgb});
                    if (entries.size() >= kChunkFrames) {
                        std::printf("seedvr2 %5.1f%% of %s\n", 100.0 * sectorsDone / sectorsTotal, file.c_str());
                        flush();
                    }
                },
                [&](std::uint64_t d, std::uint64_t t) {
                    sectorsDone = d;
                    sectorsTotal = t ? t : 1;
                },
                file);
            flush();
        });
        if (!failed) {  // every movie and background of this disc is in the cache
            std::ofstream(seedvr2Marker(cacheDir, disc->volumeId())) << path << "\n";
            std::printf("seedvr2 disc %s is complete\n", disc->volumeId().c_str());
        }
    }
    std::printf("seedvr2 %s: %u pictures added, %u taken from seedvr2.pack, in %u chunks, %zu in the SeedVR2 cache\n",
                failed ? "stopped" : "done", added, converted, chunks, video.size());
    if (!failed && converted)
        std::printf("seedvr2 once every disc has been through this, seedvr2.pack can be deleted\n");
    return failed ? 1 : 0;
}

}  // namespace app
