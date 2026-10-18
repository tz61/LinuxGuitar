// Drive effects: boost, overdrive (Tube Screamer style), distortion (RAT style), fuzz (Big Muff style).
// The nonlinear stages run at 4x oversampling.
#include <cmath>
#include <vector>

#include "dsp/Biquad.h"
#include "dsp/Oversampler.h"
#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

namespace {

// One-pole low-pass (coefficient supplied by the caller; used inside oversampled kernels, trivially copyable)
struct Lp1 {
    float y = 0.f;
    inline float run(float x, float c) {
        y += c * (x - y);
        return y;
    }
};
// One-pole high-pass: x - lp(x)
struct Hp1 {
    float y = 0.f;
    inline float run(float x, float c) {
        y += c * (x - y);
        return x - y;
    }
};

// Slightly asymmetric soft clipping (the negative half saturates a bit earlier, adding some even harmonics)
inline float asymSoft(float v, float neg) { return v >= 0.f ? softDiode(v) : softDiode(v * neg) / neg; }
inline float asymHard(float v, float neg) { return v >= 0.f ? diodeClip(v) : diodeClip(v * neg) / neg; }

// Shared part of the oversampled effects: design + scratch buffers
struct OsScratch {
    OversamplerDesign design;
    std::vector<float> hi, tmp;
    void prepare(int log2, int maxBlock) {
        design.setup(log2);
        hi.assign((size_t)maxBlock * (size_t)design.factor(), 0.f);
        tmp.assign((size_t)maxBlock * (size_t)design.factor() / 2 + 1, 0.f);
    }
};

}  // namespace

// ------------------------------------------------------------------ Boost
class Boost final : public Effect {
public:
    enum { kGain, kTilt, kLowCut };
    Boost()
        : Effect({pLin("gain", "Boost", 0.f, 24.f, 6.f, "dB"), pLin("tilt", "Tilt", -6.f, 6.f, 0.f, "dB"),
                  pLog("lowcut", "Low Cut", 20.f, 400.f, 30.f, "Hz")}) {}
    const char* typeId() const override { return "boost"; }
    const char* displayName() const override { return "Boost"; }
    FxCategory category() const override { return FxCategory::Drive; }

    void prepare(double sampleRate, int) override {
        sr_ = sampleRate;
        lastTilt_ = lastCut_ = -1e9f;
        reset();
    }
    void reset() override {
        for (auto& k : k_) k = Kernel{};
        gain_.snap(dbToGain(param(kGain)));
        lastTilt_ = lastCut_ = -1e9f;
        updateFilters();
    }
    void process(float* L, float* R, int n) override {
        updateFilters();
        gain_.begin(dbToGain(param(kGain)), n, (float)sr_);
        runMonoKernels(k_, mono_, L, R, n, [&](Kernel& k, float* x) {
            k.hp.processBlock(x, n);
            k.ls.processBlock(x, n);
            k.hs.processBlock(x, n);
            for (int i = 0; i < n; ++i) x[i] *= gain_.at(i);
        });
        gain_.end();
    }

private:
    struct Kernel {
        Biquad hp, ls, hs;
    };
    void updateFilters() {
        const float tilt = param(kTilt), cut = param(kLowCut);
        if (tilt == lastTilt_ && cut == lastCut_) return;
        lastTilt_ = tilt;
        lastCut_ = cut;
        const BiquadCoefs hp = rbj::highpass(sr_, cut, 0.707), ls = rbj::lowShelf(sr_, 800.0, -tilt * 0.5),
                          hs = rbj::highShelf(sr_, 800.0, tilt * 0.5);
        for (auto& k : k_) {
            k.hp.c = hp;
            k.ls.c = ls;
            k.hs.c = hs;
        }
    }
    double sr_ = 48000.0;
    Kernel k_[2];
    bool mono_ = true;
    BlockRamp gain_;
    float lastTilt_ = 0.f, lastCut_ = 0.f;
};

