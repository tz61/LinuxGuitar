#include "practice/Practice.h"

#include <algorithm>
#include <cmath>

namespace gs {

const char* judgeName(Judge j) {
    switch (j) {
        case Judge::Perfect: return "Perfect";
        case Judge::Good: return "Good";
        case Judge::Ok: return "OK";
        case Judge::WrongPitch: return "Wrong note";
        case Judge::Miss: return "Missed";
        default: return "";
    }
}

static bool isHit(Judge j) { return j == Judge::Perfect || j == Judge::Good || j == Judge::Ok; }

Practice::Practice(Analyzer& an) : an_(an) {}

void Practice::prepare(double fs, int outputLatencyFrames) {
    std::lock_guard<std::mutex> lk(mu_);
    fs_ = fs;
    outLatency_.store(outputLatencyFrames);
    audio_.prepare(fs, 1024);
    // The frame clock restarts at 0: drop old analysis data
    onsets_.clear();
    pframes_.clear();
    pending_.clear();
    closing_.clear();
    waitDone_ = true;
    waitEv_ = -1;
    runFresh_ = true;
}

void Practice::loadSong(std::shared_ptr<const Song> song) {
    auto tl = SongTimeline::build(std::move(song), endBeats_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        tl_ = tl;
        keepAlive_.push_back(tl);
        results_.assign(tl->events.size(), EventResult{});
        combo_ = maxCombo_ = 0;
        passes_.clear();
        passAcc_.assign(1, PracticeStats{});
        closing_.clear();
        live_ = LiveFeedback{};
        finished_ = false;
        passNo_ = 0;
    }
    audio_.setTimeline(tl.get());
}

void Practice::setEndBeats(int beats) {
    endBeats_ = std::clamp(beats, 0, 16);
    applyEndBeats(false);
}

void Practice::play() {
    applyEndBeats(true);
    audio_.play();
}

void Practice::applyEndBeats(bool startingRun) {
    const std::shared_ptr<const SongTimeline> tl = timeline();
    if (!tl || tl->endBeats == endBeats_) return;
    // A finished run may still be judging its last notes, so only a new start may rebuild then
    const TransportState st = audio_.state();
    if (st == TransportState::Stopped || (startingRun && st == TransportState::Finished)) loadSong(tl->song);
}

std::shared_ptr<const SongTimeline> Practice::timeline() const {
    std::lock_guard<std::mutex> lk(mu_);
    return tl_;
}

uint64_t Practice::expectedFrame(uint64_t reached) const {
    // What the player hears lags the "due" frame by the output latency; add ~1 ms of AD/DA delay and the
    // user's calibration offset
    double ms = 1.0 + latencyOffsetMs.load(std::memory_order_relaxed);
    double f = (double)reached + outLatency_.load(std::memory_order_relaxed) + ms * fs_ / 1000.0;
    return f > 0 ? (uint64_t)f : 0;
}

void Practice::resetResultsFrom(double pos, double to) {
    if (!tl_) return;
    for (size_t i = 0; i < results_.size() && i < tl_->events.size(); ++i) {
        double t = tl_->events[i].time;
        if (t >= pos - 1e-6 && t < to - 1e-6) results_[i] = EventResult{};
    }
}

int Practice::countJudged(double from, double to) const {
    int n = 0;
    if (!tl_) return 0;
    for (const auto& e : tl_->events)
        if (e.judged && e.time >= from - 1e-6 && e.time < to - 1e-6) ++n;
    return n;
}

void Practice::snapshot(std::vector<EventResult>& results, PracticeStats& stats) const {
    std::lock_guard<std::mutex> lk(mu_);
    results = results_;
    stats = passNo_ < (int)passAcc_.size() ? passAcc_[passNo_] : PracticeStats{};
    stats.total = audio_.loopOn.load() ? countJudged(audio_.loopA.load(), audio_.loopB.load()) : countJudged(0, 1e18);
    stats.combo = combo_;
    stats.maxCombo = maxCombo_;
}

std::vector<PassSummary> Practice::passes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return passes_;
}

