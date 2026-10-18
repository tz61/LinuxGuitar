#include <algorithm>
#include <cctype>
#include <cmath>

#include "song/SongInternal.h"

namespace gs {

static constexpr double kEps = 1e-6;

double Song::bpmAt(double beat) const {
    if (tempo.empty()) return 120.0;
    double bpm = tempo.front().bpm;
    for (const TempoPoint& t : tempo) {
        if (t.beat <= beat + kEps) bpm = t.bpm;
        else break;
    }
    return bpm;
}

double Song::beatToSec(double beat) const {
    if (tempo.empty()) return beat * 60.0 / 120.0;
    if (beat <= tempo.front().beat) return (beat - tempo.front().beat) * 60.0 / tempo.front().bpm + 0.0;
    double sec = 0.0;
    for (size_t i = 0; i < tempo.size(); ++i) {
        double b0 = tempo[i].beat;
        double b1 = (i + 1 < tempo.size()) ? tempo[i + 1].beat : beat;
        if (beat < b1) b1 = beat;
        if (b1 > b0) sec += (b1 - b0) * 60.0 / tempo[i].bpm;
        if (i + 1 >= tempo.size() || beat <= tempo[i + 1].beat) break;
    }
    return sec;
}

double Song::secToBeat(double sec) const {
    if (tempo.empty()) return sec * 120.0 / 60.0;
    if (sec <= 0.0) return tempo.front().beat + sec * tempo.front().bpm / 60.0;
    double acc = 0.0;
    for (size_t i = 0; i < tempo.size(); ++i) {
        double b0 = tempo[i].beat;
        bool last = (i + 1 >= tempo.size());
        double segSec = last ? 1e300 : (tempo[i + 1].beat - b0) * 60.0 / tempo[i].bpm;
        if (sec <= acc + segSec) return b0 + (sec - acc) * tempo[i].bpm / 60.0;
        acc += segSec;
    }
    return tempo.back().beat;
}

const TimeSig& Song::timeSigAt(double beat) const {
    static const TimeSig kDefault{};
    if (timeSigs.empty()) return kDefault;
    const TimeSig* ts = &timeSigs.front();
    for (const TimeSig& t : timeSigs) {
        if (t.beat <= beat + kEps) ts = &t;
        else break;
    }
    return *ts;
}

static bool sameNote(const SongNote& a, const SongNote& b) {
    if (a.string >= 0 && b.string >= 0 && a.string != b.string) return false;
    return a.midi == b.midi;
}

void Song::finalize() {
    // ---- tempo / time-signature maps
    for (TempoPoint& t : tempo)
        if (!(t.bpm > 1.0 && t.bpm < 1000.0)) t.bpm = std::clamp(std::isfinite(t.bpm) ? t.bpm : 120.0, 10.0, 999.0);
    std::stable_sort(tempo.begin(), tempo.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.beat < b.beat; });
    {
        std::vector<TempoPoint> t2;
        for (const TempoPoint& t : tempo) {
            TempoPoint p = t;
            if (!std::isfinite(p.beat) || p.beat < 0) p.beat = 0;
            if (!t2.empty() && std::fabs(t2.back().beat - p.beat) < kEps) t2.back() = p;
            else if (!t2.empty() && std::fabs(t2.back().bpm - p.bpm) < 1e-9) continue;
            else t2.push_back(p);
        }
        tempo.swap(t2);
    }
    if (tempo.empty()) tempo.push_back({0.0, 120.0});
    if (tempo.front().beat > kEps) tempo.insert(tempo.begin(), TempoPoint{0.0, tempo.front().bpm});
    tempo.front().beat = 0.0;

