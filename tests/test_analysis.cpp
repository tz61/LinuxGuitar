// Offline analysis tests: synthetic plucks (Karplus-Strong) check pitch accuracy, onset detection and chord chroma.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "analysis/Analyzer.h"

using namespace gs;

static const double FS = 48000;
static int g_fail = 0;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            std::printf("  [FAIL] " __VA_ARGS__); \
            std::printf("\n");                \
            ++g_fail;                         \
        }                                     \
    } while (0)

// Exactly tuned Karplus-Strong (integer delay + first-order allpass fractional delay)
static void addPluck(std::vector<float>& out, size_t start, double freq, float amp, double dur, std::mt19937& rng,
                     float bright = 0.5f) {
    // Loop: the average (y[n-N] + y[n-N+1])/2 delays by N-0.5; plus the allpass frac => total N+frac-0.5 = FS/f
    double D = FS / freq + 0.5;
    int N = (int)std::floor(D);
    double frac = D - N;
    if (frac < 0.1) {
        N -= 1;
        frac += 1.0;
    }
    float c = (float)((1 - frac) / (1 + frac));
    std::vector<float> line(N + 2, 0.f);
    std::uniform_real_distribution<float> U(-1.f, 1.f);
    float lp = 0;
    for (int i = 0; i < N; ++i) {
        lp += bright * (U(rng) - lp);
        line[i] = lp;
    }
    double m = 0;
    for (int i = 0; i < N; ++i) m += line[i];
    m /= N;
    float pk = 1e-6f;
    for (int i = 0; i < N; ++i) {
        line[i] -= (float)m;
        pk = std::max(pk, std::fabs(line[i]));
    }
    for (int i = 0; i < N; ++i) line[i] *= amp / pk;
    int pos = 0;
    float prev = 0, apX1 = 0, apY1 = 0;
    // Decay: T60 ~4 s for low notes, ~1.5 s for high notes (converted per loop period)
    const double t60 = 4.0 - 2.5 * std::min(1.0, std::max(0.0, (freq - 82.0) / 1200.0));
    const float loss = (float)std::pow(0.001, 1.0 / (t60 * freq));
    size_t len = (size_t)(dur * FS);
    for (size_t n = 0; n < len && start + n < out.size(); ++n) {
        float x = line[pos];
        int nxt = (pos + 1) % N;
        float avg = 0.5f * (x + line[nxt]) * loss;
        (void)prev;
        float ap = c * avg + apX1 - c * apY1;  // allpass
        apX1 = avg;
        apY1 = ap;
        line[pos] = ap;
        pos = nxt;
        // Envelope (avoids a click at the end)
        float env = 1.f;
        if (n + 480 > len) env = (float)(len - n) / 480.f;
        out[start + n] += x * env;
    }
}

