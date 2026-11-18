#pragma once
// ============================================================================
//  The Ham engine. Header-only, no JUCE, no allocation after prepare(), no locks.
//  Double precision. Line-for-line twin (phase 6): vst/webhost/dsp-worklet.js.
//
//  Signal path (phase 2):
//    motor (Start/Run, mains, tuning, pitch dive) -> 91 tonewheels (rotating phasors,
//    always running, never reset by keys; Age adds level spread and 2nd/3rd harmonics)
//    -> key contacts (sample-accurate, with bounce: the key click; sustain = slow release)
//    -> busbars (+ leakage: compartment partner, neighbouring busbars; + a quiet floor of
//       every wheel) -> drawbars of the active registration (32-sample ramps)
//    upper / lower -> scanner vibrato / chorus (per-manual switches)
//    upper 2nd / 3rd busbar -> percussion envelope (bypasses the scanner)
//    pedals -> pedal tone low-pass (bypasses the scanner)
//    -> preamp / expression -> outboard (overdrive -> wah -> tape echo)
//    -> rotary cabinet: tube amp (Drive; ADAA, 1x / 2x / 4x; level-matched) -> crossover -> horn / drum rotors
//       (Doppler, AM, directional filtering, reflections) -> mics (stereo)
//    -> spring reverb -> width -> patch level x master -> safety clip -> L, R.
//
//  Not a voice synth: there is no voice allocation. A key closes up to 9 contacts, each
//  adding one wheel (x its taper) to one busbar. The busbar gains busG[m][b][w] change only
//  at contact events; the wheels run all the time.
// ============================================================================
#include <vector>
#include "dsp/Common.h"
#include "dsp/Tonewheels.h"
#include "dsp/Wiring.h"
#include "dsp/Scanner.h"
#include "dsp/Preamp.h"
#include "dsp/Oversampler.h"
#include "dsp/Rotary.h"
#include "dsp/Outboard.h"
#include "dsp/SpringReverb.h"

namespace ham
{
constexpr int kEventCapacity = 1 << 16;   // pending contact events (147 keys x 9 contacts x <= 22 events)
constexpr int kNumWheelSlots = kNumWheels + 1; // wheel index 1..91 (slot 0 unused)

// Snapshot of everything the engine needs from the parameters (EngineParamsFromValues.h).
struct EngineParams
{
    int organModel = 0;
    double ampGain = 1.0, master = 0.7;
    double tuneRatio = 1.0;          // A / 440
    int mains = 0, transpose = 0;
    bool motorStart = true, motorRun = true;
    int velMode = 0;                 // 0 off, 1 volume, 2 click
    double velAmount = 0.5;
    double bars[2][2][kNumFootages] {};   // [manual][set A/B][footage]
    double pedalBars[2] { 5, 3 };
    int presetKey[2] { 11, 11 };     // 0 cancel, 1..9 fixed, 10 set A, 11 set B
    int regs[2][9][kNumFootages] {}; // fixed registrations of the preset keys C#..A, per manual
    double click[3] { 0.5, 0.5, 0.3 };
    bool keyUpClick = true;
    // percussion (upper manual, set B)
    bool percOn = false, percSoft = false, percSlow = false, percThird = true;
    double percFastTime = 0.3, percSlowTime = 1.0;   // to -60 dB
    // scanner
    bool vibUpper = false, vibLower = false;
    int vibMode = 5;                 // V1 V2 V3 C1 C2 C3
    // manual character
    double leak[2] { 0.3, 0.3 }, age[2] { 0.3, 0.3 };
    // pedals / lower
    double pedTone = 0.5, pedDecay = 0.6, loSusTime = 0.8;
    bool pedSustain = false, loSustain = false, manualBass = false;
    // MIDI
    int channel[3] { 0, 1, 2 };      // MIDI channel index per manual
    bool splitMode = false;
    int splitPoint = 60, splitLoOct = 0;
    bool pedalZone = false;
    int pedalSplit = 48, pedMidiBase = 36;
    int motorCC = 0, exprCC = 11;
    // rotary speed control
    int speedWheel = 1, speedCC = 0, varSource = 0;
    bool speedSustain = false;
    // cabinet
    double drive = 0.2;
    int cabModel = 0, rotorConfig = 0, speedMode = 0, speed = 1, hornSel = 0, drumSel = 0;
    bool cabBypass = false, drumOff = false, brakePos = false;
    double hornSlow = 0.8, hornFast = 6.7, hornAccel = 1.0, hornBrake = 1.5;
    double drumSlow = 0.7, drumFast = 5.8, drumAccel = 4.0, drumBrake = 5.0;
    int hornDir = 0, drumDir = 1;
    double brakeAngle = 0;
    int hornType = 0, hornMouth = 0, drumBaffle = 0, cabBack = 0, micLayout = 1, ampOS = 1;
    double xover = 800, hornDrumBal = 0, micDist = 0.4, micAngle = 90, micHorn = 1, micDrum = 1, micRoom = 0, width = 1;
    // outboard
    bool odOn = false, wahOn = false, echoOn = false, revOn = false;
    double odDrive = 0.5, odTone = 0.5, odLevel = 0, wahPos = 0.5;
    int wahSrc = 0, wahCC = 4;
    double echoTime = 0.35, echoFb = 0.35, echoMix = 0.25, echoWow = 0.3, echoTone = 0.5;
    double revMix = 0.2, revDecay = 2.0, revTone = 0.5;

    EngineParams()
    {
        for (int m = 0; m < 2; ++m)
            for (int k = 0; k < 9; ++k)
                for (int f = 0; f < kNumFootages; ++f) regs[m][k][f] = kFactoryRegistrations[k][f];
    }
};

// A value ramped per sample across one 32-sample chunk (set once per chunk).
struct ChunkRamp
{
    double cur = 0, target = 0, inc = 0;
    void set (double t) { cur = target; target = t; inc = (t - cur) / kChunk; }
    void jump (double t) { cur = target = t; inc = 0; }
    double next() { const double v = cur; cur += inc; return v; }
};

class SynthEngine
{
public:
    static constexpr double kOutScale = 0.06;          // one 8' key at full drawbar -> about -27 dBFS RMS
    static constexpr double kPercNormalBars = 0.60512; // drawbars with percussion Normal (setBfree: -4.4 dB)
    static constexpr double kPercSoft = 0.5012;        // percussion Soft vs Normal (-6 dB, setBfree)
    static constexpr double kLeakBase = 0.0316;        // leakage at Leak 100%: -30 dB (30%: -40 dB, setBfree's default)
    static constexpr double kFloorBase = 0.0005;       // the always-present bleed of every wheel at Leak 100%: -66 dB each
    static constexpr double kAgeContactDb = 3.0;       // contact level spread at Age 100%: +-3 dB
    static constexpr double kAgeWheelDb = 2.0;         // wheel level spread at Age 100%: +-2 dB
    static constexpr double kStage = 0.06;             // level into the analog stages (amp, pedals) of one 8' key at full drawbar

