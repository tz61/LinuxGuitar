// Modulation effects: chorus, flanger, phaser, tremolo.
#include <cmath>

#include "dsp/DelayLine.h"
#include "dsp/Svf.h"
#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

// ------------------------------------------------------------------ Chorus
class Chorus final : public Effect {
public:
    enum { kRate, kDepth, kDelay, kMix, kWidth };
    Chorus()
        : Effect({pLog("rate", "Rate", 0.05f, 5.f, 0.8f, "Hz"), pLin("depth", "Depth", 0.f, 100.f, 40.f, "%"),
                  pLin("delay", "Delay", 3.f, 30.f, 10.f, "ms"), pLin("mix", "Mix", 0.f, 100.f, 50.f, "%"),
                  pLin("width", "Width", 0.f, 100.f, 80.f, "%")}) {}
    const char* typeId() const override { return "chorus"; }
    const char* displayName() const override { return "Chorus"; }
    FxCategory category() const override { return FxCategory::Modulation; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        for (auto& l : line_) l.allocate((int)(0.05f * sr_) + 8);
        baseSm_.setTime(50.f, sr_);
        depthSm_.setTime(50.f, sr_);
        mix_.tau = 0.02f;
        reset();
    }
    void reset() override {
        for (auto& l : line_) l.reset();
        lfo_.reset();
        baseSm_.snap(param(kDelay));
        depthSm_.snap(param(kDepth) * 0.01f);
        mix_.snap(param(kMix) * 0.01f);
    }
    void process(float* L, float* R, int n) override {
        lfo_.setRate(param(kRate), sr_);
        const float baseT = param(kDelay), depthT = param(kDepth) * 0.01f;
        const double off = param(kWidth) * 0.01 * 0.25;  // width 100% = 90° phase offset
        mix_.begin(param(kMix) * 0.01f, n, sr_);
        const float msToS = 0.001f * sr_;
        for (int i = 0; i < n; ++i) {
            const float base = baseSm_.next(baseT);
            const float depthMs = depthSm_.next(depthT) * std::min(4.f, base - 1.f);
            const float dl = (base + depthMs * Lfo::sine(lfo_.phase)) * msToS;
            const float dr = (base + depthMs * Lfo::sine(lfo_.phase + off)) * msToS;
            lfo_.advance();
            line_[0].push(L[i]);
            line_[1].push(R[i]);
            const float wl = line_[0].readHermite(std::max(dl, 1.f));
            const float wr = line_[1].readHermite(std::max(dr, 1.f));
            const float m = mix_.at(i);
            L[i] += m * (wl - L[i]);
            R[i] += m * (wr - R[i]);
        }
        mix_.end();
    }

private:
    float sr_ = 48000.f;
    DelayLine line_[2];
    Lfo lfo_;
    Smoother baseSm_, depthSm_;
    BlockRamp mix_;
};

