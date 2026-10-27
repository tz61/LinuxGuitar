#include "common/DriftReader.h"

#include <algorithm>
#include <cmath>

namespace gs {

DriftReader::DriftReader(SpscRing<float>& ring, size_t maxFrames) : ring_(ring) { scratch_.assign(maxFrames * 2 + 32, 0.f); }

void DriftReader::reset(unsigned rate, unsigned targetFrames) {
    ring_.skip(ring_.readAvailable());
    rate_ = std::max(1u, rate);
    target_ = std::max(1u, targetFrames);
    primed_ = false;
    pos_ = 0;
    ratio_ = 1;
    errAvg_ = integ_ = 0;
    prevL_ = prevR_ = curL_ = curR_ = 0;
}

bool DriftReader::read(float* L, float* R, int n) {
    const size_t availFrames = ring_.readAvailable() / 2;
    if (!primed_) {
        if (availFrames < target_) {
            std::fill(L, L + n, 0.f);
            std::fill(R, R + n, 0.f);
            return false;
        }
        primed_ = true;
        errAvg_ = (double)availFrames - target_;
    }
    // Buffer level -> resampling ratio, a PI loop in frames and seconds (so it behaves the same whether it is
    // called with 16 or 500 frames). Clock drift is a few hundred ppm at most and changes slowly, so the loop is
    // slow and critically damped (natural frequency ~0.05 Hz): the pitch never audibly wobbles. The level is
    // averaged over 0.5 s first, because the other side reads or writes in bursts.
    const double dt = n / rate_;
    errAvg_ += (1.0 - std::exp(-dt / 0.5)) * (((double)availFrames - target_) - errAvg_);
    const double kP = 0.63, kI = 0.1, lim = 0.005 * rate_;  // ratio offset = (kP * error + integral) / rate
    integ_ = std::clamp(integ_ + kI * errAvg_ * dt, -lim, lim);
    ratio_ = 1.0 + std::clamp((kP * errAvg_ + integ_) / rate_, -0.005, 0.005);

    const size_t need = std::min((size_t)std::ceil(n * ratio_ + pos_) + 2, scratch_.size() / 2);
    const size_t got = ring_.peek(scratch_.data(), need * 2) / 2;
    size_t used = 0;
    for (int i = 0; i < n; ++i) {
        pos_ += ratio_;
        while (pos_ >= 1.0) {
            pos_ -= 1.0;
            if (used >= got) {  // underrun: silence for the rest, then re-buffer
                std::fill(L + i, L + n, 0.f);
                std::fill(R + i, R + n, 0.f);
                primed_ = false;
                ring_.skip(used * 2);
                return false;
            }
            prevL_ = curL_;
            prevR_ = curR_;
            curL_ = scratch_[used * 2];
            curR_ = scratch_[used * 2 + 1];
            ++used;
        }
        const float f = (float)pos_;
        L[i] = prevL_ + (curL_ - prevL_) * f;
        R[i] = prevR_ + (curR_ - prevR_) * f;
    }
    ring_.skip(used * 2);
    return true;
}

}  // namespace gs
