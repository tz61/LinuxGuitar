#include "dsp/Wav.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "common/I18n.h"
#include "dsp/DspUtil.h"

namespace gs::dsp {

namespace {

uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// Zeroth-order modified Bessel function of the first kind (for the Kaiser window)
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double h = x * 0.5;
    for (int k = 1; k < 64; ++k) {
        term *= (h / k) * (h / k);
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

}  // namespace

bool parseWav(const unsigned char* d, size_t size, WavData& out, std::string& err) {
    out = WavData{};
    if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) {
        err = tr("Not a WAV file (missing RIFF/WAVE header)");
        return false;
    }
    int fmtTag = 0, channels = 0, bits = 0, blockAlign = 0;
    uint32_t rate = 0;
    const unsigned char* dataPtr = nullptr;
    size_t dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const unsigned char* ck = d + pos;
        const uint32_t len = rd32(ck + 4);
        const size_t body = pos + 8;
        const size_t avail = size - body;
        const size_t clen = len > avail ? avail : len;  // truncated file: read what we can
        if (std::memcmp(ck, "fmt ", 4) == 0) {
            if (clen < 16) {
                err = tr("WAV fmt chunk is too short");
                return false;
            }
            fmtTag = rd16(d + body);
            channels = rd16(d + body + 2);
            rate = rd32(d + body + 4);
            blockAlign = rd16(d + body + 12);
            bits = rd16(d + body + 14);
            if (fmtTag == 0xFFFE && clen >= 26) fmtTag = rd16(d + body + 24);  // extensible: first two bytes of the sub-format GUID
        } else if (std::memcmp(ck, "data", 4) == 0) {
            dataPtr = d + body;
            dataLen = clen;
        }
        pos = body + (size_t)len + (len & 1u);
        if (pos < body) break;  // overflow guard
    }
    if (!dataPtr) {
        err = tr("WAV file has no data chunk");
        return false;
    }
    if (channels <= 0 || channels > 64 || rate < 1000 || rate > 1536000) {
        err = tr("Invalid WAV parameters (channels/sample rate)");
        return false;
    }
    const bool isFloat = (fmtTag == 3);
    if (!(fmtTag == 1 || isFloat)) {
        err = tr("Unsupported WAV encoding (only PCM and float are supported)");
        return false;
    }
    if (!((fmtTag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (isFloat && (bits == 32 || bits == 64)))) {
        err = std::string(tr("Unsupported bit depth: ")) + std::to_string(bits);
        return false;
    }
    const int bps = bits / 8;
    const int frameBytes = (blockAlign >= bps * channels) ? blockAlign : bps * channels;
    const size_t frames = dataLen / (size_t)frameBytes;
    if (frames == 0) {
        err = tr("WAV file has no audio data");
        return false;
    }
    if (frames > 20u * 1000u * 1000u) {
        err = tr("WAV file is too long");
        return false;
    }
    out.sampleRate = (int)rate;
    out.channels = channels;
    out.bits = bits;
    out.mono.resize(frames);
    const float invCh = 1.f / (float)channels;
    for (size_t f = 0; f < frames; ++f) {
        const unsigned char* p = dataPtr + f * (size_t)frameBytes;
        double acc = 0.0;
        for (int c = 0; c < channels; ++c) {
            const unsigned char* s = p + c * bps;
            double v = 0.0;
            if (isFloat) {
                if (bits == 32) {
                    float fv;
                    std::memcpy(&fv, s, 4);
                    v = fv;
                } else {
                    double dv;
                    std::memcpy(&dv, s, 8);
                    v = dv;
                }
            } else if (bits == 8) {
                v = ((int)s[0] - 128) / 128.0;
            } else if (bits == 16) {
                v = (int16_t)rd16(s) / 32768.0;
            } else if (bits == 24) {
                int32_t iv = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8;
                v = iv / 8388608.0;
            } else {
                v = (int32_t)rd32(s) / 2147483648.0;
            }
            if (!std::isfinite(v)) v = 0.0;
            acc += v;
        }
        out.mono[f] = (float)acc * invCh;
    }
    return true;
}

bool readWav(const std::string& path, WavData& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = std::string(tr("Cannot open file: ")) + path;
        return false;
    }
    std::vector<unsigned char> buf;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512L * 1024 * 1024) {
        std::fclose(f);
        err = tr("Invalid file size");
        return false;
    }
    buf.resize((size_t)sz);
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) {
        err = tr("Failed to read file");
        return false;
    }
    return parseWav(buf.data(), buf.size(), out, err);
}

bool writeWavFloat(const std::string& path, const float* data, size_t n, int sampleRate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto w32 = [&](uint32_t v) { unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto w16 = [&](uint16_t v) { unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)}; std::fwrite(b, 1, 2, f); };
    const uint32_t dataBytes = (uint32_t)(n * 4);
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(3);
    w16(1);
    w32((uint32_t)sampleRate);
    w32((uint32_t)sampleRate * 4);
    w16(4);
    w16(32);
    std::fwrite("data", 1, 4, f);
    w32(dataBytes);
    std::fwrite(data, 4, n, f);
    std::fclose(f);
    return true;
}

std::vector<float> resample(const std::vector<float>& in, double srcRate, double dstRate) {
    if (in.empty() || srcRate <= 0 || dstRate <= 0) return {};
    if (std::fabs(srcRate - dstRate) < 1e-6) return in;
    const double ratio = dstRate / srcRate;
    const size_t outLen = (size_t)std::ceil((double)in.size() * ratio);
    std::vector<float> out(outLen, 0.f);
    // When downsampling, the cutoff follows the target rate
    const double cutoff = 0.5 * std::min(1.0, ratio) * 0.97;  // relative to the source rate
    const int halfTaps = 32;
    const double halfWidth = halfTaps / std::min(1.0, ratio);  // in source samples
    const double beta = 8.0;
    const double i0b = besselI0(beta);
    for (size_t o = 0; o < outLen; ++o) {
        const double t = (double)o / ratio;  // source position
        const long first = (long)std::ceil(t - halfWidth);
        const long last = (long)std::floor(t + halfWidth);
        double acc = 0.0, wsum = 0.0;
        for (long k = first; k <= last; ++k) {
            const double x = (double)k - t;
            const double r = x / halfWidth;
            if (r <= -1.0 || r >= 1.0) continue;
            const double win = besselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
            const double arg = 2.0 * cutoff * x;
            const double sinc = (std::fabs(arg) < 1e-12) ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
            const double w = 2.0 * cutoff * sinc * win;
            wsum += w;
            if (k >= 0 && k < (long)in.size()) acc += w * in[(size_t)k];
        }
        (void)wsum;
        out[o] = (float)acc;
    }
    return out;
}

}  // namespace gs::dsp
