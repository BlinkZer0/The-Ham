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
    for (auto& m : learnMap) m = -1;
    setRegistrations (factoryRegistrations());
    setCurrentProgram (0); // a fresh instance starts on the first factory preset
    startTimerHz (50);
}

//==============================================================================
void TheHamProcessor::prepareToPlay (double sampleRate, int)
{
    engine.setParams (readParams());
    engine.prepare (sampleRate);
    reportedOS = juce::roundToInt (paramValue (ampOS));
    setLatencySamples (SynthEngine::latencySamples (reportedOS));
}

void TheHamProcessor::applyCabinetModel (int model)
{
    model = juce::jlimit (0, 3, model);
    const auto& M = cabinetModel (model);
    const std::pair<int, float> values[] {
        { cabModel, (float) model }, { rotorConfig, (float) M.rotorConfig },
        { hornSlow, (float) M.hornSlow }, { hornFast, (float) M.hornFast }, { hornAccel, (float) M.hornAccel }, { hornBrake, (float) M.hornBrake },
        { drumSlow, (float) M.drumSlow }, { drumFast, (float) M.drumFast }, { drumAccel, (float) M.drumAccel }, { drumBrake, (float) M.drumBrake },
        { drumBaffle, (float) M.drumBaffle }, { xover, (float) M.xover }, { cabBack, (float) M.cabBack }, { micLayout, (float) M.micLayout },
    };
    for (const auto& [index, v] : values)
    {
        auto* p = params[(size_t) index];
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (v));
        p->endChangeGesture();
    }
}

bool TheHamProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

EngineParams TheHamProcessor::readParams() const
{
    auto p = makeEngineParams ([this] (int i) { return paramValue (i); });
    for (int m = 0; m < 2; ++m)
        for (int k = 0; k < 9; ++k)
            for (int f = 0; f < kNumFootages; ++f)
                p.regs[m][k][f] = regStore[(size_t) ((m * 9 + k) * kNumFootages + f)].load();
    return p;
}

//==============================================================================
Registrations TheHamProcessor::registrations() const
{
    Registrations r {};
    for (int m = 0; m < 2; ++m)
        for (int k = 0; k < 9; ++k)
            for (int f = 0; f < kNumFootages; ++f)
                r[(size_t) m][(size_t) k][(size_t) f] = regStore[(size_t) ((m * 9 + k) * kNumFootages + f)].load();
    return r;
}

void TheHamProcessor::setRegistrations (const Registrations& r)
{
    for (int m = 0; m < 2; ++m)
        for (int k = 0; k < 9; ++k)
            for (int f = 0; f < kNumFootages; ++f)
                regStore[(size_t) ((m * 9 + k) * kNumFootages + f)] = juce::jlimit (0, 8, r[(size_t) m][(size_t) k][(size_t) f]);
    ++registrationsVersion;
}

void TheHamProcessor::setRegistration (int manual, int key, const std::array<int, kNumFootages>& bars)
{
    if (manual < 0 || manual > 1 || key < 0 || key > 8) return;
    for (int f = 0; f < kNumFootages; ++f)
        regStore[(size_t) ((manual * 9 + key) * kNumFootages + f)] = juce::jlimit (0, 8, bars[(size_t) f]);
    ++registrationsVersion;
}

//==============================================================================
void TheHamProcessor::forgetLearn (int paramIndex)
{
    for (auto& m : learnMap)
        if (m.load() == paramIndex) m = -1;
    if (learnArmed.load() == paramIndex) learnArmed = -1;
    ++learnVersion;
}

int TheHamProcessor::learnedCC (int paramIndex) const
{
    for (int cc = 0; cc < 128; ++cc)
        if (learnMap[(size_t) cc].load() == paramIndex) return cc;
    return -1;
}

// "cc=paramId;cc=paramId..." (parameter IDs are stable)
juce::String TheHamProcessor::learnStateString() const
{
    juce::String s;
    for (int cc = 0; cc < 128; ++cc)
        if (const int i = learnMap[(size_t) cc].load(); i >= 0 && i < kNumParams)
            s << cc << '=' << paramDefs()[(size_t) i].id << ';';
    return s;
}

void TheHamProcessor::restoreLearnState (const juce::String& s)
{
    for (auto& m : learnMap) m = -1;
    for (const auto& item : juce::StringArray::fromTokens (s, ";", ""))
    {
        const int cc = item.upToFirstOccurrenceOf ("=", false, false).getIntValue();
        const auto id = item.fromFirstOccurrenceOf ("=", false, false);
        for (int i = 0; i < kNumParams; ++i)
            if (paramDefs()[(size_t) i].id == id && cc >= 0 && cc < 128) learnMap[(size_t) cc] = i;
    }
    learnArmed = -1;
    ++learnVersion;
}

