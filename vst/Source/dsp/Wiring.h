#pragma once
// ============================================================================
//  Key -> wheel wiring charts (data, built once into tables).
//
//  Each key has up to 9 contacts. A contact connects one wheel to one busbar
//  (drawbar) of its manual, through a tapering resistor (gain).
//
//  B-3 / C-3 manuals (61 keys, key 0 = the lowest C, whose 8' is wheel 13):
//    ideal wheel = key + 13 + offset, offsets by footage
//      16' -12 | 5 1/3' +7 | 8' 0 | 4' +12 | 2 2/3' +19 | 2' +24 | 1 3/5' +28 | 1 1/3' +31 | 1' +36
//    foldback: below wheel 13 -> up an octave (the bottom octave of the 16' plays wheels 13-24;
//    wheels 1-12 are left to the pedals); above wheel 91 -> down an octave (the 1' folds
//    from key 43 (G) up, 1 1/3' from key 48, 1 3/5' from key 51, 2' from key 55, 2 2/3' from key 60).
//    Source: setBfree src/tonegen.c, applyManualDefaults(), generator variant TG_91FB12
//    (its default, "C-based generator, foldback").
//  Key taper: per key range and footage, in steps of 3.5 dB (setBfree taperingModel(),
//    which models the B-3's tapering wires). Reproduced as data below.
//
//  Pedals (25 keys, key 0 = C, 16' = wheel 1): the B-3's two pedal drawbars are composite
//  mixtures. setBfree has no model for that (it gives the pedals 9 plain busbars), so this
//  is The Ham's own approximation, to be tuned by ear in phase 2:
//    16' drawbar = wheel p+1 (16') 1.0 + p+13 (8') 0.35 + p+20 (5 1/3') 0.15
//    8'  drawbar = wheel p+13 (8') 1.0 + p+25 (4') 0.30 + p+32 (2 2/3') 0.12
// ============================================================================
#include "Common.h"
#include "Tonewheels.h"

