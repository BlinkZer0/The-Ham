#pragma once
// ============================================================================
//  Parameter table. Mirrors vst/ui/js/params.js (IDs, ranges, defaults, skew,
//  choice lists, ORDER). `TheHamTests --dump-params` prints this table and
//  tests/check-params.ps1 fails on any difference from params.js.
//
//  Stability rules (same as E-Piano / Brass Knuckles):
//   - the order below is the storage order; new parameters are only APPENDED
//   - IDs never change; ParameterID version is 1
//   - parameters appended after v1.0 must default to a neutral value
//   - choice lists are append-only too (a host lane maps values onto them)
//  The whole table for every phase is defined in phase 1 and frozen, so test
//  projects keep loading. Parameters for later phases exist but do nothing yet.
// ============================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <vector>

namespace ham
{
constexpr int kNumFootages = 9;   // 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'

enum ParamIndex
{
    // ---- global -----------------------------------------------------------------
    organModel, ampLevel, master, tuneA, mains, transpose, motorStart, motorRun, velMode, velAmount,
    // ---- drawbars: upper A, upper B, lower A, lower B (9 each), pedal 16' and 8' ----
    upA1,
    upA9 = upA1 + 8,
    upB1, upB9 = upB1 + 8,
    loA1, loA9 = loA1 + 8,
    loB1, loB9 = loB1 + 8,
    pd16, pd8,
    // ---- preset keys (C cancel, C#..A fixed, A# = set A, B = set B) -------------
    upKey, loKey,
    // ---- percussion (upper manual, set B only) ----------------------------------
    percOn, percVol, percDecay, percHarm, percFast, percSlow,
    // ---- scanner vibrato / chorus -----------------------------------------------
    vibUpper, vibLower, vibMode,
    // ---- manual character (click, leakage, age per manual) ----------------------
    clickUp, clickLo, clickPd, keyUpClick, leakUp, leakLo, ageUp, ageLo,
    // ---- pedals / lower sustain / coupling --------------------------------------
    pedTone, pedSustain, pedDecay, loSustain, loSusTime, manualBass,
    // ---- preamp -----------------------------------------------------------------
    drive, exprCC,
    // ---- MIDI -------------------------------------------------------------------
    chUpper, chLower, chPedal, splitMode, splitPoint, splitLoOct, pedalZone, pedalSplit, pedMidiBase,
    ksOn, ccMap, speedWheel, speedCC, speedSustain, motorCC, varSource,
    // ---- rotary cabinet ---------------------------------------------------------
    cabModel, cabBypass, rotorConfig, speedMode, speed, hornSpeedSel, drumSpeedSel, drumOff,
    hornSlow, hornFast, hornAccel, hornBrake, drumSlow, drumFast, drumAccel, drumBrake,
    hornDir, drumDir, brakePos, brakeAngle, hornType, hornMouth, drumBaffle, xover, hornDrumBal, cabBack,
    micLayout, micDist, micAngle, micHorn, micDrum, micRoom, ampOS, width,
    // ---- outboard pedals (between preamp and cabinet) ---------------------------
    odOn, odDrive, odTone, odLevel, wahOn, wahPos, wahSrc, wahCC,
    echoOn, echoTime, echoFb, echoMix, echoWow, echoTone,
    // ---- spring reverb (after the cabinet) --------------------------------------
    revOn, revMix, revDecay, revTone,
    kNumParams
};

// manual 0 = upper, 1 = lower; set 0 = A (A# key), 1 = B (B key); foot 0..8 (16' .. 1')
inline int drawbarParam (int manual, int set, int foot) { return upA1 + (manual * 2 + set) * kNumFootages + foot; }

enum class Kind { Float, Bool, Choice, Int };
enum class Unit { None, Seconds, Hz, LfoHz, Semitones, Cents, Percent, BipolarPercent, Decibels, Degrees,
                  Ms, Count, Note, Drawbar, Octaves };

struct ParamDef
{
    juce::String id;
    juce::String hostName;
    Kind kind;
    float min, max, def;
    Unit unit;
    bool logSkew;
    juce::StringArray choices; // Kind::Choice only
    bool noPreset = false;     // not stored in presets (master)
};

// Choice lists: the order is the stored value; must match params.js and the engine enums.
inline juce::StringArray organModelChoices() { return { "B-3", "C-3", "A-100", "M-3" }; }
inline juce::StringArray mainsChoices()      { return { "60 Hz", "50 Hz" }; }
inline juce::StringArray velModeChoices()    { return { "Off", "Volume", "Click" }; }
inline juce::StringArray presetKeyChoices()  { return { "C Cancel", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A",
                                                        "A# Set A", "B Set B" }; }
inline juce::StringArray percVolChoices()    { return { "Normal", "Soft" }; }
inline juce::StringArray percDecayChoices()  { return { "Fast", "Slow" }; }
inline juce::StringArray percHarmChoices()   { return { "Second", "Third" }; }
inline juce::StringArray vibModeChoices()    { return { "V1", "V2", "V3", "C1", "C2", "C3" }; }
inline juce::StringArray ccMapChoices()      { return { "Off", "B4D (12-20 / 21-29)", "Nord Electro (16-24 / 70-78)",
                                                        "VK / CX-3 (70-78)" }; }
inline juce::StringArray speedWheelChoices() { return { "Off", "Half Moon", "Momentary" }; }
inline juce::StringArray varSourceChoices()  { return { "Mod Wheel", "Expression", "Speed CC" }; }
inline juce::StringArray cabModelChoices()   { return { "Rotor 22", "Rotor 45", "Single Rotor", "Tone Cabinet" }; }
inline juce::StringArray rotorConfigChoices(){ return { "Horn + Drum", "Horn Only", "Drum Only", "Stationary" }; }
inline juce::StringArray speedModeChoices()  { return { "2-Speed", "3-Speed", "Variable" }; }
inline juce::StringArray speedChoices()      { return { "Stop", "Slow", "Fast" }; }
inline juce::StringArray rotorSelChoices()   { return { "Follow", "Stop", "Slow", "Fast" }; }
inline juce::StringArray dirChoices()        { return { "CCW", "CW" }; }
inline juce::StringArray hornTypeChoices()   { return { "Dual (1 Dummy)", "Dual Active" }; }
inline juce::StringArray hornMouthChoices()  { return { "Stock", "Short" }; }
inline juce::StringArray drumBaffleChoices() { return { "Stock", "Open", "Foam" }; }
inline juce::StringArray cabBackChoices()    { return { "Closed", "Open" }; }
inline juce::StringArray micLayoutChoices()  { return { "Mono", "Stereo Horn", "Wide Stereo", "3-Mic", "Room Pair" }; }
inline juce::StringArray oversampleChoices() { return { "1x", "2x", "4x" }; }
inline juce::StringArray wahSrcChoices()     { return { "Knob", "Expression", "Mod Wheel", "CC" }; }

inline juce::String footageName (int foot)
{
    static const char* names[] { "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'" };
    return names[juce::jlimit (0, 8, foot)];
}

inline const std::vector<ParamDef>& paramDefs()
{
    static const std::vector<ParamDef> defs = []
    {
        std::vector<ParamDef> d;
        auto add = [&d] (juce::String id, juce::String name, Kind kind, float mn, float mx, float def,
                         Unit unit = Unit::None, bool log = false)
        {
            d.push_back ({ id, name, kind, mn, mx, def, unit, log, {}, false });
        };
        auto flt  = [&add] (juce::String id, juce::String name, float mn, float mx, float def, Unit u = Unit::None)
                    { add (id, name, Kind::Float, mn, mx, def, u, false); };
        auto logf = [&add] (juce::String id, juce::String name, float mn, float mx, float def, Unit u)
                    { add (id, name, Kind::Float, mn, mx, def, u, true); };
        auto intp = [&add] (juce::String id, juce::String name, float mn, float mx, float def, Unit u = Unit::Count)
                    { add (id, name, Kind::Int, mn, mx, def, u, false); };
        auto bln  = [&add] (juce::String id, juce::String name, bool def)
                    { add (id, name, Kind::Bool, 0, 1, def ? 1.0f : 0.0f); };
        auto choice = [&d] (juce::String id, juce::String name, juce::StringArray c, int def)
        {
            d.push_back ({ id, name, Kind::Choice, 0.0f, (float) (c.size() - 1), (float) def, Unit::None, false, c, false });
        };

        // ---- global
        choice ("organModel", "Organ Model", organModelChoices(), 0);
        flt  ("ampLevel", "Patch Level", -30, 6, 0, Unit::Decibels);
        flt  ("master", "Master", 0, 1, 0.7f, Unit::Percent);
        d.back().noPreset = true;
        flt  ("tuneA", "Tuning A", 430, 450, 440, Unit::LfoHz);
        choice ("mains", "Mains", mainsChoices(), 0);
        intp ("transpose", "Transpose", -12, 12, 0, Unit::Semitones);
        bln  ("motorStart", "Motor Start", true);
        bln  ("motorRun", "Motor Run", true);
        choice ("velMode", "Velocity", velModeChoices(), 0);
        flt  ("velAmount", "Velocity Amount", 0, 1, 0.5f, Unit::Percent);

        // ---- drawbars
        {
            // factory registrations of the A# / B sets: upper 00 8800 000 / 88 8000 000,
            // lower 00 8000 000 / 83 8000 000
            const int def[4][9] { { 0, 0, 8, 8, 0, 0, 0, 0, 0 }, { 8, 8, 8, 0, 0, 0, 0, 0, 0 },
                                  { 0, 0, 8, 0, 0, 0, 0, 0, 0 }, { 8, 3, 8, 0, 0, 0, 0, 0, 0 } };
            const char* manualIds[] { "up", "lo" };
            const char* manualNames[] { "Upper", "Lower" };
            for (int m = 0; m < 2; ++m)
                for (int s = 0; s < 2; ++s)
                    for (int f = 0; f < kNumFootages; ++f)
                        flt (juce::String (manualIds[m]) + (s == 0 ? "A" : "B") + juce::String (f + 1),
                             juce::String (manualNames[m]) + (s == 0 ? " A " : " B ") + footageName (f),
                             0, 8, (float) def[m * 2 + s][f], Unit::Drawbar);
        }
        flt  ("pd16", "Pedal 16'", 0, 8, 5, Unit::Drawbar);
        flt  ("pd8", "Pedal 8'", 0, 8, 3, Unit::Drawbar);
        choice ("upKey", "Upper Preset Key", presetKeyChoices(), 11);
        choice ("loKey", "Lower Preset Key", presetKeyChoices(), 11);

        // ---- percussion
        bln  ("percOn", "Percussion", false);
        choice ("percVol", "Perc Volume", percVolChoices(), 0);
        choice ("percDecay", "Perc Decay", percDecayChoices(), 0);
        choice ("percHarm", "Perc Harmonic", percHarmChoices(), 1);
        logf ("percFast", "Perc Fast Time", 0.05f, 3, 0.3f, Unit::Seconds);
        logf ("percSlow", "Perc Slow Time", 0.2f, 8, 1.0f, Unit::Seconds);

        // ---- vibrato / chorus
        bln  ("vibUpper", "Vibrato Upper", false);
        bln  ("vibLower", "Vibrato Lower", false);
        choice ("vibMode", "Vibrato Mode", vibModeChoices(), 5);

        // ---- manual character
        flt  ("clickUp", "Click Upper", 0, 1, 0.5f, Unit::Percent);
        flt  ("clickLo", "Click Lower", 0, 1, 0.5f, Unit::Percent);
        flt  ("clickPd", "Click Pedal", 0, 1, 0.3f, Unit::Percent);
        bln  ("keyUpClick", "Key-Up Click", true);
        flt  ("leakUp", "Leakage Upper", 0, 1, 0.3f, Unit::Percent);
        flt  ("leakLo", "Leakage Lower", 0, 1, 0.3f, Unit::Percent);
        flt  ("ageUp", "Age Upper", 0, 1, 0.3f, Unit::Percent);
        flt  ("ageLo", "Age Lower", 0, 1, 0.3f, Unit::Percent);

        // ---- pedals / lower
        flt  ("pedTone", "Pedal Tone", 0, 1, 0.5f, Unit::Percent);
        bln  ("pedSustain", "Pedal Sustain", false);
        logf ("pedDecay", "Pedal Decay", 0.05f, 4, 0.6f, Unit::Seconds);
        bln  ("loSustain", "Lower Sustain", false);
        logf ("loSusTime", "Lower Sustain Time", 0.05f, 4, 0.8f, Unit::Seconds);
        bln  ("manualBass", "Manual Bass", false);

        // ---- preamp
        flt  ("drive", "Drive", 0, 1, 0.2f, Unit::Percent);
        intp ("exprCC", "Expression CC", 1, 119, 11);

        // ---- MIDI
        intp ("chUpper", "Upper Channel", 1, 16, 1);
        intp ("chLower", "Lower Channel", 1, 16, 2);
        intp ("chPedal", "Pedal Channel", 1, 16, 3);
        bln  ("splitMode", "Split", false);
        intp ("splitPoint", "Split Point", 24, 96, 60, Unit::Note);
        intp ("splitLoOct", "Split Lower Octave", -2, 2, 0, Unit::Count);
        bln  ("pedalZone", "Pedal Zone", false);
        intp ("pedalSplit", "Pedal Split", 24, 72, 48, Unit::Note);
        intp ("pedMidiBase", "Pedal MIDI Base", 0, 96, 36, Unit::Note);
        bln  ("ksOn", "Preset Keyswitches", false);
        choice ("ccMap", "Drawbar CC Map", ccMapChoices(), 1);
        choice ("speedWheel", "Mod Wheel Speed", speedWheelChoices(), 1);
        intp ("speedCC", "Speed CC", 0, 127, 0, Unit::Count);
        bln  ("speedSustain", "Sustain Pedal Speed", false);
        intp ("motorCC", "Motor Off CC", 0, 127, 0, Unit::Count);
        choice ("varSource", "Variable Speed Source", varSourceChoices(), 0);

        // ---- rotary cabinet
        choice ("cabModel", "Cabinet", cabModelChoices(), 0);
        bln  ("cabBypass", "Cabinet Bypass", false);
        choice ("rotorConfig", "Rotors", rotorConfigChoices(), 0);
        choice ("speedMode", "Speed Mode", speedModeChoices(), 0);
        choice ("speed", "Rotor Speed", speedChoices(), 1);
        choice ("hornSpeedSel", "Horn Speed", rotorSelChoices(), 0);
        choice ("drumSpeedSel", "Drum Speed", rotorSelChoices(), 0);
        bln  ("drumOff", "Drum Off", false);
        flt  ("hornSlow", "Horn Slow", 0.1f, 3, 0.8f, Unit::LfoHz);
        flt  ("hornFast", "Horn Fast", 2, 10, 6.7f, Unit::LfoHz);
        logf ("hornAccel", "Horn Accel", 0.1f, 12, 1.0f, Unit::Seconds);
        logf ("hornBrake", "Horn Brake", 0.1f, 12, 1.5f, Unit::Seconds);
        flt  ("drumSlow", "Drum Slow", 0.1f, 3, 0.7f, Unit::LfoHz);
        flt  ("drumFast", "Drum Fast", 2, 10, 5.8f, Unit::LfoHz);
        logf ("drumAccel", "Drum Accel", 0.1f, 12, 4.0f, Unit::Seconds);
        logf ("drumBrake", "Drum Brake", 0.1f, 12, 5.0f, Unit::Seconds);
        choice ("hornDir", "Horn Direction", dirChoices(), 0);
        choice ("drumDir", "Drum Direction", dirChoices(), 1);
        bln  ("brakePos", "Horn Brake Position", false);
        flt  ("brakeAngle", "Horn Stop Angle", 0, 360, 0, Unit::Degrees);
        choice ("hornType", "Horn Type", hornTypeChoices(), 0);
        choice ("hornMouth", "Horn Mouth", hornMouthChoices(), 0);
        choice ("drumBaffle", "Drum Baffle", drumBaffleChoices(), 0);
        logf ("xover", "Crossover", 300, 3000, 800, Unit::Hz);
        flt  ("hornDrumBal", "Horn/Drum Balance", -1, 1, 0, Unit::BipolarPercent);
        choice ("cabBack", "Cabinet Back", cabBackChoices(), 0);
        choice ("micLayout", "Mic Layout", micLayoutChoices(), 1);
        flt  ("micDist", "Mic Distance", 0, 1, 0.4f, Unit::Percent);
        flt  ("micAngle", "Mic Angle", 0, 180, 90, Unit::Degrees);
        flt  ("micHorn", "Horn Mic Level", 0, 1, 1, Unit::Percent);
        flt  ("micDrum", "Drum Mic Level", 0, 1, 1, Unit::Percent);
        flt  ("micRoom", "Room Mic Level", 0, 1, 0, Unit::Percent);
        choice ("ampOS", "Amp Oversampling", oversampleChoices(), 1);
        flt  ("width", "Stereo Width", 0, 2, 1, Unit::Percent);

        // ---- outboard
        bln  ("odOn", "Overdrive On", false);
        flt  ("odDrive", "Overdrive", 0, 1, 0.5f, Unit::Percent);
        flt  ("odTone", "Overdrive Tone", 0, 1, 0.5f, Unit::Percent);
        flt  ("odLevel", "Overdrive Level", -24, 6, 0, Unit::Decibels);
        bln  ("wahOn", "Wah On", false);
        flt  ("wahPos", "Wah Position", 0, 1, 0.5f, Unit::Percent);
        choice ("wahSrc", "Wah Source", wahSrcChoices(), 0);
        intp ("wahCC", "Wah CC", 0, 127, 4, Unit::Count);
        bln  ("echoOn", "Echo On", false);
        logf ("echoTime", "Echo Time", 0.03f, 1.2f, 0.35f, Unit::Seconds);
        flt  ("echoFb", "Echo Feedback", 0, 0.95f, 0.35f, Unit::Percent);
        flt  ("echoMix", "Echo Mix", 0, 1, 0.25f, Unit::Percent);
        flt  ("echoWow", "Echo Wow", 0, 1, 0.3f, Unit::Percent);
        flt  ("echoTone", "Echo Tone", 0, 1, 0.5f, Unit::Percent);

        // ---- reverb
        bln  ("revOn", "Reverb On", false);
        flt  ("revMix", "Reverb Mix", 0, 1, 0.2f, Unit::Percent);
        logf ("revDecay", "Reverb Decay", 0.5f, 6, 2.0f, Unit::Seconds);
        flt  ("revTone", "Reverb Tone", 0, 1, 0.5f, Unit::Percent);

        jassert ((int) d.size() == kNumParams);
        return d;
    }();
    return defs;
}

inline juce::String noteName (int n)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    n = juce::jlimit (0, 127, n);
    return juce::String (names[n % 12]) + juce::String (n / 12 - 1);
}

