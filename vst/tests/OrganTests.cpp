// Organ checks (phase 1): the tonewheel generator, the wiring and foldback, phase continuity,
// key click, drawbar law, routing, motor. Called from PresetTests.cpp main().
#include "TestRender.h"
#include "CcMap.h"

using namespace ham;
using namespace ham::test;

namespace
{
int fails = 0;
#define OCHECK(cond, ...) do { const bool ok_ = (cond); if (! ok_) ++fails; std::printf ("%-4s ", ok_ ? "ok" : "FAIL"); std::printf (__VA_ARGS__); std::printf ("\n"); } while (0)

Values organValues()
{
    auto v = defaultValues();
    for (int m = 0; m < 2; ++m) for (int s = 0; s < 2; ++s) setBars (v, m, s, "000000000");
    v[pd16] = 0; v[pd8] = 0;
    return v;
}

// ---- 1. wheel frequencies -------------------------------------------------------
// Independent of the engine's tables: gear ratios typed again from the brief, and the top
// octave's shaft rule from Electric Druid ("C# = 20 * 192 * (74/64) = 4440 Hz").
void testWheelFrequencies()
{
    const int num[12] { 85, 71, 67, 105, 103, 84, 74, 98, 96, 88, 67, 108 };
    const int den[12] { 104, 82, 73, 108, 100, 77, 64, 80, 74, 64, 46, 70 };
    double worstHz = 0, minCents = 1e9, maxCents = -1e9;
    for (int w = 1; w <= kNumWheels; ++w)
    {
        double expect;
        if (w <= 84) expect = 20.0 * std::pow (2.0, (w - 1) / 12 + 1) * num[(w - 1) % 12] / den[(w - 1) % 12];
        else         expect = 20.0 * 192.0 * num[(w - 85 + 5)] / den[(w - 85 + 5)];
        worstHz = std::max (worstHz, std::abs (wheelBaseHz (w) - expect));
        const double et = 440.0 * std::pow (2.0, (w - 46) / 12.0);
        const double cents = 1200.0 * std::log2 (wheelBaseHz (w) / et);
        minCents = std::min (minCents, cents); maxCents = std::max (maxCents, cents);
    }
    OCHECK (worstHz < 1e-9, "91 wheel frequencies = gear formula (worst difference %.2e Hz)", worstHz);
    OCHECK (wheelBaseHz (46) == 440.0, "wheel 46 = %.6f Hz (A = 440)", wheelBaseHz (46));
    OCHECK (std::abs (wheelBaseHz (1) - 32.6923) < 1e-4 && std::abs (wheelBaseHz (13) - 65.3846) < 1e-4
            && std::abs (wheelBaseHz (73) - 2092.3077) < 1e-4 && std::abs (wheelBaseHz (86) - 4440.0) < 1e-9,
            "published values: wheel 1 %.4f, 13 %.4f (lowest manual C), 73 %.4f (highest manual C), 86 %.1f Hz",
            wheelBaseHz (1), wheelBaseHz (13), wheelBaseHz (73), wheelBaseHz (86));
    OCHECK (minCents > -1.0 && maxCents < 2.0 && maxCents - minCents > 1.0,
            "off equal temperament by %.2f .. %.2f cents (not exactly ET, as on the real organ)", minCents, maxCents);
}

// The engine actually plays these frequencies: one key, one drawbar, measured by DFT.
void testPitch()
{
    auto measure = [] (Values v, int key, int foot)
    {
        v[(size_t) drawbarParam (0, 1, foot)] = 8;
        const auto a = render (v, { { 0, Event::Key, key, 100.0, 0 } }, 1.3);
        return peakFrequency (a, sec (0.2), sec (1.0), 20, 6000);
    };
    auto v = organValues();
    // key 33 = A on the upper manual (key 0 = C); 8' = wheel 46
    const double a440 = measure (v, 33, 2);
    OCHECK (std::abs (a440 - 440.0) < 0.02, "upper A, 8': %.3f Hz", a440);
    const double low = measure (v, 0, 0);    // bottom C, 16': folds back to wheel 13
    OCHECK (std::abs (low - wheelBaseHz (13)) < 0.02, "lowest C, 16' plays wheel 13 (folded back): %.3f Hz", low);
    auto v50 = v; v50[mains] = 1;
    const double m50 = measure (v50, 33, 2);
    OCHECK (std::abs (m50 - 440.0 * 5.0 / 6.0) < 0.02, "50 Hz mains: A = %.3f Hz (5/6 speed)", m50);
    auto v450 = v; v450[tuneA] = 450;
    const double t450 = measure (v450, 33, 2);
    OCHECK (std::abs (t450 - 450.0) < 0.02, "tuning A = 450: %.3f Hz", t450);
    auto vt = v; vt[transpose] = 2;
    {
        vt[(size_t) drawbarParam (0, 1, 2)] = 8;
        const auto a = render (vt, { { 0, Event::On, 36 + 31, 100.0, 0 } }, 1.3);   // G + 2 semitones = A
        const double f = peakFrequency (a, sec (0.2), sec (1.0), 20, 6000);
        OCHECK (std::abs (f - 440.0) < 0.02, "transpose +2: MIDI G plays A, %.3f Hz", f);
    }
}

// ---- 2. wiring and foldback ---------------------------------------------------------
// Expected runs per footage, transcribed from Electric Druid's B-3 foldback table (an
// independent source from setBfree's rule that the engine implements). Each run is a
// consecutive wheel range; together they cover keys 0..60 in order.
void testWiring()
{
    struct Run { int from, to; };
    const std::vector<std::vector<Run>> expected {
        { { 13, 24 }, { 13, 61 } },     // 16'    bottom octave folded up
        { { 20, 80 } },                 // 5 1/3'
        { { 13, 73 } },                 // 8'
        { { 25, 85 } },                 // 4'
        { { 32, 91 }, { 80, 80 } },     // 2 2/3' repeats the highest C
        { { 37, 91 }, { 80, 85 } },     // 2'     repeats from key 56 (1-based)
        { { 41, 91 }, { 80, 89 } },     // 1 3/5' repeats from key 52
        { { 44, 91 }, { 80, 91 }, { 80, 80 } },   // 1 1/3' repeats from key 49; the top C folds twice
                                                  // (wheel 104 -> 80; the extracted table omits this key)
        { { 49, 91 }, { 80, 91 }, { 80, 85 } },   // 1' repeats from key 44, twice
    };
    auto engine = std::make_unique<SynthEngine>();
    engine->prepare (kSr);
    int wrong = 0, folded = 0;
    for (int f = 0; f < kNumFootages; ++f)
    {
        std::vector<int> want;
        for (const auto& r : expected[(size_t) f]) for (int w = r.from; w <= r.to; ++w) want.push_back (w);
        if ((int) want.size() != kManualKeys) { ++wrong; std::printf ("     table for %s has %d keys\n", footageName (f).toRawUTF8(), (int) want.size()); continue; }
        for (int k = 0; k < kManualKeys; ++k)
            for (int m = 0; m < 2; ++m)
            {
                const auto& c = engine->wiringOf (m, k).c[(size_t) f];
                if (c.wheel != want[(size_t) k] || c.bus != f)
                {
                    if (++wrong <= 5) std::printf ("     manual %d key %d %s: wheel %d, expected %d\n", m, k, footageName (f).toRawUTF8(), c.wheel, want[(size_t) k]);
                }
                else if (m == 0 && c.wheel != k + 13 + kFootageOffset[f]) ++folded;
            }
    }
    OCHECK (wrong == 0, "every key x footage on both manuals matches the foldback chart (%d folded contacts per manual)", folded);

    // pedals: no foldback, wheels 1-12 (the complex low wheels) reachable only from here
    bool lowOnlyPedals = true;
    for (int k = 0; k < kManualKeys; ++k)
        for (int f = 0; f < kNumFootages; ++f)
            if (engine->wiringOf (0, k).c[(size_t) f].wheel < 13) lowOnlyPedals = false;
    OCHECK (lowOnlyPedals && engine->wiringOf (2, 0).c[0].wheel == 1 && engine->wiringOf (2, 24).c[0].wheel == 25,
            "pedal 16' plays wheels 1..25; manuals never use wheels 1-12");

    // taper: steps of 3.5 dB within -10 .. +7 dB
    bool taperOk = true;
    for (int k = 0; k < kManualKeys; ++k)
        for (int f = 0; f < kNumFootages; ++f)
        {
            const double t = b3TaperDb (k, f);
            if (t < -10.0 || t > 7.0) taperOk = false;
        }
    OCHECK (taperOk && b3TaperDb (0, 0) == -10.0 && b3TaperDb (30, 2) == 0.0 && b3TaperDb (60, 8) == -7.0, "key taper within -10 .. +7 dB");
}

// ---- 3. phase continuity -----------------------------------------------------------
// The wheels never reset. Render A presses a key, releases it and presses it again; render B
// only presses it the second time. Once the contacts have settled, A and B must be identical
// sample for sample: the second press joins the wheel wherever it already was.
void testPhaseContinuity()
{
    auto v = organValues();
    setBars (v, 0, 1, "888000000");
    const auto a = render (v, { { sec (0.05), Event::Key, 30, 100.0, 0 }, { sec (0.40), Event::KeyOff, 30, 0, 0 },
                                { sec (0.80), Event::Key, 30, 100.0, 0 } }, 1.2);
    const auto b = render (v, { { sec (0.80), Event::Key, 30, 100.0, 0 } }, 1.2);
    double diff = 0;
    for (int i = sec (0.85); i < sec (1.2); ++i) diff = std::max (diff, (double) std::abs (a.L[(size_t) i] - b.L[(size_t) i]));
    OCHECK (diff == 0.0 && rms (b, sec (0.85), sec (1.2)) > 1e-3,
            "a key press never resets a wheel: re-pressed key identical to a first press (max difference %.1e)", diff);

    // a whole chord pressed at different moments lands on the same waveform as pressed together
    const auto c = render (v, { { sec (0.10), Event::Key, 24, 100.0, 0 }, { sec (0.23), Event::Key, 28, 100.0, 0 },
                                { sec (0.37), Event::Key, 31, 100.0, 0 } }, 1.0);
    const auto d = render (v, { { sec (0.40), Event::Key, 24, 100.0, 0 }, { sec (0.40), Event::Key, 28, 100.0, 0 },
                                { sec (0.40), Event::Key, 31, 100.0, 0 } }, 1.0);
    double diff2 = 0;
    for (int i = sec (0.5); i < sec (1.0); ++i) diff2 = std::max (diff2, (double) std::abs (c.L[(size_t) i] - d.L[(size_t) i]));
    OCHECK (diff2 == 0.0, "chord: staggered and simultaneous presses give the same steady waveform (%.1e)", diff2);
}

// ---- 4. key click ------------------------------------------------------------------
void testKeyClick()
{
    auto v = organValues();
    setBars (v, 0, 1, "008000000");   // one low 8' sine: anything above it in the first ms is click
    auto onset = [] (const Values& vv, int key)
    {
        const auto a = render (vv, { { sec (0.1), Event::Key, key, 100.0, 0 } }, 0.4);
        // high-band energy right at the attack vs. the same length of steady tone
        return std::make_pair (hfEnergy (a, sec (0.1), sec (0.11)), hfEnergy (a, sec (0.3), sec (0.31)));
    };
    auto hard = v; hard[clickUp] = 1.0f;
    auto none = v; none[clickUp] = 0.0f;
    const auto h = onset (hard, 12), z = onset (none, 12);
    OCHECK (h.first > 20.0 * h.second, "click 100%%: attack high band %.1f dB above the steady tone", 10 * std::log10 (h.first / h.second));
    OCHECK (z.first < 4.0 * z.second, "click 0%%: attack high band %.1f dB above the steady tone (ramped contacts)", 10 * std::log10 (z.first / z.second));
    OCHECK (h.first > 10.0 * z.first, "click 100%% vs 0%%: %.1f dB more click", 10 * std::log10 (h.first / z.first));

    // the contacts of one key close at different moments (and bounce): the busbar level steps
    // several times during the first few ms instead of once
    {
        auto vv = organValues();
        setBars (vv, 0, 1, "888888888");
        vv[clickUp] = 1.0f;
        auto e = std::make_unique<SynthEngine>();
        e->setParams (makeEngineParams ([&vv] (int i) { return vv[(size_t) i]; }));
        e->prepare (kSr);
        std::vector<float> buf (64);
        e->render (buf.data(), nullptr, 64);
        e->keyOn (0, 30, 100);
        const int scheduled = e->pendingEvents();
        e->render (buf.data(), nullptr, 64);
        const int afterOneMs = e->pendingEvents();
        for (int i = 0; i < 10; ++i) e->render (buf.data(), nullptr, 64);
        OCHECK (scheduled > 9 && afterOneMs > 0 && afterOneMs < scheduled && e->pendingEvents() == 0,
                "contacts: %d contact events for 9 contacts, %d still pending after 1.3 ms, none after 15 ms", scheduled, afterOneMs);
    }

    // key-up click switch
    auto rel = [] (Values vv)
    {
        const auto a = render (vv, { { sec (0.05), Event::Key, 12, 100.0, 0 }, { sec (0.3), Event::KeyOff, 12, 0, 0 } }, 0.5);
        return hfEnergy (a, sec (0.3), sec (0.31)) / std::max (1e-30, hfEnergy (a, sec (0.2), sec (0.21)));
    };
    auto upOn = hard; upOn[keyUpClick] = 1;
    auto upOff = hard; upOff[keyUpClick] = 0;
    const double rOn = rel (upOn), rOff = rel (upOff);
    OCHECK (rOn > 10.0 * rOff, "key-up click on: release %.1f dB more high band than with it off", 10 * std::log10 (rOn / rOff));
}

// ---- 5. drawbars ---------------------------------------------------------------------
void testDrawbars()
{
    auto level = [] (const char* reg, int key = 30)
    {
        auto v = organValues();
        setBars (v, 0, 1, reg);
        const auto a = render (v, { { 0, Event::Key, key, 100.0, 0 } }, 0.6);
        return rms (a, sec (0.2), sec (0.6));
    };
    const double l8 = level ("008000000"), l5 = level ("005000000"), l1 = level ("001000000"), l0 = level ("000000000");
    OCHECK (std::abs (db (l5 / l8) + 9.0) < 0.05, "drawbar 8 -> 5: %.2f dB (3 dB per step)", db (l5 / l8));
    OCHECK (std::abs (db (l1 / l8) + 21.0) < 0.05 && l0 == 0.0, "drawbar 1: %.2f dB, drawbar 0: silent", db (l1 / l8));
    // 888000000 = three wheels in octaves/fifth: louder than one
    OCHECK (level ("888000000") > 1.3 * l8, "three drawbars sum");

    // preset keys: cancel = silent, set A vs set B, a fixed registration sounds
    auto v = organValues();
    setBars (v, 0, 0, "008000000");
    setBars (v, 0, 1, "000000000");
    auto keyed = [&v] (int pk) { auto vv = v; vv[upKey] = (float) pk; return rms (render (vv, { { 0, Event::Key, 30, 100.0, 0 } }, 0.5), sec (0.2), sec (0.5)); };
    OCHECK (keyed (0) == 0.0 && keyed (10) > 0.01 && keyed (11) == 0.0 && keyed (1) > 0.01,
            "preset keys: C cancels, A# plays set A, B plays set B, C# plays its fixed registration");

    // a drawbar moved while a key is held reaches its new level within one 32-sample chunk
    {
        auto vv = organValues();
        setBars (vv, 0, 1, "008000000");
        const auto a = render (vv, { { 0, Event::Key, 12, 100.0, 0 } }, 0.6, [] (int start, EngineParams& p)
        {
            if (start >= sec (0.3)) p.bars[0][1][2] = 5.0;
        });
        OCHECK (std::abs (db (rms (a, sec (0.4), sec (0.6)) / rms (a, sec (0.1), sec (0.3))) + 9.0) < 0.05,
                "drawbar moved 8 -> 5 while a key is held: -9 dB");
    }
}

// ---- 6. MIDI routing -----------------------------------------------------------------
void testRouting()
{
    auto v = organValues();
    setBars (v, 0, 1, "008000000");
    setBars (v, 1, 1, "000080000");    // lower: 2 2/3' only, so the two manuals have different pitches
    v[pd16] = 8;
    auto pitchOf = [] (const Values& vv, std::vector<Event> ev)
    {
        const auto a = render (vv, ev, 1.2);
        return rms (a, sec (0.2), sec (1.1)) > 1e-4 ? peakFrequency (a, sec (0.2), sec (0.9), 20, 3000) : 0.0;
    };
    const double up = pitchOf (v, { { 0, Event::On, 69, 100.0, 0 } });   // ch 1 A4
    const double lo = pitchOf (v, { { 0, Event::On, 69, 100.0, 1 } });   // ch 2 A4 -> 2 2/3' = E6
    const double pd = pitchOf (v, { { 0, Event::On, 36, 100.0, 2 } });   // ch 3 pedal C -> 16' wheel 1
    OCHECK (std::abs (up - 440.0) < 0.05 && std::abs (lo - wheelBaseHz (46 + 19)) < 0.05 && std::abs (pd - wheelBaseHz (1)) < 0.05,
            "channels: upper %.2f Hz, lower %.2f Hz (2 2/3'), pedal %.2f Hz", up, lo, pd);

    auto s = v; s[splitMode] = 1; s[splitPoint] = 60; s[pedalZone] = 1; s[pedalSplit] = 48; s[pedMidiBase] = 36;
    const double sUp = pitchOf (s, { { 0, Event::On, 69, 100.0, 0 } });
    const double sLo = pitchOf (s, { { 0, Event::On, 57, 100.0, 0 } });   // A3 below the split -> lower
    const double sPd = pitchOf (s, { { 0, Event::On, 36, 100.0, 0 } });   // C2 in the pedal zone
    OCHECK (std::abs (sUp - 440.0) < 0.05 && std::abs (sLo - wheelBaseHz (34 + 19)) < 0.05 && std::abs (sPd - wheelBaseHz (1)) < 0.05,
            "split on channel 1: upper %.2f, lower %.2f, pedal zone %.2f Hz", sUp, sLo, sPd);
    OCHECK (pitchOf (s, { { 0, Event::On, 69, 100.0, 1 } }) == 0.0, "split mode ignores the other channels");

    // note-off releases the key it pressed even if the transpose changed meanwhile
    {
        auto e = std::make_unique<SynthEngine>();
        auto p = makeEngineParams ([&v] (int i) { return v[(size_t) i]; });
        e->setParams (p); e->prepare (kSr);
        e->noteOn (60, 100, 0);
        p.transpose = 5; e->setParams (p);
        e->noteOff (60, 0);
        OCHECK (! e->keyDown (0, 24), "note-off releases the original key after a transpose change");
    }
}

// ---- 7. motor ----------------------------------------------------------------------
void testMotor()
{
    auto v = organValues();
    setBars (v, 0, 1, "008000000");
    // power off at 0.5 s: the pitch dives and the level falls with it
    const auto a = render (v, { { 0, Event::Key, 33, 100.0, 0 } }, 3.0, [] (int start, EngineParams& p)
    {
        if (start >= sec (0.5)) { p.motorStart = false; p.motorRun = false; }
    });
    const double f1 = peakFrequency (a, sec (0.1), sec (0.35), 100, 600, 0.05);
    const double f2 = peakFrequency (a, sec (1.0), sec (0.25), 50, 600, 0.05);
    OCHECK (std::abs (f1 - 440.0) < 0.2 && f2 < 0.9 * f1 && rms (a, sec (1.0), sec (1.25)) < 0.7 * rms (a, sec (0.1), sec (0.35)),
            "motor off: pitch dives (%.1f -> %.1f Hz) and the level falls", f1, f2);
    // start from rest: Run alone can't start it; Start brings it up to speed within about a second
    {
        auto e = std::make_unique<SynthEngine>();
        auto p = makeEngineParams ([&v] (int i) { return v[(size_t) i]; });
        p.motorStart = false; p.motorRun = false;
        e->setParams (p); e->prepare (kSr);
        std::vector<float> buf (480);
        for (int i = 0; i < 2000; ++i) e->render (buf.data(), nullptr, 480);   // 20 s: stopped
        const double stopped = e->motorValue();
        p.motorRun = true; e->setParams (p);
        for (int i = 0; i < 100; ++i) e->render (buf.data(), nullptr, 480);
        const double runOnly = e->motorValue();
        p.motorStart = true; e->setParams (p);
        for (int i = 0; i < 100; ++i) e->render (buf.data(), nullptr, 480);   // 1 s
        OCHECK (stopped == 0.0 && runOnly == 0.0 && e->motorValue() > 0.98,
                "motor: Run alone can't start it (%.2f); Start brings it to %.3f of speed in 1 s", runOnly, e->motorValue());
    }
    // motor CC
    auto vc = v; vc[motorCC] = 20;
    const auto c = render (vc, { { 0, Event::Key, 33, 100.0, 0 }, { sec (0.3), Event::CC, 20, 1.0, 0 } }, 3.5);
    OCHECK (rms (c, sec (3.3), sec (3.5)) < 0.3 * rms (c, sec (0.1), sec (0.3)), "motor-off CC triggers the pitch dive");
}

// ---- 8. glissando / stress -------------------------------------------------------------
void testGlissando()
{
    auto v = organValues();
    setBars (v, 0, 1, "888888888");
    std::vector<Event> ev;
    // fast chromatic run up and white-key smear down, 8 ms per key, overlapping
    for (int k = 0; k < 61; ++k) { ev.push_back ({ sec (0.05) + k * sec (0.008), Event::Key, k, 100.0, 0 }); ev.push_back ({ sec (0.07) + k * sec (0.008), Event::KeyOff, k, 0, 0 }); }
    const int white[] { 0, 2, 4, 5, 7, 9, 11 };
    int t = sec (0.8);
    for (int o = 4; o >= 0; --o)
        for (int i = 6; i >= 0; --i) { const int k = o * 12 + white[i]; ev.push_back ({ t, Event::Key, k, 100.0, 0 }); ev.push_back ({ t + sec (0.015), Event::KeyOff, k, 0, 0 }); t += sec (0.006); }
    const auto a = render (v, ev, 2.0);
    bool finite = true;
    for (int i = 0; i < a.size(); ++i) if (! std::isfinite (a.L[(size_t) i])) finite = false;
    OCHECK (finite && rms (a, sec (1.6), sec (2.0)) == 0.0 && peak (a) < 0.99,
            "glissandos (chromatic up, white-key smear down): finite, all keys released to silence, peak %.2f", peak (a));
}

// ---- 9. CC map -------------------------------------------------------------------------
void testCcMaps()
{
    OCHECK (drawbarForCC (1, 12).manual == 0 && drawbarForCC (1, 12).foot == 0 && drawbarForCC (1, 29).manual == 1 && drawbarForCC (1, 29).foot == 8
            && drawbarForCC (2, 24).foot == 8 && drawbarForCC (2, 70).manual == 1 && drawbarForCC (3, 70).manual == 0
            && drawbarForCC (0, 12).manual < 0 && drawbarForCC (1, 11).manual < 0 && drawbarForCC (1, 30).manual < 0,
            "drawbar CC maps: B4D 12-20 / 21-29, Nord 16-24 / 70-78, VK 70-78, Off");
}
} // namespace

int runOrganTests()
{
    fails = 0;
    testWheelFrequencies();
    testPitch();
    testWiring();
    testPhaseContinuity();
    testKeyClick();
    testDrawbars();
    testRouting();
    testMotor();
    testGlissando();
    testCcMaps();
    return fails;
}