void TheHamProcessor::uiKey (int manual, int key, int vel)
{
    const auto scope = uiKeyFifo.write (1);
    if (scope.blockSize1 > 0) uiKeyBuf[(size_t) scope.startIndex1] = { (int8_t) manual, (int8_t) key, (int8_t) juce::jlimit (0, 127, vel) };
    else if (scope.blockSize2 > 0) uiKeyBuf[(size_t) scope.startIndex2] = { (int8_t) manual, (int8_t) key, (int8_t) juce::jlimit (0, 127, vel) };
}

void TheHamProcessor::pushParamChange (int index, float value, bool normalized)
{
    const auto scope = paramFifo.write (1);
    const ParamChange pc { index, value, normalized };
    if (scope.blockSize1 > 0) paramBuf[(size_t) scope.startIndex1] = pc;
    else if (scope.blockSize2 > 0) paramBuf[(size_t) scope.startIndex2] = pc;
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
    pushParamChange (drawbarParam (t.manual, set, t.foot), m.getControllerValue() * 8.0f / 127.0f, false);
    return true;
}

// MIDI learn (an armed parameter takes the next learnable CC; learned CCs win over the drawbar
// maps) and the preset-key keyswitches. Returns true if the message was used up here.
bool TheHamProcessor::handleLearnAndKeyswitches (const juce::MidiMessage& m)
{
    if (m.isController())
    {
        const int cc = m.getControllerNumber();
        if (! learnableCC (cc)) return false;
        if (const int armed = learnArmed.load(); armed >= 0)
        {
            for (auto& slot : learnMap)
                if (slot.load() == armed) slot = -1;
            learnMap[(size_t) cc] = armed;
            learnArmed = -1;
            ++learnVersion;
        }
        if (const int target = learnMap[(size_t) cc].load(); target >= 0)
        {
            pushParamChange (target, m.getControllerValue() / 127.0f, true);
            return true;
        }
        return false;
    }
    if (m.isNoteOnOrOff())
    {
        const auto ks = presetKeyswitch (m.getNoteNumber(), m.getChannel() - 1, paramValue (ksOn) >= 0.5f,
                                         paramValue (splitMode) >= 0.5f, juce::roundToInt (paramValue (chUpper)) - 1,
                                         juce::roundToInt (paramValue (chLower)) - 1);
        if (ks.manual < 0) return false;
        if (m.isNoteOn()) pushParamChange (ks.manual == 0 ? upKey : loKey, (float) ks.key, false);
        return true;   // keyswitches never sound
    }
    return false;
}

void TheHamProcessor::timerCallback()
{
    const auto scope = paramFifo.read (paramFifo.getNumReady());
    auto apply = [this] (int start, int size)
    {
        for (int i = start; i < start + size; ++i)
        {
            const auto& pc = paramBuf[(size_t) i];
            auto* p = params[(size_t) pc.index];
            p->beginChangeGesture();
            p->setValueNotifyingHost (pc.normalized ? pc.value : p->convertTo0to1 (pc.value));
            p->endChangeGesture();
        }
    };
    apply (scope.startIndex1, scope.blockSize1);
    apply (scope.startIndex2, scope.blockSize2);

    // the amp's oversampling filters delay the output a few samples: tell the host
    if (const int os = juce::roundToInt (paramValue (ampOS)); os != reportedOS)
    {
        reportedOS = os;
        setLatencySamples (SynthEngine::latencySamples (os));
    }
}

void TheHamProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    engine.setParams (readParams());

    if (panicRequested.exchange (false))
        engine.panic();
    if (const float e = uiExpression.exchange (-1.0f); e >= 0.0f)
        engine.setExpression (e);

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
        if (! handleLearnAndKeyswitches (msg) && ! handleMappedCC (msg))
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
    statPerc.store ((float) engine.percussionLevel());
    statHornTurns.store ((float) engine.hornTurns());
    statDrumTurns.store ((float) engine.drumTurns());
    statHornHz.store ((float) engine.hornSpeed());
    statDrumHz.store ((float) engine.drumSpeed());
    statSpeed.store (engine.speedSwitch());
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
    setRegistrations (preset.registrations);
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
    data.registrations = registrations();

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
    {
        juce::StringArray regs;
        const auto r = registrations();
        for (int m = 0; m < 2; ++m)
            for (int k = 0; k < 9; ++k) regs.add (registrationString (r[(size_t) m][(size_t) k]));
        state.setProperty ("registrations", regs.joinIntoString ("|"), nullptr);
    }
    state.setProperty ("midiLearn", learnStateString(), nullptr);

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
            {   // fixed preset-key registrations (missing in projects saved before phase 2: factory chart)
                auto r = factoryRegistrations();
                const auto regs = juce::StringArray::fromTokens (state.getProperty ("registrations").toString(), "|", "");
                for (int i = 0; i < juce::jmin (18, regs.size()); ++i)
                    parseRegistration (regs[i], r[(size_t) (i / 9)][(size_t) (i % 9)]);
                setRegistrations (r);
            }
            restoreLearnState (state.getProperty ("midiLearn").toString());
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
