#include "VideoPack.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>

namespace app {

namespace fs = std::filesystem;

namespace {

// Windows' H.264 decoder and its low-latency switch (one picture out for each in)
const GUID kH264Decoder = {0x62CE7E72, 0x4C71, 0x4d20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};
const GUID kLowLatency = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
const IID IID_ICodecAPI = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool mediaFoundation() {
    static const bool ok = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }();
    thread_local const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    (void)com;
    return ok;
}

// BT.709, limited range, as the cache builder encodes
void nv12ToRgb(const uint8_t* y, const uint8_t* uv, int w, int h, uint8_t* rgb) {
    auto clip = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (int j = 0; j < h; ++j) {
        const uint8_t* yr = y + static_cast<size_t>(j) * w;
        const uint8_t* cr = uv + static_cast<size_t>(j / 2) * w;
        uint8_t* o = rgb + static_cast<size_t>(j) * w * 3;
        for (int i = 0; i < w; ++i, o += 3) {
            const int Y = (yr[i] - 16) * 76309;  // 1.164383 << 16
            const int U = cr[i & ~1] - 128, V = cr[(i & ~1) + 1] - 128;
            o[0] = clip((Y + 117489 * V + 32768) >> 16);
            o[1] = clip((Y - 13975 * U - 34925 * V + 32768) >> 16);
            o[2] = clip((Y + 138438 * U + 32768) >> 16);
        }
    }
}

// Splits an Annex B stream into access units (x264 writes a delimiter before each).
void splitAccessUnits(const std::vector<uint8_t>& s, std::vector<VideoPack::Frame>& frames) {
    std::vector<size_t> nals;  // start code positions
    for (size_t i = 0; i + 3 < s.size(); ++i)
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
            nals.push_back(i > 0 && s[i - 1] == 0 ? i - 1 : i);
            i += 2;
        }
    for (size_t n = 0; n < nals.size(); ++n) {
        const size_t at = nals[n], next = n + 1 < nals.size() ? nals[n + 1] : s.size();
        const size_t hdr = s[at + 2] == 1 ? at + 3 : at + 4;
        const int type = s[hdr] & 31;
        if (type == 9 || frames.empty()) frames.emplace_back();
        auto& f = frames.back();
        f.data.insert(f.data.end(), s.begin() + static_cast<long long>(at), s.begin() + static_cast<long long>(next));
        if (type == 5) f.key = true;
    }
}

}  // namespace

// ---------------------------------------------------------------- decoder

struct VideoPack::Decoder {
    IMFTransform* mft = nullptr;
    int width = 0, height = 0;
    UINT32 alignedH = 0;
    LONG stride = 0;
    DWORD outSize = 0;
    bool providesSamples = false;
    LONGLONG time = 0;
    std::vector<std::vector<uint8_t>> out;  // decoded pictures, NV12 width*height*3/2

