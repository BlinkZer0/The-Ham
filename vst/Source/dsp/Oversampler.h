#pragma once
// ============================================================================
//  Oversampling and the waveshapers (tube amp, overdrive pedal).
//
//  Half-band filters: polyphase IIR (two all-pass chains), designed at start-up
//  from a closed formula, so the JS engine computes the same coefficients:
//  the Valenzuela-Constantinides design as published in Laurent de Soras' HIIR
//  library (WTFPL). Stage A (base <-> 2x): 8 coefficients, transition 0.04:
//  flat to 20 kHz at 48 kHz, images / aliases from 28 kHz down by 99 dB (computed).
//  Stage B (2x <-> 4x): 4 coefficients, transition 0.14: rejects -82 dB from 72 kHz,
//  the band that would fold into the audible range (computed). Not linear phase: the
//  delay of a quiet 200 Hz tone is 3.3 samples at 2x, 4.2 at 4x (measured by the tests;
//  kLatency2x / kLatency4x round them, and the plugin reports them to the host).
//  Measured aliasing of a C6 sine driven x8: 1x (ADAA only) -53 dB, 2x -77 dB, 4x -78 dB.
//
//  The shaper is first-order ADAA (antiderivative anti-aliasing, as in E-Piano) at
//  every rate: f(x) = x / (1 + |x|) with a bias for asymmetry (even harmonics, the
//  tube's), normalised to unity small-signal gain.
// ============================================================================
#include "Common.h"

namespace ham
{
struct HalfbandDesign
{
    int n = 0;
    double c[12] {};
    HalfbandDesign (int numCoefs, double transition) : n (numCoefs)
    {
        const int order = n * 2 + 1;
        double k = std::tan ((1.0 - 2.0 * transition) * kPi / 4.0);
        k *= k;
        const double kk = std::sqrt (std::sqrt (1.0 - k * k));
        const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
        const double e4 = e * e * e * e;
        const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
        for (int idx = 0; idx < n; ++idx)
        {
            const int cc = idx + 1;
            double num = 0, den = 0, t;
            int i = 0, j = 1;
            do { t = std::pow (q, i * (i + 1)) * std::sin ((i * 2 + 1) * cc * kPi / order) * j; num += t; j = -j; ++i; } while (std::abs (t) > 1e-100);
            num *= std::pow (q, 0.25);
            i = 1; j = -1;
            do { t = std::pow (q, i * i) * std::cos (i * 2 * cc * kPi / order) * j; den += t; j = -j; ++i; } while (std::abs (t) > 1e-100);
            den += 0.5;
            const double ww = num / den, w2 = ww * ww;
            const double x = std::sqrt ((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
            c[idx] = (1.0 - x) / (1.0 + x);
        }
    }
};

inline const HalfbandDesign& halfbandA() { static const HalfbandDesign d (8, 0.04); return d; }
inline const HalfbandDesign& halfbandB() { static const HalfbandDesign d (4, 0.14); return d; }

// Two all-pass chains (even / odd coefficients) run side by side, as in HIIR's StageProc.
struct HalfbandState
{
    double x[12] {}, y[12] {};
    void reset() { for (int i = 0; i < 12; ++i) x[i] = y[i] = 0; }
    void run (const HalfbandDesign& d, double& s0, double& s1)
    {
        int i = 0;
        for (; i + 1 < d.n; i += 2)
        {
            const double t0 = (s0 - y[i]) * d.c[i] + x[i];
            const double t1 = (s1 - y[i + 1]) * d.c[i + 1] + x[i + 1];
            x[i] = s0; x[i + 1] = s1;
            y[i] = t0; y[i + 1] = t1;
            s0 = t0; s1 = t1;
        }
        if (i < d.n)
        {
            const double t0 = (s0 - y[i]) * d.c[i] + x[i];
            x[i] = s0; y[i] = t0; s0 = t0;
        }
    }
    // one input -> two outputs at twice the rate
    void up (const HalfbandDesign& d, double in, double& out0, double& out1) { out0 = in; out1 = in; run (d, out0, out1); }
    // two inputs -> one output at half the rate
    double down (const HalfbandDesign& d, double in0, double in1)
    {
        double s0 = in1, s1 = in0;
        run (d, s0, s1);
        return 0.5 * (s0 + s1);
    }
};

// ADAA shaper: g(x) = f(x + b) - f(b), f(x) = x / (1 + |x|), F = |x| - ln(1 + |x|).
struct Shaper
{
    double bias = 0.0, x1 = 0.0;
    void reset() { x1 = 0.0; }
    static double f (double x) { return x / (1.0 + std::abs (x)); }
    static double F (double x) { const double a = std::abs (x); return a - std::log (1.0 + a); }
    double G (double x) const { return F (x + bias) - f (bias) * x; }
    double process (double x)
    {
        const double d = x - x1;
        double y;
        if (std::abs (d) < 1e-6) y = f (0.5 * (x + x1) + bias) - f (bias);
        else y = (G (x) - G (x1)) / d;
        x1 = x;
        return y;
    }
    // small-signal slope at 0: f'(b) = 1 / (1 + |b|)^2
    double slope() const { const double a = 1.0 + std::abs (bias); return 1.0 / (a * a); }
};

// The shaper at 1x, 2x or 4x.
struct OversampledShaper
{
    // Delay (base-rate samples, rounded) of the up / down filters, measured by the tests.
    static constexpr int kLatency2x = 3, kLatency4x = 4;   // measured: 3.3 and 4.2 samples (a quiet 200 Hz sine)

    HalfbandState upA, upB, downA, downB;
    Shaper s1, s2, s4;
    void reset() { upA.reset(); upB.reset(); downA.reset(); downB.reset(); s1.reset(); s2.reset(); s4.reset(); }
    void setBias (double b) { s1.bias = s2.bias = s4.bias = b; }
    static int latency (int factor) { return factor >= 4 ? kLatency4x : factor == 2 ? kLatency2x : 0; }

    double process (double x, int factor)
    {
        if (factor <= 1) return s1.process (x);
        const auto& A = halfbandA();
        double a0, a1;
        upA.up (A, x, a0, a1);
        if (factor == 2)
        {
            const double y0 = s2.process (a0);
            const double y1 = s2.process (a1);
            return downA.down (A, y0, y1);
        }
        const auto& B = halfbandB();
        double b0, b1, b2, b3;
        upB.up (B, a0, b0, b1);
        upB.up (B, a1, b2, b3);
        // one statement per shaper call: function arguments have no evaluation order in C++,
        // and the ADAA needs its samples in sequence
        const double y0 = s4.process (b0);
        const double y1 = s4.process (b1);
        const double y2 = s4.process (b2);
        const double y3 = s4.process (b3);
        const double c0 = downB.down (B, y0, y1);
        const double c1 = downB.down (B, y2, y3);
        return downA.down (A, c0, c1);
    }
};
} // namespace ham
