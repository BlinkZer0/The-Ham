#pragma once
// ============================================================================
//  Spring reverb (after the cabinet; built into the Tone Cabinet and the A-100).
//  Two springs per channel. Each spring is a delay loop (27-46 ms) with a dispersive
//  all-pass chain inside it (the "boing": high frequencies travel at a different
//  speed than low ones, so every repeat smears into a chirp) and a damping low-pass
//  (Tone); the input is high-passed (springs carry little bass). The loop gain gives -60 dB
//  after the Decay time. The four springs have different lengths, so L and R differ.
// ============================================================================
#include <vector>
#include "Common.h"

namespace ham
{
struct Spring
{
    static constexpr int kAllpasses = 8;
    static constexpr double kDisperse = 0.62;
    std::vector<double> buf;
    int mask = 0, w = 0, len = 1;
    double apX[kAllpasses] {}, apY[kAllpasses] {};
    double lp = 0, hp = 0, aLp = 1, aHp = 0, g = 0;

    void prepare (double sr, double ms)
    {
        len = std::max (4, (int) (ms * sr / 1000.0));
        int n = 1;
        while (n < len + 4) n <<= 1;
        buf.assign ((size_t) n, 0.0);
        mask = n - 1;
        w = 0;
        reset();
        aHp = onePoleCoef (40.0, sr);   // in the loop: only keeps DC out (the input is high-passed at 200 Hz)
    }
    void reset()
    {
        std::fill (buf.begin(), buf.end(), 0.0);
        for (int i = 0; i < kAllpasses; ++i) apX[i] = apY[i] = 0;
        lp = hp = 0;
    }
    void set (double decaySec, double tone, double sr)
    {
        g = std::pow (10.0, -3.0 * (len / sr) / std::max (0.1, decaySec));
        aLp = onePoleCoef (2000.0 * std::pow (2.0, 1.6 * clampd (tone, 0.0, 1.0)), sr);
    }
    double process (double x)
    {
        double y = buf[(size_t) ((w - len) & mask)];
        for (int i = 0; i < kAllpasses; ++i)   // first-order all-passes: y = a (x - y1) + x1
        {
            const double o = kDisperse * (y - apY[i]) + apX[i];
            apX[i] = y; apY[i] = o; y = o;
        }
        lp += aLp * (y - lp);
        hp += aHp * (lp - hp);
        const double out = lp - hp;
        w = (w + 1) & mask;
        buf[(size_t) w] = x + g * out;
        return out;
    }
};

struct SpringReverb
{
    Spring s[4];
    double inHp = 0, aIn = 0;
    void prepare (double sr)
    {
        const double ms[4] { 27.3, 41.7, 33.1, 45.9 };
        for (int i = 0; i < 4; ++i) s[i].prepare (sr, ms[i]);
        aIn = onePoleCoef (200.0, sr);
        inHp = 0;
    }
    void reset() { for (auto& x : s) x.reset(); inHp = 0; }
    void set (double decay, double tone, double sr) { for (auto& x : s) x.set (decay, tone, sr); }
    // mono in (the cabinet's mid), stereo wet out
    void process (double x, double& wl, double& wr)
    {
        inHp += aIn * (x - inHp);
        const double in = 0.5 * (x - inHp);
        wl = 0.5 * (s[0].process (in) + s[1].process (in));
        wr = 0.5 * (s[2].process (in) + s[3].process (in));
    }
};
} // namespace ham