// ------------------------------------------------------------------ Flanger
class Flanger final : public Effect {
public:
    enum { kRate, kDepth, kDelay, kFeedback, kMix };
    Flanger()
        : Effect({pLog("rate", "Rate", 0.02f, 5.f, 0.25f, "Hz"), pLin("depth", "Depth", 0.f, 100.f, 70.f, "%"),
                  pLog("delay", "Delay", 0.3f, 10.f, 2.f, "ms"), pLin("feedback", "Feedback", -95.f, 95.f, 50.f, "%"),
                  pLin("mix", "Mix", 0.f, 100.f, 50.f, "%")}) {}
    const char* typeId() const override { return "flanger"; }
    const char* displayName() const override { return "Flanger"; }
    FxCategory category() const override { return FxCategory::Modulation; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        for (auto& l : line_) l.allocate((int)(0.045f * sr_) + 8);
        delaySm_.setTime(60.f, sr_);
        fb_.tau = 0.03f;
        mix_.tau = 0.02f;
        reset();
    }
    void reset() override {
        for (auto& l : line_) l.reset();
        lfo_.reset();
        delaySm_.snap(param(kDelay));
        fb_.snap(param(kFeedback) * 0.01f);
        mix_.snap(param(kMix) * 0.01f);
        last_[0] = last_[1] = 0.f;
    }
    void process(float* L, float* R, int n) override {
        lfo_.setRate(param(kRate), sr_);
        const float depth = param(kDepth) * 0.01f * 2.f;  // full depth = ±2 octaves
        const float manualT = param(kDelay);
        fb_.begin(param(kFeedback) * 0.01f, n, sr_);
        mix_.begin(param(kMix) * 0.01f, n, sr_);
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            const float manual = delaySm_.next(manualT);
            const float fb = fb_.at(i), m = mix_.at(i);
            for (int c = 0; c < 2; ++c) {
                const float lfo = Lfo::sine(lfo_.phase + 0.25 * c);
                const float dms = clampv(manual * std::exp2(depth * lfo), 0.05f, 40.f);
                const float d = std::max(dms * 0.001f * sr_, 1.f);
                const float x = io[c][i];
                line_[c].push(x + softClip(fb * last_[c]));
                const float w = line_[c].readHermite(d);
                last_[c] = undenormal(w);
                io[c][i] = x + m * (w - x);
            }
            lfo_.advance();
        }
        fb_.end();
        mix_.end();
    }

private:
    float sr_ = 48000.f;
    DelayLine line_[2];
    Lfo lfo_;
    Smoother delaySm_;
    BlockRamp fb_, mix_;
    float last_[2] = {0.f, 0.f};
};

// ------------------------------------------------------------------ Phaser
class Phaser final : public Effect {
public:
    enum { kRate, kDepth, kFeedback, kStages, kCenter, kMix, kStereo };
    static constexpr int kMaxStages = 12;
    Phaser()
        : Effect({pLog("rate", "Rate", 0.02f, 8.f, 0.5f, "Hz"), pLin("depth", "Depth", 0.f, 100.f, 70.f, "%"),
                  pLin("feedback", "Feedback", 0.f, 90.f, 40.f, "%"), pChoice("stages", "Stages", {"4", "6", "8", "12"}, 0),
                  pLog("center", "Center Freq", 100.f, 4000.f, 700.f, "Hz"), pLin("mix", "Mix", 0.f, 100.f, 50.f, "%"),
                  pLin("stereo", "Stereo", 0.f, 100.f, 50.f, "%")}) {}
    const char* typeId() const override { return "phaser"; }
    const char* displayName() const override { return "Phaser"; }
    FxCategory category() const override { return FxCategory::Modulation; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        fb_.tau = 0.03f;
        mix_.tau = 0.02f;
        centerSm_.setTime(40.f, sr_);
        reset();
    }
    void reset() override {
        for (auto& ch : ap_)
            for (auto& a : ch) a.reset();
        last_[0] = last_[1] = 0.f;
        coef_[0] = coef_[1] = 0.f;
        lfo_.reset();
        fb_.snap(param(kFeedback) * 0.01f);
        mix_.snap(param(kMix) * 0.01f);
        centerSm_.snap(param(kCenter));
        counter_ = 0;
    }
    void process(float* L, float* R, int n) override {
        static const int stagesOf[4] = {4, 6, 8, 12};
        const int ns = stagesOf[clampv((int)param(kStages), 0, 3)];
        lfo_.setRate(param(kRate), sr_);
        const float depth = param(kDepth) * 0.01f * 2.5f;  // ±2.5 octaves
        const double off = param(kStereo) * 0.01 * 0.5;    // 100% = 180°
        const float centerT = param(kCenter);
        fb_.begin(param(kFeedback) * 0.01f, n, sr_);
        mix_.begin(param(kMix) * 0.01f, n, sr_);
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            const float center = centerSm_.next(centerT);
            if ((counter_++ & 3) == 0) {
                for (int c = 0; c < 2; ++c) {
                    const float f = clampv(center * std::exp2(depth * Lfo::sine(lfo_.phase + off * c)), 30.f, 0.45f * sr_);
                    const float t = std::tan(kPiF * f / sr_);
                    coef_[c] = (t - 1.f) / (t + 1.f);
                }
            }
            lfo_.advance();
            const float fb = fb_.at(i), m = mix_.at(i);
            for (int c = 0; c < 2; ++c) {
                const float x = io[c][i];
                float y = x + softClip(fb * last_[c]);
                for (int s = 0; s < ns; ++s) y = ap_[c][s].process(y, coef_[c]);
                last_[c] = y;
                io[c][i] = x + m * (y - x);
            }
        }
        fb_.end();
        mix_.end();
    }

