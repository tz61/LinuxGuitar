// Offline tests for the song module: format parsing / round trips / lesson files / fuzzing.
// Optional: set GS_TESTDATA_DIR to a directory of real score files (searched recursively); each one is loaded
// and summarized.
#include <dirent.h>
#include <iconv.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/I18n.h"
#include "song/Song.h"
#include "song/SongInternal.h"

using namespace gs;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)
#define CHECK_MSG(cond, msg)                                                     \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::printf("  FAIL %s:%d: %s -- %s\n", __FILE__, __LINE__, #cond,   \
                        std::string(msg).c_str());                               \
        }                                                                        \
    } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static std::string tempDir() {
    static std::string dir;
    if (dir.empty()) {
        const char* t = std::getenv("TMPDIR");
        std::string tmpl = std::string(t && *t ? t : "/tmp") + "/gs_song_test_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data())) dir = buf.data();
    }
    return dir;
}

static std::string writeTemp(const std::string& name, const std::string& data) {
    std::string p = tempDir() + "/" + name;
    FILE* f = std::fopen(p.c_str(), "wb");
    if (f) {
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
    return p;
}

// "English|Chinese" with a non-empty English side free of CJK characters and a non-empty Chinese side
static bool hasCjk(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0xE4 && c <= 0xE9) return true;  // UTF-8 lead bytes of U+4000..U+9FFF
    return false;
}
static bool isBilingual(const std::string& s) {
    size_t bar = s.find('|');
    if (bar == std::string::npos) return false;
    std::string en = s.substr(0, bar), zh = s.substr(bar + 1);
    return !en.empty() && !hasCjk(en) && hasCjk(zh);
}

static std::vector<int> pitchSeq(const Song& s) {
    std::vector<int> v;
    for (const SongEvent& e : s.events)
        for (const SongNote& n : e.notes) v.push_back(n.midi);
    return v;
}

static bool playable(const Song& s, std::string* why = nullptr) {
    for (const SongEvent& e : s.events) {
        std::set<int> strs;
        for (const SongNote& n : e.notes) {
            if (n.string < 0 || n.string > 5 || n.fret < 0 || n.fret > 24) {
                if (why) *why = "bad string/fret at beat " + std::to_string(e.beat);
                return false;
            }
            if (!strs.insert(n.string).second) {
                if (why) *why = "duplicate string at beat " + std::to_string(e.beat);
                return false;
            }
            if (n.midi != s.midiOf(n.string, n.fret)) {
                if (why) *why = "midi mismatch";
                return false;
            }
        }
    }
    for (size_t i = 1; i < s.events.size(); ++i)
        if (s.events[i].beat < s.events[i - 1].beat) {
            if (why) *why = "events not sorted";
            return false;
        }
    return true;
}

// ============================================================ basics
static void testNames() {
    std::printf("[names]\n");
    CHECK(nameToMidi("C4") == 60);
    CHECK(nameToMidi("A4") == 69);
    CHECK(nameToMidi("Bb3") == 58);
    CHECK(nameToMidi("F#3") == 54);
    CHECK(nameToMidi("E2") == 40);
    CHECK(nameToMidi("C-1") == 0);
    CHECK(nameToMidi("G9") == 127);
    CHECK(nameToMidi("H4") == -1);
    CHECK(nameToMidi("C") == -1);
    CHECK(nameToMidi("") == -1);
    CHECK(midiToName(60) == "C4");
    CHECK(midiToName(61) == "C#4");
    bool rt = true;
    for (int m = 0; m < 128; ++m)
        if (nameToMidi(midiToName(m)) != m) rt = false;
    CHECK(rt);
}

static std::set<int> chordPcs(int root, const std::string& q) {
    std::vector<int> iv;
    if (q.empty() || q == "maj") iv = {0, 4, 7};
    else if (q == "m") iv = {0, 3, 7};
    else if (q == "7") iv = {0, 4, 7, 10};
    else if (q == "m7") iv = {0, 3, 7, 10};
    else if (q == "maj7") iv = {0, 4, 7, 11};
    else if (q == "sus2") iv = {0, 2, 7};
    else if (q == "sus4") iv = {0, 5, 7};
    else if (q == "5") iv = {0, 7};
    else if (q == "aug") iv = {0, 4, 8};
    else if (q == "dim") iv = {0, 3, 6};
    std::set<int> s;
    for (int i : iv) s.insert((root + i) % 12);
    return s;
}

static void testChords() {
    std::printf("[chords]\n");
    struct C { const char* name; int root; const char* q; };
    const C list[] = {
        {"C", 0, ""},     {"Cmaj7", 0, "maj7"}, {"C7", 0, "7"},      {"D", 2, ""},       {"Dm", 2, "m"},       {"D7", 2, "7"},
        {"Dsus2", 2, "sus2"}, {"Dsus4", 2, "sus4"}, {"E", 4, ""},   {"Em", 4, "m"},      {"E7", 4, "7"},       {"Em7", 4, "m7"},
        {"Esus4", 4, "sus4"}, {"F", 5, ""},     {"Fmaj7", 5, "maj7"}, {"G", 7, ""},       {"G7", 7, "7"},       {"A", 9, ""},
        {"Am", 9, "m"},   {"A7", 9, "7"},       {"Am7", 9, "m7"},    {"Asus2", 9, "sus2"}, {"Asus4", 9, "sus4"}, {"B7", 11, "7"},
        {"Bm", 11, "m"},  {"A5", 9, "5"},       {"E5", 4, "5"},      {"D5", 2, "5"},      {"G5", 7, "5"},       {"Bb", 10, ""},
        {"F#m", 6, "m"},  {"C#m7", 1, "m7"},    {"Ebmaj7", 3, "maj7"}, {"G#7", 8, "7"},   {"Bbm", 10, "m"},     {"Amin", 9, "m"},
        {"AM", 9, ""},    {"Amaj", 9, ""},      {"Caug", 0, "aug"},  {"Bdim", 11, "dim"}, {"F#", 6, ""},        {"Abm", 8, "m"},
        {"Cm", 0, "m"},   {"Gm", 7, "m"},       {"Fm", 5, "m"},      {"Db", 1, ""},       {"Eb", 3, ""},        {"B", 11, ""},
    };
    static const int kOpen[6] = {40, 45, 50, 55, 59, 64};  // string 6 -> string 1
    for (const C& c : list) {
        int f[6];
        bool ok = chordShape(c.name, f);
        CHECK_MSG(ok, c.name);
        if (!ok) continue;
        std::set<int> want = chordPcs(c.root, c.q), got;
        int played = 0, lowest = -1;
        bool inRange = true;
        for (int k = 0; k < 6; ++k) {
            if (f[k] < 0) continue;
            if (f[k] > 15) inRange = false;
            ++played;
            int pc = (kOpen[k] + f[k]) % 12;
            got.insert(pc);
            if (lowest < 0) lowest = pc;
        }
        bool subset = std::includes(want.begin(), want.end(), got.begin(), got.end());
        CHECK_MSG(played >= 2 && inRange, c.name);
        CHECK_MSG(subset, std::string(c.name) + " has non-chord tones");
        CHECK_MSG(got.count(c.root % 12), std::string(c.name) + " missing root");
        CHECK_MSG(lowest == c.root % 12 || std::string(c.q) == "maj7" || std::string(c.name) == "C7",
                  std::string(c.name) + " bass is not root");
    }
    int f[6];
    CHECK(chordShape("\xE5\xB0\x8F" "F", f) && f[2] == 3 && f[5] == 1);  // "small F" written in Chinese
    CHECK(chordShape("Fsmall", f));
    CHECK(chordShape("G/B", f) && f[0] == -1 && f[1] == 2);
    CHECK(chordShape("D/F#", f) && f[0] == 2);
    CHECK(!chordShape("Hm", f));
    CHECK(!chordShape("", f));
}

