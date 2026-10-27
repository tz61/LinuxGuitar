#include "audio/PulseOutput.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>

#include "audio/PulseBridge.h"  // runProgram

namespace gs {

namespace {
pa_simple* connectStream(const std::string& sink, unsigned rate, int* err) {
    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = rate;
    ss.channels = 2;
    pa_buffer_attr ba{};
    const uint32_t frameBytes = 2 * sizeof(float);
    ba.maxlength = (uint32_t)-1;
    ba.tlength = rate * 30 / 1000 * frameBytes;  // ask for 30 ms; Bluetooth devices add their own delay on top
    ba.prebuf = (uint32_t)-1;
    ba.minreq = rate * 5 / 1000 * frameBytes;
    ba.fragsize = (uint32_t)-1;
    return pa_simple_new(nullptr, "Guitar Studio", PA_STREAM_PLAYBACK, sink.empty() ? nullptr : sink.c_str(), "Guitar", &ss,
                         nullptr, &ba, err);
}

std::string unquote(const std::string& v) {
    std::string s = v;
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}
}  // namespace

PulseOutput::PulseOutput() {
    ring_.init(1u << 17);  // 0.68 s of 96 kHz stereo
    interleaved_.assign(8192, 0.f);
}

bool PulseOutput::start(const std::string& sink, unsigned rate, std::string& err) {
    stop();
    int e = 0;
    pa_ = connectStream(sink, rate, &e);
    if (!pa_) {
        err = pa_strerror(e);
        return false;
    }
    sink_ = sink;
    rate_ = rate;
    // Our own buffer must cover the largest burst PulseAudio takes at once (measured: ~20 ms for Bluetooth); it
    // grows after an underrun
    target_ = rate * 30 / 1000;
    latency_.store((int)(target_ + rate * 30 / 1000));  // until PulseAudio reports the real figure
    underruns_.store(0);
    run_.store(true);
    th_ = std::thread(&PulseOutput::playMain, this);
    return true;
}

void PulseOutput::stop() {
    run_.store(false);
    if (th_.joinable()) th_.join();
    if (pa_) {
        pa_simple_free(pa_);
        pa_ = nullptr;
    }
}

void PulseOutput::write(const float* L, const float* R, int n) {
    n = std::min(n, (int)interleaved_.size() / 2);
    for (int i = 0; i < n; ++i) {
        interleaved_[2 * i] = L[i];
        interleaved_[2 * i + 1] = R[i];
    }
    ring_.write(interleaved_.data(), (size_t)n * 2);
}

void PulseOutput::playMain() {
    const int chunk = (int)std::max(64u, rate_ / 200);  // 5 ms
    std::vector<float> L(chunk), R(chunk), buf((size_t)chunk * 2);
    reader_.reset(rate_, target_);
    double latSm = -1;  // PulseAudio's figure for Bluetooth wanders by ~20 ms; smoothed over a few seconds
    int k = 0;
    while (run_.load()) {
        if (!pa_) {
            // The device went away (headphones switched off): try it again, else whatever PulseAudio plays on now
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            int e = 0;
            pa_ = connectStream(sink_, rate_, &e);
            if (!pa_) pa_ = connectStream({}, rate_, &e);
            reader_.reset(rate_, target_);
            continue;
        }
        const bool wasPlaying = reader_.primed();
        if (!reader_.read(L.data(), R.data(), chunk) && wasPlaying) {  // silence while buffering
            underruns_.fetch_add(1, std::memory_order_relaxed);
            target_ = std::min(target_ + rate_ / 100, rate_ * 120 / 1000);  // +10 ms, at most 120 ms
            reader_.setTarget(target_);
        }
        for (int i = 0; i < chunk; ++i) {
            buf[2 * i] = L[i];
            buf[2 * i + 1] = R[i];
        }
        int e = 0;
        if (pa_simple_write(pa_, buf.data(), buf.size() * sizeof(float), &e) < 0) {
            pa_simple_free(pa_);
            pa_ = nullptr;
            continue;
        }
        if (++k % 20 == 0) {  // every 0.1 s
            const pa_usec_t us = pa_simple_get_latency(pa_, &e);
            if (us != (pa_usec_t)-1) {
                const double f = (double)us * rate_ / 1e6;
                latSm = latSm < 0 ? f : latSm + 0.05 * (f - latSm);
                latency_.store((int)(target_ + latSm), std::memory_order_relaxed);
            }
        }
    }
}

std::vector<PulseSink> PulseOutput::listSinks() {
    std::vector<PulseSink> out;
    std::string text;
    if (runProgram({"env", "LC_ALL=C", "pactl", "list", "sinks"}, &text, 3000) != 0) return out;
    std::istringstream is(text);
    std::string line;
    PulseSink cur;
    bool in = false;
    auto flush = [&] {
        if (in && !cur.name.empty() && cur.name != "guitarstudio_sys") out.push_back(cur);
        cur = PulseSink{};
    };
    while (std::getline(is, line)) {
        if (line.rfind("Sink #", 0) == 0) {
            flush();
            in = true;
            continue;
        }
        const size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const std::string l = line.substr(a);
        if (l.rfind("Name: ", 0) == 0) cur.name = l.substr(6);
        else if (l.rfind("Description: ", 0) == 0) cur.description = l.substr(13);
        else if (l.rfind("device.bus = ", 0) == 0) cur.bluetooth = unquote(l.substr(13)) == "bluetooth";
        else if (l.rfind("alsa.card = ", 0) == 0) cur.alsaCard = std::atoi(unquote(l.substr(12)).c_str());
    }
    flush();
    return out;
}

}  // namespace gs
