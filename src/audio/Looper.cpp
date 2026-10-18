#include "audio/Looper.h"

#include <algorithm>
#include <cstring>

namespace gs {

void Looper::prepare(double fs, double maxSeconds) {
    fs_ = fs;
    cap_ = (size_t)(fs * maxSeconds);
    bufL_.assign(cap_, 0.f);
    bufR_.assign(cap_, 0.f);
    len_ = pos_ = 0;
    state_ = Empty;
    state_pub_.store(Empty);
    len_pub_.store(0);
    pos_pub_.store(0);
}

void Looper::process(const float* inL, const float* inR, float* L, float* R, int n) {
    if (cap_ == 0) return;
    switch (cmd_.exchange(0, std::memory_order_acq_rel)) {
        case 1:
            if (state_ == Empty) {
                state_ = Recording;
                len_ = pos_ = 0;
            } else if (state_ == Recording) {
                len_ = std::max<size_t>(pos_, 1);
                pos_ = 0;
                state_ = Playing;
            } else if (state_ == Playing) {
                state_ = Overdubbing;
            } else if (state_ == Overdubbing) {
                state_ = Playing;
            } else if (state_ == Stopped) {
                pos_ = 0;
                state_ = Playing;
            }
            break;
        case 2:
            if (state_ == Recording) {
                len_ = std::max<size_t>(pos_, 1);
                pos_ = 0;
            }
            if (state_ != Empty) state_ = Stopped;
            break;
        case 3:
            state_ = Empty;
            len_ = pos_ = 0;
            break;
        default: break;
    }
    const float lvl = level.load(std::memory_order_relaxed);
    const float fstep = 1.f / (0.01f * (float)fs_);
    for (int i = 0; i < n; ++i) {
        if (state_ == Recording) {
            if (pos_ >= cap_) {  // buffer full: start looping automatically
                len_ = cap_;
                pos_ = 0;
                state_ = Playing;
            } else {
                bufL_[pos_] = inL[i];
                bufR_[pos_] = inR[i];
                ++pos_;
                continue;
            }
        }
        const bool playing = state_ == Playing || state_ == Overdubbing;
        fade_ = playing ? std::min(1.f, fade_ + fstep) : std::max(0.f, fade_ - fstep);
        if (len_ == 0 || (fade_ <= 0.f && !playing)) continue;
        if (pos_ >= len_) pos_ = 0;
        L[i] += bufL_[pos_] * lvl * fade_;
        R[i] += bufR_[pos_] * lvl * fade_;
        if (state_ == Overdubbing) {
            bufL_[pos_] = bufL_[pos_] * 0.98f + inL[i];  // fade old layers slightly so overdubs don't keep growing
            bufR_[pos_] = bufR_[pos_] * 0.98f + inR[i];
        }
        if (playing && ++pos_ >= len_) pos_ = 0;
    }
    state_pub_.store(state_, std::memory_order_relaxed);
    len_pub_.store((float)((state_ == Recording ? pos_ : len_) / fs_), std::memory_order_relaxed);
    pos_pub_.store(len_ ? (float)pos_ / (float)len_ : 0.f, std::memory_order_relaxed);
}

}  // namespace gs