// ------------------------------------------------------------------ Overdrive (Tube Screamer style)
// Circuit essentials: the gain path only amplifies above ~720 Hz (mid hump), the diodes soft-clip inside the
// feedback loop, and the output is input + clipped signal.
class Overdrive final : public Effect {
public:
    enum { kDrive, kTone, kLevel };
    Overdrive()
        : Effect({pLin("drive", "Drive", 0.f, 10.f, 4.f), pLin("tone", "Tone", 0.f, 10.f, 5.f),
                  pLin("level", "Level", -24.f, 12.f, 0.f, "dB")}) {}
    const char* typeId() const override { return "overdrive"; }
    const char* displayName() const override { return "Overdrive"; }
    FxCategory category() const override { return FxCategory::Drive; }
    int latencySamples() const override { return latency_; }

    void prepare(double sampleRate, int maxBlock) override {
        sr_ = sampleRate;
        os_.prepare(2, maxBlock);
        srHi_ = (float)(sampleRate * os_.design.factor());
        latency_ = (int)std::lround(os_.design.latencyBase(1000.0, sampleRate));
        reset();
    }
    void reset() override {
        for (auto& k : k_) {
            k = Kernel{};
            k.os.clear();
            k.dc.setup(8.f, (float)sr_);
        }
        pot_.snap(potOf(param(kDrive)));
        level_.snap(levelOf(param(kLevel)));
        lastTone_ = -1.f;
        updateTone();
    }
    void process(float* L, float* R, int n) override {
        updateTone();
        pot_.begin(potOf(param(kDrive)), n, (float)sr_);
        level_.begin(levelOf(param(kLevel)), n, (float)sr_);
        const float pot = pot_.next;
        const float rf = 51e3f + pot * 500e3f;
        const float G = 1.f + rf / 4.7e3f;
        const float fcOp = std::min(1.f / (kTwoPiF * 51e-12f * rf), 0.45f * srHi_);
        const float cOp = onePoleCoef(fcOp, srHi_);
        const float cHp = onePoleCoef(720.f, srHi_);
        const float cPost = onePoleCoef(9000.f, srHi_);
        const int F = os_.design.factor();
        runMonoKernels(k_, mono_, L, R, n, [&](Kernel& k, float* x) {
            float* hi = os_.hi.data();
            osUp(os_.design, k.os, x, n, hi, os_.tmp.data());
            const int N = n * F;
            for (int j = 0; j < N; ++j) {
                const float v = hi[j];
                const float hp = k.hp.run(v, cHp);
                const float w = k.op.run(G * hp, cOp);
                const float c = 0.5f * asymSoft(w * 2.f, 1.12f);
                hi[j] = k.post.run(v + c, cPost);
            }
            osDown(os_.design, k.os, hi, n, x);
            k.tone.processBlock(x, n);
            for (int i = 0; i < n; ++i) x[i] = k.dc.process(x[i]) * level_.at(i);
        });
        for (auto& k : k_) {
            k.hp.y = undenormal(k.hp.y);
            k.op.y = undenormal(k.op.y);
            k.post.y = undenormal(k.post.y);
        }
        pot_.end();
        level_.end();
    }

private:
    struct Kernel {
        OversamplerState os;
        Hp1 hp;
        Lp1 op, post;
        Biquad tone;
        DcBlocker dc;
    };
    static float potOf(float drive) {
        const float d = drive * 0.1f;
        return d * d;  // approximates a log taper
    }
    static float levelOf(float db) { return dbToGain(db - 5.f); }  // internal calibration: close to the bypassed level
    void updateTone() {
        const float t = param(kTone);
        if (t == lastTone_) return;
        lastTone_ = t;
        const BiquadCoefs c = rbj::lowpass(sr_, 700.0 * std::pow(12.0, t * 0.1), 0.6);
        for (auto& k : k_) k.tone.c = c;
    }
    double sr_ = 48000.0;
    float srHi_ = 192000.f;
    int latency_ = 0;
    OsScratch os_;
    Kernel k_[2];
    bool mono_ = true;
    BlockRamp pot_, level_;
    float lastTone_ = -1.f;
};

// ------------------------------------------------------------------ Distortion (RAT style)
// Op-amp gain rising with frequency (corners at 60 Hz / 1.5 kHz), op-amp bandwidth limit, hard diode clipping
// to ground, variable low-pass.
class Distortion final : public Effect {
public:
    enum { kGain, kTone, kLevel };
    Distortion()
        : Effect({pLin("gain", "Gain", 0.f, 10.f, 5.f), pLin("tone", "Tone", 0.f, 10.f, 5.f),
                  pLin("level", "Level", -24.f, 12.f, 0.f, "dB")}) {}
    const char* typeId() const override { return "distortion"; }
    const char* displayName() const override { return "Distortion"; }
    FxCategory category() const override { return FxCategory::Drive; }
    int latencySamples() const override { return latency_; }

