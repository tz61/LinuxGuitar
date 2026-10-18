#pragma once
// Real-time guitar signal analysis (own thread, not the audio thread):
//   - pitch: McLeod Pitch Method (NSDF), ~43 ms window, one frame every ~5.3 ms
//   - onsets: log spectral flux + adaptive threshold, refined to ~1.3 ms with short-time energy
//   - pitch-class energy (chroma) for chord recognition
// Frame indices match the audio engine's frame clock, so they line up with the score timeline.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "analysis/Fft.h"
#include "common/SpscRing.h"

namespace gs {

struct AnalysisFrame {
    uint64_t frame = 0;       // absolute sample index of the pitch window's center
    uint64_t hopEnd = 0;      // index of the newest sample in this hop + 1
    float rmsDb = -120.f;     // level of the latest hop
    float noiseDb = -90.f;    // estimated noise floor
    float f0 = 0.f;           // Hz, 0 = no pitch
    float clarity = 0.f;      // 0..1
    float midi = -1.f;        // fractional MIDI pitch, -1 = none
    bool onset = false;
    uint64_t onsetFrame = 0;  // onset position (samples)
    float onsetStrength = 0.f;
    float chroma[12] = {};    // L2-normalized
    // Pitch salience (harmonic sum, MIDI kSalLo..kSalLo+kSalN-1), unnormalized. Scoring compares it before and
    // after an onset, which tells whether the expected note was just plucked even while other strings ring (an
    // interval makes single-pitch detection report a virtual fundamental).
    static constexpr int kSalLo = 36, kSalN = 56;
    float salience[kSalN] = {};
};

struct PitchPoint {
    uint64_t frame;
    float midi;     // -1 = none
    float clarity;
    float rmsDb;
    bool onset;
};

class Analyzer {
public:
    Analyzer();
    ~Analyzer() { stop(); }

    void configure(double sampleRate);  // call while stopped
    void resetClock(uint64_t startFrame = 0);
    void start();
    void stop();
    bool running() const { return run_.load(); }

    // Audio thread: push the mono dry signal
    void push(const float* x, int n) {
        size_t w = ring_.write(x, (size_t)n);
        if (w < (size_t)n) dropped_.fetch_add(n - w, std::memory_order_relaxed);
    }

    // The callback runs on the analysis thread (used by the practice evaluator). Set it before start().
    void setFrameCallback(std::function<void(const AnalysisFrame&)> cb) { cb_ = std::move(cb); }

    // Offline processing (tests/simulation): synchronous, no thread
    void processOffline(const float* x, size_t n);

    // Tuner readings (smoothed)
    float tunerHz() const { return tunerHz_.load(std::memory_order_relaxed); }
    float tunerClarity() const { return tunerClarity_.load(std::memory_order_relaxed); }
    float levelDb() const { return levelDb_.load(std::memory_order_relaxed); }
    float noiseDb() const { return noiseDb_.load(std::memory_order_relaxed); }
    float currentMidi() const { return curMidi_.load(std::memory_order_relaxed); }
    uint64_t latestFrame() const { return latest_.load(std::memory_order_acquire); }
    double sampleRate() const { return fs_; }
    int hopSize() const { return hop_; }
    int pitchWindow() const { return W_; }
    int chromaWindow() const { return Nc_; }

    void copyHistory(uint64_t sinceFrame, std::vector<PitchPoint>& out) const;

    // Helpers
    static float hzToMidi(float hz) { return hz > 0 ? 69.f + 12.f * std::log2(hz / 440.f) : -1.f; }
    static float midiToHz(float m) { return 440.f * std::pow(2.f, (m - 69.f) / 12.f); }

private:
    void threadMain();
    void ingest(const float* x, size_t n);
    void processHop();
    float mpm(const float* x, int W, float& clarity);
    float onsetFlux(const float* x);
    void computeSpectrum(const float* x);           // 4096-point (48 kHz) Hann magnitude spectrum -> mag_
    void computeChroma(float* chroma);
    void computeSalience(float* sal);
    uint64_t refineOnset(uint64_t searchStart, uint64_t searchEnd);
    void copyLast(float* dst, int n, uint64_t endAbs) const;

    double fs_ = 48000;
    int hop_ = 256, W_ = 2048, Nf_ = 1024, Nc_ = 4096;
    SpscRing<float> ring_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> run_{false};
    std::thread th_;
    std::function<void(const AnalysisFrame&)> cb_;

    // Sample history (circular buffer)
    std::vector<float> hist_;
    uint64_t histMask_ = 0;
    uint64_t absPos_ = 0;      // absolute number of samples written to the history (including the start offset)
    uint64_t nextHopEnd_ = 0;
    uint64_t droppedSeen_ = 0;

    // FFT workspace
    Fft fftW2_, fftNf_, fftNc_;
    std::vector<float> re_, im_, win_, hannNf_, hannNc_, nsdf_, prevLogMag_, tmp_, mag_;

    // Onset state
    std::vector<float> odfHist_;
    float odfPrev1_ = 0, odfPrev2_ = 0, odfMax_ = 1e-3f;
    uint64_t lastOnset_ = 0;
    bool haveOnset_ = false;
    uint64_t warmupUntil_ = 0;   // warm-up after a clock reset (history still zeros, no onset detection)
    bool havePrevMag_ = false;
    float levelEnv_ = 1e-3f, noiseFloor_ = -80.f;
    uint64_t pendingOnsetCheckEnd_ = 0;

    // Tuner
    float tunerBuf_[7] = {};
    int tunerN_ = 0, tunerI_ = 0;
    std::atomic<float> tunerHz_{0}, tunerClarity_{0}, levelDb_{-120}, noiseDb_{-90}, curMidi_{-1};
    std::atomic<uint64_t> latest_{0};

    mutable std::mutex histMu_;
    std::vector<PitchPoint> pitchHist_;
    size_t pitchHistPos_ = 0, pitchHistCount_ = 0;
};

}  // namespace gs
