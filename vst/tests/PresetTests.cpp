// Console tests (no audio device / window needed).
// Build:  vst\build.bat   (target TheHamTests)
// Run without arguments for the full test suite; see EngineTools.cpp for the tool commands.
#include "TestRender.h"

using namespace ham;
using namespace ham::test;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

int runDumpParams();
int runRender (const juce::String&, const juce::File&, const juce::String&);
int runRenderAll (const juce::File&);
int runLoudness();
int runBench();
int runHostCheck (const juce::String& path);
int runOrganTests();
double presetLoudness (const PresetData&);

static void testParamTable()
{
    const auto& defs = paramDefs();
    CHECK ((int) defs.size() == kNumParams);
    std::printf ("parameters: %d\n", kNumParams);
    juce::StringArray ids;
    for (const auto& d : defs)
    {
        CHECK (! ids.contains (d.id));
        ids.add (d.id);
        CHECK (d.min < d.max);
        CHECK (d.def >= d.min && d.def <= d.max);
        if (d.kind == Kind::Choice) CHECK (d.choices.size() == (int) d.max + 1);
        if (d.logSkew) CHECK (d.min > 0);
    }
    // index helpers line up with the table
    CHECK (defs[(size_t) drawbarParam (0, 0, 0)].id == "upA1");
    CHECK (defs[(size_t) drawbarParam (0, 1, 8)].id == "upB9");
    CHECK (defs[(size_t) drawbarParam (1, 0, 4)].id == "loA5");
    CHECK (defs[(size_t) drawbarParam (1, 1, 8)].id == "loB9" && pd16 == drawbarParam (1, 1, 8) + 1);
    CHECK (defs[(size_t) upB1].hostName == "Upper B 16'" && defs[(size_t) loA9].hostName == "Lower A 1'");
    CHECK (defs[(size_t) master].noPreset);
    CHECK (defs[(size_t) revTone].id == "revTone" && revTone == kNumParams - 1);
    // factory drawbar sets: upper B = 88 8000 000
    CHECK (defs[(size_t) upB1].def == 8 && defs[(size_t) upB1 + 2].def == 8 && defs[(size_t) upB1 + 3].def == 0);
}

static void testFactoryPresets (const PresetManager& pm)
{
    const auto& f = pm.factory();
    std::printf ("factory presets: %d\n", (int) f.size());
    CHECK (f.size() == 10);   // phase 1: two starters per category; the 50 arrive in phase 5
    juce::StringArray cats, names;
    std::map<juce::String, int> perCat;
    for (const auto& p : f)
    {
        cats.addIfNotAlreadyThere (p.category);
        ++perCat[p.category];
        CHECK (p.name.isNotEmpty());
        CHECK (! names.contains (p.name));
        names.add (p.name);
        for (int i = 0; i < kNumParams; ++i)
        {
            const float v = p.values[(size_t) i];
            if (i == master) { CHECK (std::isnan (v)); continue; }
            if (! std::isnan (v)) CHECK (v >= paramDefs()[(size_t) i].min && v <= paramDefs()[(size_t) i].max);
            if (! std::isnan (v) && paramDefs()[(size_t) i].unit == Unit::Drawbar) CHECK (v == std::round (v));   // whole steps
        }
    }
    std::printf ("categories (%d): %s\n", cats.size(), cats.joinIntoString (", ").toRawUTF8());
    const juce::StringArray expected { "Jazz", "Gospel", "Rock", "Blues & Soul", "Ballad, Classic & Theatre" };
    CHECK (cats == expected);
    for (const auto& c : expected) CHECK (perCat[c] == 2);
    // template resolved: Trio Classic = upper B 888000000
    CHECK (f[0].name == "Trio Classic" && f[0].values[upB1] == 8.0f && f[0].values[upB1 + 3] == 0.0f);
}

