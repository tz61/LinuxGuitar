#include "common/I18n.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace gs {

namespace {
std::atomic<int> g_lang{(int)Lang::En};

std::unordered_map<std::string_view, const char*>& zhCatalog() {
    static std::unordered_map<std::string_view, const char*> m;
    return m;
}
}  // namespace

void setLanguage(Lang lang) { g_lang.store((int)lang, std::memory_order_relaxed); }

Lang language() { return (Lang)g_lang.load(std::memory_order_relaxed); }

void addTranslations(const TrPair* pairs, size_t count) {
    auto& m = zhCatalog();
    for (size_t i = 0; i < count; ++i) {
        if (!pairs[i].en || !pairs[i].zh) continue;
        // All modules share one table, so the same English text must not need two translations
        auto [it, inserted] = m.emplace(pairs[i].en, pairs[i].zh);
        if (!inserted && std::strcmp(it->second, pairs[i].zh) != 0)
            std::fprintf(stderr, "i18n: conflicting translations for \"%s\": \"%s\" vs \"%s\" (keeping the first)\n", pairs[i].en,
                         it->second, pairs[i].zh);
    }
}

const char* tr(const char* en) {
    if (!en || language() == Lang::En) return en;
    const auto& m = zhCatalog();
    auto it = m.find(en);
    return it == m.end() ? en : it->second;
}

std::string tr(const std::string& en) { return std::string(tr(en.c_str())); }

std::string loc(const std::string& text) {
    const size_t bar = text.find('|');
    if (bar == std::string::npos) return text;
    bool nonAscii = false;
    for (size_t i = bar + 1; i < text.size(); ++i)
        if ((unsigned char)text[i] >= 0x80) {
            nonAscii = true;
            break;
        }
    if (!nonAscii) return text;  // a '|' that is part of normal text
    std::string en = text.substr(0, bar), zh = text.substr(bar + 1);
    if (language() == Lang::Zh) return zh.empty() ? en : zh;
    return en.empty() ? zh : en;
}

}  // namespace gs
