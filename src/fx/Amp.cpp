// Amp model: multi-stage tube preamp (asymmetric saturation + inter-stage filtering) -> TMB tone stack ->
// push-pull power amp (with supply sag) -> presence / low-end resonance.
// The whole nonlinear section runs at 4x oversampling. Each channel's level is calibrated, so switching channels
// or changing the gain doesn't change the volume much.
#include <cmath>
#include <vector>

#include "dsp/Biquad.h"
#include "dsp/Oversampler.h"
#include "dsp/ToneStack.h"
#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

namespace {

struct Voicing {
    int stages;
    float inHp;                 // input high-pass (tightness)
    float g1DbMin, g1DbPerStep; // first-stage gain (dB) = min + gain knob * step
    float stageGain[4];         // inter-stage gain
    float bias[4];              // per-stage bias (asymmetry)
    float cplHp[4];             // inter-stage coupling high-pass, Hz
    float millerLp[4];          // inter-stage Miller low-pass, Hz
    ToneStack::Model stack;
    float stackMakeupDb;
    float powerDrive;
    float sag;
    float depthDb;              // low-end resonance boost
    float trimDb;               // output level calibration
    float trimPerGainDb;        // level compensation that tracks the gain knob (dB per step)
};

// Clean / Crunch / Lead / Metal
const Voicing kVoicings[4] = {
    {2, 35.f, -8.f, 1.9f, {1.6f, 1.f, 1.f, 1.f}, {0.08f, 0.12f, 0.f, 0.f}, {20.f, 25.f, 25.f, 25.f},
     {16000.f, 14000.f, 12000.f, 12000.f}, ToneStack::Fender, 13.f, 0.9f, 0.15f, 0.f, 2.0f, -0.9f},
    {3, 45.f, -14.f, 3.0f, {2.5f, 2.0f, 1.f, 1.f}, {0.15f, 0.2f, 0.12f, 0.f}, {30.f, 60.f, 40.f, 40.f},
     {13000.f, 11000.f, 9000.f, 9000.f}, ToneStack::Marshall, 12.f, 1.4f, 0.3f, 1.5f, -4.9f, -1.0f},
    {4, 70.f, -10.f, 3.0f, {3.5f, 2.5f, 2.f, 1.f}, {0.2f, 0.25f, 0.2f, 0.15f}, {40.f, 110.f, 130.f, 60.f},
     {11000.f, 9000.f, 7500.f, 7000.f}, ToneStack::Marshall, 12.f, 1.3f, 0.25f, 2.0f, -10.0f, -0.4f},
    {4, 110.f, -2.f, 3.0f, {5.f, 3.5f, 3.f, 1.f}, {0.25f, 0.3f, 0.2f, 0.1f}, {60.f, 120.f, 140.f, 90.f},
     {10000.f, 8000.f, 7000.f, 6500.f}, ToneStack::Mesa, 13.f, 1.2f, 0.2f, 2.5f, -9.8f, -0.1f},
};

struct Lp1 {
    float y = 0.f;
    inline float run(float x, float c) {
        y += c * (x - y);
        return y;
    }
};
struct Hp1 {
    float y = 0.f;
    inline float run(float x, float c) {
        y += c * (x - y);
        return x - y;
    }
};

}  // namespace

class Amp final : public Effect {
public:
    enum { kChannel, kGain, kBass, kMid, kTreble, kPresence, kMaster, kBright };
    Amp()
        : Effect({pChoice("channel", "Channel", {"Clean", "Crunch", "Lead", "Metal"}, 0),
                  pLin("gain", "Gain", 0.f, 10.f, 4.f), pLin("bass", "Bass", 0.f, 10.f, 5.f), pLin("mid", "Mid", 0.f, 10.f, 5.f),
                  pLin("treble", "Treble", 0.f, 10.f, 5.f), pLin("presence", "Presence", 0.f, 10.f, 5.f),
                  pLin("master", "Master", -30.f, 6.f, 0.f, "dB"), pToggle("bright", "Bright", false)}) {}
    const char* typeId() const override { return "amp"; }
    const char* displayName() const override { return "Amp"; }
    FxCategory category() const override { return FxCategory::Amp; }
    int latencySamples() const override { return latency_; }

