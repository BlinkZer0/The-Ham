// --host-check <path to .vst3>
// Loads the built plugin the way a DAW does (JUCE's VST3 hosting) and checks the
// basics a host like Minihost Modular relies on:
//   - it scans as an instrument with MIDI in and stereo out
//   - every parameter is exposed, with names and text values
//   - MIDI notes + drawbar CCs produce sound; automation during playback is safe
//   - state save -> restore into a fresh instance gives identical parameter values
//   - programs (factory presets) switch and report names
#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"

int runHostCheck (const juce::String& path)
{
    int fails = 0;
    auto check = [&fails] (bool ok, const char* what) { if (! ok) ++fails; std::printf ("%-4s %s\n", ok ? "ok" : "FAIL", what); };

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);
    check (types.size() == 1, "scans: one plugin in the bundle");
    if (types.isEmpty()) return 1;
    const auto& desc = *types[0];
    std::printf ("     name '%s', manufacturer '%s', category '%s', version %s\n", desc.name.toRawUTF8(),
                 desc.manufacturerName.toRawUTF8(), desc.category.toRawUTF8(), desc.version.toRawUTF8());
    check (desc.isInstrument, "is an instrument");
    check (desc.name == "The Ham", "name is 'The Ham'");

    juce::String error;
    auto make = [&] { return format.createInstanceFromDescription (desc, 48000.0, 512, error); };
    auto plugin = make();
    check (plugin != nullptr, "instantiates");
    if (plugin == nullptr) { std::printf ("     %s\n", error.toRawUTF8()); return 1; }

    check (plugin->acceptsMidi(), "accepts MIDI");
    check (plugin->getMainBusNumOutputChannels() == 2, "stereo output");

    auto& params = plugin->getParameters();
    int named = 0;
    for (auto* p : params)
        if (p->getName (64).isNotEmpty() && p->getCurrentValueAsText().isNotEmpty()) ++named;
    std::printf ("     %d host parameters (%d in the plugin's table)\n", params.size(), ham::kNumParams);
    check (params.size() >= ham::kNumParams, "every parameter is exposed");
    check (named == params.size(), "every parameter has a name and a text value");

    // play: a chord on the upper manual while a drawbar CC (B4D map, CC 14 = upper 8') moves and two parameters are automated
    plugin->setPlayConfigDetails (0, 2, 48000.0, 512);
    plugin->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buf (2, 512);
    double energy = 0, peak = 0;
    juce::AudioProcessorParameter* cutoff = nullptr;
    juce::AudioProcessorParameter* vib = nullptr;
    for (auto* p : params)
    {
        if (p->getName (64) == "Upper B 4'") cutoff = p;
        if (p->getName (64) == "Drive") vib = p;
    }
    check (cutoff != nullptr && vib != nullptr, "parameters found by host name");
    for (int b = 0; b < 200; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 1) midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 10);
        if (b == 1) { midi.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 10); midi.addEvent (juce::MidiMessage::noteOn (3, 36, (juce::uint8) 100), 10); }
        if (b > 20 && b < 120) midi.addEvent (juce::MidiMessage::controllerEvent (1, 14, juce::jlimit (0, 127, 127 - (b - 20))), 0);
        if (b == 150) midi.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
        if (cutoff != nullptr) cutoff->setValueNotifyingHost ((float) (0.5 + 0.4 * std::sin (b * 0.1)));
        if (vib != nullptr) vib->setValueNotifyingHost ((float) (b % 50) / 50.0f);
        buf.clear();
        plugin->processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
            {
                const float x = buf.getSample (ch, i);
                if (! std::isfinite (x)) { energy = -1; break; }
                energy += (double) x * x;
                peak = std::max (peak, (double) std::abs (x));
            }
    }
    check (energy > 1.0, "notes + drawbar CCs make sound (finite samples)");
    check (peak < 1.0, "output stays below full scale");

    // State round trip into a fresh instance. Only the plugin's own parameters: the
    // VST3 wrapper also exposes hidden MIDI-CC parameters (that's how hosts deliver
    // CC2 / CC11 / aftertouch) and a Program parameter, which aren't state.
    juce::StringArray ownNames;
    for (const auto& d : ham::paramDefs()) ownNames.add (d.hostName);
    std::vector<int> own;
    for (int i = 0; i < params.size(); ++i)
        if (ownNames.contains (params[i]->getName (128))) own.push_back (i);
    std::printf ("     %d of the host parameters are the plugin's own\n", (int) own.size());
    check ((int) own.size() == ham::kNumParams, "all plugin parameters found by name");
    for (const int i : own)
        params[i]->setValueNotifyingHost (std::fmod (0.37f * (float) i, 1.0f));
    {   // VST3 delivers parameter changes to the processor with the next audio block
        juce::MidiBuffer none;
        buf.clear();
        plugin->processBlock (buf, none);
    }
    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    auto other = make();
    check (other != nullptr, "second instance");
    if (other != nullptr)
    {
        other->setStateInformation (state.getData(), (int) state.getSize());
        int diffs = 0;
        auto& p2 = other->getParameters();
        // stepped parameters (choices, switches, integers) come back snapped to their step,
        // so those are compared by their displayed value
        for (const int i : own)
            if (i < p2.size() && (ham::paramDefs()[(size_t) ownNames.indexOf (params[i]->getName (128))].kind != ham::Kind::Float
                                      ? params[i]->getCurrentValueAsText() != p2[i]->getCurrentValueAsText()
                                      : std::abs (params[i]->getValue() - p2[i]->getValue()) > 1e-4f))
            {
                if (++diffs <= 6)
                    std::printf ("     differs: %s  %.4f '%s' -> %.4f '%s'\n", params[i]->getName (64).toRawUTF8(),
                                 params[i]->getValue(), params[i]->getCurrentValueAsText().toRawUTF8(),
                                 p2[i]->getValue(), p2[i]->getCurrentValueAsText().toRawUTF8());
            }
        std::printf ("     %d parameter(s) differ after restore\n", diffs);
        check (diffs == 0, "state save -> restore is exact");
    }

    // programs
    const int programs = plugin->getNumPrograms();
    std::printf ("     %d programs, e.g. '%s'\n", programs, plugin->getProgramName (programs > 3 ? 3 : 0).toRawUTF8());
    check (programs >= 10, "factory presets are host programs");
    plugin->setCurrentProgram (programs - 1);
    check (plugin->getCurrentProgram() == programs - 1, "program change sticks");

    plugin->releaseResources();
    std::printf (fails == 0 ? "HOST CHECK PASSED\n" : "HOST CHECK: %d FAILURE(S)\n", fails);
    return fails == 0 ? 0 : 1;
}