static void testTempoMap() {
    std::printf("[tempo map]\n");
    Song s;
    s.tempo = {{0, 120}, {8, 60}};
    s.events.push_back(SongEvent{0, 1, {SongNote{0, 0, -1, 1, 0}}, "", "", Strum::None});
    s.finalize();
    CHECK(near(s.beatToSec(4), 2.0));
    CHECK(near(s.beatToSec(8), 4.0));
    CHECK(near(s.beatToSec(10), 6.0));
    CHECK(near(s.beatToSec(-2), -1.0));
    for (double b : {0.0, 1.5, 7.99, 8.0, 9.25, 100.0}) CHECK(near(s.secToBeat(s.beatToSec(b)), b, 1e-9));
    CHECK(near(s.bpmAt(7.9), 120));
    CHECK(near(s.bpmAt(8.0), 60));
    s.timeSigs = {{0, 4, 4}, {8, 3, 4}};
    s.lengthBeats = 0;
    s.events.push_back(SongEvent{13, 1, {SongNote{1, 1, -1, 1, 0}}, "", "", Strum::None});
    s.finalize();
    CHECK(s.timeSigAt(9).num == 3);
    CHECK(s.timeSigAt(7).num == 4);
    // bars: 0 4 8 11 14 -> length aligned to 14
    CHECK(s.bars.size() == 4 && near(s.bars[3].beat, 11));
    CHECK(near(s.lengthBeats, 14));
}

// ============================================================ gtab
static void testGtab() {
    std::printf("[gtab]\n");
    const char* txt =
        "# test\n"
        "title: Test Song|测试曲\n"
        "artist: Someone|某人\n"
        "bpm: 100\n"
        "time: 4/4\n"
        "capo: 2\n"
        "level: 3\n"
        "desc: Line one|第一行\n"
        "desc: Line two|第二行\n"
        "---\n"
        "@Intro|前奏 3.2/4 \"La|啦\" 3.0 2.1/8 2.3 1.0/4 |   # comment\n"
        "@Slow:_two_strums|慢速:_每小节两下 [G]/2v [C:x32010]/2^ |\n"
        "1.0+2.1+3.0/4. E4/8 r/4 F#3/4 |\n"
        "3.5/8h 3.7/8p 3.5/4s 2.3/4x 2.3/4m 2.3/8 |\n"   // 4.5 beats: must produce a warning
        "{bpm=60} {time=3/4} 4.2/2.~ | 4.2/4 [A:5,7,7,6,5,5]/2 |\n"
        "6.3/8t 6.3 6.3 r/4 r/4 |\n";
    Song s;
    std::string err;
    bool ok = parseGtab(txt, s, err);
    CHECK_MSG(ok, err);
    if (!ok) return;
    CHECK(s.title == "Test Song|测试曲" && s.artist == "Someone|某人" && s.capo == 2 && s.level == 3);
    CHECK(s.description == "Line one\nLine two|第一行\n第二行");  // bilingual lines merged per side
    CHECK(err.find("bar 4 has 4.5 beats") != std::string::npos);    // warning for the 4.5-beat bar
    CHECK(s.sections.size() == 2 && s.sections[0].name == "Intro|前奏" &&
          s.sections[1].name == "Slow: two strums|慢速: 每小节两下");
    CHECK(s.tempo.size() == 2 && near(s.tempo[1].bpm, 60));
    CHECK(s.timeSigs.size() == 2 && s.timeSigs[1].num == 3);
    std::string why;
    CHECK_MSG(playable(s, &why), why);
    // first event: string 3 fret 2 with capo 2 -> A3+2
    CHECK(s.events[0].notes[0].string == 2 && s.events[0].notes[0].fret == 2 && s.events[0].notes[0].midi == 55 + 2 + 2);
    CHECK(s.events[1].lyric == "La|啦");
    CHECK(near(s.events[2].beat, 2.0) && near(s.events[2].durBeats, 0.5));
    // [G] half note, down strum
    const SongEvent* g = nullptr;
    for (const SongEvent& e : s.events)
        if (e.chordName == "G") g = &e;
    CHECK(g && g->notes.size() == 6 && g->strum == Strum::Down && near(g->durBeats, 2));
    // custom chord C (5 notes), up strum
    const SongEvent* c = nullptr;
    for (const SongEvent& e : s.events)
        if (e.chordName == "C") c = &e;
    CHECK(c && c->notes.size() == 5 && c->strum == Strum::Up);
    // note name E4 gets an automatic fingering
    bool foundE4 = false, foundFs3 = false;
    for (const SongEvent& e : s.events)
        for (const SongNote& n : e.notes) {
            if (n.midi == 64 + 0 && e.notes.size() == 1 && near(e.durBeats, 0.5)) foundE4 = true;
            if (n.midi == 54) foundFs3 = true;
        }
    CHECK(foundE4 && foundFs3);
    // technique flags
    uint32_t allFlags = 0;
    for (const SongEvent& e : s.events)
        for (const SongNote& n : e.notes) allFlags |= n.flags;
    CHECK((allFlags & NF_Hammer) && (allFlags & NF_Pull) && (allFlags & NF_Slide) && (allFlags & NF_Dead) && (allFlags & NF_PalmMute));
    // tie merge: string 4 fret 2, dotted half + quarter = 4 beats
    bool tieMerged = false;
    for (const SongEvent& e : s.events)
        for (const SongNote& n : e.notes)
            if (n.string == 3 && n.fret == 2 && near(n.durBeats, 4.0)) tieMerged = true;
    CHECK(tieMerged);
    // triplets
    int trip = 0;
    for (const SongEvent& e : s.events)
        if (near(e.durBeats, 1.0 / 3.0)) ++trip;
    CHECK(trip == 3);

    // bilingual display through gs::loc()
    setLanguage(Lang::En);
    CHECK(loc(s.title) == "Test Song" && loc(s.artist) == "Someone" && loc(s.description) == "Line one\nLine two");
    CHECK(loc(s.sections[0].name) == "Intro" && loc(s.sections[1].name) == "Slow: two strums" && loc(s.events[1].lyric) == "La");
    setLanguage(Lang::Zh);
    CHECK(loc(s.title) == "测试曲" && loc(s.description) == "第一行\n第二行" && loc(s.sections[1].name) == "慢速: 每小节两下");
    CHECK(loc(s.events[1].lyric) == "啦");
    setLanguage(Lang::En);
    // writeGtab keeps the bilingual header values, section tokens and lyrics
    {
        std::string out = writeGtab(s), e2;
        Song t;
        CHECK_MSG(parseGtab(out, t, e2), e2);
        CHECK(t.title == s.title && t.artist == s.artist && t.description == s.description);
        CHECK(t.sections.size() == 2 && t.sections[0].name == s.sections[0].name && t.sections[1].name == s.sections[1].name);
        CHECK(t.events.size() == s.events.size() && t.events.size() > 1 && t.events[1].lyric == "La|啦");
    }
    // desc lines: plain lines go to both sides; a trailing '|' means "no Chinese text"; plain-only text is unchanged
    {
        Song m;
        std::string e3;
        CHECK(parseGtab("title: Mixed\ndesc: Plain line\ndesc: Tip|提示\n3.0/1 |\n", m, e3));
        CHECK(m.description == "Plain line\nTip|Plain line\n提示");
        CHECK(loc(m.description) == "Plain line\nTip");
        CHECK(parseGtab("title: A\ndesc: Only English|\n3.0/1 |\n", m, e3) && m.description == "Only English");
        CHECK(parseGtab("title: B\ndesc: one\ndesc: two\n3.0/1 |\n", m, e3) && m.description == "one\ntwo");
        CHECK(parseGtab("title: C\ndesc: a|b plain bar\n3.0/1 |\n", m, e3) && m.description == "a|b plain bar");
        std::string out = writeGtab(m);
        Song t;
        CHECK(parseGtab(out, t, e3) && t.description == m.description);
    }

    // errors
    Song bad;
    CHECK(!parseGtab("bpm: 80\n7.2/4\n", bad, err) && !err.empty());
    CHECK(err.find("Line 2") != std::string::npos);
    CHECK(!parseGtab("bpm: 80\n[Hxyz]/4\n", bad, err));
    CHECK(!parseGtab("bpm: 80\n3.2/5\n", bad, err));
    CHECK(!parseGtab("title: Empty\n", bad, err));
    // messages are translated through the song catalog when the language is Chinese
    registerSongTranslations();
    setLanguage(Lang::Zh);
    CHECK(!parseGtab("bpm: 80\n7.2/4\n", bad, err) && err.find("第2行") != std::string::npos);
    CHECK(!parseGtab("title: Empty\n", bad, err) && err.find("谱子里没有任何音符") != std::string::npos);
    setLanguage(Lang::En);
    CHECK(!parseGtab("title: Empty\n", bad, err) && err.find("The score contains no notes") != std::string::npos);
}

