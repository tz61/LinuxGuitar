#include "dsp/Oversampler.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

#include "dsp/DspUtil.h"

namespace gs::dsp {

namespace {

// Half-band all-pass coefficients from elliptic filter theory (same method as hiir's PolyphaseIir2Designer)
void transitionParam(double transition, double& k, double& q) {
    k = std::tan((1.0 - transition * 2.0) * kPi / 4.0);
    k *= k;
    const double kksqrt = std::pow(1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    const double e2 = e * e;
    const double e4 = e2 * e2;
    q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
}

double accNum(double q, int order, int c) {
    double acc = 0.0;
    int j = 1;
    for (int i = 0; i < 200; ++i) {
        double t = std::pow(q, (double)i * (i + 1)) * std::sin((i * 2 + 1) * c * kPi / order) * j;
        acc += t;
        j = -j;
        if (std::fabs(t) < 1e-100) break;
    }
    return acc;
}

double accDen(double q, int order, int c) {
    double acc = 0.0;
    int j = -1;
    for (int i = 1; i < 200; ++i) {
        double t = std::pow(q, (double)i * i) * std::cos(i * 2 * c * kPi / order) * j;
        acc += t;
        j = -j;
        if (std::fabs(t) < 1e-100) break;
    }
    return acc;
}

double computeCoef(int index, double k, double q, int order) {
    const int c = index + 1;
    const double num = accNum(q, order, c) * std::pow(q, 0.25);
    const double den = accDen(q, order, c) + 0.5;
    const double ww = num / den;
    const double wwsq = ww * ww;
    const double x = std::sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
    return (1.0 - x) / (1.0 + x);
}

// Response of one all-pass path (in z^2) at z = e^{jw}
std::complex<double> pathResp(const HalfBandDesign& d, int path, double w) {
    const std::complex<double> z2inv = std::polar(1.0, -2.0 * w);
    std::complex<double> h(1.0, 0.0);
    for (int i = path; i < d.n; i += 2) {
        const double a = d.a[i];
        h *= (a + z2inv) / (1.0 + a * z2inv);
    }
    return h;
}

std::complex<double> hbResp(const HalfBandDesign& d, double fNorm) {
    const double w = 2.0 * kPi * fNorm;
    return 0.5 * (pathResp(d, 0, w) + std::polar(1.0, -w) * pathResp(d, 1, w));
}

}  // namespace

HalfBandDesign designHalfBand(int nbrCoefs, double transition) {
    HalfBandDesign d;
    d.n = clampv(nbrCoefs, 1, kHbMaxCoefs);
    d.transition = clampv(transition, 1e-4, 0.4999);
    double k, q;
    transitionParam(d.transition, k, q);
    const int order = d.n * 2 + 1;
    for (int i = 0; i < d.n; ++i) d.a[i] = (float)computeCoef(i, k, q, order);
    return d;
}

double halfBandMag(const HalfBandDesign& d, double fNorm) { return std::abs(hbResp(d, fNorm)); }

double halfBandGroupDelay(const HalfBandDesign& d, double fNorm) {
    const double df = 1e-6;
    const double p1 = std::arg(hbResp(d, fNorm - df));
    const double p2 = std::arg(hbResp(d, fNorm + df));
    double dp = p2 - p1;
    while (dp > kPi) dp -= 2 * kPi;
    while (dp < -kPi) dp += 2 * kPi;
    return -dp / (2.0 * kPi * 2.0 * df);
}

void HalfBandState::clear() {
    std::memset(x, 0, sizeof(x));
    std::memset(y, 0, sizeof(y));
}

void hbUpsample(const HalfBandDesign& d, HalfBandState& s, const float* in, float* out, int n) {
    const int nc = d.n;
    for (int i = 0; i < n; ++i) {
        float even = in[i], odd = in[i];
        int c = 0;
        for (; c + 1 < nc; c += 2) {
            const float t0 = (even - s.y[c]) * d.a[c] + s.x[c];
            const float t1 = (odd - s.y[c + 1]) * d.a[c + 1] + s.x[c + 1];
            s.x[c] = even;
            s.x[c + 1] = odd;
            s.y[c] = t0;
            s.y[c + 1] = t1;
            even = t0;
            odd = t1;
        }
        if (c < nc) {  // odd coefficient count: the last one belongs to path 0
            const float t0 = (even - s.y[c]) * d.a[c] + s.x[c];
            s.x[c] = even;
            s.y[c] = t0;
            even = t0;
        }
        out[2 * i] = even;
        out[2 * i + 1] = odd;
    }
    for (int c = 0; c < nc; ++c) {
        s.x[c] = undenormal(s.x[c]);
        s.y[c] = undenormal(s.y[c]);
    }
}

void hbDownsample(const HalfBandDesign& d, HalfBandState& s, const float* in, float* out, int n) {
    const int nc = d.n;
    for (int i = 0; i < n; ++i) {
        float s0 = in[2 * i + 1];
        float s1 = in[2 * i];
        int c = 0;
        for (; c + 1 < nc; c += 2) {
            const float t0 = (s0 - s.y[c]) * d.a[c] + s.x[c];
            const float t1 = (s1 - s.y[c + 1]) * d.a[c + 1] + s.x[c + 1];
            s.x[c] = s0;
            s.x[c + 1] = s1;
            s.y[c] = t0;
            s.y[c + 1] = t1;
            s0 = t0;
            s1 = t1;
        }
        if (c < nc) {
            const float t0 = (s0 - s.y[c]) * d.a[c] + s.x[c];
            s.x[c] = s0;
            s.y[c] = t0;
            s0 = t0;
        }
        out[i] = 0.5f * (s0 + s1);
    }
    for (int c = 0; c < nc; ++c) {
        s.x[c] = undenormal(s.x[c]);
        s.y[c] = undenormal(s.y[c]);
    }
}

void OversamplerDesign::setup(int factorLog2) {
    log2_ = clampv(factorLog2, 0, 3);
    // Stage 1 (base rate <-> 2x) is the steepest: passband up to ~0.417*Nyquist(2x), i.e. 20 kHz at 48k,
    // stopband from 28 kHz. Later stages only need to protect 0..20 kHz, so their transition bands are wide
    // and a few coefficients suffice. Coefficient counts give >100 dB stopband rejection (more would only
    // add latency).
    st_[0] = designHalfBand(7, 0.0833);
    st_[1] = designHalfBand(3, 0.29);
    st_[2] = designHalfBand(2, 0.39);
}

double OversamplerDesign::latencyBase(double fHz, double sampleRate) const {
    double total = 0.0;
    double rate = sampleRate;
    for (int s = 0; s < log2_; ++s) {
        rate *= 2.0;
        const double gd = halfBandGroupDelay(st_[s], fHz / rate);  // in high-rate samples
        total += 2.0 * gd * (sampleRate / rate);                   // once up + once down, in base-rate samples
    }
    return total;
}

void OversamplerState::clear() {
    for (int i = 0; i < 3; ++i) {
        up[i].clear();
        down[i].clear();
    }
}

void osUp(const OversamplerDesign& d, OversamplerState& s, const float* in, int n, float* out, float* tmp) {
    const int L = d.factorLog2();
    if (L == 0) {
        if (out != in) std::memcpy(out, in, sizeof(float) * (size_t)n);
        return;
    }
    // Alternate between out/tmp so that the last stage writes into out
    const float* src = in;
    int len = n;
    for (int st = 0; st < L; ++st) {
        const bool last = (st == L - 1);
        // With an odd number of remaining stages write out first, otherwise tmp, so the last stage lands in out
        float* dst = (((L - st) & 1) != 0) ? out : tmp;
        if (last) dst = out;
        hbUpsample(d.stage(st), s.up[st], src, dst, len);
        src = dst;
        len *= 2;
    }
}

void osDown(const OversamplerDesign& d, OversamplerState& s, float* hi, int n, float* out) {
    const int L = d.factorLog2();
    if (L == 0) {
        if (out != hi) std::memcpy(out, hi, sizeof(float) * (size_t)n);
        return;
    }
    int len = n << L;
    // In-place, stage by stage (starting from the highest rate)
    for (int st = L - 1; st >= 0; --st) {
        len >>= 1;
        float* dst = (st == 0) ? out : hi;
        hbDownsample(d.stage(st), s.down[st], hi, dst, len);
    }
}

}  // namespace gs::dsp
