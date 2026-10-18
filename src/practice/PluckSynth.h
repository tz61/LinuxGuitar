#pragma once
// Reference synth: extended Karplus-Strong plucked string (one voice per string; like a real guitar, a new note
// on a string cuts the previous one). Used to demonstrate the score in practice mode. Pitch is exact (fractional
// delay allpass), so it never misleads intonation.
#include <cstdint>
#include <random>
#include <vector>

namespace gs {

class PluckSynth {
public:
    static constexpr int kVoices = 8;  // 6 strings + 2 spare voices
    void prepare(double fs);
    void reset();
    // string 0..5 (-1 = any free voice); delaySamples: delayed start (strums); durSamples: faster decay afterwards
    void noteOn(int string, int midi, float velocity, int delaySamples, int durSamples);
    void allNotesOff();
    void render(float* L, float* R, int n, float gain);  // adds to L/R

private:
    struct Voice {
        std::vector<float> line;
        int len = 0, w = 0;
        float apC = 0, apX1 = 0, apY1 = 0, lpPrev = 0, s = 0.5f;
        float loss = 0.999f, lossRelease = 0.99f;
        float panL = 0.7f, panR = 0.7f;
        float amp = 0, fade = 1.f, fadeStep = 0;
        int durLeft = 0;
        bool active = false;
        // Pending notes (queued when delay > 0; several can be scheduled ahead on one string)
        struct Pend {
            int delay, midi, dur;
            float vel;
        };
        Pend pend[4];
        int npend = 0;
        int string = -1;
        float body1 = 0, body2 = 0;  // simple body low-pass state
    };
    void startVoice(Voice& v, int midi, float vel, int durSamples);
    double fs_ = 48000;
    Voice v_[kVoices];
    std::mt19937 rng_{12345};
    std::vector<float> scratch_;  // excitation buffer (allocated in prepare(); no allocation on the audio thread)
    int maxLen_ = 0;
};

// Metronome click
class ClickSynth {
public:
    void prepare(double fs) { fs_ = fs; }
    void trigger(bool accent, int delaySamples);
    void render(float* L, float* R, int n, float gain);

private:
    double fs_ = 48000;
    float phase_ = 0, freq_ = 1000, env_ = 0, decay_ = 0.999f, amp_ = 0;
    int delay_ = -1;
    bool nextAccent_ = false;
};

}  // namespace gs
