#include "PluginEditor.h"
#include "BinaryData.h"

using namespace ham;

namespace
{
const juce::Colour kBackground { 0xff140c09 };

const char* mimeForExtension (const juce::String& ext)
{
    if (ext == "html") return "text/html";
    if (ext == "js")   return "text/javascript";
    if (ext == "css")  return "text/css";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "json") return "application/json";
    if (ext == "png")  return "image/png";
    return "application/octet-stream";
}

std::vector<std::byte> toBytes (const void* data, size_t size)
{
    const auto* p = static_cast<const std::byte*> (data);
    return { p, p + size };
}

juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* obj = new juce::DynamicObject();
    for (const auto& [k, v] : props) obj->setProperty (k, v);
    return juce::var (obj);
}
} // namespace

//==============================================================================
TheHamEditor::TheHamEditor (TheHamProcessor& p)
    : AudioProcessorEditor (p), processor (p), browser (makeBrowserOptions())
{
    lastSent.fill (std::numeric_limits<float>::quiet_NaN());

    addAndMakeVisible (browser);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (720, 520, 2600, 2200);
    setSize (1120, 960);
    startTimerHz (30);
}

TheHamEditor::~TheHamEditor()
{
    stopTimer();
    // Close any knob gestures the UI didn't get to finish.
    for (int i = 0; i < kNumParams; ++i)
        if (inGesture[(size_t) i]) processor.param (i)->endChangeGesture();
}

void TheHamEditor::paint (juce::Graphics& g) { g.fillAll (kBackground); }
void TheHamEditor::resized() { browser.setBounds (getLocalBounds()); }

//==============================================================================
juce::WebBrowserComponent::Options TheHamEditor::makeBrowserOptions()
{
    using Opts = juce::WebBrowserComponent::Options;

    // WebView2 needs a writable data folder; the default (next to the host's exe) usually isn't.
    const auto userData = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("TheHamVST-WebView2");

    return Opts{}
        .withBackend (Opts::Backend::webview2)
        .withWinWebView2Options (Opts::WinWebView2{}
                                     .withUserDataFolder (userData)
                                     .withBackgroundColour (kBackground)
                                     .withStatusBarDisabled())
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider ([this] (const juce::String& url) { return getResource (url); })
        .withEventListener ("uiReady",    [this] (const juce::var&) { onUiReady(); })
        .withEventListener ("setParam",   [this] (const juce::var& e) { onSetParam (e); })
        .withEventListener ("gesture",    [this] (const juce::var& e) { onGesture (e); })
        // on-screen keyboards: { manual: 0 upper / 1 lower / 2 pedals, key: index on that manual, vel }
        .withEventListener ("noteOn", [this] (const juce::var& e)
        {
            processor.uiKey (juce::jlimit (0, 2, (int) e["manual"]), juce::jlimit (0, 60, (int) e["key"]),
                             juce::jlimit (1, 127, (int) e["vel"]));
        })
        .withEventListener ("noteOff", [this] (const juce::var& e)
        {
            processor.uiKey (juce::jlimit (0, 2, (int) e["manual"]), juce::jlimit (0, 60, (int) e["key"]), 0);
        })
        .withEventListener ("panic", [this] (const juce::var&) { processor.requestPanic(); })
        .withEventListener ("expression", [this] (const juce::var& e) { processor.setUiExpression ((float) (double) e["value"]); })
        // fixed preset-key registrations: { manual: 0 / 1, key: 0..8 (C#..A), bars: "00 5320 000" }
        .withEventListener ("setRegistration", [this] (const juce::var& e)
        {
            std::array<int, kNumFootages> bars {};
            if (parseRegistration (e["bars"].toString(), bars))
                processor.setRegistration ((int) e["manual"], (int) e["key"], bars);
        })
        // MIDI learn: arm a parameter (the next CC moved is assigned to it), forget it, or cancel
        .withEventListener ("midiLearn", [this] (const juce::var& e)
        {
            const int i = indexForId (e["id"].toString());
            if (i >= 0) processor.armLearn (i);
        })
        .withEventListener ("midiForget", [this] (const juce::var& e)
        {
            const int i = indexForId (e["id"].toString());
            if (i >= 0) processor.forgetLearn (i);
        })
        .withEventListener ("learnCancel", [this] (const juce::var&) { processor.cancelLearn(); })
        // a cabinet picked in the UI loads its defaults (speeds, ramps, rotors, baffle, crossover, mics)
        .withEventListener ("cabinetModel", [this] (const juce::var& e) { processor.applyCabinetModel ((int) e["model"]); })
        .withEventListener ("loadPreset", [this] (const juce::var& e)
        {
            juce::String error;
            if (! processor.loadPresetById (e["id"].toString(), error)) sendMessage (error);
            sendPreset();
        })
        .withEventListener ("savePreset", [this] (const juce::var& e)
        {
            const auto name = e["name"].toString().trim();
            bool exists = false;
            juce::String error;
            const bool ok = processor.saveUserPreset (name, (bool) e["overwrite"], exists, error);
            browser.emitEventIfBrowserIsVisible ("saveResult", makeObject ({
                { "ok", ok }, { "exists", exists }, { "name", name }, { "error", error } }));
            if (ok) { sendPresetList(); sendPreset(); }
        })
        .withEventListener ("deletePreset", [this] (const juce::var& e)
        {
            juce::String error;
            if (processor.deleteUserPreset (e["id"].toString(), error)) sendMessage ("Preset moved to the Recycle Bin");
            else sendMessage (error);
            sendPresetList();
            sendPreset();
        })
        .withEventListener ("loadPresetFile", [this] (const juce::var&) { chooseAndLoadPresetFile(); })
        // right-click on a control: the host's own parameter menu (automation, link to controller...).
        // Hosts without one get a small fallback menu.
        .withEventListener ("paramMenu", [this] (const juce::var& e) { showParamMenu (e); })
        .withEventListener ("openPresetFolder", [] (const juce::var&)
        {
            const auto folder = PresetManager::userFolder();
            folder.createDirectory();
            folder.startAsProcess();
        });
}

