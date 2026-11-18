// Rotary cabinet checks (phase 3): rotor speeds and inertia, brake, speed controls, Doppler,
// AM, stereo, clicks on speed changes, mono compatibility, the tube amp (aliasing, latency),
// the outboard pedals, the spring reverb and the organ models. Called from PresetTests.cpp main().
#include "TestRender.h"

using namespace ham;
using namespace ham::test;

namespace
{
int fails = 0;
#define KCHECK(cond, ...) do { const bool ok_ = (cond); if (! ok_) ++fails; std::printf ("%-4s ", ok_ ? "ok" : "FAIL"); std::printf (__VA_ARGS__); std::printf ("\n"); } while (0)

Values cabValues()
{
    auto v = defaultValues();
    for (int m = 0; m < 2; ++m) for (int s = 0; s < 2; ++s) setBars (v, m, s, "000000000");
    setBars (v, 0, 1, "008000000");
    v[pd16] = 0; v[pd8] = 0;
    v[leakUp] = v[leakLo] = v[ageUp] = v[ageLo] = 0;
    v[clickUp] = v[clickLo] = v[clickPd] = 0;
    return v;
}

Event key (double t, int k, int manual = 0) { return { sec (t), Event::Key, k, 100.0, manual }; }

// An engine driven directly (for rotor state checks): returns speeds every chunk.
struct Rig
{
    std::unique_ptr<SynthEngine> e = std::make_unique<SynthEngine>();
    EngineParams p;
    std::vector<float> l = std::vector<float> (kChunk), r = std::vector<float> (kChunk);
    explicit Rig (const Values& v) { p = makeEngineParams ([&v] (int i) { return v[(size_t) i]; }); e->setParams (p); e->prepare (kSr); }
    void run (double seconds) { for (int i = 0; i < (int) (seconds * kSr / kChunk); ++i) { e->setParams (p); e->render (l.data(), r.data(), kChunk); } }
    // seconds until f(engine) becomes true (checked every chunk), or -1
    template <typename F> double until (F f, double limit)
    {
        for (int i = 0; i < (int) (limit * kSr / kChunk); ++i)
        {
            e->setParams (p); e->render (l.data(), r.data(), kChunk);
            if (f (*e)) return (i + 1) * kChunk / kSr;
        }
        return -1.0;
    }
};

// ---- rotor speeds, inertia, brake ----------------------------------------------------------
void testRotors()
{
    auto v = cabValues();
    Rig g (v);
    g.run (0.5);
    KCHECK (std::abs (g.e->hornSpeed() - 0.8) < 1e-6 && std::abs (g.e->drumSpeed() - 0.7) < 1e-6,   // float parameters
            "a loaded cabinet runs at its speed at once: horn %.3f, drum %.3f Hz (Slow)", g.e->hornSpeed(), g.e->drumSpeed());

    // Slow -> Fast: each rotor gets within 2% of Fast in its Accel time; the horn first
    g.p.speed = 2;
    double tHorn = -1, tDrum = -1;
    for (int i = 0; i < (int) (8.0 * kSr / kChunk); ++i)
    {
        g.e->setParams (g.p); g.e->render (g.l.data(), g.r.data(), kChunk);
        const double t = (i + 1) * kChunk / kSr;
        if (tHorn < 0 && std::abs (g.e->hornSpeed() - 6.7) < 0.02 * 6.7) tHorn = t;
        if (tDrum < 0 && std::abs (g.e->drumSpeed() - 5.8) < 0.02 * 5.8) tDrum = t;
    }
    KCHECK (std::abs (tHorn - 1.0) < 0.02 && std::abs (tDrum - 4.0) < 0.02 && tHorn < tDrum,
            "Slow -> Fast: horn within 2%% after %.3f s (Accel 1.0), drum after %.3f s (Accel 4.0): the horn leads", tHorn, tDrum);
    KCHECK (std::abs (g.e->hornSpeed() / 6.7 - 1.0) < 0.001 && std::abs (g.e->drumSpeed() / 5.8 - 1.0) < 0.001,
            "steady Fast: horn %.3f Hz, drum %.3f Hz", g.e->hornSpeed(), g.e->drumSpeed());

    // Fast -> Slow: within 2% of Slow in the Brake time; the drum lags
    g.p.speed = 1;
    double bHorn = -1, bDrum = -1, drumAt15 = 0;
    for (int i = 0; i < (int) (8.0 * kSr / kChunk); ++i)
    {
        g.e->setParams (g.p); g.e->render (g.l.data(), g.r.data(), kChunk);
        const double t = (i + 1) * kChunk / kSr;
        if (bHorn < 0 && std::abs (g.e->hornSpeed() - 0.8) < 0.02 * 0.8) bHorn = t;
        if (bDrum < 0 && std::abs (g.e->drumSpeed() - 0.7) < 0.02 * 0.7) bDrum = t;
        if (std::abs (t - 1.5) < 0.5 * kChunk / kSr) drumAt15 = g.e->drumSpeed();
    }
    KCHECK (std::abs (bHorn - 1.5) < 0.02 && std::abs (bDrum - 5.0) < 0.02 && drumAt15 > 1.5 * 0.7,
            "Fast -> Slow: horn within 2%% after %.3f s (Brake 1.5), drum after %.3f s (Brake 5.0); drum still %.1f Hz at 1.5 s",
            bHorn, bDrum, drumAt15);

    // 3-speed Stop with the horn braking to an angle
    {
        auto v3 = cabValues(); v3[speedMode] = 1; v3[brakePos] = 1; v3[brakeAngle] = 90;
        Rig s (v3);
        s.run (1.0);
        s.p.speed = 0;
        s.run (8.0);
        KCHECK (s.e->hornSpeed() == 0.0 && s.e->drumSpeed() == 0.0 && std::abs (s.e->hornTurns() - 0.25) < 0.003,
                "3-speed Stop: both rotors stop; the horn stops facing %.1f degrees (asked 90)", s.e->hornTurns() * 360.0);
        auto v2 = cabValues(); v2[speedMode] = 0;
        Rig t (v2);
        t.p.speed = 0;
        t.run (3.0);
        KCHECK (std::abs (t.e->hornSpeed() - 0.8) < 1e-6, "2-speed: Stop means Slow (horn %.2f Hz)", t.e->hornSpeed());
    }

    // per-rotor overrides, Drum Off, stationary, directions
    {
        auto vo = cabValues(); vo[speed] = 2; vo[hornSpeedSel] = 2; vo[drumOff] = 1;
        Rig o (vo);
        o.run (10.0);
        KCHECK (std::abs (o.e->hornSpeed() - 0.8) < 1e-6 && o.e->drumSpeed() == 0.0, "Horn Speed = Slow over Fast, Drum Off: horn %.2f, drum %.2f Hz",
                o.e->hornSpeed(), o.e->drumSpeed());
        auto vs = cabValues(); vs[speed] = 2; vs[rotorConfig] = 3;
        Rig st (vs);
        st.run (1.0);
        KCHECK (st.e->hornSpeed() == 0.0 && st.e->hornTurns() == 0.0, "Stationary: rotors don't turn");
        auto vd = cabValues();
        Rig dir (vd);
        const double h0 = dir.e->hornTurns(), d0 = dir.e->drumTurns();
        dir.run (0.1);
        KCHECK (dir.e->hornTurns() > h0 && std::fmod (dir.e->drumTurns() - d0 + 1.0, 1.0) > 0.5, "default directions: horn one way, drum the other");
    }
}

// ---- speed controls --------------------------------------------------------------------------
void testSpeedControls()
{
    auto v = cabValues();
    Rig g (v);
    g.run (0.1);
    g.e->setController (1, 0.8, 0); g.run (0.05);
    const int afterUp = g.e->speedSwitch();
    g.e->setController (1, 0.2, 0); g.run (0.05);
    const int afterDown = g.e->speedSwitch();
    KCHECK (afterUp == 2 && afterDown == 1, "mod wheel, half moon: above the middle Fast (%d), below Slow (%d)", afterUp, afterDown);

    auto vm = v; vm[speedWheel] = 2;
    Rig m (vm);
    m.e->setController (1, 0.9, 0); m.run (0.05);
    const int held = m.e->speedSwitch();
    m.e->setController (1, 0.0, 0); m.run (0.05);
    KCHECK (held == 2 && m.e->speedSwitch() == 1, "mod wheel, momentary: Fast only while up");

    auto vs = v; vs[speedSustain] = 1; vs[speedCC] = 20;
    Rig s (vs);
    s.e->setController (64, 1.0, 0); s.run (0.05);
    const int pedalDown = s.e->speedSwitch();
    s.e->setController (64, 0.0, 0); s.run (0.05);
    const int pedalUp = s.e->speedSwitch();
    s.e->setController (20, 0.9, 0); s.run (0.05);
    KCHECK (pedalDown == 2 && pedalUp == 1 && s.e->speedSwitch() == 2, "sustain pedal and Speed CC switch the speed");

    auto v3 = v; v3[speedMode] = 1; v3[speedCC] = 20;
    Rig t (v3);
    t.e->setController (20, 0.1, 0); t.run (0.05);
    KCHECK (t.e->speedSwitch() == 0, "3-speed: Speed CC low third = Stop");

    auto vv = v; vv[speedMode] = 2; vv[varSource] = 0;
    Rig var (vv);
    var.e->setController (1, 0.5, 0);
    var.run (12.0);
    const double want = 0.8 + 0.5 * (6.7 - 0.8);
    KCHECK (std::abs (var.e->hornSpeed() / want - 1.0) < 0.02, "Variable: mod wheel at half -> horn %.2f Hz (%.2f)", var.e->hornSpeed(), want);
}

// ---- Doppler, AM, stereo ---------------------------------------------------------------------
double centsSpread (const std::vector<double>& f)
{
    double lo = 1e9, hi = 0;
    for (double x : f) { lo = std::min (lo, x); hi = std::max (hi, x); }
    return 0.5 * 1200.0 * std::log2 (hi / lo);
}

void testDopplerAndAm()
{
    auto v = cabValues();
    v[rotorConfig] = 1; v[micLayout] = 0; v[micDist] = 0; v[xover] = 300;   // horn only, one close mic
    auto fast = v; fast[speed] = 2;
    const auto a = render (fast, { key (0.02, 45) }, 3.0);      // 8' = wheel 58, 880 Hz
    const auto b = render (v, { key (0.02, 45) }, 3.0);
    const double cFast = centsSpread (periodFrequencies (a, sec (1.0), sec (3.0)));
    const double cSlow = centsSpread (periodFrequencies (b, sec (0.5), sec (3.0)));
    const double expect = 1200.0 * std::log2 (1.0 + 0.15 * kTwoPi * 6.7 / 343.0);   // r w / c
    KCHECK (cFast > 0.6 * expect && cFast < 2.0 * expect && cSlow < cFast * 0.4,
            "Doppler: horn Fast +-%.1f cents (r w / c = %.1f), Slow +-%.1f cents", cFast, expect, cSlow);

    // AM: RMS in windows of 10 periods (11.4 ms) at Fast varies; stationary it doesn't
    auto amDepth = [] (const Audio& x, double t0, double t1)
    {
        const double win = 10.0 / wheelBaseHz (58);
        double lo = 1e9, hi = 0;
        for (double t = t0; t < t1; t += 0.004) { const double r = rms (x, sec (t), sec (t) + (int) std::lround (win * kSr)); lo = std::min (lo, r); hi = std::max (hi, r); }
        return db (hi / lo);
    };
    auto st = v; st[rotorConfig] = 3;
    const auto s = render (st, { key (0.02, 45) }, 1.5);
    KCHECK (amDepth (a, 1.0, 2.0) > 3.0 && amDepth (s, 0.5, 1.4) < 0.3, "AM: horn Fast %.1f dB swing, stationary %.2f dB",
            amDepth (a, 1.0, 2.0), amDepth (s, 0.5, 1.4));

    // stereo: the two horn mics of the Stereo Horn layout differ (the drum below 800 Hz is mono there);
    // Mono layout, Width 0 and Bypass give identical channels
    auto stereo = cabValues(); stereo[speed] = 2; stereo[rotorConfig] = 1;
    setBars (stereo, 0, 1, "888800000");
    const auto x = render (stereo, { key (0.02, 24), key (0.02, 28), key (0.02, 31) }, 1.5);
    double num = 0, dl = 0, dr = 0;
    for (int i = sec (0.5); i < sec (1.5); ++i) { num += x.L[(size_t) i] * x.R[(size_t) i]; dl += x.L[(size_t) i] * x.L[(size_t) i]; dr += x.R[(size_t) i] * x.R[(size_t) i]; }
    const double corr = num / std::sqrt (dl * dr);
    auto mono = stereo; mono[micLayout] = 0;
    auto narrow = stereo; narrow[width] = 0;
    auto bypass = stereo; bypass[cabBypass] = 1;
    auto same = [] (const Audio& y) { for (int i = 0; i < y.size(); ++i) if (y.L[(size_t) i] != y.R[(size_t) i]) return false; return true; };
    KCHECK (corr < 0.95 && same (render (mono, { key (0.02, 24) }, 0.5)) && same (render (narrow, { key (0.02, 24) }, 0.5))
            && same (render (bypass, { key (0.02, 24) }, 0.5)),
            "stereo: horn L/R correlation %.2f at Fast (Stereo Horn); Mono layout, Width 0 and Bypass give L = R", corr);
}

// ---- no clicks on speed changes --------------------------------------------------------------------
// Energy above 12 kHz (4th-order Butterworth high-pass) in 2 ms windows. The rotating beams and the amp
// make that band swing a lot all the time (0 .. 14x its median here), so a switch counts as a click only
// if it stands out from the loudest window elsewhere.
void testSpeedChangeClicks()
{
    auto v = cabValues();
    setBars (v, 0, 1, "888000000");
    const auto a = render (v, { key (0.02, 24), key (0.02, 28), key (0.02, 31) }, 4.0, [] (int start, EngineParams& p)
    {
        p.speed = start >= sec (1.0) && start < sec (2.5) ? 2 : 1;
    });
    Biquad h1, h2;
    h1.setHighpass (12000.0, 0.5412, kSr); h2.setHighpass (12000.0, 1.3066, kSr);
    std::vector<double> hp ((size_t) a.size());
    for (int i = 0; i < a.size(); ++i) hp[(size_t) i] = h2.process (h1.process (0.5 * (a.L[(size_t) i] + a.R[(size_t) i])));
    auto win = [&] (double t) { double s = 0; for (int i = sec (t); i < sec (t + 0.002); ++i) s += hp[(size_t) i] * hp[(size_t) i]; return s; };
    std::vector<double> rest;
    for (double t = 0.3; t < 3.9; t += 0.002) if (std::abs (t - 1.0) > 0.05 && std::abs (t - 2.5) > 0.05) rest.push_back (win (t));
    std::sort (rest.begin(), rest.end());
    const double median = std::max (rest[rest.size() / 2], 1e-20);
    double around = 0, aroundAt = 0;
    for (double t : { 1.0, 2.5 }) for (double u = t - 0.04; u < t + 0.04; u += 0.002) if (win (u) > around) { around = win (u); aroundAt = u; }
    const double loudestElsewhere = rest.back();
    KCHECK (around < 1.5 * loudestElsewhere, "Slow <-> Fast switch: loudest high-band window around the switches %.1f x the median, elsewhere %.1f x (no click)",
            around / median, loudestElsewhere / median);
    if (around >= 1.5 * loudestElsewhere)   // diagnostics
    {
        std::printf ("     loudest window at %.3f s; windows (x median):", aroundAt);
        for (double t = 0.30; t < 3.9; t += 0.1) std::printf (" %.2f:%.1f", t, win (t) / median);
        std::printf ("\n");
    }
}

// ---- mono compatibility ---------------------------------------------------------------------------
// For every cabinet x mic layout at Fast: in 1/3-octave bands (125 Hz .. 8 kHz) with signal, the mono
// sum's power relative to the stereo average power. No band may lose more than 9 dB in mono.
void testMonoCompatibility()
{
    const double centres[] { 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000 };
    double worstAll = 0;
    int worstModel = -1, worstLayout = -1;
    for (int model = 0; model < 4; ++model)
        for (int layout = 0; layout < 5; ++layout)
        {
            auto v = cabValues();
            const auto& M = cabinetModel (model);
            v[cabModel] = (float) model; v[rotorConfig] = (float) M.rotorConfig; v[drumBaffle] = (float) M.drumBaffle;
            v[micLayout] = (float) layout; v[speed] = 2; v[micRoom] = 0.5f;
            setBars (v, 0, 1, "888888888");
            setBars (v, 1, 1, "008080800");
            v[pd16] = 6; v[pd8] = 4;
            const auto a = render (v, { key (0.02, 24), key (0.02, 28), key (0.02, 31), key (0.02, 12, 1), key (0.02, 16, 1), key (0.02, 0, 2) }, 2.5);
            double worst = 0, maxBand = 0;
            std::vector<std::pair<double, double>> bands;
            for (const double fc : centres)
            {
                Biquad bl, br, bm;
                auto setBp = [&] (Biquad& b)
                {
                    const double w = kTwoPi * fc / kSr, alpha = std::sin (w) / (2.0 * 4.3), cw = std::cos (w), a0 = 1.0 + alpha;
                    b.b0 = alpha / a0; b.b1 = 0; b.b2 = -alpha / a0; b.a1 = -2.0 * cw / a0; b.a2 = (1.0 - alpha) / a0;
                };
                setBp (bl); setBp (br); setBp (bm);
                double pl = 0, pr = 0, pm = 0;
                for (int i = 0; i < a.size(); ++i)
                {
                    const double l = bl.process (a.L[(size_t) i]), r = br.process (a.R[(size_t) i]);
                    const double mm = bm.process (0.5 * (a.L[(size_t) i] + a.R[(size_t) i]));
                    if (i < sec (0.5)) continue;
                    pl += l * l; pr += r * r; pm += mm * mm;
                }
                bands.push_back ({ 0.5 * (pl + pr), pm });
                maxBand = std::max (maxBand, 0.5 * (pl + pr));
            }
            for (const auto& [avg, m] : bands)
                if (avg > maxBand * 1e-4) worst = std::min (worst, 10.0 * std::log10 (std::max (m, 1e-30) / avg));
            if (worst < worstAll) { worstAll = worst; worstModel = model; worstLayout = layout; }
        }
    KCHECK (worstAll > -9.0, "mono sum, 4 cabinets x 5 mic layouts at Fast (room mic on): worst band %.1f dB (cabinet %d, layout %d)",
            worstAll, worstModel, worstLayout);
}

// ---- the tube amp: aliasing, latency ------------------------------------------------------------
// Inharmonic energy below 18 kHz of a 1046.5 Hz sine (C6) driven hard into the shaper, relative to
// the output, at 1x / 2x / 4x.
double ampAliasDb (int factor, double amplitude)
{
    OversampledShaper s;
    s.setBias (0.2);
    const double f0 = 1046.5;
    Audio a;
    const int n = sec (1.2);
    a.L.resize ((size_t) n); a.R.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double y = s.process (amplitude * std::sin (kTwoPi * f0 * i / kSr), factor);
        a.L[(size_t) i] = (float) y; a.R[(size_t) i] = (float) y;
    }
    // remove the harmonics (and DC) by fitting them, then measure what is left below 18 kHz
    const int from = sec (0.2), len = sec (1.0);
    std::vector<double> res ((size_t) len);
    for (int i = 0; i < len; ++i) res[(size_t) i] = a.L[(size_t) (from + i)];
    double total = 0;
    for (double x : res) total += x * x;
    for (int k = 0; k * f0 < 23900.0; ++k)
    {
        double c = 0, sn = 0, cc = 0, ss = 0;
        for (int i = 0; i < len; ++i)
        {
            const double ph = kTwoPi * k * f0 * (from + i) / kSr;
            c += res[(size_t) i] * std::cos (ph); sn += res[(size_t) i] * std::sin (ph);
            cc += std::cos (ph) * std::cos (ph); ss += std::sin (ph) * std::sin (ph);
        }
        const double ac = c / cc, as = ss > 0 ? sn / ss : 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double ph = kTwoPi * k * f0 * (from + i) / kSr;
            res[(size_t) i] -= ac * std::cos (ph) + as * std::sin (ph);
        }
    }
    // low-pass the residue at 18 kHz (the band that matters), 4th order
    Biquad l1, l2;
    l1.setLowpass (18000.0, 0.5412, kSr); l2.setLowpass (18000.0, 1.3066, kSr);
    double resid = 0;
    for (int i = 0; i < len; ++i) { const double y = l2.process (l1.process (res[(size_t) i])); if (i > 2000) resid += y * y; }
    return 10.0 * std::log10 (resid / total);
}