    ~Decoder() {
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            release(mft);
        }
    }

    bool open(int w, int h) {
        width = w;
        height = h;
        if (!mediaFoundation() ||
            FAILED(CoCreateInstance(kH264Decoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft))))
            return false;
        ICodecAPI* api = nullptr;
        if (SUCCEEDED(mft->QueryInterface(IID_ICodecAPI, reinterpret_cast<void**>(&api)))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = TRUE;
            api->SetValue(&kLowLatency, &v);
            release(api);
        }
        IMFMediaType* in = nullptr;
        bool ok = SUCCEEDED(MFCreateMediaType(&in)) && SUCCEEDED(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
                  SUCCEEDED(in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) &&
                  SUCCEEDED(MFSetAttributeSize(in, MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h))) &&
                  SUCCEEDED(in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) &&
                  SUCCEEDED(mft->SetInputType(0, in, 0));
        release(in);
        if (!ok || !setOutput()) return false;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool setOutput() {
        for (DWORD i = 0;; ++i) {
            IMFMediaType* t = nullptr;
            if (FAILED(mft->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub{};
            if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12 &&
                SUCCEEDED(mft->SetOutputType(0, t, 0))) {
                UINT32 aw = 0, ah = 0;
                MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &aw, &ah);
                UINT32 st = 0;
                stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? static_cast<LONG>(st) : static_cast<LONG>(aw);
                alignedH = std::max<UINT32>(ah, static_cast<UINT32>(height));
                release(t);
                MFT_OUTPUT_STREAM_INFO info{};
                mft->GetOutputStreamInfo(0, &info);
                outSize = info.cbSize;
                providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                return stride >= width;
            }
            release(t);
        }
    }

    void flush() {
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        out.clear();
    }

    bool pull() {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            IMFSample* sample = nullptr;
            if (!providesSamples) {
                IMFMediaBuffer* b = nullptr;
                if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(outSize, &b))) {
                    release(sample);
                    return false;
                }
                sample->AddBuffer(b);
                release(b);
                ob.pSample = sample;
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (providesSamples) sample = ob.pSample;
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                release(sample);
                return true;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                release(sample);
                if (!setOutput()) return false;
                continue;
            }
            if (FAILED(hr) || !sample) {
                release(sample);
                return false;
            }
            IMFMediaBuffer* buf = nullptr;
            BYTE* p = nullptr;
            DWORD len = 0;
            bool ok = SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&p, nullptr, &len));
            if (ok) {
                const size_t need = static_cast<size_t>(stride) * alignedH * 3 / 2;
                ok = len >= need;
                if (ok) {
                    std::vector<uint8_t> pic(static_cast<size_t>(width) * height * 3 / 2);
                    for (int y = 0; y < height; ++y)
                        std::memcpy(&pic[static_cast<size_t>(y) * width], p + static_cast<size_t>(y) * stride, width);
                    const BYTE* uv = p + static_cast<size_t>(stride) * alignedH;
                    uint8_t* o = &pic[static_cast<size_t>(width) * height];
                    for (int y = 0; y < height / 2; ++y)
                        std::memcpy(o + static_cast<size_t>(y) * width, uv + static_cast<size_t>(y) * stride, width);
                    out.push_back(std::move(pic));
                }
                buf->Unlock();
            }
            release(buf);
            release(sample);
            if (!ok) return false;
        }
    }

    bool feed(const uint8_t* data, size_t size) {
        IMFSample* s = nullptr;
        IMFMediaBuffer* b = nullptr;
        BYTE* p = nullptr;
        bool ok = SUCCEEDED(MFCreateSample(&s)) && SUCCEEDED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &b)) &&
                  SUCCEEDED(b->Lock(&p, nullptr, nullptr));
        if (ok) {
            std::memcpy(p, data, size);
            b->Unlock();
            b->SetCurrentLength(static_cast<DWORD>(size));
            s->AddBuffer(b);
            s->SetSampleTime(time);
            s->SetSampleDuration(666667);
            time += 666667;
            HRESULT hr = mft->ProcessInput(0, s, 0);
            if (hr == MF_E_NOTACCEPTING) {
                ok = pull();
                hr = mft->ProcessInput(0, s, 0);
            }
            ok = ok && SUCCEEDED(hr) && pull();
        }
        release(b);
        release(s);
        return ok;
    }

    bool drain() {
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        return pull();
    }
};

// ---------------------------------------------------------------- pack

VideoPack::VideoPack() = default;

VideoPack::~VideoPack() {
    for (Slot& s : slots_)
        if (s.ahead.valid()) s.ahead.wait();
    if (file_) std::fclose(file_);
}

