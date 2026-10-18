// Small XML DOM parser (non-validating, lenient). Used for MusicXML and Guitar Pro 6/7 GPIF.
#include <cctype>
#include <cstring>

#include "song/SongInternal.h"

namespace gs {
namespace detail {

const std::string* XmlNode::attr(const char* n) const {
    for (const auto& a : attrs)
        if (a.first == n) return &a.second;
    return nullptr;
}

std::string XmlNode::attrOr(const char* n, const std::string& def) const {
    const std::string* a = attr(n);
    return a ? *a : def;
}

const XmlNode* XmlNode::child(const char* n) const {
    for (const auto& c : children)
        if (c->name == n) return c.get();
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::childrenNamed(const char* n) const {
    std::vector<const XmlNode*> r;
    for (const auto& c : children)
        if (c->name == n) r.push_back(c.get());
    return r;
}

std::string XmlNode::childText(const char* n, const std::string& def) const {
    const XmlNode* c = child(n);
    return c ? trim(c->text) : def;
}

std::string XmlNode::trimmedText() const { return trim(text); }

const XmlNode* XmlNode::path(const char* p) const {
    const XmlNode* cur = this;
    std::string part;
    for (const char* s = p;; ++s) {
        if (*s == '/' || *s == '\0') {
            if (!part.empty()) {
                cur = cur->child(part.c_str());
                if (!cur) return nullptr;
                part.clear();
            }
            if (*s == '\0') break;
        } else {
            part += *s;
        }
    }
    return cur;
}

namespace {

void appendUtf8(std::string& o, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
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

// decode entities, append to out
void decodeText(const char* s, size_t n, std::string& out) {
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c != '&') { out += c; continue; }
        size_t semi = i + 1;
        while (semi < n && semi - i < 12 && s[semi] != ';') ++semi;
        if (semi >= n || s[semi] != ';') { out += c; continue; }
        std::string ent(s + i + 1, semi - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = 0;
            bool okNum = ent.size() > 1;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                okNum = ent.size() > 2;
                for (size_t k = 2; k < ent.size() && okNum; ++k) {
                    char h = ent[k];
                    int v = std::isdigit((unsigned char)h) ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (v < 0 || cp > 0x10FFFF) okNum = false;
                    else cp = cp * 16 + (uint32_t)v;
                }
            } else {
                for (size_t k = 1; k < ent.size() && okNum; ++k) {
                    if (!std::isdigit((unsigned char)ent[k]) || cp > 0x10FFFF) okNum = false;
                    else cp = cp * 10 + (uint32_t)(ent[k] - '0');
                }
            }
            if (!okNum) { out += c; continue; }
            appendUtf8(out, cp);
        } else {
            out += c;  // unknown entities are kept as-is
            continue;
        }
        i = semi;
    }
}

inline bool isNameChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == ':' || (unsigned char)c >= 0x80;
}

}  // namespace

// encoding from the XML declaration (lower case), empty if absent
static std::string declaredEncoding(const std::string& raw) {
    if (raw.size() < 6 || raw.compare(0, 5, "<?xml") != 0) return {};
    size_t end = raw.find("?>");
    if (end == std::string::npos || end > 400) return {};
    std::string decl = raw.substr(0, end);
    size_t p = decl.find("encoding");
    if (p == std::string::npos) return {};
    size_t q = decl.find_first_of("\"'", p);
    if (q == std::string::npos) return {};
    size_t q2 = decl.find(decl[q], q + 1);
    if (q2 == std::string::npos) return {};
    return toLower(decl.substr(q + 1, q2 - q - 1));
}

