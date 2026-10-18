#include "practice/PluckSynth.h"

#include <algorithm>
#include <cmath>

namespace gs {

void PluckSynth::prepare(double fs) {
    fs_ = fs;
    maxLen_ = (int)(fs / 30.0) + 8;  // down to ~30 Hz
    scratch_.assign(maxLen_, 0.f);
    for (auto& v : v_) {
        v.line.assign(maxLen_, 0.f);
        v.active = false;
        v.npend = 0;
    }
}

void PluckSynth::reset() {
    for (auto& v : v_) {
        std::fill(v.line.begin(), v.line.end(), 0.f);
        v.active = false;
        v.npend = 0;
    }
}

void PluckSynth::allNotesOff() {
    for (auto& v : v_) {
        v.npend = 0;
        if (v.active) v.fadeStep = -1.f / (0.02f * (float)fs_);
    }
}

void PluckSynth::startVoice(Voice& v, int midi, float vel, int durSamples) {
    const double f = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    v.s = 0.35f + 0.25f * (1.f - vel);  // harder = brighter (smaller s = less high-frequency loss)
    // Total loop delay = len + s (one-zero low-pass) + frac (allpass) = fs/f
    double D = fs_ / f - v.s;
    int N = (int)std::floor(D);
    double frac = D - N;
    if (frac < 0.15) {
        N -= 1;
        frac += 1.0;
    }
    N = std::clamp(N, 4, maxLen_ - 2);
    v.len = N;
    v.apC = (float)((1.0 - frac) / (1.0 + frac));
    v.apX1 = v.apY1 = v.lpPrev = 0.f;
    // Decay time: ~5 s on low strings, ~1.5 s high up the neck
    double t60 = std::clamp(5.5 - (f - 80.0) / 300.0, 1.2, 5.5);
    v.loss = (float)std::pow(0.001, 1.0 / (t60 * f));
    v.lossRelease = (float)std::pow(0.001, 1.0 / (0.25 * f));
    // Excitation: low-passed noise with a pick-position comb (~1/7 of the string length)
    std::uniform_real_distribution<float> U(-1.f, 1.f);
    const float bright = 0.25f + 0.6f * vel;
    float lp = 0;
    float* ex = scratch_.data();
    for (int i = 0; i < N; ++i) {
        lp += bright * (U(rng_) - lp);
        ex[i] = lp;
    }
    int P = std::max(1, (int)std::lround(N * 0.14));
    double mean = 0;
    for (int i = 0; i < N; ++i) {
        float e = ex[i] - (i >= P ? ex[i - P] : 0.f);
        v.line[i] = e;
        mean += e;
    }
    mean /= N;
    float pk = 1e-6f;
    for (int i = 0; i < N; ++i) {
        v.line[i] -= (float)mean;
        pk = std::max(pk, std::fabs(v.line[i]));
    }
    const float a = 0.5f * vel / pk;
    for (int i = 0; i < N; ++i) v.line[i] *= a;
    v.w = 0;
    v.amp = 1.f;
    v.fade = 1.f;
    v.fadeStep = 0.f;
    v.durLeft = durSamples > 0 ? durSamples : -1;
    // Panning: low strings slightly left, high strings slightly right
    float pan = v.string >= 0 ? 0.5f + 0.12f * (2.5f - (float)v.string) / 2.5f : 0.5f;
    pan = 1.f - pan;
    v.panL = std::cos(pan * (float)M_PI * 0.5f);
    v.panR = std::sin(pan * (float)M_PI * 0.5f);
    v.body1 = v.body2 = 0.f;
    v.active = true;
}

void PluckSynth::noteOn(int string, int midi, float velocity, int delaySamples, int durSamples) {
    if (midi < 20 || midi > 110) return;
    Voice* v = nullptr;
    if (string >= 0 && string < 6) {
        v = &v_[string];
    } else {
        for (int i = 6; i < kVoices; ++i)
            if (!v_[i].active && v_[i].npend == 0) v = &v_[i];
        if (!v) v = &v_[6];
    }
    v->string = string;
    velocity = std::clamp(velocity, 0.05f, 1.f);
    if (delaySamples > 0) {
        if (v->npend < 4) v->pend[v->npend++] = Voice::Pend{delaySamples, midi, durSamples, velocity};
    } else {
        startVoice(*v, midi, velocity, durSamples);
    }
}

void PluckSynth::render(float* L, float* R, int n, float gain) {
    for (auto& v : v_) {
        if (!v.active && v.npend == 0) continue;
        for (int i = 0; i < n; ++i) {
            if (v.npend > 0) {
                // Count down; a due note starts at once (cutting the string's previous note, like a real guitar)
                for (int k = 0; k < v.npend;) {
                    if (--v.pend[k].delay <= 0) {
                        Voice::Pend p = v.pend[k];
                        v.pend[k] = v.pend[--v.npend];
                        startVoice(v, p.midi, p.vel, p.dur);
                    } else {
                        ++k;
                    }
                }
            }
            if (!v.active) continue;
            int r = v.w;  // read and write the same slot = delay of len
            float y = v.line[r];
            float lpOut = (1.f - v.s) * y + v.s * v.lpPrev;
            v.lpPrev = y;
            float ap = v.apC * lpOut + v.apX1 - v.apC * v.apY1;
            v.apX1 = lpOut;
            v.apY1 = ap;
            float loss = v.loss;
            if (v.durLeft > 0 && --v.durLeft == 0) v.durLeft = 0;
            if (v.durLeft == 0) loss = v.lossRelease;
            v.line[r] = ap * loss;
            if (++v.w >= v.len) v.w = 0;
            // Body: gentle low-pass for a softer tone
            v.body1 += 0.45f * (y - v.body1);
            v.body2 += 0.45f * (v.body1 - v.body2);
            float out = v.body2 * v.fade * gain;
            if (v.fadeStep != 0.f) {
                v.fade += v.fadeStep;
                if (v.fade <= 0.f) {
                    v.fade = 0.f;
                    v.active = false;
                    v.fadeStep = 0.f;
                }
            }
            L[i] += out * v.panL;
            R[i] += out * v.panR;
        }
        // Stop once the voice has decayed below audibility
        if (v.active && v.durLeft == 0) {
            float e = 0;
            for (int k = 0; k < v.len; k += 4) e = std::max(e, std::fabs(v.line[k]));
            if (e < 1e-5f) v.active = false;
        }
    }
}

void ClickSynth::trigger(bool accent, int delaySamples) {
    if (delaySamples > 0) {
        delay_ = delaySamples;
        nextAccent_ = accent;
        return;
    }
    freq_ = accent ? 1760.f : 1320.f;
    amp_ = accent ? 0.9f : 0.6f;
    env_ = 1.f;
    phase_ = 0.f;
    decay_ = std::exp(-1.f / (0.010f * (float)fs_));
}

void ClickSynth::render(float* L, float* R, int n, float gain) {
    const float w = 2.f * (float)M_PI * freq_ / (float)fs_;
    for (int i = 0; i < n; ++i) {
        if (delay_ > 0 && --delay_ == 0) {
            delay_ = -1;
            trigger(nextAccent_, 0);
        }
        if (env_ < 1e-4f) continue;
        float s = std::sin(phase_) * env_ * amp_ * gain;
        phase_ += w;
        if (phase_ > 2.f * (float)M_PI) phase_ -= 2.f * (float)M_PI;
        env_ *= decay_;
        L[i] += s;
        R[i] += s;
    }
}

}  // namespace gs
