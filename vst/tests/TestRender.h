#pragma once
// Offline rendering + analysis helpers shared by the console tests and tools.
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <complex>
#include "EngineParamsFromValues.h"
#include "PresetManager.h"

namespace ham::test
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 128;

using Values = std::array<float, kNumParams>;

inline Values defaultValues()
{
    Values v {};
    for (int i = 0; i < kNumParams; ++i) v[(size_t) i] = paramDefs()[(size_t) i].def;
    return v;
}

inline void applyPresetData (Values& v, const PresetData& p)
{
    for (int i = 0; i < kNumParams; ++i)
        if (i != master && ! std::isnan (p.values[(size_t) i])) v[(size_t) i] = p.values[(size_t) i];
}

inline int paramIndex (const juce::String& id)
{
    for (int i = 0; i < kNumParams; ++i)
        if (paramDefs()[(size_t) i].id == id) return i;
    return -1;
}

inline Values presetValues (const PresetData& p, float masterLevel = 0.7f)
{
    auto v = defaultValues();
    applyPresetData (v, p);
    v[master] = masterLevel;
    return v;
}

// Sets one manual's active drawbars from a registration string such as "888000000".
inline void setBars (Values& v, int manual, int set, const char* reg)
{
    for (int f = 0; f < kNumFootages && reg[f] != 0; ++f)
        v[(size_t) drawbarParam (manual, set, f)] = (float) (reg[f] - '0');
}

// Timed events (sample position).
struct Event
{
    enum Type { On, Off, CC, Key, KeyOff };
    int time;
    Type type;
    int a = 0;       // note / cc number / key index
    double b = 0;    // velocity (0..127) or CC value (0..1)
    int ch = 0;      // MIDI channel index 0..15, or the manual for Key / KeyOff
};

struct Audio
{
    std::vector<float> L, R;
    int size() const { return (int) L.size(); }
};

inline void applyEvent (SynthEngine& e, const Event& ev)
{
    switch (ev.type)
    {
        case Event::On:     e.noteOn (ev.a, (int) ev.b, ev.ch); break;
        case Event::Off:    e.noteOff (ev.a, ev.ch); break;
        case Event::CC:     e.setController (ev.a, ev.b, ev.ch); break;
        case Event::Key:    e.keyOn (ev.ch, ev.a, ev.b > 0 ? (int) ev.b : 100); break;
        case Event::KeyOff: e.keyOff (ev.ch, ev.a); break;
    }
}

// Renders `seconds` of audio, applying events at their exact sample (like the plugin).
// `onBlock` (optional) runs before each block, e.g. to change parameters mid-render.
inline Audio render (const Values& values, std::vector<Event> events, double seconds,
                     std::function<void (int, EngineParams&)> onBlock = {})
{
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b) { return a.time < b.time; });
    auto enginePtr = std::make_unique<SynthEngine>();
    auto& engine = *enginePtr;
    auto P = makeEngineParams ([&values] (int i) { return values[(size_t) i]; });
    engine.setParams (P);
    engine.prepare (kSr);
    const int total = (int) (seconds * kSr);
    Audio out;
    out.L.assign ((size_t) total, 0.0f);
    out.R.assign ((size_t) total, 0.0f);
    size_t ei = 0;
    for (int start = 0; start < total; start += kBlock)
    {
        const int end = std::min (total, start + kBlock);
        if (onBlock) onBlock (start, P);
        engine.setParams (P);
        int pos = start;
        while (ei < events.size() && events[ei].time < end)
        {
            const int t = std::max (pos, events[ei].time);
            if (t > pos) { engine.render (out.L.data() + pos, out.R.data() + pos, t - pos); pos = t; }
            applyEvent (engine, events[ei++]);
        }
        if (end > pos) engine.render (out.L.data() + pos, out.R.data() + pos, end - pos);
    }
    return out;
}

inline int sec (double s) { return (int) (s * kSr); }

inline double rms (const Audio& a, int from, int to)
{
    from = std::max (0, from); to = std::min (a.size(), to);
    double acc = 0;
    for (int i = from; i < to; ++i) acc += 0.5 * ((double) a.L[(size_t) i] * a.L[(size_t) i] + (double) a.R[(size_t) i] * a.R[(size_t) i]);
    return to > from ? std::sqrt (acc / (to - from)) : 0.0;
}

