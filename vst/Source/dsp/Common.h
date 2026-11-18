#pragma once
// ============================================================================
//  Shared DSP building blocks. Header-only, no JUCE, no allocation.
//  Everything here gets a line-for-line twin in vst/webhost/dsp-worklet.js
//  (phase 6), so keep it plain: doubles, std:: maths only, no SIMD or
//  fast-math tricks. Same helpers as Brass Knuckles' dsp/Common.h.
// ============================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ham
{
constexpr double kPi     = 3.141592653589793;
constexpr double kTwoPi  = 6.283185307179586;
constexpr int    kChunk  = 32;  // control rate: modulation once per 32 samples, per-sample ramps in between

inline double clampd (double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

// Deterministic RNG (identical sequence in the JS engine: Math.imul LCG). Same as Brass Knuckles.
struct Rng
{
    uint32_t s = 0x2545F491u;
    void seed (uint32_t v) { s = v; }
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    double uni() { return (double) (next() >> 8) * (1.0 / 16777216.0); } // [0, 1)
    double bi()  { return uni() * 2.0 - 1.0; }                            // [-1, 1)
};

// Output safety clipper (Brass Knuckles'): exactly linear up to 0.8 (-1.9 dBFS), then a tanh
// knee that never exceeds 1. Below the knee it adds no distortion and no aliasing.
inline double safetyClip (double x)
{
    const double a = std::abs (x);
    if (a <= 0.8) return x;
    const double y = 0.8 + 0.2 * std::tanh ((a - 0.8) / 0.2);
    return x < 0 ? -y : y;
}

// Drawbar position (0..8, fractional allowed) -> gain. Step 8 = 0 dB, each step down about
// -3 dB (step 1 = -21 dB), 0 = off. Below step 1 the gain fades linearly to zero, so
// automation can sweep a bar in and out without a jump.
inline double drawbarGain (double pos)
{
    if (pos <= 0.0) return 0.0;
    if (pos >= 1.0) return std::pow (10.0, (pos - 8.0) * 3.0 / 20.0);
    return pos * 0.08912509381337455; // 10^(-21/20)
}

// Biquad, transposed direct form II; designs from the RBJ cookbook.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
    double process (double x)
    {
        const double y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { s1 = s2 = 0; }
    // RBJ cookbook
    void setAllpass (double f0, double q, double sr)
    {
        const double w = kTwoPi * f0 / sr, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha;
        b0 = (1.0 - alpha) / a0; b1 = -2.0 * cw / a0; b2 = 1.0;   // b2 = (1 + alpha) / a0
        a1 = -2.0 * cw / a0; a2 = (1.0 - alpha) / a0;
    }
    void setLowpass (double f0, double q, double sr)
    {
        const double w = kTwoPi * f0 / sr, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha;
        b0 = (1.0 - cw) / 2.0 / a0; b1 = (1.0 - cw) / a0; b2 = b0;
        a1 = -2.0 * cw / a0; a2 = (1.0 - alpha) / a0;
    }
    void setHighpass (double f0, double q, double sr)
    {
        const double w = kTwoPi * f0 / sr, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha;
        b0 = (1.0 + cw) / 2.0 / a0; b1 = -(1.0 + cw) / a0; b2 = b0;
        a1 = -2.0 * cw / a0; a2 = (1.0 - alpha) / a0;
    }
    void setPeak (double f0, double q, double db, double sr)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = kTwoPi * f0 / sr, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0; b1 = -2.0 * cw / a0; b2 = (1.0 - alpha * A) / a0;
        a1 = -2.0 * cw / a0; a2 = (1.0 - alpha / A) / a0;
    }
};

// One-pole low-pass: y += a (x - y), a from the corner frequency.
inline double onePoleCoef (double hz, double sr) { return 1.0 - std::exp (-kTwoPi * hz / sr); }
} // namespace ham
