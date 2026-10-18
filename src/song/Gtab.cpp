// .gtab text scores: parser + writer (format described in Song.h)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

#include "song/SongInternal.h"

namespace gs {

using namespace detail;

namespace {

std::string stripComment(const std::string& line) {
    bool inQ = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') inQ = !inQ;
        if (c == '#' && !inQ && (i == 0 || std::isspace((unsigned char)line[i - 1]))) return line.substr(0, i);
    }
    return line;
}

bool parseHeaderLine(const std::string& line, std::string& key, std::string& val) {
    size_t i = 0;
    while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
    size_t k0 = i;
    if (i >= line.size() || !(std::isalpha((unsigned char)line[i]) || line[i] == '_')) return false;
    while (i < line.size() && (std::isalnum((unsigned char)line[i]) || line[i] == '_')) ++i;
    size_t k1 = i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i < line.size() && line[i] == ':') ++i;
    else if (line.compare(i, 3, "\xEF\xBC\x9A") == 0) i += 3;  // full-width colon (U+FF1A)
    else return false;
    key = toLower(line.substr(k0, k1 - k0));
    val = trim(line.substr(i));
    return true;
}

// Duration string -> beats. i points just after the '/'. Returns the number of characters used, 0 = failure
size_t parseDuration(const std::string& s, size_t i, double& beats) {
    size_t j = i;
    int den = 0;
    while (j < s.size() && std::isdigit((unsigned char)s[j]) && j - i < 3) den = den * 10 + (s[j++] - '0');
    if (j == i) return 0;
    if (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32 && den != 64) return 0;
    double b = 4.0 / den;
    if (j < s.size() && s[j] == '.') {
        ++j;
        if (j < s.size() && s[j] == '.') { ++j; b *= 1.75; }
        else b *= 1.5;
    }
    if (j < s.size() && s[j] == 't') { ++j; b *= 2.0 / 3.0; }
    beats = b;
    return j - i;
}

struct Tok { std::string text; int line; };

std::vector<Tok> tokenize(const std::vector<std::string>& lines, size_t firstLine) {
    std::vector<Tok> toks;
    for (size_t li = firstLine; li < lines.size(); ++li) {
        std::string l = stripComment(lines[li]);
        size_t i = 0;
        while (i < l.size()) {
            unsigned char c = (unsigned char)l[i];
            if (std::isspace(c)) { ++i; continue; }
            if (c == '"') {
                size_t j = l.find('"', i + 1);
                if (j == std::string::npos) j = l.size();
                toks.push_back({l.substr(i, j == l.size() ? std::string::npos : j - i + 1), (int)li + 1});
                i = j + 1;
                continue;
            }
            if (c == '@') {  // section marker: may contain '|' ("English|Chinese"), ends at whitespace
                size_t j = i;
                while (j < l.size() && !std::isspace((unsigned char)l[j])) ++j;
                toks.push_back({l.substr(i, j - i), (int)li + 1});
                i = j;
                continue;
            }
            if (c == '|') { toks.push_back({"|", (int)li + 1}); ++i; continue; }
            if (c == '{') {
                size_t j = l.find('}', i);
                if (j == std::string::npos) j = l.size() - 1;
                toks.push_back({l.substr(i, j - i + 1), (int)li + 1});
                i = j + 1;
                continue;
            }
            size_t j = i;
            int depth = 0;
            while (j < l.size()) {
                char d = l[j];
                if (d == '[') ++depth;
                if (d == ']') --depth;
                if (depth <= 0 && (std::isspace((unsigned char)d) || d == '|' || d == '"' || d == '{')) break;
                ++j;
            }
            toks.push_back({l.substr(i, j - i), (int)li + 1});
            i = j;
        }
    }
    return toks;
}

// Splits a bilingual header line "English|Chinese" (same rule as gs::loc(): the part after the first '|'
// contains non-ASCII text; a trailing '|' marks an explicitly empty Chinese side). Returns false for plain text.
bool splitBilingual(const std::string& s, std::string& en, std::string& zh) {
    const size_t bar = s.find('|');
    if (bar == std::string::npos) return false;
    const std::string right = s.substr(bar + 1);
    bool nonAscii = false;
    for (unsigned char c : right)
        if (c >= 0x80) nonAscii = true;
    if (!nonAscii && !right.empty()) return false;
    en = trim(s.substr(0, bar));
    zh = trim(right);
    return true;
}

