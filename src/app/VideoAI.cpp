#include "VideoAI.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#include "StrVideo.h"
#include "onnxruntime_c_api.h"

namespace app {

const char* videoModeId(VideoMode m) {
    switch (m) {
        case VideoMode::AiFast: return "fast";
        case VideoMode::AiBest: return "best";
        default: return "original";
    }
}

bool parseVideoMode(const std::string& id, VideoMode& m) {
    if (id == "original" || id == "off") m = VideoMode::Original;
    else if (id == "fast") m = VideoMode::AiFast;
    else if (id == "best") m = VideoMode::AiBest;
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
    bool open(const fs::path& dir, std::string& error) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        path_ = dir / L"hdcache.pack";
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
    bool has(uint64_t hash) {
        std::lock_guard<std::mutex> lock(m_);
        return index_.count(hash) != 0;
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
    Pack pack;
    unsigned hits = 0, misses = 0;

    bool upscale(const fe::u8* rgb, int w, int h, std::vector<fe::u8>& out) override {
        const uint64_t hash = pictureHash(rgb, w, h);
        const size_t want = static_cast<size_t>(w) * K * h * K * 3;
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

std::shared_ptr<BestUpscaler> createBest(const fs::path& aiDir, const fs::path& cacheDir, std::string& error) {
    auto u = std::make_shared<BestUpscaler>();
    if (!u->model.load(aiDir, aiDir / L"best_x4.onnx", error)) return nullptr;
    if (!u->pack.open(cacheDir, error)) return nullptr;
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
    uint64_t hash = 0;
    int width = 0, height = 0;
    std::vector<fe::u8> rgb;
};

}  // namespace

int buildHdCache(const std::vector<std::string>& discs, const fs::path& aiDir, const fs::path& cacheDir) {
    // Three stages run at the same time so the graphics card is never waiting:
    // reading and decoding the disc, the model (two sessions, so one is
    // being fed while the other computes) and compressing the results.
    constexpr int kModels = 2, kWriters = 3;
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
    Pack pack;
    {
        std::string error;
        if (!pack.open(cacheDir, error)) {
            std::printf("hdcache cannot start: %s\n", error.c_str());
            return 1;
        }
    }
    importLooseFiles(cacheDir, pack);

    Queue<Job> toModel(8), toWriter(8);
    std::atomic<unsigned> added{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < kModels; ++i)
        workers.emplace_back([&, i] {
            Job job;
            while (toModel.pop(job)) {
                std::vector<fe::u8> out;
                if (!models[static_cast<size_t>(i)]->run(job.rgb.data(), job.width, job.height, out)) continue;
                job.rgb = std::move(out);
                toWriter.push(std::move(job));
            }
        });
    std::vector<std::thread> writers;
    for (int i = 0; i < kWriters; ++i)
        writers.emplace_back([&] {
            Job job;
            while (toWriter.pop(job))
                if (pack.append(job.hash, job.rgb, true)) ++added;
        });

    unsigned frames = 0, present = 0;
    int failed = 0;
    std::unordered_set<uint64_t> seen;  // pictures repeat (still frames, loops): enlarge each once
    for (const std::string& path : discs) {
        std::unique_ptr<fe::DiscImage> disc;
        try {
            disc = std::make_unique<fe::DiscImage>(path);
        } catch (const std::exception& e) {
            std::printf("hdcache skipping %s: %s\n", path.c_str(), e.what());
            continue;
        }
        std::printf("hdcache %s\n", path.c_str());
        std::fflush(stdout);
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t sectorsDone = 0, sectorsTotal = 1;
        unsigned queuedHere = 0;
        failed += forEachStrFrame(
            *disc,
            [&](const StrFrame& f) {
                ++frames;
                const uint64_t hash = pictureHash(f.rgb.data(), f.width, f.height);
                if (!seen.insert(hash).second) return;
                if (pack.hasRaw(hash)) {
                    ++present;
                    return;
                }
                toModel.push(Job{hash, f.width, f.height, f.rgb});
                if (++queuedHere % 50 == 1) {
                    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    const double part = static_cast<double>(sectorsDone) / static_cast<double>(sectorsTotal);
                    const double left = part > 0.001 ? s / part - s : 0;
                    std::printf("hdcache %5.1f%% of this disc, %u pictures added, %u already there, about %.0f min left\n",
                                100.0 * part, added.load(), present, left / 60);
                    std::fflush(stdout);
                }
            },
            [&](std::uint64_t done, std::uint64_t total) {
                sectorsDone = done;
                sectorsTotal = total ? total : 1;
            });
    }
    toModel.close();
    for (auto& t : workers) t.join();
    toWriter.close();
    for (auto& t : writers) t.join();
    for (auto& m : models) m->stats("best");
    std::printf("hdcache done: %u frames read, %u different pictures, %u added, %u were there already, %d frames not decoded\n",
                frames, static_cast<unsigned>(seen.size()), added.load(), present, failed);
    return 0;
}

}  // namespace app
