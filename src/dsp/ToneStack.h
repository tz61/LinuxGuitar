#pragma once
// Classic TMB tone stack (Fender/Marshall/Mesa share the topology, with different component values).
// Transfer function from D. Yeh & J. Smith, "Discretization of the '59 Fender Bassman Tone Stack" (DAFx 2006),
// discretized with the bilinear transform: third-order IIR (double, transposed direct form II).
#include <cmath>

#include "dsp/DspUtil.h"

namespace gs::dsp {

class ToneStack {
public:
    enum Model { Fender = 0, Marshall = 1, Mesa = 2 };

    void setup(Model m, double sampleRate) {
        model_ = m;
        c_ = 2.0 * sampleRate;
        switch (m) {
            case Fender:   R1 = 250e3; R2 = 1e6;   R3 = 25e3; R4 = 56e3;  C1 = 250e-12; C2 = 20e-9;  C3 = 20e-9; break;
            case Marshall: R1 = 220e3; R2 = 1e6;   R3 = 22e3; R4 = 33e3;  C1 = 470e-12; C2 = 22e-9;  C3 = 22e-9; break;
            case Mesa:     R1 = 250e3; R2 = 250e3; R3 = 25e3; R4 = 100e3; C1 = 250e-12; C2 = 100e-9; C3 = 47e-9; break;
        }
    }

    // Treble/mid/bass knobs 0..1 (mapped through pot tapers internally)
    void update(double treble, double mid, double bass) {
        const double t = clampv(treble, 0.0, 1.0);
        const double m = std::exp((clampv(mid, 0.0, 1.0) - 1.0) * 3.4);   // log taper
        const double l = std::exp((clampv(bass, 0.0, 1.0) - 1.0) * 3.4);
        const double b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
        const double b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                          m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                          l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
                          l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
        const double b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                          m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                          m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) + t * C1 * C2 * C3 * R1 * R3 * R4 -
                          t * m * C1 * C2 * C3 * R1 * R3 * R4 + t * l * C1 * C2 * C3 * R1 * R2 * R4;
        const double a0 = 1.0;
        const double a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
        const double a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                          l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                          l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
                          (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
        const double a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                          m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                          m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
                          l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;
        const double c = c_, c2 = c * c, c3 = c2 * c;
        // s = c (1 - z^-1)/(1 + z^-1), b0 = 0
        const double B0 = b1 * c + b2 * c2 + b3 * c3;
        const double B1 = b1 * c - b2 * c2 - 3 * b3 * c3;
        const double B2 = -b1 * c - b2 * c2 + 3 * b3 * c3;
        const double B3 = -b1 * c + b2 * c2 - b3 * c3;
        const double A0 = a0 + a1 * c + a2 * c2 + a3 * c3;
        const double A1 = 3 * a0 + a1 * c - a2 * c2 - 3 * a3 * c3;
        const double A2 = 3 * a0 - a1 * c - a2 * c2 + 3 * a3 * c3;
        const double A3 = a0 - a1 * c + a2 * c2 - a3 * c3;
        const double ia = 1.0 / A0;
        b_[0] = B0 * ia; b_[1] = B1 * ia; b_[2] = B2 * ia; b_[3] = B3 * ia;
        a_[1] = A1 * ia; a_[2] = A2 * ia; a_[3] = A3 * ia;
    }

    inline float process(float xf) {
        const double x = xf;
        const double y = b_[0] * x + z_[0];
        z_[0] = b_[1] * x - a_[1] * y + z_[1];
        z_[1] = b_[2] * x - a_[2] * y + z_[2];
        z_[2] = b_[3] * x - a_[3] * y;
        return (float)y;
    }
    void flush() {
        for (double& z : z_) z = undenormal(z);
    }
    void reset() { z_[0] = z_[1] = z_[2] = 0.0; }

    // Magnitude response (linear), for tests
    double magnitude(double f, double sampleRate) const {
        const double w = 2 * kPi * f / sampleRate;
        double nr = 0, ni = 0, dr = 1, di = 0;
        for (int k = 0; k <= 3; ++k) {
            nr += b_[k] * std::cos(k * w);
            ni -= b_[k] * std::sin(k * w);
            if (k > 0) {
                dr += a_[k] * std::cos(k * w);
                di -= a_[k] * std::sin(k * w);
            }
        }
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }

private:
    Model model_ = Fender;
    double c_ = 96000.0;
    double R1 = 250e3, R2 = 1e6, R3 = 25e3, R4 = 56e3, C1 = 250e-12, C2 = 20e-9, C3 = 20e-9;
    double b_[4] = {0, 0, 0, 0}, a_[4] = {1, 0, 0, 0};
    double z_[3] = {0, 0, 0};
};

}  // namespace gs::dsp