void TheHamEditor::chooseAndLoadPresetFile()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load The Ham preset", PresetManager::userFolder(),
                                                        juce::String ("*") + PresetManager::fileExtension);
    const auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync (chooserFlags, [safeThis = juce::Component::SafePointer<TheHamEditor> (this)] (const juce::FileChooser& fc)
    {
        if (safeThis == nullptr) return;
        const auto file = fc.getResult();
        if (file == juce::File()) return; // cancelled
        juce::String error;
        if (safeThis->processor.loadPresetFile (file, error)) safeThis->sendMessage ("Loaded " + file.getFileName());
        else safeThis->sendMessage (error);
        safeThis->sendPreset();
    });
}

void TheHamEditor::showParamMenu (const juce::var& e)
{
    const int i = indexForId (e["id"].toString());
    if (i < 0) return;
    auto* p = processor.param (i);
    const juce::Point<int> pos ((int) e["x"], (int) e["y"]);

    if (auto* host = getHostContext())
        if (auto menu = host->getContextMenuForParameter (p))
        {
            menu->showNativeMenu (pos);
            return;
        }

    // No host menu (e.g. Standalone): minimal fallback
    juce::PopupMenu m;
    m.addSectionHeader (p->getName (64) + ": " + p->getCurrentValueAsText());
    m.addItem (1, "Reset to default");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                .withTargetScreenArea (localAreaToGlobal (juce::Rectangle<int> (pos.x, pos.y, 1, 1))),
                     [safeThis = juce::Component::SafePointer<TheHamEditor> (this), i] (int result)
                     {
                         if (safeThis == nullptr || result != 1) return;
                         auto* param = safeThis->processor.param (i);
                         param->beginChangeGesture();
                         param->setValueNotifyingHost (param->getDefaultValue());
                         param->endChangeGesture();
                     });
}

void TheHamEditor::sendRegistrations()
{
    lastRegVersion = processor.registrationsVersion.load();
    const auto r = processor.registrations();
    juce::Array<juce::var> up, lo;
    for (int k = 0; k < 9; ++k)
    {
        up.add (registrationString (r[0][(size_t) k]));
        lo.add (registrationString (r[1][(size_t) k]));
    }
    browser.emitEventIfBrowserIsVisible ("registrations", makeObject ({ { "upper", up }, { "lower", lo } }));
}

