#pragma once
// ============================================================================
//  The tonewheel generator: 91 wheels on a gear train driven by a synchronous
//  motor. Data only (the engine owns the running state).
//
//  Tuning: f = motor rev/s x gear ratio x teeth. The motor turns at 20 rev/s on
//  60 Hz mains. Wheels 1-84 (C1..B7): ratio of their note, teeth 2, 4, 8 ... 128 by
//  octave. Wheels 85-91 (C8..F#8) have 192 teeth and sit on the shafts of F..B,
//  a fifth below (192/128 = 3/2), so they use the F..B ratios.
//  Sources: the gear table in the brief; setBfree src/tonegen.c (gears60ratios,
//  initOscillators: "select += 5; teeth = 192") for the top-octave rule.
//  Wheel 46 = A4 = 20 x 88/64 x 16 = 440.000 Hz exactly; the other wheels sit
//  -0.71 .. +1.98 cents from equal temperament (tests check every wheel).
// ============================================================================
#include "Common.h"

namespace ham
{
constexpr int kNumWheels = 91;

// Gear ratios (driving teeth / driven teeth) for C .. B, 60 Hz organ.
constexpr int kGearNum[12] { 85, 71, 67, 105, 103, 84, 74, 98, 96, 88, 67, 108 };
constexpr int kGearDen[12] { 104, 82, 73, 108, 100, 77, 64, 80, 74, 64, 46, 70 };
constexpr double kMotorRevPerSec = 20.0;   // 1200 rpm on 60 Hz mains

// Ratio index and teeth of wheel w (1..91).
inline int wheelGear (int w)  { return w <= 84 ? (w - 1) % 12 : (w - 85) + 5; }
inline int wheelTeeth (int w) { return w <= 84 ? (2 << ((w - 1) / 12)) : 192; }

// Frequency of wheel w at nominal speed (60 Hz mains, A = 440).
inline double wheelBaseHz (int w)
{
    const int g = wheelGear (w);
    return kMotorRevPerSec * wheelTeeth (w) * (double) kGearNum[g] / (double) kGearDen[g];
}

// Factory voicing of the generator (per-wheel output level). The upper wheels carry filter
// capacitors that load their pickups; modeled as a gentle fall from wheel 49 to -4 dB at
// wheel 91. Approximation: no per-wheel factory data has been found yet (see PLAN.md).
inline double wheelLevel (int w)
{
    if (w <= 48) return 1.0;
    return dbToGain (-4.0 * (w - 48) / 43.0);
}

// The lowest wheels are not pure sines: their tooth profile adds a little 3rd harmonic.
// -26 dB on wheels 1-12, -40 dB on 13-24, none above. (Age adds more in phase 2.)
inline double wheelThird (int w)
{
    if (w <= 12) return 0.05;
    if (w <= 24) return 0.01;
    return 0.0;
}

// Leakage partner: the wheel that shares a compartment (magnet / pickup chamber) with w.
// The two wheels on a shaft assembly are four octaves apart: 1-36 <-> 49-84, and the 192-tooth
// wheels 85-91 pair with 42-48; 37-41 have no partner. Source: setBfree src/tonegen.c (wheelPairs).
inline int wheelPartner (int w)
{
    if (w >= 1 && w <= 36) return w + 48;
    if (w >= 49 && w <= 84) return w - 48;
    if (w >= 42 && w <= 48) return w + 43;
    if (w >= 85 && w <= 91) return w - 43;
    return 0;
}

// Age: the upper wheels carry filter capacitors, which also tame the harmonics wear adds.
inline double wheelHarmonicRoom (int w) { return w <= 43 ? 1.0 : std::max (0.0, 1.0 - (w - 43) / 20.0); }

// Mains: a 60 Hz organ on 50 Hz mains runs its synchronous motor (and so every wheel)
// at 5/6 speed: about 3.16 semitones flat.
inline double mainsRatio (int mainsChoice) { return mainsChoice == 1 ? 50.0 / 60.0 : 1.0; }
} // namespace ham
