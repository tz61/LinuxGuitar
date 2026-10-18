#pragma once
// Circular delay line (power-of-two length) with integer / linear / Hermite interpolated reads.
// Convention: after push(x), tap(0) == x and tap(d) == the value written d samples earlier.
#include <algorithm>
#include <vector>

#include "dsp/DspUtil.h"

namespace gs::dsp {

class DelayLine {
public:
    // Non-real-time: allocate a buffer that can hold maxDelay samples of delay
    void allocate(int maxDelay) {
        size_ = nextPow2(std::max(maxDelay, 4) + 4);
        mask_ = size_ - 1;
        buf_.assign((size_t)size_, 0.f);
        w_ = 0;
    }
    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.f);
        w_ = 0;
    }
    int capacity() const { return size_ - 4; }

    inline void push(float x) {
        buf_[(size_t)w_] = x;
        w_ = (w_ + 1) & mask_;
    }
    inline float tap(int d) const { return buf_[(size_t)((w_ - 1 - d) & mask_)]; }
    inline float readLinear(float d) const {
        const int i = (int)d;
        const float f = d - (float)i;
        const float a = tap(i), b = tap(i + 1);
        return a + f * (b - a);
    }
    // 4-point, 3rd-order Hermite; requires d >= 1
    inline float readHermite(float d) const {
        const int i = (int)d;
        const float f = d - (float)i;
        const float xm1 = tap(i - 1), x0 = tap(i), x1 = tap(i + 1), x2 = tap(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

private:
    std::vector<float> buf_;
    int size_ = 0, mask_ = 0, w_ = 0;
};

// Schroeder all-pass with an embedded delay line, used for reverb diffusion
class AllpassDelay {
public:
    void allocate(int maxDelay) { line_.allocate(maxDelay + 2); }
    void reset() { line_.reset(); }
    // d: delay in samples (fractional, >= 1), g: coefficient
    inline float process(float x, float d, float g) {
        const float delayed = line_.readLinear(d - 1.f);
        const float v = x + g * delayed;
        line_.push(undenormal(v));
        return delayed - g * v;
    }
    // Read the internal delay line (for Dattorro output taps), relative to the latest write
    inline float tap(int d) const { return line_.tap(d); }

private:
    DelayLine line_;
};

}  // namespace gs::dsp
