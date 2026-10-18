#pragma once
// Loop recorder (like a looper pedal): record -> play -> overdub -> play ...; stop / clear.
// Records the guitar after the effect chain. Buffers are allocated once in prepare().
#include <atomic>
#include <cstddef>
#include <vector>

namespace gs {

class Looper {
public:
    enum State { Empty = 0, Recording, Playing, Overdubbing, Stopped };
    void prepare(double fs, double maxSeconds);
    void process(const float* inL, const float* inR, float* L, float* R, int n);  // audio thread: adds loop playback to L/R

    // UI commands
    void pressMain() { cmd_.store(1); }   // cycles record / play / overdub
    void pressStop() { cmd_.store(2); }
    void pressClear() { cmd_.store(3); }

    State state() const { return (State)state_pub_.load(std::memory_order_relaxed); }
    float lengthSec() const { return len_pub_.load(std::memory_order_relaxed); }
    float position() const { return pos_pub_.load(std::memory_order_relaxed); }  // 0..1
    std::atomic<float> level{1.f};

private:
    double fs_ = 48000;
    std::vector<float> bufL_, bufR_;
    size_t cap_ = 0, len_ = 0, pos_ = 0;
    State state_ = Empty;
    float fade_ = 0;
    std::atomic<int> cmd_{0};
    std::atomic<int> state_pub_{0};
    std::atomic<float> len_pub_{0}, pos_pub_{0};
};

}  // namespace gs
