#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "MidiRouting.h"
#include "CcMap.h"

using namespace ham;

TheHamProcessor::TheHamProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "TheHamState", createParameterLayout())
{
    for (int i = 0; i < kNumParams; ++i)
    {
        const auto& id = paramDefs()[(size_t) i].id;
        params[(size_t) i] = apvts.getParameter (id);
        rawValues[(size_t) i] = apvts.getRawParameterValue (id);
        jassert (params[(size_t) i] != nullptr && rawValues[(size_t) i] != nullptr);
    }
    setCurrentProgram (0); // a fresh instance starts on the first factory preset
    startTimerHz (50);
}

//==============================================================================
void TheHamProcessor::prepareToPlay (double sampleRate, int)
{
    engine.setParams (readParams());
    engine.prepare (sampleRate);
}

bool TheHamProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

EngineParams TheHamProcessor::readParams() const
{
    return makeEngineParams ([this] (int i) { return paramValue (i); });
}

void TheHamProcessor::uiKey (int manual, int key, int vel)
{
    const auto scope = uiKeyFifo.write (1);
    if (scope.blockSize1 > 0) uiKeyBuf[(size_t) scope.startIndex1] = { (int8_t) manual, (int8_t) key, (int8_t) juce::jlimit (0, 127, vel) };
    else if (scope.blockSize2 > 0) uiKeyBuf[(size_t) scope.startIndex2] = { (int8_t) manual, (int8_t) key, (int8_t) juce::jlimit (0, 127, vel) };
}

// Drawbar CC maps: a mapped CC becomes a parameter change of the active set's drawbar,
// handed to the message thread (timerCallback), so the host and the UI see it.
bool TheHamProcessor::handleMappedCC (const juce::MidiMessage& m)
{
    if (! m.isController()) return false;
    const int map = juce::roundToInt (paramValue (ccMap));
    const auto t = drawbarForCC (map, m.getControllerNumber());
    if (t.manual < 0) return false;
    const int key = juce::roundToInt (paramValue (t.manual == 0 ? upKey : loKey));
    const int set = key == 10 ? 0 : 1;
    const auto scope = paramFifo.write (1);
    const ParamChange pc { drawbarParam (t.manual, set, t.foot), m.getControllerValue() * 8.0f / 127.0f };
    if (scope.blockSize1 > 0) paramBuf[(size_t) scope.startIndex1] = pc;
    else if (scope.blockSize2 > 0) paramBuf[(size_t) scope.startIndex2] = pc;
    return true;
}

void TheHamProcessor::timerCallback()
{
    const auto scope = paramFifo.read (paramFifo.getNumReady());
    auto apply = [this] (int start, int size)
    {
        for (int i = start; i < start + size; ++i)
        {
            auto* p = params[(size_t) paramBuf[(size_t) i].index];
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (paramBuf[(size_t) i].value));
            p->endChangeGesture();
        }
    };
    apply (scope.startIndex1, scope.blockSize1);
    apply (scope.startIndex2, scope.blockSize2);
}

void TheHamProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    engine.setParams (readParams());

    if (panicRequested.exchange (false))
        engine.panic();

    {   // keys played on the on-screen keyboards
        const auto scope = uiKeyFifo.read (uiKeyFifo.getNumReady());
        auto apply = [this] (int start, int size)
        {
            for (int i = start; i < start + size; ++i)
            {
                const auto& k = uiKeyBuf[(size_t) i];
                if (k.vel > 0) engine.keyOn (k.manual, k.key, k.vel);
                else engine.keyOff (k.manual, k.key);
            }
        };
        apply (scope.startIndex1, scope.blockSize1);
        apply (scope.startIndex2, scope.blockSize2);
    }

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    // Sample-accurate: render up to each MIDI event, then apply it.
    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, n, meta.samplePosition);
        if (t > pos)
        {
            engine.render (L + pos, R != nullptr ? R + pos : nullptr, t - pos);
            pos = t;
        }
        const auto msg = meta.getMessage();
        if (! handleMappedCC (msg))
            routeMidi (engine, msg);   // MidiRouting.h (shared with the parity tool; mirrored in the web build)
    }
    if (pos < n)
        engine.render (L + pos, R != nullptr ? R + pos : nullptr, n - pos);
    midi.clear();

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);

    for (int m = 0; m < 3; ++m)
    {
        uint64_t bits = 0;
        for (int k = 0; k < SynthEngine::keyCount (m); ++k)
            if (engine.keyDown (m, k)) bits |= (uint64_t) 1 << k;
        keyBits[(size_t) m].store (bits);
    }
    statSustain.store (engine.sustainDown());
    statModWheel.store ((float) engine.modWheelValue());
    statExpression.store ((float) engine.expressionValue());
    statMotor.store ((float) engine.motorValue());
    const float pk = engine.takePeak();
    if (pk > statPeak.load()) statPeak.store (pk);
}