bool VideoPack::open(const fs::path& file, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    path_ = file;
    file_ = _wfopen(path_.c_str(), L"a+b");
    if (!file_) {
        error = "cannot open " + path_.string();
        return false;
    }
    std::lock_guard<std::mutex> lock(m_);
    const long long fileSize = static_cast<long long>(fs::file_size(path_, ec));
    std::fseek(file_, 0, SEEK_SET);
    long long at = 0;
    for (;;) {
        unsigned char head[8];
        if (std::fread(head, 1, 8, file_) != 8 || std::memcmp(head, "FEHV", 4) != 0) break;
        uint32_t body;
        std::memcpy(&body, head + 4, 4);
        const long long next = at + 8 + body;
        uint32_t dims[3];
        if (next > fileSize || body < 12 || std::fread(dims, 4, 3, file_) != 3) break;
        const uint32_t n = dims[2];
        if (12ull + n * 16ull > body) break;
        std::vector<unsigned char> table(static_cast<size_t>(n) * 16);
        if (n && std::fread(table.data(), 1, table.size(), file_) != table.size()) break;
        Chunk c;
        c.width = static_cast<int>(dims[0]);
        c.height = static_cast<int>(dims[1]);
        c.offset = at + 8 + 12 + static_cast<long long>(n) * 16;
        long long pos = 0;
        const uint32_t id = static_cast<uint32_t>(chunks_.size());
        for (uint32_t i = 0; i < n; ++i) {
            uint64_t hash;
            uint32_t size, key;
            std::memcpy(&hash, &table[i * 16], 8);
            std::memcpy(&size, &table[i * 16 + 8], 4);
            std::memcpy(&key, &table[i * 16 + 12], 4);
            c.sizes.push_back(size);
            c.hashes.push_back(hash);
            c.starts.push_back(pos);
            c.key.push_back(key ? 1 : 0);
            pos += size;
            index_[hash] = {id, i};
        }
        if (c.offset + pos != next) break;
        chunks_.push_back(std::move(c));
        if (_fseeki64(file_, next, SEEK_SET) != 0) break;
        at = next;
    }
    end_ = at;
    if (fileSize != at) {  // a cut-short record at the end
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

size_t VideoPack::size() {
    std::lock_guard<std::mutex> lock(m_);
    return index_.size();
}

std::vector<VideoPack::Entry> VideoPack::entries() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<std::pair<Where, uint64_t>> all;
    for (const auto& e : index_) all.push_back({e.second, e.first});
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
        return a.first.chunk != b.first.chunk ? a.first.chunk < b.first.chunk : a.first.frame < b.first.frame;
    });
    std::vector<Entry> out;
    for (const auto& a : all) out.push_back({a.second, chunks_[a.first.chunk].width, chunks_[a.first.chunk].height});
    return out;
}

bool VideoPack::has(uint64_t hash) {
    std::lock_guard<std::mutex> lock(m_);
    return index_.count(hash) != 0;
}

bool VideoPack::append(int width, int height, const std::vector<Frame>& frames) {
    if (frames.empty()) return true;
    std::vector<unsigned char> rec(8 + 12 + frames.size() * 16);
    std::memcpy(rec.data(), "FEHV", 4);
    const uint32_t dims[3] = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(frames.size())};
    std::memcpy(&rec[8], dims, 12);
    size_t total = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        const uint32_t size = static_cast<uint32_t>(frames[i].data.size()), key = frames[i].key;
        std::memcpy(&rec[20 + i * 16], &frames[i].hash, 8);
        std::memcpy(&rec[20 + i * 16 + 8], &size, 4);
        std::memcpy(&rec[20 + i * 16 + 12], &key, 4);
        total += size;
    }
    const uint32_t body = static_cast<uint32_t>(rec.size() - 8 + total);
    std::memcpy(&rec[4], &body, 4);
    for (const auto& f : frames) rec.insert(rec.end(), f.data.begin(), f.data.end());

    std::lock_guard<std::mutex> lock(m_);
    if (_fseeki64(file_, 0, SEEK_END) != 0 || std::fwrite(rec.data(), 1, rec.size(), file_) != rec.size() ||
        std::fflush(file_) != 0)
        return false;
    Chunk c;
    c.width = width;
    c.height = height;
    c.offset = end_ + 8 + 12 + static_cast<long long>(frames.size()) * 16;
    long long pos = 0;
    const uint32_t id = static_cast<uint32_t>(chunks_.size());
    for (uint32_t i = 0; i < frames.size(); ++i) {
        c.sizes.push_back(static_cast<uint32_t>(frames[i].data.size()));
        c.hashes.push_back(frames[i].hash);
        c.starts.push_back(pos);
        c.key.push_back(frames[i].key ? 1 : 0);
        pos += static_cast<long long>(frames[i].data.size());
        index_[frames[i].hash] = {id, i};
    }
    chunks_.push_back(std::move(c));
    end_ += static_cast<long long>(rec.size());
    return true;
}

