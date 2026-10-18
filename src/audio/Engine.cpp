#include "audio/Engine.h"

#include <fcntl.h>
#include <pmmintrin.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <xmmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "analysis/Analyzer.h"
#include "audio/Format.h"
#include "audio/RtKit.h"
#include "common/I18n.h"
#include "practice/PracticeAudio.h"

namespace gs {

namespace {
double nowSec() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
}  // namespace

Engine::Engine() {
    allocBuffers(1024);
    for (auto& s : snaps_) s.seq = 0;
}

Engine::~Engine() {
    stop();
    pulse_.stop();
}

void Engine::allocBuffers(int maxBlock) {
    for (auto* v : {&inL_, &inR_, &outL_, &outR_, &mono_, &dryL_, &dryR_, &guitL_, &guitR_}) v->assign(maxBlock, 0.f);
}

EngineStatus Engine::status() const {
    std::lock_guard<std::mutex> lk(statusMu_);
    EngineStatus s = status_;
    s.running = running_.load();
    return s;
}

float Engine::roundTripMs() const {
    double frames = (double)prefill_.load() + fxLatencySamples();
    return (float)(frames / rate_ * 1000.0 + 0.6);
}

int Engine::fxLatencySamples() const {
    int s = 0;
    for (auto& sl : chain_)
        if (!sl->fx->bypassed.load()) s += sl->fx->latencySamples();
    return s;
}

bool Engine::inputClipped(double withinSec) const {
    uint64_t at = inClipAt_.load(std::memory_order_relaxed);
    return at && (double)(frameClock() - at) < withinSec * rate_;
}

bool Engine::outputLimited(double withinSec) const {
    uint64_t at = limitAt_.load(std::memory_order_relaxed);
    return at && (double)(frameClock() - at) < withinSec * rate_;
}

bool Engine::start(const EngineConfig& cfg) {
    stop();
    cfg_ = cfg;
    EngineStatus st;
    st.device = cfg.alsa.device;
    bool ok = false;
    if (!cfg.simulate) {
        const int card = AlsaDuplex::cardIndexOf(cfg.alsa.device);
        if (cfg.reserveDevice && card >= 0) reservation_.acquire(card, cfg.alsa.device, st.reserveMsg);
        std::string err;
        ok = alsa_.open(cfg.alsa, err);
        if (!ok && reservation_.held()) {
            // PulseAudio may not have closed the device yet; wait a bit and retry
            for (int i = 0; i < 10 && !ok; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ok = alsa_.open(cfg.alsa, err);
            }
        }
        if (ok) {
            rate_ = alsa_.rate();
            period_ = alsa_.period();
            prefill_.store(std::min(alsa_.maxPrefill(), period_ * 6));
            st.format = alsa_.formatDesc();
            std::string m;
            if (card >= 0 && mixer_.open(card, m)) mixer_.setLoopbackMixing(false);
        } else {
            st.error = err;
            reservation_.release();
            if (!cfg.fallbackToSim) {
                std::lock_guard<std::mutex> lk(statusMu_);
                status_ = st;
                return false;
            }
        }
    }
    simulated_ = !ok;
    if (simulated_) {
        rate_ = cfg.alsa.rate;
        period_ = std::clamp(cfg.alsa.period, 16u, 1024u);
        prefill_.store(period_ * 2);
    }
    st.simulated = simulated_;
    st.rate = (unsigned)rate_;
    st.period = period_;
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_ = st;
    }

    allocBuffers((int)std::max(period_, 1024u));
    for (auto& s : chain_) s->fx->prepare(rate_, (int)period_);
    looper_.prepare(rate_, 90.0);
    if (analyzer_) {
        analyzer_->stop();
        analyzer_->configure(rate_);
        analyzer_->resetClock(0);
    }
    if (onPrepare) onPrepare(rate_, (int)period_, outputLatencyFrames());
    if (analyzer_) analyzer_->start();

    frameClock_ = 0;
    clockPub_.store(0);
    xruns_.store(0);
    masterSm_ = 0.f;  // fade in at start
    // Install the current chain directly (the audio thread isn't running yet)
    pendingSnap_.store(nullptr);
    activeSnap_ = nullptr;
    publishChain();
    activeSnap_ = pendingSnap_.exchange(nullptr);
    if (activeSnap_) rtSeq_.store(activeSnap_->seq);

