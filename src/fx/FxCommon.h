#pragma once
// Helpers shared by the effect implementations (only for src/fx/*.cpp).
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "dsp/DspUtil.h"
#include "fx/Effect.h"

namespace gs::fx {

inline ParamInfo pLin(const char* id, const char* name, float mn, float mx, float def, const char* unit = "") {
    ParamInfo p;
    p.id = id;
    p.name = name;
    p.minV = mn;
    p.maxV = mx;
    p.defV = def;
    p.unit = unit;
    p.scale = ParamScale::Linear;
    return p;
}
inline ParamInfo pLog(const char* id, const char* name, float mn, float mx, float def, const char* unit = "") {
    ParamInfo p = pLin(id, name, mn, mx, def, unit);
    p.scale = ParamScale::Log;
    return p;
}
inline ParamInfo pInt(const char* id, const char* name, int mn, int mx, int def, const char* unit = "") {
    ParamInfo p = pLin(id, name, (float)mn, (float)mx, (float)def, unit);
    p.scale = ParamScale::Int;
    return p;
}
inline ParamInfo pToggle(const char* id, const char* name, bool def) {
    ParamInfo p = pLin(id, name, 0.f, 1.f, def ? 1.f : 0.f, "");
    p.scale = ParamScale::Toggle;
    return p;
}
inline ParamInfo pChoice(const char* id, const char* name, std::vector<std::string> choices, int def) {
    ParamInfo p = pLin(id, name, 0.f, (float)(choices.size() - 1), (float)def, "");
    p.scale = ParamScale::Choice;
    p.choices = std::move(choices);
    return p;
}

// Mono kernel dispatch: when L and R are identical (mono guitar), process only the left channel and copy it.
// When the input becomes stereo, the left kernel state is first copied to the right kernel to avoid a jump.
// K must be trivially copyable.
template <class K, class Fn>
inline void runMonoKernels(K (&k)[2], bool& wasMono, float* L, float* R, int n, Fn&& fn) {
    static_assert(std::is_trivially_copyable<K>::value, "kernel must be trivially copyable");
    if (dsp::sameChannels(L, R, n)) {
        fn(k[0], L);
        std::memcpy(R, L, sizeof(float) * (size_t)n);
        wasMono = true;
    } else {
        if (wasMono) {
            k[1] = k[0];
            wasMono = false;
        }
        fn(k[0], L);
        fn(k[1], R);
    }
}

// Tempo sync: choice index -> length in beats (quarter note = 1)
inline float syncBeats(int choice, bool withDotted) {
    // withDotted: Off / 1/4 / dotted 1/8 / 1/8 / 1/8 triplet / 1/16; otherwise: Off / 1/4 / 1/8 / 1/8 triplet / 1/16
    static const float a[] = {0.f, 1.f, 0.75f, 0.5f, 1.f / 3.f, 0.25f};
    static const float b[] = {0.f, 1.f, 0.5f, 1.f / 3.f, 0.25f};
    if (withDotted) return (choice >= 0 && choice < 6) ? a[choice] : 0.f;
    return (choice >= 0 && choice < 5) ? b[choice] : 0.f;
}

// Per-effect factories (implemented in the individual .cpp files)
std::unique_ptr<Effect> makeNoiseGate();
std::unique_ptr<Effect> makeCompressor();
std::unique_ptr<Effect> makeBoost();
std::unique_ptr<Effect> makeOverdrive();
std::unique_ptr<Effect> makeDistortion();
std::unique_ptr<Effect> makeFuzz();
std::unique_ptr<Effect> makeAmp();
std::unique_ptr<Effect> makeCabinet();
std::unique_ptr<Effect> makeEq();
std::unique_ptr<Effect> makeWah();
std::unique_ptr<Effect> makeChorus();
std::unique_ptr<Effect> makeFlanger();
std::unique_ptr<Effect> makePhaser();
std::unique_ptr<Effect> makeTremolo();
std::unique_ptr<Effect> makeDelay();
std::unique_ptr<Effect> makeReverb();
std::unique_ptr<Effect> makeOctaver();
std::unique_ptr<Effect> makeVolume();

}  // namespace gs::fx