    SynthEngine() { events.resize ((size_t) kEventCapacity); }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        rng.seed (0x48414d31u); // "HAM1"
        // wheels start at fixed pseudo-random phases (same in the JS engine)
        for (int w = 1; w <= kNumWheels; ++w)
        {
            const double ph = kTwoPi * rng.uni();
            wc[(size_t) w] = std::cos (ph);
            ws[(size_t) w] = std::sin (ph);
        }
        // fixed per-instrument "wear" patterns (Age scales them) and the leakage floor
        for (int w = 1; w <= kNumWheels; ++w)
        {
            wheelRnd[(size_t) w] = rng.bi();
            floorW[(size_t) w] = kFloorBase * (0.3 + 0.7 * w / (double) kNumWheels) * (0.5 + 0.5 * rng.uni());
        }
        for (auto& man : contactRnd) for (auto& key : man) for (auto& c : key) c = rng.bi();
        builtModel = params.organModel;
        buildManualWiring (wiring[0], builtModel, 0);
        buildManualWiring (wiring[1], builtModel, 1);
        buildPedalWiring (wiring[2], builtModel);
        genAge = -1.0;               // forces the wheel tables to be built
        contactAge[0] = contactAge[1] = contactAge[2] = -1.0;
        updateAgeTables();
        motorSpeed = 1.0;            // a freshly loaded organ is already running
        ampCur = ampInc = 0.0;
        freqFactor = -1.0;           // forces the rotation coefficients to be computed
        masterGain = params.master;
        now = 0;
        chunkPos = 0;
        seq = 0;
        resetKeys();
        expression = exprSmooth = 1.0;
        modWheel = 0.0;
        sustain = false;
        motorCcOff = false;
        peak = 0.0f;
        percEnv = 0.0;
        percMult = 1.0;
        scanner.prepare (sr);
        scanRate = -1.0;
        preamp.prepare (sr);
        pedTone = -1.0;
        pedLp.reset();
        for (auto* r : { &vibU, &vibL, &vibDepth, &vibChorus, &percGain }) r->jump (0.0);
        vibU.jump (params.vibUpper ? 1.0 : 0.0);
        vibL.jump (params.vibLower ? 1.0 : 0.0);
        vibDepth.jump (depthFor (params.vibMode));
        vibChorus.jump (params.vibMode >= 3 ? 1.0 : 0.0);
        for (auto& row : barCur) row.fill (0.0);
        for (auto& row : barTarget) row.fill (0.0);
        for (auto& row : barInc) row.fill (0.0);
        coupledPedal = -1;

        // phase 3: outboard, amp, cabinet, reverb
        amp.reset();
        ampHp = 0.0;
        ampPin = ampPout = 0.0;
        ampMakeup.jump (1.0);
        ampLp.reset();
        ampLpHz = -1.0;
        cab.prepare (sr);
        od.reset();
        wah.reset();
        echo.prepare (sr);
        rev.prepare (sr);
        revSet = { -1, -1 };
        odMix.jump (params.odOn ? 1.0 : 0.0);
        wahMix.jump (params.wahOn ? 1.0 : 0.0);
        echoOnR.jump (params.echoOn ? 1.0 : 0.0);
        revMixR.jump (0.0);
        widthR.jump (params.width);
        ccVal.fill (0.0);
        speedState = params.speed;
        lastSpeedParam = params.speed;
        momentaryFast = lastWheelFast = false;
        speedCCValue = 0.0;
        rotorsStarted = false;   // the first chunk puts the rotors at their target speed (no spin-up at load)
        hornAngle = 0.0;
        drumAngle = 0.0;
        hornHz = drumHz = 0.0;
    }

    // Delay of the tube amp's oversampling filters (samples), reported to the host.
    static int latencySamples (int ampOS) { return OversampledShaper::latency (ampOS == 2 ? 4 : (ampOS == 1 ? 2 : 1)); }

    void setParams (const EngineParams& p) { params = p; }

    // ---- MIDI ----------------------------------------------------------------
    // Note on/off as received on MIDI channel ch (0..15). Routing: one channel per manual,
    // or Split (lower | upper, optional pedal zone at the bottom) on the upper channel.
    void noteOn (int note, int vel, int ch)
    {
        if (vel <= 0) { noteOff (note, ch); return; }
        int m = -1, key = -1;
        mapNote (note, ch, m, key);
        if (m < 0) return;
        noteMap[(size_t) (ch & 15)][(size_t) (note & 127)] = (int16_t) (m * kMaxKeys + key);
        keyOn (m, key, vel);
    }

    void noteOff (int note, int ch)
    {
        auto& slot = noteMap[(size_t) (ch & 15)][(size_t) (note & 127)];
        if (slot < 0) return;
        const int m = slot / kMaxKeys, key = slot % kMaxKeys;
        slot = -1;
        keyOff (m, key);
    }

    // Direct key access (on-screen keyboards, tests). m 0 upper, 1 lower, 2 pedals.
    void keyOn (int m, int key, int vel)
    {
        if (m < 0 || m >= kManuals || key < 0 || key >= keyCount (m)) return;
        auto& ks = keys[(size_t) m][(size_t) key];
        if (ks.refs++ > 0) return;   // already held (another note or source)
        ks.down = true;
        // percussion is single-trigger: it fires only when no other upper key is held
        if (m == 0 && downCount[0] == 0) percEnv = 1.0;
        ++downCount[(size_t) m];
        scheduleContacts (m, key, true, vel);
        if (m == 1 && params.manualBass) updateManualBass (vel);
    }

    void keyOff (int m, int key)
    {
        if (m < 0 || m >= kManuals || key < 0 || key >= keyCount (m)) return;
        auto& ks = keys[(size_t) m][(size_t) key];
        if (ks.refs <= 0) return;
        if (--ks.refs > 0) return;
        ks.down = false;
        --downCount[(size_t) m];
        scheduleContacts (m, key, false, 0);
        if (m == 1 && params.manualBass) updateManualBass (100);
    }

