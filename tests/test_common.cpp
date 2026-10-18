// Offline tests for src/common: PNG encode/decode (screenshots, the built-in window icon).
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "common/Png.h"

using namespace gs;

static int g_fail = 0;
#define CHECK(c, ...)                             \
    do {                                          \
        if (!(c)) {                               \
            std::printf("  [FAIL] " __VA_ARGS__); \
            std::printf("\n");                    \
            ++g_fail;                             \
        }                                         \
    } while (0)

static std::vector<unsigned char> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void testRoundTrip() {
    std::printf("PNG round trip\n");
    std::mt19937 rng(7);
    for (int size : {1, 3, 37, 128}) {
        const int w = size, h = size / 2 + 1;
        std::vector<unsigned char> img((size_t)w * h * 4);
        for (auto& v : img) v = (unsigned char)rng();
        for (bool bottomUp : {false, true}) {
            const std::vector<unsigned char> png = encodePng(w, h, img.data(), bottomUp);
            int dw = 0, dh = 0;
            std::vector<unsigned char> out;
            CHECK(decodePng(png.data(), png.size(), dw, dh, out), "%dx%d bottomUp=%d does not decode", w, h, bottomUp);
            CHECK(dw == w && dh == h, "%dx%d decoded as %dx%d", w, h, dw, dh);
            if (out.size() != img.size()) continue;
            bool same = true;
            for (int y = 0; y < h && same; ++y)
                for (int x = 0; x < w * 4; ++x)
                    if (out[(size_t)y * w * 4 + x] != img[(size_t)(bottomUp ? h - 1 - y : y) * w * 4 + x]) {
                        same = false;
                        break;
                    }
            CHECK(same, "%dx%d bottomUp=%d: pixels differ after the round trip", w, h, bottomUp);
        }
    }
}

static void testBadInput() {
    std::printf("PNG bad input\n");
    const unsigned char px[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const std::vector<unsigned char> png = encodePng(2, 2, px, false);
    int w = 0, h = 0;
    std::vector<unsigned char> out;
    for (size_t cut : {(size_t)0, (size_t)7, (size_t)20, png.size() / 2, png.size() - 13})
        CHECK(!decodePng(png.data(), cut, w, h, out), "truncated to %zu bytes but decoded", cut);
    std::vector<unsigned char> bad = png;
    bad[8 + 8 + 8] = 16;  // IHDR bit depth 16: not supported
    CHECK(!decodePng(bad.data(), bad.size(), w, h, out), "16-bit PNG accepted");
    CHECK(!decodePng(nullptr, 100, w, h, out), "null data accepted");
}

// The committed icons were written by another encoder (libpng, with row filters), so they exercise all filter types
static void testIcons() {
    std::printf("App icons\n");
    for (int size : {16, 22, 24, 32, 48, 64, 128, 256}) {
        const std::string path = std::string(GS_ASSET_DIR) + "/../packaging/icons/hicolor/" + std::to_string(size) + "x" +
                                 std::to_string(size) + "/apps/guitarstudio.png";
        const std::vector<unsigned char> png = readAll(path);
        int w = 0, h = 0;
        std::vector<unsigned char> px;
        if (!decodePng(png.data(), png.size(), w, h, px)) {
            CHECK(false, "%s does not decode", path.c_str());
            continue;
        }
        CHECK(w == size && h == size, "%d px icon is %dx%d", size, w, h);
        const unsigned char* corner = &px[0];
        const unsigned char* mid = &px[((size_t)(size * 0.62) * size + size / 2) * 4];  // lower middle of the pick
        CHECK(corner[3] == 0, "%d px icon: corner is not transparent (alpha %d)", size, corner[3]);
        CHECK(mid[3] == 255 && mid[0] > 200 && mid[0] > mid[1] && mid[1] > mid[2],
              "%d px icon: pick is not orange (%d,%d,%d,%d)", size, mid[0], mid[1], mid[2], mid[3]);
    }
}

int main() {
    testRoundTrip();
    testBadInput();
    testIcons();
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
