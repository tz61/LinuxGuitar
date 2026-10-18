// Seven-band graphic EQ with low/high cut, and wah (manual / auto envelope / LFO).
#include <cmath>

#include "dsp/Biquad.h"
#include "dsp/Svf.h"
#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

// ------------------------------------------------------------------ Graphic EQ
class GraphicEq final : public Effect {
public:
    static constexpr int kBands = 7;
    enum { kLowCut = kBands, kHighCut, kLevel };
    GraphicEq()
        : Effect({pLin("b100", "100Hz", -12.f, 12.f, 0.f, "dB"), pLin("b200", "200Hz", -12.f, 12.f, 0.f, "dB"),
                  pLin("b400", "400Hz", -12.f, 12.f, 0.f, "dB"), pLin("b800", "800Hz", -12.f, 12.f, 0.f, "dB"),
                  pLin("b1k6", "1.6kHz", -12.f, 12.f, 0.f, "dB"), pLin("b3k2", "3.2kHz", -12.f, 12.f, 0.f, "dB"),
                  pLin("b6k4", "6.4kHz", -12.f, 12.f, 0.f, "dB"), pLog("lowcut", "Low Cut", 20.f, 500.f, 20.f, "Hz"),
                  pLog("highcut", "High Cut", 2000.f, 20000.f, 20000.f, "Hz"), pLin("level", "Level", -24.f, 12.f, 0.f, "dB")}) {}
    const char* typeId() const override { return "eq"; }
    const char* displayName() const override { return "EQ"; }
    FxCategory category() const override { return FxCategory::EQ; }

    void prepare(double sampleRate, int) override {
        sr_ = sampleRate;
        for (auto& b : band_) b.tau = 0.03f;
        reset();
    }
    void reset() override {
        for (auto& k : k_) k = Kernel{};
        for (int b = 0; b < kBands; ++b) band_[b].snap(param(b));
        level_.snap(dbToGain(param(kLevel)));
        lastLow_ = lastHigh_ = -1.f;
        updateCoefs(true);
    }
    void process(float* L, float* R, int n) override {
        for (int b = 0; b < kBands; ++b) band_[b].begin(param(b), n, (float)sr_);
        level_.begin(dbToGain(param(kLevel)), n, (float)sr_);
        updateCoefs(false);
        const bool useLow = lastLow_ > 20.5f, useHigh = lastHigh_ < 19900.f;
        runMonoKernels(k_, mono_, L, R, n, [&](Kernel& k, float* x) {
            for (int b = 0; b < kBands; ++b)
                if (active_[b]) k.peak[b].processBlock(x, n);
            if (useLow) k.low.processBlock(x, n);
            if (useHigh) k.high.processBlock(x, n);
            for (int i = 0; i < n; ++i) x[i] *= level_.at(i);
        });
        for (auto& b : band_) b.end();
        level_.end();
    }

private:
    struct Kernel {
        Biquad peak[kBands];
        Biquad low, high;
    };
    void updateCoefs(bool force) {
        static const double freqs[kBands] = {100, 200, 400, 800, 1600, 3200, 6400};
        for (int b = 0; b < kBands; ++b) {
            if (!force && !band_[b].moving() && band_[b].next == shown_[b]) continue;
            shown_[b] = band_[b].next;
            active_[b] = std::fabs(shown_[b]) > 0.01f;
            const BiquadCoefs c = rbj::peaking(sr_, freqs[b], 1.4, shown_[b]);
            for (auto& k : k_) k.peak[b].c = c;
        }
        const float lo = param(kLowCut), hi = param(kHighCut);
        if (force || lo != lastLow_ || hi != lastHigh_) {
            lastLow_ = lo;
            lastHigh_ = hi;
            const BiquadCoefs lc = rbj::highpass(sr_, lo, 0.707), hc = rbj::lowpass(sr_, std::min<double>(hi, 0.45 * sr_), 0.707);
            for (auto& k : k_) {
                k.low.c = lc;
                k.high.c = hc;
            }
        }
    }
    double sr_ = 48000.0;
    Kernel k_[2];
    bool mono_ = true;
    BlockRamp band_[kBands], level_;
    float shown_[kBands] = {};
    bool active_[kBands] = {};
    float lastLow_ = -1.f, lastHigh_ = -1.f;
};

