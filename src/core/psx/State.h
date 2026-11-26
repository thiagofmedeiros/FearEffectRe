// Save states: one stream that either writes the machine's state or reads it
// back, so every part describes its fields once (`void state(StateIO&)`).
#pragma once

#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "psx/Machine.h"

namespace fe {

class StateIO {
public:
    // Writing: starts empty. Reading: from `data`.
    StateIO() : loading_(false) {}
    explicit StateIO(std::vector<u8> data) : loading_(true), buf_(std::move(data)) {}

    bool loading() const { return loading_; }
    const std::vector<u8>& data() const { return buf_; }

    void bytes(void* p, size_t n) {
        if (loading_) {
            if (pos_ + n > buf_.size()) throw std::runtime_error("save state is truncated");
            std::memcpy(p, buf_.data() + pos_, n);
            pos_ += n;
        } else {
            const u8* b = static_cast<const u8*>(p);
            buf_.insert(buf_.end(), b, b + n);
        }
    }
    template <typename T>
    void operator()(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "describe this type field by field");
        bytes(&v, sizeof v);
    }
    template <typename T>
    void operator()(std::vector<T>& v) {
        u64 n = v.size();
        (*this)(n);
        if (loading_) v.resize(static_cast<size_t>(n));
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (n) bytes(v.data(), static_cast<size_t>(n) * sizeof(T));
        } else {
            for (auto& e : v) (*this)(e);
        }
    }
    template <typename T>
    void operator()(std::deque<T>& v) {
        std::vector<T> tmp(v.begin(), v.end());
        (*this)(tmp);
        if (loading_) v.assign(tmp.begin(), tmp.end());
    }
    void operator()(std::string& s) {
        u64 n = s.size();
        (*this)(n);
        if (loading_) s.resize(static_cast<size_t>(n));
        if (n) bytes(s.data(), static_cast<size_t>(n));
    }
    // A marker between sections: a mismatch means the file is from another version.
    void tag(u32 value) {
        u32 v = value;
        (*this)(v);
        if (v != value) throw std::runtime_error("save state does not match this version of the program");
    }

private:
    bool loading_;
    std::vector<u8> buf_;
    size_t pos_ = 0;
};

}  // namespace fe
