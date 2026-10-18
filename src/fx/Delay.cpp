// Delay: digital / analog (feedback saturation + tape wow and flutter) / ping-pong.
// The delay time glides smoothly, without clicks.
#include <cmath>

#include "dsp/Biquad.h"
#include "dsp/DelayLine.h"
#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

class Delay final : public Effect {
public:
    enum { kTime, kSync, kFeedback, kMix, kTone, kMode, kMod };
    enum { Digital = 0, Analog = 1, PingPong = 2 };
    Delay()
        : Effect({pLog("time", "Time", 20.f, 2000.f, 380.f, "ms"),
                  pChoice("sync", "Sync", {"Off", "1/4", "Dotted 1/8", "1/8", "1/8 Triplet", "1/16"}, 0),
                  pLin("feedback", "Feedback", 0.f, 95.f, 35.f, "%"), pLin("mix", "Echo", 0.f, 100.f, 30.f, "%"),
                  pLog("tone", "Tone", 800.f, 16000.f, 5000.f, "Hz"),
                  pChoice("mode", "Mode", {"Digital", "Analog", "Ping-Pong"}, 0), pLin("mod", "Mod", 0.f, 100.f, 15.f, "%")}) {}
    const char* typeId() const override { return "delay"; }
    const char* displayName() const override { return "Delay"; }
    FxCategory category() const override { return FxCategory::Delay; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        maxD_ = (int)(2.1f * sr_) + 16;
        for (auto& l : line_) l.allocate(maxD_ + 8);
        dSm_.setTime(120.f, sr_);
        fb_.tau = 0.03f;
        mix_.tau = 0.03f;
        reset();
    }
    void reset() override {
        for (auto& l : line_) l.reset();
        dSm_.snap(targetDelay());
        fb_.snap(param(kFeedback) * 0.01f);
        mix_.snap(param(kMix) * 0.01f);
        lp_[0].reset();
        lp_[1].reset();
        hp_[0].reset();
        hp_[1].reset();
        wow_.reset();
        flutter_.reset(0.3);
        lastTone_ = -1.f;
        lastMode_ = -1;
    }
    void process(float* L, float* R, int n) override {
        const int mode = (int)param(kMode);
        updateFilters(mode);
        const float dT = targetDelay();
        fb_.begin(param(kFeedback) * 0.01f, n, sr_);
        mix_.begin(param(kMix) * 0.01f, n, sr_);
        wow_.setRate(0.55, sr_);
        flutter_.setRate(5.3, sr_);
        const float mod = param(kMod) * 0.01f;
        const float wowAmp = mode == Analog ? mod * 0.0018f * sr_ : mod * 0.0005f * sr_;
        const float flutAmp = mode == Analog ? mod * 0.00012f * sr_ : 0.f;
        const bool analog = mode == Analog;
        for (int i = 0; i < n; ++i) {
            const float d0 = dSm_.next(dT);
            const float dm = d0 + wowAmp * (1.f + Lfo::sine(wow_.phase)) + flutAmp * Lfo::sine(flutter_.phase);
            wow_.advance();
            flutter_.advance();
            const float d = clampv(dm, 2.f, (float)maxD_) - 1.f;  // read before write
            const float wl = line_[0].readHermite(d);
            const float wr = line_[1].readHermite(d);
            const float fb = fb_.at(i), m = mix_.at(i);
            float fl = hp_[0].process(lp_[0].process(wl)) * fb;
            float fr = hp_[1].process(lp_[1].process(wr)) * fb;
            if (analog) {
                fl = fastTanh(fl * 1.3f) * (1.f / 1.3f);
                fr = fastTanh(fr * 1.3f) * (1.f / 1.3f);
            } else {
                fl = softClip(fl * 0.8f) * 1.25f;  // protection against extreme feedback; nearly linear at normal levels
                fr = softClip(fr * 0.8f) * 1.25f;
            }
            fl = undenormal(fl);
            fr = undenormal(fr);
            const float inL = L[i], inR = R[i];
            if (mode == PingPong) {
                line_[0].push(0.5f * (inL + inR) + fr);
                line_[1].push(fl);
            } else {
                line_[0].push(inL + fl);
                line_[1].push(inR + fr);
            }
            L[i] = inL + m * wl;
            R[i] = inR + m * wr;
        }
        fb_.end();
        mix_.end();
    }

private:
    float targetDelay() const {
        const int sync = (int)param(kSync);
        float sec = param(kTime) * 0.001f;
        if (sync > 0) {
            const float bpm = clampv(globalBpm().load(std::memory_order_relaxed), 20.f, 400.f);
            sec = 60.f / bpm * syncBeats(sync, true);
        }
        return clampv(sec, 0.002f, 2.0f) * sr_;
    }
    void updateFilters(int mode) {
        const float tone = param(kTone);
        if (tone == lastTone_ && mode == lastMode_) return;
        lastTone_ = tone;
        lastMode_ = mode;
        const float fc = mode == Analog ? std::min(tone, 3500.f) : tone;
        for (auto& f : lp_) f.setup(std::min(fc, 0.45f * sr_), sr_);
        for (auto& f : hp_) f.setup(80.f, sr_);
    }
    float sr_ = 48000.f;
    int maxD_ = 0;
    DelayLine line_[2];
    Smoother dSm_;
    BlockRamp fb_, mix_;
    OnePoleLP lp_[2];
    OnePoleHP hp_[2];
    Lfo wow_, flutter_;
    float lastTone_ = -1.f;
    int lastMode_ = -1;
};

std::unique_ptr<Effect> makeDelay() { return std::make_unique<Delay>(); }

}  // namespace gs::fx
