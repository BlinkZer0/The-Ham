#pragma once
// Maps raw parameter values (indexed by ParamIndex) to the engine's snapshot.
// Shared by the plugin (atomic APVTS values) and the console tests.
#include "Params.h"
#include "SynthEngine.h"

namespace ham
{
template <typename GetValue> // float (int paramIndex)
EngineParams makeEngineParams (GetValue&& get)
{
    auto num    = [&get] (int index) { return (double) get (index); };
    auto choice = [&get] (int index, int maxIndex) { return juce::jlimit (0, maxIndex, (int) std::lround (get (index))); };
    auto on     = [&get] (int index) { return get (index) >= 0.5f; };
    auto inum   = [&get] (int index) { return (int) std::lround (get (index)); };

    EngineParams p;
    p.organModel = choice (organModel, organModelChoices().size() - 1);
    p.ampGain    = std::pow (10.0, num (ampLevel) / 20.0);
    p.master     = num (master);
    p.tuneRatio  = num (tuneA) / 440.0;
    p.mains      = choice (mains, 1);
    p.transpose  = inum (transpose);
    p.motorStart = on (motorStart);
    p.motorRun   = on (motorRun);
    p.velMode    = choice (velMode, 2);
    p.velAmount  = num (velAmount);

    for (int m = 0; m < 2; ++m)
        for (int s = 0; s < 2; ++s)
            for (int f = 0; f < kNumFootages; ++f)
                p.bars[m][s][f] = num (drawbarParam (m, s, f));
    p.pedalBars[0] = num (pd16);
    p.pedalBars[1] = num (pd8);
    p.presetKey[0] = choice (upKey, 11);
    p.presetKey[1] = choice (loKey, 11);

    p.click[0]   = num (clickUp);
    p.click[1]   = num (clickLo);
    p.click[2]   = num (clickPd);
    p.keyUpClick = on (keyUpClick);

    p.channel[0] = inum (chUpper) - 1;
    p.channel[1] = inum (chLower) - 1;
    p.channel[2] = inum (chPedal) - 1;
    p.splitMode  = on (splitMode);
    p.splitPoint = inum (splitPoint);
    p.splitLoOct = inum (splitLoOct);
    p.pedalZone  = on (pedalZone);
    p.pedalSplit = inum (pedalSplit);
    p.pedMidiBase = inum (pedMidiBase);
    p.motorCC    = inum (motorCC);
    p.exprCC     = inum (exprCC);
    return p;
}
} // namespace ham
