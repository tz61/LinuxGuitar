// Offline effect tests: stability / boundedness / reset / decay to silence / CPU / oversampling aliasing /
// preset round trip / preset loudness.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dsp/DspUtil.h"
#include "dsp/Oversampler.h"
#include "dsp/ToneStack.h"
#include "dsp/Wav.h"
#include "fx/Effect.h"
#include "fx/Preset.h"

using namespace gs;

static int g_fail = 0;
#define CHECK(cond, ...)                 \
    do {                                 \
        if (!(cond)) {                   \
            std::printf("  FAIL: ");     \
            std::printf(__VA_ARGS__);    \
            std::printf("\n");           \
            ++g_fail;                    \
        }                                \
    } while (0)

static constexpr int kMaxBlock = 1024;

// ------------------------------------------------------------------ Test signals
static void addString(std::vector<float>& out, double sr, double start, double f, float amp, std::mt19937& rng) {
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    const int N = std::max(2, (int)std::lround(sr / f));
    std::vector<float> buf((size_t)N);
    float lp = 0.f;
    for (float& v : buf) {
        lp += 0.5f * (u(rng) - lp);  // a slightly soft pick
        v = lp * amp;
    }
    const size_t s0 = (size_t)(start * sr);
    const size_t len = (size_t)(2.5 * sr);
    for (size_t i = 0; i < len && s0 + i < out.size(); ++i) {
        const size_t idx = i % (size_t)N;
        const float y = buf[idx];
        out[s0 + i] += y;
        buf[idx] = 0.996f * 0.5f * (y + buf[(idx + 1) % (size_t)N]);
    }
}

static double rmsOf(const std::vector<float>& x) {
    double s = 0;
    for (float v : x) s += (double)v * v;
    return std::sqrt(s / std::max<size_t>(1, x.size()));
}

// Guitar-like signal: one strummed chord per second plus a single note on the half beat; normalized to -18 dBFS RMS
static std::vector<float> makeGuitar(double sr, double sec, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<float> out((size_t)(sr * sec), 0.f);
    static const int chords[4][6] = {{40, 47, 52, 56, 59, 64}, {45, 52, 57, 61, 64, 0}, {43, 47, 50, 55, 59, 67}, {50, 57, 62, 66, 0, 0}};
    static const int notes[8] = {64, 67, 69, 71, 74, 76, 79, 81};
    int c = 0, k = 0;
    for (double t = 0.0; t < sec - 0.3; t += 1.0, ++c) {
        for (int s = 0; s < 6; ++s) {
            const int m = chords[c % 4][s];
            if (m > 0) addString(out, sr, t + s * 0.012, 440.0 * std::pow(2.0, (m - 69) / 12.0), 0.5f, rng);
        }
        addString(out, sr, t + 0.5, 440.0 * std::pow(2.0, (notes[k++ % 8] - 69) / 12.0), 0.8f, rng);
        addString(out, sr, t + 0.75, 440.0 * std::pow(2.0, (notes[k++ % 8] - 69) / 12.0), 0.8f, rng);
    }
    const double r = rmsOf(out);
    const float g = (float)(std::pow(10.0, -18.0 / 20.0) / std::max(r, 1e-9));
    for (float& v : out) v *= g;
    return out;
}

static std::vector<float> makeSweep(double sr, double sec, float amp) {
    std::vector<float> out((size_t)(sr * sec));
    const double f0 = 20.0, f1 = std::min(20000.0, 0.45 * sr);
    const double k = std::log(f1 / f0) / sec;
    for (size_t i = 0; i < out.size(); ++i) {
        const double t = (double)i / sr;
        out[i] = amp * (float)std::sin(2 * dsp::kPi * f0 * (std::exp(k * t) - 1.0) / k);
    }
    return out;
}

static std::vector<float> makeNoise(double sr, double sec, float amp, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    std::vector<float> out((size_t)(sr * sec));
    for (float& v : out) v = amp * u(rng);
    return out;
}

// ------------------------------------------------------------------ Processing helpers
struct Stats {
    float peak = 0.f;
    bool finite = true;
    double sumSq = 0.0;
    size_t n = 0;
    double rms() const { return std::sqrt(sumSq / std::max<size_t>(1, n)); }
};