void testAmp()
{
    const double a1 = ampAliasDb (1, 8.0), a2 = ampAliasDb (2, 8.0), a4 = ampAliasDb (4, 8.0);
    const double m4 = ampAliasDb (4, 2.0);
    KCHECK (a4 < -60.0 && a2 < a1 && a4 < a2 && m4 < -70.0,
            "tube amp aliasing at C6, heavy drive (x8): 1x %.0f dB, 2x %.0f dB, 4x %.0f dB; moderate drive (x2) at 4x %.0f dB", a1, a2, a4, m4);

    // latency: phase delay of a quiet 200 Hz sine through the shaper vs the reported value
    for (const int factor : { 1, 2, 4 })
    {
        OversampledShaper s;
        s.setBias (0.2);
        const double f = 200.0;
        double re = 0, im = 0;
        for (int i = 0; i < sec (1.0); ++i)
        {
            const double y = s.process (0.001 * std::sin (kTwoPi * f * i / kSr), factor);
            if (i < sec (0.2)) continue;
            re += y * std::cos (kTwoPi * f * i / kSr); im += y * std::sin (kTwoPi * f * i / kSr);
        }
        const double phase = std::atan2 (-re, im);   // output = A sin (w t - phase)
        const double delay = phase / (kTwoPi * f) * kSr;
        KCHECK (std::abs (delay - OversampledShaper::latency (factor)) < 0.6, "amp %dx: measured delay %.2f samples, reported %d",
                factor, delay, OversampledShaper::latency (factor));
    }
}