inline double peak (const Audio& a)
{
    double p = 0;
    for (int i = 0; i < a.size(); ++i) p = std::max (p, (double) std::max (std::abs (a.L[(size_t) i]), std::abs (a.R[(size_t) i])));
    return p;
}

inline double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }

// Frequency of the strongest component in [lo, hi] Hz of the left channel over [from, from + n):
// Hann-windowed DFT evaluated on a fine grid (step Hz), refined by a parabola. Accurate to a few mHz
// for steady tones over a second of audio.
inline double peakFrequency (const Audio& a, int from, int n, double lo, double hi, double step = 0.01)
{
    auto power = [&] (double f)
    {
        double re = 0, im = 0;
        const double w = kTwoPi * f / kSr;
        for (int i = 0; i < n; ++i)
        {
            const double win = 0.5 - 0.5 * std::cos (kTwoPi * i / (n - 1));
            const double x = a.L[(size_t) (from + i)] * win;
            re += x * std::cos (w * i);
            im -= x * std::sin (w * i);
        }
        return re * re + im * im;
    };
    // coarse search, then fine
    double best = lo, bestP = -1;
    for (double f = lo; f <= hi; f += 0.5) { const double p = power (f); if (p > bestP) { bestP = p; best = f; } }
    double fineBest = best;
    bestP = -1;
    for (double f = best - 0.5; f <= best + 0.5; f += step) { const double p = power (f); if (p > bestP) { bestP = p; fineBest = f; } }
    const double pm = power (fineBest - step), p0 = bestP, pp = power (fineBest + step);
    const double den = pm - 2 * p0 + pp;
    return den < 0 ? fineBest + 0.5 * step * (pm - pp) / den : fineBest;
}

// Energy of a 2nd-order-differenced signal: a crude high-pass that shows clicks (steps) far
// above the sine content of a low note.
inline double hfEnergy (const Audio& a, int from, int to)
{
    double acc = 0;
    for (int i = std::max (2, from); i < std::min (a.size(), to); ++i)
    {
        const double d = (double) a.L[(size_t) i] - 2.0 * a.L[(size_t) i - 1] + a.L[(size_t) i - 2];
        acc += d * d;
    }
    return acc;
}

// Amplitude of the component at f Hz in [from, from + n) (Hann-windowed single-bin DFT, scaled so
// a sine of amplitude A reads A).
inline double toneAmplitude (const Audio& a, int from, int n, double f)
{
    double re = 0, im = 0, wsum = 0;
    const double w = kTwoPi * f / kSr;
    for (int i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (kTwoPi * i / (n - 1));
        const double x = a.L[(size_t) (from + i)] * win;
        re += x * std::cos (w * i);
        im -= x * std::sin (w * i);
        wsum += win;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

// Spectral centroid (Hz) of [from, from + n), amplitude-weighted, by a DFT on a 10 Hz grid to 8 kHz.
inline double spectralCentroid (const Audio& a, int from, int n)
{
    double num = 0, den = 0;
    for (double f = 20; f <= 8000; f += 10)
    {
        const double m = toneAmplitude (a, from, n, f);
        num += f * m; den += m;
    }
    return den > 0 ? num / den : 0.0;
}

// Instantaneous frequency of a steady single tone from its upward zero crossings (linear
// interpolation between samples). Returns the frequency of each period.
inline std::vector<double> periodFrequencies (const Audio& a, int from, int to)
{
    std::vector<double> f;
    double last = -1;
    for (int i = std::max (1, from); i < std::min (a.size(), to); ++i)
    {
        const double y0 = a.L[(size_t) i - 1], y1 = a.L[(size_t) i];
        if (y0 < 0 && y1 >= 0)
        {
            const double t = (i - 1) + y0 / (y0 - y1);
            if (last >= 0) f.push_back (kSr / (t - last));
            last = t;
        }
    }
    return f;
}

inline bool writeWav (const juce::File& f, const Audio& a)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (f);
    const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (kSr).withNumChannels (2).withBitsPerSample (24);
    auto w = fmt.createWriterFor (out, opts);
    if (w == nullptr) return false;
    juce::AudioBuffer<float> b (2, a.size());
    b.copyFrom (0, 0, a.L.data(), a.size());
    b.copyFrom (1, 0, a.R.data(), a.size());
    return w->writeFromAudioSampleBuffer (b, 0, a.size());
}
} // namespace ham::test
