#include "practice/PracticeAudio.h"

#include <algorithm>
#include <cmath>

namespace gs {

PracticeAudio::PracticeAudio() {
    cmds_.init(256);
    msgs_.init(8192);
    prepare(48000, 1024);
}

void PracticeAudio::prepare(double fs, int maxBlock) {
    (void)maxBlock;
    fs_ = fs;
    synth_.prepare(fs);
    previewSynth_.prepare(fs);
    demoSynth_.prepare(fs);
    click_.prepare(fs);
}

void PracticeAudio::demoSchedule(int ev, double delaySec) {
    const TimelineEvent& e = tl_->events[ev];
    std::uniform_real_distribution<float> U(0.f, 1.f);
    if (U(demoRng_) < demoMissRate.load(std::memory_order_relaxed)) return;
    const bool wrong = U(demoRng_) < demoWrongRate.load(std::memory_order_relaxed);
    const double sp = std::clamp((double)speed.load(), 0.1, 3.0);
    int k = 0;
    for (int i = e.noteCount - 1; i >= 0; --i) {  // low strings first (down strum)
        const TimelineNote& n = tl_->notes[e.firstNote + i];
        if (!n.playable()) continue;
        int midi = n.midi + (wrong ? (U(demoRng_) < 0.5f ? 2 : -1) : 0);
        int delay = (int)(std::max(0.0, delaySec) * fs_) + k * (int)(0.008 * fs_) + 1;
        demoSynth_.noteOn(n.string, midi, 0.8f, delay, (int)(n.dur / sp * fs_) + delay);
        ++k;
    }
}

void PracticeAudio::renderDemoInput(float* buf, int n) {
    if (!demoPlayer.load(std::memory_order_relaxed)) return;
    if (tl_ != demoTl_ || std::fabs(pos_ - demoLastPos_) > 0.5 || pos_ < demoLastPos_ - 1e-6) {
        demoTl_ = tl_;  // new song / seek / loop: restart from the current position (including notes just reached)
        demoNextEv_ = tl_ ? tl_->eventAt(pos_ - 0.05 * std::clamp((double)speed.load(), 0.1, 3.0)) : 0;
    }
    if (tl_ && (state_ == TransportState::Playing || state_ == TransportState::CountIn)) {
        const double sp = std::clamp((double)speed.load(), 0.1, 3.0);
        const double lat = demoLatencyFrames.load(std::memory_order_relaxed) / fs_;
        std::normal_distribution<double> J(0.0, demoJitterMs.load(std::memory_order_relaxed) / 1000.0);
        if (waitMode.load(std::memory_order_relaxed)) {
            // Wait mode: once the transport holds, "think" for a moment before playing
            if (holding_ && demoWaitSeq_ != waitSeq_.load()) {
                demoWaitSeq_ = waitSeq_.load();
                demoSchedule(nextEv_, 0.5 + lat);
            }
            demoNextEv_ = nextEv_;
        } else {
            const double ahead = 0.25;  // schedule ahead (allows playing early)
            const double base = state_ == TransportState::CountIn
                                    ? (tl_->countInBeats * countInBeatSamples_ - countInElapsed_) / fs_
                                    : 0.0;
            while (demoNextEv_ < (int)tl_->events.size() &&
                   tl_->events[demoNextEv_].time < pos_ + ahead * sp && (state_ == TransportState::Playing || base < ahead)) {
                const double dt = base + (tl_->events[demoNextEv_].time - pos_) / sp + lat + J(demoRng_);
                if (tl_->events[demoNextEv_].judged) demoSchedule(demoNextEv_, dt);
                ++demoNextEv_;
            }
        }
    }
    demoLastPos_ = pos_;
    float tmpR[2048];
    float* tl = buf;
    int done = 0;
    while (done < n) {
        int c = std::min(n - done, 2048);
        std::fill(tmpR, tmpR + c, 0.f);
        demoSynth_.render(tl + done, tmpR, c, 0.35f);
        done += c;
    }
}

void PracticeAudio::setTimeline(const SongTimeline* tl) { cmds_.push(Cmd{CmdTimeline, 0, tl}); }
void PracticeAudio::play() { cmds_.push(Cmd{CmdPlay, 0, nullptr}); }
void PracticeAudio::pause() { cmds_.push(Cmd{CmdPause, 0, nullptr}); }
void PracticeAudio::stop() { cmds_.push(Cmd{CmdStop, 0, nullptr}); }
void PracticeAudio::seek(double sec) { cmds_.push(Cmd{CmdSeek, sec, nullptr}); }
void PracticeAudio::previewNote(int midi, int string) { cmds_.push(Cmd{CmdPreview, 0, nullptr, midi, string}); }

void PracticeAudio::post(TransportMsg::Type t, int ev, uint64_t frame, uint32_t seq) {
    TransportMsg m;
    m.type = t;
    m.event = ev;
    m.frame = frame;
    m.seq = seq;
    m.pos = pos_;
    m.tl = tl_;
    msgs_.push(m);
}

void PracticeAudio::reposition(double t) {
    if (!tl_) {
        pos_ = 0;
        nextEv_ = nextBeat_ = 0;
        return;
    }
    pos_ = std::clamp(t, 0.0, tl_->length);
    nextEv_ = tl_->eventAt(pos_);
    auto it = std::lower_bound(tl_->beatTimes.begin(), tl_->beatTimes.end(), pos_ - 1e-9);
    nextBeat_ = (int)(it - tl_->beatTimes.begin());
    holding_ = false;
}

void PracticeAudio::handleCommands(uint64_t frameClock) {
    Cmd c;
    while (cmds_.pop(c)) {
        switch (c.type) {
            case CmdTimeline:
                tl_ = c.tl;
                state_ = TransportState::Stopped;
                reposition(0);
                synth_.allNotesOff();
                post(TransportMsg::TimelineChanged, -1, frameClock);
                break;
            case CmdPlay: {
                if (!tl_) break;
                if (state_ == TransportState::Playing || state_ == TransportState::CountIn) break;
                if (state_ == TransportState::Finished || pos_ >= tl_->length - 1e-6)
                    reposition(loopOn.load() ? loopA.load() : 0.0);
                const double sp = std::clamp((double)speed.load(), 0.1, 3.0);
                if (countIn.load()) {
                    state_ = TransportState::CountIn;
                    countInClicked_ = 0;
                    countInElapsed_ = 0;
                    countInBeatSamples_ = tl_->countInBeatSec / sp * fs_;
                } else {
                    state_ = TransportState::Playing;
                }
                post(TransportMsg::Started, -1, frameClock);
                break;
            }
            case CmdPause:
                if (state_ == TransportState::Playing || state_ == TransportState::CountIn) {
                    state_ = TransportState::Paused;
                    reposition(pos_);  // a note being waited on is reached again after resuming
                    synth_.allNotesOff();
                    post(TransportMsg::Paused, -1, frameClock);
                }
                break;
            case CmdStop:
                state_ = TransportState::Stopped;
                reposition(loopOn.load() ? loopA.load() : 0.0);
                synth_.allNotesOff();
                post(TransportMsg::Stopped, -1, frameClock);
                break;
            case CmdPreview:
                previewSynth_.noteOn(c.b >= 0 && c.b < 6 ? c.b : -1, c.a, 0.85f, 0, (int)(2.5 * fs_));
                break;
            case CmdSeek:
                reposition(c.value);
                if (state_ == TransportState::Finished) state_ = TransportState::Stopped;
                synth_.allNotesOff();
                post(TransportMsg::Seeked, -1, frameClock);
                break;
        }
    }
}

void PracticeAudio::triggerEvent(int ev, double sp) {
    const TimelineEvent& e = tl_->events[ev];
    // Strums: down = low to high strings, up = the reverse; chords default to down
    int order[8];
    int cnt = 0;
    for (int i = 0; i < e.noteCount && cnt < 8; ++i) order[cnt++] = e.firstNote + i;
    std::sort(order, order + cnt, [&](int a, int b) {
        return e.strum == Strum::Up ? tl_->notes[a].string < tl_->notes[b].string
                                    : tl_->notes[a].string > tl_->notes[b].string;
    });
    const bool chord = cnt >= 3;
    const int spacing = chord ? (int)(0.010 * fs_) : 0;
    int k = 0;
    for (int i = 0; i < cnt; ++i) {
        const TimelineNote& n = tl_->notes[order[i]];
        if (!n.playable()) continue;
        float vel = 0.75f;
        if (n.flags & NF_Accent) vel = 1.f;
        if (n.flags & NF_Ghost) vel = 0.35f;
        if (n.flags & (NF_Hammer | NF_Pull)) vel = 0.5f;
        double dur = n.dur;
        if (n.flags & NF_PalmMute) dur = std::min(dur, 0.12);
        if (!(n.flags & NF_LetRing)) dur *= 1.05;
        synth_.noteOn(n.string, n.midi, vel, k * spacing, (int)(dur / sp * fs_) + k * spacing);
        ++k;
    }
}

void PracticeAudio::renderSeg(float* L, float* R, int n, float refVol, float metroVol) {
    if (n <= 0) return;
    synth_.render(L, R, n, refVol);
    previewSynth_.render(L, R, n, 0.6f);
    click_.render(L, R, n, metroVol);
}

void PracticeAudio::process(float* L, float* R, int n, uint64_t frameClock) {
    handleCommands(frameClock);
    const float metroVol = metronomeVol.load(std::memory_order_relaxed);
    const float refVol = reference.load(std::memory_order_relaxed) ? referenceVol.load(std::memory_order_relaxed) : 0.f;
    const double sp = std::clamp((double)speed.load(std::memory_order_relaxed), 0.1, 3.0);
    const bool metro = metronome.load(std::memory_order_relaxed);
    const bool wait = waitMode.load(std::memory_order_relaxed);

    int i = 0;
    int guard = 0;
    while (i < n && ++guard < 10000) {
        const int k = n - i;
        if (state_ == TransportState::CountIn && tl_) {
            const int beats = tl_->countInBeats;
            const double mark = countInClicked_ < beats ? countInClicked_ * countInBeatSamples_ : beats * countInBeatSamples_;
            const double until = mark - countInElapsed_;
            if (until <= k) {
                int c = std::clamp((int)std::ceil(until - 1e-9), 0, k);
                renderSeg(L + i, R + i, c, refVol, metroVol);
                countInElapsed_ += c;
                i += c;
                if (countInClicked_ < beats) {
                    click_.trigger(countInClicked_ == 0, 0);
                    ++countInClicked_;
                } else {
                    state_ = TransportState::Playing;
                }
                continue;
            }
            renderSeg(L + i, R + i, k, refVol, metroVol);
            countInElapsed_ += k;
            i += k;
            continue;
        }
        if (state_ == TransportState::Playing && tl_) {
            const auto& evs = tl_->events;
            const int nEv = (int)evs.size();
            if (holding_) {
                if (!wait || satisfiedSeq_.load(std::memory_order_acquire) == waitSeq_.load(std::memory_order_relaxed)) {
                    holding_ = false;  // played correctly (or wait mode turned off): continue
                    post(TransportMsg::EventReached, nextEv_, frameClock + (uint64_t)i);
                    ++nextEv_;
                } else {
                    renderSeg(L + i, R + i, k, refVol, metroVol);
                    i += k;
                    continue;
                }
            }
            const double dt = sp / fs_;
            double tNext = tl_->length;
            if (nextEv_ < nEv) tNext = std::min(tNext, evs[nextEv_].time);
            if (nextBeat_ < (int)tl_->beatTimes.size()) tNext = std::min(tNext, tl_->beatTimes[nextBeat_]);
            const double la = loopA.load(), lb = loopB.load();
            const bool loop = loopOn.load(std::memory_order_relaxed) && lb > la + 0.05 && pos_ < lb;
            if (loop) tNext = std::min(tNext, lb);
            const double samplesTo = (tNext - pos_) / dt;
            if (samplesTo > k) {
                renderSeg(L + i, R + i, k, refVol, metroVol);
                pos_ += k * dt;
                i += k;
                continue;
            }
            const int c = std::clamp((int)std::ceil(samplesTo - 1e-9), 0, k);
            renderSeg(L + i, R + i, c, refVol, metroVol);
            pos_ += c * dt;
            i += c;
            if (pos_ < tNext) pos_ = tNext;
            // Loop end first: a note/beat exactly at B belongs outside the loop
            if (loop && pos_ >= lb - 1e-9) {
                reposition(la);
                post(TransportMsg::LoopRestart, -1, frameClock + (uint64_t)i);
                continue;
            }
            while (nextBeat_ < (int)tl_->beatTimes.size() && tl_->beatTimes[nextBeat_] <= pos_ + 1e-9) {
                if (metro) click_.trigger(tl_->beatAccent[nextBeat_] != 0, 0);
                ++nextBeat_;
            }
            while (nextEv_ < nEv && evs[nextEv_].time <= pos_ + 1e-9) {
                if (wait && evs[nextEv_].judged) {
                    holding_ = true;
                    const uint32_t seq = waitSeq_.load(std::memory_order_relaxed) + 1;
                    waitSeq_.store(seq, std::memory_order_relaxed);
                    triggerEvent(nextEv_, sp);  // demonstrate what to play
                    post(TransportMsg::WaitArrived, nextEv_, frameClock + (uint64_t)i, seq);
                    break;
                }
                triggerEvent(nextEv_, sp);
                post(TransportMsg::EventReached, nextEv_, frameClock + (uint64_t)i);
                ++nextEv_;
            }
            if (holding_) continue;
            if (nextEv_ >= nEv && pos_ >= tl_->length - 1e-9) {
                state_ = TransportState::Finished;
                post(TransportMsg::SongEnd, -1, frameClock + (uint64_t)i);
            }
            continue;
        }
        // Stopped/paused: standalone metronome
        if (freeMetronome.load(std::memory_order_relaxed)) {
            const double beatSamples = 60.0 / std::clamp((double)freeBpm.load(), 20.0, 400.0) * fs_;
            if (!freeWasOn_) {
                freeWasOn_ = true;
                freePhase_ = 0;
                freeBeat_ = 0;
            }
            if (freePhase_ <= k) {
                int c = std::clamp((int)std::ceil(freePhase_ - 1e-9), 0, k);
                renderSeg(L + i, R + i, c, refVol, metroVol);
                i += c;
                const int bpb = std::max(1, freeBeatsPerBar.load());
                click_.trigger(freeBeat_ % bpb == 0, 0);
                freeBeat_pub_.store(freeBeat_ % bpb, std::memory_order_relaxed);
                ++freeBeat_;
                freePhase_ += beatSamples - c;
                continue;
            }
            freePhase_ -= k;
        } else {
            freeWasOn_ = false;
        }
        renderSeg(L + i, R + i, k, refVol, metroVol);
        i += k;
    }

    state_pub_.store((int)state_, std::memory_order_relaxed);
    pos_pub_.store(pos_);
    holding_pub_.store(holding_, std::memory_order_relaxed);
    waitEv_pub_.store(holding_ ? nextEv_ : -1, std::memory_order_relaxed);
    frame_pub_.store(frameClock + (uint64_t)n, std::memory_order_relaxed);
    if (state_ == TransportState::CountIn && tl_)
        countIn_pub_.store((float)(tl_->countInBeats - countInElapsed_ / std::max(1.0, countInBeatSamples_)),
                           std::memory_order_relaxed);
    else
        countIn_pub_.store(0.f, std::memory_order_relaxed);
}

}  // namespace gs