// ---- outboard, reverb ------------------------------------------------------------------------------
void testOutboardAndReverb()
{
    auto v = cabValues();
    v[cabBypass] = 1;
    // overdrive adds harmonics to a sine
    auto od = v; od[odOn] = 1; od[odDrive] = 0.8f;
    const auto a = render (v, { key (0.02, 33) }, 0.8), b = render (od, { key (0.02, 33) }, 0.8);
    const double h3a = toneAmplitude (a, sec (0.3), sec (0.4), 1320) / toneAmplitude (a, sec (0.3), sec (0.4), 440);
    const double h3b = toneAmplitude (b, sec (0.3), sec (0.4), 1320) / toneAmplitude (b, sec (0.3), sec (0.4), 440);
    KCHECK (db (h3b) > -30.0 && db (h3a) < -80.0, "overdrive: 3rd harmonic %.0f dB (clean %.0f dB)", db (h3b), db (h3a));

    // wah: the centroid of a full chord follows the pedal
    auto w0 = v; w0[wahOn] = 1; w0[wahPos] = 0.1f; setBars (w0, 0, 1, "888888888");
    auto w1 = w0; w1[wahPos] = 0.9f;
    const std::vector<Event> chord { key (0.02, 12), key (0.02, 24), key (0.02, 31) };
    const double c0 = spectralCentroid (render (w0, chord, 0.6), sec (0.3), 9600), c1 = spectralCentroid (render (w1, chord, 0.6), sec (0.3), 9600);
    KCHECK (c1 > 1.5 * c0, "wah: centroid %.0f Hz heel down, %.0f Hz toe down", c0, c1);

    // tape echo: a short note repeats after the echo time, each repeat ~ feedback x the last
    auto e = v; e[echoOn] = 1; e[echoTime] = 0.25f; e[echoFb] = 0.5f; e[echoMix] = 1.0f; e[echoWow] = 0;
    const auto x = render (e, { key (0.02, 33), { sec (0.07), Event::KeyOff, 33, 0.0, 0 } }, 1.0);
    const double r1 = rms (x, sec (0.30), sec (0.32)), r2 = rms (x, sec (0.55), sec (0.57)), gap = rms (x, sec (0.15), sec (0.25));
    KCHECK (r1 > 10.0 * gap && std::abs (db (r2 / r1) - db (0.5)) < 4.0, "tape echo: repeat at 0.25 s (%.0f dB above the gap), next repeat %.1f dB (feedback 0.5 = %.1f)",
            db (r1 / std::max (gap, 1e-12)), db (r2 / r1), db (0.5));

    // spring reverb: a tail that decays in about the Decay time, different left / right
    auto rv = v; rv[revOn] = 1; rv[revMix] = 1.0f; rv[revDecay] = 1.5f;
    const auto y = render (rv, { key (0.02, 33), { sec (0.3), Event::KeyOff, 33, 0.0, 0 } }, 3.0);
    const double t0 = rms (y, sec (0.40), sec (0.50)), t1 = rms (y, sec (0.40 + 0.75), sec (0.50 + 0.75));
    double num = 0, dl = 0, dr = 0;
    for (int i = sec (0.4); i < sec (1.2); ++i) { num += y.L[(size_t) i] * y.R[(size_t) i]; dl += y.L[(size_t) i] * y.L[(size_t) i]; dr += y.R[(size_t) i] * y.R[(size_t) i]; }
    KCHECK (t0 > 1e-4 && std::abs (db (t1 / t0) + 30.0) < 8.0 && num / std::sqrt (dl * dr) < 0.6,
            "spring reverb: tail %.1f dB half a Decay later (-30 expected), L/R correlation %.2f", db (t1 / t0), num / std::sqrt (dl * dr));
}