void TheHamEditor::sendLearn()
{
    lastLearnVersion = processor.learnVersion.load();
    auto* map = new juce::DynamicObject();
    for (int i = 0; i < kNumParams; ++i)
        if (const int cc = processor.learnedCC (i); cc >= 0) map->setProperty (paramDefs()[(size_t) i].id, cc);
    const int armed = processor.armedParam();
    browser.emitEventIfBrowserIsVisible ("learn", makeObject ({
        { "armed", armed >= 0 ? juce::var (paramDefs()[(size_t) armed].id) : juce::var ("") }, { "map", juce::var (map) } }));
}

void TheHamEditor::sendMessage (const juce::String& text)
{
    browser.emitEventIfBrowserIsVisible ("message", makeObject ({ { "text", text } }));
}

void TheHamEditor::sendPresetList()
{
    auto& pm = processor.presetManager;
    pm.rescanIfChanged();

    juce::Array<juce::var> list;
    const auto& factory = pm.factory();
    for (size_t i = 0; i < factory.size(); ++i)
        list.add (makeObject ({ { "id", "factory:" + juce::String ((int) i) }, { "name", factory[i].name },
                                { "category", factory[i].category }, { "factory", true } }));
    for (const auto& e : pm.userPresets())
        list.add (makeObject ({ { "id", e.id }, { "name", e.name }, { "category", e.category }, { "factory", false } }));

    browser.emitEventIfBrowserIsVisible ("presetList", makeObject ({
        { "presets", list }, { "folder", PresetManager::userFolder().getFullPathName() } }));
}

void TheHamEditor::sendPreset()
{
    const auto info = processor.currentPreset();
    lastPresetVersion = processor.presetVersion.load();
    browser.emitEventIfBrowserIsVisible ("preset", makeObject ({
        { "id", info.id }, { "name", info.name }, { "category", info.category } }));
}

std::optional<juce::WebBrowserComponent::Resource> TheHamEditor::getResource (const juce::String& url)
{
    auto path = url.upToFirstOccurrenceOf ("?", false, false).trimCharactersAtStart ("/");
    if (path.isEmpty()) path = "index.html";
    const auto fileName = path.fromLastOccurrenceOf ("/", false, false);
    const auto mime = mimeForExtension (fileName.fromLastOccurrenceOf (".", false, false).toLowerCase());

    // Dev mode: set HAM_UI_DIR to the vst/ui folder to load the UI from disk
    // (edit HTML/CSS/JS and just reopen the editor, no rebuild needed).
    const auto devDir = juce::SystemStats::getEnvironmentVariable ("HAM_UI_DIR", {});
    if (devDir.isNotEmpty())
    {
        const auto f = juce::File (devDir).getChildFile (path);
        juce::MemoryBlock mb;
        if (f.existsAsFile() && f.loadFileAsData (mb))
            return juce::WebBrowserComponent::Resource { toBytes (mb.getData(), mb.getSize()), mime };
    }

    // Embedded UI. BinaryData is keyed by file name (all UI file names are unique).
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (fileName == BinaryData::originalFilenames[i])
        {
            int size = 0;
            if (const auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
                return juce::WebBrowserComponent::Resource { toBytes (data, (size_t) size), mime };
        }
    }
    return std::nullopt;
}

//==============================================================================
int TheHamEditor::indexForId (const juce::String& id) const
{
    for (int i = 0; i < kNumParams; ++i)
        if (id == paramDefs()[(size_t) i].id) return i;
    return -1;
}

void TheHamEditor::onUiReady()
{
    uiReady = true;
    lastSent.fill (std::numeric_limits<float>::quiet_NaN()); // force a full resend
    lastKeys.fill (0);
    lastRotor.fill (-1.0f);
    sendConfig();
    sendPresetList();
    sendPreset();
    sendRegistrations();
    sendLearn();
    timerCallback();
}

void TheHamEditor::onSetParam (const juce::var& e)
{
    const int i = indexForId (e["id"].toString());
    if (i < 0) return;
    auto* p = processor.param (i);
    const float v = (float) (double) e["value"];
    const bool wrap = ! inGesture[(size_t) i];   // wheel / keys / double-click: one-shot gesture
    if (wrap) p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (v));
    if (wrap) p->endChangeGesture();
    lastSent[(size_t) i] = processor.paramValue (i); // don't echo it back to the UI
}

