#pragma once
// Song -> timeline in seconds. Read-only after build(); shared by the audio thread, evaluator and UI.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "song/Song.h"

namespace gs {

struct TimelineNote {
    int string = 0;   // 0..5
    int fret = 0;
    int midi = 0;
    float dur = 0;    // seconds
    uint32_t flags = 0;
    bool playable() const { return !(flags & (NF_TieFromPrev | NF_Dead)); }
};

struct TimelineEvent {
    double time = 0;   // seconds (at 100% speed)
    double dur = 0;    // seconds
    double beat = 0;   // beats
    int firstNote = 0, noteCount = 0;
    bool judged = false;  // has notes to play (not only ties / dead notes)
    bool legato = false;  // all hammer-ons/pull-offs/slides: no clear onset required
    int bar = 0;          // bar index (0-based)
    Strum strum = Strum::None;
    std::string chordName, lyric;
};

struct SongTimeline {
    std::shared_ptr<const Song> song;
    std::vector<TimelineEvent> events;
    std::vector<TimelineNote> notes;
    std::vector<double> beatTimes;       // metronome beats
    std::vector<uint8_t> beatAccent;     // 1 = first beat of a bar
    std::vector<double> barTimes;        // bar start times
    std::vector<std::pair<double, std::string>> sections;
    double length = 0;                   // seconds; the run ends here
    double countInBeatSec = 0.6;         // count-in click spacing (metronome beat of the first bar)
    int judgedCount = 0;
    int endBeats = 0;                    // what build() was given

    // endBeats > 0: the run ends that many metronome beats after the last note, instead of at the end of the last bar
    static std::shared_ptr<SongTimeline> build(std::shared_ptr<const Song> song, int endBeats = 0);
    int eventAt(double t) const;  // first event with time >= t
    int barAt(double t) const;
    std::vector<int> expectedMidis(int ev) const;  // notes that must be played
};

}  // namespace gs
