// Dynamics: noise gate, compressor
#include <atomic>
#include <cmath>

#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

// ------------------------------------------------------------------ Noise gate
class NoiseGate final : public Effect {
public:
    enum { kThreshold, kAttack, kHold, kRelease, kRange };
    NoiseGate()
        : Effect({pLin("threshold", "Threshold", -90.f, -10.f, -60.f, "dB"), pLog("attack", "Attack", 0.1f, 20.f, 0.5f, "ms"),
                  pLin("hold", "Hold", 0.f, 500.f, 40.f, "ms"), pLog("release", "Release", 5.f, 1000.f, 120.f, "ms"),
                  pLin("range", "Range", -90.f, 0.f, -80.f, "dB")}) {}
    const char* typeId() const override { return "noisegate"; }
    const char* displayName() const override { return "Noise Gate"; }
    FxCategory category() const override { return FxCategory::Dynamics; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        detRel_ = timeCoef(25.f, sr_);
        reset();
    }
    void reset() override {
        env_ = 0.f;
        gain_ = dbToGain(param(kRange));
        open_ = false;
        holdLeft_ = 0;
        meter_.store(0.f, std::memory_order_relaxed);
    }
    void process(float* L, float* R, int n) override {
        const float thrDb = param(kThreshold);
        const float openLin = dbToGain(thrDb);
        const float closeLin = dbToGain(thrDb - 6.f);  // 6 dB hysteresis to avoid chattering around the threshold
        const float floorG = dbToGain(param(kRange));
        const float att = timeCoef(param(kAttack), sr_);
        const float rel = timeCoef(param(kRelease), sr_);
        const int holdN = (int)(param(kHold) * 0.001f * sr_);
        for (int i = 0; i < n; ++i) {
            const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
            env_ = a > env_ ? a : a + detRel_ * (env_ - a);
            if (open_) {
                if (env_ >= closeLin) holdLeft_ = holdN;
                else if (holdLeft_ > 0) --holdLeft_;
                else open_ = false;
            } else if (env_ >= openLin) {
                open_ = true;
                holdLeft_ = holdN;
            }
            const float target = open_ ? 1.f : floorG;
            const float c = target > gain_ ? att : rel;
            gain_ = target + c * (gain_ - target);
            L[i] *= gain_;
            R[i] *= gain_;
        }
        env_ = undenormal(env_);
        meter_.store(gain_, std::memory_order_relaxed);
    }
    int numMeters() const override { return 1; }
    const char* meterName(int) const override { return "Gate"; }
    float meterValue(int) const override { return meter_.load(std::memory_order_relaxed); }

private:
    float sr_ = 48000.f, detRel_ = 0.f, env_ = 0.f, gain_ = 0.f;
    bool open_ = false;
    int holdLeft_ = 0;
    std::atomic<float> meter_{0.f};
};

// ------------------------------------------------------------------ Compressor
class Compressor final : public Effect {
public:
    enum { kThreshold, kRatio, kAttack, kRelease, kKnee, kMakeup, kMix };
    Compressor()
        : Effect({pLin("threshold", "Threshold", -60.f, 0.f, -20.f, "dB"), pLog("ratio", "Ratio", 1.f, 20.f, 4.f, ":1"),
                  pLog("attack", "Attack", 0.1f, 100.f, 8.f, "ms"), pLog("release", "Release", 10.f, 2000.f, 150.f, "ms"),
                  pLin("knee", "Knee", 0.f, 18.f, 6.f, "dB"), pLin("makeup", "Makeup", 0.f, 30.f, 6.f, "dB"),
                  pLin("mix", "Mix", 0.f, 100.f, 100.f, "%")}) {}
    const char* typeId() const override { return "compressor"; }
    const char* displayName() const override { return "Compressor"; }
    FxCategory category() const override { return FxCategory::Dynamics; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        makeup_.tau = 0.03f;
        mix_.tau = 0.03f;
        reset();
    }
    void reset() override {
        grDb_ = 0.f;
        makeup_.snap(param(kMakeup));
        mix_.snap(param(kMix) * 0.01f);
        meter_.store(0.f, std::memory_order_relaxed);
    }
    void process(float* L, float* R, int n) override {
        const float T = param(kThreshold);
        const float ratio = std::max(1.f, param(kRatio));
        const float slope = 1.f / ratio - 1.f;  // <= 0
        const float W = param(kKnee);
        const float att = timeCoef(param(kAttack), sr_);
        const float rel = timeCoef(param(kRelease), sr_);
        makeup_.begin(param(kMakeup), n, sr_);
        mix_.begin(param(kMix) * 0.01f, n, sr_);
        for (int i = 0; i < n; ++i) {
            const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
            const float xg = 20.f * std::log10(a + 1e-9f);
            // Soft-knee gain computer (Giannoulis 2012)
            float over = xg - T, target;
            if (2.f * over <= -W) target = 0.f;
            else if (W > 0.f && 2.f * std::fabs(over) < W) {
                const float t = over + 0.5f * W;
                target = -slope * t * t / (2.f * W);
            } else target = -slope * over;
            const float c = target > grDb_ ? att : rel;
            grDb_ = target + c * (grDb_ - target);
            const float g = dbToGain(makeup_.at(i) - grDb_);
            const float m = mix_.at(i);
            const float k = (1.f - m) + m * g;
            L[i] *= k;
            R[i] *= k;
        }
        makeup_.end();
        mix_.end();
        grDb_ = undenormal(grDb_);
        meter_.store(grDb_, std::memory_order_relaxed);
    }
    int numMeters() const override { return 1; }
    const char* meterName(int) const override { return "Gain Reduction dB"; }
    float meterValue(int) const override { return meter_.load(std::memory_order_relaxed); }

private:
    float sr_ = 48000.f, grDb_ = 0.f;
    BlockRamp makeup_, mix_;
    std::atomic<float> meter_{0.f};
};

std::unique_ptr<Effect> makeNoiseGate() { return std::make_unique<NoiseGate>(); }
std::unique_ptr<Effect> makeCompressor() { return std::make_unique<Compressor>(); }

}  // namespace gs::fx
