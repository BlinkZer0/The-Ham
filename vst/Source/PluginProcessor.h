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
    double getTailLengthSeconds() const override { return 10.0; }  // tape echo repeats + spring reverb

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

    // On-screen expression pedal (0 closed .. 1 open), applied at the next block.
    void setUiExpression (float v) { uiExpression = juce::jlimit (0.0f, 1.0f, v); }

    // Fixed registrations of the preset keys C#..A (not host parameters: saved in the project
    // state and in presets). Message thread writes, audio thread reads (atomics).
    ham::Registrations registrations() const;
    void setRegistrations (const ham::Registrations&);
    void setRegistration (int manual, int key, const std::array<int, ham::kNumFootages>& bars);
    std::atomic<int> registrationsVersion { 0 };

    // MIDI learn: arm a parameter, the next learnable CC is assigned to it. One CC per parameter.
    void armLearn (int paramIndex) { learnArmed = paramIndex; ++learnVersion; }
    void cancelLearn() { learnArmed = -1; ++learnVersion; }
    void forgetLearn (int paramIndex);
    int learnedCC (int paramIndex) const;            // -1 = none
    int armedParam() const { return learnArmed.load(); }
    std::atomic<int> learnVersion { 0 };

    std::atomic<float> statPeak { 0.0f };
    std::atomic<bool>  statSustain { false };
    std::atomic<float> statModWheel { 0.0f };
    std::atomic<float> statExpression { 1.0f };
    std::atomic<float> statMotor { 1.0f };
    std::atomic<float> statPerc { 0.0f };      // percussion envelope (for the UI lamp)
    // rotors (for the cabinet drawing): angle in turns, speed in Hz; speed switch 0 stop / 1 slow / 2 fast
    std::atomic<float> statHornTurns { 0.0f }, statDrumTurns { 0.0f }, statHornHz { 0.0f }, statDrumHz { 0.0f };
    std::atomic<int> statSpeed { 1 };

    // A cabinet model picked in the UI: the model and its defaults (speeds, ramps, rotors,
    // baffle, crossover, back, mics) as one undoable set of parameter changes.
    void applyCabinetModel (int model);

private:
    void applyPreset (const ham::PresetData&);
    void setPresetInfo (const juce::String& id, const juce::String& name, const juce::String& category);
    ham::EngineParams readParams() const;
    bool handleMappedCC (const juce::MidiMessage&);
    bool handleLearnAndKeyswitches (const juce::MidiMessage&);
    void pushParamChange (int index, float value, bool normalized);
    void timerCallback() override;   // applies parameter changes requested by the audio thread; latency
    int reportedOS = -1;
    juce::String learnStateString() const;
    void restoreLearnState (const juce::String&);

    std::array<juce::RangedAudioParameter*, ham::kNumParams> params {};
    std::array<std::atomic<float>*, ham::kNumParams> rawValues {};
    ham::SynthEngine engine;
    std::atomic<int> currentProgram { 0 };
    std::atomic<bool> panicRequested { false };

    struct UiKey { int8_t manual, key, vel; };
    juce::AbstractFifo uiKeyFifo { 512 };
    std::array<UiKey, 512> uiKeyBuf {};

    // audio thread -> message thread parameter changes (drawbar CC maps, MIDI learn, keyswitches)
    struct ParamChange { int index; float value; bool normalized; };
    juce::AbstractFifo paramFifo { 1024 };
    std::array<ParamChange, 1024> paramBuf {};

    std::array<std::atomic<uint64_t>, 3> keyBits {};
    std::atomic<float> uiExpression { -1.0f };   // -1 = no pending change

    std::array<std::atomic<int>, 2 * 9 * ham::kNumFootages> regStore {};
    std::array<std::atomic<int>, 128> learnMap {};   // CC -> parameter index, -1 = none
    std::atomic<int> learnArmed { -1 };

    mutable juce::CriticalSection presetLock;
    PresetInfo presetInfo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TheHamProcessor)
};