std::string joinLines(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? "\n" : "") + v[i];
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

bool parseShape(const std::string& sh, int f[6]) {
    std::vector<std::string> parts;
    if (sh.find(',') != std::string::npos) {
        std::string cur;
        for (char c : sh) {
            if (c == ',') { parts.push_back(trim(cur)); cur.clear(); }
            else cur += c;
        }
        parts.push_back(trim(cur));
    } else {
        for (char c : sh)
            if (!std::isspace((unsigned char)c)) parts.push_back(std::string(1, c));
    }
    if (parts.size() != 6) return false;
    for (int k = 0; k < 6; ++k) {
        const std::string& p = parts[k];
        if (p == "x" || p == "X" || p == "-") { f[k] = -1; continue; }
        int v;
        if (!parseIntStrict(p, v) || v < 0 || v > 24) return false;
        f[k] = v;
    }
    return true;
}

}  // namespace

bool parseGtab(const std::string& text, Song& out, std::string& err) {
    out = Song();
    err.clear();
    std::vector<std::string> lines = splitLines(text);
    std::string warn;
    std::vector<std::string> descLines;
    double bpm = 80;
    TimeSig ts0;
    size_t li = 0;
    // skip a UTF-8 BOM
    if (!lines.empty() && startsWith(lines[0], "\xEF\xBB\xBF")) lines[0] = lines[0].substr(3);
    for (; li < lines.size(); ++li) {
        std::string l = trim(stripComment(lines[li]));
        if (l.empty()) continue;
        if (l == "---") { ++li; break; }
        std::string key, val;
        if (!parseHeaderLine(l, key, val)) break;
        if (key == "title") out.title = val;
        else if (key == "artist") out.artist = val;
        else if (key == "album") out.album = val;
        else if (key == "desc" || key == "description") descLines.push_back(val);
        else if (key == "bpm" || key == "tempo") {
            double v;
            if (!parseDoubleStrict(val, v) || v < 10 || v > 999) { err = strFormat(tr("Line %d: invalid bpm"), (int)li + 1); return false; }
            bpm = v;
        } else if (key == "time") {
            int n = 0, d = 0;
            if (std::sscanf(val.c_str(), "%d/%d", &n, &d) != 2 || n < 1 || n > 32 || (d != 2 && d != 4 && d != 8 && d != 16 && d != 1)) {
                err = strFormat(tr("Line %d: invalid time signature (e.g. 4/4)"), (int)li + 1);
                return false;
            }
            ts0.num = n;
            ts0.den = d;
        } else if (key == "tuning") {
            std::string v = toLower(val);
            v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
            if (v == "standard" || v == "\xE6\xA0\x87\xE5\x87\x86") continue;  // "standard" in English or Chinese
            if (v == "dropd") { int t[6] = {64, 59, 55, 50, 45, 38}; std::copy(t, t + 6, out.tuning); continue; }
            std::istringstream is(val);
            std::vector<int> ms;
            std::string nm;
            while (is >> nm) ms.push_back(nameToMidi(nm));
            if (ms.size() != 6 || std::find(ms.begin(), ms.end(), -1) != ms.end()) {
                err = strFormat(tr("Line %d: tuning must be 6 note names, e.g. E2 A2 D3 G3 B3 E4"), (int)li + 1);
                return false;
            }
            for (int k = 0; k < 6; ++k) out.tuning[5 - k] = ms[k];
        } else if (key == "capo") {
            int v;
            if (!parseIntStrict(val, v) || v < 0 || v > 24) { err = strFormat(tr("Line %d: invalid capo"), (int)li + 1); return false; }
            out.capo = v;
        } else if (key == "level") {
            int v;
            if (parseIntStrict(val, v)) out.level = std::clamp(v, 0, 5);
        }
        // other keys are ignored
    }
    // Description: plain lines are joined with newlines; bilingual lines ("English|Chinese") are merged per side
    // into "English lines|Chinese lines", so gs::loc() picks a complete multi-line text for each language.
    {
        std::vector<std::string> en, zh;
        bool bilingual = false;
        for (const std::string& d : descLines) {
            std::string e, z;
            if (splitBilingual(d, e, z)) {
                bilingual = true;
                en.push_back(e);
                zh.push_back(z);
            } else {
                en.push_back(d);
                zh.push_back(d);
            }
        }
        if (!bilingual) out.description = joinLines(descLines);
        else {
            const std::string je = joinLines(en), jz = joinLines(zh);
            out.description = jz.empty() ? je : je + "|" + jz;
        }
    }
    out.tempo.push_back({0.0, bpm});
    ts0.beat = 0;
    out.timeSigs.push_back(ts0);

    std::vector<Tok> toks = tokenize(lines, li);
    double beat = 0, lastDur = 1.0, barStart = 0;
    int barNo = 1;
    std::string pendingLyric;
    std::vector<int> pendingTie;  // midi of the notes tied (~) in the previous token
    auto lineErr = [&](const Tok& t, const std::string& msg) {
        err = strFormat(tr("Line %d \"%s\": %s"), t.line, t.text.c_str(), msg.c_str());
        return false;
    };

    for (const Tok& t : toks) {
        const std::string& s = t.text;
        if (s.empty()) continue;
        if (s == "|") {
            const TimeSig& ts = out.timeSigs.back();
            double expect = ts.num * 4.0 / ts.den;
            double got = beat - barStart;
            if (got > 1e-6 && std::fabs(got - expect) > 1e-4) {
                char buf[160];
                std::snprintf(buf, sizeof buf, tr("Line %d: bar %d has %.3g beats, expected %.3g\n"), t.line, barNo, got, expect);
                warn += buf;
            }
            if (got > 1e-6) ++barNo;
            barStart = beat;
            continue;
        }
        if (s[0] == '@') {
            std::string name = s.substr(1);
            std::replace(name.begin(), name.end(), '_', ' ');
            out.sections.push_back({beat, name});
            continue;
        }
        if (s[0] == '"') {
            std::string ly = s.substr(1);
            if (!ly.empty() && ly.back() == '"') ly.pop_back();
            pendingLyric = ly;
            continue;
        }
        if (s[0] == '{') {
            std::string body = s.substr(1);
            if (!body.empty() && body.back() == '}') body.pop_back();
            size_t eq = body.find('=');
            if (eq == std::string::npos) return lineErr(t, tr("expected {bpm=100} or {time=3/4}"));
            std::string k = toLower(trim(body.substr(0, eq))), v = trim(body.substr(eq + 1));
            if (k == "bpm" || k == "tempo") {
                double b;
                if (!parseDoubleStrict(v, b) || b < 10 || b > 999) return lineErr(t, tr("invalid bpm"));
                out.tempo.push_back({beat, b});
            } else if (k == "time") {
                int n = 0, d = 0;
                if (std::sscanf(v.c_str(), "%d/%d", &n, &d) != 2 || n < 1 || n > 32 || (d != 1 && d != 2 && d != 4 && d != 8 && d != 16))
                    return lineErr(t, tr("invalid time signature"));
                out.timeSigs.push_back({beat, n, d});
                barStart = beat;
            } else {
                return lineErr(t, tr("unknown directive"));
            }
            continue;
        }

        // ---- note-like tokens: element [/dur] [mods]
        size_t i = 0;
        SongEvent ev;
        bool isRest = false;
        std::string elem;
        if (s[0] == '[') {
            size_t close = s.find(']');
            if (close == std::string::npos) return lineErr(t, tr("missing ]"));
            std::string inside = s.substr(1, close - 1);
            i = close + 1;
            std::string name = inside, shape;
            size_t colon = inside.find(':');
            if (colon != std::string::npos) {
                name = trim(inside.substr(0, colon));
                shape = trim(inside.substr(colon + 1));
            }
            int f[6];
            if (!shape.empty()) {
                if (!parseShape(shape, f)) return lineErr(t, tr("invalid chord shape (e.g. [C:x32010])"));
            } else if (!chordShape(name, f)) {
                return lineErr(t, tr("chord not in the chord library: ") + name);
            }
            ev.chordName = name;
            for (int k = 0; k < 6; ++k) {
                if (f[k] < 0) continue;
                SongNote n;
                n.string = 5 - k;
                n.fret = f[k];
                ev.notes.push_back(n);
            }
            if (ev.notes.empty()) return lineErr(t, tr("chord has no notes"));
        } else if (s[0] == 'r' || s[0] == 'R') {
            isRest = true;
            i = 1;
        } else {
            // "string.fret" or note names joined by '+'
            size_t end = 0;
            while (end < s.size() && s[end] != '/') {
                char c = s[end];
                bool modChar = (c == '~' || c == 'h' || c == 'p' || c == 's' || c == 'x' || c == 'm' || c == 'v' || c == '^' ||
                                c == 'b');
                if (modChar && end > 0 && (std::isdigit((unsigned char)s[end - 1]))) break;
                ++end;
            }
            elem = s.substr(0, end);
            i = end;
            size_t p0 = 0;
            while (p0 <= elem.size()) {
                size_t p1 = elem.find('+', p0);
                if (p1 == std::string::npos) p1 = elem.size();
                std::string part = elem.substr(p0, p1 - p0);
                if (part.empty()) return lineErr(t, tr("unrecognized token"));
                SongNote n;
                size_t dot = part.find('.');
                if (std::isdigit((unsigned char)part[0]) && dot != std::string::npos) {
                    int st, fr;
                    if (!parseIntStrict(part.substr(0, dot), st) || !parseIntStrict(part.substr(dot + 1), fr) || st < 1 || st > 6 ||
                        fr < 0 || fr > 24)
                        return lineErr(t, tr("expected string.fret with string 1..6 and fret 0..24"));
                    n.string = st - 1;
                    n.fret = fr;
                } else {
                    int m = nameToMidi(part);
                    if (m < 0) return lineErr(t, tr("unrecognized note"));
                    n.midi = m;
                }
                ev.notes.push_back(n);
                p0 = p1 + 1;
            }
        }
        double dur = lastDur;
        if (i < s.size() && s[i] == '/') {
            size_t used = parseDuration(s, i + 1, dur);
            if (!used) return lineErr(t, tr("invalid duration (use /1 /2 /4 /8 /16 /32, optionally with . or t)"));
            i += 1 + used;
        }
        uint32_t flags = 0;
        bool tie = false;
        for (; i < s.size(); ++i) {
            switch (s[i]) {
                case '~': tie = true; break;
                case 'h': flags |= NF_Hammer; break;
                case 'p': flags |= NF_Pull; break;
                case 's': flags |= NF_Slide; break;
                case 'x': flags |= NF_Dead; break;
                case 'm': flags |= NF_PalmMute; break;
                case 'b': flags |= NF_Bend; break;
                case 'v': ev.strum = Strum::Down; break;
                case '^': ev.strum = Strum::Up; break;
                default: return lineErr(t, tr("unknown modifier '") + std::string(1, s[i]) + "'");
            }
        }
        lastDur = dur;
        if (isRest) {
            pendingTie.clear();
            beat += dur;
            continue;
        }
        ev.beat = beat;
        ev.durBeats = dur;
        std::vector<int> nextTie;
        for (SongNote& n : ev.notes) {
            n.durBeats = dur;
            n.flags |= flags;
            int midi = n.string >= 0 ? out.tuning[n.string] + out.capo + n.fret : n.midi;
            if (std::find(pendingTie.begin(), pendingTie.end(), midi) != pendingTie.end()) n.flags |= NF_TieFromPrev;
            if (tie) nextTie.push_back(midi);
        }
        pendingTie.swap(nextTie);
        if (!pendingLyric.empty()) { ev.lyric = pendingLyric; pendingLyric.clear(); }
        out.events.push_back(std::move(ev));
        beat += dur;
        if (out.events.size() > kMaxNotes) { err = tr("Too many notes"); return false; }
    }
    out.lengthBeats = beat;
    out.finalize();
    if (out.events.empty()) {
        err = warn + tr("The score contains no notes");
        return false;
    }
    err = warn;
    return true;
}

