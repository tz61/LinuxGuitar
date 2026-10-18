#pragma once
// Radix-2 complex FFT (separate real/imaginary arrays, avoids std::complex's slow multiply path).
#include <cstdint>
#include <vector>

namespace gs {

class Fft {
public:
    Fft() = default;
    explicit Fft(int n) { init(n); }
    void init(int n);  // n must be a power of two
    int size() const { return n_; }
    // In-place transform. With inverse = true the result is already divided by n.
    void transform(float* re, float* im, bool inverse) const;

private:
    int n_ = 0;
    std::vector<uint32_t> rev_;
    std::vector<float> cos_, sin_;
};

}  // namespace gs
