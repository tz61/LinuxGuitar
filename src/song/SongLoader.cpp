// File format detection and dispatch.
#include <algorithm>

#include "song/SongInternal.h"

namespace gs {

using namespace detail;

namespace {

std::string baseNameNoExt(const std::string& path) {
    size_t sl = path.find_last_of('/');
    std::string b = sl == std::string::npos ? path : path.substr(sl + 1);
    size_t dot = b.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}

bool looksLikeGp345(const std::string& d) {
    return d.size() > 20 && d.compare(1, 18, "FICHIER GUITAR PRO") == 0;
}

int countAsciiTabLines(const std::string& text) {
    int n = 0;
    for (const std::string& l : splitLines(text)) {
        size_t dashes = std::count(l.begin(), l.end(), '-');
        if (dashes >= 8 && (l.find('|') != std::string::npos || dashes * 2 > l.size())) ++n;
    }
    return n;
}

}  // namespace

const std::vector<std::string>& supportedSongExtensions() {
    static const std::vector<std::string> kExt = {".gtab", ".txt", ".tab", ".xml", ".musicxml", ".mxl", ".mid",
                                                  ".midi", ".kar", ".gp3", ".gp4", ".gp5", ".gpx", ".gp"};
    return kExt;
}

bool loadGuitarPro(const std::string& path, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks) {
    std::string data;
    if (!readFileBytes(path, data, err)) return false;
    bool ok = false;
    if (looksLikeGp345(data)) {
        ok = loadGp345Bytes(data, out, err, track, tracks);
    } else if (data.size() >= 4 && (data.compare(0, 4, "BCFZ") == 0 || data.compare(0, 4, "BCFS") == 0)) {
        std::string xml;
        ok = gpxExtractScore(data, xml, err) && loadGpifXml(xml, out, err, track, tracks);
    } else if (isZip(data)) {
        std::string xml;
        ok = zipExtractByName(data, "Content/score.gpif", xml, err) && loadGpifXml(xml, out, err, track, tracks);
    } else {
        err = tr("Unrecognized Guitar Pro file");
    }
    if (!ok) return false;
    out.source = path;
    if (out.title.empty()) out.title = baseNameNoExt(path);
    return true;
}

bool parseSongText(const std::string& text0, Song& out, std::string& err) {
    std::string text = normalizeTextEncoding(text0);
    std::string head = trim(text.substr(0, std::min<size_t>(text.size(), 4096)));
    if (head.find("<score-partwise") != std::string::npos || head.find("<score-timewise") != std::string::npos ||
        (startsWith(head, "<?xml") && text.find("<score-") != std::string::npos))
        return loadMusicXmlString(text, out, err, -1, nullptr);
    if (head.find("<GPIF") != std::string::npos) return loadGpifXml(text, out, err, -1, nullptr);
    int asciiLines = countAsciiTabLines(text);
    std::string e1, e2;
    if (asciiLines >= 4) {
        if (parseAsciiTab(text, out, e1)) { err = e1; return true; }
        if (parseGtab(text, out, e2)) { err = e2; return true; }
        err = e1;
        return false;
    }
    if (parseGtab(text, out, e1)) { err = e1; return true; }
    if (asciiLines > 0 && parseAsciiTab(text, out, e2)) { err = e2; return true; }
    err = e1;
    return false;
}

bool loadSongFile(const std::string& path, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks) {
    err.clear();
    if (tracks) tracks->clear();
    std::string ext = fileExtLower(path);
    bool ok = false;
    if (ext == ".mxl" || ext == ".musicxml") {
        ok = loadMusicXml(path, out, err, track, tracks);
    } else if (ext == ".mid" || ext == ".midi" || ext == ".kar") {
        ok = loadMidiFile(path, out, err, track, tracks);
    } else if (ext == ".gp3" || ext == ".gp4" || ext == ".gp5" || ext == ".gpx" || ext == ".gp") {
        ok = loadGuitarPro(path, out, err, track, tracks);
    } else {
        // other extensions (.gtab .txt .tab .xml or unknown): sniff the content first
        std::string data;
        if (!readFileBytes(path, data, err)) return false;
        if (data.size() >= 4 && data.compare(0, 4, "MThd") == 0) {
            ok = loadMidiBytes(data, out, err, track, tracks);
        } else if (looksLikeGp345(data)) {
            ok = loadGp345Bytes(data, out, err, track, tracks);
        } else if (data.size() >= 4 && (data.compare(0, 4, "BCFZ") == 0 || data.compare(0, 4, "BCFS") == 0)) {
            std::string xml;
            ok = gpxExtractScore(data, xml, err) && loadGpifXml(xml, out, err, track, tracks);
        } else if (isZip(data)) {
            std::string xml;
            std::string e2;
            if (zipExtractByName(data, "Content/score.gpif", xml, e2)) ok = loadGpifXml(xml, out, err, track, tracks);
            else ok = loadMusicXml(path, out, err, track, tracks);
        } else if (ext == ".gtab") {
            ok = parseGtab(normalizeTextEncoding(data), out, err);
        } else {
            data = normalizeTextEncoding(data);
            std::string head = data.substr(0, std::min<size_t>(data.size(), 4096));
            if (head.find("<score-partwise") != std::string::npos || head.find("<score-timewise") != std::string::npos ||
                (head.find("<?xml") != std::string::npos && data.find("<score-") != std::string::npos))
                ok = loadMusicXmlString(data, out, err, track, tracks);
            else if (head.find("<GPIF") != std::string::npos)
                ok = loadGpifXml(data, out, err, track, tracks);
            else
                ok = parseSongText(data, out, err);
        }
    }
    if (!ok) return false;
    out.source = path;
    if (out.title.empty()) out.title = baseNameNoExt(path);
    return true;
}

}  // namespace gs