    void setController (int cc, double value, int /*ch*/)
    {
        if (cc >= 0 && cc < 128) ccVal[(size_t) cc] = value;
        if (cc == 1)
        {
            modWheel = value;
            // rotor speed from the mod wheel: Half Moon switches on crossing the middle,
            // Momentary is Fast while the wheel is above the middle
            const bool up = value >= 0.5;
            if (params.speedMode != 2)
            {
                if (params.speedWheel == 1 && up != lastWheelFast) speedState = up ? 2 : 1;
                if (params.speedWheel == 2) momentaryFast = up;
            }
            lastWheelFast = up;
        }
        if (params.speedCC > 0 && cc == params.speedCC)
        {
            speedCCValue = value;
            if (params.speedMode == 1) speedState = value < 1.0 / 3.0 ? 0 : (value < 2.0 / 3.0 ? 1 : 2);
            else if (params.speedMode == 0) speedState = value >= 0.5 ? 2 : 1;
        }
        if (cc == 64 && params.speedSustain && params.speedMode != 2) speedState = value >= 0.5 ? 2 : 1;
        if (cc == params.exprCC) expression = value;
        if (cc == 64) sustain = value >= 0.5;
        if (params.motorCC > 0 && cc == params.motorCC) motorCcOff = value >= 0.5;
        if (cc == 120) panic();
        if (cc == 123) allNotesOff();
        if (cc == 121) { modWheel = 0; expression = 1; sustain = false; }
    }

    // expression pedal position 0 (closed) .. 1 (open), e.g. from the on-screen pedal
    void setExpression (double v) { expression = clampd (v, 0.0, 1.0); }

    void setPitchBend (double, int) {}           // an organ has no pitch bend
    void setChannelPressure (double, int) {}
    void setPolyPressure (int, double, int) {}

    void allNotesOff()
    {
        for (auto& row : noteMap) row.fill (-1);
        for (int m = 0; m < kManuals; ++m)
            for (int k = 0; k < keyCount (m); ++k)
                if (keys[(size_t) m][(size_t) k].refs > 0)
                {
                    keys[(size_t) m][(size_t) k].refs = 1;
                    keyOff (m, k);
                }
    }

    // Everything silent now: contacts open, pending events dropped. Wheels keep running.
    void panic() { resetKeys(); }

    // ---- render ----------------------------------------------------------------
    void render (float* L, float* R, int numSamples)
    {
        int pos = 0;
        while (pos < numSamples)
        {
            if (chunkPos == 0) updateControl();
            // apply every contact event due now
            while (heapSize > 0 && events[0].time <= now) applyEvent (popEvent());
            int n = std::min (numSamples - pos, kChunk - chunkPos);
            if (heapSize > 0) n = (int) std::min<int64_t> (n, events[0].time - now);
            renderSamples (L + pos, R != nullptr ? R + pos : nullptr, n);
            pos += n;
            chunkPos = (chunkPos + n) % kChunk;
            now += n;
        }
    }

    // ---- state for the UI / tests ---------------------------------------------
    bool keyDown (int m, int key) const { return keys[(size_t) m][(size_t) key].down; }
    double modWheelValue() const { return modWheel; }
    double expressionValue() const { return expression; }
    bool sustainDown() const { return sustain; }
    double motorValue() const { return motorSpeed; }
    double percussionLevel() const { return percEnv; }
    // rotors: angle in turns (0..1, 0 = facing the front), speed in Hz; speed state 0 stop, 1 slow, 2 fast
    double hornTurns() const { return hornAngle / kTwoPi; }
    double drumTurns() const { return drumAngle / kTwoPi; }
    double hornSpeed() const { return hornHz; }
    double drumSpeed() const { return drumHz; }
    int speedSwitch() const { return momentaryFast ? 2 : speedState; }
    float takePeak() { const float p = peak; peak = 0; return p; }
    int pendingEvents() const { return heapSize + numRamps; }

    // tests
    double wheelHz (int w) const { return wheelBaseHz (w) * freqFactor; }
    double wheelPhase (int w) const { return std::atan2 (ws[(size_t) w], wc[(size_t) w]); }
    const KeyWiring& wiringOf (int m, int key) const { return wiring[(size_t) m][(size_t) key]; }
    int64_t sampleTime() const { return now; }

    static int keyCount (int m) { return m == 2 ? kPedalKeys : kManualKeys; }
    static int busCountOf (int m) { return m == 2 ? 2 : kBuses; }
    static double depthFor (int vibMode) { return 6.0 * (vibMode % 3 + 1); }   // sections swept: 6 / 12 / 18

private:
    struct KeyState
    {
        int refs = 0;
        bool down = false;
        uint32_t gen = 0;
        std::array<double, kContacts> contact {};   // current contact level (0 open .. 1 closed, x velocity)
        std::array<double, kContacts> applied {};   // the gain (taper x Age) the contact was added with
        std::array<int16_t, kContacts> rampSlot { -1, -1, -1, -1, -1, -1, -1, -1, -1 };   // running ramp, or -1
    };

    struct Event
    {
        int64_t time = 0;
        uint32_t seq = 0;
        uint32_t gen = 0;
        int16_t manual = 0, key = 0, contact = 0;
        int ramp = 0;          // samples; 0 = the contact makes or breaks at once (a step)
        double level = 0;
        int shape = 0;         // ramp shape: 0 smoothstep, 1 exponential decay to -60 dB (sustain)
    };

    struct Ramp
    {
        int16_t manual = 0, key = 0, contact = 0;
        double from = 0, to = 0;
        int pos = 0, len = 1;
        int shape = 0;
        double cur = 0, mult = 1;
    };

    // ---- MIDI -> manual / key ----------------------------------------------------
    void mapNote (int note, int ch, int& m, int& key) const
    {
        const auto& p = params;
        m = -1;
        if (p.splitMode)
        {
            if (ch != p.channel[0]) return;
            if (p.pedalZone && note < p.pedalSplit) { m = 2; key = note - p.pedMidiBase + p.transpose; }
            else if (note < p.splitPoint)           { m = 1; key = note - 36 + 12 * p.splitLoOct + p.transpose; }
            else                                    { m = 0; key = note - 36 + p.transpose; }
        }
        else
        {
            for (int i = 0; i < kManuals; ++i)
                if (ch == p.channel[i]) { m = i; break; }
            if (m < 0) return;
            key = m == 2 ? note - p.pedMidiBase + p.transpose : note - 36 + p.transpose;
        }
        if (key < 0 || key >= keyCount (m)) m = -1;
    }

