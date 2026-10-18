#include "analysis/Analyzer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gs {

namespace {
int nextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}
inline float todB(float lin) { return 20.f * std::log10(lin + 1e-9f); }

// Onset detection parameters (tuned with the synthetic plucks in test_analysis)
constexpr float kFluxGamma = 3162.f;   // log compression: a -70 dBFS component maps to ~1
constexpr float kOnsetAbs = 0.12f;     // absolute flux threshold
constexpr float kOnsetRel = 1.6f;      // multiple of the recent mean
constexpr float kMinIoiSec = 0.06f;    // minimum interval between onsets
constexpr float kMpmK = 0.90f;         // McLeod key-maximum threshold
}  // namespace

Analyzer::Analyzer() {
    ring_.init(1u << 18);
    configure(48000);
}

void Analyzer::configure(double fs) {
    fs_ = fs;
    const double s = fs / 48000.0;
    hop_ = std::max(64, (int)std::lround(256 * s));
    W_ = nextPow2((int)(2048 * s * 0.999));
    Nf_ = nextPow2((int)(1024 * s * 0.999));
    Nc_ = nextPow2((int)(4096 * s * 0.999));
    int hs = nextPow2(Nc_ + 4 * hop_ + 4096);
    hist_.assign(hs, 0.f);
    histMask_ = (uint64_t)hs - 1;
    fftW2_.init(2 * W_);
    fftNf_.init(Nf_);
    fftNc_.init(Nc_);
    size_t big = (size_t)std::max(2 * W_, Nc_);
    re_.assign(big, 0.f);
    im_.assign(big, 0.f);
    tmp_.assign(big, 0.f);
    mag_.assign(Nc_ / 2 + 2, 0.f);
    win_.assign(Nc_, 0.f);
    hannNf_.resize(Nf_);
    hannNc_.resize(Nc_);
    for (int i = 0; i < Nf_; ++i) hannNf_[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * i / Nf_);
    for (int i = 0; i < Nc_; ++i) hannNc_[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * i / Nc_);
    nsdf_.assign(W_, 0.f);
    prevLogMag_.assign(Nf_ / 2 + 1, 0.f);
    odfHist_.assign(16, 0.f);
    pitchHist_.assign(8192, PitchPoint{0, -1.f, 0.f, -120.f, false});
    resetClock(0);
}

void Analyzer::resetClock(uint64_t startFrame) {
    std::fill(hist_.begin(), hist_.end(), 0.f);
    absPos_ = startFrame;
    nextHopEnd_ = startFrame + hop_;
    droppedSeen_ = dropped_.load();
    ring_.skip(ring_.readAvailable());
    std::fill(prevLogMag_.begin(), prevLogMag_.end(), 0.f);
    std::fill(odfHist_.begin(), odfHist_.end(), 0.f);
    odfPrev1_ = odfPrev2_ = 0;
    odfMax_ = 1e-3f;
    lastOnset_ = 0;
    haveOnset_ = false;
    havePrevMag_ = false;
    noiseFloor_ = -80.f;
    warmupUntil_ = startFrame + (uint64_t)Nc_ + (uint64_t)hop_;
    tunerN_ = tunerI_ = 0;
    tunerHz_.store(0);
    latest_.store(startFrame);
    std::lock_guard<std::mutex> lk(histMu_);
    pitchHistPos_ = pitchHistCount_ = 0;
}

void Analyzer::start() {
    if (run_.exchange(true)) return;
    th_ = std::thread(&Analyzer::threadMain, this);
}

void Analyzer::stop() {
    run_.store(false);
    if (th_.joinable()) th_.join();
}

