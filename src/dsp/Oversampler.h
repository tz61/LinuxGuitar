#pragma once
// Low-latency oversampling: polyphase IIR half-band filters (two parallel chains of first-order all-pass
// sections, hiir structure), cascaded for 2x/4x/8x.
// Designs are computed at init from (coefficient count, transition bandwidth); the state is POD and can be
// copied per channel.
#include <vector>

namespace gs::dsp {

constexpr int kHbMaxCoefs = 16;

struct HalfBandDesign {
    int n = 0;                     // number of coefficients
    float a[kHbMaxCoefs] = {};     // even index -> path 0, odd index -> path 1
    double transition = 0.1;       // normalized transition bandwidth (relative to the high rate)
};

// nbrCoefs: 1..16; transition: (0, 0.5); passband edge = fs/4 - transition*fs/2
HalfBandDesign designHalfBand(int nbrCoefs, double transition);
// Magnitude (linear) and group delay (in high-rate samples) of the half-band filter
// H(z)=0.5*(A0(z^2)+z^-1*A1(z^2)) at the high rate; fNorm is the normalized frequency (0..0.5)
double halfBandMag(const HalfBandDesign& d, double fNorm);
double halfBandGroupDelay(const HalfBandDesign& d, double fNorm);

struct HalfBandState {
    float x[kHbMaxCoefs] = {};
    float y[kHbMaxCoefs] = {};
    void clear();
};

// in[n] -> out[2n]
void hbUpsample(const HalfBandDesign& d, HalfBandState& s, const float* in, float* out, int n);
// in[2n] -> out[n]  (out may alias in: the output is written more slowly than the input is read)
void hbDownsample(const HalfBandDesign& d, HalfBandState& s, const float* in, float* out, int n);

// Cascaded oversampler design (shared by all channels, read-only)
class OversamplerDesign {
public:
    void setup(int factorLog2);  // 0 = 1x, 1 = 2x, 2 = 4x, 3 = 8x
    int factorLog2() const { return log2_; }
    int factor() const { return 1 << log2_; }
    const HalfBandDesign& stage(int i) const { return st_[i]; }
    // Combined group delay of upsampling + downsampling, in base-rate samples
    double latencyBase(double fHz, double sampleRate) const;

private:
    int log2_ = 0;
    HalfBandDesign st_[3];
};

struct OversamplerState {
    HalfBandState up[3], down[3];
    void clear();
};

// Upsample n samples into out (n*factor samples). tmp needs at least n*factor/2 samples.
void osUp(const OversamplerDesign& d, OversamplerState& s, const float* in, int n, float* out, float* tmp);
// Downsample hi (n*factor samples, overwritten) into out (n samples).
void osDown(const OversamplerDesign& d, OversamplerState& s, float* hi, int n, float* out);

}  // namespace gs::dsp