    // Manual bass: the lowest held lower-manual key also plays the pedal of the same name
    // (folded into the 25 pedals), for one-keyboard bass players.
    void updateManualBass (int vel)
    {
        int lowest = -1;
        for (int k = 0; k < kManualKeys; ++k)
            if (keys[1][(size_t) k].refs > 0) { lowest = k; break; }
        int pedal = lowest;
        while (pedal >= kPedalKeys) pedal -= 12;
        if (pedal == coupledPedal) return;
        if (coupledPedal >= 0) keyOff (2, coupledPedal);
        coupledPedal = pedal;
        if (pedal >= 0) keyOn (2, pedal, vel);
    }

    // ---- contacts: the key click ----------------------------------------------------
    // Each of a key's contacts closes (or opens) at its own moment within a few ms, and
    // bounces: it flips open and closed again a few times. Click (0..1) sets how far apart the
    // contacts are, how hard they make (a step, or a smooth ramp of up to 3 ms) and how much they
    // bounce. At Click 0 all contacts ramp smoothly within 0.2 ms of each other: no click.
    // No noise or sample is added at any setting.
    // Sustain (pedals, or the lower manual): on release the contacts fade out exponentially
    // over the sustain time (to -60 dB) instead of opening.
    void scheduleContacts (int m, int key, bool close, int vel)
    {
        auto& ks = keys[(size_t) m][(size_t) key];
        ++ks.gen;
        const auto& kw = wiring[(size_t) m][(size_t) key];
        if (! close && ((m == 2 && params.pedSustain) || (m == 1 && params.loSustain)))
        {
            const int len = std::max (2, (int) ((m == 2 ? params.pedDecay : params.loSusTime) * sr));
            for (int i = 0; i < kw.count; ++i)
                pushEvent ({ now, 0, ks.gen, (int16_t) m, (int16_t) key, (int16_t) i, len, 0.0, 1 });
            return;
        }
        double c = params.click[m];
        if (! close && ! params.keyUpClick) c = 0.0;
        double target = close ? 1.0 : 0.0;
        if (close && params.velMode != 0)
        {
            const double v = 1.0 - params.velAmount + params.velAmount * (double) vel / 127.0;
            if (params.velMode == 1) target = v;   // velocity -> volume
            else c *= v;                          // velocity -> click
        }
        const double msToSmp = sr / 1000.0;
        const double spread = (0.2 + 3.8 * c) * msToSmp;
        const int ramp = (int) ((1.0 - c) * 3.0 * msToSmp);
        for (int i = 0; i < kw.count; ++i)
        {
            const double from = ks.contact[(size_t) i];
            const int64_t t0 = now + (int64_t) (rng.uni() * spread);
            pushEvent ({ t0, 0, ks.gen, (int16_t) m, (int16_t) key, (int16_t) i, ramp, target, 0 });
            int64_t t = t0 + ramp;
            const int bounces = c > 0.0 ? (int) (rng.uni() * (1.0 + 3.0 * c)) : 0;
            for (int b = 0; b < bounces; ++b)
            {
                const int64_t flip = t + 1 + (int64_t) ((0.03 + 0.25 * rng.uni()) * c * msToSmp);
                t = flip + 1 + (int64_t) ((0.03 + 0.25 * rng.uni()) * msToSmp);
                pushEvent ({ flip, 0, ks.gen, (int16_t) m, (int16_t) key, (int16_t) i, 0, from, 0 });
                pushEvent ({ t, 0, ks.gen, (int16_t) m, (int16_t) key, (int16_t) i, 0, target, 0 });
            }
        }
    }

    void applyEvent (const Event& e)
    {
        auto& ks = keys[(size_t) e.manual][(size_t) e.key];
        if (e.gen != ks.gen) return;   // superseded by a later press / release
        if (e.ramp <= 1)
        {
            stopRamp (e.manual, e.key, e.contact);
            setContact (e.manual, e.key, e.contact, e.level);
            return;
        }
        int slot = ks.rampSlot[(size_t) e.contact];
        if (slot < 0)
        {
            if (numRamps >= kMaxRamps) { setContact (e.manual, e.key, e.contact, e.level); return; }
            slot = numRamps++;
            ks.rampSlot[(size_t) e.contact] = (int16_t) slot;
        }
        const double from = ks.contact[(size_t) e.contact];
        ramps[(size_t) slot] = { (int16_t) e.manual, (int16_t) e.key, (int16_t) e.contact, from, e.level, 0, e.ramp,
                                 e.shape, from, e.shape == 1 ? std::exp (-6.907755278982137 / e.ramp) : 1.0 };
    }

    void stopRamp (int m, int key, int contact)
    {
        auto& ks = keys[(size_t) m][(size_t) key];
        const int slot = ks.rampSlot[(size_t) contact];
        if (slot < 0) return;
        ks.rampSlot[(size_t) contact] = -1;
        if (slot != --numRamps)
        {
            ramps[(size_t) slot] = ramps[(size_t) numRamps];
            const auto& r = ramps[(size_t) slot];
            keys[(size_t) r.manual][(size_t) r.key].rampSlot[(size_t) r.contact] = (int16_t) slot;
        }
    }

    // one sample of every running contact ramp: smoothstep (no corner at either end), or an
    // exponential decay for sustain
    void stepRamps()
    {
        for (int i = 0; i < numRamps;)
        {
            auto& r = ramps[(size_t) i];
            ++r.pos;
            const int m = r.manual, key = r.key, contact = r.contact;
            const bool done = r.pos >= r.len;
            double lvl;
            if (done) lvl = r.to;
            else if (r.shape == 1) { r.cur *= r.mult; lvl = r.cur; }
            else
            {
                const double x = (double) r.pos / r.len;
                lvl = r.from + (r.to - r.from) * (x * x * (3.0 - 2.0 * x));
            }
            setContact (m, key, contact, lvl);
            if (done) stopRamp (m, key, contact);   // swaps the last ramp into slot i
            else ++i;
        }
    }