static void testPresetFiles (const PresetManager& pm)
{
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ham-test.hampreset");
    juce::String err;

    // save -> load round trip
    for (const auto& src : pm.factory())
    {
        PresetData out = src;
        out.name = "Round Trip";
        CHECK (PresetManager::writeFile (tmp, out, err));
        PresetData in;
        CHECK (PresetManager::readFile (tmp, in, err));
        CHECK (in.name == "Round Trip");
        for (int i = 0; i < kNumParams; ++i)
        {
            if (i == master) continue;
            const float a = out.values[(size_t) i], b = in.values[(size_t) i];
            if (std::isnan (a)) CHECK (std::isnan (b));
            else CHECK (std::abs (b - a) < 1e-4f * std::max (1.0f, std::abs (a)));
        }
    }
    {
        const auto json = juce::JSON::parse (tmp);
        CHECK (json["format"].toString() == "The Ham Preset" && (int) json["version"] == kPresetVersion);
    }
    // drawbars are written as whole steps (automation may leave them fractional)
    {
        PresetData frac;
        frac.name = "Fraction";
        frac.values[upB1] = 6.6f;
        frac.values[ampLevel] = -3.25f;
        CHECK (PresetManager::writeFile (tmp, frac, err));
        PresetData in;
        CHECK (PresetManager::readFile (tmp, in, err));
        CHECK (in.values[upB1] == 7.0f && std::abs (in.values[ampLevel] + 3.25f) < 1e-5f);
    }

    // older/partial file (no version): missing params stay NaN (-> defaults), values clamped
    tmp.replaceWithText (R"({"name":"Partial","params":{"upB3":5,"tuneA":999,"unknownParam":5}})");
    PresetData partial;
    CHECK (PresetManager::readFile (tmp, partial, err));
    CHECK (partial.values[upB1 + 2] == 5.0f);
    CHECK (partial.values[tuneA] == 450.0f);
    CHECK (std::isnan (partial.values[ampLevel]));

    // master is never taken from a file
    tmp.replaceWithText (R"({"version":1,"params":{"master":0.1,"drive":0.5}})");
    PresetData m;
    CHECK (PresetManager::readFile (tmp, m, err));
    CHECK (std::isnan (m.values[master]) && m.values[drive] == 0.5f);

    // garbage and empty settings are rejected with a message
    tmp.replaceWithText ("not json");
    PresetData bad;
    CHECK (! PresetManager::readFile (tmp, bad, err) && err.isNotEmpty());
    tmp.replaceWithText (R"({"name":"Empty","params":{}})");
    err.clear();
    CHECK (! PresetManager::readFile (tmp, bad, err) && err.isNotEmpty());
    tmp.deleteFile();
}

// Every factory preset renders: not silent, not into the clipper, silent after release.
static void testPresetRenders (const PresetManager& pm)
{
    for (const auto& p : pm.factory())
    {
        const auto v = presetValues (p);
        std::vector<Event> ev;
        for (int n : { 60, 64, 67 }) { ev.push_back ({ sec (0.02), Event::On, n, 100.0, 0 }); ev.push_back ({ sec (1.2), Event::Off, n, 0.0, 0 }); }
        ev.push_back ({ sec (0.02), Event::On, 36, 100.0, 2 });
        ev.push_back ({ sec (1.2), Event::Off, 36, 0.0, 2 });
        const auto a = render (v, ev, 2.0);
        const double level = rms (a, sec (0.3), sec (1.1)), pk = peak (a), tail = rms (a, sec (1.7), sec (2.0));
        const bool ok = level > 0.01 && pk < 0.8 && tail == 0.0;
        if (! ok) ++failures;
        std::printf ("%-4s render %-38s rms %.3f  peak %.3f  tail %.1e\n", ok ? "ok" : "FAIL",
                     (p.category + "/" + p.name).toRawUTF8(), level, pk, tail);
    }
}

static void testLoudness (const PresetManager& pm)
{
    double lo = 0, hi = -100;
    for (const auto& p : pm.factory())
    {
        const double d = db (presetLoudness (p));
        lo = std::min (lo, d); hi = std::max (hi, d);
        if (std::abs (d + 18.0) > 1.0) { ++failures; std::printf ("FAIL loudness %-38s %.2f dB\n", (p.category + "/" + p.name).toRawUTF8(), d); }
    }
    std::printf ("%-4s loudness of %d presets: %.2f .. %.2f dB (target -18 +- 1)\n", lo >= -19 && hi <= -17 ? "ok" : "FAIL",
                 (int) pm.factory().size(), lo, hi);
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init; // keeps JUCE leak checks quiet

    const juce::String cmd = argc >= 2 ? juce::String (argv[1]) : juce::String();
    if (cmd == "--dump-params") return runDumpParams();
    if (cmd == "--render" && argc >= 4)
        return runRender (argv[2], juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]), argc >= 5 ? argv[4] : "demo");
    if (cmd == "--render-all" && argc >= 3) return runRenderAll (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    if (cmd == "--loudness") return runLoudness();
    if (cmd == "--bench") return runBench();
    if (cmd == "--host-check" && argc >= 3) return runHostCheck (argv[2]);
    if (cmd == "--organ") return runOrganTests() == 0 ? 0 : 1;

    PresetManager pm;
    testParamTable();
    testFactoryPresets (pm);
    testPresetFiles (pm);
    failures += runOrganTests();
    testPresetRenders (pm);
    testLoudness (pm);

    std::printf ("user preset folder: %s\n", PresetManager::userFolder().getFullPathName().toRawUTF8());
    std::printf (failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
