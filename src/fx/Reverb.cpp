// Reverb: room / hall = 8-line feedback delay network (Hadamard matrix, modulated delays, per-line HF damping);
// plate = Dattorro plate reverb.
#include <cmath>

#include "dsp/Biquad.h"
#include "dsp/DelayLine.h"
#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

namespace {

// 8-point fast Walsh-Hadamard transform (normalized)
inline void hadamard8(float* v) {
    for (int len = 1; len < 8; len <<= 1)
        for (int i = 0; i < 8; i += len << 1)
            for (int j = i; j < i + len; ++j) {
                const float a = v[j], b = v[j + len];
                v[j] = a + b;
                v[j + len] = a - b;
            }
    const float s = 0.35355339f;  // 1/sqrt(8)
    for (int i = 0; i < 8; ++i) v[i] *= s;
}

}  // namespace

class Reverb final : public Effect {
public:
    enum { kMode, kDecay, kSize, kPredelay, kDamping, kLowCut, kMix, kWidth, kMod };
    enum { Room = 0, Hall = 1, Plate = 2 };
    Reverb()
        : Effect({pChoice("mode", "Type", {"Room", "Hall", "Plate"}, 0), pLog("decay", "Decay", 0.2f, 10.f, 1.5f, "s"),
                  pLin("size", "Room Size", 30.f, 150.f, 100.f, "%"), pLin("predelay", "Pre-delay", 0.f, 200.f, 15.f, "ms"),
                  pLog("damping", "Damping", 1000.f, 16000.f, 6000.f, "Hz"), pLog("lowcut", "Low Cut", 20.f, 500.f, 120.f, "Hz"),
                  pLin("mix", "Amount", 0.f, 100.f, 25.f, "%"), pLin("width", "Width", 0.f, 150.f, 100.f, "%"),
                  pLin("mod", "Mod", 0.f, 100.f, 30.f, "%")}) {}
    const char* typeId() const override { return "reverb"; }
    const char* displayName() const override { return "Reverb"; }
    FxCategory category() const override { return FxCategory::Reverb; }

    void prepare(double sampleRate, int) override {
        sr_ = (float)sampleRate;
        pre_.allocate((int)(0.21f * sr_) + 8);
        // FDN: longest line (hall) 68.3 ms * 1.5 + modulation headroom
        for (auto& l : fdnLine_) l.allocate((int)(0.115f * sr_) + 16);
        for (auto& a : diff_) a.allocate((int)(0.0135f * sr_ * 1.5f) + 8);
        er_.allocate((int)(0.04f * sr_) + 8);
        // Dattorro: sample-rate ratio * maximum size 1.25
        const float ps = sr_ / 29761.f * 1.25f + 0.01f;
        static const int inDiff[4] = {142, 107, 379, 277};
        for (int i = 0; i < 4; ++i) pIn_[i].allocate((int)(inDiff[i] * ps) + 4);
        pAp1_[0].allocate((int)((672 + 32) * ps) + 4);
        pAp1_[1].allocate((int)((908 + 32) * ps) + 4);
        pDel1_[0].allocate((int)(4453 * ps) + 4);
        pDel1_[1].allocate((int)(4217 * ps) + 4);
        pAp2_[0].allocate((int)(1800 * ps) + 4);
        pAp2_[1].allocate((int)(2656 * ps) + 4);
        pDel2_[0].allocate((int)(3720 * ps) + 4);
        pDel2_[1].allocate((int)(3163 * ps) + 4);
        sizeSm_.setTime(300.f, sr_);
        mix_.tau = 0.03f;
        reset();
    }

    void reset() override {
        pre_.reset();
        for (auto& l : fdnLine_) l.reset();
        for (auto& a : diff_) a.reset();
        er_.reset();
        for (auto& a : pIn_) a.reset();
        for (int c = 0; c < 2; ++c) {
            pAp1_[c].reset();
            pDel1_[c].reset();
            pAp2_[c].reset();
            pDel2_[c].reset();
            pDamp_[c] = 0.f;
        }
        pBw_ = 0.f;
        pFeed_[0] = pFeed_[1] = 0.f;
        for (float& s : fdnLp_) s = 0.f;
        for (int i = 0; i < 8; ++i) fdnLfo_[i].reset(i * 0.137);
        mode_ = (int)param(kMode);
        sizeSm_.snap(param(kSize) * 0.01f);
        mix_.snap(param(kMix) * 0.01f);
        wetFade_ = 1.f;
        lowCut_.reset();
        lastLowCut_ = -1.f;
        updateLowCut();
    }

