#pragma once
// Single-producer / single-consumer lock-free ring buffer (real-time safe: no locks, no allocation).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace gs {

template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing only holds trivially copyable types");

public:
    SpscRing() = default;
    explicit SpscRing(size_t minCapacity) { init(minCapacity); }

    // Call from a non-real-time thread, before the producer/consumer start.
    void init(size_t minCapacity) {
        size_t c = 1;
        while (c < minCapacity) c <<= 1;
        buf_.reset(new T[c]);
        cap_ = c;
        mask_ = c - 1;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }
    size_t capacity() const { return cap_; }

    // ---- producer ----
    size_t writeAvailable() const {
        return cap_ - (size_t)(head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire));
    }
    size_t write(const T* src, size_t n) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        n = std::min(n, cap_ - (size_t)(h - t));
        if (n == 0) return 0;
        const size_t i0 = (size_t)h & mask_;
        const size_t first = std::min(n, cap_ - i0);
        std::memcpy(&buf_[i0], src, first * sizeof(T));
        if (n > first) std::memcpy(&buf_[0], src + first, (n - first) * sizeof(T));
        head_.store(h + n, std::memory_order_release);
        return n;
    }
    bool push(const T& v) { return write(&v, 1) == 1; }

    // ---- consumer ----
    size_t readAvailable() const {
        return (size_t)(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed));
    }
    size_t peek(T* dst, size_t n) const {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        n = std::min(n, (size_t)(h - t));
        if (n == 0) return 0;
        const size_t i0 = (size_t)t & mask_;
        const size_t first = std::min(n, cap_ - i0);
        std::memcpy(dst, &buf_[i0], first * sizeof(T));
        if (n > first) std::memcpy(dst + first, &buf_[0], (n - first) * sizeof(T));
        return n;
    }
    void skip(size_t n) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        n = std::min(n, (size_t)(h - t));
        tail_.store(t + n, std::memory_order_release);
    }
    size_t read(T* dst, size_t n) {
        n = peek(dst, n);
        skip(n);
        return n;
    }
    bool pop(T& v) { return read(&v, 1) == 1; }

private:
    std::unique_ptr<T[]> buf_;
    size_t cap_ = 0, mask_ = 0;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

// A double that can be read/written atomically (stored as a 64-bit bit pattern)
class AtomicDouble {
public:
    explicit AtomicDouble(double v = 0) { store(v); }
    void store(double v, std::memory_order o = std::memory_order_relaxed) {
        uint64_t b;
        std::memcpy(&b, &v, sizeof b);
        bits_.store(b, o);
    }
    double load(std::memory_order o = std::memory_order_relaxed) const {
        uint64_t b = bits_.load(o);
        double v;
        std::memcpy(&v, &b, sizeof v);
        return v;
    }

private:
    std::atomic<uint64_t> bits_{0};
};

}  // namespace gs
