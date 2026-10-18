// ASCII tab import (the common e|---0---| format found online). Rhythm is inferred from column spacing (approximate).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "song/SongInternal.h"

namespace gs {

using namespace detail;

namespace {

const int kStdTuning[6] = {64, 59, 55, 50, 45, 40};  // string 1..6

std::string normalizeDashes(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (i + 2 < s.size() && (unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0x94 || (unsigned char)s[i + 2] == 0x93 || (unsigned char)s[i + 2] == 0x92)) {
            o += '-';
            i += 2;
        } else if (s[i] == '\t') {
            o += "    ";
        } else {
            o += s[i];
        }
    }
    return o;
}

int labelPitchClass(const std::string& l) {
    if (l.empty()) return -1;
    static const int kBase[7] = {9, 11, 0, 2, 4, 5, 7};
    char c = (char)std::toupper((unsigned char)l[0]);
    if (c < 'A' || c > 'G') return -1;
    int pc = kBase[c - 'A'];
    for (size_t i = 1; i < l.size(); ++i) {
        if (l[i] == '#') ++pc;
        else if (l[i] == 'b') --pc;
        else return -1;
    }
    return ((pc % 12) + 12) % 12;
}

struct TabLine {
    std::string label;
    std::string body;
};

bool isTechChar(char c) {
    return std::strchr("hpbr/\\~xXs()<>*=^v.'\"t+|:", c) != nullptr;
}

bool parseTabLine(const std::string& raw, TabLine& out) {
    std::string line = normalizeDashes(raw);
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    std::string label;
    size_t j = i;
    while (j < line.size() && label.size() < 3 && (std::isalpha((unsigned char)line[j]) || line[j] == '#')) label += line[j++];
    if (!label.empty() && labelPitchClass(label) < 0) return false;  // e.g. "PM---", not a string line
    while (j < line.size() && line[j] == ' ' && j - i < 6) ++j;
    if (j < line.size() && (line[j] == '|' || line[j] == ':')) {
        ++j;
    } else if (!label.empty() && (j >= line.size() || line[j] != '-')) {
        return false;
    } else if (label.empty() && (i >= line.size() || (line[i] != '-' && line[i] != '|'))) {
        return false;
    }
    std::string body = line.substr(j);
    // strip trailing remarks (e.g. "x2", "repeat")
    size_t last = body.find_last_of("-|0123456789");
    if (last == std::string::npos) return false;
    body = body.substr(0, last + 1);
    int dashes = 0, digits = 0, bad = 0;
    for (char c : body) {
        if (c == '-') ++dashes;
        else if (std::isdigit((unsigned char)c)) ++digits;
        else if (c == ' ') ++bad;
        else if (!isTechChar(c)) bad += 2;
    }
    if (dashes < 4 || dashes * 10 < (int)body.size() * 4 || bad * 10 > (int)body.size() * 2) return false;
    out.label = label;
    out.body = body;
    return true;
}

struct Onset { int col; int string; int fret; uint32_t flags; bool twoDigit; };

void parseTuningText(const std::string& text, int tuning[6]) {
    std::string t = toLower(text);
    if (t.find("drop d") != std::string::npos || t.find("dropd") != std::string::npos) {
        std::copy(kStdTuning, kStdTuning + 6, tuning);
        tuning[5] = 38;
        return;
    }
    if (t.find("half step down") != std::string::npos || t.find("eb standard") != std::string::npos) {
        for (int s = 0; s < 6; ++s) tuning[s] = kStdTuning[s] - 1;
        return;
    }
    if (t.find("full step down") != std::string::npos || t.find("whole step down") != std::string::npos ||
        t.find("d standard") != std::string::npos) {
        for (int s = 0; s < 6; ++s) tuning[s] = kStdTuning[s] - 2;
        return;
    }
    // note-name sequence (low -> high): "E A D G B E", "EADGBE", "D A D G A D", "Eb Ab Db Gb Bb Eb"
    std::vector<int> pcs;
    for (size_t i = 0; i < text.size();) {
        char c = (char)std::toupper((unsigned char)text[i]);
        if (c >= 'A' && c <= 'G') {
            std::string nm(1, c);
            size_t k = i + 1;
            while (k < text.size() && (text[k] == '#' || text[k] == 'b')) nm += text[k++];
            // don't mistake letters inside words for note names (e.g. the 'g' in "Tuning")
            bool wordChar = k < text.size() && std::isalpha((unsigned char)text[k]) && !(text[k] >= 'A' && text[k] <= 'G') &&
                            !(text[k] >= 'a' && text[k] <= 'g');
            if (!wordChar) pcs.push_back(labelPitchClass(nm));
            i = k;
        } else {
            ++i;
        }
    }
    if (pcs.size() != 6) return;
    for (int k = 0; k < 6; ++k) {
        int s = 5 - k;  // the text lists low -> high
        int base = kStdTuning[s];
        int best = base, bestD = 99;
        for (int m = base - 7; m <= base + 5; ++m)
            if (((m % 12) + 12) % 12 == pcs[(size_t)k] && std::abs(m - base) < bestD) { bestD = std::abs(m - base); best = m; }
        tuning[s] = best;
    }
}

int findNumberAfter(const std::string& lower, const std::string& key, int lo, int hi) {
    size_t p = lower.find(key);
    while (p != std::string::npos) {
        size_t i = p + key.size();
        for (;;) {
            if (i < lower.size() && (lower[i] == ' ' || lower[i] == ':' || lower[i] == '=' || lower[i] == '-')) ++i;
            else if (lower.compare(i, 2, "on") == 0 || lower.compare(i, 2, "at") == 0) i += 2;
            else break;
        }
        int v = 0, nd = 0;
        while (i < lower.size() && std::isdigit((unsigned char)lower[i]) && nd < 4) { v = v * 10 + (lower[i] - '0'); ++i; ++nd; }
        if (nd > 0 && v >= lo && v <= hi) return v;
        p = lower.find(key, p + 1);
    }
    return -1;
}

}  // namespace

