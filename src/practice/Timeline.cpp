#include "practice/Timeline.h"

#include <algorithm>

namespace gs {

std::shared_ptr<SongTimeline> SongTimeline::build(std::shared_ptr<const Song> song) {
    auto tl = std::make_shared<SongTimeline>();
    tl->song = song;
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
    // Bars and metronome beats
    double end = s.lengthBeats;
    std::vector<BarLine> bars = s.bars;
    if (bars.empty()) {
        for (double b = 0; b < end; b += 4) bars.push_back(BarLine{b, (int)(b / 4) + 1});
    }
    for (size_t bi = 0; bi < bars.size(); ++bi) {
        const double b0 = bars[bi].beat;
        if (b0 >= end - 1e-9) break;
        const TimeSig& ts = s.timeSigAt(b0 + 1e-9);
        const double barLen = ts.num * 4.0 / std::max(1, ts.den);
        const double b1 = bi + 1 < bars.size() ? bars[bi + 1].beat : std::min(end, b0 + barLen);
        // Compound meters (6/8, 9/8, 12/8) click on dotted quarters
        double unit = 4.0 / std::max(1, ts.den);
        if (ts.den == 8 && ts.num % 3 == 0 && ts.num > 3) unit = 1.5;
        tl->barTimes.push_back(s.beatToSec(b0));
        int k = 0;
        for (double b = b0; b < b1 - 1e-6; b += unit, ++k) {
            tl->beatTimes.push_back(s.beatToSec(b));
            tl->beatAccent.push_back(k == 0 ? 1 : 0);
        }
    }
    for (const Section& sec : s.sections) tl->sections.emplace_back(s.beatToSec(sec.beat), sec.name);
    tl->length = s.beatToSec(end);
    if (!tl->events.empty()) tl->length = std::max(tl->length, tl->events.back().time + tl->events.back().dur);
    for (auto& ev : tl->events) ev.bar = tl->barAt(ev.time + 1e-6);
    // Count-in: beat count and beat length of the first bar
    const TimeSig& ts0 = s.timeSigAt(0);
    double unit0 = 4.0 / std::max(1, ts0.den);
    int cnt = ts0.num;
    if (ts0.den == 8 && ts0.num % 3 == 0 && ts0.num > 3) {
        unit0 = 1.5;
        cnt = ts0.num / 3;
    }
    tl->countInBeats = std::clamp(cnt, 2, 8);
    tl->countInBeatSec = s.beatToSec(unit0) - s.beatToSec(0);
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
