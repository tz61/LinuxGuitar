#pragma once
// Practice session: song loading + transport control + real-time scoring (pitch / timing).
// The evaluator runs on the analysis thread (Analyzer callback); the UI reads results through snapshot().
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "analysis/Analyzer.h"
#include "practice/PracticeAudio.h"
#include "practice/Timeline.h"

namespace gs {

enum class Judge : uint8_t { Pending, Perfect, Good, Ok, WrongPitch, Miss };

struct EventResult {
    Judge judge = Judge::Pending;
    float timingMs = 0;       // + late / - early
    float detectedMidi = -1;  // single notes: detected pitch
    float chordScore = 0;     // chords: pitch-class similarity 0..1
    uint64_t reachedFrame = 0;
    uint64_t onsetFrame = 0;
    int attempts = 0;         // wrong attempts in wait mode
};

struct PracticeStats {
    int total = 0, judged = 0, perfect = 0, good = 0, ok = 0, wrong = 0, miss = 0;
    int combo = 0, maxCombo = 0;
    double sumTiming = 0, sumAbsTiming = 0;
    int timingN = 0;
    int hits() const { return perfect + good + ok; }
    float hitRate() const { return judged ? (float)hits() / judged : 0.f; }
    float score() const { return judged ? (perfect * 1.f + good * 0.8f + ok * 0.5f) / judged : 0.f; }
    float meanTiming() const { return timingN ? (float)(sumTiming / timingN) : 0.f; }
    float meanAbsTiming() const { return timingN ? (float)(sumAbsTiming / timingN) : 0.f; }
};

struct PassSummary {
    int pass = 0;
    PracticeStats stats;
    float speed = 1.f;
    bool wait = false;
};

struct LiveFeedback {
    int event = -1;
    Judge judge = Judge::Pending;
    float timingMs = 0;
    float detectedMidi = -1;
    uint64_t frame = 0;          // when the judgment was made
    int wrongMidi = -1;          // wait mode: the most recent wrong note
    uint64_t wrongFrame = 0;
};

class Practice {
public:
    explicit Practice(Analyzer& analyzer);
    // Called when the engine (re)starts, while the analysis thread is stopped
    void prepare(double fs, int outputLatencyFrames);
    void setOutputLatency(int frames) { outLatency_.store(frames, std::memory_order_relaxed); }
    PracticeAudio& audio() { return audio_; }
    const PracticeAudio& audio() const { return audio_; }

    void loadSong(std::shared_ptr<const Song> song);  // UI thread
    std::shared_ptr<const SongTimeline> timeline() const;

    void play() { audio_.play(); }
    void pause() { audio_.pause(); }
    void stop() { audio_.stop(); }
    void seek(double sec) { audio_.seek(sec); }

    void snapshot(std::vector<EventResult>& results, PracticeStats& stats) const;
    std::vector<PassSummary> passes() const;
    LiveFeedback live() const;
    bool takeFinished();      // returns true once after a song ends (and everything is judged)
    void clearHistory();

    // Analysis thread callback
    void onAnalysisFrame(const AnalysisFrame& f);

    std::atomic<float> tolerance{1.f};        // judging tolerance: 0.6 strict .. 1.6 relaxed
    std::atomic<float> latencyOffsetMs{0.f};  // user calibration offset
    double fs() const { return fs_; }
    int outputLatencyFrames() const { return outLatency_.load(std::memory_order_relaxed); }

private:
    struct DetOnset {
        uint64_t frame = 0;
        float strength = 0;
        bool claimed = false;  // already matched to a note
    };
    struct PFrame {
        uint64_t frame, hopEnd;
        float midi, clarity, rmsDb;
        float chroma[12];
        float sal[AnalysisFrame::kSalN];
    };
    struct Pending {
        int ev;
        uint64_t expected;
        uint64_t deadline;
        int pass;
    };

    void handleMessages();
    void finalize(int ev, uint64_t expected, int pass);
    void applyResult(int ev, const EventResult& r, int pass);
    void closePasses();
    bool matchOnset(const DetOnset& o, int ev, float& detMidi, float& chordScore) const;
    bool legatoCheck(int ev, uint64_t lo, uint64_t hi, uint64_t& at, float& detMidi) const;
    bool salienceCheck(uint64_t onset, uint64_t cutoff, int expected, int& detected) const;
    float chromaSim(const float* chroma, const std::vector<int>& midis) const;
    void resetResultsFrom(double pos, double to = 1e18);
    int countJudged(double from, double to) const;
    void pushPass(int pass);
    uint64_t expectedFrame(uint64_t reached) const;

    Analyzer& an_;
    PracticeAudio audio_;
    double fs_ = 48000;
    std::atomic<int> outLatency_{128};

    mutable std::mutex mu_;  // protects the UI-visible data below
    std::shared_ptr<const SongTimeline> tl_;
    std::vector<std::shared_ptr<const SongTimeline>> keepAlive_;  // the audio thread holds raw pointers; never freed while running
    std::vector<EventResult> results_;
    int combo_ = 0, maxCombo_ = 0;
    std::vector<PassSummary> passes_;
    std::vector<PracticeStats> passAcc_;   // accumulated stats per pass (index = pass number)
    std::vector<int> closing_;             // passes that ended but still have notes to judge
    LiveFeedback live_;
    bool finished_ = false;
    int passNo_ = 0;

    // ---- analysis thread only ----
    std::deque<DetOnset> onsets_;
    std::deque<PFrame> pframes_;
    std::vector<Pending> pending_;
    uint64_t lastFrame_ = 0;
    bool runFresh_ = true;     // the next Started message begins a new pass
    bool songEnded_ = false;
    int waitEv_ = -1;
    uint32_t waitSeq_ = 0;
    uint64_t waitFrame_ = 0;
    bool waitDone_ = true;
    int waitJudged_ = -1;   // note just judged in wait mode (its following EventReached is not judged again)
};

const char* judgeName(Judge j);  // English; display through tr()

// Registers the Chinese translations of practice-module strings (implemented in practice/I18nZh.cpp).
void registerPracticeTranslations();

}  // namespace gs