bool parseAsciiTab(const std::string& text, Song& out, std::string& err) {
    out = Song();
    std::vector<std::string> lines = splitLines(text);
    // ---- find all string lines, grouped into runs of consecutive lines
    std::vector<std::vector<TabLine>> groups;
    std::vector<TabLine> cur;
    std::string metaText;  // non-tab lines, searched for tempo/capo/tuning
    for (const std::string& l : lines) {
        TabLine tl;
        if (parseTabLine(l, tl)) {
            cur.push_back(tl);
        } else {
            if (!cur.empty()) { groups.push_back(cur); cur.clear(); }
            metaText += l + "\n";
        }
    }
    if (!cur.empty()) groups.push_back(cur);

    // split into blocks of 6 strings (or 4/5/7 strings)
    std::vector<std::vector<TabLine>> blocks;
    for (auto& g : groups) {
        size_t n = g.size();
        if (n == 4 || n == 5 || n == 6 || n == 7) blocks.push_back(g);
        else if (n > 7 && n % 6 == 0)
            for (size_t k = 0; k < n; k += 6) blocks.emplace_back(g.begin() + (long)k, g.begin() + (long)k + 6);
        else if (n > 7) {
            // split where labels repeat
            size_t start = 0;
            for (size_t k = 1; k <= n; ++k) {
                if (k == n || (!g[k].label.empty() && g[k].label == g[start].label)) {
                    if (k - start >= 4 && k - start <= 7) blocks.emplace_back(g.begin() + (long)start, g.begin() + (long)k);
                    start = k;
                }
            }
        }
    }
    // keep only the most common string count
    std::map<size_t, int> cnt;
    for (auto& b : blocks) cnt[b.size()]++;
    if (blocks.empty()) { err = tr("No ASCII tab found (expected lines like e|---0---|)"); return false; }
    size_t nStr = 6;
    {
        int best = -1;
        for (auto& kv : cnt)
            if (kv.second > best || (kv.second == best && kv.first == 6)) { best = kv.second; nStr = kv.first; }
        blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [&](const std::vector<TabLine>& b) { return b.size() != nStr; }),
                     blocks.end());
    }

    // ---- metadata
    std::string lower = toLower(metaText);
    int bpm = findNumberAfter(lower, "bpm", 30, 300);
    if (bpm < 0) bpm = findNumberAfter(lower, "tempo", 30, 300);
    if (bpm < 0) {
        // "120 bpm"
        size_t p = lower.find("bpm");
        if (p != std::string::npos) {
            size_t e = p;
            while (e > 0 && lower[e - 1] == ' ') --e;
            size_t s = e;
            while (s > 0 && std::isdigit((unsigned char)lower[s - 1])) --s;
            int v;
            if (s < e && parseIntStrict(lower.substr(s, e - s), v) && v >= 30 && v <= 300) bpm = v;
        }
    }
    if (bpm < 0) bpm = 80;
    int capo = findNumberAfter(lower, "capo", 0, 12);
    if (capo > 0) out.capo = capo;
    int barBeatsNum = 4, den = 4;
    if (lower.find("3/4") != std::string::npos) barBeatsNum = 3;
    else if (lower.find("6/8") != std::string::npos) { barBeatsNum = 6; den = 8; }
    const double barBeats = barBeatsNum * 4.0 / den;
    for (const std::string& l : splitLines(metaText)) {
        std::string ll = toLower(l);
        if (ll.find("title:") == 0 || ll.find("song:") == 0) out.title = trim(l.substr(l.find(':') + 1));
        else if (ll.find("artist:") == 0 || ll.find("band:") == 0) out.artist = trim(l.substr(l.find(':') + 1));
        size_t tp = ll.find("tuning");
        if (tp != std::string::npos) parseTuningText(l.substr(tp + 6), out.tuning);
    }

    // ---- string order and tuning (from the labels)
    bool reversed = false;
    {
        const auto& b0 = blocks.front();
        if (!b0.front().label.empty() && !b0.back().label.empty()) {
            if (b0.front().label == "E" && b0.back().label == "e") reversed = true;
            if (nStr == 6 && b0.front().label == "E" && b0[1].label == "A") reversed = true;
        }
        bool allLabels = true;
        for (const TabLine& t : b0)
            if (labelPitchClass(t.label) < 0) allLabels = false;
        static const int kStd[6] = {64, 59, 55, 50, 45, 40};
        static const int kBass[6] = {43, 38, 33, 28, 23, 18};
        bool tuningFromHeader = lower.find("tuning") != std::string::npos;
        if (nStr <= 5) std::copy(kBass, kBass + 6, out.tuning);
        if (allLabels && !tuningFromHeader && nStr == 6) {
            for (size_t k = 0; k < nStr; ++k) {
                int s = reversed ? (int)(nStr - 1 - k) : (int)k;
                int pc = labelPitchClass(b0[k].label);
                int base = kStd[s], best = base, bestD = 99;
                for (int m = base - 7; m <= base + 5; ++m)
                    if (((m % 12) + 12) % 12 == pc && std::abs(m - base) < bestD) { bestD = std::abs(m - base); best = m; }
                out.tuning[s] = best;
            }
        }
    }

    // ---- parse notes and timing block by block
    out.tempo.push_back({0, (double)bpm});
    out.timeSigs.push_back({0, barBeatsNum, den});
    double timeCursor = 0;
    size_t noteTotal = 0;
    for (const auto& blk : blocks) {
        size_t maxLen = 0;
        for (const TabLine& t : blk) maxLen = std::max(maxLen, t.body.size());
        std::vector<Onset> onsets;
        std::vector<int> barCount(maxLen + 1, 0);
        for (size_t k = 0; k < blk.size(); ++k) {
            int s = reversed ? (int)(nStr - 1 - k) : (int)k;  // 0 = highest string
            const std::string& b = blk[k].body;
            int lastOnsetIdx = -1;
            for (size_t c = 0; c < b.size(); ++c) {
                char ch = b[c];
                if (ch == '|') { barCount[c]++; continue; }
                if (std::isdigit((unsigned char)ch)) {
                    int fret = ch - '0';
                    bool two = false;
                    if (c + 1 < b.size() && std::isdigit((unsigned char)b[c + 1])) {
                        int f2 = fret * 10 + (b[c + 1] - '0');
                        if (f2 <= 24) { fret = f2; two = true; }
                    }
                    char prev = c > 0 ? b[c - 1] : '-';
                    if ((prev == 'b' || prev == 'r') && lastOnsetIdx >= 0) {
                        onsets[(size_t)lastOnsetIdx].flags |= NF_Bend;  // bend target, not a new pluck
                    } else {
                        uint32_t fl = 0;
                        if (prev == 'h') fl |= NF_Hammer;
                        else if (prev == 'p') fl |= NF_Pull;
                        else if (prev == '/' || prev == '\\' || prev == 's') fl |= NF_Slide;
                        else if (prev == '<') fl |= NF_Harmonic;
                        else if (prev == '(') fl |= NF_Ghost;
                        onsets.push_back({(int)c, s, fret, fl, two});
                        lastOnsetIdx = (int)onsets.size() - 1;
                    }
                    if (two) ++c;
                } else if (ch == 'x' || ch == 'X') {
                    onsets.push_back({(int)c, s, 0, NF_Dead, false});
                    lastOnsetIdx = (int)onsets.size() - 1;
                } else if (ch == '~' && lastOnsetIdx >= 0) {
                    onsets[(size_t)lastOnsetIdx].flags |= NF_Vibrato;
                } else if (ch == 'b' && lastOnsetIdx >= 0) {
                    onsets[(size_t)lastOnsetIdx].flags |= NF_Bend;
                }
            }
        }
        // a note in the column right after a two-digit fret joins the same time slot
        std::sort(onsets.begin(), onsets.end(), [](const Onset& a, const Onset& b) { return a.col < b.col; });
        std::set<int> twoDigitCols;
        for (const Onset& o : onsets)
            if (o.twoDigit) twoDigitCols.insert(o.col);
        for (Onset& o : onsets)
            if (!o.twoDigit && twoDigitCols.count(o.col - 1)) {
                bool clash = false;
                for (const Onset& p : onsets)
                    if (p.col == o.col - 1 && p.string == o.string) clash = true;
                if (!clash) o.col -= 1;
            }
        std::vector<int> cols;
        for (const Onset& o : onsets)
            if (cols.empty() || cols.back() != o.col) cols.push_back(o.col);
        std::sort(cols.begin(), cols.end());
        cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
        // bar line: at least half of the strings have '|' in this column
        std::vector<int> bounds{-1};
        for (size_t c = 0; c < barCount.size(); ++c)
            if (barCount[c] * 2 >= (int)blk.size()) bounds.push_back((int)c);
        bounds.push_back((int)maxLen);
        // most common spacing overall
        std::map<int, int> gapHist;
        for (size_t i = 1; i < cols.size(); ++i) gapHist[cols[i] - cols[i - 1]]++;
        int modeGap = 2;
        {
            int best = 0;
            for (auto& kv : gapHist)
                if (kv.first >= 1 && kv.second > best) { best = kv.second; modeGap = kv.first; }
        }
        std::map<int, double> colBeat;  // column -> beat
        for (size_t bi = 0; bi + 1 < bounds.size(); ++bi) {
            int a = bounds[bi], b = bounds[bi + 1];
            std::vector<int> segCols;
            for (int c : cols)
                if (c > a && c < b) segCols.push_back(c);
            int width = b - a - 1;
            if (segCols.empty()) {
                if (width >= 8) timeCursor += barBeats;  // empty bar = whole rest
                continue;
            }
            const size_t K = segCols.size();
            if (K == 1) {
                colBeat[segCols[0]] = timeCursor;
                timeCursor += barBeats;
                continue;
            }
            std::map<int, int> hist;
            for (size_t i = 1; i < K; ++i) hist[segCols[i] - segCols[i - 1]]++;
            int g = modeGap;
            {
                int best = 0;
                for (auto& kv : hist)
                    if (kv.second > best) { best = kv.second; g = kv.first; }
                if (best <= 1) g = hist.begin()->first;  // no clear mode: use the smallest spacing
            }
            g = std::max(1, g);
            double unitBeats;
            double segBeats = barBeats;
            int lastUnits = (int)std::lround((double)(segCols.back() - segCols.front()) / g);
            if (K <= 16) {
                int units = std::max((int)K, (int)std::lround((double)(b - segCols.front()) / g));
                double raw = barBeats / units;
                static const double kSnap[5] = {2, 1, 0.5, 0.25, 0.125};
                unitBeats = 0.125;
                double bestErr = 1e9;
                for (double sv : kSnap) {
                    double e = std::fabs(std::log(sv / raw));
                    if (e < bestErr) { bestErr = e; unitBeats = sv; }
                }
                while (lastUnits * unitBeats >= barBeats - 1e-9 && unitBeats > 0.125) unitBeats /= 2;
                if (lastUnits * unitBeats >= barBeats - 1e-9) segBeats = std::ceil((lastUnits * unitBeats + unitBeats) / barBeats) * barBeats;
            } else {
                unitBeats = 0.5;
                segBeats = std::max(barBeats, std::ceil((lastUnits * unitBeats + unitBeats) / barBeats - 1e-9) * barBeats);
            }
            double prev = -1;
            for (int c : segCols) {
                double pos = std::lround((double)(c - segCols.front()) / g) * unitBeats;
                if (pos <= prev + 1e-9) pos = prev + unitBeats / 2;
                prev = pos;
                colBeat[c] = timeCursor + pos;
            }
            timeCursor += std::max(segBeats, prev + unitBeats / 2);
        }
        // generate events
        std::map<double, SongEvent> evs;
        for (const Onset& o : onsets) {
            auto it = colBeat.find(o.col);
            if (it == colBeat.end()) continue;
            SongEvent& e = evs[it->second];
            e.beat = it->second;
            SongNote n;
            if (o.string <= 5) {
                n.string = o.string;
                n.fret = o.fret;
            } else {
                n.midi = 35 + o.fret + out.capo;  // 7th string (B1), left to auto-fingering
            }
            n.flags = o.flags;
            e.notes.push_back(n);
            if (++noteTotal > kMaxNotes) { err = tr("Too many notes"); return false; }
        }
        std::vector<double> beats;
        for (auto& kv : evs) beats.push_back(kv.first);
        for (size_t i = 0; i < beats.size(); ++i) {
            SongEvent& e = evs[beats[i]];
            double next = i + 1 < beats.size() ? beats[i + 1] : timeCursor;
            e.durBeats = std::max(0.125, std::min(next - e.beat, 4.0));
            for (SongNote& n : e.notes) n.durBeats = e.durBeats;
            out.events.push_back(e);
        }
    }
    out.lengthBeats = timeCursor;
    out.finalize();
    if (out.events.empty()) { err = tr("No notes recognized in the tab"); return false; }
    return true;
}

}  // namespace gs
