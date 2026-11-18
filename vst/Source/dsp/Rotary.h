#pragma once
// ============================================================================
//  The rotary cabinet: one engine for every cabinet model (dsp/Cabinets.h).
//
//  amp output -> crossover (Linkwitz-Riley 4th order) -> horn (treble) / drum (bass,
//  through the cabinet's box resonance) -> each rotor's signal goes into its own delay
//  line. Every "pickup" reads it back for one mic: the direct sound and two
//  reflections (back wall, side wall). For a pickup at angle phi around the cabinet:
//     c       = cos (rotor angle - phi)               1 = the rotor points at the mic
//     delay   = base + path + radius (1 - c) / c_air  Doppler: a moving source
//     filter  = one-pole low-pass, corner from "away" to "facing" by (1 + c) / 2
//                                                     directional filtering: highs beam
//     gain    = 1 - depth + depth * t^n,  t = (1 + c) / 2   amplitude modulation
//                                                     (horn n = 2, a narrow beam; drum n = 1)
//  Dual active horns add a second horn at angle + 180 degrees. The mic layout sets
//  where the pickups are and which channel they feed; mic distance trades AM for
//  reflections. The Room mic adds two late, dark pickups.
//  Rotor motion (speeds, inertia, brake) lives in the engine; this class gets each
//  rotor's angle and per-sample increment once per 32-sample chunk.
// ============================================================================
#include <vector>
#include "Common.h"
#include "Cabinets.h"

namespace ham
{
struct CabSettings
{
    int model = 0, rotorConfig = 0, hornType = 0, hornMouth = 0, drumBaffle = 0, cabBack = 0, micLayout = 1;
    double xover = 800, balance = 0, micDist = 0.4, micAngle = 90, micHorn = 1, micDrum = 1, micRoom = 0;
    bool operator== (const CabSettings& o) const
    {
        return model == o.model && rotorConfig == o.rotorConfig && hornType == o.hornType && hornMouth == o.hornMouth
            && drumBaffle == o.drumBaffle && cabBack == o.cabBack && micLayout == o.micLayout && xover == o.xover
            && balance == o.balance && micDist == o.micDist && micAngle == o.micAngle && micHorn == o.micHorn
            && micDrum == o.micDrum && micRoom == o.micRoom;
    }
    bool operator!= (const CabSettings& o) const { return ! (*this == o); }
};

class RotaryCabinet
{
public:
    static constexpr int kMaxPickups = 16;
    static constexpr double kSpeedOfSound = 343.0;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        int n = 1;
        while (n < (int) (0.05 * sr)) n <<= 1;   // 50 ms: the longest pickup path with margin
        for (auto& b : buf) b.assign ((size_t) n, 0.0);
        mask = n - 1;
        w = 0;
        for (auto& r : rotor) r = {};
        for (auto& f : xo) f.reset();
        for (auto& v : voice) v.reset();
        box.reset();
        configured = false;
    }