//==============================================================================
// Sets every parameter except master. Anything the preset doesn't specify
// (e.g. an older file saved before a parameter existed) goes to its default.
void TheHamProcessor::applyPreset (const PresetData& preset)
{
    for (int i = 0; i < kNumParams; ++i)
    {
        if (i == master || paramDefs()[(size_t) i].noPreset) continue;
        const float stored = preset.values[(size_t) i];
        const float v = std::isnan (stored) ? paramDefs()[(size_t) i].def : stored;
        auto* p = params[(size_t) i];
        const float norm = p->convertTo0to1 (v);
        if (std::abs (p->getValue() - norm) < 1e-7f) continue; // unchanged: don't spam the host
        p->beginChangeGesture();
        p->setValueNotifyingHost (norm);
        p->endChangeGesture();
    }
}

void TheHamProcessor::setPresetInfo (const juce::String& id, const juce::String& name, const juce::String& category)
{
    {
        const juce::ScopedLock sl (presetLock);
        presetInfo = { id, name, category };
    }
    ++presetVersion;
}

TheHamProcessor::PresetInfo TheHamProcessor::currentPreset() const
{
    const juce::ScopedLock sl (presetLock);
    return presetInfo;
}

void TheHamProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, getNumPrograms() - 1, index);
    currentProgram = index;
    const auto& preset = presetManager.factory()[(size_t) index];
    applyPreset (preset);
    setPresetInfo ("factory:" + juce::String (index), preset.name, preset.category);
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

const juce::String TheHamProcessor::getProgramName (int index)
{
    if (index < 0 || index >= getNumPrograms()) return {};
    const auto& p = presetManager.factory()[(size_t) index];
    return p.category + " - " + p.name;
}

bool TheHamProcessor::loadPresetById (const juce::String& id, juce::String& error)
{
    if (id.startsWith ("factory:"))
    {
        const int index = id.fromFirstOccurrenceOf (":", false, false).getIntValue();
        if (index < 0 || index >= getNumPrograms()) { error = "Unknown preset"; return false; }
        setCurrentProgram (index);
        return true;
    }

    presetManager.rescanIfChanged();
    if (const auto* entry = presetManager.findUser (id))
    {
        PresetData data;
        if (! PresetManager::readFile (entry->file, data, error)) return false;
        applyPreset (data);
        setPresetInfo (entry->id, entry->name, entry->category);
        return true;
    }
    error = "Preset not found (was it moved or deleted?)";
    return false;
}

bool TheHamProcessor::loadPresetFile (const juce::File& file, juce::String& error)
{
    PresetData data;
    if (! PresetManager::readFile (file, data, error)) return false;
    applyPreset (data);
    const auto userId = presetManager.idForFile (file);
    if (userId.isNotEmpty())
        setPresetInfo (userId, file.getFileNameWithoutExtension(), presetManager.categoryForFile (file));
    else
        setPresetInfo ("file:" + file.getFullPathName(), data.name, "File");
    return true;
}

bool TheHamProcessor::saveUserPreset (const juce::String& rawName, bool overwrite, bool& alreadyExists, juce::String& error)
{
    alreadyExists = false;
    const auto name = rawName.trim();
    if (name.isEmpty()) { error = "Enter a name for the preset"; return false; }

    const auto file = presetManager.fileForName (name);
    if (file.existsAsFile() && ! overwrite) { alreadyExists = true; return false; }

    PresetData data;
    data.name = name;
    data.category = "User";
    for (int i = 0; i < kNumParams; ++i)
        if (i != master && ! paramDefs()[(size_t) i].noPreset) data.values[(size_t) i] = paramValue (i);

    if (! PresetManager::writeFile (file, data, error)) return false;
    presetManager.rescanIfChanged();
    setPresetInfo (presetManager.idForFile (file), file.getFileNameWithoutExtension(), presetManager.categoryForFile (file));
    return true;
}

bool TheHamProcessor::deleteUserPreset (const juce::String& id, juce::String& error)
{
    presetManager.rescanIfChanged();
    const auto* entry = presetManager.findUser (id);
    if (entry == nullptr) { error = "Only user presets can be deleted"; return false; }
    const auto file = entry->file;
    if (! file.moveToTrash() && ! file.deleteFile()) { error = "Couldn't delete " + file.getFullPathName(); return false; }
    presetManager.rescanIfChanged();
    if (currentPreset().id == id)                       // keep the sound, drop the link to the file
        setPresetInfo ({}, currentPreset().name, "User");
    return true;
}

//==============================================================================
void TheHamProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    const auto info = currentPreset();
    state.setProperty ("program", currentProgram.load(), nullptr);
    state.setProperty ("presetId", info.id, nullptr);
    state.setProperty ("presetName", info.name, nullptr);
    state.setProperty ("presetCategory", info.category, nullptr);
    state.setProperty ("stateVersion", kPresetVersion, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TheHamProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            currentProgram = juce::jlimit (0, getNumPrograms() - 1, (int) state.getProperty ("program", 0));
            const auto name = state.getProperty ("presetName").toString();
            if (name.isEmpty())
            {
                const auto& p = presetManager.factory()[(size_t) currentProgram.load()];
                setPresetInfo ("factory:" + juce::String (currentProgram.load()), p.name, p.category);
            }
            else
            {
                setPresetInfo (state.getProperty ("presetId").toString(), name,
                               state.getProperty ("presetCategory").toString());
            }
            apvts.replaceState (state);
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* TheHamProcessor::createEditor()
{
    return new TheHamEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TheHamProcessor();
}
