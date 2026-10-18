// score.gpif (XML) import for Guitar Pro 6 (.gpx, BCFZ/BCFS container) and Guitar Pro 7/8 (.gp, ZIP).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#include "song/SongInternal.h"

namespace gs {

using namespace detail;

namespace {

// ---------------------------------------------------------------- BCFZ decompression
class BitIn {
public:
    BitIn(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool eof() const { return bad_; }
    int bit() {
        if (pos_ >= 8) {
            if (byte_ >= n_) { bad_ = true; return 0; }
            cur_ = d_[byte_++];
            pos_ = 0;
        }
        return (cur_ >> (7 - pos_++)) & 1;
    }
    uint32_t bits(int count) {
        uint32_t v = 0;
        for (int i = count - 1; i >= 0; --i) v |= (uint32_t)bit() << i;
        return v;
    }
    uint32_t bitsReversed(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) v |= (uint32_t)bit() << i;
        return v;
    }
    uint8_t byte() { return (uint8_t)bits(8); }

private:
    const uint8_t* d_;
    size_t n_;
    size_t byte_ = 0;
    int pos_ = 8;
    uint8_t cur_ = 0;
    bool bad_ = false;
};

uint32_t le32(const std::string& s, size_t off) {
    if (off + 4 > s.size()) return 0;
    return (uint32_t)(uint8_t)s[off] | ((uint32_t)(uint8_t)s[off + 1] << 8) | ((uint32_t)(uint8_t)s[off + 2] << 16) |
           ((uint32_t)(uint8_t)s[off + 3] << 24);
}

bool bcfzDecompress(const std::string& data, std::string& out, std::string& err) {
    // data: the bytes after "BCFZ"
    BitIn in(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    uint32_t expected = 0;
    for (int i = 0; i < 4; ++i) expected |= (uint32_t)in.byte() << (8 * i);
    if (in.eof() || expected > kMaxFileBytes) { err = tr("Corrupt GPX data"); return false; }
    out.clear();
    out.reserve(std::min<size_t>(expected, 16u << 20));
    while (out.size() < expected && !in.eof()) {
        if (in.bit()) {
            int word = (int)in.bits(4);
            uint32_t offset = in.bitsReversed(word);
            uint32_t size = in.bitsReversed(word);
            if (in.eof()) break;
            if (offset == 0 || offset > out.size()) { err = tr("Corrupt GPX compressed data"); return false; }
            size_t src = out.size() - offset;
            size_t toRead = std::min(offset, size);
            for (size_t k = 0; k < toRead; ++k) out += out[src + k];
        } else {
            uint32_t size = in.bitsReversed(2);
            for (uint32_t k = 0; k < size && !in.eof(); ++k) out += (char)in.byte();
        }
    }
    return true;
}

bool bcfsFind(const std::string& fs, const std::string& want, std::string& out) {
    // fs: the bytes after "BCFS". The first sector (0x1000) is empty
    const size_t kSector = 0x1000;
    size_t offset = kSector;
    int guard = 0;
    while (offset + 3 < fs.size() && guard++ < 200000) {
        uint32_t type = le32(fs, offset);
        if (type == 2) {
            std::string name;
            for (size_t k = 0; k < 127 && offset + 4 + k < fs.size(); ++k) {
                char c = fs[offset + 4 + k];
                if (!c) break;
                name += c;
            }
            uint32_t fileSize = le32(fs, offset + 0x8c);
            size_t ptr = offset + 0x94;
            std::string fileData;
            bool store = (name == want) && fileSize <= kMaxFileBytes;
            for (int sc = 0; sc < 100000; ++sc) {
                if (ptr + 4 * (size_t)sc + 4 > fs.size()) break;
                uint32_t sector = le32(fs, ptr + 4 * (size_t)sc);
                if (sector == 0) break;
                size_t so = (size_t)sector * kSector;
                if (so >= fs.size()) break;
                offset = so;
                if (store) fileData.append(fs, so, std::min(kSector, fs.size() - so));
            }
            if (store) {
                if (fileData.size() > fileSize) fileData.resize(fileSize);
                out.swap(fileData);
                return true;
            }
        }
        offset += kSector;
    }
    return false;
}

std::vector<int> intList(const std::string& s) {
    std::vector<int> v;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) {
        int x;
        if (parseIntStrict(tok, x)) v.push_back(x);
        if (v.size() > 100000) break;
    }
    return v;
}

const XmlNode* findProperty(const XmlNode* props, const char* name) {
    if (!props) return nullptr;
    for (const XmlNode* p : props->childrenNamed("Property"))
        if (p->attrOr("name") == name) return p;
    return nullptr;
}

double noteValueBeats(const std::string& v) {
    static const std::map<std::string, double> m = {{"DoubleWhole", 8}, {"Whole", 4},   {"Half", 2},     {"Quarter", 1},
                                                    {"Eighth", 0.5},    {"16th", 0.25}, {"32nd", 0.125}, {"64th", 0.0625},
                                                    {"128th", 0.03125}, {"256th", 0.015625}};
    auto it = m.find(v);
    return it == m.end() ? 1.0 : it->second;
}

struct GTrack {
    std::string name;
    const XmlNode* node = nullptr;
    const XmlNode* props = nullptr;  // properties of the first staff (GP7) or of the track (GP6)
    int tuningLowToHigh[8] = {0};
    int numStrings = 0;
    int capo = 0;
    int staves = 1;
    std::map<int, std::string> chordNames;
};

}  // namespace

bool detail::gpxExtractScore(const std::string& data, std::string& scoreXml, std::string& err) {
    if (data.size() < 4) { err = tr("GPX file too small"); return false; }
    std::string head = data.substr(0, 4);
    std::string fs;
    if (head == "BCFZ") {
        std::string un;
        if (!bcfzDecompress(data.substr(4), un, err)) return false;
        if (un.size() < 4 || un.compare(0, 4, "BCFS") != 0) { err = tr("Invalid GPX format after decompression"); return false; }
        fs = un.substr(4);
    } else if (head == "BCFS") {
        fs = data.substr(4);
    } else {
        err = tr("Not a Guitar Pro 6 (.gpx) file");
        return false;
    }
    if (!bcfsFind(fs, "score.gpif", scoreXml)) { err = tr("score.gpif not found in the GPX file"); return false; }
    return true;
}

bool detail::loadGpifXml(const std::string& xml, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracksOut) {
    out = Song();
    std::unique_ptr<XmlNode> doc = parseXml(xml, err);
    if (!doc) return false;
    if (doc->name != "GPIF") { err = tr("Not a GPIF score"); return false; }
    if (const XmlNode* sc = doc->child("Score")) {
        out.title = sc->childText("Title");
        out.artist = sc->childText("Artist");
        out.album = sc->childText("Album");
    }
    // ---- id index
    auto index = [&](const char* group, const char* item) {
        std::map<int, const XmlNode*> m;
        if (const XmlNode* g = doc->child(group))
            for (const XmlNode* n : g->childrenNamed(item)) {
                int id;
                if (parseIntStrict(n->attrOr("id"), id)) m[id] = n;
            }
        return m;
    };
    auto bars = index("Bars", "Bar");
    auto voices = index("Voices", "Voice");
    auto beats = index("Beats", "Beat");
    auto notes = index("Notes", "Note");
    auto rhythms = index("Rhythms", "Rhythm");

    // ---- tracks
    std::vector<GTrack> trs;
    if (const XmlNode* tg = doc->child("Tracks")) {
        for (const XmlNode* t : tg->childrenNamed("Track")) {
            GTrack g;
            g.node = t;
            g.name = t->childText("Name");
            g.props = t->child("Properties");
            if (const XmlNode* stv = t->child("Staves")) {
                auto ss = stv->childrenNamed("Staff");
                g.staves = std::max<int>(1, (int)ss.size());
                if (!ss.empty() && ss[0]->child("Properties")) g.props = ss[0]->child("Properties");
            }
            // percussion tracks: GP7 InstrumentSet/Type=drumKit, GP6 Instrument ref=drumkit, or MIDI channel 10
            bool perc = false;
            if (const XmlNode* is = t->child("InstrumentSet")) {
                std::string ty = toLower(is->childText("Type"));
                if (ty.find("drum") != std::string::npos || ty.find("percussion") != std::string::npos) perc = true;
            }
            if (const XmlNode* in = t->child("Instrument"))
                if (toLower(in->attrOr("ref")).find("drum") != std::string::npos) perc = true;
            for (const char* mc : {"GeneralMidi", "MidiConnection"})
                if (const XmlNode* gm = t->child(mc))
                    if (gm->childText("PrimaryChannel") == "9") perc = true;
            if (perc) {
                trs.push_back(g);  // keep the slot (bar indices follow track order); numStrings = 0 -> not selectable
                continue;
            }
            if (const XmlNode* tp = findProperty(g.props, "Tuning")) {
                std::vector<int> p = intList(tp->childText("Pitches"));
                g.numStrings = std::min<int>((int)p.size(), 8);
                for (int k = 0; k < g.numStrings; ++k) g.tuningLowToHigh[k] = std::clamp(p[(size_t)k], 0, 127);
            }
            if (const XmlNode* cp = findProperty(g.props, "CapoFret")) {
                int c;
                if (parseIntStrict(cp->childText("Fret"), c)) g.capo = std::clamp(c, 0, 24);
            }
            if (const XmlNode* dc = findProperty(g.props, "DiagramCollection"))
                if (const XmlNode* items = dc->child("Items"))
                    for (const XmlNode* it : items->childrenNamed("Item")) {
                        int id;
                        if (parseIntStrict(it->attrOr("id"), id)) g.chordNames[id] = it->attrOr("name");
                    }
            trs.push_back(g);
        }
    }
    if (trs.empty()) { err = tr("The GPIF score has no tracks"); return false; }
    std::vector<int> staffOffset(trs.size(), 0);
    for (size_t t = 1; t < trs.size(); ++t) staffOffset[t] = staffOffset[t - 1] + trs[t - 1].staves;
    const XmlNode* mbs = doc->child("MasterBars");
    std::vector<const XmlNode*> masterBars = mbs ? mbs->childrenNamed("MasterBar") : std::vector<const XmlNode*>();
    if (masterBars.empty()) { err = tr("The GPIF score has no bars"); return false; }
    if (masterBars.size() > 20000) { err = tr("Too many bars"); return false; }

    auto barIdFor = [&](const XmlNode* mb, size_t t) -> int {
        std::vector<int> ids = intList(mb->childText("Bars"));
        size_t k = (size_t)staffOffset[t];
        return k < ids.size() ? ids[k] : -1;
    };
    // count notes
    std::vector<TrackInfo> infos;
    std::vector<int> map;
    for (size_t t = 0; t < trs.size(); ++t) {
        if (trs[t].numStrings < 1) continue;  // no tuning = drums or a non-string instrument
        int count = 0;
        for (const XmlNode* mb : masterBars) {
            auto bit = bars.find(barIdFor(mb, t));
            if (bit == bars.end()) continue;
            for (int vid : intList(bit->second->childText("Voices"))) {
                auto vit = voices.find(vid);
                if (vit == voices.end()) continue;
                for (int bid : intList(vit->second->childText("Beats"))) {
                    auto beit = beats.find(bid);
                    if (beit != beats.end()) count += (int)intList(beit->second->childText("Notes")).size();
                }
            }
            if (count > (int)kMaxNotes) break;
        }
        TrackInfo ti;
        ti.index = (int)infos.size();
        ti.name = trs[t].name.empty() ? (tr("Track ") + std::to_string(t + 1)) : trs[t].name;
        ti.isGuitarLike = trs[t].numStrings >= 6;
        ti.noteCount = count;
        infos.push_back(ti);
        map.push_back((int)t);
    }
    if (tracksOut) *tracksOut = infos;
    if (infos.empty()) { err = tr("No usable guitar track"); return false; }
    int sel = (track >= 0 && track < (int)infos.size()) ? track : pickBestTrack(infos);
    if (sel < 0) { err = tr("No usable track"); return false; }
    const size_t ti = (size_t)map[(size_t)sel];
    const GTrack& trk = trs[ti];
    out.capo = trk.capo;
    const int ns = trk.numStrings;
    // tuningLowToHigh[0] = lowest string -> in this model string 0 = highest string
    for (int s = 0; s < 6; ++s) {
        int k = ns - 1 - s;
        if (k >= 0) out.tuning[s] = trk.tuningLowToHigh[k];
        else out.tuning[s] = std::max(10, out.tuning[s - 1] - 5);
    }

    // ---- tempo automation
    struct TempoAuto { int bar; double pos; double bpm; };
    std::vector<TempoAuto> tempos;
    if (const XmlNode* mt = doc->child("MasterTrack"))
        if (const XmlNode* au = mt->child("Automations"))
            for (const XmlNode* a : au->childrenNamed("Automation")) {
                if (a->childText("Type") != "Tempo") continue;
                std::vector<std::string> parts;
                std::istringstream is(a->childText("Value"));
                std::string w;
                while (is >> w) parts.push_back(w);
                double bpm;
                if (parts.empty() || !parseDoubleStrict(parts[0], bpm)) continue;
                int ref = 2;
                if (parts.size() > 1) parseIntStrict(parts[1], ref);
                static const double kRef[6] = {1, 0.5, 1, 1.5, 2, 3};
                bpm *= kRef[std::clamp(ref, 0, 5)];
                int bar = 0;
                parseIntStrict(a->childText("Bar", "0"), bar);
                double pos = 0;
                parseDoubleStrict(a->childText("Position", "0"), pos);
                if (bpm > 10 && bpm < 1000) tempos.push_back({bar, std::clamp(pos, 0.0, 1.0), bpm});
            }

    // ---- bar info + repeat expansion
    const size_t M = masterBars.size();
    std::vector<int> num(M, 4), den(M, 4), repCount(M, 0), endMask(M, 0);
    std::vector<bool> repStart(M, false);
    std::vector<std::string> sectionText(M);
    for (size_t i = 0; i < M; ++i) {
        const XmlNode* mb = masterBars[i];
        std::string t = mb->childText("Time");
        int a = 0, b = 0;
        if (std::sscanf(t.c_str(), "%d/%d", &a, &b) == 2 && a >= 1 && a <= 64 && (b == 1 || b == 2 || b == 4 || b == 8 || b == 16 || b == 32)) {
            num[i] = a;
            den[i] = b;
        } else if (i > 0) {
            num[i] = num[i - 1];
            den[i] = den[i - 1];
        }
        if (const XmlNode* r = mb->child("Repeat")) {
            repStart[i] = r->attrOr("start") == "true";
            if (r->attrOr("end") == "true") {
                int c = 2;
                parseIntStrict(r->attrOr("count", "2"), c);
                repCount[i] = std::clamp(c, 2, 8);
            }
        }
        for (int e : intList(mb->childText("AlternateEndings")))
            if (e >= 1 && e <= 30) endMask[i] |= 1 << (e - 1);
        if (const XmlNode* sec = mb->child("Section")) {
            std::string txt = sec->childText("Text");
            if (txt.empty()) txt = sec->childText("Letter");
            sectionText[i] = txt;
        }
    }
    std::vector<size_t> order;
    {
        std::vector<int> jumps(M, 0);
        size_t i = 0, rs = 0;
        int pass = 1;
        bool jumped = false;
        while (i < M && order.size() < 20000) {
            if (repStart[i] && !jumped) { rs = i; pass = 1; }
            jumped = false;
            if (endMask[i] && !(endMask[i] & (1 << std::min(pass - 1, 30)))) { ++i; continue; }
            order.push_back(i);
            if (repCount[i] > 0) {
                if (jumps[i] < repCount[i] - 1) { jumps[i]++; pass++; i = rs; jumped = true; continue; }
                jumps[i] = 0;
                pass = 1;
                rs = i + 1;
            }
            ++i;
        }
    }

    // ---- generate events
    double barStart = 0;
    size_t total = 0;
    int pendingLegato[6] = {-1, -1, -1, -1, -1, -1};
    for (size_t oi = 0; oi < order.size(); ++oi) {
        size_t mi = order[oi];
        double barLen = num[mi] * 4.0 / den[mi];
        out.timeSigs.push_back({barStart, num[mi], den[mi]});
        if (!sectionText[mi].empty()) out.sections.push_back({barStart, sectionText[mi]});
        for (const TempoAuto& ta : tempos)
            if (ta.bar == (int)mi) out.tempo.push_back({barStart + ta.pos * barLen, ta.bpm});
        auto bit = bars.find(barIdFor(masterBars[mi], ti));
        if (bit != bars.end()) {
            for (int vid : intList(bit->second->childText("Voices"))) {
                auto vit = voices.find(vid);
                if (vit == voices.end()) continue;
                double pos = 0;
                for (int bid : intList(vit->second->childText("Beats"))) {
                    auto beit = beats.find(bid);
                    if (beit == beats.end()) continue;
                    const XmlNode* be = beit->second;
                    double dur = 1.0;
                    if (const XmlNode* rr = be->child("Rhythm")) {
                        int rid;
                        if (parseIntStrict(rr->attrOr("ref"), rid)) {
                            auto rit = rhythms.find(rid);
                            if (rit != rhythms.end()) {
                                const XmlNode* rh = rit->second;
                                dur = noteValueBeats(rh->childText("NoteValue"));
                                if (const XmlNode* dot = rh->child("AugmentationDot")) {
                                    int c = 1;
                                    parseIntStrict(dot->attrOr("count", "1"), c);
                                    dur *= c >= 2 ? 1.75 : 1.5;
                                }
                                for (const char* tn : {"PrimaryTuplet", "SecondaryTuplet"})
                                    if (const XmlNode* tp = rh->child(tn)) {
                                        int n = 1, d = 1;
                                        parseIntStrict(tp->attrOr("num", "1"), n);
                                        parseIntStrict(tp->attrOr("den", "1"), d);
                                        if (n > 0 && d > 0 && n < 64 && d < 64) dur = dur * d / n;
                                    }
                            }
                        }
                    }
                    bool grace = be->child("GraceNotes") != nullptr;
                    std::vector<int> nids = intList(be->childText("Notes"));
                    if (!grace && !nids.empty()) {
                        SongEvent ev;
                        ev.beat = barStart + pos;
                        ev.durBeats = dur;
                        if (const XmlNode* ch = be->child("Chord")) {
                            int cid;
                            if (parseIntStrict(ch->trimmedText(), cid)) {
                                auto cn = trk.chordNames.find(cid);
                                if (cn != trk.chordNames.end()) ev.chordName = cn->second;
                            }
                        }
                        if (const XmlNode* br = findProperty(be->child("Properties"), "Brush")) {
                            std::string d = br->childText("Direction");
                            ev.strum = d == "Up" ? Strum::Up : d == "Down" ? Strum::Down : Strum::None;
                        }
                        for (int nid : nids) {
                            auto nit = notes.find(nid);
                            if (nit == notes.end()) continue;
                            const XmlNode* nn = nit->second;
                            const XmlNode* props = nn->child("Properties");
                            SongNote sn;
                            sn.durBeats = dur;
                            int fret = -1, str = -1;
                            if (const XmlNode* p = findProperty(props, "Fret")) parseIntStrict(p->childText("Fret"), fret);
                            if (const XmlNode* p = findProperty(props, "String")) parseIntStrict(p->childText("String"), str);
                            if (str >= 0 && str < ns && fret >= 0 && fret <= 30) {
                                int ours = ns - 1 - str;
                                if (ours <= 5) {
                                    sn.string = ours;
                                    sn.fret = fret;
                                } else {
                                    sn.midi = trk.tuningLowToHigh[str] + trk.capo + fret;
                                }
                            } else if (const XmlNode* p = findProperty(props, "Midi")) {
                                int m;
                                if (!parseIntStrict(p->childText("Number"), m) || m < 0 || m > 127) continue;
                                sn.midi = m;
                            } else {
                                continue;
                            }
                            if (const XmlNode* tie = nn->child("Tie"))
                                if (tie->attrOr("destination") == "true") sn.flags |= NF_TieFromPrev;
                            if (findProperty(props, "PalmMuted")) sn.flags |= NF_PalmMute;
                            if (findProperty(props, "Muted")) sn.flags |= NF_Dead;
                            if (findProperty(props, "Slide")) sn.flags |= NF_Slide;
                            if (findProperty(props, "Bended")) sn.flags |= NF_Bend;
                            if (findProperty(props, "HarmonicType") || findProperty(props, "HarmonicFret")) sn.flags |= NF_Harmonic;
                            if (nn->child("Vibrato")) sn.flags |= NF_Vibrato;
                            if (nn->child("LetRing")) sn.flags |= NF_LetRing;
                            if (nn->child("Accent")) sn.flags |= NF_Accent;
                            if (sn.string >= 0) {
                                int& pl = pendingLegato[sn.string];
                                if (findProperty(props, "HopoDestination") && pl >= 0 && !(sn.flags & NF_TieFromPrev))
                                    sn.flags |= sn.fret >= pl ? NF_Hammer : NF_Pull;
                                pl = findProperty(props, "HopoOrigin") ? sn.fret : -1;
                            }
                            ev.notes.push_back(sn);
                            if (++total > kMaxNotes) { err = tr("Too many notes"); return false; }
                        }
                        if (!ev.notes.empty()) out.events.push_back(std::move(ev));
                    }
                    if (!grace) pos += dur;
                }
            }
        }
        barStart += barLen;
    }
    out.lengthBeats = barStart;
    out.finalize();
    if (out.events.empty()) { err = tr("The selected track has no notes"); return false; }
    if (out.title.empty()) out.title = infos[(size_t)sel].name;
    return true;
}

}  // namespace gs
