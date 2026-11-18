#pragma once
// ============================================================================
//  The Ham engine. Header-only, no JUCE, no allocation after prepare(), no locks.
//  Double precision. Line-for-line twin (phase 6): vst/webhost/dsp-worklet.js.
//
//  Signal path (phase 1):
//    motor (Start/Run, mains, tuning, pitch dive) -> 91 tonewheels (rotating phasors,
//    always running, never reset by keys) -> key contacts (sample-accurate, with bounce:
//    the key click) -> busbars (per manual: 9, pedals: 2) -> drawbars (32-sample ramps)
//    -> sum -> patch level x master -> safety clip -> L = R.
//
//  Not a voice synth: there is no voice allocation. A key closes up to 9 contacts, each
//  adding one wheel (x its taper) to one busbar. The busbar gains busG[m][b][w] change only
//  at contact events; the wheels run all the time.
// ============================================================================
#include <vector>
#include "dsp/Common.h"
#include "dsp/Tonewheels.h"
#include "dsp/Wiring.h"

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
    double click[3] { 0.5, 0.5, 0.3 };
    bool keyUpClick = true;
    int channel[3] { 0, 1, 2 };      // MIDI channel index per manual
    bool splitMode = false;
    int splitPoint = 60, splitLoOct = 0;
    bool pedalZone = false;
    int pedalSplit = 48, pedMidiBase = 36;
    int motorCC = 0, exprCC = 11;
};

class SynthEngine
{
public:
    static constexpr double kOutScale = 0.06;   // one 8' key at full drawbar -> about -27 dBFS RMS

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
            level[(size_t) w] = wheelLevel (w);
            third[(size_t) w] = wheelThird (w);
        }
        buildManualWiring (wiring[0], params.organModel);
        buildManualWiring (wiring[1], params.organModel);
        buildPedalWiring (wiring[2]);
        motorSpeed = 1.0;      // a freshly loaded organ is already running
        ampCur = ampInc = 0.0;
        freqFactor = -1.0;     // forces the rotation coefficients to be computed
        masterGain = params.master;
        now = 0;
        chunkPos = 0;
        seq = 0;
        resetKeys();
        expression = 1.0;
        modWheel = 0.0;
        sustain = false;
        motorCcOff = false;
        peak = 0.0f;
    }

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
        scheduleContacts (m, key, true, vel);
    }

    void keyOff (int m, int key)
    {
        if (m < 0 || m >= kManuals || key < 0 || key >= keyCount (m)) return;
        auto& ks = keys[(size_t) m][(size_t) key];
        if (ks.refs <= 0) return;
        if (--ks.refs > 0) return;
        ks.down = false;
        scheduleContacts (m, key, false, 0);
    }

    void setController (int cc, double value, int /*ch*/)
    {
        if (cc == 1) modWheel = value;
        if (cc == params.exprCC) expression = value;
        if (cc == 64) sustain = value >= 0.5;
        if (params.motorCC > 0 && cc == params.motorCC) motorCcOff = value >= 0.5;
        if (cc == 120) panic();
        if (cc == 123) allNotesOff();
        if (cc == 121) { modWheel = 0; expression = 1; sustain = false; }
    }

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
    float takePeak() { const float p = peak; peak = 0; return p; }
    int pendingEvents() const { return heapSize + numRamps; }

    // tests
    double wheelHz (int w) const { return wheelBaseHz (w) * freqFactor; }
    double wheelPhase (int w) const { return std::atan2 (ws[(size_t) w], wc[(size_t) w]); }
    const KeyWiring& wiringOf (int m, int key) const { return wiring[(size_t) m][(size_t) key]; }
    int64_t sampleTime() const { return now; }

    static int keyCount (int m) { return m == 2 ? kPedalKeys : kManualKeys; }

