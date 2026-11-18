// Engine tools for the console test app:
//   --dump-params                 the parameter table as JSON (tests/check-params.ps1 compares it with params.js)
//   --render <preset> <out.wav> [phrase]   offline render; preset = index or "Category/Name"
//                                 phrases: demo (default), chord, gliss, click
//   --render-all <folder>         every factory preset, demo phrase, one WAV each
//   --loudness                    loudness of every factory preset (level matching)
//   --bench                       worst-case CPU
#include "TestRender.h"

using namespace ham;
using namespace ham::test;

namespace
{
const char* kindName (Kind k)
{
    switch (k) { case Kind::Bool: return "bool"; case Kind::Choice: return "choice"; case Kind::Int: return "int"; default: return "float"; }
}

const char* unitName (Unit u)
{
    switch (u)
    {
        case Unit::Seconds: return "s";       case Unit::Hz: return "Hz";        case Unit::LfoHz: return "lfohz";
        case Unit::Semitones: return "st";    case Unit::Cents: return "ct";     case Unit::Percent: return "%";
        case Unit::BipolarPercent: return "bip"; case Unit::Decibels: return "dB"; case Unit::Degrees: return "deg";
        case Unit::Ms: return "ms";           case Unit::Count: return "count";  case Unit::Note: return "note";
        case Unit::Drawbar: return "bar";     case Unit::Octaves: return "oct";  default: return "";
    }
}

const PresetData* findPreset (const PresetManager& pm, const juce::String& spec)
{
    const auto& f = pm.factory();
    if (spec.containsOnly ("0123456789"))
    {
        const int i = spec.getIntValue();
        return i >= 0 && i < (int) f.size() ? &f[(size_t) i] : nullptr;
    }
    for (const auto& p : f)
        if (p.category + "/" + p.name == spec || p.name == spec) return &p;
    return nullptr;
}

// Audition phrases (times in seconds). Upper manual = channel 1, lower = 2, pedals = 3, as
// the default MIDI setup; keys given as MIDI notes (manual key 0 = MIDI 36, pedal key 0 = MIDI 36).
std::vector<Event> phrase (const juce::String& name, double& length)
{
    std::vector<Event> ev;
    auto on  = [&ev] (double t, int note, int ch) { ev.push_back ({ sec (t), Event::On, note, 100.0, ch }); };
    auto off = [&ev] (double t, int note, int ch) { ev.push_back ({ sec (t), Event::Off, note, 0.0, ch }); };

    if (name == "chord")
    {
        for (int n : { 60, 64, 67 }) { on (0.1, n, 0); off (2.1, n, 0); }
        length = 2.6;
    }
    else if (name == "gliss")   // white-key smear down and up the upper manual
    {
        const int white[] { 0, 2, 4, 5, 7, 9, 11 };
        double t = 0.1;
        for (int o = 4; o >= 0; --o) for (int i = 6; i >= 0; --i) { const int n = 36 + o * 12 + white[i]; on (t, n, 0); off (t + 0.02, n, 0); t += 0.012; }
        t += 0.3;
        for (int o = 0; o < 5; ++o) for (int i = 0; i < 7; ++i) { const int n = 36 + o * 12 + white[i]; on (t, n, 0); off (t + 0.02, n, 0); t += 0.012; }
        on (t, 96, 0); off (t + 1.0, 96, 0);
        length = t + 1.5;
    }
    else if (name == "click")   // one low key, repeated, to hear the contacts
    {
        for (int i = 0; i < 6; ++i) { on (0.1 + i * 0.5, 48, 0); off (0.35 + i * 0.5, 48, 0); }
        length = 3.3;
    }
    else                        // "demo": a short comp on the upper manual over lower chords and pedal bass
    {
        const int chords[4][3] { { 60, 64, 67 }, { 62, 65, 69 }, { 59, 62, 67 }, { 60, 64, 67 } };
        const int bass[4] { 36, 38, 43, 36 };
        for (int c = 0; c < 4; ++c)
        {
            const double t = 0.1 + c * 1.0;
            for (int n : chords[c]) { on (t, n - 12, 1); off (t + 0.95, n - 12, 1); }
            on (t, bass[c], 2); off (t + 0.45, bass[c], 2);
            for (int k = 0; k < 3; ++k) { const int n = chords[c][k] + 12; on (t + 0.5 + k * 0.12, n, 0); off (t + 0.5 + k * 0.12 + 0.1, n, 0); }
        }
        for (int n : { 72, 76, 79, 84 }) { on (4.2, n, 0); off (5.4, n, 0); }
        length = 6.0;
    }
    return ev;
}
} // namespace

