// Guitar Pro 3 / 4 / 5 (.gp3 .gp4 .gp5) binary import.
// Layout follows the public format analyses (PyGuitarPro / TuxGuitar / alphaTab). Every read is bounds-checked.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "song/SongInternal.h"

namespace gs {

using namespace detail;

namespace {

constexpr uint32_t kGpLegatoOrigin = 1u << 30;

bool gpDebug() {
    static const bool on = std::getenv("GS_GP_DEBUG") != nullptr;
    return on;
}
#define GP_TRACE(...) \
    do { if (gpDebug()) std::fprintf(stderr, __VA_ARGS__); } while (0)  // debug trace (GS_GP_DEBUG=1)

struct GpMasterBar {
    int num = 4, den = 4;
    bool repeatStart = false;
    int repeatCount = 0;   // >0: this bar ends with a repeat sign; played repeatCount times in total
    int altEndings = 0;    // bit mask: bit i = ending for pass i+1
    std::string marker;
};

struct GpTrackInfo {
    std::string name;
    int numStrings = 6;
    int tuning[7] = {64, 59, 55, 50, 45, 40, 35};
    int capo = 0;
    bool percussion = false;
    int program = -1;
};

struct GpNote {
    int string = 0;  // 1 = highest string
    int fret = 0;
    uint32_t flags = 0;
};

struct GpBeat {
    double pos = 0;  // position within the bar (beats)
    double dur = 1;
    std::vector<GpNote> notes;
    std::string chord;
    Strum strum = Strum::None;
};

class GpReader {
public:
    GpReader(const std::string& data) : r_(data.data(), data.size()) {}

    bool read(std::string& err);

    int version = 0;
    std::string title, artist, album;
    int tempo = 120;
    std::vector<GpMasterBar> bars;
    std::vector<GpTrackInfo> tracks;
    std::vector<std::vector<std::vector<GpBeat>>> beats;  // [track][bar] → beats
    std::vector<std::pair<std::pair<int, double>, double>> tempoChanges;  // ((bar, pos), bpm)
    size_t noteTotal = 0;

private:
    ByteReader r_;
    int programs_[64] = {0};
    std::vector<std::vector<int>> lastFret_;  // [track][string], used for ties

