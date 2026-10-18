#include "ui/Library.h"

#include <algorithm>
#include <ctime>
#include <sstream>

#include "common/I18n.h"
#include "song/Song.h"
#include "ui/Settings.h"

namespace gs {

SongEntry Library::probe(const std::string& path, bool builtin) const {
    SongEntry e;
    e.path = path;
    e.builtin = builtin;
    Song s;
    std::string err;
    if (loadSongFile(path, s, err)) {
        e.title = s.title.empty() ? baseName(path) : s.title;
        e.artist = s.artist;
        e.desc = s.description;
        e.level = s.level;
        e.bpm = s.tempo.empty() ? 0 : s.tempo.front().bpm;
        if (!s.timeSigs.empty()) {
            e.num = s.timeSigs.front().num;
            e.den = s.timeSigs.front().den;
        }
        e.events = (int)s.events.size();
        e.lengthSec = s.lengthSec();
    } else {
        e.title = baseName(path);
        e.error = err;
    }
    auto it = scores_.find(baseName(path));
    if (it != scores_.end()) {
        e.best = it->second.first;
        e.plays = it->second.second;
    }
    return e;
}

void Library::scan(const std::string& builtinDir, const std::string& userDir) {
    userDir_ = userDir;
    entries_.clear();
    const auto& exts = supportedSongExtensions();
    auto supported = [&](const std::string& p) {
        std::string e = extLower(p);
        return std::find(exts.begin(), exts.end(), e) != exts.end();
    };
    for (auto& p : listDir(builtinDir))
        if (supported(p)) entries_.push_back(probe(p, true));
    for (auto& p : listDir(userDir))
        if (supported(p)) entries_.push_back(probe(p, false));
}

int Library::find(const std::string& path) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].path == path) return (int)i;
    return -1;
}

std::string Library::importFile(const std::string& src, std::string& err) {
    bool ok = false;
    std::string data = readFile(src, &ok);
    if (!ok) {
        err = std::string(tr("Cannot read file: ")) + src;
        return {};
    }
    if (data.size() > 64u * 1024 * 1024) {
        err = tr("File is too large");
        return {};
    }
    // Make sure it parses first
    Song s;
    if (!loadSongFile(src, s, err)) return {};
    ensureDir(userDir_);
    std::string name = sanitizeFileName(baseName(src));
    std::string dst = userDir_ + "/" + name;
    if (dst == src) return dst;  // already in the library
    // Same name and same content: reuse it; otherwise add a number
    for (int i = 2; fileExists(dst); ++i) {
        bool same = false;
        if (readFile(dst, &same) == data && same) return dst;
        auto dot = name.find_last_of('.');
        std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
        std::string ext = dot == std::string::npos ? "" : name.substr(dot);
        dst = userDir_ + "/" + stem + "_" + std::to_string(i) + ext;
    }
    if (!writeFile(dst, data)) {
        err = std::string(tr("Cannot write ")) + dst;
        return {};
    }
    entries_.push_back(probe(dst, false));
    return dst;
}

std::string Library::importText(const std::string& text, std::string& err) {
    Song s;
    if (!parseSongText(text, s, err)) return {};
    ensureDir(userDir_);
    std::string title = s.title.empty() ? std::string(tr("Pasted song")) : s.title;
    char ts[32];
    time_t now = time(nullptr);
    strftime(ts, sizeof ts, "%m%d_%H%M%S", localtime(&now));
    // Always store as .gtab (keeps title etc. and is easy to edit by hand later)
    if (s.title.empty()) s.title = title;
    std::string dst = userDir_ + "/" + sanitizeFileName(loc(title)) + "_" + ts + ".gtab";
    if (!writeFile(dst, writeGtab(s))) {
        err = std::string(tr("Cannot write ")) + dst;
        return {};
    }
    entries_.push_back(probe(dst, false));
    return dst;
}

bool Library::remove(const std::string& path) {
    int i = find(path);
    if (i < 0 || entries_[i].builtin) return false;
    if (std::remove(path.c_str()) != 0) return false;
    entries_.erase(entries_.begin() + i);
    return true;
}

void Library::recordScore(const std::string& path, float score) {
    auto& v = scores_[baseName(path)];
    v.first = std::max(v.first, score);
    v.second += 1;
    int i = find(path);
    if (i >= 0) {
        entries_[i].best = v.first;
        entries_[i].plays = v.second;
    }
}

void Library::loadScores(const std::string& file) {
    bool ok = false;
    std::istringstream is(readFile(file, &ok));
    std::string line;
    while (std::getline(is, line)) {
        // file name \t best score \t play count
        auto a = line.find('\t');
        auto b = line.find('\t', a == std::string::npos ? a : a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        scores_[line.substr(0, a)] = {std::strtof(line.substr(a + 1, b - a - 1).c_str(), nullptr),
                                      std::atoi(line.substr(b + 1).c_str())};
    }
}

void Library::saveScores(const std::string& file) const {
    std::ostringstream o;
    for (auto& kv : scores_) o << kv.first << "\t" << kv.second.first << "\t" << kv.second.second << "\n";
    writeFile(file, o.str());
}

int Library::nextOf(const std::string& path) const {
    int i = find(path);
    if (i < 0 || i + 1 >= (int)entries_.size()) return -1;
    if (entries_[i].builtin != entries_[i + 1].builtin) return -1;
    return i + 1;
}

}  // namespace gs
