#pragma once
// ============================================================================
//  Preamp with the expression (swell) pedal.
//
//  The organ's expression control does not just turn the volume down: closing the
//  pedal cuts the mids and the treble more than the deep bass, so a half-closed
//  pedal sounds darker as well as quieter. Modeled as a three-band split (one-pole
//  low-passes at 200 Hz and 2.5 kHz) with one gain per band:
//      pedal closed: bass -14 dB, mids -26 dB, treble -32 dB; in dB, linear in the
//      pedal position in between; pedal open: every gain 1, so the split sums back
//      to the input (the preamp is transparent at full expression).
//  The tube amp and its Drive come with the cabinet (phase 3).
// ============================================================================
#include "Common.h"

namespace ham
{
struct Preamp
{
    static constexpr double kBassHz = 200.0, kTrebleHz = 2500.0;
    static constexpr double kClosedBassDb = -14.0, kClosedMidDb = -26.0, kClosedTrebleDb = -32.0;

    double aB = 0, aT = 0;     // one-pole coefficients
    double zB = 0, zT = 0;     // states
    double gB = 1, gM = 1, gT = 1, iB = 0, iM = 0, iT = 0;   // per-sample ramped gains
    double tB = 1, tM = 1, tT = 1;

    void prepare (double sr)
    {
        aB = std::exp (-kTwoPi * kBassHz / sr);
        aT = std::exp (-kTwoPi * kTrebleHz / sr);
        zB = zT = 0;
        gB = gM = gT = tB = tM = tT = 1;
        iB = iM = iT = 0;
    }

    // once per chunk: e = smoothed pedal position 0 (closed) .. 1 (open)
    void setExpression (double e, int chunk)
    {
        gB = tB; gM = tM; gT = tT;   // previous ramp landed
        const double closed = 1.0 - clampd (e, 0.0, 1.0);
        tB = closed == 0.0 ? 1.0 : dbToGain (kClosedBassDb * closed);
        tM = closed == 0.0 ? 1.0 : dbToGain (kClosedMidDb * closed);
        tT = closed == 0.0 ? 1.0 : dbToGain (kClosedTrebleDb * closed);
        iB = (tB - gB) / chunk; iM = (tM - gM) / chunk; iT = (tT - gT) / chunk;
    }

    double process (double x)
    {
        zB = x + aB * (zB - x);   // one-pole low-pass: z += (1 - a)(x - z)
        zT = x + aT * (zT - x);
        const double y = gB * zB + gM * (zT - zB) + gT * (x - zT);
        gB += iB; gM += iM; gT += iT;
        return y;
    }
};
} // namespace ham