    void process(float* L, float* R, int n) override {
        const int mode = (int)param(kMode);
        if (mode != mode_) {  // type changed: clear the tail and fade the wet signal in
            mode_ = mode;
            for (auto& l : fdnLine_) l.reset();
            for (auto& a : diff_) a.reset();
            for (auto& a : pIn_) a.reset();
            for (int c = 0; c < 2; ++c) {
                pAp1_[c].reset();
                pDel1_[c].reset();
                pAp2_[c].reset();
                pDel2_[c].reset();
            }
            wetFade_ = 0.f;
        }
        updateLowCut();
        mix_.begin(param(kMix) * 0.01f, n, sr_);
        const float decay = param(kDecay);
        const float preD = std::max(1.f, param(kPredelay) * 0.001f * sr_);
        const float dampC = onePoleCoef(std::min(param(kDamping), 0.45f * sr_), sr_);
        const float width = param(kWidth) * 0.01f;
        const float mod = param(kMod) * 0.01f;
        const float sizeT = param(kSize) * 0.01f;
        const float fadeInc = 1.f / (0.03f * sr_);
        if (mode_ == Plate) processPlate(L, R, n, decay, preD, dampC, width, mod, sizeT, fadeInc);
        else processFdn(L, R, n, decay, preD, dampC, width, mod, sizeT, fadeInc);
        mix_.end();
    }

private:
    void updateLowCut() {
        const float lc = param(kLowCut);
        if (lc == lastLowCut_) return;
        lastLowCut_ = lc;
        lowCut_.c = rbj::highpass(sr_, lc, 0.707);
    }

    inline void output(float* L, float* R, int i, float wl, float wr, float width, float fadeInc) {
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
        const float g = mix_.at(i) * wetFade_;
        wetFade_ = std::min(1.f, wetFade_ + fadeInc);
        L[i] += g * (mid + side);
        R[i] += g * (mid - side);
    }

    void processFdn(float* L, float* R, int n, float decay, float preD, float dampC, float width, float mod, float sizeT,
                    float fadeInc) {
        static const float roomMs[8] = {7.3f, 9.1f, 11.7f, 13.9f, 16.1f, 18.7f, 21.3f, 23.9f};
        static const float hallMs[8] = {29.3f, 33.1f, 38.9f, 43.7f, 49.1f, 55.3f, 61.9f, 68.3f};
        static const float roomDiff[4] = {1.9f, 3.1f, 4.7f, 6.1f};
        static const float hallDiff[4] = {3.7f, 5.3f, 8.9f, 12.7f};
        static const float lfoHz[8] = {0.31f, 0.43f, 0.57f, 0.71f, 0.37f, 0.53f, 0.67f, 0.83f};
        static const float inSign[8] = {1, -1, 1, -1, -1, 1, -1, 1};
        static const float outL[8] = {1, -1, 1, -1, 1, -1, 1, -1};
        static const float outR[8] = {1, 1, -1, -1, 1, 1, -1, -1};
        // Early reflections (room only): ms, gain (alternating left/right)
        static const float erMs[6] = {4.3f, 7.9f, 11.2f, 15.7f, 19.3f, 24.1f};
        static const float erG[6] = {0.6f, -0.45f, 0.38f, -0.3f, 0.25f, -0.2f};
        const bool hall = (mode_ == Hall);
        const float* lineMs = hall ? hallMs : roomMs;
        const float* diffMs = hall ? hallDiff : roomDiff;
        const float diffG = hall ? 0.65f : 0.6f;
        const float msS = 0.001f * sr_;
        const float modAmp = mod * 0.0004f * sr_;  // at most ±0.4 ms
        // Per-line decay coefficients (RT60 = decay seconds); size smoothing is slow, so once per block is enough
        float gk[8];
        for (int k = 0; k < 8; ++k) {
            fdnLfo_[k].setRate(lfoHz[k], sr_);
            gk[k] = std::pow(10.f, -3.f * lineMs[k] * sizeSm_.y * msS / (decay * sr_));
        }
        for (int i = 0; i < n; ++i) {
            const float size = sizeSm_.next(sizeT);
            const float x = 0.5f * (L[i] + R[i]);
            pre_.push(x);
            float in = lowCut_.process(pre_.readLinear(preD - 1.f));
            for (int k = 0; k < 4; ++k) in = diff_[k].process(in, std::max(1.f, diffMs[k] * size * msS), diffG);
            // Read each line (with modulation)
            float y[8], v[8];
            for (int k = 0; k < 8; ++k) {
                const float d = lineMs[k] * size * msS;
                const float dm = d + modAmp * Lfo::sine(fdnLfo_[k].phase);
                fdnLfo_[k].advance();
                y[k] = fdnLine_[k].readLinear(std::max(dm, 2.f) - 1.f);
                fdnLp_[k] += dampC * (y[k] - fdnLp_[k]);
                fdnLp_[k] = undenormal(fdnLp_[k]);
                v[k] = gk[k] * fdnLp_[k];
            }
            hadamard8(v);
            for (int k = 0; k < 8; ++k) fdnLine_[k].push(v[k] + in * inSign[k] * 0.35f);
            float wl = 0.f, wr = 0.f;
            for (int k = 0; k < 8; ++k) {
                wl += outL[k] * y[k];
                wr += outR[k] * y[k];
            }
            wl *= 0.3f;
            wr *= 0.3f;
            if (!hall) {
                er_.push(in);
                float el = 0.f, ernR = 0.f;
                for (int k = 0; k < 6; ++k) {
                    const float t = er_.readLinear(std::max(1.f, erMs[k] * size * msS));
                    if (k & 1) ernR += erG[k] * t;
                    else el += erG[k] * t;
                }
                wl += 0.5f * el;
                wr += 0.5f * ernR;
            }
            output(L, R, i, wl, wr, width, fadeInc);
        }
    }