static bool sameSong(const Song& a, const Song& b, std::string& why) {
    if (a.events.size() != b.events.size()) { why = "event count " + std::to_string(a.events.size()) + " vs " + std::to_string(b.events.size()); return false; }
    for (size_t i = 0; i < a.events.size(); ++i) {
        const SongEvent &x = a.events[i], &y = b.events[i];
        if (!near(x.beat, y.beat, 1e-6) || x.notes.size() != y.notes.size() || x.chordName != y.chordName || x.strum != y.strum ||
            x.lyric != y.lyric) {
            why = "event " + std::to_string(i) + " at beat " + std::to_string(x.beat);
            return false;
        }
        for (size_t k = 0; k < x.notes.size(); ++k) {
            const SongNote &n = x.notes[k], &m = y.notes[k];
            if (n.string != m.string || n.fret != m.fret || !near(n.durBeats, m.durBeats, 1e-6) ||
                (n.flags & ~NF_TieFromPrev) != (m.flags & ~NF_TieFromPrev)) {
                why = "note " + std::to_string(k) + " of event " + std::to_string(i) + " dur " + std::to_string(n.durBeats) + " vs " +
                      std::to_string(m.durBeats);
                return false;
            }
        }
    }
    if (a.tempo.size() != b.tempo.size() || a.timeSigs.size() != b.timeSigs.size() || a.sections.size() != b.sections.size()) {
        why = "tempo/time/sections";
        return false;
    }
    for (size_t i = 0; i < a.tempo.size(); ++i)
        if (!near(a.tempo[i].beat, b.tempo[i].beat) || !near(a.tempo[i].bpm, b.tempo[i].bpm)) { why = "tempo"; return false; }
    if (!near(a.lengthBeats, b.lengthBeats)) { why = "length"; return false; }
    if (a.title != b.title || a.capo != b.capo || !std::equal(a.tuning, a.tuning + 6, b.tuning)) { why = "header"; return false; }
    return true;
}

static void roundTrip(const Song& s, const std::string& label) {
    std::string txt = writeGtab(s), err;
    Song t;
    bool ok = parseGtab(txt, t, err);
    CHECK_MSG(ok, label + ": " + err);
    if (!ok) return;
    CHECK_MSG(err.empty(), label + " written score has bar warnings: " + err);
    std::string why;
    CHECK_MSG(sameSong(s, t, why), label + " round trip: " + why);
}

// ============================================================ lesson files
static std::vector<std::string> listDir(const std::string& dir, bool recursive) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string p = dir + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (recursive) {
                auto sub = listDir(p, true);
                out.insert(out.end(), sub.begin(), sub.end());
            }
        } else {
            out.push_back(p);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

static void testLessons() {
    std::printf("[lessons]\n");
    std::vector<std::string> files;
    for (const std::string& f : listDir(std::string(GS_ASSET_DIR) + "/songs", false))
        if (detail::fileExtLower(f) == ".gtab") files.push_back(f);
    CHECK(files.size() >= 19);
    for (const std::string& f : files) {
        Song s;
        std::string err;
        bool ok = loadSongFile(f, s, err);
        std::string base = f.substr(f.find_last_of('/') + 1);
        CHECK_MSG(ok, base + ": " + err);
        if (!ok) continue;
        CHECK_MSG(err.empty(), base + " warnings: " + err);
        // every built-in lesson is bilingual: title, description, sections and lyrics ("English|Chinese")
        CHECK_MSG(isBilingual(s.title), base + " title not bilingual: " + s.title);
        CHECK_MSG(isBilingual(s.description), base + " description not bilingual");
        CHECK_MSG(s.artist.empty() || isBilingual(s.artist) || !hasCjk(s.artist), base + " artist: " + s.artist);
        for (const Section& sec : s.sections)
            CHECK_MSG(isBilingual(sec.name) || !hasCjk(sec.name), base + " section: " + sec.name);
        for (const SongEvent& e : s.events)
            CHECK_MSG(e.lyric.empty() || isBilingual(e.lyric) || !hasCjk(e.lyric), base + " lyric: " + e.lyric);
        std::string why;
        CHECK_MSG(playable(s, &why), base + ": " + why);
        bool fretsOk = true;
        for (const SongEvent& e : s.events)
            for (const SongNote& n : e.notes)
                if (n.fret > 20) fretsOk = false;
        CHECK_MSG(fretsOk, base);
        CHECK_MSG(!s.title.empty() && s.level >= 1 && s.level <= 5, base);
        CHECK_MSG(s.lengthBeats > 4 && s.lengthBeats < 2000 && s.lengthSec() < 600, base);
        CHECK_MSG(s.tempo.front().bpm >= 40 && s.tempo.front().bpm <= 200, base);
        CHECK_MSG(!s.description.empty(), base);
        setLanguage(Lang::En);
        std::printf("  %-26s %-39s bpm %3.0f  %d/%d  L%d  %3zu events  %5.1fs\n", base.c_str(), loc(s.title).c_str(), s.tempo.front().bpm,
                    s.timeSigs.front().num, s.timeSigs.front().den, s.level, s.events.size(), s.lengthSec());
        roundTrip(s, base);
    }
}

// ============================================================ ASCII tab
static void testAscii() {
    std::printf("[ascii tab]\n");
    {
        const char* t =
            "Twinkle-ish test\n"
            "e|-------0---|-------------|\n"
            "B|---1-3---3-|-1-----------|\n"
            "G|-0---------|---2-0-------|\n"
            "D|-----------|-------------|\n"
            "A|-----------|-------------|\n"
            "E|-----------|-------------|\n";
        Song s;
        std::string err;
        bool ok = parseAsciiTab(t, s, err);
        CHECK_MSG(ok, err);
        std::vector<int> want = {55, 60, 62, 64, 62, 60, 57, 55};
        CHECK(pitchSeq(s) == want);
        bool mono = true;
        for (size_t i = 1; i < s.events.size(); ++i)
            if (!(s.events[i].beat > s.events[i - 1].beat)) mono = false;
        CHECK(mono);
        CHECK(near(s.tempo.front().bpm, 80));
        std::string why;
        CHECK_MSG(playable(s, &why), why);
    }
    {
        const char* t =
            "Tempo: 100 bpm\n"
            "e|--12--10h12p10----|----------------|\n"
            "B|--------------13--|-15b17--15~~----|\n"
            "G|------------------|----------------|\n"
            "D|------------------|----------------|\n"
            "A|------------------|----------------|\n"
            "E|------------------|----------------|\n";
        Song s;
        std::string err;
        bool ok = parseAsciiTab(t, s, err);
        CHECK_MSG(ok, err);
        std::vector<int> want = {76, 74, 76, 74, 72, 74, 74};
        CHECK_MSG(pitchSeq(s) == want, "multi-digit/techniques");
        CHECK(near(s.tempo.front().bpm, 100));
        uint32_t fl = 0;
        for (const SongEvent& e : s.events)
            for (const SongNote& n : e.notes) fl |= n.flags;
        CHECK((fl & NF_Hammer) && (fl & NF_Pull) && (fl & NF_Bend) && (fl & NF_Vibrato));
    }
    {
        const char* t =
            "   G        C\n"
            "e|-3---3---|-0---0---|\n"
            "B|-0---0---|-1---1---|\n"
            "G|-0---0---|-0---0---|\n"
            "D|-0---0---|-2---2---|\n"
            "A|-2---2---|-3---3---|\n"
            "E|-3---3---|-----x---|\n"
            "\n"
            "e|-3-------|\n"
            "B|-0-------|\n"
            "G|-0-------|\n"
            "D|-0-------|\n"
            "A|-2-------|\n"
            "E|-3-------|\n";
        Song s;
        std::string err;
        bool ok = parseAsciiTab(t, s, err);
        CHECK_MSG(ok, err);
        CHECK(s.events.size() == 5);
        if (s.events.size() == 5) {
            CHECK(s.events[0].notes.size() == 6 && s.events[2].notes.size() == 5);
            CHECK(s.events[3].notes.size() == 6);  // includes a dead note
            CHECK(near(s.events[4].beat, 8.0));      // the second block follows the first
        }
    }
    {
        const char* t =
            "Tuning: D A D G B E\n"
            "Capo: 2\n"
            "e|---------|\n"
            "B|---------|\n"
            "G|---------|\n"
            "D|-------0-|\n"
            "A|---------|\n"
            "D|-0-0-5---|\n";
        Song s;
        std::string err;
        bool ok = parseAsciiTab(t, s, err);
        CHECK_MSG(ok, err);
        CHECK(s.tuning[5] == 38 && s.capo == 2);
        CHECK(!s.events.empty() && s.events[0].notes[0].midi == 38 + 2);
    }
    {
        // string labels in reverse order (low string on top)
        const char* t =
            "E|-0-----|\n"
            "A|---2---|\n"
            "D|-----2-|\n"
            "G|-------|\n"
            "B|-------|\n"
            "e|-------|\n";
        Song s;
        std::string err;
        CHECK(parseAsciiTab(t, s, err));
        std::vector<int> want = {40, 47, 52};
        CHECK(pitchSeq(s) == want);
    }
    Song s;
    std::string err;
    CHECK(!parseAsciiTab("No tab here\nonly some text\n", s, err));
    // auto-detection
    CHECK(parseSongText("e|--0--1--3--|\nB|-----------|\nG|-----------|\nD|-----------|\nA|-----------|\nE|-----------|\n", s, err));
    CHECK(parseSongText("bpm: 90\n3.0/4 3.2 2.0 2.1 |\n", s, err) && s.events.size() == 4);
}

// ============================================================ MIDI
static void putVlq(std::string& o, uint32_t v) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = v & 0x7F;
    while (v >>= 7) buf[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n--) o += (char)buf[n];
}
static void put32(std::string& o, uint32_t v) { for (int i = 3; i >= 0; --i) o += (char)((v >> (8 * i)) & 0xFF); }
static void put16(std::string& o, uint16_t v) { o += (char)(v >> 8); o += (char)(v & 0xFF); }
static std::string chunk(const char* id, const std::string& body) {
    std::string o(id, 4);
    put32(o, (uint32_t)body.size());
    return o + body;
}

