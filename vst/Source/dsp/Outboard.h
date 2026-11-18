#pragma once
// ============================================================================
//  Outboard pedals between the preamp and the cabinet (the classic rock chain):
//  overdrive -> wah -> tape echo. Each one optional.
//
//  Overdrive  gain 0..+36 dB into the ADAA shaper (asymmetric soft clip), tone low-pass
//             800 Hz .. 8 kHz, level. Output scaled by 1 / sqrt(gain): louder when driven, like a pedal.
//  Wah        resonant band-pass (TPT state-variable filter, Q 4) swept 350 Hz .. 2.3 kHz,
//             plus a little low-pass. Position from the knob, the expression pedal, the mod
//             wheel or a CC (when the expression pedal works the wah, it stops being the
//             volume pedal).
//  Tape echo  delay up to 1.2 s; wow (0.6 Hz) and flutter (5.5 Hz) on the delay time; the
//             repeats go through a low-pass (tone), a high-pass (120 Hz) and soft
//             saturation. Changing the time glides (like changing tape speed).
// ============================================================================
#include <vector>
#include "Common.h"
#include "Oversampler.h"

namespace ham
{
struct Overdrive
{
    Shaper shaper;
    double lp = 0, a = 1, gain = 1, outGain = 1;
    void reset() { shaper.reset(); lp = 0; shaper.bias = 0.12; }
    void set (double drive, double tone, double levelDb, double sr)
    {
        gain = dbToGain (36.0 * clampd (drive, 0.0, 1.0));
        a = onePoleCoef (800.0 * std::pow (10.0, clampd (tone, 0.0, 1.0)), sr);
        // unity small-signal gain would lose most of the level at high gain; like a real pedal it gets
        // louder as it is driven, but only by the square root of the gain (Level trims the rest)
        outGain = dbToGain (levelDb) / (std::sqrt (gain) * shaper.slope());
    }
    double process (double x)
    {
        const double y = shaper.process (x * gain);
        lp += a * (y - lp);
        return lp * outGain;
    }
};

struct Wah
{
    double ic1 = 0, ic2 = 0, g = 0.05, gInc = 0, gTarget = 0.05;
    static constexpr double kK = 0.25;   // 1 / Q
    void reset() { ic1 = ic2 = 0; }
    void setPosition (double pos, double sr)   // once per chunk
    {
        g = gTarget;
        const double fc = 350.0 * std::pow (2.0, 2.7 * clampd (pos, 0.0, 1.0));
        gTarget = std::tan (kPi * fc / sr);
        gInc = (gTarget - g) / kChunk;
    }
    double process (double x)
    {
        const double gg = g;
        g += gInc;
        const double a1 = 1.0 / (1.0 + gg * (gg + kK)), a2 = gg * a1, a3 = gg * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return 2.0 * kK * v1 + 0.2 * v2;   // normalised band-pass (k v1 peaks at 1) x 2 (+6 dB, the pedal's boost) + some low-pass
    }
};

struct TapeEcho
{
    std::vector<double> buf;
    int mask = 0, w = 0;
    double delay = 0, delayTarget = 0;   // samples
    double wowC = 1, wowS = 0, wowRc = 1, wowRs = 0, flC = 1, flS = 0, flRc = 1, flRs = 0;
    double lp = 0, aLp = 1, hp = 0, aHp = 0, fb = 0, mix = 0, wow = 0;
    double sr = 48000;
    bool first = true;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        int n = 1;
        while (n < (int) (1.35 * sr)) n <<= 1;
        buf.assign ((size_t) n, 0.0);
        mask = n - 1;
        w = 0;
        delay = delayTarget = 0.35 * sr;
        wowC = 1; wowS = 0; flC = 1; flS = 0;
        const double i1 = kTwoPi * 0.6 / sr, i2 = kTwoPi * 5.5 / sr;
        wowRc = std::cos (i1); wowRs = std::sin (i1); flRc = std::cos (i2); flRs = std::sin (i2);
        aHp = onePoleCoef (120.0, sr);
        lp = hp = 0;
        first = true;
    }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0); lp = hp = 0; }

    void set (double timeSec, double feedback, double mixAmt, double wowAmt, double tone)   // once per chunk
    {
        delayTarget = clampd (timeSec, 0.03, 1.2) * sr;
        if (first) { delay = delayTarget; first = false; }   // starts at its time; later changes glide
        fb = clampd (feedback, 0.0, 0.95);
        mix = clampd (mixAmt, 0.0, 1.0);
        wow = clampd (wowAmt, 0.0, 1.0);
        aLp = onePoleCoef (1500.0 * std::pow (2.0, 2.4 * clampd (tone, 0.0, 1.0)), sr);
        // keep the wow / flutter phasors on the unit circle
        double k = 1.5 - 0.5 * (wowC * wowC + wowS * wowS); wowC *= k; wowS *= k;
        k = 1.5 - 0.5 * (flC * flC + flS * flS); flC *= k; flS *= k;
    }

    double process (double x)
    {
        delay += (delayTarget - delay) * 0.0002;   // tape-speed glide (~100 ms)
        double c = wowC * wowRc - wowS * wowRs, s = wowS * wowRc + wowC * wowRs; wowC = c; wowS = s;
        c = flC * flRc - flS * flRs; s = flS * flRc + flC * flRs; flC = c; flS = s;
        const double d = delay * (1.0 + wow * (0.004 * wowS + 0.0012 * flS));
        // linear read (the tape smears anyway)
        const double pos = (double) w - d;
        const double fl = std::floor (pos);
        const int i = (int) fl;
        const double f = pos - fl;
        const double y = buf[(size_t) (i & mask)] + f * (buf[(size_t) ((i + 1) & mask)] - buf[(size_t) (i & mask)]);
        lp += aLp * (y - lp);
        hp += aHp * (lp - hp);
        const double rep = lp - hp;
        const double sat = rep / (1.0 + std::abs (rep));   // the repeats saturate softly
        w = (w + 1) & mask;
        buf[(size_t) w] = x + fb * sat * 1.15;
        return x + mix * rep;
    }
};
} // namespace ham