bool VideoPack::read(uint64_t hash, int width, int height, std::vector<uint8_t>& rgb) {
    std::lock_guard<std::mutex> lock(m_);
    auto it = index_.find(hash);
    if (it == index_.end()) return false;
    const uint32_t c = it->second.chunk, i = it->second.frame;
    const Chunk& ch = chunks_[c];
    if (ch.width != width || ch.height != height) return false;
    for (Slot& s : slots_)  // the decoders must be idle before they are looked at
        if (s.ahead.valid() && !s.ahead.get()) s.aheadFrame = -1;
    for (Slot& s : slots_) {
        if (s.chunk != c) continue;
        if (s.lastFrame == i) {
            s.used = ++clock_;
            rgb = s.lastRgb;
            return true;
        }
        if (s.aheadFrame == i) {  // decoded ahead while the game was busy
            s.used = ++clock_;
            s.lastRgb.swap(s.aheadRgb);
            s.lastFrame = i;
            s.aheadFrame = -1;
            rgb = s.lastRgb;
            decodeAhead(s, c);
            return true;
        }
    }
    if (recall(hash, width, height, rgb)) return true;
    uint32_t key = i;
    while (key > 0 && !ch.key[key]) --key;
    // a decoder that can play on to the picture, closer to it than its keyframe;
    // else the one used least recently starts over from the keyframe
    Slot* slot = nullptr;
    for (Slot& s : slots_)
        if (s.dec && s.chunk == c && s.frame >= key && s.frame < i) slot = &s;
    if (!slot) {
        slot = &slots_[0];
        for (Slot& s : slots_)
            if (s.used < slot->used) slot = &s;
        slot->frame = -1;
    }
    slot->used = ++clock_;
    slot->aheadFrame = -1;
    bool ok = decode(*slot, c, slot->frame >= 0 ? static_cast<uint32_t>(slot->frame + 1) : key, i, slot->lastRgb);
    if (!ok) {  // start over from the keyframe with a new decoder
        slot->dec.reset();
        slot->frame = -1;
        ok = decode(*slot, c, key, i, slot->lastRgb);
    }
    if (!ok) {
        slot->lastFrame = -1;
        return false;
    }
    slot->lastFrame = i;
    rgb = slot->lastRgb;
    decodeAhead(*slot, c);
    return true;
}

namespace {
template <typename List, typename Index>
void keep(List& list, Index& index, size_t limit, uint64_t hash, std::vector<uint8_t>&& yuv) {
    auto it = index.find(hash);
    if (it != index.end()) {
        list.splice(list.begin(), list, it->second);
        return;
    }
    if (list.size() >= limit) {  // reuse the oldest one's memory
        auto last = std::prev(list.end());
        index.erase(last->first);
        last->first = hash;
        last->second.swap(yuv);
        list.splice(list.begin(), list, last);
    } else {
        list.emplace_front(hash, std::move(yuv));
    }
    index[hash] = list.begin();
}
}  // namespace

void VideoPack::remember(uint64_t hash, std::vector<uint8_t>&& yuv, bool jump) {
    std::lock_guard<std::mutex> lock(recentMutex_);
    if (jump) {
        std::vector<uint8_t> copy(yuv);
        keep(jumped_, jumpedIndex_, kJumped, hash, std::move(copy));
    }
    keep(recent_, recentIndex_, kRecent, hash, std::move(yuv));
}

bool VideoPack::recall(uint64_t hash, int width, int height, std::vector<uint8_t>& rgb) {
    std::lock_guard<std::mutex> lock(recentMutex_);
    const std::vector<uint8_t>* found = nullptr;
    if (auto it = recentIndex_.find(hash); it != recentIndex_.end()) {
        recent_.splice(recent_.begin(), recent_, it->second);
        found = &it->second->second;
    } else if (auto jt = jumpedIndex_.find(hash); jt != jumpedIndex_.end()) {
        jumped_.splice(jumped_.begin(), jumped_, jt->second);
        found = &jt->second->second;
    }
    if (!found) return false;
    const auto& yuv = *found;
    const size_t plane = static_cast<size_t>(width) * height;
    if (yuv.size() != plane * 3 / 2) return false;
    rgb.resize(plane * 3);
    nv12ToRgb(yuv.data(), yuv.data() + plane, width, height, rgb.data());
    return true;
}