static std::string makeMidi() {
    const int tpq = 480;
    std::string t0, t1, t2;
    // track 0: tempo 120, 4/4, changes to 60 at beat 4
    putVlq(t0, 0); t0 += "\xFF\x03\x04Song"; 
    putVlq(t0, 0); t0 += std::string("\xFF\x51\x03\x07\xA1\x20", 6);  // 500000us
    putVlq(t0, 0); t0 += std::string("\xFF\x58\x04\x04\x02\x18\x08", 7);
    putVlq(t0, 4 * tpq); t0 += std::string("\xFF\x51\x03\x0F\x42\x40", 6);  // 1000000us
    putVlq(t0, 0); t0 += std::string("\xFF\x2F\x00", 3);
    // track 1: guitar (program 25), melody E4 G4 A4 (half beat) B4 (half beat) + chord C4 E4 G4, using running status
    putVlq(t1, 0); t1 += "\xFF\x03\x06Guitar";
    putVlq(t1, 0); t1 += "\xC0\x19";
    struct N { int beat2; int len2; int pitch; };  // in half beats
    const N notes[] = {{0, 2, 64}, {2, 2, 67}, {4, 1, 69}, {5, 1, 71}, {6, 4, 60}, {6, 4, 64}, {6, 4, 67}, {10, 2, 72}};
    struct Ev { int tick; bool on; int pitch; };
    std::vector<Ev> evs;
    for (const N& n : notes) {
        evs.push_back({n.beat2 * tpq / 2, true, n.pitch});
        evs.push_back({(n.beat2 + n.len2) * tpq / 2 - 10, false, n.pitch});
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.tick < b.tick; });
    int last = 0;
    bool first = true;
    for (const Ev& e : evs) {
        putVlq(t1, (uint32_t)(e.tick - last));
        last = e.tick;
        if (first) { t1 += '\x90'; first = false; }  // running status from here on (velocity 0 = off)
        t1 += (char)e.pitch;
        t1 += (char)(e.on ? 100 : 0);
    }
    putVlq(t1, 0); t1 += std::string("\xFF\x2F\x00", 3);
    // track 2: drums (channel 10) with many notes - must be excluded
    for (int i = 0; i < 16; ++i) {
        putVlq(t2, i == 0 ? 0 : tpq / 2); t2 += "\x99\x24\x64";
        putVlq(t2, 10); t2 += std::string("\x89\x24\x00", 3);
    }
    putVlq(t2, 0); t2 += std::string("\xFF\x2F\x00", 3);
    std::string hdr;
    put16(hdr, 1); put16(hdr, 3); put16(hdr, tpq);
    return chunk("MThd", hdr) + chunk("MTrk", t0) + chunk("MTrk", t1) + chunk("MTrk", t2);
}

static void testMidi() {
    std::printf("[midi]\n");
    std::string data = makeMidi();
    Song s;
    std::string err;
    std::vector<TrackInfo> tracks;
    bool ok = detail::loadMidiBytes(data, s, err, -1, &tracks);
    CHECK_MSG(ok, err);
    CHECK(tracks.size() == 1);  // the drum track is excluded
    if (!tracks.empty()) CHECK(tracks[0].isGuitarLike && tracks[0].noteCount == 8);
    CHECK(s.events.size() == 6);
    if (s.events.size() == 6) {
        CHECK(near(s.events[0].beat, 0) && s.events[0].notes[0].midi == 64);
        CHECK(near(s.events[2].beat, 2) && near(s.events[2].durBeats, 0.5));
        CHECK(near(s.events[4].beat, 3) && s.events[4].notes.size() == 3);
        CHECK(near(s.events[5].beat, 5));
    }
    CHECK(s.tempo.size() == 2 && near(s.tempo[1].beat, 4) && near(s.tempo[1].bpm, 60));
    CHECK(near(s.beatToSec(5), 4 * 0.5 + 1 * 1.0));
    std::string why;
    CHECK_MSG(playable(s, &why), why);
    // load through a file (dispatch by extension)
    std::string p = writeTemp("t.mid", data);
    Song s2;
    CHECK(loadSongFile(p, s2, err) && s2.events.size() == 6 && s2.source == p);
    // unknown extensions are sniffed too
    std::string p2 = writeTemp("t.bin", data);
    CHECK(loadSongFile(p2, s2, err) && s2.events.size() == 6);
}