// Run a mono input through an effect (or chain) with identical left and right channels
template <class Proc>
static Stats runSignal(Proc&& proc, const std::vector<float>& in, std::mt19937& rng, int fixedBlock, std::vector<float>* outL = nullptr) {
    Stats st;
    std::vector<float> L(kMaxBlock), R(kMaxBlock);
    std::uniform_int_distribution<int> bs(16, kMaxBlock);
    size_t pos = 0;
    if (outL) outL->clear();
    while (pos < in.size()) {
        int n = fixedBlock > 0 ? fixedBlock : bs(rng);
        n = (int)std::min<size_t>((size_t)n, in.size() - pos);
        std::memcpy(L.data(), in.data() + pos, sizeof(float) * (size_t)n);
        std::memcpy(R.data(), in.data() + pos, sizeof(float) * (size_t)n);
        proc(L.data(), R.data(), n);
        for (int i = 0; i < n; ++i) {
            const float a = L[i], b = R[i];
            if (!std::isfinite(a) || !std::isfinite(b)) st.finite = false;
            st.peak = std::max(st.peak, std::max(std::fabs(a), std::fabs(b)));
            st.sumSq += 0.5 * ((double)a * a + (double)b * b);
            if (outL) outL->push_back(a);
        }
        st.n += (size_t)n;
        pos += (size_t)n;
    }
    return st;
}

static void setRandomParams(Effect& fx, std::mt19937& rng) {
    for (int i = 0; i < fx.numParams(); ++i) {
        const ParamInfo& p = fx.paramInfo(i);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        float t = u(rng);
        float v;
        if (p.scale == ParamScale::Log) v = p.minV * std::pow(p.maxV / p.minV, t);
        else v = p.minV + t * (p.maxV - p.minV);
        fx.setParam(i, v);
    }
}

// Simple effect chain (for testing presets)
struct Chain {
    std::vector<std::unique_ptr<Effect>> fx;
    bool build(const PresetData& p, double sr) {
        fx.clear();
        for (const FxSlotState& s : p.slots) {
            auto e = createEffect(s.typeId);
            if (!e) return false;
            e->prepare(sr, kMaxBlock);
            for (const auto& kv : s.params)
                if (!e->setParam(kv.first, kv.second)) return false;
            e->bypassed = s.bypass;
            e->reset();
            fx.push_back(std::move(e));
        }
        return true;
    }
    void process(float* L, float* R, int n) {
        for (auto& e : fx)
            if (!e->bypassed) e->process(L, R, n);
    }
};

// ------------------------------------------------------------------ FFT (aliasing test)
static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * dsp::kPi / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Returns alias power / fundamental power (dB). Bins within ±6 of a harmonic (k*f0 < sr/2) count as signal.
// Only bins below fMax are counted.
static double aliasRatioDb(const std::vector<float>& x, double sr, double f0, double fMax = 1e9) {
    const size_t N = 65536;
    std::vector<std::complex<double>> a(N);
    const size_t off = x.size() > N ? (x.size() - N) / 2 : 0;
    for (size_t i = 0; i < N; ++i) {
        const double t = (double)i / (N - 1);
        const double w = 0.35875 - 0.48829 * std::cos(2 * dsp::kPi * t) + 0.14128 * std::cos(4 * dsp::kPi * t) - 0.01168 * std::cos(6 * dsp::kPi * t);
        a[i] = (i + off < x.size() ? x[i + off] : 0.f) * w;
    }
    fft(a);
    double fund = 0, alias = 0;
    for (size_t k = 1; k < N / 2; ++k) {
        const double f = (double)k * sr / N;
        const double p = std::norm(a[k]);
        const double h = f / f0;
        const double nearest = std::round(h);
        const bool harmonic = nearest >= 1 && std::fabs(f - nearest * f0) <= 6.0 * sr / N;
        if (harmonic && nearest == 1) fund += p;
        else if (!harmonic && f > 30.0 && f < fMax) alias += p;
    }
    return 10 * std::log10(alias / std::max(fund, 1e-30) + 1e-30);
}