    // A contact's level changes: its wheel moves on its busbar (x taper x Age spread), and so
    // does its leakage (the compartment partner on the same busbar, the same wheel on the
    // neighbouring busbars at half that).
    void setContact (int m, int key, int i, double lvl)
    {
        auto& ks = keys[(size_t) m][(size_t) key];
        const double old = ks.contact[(size_t) i];
        if (old == lvl) return;
        const auto& c = wiring[(size_t) m][(size_t) key].c[(size_t) i];
        const double g = contactGain[(size_t) m][(size_t) key][(size_t) i];
        const double delta = lvl * g - old * ks.applied[(size_t) i];
        ks.contact[(size_t) i] = lvl;
        ks.applied[(size_t) i] = lvl == 0.0 ? 0.0 : g;
        auto& row = busG[(size_t) m][(size_t) c.bus];
        row[(size_t) c.wheel] += delta;
        auto& lk = busLeak[(size_t) m];
        if (const int p = wheelPartner (c.wheel)) lk[(size_t) c.bus][(size_t) p] += delta;
        if (c.bus > 0) lk[(size_t) c.bus - 1][(size_t) c.wheel] += 0.5 * delta;
        if (c.bus + 1 < busCountOf (m)) lk[(size_t) c.bus + 1][(size_t) c.wheel] += 0.5 * delta;
        int& count = busCount[(size_t) m][(size_t) c.bus];
        if (old == 0.0 && lvl != 0.0) { ++count; ++manualContacts[(size_t) m]; }
        else if (old != 0.0 && lvl == 0.0)
        {
            if (--count == 0) row.fill (0.0);   // exact zero again (no rounding residue)
            if (--manualContacts[(size_t) m] == 0) for (auto& r : lk) r.fill (0.0);
        }
    }

    void resetKeys()
    {
        heapSize = 0;
        numRamps = 0;
        for (auto& row : noteMap) row.fill (-1);
        for (auto& man : keys)
            for (auto& k : man) { const auto g = k.gen + 1; k = {}; k.gen = g; }
        for (auto& man : busG) for (auto& row : man) row.fill (0.0);
        for (auto& man : busLeak) for (auto& row : man) row.fill (0.0);
        for (auto& man : busCount) man.fill (0);
        manualContacts.fill (0);
        downCount.fill (0);
        coupledPedal = -1;
    }

    // binary min-heap ordered by (time, seq)
    static bool earlier (const Event& a, const Event& b) { return a.time < b.time || (a.time == b.time && a.seq < b.seq); }

    void pushEvent (Event e)
    {
        e.seq = seq++;
        if (heapSize >= kEventCapacity) { applyEvent (e); return; }   // full (never in practice): apply now
        int i = heapSize++;
        while (i > 0)
        {
            const int parent = (i - 1) / 2;
            if (! earlier (e, events[(size_t) parent])) break;
            events[(size_t) i] = events[(size_t) parent];
            i = parent;
        }
        events[(size_t) i] = e;
    }

    Event popEvent()
    {
        const Event top = events[0];
        const Event last = events[(size_t) --heapSize];
        int i = 0;
        for (;;)
        {
            int child = 2 * i + 1;
            if (child >= heapSize) break;
            if (child + 1 < heapSize && earlier (events[(size_t) child + 1], events[(size_t) child])) ++child;
            if (! earlier (events[(size_t) child], last)) break;
            events[(size_t) i] = events[(size_t) child];
            i = child;
        }
        if (heapSize > 0) events[(size_t) i] = last;
        return top;
    }

    // ---- Age ------------------------------------------------------------------------
    // The generator is shared by both manuals: its wear follows the higher Age setting.
    // Contacts belong to a manual: their level spread follows that manual's Age.
    // A changed Age applies to contacts as they next make or break.
    void updateAgeTables()
    {
        const double ag = std::max (params.age[0], params.age[1]);
        if (ag != genAge)
        {
            genAge = ag;
            for (int w = 1; w <= kNumWheels; ++w)
            {
                level[(size_t) w] = wheelLevel (w) * (ag == 0.0 ? 1.0 : dbToGain (ag * kAgeWheelDb * wheelRnd[(size_t) w]));
                second[(size_t) w] = ag * 0.02 * wheelHarmonicRoom (w);
                third[(size_t) w] = wheelThird (w) + ag * 0.03 * wheelHarmonicRoom (w);
            }
        }
        for (int m = 0; m < kManuals; ++m)
        {
            const double a = m == 2 ? params.age[1] : params.age[(size_t) m];
            if (a == contactAge[(size_t) m]) continue;
            contactAge[(size_t) m] = a;
            for (int k = 0; k < kMaxKeys; ++k)
                for (int i = 0; i < kContacts; ++i)
                {
                    const double r = contactRnd[(size_t) m][(size_t) k][(size_t) i];
                    contactGain[(size_t) m][(size_t) k][(size_t) i] = wiring[(size_t) m][(size_t) k].c[(size_t) i].gain
                                                                      * (a == 0.0 ? 1.0 : dbToGain (a * kAgeContactDb * r));
                }
        }
    }