    static bool locked = false;
    if (!locked) locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;  // keep audio memory from being paged out
    // Deep CPU sleep (C2 wakeup ~130 µs) causes occasional xruns: request low wakeup latency when permitted
    // (scripts/setup_realtime.sh grants access)
    if (!simulated_ && dmaLatencyFd_ < 0) {
        dmaLatencyFd_ = ::open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
        if (dmaLatencyFd_ >= 0) {
            int32_t us = 20;
            if (::write(dmaLatencyFd_, &us, sizeof us) != (ssize_t)sizeof us) {
                ::close(dmaLatencyFd_);
                dmaLatencyFd_ = -1;
            }
        }
        std::lock_guard<std::mutex> lk(statusMu_);
        if (dmaLatencyFd_ >= 0) status_.reserveMsg += tr("; CPU low-latency mode on");
    }

    startTime_ = nowSec();
    running_.store(true);
    th_ = std::thread(simulated_ ? &Engine::simMain : &Engine::audioMain, this);
    return !simulated_ || cfg.simulate;
}

void Engine::stop() {
    if (running_.exchange(false) || th_.joinable()) {
        if (th_.joinable()) th_.join();
    }
    alsa_.close();
    reservation_.release();
    if (dmaLatencyFd_ >= 0) {
        ::close(dmaLatencyFd_);
        dmaLatencyFd_ = -1;
    }
    if (analyzer_) analyzer_->stop();
    // The audio thread has stopped, so retired effects can be freed right away
    graves_.clear();
}

void Engine::noteXrun() {
    int k = xruns_.fetch_add(1, std::memory_order_relaxed);
    if (k < 16) xrunAt_[k].store(nowSec() - startTime_, std::memory_order_relaxed);
}