// ------------------------------------------------------------------ Tests
static void testOversampler() {
    std::printf("== Oversampling half-band filters\n");
    dsp::OversamplerDesign d;
    d.setup(3);
    for (int s = 0; s < 3; ++s) {
        const auto& h = d.stage(s);
        const double fp = 0.25 - h.transition / 2, fs = 0.25 + h.transition / 2;
        double ripple = 0, stop = -1000;
        for (int i = 0; i <= 2000; ++i) {
            const double f = fp * i / 2000.0;
            ripple = std::max(ripple, std::fabs(20 * std::log10(dsp::halfBandMag(h, f))));
            const double g = fs + (0.5 - fs) * i / 2000.0;
            stop = std::max(stop, 20 * std::log10(dsp::halfBandMag(h, g) + 1e-30));
        }
        std::printf("  stage %d: %2d coefs, transition %.3f, passband ripple %.5f dB, stopband %.1f dB, 1 kHz group delay %.2f samples (high rate)\n", s + 1, h.n,
                    h.transition, ripple, -stop, dsp::halfBandGroupDelay(h, 1000.0 / (48000.0 * (2 << s))));
        CHECK(-stop > 90.0, "stage %d: insufficient stopband attenuation (%.1f dB)", s + 1, -stop);
        CHECK(ripple < 0.01, "stage %d: passband ripple too large", s + 1);
    }
    for (int L = 1; L <= 3; ++L) {
        dsp::OversamplerDesign od;
        od.setup(L);
        std::printf("  %dx oversampling round-trip latency (1 kHz): %.2f samples @48k\n", 1 << L, od.latencyBase(1000.0, 48000.0));
    }

    // Linear round-trip gain + aliasing comparison
    const double sr = 48000.0;
    for (double f : {1000.0, 10000.0, 18000.0}) {
        dsp::OversamplerDesign od;
        od.setup(2);
        dsp::OversamplerState st;
        st.clear();
        std::vector<float> x((size_t)sr), hi(x.size() * 4), tmp(x.size() * 2), y(x.size());
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * (float)std::sin(2 * dsp::kPi * f * i / sr);
        dsp::osUp(od, st, x.data(), (int)x.size(), hi.data(), tmp.data());
        dsp::osDown(od, st, hi.data(), (int)x.size(), y.data());
        double a = 0, b = 0;
        for (size_t i = x.size() / 2; i < x.size(); ++i) {
            a += (double)x[i] * x[i];
            b += (double)y[i] * y[i];
        }
        const double gdb = 10 * std::log10(b / a);
        std::printf("  4x linear round-trip gain @ %5.0f Hz: %+.4f dB\n", f, gdb);
        CHECK(std::fabs(gdb) < 0.05, "4x round-trip gain error @ %.0f Hz", f);
    }
    {
        const double f0 = 5000.0;
        std::vector<float> x((size_t)(sr * 2));
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.9f * (float)std::sin(2 * dsp::kPi * f0 * i / sr);
        auto clip = [](float v) { return std::max(-1.f, std::min(1.f, v * 10.f)); };
        std::vector<float> y1(x.size());
        for (size_t i = 0; i < x.size(); ++i) y1[i] = clip(x[i]);
        std::vector<double> res, aud;
        for (int L = 1; L <= 3; ++L) {
            dsp::OversamplerDesign od;
            od.setup(L);
            dsp::OversamplerState st;
            st.clear();
            const int F = od.factor();
            std::vector<float> hi(x.size() * (size_t)F), tmp(x.size() * (size_t)F / 2 + 1), y(x.size());
            dsp::osUp(od, st, x.data(), (int)x.size(), hi.data(), tmp.data());
            for (float& v : hi) v = clip(v);
            dsp::osDown(od, st, hi.data(), (int)x.size(), y.data());
            res.push_back(aliasRatioDb(y, sr, f0));
            aud.push_back(aliasRatioDb(y, sr, f0, 20000.0));
        }
        const double a1 = aliasRatioDb(y1, sr, f0), a1a = aliasRatioDb(y1, sr, f0, 20000.0);
        std::printf("  5 kHz sine x10 hard-clipped, alias/fundamental (full band): 1x %.1f dB, 2x %.1f dB, 4x %.1f dB, 8x %.1f dB\n", a1, res[0], res[1], res[2]);
        std::printf("  5 kHz sine x10 hard-clipped, alias/fundamental (<20 kHz):   1x %.1f dB, 2x %.1f dB, 4x %.1f dB, 8x %.1f dB\n", a1a, aud[0], aud[1], aud[2]);
        CHECK(res[1] < a1 - 15.0, "4x oversampling should clearly reduce aliasing");
    }
}

