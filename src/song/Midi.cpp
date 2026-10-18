// Standard MIDI File (SMF 0/1/2) import. There is no string data; autoFinger assigns the fingering.
#include <algorithm>
#include <cmath>
#include <map>

#include "song/SongInternal.h"

namespace gs {

using namespace detail;

namespace {

struct RawNote { int64_t on, off; int pitch, vel; };

struct ChanTrack {
    int track = 0, channel = 0;
    int program = -1;
    std::string name;
    std::vector<RawNote> notes;
};

bool readVlq(ByteReader& r, size_t end, uint32_t& v) {
    v = 0;
    for (int k = 0; k < 4; ++k) {
        if (r.pos() >= end || !r.ok()) return false;
        uint8_t b = r.u8();
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return false;
}

const char* gmName(int prog) {
    // General MIDI instrument families (displayed through tr())
    static const char* kNames[16] = {"Piano", "Chromatic Percussion", "Organ", "Guitar", "Bass Guitar", "Strings",
                                     "Ensemble", "Brass", "Reed", "Pipe", "Synth Lead", "Synth Pad",
                                     "Synth Effects", "Ethnic", "Percussive", "Sound Effects"};
    if (prog < 0 || prog > 127) return "";
    return tr(kNames[prog / 8]);
}

}  // namespace

bool detail::loadMidiBytes(const std::string& data, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks) {
    out = Song();
    ByteReader r(data.data(), data.size());
    if (r.bytes(4) != "MThd") { err = tr("Not a MIDI file"); return false; }
    uint32_t hlen = r.u32be();
    if (hlen < 6 || hlen > 1024) { err = tr("Corrupt MIDI header"); return false; }
    uint16_t format = r.u16be();
    uint16_t ntrks = r.u16be();
    uint16_t division = r.u16be();
    r.skip(hlen - 6);
    if (!r.ok() || format > 2 || ntrks == 0) { err = tr("Corrupt MIDI header"); return false; }
    double tpq;
    if (division & 0x8000) {
        int fps = -(int8_t)(division >> 8);
        int tpf = division & 0xFF;
        if (fps <= 0 || tpf <= 0) { err = tr("Invalid MIDI time division"); return false; }
        tpq = fps * tpf / 2.0;  // approximate as 120 bpm
    } else {
        tpq = division;
        if (tpq <= 0) { err = tr("Invalid MIDI time division"); return false; }
    }

    std::map<std::pair<int, int>, ChanTrack> chans;  // (track, channel)
    std::vector<std::pair<int64_t, double>> tempos;  // tick, bpm
    std::vector<std::pair<int64_t, std::pair<int, int>>> sigs;
    std::vector<std::pair<int64_t, std::string>> markers;
    std::map<int, std::string> trackNames;
    size_t totalNotes = 0;
    int64_t format2Offset = 0;  // format 2: tracks play one after another

    for (int t = 0; t < ntrks && r.ok() && !r.eof(); ++t) {
        std::string id = r.bytes(4);
        uint32_t len = r.u32be();
        if (!r.ok()) break;
        if (len > r.remaining()) len = (uint32_t)r.remaining();
        size_t end = r.pos() + len;
        if (id != "MTrk") { r.seek(end); --t; continue; }
        int64_t tick = format == 2 ? format2Offset : 0;
        uint8_t running = 0;
        std::map<int, std::vector<RawNote>> open[16];  // pitch -> notes still sounding
        int programs[16];
        std::fill(programs, programs + 16, -1);
        while (r.ok() && r.pos() < end) {
            uint32_t delta;
            if (!readVlq(r, end, delta)) break;
            tick += delta;
            if (r.pos() >= end) break;
            uint8_t st = r.u8();
            if (st == 0xFF) {
                if (r.pos() >= end) break;
                uint8_t type = r.u8();
                uint32_t mlen;
                if (!readVlq(r, end, mlen) || mlen > end - r.pos()) break;
                std::string md = r.bytes(mlen);
                if (type == 0x51 && mlen == 3) {
                    uint32_t us = ((uint8_t)md[0] << 16) | ((uint8_t)md[1] << 8) | (uint8_t)md[2];
                    if (us > 0) tempos.push_back({tick, 60000000.0 / us});
                } else if (type == 0x58 && mlen >= 2) {
                    int nn = (uint8_t)md[0], dd = (uint8_t)md[1];
                    if (nn >= 1 && dd <= 5) sigs.push_back({tick, {nn, 1 << dd}});
                } else if (type == 0x03) {
                    if (!trackNames.count(t)) trackNames[t] = toUtf8Lossy(trim(md));
                } else if (type == 0x06) {
                    markers.push_back({tick, toUtf8Lossy(trim(md))});
                } else if (type == 0x2F) {
                    break;
                }
                running = 0;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {
                uint32_t slen;
                if (!readVlq(r, end, slen) || slen > end - r.pos()) break;
                r.skip(slen);
                running = 0;
                continue;
            }
            uint8_t d1;
            if (st & 0x80) {
                running = st;
                if (r.pos() >= end) break;
                d1 = r.u8();
            } else {
                if (!running) break;  // no running status: corrupt
                d1 = st;
                st = running;
            }
            int ch = st & 0x0F;
            int kind = st & 0xF0;
            uint8_t d2 = 0;
            if (kind != 0xC0 && kind != 0xD0) {
                if (r.pos() >= end) break;
                d2 = r.u8();
            }
            if (kind == 0xC0) {
                programs[ch] = d1 & 0x7F;
                auto it = chans.find({t, ch});
                if (it != chans.end() && it->second.program < 0) it->second.program = programs[ch];
            } else if (kind == 0x90 && d2 > 0) {
                open[ch][d1 & 0x7F].push_back({tick, -1, d1 & 0x7F, d2});
            } else if (kind == 0x80 || (kind == 0x90 && d2 == 0)) {
                auto& v = open[ch][d1 & 0x7F];
                if (!v.empty()) {
                    RawNote n = v.front();
                    v.erase(v.begin());
                    n.off = tick;
                    ChanTrack& ct = chans[{t, ch}];
                    ct.track = t;
                    ct.channel = ch;
                    if (ct.program < 0) ct.program = programs[ch];
                    ct.notes.push_back(n);
                    if (++totalNotes > kMaxNotes) { err = tr("Too many notes"); return false; }
                }
            }
        }
        // notes never released: end them at the end of the track
        for (int ch = 0; ch < 16; ++ch)
            for (auto& kv : open[ch])
                for (RawNote n : kv.second) {
                    n.off = std::max(n.on + 1, tick);
                    ChanTrack& ct = chans[{t, ch}];
                    ct.track = t;
                    ct.channel = ch;
                    if (ct.program < 0) ct.program = programs[ch];
                    ct.notes.push_back(n);
                    if (++totalNotes > kMaxNotes) { err = tr("Too many notes"); return false; }
                }
        if (format == 2) format2Offset = tick;
        r.seek(end);
    }

    std::vector<ChanTrack*> list;
    for (auto& kv : chans)
        if (kv.second.channel != 9 && !kv.second.notes.empty()) list.push_back(&kv.second);
    if (list.empty()) { err = tr("The MIDI file has no usable melodic track (drum tracks excluded)"); return false; }
    std::vector<TrackInfo> infos;
    for (size_t i = 0; i < list.size(); ++i) {
        ChanTrack& ct = *list[i];
        TrackInfo ti;
        ti.index = (int)i;
        auto nm = trackNames.find(ct.track);
        ti.name = nm != trackNames.end() && !nm->second.empty() ? nm->second : (tr("Track ") + std::to_string(ct.track + 1));
        if (ct.program >= 0) ti.name += std::string(" (") + gmName(ct.program) + ")";
        ti.name += " ch" + std::to_string(ct.channel + 1);
        std::string ln = toLower(ti.name);
        ti.isGuitarLike = (ct.program >= 24 && ct.program <= 39) || ln.find("guitar") != std::string::npos ||
                          ln.find("gtr") != std::string::npos || ti.name.find("\xE5\x90\x89\xE4\xBB\x96") != std::string::npos;  // "guitar" in Chinese
        ti.noteCount = (int)ct.notes.size();
        infos.push_back(ti);
    }
    if (tracks) *tracks = infos;
    int sel = (track >= 0 && track < (int)list.size()) ? track : pickBestTrack(infos);
    if (sel < 0) { err = tr("No usable track"); return false; }
    ChanTrack& ct = *list[(size_t)sel];

    auto q = [&](int64_t tick) { return std::round((double)tick / tpq * 24.0) / 24.0; };
    std::sort(tempos.begin(), tempos.end());
    for (auto& tp : tempos) out.tempo.push_back({q(tp.first), tp.second});
    std::sort(sigs.begin(), sigs.end());
    for (auto& s : sigs) out.timeSigs.push_back({q(s.first), s.second.first, s.second.second});
    for (auto& m : markers)
        if (!m.second.empty()) out.sections.push_back({q(m.first), m.second});
    std::sort(ct.notes.begin(), ct.notes.end(), [](const RawNote& a, const RawNote& b) { return a.on < b.on; });
    double end = 0;
    for (const RawNote& n : ct.notes) {
        SongEvent ev;
        ev.beat = q(n.on);
        double d = std::max(1.0 / 24.0, q(n.off) - ev.beat);
        SongNote sn;
        sn.midi = n.pitch;
        sn.durBeats = d;
        if (n.vel >= 110) sn.flags |= NF_Accent;
        ev.durBeats = d;
        ev.notes.push_back(sn);
        end = std::max(end, ev.beat + d);
        out.events.push_back(std::move(ev));
    }
    auto tn = trackNames.find(0);
    out.title = (tn != trackNames.end() && !tn->second.empty() && format == 1 && ct.track != 0) ? tn->second : std::string();
    out.lengthBeats = end;
    out.finalize();
    if (out.events.empty()) { err = tr("The selected track has no notes"); return false; }
    return true;
}

bool loadMidiFile(const std::string& path, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks) {
    std::string data;
    if (!readFileBytes(path, data, err)) return false;
    if (!loadMidiBytes(data, out, err, track, tracks)) return false;
    out.source = path;
    if (out.title.empty()) {
        size_t sl = path.find_last_of('/');
        std::string base = sl == std::string::npos ? path : path.substr(sl + 1);
        size_t dot = base.find_last_of('.');
        out.title = dot == std::string::npos ? base : base.substr(0, dot);
    }
    return true;
}

}  // namespace gs
