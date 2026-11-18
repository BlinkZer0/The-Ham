#pragma once
// ============================================================================
//  Cabinet models (data). One shared rotary engine (dsp/Rotary.h); each model
//  sets its fixed voicing here, and the parameter defaults that are loaded when
//  the model is picked in the UI (every option stays editable afterwards).
//
//  Rotor 22      modeled on the 122 / 147: horn + drum, 40 W tube amp, big closed
//                cabinet: deep drum, wide swirl, smooth breakup.
//  Rotor 45      modeled on the 145: same rotors, smaller cabinet: tighter, boxier,
//                more midrange punch, breaks up sooner.
//  Single Rotor  Vibratone-style: one 10" speaker behind a rotating foam drum, no horn:
//                narrow, mid-heavy, watery.
//  Tone Cabinet  stationary (PR-40-style): fixed speakers, built-in spring reverb.
//
//  The voicing numbers (radii, AM depths, filter corners, box resonance, amp) are
//  The Ham's estimates: no measured data for these cabinets was found. The speed
//  defaults of Rotor 22 are the brief's (horn 0.8 / 6.7 Hz, 1 s up / 1.5 s down;
//  drum 0.7 / 5.8 Hz, 4 s / 5 s).
// ============================================================================

namespace ham
{
struct CabinetModel
{
    // parameter defaults (loaded with the model)
    int rotorConfig;                                    // 0 horn+drum, 1 horn only, 2 drum only, 3 stationary
    double hornSlow, hornFast, hornAccel, hornBrake;    // Hz, s
    double drumSlow, drumFast, drumAccel, drumBrake;
    int drumBaffle;                                     // 0 stock, 1 open, 2 foam
    double xover;                                       // Hz
    int cabBack, micLayout;
    // fixed voicing
    double hornRadius, drumRadius;                      // m: the moving source's radius (Doppler)
    double hornAm, drumAm;                              // amplitude modulation depth 0..1
    double hornLpFacing, hornLpAway, drumLpFacing, drumLpAway;   // Hz: directional low-pass
    double boxHz, boxDb, boxQ;                          // cabinet resonance (closed back)
    double wallMs;                                      // back-wall reflection path (closed back)
    double ampGainDb, ampBias, ampHpHz, ampLpHz;        // tube amp voicing
    double reverbMix;                                   // built-in spring reverb
    bool stationary;                                    // fixed speakers, no rotors
    double outGain;
};

inline const CabinetModel& cabinetModel (int i)
{
    static const CabinetModel models[4] {
        // Rotor 22
        { 0, 0.8, 6.7, 1.0, 1.5,   0.7, 5.8, 4.0, 5.0,   0, 800.0, 0, 1,
          0.15, 0.10,  0.55, 0.45,  12000.0, 3000.0, 2500.0, 900.0,  120.0, 3.0, 1.2,  1.4,
          6.0, 0.20, 40.0, 7000.0,  0.0, false, 0.80 },
        // Rotor 45
        { 0, 0.85, 6.8, 0.9, 1.4,  0.75, 6.0, 3.5, 4.5,  0, 800.0, 0, 1,
          0.15, 0.10,  0.55, 0.45,  11000.0, 2800.0, 2500.0, 900.0,  180.0, 5.0, 1.5,  0.9,
          10.0, 0.25, 70.0, 6500.0, 0.0, false, 0.80 },
        // Single Rotor
        { 2, 0.9, 6.0, 1.2, 2.0,   0.9, 6.0, 1.2, 2.0,   2, 800.0, 0, 2,
          0.15, 0.10,  0.40, 0.60,  6000.0, 2000.0, 5000.0, 1500.0,  300.0, 4.0, 1.0,  0.7,
          8.0, 0.30, 120.0, 5000.0, 0.0, false, 0.90 },
        // Tone Cabinet
        { 3, 0.8, 6.7, 1.0, 1.5,   0.7, 5.8, 4.0, 5.0,   0, 800.0, 0, 2,
          0.15, 0.10,  0.0, 0.0,    12000.0, 12000.0, 4000.0, 4000.0,  100.0, 2.0, 1.0,  2.5,
          3.0, 0.10, 40.0, 9000.0,  0.25, true, 0.80 },
    };
    return models[i < 0 ? 0 : (i > 3 ? 3 : i)];
}
} // namespace ham