// Harmonic distortion of each amp channel (220 Hz sine, -18 dBFS): report only, plus loose checks
static void testAmpCharacter() {
    std::printf("== Amp character (THD of a 220 Hz sine at -18 dBFS)\n");
    const double sr = 48000.0;
    const char* names[4] = {"Clean", "Crunch", "Lead", "Metal"};
    for (int ch = 0; ch < 4; ++ch) {
        std::printf("  %-8s", names[ch]);
        for (float g : {0.f, 2.5f, 5.f, 7.5f, 10.f}) {
            auto a = createEffect("amp");
            a->prepare(sr, kMaxBlock);
            a->setParam("channel", (float)ch);
            a->setParam("gain", g);
            a->reset();
            std::vector<float> x((size_t)(sr * 1.5)), y;
            for (size_t i = 0; i < x.size(); ++i) x[i] = 0.125f * (float)std::sin(2 * dsp::kPi * 220.0 * i / sr);
            std::mt19937 rng(1);
            auto proc = [&](float* L, float* R, int n) { a->process(L, R, n); };
            runSignal(proc, x, rng, 256, &y);
            // DFT for the fundamental and harmonics 2..10
            const size_t N = 48000, off = y.size() - N;
            double h[11] = {};
            for (int k = 1; k <= 10; ++k) {
                std::complex<double> acc = 0;
                for (size_t i = 0; i < N; ++i) acc += (double)y[off + i] * std::polar(1.0, -2 * dsp::kPi * 220.0 * k * i / sr);
                h[k] = std::abs(acc);
            }
            double harm = 0;
            for (int k = 2; k <= 10; ++k) harm += h[k] * h[k];
            const double thd = 100.0 * std::sqrt(harm) / std::max(h[1], 1e-12);
            std::printf("  g%-4.1f %6.1f%%", g, thd);
            if (ch == 0 && g == 0.f) CHECK(thd < 5.0, "clean channel distorts too much at low gain (%.1f%%)", thd);
            if (ch == 3 && g == 5.f) CHECK(thd > 20.0, "metal channel does not distort enough (%.1f%%)", thd);
        }
        std::printf("\n");
    }
}

static void testToneStack() {
    std::printf("== Tone stacks (all knobs at 5)\n");
    const char* names[3] = {"Fender", "Marshall", "Mesa"};
    for (int m = 0; m < 3; ++m) {
        dsp::ToneStack ts;
        ts.setup((dsp::ToneStack::Model)m, 192000.0);
        ts.update(0.5, 0.5, 0.5);
        std::printf("  %-8s", names[m]);
        for (double f : {60.0, 150.0, 400.0, 1000.0, 3000.0, 8000.0}) std::printf("  %5.0fHz %6.1f dB", f, 20 * std::log10(ts.magnitude(f, 192000.0)));
        std::printf("\n");
        const double lo = ts.magnitude(150, 192000), mid = ts.magnitude(600, 192000), hi = ts.magnitude(3000, 192000);
        CHECK(std::isfinite(lo) && std::isfinite(mid) && std::isfinite(hi), "%s: invalid tone stack response", names[m]);
        if (m != 2) CHECK(mid < lo && mid < hi, "%s: should have a mid scoop", names[m]);
    }
}

struct FxReport {
    std::string id;
    float peakDefault = 0.f;
    double cpuUsPerSec = 0.0;
    int latency = 0;
};

