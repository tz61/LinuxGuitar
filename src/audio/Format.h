#pragma once
// printf-style formatting into std::string (used with tr()-translated format strings).
#include <cstdarg>
#include <cstdio>
#include <string>

namespace gs {

inline std::string strf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline std::string strf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

}  // namespace gs
