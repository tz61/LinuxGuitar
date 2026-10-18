// Offline end-to-end practice test: transport + simulated player (synthetic plucks with timing errors, wrong and
// missed notes) + analysis + scoring.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "analysis/Analyzer.h"
#include "practice/Practice.h"
#include "song/Song.h"

using namespace gs;

static int g_fail = 0;
#define CHECK(c, ...)                             \
    do {                                          \
        if (!(c)) {                               \
            std::printf("  [FAIL] " __VA_ARGS__); \
            std::printf("\n");                    \
            ++g_fail;                             \
        }                                         \
    } while (0)

struct RunResult {
    PracticeStats st;
    std::vector<PassSummary> passes;
    bool finished = false;
    double seconds = 0;
};

static RunResult runSong(const std::string& gtab, float jitterMs, float wrong, float miss, bool wait, float speed = 1.f,
                         bool loop = false, double maxSec = 120) {
    const double fs = 48000;
    const int block = 64, latency = 136;
    Analyzer an;
    an.configure(fs);
    Practice pr(an);
    pr.prepare(fs, latency);
    an.setFrameCallback([&](const AnalysisFrame& f) { pr.onAnalysisFrame(f); });
    auto song = std::make_shared<Song>();
    std::string err;
    if (!parseGtab(gtab, *song, err)) {
        std::printf("  parse failed: %s\n", err.c_str());
        ++g_fail;
        return {};
    }
    pr.loadSong(song);
    auto& pa = pr.audio();
    pa.demoPlayer = true;
    pa.demoLatencyFrames = latency;
    pa.demoJitterMs = jitterMs;
    pa.demoWrongRate = wrong;
    pa.demoMissRate = miss;
    pa.waitMode = wait;
    pa.speed = speed;
    pa.countIn = true;
    if (loop) {
        auto tl = pr.timeline();
        pa.loopA.store(0);
        pa.loopB.store(tl->barTimes.size() > 2 ? tl->barTimes[2] : tl->length);
        pa.loopOn = true;
    }
    pr.play();
    std::vector<float> in(block), L(block), R(block);
    uint64_t clock = 0;
    RunResult rr;
    double extra = 0;
    while (clock < (uint64_t)(maxSec * fs)) {
        std::fill(in.begin(), in.end(), 0.f);
        pa.renderDemoInput(in.data(), block);
        an.processOffline(in.data(), block);
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        pa.process(L.data(), R.data(), block, clock);
        clock += block;
        if (pr.takeFinished()) rr.finished = true;
        if (pa.state() == TransportState::Finished) extra += block / fs;
        if (extra > 1.0) break;
        if (loop && pr.passes().size() >= 3) break;
    }
    std::vector<EventResult> res;
    pr.snapshot(res, rr.st);
    rr.passes = pr.passes();
    if (getenv("GS_DEBUG")) {
        auto tl = pr.timeline();
        for (size_t i = 0; i < res.size(); ++i) {
            const auto& r = res[i];
            if (r.judge == Judge::Perfect || r.judge == Judge::Good || r.judge == Judge::Ok) continue;
            std::printf("    event %zu t=%.2fs expected", i, tl->events[i].time);
            for (int m : tl->expectedMidis((int)i)) std::printf(" %d", m);
            std::printf(" -> %s detected %.2f chord %.2f offset %+.1fms\n", judgeName(r.judge), r.detectedMidi, r.chordScore, r.timingMs);
        }
    }
    rr.seconds = clock / fs;
    return rr;
}

static void print(const char* name, const RunResult& r) {
    const auto& s = r.st;
    std::printf("  %-30s notes %3d judged %3d | perfect %3d good %3d ok %2d wrong %2d miss %2d | hit %5.1f%% score %5.1f%% | mean offset %+5.1f ms (|%4.1f|) combo %d%s\n",
                name, s.total, s.judged, s.perfect, s.good, s.ok, s.wrong, s.miss, s.hitRate() * 100, s.score() * 100,
                s.meanTiming(), s.meanAbsTiming(), s.maxCombo, r.finished ? "" : " (not finished!)");
}

