// MusicXML import (.xml / .musicxml / .mxl). Supports partwise and timewise scores.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "song/SongInternal.h"

namespace gs {

using namespace detail;

namespace {

struct MeasureRef {
    const XmlNode* content = nullptr;  // element containing the notes/attributes
    const XmlNode* attrs = nullptr;    // measure element carrying the number/implicit attributes
};

struct PartRef {
    std::string id, name;
    int midiProgram = -1;  // 1-based (MusicXML)
    std::vector<MeasureRef> measures;
};

int stepToPc(const std::string& s) {
    if (s.empty()) return -1;
    switch (std::toupper((unsigned char)s[0])) {
        case 'C': return 0;
        case 'D': return 2;
        case 'E': return 4;
        case 'F': return 5;
        case 'G': return 7;
        case 'A': return 9;
        case 'B': return 11;
        default: return -1;
    }
}

int toInt(const std::string& s, int def = 0) {
    double v;
    if (!parseDoubleStrict(s, v) || !std::isfinite(v) || std::fabs(v) > 1e9) return def;
    return (int)std::lround(v);
}

double toDouble(const std::string& s, double def = 0) {
    double v;
    if (!parseDoubleStrict(s, v) || !std::isfinite(v)) return def;
    return v;
}

std::string harmonyName(const XmlNode* h) {
    const XmlNode* root = h->child("root");
    if (!root) return {};
    std::string step = root->childText("root-step");
    if (step.empty()) return {};
    std::string name(1, (char)std::toupper((unsigned char)step[0]));
    int alter = toInt(root->childText("root-alter", "0"));
    if (alter > 0) name += std::string((size_t)std::min(alter, 2), '#');
    if (alter < 0) name += std::string((size_t)std::min(-alter, 2), 'b');
    const XmlNode* kind = h->child("kind");
    std::string k = kind ? kind->trimmedText() : "major";
    static const std::map<std::string, std::string> kSuf = {
        {"major", ""},          {"minor", "m"},           {"dominant", "7"},         {"major-seventh", "maj7"},
        {"minor-seventh", "m7"}, {"diminished", "dim"},    {"augmented", "aug"},      {"suspended-fourth", "sus4"},
        {"suspended-second", "sus2"}, {"power", "5"},      {"major-sixth", "6"},      {"minor-sixth", "m6"},
        {"dominant-ninth", "9"}, {"half-diminished", "m7b5"}, {"diminished-seventh", "dim7"}, {"major-ninth", "maj9"},
        {"minor-ninth", "m9"},  {"dominant-11th", "11"},  {"dominant-13th", "13"},   {"none", ""}};
    auto it = kSuf.find(k);
    name += it != kSuf.end() ? it->second : "";
    if (const XmlNode* bass = h->child("bass")) {
        std::string bs = bass->childText("bass-step");
        if (!bs.empty()) {
            name += "/" + std::string(1, (char)std::toupper((unsigned char)bs[0]));
            int ba = toInt(bass->childText("bass-alter", "0"));
            if (ba > 0) name += "#";
            if (ba < 0) name += "b";
        }
    }
    return name;
}

struct StaffInfo {
    int lines = 5;
    bool tabClef = false;
    int tuning[8] = {0};
    bool hasTuning = false;
};

// pre-scan: note count per staff, notes with string/fret data, and staff details
struct PartScan {
    std::map<int, int> notesPerStaff, tabNotesPerStaff;
    std::map<int, StaffInfo> staves;
    int noteCount = 0;
    bool guitarHint = false;
};

void scanAttributes(const XmlNode* at, std::map<int, StaffInfo>& staves) {
    for (const XmlNode* c : at->childrenNamed("clef")) {
        int num = toInt(c->attrOr("number", "1"), 1);
        if (iequals(c->childText("sign"), "TAB")) staves[num].tabClef = true;
    }
    for (const XmlNode* sd : at->childrenNamed("staff-details")) {
        int num = toInt(sd->attrOr("number", "1"), 1);
        StaffInfo& si = staves[num];
        std::string sl = sd->childText("staff-lines");
        if (!sl.empty()) si.lines = std::clamp(toInt(sl, 5), 1, 8);
        for (const XmlNode* st : sd->childrenNamed("staff-tuning")) {
            int line = toInt(st->attrOr("line", "0"));
            int pc = stepToPc(st->childText("tuning-step"));
            if (line < 1 || line > 8 || pc < 0) continue;
            int oct = std::clamp(toInt(st->childText("tuning-octave", "3"), 3), -1, 10);
            int alt = std::clamp(toInt(st->childText("tuning-alter", "0")), -3, 3);
            si.tuning[line - 1] = std::clamp((oct + 1) * 12 + pc + alt, 0, 127);
            si.hasTuning = true;
        }
    }
}

PartScan scanPart(const PartRef& p) {
    PartScan ps;
    std::string lname = toLower(p.name);
    if (lname.find("guitar") != std::string::npos || lname.find("gtr") != std::string::npos ||
        p.name.find("\xE5\x90\x89\xE4\xBB\x96") != std::string::npos ||  // "guitar" in Chinese
        lname.find("bass") != std::string::npos)
        ps.guitarHint = true;
    if (p.midiProgram >= 25 && p.midiProgram <= 40) ps.guitarHint = true;
    for (const MeasureRef& m : p.measures) {
        for (const auto& ch : m.content->children) {
            if (ch->name == "attributes") scanAttributes(ch.get(), ps.staves);
            if (ch->name != "note") continue;
            const XmlNode* n = ch.get();
            if (n->child("rest") || n->child("grace") || n->child("cue")) continue;
            if (!n->child("pitch") && !n->path("notations/technical/fret")) continue;
            int staff = toInt(n->childText("staff", "1"), 1);
            ps.notesPerStaff[staff]++;
            ps.noteCount++;
            if (n->path("notations/technical/string") && n->path("notations/technical/fret")) ps.tabNotesPerStaff[staff]++;
        }
    }
    for (auto& kv : ps.staves)
        if (kv.second.tabClef || kv.second.lines == 6) ps.guitarHint = true;
    if (!ps.tabNotesPerStaff.empty()) ps.guitarHint = true;
    return ps;
}

// expand repeats: returns the playback order of the measures
std::vector<size_t> playOrder(const std::vector<MeasureRef>& ms) {
    const size_t M = ms.size();
    std::vector<bool> fwd(M, false), bwd(M, false);
    std::vector<int> times(M, 2);
    std::vector<std::set<int>> endings(M);
    std::set<int> curEnding;
    for (size_t i = 0; i < M; ++i) {
        for (const XmlNode* bl : ms[i].content->childrenNamed("barline")) {
            if (const XmlNode* r = bl->child("repeat")) {
                std::string dir = r->attrOr("direction");
                if (dir == "forward") fwd[i] = true;
                if (dir == "backward") {
                    bwd[i] = true;
                    times[i] = std::clamp(toInt(r->attrOr("times", "2"), 2), 2, 8);
                }
            }
            if (const XmlNode* e = bl->child("ending")) {
                std::string type = e->attrOr("type");
                if (type == "start") {
                    curEnding.clear();
                    std::string nums = e->attrOr("number");
                    int v = 0;
                    bool have = false;
                    for (char c : nums + ",") {
                        if (std::isdigit((unsigned char)c)) { v = v * 10 + (c - '0'); have = true; }
                        else if (have) { curEnding.insert(v); v = 0; have = false; }
                    }
                }
                if (!curEnding.empty()) endings[i] = curEnding;
                if (type == "stop" || type == "discontinue") curEnding.clear();
            }
        }
        if (!curEnding.empty()) endings[i] = curEnding;
    }
    std::vector<size_t> order;
    std::vector<int> jumps(M, 0);
    size_t i = 0, repeatStart = 0;
    int pass = 1;
    bool justJumped = false;
    while (i < M && order.size() < 20000) {
        if (fwd[i] && !justJumped) { repeatStart = i; pass = 1; }
        justJumped = false;
        if (!endings[i].empty() && !endings[i].count(pass)) {
            // an ending that doesn't belong to this pass: skip it (if no later ending matches, continue normally)
            ++i;
            continue;
        }
        order.push_back(i);
        if (bwd[i]) {
            if (jumps[i] < times[i] - 1) {
                jumps[i]++;
                pass++;
                i = repeatStart;
                justJumped = true;
                continue;
            }
            jumps[i] = 0;
            pass = 1;
            repeatStart = i + 1;
        }
        ++i;
    }
    return order;
}

}  // namespace

bool detail::loadMusicXmlString(const std::string& xml, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks) {
    out = Song();
    std::unique_ptr<XmlNode> doc = parseXml(xml, err);
    if (!doc) return false;
    bool timewise = doc->name == "score-timewise";
    if (doc->name != "score-partwise" && !timewise) { err = tr("Not a MusicXML score (root element: ") + doc->name + ")"; return false; }

    // ---- title
    if (const XmlNode* w = doc->child("work")) out.title = w->childText("work-title");
    if (out.title.empty()) out.title = doc->childText("movement-title");
    if (const XmlNode* id = doc->child("identification"))
        for (const XmlNode* c : id->childrenNamed("creator"))
            if (out.artist.empty() || c->attrOr("type") == "composer") out.artist = c->trimmedText();

    // ---- part list
    std::vector<PartRef> parts;
    std::map<std::string, size_t> partIdx;
    if (const XmlNode* pl = doc->child("part-list")) {
        for (const XmlNode* sp : pl->childrenNamed("score-part")) {
            PartRef p;
            p.id = sp->attrOr("id");
            p.name = sp->childText("part-name");
            if (const XmlNode* mi = sp->child("midi-instrument")) p.midiProgram = toInt(mi->childText("midi-program", "-1"), -1);
            partIdx[p.id] = parts.size();
            parts.push_back(p);
        }
    }
    auto partFor = [&](const std::string& id) -> PartRef& {
        auto it = partIdx.find(id);
        if (it == partIdx.end()) {
            PartRef p;
            p.id = id;
            p.name = id;
            partIdx[id] = parts.size();
            parts.push_back(p);
            return parts.back();
        }
        return parts[it->second];
    };
    if (!timewise) {
        for (const XmlNode* pn : doc->childrenNamed("part")) {
            PartRef& p = partFor(pn->attrOr("id"));
            for (const XmlNode* m : pn->childrenNamed("measure")) p.measures.push_back({m, m});
        }
    } else {
        for (const XmlNode* m : doc->childrenNamed("measure"))
            for (const XmlNode* pn : m->childrenNamed("part")) partFor(pn->attrOr("id")).measures.push_back({pn, m});
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const PartRef& p) { return p.measures.empty(); }), parts.end());
    if (parts.empty()) { err = tr("The MusicXML file has no parts"); return false; }

    std::vector<PartScan> scans;
    std::vector<TrackInfo> infos;
    for (size_t i = 0; i < parts.size(); ++i) {
        scans.push_back(scanPart(parts[i]));
        TrackInfo ti;
        ti.index = (int)i;
        ti.name = parts[i].name.empty() ? parts[i].id : parts[i].name;
        ti.isGuitarLike = scans[i].guitarHint;
        ti.noteCount = scans[i].noteCount;
        infos.push_back(ti);
    }
    if (tracks) *tracks = infos;
    int sel = (track >= 0 && track < (int)parts.size()) ? track : pickBestTrack(infos);
    if (sel < 0) { err = tr("No usable part"); return false; }
    const PartRef& part = parts[(size_t)sel];
    const PartScan& scan = scans[(size_t)sel];

    // ---- pick a staff: prefer a tab staff (string/fret data), avoiding duplicates from the notation staff
    int useStaff = 0;  // 0 = all
    if (scan.notesPerStaff.size() > 1 && !scan.tabNotesPerStaff.empty()) {
        int best = 0, bestN = -1;
        for (auto& kv : scan.tabNotesPerStaff)
            if (kv.second > bestN) { bestN = kv.second; best = kv.first; }
        useStaff = best;
    }
    // tuning
    {
        const StaffInfo* si = nullptr;
        for (auto& kv : scan.staves)
            if ((useStaff == 0 || kv.first == useStaff) && kv.second.hasTuning) { si = &kv.second; break; }
        if (si) {
            int lines = std::clamp(si->lines, 1, 8);
            // line 1 = lowest string; in this model string 0 = highest string
            for (int L = 1; L <= lines && L <= 8; ++L) {
                int strIdx = lines - L;  // 0 = highest
                if (strIdx >= 0 && strIdx < 6 && si->tuning[L - 1] > 0) out.tuning[strIdx] = si->tuning[L - 1];
            }
        }
    }
    int numLines = 6;
    for (auto& kv : scan.staves)
        if ((useStaff == 0 || kv.first == useStaff) && (kv.second.tabClef || kv.second.hasTuning)) numLines = kv.second.lines;

    // ---- parse measure by measure
    std::vector<size_t> order = playOrder(part.measures);
    int divisions = 1;
    int transpose = 0;
    double measureStart = 0;
    double firstOffset = 0;
    TimeSig curTs;
    bool firstMeasure = true;
    struct ChordAt { double beat; std::string name; };
    std::vector<ChordAt> chordMarks;
    size_t noteTotal = 0;

    for (size_t oi = 0; oi < order.size(); ++oi) {
        const MeasureRef& m = part.measures[order[oi]];
        double pos = 0, maxPos = 0;  // in divisions
        double lastNoteStart = 0;
        bool implicit = m.attrs->attrOr("implicit") == "yes";
        for (const auto& chp : m.content->children) {
            const XmlNode* ch = chp.get();
            if (ch->name == "attributes") {
                std::string dv = ch->childText("divisions");
                if (!dv.empty()) divisions = std::clamp(toInt(dv, 1), 1, 1 << 20);
                if (const XmlNode* t = ch->child("time")) {
                    int num = toInt(t->childText("beats", "4"), 4);
                    int den = toInt(t->childText("beat-type", "4"), 4);
                    if (num >= 1 && num <= 64 && (den == 1 || den == 2 || den == 4 || den == 8 || den == 16 || den == 32)) {
                        curTs.num = num;
                        curTs.den = den;
                        out.timeSigs.push_back({measureStart + pos / divisions, num, den});
                    }
                }
                if (const XmlNode* tp = ch->child("transpose"))
                    transpose = std::clamp(toInt(tp->childText("chromatic", "0")), -24, 24) +
                                12 * std::clamp(toInt(tp->childText("octave-change", "0")), -4, 4);
                continue;
            }
            if (ch->name == "backup") { pos = std::max(0.0, pos - toDouble(ch->childText("duration", "0"))); continue; }
            if (ch->name == "forward") {
                pos += std::max(0.0, toDouble(ch->childText("duration", "0")));
                maxPos = std::max(maxPos, pos);
                continue;
            }
            if (ch->name == "sound" || ch->name == "direction") {
                const XmlNode* snd = ch->name == "sound" ? ch : ch->child("sound");
                double beatHere = measureStart + pos / divisions;
                bool gotTempo = false;
                if (snd) {
                    double t = toDouble(snd->attrOr("tempo", ""), 0);
                    if (t > 10 && t < 1000) { out.tempo.push_back({beatHere, t}); gotTempo = true; }
                }
                if (ch->name == "direction") {
                    for (const XmlNode* dt : ch->childrenNamed("direction-type")) {
                        if (const XmlNode* mm = dt->child("metronome"); mm && !gotTempo) {
                            double pm = toDouble(mm->childText("per-minute", "0"), 0);
                            std::string unit = mm->childText("beat-unit", "quarter");
                            double f = unit == "half" ? 2 : unit == "eighth" ? 0.5 : unit == "whole" ? 4 : unit == "16th" ? 0.25 : 1;
                            if (mm->child("beat-unit-dot")) f *= 1.5;
                            if (pm * f > 10 && pm * f < 1000) out.tempo.push_back({beatHere, pm * f});
                        }
                        if (const XmlNode* rh = dt->child("rehearsal")) out.sections.push_back({beatHere, rh->trimmedText()});
                    }
                }
                continue;
            }
            if (ch->name == "harmony") {
                std::string nm = harmonyName(ch);
                if (!nm.empty()) chordMarks.push_back({measureStart + pos / divisions, nm});
                continue;
            }
            if (ch->name != "note") continue;
            const XmlNode* n = ch;
            if (n->child("grace") || n->child("cue")) continue;
            bool isChord = n->child("chord") != nullptr;
            double dur = std::max(0.0, toDouble(n->childText("duration", "0")));
            double start = isChord ? lastNoteStart : pos;
            if (!isChord) {
                lastNoteStart = pos;
                pos += dur;
                maxPos = std::max(maxPos, pos);
            }
            if (n->child("rest")) continue;
            int staff = toInt(n->childText("staff", "1"), 1);
            if (useStaff && staff != useStaff) continue;
            SongNote sn;
            const XmlNode* tech = n->path("notations/technical");
            int midi = -1;
            if (const XmlNode* p = n->child("pitch")) {
                int pc = stepToPc(p->childText("step"));
                int oct = std::clamp(toInt(p->childText("octave", "4"), 4), -1, 10);
                int alt = std::clamp(toInt(p->childText("alter", "0")), -3, 3);
                if (pc >= 0) midi = (oct + 1) * 12 + pc + alt + transpose;
            }
            if (tech && tech->child("string") && tech->child("fret")) {
                int st = toInt(tech->childText("string"), 0);
                int fr = toInt(tech->childText("fret"), -1);
                // 1 = highest string; notes beyond 6 strings (7-string guitar) are left to auto-fingering
                if (st >= 1 && st <= std::min(numLines, 6) && fr >= 0 && fr <= 30) {
                    sn.string = st - 1;
                    sn.fret = fr;
                }
            }
            if (sn.string < 0) {
                if (midi < 0 || midi > 127) continue;
                sn.midi = midi;
            }
            sn.durBeats = std::max(dur / divisions, 1.0 / 64);
            for (const XmlNode* t : n->childrenNamed("tie"))
                if (t->attrOr("type") == "stop") sn.flags |= NF_TieFromPrev;
            if (const XmlNode* nt = n->child("notations")) {
                for (const XmlNode* t : nt->childrenNamed("tied"))
                    if (t->attrOr("type") == "stop") sn.flags |= NF_TieFromPrev;
                for (const XmlNode* t : nt->childrenNamed("slide"))
                    if (t->attrOr("type") == "stop") sn.flags |= NF_Slide;
                for (const XmlNode* t : nt->childrenNamed("glissando"))
                    if (t->attrOr("type") == "stop") sn.flags |= NF_Slide;
                if (tech) {
                    for (const XmlNode* t : tech->childrenNamed("hammer-on"))
                        if (t->attrOr("type") == "stop") sn.flags |= NF_Hammer;
                    for (const XmlNode* t : tech->childrenNamed("pull-off"))
                        if (t->attrOr("type") == "stop") sn.flags |= NF_Pull;
                    if (tech->child("bend")) sn.flags |= NF_Bend;
                    if (tech->child("harmonic")) sn.flags |= NF_Harmonic;
                }
                if (nt->path("ornaments/vibrato") || nt->path("ornaments/wavy-line")) sn.flags |= NF_Vibrato;
                if (nt->path("articulations/accent")) sn.flags |= NF_Accent;
            }
            std::string head = n->childText("notehead");
            if (head == "x" || head == "cross") sn.flags |= NF_Dead;
            SongEvent ev;
            ev.beat = measureStart + start / divisions;
            ev.durBeats = sn.durBeats;
            ev.notes.push_back(sn);
            if (const XmlNode* ly = n->child("lyric")) {
                std::string num = ly->attrOr("number", "1");
                if (num == "1" || num.empty()) {
                    std::string txt = ly->childText("text");
                    if (!txt.empty()) ev.lyric = txt;
                }
            }
            out.events.push_back(std::move(ev));
            if (++noteTotal > kMaxNotes) { err = tr("Too many notes"); return false; }
        }
        double tsLen = curTs.num * 4.0 / curTs.den * divisions;
        double len = implicit ? maxPos : std::max(maxPos, tsLen);
        if (len <= 0) len = tsLen;
        if (firstMeasure) {
            firstMeasure = false;
            if (maxPos > 0 && maxPos < tsLen - 1e-9) {
                // pickup measure: pad with a rest in front so later bars line up
                firstOffset = (tsLen - maxPos) / divisions;
                len = maxPos;
            }
        }
        measureStart += len / divisions;
    }
    if (firstOffset > 0) {
        for (SongEvent& e : out.events) e.beat += firstOffset;
        for (TempoPoint& t : out.tempo) if (t.beat > 0) t.beat += firstOffset;
        for (TimeSig& t : out.timeSigs) if (t.beat > 0) t.beat += firstOffset;
        for (Section& s : out.sections) s.beat += firstOffset;
        for (ChordAt& c : chordMarks) c.beat += firstOffset;
    }
    out.lengthBeats = measureStart + firstOffset;
    out.finalize();
    // attach chord names to the nearest event
    for (const ChordAt& c : chordMarks) {
        for (SongEvent& e : out.events) {
            if (e.beat >= c.beat - 1e-6) {
                if (e.beat - c.beat < 4.0 && e.chordName.empty()) e.chordName = c.name;
                break;
            }
        }
    }
    if (out.events.empty()) { err = tr("The selected part has no notes"); return false; }
    if (out.title.empty()) out.title = infos[(size_t)sel].name;
    return true;
}

