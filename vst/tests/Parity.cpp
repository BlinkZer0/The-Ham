// --parity <scenarios.json>: renders every scenario with the C++ engine and prints the
// results as JSON. tests/run-parity.ps1 renders the same scenarios with the JS engine
// (vst/webhost/dsp-worklet.js) in headless Edge and compares the two.
//
// Scenario format (tests/parity_scenarios.json):
//   { "name": "...", "preset": "Category/Name"?, "params": { id: value }?, "blocks": N,
//     "events": [ [block, type, args...], ... ] }
// Blocks are 128 samples at 48 kHz. Events are applied at the start of their block, after the
// block's parameter snapshot (as in the plugin). Types:
//   on note vel [ch] · off note [ch] · cc num value(0..1) [ch] · manual m key vel (vel 0 = release:
//   the on-screen keyboards) · expression value (the on-screen pedal) · param id value (a parameter
//   change, from this block on) · presetKey m key · motor start run · speed 0|1|2 ·
//   midi [status, d1, d2] (through the plugin's MIDI routing) · allOff · panic
// Output per scenario: RMS of each channel in 16 equal windows, the peak, and the render time.
#include "TestRender.h"
#include "MidiRouting.h"

using namespace ham;
using namespace ham::test;

namespace
{
constexpr int kWindows = 16;

const PresetData* findFactory (const PresetManager& pm, const juce::String& spec)
{
    for (const auto& p : pm.factory())
        if (p.category + "/" + p.name == spec || p.name == spec) return &p;
    return nullptr;
}

int argInt (const juce::var& ev, int i, int def = 0) { return ev.size() > i ? (int) ev[i] : def; }
double argNum (const juce::var& ev, int i, double def = 0) { return ev.size() > i ? (double) ev[i] : def; }

// The scenario's parameter state: raw values (floats, as the plugin's) and the engine snapshot made from them.
struct Scenario
{
    Values values = defaultValues();
    Registrations regs = factoryRegistrations();
    EngineParams P;

    void rebuild()
    {
        P = makeEngineParams ([this] (int i) { return values[(size_t) i]; });
        for (int m = 0; m < 2; ++m)
            for (int k = 0; k < 9; ++k)
                for (int f = 0; f < kNumFootages; ++f) P.regs[m][k][f] = regs[(size_t) m][(size_t) k][(size_t) f];
    }

    bool set (SynthEngine& e, const juce::String& id, double v)
    {
        const int i = paramIndex (id);
        if (i < 0) { std::printf ("unknown param %s\n", id.toRawUTF8()); return false; }
        values[(size_t) i] = (float) v;
        rebuild();
        e.setParams (P);   // at once, as the JS processor applies pending values before its next message
        return true;
    }
};

bool applyScenarioEvent (SynthEngine& e, const juce::var& ev, Scenario& s)
{
    const auto type = ev[1].toString();
    if (type == "on")              e.noteOn (argInt (ev, 2), argInt (ev, 3), argInt (ev, 4));
    else if (type == "off")        e.noteOff (argInt (ev, 2), argInt (ev, 3));
    else if (type == "cc")         e.setController (argInt (ev, 2), argNum (ev, 3), argInt (ev, 4));
    else if (type == "manual")
    {
        const int vel = argInt (ev, 4);
        if (vel > 0) e.keyOn (argInt (ev, 2), argInt (ev, 3), vel);
        else e.keyOff (argInt (ev, 2), argInt (ev, 3));
    }
    else if (type == "expression") e.setExpression (argNum (ev, 2));
    else if (type == "param")      return s.set (e, ev[2].toString(), argNum (ev, 3));
    else if (type == "presetKey")  return s.set (e, argInt (ev, 2) == 0 ? "upKey" : "loKey", argNum (ev, 3));
    else if (type == "motor")      return s.set (e, "motorStart", argNum (ev, 2)) && s.set (e, "motorRun", argNum (ev, 3));
    else if (type == "speed")      return s.set (e, "speed", argNum (ev, 2));
    else if (type == "allOff")     e.allNotesOff();
    else if (type == "panic")      e.panic();
    else if (type == "midi")
    {
        const auto& d = ev[2];
        const auto msg = d.size() >= 3 ? juce::MidiMessage ((int) d[0], (int) d[1], (int) d[2])
                                       : juce::MidiMessage ((int) d[0], (int) d[1]);
        routeMidi (e, msg);
    }
    else { std::printf ("unknown event type %s\n", type.toRawUTF8()); return false; }
    return true;
}
} // namespace

int runParity (const juce::File& scenarioFile)
{
    const auto json = juce::JSON::parse (scenarioFile);
    if (! json.isArray()) { std::printf ("cannot read %s\n", scenarioFile.getFullPathName().toRawUTF8()); return 2; }

    PresetManager pm;
    juce::Array<juce::var> results;

    for (const auto& sc : *json.getArray())
    {
        Scenario s;
        if (sc.hasProperty ("preset"))
        {
            const auto* p = findFactory (pm, sc["preset"].toString());
            if (p == nullptr) { std::printf ("unknown preset %s\n", sc["preset"].toString().toRawUTF8()); return 2; }
            applyPresetData (s.values, *p);
            s.regs = p->registrations;
        }
        if (auto* obj = sc["params"].getDynamicObject())
            for (const auto& prop : obj->getProperties())
            {
                const int i = paramIndex (prop.name.toString());
                if (i < 0) { std::printf ("unknown param %s\n", prop.name.toString().toRawUTF8()); return 2; }
                s.values[(size_t) i] = (float) (double) prop.value;
            }
        s.rebuild();

        const int blocks = (int) sc["blocks"];
        const auto* events = sc["events"].getArray();

        auto engine = std::make_unique<SynthEngine>();   // fresh per scenario, like the JS runner (too big for the stack)
        engine->setParams (s.P);                         // parameters first, then prepare (as the plugin does)
        engine->prepare (kSr);
        std::array<float, kBlock> L {}, R {};
        const int per = blocks * kBlock / kWindows;
        double sl[kWindows] {}, sr[kWindows] {}, peak = 0;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < blocks; ++b)
        {
            engine->setParams (s.P);
            if (events != nullptr)
                for (const auto& ev : *events)
                    if ((int) ev[0] == b && ! applyScenarioEvent (*engine, ev, s)) return 2;
            engine->render (L.data(), R.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                const int w = std::min (kWindows - 1, (b * kBlock + i) / std::max (per, 1));
                sl[w] += (double) L[(size_t) i] * L[(size_t) i];
                sr[w] += (double) R[(size_t) i] * R[(size_t) i];
                peak = std::max (peak, (double) std::max (std::abs (L[(size_t) i]), std::abs (R[(size_t) i])));
            }
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;

        juce::Array<juce::var> rl, rr;
        for (int w = 0; w < kWindows; ++w)
        {
            rl.add (std::sqrt (sl[w] / std::max (per, 1)));
            rr.add (std::sqrt (sr[w] / std::max (per, 1)));
        }
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", sc["name"]);
        o->setProperty ("rmsL", rl);
        o->setProperty ("rmsR", rr);
        o->setProperty ("peak", peak);
        o->setProperty ("ms", ms);
        o->setProperty ("audioMs", blocks * kBlock / kSr * 1000.0);
        results.add (juce::var (o));
    }
    std::printf ("%s\n", juce::JSON::toString (juce::var (results), true, 9).toRawUTF8());
    return 0;
}
