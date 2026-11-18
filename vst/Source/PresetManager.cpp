#include "PresetManager.h"
#include "BinaryData.h"

namespace ham
{
namespace
{
float clampParam (int index, float v)
{
    const auto& d = paramDefs()[(size_t) index];
    return juce::jlimit (d.min, d.max, v);
}

int indexForId (const juce::String& id)
{
    static const std::unordered_map<juce::String, int> map = []
    {
        std::unordered_map<juce::String, int> m;
        for (int i = 0; i < kNumParams; ++i) m[paramDefs()[(size_t) i].id] = i;
        return m;
    }();
    const auto it = map.find (id);
    return it == map.end() ? -1 : it->second;
}

// Copies every known { "paramId": value } of obj into values. Returns how many were found.
int applyParamObject (const juce::var& obj, std::array<float, kNumParams>& values)
{
    int found = 0;
    if (auto* o = obj.getDynamicObject())
        for (const auto& prop : o->getProperties())
        {
            const int i = indexForId (prop.name.toString());
            if (i < 0 || i == master || ! (prop.value.isDouble() || prop.value.isInt() || prop.value.isInt64() || prop.value.isBool()))
                continue;
            values[(size_t) i] = clampParam (i, (float) (double) prop.value);
            ++found;
        }
    return found;
}
} // namespace

PresetManager::PresetManager()
{
    loadFactory();
}

//==============================================================================
// Factory file format:
//   { "templates": { "<name>": { "base": "<other template>"?, "params": { id: value } } },
//     "categories": [ { "name": "Trumpet",
//                       "presets": [ { "name": "...", "base": "<template>"?, "params": { id: value } } ] } ] }
// A preset's values = parameter defaults <- template chain (base first) <- its own params.
void PresetManager::loadFactory()
{
    const auto json = juce::JSON::parse (juce::String::fromUTF8 (BinaryData::factory_presets_json,
                                                                 BinaryData::factory_presets_jsonSize));
    const auto templates = json["templates"];

    std::function<void (const juce::String&, std::array<float, kNumParams>&, int)> applyTemplate;
    applyTemplate = [&] (const juce::String& name, std::array<float, kNumParams>& values, int depth)
    {
        if (name.isEmpty() || depth > 8) return;
        const auto t = templates[juce::Identifier (name)];
        jassert (t.isObject()); // unknown template name
        if (! t.isObject()) return;
        applyTemplate (t["base"].toString(), values, depth + 1);
        applyParamObject (t["params"], values);
    };

    if (const auto* cats = json["categories"].getArray())
    {
        for (const auto& cat : *cats)
        {
            const auto* list = cat["presets"].getArray();
            if (list == nullptr) continue;
            for (const auto& p : *list)
            {
                PresetData d;
                d.name = p["name"].toString();
                d.category = cat["name"].toString();
                applyTemplate (p["base"].toString(), d.values, 0);
                applyParamObject (p["params"], d.values);
                factoryPresets.push_back (std::move (d));
            }
        }
    }

    if (factoryPresets.empty()) // never leave the host with zero programs
    {
        PresetData init;
        init.name = "Init";
        init.category = "Factory";
        factoryPresets.push_back (init);
    }
}

//==============================================================================
juce::File PresetManager::userFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("The Ham")
        .getChildFile ("Presets");
}

juce::String PresetManager::idForFile (const juce::File& f) const
{
    const auto root = userFolder();
    if (! f.isAChildOf (root)) return {};
    return "user:" + f.getRelativePathFrom (root).replaceCharacter ('\\', '/');
}

juce::String PresetManager::categoryForFile (const juce::File& f) const
{
    const auto root = userFolder();
    const auto parent = f.getParentDirectory();
    if (parent == root || ! f.isAChildOf (root)) return "User";
    return parent.getRelativePathFrom (root).replaceCharacter ('\\', '/');
}

