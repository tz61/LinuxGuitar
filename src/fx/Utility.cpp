// Octaver (analog flip-flop style sub-octaves) and a volume / pan / width utility.
#include <cmath>

#include "dsp/Biquad.h"
#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

// ------------------------------------------------------------------ Octaver
// Similar to a Boss OC-2: zero crossings of the signal toggle flip-flops, and the flip-flop outputs (f/2 and f/4
// square waves) flip the polarity of the low-passed input. The result is one and two octaves down and keeps the
// picking envelope. Tracking only works on single notes.
class Octaver final : public Effect {
public:
    enum { kSub1, kSub2, kDry, kTone };
    Octaver()
        : Effect({pLin("sub1", "Sub -1 Oct", 0.f, 100.f, 60.f, "%"), pLin("sub2", "Sub -2 Oct", 0.f, 100.f, 0.f, "%"),
                  pLin("dry", "Dry", 0.f, 100.f, 80.f, "%"), pLog("tone", "Tone", 150.f, 2000.f, 600.f, "Hz")}) {}
    const char* typeId() const override { return "octaver"; }
    const char* displayName() const override { return "Octaver"; }
    FxCategory category() const override { return FxCategory::Pitch; }

    void prepare(double sampleRate, int) override {
        sr_ = sampleRate;
        trkLp_.c = rbj::lowpass(sr_, 900.0, 0.7);
        trkHp_.c = rbj::highpass(sr_, 60.0, 0.7);
        env_.setup(1.f, 60.f, (float)sr_);
        gateSm_.setTime(15.f, (float)sr_);
        for (auto* r : {&sub1_, &sub2_, &dry_}) r->tau = 0.02f;
        reset();
    }
    void reset() override {
        trkLp_.reset();
        trkHp_.reset();
        for (auto& b : subLp1_) b.reset();
        for (auto& b : subLp2_) b.reset();
        env_.reset();
        gateSm_.snap(0.f);
        schmitt_ = false;
        ff1_ = ff2_ = 1.f;
        sub1_.snap(param(kSub1) * 0.01f);
        sub2_.snap(param(kSub2) * 0.01f);
        dry_.snap(param(kDry) * 0.01f);
        lastTone_ = -1.f;
        updateTone();
    }
    void process(float* L, float* R, int n) override {
        updateTone();
        sub1_.begin(param(kSub1) * 0.01f, n, (float)sr_);
        sub2_.begin(param(kSub2) * 0.01f, n, (float)sr_);
        dry_.begin(param(kDry) * 0.01f, n, (float)sr_);
        const float gateThr = dbToGain(-52.f);
        for (int i = 0; i < n; ++i) {
            const float x = 0.5f * (L[i] + R[i]);
            const float t = trkHp_.process(trkLp_.process(x));
            const float e = env_.process(t);
            const float hyst = 0.2f * e;
            const bool prev = schmitt_;
            if (schmitt_ && t < -hyst) schmitt_ = false;
            else if (!schmitt_ && t > hyst) schmitt_ = true;
            if (schmitt_ && !prev) {  // rising edge
                ff1_ = -ff1_;
                if (ff1_ > 0.f) ff2_ = -ff2_;
            }
            const float gate = gateSm_.next(e > gateThr ? 1.f : 0.f);
            const float s1 = subLp1_[1].process(subLp1_[0].process(t * ff1_));
            const float s2 = subLp2_[1].process(subLp2_[0].process(t * ff2_));
            const float y = dry_.at(i) * x + gate * 2.2f * (sub1_.at(i) * s1 + sub2_.at(i) * s2);
            L[i] = y;
            R[i] = y;
        }
        sub1_.end();
        sub2_.end();
        dry_.end();
    }

private:
    void updateTone() {
        const float tone = param(kTone);
        if (tone == lastTone_) return;
        lastTone_ = tone;
        const BiquadCoefs c = rbj::lowpass(sr_, tone, 0.6);
        const BiquadCoefs c2 = rbj::lowpass(sr_, tone * 0.6, 0.6);
        for (auto& b : subLp1_) b.c = c;
        for (auto& b : subLp2_) b.c = c2;
    }
    double sr_ = 48000.0;
    Biquad trkLp_, trkHp_, subLp1_[2], subLp2_[2];
    EnvFollower env_;
    Smoother gateSm_;
    bool schmitt_ = false;
    float ff1_ = 1.f, ff2_ = 1.f;
    BlockRamp sub1_, sub2_, dry_;
    float lastTone_ = -1.f;
};

// ------------------------------------------------------------------ Volume / pan / width
class Volume final : public Effect {
public:
    enum { kGain, kPan, kWidth };
    Volume()
        : Effect({pLin("gain", "Volume", -40.f, 12.f, 0.f, "dB"), pLin("pan", "Pan", -100.f, 100.f, 0.f, ""),
                  pLin("width", "Width", 0.f, 200.f, 100.f, "%")}) {}
    const char* typeId() const override { return "volume"; }
    const char* displayName() const override { return "Volume"; }
    FxCategory category() const override { return FxCategory::Utility; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        for (auto* r : {&gl_, &gr_, &width_}) r->tau = 0.02f;
        reset();
    }
    void reset() override {
        float l, r;
        gains(l, r);
        gl_.snap(l);
        gr_.snap(r);
        width_.snap(param(kWidth) * 0.01f);
    }
    void process(float* L, float* R, int n) override {
        float l, r;
        gains(l, r);
        gl_.begin(l, n, sr_);
        gr_.begin(r, n, sr_);
        width_.begin(param(kWidth) * 0.01f, n, sr_);
        for (int i = 0; i < n; ++i) {
            const float w = width_.at(i);
            const float mid = 0.5f * (L[i] + R[i]), side = 0.5f * (L[i] - R[i]) * w;
            L[i] = (mid + side) * gl_.at(i);
            R[i] = (mid - side) * gr_.at(i);
        }
        gl_.end();
        gr_.end();
        width_.end();
    }

private:
    void gains(float& l, float& r) const {
        const float g = dbToGain(param(kGain));
        const float th = (param(kPan) * 0.01f + 1.f) * 0.25f * kPiF;  // equal-power pan; both gains are 1 at center
        l = g * std::cos(th) * 1.41421356f;
        r = g * std::sin(th) * 1.41421356f;
    }
    float sr_ = 48000.f;
    BlockRamp gl_, gr_, width_;
};

std::unique_ptr<Effect> makeOctaver() { return std::make_unique<Octaver>(); }
std::unique_ptr<Effect> makeVolume() { return std::make_unique<Volume>(); }

}  // namespace gs::fx