void TheHamEditor::onGesture (const juce::var& e)
{
    const int i = indexForId (e["id"].toString());
    if (i < 0) return;
    const bool begin = (bool) e["begin"];
    if (begin == inGesture[(size_t) i]) return;
    inGesture[(size_t) i] = begin;
    if (begin) processor.param (i)->beginChangeGesture();
    else       processor.param (i)->endChangeGesture();
}

void TheHamEditor::sendConfig()
{
    const auto wrapper = juce::String (juce::AudioProcessor::getWrapperTypeDescription (processor.wrapperType));
    browser.emitEventIfBrowserIsVisible ("config", makeObject ({
        { "sampleRate", processor.getSampleRate() },
        { "blockSize",  processor.getBlockSize() },
        { "wrapper",    wrapper },
        // dev / screenshots: HAM_UI_PAGE=harm (etc.) opens that page first
        { "startPage",  juce::SystemStats::getEnvironmentVariable ("HAM_UI_PAGE", {}) },
    }));
}

void TheHamEditor::timerCallback()
{
    if (! uiReady) return;

    // Parameter changes (host automation, preset loads, state restore)
    auto* changed = new juce::DynamicObject();
    for (int i = 0; i < kNumParams; ++i)
    {
        const float v = processor.paramValue (i);
        if (v != lastSent[(size_t) i]) // NaN != anything -> always sent after uiReady
        {
            changed->setProperty (paramDefs()[(size_t) i].id, v);
            lastSent[(size_t) i] = v;
        }
    }
    juce::var changedVar (changed);
    if (changed->getProperties().size() > 0)
        browser.emitEventIfBrowserIsVisible ("params", changedVar);

    // Preset changed outside the UI (host program change, project reload)
    if (processor.presetVersion.load() != lastPresetVersion)
        sendPreset();
    if (processor.registrationsVersion.load() != lastRegVersion)
        sendRegistrations();
    if (processor.learnVersion.load() != lastLearnVersion)
        sendLearn();

    // Autoload: pick up preset files added/removed/edited in the preset folder (every ~2 s)
    if (++ticks % 60 == 0 && processor.presetManager.rescanIfChanged())
        sendPresetList();

    // Keys held on each manual (host MIDI + UI keyboards): notes { on: [[manual, key]...], off: [...] }
    juce::Array<juce::var> on, off;
    for (int m = 0; m < 3; ++m)
    {
        const uint64_t down = processor.keysDown (m), flipped = down ^ lastKeys[(size_t) m];
        if (flipped == 0) continue;
        for (int k = 0; k < 64; ++k)
            if ((flipped >> k) & 1u)
                ((down >> k) & 1u ? on : off).add (juce::var (juce::Array<juce::var> { m, k }));
        lastKeys[(size_t) m] = down;
    }
    if (! on.isEmpty() || ! off.isEmpty())
        browser.emitEventIfBrowserIsVisible ("notes", makeObject ({ { "on", on }, { "off", off } }));

    // rotors, for the cabinet drawing (every tick while they move: the UI interpolates between events)
    {
        const float h = processor.statHornTurns.load(), d = processor.statDrumTurns.load();
        const float hz = processor.statHornHz.load(), dz = processor.statDrumHz.load();
        if (h != lastRotor[0] || d != lastRotor[1] || hz != lastRotor[2] || dz != lastRotor[3])
        {
            lastRotor = { h, d, hz, dz };
            browser.emitEventIfBrowserIsVisible ("rotor", makeObject ({ { "hornAngle", h }, { "drumAngle", d },
                                                                         { "hornHz", hz }, { "drumHz", dz } }));
        }
    }

    browser.emitEventIfBrowserIsVisible ("stats", makeObject ({
        { "peak",       processor.statPeak.exchange (0.0f) },
        { "sustain",    processor.statSustain.load() },
        { "modWheel",   processor.statModWheel.load() },
        { "expression", processor.statExpression.load() },
        { "motor",      processor.statMotor.load() },
        { "perc",       processor.statPerc.load() },
        { "speed",      processor.statSpeed.load() },
    }));
}
