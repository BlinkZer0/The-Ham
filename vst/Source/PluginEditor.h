#pragma once

#include <bitset>
#include "PluginProcessor.h"

// Hosts the HTML/SVG UI (vst/ui, compiled in as BinaryData) in a WebView2 and
// bridges it to the processor:
//   UI -> C++ events : uiReady, setParam{id,value}, gesture{id,begin},
//                      noteOn{manual,key,vel}, noteOff{manual,key}, panic,
//                      loadPreset{id}, savePreset{name,overwrite}, deletePreset{id},
//                      loadPresetFile, openPresetFolder, paramMenu{id,x,y}
//   C++ -> UI events : params{id:value...}, notes{on:[[manual,key]...],off:[...]},
//                      stats{peak,sustain,modWheel,expression,motor},
//                      config{...}, presetList{presets[],folder}, preset{id,name,category},
//                      saveResult{ok,exists,name,error}, message{text}
class TheHamEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TheHamEditor (TheHamProcessor&);
    ~TheHamEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    juce::WebBrowserComponent::Options makeBrowserOptions();
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    int indexForId (const juce::String& id) const;
    void onUiReady();
    void onSetParam (const juce::var&);
    void onGesture (const juce::var&);
    void sendConfig();
    void sendPresetList();
    void sendPreset();
    void sendMessage (const juce::String& text);
    void chooseAndLoadPresetFile();
    void showParamMenu (const juce::var&);

    TheHamProcessor& processor;

    std::array<float, ham::kNumParams> lastSent {};
    std::array<bool, ham::kNumParams> inGesture {};
    std::array<uint64_t, 3> lastKeys {};
    int lastPresetVersion = -1;
    int ticks = 0;
    bool uiReady = false;
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::WebBrowserComponent browser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TheHamEditor)
};
