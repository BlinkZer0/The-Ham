#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Params.h"
#include "PresetManager.h"
#include "SynthEngine.h"
#include "EngineParamsFromValues.h"

class TheHamProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    TheHamProcessor();
    ~TheHamProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 6.0; }   // longest reverb decay (phase 3)

    // The factory presets are exposed to the host as programs.
    int getNumPrograms() override { return (int) presetManager.factory().size(); }
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Presets (message thread)
    struct PresetInfo { juce::String id, name, category; };
    PresetInfo currentPreset() const;
    std::atomic<int> presetVersion { 0 };   // bumps whenever currentPreset() changes

    bool loadPresetById (const juce::String& id, juce::String& error);
    bool loadPresetFile (const juce::File& file, juce::String& error);
    bool saveUserPreset (const juce::String& name, bool overwrite, bool& alreadyExists, juce::String& error);
    bool deleteUserPreset (const juce::String& id, juce::String& error);

    ham::PresetManager presetManager;

    //==============================================================================
    // Used by the editor (message thread)
    juce::AudioProcessorValueTreeState apvts;
    juce::RangedAudioParameter* param (int index) const { return params[(size_t) index]; }
    float paramValue (int index) const { return rawValues[(size_t) index]->load(); }
    void requestPanic() { panicRequested = true; }

    // On-screen keyboards: manual 0 upper, 1 lower, 2 pedals; key index on that manual.
    // Lock-free queue, applied at the start of the next audio block.
    void uiKey (int manual, int key, int vel);
    // keys currently held on each manual (bit k = key k), from any source
    uint64_t keysDown (int manual) const { return keyBits[(size_t) juce::jlimit (0, 2, manual)].load(); }

    std::atomic<float> statPeak { 0.0f };
    std::atomic<bool>  statSustain { false };
    std::atomic<float> statModWheel { 0.0f };
    std::atomic<float> statExpression { 1.0f };
    std::atomic<float> statMotor { 1.0f };

private:
    void applyPreset (const ham::PresetData&);
    void setPresetInfo (const juce::String& id, const juce::String& name, const juce::String& category);
    ham::EngineParams readParams() const;
    bool handleMappedCC (const juce::MidiMessage&);
    void timerCallback() override;   // applies parameter changes requested by the audio thread

    std::array<juce::RangedAudioParameter*, ham::kNumParams> params {};
    std::array<std::atomic<float>*, ham::kNumParams> rawValues {};
    ham::SynthEngine engine;
    std::atomic<int> currentProgram { 0 };
    std::atomic<bool> panicRequested { false };

    struct UiKey { int8_t manual, key, vel; };
    juce::AbstractFifo uiKeyFifo { 512 };
    std::array<UiKey, 512> uiKeyBuf {};

    // audio thread -> message thread parameter changes (drawbar CC maps; MIDI learn in phase 2)
    struct ParamChange { int index; float value; };
    juce::AbstractFifo paramFifo { 1024 };
    std::array<ParamChange, 1024> paramBuf {};

    std::array<std::atomic<uint64_t>, 3> keyBits {};

    mutable juce::CriticalSection presetLock;
    PresetInfo presetInfo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TheHamProcessor)
};