static void testEffect(const EffectTypeInfo& ti, FxReport& rep) {
    std::mt19937 rng(1234);
    const double sr = 48000.0;
    auto fx = createEffect(ti.typeId);
    CHECK(fx != nullptr, "createEffect(%s) failed", ti.typeId.c_str());
    if (!fx) return;
    CHECK(ti.typeId == fx->typeId(), "typeId mismatch: %s", ti.typeId.c_str());
    for (int i = 0; i < fx->numParams(); ++i) {
        const ParamInfo& p = fx->paramInfo(i);
        CHECK(p.defV >= p.minV && p.defV <= p.maxV, "%s.%s: default value out of range", ti.typeId.c_str(), p.id.c_str());
        CHECK(p.scale != ParamScale::Log || p.minV > 0, "%s.%s: log parameter minimum must be > 0", ti.typeId.c_str(), p.id.c_str());
        CHECK(p.scale != ParamScale::Choice || (int)p.choices.size() == (int)(p.maxV - p.minV) + 1, "%s.%s: choice count mismatch", ti.typeId.c_str(),
              p.id.c_str());
        CHECK(fx->paramIndex(p.id) == i, "%s.%s: duplicate parameter id", ti.typeId.c_str(), p.id.c_str());
    }
    fx->prepare(sr, kMaxBlock);
    rep.id = ti.typeId;
    rep.latency = fx->latencySamples();
    auto proc = [&](float* L, float* R, int n) { fx->process(L, R, n); };

    // 1) Default parameters, various signals
    std::vector<std::vector<float>> sigs;
    sigs.push_back(std::vector<float>((size_t)sr, 0.f));
    {
        std::vector<float> imp((size_t)sr, 0.f);
        imp[100] = 1.f;
        sigs.push_back(imp);
    }
    sigs.push_back(makeSweep(sr, 3.0, 0.5f));
    sigs.push_back(makeSweep(sr, 3.0, 0.125f));
    sigs.push_back(makeNoise(sr, 2.0, 0.25f, 7));
    sigs.push_back(makeGuitar(sr, 4.0, 3));
    for (const auto& s : sigs) {
        const Stats st = runSignal(proc, s, rng, 0);
        CHECK(st.finite, "%s: non-finite output with default parameters", ti.typeId.c_str());
        rep.peakDefault = std::max(rep.peakDefault, st.peak);
    }
    CHECK(rep.peakDefault < 8.f, "%s: peak too high with default parameters: %.2f", ti.typeId.c_str(), rep.peakDefault);

    // 2) Random parameters
    const auto guitar = makeGuitar(sr, 2.0, 11);
    const auto noise = makeNoise(sr, 1.0, 0.3f, 13);
    for (int round = 0; round < 8; ++round) {
        setRandomParams(*fx, rng);
        Stats a = runSignal(proc, guitar, rng, 0);
        Stats b = runSignal(proc, noise, rng, 0);
        CHECK(a.finite && b.finite, "%s: non-finite output with random parameters (round %d)", ti.typeId.c_str(), round);
        CHECK(a.peak < 100.f && b.peak < 100.f, "%s: random parameters, round %d: peak %.1f/%.1f (diverging?)", ti.typeId.c_str(), round, a.peak, b.peak);
    }

    // 3) Reset: after restoring defaults + reset(), silent input must give silent output immediately
    fx->resetParamsToDefault();
    fx->reset();
    {
        Stats st = runSignal(proc, std::vector<float>((size_t)(sr * 0.5), 0.f), rng, 0);
        CHECK(st.peak < 1e-6f, "%s: output on silent input after reset (peak %.2e)", ti.typeId.c_str(), st.peak);
    }

    // 4) Decay to silence: 30 s of silence after playing; the last second must be silent with no DC
    {
        runSignal(proc, makeGuitar(sr, 2.0, 5), rng, 0);
        std::vector<float> tail;
        runSignal(proc, std::vector<float>((size_t)(sr * 30), 0.f), rng, 0, &tail);
        std::vector<float> last(tail.end() - (long)sr, tail.end());
        double mean = 0;
        for (float v : last) mean += v;
        mean /= (double)last.size();
        const double r = rmsOf(last);
        CHECK(r < 1e-5, "%s: still outputs after 30 s of silence (RMS %.2e)", ti.typeId.c_str(), r);
        CHECK(std::fabs(mean) < 1e-6, "%s: DC offset after silence: %.2e", ti.typeId.c_str(), mean);
    }

    // 5) Other sample rates
    for (double sr2 : {44100.0, 96000.0}) {
        auto f2 = createEffect(ti.typeId);
        f2->prepare(sr2, kMaxBlock);
        auto p2 = [&](float* L, float* R, int n) { f2->process(L, R, n); };
        Stats a = runSignal(p2, makeGuitar(sr2, 2.0, 21), rng, 0);
        Stats b = runSignal(p2, makeSweep(sr2, 1.0, 0.5f), rng, 0);
        CHECK(a.finite && b.finite && a.peak < 8.f && b.peak < 8.f, "%s: bad output at %.0f Hz (peak %.2f/%.2f)", ti.typeId.c_str(), sr2, a.peak, b.peak);
    }

    // 6) CPU: 64-frame blocks, 10 seconds
    {
        fx->resetParamsToDefault();
        fx->reset();
        std::vector<float> in = makeGuitar(sr, 10.0, 17);
        const auto nz = makeNoise(sr, 10.0, 0.05f, 19);
        for (size_t i = 0; i < in.size(); ++i) in[i] += nz[i];
        std::vector<float> L(64), R(64);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t pos = 0; pos + 64 <= in.size(); pos += 64) {
            std::memcpy(L.data(), in.data() + pos, 64 * sizeof(float));
            std::memcpy(R.data(), in.data() + pos, 64 * sizeof(float));
            fx->process(L.data(), R.data(), 64);
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        rep.cpuUsPerSec = us / 10.0;
    }
}

