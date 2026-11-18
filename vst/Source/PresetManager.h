#pragma once
// ============================================================================
//  Presets
//   - factory presets, compiled in from presets/factory_presets.json
//     (templates + per-preset overrides; also exposed as programs)
//   - user presets: *.hampreset JSON files in Documents\The Ham\Presets
//     (shared by every DAW / instance; sub-folders become categories,
//      the folder is re-scanned automatically)
//   - files carry a format version; older versions are migrated on load
// ============================================================================
#include "Params.h"
#include "dsp/Wiring.h"
#include <vector>

namespace ham
{
constexpr int kPresetVersion = 1;

// Fixed registrations of the preset keys C#..A: [manual 0 upper / 1 lower][key 0..8][footage]
using Registrations = std::array<std::array<std::array<int, kNumFootages>, 9>, 2>;

inline Registrations factoryRegistrations()
{
    Registrations r {};
    for (int m = 0; m < 2; ++m)
        for (int k = 0; k < 9; ++k)
            for (int f = 0; f < kNumFootages; ++f) r[(size_t) m][(size_t) k][(size_t) f] = kFactoryRegistrations[k][f];
    return r;
}

// "00 5320 000" <-> 9 values (spaces ignored; missing digits = 0). Returns false if not a registration.
inline bool parseRegistration (const juce::String& text, std::array<int, kNumFootages>& out)
{
    const auto digits = text.removeCharacters (" -_.");
    if (digits.isEmpty() || digits.length() > kNumFootages || ! digits.containsOnly ("012345678")) return false;
    for (int f = 0; f < kNumFootages; ++f) out[(size_t) f] = f < digits.length() ? digits[f] - '0' : 0;
    return true;
}

inline juce::String registrationString (const std::array<int, kNumFootages>& r)
{
    juce::String s;
    for (int f = 0; f < kNumFootages; ++f)
    {
        if (f == 2 || f == 6) s << ' ';
        s << juce::jlimit (0, 8, r[(size_t) f]);
    }
    return s;
}

struct PresetData
{
    juce::String name, category;
    std::array<float, kNumParams> values; // NaN = not in preset -> parameter default
    // Fixed preset-key registrations. A preset without them (all factory presets so far, files
    // from before phase 2) gets the factory chart.
    Registrations registrations = factoryRegistrations();

    PresetData() { values.fill (NAN); }
};

struct PresetEntry
{
    juce::String id;        // "user:<relative/path.hampreset>"
    juce::String name, category;
    juce::File file;
};

class PresetManager
{
public:
    PresetManager();

    static juce::File userFolder();
    static constexpr const char* fileExtension = ".hampreset";
    static constexpr const char* formatName = "The Ham Preset";

    // Factory (immutable, any thread)
    const std::vector<PresetData>& factory() const { return factoryPresets; }

    // User library (message thread only)
    bool rescanIfChanged();                     // true if the list changed
    const std::vector<PresetEntry>& userPresets() const { return user; }
    const PresetEntry* findUser (const juce::String& id) const;
    juce::File fileForName (const juce::String& name) const;
    juce::String idForFile (const juce::File& f) const;  // "" if not inside the user folder
    juce::String categoryForFile (const juce::File& f) const;

    static bool readFile (const juce::File&, PresetData&, juce::String& error);
    static bool readJson (const juce::var& json, PresetData&, juce::String& error);
    static bool writeFile (const juce::File&, const PresetData&, juce::String& error);
    static juce::var toJson (const PresetData&);

    // Brings a preset object written by an older version up to kPresetVersion (in place).
    static void migrate (juce::var& json);

private:
    void loadFactory();

    std::vector<PresetData> factoryPresets;
    std::vector<PresetEntry> user;
    juce::String signature;
    bool scanned = false;
};
} // namespace ham