LiveFeedback Practice::live() const {
    std::lock_guard<std::mutex> lk(mu_);
    return live_;
}

bool Practice::takeFinished() {
    std::lock_guard<std::mutex> lk(mu_);
    bool f = finished_;
    finished_ = false;
    return f;
}

void Practice::clearHistory() {
    std::lock_guard<std::mutex> lk(mu_);
    passes_.clear();
}

void Practice::pushPass(int pass) {
    if (pass < 0 || pass >= (int)passAcc_.size()) return;
    PassSummary p;
    p.pass = pass;
    p.stats = passAcc_[pass];
    p.stats.total = audio_.loopOn.load() ? countJudged(audio_.loopA.load(), audio_.loopB.load()) : countJudged(0, 1e18);
    p.stats.maxCombo = maxCombo_;
    p.speed = audio_.speed.load();
    p.wait = audio_.waitMode.load();
    if (p.stats.judged > 0) passes_.push_back(p);
    if (passes_.size() > 200) passes_.erase(passes_.begin());
}

// Ended passes: produce the summary once all of their notes are judged
void Practice::closePasses() {
    for (size_t i = 0; i < closing_.size();) {
        const int ps = closing_[i];
        bool busy = false;
        for (const auto& p : pending_)
            if (p.pass == ps) busy = true;
        if (!busy) {
            pushPass(ps);
            closing_.erase(closing_.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

void Practice::applyResult(int ev, const EventResult& r, int pass) {
    results_[ev] = r;
    if (pass >= 0) {
        if (pass >= (int)passAcc_.size()) passAcc_.resize(pass + 1);
        PracticeStats& s = passAcc_[pass];
        ++s.judged;
        switch (r.judge) {
            case Judge::Perfect: ++s.perfect; break;
            case Judge::Good: ++s.good; break;
            case Judge::Ok: ++s.ok; break;
            case Judge::WrongPitch: ++s.wrong; break;
            case Judge::Miss: ++s.miss; break;
            default: break;
        }
        if (isHit(r.judge) && r.onsetFrame) {
            s.sumTiming += r.timingMs;
            s.sumAbsTiming += std::fabs(r.timingMs);
            ++s.timingN;
        }
    }
    if (isHit(r.judge)) {
        ++combo_;
        maxCombo_ = std::max(maxCombo_, combo_);
    } else {
        combo_ = 0;
    }
    live_.event = ev;
    live_.judge = r.judge;
    live_.timingMs = r.timingMs;
    live_.detectedMidi = r.detectedMidi;
    live_.frame = lastFrame_;
}

float Practice::chromaSim(const float* chroma, const std::vector<int>& midis) const {
    float t[12] = {};
    for (int m : midis) {
        t[m % 12] += 1.f;
        t[(m + 7) % 12] += 0.35f;  // 3rd harmonic
        t[(m + 4) % 12] += 0.15f;  // 5th harmonic
    }
    float n = 0, sim = 0;
    for (int i = 0; i < 12; ++i) n += t[i] * t[i];
    n = std::sqrt(n);
    if (n <= 0) return 0.f;
    for (int i = 0; i < 12; ++i) sim += t[i] / n * chroma[i];
    return sim;
}

// Pitch (median) and pitch classes after an onset, using only analysis frames that contain neither the previous
// nor the next note; then compare with the expected notes.
bool Practice::matchOnset(const DetOnset& o, int ev, float& detMidi, float& chordScore) const {
    detMidi = -1;
    chordScore = 0;
    // Next onset (cutoff)
    uint64_t cutoff = UINT64_MAX;
    for (const auto& x : onsets_)
        if (x.frame > o.frame) {
            cutoff = x.frame;
            break;
        }
    const uint64_t halfW = (uint64_t)an_.pitchWindow() / 2;
    const uint64_t Nc = (uint64_t)an_.chromaWindow();
    const uint64_t ms3 = (uint64_t)(0.003 * fs_), ms250 = (uint64_t)(0.25 * fs_), ms200 = (uint64_t)(0.2 * fs_);
    float pit[64];
    int np = 0, nc = 0;
    float ch[12] = {};
    for (const auto& p : pframes_) {
        if (p.frame >= o.frame + halfW + ms3 && p.frame + halfW <= cutoff && p.frame <= o.frame + ms250 && p.midi > 0 &&
            p.clarity > 0.8f && np < 64)
            pit[np++] = p.midi;
        if (p.hopEnd >= o.frame + Nc && p.hopEnd <= o.frame + Nc + ms200 &&
            (cutoff == UINT64_MAX || p.hopEnd <= cutoff + (uint64_t)(0.01 * fs_))) {
            for (int k = 0; k < 12; ++k) ch[k] += p.chroma[k];
            ++nc;
        }
    }
    // Very fast notes: relax the conditions
    if (np == 0)
        for (const auto& p : pframes_)
            if (p.frame >= o.frame + (uint64_t)(0.012 * fs_) && p.frame <= std::min(cutoff, o.frame + ms200) && p.midi > 0 &&
                p.clarity > 0.8f && np < 64)
                pit[np++] = p.midi;
    if (nc == 0)
        for (const auto& p : pframes_)
            if (p.hopEnd >= o.frame + (uint64_t)(0.04 * fs_) &&
                p.hopEnd <= std::min(cutoff == UINT64_MAX ? UINT64_MAX : cutoff + Nc / 2, o.frame + ms250)) {
                for (int k = 0; k < 12; ++k) ch[k] += p.chroma[k];
                ++nc;
            }
    float midi = -1;
    if (np > 0) {
        std::sort(pit, pit + np);
        midi = pit[np / 2];
    }
    float n = 0;
    for (float v : ch) n += v * v;
    n = std::sqrt(n);
    if (n > 1e-9f)
        for (float& v : ch) v /= n;

    const std::vector<int> mids = tl_->expectedMidis(ev);
    if (mids.empty()) return false;
    if (mids.size() == 1) {
        const int e = mids[0];
        if (np >= 2 && std::fabs(midi - (float)e) <= 0.5f) {
            detMidi = midi;
            return true;
        }
        // Single-pitch detection disagrees: other strings may still ring (forming an interval) -> check whether
        // the expected note's salience rose clearly after the onset
        int det = -1;
        if (salienceCheck(o.frame, cutoff, e, det)) {
            detMidi = (float)e;
            return true;
        }
        if (np < 2 && nc > 0) {
            chordScore = chromaSim(ch, mids);
            if (chordScore >= 0.85f) {
                detMidi = (float)e;
                return true;
            }
        }
        // For display: if the single-pitch result is just a "virtual fundamental" of the expected note
        // (an octave/twelfth below...), show the most salient note instead
        const int r = np >= 2 ? (int)std::lround(midi) : -1;
        const bool subHarm = r == e - 12 || r == e - 19 || r == e - 24 || r == e - 28;
        detMidi = (r >= 0 && !subHarm) ? midi : (float)det;
        return false;
    }
    detMidi = np >= 2 ? midi : -1.f;
    if (nc == 0) return false;
    chordScore = chromaSim(ch, mids);
    const float thr = 0.86f - 0.06f * tolerance.load(std::memory_order_relaxed);
    return chordScore >= thr;
}

bool Practice::salienceCheck(uint64_t onset, uint64_t cutoff, int expected, int& detected) const {
    constexpr int lo = AnalysisFrame::kSalLo, N = AnalysisFrame::kSalN;
    detected = -1;
    const int e = expected - lo;
    if (e < 0 || e >= N) return false;
    float before[N] = {}, after[N] = {};
    int nb = 0, na = 0;
    const uint64_t Nc = (uint64_t)an_.chromaWindow();
    const uint64_t pre = (uint64_t)(0.08 * fs_), eps = (uint64_t)(0.004 * fs_), post = (uint64_t)(0.12 * fs_);
    const uint64_t cut = cutoff == UINT64_MAX ? UINT64_MAX : cutoff + (uint64_t)(0.01 * fs_);
    for (const auto& p : pframes_) {
        if (p.hopEnd + pre >= onset && p.hopEnd <= onset + eps) {  // analysis window ends before the onset
            for (int i = 0; i < N; ++i) before[i] += p.sal[i];
            ++nb;
        }
        if (p.hopEnd >= onset + Nc && p.hopEnd <= onset + Nc + post && p.hopEnd <= cut) {  // whole window after the onset
            for (int i = 0; i < N; ++i) after[i] += p.sal[i];
            ++na;
        }
    }
    if (na == 0) {  // very fast notes: use partially overlapping frames
        const uint64_t hi = std::min(onset + (uint64_t)(0.2 * fs_), cutoff == UINT64_MAX ? UINT64_MAX : cutoff + Nc / 2);
        for (const auto& p : pframes_)
            if (p.hopEnd >= onset + (uint64_t)(0.045 * fs_) && p.hopEnd <= hi) {
                for (int i = 0; i < N; ++i) after[i] += p.sal[i];
                ++na;
            }
    }
    if (na == 0) return false;
    float d[N], amax = 1e-9f;
    for (int i = 0; i < N; ++i) {
        after[i] /= (float)na;
        d[i] = after[i] - (nb ? before[i] / (float)nb : 0.f);
        amax = std::max(amax, after[i]);
    }
    float nbMax = -1e9f;
    int best = e;
    for (int k = -3; k <= 3; ++k) {
        int i = e + k;
        if (i < 0 || i >= N) continue;
        if (k != 0 && k >= -2 && k <= 2) nbMax = std::max(nbMax, d[i]);
        if (d[i] > d[best]) best = i;
    }
    detected = best + lo;
    return d[e] > 0.25f * amax && d[e] >= 0.8f * nbMax;
}

bool Practice::legatoCheck(int ev, uint64_t lo, uint64_t hi, uint64_t& at, float& detMidi) const {
    const std::vector<int> mids = tl_->expectedMidis(ev);
    if (mids.size() != 1) return false;
    int run = 0;
    uint64_t first = 0;
    for (const auto& p : pframes_) {
        if (p.frame < lo || p.frame > hi) continue;
        if (p.midi > 0 && p.clarity > 0.8f && std::fabs(p.midi - (float)mids[0]) <= 0.5f) {
            if (run == 0) first = p.frame;
            if (++run >= 3) {
                at = first;
                detMidi = p.midi;
                return true;
            }
        } else {
            run = 0;
        }
    }
    return false;
}

void Practice::finalize(int ev, uint64_t E, int pass) {
    if (!tl_ || ev < 0 || ev >= (int)results_.size() || results_[ev].judge != Judge::Pending) return;
    const auto& e = tl_->events[ev];
    const double tol = std::clamp((double)tolerance.load(std::memory_order_relaxed), 0.5, 2.0);
    const uint64_t win = (uint64_t)(std::clamp(0.2 * tol, 0.1, 0.35) * fs_);
    int best = -1;
    double bestScore = -1e18;
    bool bestOk = false;
    float bestMidi = -1, bestChord = 0;
    for (size_t i = 0; i < onsets_.size(); ++i) {
        const DetOnset& o = onsets_[i];
        if (o.claimed) continue;
        double d = (double)o.frame - (double)E;
        if (std::fabs(d) > (double)win) continue;
        float dm, cs;
        bool ok = matchOnset(o, ev, dm, cs);
        double score = (ok ? 1e9 : 0.0) - std::fabs(d);
        if (score > bestScore) {
            bestScore = score;
            best = (int)i;
            bestOk = ok;
            bestMidi = dm;
            bestChord = cs;
        }
    }
    EventResult r = results_[ev];
    uint64_t at = 0;
    float lm = -1;
    if (best >= 0 && bestOk) {
        onsets_[best].claimed = true;
        r.onsetFrame = onsets_[best].frame;
        r.timingMs = (float)(((double)onsets_[best].frame - (double)E) / fs_ * 1000.0);
        r.detectedMidi = bestMidi;
        r.chordScore = bestChord;
        const float a = std::fabs(r.timingMs);
        r.judge = a <= 45.0 * tol ? Judge::Perfect : (a <= 100.0 * tol ? Judge::Good : Judge::Ok);
    } else if (e.legato && legatoCheck(ev, E > win / 2 ? E - win / 2 : 0, E + win, at, lm)) {
        r.onsetFrame = 0;  // hammer-ons/pull-offs/slides have no onset; not counted in timing stats
        r.timingMs = (float)(((double)at - (double)E) / fs_ * 1000.0);
        r.detectedMidi = lm;
        r.judge = Judge::Good;
    } else if (best >= 0) {
        onsets_[best].claimed = true;
        r.onsetFrame = onsets_[best].frame;
        r.timingMs = (float)(((double)onsets_[best].frame - (double)E) / fs_ * 1000.0);
        r.detectedMidi = bestMidi;
        r.chordScore = bestChord;
        r.judge = Judge::WrongPitch;
    } else {
        r.judge = Judge::Miss;
    }
    applyResult(ev, r, pass);
}

void Practice::handleMessages() {
    TransportMsg m;
    auto& q = audio_.messages();
    while (q.pop(m)) {
        if (!tl_ || m.tl != tl_.get()) continue;  // message from a previous song
        switch (m.type) {
            case TransportMsg::TimelineChanged:
                pending_.clear();
                waitDone_ = true;
                waitEv_ = -1;
                runFresh_ = true;
                songEnded_ = false;
                break;
            case TransportMsg::Started:
                if (runFresh_) {
                    resetResultsFrom(m.pos);
                    combo_ = 0;
                    ++passNo_;
                    if (passNo_ >= (int)passAcc_.size()) passAcc_.resize(passNo_ + 1);
                    passAcc_[passNo_] = PracticeStats{};
                    finished_ = false;
                    songEnded_ = false;
                    runFresh_ = false;
                }
                break;
            case TransportMsg::Paused:
                waitDone_ = true;
                break;
            case TransportMsg::Stopped:
                pending_.clear();
                waitDone_ = true;
                runFresh_ = true;
                break;
            case TransportMsg::Seeked:
                pending_.clear();
                waitDone_ = true;
                resetResultsFrom(m.pos);
                break;
            case TransportMsg::EventReached:
                if (m.event >= 0 && m.event < (int)results_.size() && tl_->events[m.event].judged) {
                    bool already = false;  // already judged in wait mode (same pass)
                    for (const auto& p : pending_)
                        if (p.ev == m.event) already = true;
                    if (waitJudged_ == m.event) already = true;
                    if (already) break;
                    results_[m.event] = EventResult{};  // results from the previous (loop) pass are judged again
                    results_[m.event].reachedFrame = m.frame;
                    const uint64_t E = expectedFrame(m.frame);
                    const double tol = std::clamp((double)tolerance.load(), 0.5, 2.0);
                    const uint64_t win = (uint64_t)(std::clamp(0.2 * tol, 0.1, 0.35) * fs_);
                    pending_.push_back(Pending{m.event, E, E + win + (uint64_t)(0.32 * fs_), passNo_});
                }
                break;
            case TransportMsg::WaitArrived:
                if (m.event >= 0 && m.event < (int)results_.size()) {
                    waitEv_ = m.event;
                    waitSeq_ = m.seq;
                    waitFrame_ = expectedFrame(m.frame);
                    waitDone_ = false;
                    waitJudged_ = -1;
                    results_[m.event] = EventResult{};
                    results_[m.event].reachedFrame = m.frame;
                }
                break;
            case TransportMsg::LoopRestart:
                // This pass ends; notes still pending are judged as usual and count toward it
                closing_.push_back(passNo_);
                combo_ = 0;
                ++passNo_;
                if (passNo_ >= (int)passAcc_.size()) passAcc_.resize(passNo_ + 1);
                passAcc_[passNo_] = PracticeStats{};
                break;
            case TransportMsg::SongEnd:
                songEnded_ = true;
                runFresh_ = true;
                break;
        }
    }
}

void Practice::onAnalysisFrame(const AnalysisFrame& f) {
    std::lock_guard<std::mutex> lk(mu_);
    lastFrame_ = f.hopEnd;
    handleMessages();
    if (!tl_) return;

    PFrame pf;
    pf.frame = f.frame;
    pf.hopEnd = f.hopEnd;
    pf.midi = f.midi;
    pf.clarity = f.clarity;
    pf.rmsDb = f.rmsDb;
    std::copy(f.chroma, f.chroma + 12, pf.chroma);
    std::copy(f.salience, f.salience + AnalysisFrame::kSalN, pf.sal);
    pframes_.push_back(pf);
    const uint64_t keep = (uint64_t)(6.0 * fs_);
    while (!pframes_.empty() && pframes_.front().hopEnd + keep < f.hopEnd) pframes_.pop_front();
    if (f.onset) {
        DetOnset o;
        o.frame = f.onsetFrame;
        o.strength = f.onsetStrength;
        onsets_.push_back(o);
    }
    while (!onsets_.empty() && onsets_.front().frame + keep < f.hopEnd) onsets_.pop_front();

    // ---- Wait mode: release the transport once the note is played correctly ----
    if (!waitDone_ && waitEv_ >= 0 && waitEv_ < (int)results_.size()) {
        const auto mids = tl_->expectedMidis(waitEv_);
        const bool single = mids.size() == 1;
        const uint64_t early = (uint64_t)(0.4 * fs_);
        for (auto& o : onsets_) {
            if (o.claimed || o.frame + early < waitFrame_) continue;
            // Enough analysis frames after the onset are needed to decide
            const uint64_t need = single ? (uint64_t)an_.pitchWindow() / 2 + (uint64_t)(0.03 * fs_)
                                         : (uint64_t)an_.chromaWindow() + (uint64_t)(0.03 * fs_);
            if (f.hopEnd < o.frame + need) continue;
            float dm, cs;
            if (matchOnset(o, waitEv_, dm, cs)) {
                o.claimed = true;
                EventResult r = results_[waitEv_];
                r.judge = Judge::Perfect;
                r.timingMs = 0;
                r.detectedMidi = dm;
                r.chordScore = cs;
                r.onsetFrame = 0;  // wait mode doesn't score timing
                applyResult(waitEv_, r, passNo_);
                waitDone_ = true;
                waitJudged_ = waitEv_;
                audio_.satisfyWait(waitSeq_);
                break;
            }
            if (f.hopEnd >= o.frame + need + (uint64_t)(0.05 * fs_)) {  // confirmed wrong
                o.claimed = true;
                results_[waitEv_].attempts++;
                live_.wrongMidi = dm > 0 ? (int)std::lround(dm) : -1;
                live_.wrongFrame = f.hopEnd;
            }
        }
        if (!waitDone_ && tl_->events[waitEv_].legato) {
            uint64_t at = 0;
            float dm = -1;
            if (legatoCheck(waitEv_, waitFrame_ > early ? waitFrame_ - early : 0, f.hopEnd, at, dm)) {
                EventResult r = results_[waitEv_];
                r.judge = Judge::Perfect;
                r.detectedMidi = dm;
                applyResult(waitEv_, r, passNo_);
                waitDone_ = true;
                waitJudged_ = waitEv_;
                audio_.satisfyWait(waitSeq_);
            }
        }
    }

    // ---- Normal mode: judge once the timing window has passed ----
    for (size_t i = 0; i < pending_.size();) {
        if (f.hopEnd >= pending_[i].deadline) {
            const Pending p = pending_[i];
            pending_.erase(pending_.begin() + (long)i);
            finalize(p.ev, p.expected, p.pass);
        } else {
            ++i;
        }
    }
    closePasses();
    if (songEnded_ && pending_.empty()) {
        songEnded_ = false;
        pushPass(passNo_);
        finished_ = true;
    }
}

}  // namespace gs