static void testFeedbackBounded() {
    std::printf("== Extreme feedback stays bounded (40 s after a noise burst)\n");
    struct Case {
        const char* type;
        std::vector<std::pair<const char*, float>> p;
    };
    const std::vector<Case> cases = {
        {"delay", {{"feedback", 95}, {"tone", 16000}, {"time", 60}, {"mode", 0}}},
        {"delay", {{"feedback", 95}, {"tone", 16000}, {"time", 300}, {"mode", 1}, {"mod", 100}}},
        {"delay", {{"feedback", 95}, {"time", 25}, {"mode", 2}}},
        {"flanger", {{"feedback", 95}, {"depth", 100}, {"delay", 0.3f}}},
        {"flanger", {{"feedback", -95}, {"depth", 100}, {"delay", 10}}},
        {"phaser", {{"feedback", 90}, {"stages", 3}, {"depth", 100}}},
        {"reverb", {{"decay", 10}, {"mode", 1}, {"size", 150}, {"damping", 16000}, {"mix", 100}}},
        {"reverb", {{"decay", 10}, {"mode", 2}, {"size", 150}, {"damping", 16000}, {"mix", 100}}},
        {"reverb", {{"decay", 10}, {"mode", 0}, {"size", 30}, {"damping", 16000}, {"mix", 100}}},
    };
    std::mt19937 rng(99);
    for (const Case& c : cases) {
        auto fx = createEffect(c.type);
        fx->prepare(48000.0, kMaxBlock);
        for (const auto& kv : c.p) fx->setParam(kv.first, kv.second);
        fx->reset();
        auto proc = [&](float* L, float* R, int n) { fx->process(L, R, n); };
        Stats a = runSignal(proc, makeNoise(48000.0, 1.0, 0.5f, 3), rng, 0);
        std::vector<float> tail;
        Stats b = runSignal(proc, std::vector<float>(48000 * 40, 0.f), rng, 0, &tail);
        float lastPeak = 0.f;
        for (size_t i = tail.size() - 48000 * 5; i < tail.size(); ++i) lastPeak = std::max(lastPeak, std::fabs(tail[i]));
        std::printf("  %-8s burst peak %.2f, silence peak %.2f, last 5 s peak %.2e\n", c.type, a.peak, b.peak, lastPeak);
        CHECK(a.finite && b.finite, "%s: non-finite output with extreme feedback", c.type);
        CHECK(b.peak < 8.f, "%s: peak too high with extreme feedback", c.type);
        CHECK(lastPeak <= std::max(a.peak, b.peak), "%s: extreme feedback keeps growing", c.type);
    }
}

