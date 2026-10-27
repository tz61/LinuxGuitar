#pragma once
// Plays the engine's output through PulseAudio (e.g. Bluetooth headphones) instead of the sound card.
// The audio thread, clocked by the sound card's capture, writes into a ring buffer; a separate thread feeds a
// PulseAudio stream at the output device's own pace, resampling slightly (DriftReader) to absorb the drift
// between the two clocks. The guitar is still recorded directly from the sound card.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "common/DriftReader.h"
#include "common/SpscRing.h"

typedef struct pa_simple pa_simple;

namespace gs {

struct PulseSink {
    std::string name;         // "bluez_sink.30_7A_D2_4C_74_0C.a2dp_sink"
    std::string description;  // "WH-1000XM4"
    bool bluetooth = false;
    int alsaCard = -1;        // ALSA card behind the sink, -1 if none
};

class PulseOutput {
public:
    PulseOutput();
    ~PulseOutput() { stop(); }
    // UI thread. Connects right away, so a missing device is reported here.
    bool start(const std::string& sink, unsigned rate, std::string& err);
    void stop();
    bool running() const { return run_.load(); }
    // Audio thread: n frames of output (dropped if the device has stalled)
    void write(const float* L, const float* R, int n);
    // Frames from write() to the device's output: our buffer + the delay PulseAudio reports for the stream and
    // the device (Bluetooth headphones may add some more that nobody can measure)
    int latencyFrames() const { return latency_.load(std::memory_order_relaxed); }
    int underruns() const { return underruns_.load(std::memory_order_relaxed); }  // our buffer ran dry

    // Playback devices PulseAudio knows, except our own virtual sink; empty if PulseAudio isn't running
    static std::vector<PulseSink> listSinks();

private:
    void playMain();

    SpscRing<float> ring_;
    DriftReader reader_{ring_};
    std::vector<float> interleaved_;  // audio thread scratch
    pa_simple* pa_ = nullptr;         // owned by the playback thread while it runs
    std::string sink_;
    unsigned rate_ = 48000, target_ = 720;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<int> latency_{0}, underruns_{0};
};

}  // namespace gs