    // ---- control rate (every 32 samples) --------------------------------------------
    void updateControl()
    {
        const auto& p = params;
        const double dt = kChunk / sr;
        if (p.organModel != builtModel) rebuildModel();

        // Motor. Start spins the generator up; Run keeps it turning once it is up to speed but
        // cannot start it from rest; with both off (or the motor CC) it coasts down: the pitch
        // dive. Pickup output is proportional to wheel speed, so the level dives with the pitch.
        double target = p.motorStart ? 1.0 : (p.motorRun && motorSpeed > 0.25 ? 1.0 : 0.0);
        if (motorCcOff) target = 0.0;
        const double tau = target > motorSpeed ? 0.25 : 1.5;
        motorSpeed += (target - motorSpeed) * (1.0 - std::exp (-dt / tau));
        if (target == 0.0 && motorSpeed < 1e-4) motorSpeed = 0.0;
        if (target == 1.0 && motorSpeed > 1.0 - 1e-9) motorSpeed = 1.0;

        const double ff = p.tuneRatio * mainsRatio (p.mains) * motorSpeed;
        if (ff != freqFactor)
        {
            freqFactor = ff;
            for (int w = 1; w <= kNumWheels; ++w)
            {
                const double inc = kTwoPi * wheelBaseHz (w) * ff / sr;
                rc[(size_t) w] = std::cos (inc);
                rs[(size_t) w] = std::sin (inc);
            }
        }
        // keep each phasor on the unit circle (one Newton step, + - * only)
        for (int w = 1; w <= kNumWheels; ++w)
        {
            const double k = 1.5 - 0.5 * (wc[(size_t) w] * wc[(size_t) w] + ws[(size_t) w] * ws[(size_t) w]);
            wc[(size_t) w] *= k;
            ws[(size_t) w] *= k;
        }
        ampInc = (motorSpeed - ampCur) / kChunk;
        updateAgeTables();

        // percussion: upper manual, drawbar set B only. On, it cancels the 1' bar; at Normal
        // volume it also lowers the drawbars (as on the B-3). Soft = -6 dB.
        const bool percActive = p.percOn && p.presetKey[0] == 11;
        percGain.set (percActive ? (p.percSoft ? kPercSoft : 1.0) : 0.0);
        percBus = p.percThird ? 4 : 3;   // 2 2/3' (third harmonic) or 4' (second)
        const double percT = p.percSlow ? p.percSlowTime : p.percFastTime;
        percMult = std::exp (-6.907755278982137 / (std::max (0.01, percT) * sr));   // -60 dB over percT
        const double upperScale = percActive && ! p.percSoft ? kPercNormalBars : 1.0;

        // drawbars of the active registration on each manual, ramped across the chunk
        for (int m = 0; m < 2; ++m)
        {
            const int pk = p.presetKey[m];
            for (int b = 0; b < kBuses; ++b)
            {
                double pos = 0.0;
                if (pk == 10 || pk == 11) pos = p.bars[m][pk - 10][b];
                else if (pk >= 1 && pk <= 9) pos = p.regs[m][pk - 1][b];
                if (m == 0 && percActive && b == 8) pos = 0.0;   // percussion takes the 1' busbar
                setBarTarget (m, b, drawbarGain (pos) * (m == 0 ? upperScale : 1.0));
            }
        }
        setBarTarget (2, 0, drawbarGain (p.pedalBars[0]));
        setBarTarget (2, 1, drawbarGain (p.pedalBars[1]));
        for (int b = 2; b < kBuses; ++b) setBarTarget (2, b, 0.0);

        // leakage (Age adds to it)
        for (int m = 0; m < kManuals; ++m)
        {
            const int s = m == 2 ? 1 : m;
            leakEff[(size_t) m] = p.leak[s] * (1.0 + p.age[s]) * kLeakBase;
        }
        floorUp = p.leak[0] * (1.0 + p.age[0]);
        floorLo = p.leak[1] * (1.0 + p.age[1]);

        // scanner: turned by the generator motor
        const double rate = Scanner::kRateHz * mainsRatio (p.mains) * motorSpeed;
        if (rate != scanRate) { scanRate = rate; scanner.setRate (rate, sr); }
        scanner.renormalize();
        vibU.set (p.vibUpper ? 1.0 : 0.0);
        vibL.set (p.vibLower ? 1.0 : 0.0);
        vibDepth.set (depthFor (p.vibMode));
        vibChorus.set (p.vibMode >= 3 ? 1.0 : 0.0);

        // pedal tone: a 2-pole low-pass, 120 Hz .. 1.9 kHz
        if (p.pedTone != pedTone)
        {
            pedTone = p.pedTone;
            pedLp.setLowpass (120.0 * std::pow (2.0, 4.0 * clampd (pedTone, 0.0, 1.0)), 0.7071067811865476, sr);
        }

        // expression: smoothed (CCs arrive in 1/127 steps), then ramped per sample by the preamp
        exprSmooth += (expression - exprSmooth) * (1.0 - std::exp (-dt / 0.012));
        // when the expression pedal works the wah, it is no longer the volume pedal
        preamp.setExpression (p.wahOn && p.wahSrc == 1 ? 1.0 : exprSmooth, kChunk);

        if (! p.manualBass && coupledPedal >= 0) { keyOff (2, coupledPedal); coupledPedal = -1; }
        else if (p.manualBass && coupledPedal < 0 && downCount[1] > 0) updateManualBass (100);

        updateCabinet (dt);
    }

