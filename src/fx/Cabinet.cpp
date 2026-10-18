// Cabinet simulation: zero-latency direct-form FIR convolution. Five built-in synthetic IRs (resonance /
// presence peak / cone breakup / steep high-frequency roll-off + early reflections); WAV impulse responses can
// also be loaded (automatically resampled / normalized / truncated). IR changes crossfade at block boundaries
// and are thread-safe.
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <string>
#include <vector>

#include "common/I18n.h"
#include "dsp/Biquad.h"
#include "dsp/Wav.h"
#include "fx/FxCommon.h"

namespace gs::fx {

using namespace gs::dsp;

namespace {

struct Ir {
    std::vector<float> rev;  // time-reversed coefficients
    int len = 0;
};

struct Peak {
    double f, q, db;
};
struct Reflection {
    double ms, gain;
};
struct CabSpec {
    double hpHz, hpQ;
    std::vector<Peak> peaks;
    double lpHz;
    int lpOrder;  // 4 or 6
    std::vector<Reflection> refl;
};

const CabSpec& cabSpec(int model) {
    static const CabSpec specs[5] = {
        // 1x12 open back: less low end, inverted rear reflection
        {75, 0.7, {{110, 1.2, 2}, {400, 1.0, -2}, {1500, 1.5, 2}, {2800, 2.0, 4}, {4200, 3.0, -6}}, 5400, 4, {{1.1, -0.2}, {2.9, 0.08}}},
        // 2x12 Combo
        {70, 0.75, {{120, 1.3, 3}, {500, 1.0, -1.5}, {2200, 1.8, 4}, {3500, 2.5, 3}, {5000, 3.0, -5}}, 5800, 4, {{0.6, 0.12}, {1.7, -0.1}}},
        // 4x12 closed back: tight low-end resonance + mid scoop
        {65, 1.0, {{105, 1.6, 5}, {300, 1.0, 1}, {700, 1.0, -3}, {2500, 1.5, 5}, {3800, 4.0, -4}, {4600, 3.0, 2}}, 5000, 6, {{0.35, 0.15}, {2.3, 0.08}}},
        // Bright
        {80, 0.7, {{150, 1.0, 1}, {3000, 1.5, 5}, {6000, 2.0, 2}}, 7500, 4, {{0.8, 0.1}}},
        // Vintage (darker, pronounced mids)
        {90, 0.7, {{200, 1.0, 2}, {1200, 1.2, 3}, {2400, 1.5, 2}}, 3900, 6, {{1.4, -0.18}}},
    };
    return specs[clampv(model, 0, 4)];
}

// Normalize so the average power gain at log-spaced frequencies from 100 Hz to 5 kHz is 0 dB
void normalizeIr(std::vector<float>& h, double sr) {
    if (h.empty()) return;
    double sum = 0.0;
    const int nf = 64;
    for (int k = 0; k < nf; ++k) {
        const double f = 100.0 * std::pow(50.0, (double)k / (nf - 1));
        const double w = 2.0 * kPi * f / sr;
        std::complex<double> acc(0.0, 0.0);
        const std::complex<double> step = std::polar(1.0, -w);
        std::complex<double> ph(1.0, 0.0);
        for (float v : h) {
            acc += (double)v * ph;
            ph *= step;
        }
        sum += std::norm(acc);
    }
    const double mean = sum / nf;
    if (mean > 1e-20) {
        const float s = (float)(1.0 / std::sqrt(mean));
        for (float& v : h) v *= s;
    }
}

void fadeTail(std::vector<float>& h, double frac) {
    const int n = (int)h.size();
    const int f = std::max(1, (int)(n * frac));
    for (int i = 0; i < f; ++i) {
        const double t = (double)i / f;
        h[(size_t)(n - f + i)] *= (float)(0.5 + 0.5 * std::cos(kPi * t));
    }
}

std::vector<float> makeCabIr(int model, double sr, int len) {
    const CabSpec& s = cabSpec(model);
    std::vector<BiquadCoefs> chain;
    chain.push_back(rbj::highpass(sr, s.hpHz, s.hpQ));
    for (const Peak& p : s.peaks) chain.push_back(rbj::peaking(sr, p.f, p.q, p.db));
    if (s.lpOrder >= 6) {
        chain.push_back(rbj::lowpass(sr, s.lpHz, 0.5176));
        chain.push_back(rbj::lowpass(sr, s.lpHz, 0.7071));
        chain.push_back(rbj::lowpass(sr, s.lpHz, 1.9319));
    } else {
        chain.push_back(rbj::lowpass(sr, s.lpHz, 0.5412));
        chain.push_back(rbj::lowpass(sr, s.lpHz, 1.3066));
    }
    std::vector<float> h((size_t)len, 0.f);
    h[0] = 1.f;
    for (const BiquadCoefs& c : chain) {
        Biquad b;
        b.c = c;
        for (int i = 0; i < len; ++i) h[(size_t)i] = b.process(h[(size_t)i]);
    }
    // Early reflections (comb-filter ripples from mic / in-cabinet reflections make it sound less "synthetic")
    std::vector<float> out = h;
    for (const Reflection& r : s.refl) {
        const int d = (int)std::lround(r.ms * 0.001 * sr);
        for (int i = d; i < len; ++i) out[(size_t)i] += (float)r.gain * h[(size_t)(i - d)];
    }
    fadeTail(out, 0.25);
    normalizeIr(out, sr);
    return out;
}

Ir* makeIrObject(const std::vector<float>& h) {
    Ir* ir = new Ir;
    ir->len = (int)h.size();
    ir->rev.assign(h.rbegin(), h.rend());
    return ir;
}

// Turn a raw IR into coefficients for the current sample rate (not real-time)
std::vector<float> conditionIr(const std::vector<float>& raw, double rawRate, double sr, int maxTaps) {
    std::vector<float> h = resample(raw, rawRate, sr);
    if (h.empty()) return h;
    // Strip leading silence (lowers latency)
    float peak = 0.f;
    for (float v : h) peak = std::max(peak, std::fabs(v));
    if (peak <= 0.f) return {};
    size_t start = 0;
    while (start < h.size() && std::fabs(h[start]) < 0.01f * peak) ++start;
    start = start > 2 ? start - 2 : 0;
    h.erase(h.begin(), h.begin() + (long)start);
    if ((int)h.size() > maxTaps) {
        h.resize((size_t)maxTaps);
        fadeTail(h, 0.1);
    } else {
        fadeTail(h, 0.02);
    }
    normalizeIr(h, sr);
    return h;
}

}  // namespace

class Cabinet final : public Effect {
public:
    enum { kModel, kLowCut, kHighCut, kLevel };
    static constexpr int kCustom = 5;
    Cabinet()
        : Effect({pChoice("model", "Cabinet", {"1x12 Open Back", "2x12 Combo", "4x12 Closed", "Bright", "Vintage", "Custom IR"}, 1),
                  pLog("lowcut", "Low Cut", 20.f, 500.f, 40.f, "Hz"), pLog("highcut", "High Cut", 2000.f, 20000.f, 12000.f, "Hz"),
                  pLin("level", "Level", -24.f, 12.f, 0.f, "dB")}) {}
    ~Cabinet() override {
        delete pending_.exchange(nullptr);
        for (auto& g : grave_) delete g.exchange(nullptr);
        delete custom_;
        for (Ir* b : builtin_) delete b;
    }
    const char* typeId() const override { return "cabinet"; }
    const char* displayName() const override { return "Cabinet"; }
    FxCategory category() const override { return FxCategory::Cabinet; }