    void reset()
    {
        for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0);
        for (auto& f : xo) f.reset();
        box.reset();
        for (auto& list : pickups) for (auto& p : list) p.z = 0.0;
    }

    // once per chunk (rebuilds only when something changed)
    void configure (const CabSettings& s)
    {
        if (configured && s == settings) return;
        settings = s;
        configured = true;
        const auto& M = cabinetModel (s.model);
        xo[0].setLowpass (s.xover, 0.7071067811865476, sr);
        xo[1].setLowpass (s.xover, 0.7071067811865476, sr);
        xo[2].setHighpass (s.xover, 0.7071067811865476, sr);
        xo[3].setHighpass (s.xover, 0.7071067811865476, sr);
        box.setPeak (M.boxHz, M.boxQ, M.boxDb * (s.cabBack == 1 ? 0.3 : 1.0), sr);
        voice[0].setHighpass (M.spkHpHz, 0.5412, sr);
        voice[1].setHighpass (M.spkHpHz, 1.3066, sr);
        voice[2].setLowpass (std::min (M.spkLpHz, 0.45 * sr), 0.5412, sr);
        voice[3].setLowpass (std::min (M.spkLpHz, 0.45 * sr), 1.3066, sr);
        voice[4].setPeak (M.midHz, M.midQ, M.midDb, sr);
        hornGain = s.balance > 0 ? 1.0 : 1.0 + s.balance;
        drumGain = s.balance < 0 ? 1.0 : 1.0 - s.balance;
        outGain = M.outGain;
        buildPickups (s, M);
    }

    // rotor 0 = horn, 1 = drum: angle (rad) at the start of the chunk, increment (rad / sample)
    void setRotor (int i, double angle, double inc)
    {
        auto& r = rotor[(size_t) i];
        r.pc = std::cos (angle); r.ps = std::sin (angle);
        r.rc = std::cos (inc);   r.rs = std::sin (inc);
    }

    void process (double x, double& L, double& R)
    {
        // the speakers' band and the cabinet's colour
        for (auto& v : voice) x = v.process (x);
        // crossover: LR4 = two Butterworth sections each way
        double lo, hi;
        const int cfg = settings.rotorConfig;
        if (cfg == 1)      { hi = x; lo = 0.0; }   // horn only
        else if (cfg == 2) { hi = 0.0; lo = x; }   // drum only
        else
        {
            lo = xo[1].process (xo[0].process (x));
            hi = xo[3].process (xo[2].process (x));
        }
        w = (w + 1) & mask;
        buf[0][(size_t) w] = hi * hornGain;
        buf[1][(size_t) w] = box.process (lo) * drumGain;

        L = R = 0.0;
        for (int ri = 0; ri < 2; ++ri)
        {
            auto& r = rotor[(size_t) ri];
            const double c2 = r.pc * r.rc - r.ps * r.rs, s2 = r.ps * r.rc + r.pc * r.rs;
            r.pc = c2; r.ps = s2;
            const auto& b = buf[(size_t) ri];
            for (int k = 0; k < numPickups[(size_t) ri]; ++k)
            {
                auto& p = pickups[(size_t) ri][(size_t) k];
                double c = r.pc * p.cphi + r.ps * p.sphi;
                if (p.flip) c = -c;
                const double t = 0.5 + 0.5 * c;
                const double v = read (b, p.base + p.rad * (1.0 - c));
                p.z += (p.aAway + (p.aFacing - p.aAway) * t) * (v - p.z);
                const double shape = p.beam2 ? t * t : t;
                const double s = (1.0 - p.am + p.am * shape) * p.gain * p.z;
                L += p.wL * s;
                R += p.wR * s;
            }
        }
        L *= outGain;
        R *= outGain;
    }

    int pickupCount (int rotorIndex) const { return numPickups[(size_t) rotorIndex]; }