// ============================================================ writer
namespace {

struct DurName { double beats; const char* s; };
const DurName kDurs[] = {{6.0, "/1."}, {4.0, "/1"}, {3.0, "/2."}, {2.0, "/2"}, {4.0 / 3.0, "/2t"}, {1.5, "/4."}, {1.0, "/4"},
                         {0.75, "/8."}, {2.0 / 3.0, "/4t"}, {0.5, "/8"}, {0.375, "/16."}, {1.0 / 3.0, "/8t"}, {0.25, "/16"},
                         {0.1875, "/32."}, {1.0 / 6.0, "/16t"}, {0.125, "/32"}, {1.0 / 12.0, "/32t"}, {0.0625, "/64"}};

// decompose a length into a sequence of writable durations
std::vector<const char*> decompose(double len) {
    std::vector<const char*> r;
    int guard = 0;
    while (len > 1e-4 && guard++ < 64) {
        const DurName* pick = nullptr;
        for (const DurName& d : kDurs)
            if (d.beats <= len + 1e-4) { pick = &d; break; }
        if (!pick) {
            if (len > 0.03) r.push_back("/64");
            break;
        }
        r.push_back(pick->s);
        len -= pick->beats;
    }
    return r;
}

std::string shapeString(const int f[6]) {
    bool big = false;
    for (int k = 0; k < 6; ++k)
        if (f[k] > 9) big = true;
    std::string s;
    for (int k = 0; k < 6; ++k) {
        if (big && k) s += ',';
        s += f[k] < 0 ? std::string("x") : std::to_string(f[k]);
    }
    return s;
}

struct Seg {
    double beat = 0, len = 0;
    std::string elem;   // empty = rest
    std::string mods;   // modifiers (without ~)
    std::string lyric;
};

}  // namespace