// --host-cab <path>: the built plugin, the same chord with the Cabinet parameter at each model.
int runHostCab (const juce::String& path)
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);
    if (types.isEmpty()) return 1;
    juce::String error;
    std::vector<std::vector<float>> outs;
    for (int m = 0; m < 4; ++m)
    {
        auto plugin = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
        if (plugin == nullptr) return 1;
        for (auto* p : plugin->getParameters())
            if (p->getName (64) == "Cabinet") p->setValueNotifyingHost ((float) m / 3.0f);
        plugin->setPlayConfigDetails (0, 2, 48000.0, 512);
        plugin->prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buf (2, 512);
        std::vector<float> out;
        for (int b = 0; b < 200; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 2) for (int n : { 60, 64, 67 }) midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), 0);
            buf.clear();
            plugin->processBlock (buf, midi);
            for (int i = 0; i < 512; ++i) out.push_back (buf.getSample (0, i));
        }
        outs.push_back (out);
        plugin->releaseResources();
    }
    for (int m = 1; m < 4; ++m)
    {
        double d = 0, r = 0;
        for (size_t i = 48000; i < outs[0].size(); ++i) { d += (outs[m][i] - outs[0][i]) * (outs[m][i] - outs[0][i]); r += outs[0][i] * outs[0][i]; }
        std::printf ("plugin, Cabinet %d vs 0: difference %.1f dB below the signal%s\n", m, 10 * std::log10 (r / std::max (d, 1e-30)), d == 0 ? " (IDENTICAL)" : "");
    }
    return 0;
}