void Analyzer::threadMain() {
    std::vector<float> buf(4096);
    while (run_.load(std::memory_order_relaxed)) {
        size_t avail = ring_.readAvailable();
        if (avail < (size_t)hop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        uint64_t d = dropped_.load(std::memory_order_relaxed);
        if (d != droppedSeen_) {  // ring overflow (rare): advance the clock past the lost samples
            absPos_ += d - droppedSeen_;
            while (nextHopEnd_ + hop_ <= absPos_) nextHopEnd_ += hop_;
            droppedSeen_ = d;
        }
        size_t n = ring_.read(buf.data(), std::min(avail, buf.size()));
        ingest(buf.data(), n);
    }
}

void Analyzer::processOffline(const float* x, size_t n) {
    while (n > 0) {
        size_t k = std::min<size_t>(n, 4096);
        ingest(x, k);
        x += k;
        n -= k;
    }
}

void Analyzer::ingest(const float* x, size_t n) {
    for (size_t i = 0; i < n; ++i) hist_[(absPos_ + i) & histMask_] = x[i];
    absPos_ += n;
    while (nextHopEnd_ <= absPos_) {
        processHop();
        nextHopEnd_ += hop_;
    }
}

void Analyzer::copyLast(float* dst, int n, uint64_t endAbs) const {
    uint64_t start = endAbs - (uint64_t)n;
    for (int i = 0; i < n; ++i) dst[i] = hist_[(start + i) & histMask_];
}

float Analyzer::onsetFlux(const float* x) {
    const int N = Nf_;
    double mean = 0;
    for (int i = 0; i < N; ++i) mean += x[i];
    mean /= N;
    for (int i = 0; i < N; ++i) {
        re_[i] = (x[i] - (float)mean) * hannNf_[i];
        im_[i] = 0.f;
    }
    fftNf_.transform(re_.data(), im_.data(), false);
    const float norm = 4.f / N;  // full-scale sine -> magnitude ~1
    const int k0 = std::max(1, (int)(60.0 * N / fs_));
    const int k1 = std::min(N / 2, (int)(8000.0 * N / fs_));
    float flux = 0.f;
    for (int k = k0; k <= k1; ++k) {
        float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]) * norm;
        float lm = std::log1p(kFluxGamma * mag);
        float d = lm - prevLogMag_[k];
        if (d > 0 && havePrevMag_) flux += d;
        prevLogMag_[k] = lm;
    }
    havePrevMag_ = true;
    return flux / (float)(k1 - k0 + 1);
}

uint64_t Analyzer::refineOnset(uint64_t searchStart, uint64_t searchEnd) {
    const int blk = std::max(32, hop_ / 4);
    if (searchEnd <= searchStart + 2 * (uint64_t)blk) return searchStart;
    int nb = (int)((searchEnd - searchStart) / blk);
    nb = std::min(nb, 64);
    float e[66];
    float emax = 0.f;
    for (int b = 0; b <= nb; ++b) {
        uint64_t s0 = searchStart + (uint64_t)(b - 1) * blk;  // b=0 is the preceding block, used as reference
        double acc = 0;
        for (int i = 0; i < blk; ++i) {
            float v = hist_[(s0 + i) & histMask_];
            acc += v * v;
        }
        e[b] = (float)(acc / blk) + 1e-12f;
        emax = std::max(emax, e[b]);
    }
    int best = 1;
    float bestScore = -1e9f;
    for (int b = 1; b <= nb; ++b) {
        if (e[b] < 0.03f * emax) continue;
        float sc = std::log(e[b] / e[b - 1]);
        if (sc > bestScore) {
            bestScore = sc;
            best = b;
        }
    }
    return searchStart + (uint64_t)(best - 1) * blk + blk / 2;
}

float Analyzer::mpm(const float* x, int W, float& clarity) {
    clarity = 0.f;
    const int N = 2 * W;
    double mean = 0;
    for (int i = 0; i < W; ++i) mean += x[i];
    mean /= W;
    double m = 0;
    for (int i = 0; i < W; ++i) {
        float v = x[i] - (float)mean;
        tmp_[i] = v;
        re_[i] = v;
        im_[i] = 0.f;
        m += (double)v * v;
    }
    for (int i = W; i < N; ++i) re_[i] = im_[i] = 0.f;
    if (m < 1e-12) return 0.f;
    fftW2_.transform(re_.data(), im_.data(), false);
    for (int i = 0; i < N; ++i) {
        re_[i] = re_[i] * re_[i] + im_[i] * im_[i];
        im_[i] = 0.f;
    }
    fftW2_.transform(re_.data(), im_.data(), true);  // re_[tau] = autocorrelation r(tau)

    const int tauMin = std::max(2, (int)(fs_ / 1500.0));
    const int tauMax = std::min(W - 2, (int)(fs_ / 60.0));
    m *= 2.0;  // m(0) = 2 Σ x²
    nsdf_[0] = 1.f;
    for (int tau = 1; tau <= tauMax + 1; ++tau) {
        m -= (double)tmp_[tau - 1] * tmp_[tau - 1] + (double)tmp_[W - tau] * tmp_[W - tau];
        nsdf_[tau] = m > 1e-12 ? (float)(2.0 * re_[tau] / m) : 0.f;
    }
    // Key maxima: the maximum of each positive region
    int peaks[96];
    int np = 0;
    int pos = 0;
    while (pos < tauMax && nsdf_[pos] > 0) ++pos;  // skip the positive region around tau = 0
    while (pos < tauMax && np < 96) {
        while (pos < tauMax && nsdf_[pos] <= 0) ++pos;
        if (pos >= tauMax) break;
        int best = pos;
        while (pos < tauMax && nsdf_[pos] > 0) {
            if (nsdf_[pos] > nsdf_[best]) best = pos;
            ++pos;
        }
        if (best >= tauMin && best < tauMax) peaks[np++] = best;
    }
    if (np == 0) return 0.f;
    float highest = 0.f;
    for (int i = 0; i < np; ++i) highest = std::max(highest, nsdf_[peaks[i]]);
    if (highest < 0.45f) {
        clarity = highest;
        return 0.f;
    }
    const float thr = kMpmK * highest;
    int t = peaks[0];
    for (int i = 0; i < np; ++i)
        if (nsdf_[peaks[i]] >= thr) {
            t = peaks[i];
            break;
        }
    float a = nsdf_[t - 1], b = nsdf_[t], c = nsdf_[t + 1];
    float den = a - 2.f * b + c;
    float delta = std::fabs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.f;
    delta = std::clamp(delta, -0.5f, 0.5f);
    clarity = std::min(1.f, b - 0.25f * (a - c) * delta);
    return (float)(fs_ / (t + delta));
}