int main() {
    const std::string melody = R"(title: Twinkle test
bpm: 100
time: 4/4
3.0 3.0 2.3 2.3 | 1.0 1.0 2.3/2 | 2.1 2.1 2.0 2.0 | 3.2 3.2 3.0/2 |
2.3 2.3 2.1 2.1 | 2.0 2.0 3.2/2 | 2.3 2.3 2.1 2.1 | 2.0 2.0 3.2/2 |
)";
    const std::string fast = R"(title: Eighth-note scale
bpm: 120
time: 4/4
5.3/8 4.0 4.2 4.3 3.0 3.2 2.0 2.1 | 2.1 2.0 3.2 3.0 4.3 4.2 4.0 5.3 | 6.0/8 6.3 5.0 5.2 4.0 4.2 3.0 3.2 | 2.0 2.3 1.0 1.3 1.3 1.0 2.3 2.0 |
)";
    const std::string chords = R"(title: Chord test
bpm: 80
time: 4/4
[G]/2 [G]/2 | [C]/2 [C]/2 | [D]/2 [D]/2 | [Em]/2 [Em]/2 | [Am]/2 [Am]/2 | [C]/2 [D]/2 | [G]/1 |
)";
    std::printf("== End-to-end practice scoring (simulated player) ==\n");
    {
        auto r = runSong(melody, 12, 0, 0, false);
        print("single notes, accurate (+-12ms)", r);
        CHECK(r.finished, "accurate single notes: did not finish");
        CHECK(r.st.hitRate() > 0.95f, "accurate single notes: hit rate %.2f", r.st.hitRate());
        CHECK(std::fabs(r.st.meanTiming()) < 12, "accurate single notes: mean offset %.1f ms", r.st.meanTiming());
    }
    {
        auto r = runSong(melody, 70, 0, 0, false);
        print("single notes, sloppy (+-70ms)", r);
        CHECK(r.st.perfect < r.st.total * 0.75, "too many perfects with sloppy timing");
        CHECK(r.st.hitRate() > 0.75f, "sloppy timing: hit rate %.2f", r.st.hitRate());
    }
    {
        auto r = runSong(melody, 12, 0.3f, 0, false);
        print("single notes, 30% wrong", r);
        float wr = r.st.judged ? (float)r.st.wrong / r.st.judged : 0;
        CHECK(wr > 0.15f && wr < 0.45f, "wrong-note ratio %.2f", wr);
    }
    {
        auto r = runSong(melody, 12, 0, 0.3f, false);
        print("single notes, 30% missed", r);
        float mr = r.st.judged ? (float)r.st.miss / r.st.judged : 0;
        CHECK(mr > 0.15f && mr < 0.45f, "missed ratio %.2f", mr);
    }
    {
        auto r = runSong(fast, 10, 0, 0, false);
        print("eighths @120, accurate", r);
        CHECK(r.st.hitRate() > 0.9f, "fast scale hit rate %.2f", r.st.hitRate());
    }
    {
        auto r = runSong(melody, 12, 0, 0, false, 0.6f);
        print("single notes, 60% speed", r);
        CHECK(r.st.hitRate() > 0.95f, "slow speed hit rate %.2f", r.st.hitRate());
    }
    {
        auto r = runSong(chords, 12, 0, 0, false);
        print("chords, accurate", r);
        CHECK(r.st.hitRate() > 0.9f, "chord hit rate %.2f", r.st.hitRate());
    }
    {
        auto r = runSong(chords, 12, 0.5f, 0, false);
        print("chords, 50% wrong", r);
        CHECK(r.st.wrong > 0, "wrong chords were not detected");
    }
    {
        auto r = runSong(melody, 0, 0, 0, true);
        print("wait mode", r);
        CHECK(r.finished, "wait mode did not finish");
        CHECK(r.st.hitRate() > 0.95f, "wait mode hit rate %.2f", r.st.hitRate());
        std::printf("    (wait mode took %.1f s)\n", r.seconds);
    }
    {
        auto r = runSong(melody, 15, 0, 0, false, 1.f, true, 60);
        std::printf("  A-B loop (first 2 bars): %zu passes recorded:", r.passes.size());
        for (auto& p : r.passes)
            std::printf(" pass %d %.0f%% (judged %d/%d perfect %d miss %d wrong %d)", p.pass, p.stats.score() * 100, p.stats.judged, p.stats.total,
                        p.stats.perfect, p.stats.miss, p.stats.wrong);
        std::printf("\n");
        CHECK(r.passes.size() >= 2, "loop practice recorded no passes");
    }
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