static double midiHz(double m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

struct Collector {
    std::vector<AnalysisFrame> frames;
};

static void testPitch() {
    std::printf("== Pitch accuracy (MPM) ==\n");
    std::mt19937 rng(1);
    const int midis[] = {40, 45, 50, 55, 59, 64, 52, 57, 62, 67, 69, 72, 76, 81, 84, 88};
    double worst = 0;
    for (int m : midis) {
        Analyzer an;
        an.configure(FS);
        Collector col;
        an.setFrameCallback([&](const AnalysisFrame& f) { col.frames.push_back(f); });
        std::vector<float> sig((size_t)(FS * 1.2), 0.f);
        addPluck(sig, 2400, midiHz(m), 0.3f, 1.1, rng);
        an.processOffline(sig.data(), sig.size());
        // Frames 60..700 ms after the onset
        std::vector<float> errs;
        int voiced = 0, total = 0;
        for (auto& f : col.frames) {
            if (f.frame < 2400 + FS * 0.06 || f.frame > 2400 + FS * 0.7) continue;
            ++total;
            if (f.midi < 0) continue;
            ++voiced;
            errs.push_back((f.midi - m) * 100.f);
        }
        std::sort(errs.begin(), errs.end());
        float med = errs.empty() ? 9999 : errs[errs.size() / 2];
        int octaveErr = 0;
        for (float e : errs)
            if (std::fabs(e) > 50) ++octaveErr;
        worst = std::max(worst, (double)std::fabs(med));
        std::printf("  MIDI %2d (%7.2f Hz): voiced %3d/%3d  median error %+6.2f cents  large-error frames %d\n", m, midiHz(m), voiced, total,
                    med, octaveErr);
        CHECK(std::fabs(med) < 5, "MIDI %d median error %.1f cents", m, med);
        CHECK(voiced > total * 0.9, "MIDI %d too few voiced frames %d/%d", m, voiced, total);
        CHECK(octaveErr < total * 0.05 + 1, "MIDI %d octave errors %d", m, octaveErr);
    }
    std::printf("  worst median error %.2f cents\n", worst);
}

static void testOnsets(float ampDb, float noiseDb, double ioi, bool sameNote, const char* name) {
    std::mt19937 rng(7);
    Analyzer an;
    an.configure(FS);
    Collector col;
    an.setFrameCallback([&](const AnalysisFrame& f) { col.frames.push_back(f); });
    const int count = 24;
    std::vector<float> sig((size_t)(FS * (1.0 + count * ioi + 1.0)), 0.f);
    std::normal_distribution<float> N(0.f, 1.f);
    float na = std::pow(10.f, noiseDb / 20.f);
    for (auto& s : sig) s = na * N(rng);
    float amp = std::pow(10.f, ampDb / 20.f);
    std::vector<size_t> truth;
    const int seq[] = {52, 55, 57, 59, 60, 62, 64, 67, 45, 50, 64, 59};
    std::uniform_real_distribution<float> A(0.6f, 1.0f);
    for (int i = 0; i < count; ++i) {
        size_t t = (size_t)(FS * (0.5 + i * ioi));
        int m = sameNote ? 57 : seq[i % 12];
        addPluck(sig, t, midiHz(m), amp * A(rng), ioi * 0.95, rng);
        truth.push_back(t);
    }
    an.processOffline(sig.data(), sig.size());
    std::vector<uint64_t> det;
    for (auto& f : col.frames)
        if (f.onset) det.push_back(f.onsetFrame);
    int hits = 0;
    std::vector<double> errMs;
    std::vector<bool> used(det.size(), false);
    for (size_t t : truth) {
        int best = -1;
        double bd = 1e9;
        for (size_t j = 0; j < det.size(); ++j) {
            double d = ((double)det[j] - (double)t) / FS * 1000.0;
            if (!used[j] && std::fabs(d) < 50 && std::fabs(d) < bd) {
                bd = std::fabs(d);
                best = (int)j;
            }
        }
        if (best >= 0) {
            used[best] = true;
            ++hits;
            errMs.push_back(((double)det[best] - (double)t) / FS * 1000.0);
        }
    }
    int fp = 0;
    for (bool u : used)
        if (!u) ++fp;
    double mean = 0, sd = 0;
    for (double e : errMs) mean += e;
    if (!errMs.empty()) mean /= errMs.size();
    for (double e : errMs) sd += (e - mean) * (e - mean);
    if (!errMs.empty()) sd = std::sqrt(sd / errMs.size());
    std::printf("  %-34s hits %2d/%2d  false %d  timing error %+5.1f +- %4.1f ms\n", name, hits, count, fp, mean, sd);
    CHECK(hits >= count - 1, "%s missed %d", name, count - hits);
    CHECK(fp <= 1, "%s false positives %d", name, fp);
    CHECK(std::fabs(mean) < 6 && sd < 6, "%s timing error %.1f+-%.1f", name, mean, sd);
}

// A sustained note with vibrato (+-25 cents, 5.5 Hz) must not produce extra onsets; pure noise must produce none
static void testFalsePositives() {
    std::printf("== False-positive checks ==\n");
    std::mt19937 rng(11);
    std::normal_distribution<float> N(0.f, 1.f);
    {
        Analyzer an;
        an.configure(FS);
        int onsets = 0;
        an.setFrameCallback([&](const AnalysisFrame& f) { onsets += f.onset; });
        std::vector<float> sig((size_t)(FS * 4), 0.f);
        double ph = 0;
        for (size_t i = 0; i < sig.size(); ++i) {
            double t = i / FS;
            double f = 220.0 * std::pow(2.0, 0.25 / 12 * std::sin(2 * M_PI * 5.5 * t));
            ph += 2 * M_PI * f / FS;
            float env = (float)std::exp(-t * 0.6) * (t < 0.005 ? (float)(t / 0.005) : 1.f);
            sig[i] = 0.2f * env * (float)(std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.25 * std::sin(3 * ph)) + 3e-4f * N(rng);
        }
        an.processOffline(sig.data(), sig.size());
        std::printf("  4 s sustained note with vibrato: onsets %d (expected <= 1)\n", onsets);
        CHECK(onsets <= 1, "vibrato false positives %d", onsets - 1);
    }
    for (float nd : {-80.f, -65.f, -50.f}) {
        Analyzer an;
        an.configure(FS);
        int onsets = 0;
        an.setFrameCallback([&](const AnalysisFrame& f) { onsets += f.onset; });
        std::vector<float> sig((size_t)(FS * 5), 0.f);
        float a = std::pow(10.f, nd / 20.f);
        for (auto& v : sig) v = a * N(rng);
        an.processOffline(sig.data(), sig.size());
        std::printf("  5 s of pure noise at %.0f dBFS: onsets %d\n", nd, onsets);
        CHECK(onsets == 0, "noise %.0f dB false positives %d", nd, onsets);
    }
}

static void testChords() {
    std::printf("== Chord chroma ==\n");
    struct Ch { const char* name; int midi[6]; };  // string 6 -> string 1, -1 = not played
    const Ch chords[] = {
        {"E", {40, 47, 52, 56, 59, 64}},  {"Em", {40, 47, 52, 55, 59, 64}}, {"A", {-1, 45, 52, 57, 61, 64}},
        {"Am", {-1, 45, 52, 57, 60, 64}}, {"C", {-1, 48, 52, 55, 60, 64}},  {"G", {43, 47, 50, 55, 59, 67}},
        {"D", {-1, -1, 50, 57, 62, 66}},  {"Dm", {-1, -1, 50, 57, 62, 65}}, {"F", {41, 48, 53, 57, 60, 65}},
    };
    const int nC = sizeof(chords) / sizeof(chords[0]);
    auto tmpl = [](const Ch& c, float* t) {
        for (int i = 0; i < 12; ++i) t[i] = 0;
        for (int s = 0; s < 6; ++s) {
            if (c.midi[s] < 0) continue;
            int m = c.midi[s];
            t[m % 12] += 1.f;
            t[(m + 7) % 12] += 0.35f;   // 3rd harmonic
            t[(m + 4) % 12] += 0.15f;   // 5th harmonic
        }
        float n = 0;
        for (int i = 0; i < 12; ++i) n += t[i] * t[i];
        n = std::sqrt(n);
        for (int i = 0; i < 12; ++i) t[i] /= n;
    };
    int correct = 0;
    std::mt19937 rng(3);
    for (int ci = 0; ci < nC; ++ci) {
        Analyzer an;
        an.configure(FS);
        std::vector<float> acc(12, 0.f);
        int nf = 0;
        an.setFrameCallback([&](const AnalysisFrame& f) {
            if (f.frame > 4800 + FS * 0.05 && f.frame < 4800 + FS * 0.25) {
                for (int i = 0; i < 12; ++i) acc[i] += f.chroma[i];
                ++nf;
            }
        });
        std::vector<float> sig((size_t)(FS * 1.0), 0.f);
        int k = 0;
        for (int s = 0; s < 6; ++s) {
            if (chords[ci].midi[s] < 0) continue;
            addPluck(sig, 4800 + k * 480, midiHz(chords[ci].midi[s]), 0.15f, 0.9, rng);  // 10 ms strum spacing
            ++k;
        }
        an.processOffline(sig.data(), sig.size());
        float n = 0;
        for (float v : acc) n += v * v;
        n = std::sqrt(n) + 1e-9f;
        int best = -1;
        float bestSim = -1, own = 0;
        for (int cj = 0; cj < nC; ++cj) {
            float t[12];
            tmpl(chords[cj], t);
            float sim = 0;
            for (int i = 0; i < 12; ++i) sim += t[i] * acc[i] / n;
            if (cj == ci) own = sim;
            if (sim > bestSim) {
                bestSim = sim;
                best = cj;
            }
        }
        std::printf("  %-3s own similarity %.3f  best match %-3s (%.3f)\n", chords[ci].name, own, chords[best].name, bestSim);
        if (best == ci) ++correct;
        CHECK(own > 0.75f, "%s own similarity %.2f too low", chords[ci].name, own);
    }
    std::printf("  recognized %d/%d\n", correct, nC);
    CHECK(correct >= nC - 1, "chords recognized %d/%d", correct, nC);
}

int main() {
    testPitch();
    std::printf("== Onset detection ==\n");
    testOnsets(-20, -75, 0.5, false, "medium level, slow");
    testOnsets(-20, -75, 0.25, false, "medium level, eighths @120");
    testOnsets(-20, -75, 0.125, false, "sixteenths @120");
    testOnsets(-20, -75, 0.35, true, "same note repeated");
    testOnsets(-40, -72, 0.4, false, "quiet -40 dB, noise -72 dB");
    testOnsets(-8, -60, 0.4, false, "loud, noise -60 dB");
    testFalsePositives();
    testChords();
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