// --host-rotor <path>: the built plugin, one instance, its rotor parameters changed while a note is
// held (as a player does): Fast / Slow / Stop / a different Horn Fast / Bypass / the Tone Cabinet.
// Measures the amplitude modulation the mics see (swing in dB, and its rate from the envelope).
int runHostRotor (const juce::String& path)
{
    int fails = 0;
    auto check = [&fails] (bool ok, const juce::String& what) { if (! ok) ++fails; std::printf ("%-4s %s\n", ok ? "ok" : "FAIL", what.toRawUTF8()); };

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);
    if (types.isEmpty()) { std::printf ("FAIL no plugin at %s\n", path.toRawUTF8()); return 1; }
    juce::String error;
    auto plugin = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
    if (plugin == nullptr) { std::printf ("FAIL %s\n", error.toRawUTF8()); return 1; }

    // set a plugin parameter (by index or id), in real units, through the host API (normalised)
    auto setIndex = [&] (int index, float value)
    {
        const auto& d = ham::paramDefs()[(size_t) index];
        float n = 0.0f;
        if (d.kind == ham::Kind::Float && d.logSkew) n = ham::logRange (d.min, d.max).convertTo0to1 (value);
        else n = (value - d.min) / std::max (1e-9f, d.max - d.min);
        for (auto* p : plugin->getParameters())
            if (p->getName (128) == d.hostName) { p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, n)); return; }
        std::printf ("FAIL host parameter '%s' not found\n", d.hostName.toRawUTF8());
        ++fails;
    };
    auto set = [&] (const char* id, float value)
    {
        const auto& defs = ham::paramDefs();
        for (size_t i = 0; i < defs.size(); ++i)
            if (defs[i].id == id) { setIndex ((int) i, value); return; }
        std::printf ("FAIL unknown id %s\n", id); ++fails;
    };
    // one clean tone: upper set B, 8' only, no click / percussion / vibrato / leakage / age
    for (int m = 0; m < 2; ++m) for (int st = 0; st < 2; ++st) for (int f = 0; f < ham::kNumFootages; ++f) setIndex (ham::drawbarParam (m, st, f), 0);
    setIndex (ham::drawbarParam (0, 1, 2), 8);
    set ("upKey", 11); set ("pd16", 0); set ("pd8", 0);
    set ("percOn", 0); set ("vibUpper", 0); set ("vibLower", 0);
    set ("leakUp", 0); set ("leakLo", 0); set ("ageUp", 0); set ("ageLo", 0);
    set ("clickUp", 0); set ("clickLo", 0); set ("clickPd", 0);
    set ("odOn", 0); set ("wahOn", 0); set ("echoOn", 0); set ("revOn", 0); set ("drive", 0);

    plugin->setPlayConfigDetails (0, 2, 48000.0, 512);
    plugin->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buf (2, 512);
    std::vector<float> out;
    auto run = [&] (double seconds, int noteOn)
    {
        const int blocks = (int) (seconds * 48000.0 / 512.0);
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0 && noteOn > 0) midi.addEvent (juce::MidiMessage::noteOn (1, noteOn, (juce::uint8) 100), 0);
            buf.clear();
            plugin->processBlock (buf, midi);
            for (int i = 0; i < 512; ++i) out.push_back (0.5f * (buf.getSample (0, i) + buf.getSample (1, i)));
        }
    };
    // envelope: RMS in 5 ms windows every 2 ms over the last `seconds` of `out`
    auto envelope = [&] (double seconds)
    {
        std::vector<double> e;
        const int win = 240, hop = 96, n = (int) (seconds * 48000.0);
        for (int s = std::max (0, (int) out.size() - n); s + win <= (int) out.size(); s += hop)
        {
            double acc = 0;
            for (int i = 0; i < win; ++i) acc += (double) out[(size_t) (s + i)] * out[(size_t) (s + i)];
            e.push_back (std::sqrt (acc / win));
        }
        return e;
    };
    auto swingDb = [] (const std::vector<double>& e)
    {
        double lo = 1e9, hi = 0;
        for (double x : e) { lo = std::min (lo, x); hi = std::max (hi, x); }
        return 20.0 * std::log10 (std::max (hi, 1e-12) / std::max (lo, 1e-12));
    };
    auto rateHz = [] (const std::vector<double>& e)   // strongest component of the envelope, 0.3..15 Hz
    {
        const double fs = 48000.0 / 96.0;
        double mean = 0; for (double x : e) mean += x; mean /= (double) std::max<size_t> (1, e.size());
        double best = 0, bestP = -1;
        for (double f = 0.3; f <= 15.0; f += 0.02)
        {
            double re = 0, im = 0;
            for (size_t i = 0; i < e.size(); ++i) { const double w = 2.0 * juce::MathConstants<double>::pi * f * (double) i / fs; re += (e[i] - mean) * std::cos (w); im -= (e[i] - mean) * std::sin (w); }
            const double p = re * re + im * im;
            if (p > bestP) { bestP = p; best = f; }
        }
        return best;
    };

    // Rotor 22, horn only through one close mono mic so the horn's own modulation is what's measured
    set ("cabModel", 0); set ("cabBypass", 0); set ("rotorConfig", 1); set ("micLayout", 0); set ("micDist", 0);
    set ("xover", 300); set ("speedMode", 1); set ("speed", 2); set ("hornFast", 6.7f); set ("hornSlow", 0.8f);
    set ("speedWheel", 0);
    run (3.0, 81);   // A5 on the upper manual, held from here on
    auto e = envelope (2.0);
    std::printf ("     Fast:            swing %.1f dB, rate %.2f Hz (Horn Fast 6.7)\n", swingDb (e), rateHz (e));
    check (swingDb (e) > 3.0 && std::abs (rateHz (e) - 6.7) < 0.5, "plugin: Fast modulates at the Horn Fast rate");

    set ("speed", 1);
    run (6.0, 0);
    e = envelope (4.0);
    std::printf ("     Slow:            swing %.1f dB, rate %.2f Hz (Horn Slow 0.8)\n", swingDb (e), rateHz (e));
    check (swingDb (e) > 3.0 && std::abs (rateHz (e) - 0.8) < 0.2, "plugin: Slow modulates at the Horn Slow rate");

    set ("speed", 0);
    run (6.0, 0);
    e = envelope (1.5);
    std::printf ("     Stop (3-speed):  swing %.2f dB\n", swingDb (e));
    check (swingDb (e) < 0.3, "plugin: Stop stops the modulation");

    set ("hornFast", 10.0f); set ("speed", 2);
    run (4.0, 0);
    e = envelope (2.0);
    std::printf ("     Horn Fast 10 Hz: swing %.1f dB, rate %.2f Hz\n", swingDb (e), rateHz (e));
    check (swingDb (e) > 3.0 && std::abs (rateHz (e) - 10.0) < 0.5, "plugin: Horn Fast changes the rate");

    set ("cabBypass", 1);
    run (3.0, 0);
    e = envelope (1.5);
    std::printf ("     Bypass:          swing %.2f dB\n", swingDb (e));
    check (swingDb (e) < 0.3, "plugin: Bypass removes the modulation");

    set ("cabBypass", 0); set ("cabModel", 3);
    run (3.0, 0);
    e = envelope (1.5);
    std::printf ("     Tone Cabinet:    swing %.2f dB\n", swingDb (e));
    check (swingDb (e) < 0.3, "plugin: the Tone Cabinet has no rotors");

    plugin->releaseResources();
    std::printf (fails == 0 ? "HOST ROTOR CHECK PASSED\n" : "HOST ROTOR CHECK: %d FAILURE(S)\n", fails);
    return fails == 0 ? 0 : 1;
}
