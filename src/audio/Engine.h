#pragma once
// Audio engine: real-time thread (direct ALSA or a simulated no-device mode), input conditioning, effect chain,
// looper, practice audio, system-audio passthrough, master volume + safety limiter, meters.
// Effect-chain structure changes reach the audio thread through lock-free snapshots (RCU).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/AlsaDuplex.h"
#include "audio/CardMixer.h"
#include "audio/DeviceReserve.h"
#include "audio/Looper.h"
#include "audio/PulseBridge.h"
#include "audio/PulseOutput.h"
#include "fx/Effect.h"
#include "fx/Preset.h"

namespace gs {

class Analyzer;
class PracticeAudio;

enum class InputChannel : int { Auto = 0, Left = 1, Right = 2, Mix = 3 };

struct EngineConfig {
    AlsaConfig alsa;
    int rtPriority = 80;
    bool reserveDevice = true;  // ask PulseAudio (via D-Bus) to release the card
    bool simulate = false;      // no-device (simulated) mode
    bool fallbackToSim = true;  // fall back to no-device mode if the card can't be opened (UI keeps working)
    std::string outputSink;     // PulseAudio sink to play through (e.g. Bluetooth headphones); empty = the sound card
};

struct EngineStatus {
    bool running = false, simulated = false;
    std::string device, error, rtMsg, reserveMsg, format;
    unsigned rate = 0, period = 0;
    unsigned fifoFrames = 0, usedFrames = 0;  // calibration: FIFO prefetch / minimum prefill actually needed
    bool calibrated = false;
    int rtPriority = 0;
    bool pulseOutput = false;  // playing through PulseAudio (outputSink) instead of the sound card
    std::string outputMsg;     // why the requested output couldn't be used
};

struct FxSlot {
    std::unique_ptr<Effect> fx;
    uint64_t id = 0;
    // ---- audio thread only ----
    float fade = 0.f;
};

class Engine {
public:
    static constexpr int kMaxFx = 24;
    Engine();
    ~Engine();

    // ---- lifecycle (UI thread) ----
    bool start(const EngineConfig& cfg);
    void stop();
    bool running() const { return running_.load(); }
    EngineStatus status() const;
    void attachAnalyzer(Analyzer* a) { analyzer_ = a; }
    void attachPractice(PracticeAudio* p) { practice_ = p; }
    // Called before the audio thread starts (sample rate, period, output latency frames) to prepare other modules
    std::function<void(double, int, int)> onPrepare;
    // Generates the input in no-device mode (tests / demo)
    std::function<void(float*, float*, int, uint64_t)> simInput;

    double sampleRate() const { return rate_; }
    int period() const { return (int)period_; }
    int prefillFrames() const { return (int)prefill_.load(std::memory_order_relaxed); }
    // How many frames our output lags the corresponding input (sound card: the prefill; PulseAudio: its buffers
    // and the device's delay); practice scoring and the practice display compensate for it
    int outputLatencyFrames() const { return pulseMode_.load() ? pulseOut_.latencyFrames() : prefillFrames(); }
    bool pulseOutput() const { return pulseMode_.load(); }
    float roundTripMs() const;  // round-trip estimate: prefill + effect latency + ~0.6 ms AD/DA

    // ---- effect chain (UI thread) ----
    const std::vector<std::unique_ptr<FxSlot>>& chain() const { return chain_; }
    FxSlot* addEffect(const std::string& typeId, int index = -1);
    void removeEffect(int index);
    void moveEffect(int from, int to);
    void clearChain();
    bool loadPreset(const PresetData& p, std::string* warnings = nullptr);
    PresetData currentPreset(const std::string& name) const;
    void collectGarbage();  // call every UI frame: frees effects the audio thread no longer uses
    int fxLatencySamples() const;

    // ---- parameters (any thread) ----
    std::atomic<int> inputChannel{0};      // InputChannel
    std::atomic<float> inputGainDb{0.f};   // digital input gain
    std::atomic<float> guitarDb{0.f};      // guitar (post-effects) level
    std::atomic<float> masterDb{-6.f};     // master volume
    std::atomic<float> systemAudioDb{-6.f};
    std::atomic<bool> mute{false};
    std::atomic<bool> guitarMute{false};   // mute only the guitar (e.g. while tuning)
    std::atomic<bool> limiterOn{true};

