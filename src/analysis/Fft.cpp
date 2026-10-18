#include "analysis/Fft.h"

#include <cmath>
#include <utility>

namespace gs {

void Fft::init(int n) {
    n_ = n;
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    rev_.resize(n);
    for (int i = 0; i < n; ++i) {
        uint32_t r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1u << (bits - 1 - b);
        rev_[i] = r;
    }
    cos_.resize(n / 2);
    sin_.resize(n / 2);
    for (int k = 0; k < n / 2; ++k) {
        double a = -2.0 * M_PI * k / n;
        cos_[k] = (float)std::cos(a);
        sin_[k] = (float)std::sin(a);
    }
}

void Fft::transform(float* re, float* im, bool inverse) const {
    const int n = n_;
    for (int i = 0; i < n; ++i) {
        int j = (int)rev_[i];
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    const float sgn = inverse ? -1.f : 1.f;
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; ++j) {
                const float wr = cos_[j * step], wi = sgn * sin_[j * step];
                const int a = i + j, b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
    if (inverse) {
        const float k = 1.f / n;
        for (int i = 0; i < n; ++i) {
            re[i] *= k;
            im[i] *= k;
        }
    }
}

}  // namespace gs
