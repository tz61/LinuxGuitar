#pragma once
// State-variable filter (Simper/Cytomic TPT structure): the cutoff can be modulated per sample, numerically stable.
#include <cmath>

#include "dsp/DspUtil.h"

namespace gs::dsp {

struct Svf {
    float ic1 = 0.f, ic2 = 0.f;
    float k = 1.f, a1 = 0.f, a2 = 0.f, a3 = 0.f;

    void set(float fc, float q, float sr) {
        const float g = std::tan(kPiF * clampv(fc, 5.f, 0.49f * sr) / sr);
        setG(g, q);
    }
    // g = tan(pi*fc/sr); when modulating, callers can compute g themselves and skip the tan
    void setG(float g, float q) {
        k = 1.f / std::max(q, 0.05f);
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    // Outputs: lp low-pass, bp band-pass (peak gain = Q; multiply by k for a 0 dB band-pass), hp high-pass
    inline void process(float v0, float& lp, float& bp, float& hp) {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = undenormal(2.f * v1 - ic1);
        ic2 = undenormal(2.f * v2 - ic2);
        lp = v2;
        bp = v1;
        hp = v0 - k * v1 - v2;
    }
    inline float lowpass(float v0) {
        float lp, bp, hp;
        process(v0, lp, bp, hp);
        return lp;
    }
    inline float bandpassNorm(float v0) {
        float lp, bp, hp;
        process(v0, lp, bp, hp);
        return k * bp;
    }
    void reset() { ic1 = ic2 = 0.f; }
};

// First-order all-pass (for the phaser): y = a*x + x1 - a*y1, a = (t-1)/(t+1), t = tan(pi*f/sr)
struct Allpass1 {
    float x1 = 0.f, y1 = 0.f;
    inline float process(float x, float a) {
        const float y = a * x + x1 - a * y1;
        x1 = x;
        y1 = undenormal(y);
        return y;
    }
    void reset() { x1 = y1 = 0.f; }
};

}  // namespace gs::dsp