int runDumpParams()
{
    juce::Array<juce::var> list;
    for (const auto& d : paramDefs())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", d.id);
        o->setProperty ("type", kindName (d.kind));
        o->setProperty ("min", d.min);
        o->setProperty ("max", d.max);
        o->setProperty ("def", d.def);
        o->setProperty ("unit", unitName (d.unit));
        o->setProperty ("log", d.logSkew);
        o->setProperty ("noPreset", d.noPreset);
        juce::Array<juce::var> ch;
        for (const auto& c : d.choices) ch.add (c);
        o->setProperty ("choices", ch);
        list.add (juce::var (o));
    }
    std::printf ("%s\n", juce::JSON::toString (juce::var (list), true).toRawUTF8());
    return 0;
}

int runRender (const juce::String& spec, const juce::File& out, const juce::String& phraseName)
{
    PresetManager pm;
    const auto* p = findPreset (pm, spec);
    if (p == nullptr) { std::printf ("unknown preset %s\n", spec.toRawUTF8()); return 2; }
    const auto v = presetValues (*p);
    double len = 0;
    const auto ev = phrase (phraseName, len);
    const auto a = render (v, ev, len);
    if (! writeWav (out, a)) { std::printf ("cannot write %s\n", out.getFullPathName().toRawUTF8()); return 2; }
    std::printf ("%s/%s -> %s  (%.1f s, peak %.3f)\n", p->category.toRawUTF8(), p->name.toRawUTF8(),
                 out.getFullPathName().toRawUTF8(), len, peak (a));
    return 0;
}

int runRenderAll (const juce::File& folder)
{
    PresetManager pm;
    folder.createDirectory();
    int i = 0;
    for (const auto& p : pm.factory())
    {
        const auto file = folder.getChildFile (juce::String (i++).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (p.category + " - " + p.name) + ".wav");
        double len = 0;
        const auto a = render (presetValues (p), phrase ("demo", len), len);
        writeWav (file, a);
        std::printf ("%s\n", file.getFileName().toRawUTF8());
    }
    return 0;
}

// Loudness of a preset: upper C-E-G chord + lower chord + pedal C, loudest 100 ms RMS, at
// master 0.05 (so the safety clipper never acts), scaled back to master 1.
double presetLoudness (const PresetData& p)
{
    auto v = presetValues (p, 0.05f);
    std::vector<Event> ev;
    for (int n : { 60, 64, 67 }) ev.push_back ({ sec (0.05), Event::On, n, 100.0, 0 });
    for (int n : { 48, 52, 55 }) ev.push_back ({ sec (0.05), Event::On, n, 100.0, 1 });
    ev.push_back ({ sec (0.05), Event::On, 36, 100.0, 2 });
    const auto a = render (v, ev, 1.5);
    double best = 0;
    for (int w = 0; w + sec (0.1) <= a.size(); w += sec (0.05)) best = std::max (best, rms (a, w, w + sec (0.1)));
    return best / 0.05;
}

int runLoudness()
{
    PresetManager pm;
    std::printf ("[\n");
    const auto& f = pm.factory();
    for (size_t pi = 0; pi < f.size(); ++pi)
    {
        auto v = presetValues (f[pi], 0.05f);
        const double loud = presetLoudness (f[pi]);
        const double withoutAmp = loud / std::pow (10.0, v[ampLevel] / 20.0);
        std::printf ("  { \"index\": %d, \"name\": \"%s\", \"category\": \"%s\", \"loudness\": %.6f, \"loudnessDb\": %.2f, \"loudnessNoAmp\": %.6f, \"ampLevel\": %.2f }%s\n",
                     (int) pi, f[pi].name.toRawUTF8(), f[pi].category.toRawUTF8(), loud, 20 * std::log10 (std::max (loud, 1e-9)),
                     withoutAmp, v[ampLevel], pi + 1 < f.size() ? "," : "");
    }
    std::printf ("]\n");
    return 0;
}

// Worst case: every key of both manuals and the pedals held, all drawbars out, click on.
// Grows each phase (percussion + scanner in phase 2, cabinet at 4x in phase 3).
int runBench()
{
    auto v = defaultValues();
    for (int m = 0; m < 2; ++m) for (int s = 0; s < 2; ++s) setBars (v, m, s, "888888888");
    v[pd16] = 8; v[pd8] = 8;
    std::vector<Event> ev;
    for (int m = 0; m < 3; ++m)
        for (int k = 0; k < SynthEngine::keyCount (m); ++k) ev.push_back ({ sec (0.01) + k * 7, Event::Key, k, 100.0, m });
    const double seconds = 10.0;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto a = render (v, ev, seconds);
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    std::printf ("bench: 61 + 61 + 25 keys held, all drawbars: %.1f%% of one core (%.0f ms for %.0f s of audio), peak %.2f\n",
                 100.0 * ms / (seconds * 1000.0), ms, seconds, peak (a));

    // one key only, for comparison (the generator always runs)
    std::vector<Event> one { { sec (0.01), Event::Key, 30, 100.0, 0 } };
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    render (v, one, seconds);
    const double ms1 = juce::Time::getMillisecondCounterHiRes() - t1;
    std::printf ("bench: one key: %.1f%% of one core\n", 100.0 * ms1 / (seconds * 1000.0));
    return 0;
}
