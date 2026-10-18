#pragma once
// Minimal PNG support for 8-bit images: screenshots, and the window icon that is built into the program.
#include <cstddef>
#include <string>
#include <vector>

namespace gs {

// RGBA8 pixels -> PNG file contents (empty on failure). bottomUp: rows are stored bottom row first (OpenGL read-back)
std::vector<unsigned char> encodePng(int w, int h, const unsigned char* rgba, bool bottomUp = false);
bool writePng(const std::string& path, int w, int h, const unsigned char* rgba, bool bottomUp = false);

// Non-interlaced 8-bit RGB or RGBA PNG -> RGBA8 pixels, top row first. Other kinds of PNG are rejected.
bool decodePng(const unsigned char* data, size_t size, int& w, int& h, std::vector<unsigned char>& rgba);

}  // namespace gs