    for (TimeSig& t : timeSigs) {
        if (t.num < 1 || t.num > 64) t.num = 4;
        if (t.den != 1 && t.den != 2 && t.den != 4 && t.den != 8 && t.den != 16 && t.den != 32) t.den = 4;
        if (!std::isfinite(t.beat) || t.beat < 0) t.beat = 0;
    }
    std::stable_sort(timeSigs.begin(), timeSigs.end(), [](const TimeSig& a, const TimeSig& b) { return a.beat < b.beat; });
    {
        std::vector<TimeSig> t2;
        for (const TimeSig& t : timeSigs) {
            if (!t2.empty() && std::fabs(t2.back().beat - t.beat) < kEps) t2.back() = t;
            else if (!t2.empty() && t2.back().num == t.num && t2.back().den == t.den) continue;
            else t2.push_back(t);
        }
        timeSigs.swap(t2);
    }
    if (timeSigs.empty()) timeSigs.push_back(TimeSig{});
    timeSigs.front().beat = 0.0;

    for (int s = 0; s < 6; ++s) tuning[s] = std::clamp(tuning[s], 10, 100);
    capo = std::clamp(capo, 0, 24);

    // ---- clean up notes, compute midi
    for (SongEvent& e : events) {
        if (!std::isfinite(e.beat) || e.beat < 0) e.beat = 0;
        if (!std::isfinite(e.durBeats) || e.durBeats <= 0) e.durBeats = 0.25;
        std::vector<SongNote> keep;
        for (SongNote n : e.notes) {
            if (!std::isfinite(n.durBeats) || n.durBeats <= 0) n.durBeats = e.durBeats;
            if (n.string >= 0) {
                if (n.string > 5 || n.fret < 0 || n.fret > 30) continue;
                n.midi = midiOf(n.string, n.fret);
            } else {
                if (n.midi < 0 || n.midi > 127) continue;
            }
            keep.push_back(n);
        }
        e.notes.swap(keep);
    }