private:
    struct KeyState
    {
        int refs = 0;
        bool down = false;
        uint32_t gen = 0;
        std::array<double, kContacts> contact {};   // current contact level (0 open .. 1 closed, x velocity)
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
    };

    struct Ramp
    {
        int16_t manual = 0, key = 0, contact = 0;
        double from = 0, to = 0;
        int pos = 0, len = 1;
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

    // ---- contacts: the key click ----------------------------------------------------
    // Each of a key's contacts closes (or opens) at its own moment within a few ms, and
    // bounces: it flips open and closed again a few times. Click (0..1) sets how far apart the
    // contacts are, how hard they make (a step, or a smooth ramp of up to 3 ms) and how much they
    // bounce. At Click 0 all contacts ramp smoothly within 0.2 ms of each other: no click.
    // No noise or sample is added at any setting.
    void scheduleContacts (int m, int key, bool close, int vel)
    {
        auto& ks = keys[(size_t) m][(size_t) key];
        ++ks.gen;
        const auto& kw = wiring[(size_t) m][(size_t) key];
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
            pushEvent ({ t0, 0, ks.gen, (int16_t) m, (int16_t) key, (int16_t) i, ramp, target });
            int64_t t = t0 + ramp;
            const int bounces = c > 0.0 ? (int) (rng.uni() * (1.0 + 3.0 * c)) : 0;
            for (int b = 0; b < bounces; ++b)
            {
                const int64_t flip = t + 1 + (int64_t) ((0.03 + 0.25 * rng.uni()) * c * msToSmp);
                t = flip + 1 + (int64_t) ((0.03 + 0.25 * rng.uni()) * msToSmp);
                pushEvent ({ flip, 0, ks.gen, (int16_t) m, (int16_t) key, (int16_t) i, 0, from });
                pushEvent ({ t, 0, ks.gen, (int16_t) m, (int16_t) key, (int16_t) i, 0, target });
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
        ramps[(size_t) slot] = { (int16_t) e.manual, (int16_t) e.key, (int16_t) e.contact,
                                 ks.contact[(size_t) e.contact], e.level, 0, e.ramp };
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

    // one sample of every running contact ramp (smoothstep: no corner at either end)
    void stepRamps()
    {
        for (int i = 0; i < numRamps;)
        {
            auto& r = ramps[(size_t) i];
            ++r.pos;
            const double x = (double) r.pos / r.len;
            const double s = x * x * (3.0 - 2.0 * x);
            const int m = r.manual, key = r.key, contact = r.contact;
            const bool done = r.pos >= r.len;
            setContact (m, key, contact, done ? r.to : r.from + (r.to - r.from) * s);
            if (done) stopRamp (m, key, contact);   // swaps the last ramp into slot i
            else ++i;
        }
    }

    void setContact (int m, int key, int i, double lvl)
    {
        auto& ks = keys[(size_t) m][(size_t) key];
        const double old = ks.contact[(size_t) i];
        if (old == lvl) return;
        const auto& c = wiring[(size_t) m][(size_t) key].c[(size_t) i];
        ks.contact[(size_t) i] = lvl;
        auto& row = busG[(size_t) m][(size_t) c.bus];
        row[(size_t) c.wheel] += (lvl - old) * c.gain;
        int& count = busCount[(size_t) m][(size_t) c.bus];
        if (old == 0.0 && lvl != 0.0) ++count;
        else if (old != 0.0 && lvl == 0.0 && --count == 0)
            row.fill (0.0);   // exact zero again (no rounding residue)
    }

    void resetKeys()
    {
        heapSize = 0;
        numRamps = 0;
        for (auto& row : noteMap) row.fill (-1);
        for (auto& man : keys)
            for (auto& k : man) { const auto g = k.gen + 1; k = {}; k.gen = g; }
        for (auto& man : busG) for (auto& row : man) row.fill (0.0);
        for (auto& man : busCount) man.fill (0);
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

    // ---- control rate (every 32 samples) --------------------------------------------
    void updateControl()
    {
        const auto& p = params;
        const double dt = kChunk / sr;

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

        // drawbars of the active registration on each manual, ramped across the chunk
        for (int m = 0; m < 2; ++m)
        {
            const int pk = p.presetKey[m];
            for (int b = 0; b < kBuses; ++b)
            {
                double pos = 0.0;
                if (pk == 10 || pk == 11) pos = p.bars[m][pk - 10][b];
                else if (pk >= 1 && pk <= 9) pos = kFactoryRegistrations[pk - 1][b];
                setBarTarget (m, b, drawbarGain (pos));
            }
        }
        setBarTarget (2, 0, drawbarGain (p.pedalBars[0]));
        setBarTarget (2, 1, drawbarGain (p.pedalBars[1]));
        for (int b = 2; b < kBuses; ++b) setBarTarget (2, b, 0.0);
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
        std::array<double, kNumWheelSlots> x {};
        for (int i = 0; i < n; ++i)
        {
            if (numRamps > 0) stepRamps();

            // wheels: rotate every phasor one sample; the low wheels add their 3rd harmonic
            const double amp = ampCur;
            ampCur += ampInc;
            for (int w = 1; w <= kNumWheels; ++w)
            {
                const double c = wc[(size_t) w], s = ws[(size_t) w];
                const double c2 = c * rc[(size_t) w] - s * rs[(size_t) w];
                const double s2 = s * rc[(size_t) w] + c * rs[(size_t) w];
                wc[(size_t) w] = c2;
                ws[(size_t) w] = s2;
                const double h = third[(size_t) w];
                x[(size_t) w] = amp * level[(size_t) w] * (h == 0.0 ? s2 : s2 + h * (3.0 * s2 - 4.0 * s2 * s2 * s2));
            }

            // busbars x drawbars
            double sum = 0.0;
            for (int m = 0; m < kManuals; ++m)
                for (int b = 0; b < kBuses; ++b)
                {
                    const double g = barCur[(size_t) m][(size_t) b];
                    barCur[(size_t) m][(size_t) b] = g + barInc[(size_t) m][(size_t) b];
                    if (busCount[(size_t) m][(size_t) b] == 0) continue;
                    const auto& row = busG[(size_t) m][(size_t) b];
                    double bus = 0.0;
                    for (int w = 1; w <= kNumWheels; ++w) bus += row[(size_t) w] * x[(size_t) w];
                    sum += g * bus;
                }

            masterGain += (params.master - masterGain) * kMasterSmooth;
            const double y = safetyClip (sum * outGain * masterGain);
            const float f = (float) y;
            L[i] = f;
            if (R != nullptr) R[i] = f;
            peak = std::max (peak, std::abs (f));
        }
    }

    static constexpr double kMasterSmooth = 0.002;

    EngineParams params;
    double sr = 48000.0;
    Rng rng;

    // generator
    std::array<double, kNumWheelSlots> wc {}, ws {}, rc {}, rs {}, level {}, third {};
    double motorSpeed = 1.0, freqFactor = -1.0, ampCur = 0.0, ampInc = 0.0;
    bool motorCcOff = false;

    // keys, contacts, busbars
    std::array<std::array<KeyWiring, kMaxKeys>, kManuals> wiring {};
    std::array<std::array<KeyState, kMaxKeys>, kManuals> keys {};
    std::array<std::array<std::array<double, kNumWheelSlots>, kBuses>, kManuals> busG {};
    std::array<std::array<int, kBuses>, kManuals> busCount {};
    std::array<std::array<double, kBuses>, kManuals> barCur {}, barTarget {}, barInc {};
    std::array<std::array<int16_t, 128>, 16> noteMap {};

    // contact events and running contact ramps
    static constexpr int kMaxRamps = 2048;   // >= every contact of every key (147 x 9)
    std::vector<Event> events;
    int heapSize = 0;
    std::array<Ramp, kMaxRamps> ramps {};
    int numRamps = 0;
    uint32_t seq = 0;
    int64_t now = 0;
    int chunkPos = 0;

    // controllers / output
    double expression = 1.0, modWheel = 0.0;
    bool sustain = false;
    double masterGain = 0.7;
    float peak = 0.0f;
};
} // namespace ham