    void prepare(double sampleRate, int maxBlock) override {
        sr_ = sampleRate;
        os_.prepare(2, maxBlock);
        srHi_ = (float)(sampleRate * os_.design.factor());
        latency_ = (int)std::lround(os_.design.latencyBase(1000.0, sampleRate));
        reset();
    }
    void reset() override {
        for (auto& k : k_) {
            k = Kernel{};
            k.os.clear();
            k.dc.setup(8.f, (float)sr_);
        }
        rf_.snap(rfOf(param(kGain)));
        level_.snap(dbToGain(param(kLevel) - 7.f));
        lastTone_ = -1.f;
        updateTone();
    }
    void process(float* L, float* R, int n) override {
        updateTone();
        rf_.begin(rfOf(param(kGain)), n, (float)sr_);
        level_.begin(dbToGain(param(kLevel) - 7.f), n, (float)sr_);
        const float rf = rf_.next;
        const float gMid = rf / 560.f;
        const float gHi = rf / 43.f - rf / 560.f;
        const float fcBw = clampv(3e6f / (1.f + rf / 43.f), 1800.f, 0.45f * srHi_);
        const float cBw = onePoleCoef(fcBw, srHi_);
        const float cA = onePoleCoef(60.f, srHi_), cB = onePoleCoef(1539.f, srHi_);
        const float cPost = onePoleCoef(10000.f, srHi_);
        const int F = os_.design.factor();
        runMonoKernels(k_, mono_, L, R, n, [&](Kernel& k, float* x) {
            float* hi = os_.hi.data();
            osUp(os_.design, k.os, x, n, hi, os_.tmp.data());
            const int N = n * F;
            for (int j = 0; j < N; ++j) {
                const float v = hi[j];
                float w = v + gMid * k.hpA.run(v, cA) + gHi * k.hpB.run(v, cB);
                w = k.bw.run(w, cBw);
                w = 7.5f * fastTanh(w * (1.f / (7.5f * 0.6f)));  // op-amp supply rails (in units of the diode forward voltage)
                hi[j] = k.post.run(0.6f * diodeClip(w), cPost);   // hard diode clipping to ground
            }
            osDown(os_.design, k.os, hi, n, x);
            k.tone.processBlock(x, n);
            for (int i = 0; i < n; ++i) x[i] = k.dc.process(x[i]) * level_.at(i);
        });
        for (auto& k : k_) {
            k.hpA.y = undenormal(k.hpA.y);
            k.hpB.y = undenormal(k.hpB.y);
            k.bw.y = undenormal(k.bw.y);
            k.post.y = undenormal(k.post.y);
        }
        rf_.end();
        level_.end();
    }

private:
    struct Kernel {
        OversamplerState os;
        Hp1 hpA, hpB;
        Lp1 bw, post;
        Biquad tone;
        DcBlocker dc;
    };
    static float rfOf(float gain) {
        const float g = gain * 0.1f;
        return 100.f + 100e3f * g * g * g;  // log taper
    }
    void updateTone() {
        const float t = param(kTone);
        if (t == lastTone_) return;
        lastTone_ = t;
        const double fc = 500.0 * std::pow(24.0, t * 0.1);  // 500 Hz .. 12 kHz
        const BiquadCoefs c = rbj::lowpass(sr_, fc, 0.55);
        for (auto& k : k_) k.tone.c = c;
    }
    double sr_ = 48000.0;
    float srHi_ = 192000.f;
    int latency_ = 0;
    OsScratch os_;
    Kernel k_[2];
    bool mono_ = true;
    BlockRamp rf_, level_;
    float lastTone_ = -1.f;
};