namespace ham
{
constexpr int kManualKeys = 61;
constexpr int kPedalKeys  = 25;
constexpr int kMaxKeys    = 61;
constexpr int kContacts   = 9;    // contacts per key (max)
constexpr int kBuses      = 9;    // busbars per manual (pedals use 0 = 16', 1 = 8')
constexpr int kManuals    = 3;    // 0 upper, 1 lower, 2 pedals

constexpr int kFootageOffset[9] { -12, 7, 0, 12, 19, 24, 28, 31, 36 };

struct Contact
{
    int wheel = 0;     // 1..91, 0 = no contact
    int bus = 0;       // busbar (drawbar) index
    double gain = 0;   // taper
};

struct KeyWiring
{
    std::array<Contact, kContacts> c {};
    int count = 0;
};

// B-3 manual wiring with foldback. Returns 1..91.
inline int b3ManualWheel (int key, int foot)
{
    int t = key + 13 + kFootageOffset[foot];
    while (t < 13) t += 12;
    while (t > kNumWheels) t -= 12;
    return t;
}

// B-3 key taper in dB (setBfree's taperingModel; key 0..60, foot = busbar 0..8).
inline double b3TaperDb (int key, int foot)
{
    // { first key of the next range, dB } ... the last entry covers the rest of the keyboard
    struct Step { int below; double db; };
    static const Step t16[]  { { 12, -10.0 }, { 17, -7.0 }, { 24, -3.5 }, { 36, 0.0 }, { 48, 3.5 }, { 99, 7.0 } };
    static const Step t513[] { { 15, -3.5 }, { 38, 0.0 }, { 50, 3.5 }, { 99, 7.0 } };
    static const Step t8[]   { { 17, -7.0 }, { 22, -3.5 }, { 37, 0.0 }, { 49, 3.5 }, { 99, 7.0 } };
    static const Step t4[]   { { 17, -3.5 }, { 39, 0.0 }, { 99, -3.5 } };
    static const Step t223[] { { 14, 7.0 }, { 20, 3.5 }, { 40, 0.0 }, { 50, -3.5 }, { 99, -7.0 } };
    static const Step t2[]   { { 12, 7.0 }, { 15, 3.5 }, { 41, 0.0 }, { 54, -3.5 }, { 99, -7.0 } };
    static const Step t135[] { { 14, 3.5 }, { 42, 0.0 }, { 50, -3.5 }, { 99, -7.0 } };
    static const Step t113[] { { 43, 0.0 }, { 48, -3.5 }, { 99, -7.0 } };
    static const Step t1[]   { { 43, 0.0 }, { 99, -7.0 } };
    static const Step* tables[9] { t16, t513, t8, t4, t223, t2, t135, t113, t1 };
    for (const Step* s = tables[foot];; ++s)
        if (key < s->below) return s->db;
}

// Organ models: 0 B-3, 1 C-3 (the same instrument in a churchier case), 2 A-100 (the B-3
// voice with a built-in amp, speakers and reverb), 3 M-3 spinet.
//
// M-3: two 44-key manuals (F..C: keys 5..48 of the 61-key frame here), no 16' on the lower
// manual, 13 pedals. Its generator is the A-based one (setBfree TG_82FB09: lowest wheel 10,
// footages below it fold up an octave). No taper data was found, so its manuals are untapered
// (0 dB) - an approximation, like the rest of this model.
constexpr int kM3FirstKey = 5, kM3LastKey = 48, kM3Pedals = 13;

inline int m3ManualWheel (int key, int foot)
{
    int t = key + 13 + kFootageOffset[foot];
    while (t < 10) t += 12;
    while (t > kNumWheels) t -= 12;
    return t;
}

// Builds the wiring of one manual (0 upper, 1 lower) for an organ model.
inline void buildManualWiring (std::array<KeyWiring, kMaxKeys>& w, int organModel, int manual = 0)
{
    const bool m3 = organModel == 3;
    for (int k = 0; k < kMaxKeys; ++k)
    {
        auto& kw = w[(size_t) k];
        kw = {};
        if (m3)
        {
            if (k < kM3FirstKey || k > kM3LastKey) continue;   // the spinet's keyboard is shorter
            for (int f = manual == 1 ? 1 : 0; f < kContacts; ++f)
                kw.c[(size_t) kw.count++] = { m3ManualWheel (k, f), f, 1.0 };
            continue;
        }
        for (int f = 0; f < kContacts; ++f)
            kw.c[(size_t) f] = { b3ManualWheel (k, f), f, dbToGain (b3TaperDb (k, f)) };
        kw.count = kContacts;
    }
}

inline void buildPedalWiring (std::array<KeyWiring, kMaxKeys>& w, int organModel = 0)
{
    const bool m3 = organModel == 3;
    auto wheel = [m3] (int t) { if (m3) while (t < 10) t += 12; return t; };
    for (int k = 0; k < kMaxKeys; ++k)
    {
        auto& kw = w[(size_t) k];
        kw = {};
        if (k >= (m3 ? kM3Pedals : kPedalKeys)) continue;
        kw.c[0] = { wheel (k + 1),  0, 1.0 };
        kw.c[1] = { wheel (k + 13), 0, 0.35 };
        kw.c[2] = { wheel (k + 20), 0, 0.15 };
        kw.c[3] = { wheel (k + 13), 1, 1.0 };
        kw.c[4] = { wheel (k + 25), 1, 0.30 };
        kw.c[5] = { wheel (k + 32), 1, 0.12 };
        kw.count = 6;
    }
}

// Fixed registrations of the preset keys C# .. A (index 1..9 of the preset-key choice).
// The B-3's factory preset-key chart as commonly published (upper manual). Phase 1 uses it for
// both manuals; phase 2 checks it against the service manual and adds the lower manual's chart.
// Players re-wire these on a real organ; here they are stored per preset (the "registrations"
// block) once phase 2 adds editing.
constexpr int kFactoryRegistrations[9][9] {
    { 0, 0, 5, 3, 2, 0, 0, 0, 0 },   // C#  Stopped Flute
    { 0, 0, 4, 4, 3, 2, 0, 0, 0 },   // D   Dulciana
    { 0, 0, 8, 7, 4, 0, 0, 0, 0 },   // D#  French Horn
    { 0, 0, 4, 5, 4, 4, 2, 2, 2 },   // E   Salicional
    { 0, 0, 5, 4, 0, 3, 0, 0, 0 },   // F   Flutes 8' & 4'
    { 0, 0, 4, 6, 7, 5, 3, 0, 0 },   // F#  Oboe Horn
    { 0, 0, 5, 6, 4, 4, 3, 2, 0 },   // G   Swell Diapason
    { 0, 0, 6, 8, 7, 6, 5, 4, 0 },   // G#  Trumpet
    { 3, 2, 7, 6, 4, 5, 2, 2, 2 },   // A   Full Swell
};
} // namespace ham