void Analyzer::computeSpectrum(const float* x) {
    const int N = Nc_;
    double mean = 0;
    for (int i = 0; i < N; ++i) mean += x[i];
    mean /= N;
    for (int i = 0; i < N; ++i) {
        re_[i] = (x[i] - (float)mean) * hannNc_[i];
        im_[i] = 0.f;
    }
    fftNc_.transform(re_.data(), im_.data(), false);
    const float norm = 4.f / N;
    for (int k = 0; k <= N / 2; ++k) mag_[k] = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]) * norm;
}

void Analyzer::computeChroma(float* chroma) {
    const int N = Nc_;
    const int k0 = std::max(2, (int)(70.0 * N / fs_));
    const int k1 = std::min(N / 2 - 2, (int)(3000.0 * N / fs_));
    float maxMag = 1e-9f;
    for (int k = k0 - 1; k <= k1 + 1; ++k) maxMag = std::max(maxMag, mag_[k]);
    for (int i = 0; i < 12; ++i) chroma[i] = 0.f;
    // Spectral peaks + parabolic interpolation give the true frequency, so low notes map to the right pitch class
    for (int k = k0; k <= k1; ++k) {
        float mk = mag_[k];
        if (mk < 0.01f * maxMag || mk < mag_[k - 1] || mk < mag_[k + 1]) continue;
        float la = std::log(mag_[k - 1] + 1e-12f), lb = std::log(mk + 1e-12f), lc = std::log(mag_[k + 1] + 1e-12f);
        float den = la - 2.f * lb + lc;
        float p = std::fabs(den) > 1e-9f ? 0.5f * (la - lc) / den : 0.f;
        p = std::clamp(p, -0.5f, 0.5f);
        float f = (k + p) * (float)fs_ / N;
        float midi = 69.f + 12.f * std::log2(f / 440.f);
        float nearest = std::round(midi);
        float d = midi - nearest;
        float w = std::exp(-d * d / (2.f * 0.15f * 0.15f));
        int pc = ((int)nearest % 12 + 12) % 12;
        chroma[pc] += w * std::log1p(100.f * mk / maxMag);
    }
    float s = 0.f;
    for (int i = 0; i < 12; ++i) s += chroma[i] * chroma[i];
    s = std::sqrt(s);
    if (s > 1e-9f)
        for (int i = 0; i < 12; ++i) chroma[i] /= s;
}

void Analyzer::computeSalience(float* sal) {
    // Harmonic sum: S(p) = sum_h h^-0.5 * sqrt(|X(h*f0)|), using the larger of the two bins around h*f0
    const int N = Nc_;
    const float binHz = (float)fs_ / N;
    for (int i = 0; i < AnalysisFrame::kSalN; ++i) {
        const float f0 = midiToHz((float)(AnalysisFrame::kSalLo + i));
        float acc = 0.f;
        for (int h = 1; h <= 8; ++h) {
            const float f = f0 * h;
            if (f > 7500.f) break;
            const float kf = f / binHz;
            const int k = (int)kf;
            if (k + 1 > N / 2) break;
            const float m = std::max(mag_[k], mag_[k + 1]);
            acc += std::sqrt(m) / std::sqrt((float)h);
        }
        sal[i] = acc;
    }
}