// ============================================================ MusicXML
static const char* kXmlTab = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE score-partwise PUBLIC "-//Recordare//DTD MusicXML 3.1 Partwise//EN" "http://www.musicxml.org/dtds/partwise.dtd">
<score-partwise version="3.1">
  <work><work-title>XML &amp; Tab</work-title></work>
  <identification><creator type="composer">Tester</creator></identification>
  <part-list>
    <score-part id="P1"><part-name>Flute</part-name></score-part>
    <score-part id="P2"><part-name>Acoustic Guitar</part-name>
      <midi-instrument id="P2-I1"><midi-program>26</midi-program></midi-instrument></score-part>
  </part-list>
  <part id="P1">
    <measure number="1"><attributes><divisions>1</divisions><time><beats>4</beats><beat-type>4</beat-type></time></attributes>
      <note><pitch><step>C</step><octave>5</octave></pitch><duration>4</duration></note></measure>
  </part>
  <part id="P2">
    <measure number="0" implicit="yes">
      <attributes><divisions>2</divisions><staves>2</staves>
        <time><beats>4</beats><beat-type>4</beat-type></time>
        <clef number="1"><sign>G</sign><line>2</line></clef>
        <clef number="2"><sign>TAB</sign><line>5</line></clef>
        <staff-details number="2"><staff-lines>6</staff-lines>
          <staff-tuning line="1"><tuning-step>D</tuning-step><tuning-octave>2</tuning-octave></staff-tuning>
          <staff-tuning line="2"><tuning-step>A</tuning-step><tuning-octave>2</tuning-octave></staff-tuning>
          <staff-tuning line="3"><tuning-step>D</tuning-step><tuning-octave>3</tuning-octave></staff-tuning>
          <staff-tuning line="4"><tuning-step>G</tuning-step><tuning-octave>3</tuning-octave></staff-tuning>
          <staff-tuning line="5"><tuning-step>B</tuning-step><tuning-octave>3</tuning-octave></staff-tuning>
          <staff-tuning line="6"><tuning-step>E</tuning-step><tuning-octave>4</tuning-octave></staff-tuning>
        </staff-details>
      </attributes>
      <direction><sound tempo="90"/></direction>
      <note><pitch><step>D</step><octave>4</octave></pitch><duration>2</duration><voice>1</voice><staff>1</staff></note>
      <backup><duration>2</duration></backup>
      <note><pitch><step>D</step><octave>3</octave></pitch><duration>2</duration><voice>5</voice><staff>2</staff>
        <notations><technical><string>2</string><fret>3</fret></technical></notations></note>
    </measure>
    <measure number="1">
      <barline location="left"><repeat direction="forward"/></barline>
      <harmony><root><root-step>G</root-step></root><kind>major</kind></harmony>
      <note><pitch><step>G</step><octave>3</octave></pitch><duration>4</duration><voice>1</voice><staff>1</staff></note>
      <note><pitch><step>B</step><octave>3</octave></pitch><duration>4</duration><voice>1</voice><staff>1</staff></note>
      <backup><duration>8</duration></backup>
      <note><pitch><step>G</step><octave>2</octave></pitch><duration>4</duration><voice>5</voice><staff>2</staff>
        <notations><technical><string>6</string><fret>5</fret></technical></notations></note>
      <note><chord/><pitch><step>B</step><octave>2</octave></pitch><duration>4</duration><voice>5</voice><staff>2</staff>
        <notations><technical><string>5</string><fret>2</fret></technical></notations></note>
      <note><pitch><step>A</step><octave>2</octave></pitch><duration>2</duration><voice>5</voice><staff>2</staff>
        <notations><technical><string>4</string><fret>7</fret><hammer-on type="start">H</hammer-on></technical></notations></note>
      <note><pitch><step>B</step><octave>2</octave></pitch><duration>2</duration><voice>5</voice><staff>2</staff>
        <notations><technical><string>4</string><fret>9</fret><hammer-on type="stop"/></technical></notations></note>
      <barline location="right"><repeat direction="backward"/></barline>
    </measure>
    <measure number="2">
      <note><rest/><duration>4</duration><staff>2</staff></note>
      <note><pitch><step>E</step><octave>3</octave></pitch><duration>4</duration><voice>5</voice><staff>2</staff>
        <tie type="start"/><notations><tied type="start"/><technical><string>4</string><fret>2</fret></technical></notations></note>
    </measure>
    <measure number="3">
      <note><pitch><step>E</step><octave>3</octave></pitch><duration>8</duration><voice>5</voice><staff>2</staff>
        <tie type="stop"/><notations><tied type="stop"/><technical><string>4</string><fret>2</fret></technical></notations></note>
    </measure>
  </part>
</score-partwise>
)";

static const char* kXmlPlain = R"(<?xml version="1.0"?>
<score-partwise version="4.0"><part-list><score-part id="P1"><part-name>Melody</part-name></score-part></part-list>
<part id="P1"><measure number="1"><attributes><divisions>4</divisions><time><beats>3</beats><beat-type>4</beat-type></time></attributes>
<direction><direction-type><metronome><beat-unit>half</beat-unit><per-minute>40</per-minute></metronome></direction-type></direction>
<note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration></note>
<note><pitch><step>E</step><octave>4</octave></pitch><duration>2</duration></note>
<note><pitch><step>F</step><alter>1</alter><octave>4</octave></pitch><duration>2</duration></note>
<note><pitch><step>G</step><octave>4</octave></pitch><duration>4</duration><lyric number="1"><text>la</text></lyric></note>
</measure><measure number="2">
<note><pitch><step>C</step><octave>2</octave></pitch><duration>12</duration></note>
</measure></part></score-partwise>
)";

// builds a ZIP (deflate) in memory
static std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, cd;
    auto p16 = [](std::string& o, uint16_t v) { o += (char)(v & 0xFF); o += (char)(v >> 8); };
    auto p32 = [](std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o += (char)((v >> (8 * i)) & 0xFF); };
    for (const auto& f : files) {
        z_stream zs{};
        deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        std::string comp(deflateBound(&zs, (uLong)f.second.size()) + 16, '\0');
        zs.next_in = (Bytef*)f.second.data();
        zs.avail_in = (uInt)f.second.size();
        zs.next_out = (Bytef*)&comp[0];
        zs.avail_out = (uInt)comp.size();
        deflate(&zs, Z_FINISH);
        comp.resize(zs.total_out);
        deflateEnd(&zs);
        uint32_t crc = (uint32_t)crc32(0, (const Bytef*)f.second.data(), (uInt)f.second.size());
        uint32_t off = (uint32_t)out.size();
        p32(out, 0x04034b50); p16(out, 20); p16(out, 0); p16(out, 8); p16(out, 0); p16(out, 0);
        p32(out, crc); p32(out, (uint32_t)comp.size()); p32(out, (uint32_t)f.second.size());
        p16(out, (uint16_t)f.first.size()); p16(out, 0);
        out += f.first + comp;
        p32(cd, 0x02014b50); p16(cd, 20); p16(cd, 20); p16(cd, 0); p16(cd, 8); p16(cd, 0); p16(cd, 0);
        p32(cd, crc); p32(cd, (uint32_t)comp.size()); p32(cd, (uint32_t)f.second.size());
        p16(cd, (uint16_t)f.first.size()); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, off);
        cd += f.first;
    }
    uint32_t cdOff = (uint32_t)out.size();
    out += cd;
    p32(out, 0x06054b50); p16(out, 0); p16(out, 0); p16(out, (uint16_t)files.size()); p16(out, (uint16_t)files.size());
    p32(out, (uint32_t)cd.size()); p32(out, cdOff); p16(out, 0);
    return out;
}