    // ---- rotary cabinet, amp, outboard, reverb (control rate) -------------------------
    void updateCabinet (double dt)
    {
        const auto& p = params;
        const auto& M = cabinetModel (p.cabModel);

        // speed: the Speed parameter (UI switch, automation) sets the state when it changes; the
        // mod wheel, the speed CC and the sustain pedal set it in setController(). 2-speed has no Stop.
        if (p.speed != lastSpeedParam) { speedState = p.speed; lastSpeedParam = p.speed; }
        int state = momentaryFast ? 2 : speedState;
        if (p.speedMode == 0 && state == 0) state = 1;
        double variable = 0.0;
        if (p.speedMode == 2)
            variable = clampd (p.varSource == 0 ? modWheel : (p.varSource == 1 ? exprSmooth : speedCCValue), 0.0, 1.0);

        const bool stationary = p.cabBypass || p.rotorConfig == 3 || M.stationary;
        for (int i = 0; i < 2; ++i)
        {
            const bool horn = i == 0;
            const double slow = horn ? p.hornSlow : p.drumSlow, fast = horn ? p.hornFast : p.drumFast;
            const double accel = horn ? p.hornAccel : p.drumAccel, brake = horn ? p.hornBrake : p.drumBrake;
            double target = p.speedMode == 2 ? slow + (fast - slow) * variable
                                             : (state == 2 ? fast : (state == 1 ? slow : 0.0));
            const int sel = horn ? p.hornSel : p.drumSel;   // per-rotor override
            if (sel == 1) target = 0.0;
            else if (sel == 2) target = slow;
            else if (sel == 3) target = fast;
            if (! horn && p.drumOff) target = 0.0;

            double& hz = horn ? hornHz : drumHz;
            double& angle = horn ? hornAngle : drumAngle;
            if (stationary)
            {
                hz = 0.0;
                angle = M.stationary ? (horn ? 0.7 : -0.5) : 0.0;   // tone cabinet: speakers spread; else facing front
            }
            else if (! rotorsStarted) hz = target;
            else
            {
                // inertia: the Accel / Brake times are the time from one speed to within 2% of the other
                const double spread = std::max (0.01, std::abs (fast - slow));
                const double tauUp = accel / std::log (std::max (1.5, spread / (0.02 * std::max (0.01, fast))));
                const double tauDn = brake / std::log (std::max (1.5, spread / (0.02 * std::max (0.01, slow))));
                const double tau = target > hz ? tauUp : tauDn;
                hz += (target - hz) * (1.0 - std::exp (-dt / std::max (0.01, tau)));
                if (target == 0.0 && hz < 1e-4) hz = 0.0;
                // horn brake to a chosen angle: below 0.4 Hz steer onto it
                if (horn && target == 0.0 && p.brakePos && hz < 0.4)
                {
                    const double dir = p.hornDir == 0 ? 1.0 : -1.0;
                    double remaining = dir * (p.brakeAngle * kPi / 180.0 - angle);
                    remaining -= kTwoPi * std::floor (remaining / kTwoPi);
                    if (remaining < 0.01 || remaining > kTwoPi - 0.01) { hz = 0.0; angle = p.brakeAngle * kPi / 180.0; }
                    else hz = std::min (std::max (hz, 0.15), std::max (0.15, 1.5 * remaining / kTwoPi));   // at least 0.15 Hz: arrives within seconds
                }
            }
            const double dir = (horn ? p.hornDir : p.drumDir) == 0 ? 1.0 : -1.0;
            const double inc = dir * kTwoPi * hz / sr;
            cab.setRotor (i, angle, inc);
            angle += inc * kChunk;
            angle -= kTwoPi * std::floor (angle / kTwoPi);
        }
        rotorsStarted = true;

        CabSettings cs;
        cs.model = p.cabModel; cs.rotorConfig = p.rotorConfig; cs.hornType = p.hornType; cs.hornMouth = p.hornMouth;
        cs.drumBaffle = p.drumBaffle; cs.cabBack = p.cabBack; cs.micLayout = p.micLayout; cs.xover = p.xover;
        cs.balance = p.hornDrumBal; cs.micDist = p.micDist; cs.micAngle = p.micAngle; cs.micHorn = p.micHorn;
        cs.micDrum = p.micDrum; cs.micRoom = p.micRoom;
        cab.configure (cs);

        // tube amp: Drive 0..+30 dB on top of the cabinet's own gain; unity small-signal gain
        tubeGain = dbToGain (30.0 * clampd (p.drive, 0.0, 1.0) + M.ampGainDb);
        amp.setBias (M.ampBias);
        ampNorm = 1.0 / (tubeGain * amp.s1.slope());
        ampHpA = onePoleCoef (M.ampHpHz, sr);
        if (M.ampLpHz != ampLpHz) { ampLpHz = M.ampLpHz; ampLp.setLowpass (ampLpHz, 0.7071067811865476, sr); }
        ampFactor = p.ampOS == 2 ? 4 : (p.ampOS == 1 ? 2 : 1);
        ampPowA = 1.0 - std::exp (-1.0 / (0.3 * sr));
        if (ampPin > 1e-12 && ampPout > 1e-12) ampMakeup.set (clampd (std::sqrt (ampPin / ampPout), 1.0, 20.0));
        else ampMakeup.set (ampMakeup.target);   // silence: hold
        if (ampFactor != lastAmpFactor) { amp.reset(); lastAmpFactor = ampFactor; }

        // outboard
        od.set (p.odDrive, p.odTone, p.odLevel, sr);
        odMix.set (p.odOn ? 1.0 : 0.0);
        const double wahPos = p.wahSrc == 1 ? exprSmooth : (p.wahSrc == 2 ? modWheel : (p.wahSrc == 3 ? ccVal[(size_t) clampCC (p.wahCC)] : p.wahPos));
        wah.setPosition (wahPos, sr);
        wahMix.set (p.wahOn ? 1.0 : 0.0);
        echo.set (p.echoTime, p.echoFb, p.echoMix, p.echoWow, p.echoTone);
        echoOnR.set (p.echoOn ? 1.0 : 0.0);

        // reverb: built into the Tone Cabinet and the A-100, plus the optional spring
        const double builtIn = (p.cabBypass ? 0.0 : M.reverbMix) + (p.organModel == 2 ? 0.15 : 0.0);
        revMixR.set (clampd (builtIn + (p.revOn ? p.revMix : 0.0), 0.0, 1.0));
        const double decay = p.revOn ? p.revDecay : 2.2, tone = p.revOn ? p.revTone : 0.5;
        if (decay != revSet[0] || tone != revSet[1]) { revSet = { decay, tone }; rev.set (decay, tone, sr); }
        widthR.set (p.width);
    }

    static int clampCC (int cc) { return cc < 0 ? 0 : (cc > 127 ? 127 : cc); }

    // Organ model changed: re-wire the keyboards (every key is released first).
    void rebuildModel()
    {
        builtModel = params.organModel;
        resetKeys();
        buildManualWiring (wiring[0], builtModel, 0);
        buildManualWiring (wiring[1], builtModel, 1);
        buildPedalWiring (wiring[2], builtModel);
        contactAge[0] = contactAge[1] = contactAge[2] = -1.0;
        updateAgeTables();
    }

    void setBarTarget (int m, int b, double g)
    {
        // the previous ramp has just finished: land on its target exactly
        barCur[(size_t) m][(size_t) b] = barTarget[(size_t) m][(size_t) b];
        barTarget[(size_t) m][(size_t) b] = g;
        barInc[(size_t) m][(size_t) b] = (g - barCur[(size_t) m][(size_t) b]) / kChunk;
    }