    // ---- sort + merge events at the same time
    std::stable_sort(events.begin(), events.end(), [](const SongEvent& a, const SongEvent& b) { return a.beat < b.beat; });
    {
        std::vector<SongEvent> merged;
        merged.reserve(events.size());
        for (SongEvent& e : events) {
            if (!merged.empty() && std::fabs(merged.back().beat - e.beat) < 1e-4) {
                SongEvent& m = merged.back();
                for (const SongNote& n : e.notes) {
                    bool dup = false;
                    for (SongNote& x : m.notes) {
                        if ((n.string >= 0 && x.string == n.string) || (n.string < 0 && x.string < 0 && x.midi == n.midi)) {
                            x.durBeats = std::max(x.durBeats, n.durBeats);
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) m.notes.push_back(n);
                }
                m.durBeats = std::max(m.durBeats, e.durBeats);
                if (m.chordName.empty()) m.chordName = e.chordName;
                if (!e.lyric.empty()) m.lyric = m.lyric.empty() ? e.lyric : m.lyric + " " + e.lyric;
                if (m.strum == Strum::None) m.strum = e.strum;
            } else {
                merged.push_back(std::move(e));
            }
        }
        events.swap(merged);
    }

    // ---- merge ties: fold NF_TieFromPrev notes into the previous note of the same pitch
    for (size_t i = 0; i < events.size(); ++i) {
        SongEvent& e = events[i];
        for (size_t k = 0; k < e.notes.size();) {
            SongNote& n = e.notes[k];
            if (!(n.flags & NF_TieFromPrev)) { ++k; continue; }
            bool done = false;
            for (size_t j = i; j-- > 0 && !done;) {
                SongEvent& p = events[j];
                if (e.beat - p.beat > 64.0) break;
                for (SongNote& pn : p.notes) {
                    if (!sameNote(pn, n)) continue;
                    double newEnd = e.beat + n.durBeats;
                    pn.durBeats = std::max(pn.durBeats, newEnd - p.beat);
                    p.durBeats = std::max(p.durBeats, pn.durBeats);
                    if (pn.string < 0 && n.string >= 0) { pn.string = n.string; pn.fret = n.fret; }
                    done = true;
                    break;
                }
            }
            if (done) {
                e.notes.erase(e.notes.begin() + (long)k);
            } else {
                n.flags &= ~(uint32_t)NF_TieFromPrev;
                ++k;
            }
        }
    }
    events.erase(std::remove_if(events.begin(), events.end(), [](const SongEvent& e) { return e.notes.empty(); }), events.end());

    // ---- automatic fingering
    bool needFinger = false;
    for (const SongEvent& e : events)
        for (const SongNote& n : e.notes)
            if (n.string < 0) needFinger = true;
    if (needFinger) autoFinger(*this);
    for (SongEvent& e : events) {
        std::vector<SongNote> keep;
        for (SongNote& n : e.notes) {
            if (n.string < 0 || n.string > 5 || n.fret < 0 || n.fret > 30) continue;
            bool dup = false;
            for (const SongNote& x : keep)
                if (x.string == n.string) dup = true;
            if (dup) continue;
            n.midi = midiOf(n.string, n.fret);
            keep.push_back(n);
        }
        // sort notes from string 1 to string 6 for display
        std::sort(keep.begin(), keep.end(), [](const SongNote& a, const SongNote& b) { return a.string < b.string; });
        e.notes.swap(keep);
        double d = 0;
        for (const SongNote& n : e.notes) d = std::max(d, n.durBeats);
        if (d > 0 && e.durBeats <= 0) e.durBeats = d;
    }
    events.erase(std::remove_if(events.begin(), events.end(), [](const SongEvent& e) { return e.notes.empty(); }), events.end());

    std::stable_sort(sections.begin(), sections.end(), [](const Section& a, const Section& b) { return a.beat < b.beat; });

    // ---- length + bar lines
    double rawEnd = 0.0;
    for (const SongEvent& e : events) {
        rawEnd = std::max(rawEnd, e.beat + e.durBeats);
        for (const SongNote& n : e.notes) rawEnd = std::max(rawEnd, e.beat + std::min(n.durBeats, 64.0));
    }
    rawEnd = std::max(rawEnd, lengthBeats);  // loaders may preset the length (e.g. trailing rests)
    bars.clear();
    double b = 0.0;
    size_t tsIdx = 0;
    int number = 1;
    const size_t kMaxBars = 100000;
    do {
        while (tsIdx + 1 < timeSigs.size() && timeSigs[tsIdx + 1].beat <= b + kEps) ++tsIdx;
        bars.push_back(BarLine{b, number++});
        const TimeSig& ts = timeSigs[tsIdx];
        double len = ts.num * 4.0 / ts.den;
        double next = b + len;
        if (tsIdx + 1 < timeSigs.size() && timeSigs[tsIdx + 1].beat < next - kEps && timeSigs[tsIdx + 1].beat > b + kEps)
            next = timeSigs[tsIdx + 1].beat;  // time-signature change off a bar line: start a new bar there
        b = next;
    } while (b < rawEnd - 1e-4 && bars.size() < kMaxBars);
    lengthBeats = b;
}

std::string midiToName(int midi) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (midi < 0 || midi > 127) return "?";
    return std::string(kNames[midi % 12]) + std::to_string(midi / 12 - 1);
}

int nameToMidi(const std::string& name0) {
    std::string s = detail::trim(name0);
    if (s.empty()) return -1;
    static const int kBase[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    char L = (char)std::toupper((unsigned char)s[0]);
    if (L < 'A' || L > 'G') return -1;
    int pc = kBase[L - 'A'];
    size_t i = 1;
    for (;;) {
        if (i < s.size() && s[i] == '#') { ++pc; ++i; }
        else if (i < s.size() && s[i] == 'b') { --pc; ++i; }
        else if (s.compare(i, 3, "\xE2\x99\xAF") == 0) { ++pc; i += 3; }  // ♯
        else if (s.compare(i, 3, "\xE2\x99\xAD") == 0) { --pc; i += 3; }  // ♭
        else break;
    }
    if (i >= s.size()) return -1;
    int oct = 0;
    if (!detail::parseIntStrict(s.substr(i), oct) || oct < -1 || oct > 9) return -1;
    int m = (oct + 1) * 12 + pc;
    return (m >= 0 && m <= 127) ? m : -1;
}

}  // namespace gs