    std::string byteSizeString(int size) {
        int len = r_.u8();
        int count = size > 0 ? size : len;
        std::string s = r_.bytes((size_t)std::max(0, count));
        return toUtf8Lossy(s.substr(0, (size_t)std::min(len, (int)s.size())));
    }
    std::string intByteSizeString() {
        int64_t d = (int64_t)r_.s32le() - 1;
        if (d < -1 || d > (1 << 20)) { r_.fail(); return {}; }
        return byteSizeString((int)d);
    }
    std::string intSizeString() {
        int32_t n = r_.s32le();
        if (n < 0 || n > (1 << 20)) { r_.fail(); return {}; }
        return toUtf8Lossy(r_.bytes((size_t)n));
    }
    void skipBend() {
        r_.skip(5);  // type (1) + value (4)
        int32_t pts = r_.s32le();
        if (pts < 0 || pts > 1000) { r_.fail(); return; }
        r_.skip((size_t)pts * 9);
    }
    std::string readChord(int numStrings);
    void readBeatEffects(GpBeat& b, uint32_t& noteFlags);
    double readMixTable();  // returns the tempo (bpm), -1 if unchanged
    void readNote(int track, GpNote& n);
    void readNoteEffects(GpNote& n);
    bool readBeat(int track, int bar, double& pos, std::vector<GpBeat>& out);
};

std::string GpReader::readChord(int numStrings) {
    std::string name;
    if (version >= 500) {
        r_.skip(17);
        name = byteSizeString(21);
        r_.skip(4);
        r_.skip(4);       // first fret
        r_.skip(7 * 4);   // frets
        r_.skip(1 + 5);   // barre count + barre frets
        r_.skip(26);
        return name;
    }
    if (r_.u8() != 0) {
        if (version >= 400) {
            r_.skip(16);
            name = byteSizeString(21);
            r_.skip(4);
            r_.skip(4);
            r_.skip(7 * 4);
            r_.skip(1 + 5);
            r_.skip(26);
        } else {
            r_.skip(25);
            name = byteSizeString(34);
            r_.skip(4);
            r_.skip(6 * 4);
            r_.skip(36);
        }
    } else {
        int strings = version >= 406 ? 7 : 6;
        name = intByteSizeString();
        int32_t firstFret = r_.s32le();
        if (firstFret > 0) r_.skip((size_t)strings * 4);
    }
    (void)numStrings;
    return name;
}

void GpReader::readBeatEffects(GpBeat& b, uint32_t& noteFlags) {
    if (version < 400) {
        uint8_t f1 = r_.u8();
        if (f1 & 0x01) noteFlags |= NF_Vibrato;
        if (f1 & 0x04 || f1 & 0x08) noteFlags |= NF_Harmonic;
        if (f1 & 0x20) {
            r_.u8();
            r_.skip(4);
        }
        if (f1 & 0x40) {
            int8_t down = r_.s8(), up = r_.s8();
            if (up > 0) b.strum = Strum::Up;
            else if (down > 0) b.strum = Strum::Down;
        }
        return;
    }
    uint8_t f1 = r_.u8(), f2 = r_.u8();
    if (f1 & 0x01) noteFlags |= NF_Vibrato;
    if (f1 & 0x20) r_.u8();
    if (f2 & 0x04) skipBend();
    if (f1 & 0x40) {
        int8_t a = r_.s8(), c = r_.s8();
        if (version >= 500) {  // GP5 uses the opposite direction to GP3/4
            if (c > 0) b.strum = Strum::Down;
            else if (a > 0) b.strum = Strum::Up;
        } else {
            if (c > 0) b.strum = Strum::Up;
            else if (a > 0) b.strum = Strum::Down;
        }
    }
    if (f2 & 0x02) r_.u8();
}

double GpReader::readMixTable() {
    r_.s8();  // instrument
    if (version >= 500) r_.skip(16);
    int8_t vol = r_.s8(), bal = r_.s8(), cho = r_.s8(), rev = r_.s8(), pha = r_.s8(), tre = r_.s8();
    if (version >= 500) intByteSizeString();
    int32_t t = r_.s32le();
    if (vol >= 0) r_.u8();
    if (bal >= 0) r_.u8();
    if (cho >= 0) r_.u8();
    if (rev >= 0) r_.u8();
    if (pha >= 0) r_.u8();
    if (tre >= 0) r_.u8();
    if (t >= 0) {
        r_.u8();
        if (version >= 510) r_.u8();
    }
    if (version >= 400) r_.u8();
    if (version >= 500) r_.u8();
    if (version >= 510) {
        intByteSizeString();
        intByteSizeString();
    }
    return (t > 10 && t < 1000) ? (double)t : -1.0;
}

void GpReader::readNoteEffects(GpNote& n) {
    if (version < 400) {
        uint8_t f = r_.u8();
        if (f & 0x02) n.flags |= kGpLegatoOrigin;
        if (f & 0x08) n.flags |= NF_LetRing;
        if (f & 0x01) { skipBend(); n.flags |= NF_Bend; }
        if (f & 0x10) r_.skip(4);  // grace
        if (f & 0x04) n.flags |= NF_Slide;
        return;
    }
    uint8_t f1 = r_.u8(), f2 = r_.u8();
    if (f1 & 0x02) n.flags |= kGpLegatoOrigin;  // hammer-on/pull-off (GP marks the origin note without telling which; decided later from the fret direction)
    if (f1 & 0x08) n.flags |= NF_LetRing;
    if (f2 & 0x02) n.flags |= NF_PalmMute;
    if (f2 & 0x40) n.flags |= NF_Vibrato;
    if (f1 & 0x01) { skipBend(); n.flags |= NF_Bend; }
    if (f1 & 0x10) r_.skip(version >= 500 ? 5 : 4);  // grace
    if (f2 & 0x04) r_.u8();                          // tremolo picking
    if (f2 & 0x08) { r_.u8(); n.flags |= NF_Slide; }
    if (f2 & 0x10) {
        n.flags |= NF_Harmonic;
        int8_t ht = r_.s8();
        if (version >= 500) {
            if (ht == 2) r_.skip(3);
            else if (ht == 3) r_.skip(1);
        }
    }
    if (f2 & 0x20) r_.skip(2);  // trill
}

void GpReader::readNote(int track, GpNote& n) {
    uint8_t f = r_.u8();
    int type = 1;
    if (f & 0x04) n.flags |= NF_Ghost;
    if (f & 0x40) n.flags |= NF_Accent;
    if (f & 0x20) type = r_.u8();
    if (version < 500 && (f & 0x01)) r_.skip(2);  // independent duration
    if (f & 0x10) r_.s8();                          // velocity
    int fret = 0;
    if (f & 0x20) fret = r_.s8();
    if (f & 0x80) r_.skip(2);  // fingering
    if (version >= 500) {
        if (f & 0x01) r_.skip(8);  // durationPercent (double)
        r_.u8();                   // flags2
    }
    if (f & 0x08) readNoteEffects(n);
    std::vector<int>& lf = lastFret_[(size_t)track];
    int si = std::clamp(n.string - 1, 0, 6);
    if (type == 2) {
        n.flags |= NF_TieFromPrev;
        fret = lf[(size_t)si] >= 0 ? lf[(size_t)si] : std::max(0, fret);
    } else if (type == 3) {
        n.flags |= NF_Dead;
    }
    n.fret = std::clamp(fret, 0, 30);
    lf[(size_t)si] = n.fret;
}

bool GpReader::readBeat(int track, int bar, double& pos, std::vector<GpBeat>& out) {
    uint8_t flags = r_.u8();
    int status = 1;  // 0 = empty, 1 = normal, 2 = rest
    if (flags & 0x40) status = r_.u8();
    int8_t dv = r_.s8();
    if (dv < -2 || dv > 6) { r_.fail(); return false; }
    double dur = 4.0 / (double)(1 << (dv + 2));
    if (flags & 0x01) dur *= 1.5;
    if (flags & 0x20) {
        int32_t tup = r_.s32le();
        int times = 0;
        switch (tup) {
            case 3: times = 2; break;
            case 5: case 6: case 7: times = 4; break;
            case 9: case 10: case 11: case 12: case 13: times = 8; break;
            default: times = 0; break;
        }
        if (times) dur = dur * times / tup;
    }
    GpBeat b;
    b.pos = pos;
    b.dur = dur;
    const GpTrackInfo& trk = tracks[(size_t)track];
    if (flags & 0x02) b.chord = trim(readChord(trk.numStrings));
    if (flags & 0x04) intByteSizeString();  // text
    uint32_t beatNoteFlags = 0;
    if (flags & 0x08) readBeatEffects(b, beatNoteFlags);
    if (flags & 0x10) {
        double t = readMixTable();
        if (t > 0) tempoChanges.push_back({{bar, pos}, t});
    }
    uint8_t sf = r_.u8();
    for (int s = 1; s <= 7; ++s) {
        if (!(sf & (1 << (7 - s)))) continue;
        GpNote n;
        n.string = s;
        readNote(track, n);
        if (!r_.ok()) return false;
        n.flags |= beatNoteFlags;
        if (s <= trk.numStrings) b.notes.push_back(n);
    }
    if (version >= 500) {
        uint16_t f2 = r_.u16le();
        if (f2 & 0x0800) r_.u8();
    }
    if (status != 0) pos += dur;
    if (status == 2) b.notes.clear();
    if (!b.notes.empty() && status != 0) {
        noteTotal += b.notes.size();
        out.push_back(std::move(b));
    }
    return r_.ok();
}

bool GpReader::read(std::string& err) {
    std::string ver = byteSizeString(30);
    const std::string kPrefix = "FICHIER GUITAR PRO ";
    if (!startsWith(ver, kPrefix) || ver.size() < kPrefix.size() + 5) { err = tr("Not a Guitar Pro 3/4/5 file"); return false; }
    {
        std::string v = ver.substr(kPrefix.size() + 1);  // skip 'v' / 'L'
        size_t dot = v.find('.');
        int major = 0, minor = 0;
        if (dot == std::string::npos || !parseIntStrict(v.substr(0, dot), major) || !parseIntStrict(v.substr(dot + 1, 2), minor)) {
            err = tr("Unrecognized Guitar Pro version: ") + ver;
            return false;
        }
        version = major * 100 + minor;
        if (version < 300 || version > 599) { err = tr("Unsupported Guitar Pro version: ") + ver; return false; }
    }
    // song info
    title = intByteSizeString();
    intByteSizeString();  // subtitle
    artist = intByteSizeString();
    album = intByteSizeString();
    intByteSizeString();  // words
    if (version >= 500) intByteSizeString();  // music
    intByteSizeString();  // copyright
    intByteSizeString();  // tab
    intByteSizeString();  // instructions
    int32_t notices = r_.s32le();
    if (notices < 0 || notices > 1000) { err = tr("Corrupt Guitar Pro header"); return false; }
    for (int i = 0; i < notices && r_.ok(); ++i) intByteSizeString();
    if (version < 500) r_.u8();  // triplet feel
    if (version >= 400) {
        r_.s32le();  // lyrics track
        for (int i = 0; i < 5 && r_.ok(); ++i) {
            r_.s32le();
            intSizeString();
        }
    }
    if (version >= 510) r_.skip(19);  // RSE master
    if (version >= 500) {
        r_.skip(30);  // page setup
        for (int i = 0; i < 10 && r_.ok(); ++i) intByteSizeString();
        intByteSizeString();  // tempo name
    }
    tempo = r_.s32le();
    if (tempo < 10 || tempo > 1000) tempo = 120;
    if (version >= 510) r_.u8();  // hide tempo
    r_.s32le();                   // key
    if (version >= 400) r_.s8();  // octave
    for (int i = 0; i < 64 && r_.ok(); ++i) {
        programs_[i] = r_.s32le();
        r_.skip(8);
    }
    if (version >= 500) r_.skip(19 * 2 + 4);  // directions + master reverb
    int32_t barCount = r_.s32le(), trackCount = r_.s32le();
    GP_TRACE("gp: version %d tempo %d bars %d tracks %d pos %zu\n", version, tempo, barCount, trackCount, r_.pos());
    if (!r_.ok() || barCount < 1 || barCount > 20000 || trackCount < 1 || trackCount > 128) {
        err = tr("Corrupt Guitar Pro file (bad bar/track count)");
        return false;
    }
    // bar headers
    for (int i = 0; i < barCount && r_.ok(); ++i) {
        GpMasterBar mb;
        if (!bars.empty()) { mb.num = bars.back().num; mb.den = bars.back().den; }
        if (version >= 500 && i > 0) r_.skip(1);
        uint8_t f = r_.u8();
        if (f & 0x01) mb.num = r_.s8();
        if (f & 0x02) mb.den = r_.s8();
        mb.repeatStart = (f & 0x04) != 0;
        if (f & 0x08) mb.repeatCount = r_.s8() + (version >= 500 ? 0 : 1);
        if (version < 500) {
            if (f & 0x10) {
                int existing = 0;
                for (size_t k = bars.size(); k-- > 0;) {
                    if (bars[k].repeatCount > 0 && k + 1 != bars.size()) break;
                    if (bars[k].repeatStart) break;
                    existing |= bars[k].altEndings;
                }
                int v = r_.u8();
                int alt = 0;
                for (int b = 0; b < 8; ++b)
                    if (v > b && !(existing & (1 << b))) alt |= 1 << b;
                mb.altEndings = alt;
            }
            if (f & 0x20) { mb.marker = intByteSizeString(); r_.skip(4); }
            if (f & 0x40) r_.skip(2);
        } else {
            if (f & 0x20) { mb.marker = intByteSizeString(); r_.skip(4); }
            if (f & 0x40) r_.skip(2);
            if (f & 0x10) mb.altEndings = r_.u8();
            if (f & 0x03) r_.skip(4);  // beams
            if (!(f & 0x10)) r_.skip(1);
            r_.u8();  // triplet feel
        }
        if (mb.num < 1 || mb.num > 64) mb.num = 4;
        if (mb.den != 1 && mb.den != 2 && mb.den != 4 && mb.den != 8 && mb.den != 16 && mb.den != 32) mb.den = 4;
        if (mb.repeatCount > 0) mb.repeatCount = std::clamp(mb.repeatCount, 2, 8);
        GP_TRACE("gp: bar %d flags %02x %d/%d rep %d alt %d marker '%s' pos %zu\n", i, f, mb.num, mb.den, mb.repeatCount,
                 mb.altEndings, mb.marker.c_str(), r_.pos());
        bars.push_back(mb);
    }
    // tracks
    for (int t = 0; t < trackCount && r_.ok(); ++t) {
        GpTrackInfo trk;
        if (version >= 500 && (t == 0 || version == 500)) r_.skip(1);
        uint8_t f = r_.u8();
        trk.percussion = (f & 0x01) != 0;
        trk.name = trim(byteSizeString(40));
        int32_t ns = r_.s32le();
        trk.numStrings = std::clamp((int)ns, 1, 7);
        for (int s = 0; s < 7; ++s) trk.tuning[s] = std::clamp((int)r_.s32le(), 0, 127);
        r_.s32le();  // port
        int64_t ch = (int64_t)r_.s32le() - 1;
        r_.s32le();  // effect channel
        if (ch >= 0 && ch < 64) {
            trk.program = programs_[(size_t)ch];
            if (ch % 16 == 9) trk.percussion = true;
        }
        r_.s32le();  // fret count
        trk.capo = std::clamp((int)r_.s32le(), 0, 24);
        r_.skip(4);  // color
        if (version >= 500) {
            r_.skip(2 + 1 + 1 + 1 + 12 + 12 + 12);
            if (version == 500) r_.skip(3);
            else r_.skip(4);
            if (version >= 510) {
                r_.skip(4);
                intByteSizeString();
                intByteSizeString();
            }
        }
        GP_TRACE("gp: track %d '%s' strings %d capo %d perc %d pos %zu\n", t, trk.name.c_str(), trk.numStrings, trk.capo,
                 (int)trk.percussion, r_.pos());
        tracks.push_back(trk);
    }
    if (!r_.ok()) { err = tr("Corrupt Guitar Pro file (track info)"); return false; }
    if (version >= 500) r_.skip(version == 500 ? 2 : 1);
    // bar contents
    beats.assign(tracks.size(), std::vector<std::vector<GpBeat>>(bars.size()));
    lastFret_.assign(tracks.size(), std::vector<int>(7, -1));
    const int voices = version >= 500 ? 2 : 1;
    for (int b = 0; b < barCount && r_.ok(); ++b) {
        for (int t = 0; t < trackCount && r_.ok(); ++t) {
            for (int v = 0; v < voices && r_.ok(); ++v) {
                int32_t nb = r_.s32le();
                GP_TRACE("gp: bar %d track %d voice %d beats %d pos %zu\n", b, t, v, nb, r_.pos());
                if (nb < 0 || nb > 512) { r_.fail(); break; }
                double pos = 0;
                for (int k = 0; k < nb && r_.ok(); ++k) {
                    if (!readBeat(t, b, pos, beats[(size_t)t][(size_t)b])) break;
                    if (noteTotal > kMaxNotes) { err = tr("Too many notes"); return false; }
                }
            }
            if (version >= 500 && r_.remaining() > 0) r_.u8();  // line break (some files lack this byte at the end)
        }
    }
    if (!r_.ok()) {
        // damaged tail: accept if enough content was read
        size_t got = 0;
        for (auto& tb : beats)
            for (auto& bb : tb) got += bb.size();
        if (got == 0) { err = tr("Corrupt Guitar Pro file (bar contents)"); return false; }
    }
    return true;
}

}  // namespace

bool detail::loadGp345Bytes(const std::string& data, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracksOut) {
    out = Song();
    GpReader g(data);
    if (!g.read(err)) return false;
    // selectable tracks (percussion excluded)
    std::vector<int> map;
    std::vector<TrackInfo> infos;
    for (size_t t = 0; t < g.tracks.size(); ++t) {
        const GpTrackInfo& trk = g.tracks[t];
        if (trk.percussion) continue;
        TrackInfo ti;
        ti.index = (int)infos.size();
        ti.name = trk.name.empty() ? (tr("Track ") + std::to_string(t + 1)) : trk.name;
        ti.isGuitarLike = trk.numStrings >= 6;
        int n = 0;
        for (auto& bb : g.beats[t])
            for (auto& be : bb) n += (int)be.notes.size();
        ti.noteCount = n;
        infos.push_back(ti);
        map.push_back((int)t);
    }
    if (tracksOut) *tracksOut = infos;
    if (infos.empty()) { err = tr("No usable guitar track"); return false; }
    int sel = (track >= 0 && track < (int)infos.size()) ? track : pickBestTrack(infos);
    if (sel < 0) { err = tr("No usable track"); return false; }
    const int ti = map[(size_t)sel];
    const GpTrackInfo& trk = g.tracks[(size_t)ti];

    out.title = trim(g.title);
    out.artist = trim(g.artist);
    out.album = trim(g.album);
    out.capo = trk.capo;
    // tuning: GP string 1 = highest. 4/5-string bass: fill the remaining strings with lower notes
    for (int s = 0; s < 6; ++s) {
        if (s < trk.numStrings) out.tuning[s] = trk.tuning[s];
        else out.tuning[s] = std::max(10, out.tuning[s - 1] - 5);
    }

    // expand repeats
    const size_t M = g.bars.size();
    std::vector<size_t> order;
    {
        std::vector<int> jumps(M, 0);
        size_t i = 0, repeatStart = 0;
        int pass = 1;
        bool justJumped = false;
        while (i < M && order.size() < 20000) {
            const GpMasterBar& mb = g.bars[i];
            if (mb.repeatStart && !justJumped) { repeatStart = i; pass = 1; }
            justJumped = false;
            if (mb.altEndings && !(mb.altEndings & (1 << std::min(pass - 1, 30)))) { ++i; continue; }
            order.push_back(i);
            if (mb.repeatCount > 0) {
                if (jumps[i] < mb.repeatCount - 1) {
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
    }

    out.tempo.push_back({0.0, (double)g.tempo});
    std::map<std::pair<int, double>, double> tempoAt;
    for (auto& tc : g.tempoChanges) tempoAt[tc.first] = tc.second;
    double barStart = 0;
    int lastNum = -1, lastDen = -1;
    int pendingLegato[6] = {-1, -1, -1, -1, -1, -1};  // per string: fret of the legato origin note (-1 = none)
    for (size_t oi = 0; oi < order.size(); ++oi) {
        size_t bi = order[oi];
        const GpMasterBar& mb = g.bars[bi];
        if (mb.num != lastNum || mb.den != lastDen) {
            out.timeSigs.push_back({barStart, mb.num, mb.den});
            lastNum = mb.num;
            lastDen = mb.den;
        }
        if (!mb.marker.empty()) out.sections.push_back({barStart, mb.marker});
        for (auto it = tempoAt.lower_bound({(int)bi, -1e9}); it != tempoAt.end() && it->first.first == (int)bi; ++it)
            out.tempo.push_back({barStart + it->first.second, it->second});
        for (const GpBeat& be : g.beats[(size_t)ti][bi]) {
            SongEvent ev;
            ev.beat = barStart + be.pos;
            ev.durBeats = be.dur;
            ev.chordName = be.chord;
            ev.strum = be.strum;
            for (const GpNote& gn : be.notes) {
                SongNote sn;
                sn.durBeats = be.dur;
                sn.flags = gn.flags;
                if (gn.string >= 1 && gn.string <= 6) {
                    sn.string = gn.string - 1;
                    sn.fret = gn.fret;
                    int& pl = pendingLegato[sn.string];
                    if (pl >= 0 && !(sn.flags & NF_TieFromPrev)) sn.flags |= (sn.fret >= pl) ? NF_Hammer : NF_Pull;
                    pl = (sn.flags & kGpLegatoOrigin) ? sn.fret : -1;
                    sn.flags &= ~kGpLegatoOrigin;
                } else {
                    sn.flags &= ~kGpLegatoOrigin;
                    sn.midi = trk.tuning[std::clamp(gn.string - 1, 0, 6)] + trk.capo + gn.fret;  // 7th string: left to auto-fingering
                }
                ev.notes.push_back(sn);
            }
            out.events.push_back(std::move(ev));
        }
        barStart += mb.num * 4.0 / mb.den;
    }
    out.lengthBeats = barStart;
    out.finalize();
    if (out.events.empty()) { err = tr("The selected track has no notes"); return false; }
    if (out.title.empty()) out.title = infos[(size_t)sel].name;
    return true;
}

}  // namespace gs
