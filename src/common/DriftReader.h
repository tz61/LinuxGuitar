#pragma once
// Reads stereo audio from a ring buffer that is filled on another clock (another sound device, PulseAudio's
// timer), resampling very slightly so the buffer stays near a target level (PI control). This absorbs the
// drift between the two crystals without dropouts or clicks. Used from the consumer thread only.
#include <cstddef>
#include <vector>

#include "common/SpscRing.h"

namespace gs {

class DriftReader {
public:
    // ring: interleaved stereo; maxFrames: the largest n passed to read()
    explicit DriftReader(SpscRing<float>& ring, size_t maxFrames = 4096);
    // Start over (new stream): drop what is buffered and wait until targetFrames are available
    void reset(unsigned rate, unsigned targetFrames);
    // Writes n frames to L/R. While (re)buffering (at the start, after an underrun) it writes silence and
    // returns false.
    bool read(float* L, float* R, int n);
    double ratio() const { return ratio_; }  // input frames consumed per output frame
    bool primed() const { return primed_; }  // false while (re)buffering
    void setTarget(unsigned targetFrames) { target_ = targetFrames ? targetFrames : 1; }  // keeps what is buffered

private:
    SpscRing<float>& ring_;
    std::vector<float> scratch_;
    double rate_ = 48000;
    unsigned target_ = 1440;
    bool primed_ = false;
    double pos_ = 0, ratio_ = 1, errAvg_ = 0, integ_ = 0;
    float prevL_ = 0, prevR_ = 0, curL_ = 0, curR_ = 0;
};

}  // namespace gs
