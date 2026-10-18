// Presets: text read/write + built-in presets.
#include "fx/Preset.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>

#include "common/I18n.h"
#include "fx/Effect.h"

namespace gs {

namespace {

std::string fmtFloat(float v) {
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

bool parseFloat(const std::string& s, float& v) {
    if (s.empty()) return false;
    const char* b = s.data();
    const char* e = b + s.size();
    if (*b == '+') ++b;
    const auto r = std::from_chars(b, e, v);
    return r.ec == std::errc() && r.ptr == e && std::isfinite(v);
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

// printf-style formatting into a std::string (for translated messages with placeholders)
template <class... A>
std::string fmtStr(const char* f, A... a) {
    const int n = std::snprintf(nullptr, 0, f, a...);
    if (n <= 0) return {};
    std::string s((size_t)n + 1, '\0');
    std::snprintf(s.data(), s.size(), f, a...);
    s.resize((size_t)n);
    return s;
}

FxSlotState slot(const char* type, std::vector<std::pair<std::string, float>> params, bool bypass = false) {
    FxSlotState s;
    s.typeId = type;
    s.bypass = bypass;
    s.params = std::move(params);
    return s;
}

}  // namespace

std::string presetToText(const PresetData& p) {
    std::string out = "# Guitar Studio effect chain preset\n";
    out += "name=" + oneLine(p.name) + "\n";
    if (!p.description.empty()) out += "desc=" + oneLine(p.description) + "\n";
    for (const FxSlotState& s : p.slots) {
        out += "fx=" + s.typeId + " bypass=" + (s.bypass ? "1" : "0");
        for (const auto& kv : s.params) out += " " + kv.first + "=" + fmtFloat(kv.second);
        out += "\n";
        if (!s.file.empty()) out += "file=" + oneLine(s.file) + "\n";
    }
    return out;
}

bool presetFromText(const std::string& text, PresetData& out, std::string& err) {
    out = PresetData{};
    err.clear();
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    bool any = false;
    std::unique_ptr<Effect> probe;  // effect for the current fx line (used to validate parameter names)
    bool skipping = false;          // the current fx type is unknown; ignore its file= line
    auto warn = [&](const std::string& m) { err += fmtStr(tr("Line %d: "), lineNo) + m + "\n"; };
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warn(tr("Unrecognized: ") + line);
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "name") {
            out.name = val;
            any = true;
        } else if (key == "desc") {
            out.description = val;
            any = true;
        } else if (key == "fx") {
            any = true;
            std::istringstream ts(val);
            std::string type, tok;
            ts >> type;
            probe = createEffect(type);
            if (!probe) {
                warn(fmtStr(tr("Unknown effect type \"%s\", ignored"), type.c_str()));
                skipping = true;
                continue;
            }
            skipping = false;
            FxSlotState s;
            s.typeId = type;
            while (ts >> tok) {
                const size_t e2 = tok.find('=');
                if (e2 == std::string::npos) {
                    warn(tr("Bad parameter format: ") + tok);
                    continue;
                }
                const std::string pk = tok.substr(0, e2), pv = tok.substr(e2 + 1);
                float v = 0.f;
                if (!parseFloat(pv, v)) {
                    warn(tr("Invalid parameter value: ") + tok);
                    continue;
                }
                if (pk == "bypass") {
                    s.bypass = v > 0.5f;
                } else if (probe->paramIndex(pk) < 0) {
                    warn(fmtStr(tr("%s has no parameter \"%s\", ignored"), type.c_str(), pk.c_str()));
                } else {
                    s.params.emplace_back(pk, v);
                }
            }
            out.slots.push_back(std::move(s));
        } else if (key == "file") {
            if (skipping) continue;
            if (out.slots.empty()) warn(tr("file= line without a preceding fx= line"));
            else out.slots.back().file = val;
        } else {
            warn(fmtStr(tr("Unknown field \"%s\""), key.c_str()));
        }
    }
    if (!any) {
        if (err.empty()) err = tr("Not a valid preset file");
        return false;
    }
    return true;
}

const std::vector<PresetData>& builtinPresets() {
    static const std::vector<PresetData> presets = [] {
        std::vector<PresetData> v;
        v.push_back({"Practice Clean", "A clean, warm tone where every note is clear. Great for everyday practice.",
                     {slot("noisegate", {{"threshold", -62}, {"release", 150}}),
                      slot("compressor", {{"threshold", -24}, {"ratio", 3}, {"attack", 10}, {"release", 180}, {"makeup", 4}, {"mix", 70}}),
                      slot("amp", {{"channel", 0}, {"gain", 3.5f}, {"bass", 5}, {"mid", 5.5f}, {"treble", 6}, {"presence", 5.5f}, {"master", 0}, {"bright", 1}}),
                      slot("cabinet", {{"model", 1}}),
                      slot("reverb", {{"mode", 0}, {"decay", 1.2f}, {"predelay", 10}, {"mix", 15}})}});
        v.push_back({"Dry (No Effects)", "No effects: hear the raw guitar sound (good for tuning).", {}});
        v.push_back({"Clean Chorus", "Shimmering, wide clean chorus for arpeggios.",
                     {slot("noisegate", {{"threshold", -62}}),
                      slot("compressor", {{"threshold", -22}, {"ratio", 3}, {"makeup", 3}, {"mix", 70}}),
                      slot("amp", {{"channel", 0}, {"gain", 3}, {"bass", 5}, {"mid", 5}, {"treble", 6.5f}, {"master", 2}, {"bright", 1}}),
                      slot("cabinet", {{"model", 1}}),
                      slot("chorus", {{"rate", 0.7f}, {"depth", 45}, {"mix", 50}, {"width", 90}}),
                      slot("reverb", {{"mode", 1}, {"decay", 2.2f}, {"predelay", 20}, {"mix", 20}})}});
        v.push_back({"Blues Crunch", "An overdrive pushing a crunchy amp: warm and gritty, follows your picking dynamics.",
                     {slot("noisegate", {{"threshold", -60}}),
                      slot("overdrive", {{"drive", 3}, {"tone", 5.5f}, {"level", 2}}),
                      slot("amp", {{"channel", 1}, {"gain", 4.5f}, {"bass", 5}, {"mid", 6.5f}, {"treble", 6}, {"presence", 5.5f}, {"master", -4}}),
                      slot("cabinet", {{"model", 0}}),
                      slot("reverb", {{"mode", 2}, {"decay", 1.6f}, {"mix", 18}})}});
        v.push_back({"Classic Rock", "Medium British-amp distortion for power chords and riffs.",
                     {slot("noisegate", {{"threshold", -58}}),
                      slot("amp", {{"channel", 1}, {"gain", 7}, {"bass", 5.5f}, {"mid", 6.5f}, {"treble", 6.5f}, {"presence", 6}}),
                      slot("cabinet", {{"model", 2}}),
                      slot("delay", {{"time", 320}, {"feedback", 20}, {"mix", 15}, {"tone", 4000}}),
                      slot("reverb", {{"mode", 0}, {"decay", 1.2f}, {"mix", 12}})}});
        v.push_back({"Heavy Metal", "Tight high gain: noise gate + overdrive boost + metal channel + 4x12 cabinet.",
                     {slot("noisegate", {{"threshold", -50}, {"attack", 0.3f}, {"hold", 20}, {"release", 60}, {"range", -90}}),
                      slot("overdrive", {{"drive", 0.5f}, {"tone", 6}, {"level", 8}}),
                      slot("amp", {{"channel", 3}, {"gain", 7}, {"bass", 6}, {"mid", 3.5f}, {"treble", 6.5f}, {"presence", 6.5f}}),
                      slot("cabinet", {{"model", 2}}),
                      slot("eq", {{"b100", 1.5f}, {"b400", -2}, {"b800", -1}, {"b3k2", 1.5f}})}});
        v.push_back({"Lead Solo", "Smooth, sustaining lead tone with a dotted-eighth delay and hall reverb.",
                     {slot("noisegate", {{"threshold", -55}, {"release", 100}}),
                      slot("amp", {{"channel", 2}, {"gain", 7.5f}, {"bass", 5}, {"mid", 7}, {"treble", 6}, {"presence", 6}}),
                      slot("cabinet", {{"model", 2}}),
                      slot("delay", {{"sync", 2}, {"feedback", 30}, {"mix", 25}, {"mode", 1}, {"tone", 3500}}),
                      slot("reverb", {{"mode", 1}, {"decay", 2.2f}, {"predelay", 25}, {"mix", 18}})}});
        v.push_back({"Ambient", "Chorus + ping-pong delay + long hall reverb for a spacious, ethereal sound.",
                     {slot("compressor", {{"threshold", -26}, {"ratio", 3}, {"makeup", 4}, {"mix", 60}}),
                      slot("amp", {{"channel", 0}, {"gain", 3}, {"bass", 5}, {"mid", 5}, {"treble", 6}, {"master", 3}}),
                      slot("cabinet", {{"model", 1}}),
                      slot("chorus", {{"rate", 0.3f}, {"depth", 50}, {"mix", 40}, {"width", 100}}),
                      slot("delay", {{"time", 520}, {"feedback", 50}, {"mix", 35}, {"tone", 3500}, {"mode", 2}, {"mod", 30}}),
                      slot("reverb", {{"mode", 1}, {"decay", 5}, {"size", 130}, {"predelay", 40}, {"mix", 40}, {"damping", 5000}})}});
        v.push_back({"Funk", "Compressor + auto-wah + bright clean tone for funky rhythm playing.",
                     {slot("compressor", {{"threshold", -30}, {"ratio", 6}, {"attack", 3}, {"release", 100}, {"makeup", 8}}),
                      slot("wah", {{"mode", 1}, {"sensitivity", 60}, {"resonance", 5}, {"low", 400}, {"high", 2400}}),
                      slot("amp", {{"channel", 0}, {"gain", 3}, {"bass", 4.5f}, {"mid", 5}, {"treble", 7}, {"bright", 1}}),
                      slot("cabinet", {{"model", 3}}),
                      slot("reverb", {{"mode", 0}, {"decay", 0.8f}, {"mix", 10}})}});
        v.push_back({"Fuzz", "Thick, hairy fuzz with huge sustain.",
                     {slot("noisegate", {{"threshold", -55}}),
                      slot("fuzz", {{"fuzz", 7}, {"tone", 5}, {"level", 0}}),
                      slot("amp", {{"channel", 0}, {"gain", 4}, {"bass", 5}, {"mid", 6}, {"treble", 5}, {"master", -3.5f}}),
                      slot("cabinet", {{"model", 1}}),
                      slot("reverb", {{"mode", 2}, {"decay", 1.4f}, {"mix", 15}})}});
        return v;
    }();
    return presets;
}

}  // namespace gs