    void prepare(double sampleRate, int maxBlock) override {
        sr_ = sampleRate;
        design_.setup(2);
        F_ = design_.factor();
        srHi_ = (float)(sampleRate * F_);
        hi_.assign((size_t)maxBlock * (size_t)F_, 0.f);
        tmp_.assign((size_t)maxBlock * (size_t)F_ / 2 + 1, 0.f);
        latency_ = (int)std::lround(design_.latencyBase(1000.0, sampleRate));
        reset();
    }

    void reset() override {
        channel_ = (int)param(kChannel);
        configure(channel_);
        gain_.snap(param(kGain));
        bass_.snap(param(kBass) * 0.1f);
        mid_.snap(param(kMid) * 0.1f);
        treble_.snap(param(kTreble) * 0.1f);
        out_.snap(outGain());
        fade_ = 1.f;
        fadingOut_ = false;
        lastPresence_ = lastBright_ = -100.f;
        updateTone(true);
    }

    void process(float* L, float* R, int n) override {
        // Channel switch: fade out over one block, reconfigure, then fade back in
        const int ch = (int)param(kChannel);
        if (ch != channel_ && !fadingOut_) fadingOut_ = true;
        float fadeFrom = fade_, fadeTo = fade_;
        if (fadingOut_) fadeTo = 0.f;
        else if (fade_ < 1.f) fadeTo = std::min(1.f, fade_ + (float)n / (0.01f * (float)sr_));

        const Voicing& v = kVoicings[channel_];
        gain_.begin(param(kGain), n, (float)sr_);
        bass_.begin(param(kBass) * 0.1f, n, (float)sr_);
        mid_.begin(param(kMid) * 0.1f, n, (float)sr_);
        treble_.begin(param(kTreble) * 0.1f, n, (float)sr_);
        out_.begin(outGain(), n, (float)sr_);
        updateTone(false);

        const float g1a = dbToGain(v.g1DbMin + v.g1DbPerStep * gain_.cur);
        const float g1b = dbToGain(v.g1DbMin + v.g1DbPerStep * gain_.next);
        const float g1Inc = (g1b - g1a) / (float)(n * F_);
        float cCpl[4], cMil[4];
        for (int s = 0; s < 4; ++s) {
            cCpl[s] = onePoleCoef(v.cplHp[s], srHi_);
            cMil[s] = onePoleCoef(v.millerLp[s], srHi_);
        }
        float tb[4];
        for (int s = 0; s < 4; ++s) tb[s] = std::tanh(v.bias[s]);
        const float stackMk = dbToGain(v.stackMakeupDb);
        const float sagAtt = timeCoef(8.f, srHi_), sagRel = timeCoef(180.f, srHi_);
        const int stages = v.stages;

        runMonoKernels(k_, mono_, L, R, n, [&](Kernel& k, float* x) {
            k.inHp.processBlock(x, n);
            k.bright.processBlock(x, n);
            float* hi = hi_.data();
            osUp(design_, k.os, x, n, hi, tmp_.data());
            const int N = n * F_;
            float g1 = g1a;
            for (int j = 0; j < N; ++j) {
                float s = hi[j] * g1;
                g1 += g1Inc;
                for (int st = 0; st < stages; ++st) {
                    s = fastTanh(s + v.bias[st]) - tb[st];
                    s = k.cpl[st].run(s, cCpl[st]);
                    s = k.mil[st].run(s, cMil[st]);
                    s *= -v.stageGain[st];
                }
                s = k.stack.process(s) * stackMk;
                // Power amp: supply sag lowers the gain on loud signals
                const float a = std::fabs(s);
                k.sagEnv = a + (a > k.sagEnv ? sagAtt : sagRel) * (k.sagEnv - a);
                const float gpa = v.powerDrive / (1.f + v.sag * k.sagEnv);
                hi[j] = fastTanh(s * gpa);
            }
            osDown(design_, k.os, hi, n, x);
            k.presence.processBlock(x, n);
            k.depth.processBlock(x, n);
            for (int i = 0; i < n; ++i) x[i] = k.dc.process(x[i]) * out_.at(i);
        });
        for (auto& k : k_) {
            for (int s = 0; s < 4; ++s) {
                k.cpl[s].y = undenormal(k.cpl[s].y);
                k.mil[s].y = undenormal(k.mil[s].y);
            }
            k.stack.flush();
            k.sagEnv = undenormal(k.sagEnv);
        }
        gain_.end();
        bass_.end();
        mid_.end();
        treble_.end();
        out_.end();

        if (fadeFrom != 1.f || fadeTo != 1.f) {
            const float inc = (fadeTo - fadeFrom) / (float)n;
            for (int i = 0; i < n; ++i) {
                const float g = fadeFrom + inc * (float)i;
                L[i] *= g;
                R[i] *= g;
            }
        }
        fade_ = fadeTo;
        if (fadingOut_ && fade_ <= 0.f) {
            channel_ = ch;
            configure(channel_);
            out_.snap(outGain());
            lastPresence_ = lastBright_ = -100.f;
            updateTone(true);
            fadingOut_ = false;
            fade_ = 0.f;
        }
    }

private:
    struct Kernel {
        OversamplerState os;
        Biquad inHp, bright, presence, depth;
        Hp1 cpl[4];
        Lp1 mil[4];
        ToneStack stack;
        float sagEnv;
        DcBlocker dc;
    };

