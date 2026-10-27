// Offline tests for src/common: PNG encode/decode (screenshots, the built-in window icon).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "common/DriftReader.h"
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

// Two clocks: a producer (sound card capture) writes 16-frame blocks, a consumer (PulseAudio) reads 5 ms chunks at
// a slightly different real rate. The reader must keep its buffer near the target without underruns and
// without clicks.
static void testDriftReader() {
    std::printf("Drift compensation\n");
    const double fs = 48000;
    struct Case {
        double ppm;
        int pblock, cblock;
        unsigned target;
    };
    // PulseOutput: the sound card writes 16 frames, PulseAudio takes 5 ms; system audio passthrough: the other way round
    const Case cases[] = {{-300, 16, 240, 1440}, {0, 16, 240, 1440}, {120, 16, 240, 1440}, {300, 16, 240, 1440},
                          {-300, 240, 16, 1440}, {300, 240, 16, 1440}};
    for (const Case& cs : cases) {
        const double ppm = cs.ppm;
        SpscRing<float> ring(1u << 16);
        DriftReader rd(ring);
        const unsigned target = cs.target;
        rd.reset((unsigned)fs, target);
        const double prodRate = fs, consRate = fs * (1 + ppm * 1e-6);  // true frames per second of each clock
        const int pblock = cs.pblock, cblock = cs.cblock;
        double tProd = 0, tCons = 0.002;
        uint64_t produced = 0;
        int underruns = 0, reads = 0;
        size_t minLevel = SIZE_MAX, maxLevel = 0;
        float maxStep = 0, prev = 0;
        bool havePrev = false;
        std::vector<float> blk(pblock * 2), L(cblock), R(cblock);
        const double seconds = 600;
        while (tCons < seconds) {
            if (tProd <= tCons) {  // producer: a 440 Hz sine
                for (int i = 0; i < pblock; ++i) {
                    const float v = 0.5f * (float)std::sin(2 * M_PI * 440.0 * (double)(produced + i) / fs);
                    blk[2 * i] = blk[2 * i + 1] = v;
                }
                ring.write(blk.data(), blk.size());
                produced += pblock;
                tProd += pblock / prodRate;
                continue;
            }
            const bool was = rd.primed();
            const bool ok = rd.read(L.data(), R.data(), cblock);
            ++reads;
            if (!ok && was) ++underruns;
            if (tCons > 10) {  // after settling
                const size_t lvl = ring.readAvailable() / 2;
                minLevel = std::min(minLevel, lvl);
                maxLevel = std::max(maxLevel, lvl);
                for (int i = 0; i < cblock; ++i) {
                    if (havePrev) maxStep = std::max(maxStep, std::fabs(L[i] - prev));
                    prev = L[i];
                    havePrev = true;
                }
            }
            tCons += cblock / consRate;
        }
        // A 440 Hz sine at 0.5 changes by at most 0.5 * 2 pi * 440 / 48000 = 0.0288 per sample
        std::printf("  %+5.0f ppm, write %3d / read %3d: ratio %.6f, buffer %zu..%zu frames (target %u), underruns %d, max step %.4f\n",
                    ppm, pblock, cblock, rd.ratio(), minLevel, maxLevel, target, underruns, maxStep);
        CHECK(underruns == 0, "%+.0f ppm: %d underruns", ppm, underruns);
        CHECK(minLevel > target / 3 && maxLevel < target * 2, "%+.0f ppm: buffer drifted to %zu..%zu frames", ppm, minLevel, maxLevel);
        // Input consumed per output frame must match the clock ratio (to within the controller's ripple)
        CHECK(std::fabs(rd.ratio() - prodRate / consRate) < 1e-4, "%+.0f ppm: ratio %.6f, expected %.6f", ppm, rd.ratio(),
              prodRate / consRate);
        CHECK(maxStep < 0.031f, "%+.0f ppm: a click (step %.4f)", ppm, maxStep);
    }
}

int main() {
    testDriftReader();
    testRoundTrip();
    testBadInput();
    testIcons();
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
