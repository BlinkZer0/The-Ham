// Organ voice checks (phase 2): percussion, scanner vibrato / chorus, expression, leakage, Age,
// sustain, manual bass, preset-key registrations and keyswitches. Called from PresetTests.cpp main().
#include "TestRender.h"
#include "CcMap.h"

using namespace ham;
using namespace ham::test;

namespace
{
int fails = 0;
#define VCHECK(cond, ...) do { const bool ok_ = (cond); if (! ok_) ++fails; std::printf ("%-4s ", ok_ ? "ok" : "FAIL"); std::printf (__VA_ARGS__); std::printf ("\n"); } while (0)

// a clean organ: no drawbars, no leakage, no wear, no click
Values cleanValues()
{
    auto v = defaultValues();
    for (int m = 0; m < 2; ++m) for (int s = 0; s < 2; ++s) setBars (v, m, s, "000000000");
    v[pd16] = 0; v[pd8] = 0;
    v[leakUp] = v[leakLo] = v[ageUp] = v[ageLo] = 0;
    v[clickUp] = v[clickLo] = v[clickPd] = 0;
    v[cabBypass] = 1;   // the voice is measured dry (no cabinet)
    return v;
}

Event key (double t, int k, int manual = 0) { return { sec (t), Event::Key, k, 100.0, manual }; }
Event keyOff (double t, int k, int manual = 0) { return { sec (t), Event::KeyOff, k, 0.0, manual }; }

// ---- percussion -----------------------------------------------------------------
void testPercussion()
{
    auto v = cleanValues();
    v[percOn] = 1; v[percVol] = 0; v[percHarm] = 1; v[percDecay] = 0;   // Normal, Third, Fast

    // decay: -60 dB over the decay time (exponential: -30 dB at half of it)
    for (const int slow : { 0, 1 })
    {
        auto vv = v; vv[percDecay] = (float) slow;
        const double T = slow ? vv[percSlow] : vv[percFast];
        const auto a = render (vv, { key (0.1, 33) }, 0.2 + T);
        const double l0 = rms (a, sec (0.11), sec (0.13)), l1 = rms (a, sec (0.11 + T / 2), sec (0.13 + T / 2));
        VCHECK (std::abs (db (l1 / l0) + 30.0) < 1.0, "percussion %s: %.1f dB after half of %.2f s (expect -30: -60 dB over the decay time)",
                slow ? "slow" : "fast", db (l1 / l0), T);
    }

    // pitch: Third = 2 2/3' (wheel of the 8' + 19), Second = 4' (+ 12)
    {
        const auto a = render (v, { key (0.05, 33) }, 0.3);
        auto v2 = v; v2[percHarm] = 0;
        const auto b = render (v2, { key (0.05, 33) }, 0.3);
        const double f3 = peakFrequency (a, sec (0.06), sec (0.1), 100, 2000), f2 = peakFrequency (b, sec (0.06), sec (0.1), 100, 2000);
        VCHECK (std::abs (f3 - wheelBaseHz (46 + 19)) < 1.0 && std::abs (f2 - wheelBaseHz (46 + 12)) < 1.0,
                "percussion third = %.1f Hz (2 2/3'), second = %.1f Hz (4')", f3, f2);
    }

    // single trigger: a legato note doesn't retrigger; after every key is up, the next one does
    {
        const auto a = render (v, { key (0.1, 33), key (0.6, 40), keyOff (1.0, 33), keyOff (1.0, 40), key (1.2, 45) }, 1.5);
        const double first = rms (a, sec (0.11), sec (0.15)), legato = rms (a, sec (0.62), sec (0.66)), again = rms (a, sec (1.21), sec (1.25));
        VCHECK (first > 0.01 && legato < first * 1e-3 && again > 0.5 * first,
                "single trigger: first %.3f, legato note %.1e (no retrigger), after release %.3f", first, legato, again);
    }

    // only on drawbar set B
    {
        auto vA = v; vA[upKey] = 10;
        VCHECK (rms (render (vA, { key (0.05, 33) }, 0.2), sec (0.06), sec (0.1)) == 0.0, "percussion silent on drawbar set A");
    }

    // it takes the 1' busbar; at Normal it lowers the drawbars (-4.4 dB), at Soft it doesn't
    {
        auto one = v; setBars (one, 0, 1, "000000008");
        auto off = one; off[percOn] = 0;
        const double withPerc = rms (render (one, { key (0.05, 20) }, 1.5), sec (1.2), sec (1.5));
        const double without = rms (render (off, { key (0.05, 20) }, 1.5), sec (1.2), sec (1.5));
        VCHECK (without > 0.005 && withPerc < 1e-6, "percussion on cancels the 1' drawbar (%.3f -> %.1e)", without, withPerc);

        auto full = v; setBars (full, 0, 1, "888000000");
        auto fullOff = full; fullOff[percOn] = 0;
        auto fullSoft = full; fullSoft[percVol] = 1;
        const double lOn = rms (render (full, { key (0.05, 20) }, 1.5), sec (1.2), sec (1.5));
        const double lOff = rms (render (fullOff, { key (0.05, 20) }, 1.5), sec (1.2), sec (1.5));
        const double lSoft = rms (render (fullSoft, { key (0.05, 20) }, 1.5), sec (1.2), sec (1.5));
        VCHECK (std::abs (db (lOn / lOff) + 4.36) < 0.05 && std::abs (db (lSoft / lOff)) < 0.05,
                "percussion Normal lowers the drawbars %.2f dB, Soft %.2f dB", db (lOn / lOff), db (lSoft / lOff));

        auto pSoft = v; pSoft[percVol] = 1;
        const double pn = rms (render (v, { key (0.05, 33) }, 0.2), sec (0.06), sec (0.08));
        const double ps = rms (render (pSoft, { key (0.05, 33) }, 0.2), sec (0.06), sec (0.08));
        VCHECK (std::abs (db (ps / pn) + 6.0) < 0.1, "percussion Soft = %.2f dB", db (ps / pn));
    }
}

// ---- scanner vibrato / chorus ----------------------------------------------------------
// Pitch deviation (cents, half of max - min of the per-period frequency) of a 440 Hz 8' tone.
double vibratoCents (const Values& v)
{
    const auto a = render (v, { key (0.02, 33) }, 1.6);
    const auto f = periodFrequencies (a, sec (0.4), sec (1.6));
    double lo = 1e9, hi = 0;
    for (const double x : f) { lo = std::min (lo, x); hi = std::max (hi, x); }
    return 0.5 * 1200.0 * std::log2 (hi / lo);
}

void testScanner()
{
    auto v = cleanValues();
    setBars (v, 0, 1, "008000000");
    const double off = vibratoCents (v);
    double cents[3] {};
    for (int m = 0; m < 3; ++m)
    {
        auto vv = v; vv[vibUpper] = 1; vv[vibMode] = (float) m;
        cents[m] = vibratoCents (vv);
    }
    VCHECK (off < 0.5, "vibrato off: pitch steady (+-%.2f cents)", off);
    VCHECK (cents[0] < cents[1] && cents[1] < cents[2] && cents[0] > 3.0 && cents[2] < 120.0,
            "vibrato depth V1 < V2 < V3: +-%.1f, +-%.1f, +-%.1f cents", cents[0], cents[1], cents[2]);

    // the rate: 6.87 Hz (frequency of the pitch wobble, from the period sequence)
    {
        auto vv = v; vv[vibUpper] = 1; vv[vibMode] = 2;
        const auto a = render (vv, { key (0.02, 33) }, 2.5);
        const auto f = periodFrequencies (a, sec (0.5), sec (2.5));
        double mean = 0; for (double x : f) mean += x; mean /= (double) f.size();
        int crossings = 0;
        for (size_t i = 1; i < f.size(); ++i) if (f[i - 1] < mean && f[i] >= mean) ++crossings;
        const double rate = crossings / 2.0;
        VCHECK (std::abs (rate - 6.87) < 0.6, "scanner rate: %.1f pitch cycles per second (6.87 Hz)", rate);
    }

    // chorus = vibrato and the straight signal mixed 50:50 (exactly, the path is linear)
    {
        auto vVib = v; vVib[vibUpper] = 1; vVib[vibMode] = 2;
        auto vCh = v;  vCh[vibUpper] = 1;  vCh[vibMode] = 5;
        const std::vector<Event> ev { key (0.02, 33), key (0.02, 37), key (0.02, 40) };
        const auto dry = render (v, ev, 0.6), vib = render (vVib, ev, 0.6), ch = render (vCh, ev, 0.6);
        double worst = 0;
        for (int i = sec (0.1); i < sec (0.6); ++i)
            worst = std::max (worst, std::abs ((double) ch.L[(size_t) i] - 0.5 * ((double) dry.L[(size_t) i] + vib.L[(size_t) i])));
        VCHECK (worst < 1e-6 && vibratoCents (vCh) > 1.0, "chorus C3 = (V3 + dry) / 2 (worst difference %.1e)", worst);
    }

    // per-manual switches: Vibrato Upper doesn't touch the lower manual
    {
        auto vl = cleanValues(); setBars (vl, 1, 1, "008000000");
        auto vlUp = vl; vlUp[vibUpper] = 1;
        auto vlLo = vl; vlLo[vibLower] = 1;
        auto manualCents = [] (const Values& x)
        {
            const auto a = render (x, { key (0.02, 33, 1) }, 1.4);
            const auto f = periodFrequencies (a, sec (0.4), sec (1.4));
            double lo = 1e9, hi = 0; for (double y : f) { lo = std::min (lo, y); hi = std::max (hi, y); }
            return 0.5 * 1200.0 * std::log2 (hi / lo);
        };
        const double withUp = manualCents (vlUp), withLo = manualCents (vlLo);
        VCHECK (withUp < 0.5 && withLo > 3.0, "vibrato switches are per manual (lower with Upper on: +-%.2f, with Lower on: +-%.1f cents)",
                withUp, withLo);
    }
}

// ---- expression ------------------------------------------------------------------------
void testExpression()
{
    auto v = cleanValues();
    setBars (v, 0, 1, "888888888");
    setBars (v, 1, 1, "808000000");
    std::vector<Event> ev { key (0.02, 0, 1), key (0.02, 12, 1), key (0.02, 28), key (0.02, 31), key (0.02, 36) };
    const auto open = render (v, ev, 1.0);
    auto half = ev; half.push_back ({ 0, Event::CC, 11, 0.5, 0 });
    auto closed = ev; closed.push_back ({ 0, Event::CC, 11, 0.0, 0 });
    const auto h = render (v, half, 1.0), c = render (v, closed, 1.0);
    const double lO = rms (open, sec (0.5), sec (1.0)), lH = rms (h, sec (0.5), sec (1.0)), lC = rms (c, sec (0.5), sec (1.0));
    const double cO = spectralCentroid (open, sec (0.5), 9600), cH = spectralCentroid (h, sec (0.5), 9600), cC = spectralCentroid (c, sec (0.5), 9600);
    VCHECK (lH < lO * 0.5 && lC < lH * 0.5 && lC > 0.0, "expression quietens: open %.1f, half %.1f, closed %.1f dB", db (lO), db (lH), db (lC));
    VCHECK (cH < 0.9 * cO && cC < 0.9 * cH, "expression darkens: centroid open %.0f, half %.0f, closed %.0f Hz", cO, cH, cC);
    // bass is cut less than the mids: the pedal note (wheel 1-13) loses less than the upper chord
    const double bassO = toneAmplitude (open, sec (0.5), 9600, wheelBaseHz (13)), bassC = toneAmplitude (c, sec (0.5), 9600, wheelBaseHz (13));
    const double midO = toneAmplitude (open, sec (0.5), 9600, wheelBaseHz (13 + 28 + 12)), midC = toneAmplitude (c, sec (0.5), 9600, wheelBaseHz (13 + 28 + 12));
    VCHECK (db (bassC / bassO) > db (midC / midO) + 6.0, "closed pedal: bass %.1f dB, mids %.1f dB", db (bassC / bassO), db (midC / midO));
}

// ---- leakage ---------------------------------------------------------------------------
void testLeakage()
{
    auto v = cleanValues();
    setBars (v, 0, 1, "008000000");
    // key 33, 8' = wheel 46; its compartment partner is wheel 89 (42-48 <-> 85-91)
    const double fMain = wheelBaseHz (46), fPartner = wheelBaseHz (wheelPartner (46));
    auto lv = v; lv[leakUp] = 1;
    const auto a = render (lv, { key (0.02, 33) }, 0.6), b = render (v, { key (0.02, 33) }, 0.6);
    const double rel = toneAmplitude (a, sec (0.2), sec (0.4), fPartner) / toneAmplitude (a, sec (0.2), sec (0.4), fMain);
    const double relOff = toneAmplitude (b, sec (0.2), sec (0.4), fPartner) / toneAmplitude (b, sec (0.2), sec (0.4), fMain);
    // -30 dB of leakage, on top of the partner wheel's own voicing level
    const double expect = -30.0 + db (wheelLevel (wheelPartner (46)) / wheelLevel (46));
    VCHECK (std::abs (db (rel) - expect) < 1.0 && db (relOff) < -90.0,
            "leakage 100%%: compartment partner (wheel %d) at %.1f dB (expect %.1f); leakage 0: %.0f dB", wheelPartner (46), db (rel), expect, db (relOff));

    // the floor: every wheel bleeds a little, always (no keys held)
    const auto quiet = render (lv, {}, 0.5), none = render (v, {}, 0.5);
    VCHECK (rms (quiet, sec (0.2), sec (0.5)) > 0.0 && db (rms (quiet, sec (0.2), sec (0.5))) < -60.0 && rms (none, 0, sec (0.5)) == 0.0,
            "leakage floor with no keys: %.1f dBFS at 100%%, silent at 0", db (rms (quiet, sec (0.2), sec (0.5))));
}

// ---- Age -------------------------------------------------------------------------------
void testAge()
{
    auto v = cleanValues();
    setBars (v, 0, 1, "008000000");
    auto old = v; old[ageUp] = 1;
    // the 2nd harmonic of a low wheel (key 0, 8' = wheel 13)
    const auto n = render (v, { key (0.02, 0) }, 0.8), o = render (old, { key (0.02, 0) }, 0.8);
    const double f = wheelBaseHz (13);
    const double h2new = toneAmplitude (n, sec (0.2), sec (0.6), 2 * f) / toneAmplitude (n, sec (0.2), sec (0.6), f);
    const double h2old = toneAmplitude (o, sec (0.2), sec (0.6), 2 * f) / toneAmplitude (o, sec (0.2), sec (0.6), f);
    VCHECK (db (h2new) < -90.0 && db (h2old) > -40.0 && db (h2old) < -25.0, "Age adds harmonics: 2nd harmonic new %.0f dB, vintage %.1f dB", db (h2new), db (h2old));

    // uneven levels: 12 keys in a range where the 8' taper is flat (keys 22-33)
    auto spread = [] (const Values& x)
    {
        double lo = 1e9, hi = -1e9;
        for (int k = 22; k <= 33; ++k)
        {
            const double l = db (rms (render (x, { key (0.02, k) }, 0.3), sec (0.15), sec (0.3)));
            lo = std::min (lo, l); hi = std::max (hi, l);
        }
        return hi - lo;
    };
    const double sNew = spread (v), sOld = spread (old);
    VCHECK (sNew < 0.1 && sOld > 1.0, "Age makes levels uneven: key-to-key spread new %.2f dB, vintage %.2f dB", sNew, sOld);
}

// ---- sustain, manual bass ----------------------------------------------------------------
void testSustainAndBass()
{
    auto v = cleanValues();
    v[pd16] = 8;
    auto s = v; s[pedSustain] = 1; s[pedDecay] = 0.6f;
    const auto a = render (s, { key (0.02, 5, 2), keyOff (0.5, 5, 2) }, 1.5);
    const auto b = render (v, { key (0.02, 5, 2), keyOff (0.5, 5, 2) }, 1.5);
    const double held = rms (a, sec (0.3), sec (0.5)), after = rms (a, sec (0.6) - 960, sec (0.6) + 960);
    // without sustain only the pedal tone filter's own tail is left 20 ms after release
    VCHECK (std::abs (db (after / held) + 10.0) < 1.5 && rms (b, sec (0.52), sec (0.6)) < held * 1e-4,
            "pedal sustain: %.1f dB 0.1 s after release (-60 dB over 0.6 s: -10); without it gone (%.0f dB)",
            db (after / held), db (rms (b, sec (0.52), sec (0.6)) / held));

    auto lv = cleanValues(); setBars (lv, 1, 1, "008000000");
    auto ls = lv; ls[loSustain] = 1; ls[loSusTime] = 1.0f;
    const auto c = render (ls, { key (0.02, 24, 1), keyOff (0.5, 24, 1) }, 1.2);
    const double cHeld = rms (c, sec (0.3), sec (0.5)), cAfter = rms (c, sec (0.75) - 960, sec (0.75) + 960);
    VCHECK (std::abs (db (cAfter / cHeld) + 15.0) < 1.5, "lower sustain: %.1f dB 0.25 s after release (-60 dB over 1 s: -15)", db (cAfter / cHeld));

    // manual bass: the lowest lower key plays the pedal
    {
        auto mv = cleanValues(); mv[manualBass] = 1;
        auto e = std::make_unique<SynthEngine>();
        e->setParams (makeEngineParams ([&mv] (int i) { return mv[(size_t) i]; }));
        e->prepare (kSr);
        std::vector<float> buf (64);
        e->render (buf.data(), nullptr, 64);
        e->keyOn (1, 19, 100);
        const bool p19 = e->keyDown (2, 19);
        e->keyOn (1, 7, 100);
        const bool p7 = e->keyDown (2, 7) && ! e->keyDown (2, 19);
        e->keyOn (1, 40, 100);   // higher: the bass stays on 7
        const bool still7 = e->keyDown (2, 7);
        e->keyOff (1, 7);
        const bool back19 = e->keyDown (2, 19) && ! e->keyDown (2, 7);
        e->keyOff (1, 19); e->keyOff (1, 40);
        bool any = false; for (int k = 0; k < 25; ++k) any = any || e->keyDown (2, k);
        e->keyOn (1, 50, 100);   // above the pedals: folded down by octaves (50 -> 14)
        const bool folded = e->keyDown (2, 14);
        VCHECK (p19 && p7 && still7 && back19 && ! any && folded, "manual bass: the lowest lower key plays its pedal (follows, releases, folds)");
    }
}

// ---- registrations, keyswitches, learn ------------------------------------------------------
void testPresetKeysAndControls()
{
    auto v = cleanValues();
    v[upKey] = 1;   // C#
    auto P = makeEngineParams ([&v] (int i) { return v[(size_t) i]; });
    for (int f = 0; f < 9; ++f) P.regs[0][0][f] = f == 2 ? 8 : 0;   // C# = 008000000
    auto P2 = P;
    for (int f = 0; f < 9; ++f) P2.regs[0][0][f] = f == 3 ? 8 : 0;  // C# = 000800000
    auto playWith = [] (EngineParams p)
    {
        auto vv = cleanValues();
        return render (vv, { key (0.02, 33) }, 1.0, [p] (int, EngineParams& q) { q = p; });
    };
    const double f8 = peakFrequency (playWith (P), sec (0.2), sec (0.7), 100, 2000);
    const double f4 = peakFrequency (playWith (P2), sec (0.2), sec (0.7), 100, 2000);
    VCHECK (std::abs (f8 - 440.0) < 0.1 && std::abs (f4 - 880.0) < 0.2, "preset key C# plays its (editable) registration: %.1f / %.1f Hz", f8, f4);

    const auto ks1 = presetKeyswitch (34, 0, true, false, 0, 1), ks2 = presetKeyswitch (25, 1, true, false, 0, 1);
    VCHECK (ks1.manual == 0 && ks1.key == 10 && ks2.manual == 1 && ks2.key == 1 && presetKeyswitch (34, 0, false, false, 0, 1).manual < 0
            && presetKeyswitch (34, 0, true, true, 0, 1).manual < 0 && presetKeyswitch (36, 0, true, false, 0, 1).manual < 0
            && presetKeyswitch (34, 2, true, false, 0, 1).manual < 0,
            "keyswitches: MIDI 24-35 on a manual's channel = its preset keys (off by default and in Split)");
    VCHECK (learnableCC (14) && learnableCC (1) && ! learnableCC (0) && ! learnableCC (64) && ! learnableCC (123), "MIDI learn takes ordinary CCs only");
}
} // namespace

int runVoiceTests()
{
    fails = 0;
    testPercussion();
    testScanner();
    testExpression();
    testLeakage();
    testAge();
    testSustainAndBass();
    testPresetKeysAndControls();
    return fails;
}
