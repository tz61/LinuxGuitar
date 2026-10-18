// Effect type table and factory.
#include "fx/FxCommon.h"

namespace gs {

namespace {

struct Entry {
    EffectTypeInfo info;
    std::unique_ptr<Effect> (*make)();
};

const std::vector<Entry>& entries() {
    using namespace gs::fx;
    static const std::vector<Entry> e = {
        {{"noisegate", "Noise Gate", FxCategory::Dynamics,
          "Silences the guitar when you're not playing, removing hiss and hum. A must for high-gain tones."}, makeNoiseGate},
        {{"compressor", "Compressor", FxCategory::Dynamics,
          "Evens out soft and hard picking for a smoother sound and longer sustain."}, makeCompressor},
        {{"boost", "Boost", FxCategory::Drive,
          "Cleanly raises the level to push a drive pedal or the amp harder, or to make solos louder."}, makeBoost},
        {{"overdrive", "Overdrive", FxCategory::Drive,
          "Warm, mild distortion with a full midrange (Tube Screamer style), common in blues and rock."}, makeOverdrive},
        {{"distortion", "Distortion", FxCategory::Drive,
          "Stronger, grittier distortion (RAT style) for rock and punk."}, makeDistortion},
        {{"fuzz", "Fuzz", FxCategory::Drive, "Thick, woolly distortion with huge sustain (Big Muff style)."}, makeFuzz},
        {{"amp", "Amp", FxCategory::Amp,
          "Tube guitar amp model: four channels from clean to metal with bass/mid/treble controls. The heart of your tone."}, makeAmp},
        {{"cabinet", "Cabinet", FxCategory::Cabinet,
          "Simulates the speaker cabinet so distortion isn't harsh. Put it after the amp; WAV impulse responses can be loaded too."},
         makeCabinet},
        {{"eq", "EQ", FxCategory::EQ, "Seven-band graphic EQ for fine-tuning lows, mids and highs."}, makeEq},
        {{"wah", "Wah", FxCategory::Filter, "A sweeping filter that says \"wah\". Auto mode follows your picking dynamics."}, makeWah},
        {{"chorus", "Chorus", FxCategory::Modulation,
          "Makes one guitar sound like several playing together: wider and shimmering, great on clean tones."}, makeChorus},
        {{"flanger", "Flanger", FxCategory::Modulation, "A swooshing jet-plane sweep."}, makeFlanger},
        {{"phaser", "Phaser", FxCategory::Modulation, "A swirling, whooshing sweep, common in funk and rock."}, makePhaser},
        {{"tremolo", "Tremolo", FxCategory::Modulation, "Pulses the volume rhythmically for a vintage surf-rock feel."}, makeTremolo},
        {{"delay", "Delay", FxCategory::Delay, "Echoes that can sync to the tempo; essential for solos and ambient tones."}, makeDelay},
        {{"reverb", "Reverb", FxCategory::Reverb, "Adds room, hall or plate ambience so the guitar sounds less dry."}, makeReverb},
        {{"octaver", "Octaver", FxCategory::Pitch,
          "Adds a voice one or two octaves below so single notes sound as thick as a bass. Single notes only."}, makeOctaver},
        {{"volume", "Volume", FxCategory::Utility, "Adjusts volume, left/right pan and stereo width."}, makeVolume},
    };
    return e;
}

}  // namespace

const std::vector<EffectTypeInfo>& effectTypes() {
    static const std::vector<EffectTypeInfo> types = [] {
        std::vector<EffectTypeInfo> t;
        for (const Entry& e : entries()) t.push_back(e.info);
        return t;
    }();
    return types;
}

std::unique_ptr<Effect> createEffect(const std::string& typeId) {
    for (const Entry& e : entries())
        if (e.info.typeId == typeId) return e.make();
    return nullptr;
}

}  // namespace gs
