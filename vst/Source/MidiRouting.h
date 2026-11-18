#pragma once
// How a MIDI message reaches the engine. Shared by the plugin (PluginProcessor) and the
// parity tool (phase 6), and mirrored in vst/webhost/dsp-worklet.js.
// Drawbar CC maps and MIDI learn act on parameters, so they live in the processor
// (CcMap.h), not here.
#include <juce_audio_basics/juce_audio_basics.h>
#include "SynthEngine.h"

namespace ham
{
inline void routeMidi (SynthEngine& engine, const juce::MidiMessage& m)
{
    const int ch = juce::jmax (0, m.getChannel() - 1);
    if (m.isNoteOn())                  engine.noteOn (m.getNoteNumber(), m.getVelocity(), ch);
    else if (m.isNoteOff())            engine.noteOff (m.getNoteNumber(), ch);
    else if (m.isAllSoundOff())        engine.panic();
    else if (m.isAllNotesOff())        engine.allNotesOff();
    else if (m.isController())         engine.setController (m.getControllerNumber(), m.getControllerValue() / 127.0, ch);
}
} // namespace ham