static void testMusicXml() {
    std::printf("[musicxml]\n");
    Song s;
    std::string err;
    std::vector<TrackInfo> tracks;
    bool ok = detail::loadMusicXmlString(kXmlTab, s, err, -1, &tracks);
    CHECK_MSG(ok, err);
    if (!ok) return;
    CHECK(tracks.size() == 2 && tracks[1].isGuitarLike && !tracks[0].isGuitarLike);
    CHECK(s.title == "XML & Tab" && s.artist == "Tester");
    CHECK(s.tuning[5] == 38);  // Drop D
    CHECK(near(s.tempo.front().bpm, 90));
    // 1-beat pickup: padded with 3 beats. Events: 3: B string fret 3 (D4)
    //  bar 1 (repeated twice): 4: G2+B2 (string 6 fret 5 + string 5 fret 2), 6: string 4 fret 7, 7: string 4 fret 9
    //  (hammer-on)  x2 -> 8, 10, 11
    //  bar 2: 14: string 4 fret 2 (tied over 4 beats -> merged into 6 beats)
    std::vector<double> beats;
    for (const SongEvent& e : s.events) beats.push_back(e.beat);
    std::vector<double> want = {3, 4, 6, 7, 8, 10, 11, 14};
    CHECK_MSG(beats == want, "beats");
    if (s.events.size() == 8) {
        CHECK(s.events[0].notes[0].string == 1 && s.events[0].notes[0].fret == 3);
        CHECK(s.events[1].notes.size() == 2 && s.events[1].chordName == "G");
        CHECK(s.events[3].notes[0].flags & NF_Hammer);
        CHECK(near(s.events[7].notes[0].durBeats, 6.0));
    }
    std::string why;
    CHECK_MSG(playable(s, &why), why);
    // select the other part
    Song f;
    CHECK(detail::loadMusicXmlString(kXmlTab, f, err, 0, nullptr) && f.events.size() == 1);

    Song p;
    ok = detail::loadMusicXmlString(kXmlPlain, p, err, -1, nullptr);
    CHECK_MSG(ok, err);
    if (ok) {
        CHECK(near(p.tempo.front().bpm, 80));  // half note = 40 -> quarter 80
        CHECK(p.timeSigs.front().num == 3);
        CHECK(p.events.size() == 5);
        CHECK_MSG(playable(p, &why), why);
        bool lowPos = true;
        for (const SongEvent& e : p.events)
            for (const SongNote& n : e.notes)
                if (n.fret > 5) lowPos = false;
        CHECK(lowPos);
        CHECK(p.events.size() > 3 && p.events[3].lyric == "la");
        CHECK(p.events.back().notes[0].midi == 48);  // C2 is below the guitar's range, folded to C3
    }
    // .mxl
    std::string container =
        "<?xml version=\"1.0\"?><container><rootfiles><rootfile full-path=\"score/song.musicxml\"/></rootfiles></container>";
    std::string mxl = makeZip({{"META-INF/container.xml", container}, {"score/song.musicxml", kXmlTab}});
    std::string path = writeTemp("t.mxl", mxl);
    Song m;
    ok = loadSongFile(path, m, err);
    CHECK_MSG(ok, err);
    CHECK(m.events.size() == 8 && m.title == "XML & Tab");
    // pasted-text detection
    Song pt;
    CHECK(parseSongText(kXmlPlain, pt, err) && pt.events.size() == 5);
}

// ============================================================ text encodings
static std::string toUtf16(const std::string& utf8, bool be, bool bom) {
    iconv_t cd = iconv_open(be ? "UTF-16BE" : "UTF-16LE", "UTF-8");
    std::string out(utf8.size() * 4 + 8, '\0');
    char* src = const_cast<char*>(utf8.data());
    size_t sl = utf8.size(), dl = out.size();
    char* dst = &out[0];
    iconv(cd, &src, &sl, &dst, &dl);
    iconv_close(cd);
    out.resize(out.size() - dl);
    if (bom) out = (be ? std::string("\xFE\xFF") : std::string("\xFF\xFE")) + out;
    return out;
}
static std::string toGbk(const std::string& utf8) {
    iconv_t cd = iconv_open("GB18030", "UTF-8");
    std::string out(utf8.size() * 2 + 8, '\0');
    char* src = const_cast<char*>(utf8.data());
    size_t sl = utf8.size(), dl = out.size();
    char* dst = &out[0];
    iconv(cd, &src, &sl, &dst, &dl);
    iconv_close(cd);
    out.resize(out.size() - dl);
    return out;
}

static void testEncodings() {
    std::printf("[encodings]\n");
    std::string xml = std::string(kXmlPlain);
    size_t p = xml.find("encoding");
    (void)p;
    for (int v = 0; v < 3; ++v) {
        std::string d = toUtf16(xml, v == 1, v != 2);
        Song s;
        std::string err;
        bool ok = detail::loadMusicXmlString(d, s, err, -1, nullptr);
        CHECK_MSG(ok && s.events.size() == 5, "utf16 variant " + std::to_string(v) + ": " + err);
    }
    // GBK-encoded gtab (common on Chinese Windows); Chinese test data on purpose
    std::string g = toGbk("title: 两只老虎\nbpm: 90\n3.0/4 3.2 2.0 3.0 |\n");
    CHECK(!detail::isValidUtf8(g));
    std::string path = writeTemp("gbk.gtab", g);
    Song s;
    std::string err;
    CHECK(loadSongFile(path, s, err) && s.title == "两只老虎" && s.events.size() == 4);
    // GBK-encoded ASCII tab (.txt)
    std::string t = toGbk("歌名: 测试\ne|--0--1--3--|\nB|-----------|\nG|-----------|\nD|-----------|\nA|-----------|\nE|-----------|\n");
    CHECK(loadSongFile(writeTemp("gbk.txt", t), s, err) && s.events.size() == 3);
    // XML declaring latin1
    std::string lat = std::string("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n<score-partwise><work><work-title>Caf\xE9</work-title></work>") +
                      "<part-list><score-part id=\"P1\"><part-name>G</part-name></score-part></part-list><part id=\"P1\"><measure>" +
                      "<attributes><divisions>1</divisions></attributes><note><pitch><step>E</step><octave>4</octave></pitch>" +
                      "<duration>4</duration></note></measure></part></score-partwise>";
    CHECK(detail::loadMusicXmlString(lat, s, err, -1, nullptr) && s.title == "Caf\xC3\xA9");
}

// ============================================================ auto-fingering
static void testFingering() {
    std::printf("[autoFinger]\n");
    Song s;
    const int scale[] = {48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 65, 64, 62, 60};
    double b = 0;
    for (int m : scale) {
        SongEvent e;
        e.beat = b;
        e.durBeats = 0.5;
        SongNote n;
        n.midi = m;
        n.durBeats = 0.5;
        e.notes.push_back(n);
        s.events.push_back(e);
        b += 0.5;
    }
    // C major chord + a very low note (must be folded up)
    SongEvent ch;
    ch.beat = b;
    for (int m : {48, 52, 55, 60, 64}) {
        SongNote n;
        n.midi = m;
        ch.notes.push_back(n);
    }
    s.events.push_back(ch);
    SongEvent low;
    low.beat = b + 1;
    SongNote ln;
    ln.midi = 28;  // E1
    low.notes.push_back(ln);
    s.events.push_back(low);
    s.finalize();
    std::string why;
    CHECK_MSG(playable(s, &why), why);
    int maxFret = 0;
    for (size_t i = 0; i < 16 && i < s.events.size(); ++i) maxFret = std::max(maxFret, s.events[i].notes[0].fret);
    CHECK_MSG(maxFret <= 4, "scale max fret " + std::to_string(maxFret));
    CHECK(s.events.size() == 18);
    if (s.events.size() == 18) {
        const SongEvent& c = s.events[16];
        CHECK(c.notes.size() == 5);
        int mn = 99, mx = 0;
        for (const SongNote& n : c.notes)
            if (n.fret > 0) { mn = std::min(mn, n.fret); mx = std::max(mx, n.fret); }
        CHECK(mx - mn <= 3);
        CHECK(s.events[17].notes[0].midi == 40);  // E1 -> E2
    }
}

