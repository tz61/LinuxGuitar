#pragma once
// Song library: built-in lessons (assets/songs) + imported songs (~/.local/share/guitarstudio/songs) and best scores.
// Titles/descriptions may be bilingual ("English|Chinese"); display them through gs::loc().
#include <map>
#include <string>
#include <vector>

namespace gs {

struct SongEntry {
    std::string path, title, artist, desc, error;
    int level = 0;
    bool builtin = false;
    double bpm = 0;
    int num = 4, den = 4;
    int events = 0;
    double lengthSec = 0;
    float best = -1.f;  // best score 0..1
    int plays = 0;
};

class Library {
public:
    void scan(const std::string& builtinDir, const std::string& userDir);
    std::vector<SongEntry>& entries() { return entries_; }
    int find(const std::string& path) const;
    // Copy an external file into the user song folder; returns the new path (empty on failure)
    std::string importFile(const std::string& src, std::string& err);
    // Save pasted text as a song file
    std::string importText(const std::string& text, std::string& err);
    bool remove(const std::string& path);

    void recordScore(const std::string& path, float score);
    void loadScores(const std::string& file);
    void saveScores(const std::string& file) const;
    int nextOf(const std::string& path) const;  // next song (lesson order)

private:
    SongEntry probe(const std::string& path, bool builtin) const;
    std::vector<SongEntry> entries_;
    std::string userDir_;
    std::map<std::string, std::pair<float, int>> scores_;  // file name -> (best score, play count)
};

}  // namespace gs