private:
    float sr_ = 48000.f;
    Allpass1 ap_[2][kMaxStages];
    float last_[2] = {0.f, 0.f}, coef_[2] = {0.f, 0.f};
    Lfo lfo_;
    BlockRamp fb_, mix_;
    Smoother centerSm_;
    unsigned counter_ = 0;
};

// ------------------------------------------------------------------ Tremolo (amplitude modulation)
class Tremolo final : public Effect {
public:
    enum { kRate, kSync, kDepth, kWave, kPan };
    Tremolo()
        : Effect({pLog("rate", "Rate", 0.5f, 15.f, 5.f, "Hz"), pChoice("sync", "Sync", {"Off", "1/4", "1/8", "1/8 Triplet", "1/16"}, 0),
                  pLin("depth", "Depth", 0.f, 100.f, 50.f, "%"), pChoice("wave", "Wave", {"Sine", "Triangle", "Square"}, 0),
                  pToggle("pan", "Auto Pan", false)}) {}
    const char* typeId() const override { return "tremolo"; }
    const char* displayName() const override { return "Tremolo"; }
    FxCategory category() const override { return FxCategory::Modulation; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        sq_.setTime(2.5f, sr_);
        depth_.tau = 0.03f;
        reset();
    }
    void reset() override {
        lfo_.reset();
        sq_.snap(1.f);
        depth_.snap(param(kDepth) * 0.01f);
    }
    void process(float* L, float* R, int n) override {
        const int sync = (int)param(kSync);
        float rate = param(kRate);
        if (sync > 0) {
            const float beats = syncBeats(sync, false);
            rate = clampv(globalBpm().load(std::memory_order_relaxed) / 60.f / beats, 0.05f, 40.f);
        }
        lfo_.setRate(rate, sr_);
        const int wave = (int)param(kWave);
        const bool pan = param(kPan) > 0.5f;
        depth_.begin(param(kDepth) * 0.01f, n, sr_);
        for (int i = 0; i < n; ++i) {
            float w;
            if (wave == 0) w = Lfo::sine(lfo_.phase);
            else if (wave == 1) w = Lfo::triangle(lfo_.phase);
            else w = sq_.next(lfo_.phase < 0.5 ? 1.f : -1.f);
            lfo_.advance();
            const float d = depth_.at(i);
            if (pan) {
                L[i] *= 1.f - d * (0.5f - 0.5f * w);
                R[i] *= 1.f - d * (0.5f + 0.5f * w);
            } else {
                const float g = 1.f - d * (0.5f - 0.5f * w);
                L[i] *= g;
                R[i] *= g;
            }
        }
        depth_.end();
    }

private:
    float sr_ = 48000.f;
    Lfo lfo_;
    Smoother sq_;
    BlockRamp depth_;
};

std::unique_ptr<Effect> makeChorus() { return std::make_unique<Chorus>(); }
std::unique_ptr<Effect> makeFlanger() { return std::make_unique<Flanger>(); }
std::unique_ptr<Effect> makePhaser() { return std::make_unique<Phaser>(); }
std::unique_ptr<Effect> makeTremolo() { return std::make_unique<Tremolo>(); }

}  // namespace gs::fx
