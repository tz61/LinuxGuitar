#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iconv.h>
#include <sys/stat.h>

#include "song/SongInternal.h"

namespace gs {
namespace detail {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool iequals(const std::string& a, const std::string& b) { return toLower(a) == toLower(b); }

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            lines.push_back(cur);
            cur.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::string fileExtLower(const std::string& path) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return toLower(path.substr(dot));
}

bool readFileBytes(const std::string& path, std::string& out, std::string& err, size_t maxBytes) {
    out.clear();
    struct stat st;
    if (stat(path.c_str(), &st) != 0) { err = tr("Cannot open file: ") + path; return false; }
    if (!S_ISREG(st.st_mode)) { err = tr("Not a regular file: ") + path; return false; }
    if ((size_t)st.st_size > maxBytes) { err = tr("File too large (limit 64 MB): ") + path; return false; }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = tr("Cannot open file: ") + path; return false; }
    out.resize((size_t)st.st_size);
    size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) { err = tr("Failed to read file: ") + path; out.clear(); return false; }
    return true;
}

std::string strFormat(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out;
    if (n > 0) {
        out.resize((size_t)n + 1);
        std::vsnprintf(&out[0], out.size(), fmt, ap2);
        out.resize((size_t)n);
    }
    va_end(ap2);
    return out;
}

bool parseIntStrict(const std::string& s0, int& v) {
    std::string s = trim(s0);
    if (s.empty() || s.size() > 11) return false;
    char* end = nullptr;
    errno = 0;
    long x = std::strtol(s.c_str(), &end, 10);
    if (errno || end == s.c_str() || *end != '\0' || x < -2147483647L || x > 2147483647L) return false;
    v = (int)x;
    return true;
}

bool parseDoubleStrict(const std::string& s0, double& v) {
    std::string s = trim(s0);
    if (s.empty() || s.size() > 40) return false;
    char* end = nullptr;
    errno = 0;
    double x = std::strtod(s.c_str(), &end);
    if (errno || end == s.c_str() || *end != '\0' || !(x == x)) return false;
    v = x;
    return true;
}

bool isValidUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > n) return false;
        for (int k = 1; k < len; ++k)
            if (((unsigned char)s[i + k] >> 6) != 0x2) return false;
        i += len;
    }
    return true;
}

std::string toUtf8Lossy(const std::string& s) {
    if (isValidUtf8(s)) return s;
    // map cp1252 0x80..0x9F, everything else as latin1
    static const uint16_t cp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                        0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string o;
    o.reserve(s.size() * 2);
    for (unsigned char c : s) {
        uint32_t cp = c;
        if (c >= 0x80 && c < 0xA0) cp = cp1252[c - 0x80];
        if (cp < 0x80) {
            o += (char)cp;
        } else if (cp < 0x800) {
            o += (char)(0xC0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xE0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

static std::string utf16ToUtf8(const std::string& d, size_t start, bool be) {
    std::string o;
    o.reserve(d.size());
    auto unit = [&](size_t i) -> uint32_t {
        uint8_t a = (uint8_t)d[i], b = (uint8_t)d[i + 1];
        return be ? (uint32_t)((a << 8) | b) : (uint32_t)((b << 8) | a);
    };
    for (size_t i = start; i + 1 < d.size(); i += 2) {
        uint32_t cp = unit(i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < d.size()) {
            uint32_t lo = unit(i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

static bool gbToUtf8(const std::string& in, std::string& out) {
    iconv_t cd = iconv_open("UTF-8", "GB18030");
    if (cd == (iconv_t)-1) return false;
    out.assign(in.size() * 2 + 16, '\0');
    char* src = const_cast<char*>(in.data());
    size_t srcLeft = in.size();
    char* dst = &out[0];
    size_t dstLeft = out.size();
    size_t r = iconv(cd, &src, &srcLeft, &dst, &dstLeft);
    iconv_close(cd);
    if (r == (size_t)-1 || srcLeft != 0) return false;
    out.resize(out.size() - dstLeft);
    return true;
}

std::string normalizeTextEncoding(const std::string& d) {
    if (d.size() >= 3 && (uint8_t)d[0] == 0xEF && (uint8_t)d[1] == 0xBB && (uint8_t)d[2] == 0xBF) return d.substr(3);
    if (d.size() >= 2 && (uint8_t)d[0] == 0xFF && (uint8_t)d[1] == 0xFE) return utf16ToUtf8(d, 2, false);
    if (d.size() >= 2 && (uint8_t)d[0] == 0xFE && (uint8_t)d[1] == 0xFF) return utf16ToUtf8(d, 2, true);
    if (d.size() >= 4 && d[0] != 0 && d[1] == 0 && d[2] != 0 && d[3] == 0) return utf16ToUtf8(d, 0, false);
    if (d.size() >= 4 && d[0] == 0 && d[1] != 0 && d[2] == 0 && d[3] != 0) return utf16ToUtf8(d, 0, true);
    if (isValidUtf8(d)) return d;
    std::string g;
    if (gbToUtf8(d, g)) return g;
    return toUtf8Lossy(d);
}

int pickBestTrack(const std::vector<TrackInfo>& tracks) {
    int best = -1;
    long bestScore = -1;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const TrackInfo& t = tracks[i];
        if (t.noteCount <= 0) continue;
        long score = (long)std::min(t.noteCount, 100000) + (t.isGuitarLike ? 1000000L : 0L);
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    if (best < 0 && !tracks.empty()) best = 0;
    return best;
}

}  // namespace detail
}  // namespace gs