void Engine::audioMain() {
    std::string rtMsg;
    int prio = makeThreadRealtime(cfg_.rtPriority, rtMsg);
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.rtPriority = prio;
        status_.rtMsg = rtMsg;
    }
    _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ + DAZ: avoid slow denormal arithmetic

    // Calibration: run half a second of silence with a large prefill to measure the minimum prefill this card
    // really needs (FIFO + one period + capture readiness jitter)
    const unsigned safety = (unsigned)std::lround(std::max(0.f, cfg_.alsa.safetyMs) * rate_ / 1000.0);
    unsigned prefill = cfg_.alsa.fixedPrefill;
    if (prefill == 0) {
        AlsaCalibration cal = alsa_.calibrate(0.5);
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.calibrated = cal.ok;
        status_.fifoFrames = cal.fifoFrames;
        status_.usedFrames = cal.usedFrames;
        prefill = cal.ok ? cal.usedFrames + safety : period_ * 8;
    }
    prefill = std::clamp(prefill, period_ * 2, alsa_.maxPrefill());
    prefill_.store(prefill);
    if (!alsa_.start(prefill)) {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.error = tr("Failed to start the sound card");
    }
    const unsigned maxGrow = prefill + (unsigned)(3e-3 * rate_);  // xrun adaptation adds at most 3 ms
    const double periodSec = (double)period_ / rate_;
    float avg = 0.f, peak = 0.f;
    while (running_.load(std::memory_order_relaxed)) {
        int r = alsa_.readPeriod(inL_.data(), inR_.data());
        if (r < 0) {
            noteXrun();
            // On an xrun grow the prefill a little (+0.25 ms each); it settles quickly
            prefill = std::min({prefill + std::max(12u, (unsigned)(0.25e-3 * rate_)), maxGrow, alsa_.maxPrefill()});
            prefill_.store(prefill);
            if (!alsa_.start(prefill)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const double t0 = nowSec();
        processBlock(inL_.data(), inR_.data(), outL_.data(), outR_.data(), (int)period_);
        const double t1 = nowSec();
        r = alsa_.writePeriod(outL_.data(), outR_.data());
        if (r < 0) {
            noteXrun();
            prefill = std::min({prefill + std::max(12u, (unsigned)(0.25e-3 * rate_)), maxGrow, alsa_.maxPrefill()});
            prefill_.store(prefill);
            if (!alsa_.start(prefill)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const float load = (float)((t1 - t0) / periodSec);
        avg += 0.01f * (load - avg);
        peak = std::max(load, peak * 0.9995f);
        cpuAvg_.store(avg, std::memory_order_relaxed);
        cpuPeak_.store(peak, std::memory_order_relaxed);
    }
    alsa_.stop();
}

void Engine::simMain() {
    _mm_setcsr(_mm_getcsr() | 0x8040);
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.rtMsg = tr("No-device mode (simulated)");
    }
    timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    const long periodNs = (long)(1e9 * period_ / rate_);
    const double periodSec = (double)period_ / rate_;
    float avg = 0.f, peak = 0.f;
    while (running_.load(std::memory_order_relaxed)) {
        next.tv_nsec += periodNs;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            ++next.tv_sec;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        std::fill(inL_.begin(), inL_.begin() + period_, 0.f);
        std::fill(inR_.begin(), inR_.begin() + period_, 0.f);
        if (simInput) simInput(inL_.data(), inR_.data(), (int)period_, frameClock_);
        const double t0 = nowSec();
        processBlock(inL_.data(), inR_.data(), outL_.data(), outR_.data(), (int)period_);
        const float load = (float)((nowSec() - t0) / periodSec);
        avg += 0.01f * (load - avg);
        peak = std::max(load, peak * 0.9995f);
        cpuAvg_.store(avg, std::memory_order_relaxed);
        cpuPeak_.store(peak, std::memory_order_relaxed);
    }
}

void Engine::processBlock(const float* inL, const float* inR, float* L, float* R, int n) {
    if (ChainSnap* p = pendingSnap_.exchange(nullptr, std::memory_order_acq_rel)) {
        activeSnap_ = p;
        rtSeq_.store(p->seq, std::memory_order_release);
    }
    const float fs = (float)rate_;

    // ---- 1. Input: channel selection + DC blocker + gain + meters ----
    double pl = 0, pr = 0;
    float rawPeak = 0.f;
    for (int i = 0; i < n; ++i) {
        pl += (double)inL[i] * inL[i];
        pr += (double)inR[i] * inR[i];
        rawPeak = std::max(rawPeak, std::max(std::fabs(inL[i]), std::fabs(inR[i])));
    }
    // Update channel energy only while there is signal (keep the last decision during silence, so the level
    // doesn't jump every time playing starts)
    if (std::max(pl, pr) / n > 3e-7) {
        const double a = std::exp(-(double)n / (0.4 * rate_));
        powL_ = powL_ * a + (pl / n) * (1 - a);
        powR_ = powR_ * a + (pr / n) * (1 - a);
    }
    float wL;
    switch ((InputChannel)inputChannel.load(std::memory_order_relaxed)) {
        case InputChannel::Left: wL = 1.f; break;
        case InputChannel::Right: wL = 0.f; break;
        case InputChannel::Mix: wL = 0.5f; break;
        default: {  // auto: weight by channel energy (a mono TS plug carries signal on one channel only)
            double t = powL_ / (powL_ + powR_ + 1e-12);
            wL = (float)t;
            break;
        }
    }
    wL_ += 0.02f * (wL - wL_);
    const float wR = 1.f - wL_;
    const float gTarget = dbToLin(inputGainDb.load(std::memory_order_relaxed));
    float g = inGainSm_;
    const float gk = 1.f - std::exp(-1.f / (0.02f * fs));
    float pk = 0.f;
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        float x = wL_ * inL[i] + wR * inR[i];
        float y = x - dcX1_ + 0.9995f * dcY1_;  // DC blocker (~4 Hz)
        dcX1_ = x;
        dcY1_ = y;
        g += gk * (gTarget - g);
        y *= g;
        mono_[i] = y;
        pk = std::max(pk, std::fabs(y));
        sum += (double)y * y;
    }
    if (std::fabs(dcY1_) < 1e-20f) dcY1_ = 0.f;
    inGainSm_ = g;
    inPeak_.store(linToDb(pk), std::memory_order_relaxed);
    inRms_.store(linToDb((float)std::sqrt(sum / n)), std::memory_order_relaxed);
    inLdb_.store(10.f * std::log10((float)powL_ + 1e-12f), std::memory_order_relaxed);
    inRdb_.store(10.f * std::log10((float)powR_ + 1e-12f), std::memory_order_relaxed);
    wLpub_.store(wL_, std::memory_order_relaxed);
    if (rawPeak > 0.99f || pk > 0.99f) inClipAt_.store(frameClock_ + 1, std::memory_order_relaxed);

    // ---- 2. Analysis (dry signal) ----
    if (analyzer_) analyzer_->push(mono_.data(), n);

    // ---- 3. Effect chain ----
    std::copy(mono_.begin(), mono_.begin() + n, guitL_.begin());
    std::copy(mono_.begin(), mono_.begin() + n, guitR_.begin());
    float* gl = guitL_.data();
    float* gr = guitR_.data();
    if (ChainSnap* snap = activeSnap_) {
        const float step = 1.f / (0.012f * fs);
        for (int k = 0; k < snap->count; ++k) {
            FxSlot* s = snap->slots[k];
            Effect* fx = s->fx.get();
            const bool on = !fx->bypassed.load(std::memory_order_relaxed);
            const float target = on ? 1.f : 0.f;
            if (s->fade == target) {
                if (on) fx->process(gl, gr, n);
                continue;
            }
            if (s->fade == 0.f && on) fx->reset();  // leaving bypass: clear stale state
            std::copy(gl, gl + n, dryL_.begin());
            std::copy(gr, gr + n, dryR_.begin());
            fx->process(gl, gr, n);
            float f = s->fade;
            for (int i = 0; i < n; ++i) {
                f = on ? std::min(1.f, f + step) : std::max(0.f, f - step);
                gl[i] = dryL_[i] + f * (gl[i] - dryL_[i]);
                gr[i] = dryR_[i] + f * (gr[i] - dryR_[i]);
            }
            s->fade = f;
        }
    }
    // Guitar level (smoothed) + mute
    const float gtTarget = guitarMute.load(std::memory_order_relaxed) ? 0.f : dbToLin(guitarDb.load(std::memory_order_relaxed));
    float gt = guitarSm_;
    for (int i = 0; i < n; ++i) {
        gt += gk * (gtTarget - gt);
        gl[i] *= gt;
        gr[i] *= gt;
        if (!std::isfinite(gl[i]) || !std::isfinite(gr[i])) gl[i] = gr[i] = 0.f;  // safety net
    }
    guitarSm_ = gt;

    // ---- 4. Mix: guitar + looper + practice (metronome/reference) + system audio ----
    std::copy(gl, gl + n, L);
    std::copy(gr, gr + n, R);
    looper_.process(gl, gr, L, R, n);
    if (practice_) practice_->process(L, R, n, frameClock_);
    pulse_.mixInto(L, R, n, dbToLin(systemAudioDb.load(std::memory_order_relaxed)));

    // ---- 5. Master volume + safety limiter + meters ----
    const float mTarget = mute.load(std::memory_order_relaxed) ? 0.f : dbToLin(masterDb.load(std::memory_order_relaxed));
    float m = masterSm_;
    const bool lim = limiterOn.load(std::memory_order_relaxed);
    const float thr = 0.89f;  // -1 dBFS
    const float rel = 1.f - std::exp(-1.f / (0.15f * fs));
    float lg = limGain_, opk = 0.f;
    bool limited = false;
    for (int i = 0; i < n; ++i) {
        m += gk * (mTarget - m);
        float l = L[i] * m, r = R[i] * m;
        if (lim) {
            float p = std::max(std::fabs(l), std::fabs(r));
            float want = p > thr ? thr / p : 1.f;
            if (want < lg) lg = want;
            else lg += rel * (1.f - lg);
            if (lg < 0.999f) limited = true;
            l *= lg;
            r *= lg;
        }
        L[i] = l;
        R[i] = r;
        opk = std::max(opk, std::max(std::fabs(l), std::fabs(r)));
    }
    masterSm_ = m;
    limGain_ = lg;
    if (limited) limitAt_.store(frameClock_ + 1, std::memory_order_relaxed);
    outPeak_.store(linToDb(opk), std::memory_order_relaxed);
    frameClock_ += (uint64_t)n;
    clockPub_.store(frameClock_, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- effect chain management (UI thread)

void Engine::publishChain() {
    // Find a free snapshot: any with a sequence number below the one the audio thread uses can be reused
    const uint64_t inUse = rtSeq_.load(std::memory_order_acquire);
    ChainSnap* snap = nullptr;
    for (auto& s : snaps_)
        if (s.seq < inUse || s.seq == 0) {
            snap = &s;
            break;
        }
    if (!snap) {  // audio thread stalled? wait a moment
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        snap = &snaps_[(pubSeq_ + 1) % 64];
    }
    snap->count = 0;
    for (auto& s : chain_)
        if (snap->count < kMaxFx) snap->slots[snap->count++] = s.get();
    snap->seq = ++pubSeq_;
    ChainSnap* old = pendingSnap_.exchange(snap, std::memory_order_acq_rel);
    (void)old;  // a snapshot the audio thread never picked up has a lower sequence and becomes reusable
}

FxSlot* Engine::addEffect(const std::string& typeId, int index) {
    if ((int)chain_.size() >= kMaxFx) return nullptr;
    auto fx = createEffect(typeId);
    if (!fx) return nullptr;
    fx->prepare(rate_, (int)std::max(period_, 16u));
    auto slot = std::make_unique<FxSlot>();
    slot->fx = std::move(fx);
    slot->id = nextId_++;
    slot->fade = 0.f;  // fade in
    FxSlot* raw = slot.get();
    if (index < 0 || index > (int)chain_.size()) index = (int)chain_.size();
    chain_.insert(chain_.begin() + index, std::move(slot));
    publishChain();
    return raw;
}

void Engine::removeEffect(int index) {
    if (index < 0 || index >= (int)chain_.size()) return;
    graves_.push_back(Grave{std::move(chain_[index]), 0});
    chain_.erase(chain_.begin() + index);
    publishChain();
    graves_.back().seq = pubSeq_;
}

void Engine::moveEffect(int from, int to) {
    if (from < 0 || from >= (int)chain_.size() || to < 0 || to >= (int)chain_.size() || from == to) return;
    auto s = std::move(chain_[from]);
    chain_.erase(chain_.begin() + from);
    chain_.insert(chain_.begin() + to, std::move(s));
    publishChain();
}

void Engine::clearChain() {
    for (auto& s : chain_) graves_.push_back(Grave{std::move(s), 0});
    chain_.clear();
    publishChain();
    for (auto& g : graves_)
        if (g.seq == 0) g.seq = pubSeq_;
}

void Engine::collectGarbage() {
    const uint64_t inUse = rtSeq_.load(std::memory_order_acquire);
    const bool rt = running_.load();
    graves_.erase(std::remove_if(graves_.begin(), graves_.end(),
                                 [&](const Grave& g) { return !rt || (g.seq != 0 && inUse >= g.seq); }),
                  graves_.end());
}

bool Engine::loadPreset(const PresetData& p, std::string* warnings) {
    clearChain();
    for (const auto& st : p.slots) {
        FxSlot* s = addEffect(st.typeId);
        if (!s) {
            if (warnings) *warnings += strf(tr("Unknown effect: %s"), st.typeId.c_str()) + "\n";
            continue;
        }
        for (const auto& kv : st.params)
            if (!s->fx->setParam(kv.first, kv.second) && warnings)
                *warnings += strf(tr("%s has no parameter %s"), st.typeId.c_str(), kv.first.c_str()) + "\n";
        s->fx->bypassed.store(st.bypass);
        if (!st.file.empty() && s->fx->acceptsFile()) {
            std::string err;
            if (!s->fx->loadFile(st.file, err) && warnings) *warnings += strf(tr("Failed to load IR: %s"), err.c_str()) + "\n";
        }
    }
    return true;
}

PresetData Engine::currentPreset(const std::string& name) const {
    PresetData p;
    p.name = name;
    for (const auto& s : chain_) {
        FxSlotState st;
        st.typeId = s->fx->typeId();
        st.bypass = s->fx->bypassed.load();
        for (int i = 0; i < s->fx->numParams(); ++i) st.params.emplace_back(s->fx->paramInfo(i).id, s->fx->param(i));
        st.file = s->fx->filePath();
        p.slots.push_back(std::move(st));
    }
    return p;
}

}  // namespace gs
