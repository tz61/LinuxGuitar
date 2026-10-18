#pragma once
// Direct ALSA full duplex on a hw device: capture and playback are linked and started together; each cycle
// reads one period -> processes it -> writes one period.
//
// Latency model: the playback stream runs X "prefill" frames ahead of capture, so the round trip is
// X / rate + AD/DA converter delay (~0.6 ms). X cannot be too small: at start the HDA controller's FIFO
// prefetches ~48 frames immediately, and capture period readiness jitters (IRQ/DMA timing). So at startup we
// run about half a second of silence with a large prefill, measure the worst headroom actually consumed, and
// use "required + safety margin" as X.
#include <string>
#include <vector>

typedef struct _snd_pcm snd_pcm_t;

namespace gs {

struct AlsaConfig {
    std::string device = "hw:PCH,0";
    unsigned rate = 48000;
    unsigned period = 16;        // frames per period (smaller = lower latency, more frequent wakeups)
    float safetyMs = 1.0f;       // safety margin on top of the calibration (0.5 ms = lower latency, occasional xruns)
    unsigned fixedPrefill = 0;   // > 0: skip calibration and use this prefill (frames)
};

struct AlsaDeviceInfo {
    std::string id;     // "hw:PCH,0"
    std::string name;   // "HDA Intel PCH: ALCS1200A Analog"
    int card = -1, device = 0;
    bool canCapture = false, canPlayback = false;
};

struct AlsaCalibration {
    unsigned fifoFrames = 0;   // FIFO prefetch at playback start
    unsigned usedFrames = 0;   // worst headroom consumed during calibration (FIFO + one period + capture jitter)
    unsigned prefill = 0;      // resulting prefill
    bool ok = false;
};

class AlsaDuplex {
public:
    ~AlsaDuplex() { close(); }
    bool open(const AlsaConfig& cfg, std::string& err);
    void close();
    bool isOpen() const { return cap_ && play_; }

    // Called from the audio thread
    AlsaCalibration calibrate(double seconds = 0.5);   // run silence to measure the required prefill
    bool start(unsigned prefillFrames);                // prefill silence and start both streams
    int readPeriod(float* L, float* R);                // blocking read of one period; <0 = error (xrun)
    int writePeriod(const float* L, const float* R);
    bool recover() { return start(prefill_); }
    void stop();

    unsigned rate() const { return rate_; }
    unsigned period() const { return period_; }
    unsigned prefill() const { return prefill_; }
    unsigned maxPrefill() const { return playBufFrames_ > 2 * period_ ? playBufFrames_ - 2 * period_ : period_; }
    unsigned captureChannels() const { return capCh_; }
    std::string formatDesc() const;

    static std::vector<AlsaDeviceInfo> listDevices();
    static int cardIndexOf(const std::string& device);  // "hw:PCH,0" -> 1

private:
    bool setupHw(snd_pcm_t* pcm, bool playback, unsigned& rate, unsigned& period, unsigned bufFrames,
                 unsigned& channels, int& format, unsigned& actualBuf, std::string& err);
    bool setupSw(snd_pcm_t* pcm, unsigned period);
    bool writeSilence(unsigned frames);

    snd_pcm_t* cap_ = nullptr;
    snd_pcm_t* play_ = nullptr;
    bool linked_ = false;
    unsigned rate_ = 0, period_ = 0, prefill_ = 0, capCh_ = 2, playCh_ = 2, playBufFrames_ = 0, capBufFrames_ = 0;
    int capFmt_ = 0, playFmt_ = 0;  // snd_pcm_format_t
    std::vector<char> capBuf_, playBuf_, silence_;
};

}  // namespace gs
