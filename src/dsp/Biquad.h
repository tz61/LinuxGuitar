#pragma once
// RBJ biquad filters (coefficients and state in double, so they stay stable at low frequencies / high sample rates).
#include <cmath>

#include "dsp/DspUtil.h"

namespace gs::dsp {

struct BiquadCoefs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

namespace rbj {

inline double clampFreq(double sr, double f) { return clampv(f, 1.0, 0.49 * sr); }

inline BiquadCoefs normalize(double b0, double b1, double b2, double a0, double a1, double a2) {
    BiquadCoefs c;
    const double ia = 1.0 / a0;
    c.b0 = b0 * ia;
    c.b1 = b1 * ia;
    c.b2 = b2 * ia;
    c.a1 = a1 * ia;
    c.a2 = a2 * ia;
    return c;
}

inline BiquadCoefs lowpass(double sr, double f, double q = 0.70710678) {
    const double w = 2 * kPi * clampFreq(sr, f) / sr, cw = std::cos(w), al = std::sin(w) / (2 * q);
    return normalize((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + al, -2 * cw, 1 - al);
}
inline BiquadCoefs highpass(double sr, double f, double q = 0.70710678) {
    const double w = 2 * kPi * clampFreq(sr, f) / sr, cw = std::cos(w), al = std::sin(w) / (2 * q);
    return normalize((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + al, -2 * cw, 1 - al);
}
// Band-pass (0 dB peak gain)
inline BiquadCoefs bandpass(double sr, double f, double q) {
    const double w = 2 * kPi * clampFreq(sr, f) / sr, cw = std::cos(w), al = std::sin(w) / (2 * q);
    return normalize(al, 0, -al, 1 + al, -2 * cw, 1 - al);
}
inline BiquadCoefs notch(double sr, double f, double q) {
    const double w = 2 * kPi * clampFreq(sr, f) / sr, cw = std::cos(w), al = std::sin(w) / (2 * q);
    return normalize(1, -2 * cw, 1, 1 + al, -2 * cw, 1 - al);
}
inline BiquadCoefs allpass(double sr, double f, double q) {
    const double w = 2 * kPi * clampFreq(sr, f) / sr, cw = std::cos(w), al = std::sin(w) / (2 * q);
    return normalize(1 - al, -2 * cw, 1 + al, 1 + al, -2 * cw, 1 - al);
}
inline BiquadCoefs peaking(double sr, double f, double q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = 2 * kPi * clampFreq(sr, f) / sr, cw = std::cos(w), al = std::sin(w) / (2 * q);
    return normalize(1 + al * A, -2 * cw, 1 - al * A, 1 + al / A, -2 * cw, 1 - al / A);
}
inline BiquadCoefs lowShelf(double sr, double f, double gainDb, double q = 0.70710678) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = 2 * kPi * clampFreq(sr, f) / sr, cw = std::cos(w), al = std::sin(w) / (2 * q);
    const double sa = 2 * std::sqrt(A) * al;
    return normalize(A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa),
                     (A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa);
}
inline BiquadCoefs highShelf(double sr, double f, double gainDb, double q = 0.70710678) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = 2 * kPi * clampFreq(sr, f) / sr, cw = std::cos(w), al = std::sin(w) / (2 * q);
    const double sa = 2 * std::sqrt(A) * al;
    return normalize(A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa),
                     (A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa);
}

}  // namespace rbj

struct Biquad {
    BiquadCoefs c;
    double z1 = 0, z2 = 0;
    inline float process(float xf) {
        const double x = xf;
        const double y = c.b0 * x + z1;
        z1 = undenormal(c.b1 * x - c.a1 * y + z2);
        z2 = undenormal(c.b2 * x - c.a2 * y);
        return (float)y;
    }
    void processBlock(float* x, int n) {
        double s1 = z1, s2 = z2;
        const BiquadCoefs k = c;
        for (int i = 0; i < n; ++i) {
            const double in = x[i];
            const double y = k.b0 * in + s1;
            s1 = k.b1 * in - k.a1 * y + s2;
            s2 = k.b2 * in - k.a2 * y;
            x[i] = (float)y;
        }
        z1 = undenormal(s1);
        z2 = undenormal(s2);
    }
    void reset() { z1 = z2 = 0; }
};

// Magnitude response (dB), used by tests and the UI
inline double biquadMagDb(const BiquadCoefs& c, double f, double sr) {
    const double w = 2 * kPi * f / sr;
    const double cr1 = std::cos(w), ci1 = -std::sin(w), cr2 = std::cos(2 * w), ci2 = -std::sin(2 * w);
    const double nr = c.b0 + c.b1 * cr1 + c.b2 * cr2, ni = c.b1 * ci1 + c.b2 * ci2;
    const double dr = 1 + c.a1 * cr1 + c.a2 * cr2, di = c.a1 * ci1 + c.a2 * ci2;
    return 10 * std::log10((nr * nr + ni * ni) / std::max(dr * dr + di * di, 1e-300) + 1e-300);
}

}  // namespace gs::dsp