// ------------------------------------------------------------------ Wah
class Wah final : public Effect {
public:
    enum { kMode, kPosition, kSens, kRate, kReso, kLow, kHigh, kMix };
    Wah()
        : Effect({pChoice("mode", "Mode", {"Manual", "Auto", "LFO"}, 1), pLin("position", "Pedal", 0.f, 1.f, 0.5f),
                  pLin("sensitivity", "Sensitivity", 0.f, 100.f, 50.f, "%"), pLog("rate", "Rate", 0.1f, 10.f, 1.5f, "Hz"),
                  pLog("resonance", "Resonance", 1.f, 12.f, 4.f), pLog("low", "Low Freq", 200.f, 1000.f, 380.f, "Hz"),
                  pLog("high", "High Freq", 1000.f, 5000.f, 2200.f, "Hz"), pLin("mix", "Mix", 0.f, 100.f, 100.f, "%")}) {}
    const char* typeId() const override { return "wah"; }
    const char* displayName() const override { return "Wah"; }
    FxCategory category() const override { return FxCategory::Filter; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        env_.setup(4.f, 140.f, sr_);
        posSm_.setTime(6.f, sr_);
        mix_.tau = 0.02f;
        reset();
    }
    void reset() override {
        svf_[0].reset();
        svf_[1].reset();
        env_.reset();
        lfo_.reset();
        posSm_.snap(param(kPosition));
        mix_.snap(param(kMix) * 0.01f);
        counter_ = 0;
    }
    void process(float* L, float* R, int n) override {
        const int mode = (int)param(kMode);
        const float manual = param(kPosition);
        const float sensGain = dbToGain(6.f + param(kSens) * 0.4f);  // 6..46 dB
        lfo_.setRate(param(kRate), sr_);
        const float q = param(kReso);
        const float lo = param(kLow), hi = std::max(param(kHigh), lo * 1.05f);
        const float ratio = std::log(hi / lo);
        mix_.begin(param(kMix) * 0.01f, n, sr_);
        const bool mono = sameChannels(L, R, n);
        for (int i = 0; i < n; ++i) {
            const float in = std::max(std::fabs(L[i]), std::fabs(R[i]));
            const float e = env_.process(in);
            float target;
            if (mode == 0) target = manual;
            else if (mode == 1) target = std::sqrt(clampv(e * sensGain, 0.f, 1.f));
            else target = 0.5f + 0.5f * Lfo::sine(lfo_.phase);
            lfo_.advance();
            const float pos = posSm_.next(target);
            if ((counter_++ & 3) == 0) {  // update the coefficients every 4 samples
                const float f = lo * std::exp(ratio * pos);
                const float g = std::tan(kPiF * std::min(f, 0.45f * sr_) / sr_);
                svf_[0].setG(g, q);
                svf_[1].setG(g, q);
            }
            const float m = mix_.at(i);
            L[i] = run(svf_[0], L[i], m);
            if (mono) R[i] = L[i];
            else R[i] = run(svf_[1], R[i], m);
        }
        if (mono) {
            svf_[1].ic1 = svf_[0].ic1;
            svf_[1].ic2 = svf_[0].ic2;
        }
        mix_.end();
    }

private:
    static inline float run(Svf& s, float x, float mix) {
        float lp, bp, hp;
        s.process(x, lp, bp, hp);
        const float wet = 3.0f * s.k * bp + 0.2f * lp;  // mostly band-pass, with a little low-pass to keep the body
        return x + mix * (wet - x);
    }
    float sr_ = 48000.f;
    Svf svf_[2];
    EnvFollower env_;
    Lfo lfo_;
    Smoother posSm_;
    BlockRamp mix_;
    unsigned counter_ = 0;
};

std::unique_ptr<Effect> makeEq() { return std::make_unique<GraphicEq>(); }
std::unique_ptr<Effect> makeWah() { return std::make_unique<Wah>(); }

}  // namespace gs::fx
