#include "common/Png.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gs {

namespace {
const unsigned char kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

void putBe32(std::vector<unsigned char>& out, unsigned v) {
    out.push_back((unsigned char)(v >> 24));
    out.push_back((unsigned char)(v >> 16));
    out.push_back((unsigned char)(v >> 8));
    out.push_back((unsigned char)v);
}

unsigned be32(const unsigned char* b) { return (unsigned)b[0] << 24 | (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3]; }

void putChunk(std::vector<unsigned char>& out, const char* type, const unsigned char* data, size_t len) {
    putBe32(out, (unsigned)len);
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0, (const Bytef*)type, 4);
    if (len) crc = crc32(crc, data, (uInt)len);
    putBe32(out, (unsigned)crc);
}
}  // namespace

std::vector<unsigned char> encodePng(int w, int h, const unsigned char* rgba, bool bottomUp) {
    if (w <= 0 || h <= 0 || !rgba) return {};
    std::vector<unsigned char> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        const unsigned char* row = rgba + (size_t)(bottomUp ? h - 1 - y : y) * w * 4;
        raw.insert(raw.end(), row, row + (size_t)w * 4);
    }
    uLongf clen = compressBound(raw.size());
    std::vector<unsigned char> comp(clen);
    if (compress2(comp.data(), &clen, raw.data(), raw.size(), 6) != Z_OK) return {};
    std::vector<unsigned char> out(kSignature, kSignature + 8);
    unsigned char ihdr[13] = {};
    for (int i = 0; i < 4; ++i) {
        ihdr[i] = (unsigned char)((unsigned)w >> (24 - 8 * i));
        ihdr[4 + i] = (unsigned char)((unsigned)h >> (24 - 8 * i));
    }
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    putChunk(out, "IHDR", ihdr, 13);
    putChunk(out, "IDAT", comp.data(), clen);
    putChunk(out, "IEND", nullptr, 0);
    return out;
}

bool writePng(const std::string& path, int w, int h, const unsigned char* rgba, bool bottomUp) {
    const std::vector<unsigned char> png = encodePng(w, h, rgba, bottomUp);
    if (png.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

bool decodePng(const unsigned char* data, size_t size, int& w, int& h, std::vector<unsigned char>& rgba) {
    if (!data || size < 8 || std::memcmp(data, kSignature, 8) != 0) return false;
    int channels = 0;
    std::vector<unsigned char> idat;
    for (size_t pos = 8; pos + 12 <= size;) {
        const size_t len = be32(data + pos);
        if (len > size - pos - 12) return false;
        const unsigned char* type = data + pos + 4;
        const unsigned char* body = data + pos + 8;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len < 13) return false;
            const unsigned W = be32(body), H = be32(body + 4);
            const int depth = body[8], color = body[9], interlace = body[12];
            channels = color == 6 ? 4 : color == 2 ? 3 : 0;
            if (depth != 8 || !channels || interlace != 0 || W == 0 || H == 0 || W > 8192 || H > 8192) return false;
            w = (int)W;
            h = (int)H;
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (!channels || idat.empty()) return false;

    const size_t stride = (size_t)w * channels;
    std::vector<unsigned char> raw((stride + 1) * h);
    uLongf rawLen = raw.size();
    if (uncompress(raw.data(), &rawLen, idat.data(), idat.size()) != Z_OK || rawLen != raw.size()) return false;

    // Undo the per-row filters (in place: each row becomes the plain pixel bytes)
    for (int y = 0; y < h; ++y) {
        unsigned char* row = &raw[y * (stride + 1) + 1];
        const unsigned char* up = y > 0 ? row - (stride + 1) : nullptr;
        const int filter = row[-1];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= (size_t)channels ? row[x - channels] : 0;
            const int b = up ? up[x] : 0;
            const int c = up && x >= (size_t)channels ? up[x - channels] : 0;
            int pred = 0;
            switch (filter) {
                case 0: break;
                case 1: pred = a; break;
                case 2: pred = b; break;
                case 3: pred = (a + b) / 2; break;
                case 4: {
                    const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
                    pred = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                    break;
                }
                default: return false;
            }
            row[x] = (unsigned char)(row[x] + pred);
        }
    }

    rgba.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = &raw[y * (stride + 1) + 1];
        unsigned char* dst = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; ++x, src += channels, dst += 4) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = channels == 4 ? src[3] : 255;
        }
    }
    return true;
}

}  // namespace gs