bool PresetManager::rescanIfChanged()
{
    const auto root = userFolder();
    root.createDirectory();

    const auto files = root.findChildFiles (juce::File::findFiles, true, juce::String ("*") + fileExtension);

    juce::String sig;
    for (const auto& f : files)
        sig << f.getFullPathName() << '|' << f.getSize() << '|' << f.getLastModificationTime().toMilliseconds() << ';';

    if (scanned && sig == signature) return false;
    scanned = true;
    signature = sig;

    user.clear();
    for (const auto& f : files)
        user.push_back ({ idForFile (f), f.getFileNameWithoutExtension(), categoryForFile (f), f });

    std::sort (user.begin(), user.end(), [] (const PresetEntry& a, const PresetEntry& b)
    {
        if (a.category != b.category) return a.category.compareNatural (b.category) < 0;
        return a.name.compareNatural (b.name) < 0;
    });
    return true;
}

const PresetEntry* PresetManager::findUser (const juce::String& id) const
{
    for (const auto& e : user)
        if (e.id == id) return &e;
    return nullptr;
}

juce::File PresetManager::fileForName (const juce::String& name) const
{
    return userFolder().getChildFile (juce::File::createLegalFileName (name.trim()) + fileExtension);
}

//==============================================================================
// Migration. Each step takes a preset object of version N to N + 1. The web
// build has the same steps (webhost/factory.js, hamMigratePreset). Add a step whenever a
// release renames a parameter or changes what a stored value means; plain
// additions need nothing (missing params load at their defaults).
void PresetManager::migrate (juce::var& json)
{
    if (! json.isObject()) return;
    int version = json.hasProperty ("version") ? (int) json["version"] : 1;
    // while (version < kPresetVersion) { switch (version) { case 1: /* 1 -> 2 */ break; } ++version; }
    version = std::max (version, 1);
    if (version < kPresetVersion) version = kPresetVersion;
    json.getDynamicObject()->setProperty ("version", version);
}

//==============================================================================
// User file format:
//   { "format": "The Ham Preset", "version": 1, "name": "...", "category": "...",
//     "params": { "<paramId>": value, ... } }   (master is never stored)
bool PresetManager::readJson (const juce::var& input, PresetData& d, juce::String& error)
{
    if (! input.isObject()) { error = "Not a The Ham preset"; return false; }
    auto json = juce::JSON::parse (juce::JSON::toString (input, true)); // private copy for migration
    migrate (json);
    if (applyParamObject (json["params"], d.values) == 0)
    {
        error = "No The Ham settings found";
        return false;
    }
    d.name = json["name"].toString().trim();
    d.category = json["category"].toString();
    return true;
}

bool PresetManager::readFile (const juce::File& f, PresetData& d, juce::String& error)
{
    juce::var json;
    if (! f.existsAsFile() || juce::JSON::parse (f.loadFileAsString(), json).failed() || ! json.isObject())
    {
        error = "Not a valid preset file: " + f.getFileName();
        return false;
    }
    if (! readJson (json, d, error))
    {
        error << " in " << f.getFileName();
        return false;
    }
    if (d.name.isEmpty()) d.name = f.getFileNameWithoutExtension();
    return true;
}

juce::var PresetManager::toJson (const PresetData& d)
{
    auto* params = new juce::DynamicObject();
    for (int i = 0; i < kNumParams; ++i)
    {
        if (i == master || paramDefs()[(size_t) i].noPreset || std::isnan (d.values[(size_t) i])) continue;
        // round to 6 significant digits so the files stay readable; drawbars are stored as
        // whole steps (the engine takes fractions from automation, presets don't)
        const auto& def = paramDefs()[(size_t) i];
        const double v = def.unit == Unit::Drawbar ? (double) std::lround (d.values[(size_t) i])
                                                   : juce::String (d.values[(size_t) i], 6).getDoubleValue();
        params->setProperty (paramDefs()[(size_t) i].id, v);
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", formatName);
    root->setProperty ("version", kPresetVersion);
    root->setProperty ("name", d.name);
    root->setProperty ("category", d.category);
    root->setProperty ("params", juce::var (params));
    return juce::var (root);
}

bool PresetManager::writeFile (const juce::File& f, const PresetData& d, juce::String& error)
{
    if (! f.getParentDirectory().createDirectory() || ! f.replaceWithText (juce::JSON::toString (toJson (d))))
    {
        error = "Couldn't write " + f.getFullPathName();
        return false;
    }
    return true;
}
} // namespace ham
