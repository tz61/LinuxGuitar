// Chord library: common open chords + generic barre shapes (E shape / A shape).
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "song/SongInternal.h"

namespace gs {

namespace {

struct Shape { int f[6]; };  // string 6 -> string 1, -1 = not played
constexpr int X = -1;

const std::map<std::string, Shape>& openShapes() {
    static const std::map<std::string, Shape> m = {
        {"C", {{X, 3, 2, 0, 1, 0}}},     {"Cmaj7", {{X, 3, 2, 0, 0, 0}}}, {"C7", {{X, 3, 2, 3, 1, 0}}},
        {"Cadd9", {{X, 3, 2, 0, 3, 0}}}, {"Csus2", {{X, 3, 0, 0, 1, 3}}}, {"Csus4", {{X, 3, 3, 0, 1, 1}}},
        {"C5", {{X, 3, 5, 5, X, X}}},    {"Caug", {{X, 3, 2, 1, 1, 0}}},
        {"D", {{X, X, 0, 2, 3, 2}}},     {"Dm", {{X, X, 0, 2, 3, 1}}},    {"D7", {{X, X, 0, 2, 1, 2}}},
        {"Dmaj7", {{X, X, 0, 2, 2, 2}}}, {"Dsus2", {{X, X, 0, 2, 3, 0}}}, {"Dsus4", {{X, X, 0, 2, 3, 3}}},
        {"Dm7", {{X, X, 0, 2, 1, 1}}},   {"D5", {{X, X, 0, 2, 3, X}}},    {"D6", {{X, X, 0, 2, 0, 2}}},
        {"E", {{0, 2, 2, 1, 0, 0}}},     {"Em", {{0, 2, 2, 0, 0, 0}}},    {"E7", {{0, 2, 0, 1, 0, 0}}},
        {"Em7", {{0, 2, 2, 0, 3, 0}}},   {"Esus4", {{0, 2, 2, 2, 0, 0}}}, {"Emaj7", {{0, 2, 1, 1, 0, 0}}},
        {"E5", {{0, 2, 2, X, X, X}}},    {"Esus2", {{0, 2, 4, 4, 0, 0}}},
        {"F", {{1, 3, 3, 2, 1, 1}}},     {"Fmaj7", {{X, X, 3, 2, 1, 0}}}, {"Fsmall", {{X, X, 3, 2, 1, 1}}},
        {"\xE5\xB0\x8F" "F", {{X, X, 3, 2, 1, 1}}},  // "small F" written in Chinese (common name for the easy F)   {"Fm", {{1, 3, 3, 1, 1, 1}}},    {"F5", {{1, 3, 3, X, X, X}}},
        {"F7", {{1, 3, 1, 2, 1, 1}}},
        {"G", {{3, 2, 0, 0, 0, 3}}},     {"G7", {{3, 2, 0, 0, 0, 1}}},    {"Gmaj7", {{3, 2, 0, 0, 0, 2}}},
        {"Gsus4", {{3, 3, 0, 0, 1, 3}}}, {"G5", {{3, 5, 5, X, X, X}}},    {"G6", {{3, 2, 0, 0, 0, 0}}},
        {"Gadd9", {{3, 2, 0, 2, 0, 3}}},
        {"A", {{X, 0, 2, 2, 2, 0}}},     {"Am", {{X, 0, 2, 2, 1, 0}}},    {"A7", {{X, 0, 2, 0, 2, 0}}},
        {"Am7", {{X, 0, 2, 0, 1, 0}}},   {"Amaj7", {{X, 0, 2, 1, 2, 0}}}, {"Asus2", {{X, 0, 2, 2, 0, 0}}},
        {"Asus4", {{X, 0, 2, 2, 3, 0}}}, {"A5", {{X, 0, 2, 2, X, X}}},    {"A6", {{X, 0, 2, 2, 2, 2}}},
        {"B7", {{X, 2, 1, 2, 0, 2}}},    {"Bm", {{X, 2, 4, 4, 3, 2}}},    {"B", {{X, 2, 4, 4, 4, 2}}},
        {"Bm7", {{X, 2, 0, 2, 0, 2}}},   {"B5", {{X, 2, 4, 4, X, X}}},
        // common inversions / slash chords
        {"C/G", {{3, 3, 2, 0, 1, 0}}},   {"C/B", {{X, 2, 2, 0, 1, 0}}},   {"C/E", {{0, 3, 2, 0, 1, 0}}},
        {"G/B", {{X, 2, 0, 0, 0, 3}}},   {"G/D", {{X, X, 0, 0, 0, 3}}},   {"D/F#", {{2, X, 0, 2, 3, 2}}},
        {"D/A", {{X, 0, 0, 2, 3, 2}}},   {"Am/G", {{3, X, 2, 2, 1, 0}}},  {"Am/E", {{0, 0, 2, 2, 1, 0}}},
        {"Em/D", {{X, X, 0, 0, 0, 0}}},  {"F/C", {{X, 3, 3, 2, 1, 1}}},   {"A/E", {{0, 0, 2, 2, 2, 0}}},
    };
    return m;
}

enum Quality { Q_Maj, Q_Min, Q_7, Q_M7, Q_Maj7, Q_Sus2, Q_Sus4, Q_5, Q_Dim, Q_Aug, Q_6, Q_m6, Q_9, Q_Add9, Q_7Sus4, Q_m7b5, Q_Unknown };

Quality parseQuality(const std::string& q) {
    static const std::map<std::string, Quality> m = {
        {"", Q_Maj},       {"M", Q_Maj},       {"maj", Q_Maj},     {"major", Q_Maj},  {"m", Q_Min},
        {"min", Q_Min},    {"-", Q_Min},       {"minor", Q_Min},   {"7", Q_7},        {"dom7", Q_7},
        {"m7", Q_M7},      {"min7", Q_M7},     {"-7", Q_M7},       {"maj7", Q_Maj7},  {"M7", Q_Maj7},
        {"ma7", Q_Maj7},   {"\xCE\x94", Q_Maj7}, {"\xCE\x94" "7", Q_Maj7}, {"sus2", Q_Sus2}, {"sus4", Q_Sus4},
        {"sus", Q_Sus4},   {"5", Q_5},         {"dim", Q_Dim},     {"dim7", Q_Dim},   {"o", Q_Dim},
        {"o7", Q_Dim},     {"\xC2\xB0", Q_Dim}, {"aug", Q_Aug},    {"+", Q_Aug},      {"6", Q_6},
        {"m6", Q_m6},      {"min6", Q_m6},     {"9", Q_9},         {"add9", Q_Add9},  {"add2", Q_Add9},
        {"2", Q_Add9},     {"7sus4", Q_7Sus4}, {"7sus", Q_7Sus4},  {"m7b5", Q_m7b5},  {"\xC3\xB8", Q_m7b5},
        {"min7b5", Q_m7b5},
    };
    auto it = m.find(q);
    return it == m.end() ? Q_Unknown : it->second;
}

// shapes relative to the E shape (root on string 6) / A shape (root on string 5), X = not played
bool eShape(Quality q, Shape& s) {
    switch (q) {
        case Q_Maj: s = {{0, 2, 2, 1, 0, 0}}; return true;
        case Q_Min: s = {{0, 2, 2, 0, 0, 0}}; return true;
        case Q_7: s = {{0, 2, 0, 1, 0, 0}}; return true;
        case Q_M7: s = {{0, 2, 0, 0, 0, 0}}; return true;
        case Q_Maj7: s = {{0, X, 1, 1, 0, X}}; return true;
        case Q_Sus4: s = {{0, 2, 2, 2, 0, 0}}; return true;
        case Q_5: s = {{0, 2, 2, X, X, X}}; return true;
        case Q_7Sus4: s = {{0, 2, 0, 2, 0, 0}}; return true;
        case Q_6: s = {{0, 2, 2, 1, 2, 0}}; return true;
        default: return false;
    }
}

bool aShape(Quality q, Shape& s) {
    switch (q) {
        case Q_Maj: s = {{X, 0, 2, 2, 2, 0}}; return true;
        case Q_Min: s = {{X, 0, 2, 2, 1, 0}}; return true;
        case Q_7: s = {{X, 0, 2, 0, 2, 0}}; return true;
        case Q_M7: s = {{X, 0, 2, 0, 1, 0}}; return true;
        case Q_Maj7: s = {{X, 0, 2, 1, 2, 0}}; return true;
        case Q_Sus2: s = {{X, 0, 2, 2, 0, 0}}; return true;
        case Q_Sus4: s = {{X, 0, 2, 2, 3, 0}}; return true;
        case Q_5: s = {{X, 0, 2, 2, X, X}}; return true;
        case Q_Dim: s = {{X, 0, 1, 2, 1, X}}; return true;
        case Q_Aug: s = {{X, 0, 3, 2, 2, 1}}; return true;
        case Q_6: s = {{X, 0, 2, 2, 2, 2}}; return true;
        case Q_m6: s = {{X, 0, 2, 2, 1, 2}}; return true;
        case Q_9: s = {{X, 0, 2, 4, 2, 3}}; return true;
        case Q_Add9: s = {{X, 0, 2, 4, 2, 0}}; return true;
        case Q_7Sus4: s = {{X, 0, 2, 0, 3, 0}}; return true;
        case Q_m7b5: s = {{X, 0, 1, 0, 1, X}}; return true;
        default: return false;
    }
}

int rootPitchClass(const std::string& s, size_t& i) {
    static const int kBase[7] = {9, 11, 0, 2, 4, 5, 7};
    if (s.empty()) return -1;
    char L = (char)std::toupper((unsigned char)s[0]);
    if (L < 'A' || L > 'G') return -1;
    int pc = kBase[L - 'A'];
    i = 1;
    while (i < s.size()) {
        if (s[i] == '#') { ++pc; ++i; }
        else if (s[i] == 'b') { --pc; ++i; }
        else if (s.compare(i, 3, "\xE2\x99\xAF") == 0) { ++pc; i += 3; }
        else if (s.compare(i, 3, "\xE2\x99\xAD") == 0) { --pc; i += 3; }
        else break;
    }
    return ((pc % 12) + 12) % 12;
}

bool buildShape(Quality q, int pc, Shape& out) {
    Shape e{}, a{};
    bool hasE = eShape(q, e), hasA = aShape(q, a);
    int fe = ((pc - 4) % 12 + 12) % 12;  // root fret of the E shape (string 6)
    int fa = ((pc - 9) % 12 + 12) % 12;  // root fret of the A shape (string 5)
    // pick the shape with the lower fret (open shapes at fret 0 win)
    bool useE;
    if (hasE && hasA) useE = fe <= fa;
    else if (hasE) useE = true;
    else if (hasA) useE = false;
    else return false;
    const Shape& base = useE ? e : a;
    int f = useE ? fe : fa;
    for (int k = 0; k < 6; ++k) out.f[k] = base.f[k] < 0 ? -1 : base.f[k] + f;
    return true;
}

}  // namespace

bool chordShape(const std::string& name0, int fretsLowToHigh[6]) {
    std::string name = detail::trim(name0);
    if (name.empty()) return false;
    const auto& tbl = openShapes();
    auto it = tbl.find(name);
    if (it != tbl.end()) {
        std::memcpy(fretsLowToHigh, it->second.f, sizeof(int) * 6);
        return true;
    }
    // strip parentheses, normalize the spelling
    std::string base = name, bass;
    size_t slash = name.find('/');
    if (slash != std::string::npos) {
        base = name.substr(0, slash);
        bass = name.substr(slash + 1);
    }
    size_t i = 0;
    int pc = rootPitchClass(base, i);
    if (pc < 0) return false;
    std::string q = base.substr(i);
    if (!q.empty() && q.front() == '(' && q.back() == ')') q = q.substr(1, q.size() - 2);
    Quality qu = parseQuality(q);
    if (qu == Q_Unknown) {
        // try common prefixes: "maj9" -> maj7, "m9"/"m11" -> m7, "13"/"11" -> 7, anything else starting with m -> minor
        if (detail::startsWith(q, "maj")) qu = Q_Maj7;
        else if (detail::startsWith(q, "m7") || detail::startsWith(q, "m9") || detail::startsWith(q, "m1")) qu = Q_M7;
        else if (!q.empty() && q[0] == 'm') qu = Q_Min;
        else if (!q.empty() && std::isdigit((unsigned char)q[0])) qu = Q_7;
        else qu = Q_Maj;
    }
    // is the canonical name in the table? (e.g. "Amin" -> "Am")
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const std::map<int, const char*> kSuffix = {{Q_Maj, ""}, {Q_Min, "m"}, {Q_7, "7"}, {Q_M7, "m7"}, {Q_Maj7, "maj7"},
                                                       {Q_Sus2, "sus2"}, {Q_Sus4, "sus4"}, {Q_5, "5"}, {Q_6, "6"}, {Q_Add9, "add9"}};
    Shape s{};
    bool found = false;
    auto sx = kSuffix.find(qu);
    if (sx != kSuffix.end()) {
        auto it2 = tbl.find(std::string(kNames[pc]) + sx->second);
        if (it2 != tbl.end()) { s = it2->second; found = true; }
    }
    if (!found) found = buildShape(qu, pc, s);
    if (!found && qu != Q_Maj && qu != Q_Min) {
        Quality fb = (qu == Q_m6 || qu == Q_M7 || qu == Q_m7b5 || qu == Q_Dim) ? Q_Min : Q_Maj;
        found = buildShape(fb, pc, s);
    }
    if (!found) return false;
    // bass note: try to play it on string 6 or 5 (near the chord's frets), otherwise ignore it
    if (!bass.empty()) {
        size_t j = 0;
        int bpc = rootPitchClass(bass, j);
        if (bpc >= 0 && j == bass.size()) {
            int minF = 99, maxF = 0;
            for (int k = 0; k < 6; ++k)
                if (s.f[k] > 0) { minF = std::min(minF, s.f[k]); maxF = std::max(maxF, s.f[k]); }
            if (minF == 99) minF = maxF = 0;
            const int open6 = 4, open5 = 9;  // pitch classes of E / A
            for (int str = 0; str < 2; ++str) {
                int open = str == 0 ? open6 : open5;
                int fr = ((bpc - open) % 12 + 12) % 12;
                if (fr <= std::max(maxF, 3) && fr >= std::max(0, minF - 3)) {
                    s.f[str] = fr;
                    for (int k = 0; k < str; ++k) s.f[k] = -1;
                    break;
                }
            }
        }
    }
    std::memcpy(fretsLowToHigh, s.f, sizeof(int) * 6);
    return true;
}

}  // namespace gs
