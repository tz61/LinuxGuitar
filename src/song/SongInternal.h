#pragma once
// Internal helpers of the song module: strings, a bounds-checked byte reader, an XML DOM, ZIP extraction.
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/I18n.h"
#include "song/Song.h"

namespace gs {
namespace detail {

constexpr size_t kMaxFileBytes = 64u << 20;  // size limit for external files: 64 MB
constexpr size_t kMaxNotes = 500000;         // maximum number of notes

// ---------------------------------------------------------------- strings
std::string trim(const std::string& s);
std::string toLower(std::string s);
bool startsWith(const std::string& s, const std::string& p);
bool endsWith(const std::string& s, const std::string& p);
bool iequals(const std::string& a, const std::string& b);
std::vector<std::string> splitLines(const std::string& text);  // handles \r\n / \r / \n
std::string fileExtLower(const std::string& path);             // ".gp5"
bool readFileBytes(const std::string& path, std::string& out, std::string& err, size_t maxBytes = kMaxFileBytes);
bool parseIntStrict(const std::string& s, int& v);
// printf-style formatting into a std::string (works with translated tr() format strings)
std::string strFormat(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool parseDoubleStrict(const std::string& s, double& v);
// Converts latin1 / cp1252 bytes to UTF-8 (GP3-5 strings); valid UTF-8 is returned unchanged.
std::string toUtf8Lossy(const std::string& s);
bool isValidUtf8(const std::string& s);
// Normalizes text files to UTF-8: strips a UTF-8 BOM; decodes UTF-16 LE/BE (with or without BOM); for invalid
// UTF-8 tries GB18030 (GBK), then falls back to cp1252.
std::string normalizeTextEncoding(const std::string& data);

// ---------------------------------------------------------------- byte reader
// After any out-of-bounds access ok() stays false and every further read returns 0/empty; never reads past the end.
class ByteReader {
public:
    ByteReader(const void* d, size_t n) : d_(static_cast<const uint8_t*>(d)), n_(n) {}
    bool ok() const { return ok_; }
    void fail() { ok_ = false; p_ = n_; }
    size_t pos() const { return p_; }
    size_t size() const { return n_; }
    size_t remaining() const { return ok_ ? n_ - p_ : 0; }
    bool eof() const { return p_ >= n_; }
    void seek(size_t p) { if (p > n_) fail(); else p_ = p; }
    void skip(size_t k) { if (!ok_ || k > n_ - p_) fail(); else p_ += k; }
    bool has(size_t k) const { return ok_ && k <= n_ - p_; }
    const uint8_t* ptr() const { return d_ + p_; }

    uint8_t u8() { if (!has(1)) { fail(); return 0; } return d_[p_++]; }
    int8_t s8() { return (int8_t)u8(); }
    bool boolean() { return u8() != 0; }
    uint16_t u16le() { if (!has(2)) { fail(); return 0; } uint16_t v = (uint16_t)(d_[p_] | (d_[p_ + 1] << 8)); p_ += 2; return v; }
    int16_t s16le() { return (int16_t)u16le(); }
    uint32_t u32le() {
        if (!has(4)) { fail(); return 0; }
        uint32_t v = (uint32_t)d_[p_] | ((uint32_t)d_[p_ + 1] << 8) | ((uint32_t)d_[p_ + 2] << 16) | ((uint32_t)d_[p_ + 3] << 24);
        p_ += 4; return v;
    }
    int32_t s32le() { return (int32_t)u32le(); }
    uint16_t u16be() { if (!has(2)) { fail(); return 0; } uint16_t v = (uint16_t)((d_[p_] << 8) | d_[p_ + 1]); p_ += 2; return v; }
    uint32_t u32be() {
        if (!has(4)) { fail(); return 0; }
        uint32_t v = ((uint32_t)d_[p_] << 24) | ((uint32_t)d_[p_ + 1] << 16) | ((uint32_t)d_[p_ + 2] << 8) | (uint32_t)d_[p_ + 3];
        p_ += 4; return v;
    }
    double f64le() { uint64_t lo = u32le(); uint64_t hi = u32le(); uint64_t b = lo | (hi << 32); double d; std::memcpy(&d, &b, 8); return d; }
    float f32le() { uint32_t b = u32le(); float f; std::memcpy(&f, &b, 4); return f; }
    std::string bytes(size_t k) {
        if (!has(k)) { fail(); return {}; }
        std::string s(reinterpret_cast<const char*>(d_ + p_), k); p_ += k; return s;
    }

private:
    const uint8_t* d_;
    size_t n_;
    size_t p_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------- XML DOM
struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;  // concatenated direct child text (including CDATA)
    std::vector<std::unique_ptr<XmlNode>> children;

    const std::string* attr(const char* n) const;
    std::string attrOr(const char* n, const std::string& def = std::string()) const;
    const XmlNode* child(const char* n) const;
    std::vector<const XmlNode*> childrenNamed(const char* n) const;
    std::string childText(const char* n, const std::string& def = std::string()) const;  // trimmed
    const XmlNode* path(const char* slashPath) const;  // "a/b/c"
    std::string trimmedText() const;
};
std::unique_ptr<XmlNode> parseXml(const std::string& text, std::string& err);

// ---------------------------------------------------------------- ZIP
struct ZipEntry {
    std::string name;
    uint16_t method = 0;  // 0 = stored, 8 = deflate
    uint32_t compSize = 0, uncompSize = 0, localOffset = 0;
};
bool zipList(const std::string& data, std::vector<ZipEntry>& out, std::string& err);
bool zipExtract(const std::string& data, const ZipEntry& e, std::string& out, std::string& err, size_t maxOut = kMaxFileBytes);
bool zipExtractByName(const std::string& data, const std::string& name, std::string& out, std::string& err);
bool isZip(const std::string& data);

// ---------------------------------------------------------------- internal per-format entry points
bool loadMusicXmlString(const std::string& xml, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks);
bool loadMidiBytes(const std::string& data, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks);
bool loadGp345Bytes(const std::string& data, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks);
bool loadGpifXml(const std::string& xml, Song& out, std::string& err, int track, std::vector<TrackInfo>* tracks);
bool gpxExtractScore(const std::string& data, std::string& scoreXml, std::string& err);  // GP6 .gpx -> score.gpif

// Picks the most guitar-like track: isGuitarLike first, then the most notes. Returns -1 for an empty list.
int pickBestTrack(const std::vector<TrackInfo>& tracks);

}  // namespace detail
}  // namespace gs