    void prepare(double sampleRate, int maxBlock) override {
        sr_ = sampleRate;
        maxBlock_ = maxBlock;
        maxTaps_ = std::max(256, (int)std::lround(2048.0 * sampleRate / 48000.0));
        for (Ir* b : builtin_) delete b;
        builtin_.clear();
        for (int m = 0; m < 5; ++m) builtin_.push_back(makeIrObject(makeCabIr(m, sampleRate, maxTaps_)));
        // Sample rate changed: re-condition the custom IR (the audio thread isn't running during prepare)
        delete pending_.exchange(nullptr);
        if (!customRaw_.empty()) {
            delete custom_;
            custom_ = makeIrObject(conditionIr(customRaw_, customRawRate_, sampleRate, maxTaps_));
        }
        const size_t histLen = (size_t)(maxTaps_ - 1 + maxBlock);
        for (auto& h : hist_) h.assign(histLen, 0.f);
        reset();
    }

    void reset() override {
        if (builtin_.empty()) return;
        for (auto& h : hist_) std::fill(h.begin(), h.end(), 0.f);
        cur_ = selectIr();
        prev_ = nullptr;
        level_.snap(dbToGain(param(kLevel)));
        lastLow_ = lastHigh_ = -1.f;
        for (auto& b : lowCut_) b.reset();
        for (auto& b : highCut_) b.reset();
        updateFilters();
    }

    void process(float* L, float* R, int n) override {
        if (builtin_.empty()) return;
        // Pick up a newly loaded custom IR (the old one goes to the UI thread to be freed after this block's crossfade)
        Ir* toBury = nullptr;
        if (Ir* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            toBury = custom_;
            custom_ = p;
        }
        const Ir* want = selectIr();
        if (want != cur_) {
            prev_ = cur_;
            cur_ = want;
        }
        updateFilters();
        level_.begin(dbToGain(param(kLevel)), n, (float)sr_);

        const bool mono = sameChannels(L, R, n);
        if (mono) {
            convolve(0, L, n);
            std::memcpy(R, L, sizeof(float) * (size_t)n);
            wasMono_ = true;
        } else {
            if (wasMono_) {
                hist_[1] = hist_[0];  // same size, no allocation
                lowCut_[1] = lowCut_[0];
                highCut_[1] = highCut_[0];
                wasMono_ = false;
            }
            convolve(0, L, n);
            convolve(1, R, n);
        }
        prev_ = nullptr;
        level_.end();
        if (toBury) bury(toBury);
    }

