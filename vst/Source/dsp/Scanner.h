#pragma once
// ============================================================================
//  Scanner vibrato / chorus.
//
//  The B-3's vibrato is a delay line made of LC sections; a capacitive scanner,
//  turned by the generator's motor, picks the signal off the line's taps one
//  after another, sweeping up and down the line. Reading a moving point of a
//  delay line shifts the pitch: that is the vibrato. Chorus mixes the scanned
//  signal with the straight one.
//
//  Model:
//   - line: kSections second-order all-pass sections in series (an LC ladder has
//     a nearly flat magnitude and a nearly constant group delay well into the
//     audio band; an all-pass cascade gives the same delay without compounding
//     roll-off). Tap k = the output of section k, tap 0 = the input.
//   - scanner: position p = depth * (1 - cos(rotor)) / 2 taps; adjacent taps are
//     blended linearly by p's fraction (the rotor plate overlaps two stator plates).
//   - the line's own treble loss: one 2nd-order low-pass on the scanned signal.
//   - V1 / V2 / V3 sweep 6 / 12 / 18 sections; C1-C3 the same, mixed 50:50 with dry.
//  The rate is 6.87 Hz on 60 Hz mains at full motor speed; it follows the motor.
//  Depths are measured by the tests and recorded in docs/PLAN.md.
// ============================================================================
#include "Common.h"

namespace ham
{
struct Scanner
{
    static constexpr int kSections = 18;
    static constexpr double kSectionHz = 7900.0, kSectionQ = 0.6;   // per-section all-pass
    static constexpr double kRateHz = 6.87;                          // 60 Hz mains, full speed
    static constexpr double kLineLowpassHz = 6500.0;

    std::array<Biquad, kSections> line {};
    Biquad lp;
    std::array<double, kSections + 1> tap {};
    double rc = 1, rs = 0, pc = 1, ps = 0;   // rotor phasor (cos, sin) and its per-sample rotation

    void prepare (double sr)
    {
        for (auto& s : line) { s.setAllpass (kSectionHz, kSectionQ, sr); s.reset(); }
        lp.setLowpass (kLineLowpassHz, 0.7071067811865476, sr);
        lp.reset();
        pc = 1; ps = 0;   // rotor at the start of the line
        setRate (kRateHz, sr);
    }

    void setRate (double hz, double sr)
    {
        const double inc = kTwoPi * hz / sr;
        rc = std::cos (inc);
        rs = std::sin (inc);
    }

    void renormalize()
    {
        const double k = 1.5 - 0.5 * (pc * pc + ps * ps);
        pc *= k; ps *= k;
    }

    // One sample: x into the line; depth = how many sections the scanner sweeps (0..kSections).
    double process (double x, double depth)
    {
        const double c2 = pc * rc - ps * rs, s2 = ps * rc + pc * rs;
        pc = c2; ps = s2;
        tap[0] = x;
        for (int k = 0; k < kSections; ++k) tap[(size_t) k + 1] = line[(size_t) k].process (tap[(size_t) k]);
        const double p = depth * (1.0 - pc) * 0.5;
        int i = (int) p;
        if (i >= kSections) i = kSections - 1;
        const double f = p - i;
        return lp.process (tap[(size_t) i] + f * (tap[(size_t) i + 1] - tap[(size_t) i]));
    }
};
} // namespace ham