    // ---- audio --------------------------------------------------------------------
    void renderSamples (float* L, float* R, int n)
    {
        const double outGain = kOutScale * params.ampGain;
        const bool floorOn = floorUp > 0.0 || floorLo > 0.0;
        std::array<double, kNumWheelSlots> x {};
        for (int i = 0; i < n; ++i)
        {
            if (numRamps > 0) stepRamps();

            // wheels: rotate every phasor one sample; worn / low wheels add 2nd and 3rd harmonics
            const double motorAmp = ampCur;
            ampCur += ampInc;
            double floorSum = 0.0;
            for (int w = 1; w <= kNumWheels; ++w)
            {
                const double c = wc[(size_t) w], s = ws[(size_t) w];
                const double c2 = c * rc[(size_t) w] - s * rs[(size_t) w];
                const double s2 = s * rc[(size_t) w] + c * rs[(size_t) w];
                wc[(size_t) w] = c2;
                ws[(size_t) w] = s2;
                const double h2 = second[(size_t) w], h3 = third[(size_t) w];
                double v = s2;
                if (h2 != 0.0) v += h2 * 2.0 * s2 * c2;
                if (h3 != 0.0) v += h3 * (3.0 * s2 - 4.0 * s2 * s2 * s2);
                x[(size_t) w] = motorAmp * level[(size_t) w] * v;
                floorSum += floorW[(size_t) w] * x[(size_t) w];
            }
            if (! floorOn) floorSum = 0.0;

            // busbars x drawbars, per manual
            double out[kManuals] { 0.0, 0.0, 0.0 };
            double percBusValue = 0.0;
            for (int m = 0; m < kManuals; ++m)
            {
                const double le = leakEff[(size_t) m];
                for (int b = 0; b < kBuses; ++b)
                {
                    const double g = barCur[(size_t) m][(size_t) b];
                    barCur[(size_t) m][(size_t) b] = g + barInc[(size_t) m][(size_t) b];
                    if (busCount[(size_t) m][(size_t) b] == 0) continue;
                    const auto& row = busG[(size_t) m][(size_t) b];
                    double bus = 0.0;
                    if (le == 0.0)
                        for (int w = 1; w <= kNumWheels; ++w) bus += row[(size_t) w] * x[(size_t) w];
                    else
                    {
                        const auto& lk = busLeak[(size_t) m][(size_t) b];
                        for (int w = 1; w <= kNumWheels; ++w) bus += (row[(size_t) w] + le * lk[(size_t) w]) * x[(size_t) w];
                    }
                    if (m == 0 && b == percBus) percBusValue = bus;
                    out[m] += g * bus;
                }
            }
            const double up = out[0] + floorUp * floorSum, lo = out[1] + floorLo * floorSum;

            // percussion: the 2nd / 3rd harmonic busbar through a decaying envelope
            const double perc = percGain.next() * percEnv * percBusValue;
            percEnv *= percMult;

            // scanner vibrato / chorus on the switched manuals
            const double vu = vibU.next(), vl = vibL.next();
            const double vibIn = up * vu + lo * vl;
            const double dry = up * (1.0 - vu) + lo * (1.0 - vl);
            const double cm = vibChorus.next();
            const double scanned = scanner.process (vibIn, vibDepth.next());
            const double vib = scanned * (1.0 - 0.5 * cm) + vibIn * 0.5 * cm;

            const double pd = pedLp.process (out[2]);
            double sig = preamp.process (dry + vib + perc + pd);

            // outboard pedals (each crossfades in and out over one chunk). The analog stages work at
            // a calibrated level: kStage = one key with one drawbar at 8 (a full organ chord is ~15x that).
            sig += odMix.next() * (od.process (sig * kStage) / kStage - sig);
            sig += wahMix.next() * (wah.process (sig) - sig);
            sig += echoOnR.next() * (echo.process (sig * kStage) / kStage - sig);

            // rotary cabinet: tube amp -> rotors -> mics
            double l, r;
            if (params.cabBypass) l = r = sig;
            else
            {
                ampHp += ampHpA * (sig - ampHp);
                const double in = sig - ampHp;
                const double raw = amp.process (tubeGain * kStage * in, ampFactor) * ampNorm / kStage;
                // level matching (as Brass Knuckles' drive): input vs output power over ~300 ms, boost only
                ampPin += ampPowA * (in * in - ampPin);
                ampPout += ampPowA * (raw * raw - ampPout);
                const double a = ampLp.process (raw * ampMakeup.next());
                cab.process (a, l, r);
            }

            // spring reverb, stereo width
            double wl, wr;
            rev.process (0.5 * (l + r), wl, wr);
            const double rm = revMixR.next();
            l += rm * wl;
            r += rm * wr;
            const double wd = widthR.next();
            const double mid = 0.5 * (l + r), side = 0.5 * (l - r) * wd;
            l = mid + side;
            r = mid - side;

            masterGain += (params.master - masterGain) * kMasterSmooth;
            const double g = outGain * masterGain;
            if (R != nullptr)
            {
                const float fl = (float) safetyClip (l * g), fr = (float) safetyClip (r * g);
                L[i] = fl;
                R[i] = fr;
                peak = std::max (peak, std::max (std::abs (fl), std::abs (fr)));
            }
            else
            {
                const float f = (float) safetyClip (0.5 * (l + r) * g);
                L[i] = f;
                peak = std::max (peak, std::abs (f));
            }
        }
    }

    static constexpr double kMasterSmooth = 0.002;

    EngineParams params;
    double sr = 48000.0;
    Rng rng;

    // generator
    std::array<double, kNumWheelSlots> wc {}, ws {}, rc {}, rs {}, level {}, second {}, third {}, wheelRnd {}, floorW {};
    double motorSpeed = 1.0, freqFactor = -1.0, ampCur = 0.0, ampInc = 0.0;
    double genAge = -1.0;
    std::array<double, kManuals> contactAge {};
    bool motorCcOff = false;

    // keys, contacts, busbars
    std::array<std::array<KeyWiring, kMaxKeys>, kManuals> wiring {};
    std::array<std::array<std::array<double, kContacts>, kMaxKeys>, kManuals> contactRnd {}, contactGain {};
    std::array<std::array<KeyState, kMaxKeys>, kManuals> keys {};
    std::array<std::array<std::array<double, kNumWheelSlots>, kBuses>, kManuals> busG {}, busLeak {};
    std::array<std::array<int, kBuses>, kManuals> busCount {};
    std::array<int, kManuals> manualContacts {}, downCount {};
    std::array<std::array<double, kBuses>, kManuals> barCur {}, barTarget {}, barInc {};
    std::array<std::array<int16_t, 128>, 16> noteMap {};
    std::array<double, kManuals> leakEff {};
    double floorUp = 0.0, floorLo = 0.0;
    int coupledPedal = -1;

    // contact events and running contact ramps
    static constexpr int kMaxRamps = 2048;   // >= every contact of every key (147 x 9)
    std::vector<Event> events;
    int heapSize = 0;
    std::array<Ramp, kMaxRamps> ramps {};
    int numRamps = 0;
    uint32_t seq = 0;
    int64_t now = 0;
    int chunkPos = 0;

    // voice
    double percEnv = 0.0, percMult = 1.0;
    int percBus = 4;
    ChunkRamp percGain, vibU, vibL, vibDepth, vibChorus;
    Scanner scanner;
    double scanRate = -1.0;
    Biquad pedLp;
    double pedTone = -1.0;
    Preamp preamp;

    // cabinet, amp, outboard, reverb
    OversampledShaper amp;
    double ampHp = 0.0, ampHpA = 0.0, tubeGain = 1.0, ampNorm = 1.0, ampLpHz = -1.0;
    double ampPin = 0.0, ampPout = 0.0, ampPowA = 0.0;
    ChunkRamp ampMakeup;
    Biquad ampLp;
    int ampFactor = 2, lastAmpFactor = -1;
    RotaryCabinet cab;
    Overdrive od;
    Wah wah;
    TapeEcho echo;
    SpringReverb rev;
    std::array<double, 2> revSet {};
    ChunkRamp odMix, wahMix, echoOnR, revMixR, widthR;
    std::array<double, 128> ccVal {};
    int speedState = 1, lastSpeedParam = 1;
    bool momentaryFast = false, lastWheelFast = false, rotorsStarted = false;
    double speedCCValue = 0.0, hornAngle = 0.0, drumAngle = 0.0, hornHz = 0.0, drumHz = 0.0;
    int builtModel = 0;

    // controllers / output
    double expression = 1.0, exprSmooth = 1.0, modWheel = 0.0;
    bool sustain = false;
    double masterGain = 0.7;
    float peak = 0.0f;
};
} // namespace ham