// ---- organ models -------------------------------------------------------------------------------------
void testOrganModels()
{
    auto v = cabValues();
    v[cabBypass] = 1;
    auto level = [] (Values x, int k, int manual) { return rms (render (x, { key (0.02, k, manual) }, 0.4), sec (0.2), sec (0.4)); };
    auto m3 = v; m3[organModel] = 3;
    KCHECK (level (m3, 0, 0) == 0.0 && level (m3, 5, 0) > 1e-3 && level (m3, 50, 0) == 0.0 && level (v, 0, 0) > 1e-3,
            "M-3: 44-key manuals (F..C): the lowest C and the top keys are silent, the F sounds");
    auto lo16 = v; setBars (lo16, 1, 1, "800000000");
    auto m3lo = lo16; m3lo[organModel] = 3;
    KCHECK (level (lo16, 20, 1) > 1e-3 && level (m3lo, 20, 1) == 0.0, "M-3: no 16' on the lower manual");
    auto c3 = v; c3[organModel] = 1;
    const auto b = render (v, { key (0.02, 33) }, 0.4), c = render (c3, { key (0.02, 33) }, 0.4);
    bool same = true; for (int i = 0; i < b.size(); ++i) if (b.L[(size_t) i] != c.L[(size_t) i]) same = false;
    auto a100 = v; a100[organModel] = 2;
    const auto t = render (a100, { key (0.02, 33), { sec (0.3), Event::KeyOff, 33, 0.0, 0 } }, 1.0);
    const auto u = render (v, { key (0.02, 33), { sec (0.3), Event::KeyOff, 33, 0.0, 0 } }, 1.0);
    KCHECK (same && rms (t, sec (0.45), sec (0.6)) > 1e-4 && rms (u, sec (0.45), sec (0.6)) < 1e-6,
            "C-3 sounds exactly like the B-3; the A-100's built-in reverb leaves a tail");
}
} // namespace

int runCabinetTests()
{
    fails = 0;
    testRotors();
    testSpeedControls();
    testDopplerAndAm();
    testSpeedChangeClicks();
    testMonoCompatibility();
    testAmp();
    testOutboardAndReverb();
    testOrganModels();
    return fails;
}