static void testCabinetFile() {
    std::printf("== Cabinet WAV IR loading\n");
    // Write a 44.1 kHz / 24-bit / stereo test IR
    const char* path = "gs_test_ir_tmp.wav";
    {
        const int sr = 44100, frames = 3000;
        std::vector<unsigned char> d;
        auto w32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back((unsigned char)(v >> (8 * i))); };
        auto w16 = [&](uint16_t v) { d.push_back((unsigned char)v); d.push_back((unsigned char)(v >> 8)); };
        d.insert(d.end(), {'R', 'I', 'F', 'F'});
        w32(36 + frames * 6);
        d.insert(d.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        w32(16);
        w16(1);
        w16(2);
        w32(sr);
        w32(sr * 6);
        w16(6);
        w16(24);
        d.insert(d.end(), {'d', 'a', 't', 'a'});
        w32(frames * 6);
        for (int i = 0; i < frames; ++i) {
            const double v = (i < 40 ? 0.0 : std::exp(-(i - 40) / 300.0) * std::cos(i * 0.3)) * 0.8;
            const int32_t s = (int32_t)(v * 8388607.0);
            for (int c = 0; c < 2; ++c) {
                d.push_back((unsigned char)s);
                d.push_back((unsigned char)(s >> 8));
                d.push_back((unsigned char)(s >> 16));
            }
        }
        FILE* f = std::fopen(path, "wb");
        std::fwrite(d.data(), 1, d.size(), f);
        std::fclose(f);
    }
    auto cab = createEffect("cabinet");
    cab->prepare(48000.0, kMaxBlock);
    std::string err;
    const bool ok = cab->loadFile(path, err);
    CHECK(ok, "IR load failed: %s", err.c_str());
    CHECK(cab->param(cab->paramIndex("model")) == 5.f, "loading an IR should switch to Custom IR");
    CHECK(cab->fileLabel() == path, "wrong fileLabel: %s", cab->fileLabel().c_str());
    std::mt19937 rng(5);
    auto proc = [&](float* L, float* R, int n) { cab->process(L, R, n); };
    Stats st = runSignal(proc, makeGuitar(48000.0, 2.0, 9), rng, 0);
    CHECK(st.finite && st.peak > 1e-3f && st.peak < 8.f, "bad custom IR output (peak %.3f)", st.peak);
    std::printf("  44.1k/24-bit/stereo IR -> 48k: output RMS %.1f dBFS, peak %.2f\n", 20 * std::log10(st.rms()), st.peak);
    // Load again while running (thread-safe path) + switch to a built-in model
    CHECK(cab->loadFile(path, err), "second load failed");
    cab->setParam("model", 2);
    st = runSignal(proc, makeGuitar(48000.0, 1.0, 10), rng, 0);
    CHECK(st.finite, "non-finite output after switching IR");
    // Re-preparing (sample rate change) keeps the custom IR
    cab->setParam("model", 5);
    cab->prepare(96000.0, kMaxBlock);
    st = runSignal(proc, makeGuitar(96000.0, 1.0, 12), rng, 0);
    CHECK(st.finite && st.peak > 1e-3f, "bad custom IR output at 96k");
    std::remove(path);
    // Invalid file
    {
        FILE* f = std::fopen(path, "wb");
        std::fputs("not a wav file at all", f);
        std::fclose(f);
    }
    CHECK(!cab->loadFile(path, err) && !err.empty(), "loading a non-WAV file should fail");
    std::printf("  non-WAV file: \"%s\"\n", err.c_str());
    std::remove(path);
    CHECK(!cab->loadFile("/nonexistent/x.wav", err), "loading a missing file should fail");
}