void VideoPack::decodeAhead(Slot& s, uint32_t c) {
    const long long next = s.lastFrame + 1;
    if (s.frame != s.lastFrame || next >= static_cast<long long>(chunks_[c].sizes.size())) return;
    s.aheadFrame = next;
    Slot* sp = &s;
    s.ahead = std::async(std::launch::async, [this, sp, c, next] {
        return decode(*sp, c, static_cast<uint32_t>(next), static_cast<uint32_t>(next), sp->aheadRgb);
    });
}

// Feeds frames from..to of chunk c to the slot's decoder (a keyframe first
// when the decoder is not already at from-1) and converts the last one.
bool VideoPack::decode(Slot& s, uint32_t c, uint32_t from, uint32_t to, std::vector<uint8_t>& rgb) {
    const Chunk& ch = chunks_[c];
    if (s.loaded != c) {
        const long long bytes = ch.starts.back() + ch.sizes.back();
        s.data.resize(static_cast<size_t>(bytes));
        if (_fseeki64(file_, ch.offset, SEEK_SET) != 0 || std::fread(s.data.data(), 1, s.data.size(), file_) != s.data.size()) {
            s.loaded = -1;
            return false;
        }
        s.loaded = c;
    }
    const bool restart = !(s.chunk == c && s.frame >= 0 && static_cast<uint32_t>(s.frame + 1) == from);
    s.chunk = c;
    s.frame = -1;
    if (s.dec && (s.dec->width != ch.width || s.dec->height != ch.height)) s.dec.reset();
    if (!s.dec) {
        s.dec = std::make_unique<Decoder>();
        if (!s.dec->open(ch.width, ch.height)) {
            s.dec.reset();
            return false;
        }
    } else if (restart) {
        s.dec->flush();
    }
    s.dec->out.clear();
    const size_t fed = to - from + 1;
    for (uint32_t j = from; j <= to; ++j)
        if (!s.dec->feed(&s.data[static_cast<size_t>(ch.starts[j])], ch.sizes[j])) return false;
    bool drained = false;
    if (s.dec->out.size() < fed) {  // the decoder held pictures back
        if (!s.dec->drain()) return false;
        drained = true;
    }
    if (s.dec->out.size() != fed) return false;
    rgb.resize(static_cast<size_t>(ch.width) * ch.height * 3);
    const auto& pic = s.dec->out.back();
    nv12ToRgb(pic.data(), pic.data() + static_cast<size_t>(ch.width) * ch.height, ch.width, ch.height, rgb.data());
    // a picture reached by decoding several frames from a keyframe is kept apart
    for (size_t k = 0; k < fed; ++k) remember(ch.hashes[from + k], std::move(s.dec->out[k]), fed > 1 && k + 1 == fed);
    s.dec->out.clear();
    s.frame = drained ? -1 : static_cast<long long>(to);  // after a drain, restart from a keyframe
    return true;
}

// ---------------------------------------------------------------- encoder

struct VideoChunkWriter::Process {
    HANDLE in = nullptr, out = nullptr, process = nullptr;
    std::thread reader;
    std::vector<uint8_t> stream;
};