bool loadMusicXml(const std::string& path, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks) {
    std::string data;
    if (!readFileBytes(path, data, err)) return false;
    std::string xml;
    if (isZip(data)) {
        std::vector<ZipEntry> es;
        if (!zipList(data, es, err)) return false;
        std::string rootPath;
        for (const ZipEntry& e : es) {
            if (e.name == "META-INF/container.xml") {
                std::string c;
                if (zipExtract(data, e, c, err)) {
                    std::string e2;
                    if (auto cd = parseXml(c, e2)) {
                        if (const XmlNode* rfs = cd->child("rootfiles"))
                            if (const XmlNode* rf = rfs->child("rootfile")) rootPath = rf->attrOr("full-path");
                    }
                }
            }
        }
        const ZipEntry* pick = nullptr;
        for (const ZipEntry& e : es)
            if (!rootPath.empty() && e.name == rootPath) pick = &e;
        if (!pick)
            for (const ZipEntry& e : es) {
                std::string ext = fileExtLower(e.name);
                if ((ext == ".xml" || ext == ".musicxml") && !startsWith(e.name, "META-INF/")) { pick = &e; break; }
            }
        if (!pick) { err = tr("No score found inside the MXL archive"); return false; }
        if (!zipExtract(data, *pick, xml, err)) return false;
    } else {
        xml.swap(data);
    }
    if (!loadMusicXmlString(xml, out, err, track, tracks)) return false;
    out.source = path;
    return true;
}

}  // namespace gs