static void testPresets() {
    std::printf("== Presets\n");
    const auto& ps = builtinPresets();
    CHECK(!ps.empty(), "no built-in presets");
    for (const PresetData& p : ps) {
        // Round trip
        const std::string text = presetToText(p);
        PresetData q;
        std::string err;
        CHECK(presetFromText(text, q, err), "preset %s: failed to parse", p.name.c_str());
        CHECK(err.empty(), "preset %s: parse warnings: %s", p.name.c_str(), err.c_str());
        bool same = q.name == p.name && q.description == p.description && q.slots.size() == p.slots.size();
        for (size_t i = 0; same && i < p.slots.size(); ++i) {
            same = q.slots[i].typeId == p.slots[i].typeId && q.slots[i].bypass == p.slots[i].bypass && q.slots[i].params == p.slots[i].params &&
                   q.slots[i].file == p.slots[i].file;
        }
        CHECK(same, "preset %s: round trip mismatch", p.name.c_str());
        Chain c;
        CHECK(c.build(p, 48000.0), "preset %s: unknown types or parameters", p.name.c_str());
    }
    // Tolerant parsing
    {
        PresetData q;
        std::string err;
        const bool ok = presetFromText("name=Test\nfx=amp gain=5 foo=1 bypass=1\nfx=nosuch x=1\nfile=/a b/c.wav\nfx=cabinet\nfile=/ir dir/x.wav\njunk\n", q, err);
        CHECK(ok && q.slots.size() == 2 && q.slots[0].bypass && q.slots[0].params.size() == 1 && q.slots[1].file == "/ir dir/x.wav",
              "wrong tolerant-parse result");
        CHECK(!err.empty(), "tolerant parsing should produce warnings");
        CHECK(!presetFromText("", q, err), "empty text should fail");
    }

    // Loudness: guitar-like input at -18 dBFS RMS
    std::printf("  %-26s %11s %11s %8s   %11s\n", "Preset", "Out RMS", "Rel RMS", "Out peak", "Rel peak");
    const double sr = 48000.0;
    const auto in = makeGuitar(sr, 6.0, 31);
    float inPeak = 0.f;
    for (float v : in) inPeak = std::max(inPeak, std::fabs(v));
    const double inRms = rmsOf(in);
    std::mt19937 rng(77);
    for (const PresetData& p : ps) {
        Chain c;
        c.build(p, sr);
        auto proc = [&](float* L, float* R, int n) { c.process(L, R, n); };
        const Stats st = runSignal(proc, in, rng, 128);
        const double dr = 20 * std::log10(st.rms() / inRms), dp = 20 * std::log10(st.peak / inPeak);
        std::printf("  %-26s %8.1f dB %+8.1f dB %8.2f   %+8.1f dB\n", p.name.c_str(), 20 * std::log10(st.rms()), dr, st.peak, dp);
        CHECK(st.finite, "preset %s: non-finite output", p.name.c_str());
        CHECK(dp < 6.5, "preset %s: peak %.1f dB above dry (more than 6 dB)", p.name.c_str(), dp);
        CHECK(dr > -12.0, "preset %s: too quiet (%.1f dB)", p.name.c_str(), dr);
    }
    // CPU: Heavy Metal, Ambient, Practice Clean
    for (const char* name : {"Heavy Metal", "Ambient", "Practice Clean"}) {
        for (const PresetData& p : ps) {
            if (p.name != name) continue;
            Chain c;
            c.build(p, sr);
            std::vector<float> in10 = makeGuitar(sr, 10.0, 41);
            std::vector<float> L(64), R(64);
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t pos = 0; pos + 64 <= in10.size(); pos += 64) {
                std::memcpy(L.data(), in10.data() + pos, 64 * sizeof(float));
                std::memcpy(R.data(), in10.data() + pos, 64 * sizeof(float));
                c.process(L.data(), R.data(), 64);
            }
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 10.0;
            int lat = 0;
            for (auto& e : c.fx) lat += e->latencySamples();
            std::printf("  CPU %-24s %8.0f us per second of audio (%.2f%% of one core), %.2f us per 64-frame block, chain latency %d samples\n", name, us, us / 1e4,
                        us / (48000.0 / 64.0), lat);
        }
    }
}

int main() {
    std::printf("Guitar Studio effect tests\n");
    testOversampler();
    testToneStack();
    testAmpCharacter();
    std::vector<FxReport> reps;
    std::printf("== Effects (defaults / random parameters / reset / decay to silence / sample rates / CPU)\n");
    for (const EffectTypeInfo& ti : effectTypes()) {
        FxReport r;
        const int before = g_fail;
        testEffect(ti, r);
        reps.push_back(r);
        std::printf("  %-11s %-12s default peak %6.3f  CPU %7.0f us/s  latency %d  %s\n", ti.typeId.c_str(), ti.displayName.c_str(), r.peakDefault,
                    r.cpuUsPerSec, r.latency, g_fail == before ? "OK" : "FAILED");
    }
    testFeedbackBounded();
    testCabinetFile();
    testPresets();
    std::printf("\n%s (%d failed)\n", g_fail == 0 ? "All passed" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