private:
    struct Pickup
    {
        double cphi = 1, sphi = 0, base = 0, rad = 0, gain = 0, wL = 0, wR = 0, am = 0, aFacing = 1, aAway = 1;
        bool flip = false, beam2 = false;
        double z = 0;
    };
    struct RotorPhasor { double pc = 1, ps = 0, rc = 1, rs = 0; };

    // 4-point Hermite read, d samples back from the newest sample (d >= 2)
    double read (const std::vector<double>& b, double d) const
    {
        const double pos = (double) w - d;
        const double fl = std::floor (pos);
        const int i = (int) fl;
        const double f = pos - fl;
        const double xm1 = b[(size_t) ((i - 1) & mask)], x0 = b[(size_t) (i & mask)];
        const double x1 = b[(size_t) ((i + 1) & mask)], x2 = b[(size_t) ((i + 2) & mask)];
        const double c1 = 0.5 * (x1 - xm1);
        const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
        const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    void buildPickups (const CabSettings& s, const CabinetModel& M)
    {
        const double ms = sr / 1000.0;
        const bool shortMouth = s.hornMouth == 1, open = s.cabBack == 1;
        const double hornR = M.hornRadius * (shortMouth ? 0.9 : 1.0);
        const double drumR = M.drumRadius * (s.drumBaffle == 2 ? 0.5 : 1.0);
        const double hornAm = std::min (1.0, M.hornAm * (shortMouth ? 1.15 : 1.0));
        const double drumAm = M.drumAm * (s.drumBaffle == 1 ? 0.5 : (s.drumBaffle == 2 ? 0.7 : 1.0));
        const double hornLp = shortMouth ? 1.4 : 1.0;
        const double drumLp = s.drumBaffle == 1 ? 1.6 : (s.drumBaffle == 2 ? 0.6 : 1.0);
        double amScale = 1.0 - 0.5 * s.micDist, reflScale = 0.6 + 0.8 * s.micDist;
        const double base = (0.3 + 2.0 * s.micDist) * ms;
        const double wall = M.wallMs * (open ? 1.5 : 1.0) * ms, backGain = open ? 0.45 : 0.22;
        const double half = 0.5 * s.micAngle * kPi / 180.0;

        struct Mic { double phi, wL, wR, amMul, reflMul; };
        Mic hornMics[2] {}, drumMics[2] {};
        int nh = 0, nd = 0;
        switch (s.micLayout)
        {
            case 0:  hornMics[nh++] = { 0, 1, 1, 1, 1 };            drumMics[nd++] = { 0, 1, 1, 1, 1 }; break;
            case 2:  hornMics[nh++] = { half, 1, 0, 1, 1 }; hornMics[nh++] = { -half, 0, 1, 1, 1 };
                     drumMics[nd++] = { half, 1, 0, 1, 1 }; drumMics[nd++] = { -half, 0, 1, 1, 1 }; break;
            case 3:  hornMics[nh++] = { half, 1, 0, 1, 1 }; hornMics[nh++] = { -half, 0, 1, 1, 1 };
                     drumMics[nd++] = { 0, 1, 1, 1.0, 0.5 }; break;   // drum close-miked at its port
            case 4:  hornMics[nh++] = { 0, 1, 0, 0.6, 1.6 }; hornMics[nh++] = { kPi, 0, 1, 0.6, 1.6 };   // room pair: front / back
                     drumMics[nd++] = { 0, 1, 0, 0.6, 1.6 }; drumMics[nd++] = { kPi, 0, 1, 0.6, 1.6 }; break;
            default: hornMics[nh++] = { half, 1, 0, 1, 1 }; hornMics[nh++] = { -half, 0, 1, 1, 1 };
                     drumMics[nd++] = { 0, 1, 1, 0.7, 1 }; break;     // stereo horn + mono drum (drum mic further)
        }

        for (int ri = 0; ri < 2; ++ri)
        {
            const bool horn = ri == 0;
            auto& list = pickups[(size_t) ri];
            int n = 0;
            const double rad = (horn ? hornR : drumR) / kSpeedOfSound * sr;
            const double am = horn ? hornAm : drumAm;
            const double aF = onePoleCoef (std::min (0.45 * sr, (horn ? M.hornLpFacing * hornLp : M.drumLpFacing * drumLp)), sr);
            const double aA = onePoleCoef (std::min (0.45 * sr, (horn ? M.hornLpAway * hornLp : M.drumLpAway * drumLp)), sr);
            const double level = horn ? s.micHorn : s.micDrum;
            const Mic* mics = horn ? hornMics : drumMics;
            const int nm = horn ? nh : nd;
            const int copies = horn && s.hornType == 1 ? 2 : 1;   // dual active horns
            const double copyGain = copies == 2 ? 0.75 : 1.0;
            for (int copy = 0; copy < copies; ++copy)
                for (int m = 0; m < nm; ++m)
                {
                    const auto& mic = mics[m];
                    auto add = [&] (double phi, double extra, double gain, double facing, double away, double amMul)
                    {
                        if (n >= kMaxPickups) return;
                        auto& p = list[(size_t) n++];
                        p = {};
                        p.cphi = std::cos (phi); p.sphi = std::sin (phi);
                        p.base = base + extra; p.rad = rad; p.gain = gain * level * copyGain;
                        p.wL = mic.wL; p.wR = mic.wR;
                        p.am = clampd (am * amScale * mic.amMul * amMul, 0.0, 1.0);
                        p.aFacing = facing; p.aAway = away;
                        p.flip = copy == 1; p.beam2 = horn;
                    };
                    add (mic.phi, 0.0, 1.0, aF, aA, 1.0);                                              // direct
                    add (mic.phi + kPi, wall, backGain * reflScale * mic.reflMul, aA, aA, 1.0);        // back wall
                    add (mic.phi + 0.5 * kPi, 0.6 * ms, 0.15 * reflScale * mic.reflMul, aA, aA, 1.0); // side wall
                }
            if (s.micRoom > 0.0)   // room mic: late, dark, decorrelated
            {
                const double aRoom = onePoleCoef (3000.0, sr);
                const double g = 0.6 * s.micRoom * level;
                for (int side = 0; side < 2 && n < kMaxPickups; ++side)
                {
                    auto& p = list[(size_t) n++];
                    p = {};
                    const double phi = side == 0 ? 0.5 * kPi : -0.5 * kPi;
                    p.cphi = std::cos (phi); p.sphi = std::sin (phi);
                    p.base = base + (side == 0 ? 9.0 : 12.5) * ms; p.rad = rad; p.gain = g;
                    p.wL = side == 0 ? 1.0 : 0.0; p.wR = side == 0 ? 0.0 : 1.0;
                    p.am = 0.3 * am; p.aFacing = aRoom; p.aAway = aRoom; p.beam2 = horn;
                }
            }
            numPickups[(size_t) ri] = n;
        }
    }

    double sr = 48000.0;
    std::array<std::vector<double>, 2> buf;
    int mask = 0, w = 0;
    std::array<Biquad, 4> xo {};
    std::array<Biquad, 5> voice {};   // speaker high-pass (2), low-pass (2), cabinet colour peak
    Biquad box;
    std::array<RotorPhasor, 2> rotor {};
    std::array<std::array<Pickup, kMaxPickups>, 2> pickups {};
    std::array<int, 2> numPickups {};
    double hornGain = 1, drumGain = 1, outGain = 1;
    CabSettings settings;
    bool configured = false;
};
} // namespace ham