// ============================================================ GPIF (GP6/7)
static const char* kGpif = R"(<?xml version="1.0" encoding="utf-8"?>
<GPIF>
<GPVersion>7</GPVersion>
<Score><Title><![CDATA[GPIF Test]]></Title><Artist><![CDATA[Me]]></Artist></Score>
<MasterTrack><Tracks>0 1</Tracks><Automations><Automation><Type>Tempo</Type><Linear>false</Linear><Bar>0</Bar><Position>0</Position>
<Visible>true</Visible><Value>50 4</Value></Automation></Automations></MasterTrack>
<Tracks>
<Track id="0"><Name><![CDATA[Drums]]></Name></Track>
<Track id="1"><Name><![CDATA[Lead]]></Name><Staves><Staff><Properties>
<Property name="Tuning"><Pitches>40 45 50 55 59 64</Pitches></Property><Property name="CapoFret"><Fret>0</Fret></Property>
<Property name="DiagramCollection"><Items><Item id="0" name="Am"/></Items></Property>
</Properties></Staff></Staves></Track>
</Tracks>
<MasterBars>
<MasterBar><Time>4/4</Time><Bars>0 2</Bars><Section><Letter><![CDATA[A]]></Letter><Text><![CDATA[Intro]]></Text></Section></MasterBar>
<MasterBar><Time>4/4</Time><Bars>1 3</Bars><Repeat start="false" end="true" count="2"/></MasterBar>
</MasterBars>
<Bars><Bar id="0"><Voices>-1 -1 -1 -1</Voices></Bar><Bar id="1"><Voices>-1 -1 -1 -1</Voices></Bar>
<Bar id="2"><Voices>0 -1 -1 -1</Voices></Bar><Bar id="3"><Voices>1 -1 -1 -1</Voices></Bar></Bars>
<Voices><Voice id="0"><Beats>0 1 2</Beats></Voice><Voice id="1"><Beats>3</Beats></Voice></Voices>
<Beats>
<Beat id="0"><Rhythm ref="0"/><Notes>0</Notes></Beat>
<Beat id="1"><Rhythm ref="0"/><Chord>0</Chord><Notes>1 2</Notes><Properties><Property name="Brush"><Direction>Up</Direction></Property></Properties></Beat>
<Beat id="2"><Rhythm ref="1"/><Notes>3</Notes></Beat>
<Beat id="3"><Rhythm ref="2"/><Notes>4</Notes></Beat>
</Beats>
<Notes>
<Note id="0"><Properties><Property name="String"><String>0</String></Property><Property name="Fret"><Fret>3</Fret></Property>
<Property name="HopoOrigin"><Enable/></Property></Properties></Note>
<Note id="1"><Properties><Property name="String"><String>5</String></Property><Property name="Fret"><Fret>0</Fret></Property></Properties></Note>
<Note id="2"><Properties><Property name="String"><String>4</String></Property><Property name="Fret"><Fret>1</Fret></Property></Properties></Note>
<Note id="3"><Properties><Property name="String"><String>2</String></Property><Property name="Fret"><Fret>2</Fret></Property>
<Property name="PalmMuted"><Enable/></Property></Properties><Tie origin="false" destination="false"/></Note>
<Note id="4"><Properties><Property name="String"><String>3</String></Property><Property name="Fret"><Fret>0</Fret></Property></Properties></Note>
</Notes>
<Rhythms>
<Rhythm id="0"><NoteValue>Quarter</NoteValue></Rhythm>
<Rhythm id="1"><NoteValue>Quarter</NoteValue><AugmentationDot count="1"/><PrimaryTuplet num="3" den="2"/></Rhythm>
<Rhythm id="2"><NoteValue>Whole</NoteValue></Rhythm>
</Rhythms>
</GPIF>
)";

// simple BCFZ encoder (LZ with 8-bit offset/length + literals) to exercise the decompressor
struct BitOut {
    std::string out;
    int nbits = 0;
    uint8_t cur = 0;
    void bit(int b) { cur = (uint8_t)((cur << 1) | (b & 1)); if (++nbits == 8) { out += (char)cur; cur = 0; nbits = 0; } }
    void bits(uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) bit((v >> i) & 1); }
    void bitsRev(uint32_t v, int n) { for (int i = 0; i < n; ++i) bit((v >> i) & 1); }
    void flush() { while (nbits) bit(0); }
};
static std::string bcfzEncode(const std::string& data) {
    BitOut w;
    uint32_t n = (uint32_t)data.size();
    for (int i = 0; i < 4; ++i) w.bits((n >> (8 * i)) & 0xFF, 8);
    size_t i = 0;
    while (i < data.size()) {
        size_t bestLen = 0, bestOff = 0;
        for (size_t off = 1; off <= 255 && off <= i; ++off) {
            size_t len = 0;
            while (len < off && len < 255 && i + len < data.size() && data[i + len] == data[i - off + len]) ++len;
            if (len > bestLen) { bestLen = len; bestOff = off; }
        }
        if (bestLen >= 4) {
            w.bit(1);
            w.bits(8, 4);
            w.bitsRev((uint32_t)bestOff, 8);
            w.bitsRev((uint32_t)bestLen, 8);
            i += bestLen;
        } else {
            size_t k = std::min<size_t>(3, data.size() - i);
            w.bit(0);
            w.bitsRev((uint32_t)k, 2);
            for (size_t j = 0; j < k; ++j) w.bits((uint8_t)data[i + j], 8);
            i += k;
        }
    }
    w.flush();
    return "BCFZ" + w.out;
}
static std::string makeBcfs(const std::string& name, const std::string& content) {
    const size_t S = 0x1000;
    size_t sectors = (content.size() + S - 1) / S;
    std::string fs(S * (2 + sectors), '\0');
    std::fill(fs.begin(), fs.begin() + (long)S, (char)0xFF);
    auto w32 = [&](size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) fs[off + i] = (char)((v >> (8 * i)) & 0xFF); };
    w32(S, 2);
    std::memcpy(&fs[S + 4], name.data(), name.size());
    w32(S + 0x8c, (uint32_t)content.size());
    for (size_t k = 0; k < sectors; ++k) w32(S + 0x94 + 4 * k, (uint32_t)(2 + k));
    std::memcpy(&fs[2 * S], content.data(), content.size());
    return "BCFS" + fs;
}

static void checkGpifSong(const Song& s, const std::string& label) {
    // order: bar 0, bar 1, (repeat) bar 0, bar 1
    std::vector<double> beats;
    for (const SongEvent& e : s.events) beats.push_back(e.beat);
    std::vector<double> want = {0, 1, 2, 4, 8, 9, 10, 12};
    CHECK_MSG(beats == want, label + " beats");
    CHECK_MSG(near(s.tempo.front().bpm, 100), label + " tempo");  // 50 (half notes) -> 100
    CHECK_MSG(s.title == "GPIF Test", label);
    if (s.events.size() == 8) {
        CHECK(s.events[0].notes[0].string == 5 && s.events[0].notes[0].fret == 3);
        CHECK(s.events[1].notes.size() == 2 && s.events[1].chordName == "Am" && s.events[1].strum == Strum::Up);
        CHECK(near(s.events[2].durBeats, 1.0));  // dotted quarter triplet = 1.5 * 2/3
        CHECK(s.events[2].notes[0].string == 3 && (s.events[2].notes[0].flags & NF_PalmMute));
        CHECK(s.events[3].notes[0].string == 2 && near(s.events[3].durBeats, 4));
    }
    CHECK(!s.sections.empty() && s.sections[0].name == "Intro");
    std::string why;
    CHECK_MSG(playable(s, &why), label + ": " + why);
}

static void testGpif() {
    std::printf("[gpif / gp7 / gpx]\n");
    Song s;
    std::string err;
    std::vector<TrackInfo> tracks;
    bool ok = detail::loadGpifXml(kGpif, s, err, -1, &tracks);
    CHECK_MSG(ok, err);
    CHECK(tracks.size() == 1 && tracks[0].name == "Lead");  // the drum track without tuning is excluded
    if (ok) checkGpifSong(s, "gpif");
    // GP7: ZIP
    std::string gp = makeZip({{"VERSION", "7.0"}, {"Content/score.gpif", kGpif}});
    Song s7;
    ok = loadSongFile(writeTemp("t.gp", gp), s7, err);
    CHECK_MSG(ok, err);
    if (ok) checkGpifSong(s7, "gp7");
    // GP6: BCFS (uncompressed) and BCFZ (compressed)
    std::string bcfs = makeBcfs("score.gpif", kGpif);
    Song s6;
    ok = loadSongFile(writeTemp("t.gpx", bcfs), s6, err);
    CHECK_MSG(ok, "BCFS: " + err);
    if (ok) checkGpifSong(s6, "gpx-bcfs");
    std::string bcfz = bcfzEncode(bcfs);
    CHECK(bcfz.size() < bcfs.size());
    Song s6z;
    ok = loadSongFile(writeTemp("t2.gpx", bcfz), s6z, err);
    CHECK_MSG(ok, "BCFZ: " + err);
    if (ok) checkGpifSong(s6z, "gpx-bcfz");
}

