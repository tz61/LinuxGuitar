#pragma once
// System audio passthrough: while we own the sound card, audio from other programs (browser, players) would
// stall. We create a virtual output in PulseAudio, make it the default, record its monitor and mix it into our
// output, so lesson videos / backing tracks keep playing while practicing. The ~30 ms backing-track delay does
// not affect guitar monitoring latency.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "common/SpscRing.h"

namespace gs {

class PulseBridge {
public:
    PulseBridge();
    ~PulseBridge() { stop(); }
    bool start(unsigned rate, std::string& msg);  // UI thread
    void stop();                                  // UI thread: restore the default sink, move streams back, unload the virtual sink
    bool running() const { return run_.load(); }
    // Audio thread: add n frames to L/R (with clock-drift compensation)
    void mixInto(float* L, float* R, int n, float gain);
    // At startup: remove a virtual sink left behind by a previous crash
    static void cleanupStale();
    // Called from a crash signal handler (async-signal-safe calls only): unload the virtual sink so system
    // audio returns to the original device
    static void emergencyRestore();
    static bool pactlAvailable();

private:
    void captureMain(unsigned rate);

    SpscRing<float> ring_;  // interleaved stereo; allocated in the constructor, never reallocated
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<uint32_t> gen_{0};
    std::atomic<unsigned> target_{1440};
    std::string origSink_;
    int module_ = -1;

    // ---- audio thread only ----
    uint32_t seenGen_ = 0;
    bool primed_ = false;
    double pos_ = 0, ratio_ = 1, errAvg_ = 0, integ_ = 0;
    float prevL_ = 0, prevR_ = 0, curL_ = 0, curR_ = 0;
    float gainSm_ = 0;
    std::vector<float> scratch_;
};

// Run an external program (no shell) and capture its stdout. Returns the exit code, -1 if it couldn't run.
int runProgram(const std::vector<std::string>& argv, std::string* out = nullptr, int timeoutMs = 5000);

}  // namespace gs