fs::path VideoChunkWriter::findFfmpeg(const fs::path& aiDir) {
    std::error_code ec;
    if (fs::exists(aiDir / L"ffmpeg.exe", ec)) return aiDir / L"ffmpeg.exe";
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

VideoChunkWriter::VideoChunkWriter(VideoPack& pack, fs::path ffmpeg) : pack_(pack), ffmpeg_(std::move(ffmpeg)) {}
VideoChunkWriter::~VideoChunkWriter() { stop(); }

void VideoChunkWriter::stop() {
    if (!proc_) return;
    if (proc_->in) CloseHandle(proc_->in);
    if (proc_->reader.joinable()) proc_->reader.join();
    if (proc_->out) CloseHandle(proc_->out);
    if (proc_->process) {
        WaitForSingleObject(proc_->process, INFINITE);
        CloseHandle(proc_->process);
    }
    proc_.reset();
}

bool VideoChunkWriter::add(uint64_t hash, int width, int height, const uint8_t* rgb) {
    if (!proc_) {
        width_ = width;
        height_ = height;
        hashes_.clear();
        auto p = std::make_unique<Process>();
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE inRead = nullptr, outWrite = nullptr;
        if (!CreatePipe(&inRead, &p->in, &sa, 1 << 20)) return false;
        if (!CreatePipe(&p->out, &outWrite, &sa, 1 << 20)) {
            CloseHandle(inRead);
            CloseHandle(p->in);
            return false;
        }
        SetHandleInformation(p->in, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(p->out, HANDLE_FLAG_INHERIT, 0);
        // BT.709 limited range 4:2:0, no B-frames, a keyframe (with the
        // stream headers) every 30 frames and a delimiter before each frame
        wchar_t size[32];
        std::swprintf(size, 32, L"%dx%d", width, height);
        std::wstring cmd = L"\"" + ffmpeg_.wstring() +
                           L"\" -hide_banner -loglevel error -f rawvideo -pix_fmt rgb24 -s " + size +
                           L" -r 15 -i - -vf scale=out_color_matrix=bt709:out_range=tv:flags=bicubic+accurate_rnd+full_chroma_int"
                           L" -pix_fmt yuv420p -c:v libx264 -preset slow -crf 18 -bf 0 -g 30"
                           L" -x264-params keyint=30:min-keyint=30:scenecut=0:aud=1:repeat-headers=1"
                           L" -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -f h264 -";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                            nullptr, &si, &pi);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        if (!started) {
            CloseHandle(p->in);
            CloseHandle(p->out);
            std::printf("hdcache cannot run %s\n", ffmpeg_.string().c_str());
            return false;
        }
        CloseHandle(pi.hThread);
        p->process = pi.hProcess;
        Process* raw = p.get();
        p->reader = std::thread([raw] {
            uint8_t buf[1 << 16];
            DWORD got = 0;
            while (ReadFile(raw->out, buf, sizeof buf, &got, nullptr) && got) raw->stream.insert(raw->stream.end(), buf, buf + got);
        });
        proc_ = std::move(p);
    }
    if (width != width_ || height != height_) return false;
    size_t left = static_cast<size_t>(width) * height * 3;
    const uint8_t* at = rgb;
    while (left) {
        DWORD wrote = 0;
        if (!WriteFile(proc_->in, at, static_cast<DWORD>(std::min<size_t>(left, 1 << 20)), &wrote, nullptr) || !wrote) {
            stop();
            hashes_.clear();
            return false;
        }
        at += wrote;
        left -= wrote;
    }
    hashes_.push_back(hash);
    return true;
}

bool VideoChunkWriter::finish() {
    if (!proc_) return true;
    CloseHandle(proc_->in);
    proc_->in = nullptr;
    proc_->reader.join();
    WaitForSingleObject(proc_->process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(proc_->process, &code);
    std::vector<uint8_t> stream = std::move(proc_->stream);
    stop();
    std::vector<VideoPack::Frame> frames;
    if (code == 0) splitAccessUnits(stream, frames);
    const bool ok = code == 0 && frames.size() == hashes_.size() && !frames.empty() && frames.front().key;
    if (!ok) {
        std::printf("hdcache encoding a chunk of %zu pictures failed (ffmpeg exit code %lu, %zu frames out)\n",
                    hashes_.size(), static_cast<unsigned long>(code), frames.size());
        hashes_.clear();
        return false;
    }
    for (size_t i = 0; i < frames.size(); ++i) frames[i].hash = hashes_[i];
    hashes_.clear();
    return pack_.append(width_, height_, frames);
}

}  // namespace app