// ============================================================ fuzzing
static void fuzzBytes(const std::string& name, const std::string& seed, int iters, const std::function<void(const std::string&)>& fn) {
    std::mt19937 rng(12345);
    for (int it = 0; it < iters; ++it) {
        std::string d = seed;
        int mode = (int)(rng() % 4);
        if (mode == 0 && !d.empty()) d.resize(rng() % d.size());
        int flips = 1 + (int)(rng() % 16);
        for (int k = 0; k < flips && !d.empty(); ++k) {
            size_t p = rng() % d.size();
            switch (rng() % 4) {
                case 0: d[p] = (char)(rng() & 0xFF); break;
                case 1: d[p] = (char)(d[p] ^ (1 << (rng() % 8))); break;
                case 2: d[p] = (char)(rng() % 2 ? 0x7F : 0xFF); break;
                case 3: d.insert(d.begin() + (long)p, (char)(rng() & 0xFF)); break;
            }
        }
        if (mode == 3 && d.size() > 8) {
            size_t a = rng() % d.size(), len = rng() % std::min<size_t>(64, d.size() - a);
            d.erase(a, len);
        }
        fn(d);
    }
    std::printf("  fuzz %-12s %d iterations, no crash\n", name.c_str(), iters);
}

static void testFuzz(const std::vector<std::string>& realBinaries) {
    std::printf("[fuzz]\n");
    int N = 3000;
    if (const char* e = std::getenv("GS_FUZZ_ITERS")) N = std::max(10, std::atoi(e));
    Song s;
    std::string err;
    std::vector<TrackInfo> trk;
    fuzzBytes("midi", makeMidi(), N, [&](const std::string& d) { detail::loadMidiBytes(d, s, err, -1, &trk); });
    fuzzBytes("musicxml", kXmlTab, N, [&](const std::string& d) { detail::loadMusicXmlString(d, s, err, -1, &trk); });
    fuzzBytes("gpif", kGpif, N, [&](const std::string& d) { detail::loadGpifXml(d, s, err, -1, &trk); });
    std::string mxl = makeZip({{"META-INF/container.xml", "<container/>"}, {"a.musicxml", kXmlTab}});
    fuzzBytes("zip", mxl, N, [&](const std::string& d) {
        std::vector<detail::ZipEntry> es;
        if (detail::zipList(d, es, err))
            for (const auto& e : es) {
                std::string o;
                if (detail::zipExtract(d, e, o, err, 1 << 20)) detail::loadMusicXmlString(o, s, err, -1, nullptr);
            }
    });
    std::string bcfz = bcfzEncode(makeBcfs("score.gpif", kGpif));
    fuzzBytes("gpx", bcfz, N, [&](const std::string& d) {
        std::string x;
        if (detail::gpxExtractScore(d, x, err)) detail::loadGpifXml(x, s, err, -1, nullptr);
    });
    for (const char* v : {"v3.00", "v4.06", "v5.00", "v5.10"}) {
        std::string hdr = std::string("FICHIER GUITAR PRO ") + v;
        std::string seed = std::string(1, (char)hdr.size()) + hdr + std::string(30 - hdr.size(), '\0');
        std::mt19937 rng(7);
        for (int k = 0; k < 2000; ++k) seed += (char)(rng() % 4 == 0 ? rng() & 0xFF : rng() % 8);
        fuzzBytes(std::string("gp ") + v, seed, N / 2, [&](const std::string& d) { detail::loadGp345Bytes(d, s, err, -1, &trk); });
    }
    std::string gtabSeed = "bpm: 90\ntime: 3/4\n[G]/4v 3.2/8. E4/16t r/2 | {bpm=70} 1.0+2.1~ \"x\" @A 6.3/32 |\n";
    fuzzBytes("gtab", gtabSeed, N, [&](const std::string& d) { parseGtab(d, s, err); writeGtab(s); });
    std::string asciiSeed = "e|--12--10h12p10----|\nB|-------------13---|\nG|-0---2--4b6r4-----|\nD|--------x---------|\nA|-----------/7\\5---|\nE|-3-----------------|\n";
    fuzzBytes("ascii", asciiSeed, N, [&](const std::string& d) { parseAsciiTab(d, s, err); });
    for (const std::string& p : realBinaries) {
        std::string data;
        if (!detail::readFileBytes(p, data, err)) continue;
        std::string ext = detail::fileExtLower(p);
        fuzzBytes(p.substr(p.find_last_of('/') + 1).substr(0, 12), data, std::max(50, N / 30), [&](const std::string& d) {
            if (ext == ".gpx") {
                std::string x;
                if (detail::gpxExtractScore(d, x, err)) detail::loadGpifXml(x, s, err, -1, nullptr);
            } else if (ext == ".gp") {
                std::string x;
                if (detail::zipExtractByName(d, "Content/score.gpif", x, err)) detail::loadGpifXml(x, s, err, -1, nullptr);
            } else if (ext == ".mid" || ext == ".midi") {
                detail::loadMidiBytes(d, s, err, -1, nullptr);
            } else if (ext == ".xml" || ext == ".musicxml") {
                detail::loadMusicXmlString(d, s, err, -1, nullptr);
            } else {
                detail::loadGp345Bytes(d, s, err, -1, nullptr);
            }
        });
    }
}

// ============================================================ real files (optional)
static std::vector<std::string> testRealFiles() {
    std::vector<std::string> bins;
    const char* dir = std::getenv("GS_TESTDATA_DIR");
    if (!dir || !*dir) return bins;
    std::printf("[real files: %s]\n", dir);
    const auto& exts = supportedSongExtensions();
    int okCount = 0, total = 0;
    for (const std::string& p : listDir(dir, true)) {
        std::string ext = detail::fileExtLower(p);
        if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
        ++total;
        Song s;
        std::string err;
        std::vector<TrackInfo> tracks;
        bool ok = loadSongFile(p, s, err, -1, &tracks);
        std::string base = p.substr(std::string(dir).size() + 1);
        if (ok) {
            ++okCount;
            std::string why;
            bool pl = playable(s, &why);
            CHECK_MSG(pl, base + ": " + why);
            std::string names;
            for (size_t i = 0; i < tracks.size() && i < 3; ++i) names += (i ? ", " : "") + tracks[i].name;
            if (tracks.size() > 3) names += ", ...";
            std::printf("  OK   %-48s \"%s\" %zu tracks [%s] %zu events %.0f bpm %.0f beats%s\n", base.c_str(), s.title.c_str(),
                        tracks.size(), names.c_str(), s.events.size(), s.tempo.front().bpm, s.lengthBeats, pl ? "" : " (NOT PLAYABLE!)");
        } else {
            std::printf("  FAIL %-48s %s\n", base.c_str(), err.c_str());
        }
        if (ext != ".txt" && ext != ".gtab" && ext != ".tab") bins.push_back(p);
    }
    std::printf("  real files: %d/%d loaded\n", okCount, total);
    return bins;
}

int main() {
    testNames();
    testChords();
    testTempoMap();
    testGtab();
    testLessons();
    testAscii();
    testMidi();
    testMusicXml();
    testEncodings();
    testFingering();
    testGpif();
    std::vector<std::string> bins = testRealFiles();
    testFuzz(bins);
    // clean up temporary files
    if (!tempDir().empty()) {
        for (const std::string& f : listDir(tempDir(), false)) unlink(f.c_str());
        rmdir(tempDir().c_str());
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