// ------------------------------------------------------------------ Fuzz (Big Muff style)
// Two cascaded diode soft-clipping stages + a mid-scooped tone network.
class Fuzz final : public Effect {
public:
    enum { kFuzz, kTone, kLevel };
    Fuzz()
        : Effect({pLin("fuzz", "Fuzz", 0.f, 10.f, 6.f), pLin("tone", "Tone", 0.f, 10.f, 5.f),
                  pLin("level", "Level", -24.f, 12.f, 0.f, "dB")}) {}
    const char* typeId() const override { return "fuzz"; }
    const char* displayName() const override { return "Fuzz"; }
    FxCategory category() const override { return FxCategory::Drive; }
    int latencySamples() const override { return latency_; }

    void prepare(double sampleRate, int maxBlock) override {
        sr_ = sampleRate;
        os_.prepare(2, maxBlock);
        srHi_ = (float)(sampleRate * os_.design.factor());
        latency_ = (int)std::lround(os_.design.latencyBase(1000.0, sampleRate));
        reset();
    }
    void reset() override {
        for (auto& k : k_) {
            k = Kernel{};
            k.os.clear();
        }
        gain_.snap(gainOf(param(kFuzz)));
        tone_.snap(param(kTone) * 0.1f);
        level_.snap(dbToGain(param(kLevel) - 4.f));
        const BiquadCoefs lp = rbj::lowpass(sr_, 420.0, 0.5), hp = rbj::highpass(sr_, 1100.0, 0.5);
        for (auto& k : k_) {
            k.toneLp.c = lp;
            k.toneHp.c = hp;
            k.dc.setup(8.f, (float)sr_);
        }
    }
    void process(float* L, float* R, int n) override {
        gain_.begin(gainOf(param(kFuzz)), n, (float)sr_);
        tone_.begin(param(kTone) * 0.1f, n, (float)sr_);
        level_.begin(dbToGain(param(kLevel) - 4.f), n, (float)sr_);
        const float G1 = gain_.next;
        const float cHp1 = onePoleCoef(100.f, srHi_), cLp1 = onePoleCoef(5500.f, srHi_);
        const float cHp2 = onePoleCoef(90.f, srHi_), cLp2 = onePoleCoef(4000.f, srHi_);
        const int F = os_.design.factor();
        runMonoKernels(k_, mono_, L, R, n, [&](Kernel& k, float* x) {
            float* hi = os_.hi.data();
            osUp(os_.design, k.os, x, n, hi, os_.tmp.data());
            const int N = n * F;
            for (int j = 0; j < N; ++j) {
                float s = k.hp1.run(hi[j], cHp1) * G1;
                s = 0.6f * asymHard(s * (1.f / 0.6f), 1.05f);
                s = k.lp1.run(s, cLp1);
                s = k.hp2.run(s, cHp2) * 18.f;
                s = 0.6f * asymHard(s * (1.f / 0.6f), 1.08f);
                hi[j] = k.lp2.run(s, cLp2);
            }
            osDown(os_.design, k.os, hi, n, x);
            for (int i = 0; i < n; ++i) {
                const float t = tone_.at(i);
                const float lp = k.toneLp.process(x[i]), hp = k.toneHp.process(x[i]);
                x[i] = k.dc.process((1.f - t) * lp * 1.6f + t * hp * 2.0f) * level_.at(i);
            }
        });
        for (auto& k : k_) {
            k.hp1.y = undenormal(k.hp1.y);
            k.lp1.y = undenormal(k.lp1.y);
            k.hp2.y = undenormal(k.hp2.y);
            k.lp2.y = undenormal(k.lp2.y);
        }
        gain_.end();
        tone_.end();
        level_.end();
    }

private:
    struct Kernel {
        OversamplerState os;
        Hp1 hp1, hp2;
        Lp1 lp1, lp2;
        Biquad toneLp, toneHp;
        DcBlocker dc;
    };
    static float gainOf(float f) { return dbToGain(14.f + f * 3.6f); }
    double sr_ = 48000.0;
    float srHi_ = 192000.f;
    int latency_ = 0;
    OsScratch os_;
    Kernel k_[2];
    bool mono_ = true;
    BlockRamp gain_, tone_, level_;
};

std::unique_ptr<Effect> makeBoost() { return std::make_unique<Boost>(); }
std::unique_ptr<Effect> makeOverdrive() { return std::make_unique<Overdrive>(); }
std::unique_ptr<Effect> makeDistortion() { return std::make_unique<Distortion>(); }
std::unique_ptr<Effect> makeFuzz() { return std::make_unique<Fuzz>(); }

}  // namespace gs::fx
