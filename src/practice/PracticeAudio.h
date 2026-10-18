#pragma once
// Practice transport (runs on the audio thread, sample-accurate): count-in, metronome, reference synth, A-B loop,
// speed change, wait mode. The UI controls it through a lock-free command queue; the transport tells the
// evaluator "note N is due now" through a lock-free message queue.
#include <atomic>
#include <cstdint>

#include "common/SpscRing.h"
#include "practice/PluckSynth.h"
#include "practice/Timeline.h"

namespace gs {

enum class TransportState : uint8_t { Stopped, CountIn, Playing, Paused, Finished };

struct TransportMsg {  // audio thread -> evaluator
    enum Type : uint8_t { Started, Paused, Stopped, Seeked, EventReached, WaitArrived, LoopRestart, SongEnd, TimelineChanged };
    Type type = Started;
    int event = -1;
    uint32_t seq = 0;
    uint64_t frame = 0;                // engine frame clock
    double pos = 0;                    // song time (seconds)
    const SongTimeline* tl = nullptr;  // timeline the message belongs to
};

class PracticeAudio {
public:
    PracticeAudio();
    void prepare(double fs, int maxBlock);
    double sampleRate() const { return fs_; }

    // ---- audio thread ----
    void process(float* L, float* R, int n, uint64_t frameClock);

    // ---- UI thread commands ----
    void setTimeline(const SongTimeline* tl);  // tl must stay valid for the program's lifetime
    void play();
    void pause();
    void stop();
    void seek(double sec);
    void previewNote(int midi, int string = -1);  // audition a note (tuner / clicking a note)

    // ---- settings (any thread) ----
    std::atomic<float> speed{1.f};
    std::atomic<bool> waitMode{false};
    std::atomic<bool> metronome{true};       // click along while a song plays
    std::atomic<bool> freeMetronome{false};  // standalone metronome when no song is playing
    std::atomic<float> freeBpm{80.f};
    std::atomic<int> freeBeatsPerBar{4};
    std::atomic<float> metronomeVol{0.45f};
    std::atomic<bool> reference{true};
    std::atomic<float> referenceVol{0.5f};
    std::atomic<bool> countIn{true};
    std::atomic<int> countInBeats{2};  // clicks before playing starts (and before resuming)
    std::atomic<bool> loopOn{false};
    AtomicDouble loopA{0}, loopB{0};

    // ---- state (read by the UI) ----
    TransportState state() const { return (TransportState)state_pub_.load(std::memory_order_relaxed); }
    double position() const { return pos_pub_.load(); }
    bool holding() const { return holding_pub_.load(std::memory_order_relaxed); }
    int waitingEvent() const { return waitEv_pub_.load(std::memory_order_relaxed); }
    float countInLeft() const { return countIn_pub_.load(std::memory_order_relaxed); }  // beats remaining
    uint64_t frameClock() const { return frame_pub_.load(std::memory_order_relaxed); }
    int freeBeatIndex() const { return freeBeat_pub_.load(std::memory_order_relaxed); }

    // ---- demo/test: a simulated "player" plays the score into the guitar input (no-device mode only) ----
    std::atomic<bool> demoPlayer{false};
    std::atomic<float> demoJitterMs{25.f};   // timing error (standard deviation)
    std::atomic<float> demoWrongRate{0.06f}; // probability of a wrong note
    std::atomic<float> demoMissRate{0.03f};  // probability of a missed note
    std::atomic<int> demoLatencyFrames{0};   // engine output latency (the player reacts to what they hear)
    // Audio thread, called before process(): generates this block's "guitar" input
    void renderDemoInput(float* buf, int n);

    // ---- evaluator interface (analysis thread) ----
    SpscRing<TransportMsg>& messages() { return msgs_; }
    void satisfyWait(uint32_t seq) { satisfiedSeq_.store(seq, std::memory_order_release); }

private:
    enum CmdType : uint8_t { CmdPlay, CmdPause, CmdStop, CmdSeek, CmdTimeline, CmdPreview };
    struct Cmd {
        CmdType type;
        double value;
        const SongTimeline* tl;
        int a = 0, b = -1;
    };
    void post(TransportMsg::Type t, int ev, uint64_t frame, uint32_t seq = 0);
    void reposition(double t);
    void triggerEvent(int ev, double speed);
    void handleCommands(uint64_t frameClock);
    void renderSeg(float* L, float* R, int n, float refVol, float metroVol);

    double fs_ = 48000;
    SpscRing<Cmd> cmds_;
    SpscRing<TransportMsg> msgs_;
    PluckSynth synth_;
    PluckSynth previewSynth_;  // auditioning (independent of the reference switch)
    ClickSynth click_;
    const SongTimeline* tl_ = nullptr;
    TransportState state_ = TransportState::Stopped;
    double pos_ = 0;
    int nextEv_ = 0, nextBeat_ = 0;
    int countInClicked_ = 0, countInTotal_ = 2;
    double countInElapsed_ = 0, countInBeatSamples_ = 0;
    bool holding_ = false;
    std::atomic<uint32_t> waitSeq_{0}, satisfiedSeq_{0};
    double freePhase_ = 0;
    int freeBeat_ = 0;
    bool freeWasOn_ = false;
    // Demo player
    PluckSynth demoSynth_;
    std::mt19937 demoRng_{2024};
    int demoNextEv_ = 0;
    uint32_t demoWaitSeq_ = 0;
    const SongTimeline* demoTl_ = nullptr;
    double demoLastPos_ = -1;
    void demoSchedule(int ev, double delaySec);

    std::atomic<int> state_pub_{0};
    AtomicDouble pos_pub_{0};
    std::atomic<bool> holding_pub_{false};
    std::atomic<int> waitEv_pub_{-1};
    std::atomic<uint64_t> frame_pub_{0};
    std::atomic<float> countIn_pub_{0};
    std::atomic<int> freeBeat_pub_{0};
};

}  // namespace gs