std::string writeGtab(const Song& song) {
    std::ostringstream o;
    o << "# Guitar Studio score (.gtab)\n";
    if (!song.title.empty()) o << "title: " << song.title << "\n";
    if (!song.artist.empty()) o << "artist: " << song.artist << "\n";
    if (!song.album.empty()) o << "album: " << song.album << "\n";
    double bpm0 = song.tempo.empty() ? 120.0 : song.tempo.front().bpm;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", bpm0);
    o << "bpm: " << buf << "\n";
    TimeSig ts0 = song.timeSigs.empty() ? TimeSig{} : song.timeSigs.front();
    o << "time: " << ts0.num << "/" << ts0.den << "\n";
    static const int kStd[6] = {64, 59, 55, 50, 45, 40};
    if (!std::equal(song.tuning, song.tuning + 6, kStd)) {
        o << "tuning:";
        for (int k = 5; k >= 0; --k) o << " " << midiToName(song.tuning[k]);
        o << "\n";
    }
    if (song.capo) o << "capo: " << song.capo << "\n";
    if (song.level) o << "level: " << song.level << "\n";
    if (!song.description.empty()) {
        // bilingual "English lines|Chinese lines" is written back as per-line pairs (see parseGtab)
        const size_t bar = song.description.find('|');
        bool bilingual = false;
        if (bar != std::string::npos)
            for (size_t k = bar + 1; k < song.description.size(); ++k)
                if ((unsigned char)song.description[k] >= 0x80) bilingual = true;
        if (bilingual) {
            std::vector<std::string> en = splitLines(song.description.substr(0, bar));
            std::vector<std::string> zh = splitLines(song.description.substr(bar + 1));
            const size_t n = std::max(en.size(), zh.size());
            for (size_t k = 0; k < n; ++k)
                o << "desc: " << (k < en.size() ? en[k] : std::string()) << "|" << (k < zh.size() ? zh[k] : std::string()) << "\n";
        } else {
            for (const std::string& l : splitLines(song.description)) o << "desc: " << l << "\n";
        }
    }
    o << "---\n";

    // 1) events -> non-overlapping segments
    std::vector<Seg> segs;
    double cursor = 0;
    for (size_t i = 0; i < song.events.size(); ++i) {
        const SongEvent& e = song.events[i];
        if (e.beat > cursor + 1e-6) segs.push_back({cursor, e.beat - cursor, "", "", ""});
        double maxDur = 0;
        for (const SongNote& n : e.notes) maxDur = std::max(maxDur, n.durBeats);
        if (maxDur <= 0) maxDur = e.durBeats;
        double next = i + 1 < song.events.size() ? song.events[i + 1].beat : 1e300;
        double len = std::min(maxDur, next - e.beat);
        if (len <= 1e-6) len = std::max(1e-3, std::min(e.durBeats, next - e.beat));
        Seg sg;
        sg.beat = e.beat;
        sg.len = len;
        sg.lyric = e.lyric;
        // element
        int f[6] = {-1, -1, -1, -1, -1, -1};
        for (const SongNote& n : e.notes)
            if (n.string >= 0 && n.string < 6) f[5 - n.string] = n.fret;
        int shape[6];
        if (!e.chordName.empty() && e.chordName.find_first_of(" []:") == std::string::npos) {
            if (chordShape(e.chordName, shape) && std::equal(shape, shape + 6, f)) sg.elem = "[" + e.chordName + "]";
            else sg.elem = "[" + e.chordName + ":" + shapeString(f) + "]";
        } else {
            for (const SongNote& n : e.notes) {
                if (!sg.elem.empty()) sg.elem += "+";
                sg.elem += std::to_string(n.string + 1) + "." + std::to_string(n.fret);
            }
        }
        uint32_t fl = 0;
        for (const SongNote& n : e.notes) fl |= n.flags;
        if (fl & NF_Hammer) sg.mods += 'h';
        if (fl & NF_Pull) sg.mods += 'p';
        if (fl & NF_Slide) sg.mods += 's';
        if (fl & NF_Dead) sg.mods += 'x';
        if (fl & NF_PalmMute) sg.mods += 'm';
        if (fl & NF_Bend) sg.mods += 'b';
        if (e.strum == Strum::Down) sg.mods += 'v';
        if (e.strum == Strum::Up) sg.mods += '^';
        segs.push_back(sg);
        cursor = e.beat + len;
    }
    if (song.lengthBeats > cursor + 1e-6) segs.push_back({cursor, song.lengthBeats - cursor, "", "", ""});

    // 2) split at bar lines and write out
    std::vector<double> barStarts;
    for (const BarLine& b : song.bars) barStarts.push_back(b.beat);
    if (barStarts.empty()) barStarts.push_back(0);
    size_t barIdx = 1;
    size_t tempoIdx = 1, tsIdx = 1, secIdx = 0;
    int col = 0;
    auto emit = [&](const std::string& tok) {
        o << tok;
        col += (int)tok.size() + 1;
        if (col > 90) { o << "\n"; col = 0; }
        else o << " ";
    };
    auto emitMarkers = [&](double at) {
        while (secIdx < song.sections.size() && song.sections[secIdx].beat <= at + 1e-6) {
            std::string nm = song.sections[secIdx].name;
            std::replace(nm.begin(), nm.end(), ' ', '_');
            if (!nm.empty()) emit("@" + nm);
            ++secIdx;
        }
        while (tempoIdx < song.tempo.size() && song.tempo[tempoIdx].beat <= at + 1e-6) {
            char b2[64];
            std::snprintf(b2, sizeof b2, "{bpm=%.6g}", song.tempo[tempoIdx].bpm);
            emit(b2);
            ++tempoIdx;
        }
        while (tsIdx < song.timeSigs.size() && song.timeSigs[tsIdx].beat <= at + 1e-6) {
            emit("{time=" + std::to_string(song.timeSigs[tsIdx].num) + "/" + std::to_string(song.timeSigs[tsIdx].den) + "}");
            ++tsIdx;
        }
    };
    for (const Seg& sg0 : segs) {
        double pos = sg0.beat, remain = sg0.len;
        bool first = true;
        while (remain > 1e-4) {
            while (barIdx < barStarts.size() && barStarts[barIdx] <= pos + 1e-6) {
                emit("|");
                if (col > 60) { o << "\n"; col = 0; }
                ++barIdx;
            }
            emitMarkers(pos);
            double barEnd = barIdx < barStarts.size() ? barStarts[barIdx] : 1e300;
            double part = std::min(remain, barEnd - pos);
            if (part <= 1e-6) part = remain;
            std::vector<const char*> ds = decompose(part);
            for (size_t k = 0; k < ds.size(); ++k) {
                if (sg0.elem.empty()) {
                    emit(std::string("r") + ds[k]);
                    continue;
                }
                if (first && !sg0.lyric.empty()) {
                    std::string ly = sg0.lyric;
                    std::replace(ly.begin(), ly.end(), '"', '\'');
                    emit("\"" + ly + "\"");
                }
                bool more = (k + 1 < ds.size()) || (remain - part > 1e-4);
                std::string tok = sg0.elem + ds[k] + (first ? sg0.mods : std::string()) + (more ? "~" : "");
                emit(tok);
                first = false;
            }
            pos += part;
            remain -= part;
        }
    }
    o << "|\n";
    return o.str();
}

}  // namespace gs