    // ---- meters (read by the UI) ----
    float inputPeakDb() const { return inPeak_.load(std::memory_order_relaxed); }
    float inputRmsDb() const { return inRms_.load(std::memory_order_relaxed); }
    float outputPeakDb() const { return outPeak_.load(std::memory_order_relaxed); }
    bool inputClipped(double withinSec = 1.5) const;
    bool outputLimited(double withinSec = 1.0) const;
    float cpuLoad() const { return cpuAvg_.load(std::memory_order_relaxed); }
    float cpuPeak() const { return cpuPeak_.load(std::memory_order_relaxed); }
    int xruns() const { return xruns_.load(std::memory_order_relaxed) + (pulseMode_.load() ? pulseOut_.underruns() : 0); }
    float autoLeftWeight() const { return wLpub_.load(std::memory_order_relaxed); }
    float inputLeftDb() const { return inLdb_.load(std::memory_order_relaxed); }
    float inputRightDb() const { return inRdb_.load(std::memory_order_relaxed); }
    uint64_t frameClock() const { return clockPub_.load(std::memory_order_relaxed); }
    void resetXruns() { xruns_.store(0); }
    // Times of the first xruns (seconds since engine start), for diagnostics
    std::vector<double> xrunTimes() const {
        std::vector<double> v;
        int n = std::min(xruns_.load(), 16);
        for (int i = 0; i < n; ++i) v.push_back(xrunAt_[i].load());
        return v;
    }

    Looper& looper() { return looper_; }
    CardMixer& mixer() { return mixer_; }
    PulseBridge& pulse() { return pulse_; }
    DeviceReservation& reservation() { return reservation_; }

private:
    struct ChainSnap {
        FxSlot* slots[kMaxFx];
        int count = 0;
        uint64_t seq = 0;
    };
    struct Grave {
        std::unique_ptr<FxSlot> slot;
        uint64_t seq;
    };
    void audioMain();
    void captureOnlyLoop();  // sound card records, PulseOutput plays
    void simMain();
    void processBlock(const float* inL, const float* inR, float* L, float* R, int n);
    void publishChain();
    void allocBuffers(int maxBlock);

    EngineConfig cfg_;
    AlsaDuplex alsa_;
    CardMixer mixer_;
    DeviceReservation reservation_;
    PulseBridge pulse_;
    PulseOutput pulseOut_;
    std::atomic<bool> pulseMode_{false};
    Looper looper_;
    Analyzer* analyzer_ = nullptr;
    PracticeAudio* practice_ = nullptr;

    std::thread th_;
    std::atomic<bool> running_{false};
    bool simulated_ = false;
    double rate_ = 48000;
    unsigned period_ = 16;
    std::atomic<unsigned> prefill_{64};
    mutable std::mutex statusMu_;
    EngineStatus status_;

    // Effect chain
    std::vector<std::unique_ptr<FxSlot>> chain_;
    std::vector<Grave> graves_;
    ChainSnap snaps_[64];
    std::atomic<ChainSnap*> pendingSnap_{nullptr};
    ChainSnap* activeSnap_ = nullptr;
    std::atomic<uint64_t> rtSeq_{0};
    uint64_t pubSeq_ = 0, nextId_ = 1;

    // Audio-thread buffers and state
    std::vector<float> inL_, inR_, outL_, outR_, mono_, dryL_, dryR_, guitL_, guitR_;
    uint64_t frameClock_ = 0;
    double powL_ = 1e-9, powR_ = 1e-9;
    float wL_ = 0.5f, inGainSm_ = 1.f, guitarSm_ = 1.f, masterSm_ = 0.f;
    float dcX1_ = 0, dcY1_ = 0, limGain_ = 1.f;

    std::atomic<float> inPeak_{-120}, inRms_{-120}, outPeak_{-120}, cpuAvg_{0}, cpuPeak_{0}, wLpub_{0.5f};
    std::atomic<float> inLdb_{-120}, inRdb_{-120};
    std::atomic<uint64_t> inClipAt_{0}, limitAt_{0}, clockPub_{0};
    std::atomic<int> xruns_{0};
    std::atomic<double> xrunAt_[16] = {};
    double startTime_ = 0;
    int dmaLatencyFd_ = -1;  // /dev/cpu_dma_latency (keeps CPUs out of deep sleep; needs permission)
    void noteXrun();
};

// Registers the Chinese translations of audio-module messages (implemented in audio/I18nZh.cpp).
void registerAudioTranslations();

inline float dbToLin(float db) { return db <= -120.f ? 0.f : std::pow(10.f, db / 20.f); }
inline float linToDb(float v) { return v <= 1e-6f ? -120.f : 20.f * std::log10(v); }

}  // namespace gs