    void processPlate(float* L, float* R, int n, float decay, float preD, float dampC, float width, float mod, float sizeT,
                      float fadeInc) {
        lfoP_.setRate(1.0, sr_);
        // RT60 -> decay coefficient (one full loop applies the decay 4 times), computed once per block
        const float loop = 21589.f * (0.5f + 0.5f * sizeSm_.y) / 29761.f;
        const float dec = clampv(std::pow(10.f, -3.f * loop / (4.f * decay)), 0.f, 0.97f);
        for (int i = 0; i < n; ++i) {
            const float size = sizeSm_.next(sizeT);
            const float s = sr_ / 29761.f * (0.5f + 0.5f * size);
            const float x = 0.5f * (L[i] + R[i]);
            pre_.push(x);
            float in = lowCut_.process(pre_.readLinear(preD - 1.f));
            pBw_ += 0.9995f * (in - pBw_);  // input bandwidth
            in = pBw_;
            in = pIn_[0].process(in, 142.f * s, 0.75f);
            in = pIn_[1].process(in, 107.f * s, 0.75f);
            in = pIn_[2].process(in, 379.f * s, 0.625f);
            in = pIn_[3].process(in, 277.f * s, 0.625f);
            const float exc = 16.f * s * mod;
            const float m0 = Lfo::sine(lfoP_.phase), m1 = Lfo::sine(lfoP_.phase + 0.25);
            lfoP_.advance();
            static const float ap1Len[2] = {672.f, 908.f}, del1Len[2] = {4453.f, 4217.f};
            static const float ap2Len[2] = {1800.f, 2656.f}, del2Len[2] = {3720.f, 3163.f};
            float newFeed[2];
            for (int c = 0; c < 2; ++c) {
                float t = in + pFeed_[1 - c];
                t = pAp1_[c].process(t, (ap1Len[c] + 16.f) * s + exc * (c ? m1 : m0), -0.7f);
                pDel1_[c].push(t);
                t = pDel1_[c].readLinear(del1Len[c] * s - 1.f);
                pDamp_[c] += dampC * (t - pDamp_[c]);
                pDamp_[c] = undenormal(pDamp_[c]);
                t = pAp2_[c].process(pDamp_[c] * dec, ap2Len[c] * s, 0.5f);
                pDel2_[c].push(t);
                newFeed[c] = pDel2_[c].readLinear(del2Len[c] * s - 1.f) * dec;
            }
            pFeed_[0] = newFeed[0];
            pFeed_[1] = newFeed[1];
            auto T = [&](const DelayLine& d, float pos) { return d.tap((int)(pos * s)); };
            auto A = [&](const AllpassDelay& a, float pos) { return a.tap((int)(pos * s)); };
            const float wl = T(pDel1_[1], 266) + T(pDel1_[1], 2974) - A(pAp2_[1], 1913) + T(pDel2_[1], 1996) - T(pDel1_[0], 1990) -
                             A(pAp2_[0], 187) - T(pDel2_[0], 1066);
            const float wr = T(pDel1_[0], 353) + T(pDel1_[0], 3627) - A(pAp2_[0], 1228) + T(pDel2_[0], 2673) - T(pDel1_[1], 2111) -
                             A(pAp2_[1], 335) - T(pDel2_[1], 121);
            output(L, R, i, 0.6f * wl, 0.6f * wr, width, fadeInc);
        }
    }

    float sr_ = 48000.f;
    int mode_ = 0;
    DelayLine pre_;
    Biquad lowCut_;
    float lastLowCut_ = -1.f;
    // FDN
    DelayLine fdnLine_[8];
    float fdnLp_[8] = {};
    Lfo fdnLfo_[8];
    AllpassDelay diff_[4];
    DelayLine er_;
    // Dattorro
    AllpassDelay pIn_[4], pAp1_[2], pAp2_[2];
    DelayLine pDel1_[2], pDel2_[2];
    float pDamp_[2] = {0.f, 0.f}, pFeed_[2] = {0.f, 0.f}, pBw_ = 0.f;
    Lfo lfoP_;
    Smoother sizeSm_;
    BlockRamp mix_;
    float wetFade_ = 1.f;
};

std::unique_ptr<Effect> makeReverb() { return std::make_unique<Reverb>(); }

}  // namespace gs::fx
