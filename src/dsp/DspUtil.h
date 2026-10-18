#pragma once
// Small general-purpose DSP helpers (header-only, all real-time safe).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gs::dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr float kPiF = 3.14159265358979323846f;
constexpr float kTwoPiF = 6.28318530717958647692f;

inline float dbToGain(float db) { return std::exp(db * 0.115129254649702f); }  // ln(10)/20
inline float gainToDb(float g) { return 20.f * std::log10(std::max(g, 1e-9f)); }
template <class T>
inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Flush tiny values to zero (keeps denormals in recursive state from slowing down the CPU)
inline float undenormal(float x) { return std::fabs(x) < 1e-18f ? 0.f : x; }
inline double undenormal(double x) { return std::fabs(x) < 1e-30 ? 0.0 : x; }

// Rational tanh approximation: error < 2e-6 for |x| <= 4.97, saturates to ±1 beyond
inline float fastTanh(float x) {
    x = clampv(x, -4.97f, 4.97f);
    const float x2 = x * x;
    const float a = x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)));
    const float b = 135135.f + x2 * (62370.f + x2 * (3150.f + x2 * 28.f));
    return a / b;
}

// Cubic soft clipper: outputs ±1 for |x| >= 1.5, continuous derivative
inline float softClip(float x) {
    x = clampv(x, -1.5f, 1.5f);
    return x - (4.f / 27.f) * x * x * x;
}

// Soft diode clipper: x / sqrt(1+x^2), a gentler knee than tanh
inline float softDiode(float x) { return x / std::sqrt(1.f + x * x); }

// Hard-knee diode clipper: x / (1+x^4)^(1/4), harder than tanh
inline float diodeClip(float x) {
    const float x2 = x * x;
    return x / std::sqrt(std::sqrt(1.f + x2 * x2));
}

// One-pole coefficient: y += c * (x - y)
inline float onePoleCoef(float fc, float sr) { return 1.f - std::exp(-kTwoPiF * fc / sr); }
// Time-constant coefficient: y = target + a * (y - target)
inline float timeCoef(float ms, float sr) { return ms <= 0.f ? 0.f : std::exp(-1.f / (0.001f * ms * sr)); }

// Per-block parameter smoothing: a one-pole smoothed target per block, linearly interpolated within the
// block. One instance can be shared by both channel kernels.
struct BlockRamp {
    float cur = 0.f, next = 0.f, inc = 0.f;
    float tau = 0.02f;  // seconds
    void snap(float v) { cur = next = v; inc = 0.f; }
    void begin(float target, int n, float sr) {
        const float a = std::exp(-(float)n / (tau * sr));
        next = target + a * (cur - target);
        if (std::fabs(next - target) <= 1e-6f * (1.f + std::fabs(target))) next = target;
        inc = (next - cur) / (float)n;
    }
    float at(int i) const { return cur + inc * (float)i; }
    bool moving() const { return inc != 0.f; }
    void end() { cur = next; }
};

// Per-sample one-pole smoothing
struct Smoother {
    float y = 0.f, a = 0.f;
    void setTime(float ms, float sr) { a = timeCoef(ms, sr); }
    void snap(float v) { y = v; }
    float next(float target) {
        y = undenormal(target + a * (y - target));
        return y;
    }
};

// One-pole low-pass / high-pass (simple form, for static or slowly changing cutoffs)
struct OnePoleLP {
    float c = 1.f, y = 0.f;
    void setup(float fc, float sr) { c = onePoleCoef(fc, sr); }
    float process(float x) {
        y += c * (x - y);
        y = undenormal(y);
        return y;
    }
    void reset() { y = 0.f; }
};
struct OnePoleHP {
    OnePoleLP lp;
    void setup(float fc, float sr) { lp.setup(fc, sr); }
    float process(float x) { return x - lp.process(x); }
    void reset() { lp.reset(); }
};

// DC blocker
struct DcBlocker {
    float x1 = 0.f, y1 = 0.f, R = 0.999f;
    void setup(float fc, float sr) { R = clampv(1.f - kTwoPiF * fc / sr, 0.9f, 0.99999f); }
    float process(float x) {
        const float y = x - x1 + R * y1;
        x1 = x;
        y1 = undenormal(y);
        return y;
    }
    void reset() { x1 = y1 = 0.f; }
};

// Envelope follower (peak, separate attack/release)
struct EnvFollower {
    float env = 0.f, att = 0.f, rel = 0.f;
    void setup(float attMs, float relMs, float sr) {
        att = timeCoef(attMs, sr);
        rel = timeCoef(relMs, sr);
    }
    float process(float x) {
        const float a = std::fabs(x);
        const float c = a > env ? att : rel;
        env = a + c * (env - a);
        env = undenormal(env);
        return env;
    }
    void reset() { env = 0.f; }
};

// Phase-accumulator LFO (phase 0..1)
struct Lfo {
    double phase = 0.0, inc = 0.0;
    void setRate(double hz, double sr) { inc = hz / sr; }
    void advance() {
        phase += inc;
        if (phase >= 1.0) phase -= 1.0;
    }
    static float sine(double ph) { return (float)std::sin(2.0 * kPi * ph); }
    static float triangle(double ph) {
        const double p = ph - std::floor(ph);
        return (float)(p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p);
    }
    void reset(double ph = 0.0) { phase = ph; }
};

inline int nextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

// White noise (xorshift32), real-time safe
struct Noise {
    uint32_t s = 0x12345678u;
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (float)(int32_t)s * (1.f / 2147483648.f);
    }
};

// True when left and right are identical (the usual case with a mono guitar input)
inline bool sameChannels(const float* L, const float* R, int n) {
    return std::memcmp(L, R, sizeof(float) * (size_t)n) == 0;
}

}  // namespace gs::dsp
