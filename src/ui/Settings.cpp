#include "ui/Settings.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#ifndef GS_ASSET_DIR
#define GS_ASSET_DIR "assets"
#endif
#ifndef GS_LESSON_DIR_REL
#define GS_LESSON_DIR_REL "../share/guitarstudio/lessons"
#endif

namespace gs {

namespace {
struct Field {
    const char* key;
    std::function<std::string()> get;
    std::function<void(const std::string&)> set;
};

std::vector<Field> fields(Settings& s) {
    auto F = [](const char* k, float& v) {
        return Field{k, [&v] { std::ostringstream o; o << v; return o.str(); },
                     [&v](const std::string& t) { v = std::strtof(t.c_str(), nullptr); }};
    };
    auto I = [](const char* k, int& v) {
        return Field{k, [&v] { return std::to_string(v); }, [&v](const std::string& t) { v = std::atoi(t.c_str()); }};
    };
    auto B = [](const char* k, bool& v) {
        return Field{k, [&v] { return std::string(v ? "1" : "0"); }, [&v](const std::string& t) { v = t == "1" || t == "true"; }};
    };
    auto S = [](const char* k, std::string& v) {
        return Field{k, [&v] { return v; }, [&v](const std::string& t) { v = t; }};
    };
    return {
        S("device", s.device), I("rate", s.rate), I("period", s.period), F("safetyMs", s.safetyMs),
        B("reserveDevice", s.reserveDevice), B("systemAudio", s.systemAudio), F("systemAudioDb", s.systemAudioDb),
        S("inputSource", s.inputSource), I("boost", s.boost), F("captureDb", s.captureDb), B("applyHwGain", s.applyHwGain),
        I("inputChannel", s.inputChannel), F("inputGainDb", s.inputGainDb), I("outputTarget", s.outputTarget), F("masterDb", s.masterDb),
        F("guitarDb", s.guitarDb), F("speed", s.speed), B("waitMode", s.waitMode), B("metronome", s.metronome),
        F("metronomeVol", s.metronomeVol), B("reference", s.reference), F("referenceVol", s.referenceVol),
        B("countIn", s.countIn), F("tolerance", s.tolerance), F("latencyOffsetMs", s.latencyOffsetMs),
        B("autoStart", s.autoStart), F("zoom", s.zoom), F("freeBpm", s.freeBpm), I("beatsPerBar", s.beatsPerBar),
        F("a4", s.a4), B("tunerMute", s.tunerMute), I("tuning", s.tuning), I("language", s.language), F("uiScale", s.uiScale), I("winW", s.winW),
        I("winH", s.winH), B("winMax", s.winMax), I("tab", s.tab),
        S("lastSong", s.lastSong), S("lastPreset", s.lastPreset), B("showLibrary", s.showLibrary),
    };
}
}  // namespace

bool Settings::load(const std::string& path) {
    bool ok = false;
    std::string text = readFile(path, &ok);
    if (!ok) return false;
    std::map<std::string, std::string> kv;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    for (auto& f : fields(*this)) {
        auto it = kv.find(f.key);
        if (it != kv.end()) f.set(it->second);
    }
    // Sanity checks
    period = std::clamp(period, 16, 1024);
    rate = (rate == 44100 || rate == 48000 || rate == 96000) ? rate : 48000;
    uiScale = std::clamp(uiScale, 0.6f, 2.5f);
    language = std::clamp(language, 0, 1);
    speed = std::clamp(speed, 0.25f, 1.5f);
    return true;
}

bool Settings::save(const std::string& path) const {
    std::ostringstream o;
    o << "# Guitar Studio settings\n";
    for (auto& f : fields(const_cast<Settings&>(*this))) o << f.key << "=" << f.get() << "\n";
    return writeFile(path, o.str());
}

std::string homeDir() {
    const char* h = getenv("HOME");
    return h ? h : "/tmp";
}

static std::string xdg(const char* var, const char* fallback) {
    const char* v = getenv(var);
    if (v && *v) return v;
    return homeDir() + fallback;
}

std::string configDir() { return xdg("XDG_CONFIG_HOME", "/.config") + "/guitarstudio"; }
std::string dataDir() { return xdg("XDG_DATA_HOME", "/.local/share") + "/guitarstudio"; }
std::string userSongDir() { return dataDir() + "/songs"; }
std::string userPresetDir() { return configDir() + "/presets"; }

std::string builtinSongDir() {
    // An installed copy is found relative to the executable, whatever the install prefix (~/.local, /usr/local, ...)
    char exe[4096], real[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        std::string dir(exe);
        dir = dir.substr(0, dir.rfind('/') + 1) + GS_LESSON_DIR_REL;
        if (realpath(dir.c_str(), real)) return real;
    }
    return std::string(GS_ASSET_DIR) + "/songs";  // run from the build directory: straight from the source tree
}

bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    std::istringstream is(path);
    std::string part;
    if (path[0] == '/') cur = "/";
    while (std::getline(is, part, '/')) {
        if (part.empty()) continue;
        cur += part + "/";
        mkdir(cur.c_str(), 0755);
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string readFile(const std::string& path, bool* ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (ok) *ok = false;
        return {};
    }
    std::ostringstream o;
    o << f.rdbuf();
    if (ok) *ok = true;
    return o.str();
}

bool writeFile(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == ".." || n[0] == '.') continue;
        std::string full = dir + "/" + n;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(full);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string baseName(const std::string& path) {
    auto p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string extLower(const std::string& path) {
    std::string b = baseName(path);
    auto p = b.find_last_of('.');
    if (p == std::string::npos) return {};
    std::string e = b.substr(p);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

std::string sanitizeFileName(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c < 32)
            o += '_';
        else
            o += (char)c;
    }
    if (o.empty() || o == "." || o == "..") o = "untitled";
    if (o.size() > 120) o.resize(120);
    return o;
}

}  // namespace gs
