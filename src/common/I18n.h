#pragma once
// Minimal runtime i18n.
//
// Source code uses English strings everywhere. Each module registers a table of Chinese translations
// at startup (registerXxxTranslations()), and user-facing text goes through tr() when it is displayed or
// composed, so the language can be switched at runtime. Untranslated strings fall back to English.
//
// Bilingual data (song titles, lesson descriptions, section names, lyrics) is written as "English|Chinese";
// loc() picks the part for the current language.
#include <cstddef>
#include <string>

namespace gs {

enum class Lang : int { En = 0, Zh = 1 };

struct TrPair {
    const char* en;
    const char* zh;
};

void setLanguage(Lang lang);
Lang language();

// Register translations from a static table. Call during startup, before other threads call tr().
// The strings are not copied, so the table must outlive the program (use static arrays of literals).
void addTranslations(const TrPair* pairs, size_t count);
template <size_t N>
inline void addTranslations(const TrPair (&pairs)[N]) {
    addTranslations(pairs, N);
}

// Translate an English string into the current language; returns the input itself when there is no entry.
// Pass string literals or other long-lived strings (the returned pointer may alias the argument).
// printf-style format strings work too; translations must keep the same conversion specifiers in order.
const char* tr(const char* en);
std::string tr(const std::string& en);

// Pick the variant of a bilingual text "English|Chinese" for the current language. Text without a separator
// (or whose part after '|' is pure ASCII) is returned unchanged. An empty side falls back to the other.
std::string loc(const std::string& text);

}  // namespace gs