inline juce::String formatValue (Unit unit, float v)
{
    auto signedNum = [] (float x, int decimals) { return (x > 0 ? "+" : "") + juce::String (x, decimals); };
    switch (unit)
    {
        case Unit::Seconds:   return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms"
                                              : juce::String (v, 2) + " s";
        case Unit::Hz:        return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                                                  : juce::String (juce::roundToInt (v)) + " Hz";
        case Unit::LfoHz:     return juce::String (v, v < 1.0f ? 3 : (v < 10.0f ? 2 : 1)) + " Hz";
        case Unit::Semitones: return std::abs (v - std::round (v)) > 0.05f ? signedNum (v, 1) + " st"
                                                                            : (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + " st";
        case Unit::Cents:     return (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct";
        case Unit::Percent:   return juce::String (juce::roundToInt (v * 100.0f)) + "%";
        case Unit::BipolarPercent: return (v > 0.004f ? "+" : "") + juce::String (juce::roundToInt (v * 100.0f)) + "%";
        case Unit::Decibels:  return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB";
        case Unit::Degrees:   return juce::String (juce::roundToInt (v)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
        case Unit::Ms:        return juce::String (juce::roundToInt (v)) + " ms";
        case Unit::Count:     return juce::String (juce::roundToInt (v));
        case Unit::Note:      return noteName (juce::roundToInt (v));
        case Unit::Drawbar:   return std::abs (v - std::round (v)) < 0.05f ? juce::String (juce::roundToInt (v)) : juce::String (v, 1);
        case Unit::Octaves:   return (v > 0.005f ? "+" : "") + juce::String (v, 1) + " oct";
        case Unit::None:
        default:              return juce::String (v, 2);
    }
}

// Same log mapping as paramToNorm/paramFromNorm in params.js
inline juce::NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float start, float end, float n) { return start * std::pow (end / start, n); },
             [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
             [] (float start, float end, float v) { return juce::jlimit (start, end, v); } };
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (const auto& d : paramDefs())
    {
        const juce::ParameterID pid { d.id, 1 };
        const auto unit = d.unit;

        switch (d.kind)
        {
            case Kind::Bool:
                layout.add (std::make_unique<juce::AudioParameterBool> (pid, d.hostName, d.def >= 0.5f));
                break;

            case Kind::Choice:
                layout.add (std::make_unique<juce::AudioParameterChoice> (pid, d.hostName, d.choices, (int) d.def));
                break;

            case Kind::Int:
                layout.add (std::make_unique<juce::AudioParameterInt> (
                    pid, d.hostName, (int) d.min, (int) d.max, (int) d.def,
                    juce::AudioParameterIntAttributes().withStringFromValueFunction (
                        [unit] (int v, int) { return formatValue (unit, (float) v); })));
                break;

            case Kind::Float:
            default:
                layout.add (std::make_unique<juce::AudioParameterFloat> (
                    pid, d.hostName,
                    d.logSkew ? logRange (d.min, d.max) : juce::NormalisableRange<float> (d.min, d.max),
                    d.def,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction (
                        [unit] (float v, int) { return formatValue (unit, v); })));
                break;
        }
    }
    return layout;
}
} // namespace ham