void Analyzer::processHop() {
    const uint64_t end = nextHopEnd_;
    const float hopSec = (float)(hop_ / fs_);
    AnalysisFrame fr;
    fr.hopEnd = end;
    copyLast(win_.data(), Nc_, end);
    const float* xW = win_.data() + (Nc_ - W_);
    const float* xNf = win_.data() + (Nc_ - Nf_);
    const float* xHop = win_.data() + (Nc_ - hop_);

    double e = 0;
    for (int i = 0; i < hop_; ++i) e += (double)xHop[i] * xHop[i];
    const float rms = (float)std::sqrt(e / hop_);
    const float rmsDb = todB(rms);
    fr.rmsDb = rmsDb;
    // Noise floor: fast fall, slow rise (~1.5 dB/s), starting low (minimum tracking)
    if (rmsDb < noiseFloor_) noiseFloor_ += 0.3f * (rmsDb - noiseFloor_);
    else noiseFloor_ += 1.5f * hopSec;
    noiseFloor_ = std::clamp(noiseFloor_, -110.f, -25.f);
    fr.noiseDb = noiseFloor_;
    const float gateDb = std::max(noiseFloor_ + 9.f, -78.f);

    // ---- Onsets (the candidate is the previous hop; compare with this hop to confirm a peak) ----
    const float odf = onsetFlux(xNf);
    float meanOdf = 0.f;
    for (float v : odfHist_) meanOdf += v;
    meanOdf /= (float)odfHist_.size();
    const float cand = odfPrev1_;
    const bool isPeak = cand > odfPrev2_ && cand >= odf;
    const float thr = std::max(kOnsetAbs, kOnsetRel * meanOdf + 0.05f);
    const uint64_t candEnd = end - (uint64_t)hop_;
    const uint64_t minIoi = (uint64_t)(kMinIoiSec * fs_);
    if (end >= warmupUntil_ + (uint64_t)hop_ && isPeak && cand > thr && rmsDb > gateDb &&
        (!haveOnset_ || candEnd > lastOnset_ + minIoi)) {
        uint64_t s0 = candEnd > (uint64_t)(Nf_ * 3 / 4) ? candEnd - (uint64_t)(Nf_ * 3 / 4) : 0;
        if (haveOnset_ && s0 < lastOnset_ + minIoi) s0 = lastOnset_ + minIoi;
        uint64_t on = refineOnset(s0, end);
        if (!haveOnset_ || on > lastOnset_ + minIoi) {
            haveOnset_ = true;
            fr.onset = true;
            fr.onsetFrame = on;
            fr.onsetStrength = cand;
            lastOnset_ = on;
        }
    }
    for (size_t i = odfHist_.size() - 1; i > 0; --i) odfHist_[i] = odfHist_[i - 1];
    odfHist_[0] = cand;
    odfPrev2_ = odfPrev1_;
    odfPrev1_ = odf;

    // ---- Pitch ----
    fr.frame = end - (uint64_t)(W_ / 2);
    if (rmsDb > gateDb) {
        float clarity = 0.f;
        float f0 = mpm(xW, W_, clarity);
        fr.clarity = clarity;
        if (f0 > 0.f && clarity > 0.5f) {
            fr.f0 = f0;
            fr.midi = hzToMidi(f0);
        }
        computeSpectrum(win_.data());
        computeChroma(fr.chroma);
        computeSalience(fr.salience);
    }

    // ---- Tuner (median smoothing) ----
    if (fr.f0 > 0 && fr.clarity > 0.85f) {
        tunerBuf_[tunerI_] = fr.f0;
        tunerI_ = (tunerI_ + 1) % 7;
        tunerN_ = std::min(tunerN_ + 1, 7);
        float tmp[7];
        std::copy(tunerBuf_, tunerBuf_ + tunerN_, tmp);
        std::sort(tmp, tmp + tunerN_);
        tunerHz_.store(tmp[tunerN_ / 2], std::memory_order_relaxed);
        tunerClarity_.store(fr.clarity, std::memory_order_relaxed);
    } else if (rmsDb < gateDb) {
        tunerN_ = 0;
        tunerHz_.store(0.f, std::memory_order_relaxed);
        tunerClarity_.store(0.f, std::memory_order_relaxed);
    }
    levelDb_.store(rmsDb, std::memory_order_relaxed);
    noiseDb_.store(noiseFloor_, std::memory_order_relaxed);
    curMidi_.store(fr.midi, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(histMu_);
        pitchHist_[pitchHistPos_] = PitchPoint{fr.frame, fr.midi, fr.clarity, rmsDb, fr.onset};
        pitchHistPos_ = (pitchHistPos_ + 1) % pitchHist_.size();
        pitchHistCount_ = std::min(pitchHistCount_ + 1, pitchHist_.size());
    }
    latest_.store(end, std::memory_order_release);
    if (cb_) cb_(fr);
}

void Analyzer::copyHistory(uint64_t sinceFrame, std::vector<PitchPoint>& out) const {
    out.clear();
    std::lock_guard<std::mutex> lk(histMu_);
    const size_t n = pitchHistCount_, cap = pitchHist_.size();
    for (size_t i = 0; i < n; ++i) {
        const PitchPoint& p = pitchHist_[(pitchHistPos_ + cap - n + i) % cap];
        if (p.frame >= sinceFrame) out.push_back(p);
    }
}

}  // namespace gs
