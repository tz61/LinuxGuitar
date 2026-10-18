#pragma once
// Effect base interface.
//
// Threading:
//   - prepare()/loadFile() run on a non-real-time thread (they may allocate).
//   - process()/reset() run on the real-time audio thread: no allocation, locks, syscalls or exceptions.
//   - setParam() may be called from any thread (UI). Parameters are atomic floats that the effect reads at the
//     start of process() and smooths itself.
//   - Bypass is handled by the effect chain (crossfade); effects don't deal with it. When coming back from
//     bypass the chain calls reset() first.
//
// All human-readable strings (display names, parameter names, choices, descriptions, meter names) are plain
// English; the UI translates them with tr() when it draws them.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gs {

enum class ParamScale : uint8_t {
    Linear,  // linear
    Log,     // logarithmic (frequency/time), requires minV > 0
    Int,     // integer steps
    Toggle,  // on/off (0/1)
    Choice,  // option index (0..choices.size()-1)
};

struct ParamInfo {
    std::string id;                   // stable key used by preset files, e.g. "drive"
    std::string name;                 // display name (English), e.g. "Drive"
    float minV = 0.f, maxV = 1.f, defV = 0.f;
    std::string unit;                 // "dB" "ms" "Hz" "%" "" ...
    ParamScale scale = ParamScale::Linear;
    std::vector<std::string> choices; // option names when scale == Choice
};

enum class FxCategory : uint8_t { Dynamics, Drive, Amp, Cabinet, EQ, Filter, Modulation, Delay, Reverb, Pitch, Utility };

class Effect {
public:
    explicit Effect(std::vector<ParamInfo> params);
    virtual ~Effect() = default;
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;

    virtual const char* typeId() const = 0;       // "overdrive"
    virtual const char* displayName() const = 0;  // "Overdrive"
    virtual FxCategory category() const = 0;

    virtual void prepare(double sampleRate, int maxBlock) = 0;
    virtual void reset() = 0;
    // Process n stereo frames in place, 0 < n <= maxBlock. Usually L == R (the guitar is mono).
    virtual void process(float* L, float* R, int n) = 0;
    // Latency added by this effect (samples); only used to display the total latency.
    virtual int latencySamples() const { return 0; }

    // Optional external file (e.g. a cabinet IR). Called from a non-real-time thread; switching to the new
    // data must be thread-safe.
    virtual bool acceptsFile() const { return false; }
    virtual bool loadFile(const std::string& path, std::string& err);
    virtual std::string fileLabel() const { return {}; }
    virtual std::string filePath() const { return {}; }

    // Optional meters for the UI (e.g. compressor gain reduction in dB, gate opening 0..1).
    // Names of dB-valued meters contain "dB".
    virtual int numMeters() const { return 0; }
    virtual const char* meterName(int i) const { (void)i; return ""; }
    virtual float meterValue(int i) const { (void)i; return 0.f; }

    int numParams() const { return (int)info_.size(); }
    const ParamInfo& paramInfo(int i) const { return info_[i]; }
    int paramIndex(const std::string& id) const;  // returns -1 if not found
    void setParam(int i, float v);                // clamps (rounds Int/Toggle/Choice)
    bool setParam(const std::string& id, float v);
    float param(int i) const { return values_[i].load(std::memory_order_relaxed); }
    void resetParamsToDefault();

    std::atomic<bool> bypassed{false};

protected:
    std::vector<ParamInfo> info_;
    std::unique_ptr<std::atomic<float>[]> values_;
};

struct EffectTypeInfo {
    std::string typeId;
    std::string displayName;
    FxCategory category;
    std::string description;  // one-sentence English description for beginners
};

// Factory (see fx/Registry.cpp)
const std::vector<EffectTypeInfo>& effectTypes();
std::unique_ptr<Effect> createEffect(const std::string& typeId);  // returns nullptr for unknown types

// Global tempo (quarter-note BPM) for tempo-synced delay/tremolo. Written by the UI/practice module, read by effects.
std::atomic<float>& globalBpm();

// Registers the Chinese translations of all effect/preset user-facing strings (implemented in fx/I18nZh.cpp).
void registerFxTranslations();

}  // namespace gs