    float outGain() const {
        const Voicing& v = kVoicings[channel_];
        return dbToGain(param(kMaster) + v.trimDb + v.trimPerGainDb * param(kGain));
    }

    void configure(int ch) {
        const Voicing& v = kVoicings[ch];
        for (auto& k : k_) {
            k = Kernel{};
            k.os.clear();
            k.inHp.c = rbj::highpass(sr_, v.inHp, 0.6);
            k.depth.c = rbj::peaking(sr_, 105.0, 1.0, v.depthDb);
            k.stack.setup(v.stack, sr_ * F_);
            k.dc.setup(6.f, (float)sr_);
        }
    }

    void updateTone(bool force) {
        if (force || bass_.moving() || mid_.moving() || treble_.moving() || bass_.next != stackL_ || mid_.next != stackM_ ||
            treble_.next != stackT_) {
            stackL_ = bass_.next;
            stackM_ = mid_.next;
            stackT_ = treble_.next;
            for (auto& k : k_) k.stack.update(stackT_, stackM_, stackL_);
        }
        const float pres = param(kPresence), bright = param(kBright), gain = param(kGain);
        if (force || pres != lastPresence_ || bright != lastBright_ || std::fabs(gain - lastGainForBright_) > 0.05f) {
            lastPresence_ = pres;
            lastBright_ = bright;
            lastGainForBright_ = gain;
            const BiquadCoefs pc = rbj::highShelf(sr_, 3500.0, (pres - 5.f) * 1.6f, 0.7);
            // Bright cap: the lower the gain, the stronger the effect
            const BiquadCoefs bc = rbj::highShelf(sr_, 2200.0, bright > 0.5f ? 7.0 * (1.0 - gain * 0.085) : 0.0, 0.6);
            for (auto& k : k_) {
                k.presence.c = pc;
                k.bright.c = bc;
            }
        }
    }

    double sr_ = 48000.0;
    float srHi_ = 192000.f;
    int F_ = 4, latency_ = 0;
    OversamplerDesign design_;
    std::vector<float> hi_, tmp_;
    Kernel k_[2];
    bool mono_ = true;
    int channel_ = 0;
    float fade_ = 1.f;
    bool fadingOut_ = false;
    BlockRamp gain_, bass_, mid_, treble_, out_;
    float stackL_ = -1.f, stackM_ = -1.f, stackT_ = -1.f;
    float lastPresence_ = -100.f, lastBright_ = -100.f, lastGainForBright_ = -100.f;
};

std::unique_ptr<Effect> makeAmp() { return std::make_unique<Amp>(); }

}  // namespace gs::fx
