#pragma once
// WAV reader (non-real-time) + windowed-sinc resampling.
#include <string>
#include <vector>

namespace gs::dsp {

struct WavData {
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    std::vector<float> mono;  // mono mix (average of all channels)
};

// Supports 8/16/24/32-bit integer PCM, 32/64-bit float and WAVE_FORMAT_EXTENSIBLE.
bool readWav(const std::string& path, WavData& out, std::string& err);
bool parseWav(const unsigned char* data, size_t size, WavData& out, std::string& err);
// Write a 32-bit float mono WAV (for tests/debugging)
bool writeWavFloat(const std::string& path, const float* data, size_t n, int sampleRate);

// Kaiser-windowed sinc resampling (non-real-time, meant for short signals such as IRs)
std::vector<float> resample(const std::vector<float>& in, double srcRate, double dstRate);

}  // namespace gs::dsp