    bool acceptsFile() const override { return true; }
    bool loadFile(const std::string& path, std::string& err) override {
        WavData w;
        if (!readWav(path, w, err)) return false;
        if (w.mono.size() < 8) {
            err = tr("IR is too short");
            return false;
        }
        // Keep only the first second of raw data (anything longer is truncated)
        if (w.mono.size() > (size_t)w.sampleRate) w.mono.resize((size_t)w.sampleRate);
        customRaw_ = w.mono;
        customRawRate_ = w.sampleRate;
        customPath_ = path;
        const size_t slash = path.find_last_of('/');
        customLabel_ = slash == std::string::npos ? path : path.substr(slash + 1);
        if (sr_ > 0.0) {
            std::vector<float> h = conditionIr(customRaw_, customRawRate_, sr_, maxTaps_);
            if (h.empty()) {
                err = tr("IR is completely silent");
                return false;
            }
            // First free old IRs retired by the audio thread
            for (auto& g : grave_) delete g.exchange(nullptr, std::memory_order_acq_rel);
            delete pending_.exchange(makeIrObject(h), std::memory_order_acq_rel);
        }
        setParam(kModel, (float)kCustom);
        return true;
    }
    std::string fileLabel() const override { return customLabel_; }
    std::string filePath() const override { return customPath_; }

private:
    const Ir* selectIr() const {
        const int m = (int)param(kModel);
        if (m == kCustom) return custom_ ? custom_ : builtin_[1];
        return builtin_[(size_t)clampv(m, 0, 4)];
    }

    void bury(Ir* old) {
        for (auto& g : grave_) {
            Ir* expected = nullptr;
            if (g.compare_exchange_strong(expected, old, std::memory_order_acq_rel)) return;
        }
        // Graveyard full (very rare): better to leak than to free memory on the real-time thread
    }

    static float dot(const float* a, const float* b, int len) {
        float acc = 0.f;
#pragma omp simd reduction(+ : acc)
        for (int j = 0; j < len; ++j) acc += a[j] * b[j];
        return acc;
    }

    void convolve(int ch, float* x, int n) {
        std::vector<float>& hist = hist_[(size_t)ch];
        const int base = maxTaps_ - 1;
        std::memcpy(hist.data() + base, x, sizeof(float) * (size_t)n);
        const Ir* ir = cur_;
        const float* hb = hist.data() + (maxTaps_ - ir->len);
        if (prev_ && prev_ != cur_) {
            const Ir* pv = prev_;
            const float* pb = hist.data() + (maxTaps_ - pv->len);
            const float inc = 1.f / (float)n;
            for (int i = 0; i < n; ++i) {
                const float t = (float)i * inc;
                const float a = dot(ir->rev.data(), hb + i, ir->len);
                const float b = dot(pv->rev.data(), pb + i, pv->len);
                x[i] = a * t + b * (1.f - t);
            }
        } else {
            for (int i = 0; i < n; ++i) x[i] = dot(ir->rev.data(), hb + i, ir->len);
        }
        std::memmove(hist.data(), hist.data() + n, sizeof(float) * (size_t)base);
        lowCut_[(size_t)ch].processBlock(x, n);
        highCut_[(size_t)ch].processBlock(x, n);
        for (int i = 0; i < n; ++i) x[i] *= level_.at(i);
    }

    void updateFilters() {
        const float lo = param(kLowCut), hi = param(kHighCut);
        if (lo == lastLow_ && hi == lastHigh_) return;
        lastLow_ = lo;
        lastHigh_ = hi;
        const BiquadCoefs lc = rbj::highpass(sr_, lo, 0.707), hc = rbj::lowpass(sr_, std::min<double>(hi, 0.45 * sr_), 0.707);
        for (auto& b : lowCut_) b.c = lc;
        for (auto& b : highCut_) b.c = hc;
    }

    double sr_ = 0.0;
    int maxBlock_ = 0, maxTaps_ = 2048;
    std::vector<Ir*> builtin_;
    Ir* custom_ = nullptr;                // used by the audio thread (prepare also changes it, while audio is stopped)
    std::atomic<Ir*> pending_{nullptr};   // UI -> audio
    std::atomic<Ir*> grave_[8] = {};      // audio -> UI (to be freed)
    const Ir* cur_ = nullptr;
    const Ir* prev_ = nullptr;
    std::vector<float> hist_[2];
    Biquad lowCut_[2], highCut_[2];
    bool wasMono_ = true;
    BlockRamp level_;
    float lastLow_ = -1.f, lastHigh_ = -1.f;
    std::vector<float> customRaw_;
    int customRawRate_ = 48000;
    std::string customPath_, customLabel_;
};

std::unique_ptr<Effect> makeCabinet() { return std::make_unique<Cabinet>(); }

}  // namespace gs::fx
