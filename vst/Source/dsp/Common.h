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
} // namespace ham