std::unique_ptr<XmlNode> parseXml(const std::string& raw, std::string& err) {
    std::string enc = declaredEncoding(raw);
    const std::string text = (!isValidUtf8(raw) && (enc == "iso-8859-1" || enc == "latin1" || enc == "windows-1252" ||
                                                      enc == "us-ascii" || enc == "iso-8859-15"))
                                 ? toUtf8Lossy(raw)
                                 : normalizeTextEncoding(raw);
    const char* s = text.data();
    const size_t n = text.size();
    size_t i = 0;
    if (n >= 3 && std::memcmp(s, "\xEF\xBB\xBF", 3) == 0) i = 3;
    auto root = std::make_unique<XmlNode>();
    root->name = "#document";
    std::vector<XmlNode*> stack{root.get()};
    size_t nodeCount = 0;
    const size_t kMaxNodes = 8u << 20;
    const size_t kMaxDepth = 1024;

    while (i < n) {
        if (s[i] != '<') {
            size_t j = i;
            while (j < n && s[j] != '<') ++j;
            if (stack.size() > 1) decodeText(s + i, j - i, stack.back()->text);
            i = j;
            continue;
        }
        // s[i] == '<'
        if (i + 3 < n && std::memcmp(s + i, "<!--", 4) == 0) {
            const char* e = (const char*)memmem(s + i + 4, n - i - 4, "-->", 3);
            if (!e) break;
            i = (size_t)(e - s) + 3;
            continue;
        }
        if (i + 8 < n && std::memcmp(s + i, "<![CDATA[", 9) == 0) {
            const char* e = (const char*)memmem(s + i + 9, n - i - 9, "]]>", 3);
            size_t end = e ? (size_t)(e - s) : n;
            if (stack.size() > 1) stack.back()->text.append(s + i + 9, end - i - 9);
            i = e ? end + 3 : n;
            continue;
        }
        if (i + 1 < n && s[i + 1] == '?') {
            const char* e = (const char*)memmem(s + i + 2, n - i - 2, "?>", 2);
            if (!e) break;
            i = (size_t)(e - s) + 2;
            continue;
        }
        if (i + 1 < n && s[i + 1] == '!') {
            // DOCTYPE etc.: skip, including an internal subset [...]
            size_t j = i + 2;
            int bracket = 0;
            while (j < n) {
                if (s[j] == '[') ++bracket;
                else if (s[j] == ']') --bracket;
                else if (s[j] == '>' && bracket <= 0) break;
                ++j;
            }
            i = j + 1;
            continue;
        }
        if (i + 1 < n && s[i + 1] == '/') {
            size_t j = i + 2;
            size_t nb = j;
            while (j < n && isNameChar(s[j])) ++j;
            std::string name(s + nb, j - nb);
            while (j < n && s[j] != '>') ++j;
            i = j + 1;
            // lenient: pop up to the element with the same name; ignore if not found
            for (size_t k = stack.size(); k-- > 1;) {
                if (stack[k]->name == name) {
                    stack.resize(k);
                    break;
                }
            }
            continue;
        }
        // start tag
        size_t j = i + 1;
        size_t nb = j;
        while (j < n && isNameChar(s[j])) ++j;
        if (j == nb) {  // not a tag, treat as text
            if (stack.size() > 1) stack.back()->text += '<';
            ++i;
            continue;
        }
        if (++nodeCount > kMaxNodes) { err = tr("Too many XML nodes"); return nullptr; }
        auto node = std::make_unique<XmlNode>();
        node->name.assign(s + nb, j - nb);
        bool selfClose = false;
        for (;;) {
            while (j < n && std::isspace((unsigned char)s[j])) ++j;
            if (j >= n) break;
            if (s[j] == '>') { ++j; break; }
            if (s[j] == '/' && j + 1 < n && s[j + 1] == '>') { selfClose = true; j += 2; break; }
            size_t an = j;
            while (j < n && isNameChar(s[j])) ++j;
            if (j == an) { ++j; continue; }  // garbage character, skip
            std::string aname(s + an, j - an);
            while (j < n && std::isspace((unsigned char)s[j])) ++j;
            std::string aval;
            if (j < n && s[j] == '=') {
                ++j;
                while (j < n && std::isspace((unsigned char)s[j])) ++j;
                if (j < n && (s[j] == '"' || s[j] == '\'')) {
                    char q = s[j++];
                    size_t vb = j;
                    while (j < n && s[j] != q) ++j;
                    decodeText(s + vb, j - vb, aval);
                    if (j < n) ++j;
                } else {
                    size_t vb = j;
                    while (j < n && !std::isspace((unsigned char)s[j]) && s[j] != '>' && s[j] != '/') ++j;
                    decodeText(s + vb, j - vb, aval);
                }
            }
            node->attrs.emplace_back(std::move(aname), std::move(aval));
        }
        i = j;
        XmlNode* raw = node.get();
        stack.back()->children.push_back(std::move(node));
        if (!selfClose) {
            if (stack.size() >= kMaxDepth) { err = tr("XML nested too deeply"); return nullptr; }
            stack.push_back(raw);
        }
    }
    for (auto& c : root->children) {
        if (!c->name.empty()) {
            std::unique_ptr<XmlNode> doc = std::move(c);
            return doc;
        }
    }
    err = tr("Not valid XML");
    return nullptr;
}

}  // namespace detail
}  // namespace gs
