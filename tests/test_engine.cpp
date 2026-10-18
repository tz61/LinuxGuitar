// Engine hardware test: run the real sound card for a few seconds (output muted) and report xruns / CPU / RT priority.
// Usage: test_engine [device=hw:PCH,0] [seconds=3]
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "audio/AlsaDuplex.h"
#include "audio/Engine.h"

using namespace gs;

static bool runOnce(const std::string& dev, unsigned period, float safetyMs, double secs) {
    Engine eng;
    eng.mute.store(true);  // keep the speakers silent during the test
    EngineConfig cfg;
    cfg.alsa.device = dev;
    cfg.alsa.period = period;
    cfg.alsa.safetyMs = safetyMs;
    cfg.fallbackToSim = false;
    if (!eng.start(cfg)) {
        auto st = eng.status();
        std::printf("  period %4u: open failed: %s\n", period, st.error.c_str());
        return false;
    }
    for (const char* t : {"noisegate", "compressor", "amp", "cabinet", "eq", "chorus", "delay", "reverb"}) eng.addEffect(t);
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
    auto st = eng.status();
    std::printf("  period %3u safety %.2fms: FIFO %3u calibrated %3u prefill %3d frames -> round trip ~%5.2f ms  xrun %d  CPU avg %4.1f%% peak %5.1f%%\n",
                st.period, safetyMs, st.fifoFrames, st.usedFrames, eng.prefillFrames(), eng.roundTripMs(), eng.xruns(),
                eng.cpuLoad() * 100, eng.cpuPeak() * 100);
    static bool once = false;
    if (!once) {
        once = true;
        std::printf("  real-time: %s\n  reservation: %s\n  format: %s\n", st.rtMsg.c_str(), st.reserveMsg.c_str(), st.format.c_str());
    }
    eng.stop();
    return true;
}

int main(int argc, char** argv) {
    std::string dev = argc > 1 ? argv[1] : "hw:PCH,0";
    double secs = argc > 2 ? std::atof(argv[2]) : 3.0;
    std::printf("== ALSA devices ==\n");
    for (auto& d : AlsaDuplex::listDevices())
        std::printf("  %-12s %s %s%s\n", d.id.c_str(), d.name.c_str(), d.canCapture ? "[capture]" : "", d.canPlayback ? "[playback]" : "");
    std::printf("== Full-duplex test (%s, %.0f s per setting, 8 effects loaded, output muted) ==\n", dev.c_str(), secs);
    bool ok = runOnce(dev, 16, 0.5f, secs);
    if (ok) {
        runOnce(dev, 16, 0.25f, secs);
        runOnce(dev, 32, 0.5f, secs);
        runOnce(dev, 64, 0.5f, secs);
        runOnce(dev, 128, 0.5f, secs);
    }
    return ok ? 0 : 1;
}
