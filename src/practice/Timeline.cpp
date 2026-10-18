#include "practice/Timeline.h"

#include <algorithm>

namespace gs {

// Metronome beat (in quarter-note beats) for a time signature: compound meters (6/8, 9/8, 12/8) click on dotted quarters
static double clickUnit(const TimeSig& ts) {
    if (ts.den == 8 && ts.num % 3 == 0 && ts.num > 3) return 1.5;
    return 4.0 / std::max(1, ts.den);
}

std::shared_ptr<SongTimeline> SongTimeline::build(std::shared_ptr<const Song> song, int endBeats) {
    auto tl = std::make_shared<SongTimeline>();
    tl->song = song;
    tl->endBeats = endBeats;
    const Song& s = *song;
    for (const SongEvent& e : s.events) {
        TimelineEvent te;
        te.beat = e.beat;
        te.time = s.beatToSec(e.beat);
        te.dur = std::max(0.02, s.beatToSec(e.beat + e.durBeats) - te.time);
        te.firstNote = (int)tl->notes.size();
        te.strum = e.strum;
        te.chordName = e.chordName;
        te.lyric = e.lyric;
        bool any = false, allLegato = true;
        for (const SongNote& n : e.notes) {
            if (n.string < 0 || n.string > 5) continue;
            TimelineNote tn;
            tn.string = n.string;
            tn.fret = n.fret;
            tn.midi = n.midi >= 0 ? n.midi : s.midiOf(n.string, n.fret);
            tn.dur = (float)std::max(0.02, s.beatToSec(e.beat + n.durBeats) - te.time);
            tn.flags = n.flags;
            tl->notes.push_back(tn);
            if (tn.playable()) {
                any = true;
                if (!(n.flags & (NF_Hammer | NF_Pull | NF_Slide))) allLegato = false;
            }
        }
        te.noteCount = (int)tl->notes.size() - te.firstNote;
        if (te.noteCount == 0) continue;
        te.judged = any;
        te.legato = any && allLegato;
        if (te.judged) ++tl->judgedCount;
        tl->events.push_back(std::move(te));
    }
    // Where the run ends: the end of the last bar, or endBeats metronome beats after the last note
    double end = s.lengthBeats;
    int lastJudged = -1;
    for (int i = (int)tl->events.size() - 1; i >= 0 && lastJudged < 0; --i)
        if (tl->events[i].judged) lastJudged = i;
    const bool shortEnd = endBeats > 0 && lastJudged >= 0;
    if (shortEnd) {
        const double b = tl->events[lastJudged].beat;
        end = b + endBeats * clickUnit(s.timeSigAt(b + 1e-9));
        // Ties / dead notes after the end would never be reached (the run only finishes after the last event)
        const double endSec = s.beatToSec(end);
        while (!tl->events.empty() && tl->events.back().time >= endSec - 1e-9) tl->events.pop_back();
    }
    // Bars and metronome beats; the bars continue past the song when the ending reaches beyond its last bar
    std::vector<BarLine> bars = s.bars;
    if (bars.empty()) {
        for (double b = 0; b < s.lengthBeats; b += 4) bars.push_back(BarLine{b, (int)(b / 4) + 1});
    }
    while (!bars.empty()) {
        const TimeSig& ts = s.timeSigAt(bars.back().beat + 1e-9);
        const double next = bars.back().beat + ts.num * 4.0 / std::max(1, ts.den);
        if (next >= end - 1e-9) break;
        bars.push_back(BarLine{next, bars.back().number + 1});
    }
    for (size_t bi = 0; bi < bars.size(); ++bi) {
        const double b0 = bars[bi].beat;
        if (b0 >= end - 1e-9) break;
        const TimeSig& ts = s.timeSigAt(b0 + 1e-9);
        const double barLen = ts.num * 4.0 / std::max(1, ts.den);
        const double b1 = std::min(end, bi + 1 < bars.size() ? bars[bi + 1].beat : b0 + barLen);
        const double unit = clickUnit(ts);
        tl->barTimes.push_back(s.beatToSec(b0));
        int k = 0;
        for (double b = b0; b < b1 - 1e-6; b += unit, ++k) {
            tl->beatTimes.push_back(s.beatToSec(b));
            tl->beatAccent.push_back(k == 0 ? 1 : 0);
        }
    }
    for (const Section& sec : s.sections) tl->sections.emplace_back(s.beatToSec(sec.beat), sec.name);
    tl->length = s.beatToSec(end);
    if (!shortEnd && !tl->events.empty())
        tl->length = std::max(tl->length, tl->events.back().time + tl->events.back().dur);
    for (auto& ev : tl->events) ev.bar = tl->barAt(ev.time + 1e-6);
    // Count-in clicks use the metronome beat of the first bar (the number of clicks is a setting)
    tl->countInBeatSec = s.beatToSec(clickUnit(s.timeSigAt(0))) - s.beatToSec(0);
    if (tl->countInBeatSec <= 0) tl->countInBeatSec = 0.5;
    return tl;
}

int SongTimeline::eventAt(double t) const {
    auto it = std::lower_bound(events.begin(), events.end(), t - 1e-9,
                               [](const TimelineEvent& e, double v) { return e.time < v; });
    return (int)(it - events.begin());
}

int SongTimeline::barAt(double t) const {
    auto it = std::upper_bound(barTimes.begin(), barTimes.end(), t);
    return std::max(0, (int)(it - barTimes.begin()) - 1);
}

std::vector<int> SongTimeline::expectedMidis(int ev) const {
    std::vector<int> out;
    if (ev < 0 || ev >= (int)events.size()) return out;
    const auto& e = events[ev];
    for (int i = 0; i < e.noteCount; ++i) {
        const auto& n = notes[e.firstNote + i];
        if (n.playable()) out.push_back(n.midi);
    }
    return out;
}

}  // namespace gs
