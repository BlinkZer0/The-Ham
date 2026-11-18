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

    p.percOn       = on (percOn);
    p.percSoft     = choice (percVol, 1) == 1;
    p.percSlow     = choice (percDecay, 1) == 1;
    p.percThird    = choice (percHarm, 1) == 1;
    p.percFastTime = num (percFast);
    p.percSlowTime = num (percSlow);
    p.vibUpper     = on (vibUpper);
    p.vibLower     = on (vibLower);
    p.vibMode      = choice (vibMode, 5);
    p.leak[0]      = num (leakUp);
    p.leak[1]      = num (leakLo);
    p.age[0]       = num (ageUp);
    p.age[1]       = num (ageLo);
    p.pedTone      = num (pedTone);
    p.pedSustain   = on (pedSustain);
    p.pedDecay     = num (pedDecay);
    p.loSustain    = on (loSustain);
    p.loSusTime    = num (loSusTime);
    p.manualBass   = on (manualBass);
    // p.regs (fixed preset-key registrations) are not parameters: the processor fills them
    // from its state; the default is the factory chart.

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

    p.speedWheel   = choice (speedWheel, 2);
    p.speedCC      = inum (speedCC);
    p.speedSustain = on (speedSustain);
    p.varSource    = choice (varSource, 2);

    p.drive       = num (drive);
    p.cabModel    = choice (cabModel, 3);
    p.cabBypass   = on (cabBypass);
    p.rotorConfig = choice (rotorConfig, 3);
    p.speedMode   = choice (speedMode, 2);
    p.speed       = choice (speed, 2);
    p.hornSel     = choice (hornSpeedSel, 3);
    p.drumSel     = choice (drumSpeedSel, 3);
    p.drumOff     = on (drumOff);
    p.hornSlow    = num (hornSlow);  p.hornFast  = num (hornFast);
    p.hornAccel   = num (hornAccel); p.hornBrake = num (hornBrake);
    p.drumSlow    = num (drumSlow);  p.drumFast  = num (drumFast);
    p.drumAccel   = num (drumAccel); p.drumBrake = num (drumBrake);
    p.hornDir     = choice (hornDir, 1);
    p.drumDir     = choice (drumDir, 1);
    p.brakePos    = on (brakePos);
    p.brakeAngle  = num (brakeAngle);
    p.hornType    = choice (hornType, 1);
    p.hornMouth   = choice (hornMouth, 1);
    p.drumBaffle  = choice (drumBaffle, 2);
    p.xover       = num (xover);
    p.hornDrumBal = num (hornDrumBal);
    p.cabBack     = choice (cabBack, 1);
    p.micLayout   = choice (micLayout, 4);
    p.micDist     = num (micDist);
    p.micAngle    = num (micAngle);
    p.micHorn     = num (micHorn);
    p.micDrum     = num (micDrum);
    p.micRoom     = num (micRoom);
    p.ampOS       = choice (ampOS, 2);
    p.width       = num (width);

    p.odOn     = on (odOn);   p.odDrive = num (odDrive); p.odTone = num (odTone); p.odLevel = num (odLevel);
    p.wahOn    = on (wahOn);  p.wahPos  = num (wahPos);  p.wahSrc = choice (wahSrc, 3); p.wahCC = inum (wahCC);
    p.echoOn   = on (echoOn); p.echoTime = num (echoTime); p.echoFb = num (echoFb); p.echoMix = num (echoMix);
    p.echoWow  = num (echoWow); p.echoTone = num (echoTone);
    p.revOn    = on (revOn);  p.revMix = num (revMix); p.revDecay = num (revDecay); p.revTone = num (revTone);
    return p;
}
} // namespace ham
