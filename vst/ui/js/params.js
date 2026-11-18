// ============================================================================
//  Parameter definitions. Must match vst/Source/Params.h exactly (IDs, ranges,
//  defaults, skew, choice lists, ORDER); tests/check-params.ps1 compares them.
//  New parameters are only appended. The web build is generated from this file.
// ============================================================================
'use strict';

const ORGAN_MODELS = ['B-3', 'C-3', 'A-100', 'M-3'];
const MAINS = ['60 Hz', '50 Hz'];
const VEL_MODES = ['Off', 'Volume', 'Click'];
const PRESET_KEYS = ['C Cancel', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A# Set A', 'B Set B'];
const PERC_VOLS = ['Normal', 'Soft'];
const PERC_DECAYS = ['Fast', 'Slow'];
const PERC_HARMS = ['Second', 'Third'];
const VIB_MODES = ['V1', 'V2', 'V3', 'C1', 'C2', 'C3'];
const CC_MAPS = ['Off', 'B4D (12-20 / 21-29)', 'Nord Electro (16-24 / 70-78)', 'VK / CX-3 (70-78)'];
const SPEED_WHEELS = ['Off', 'Half Moon', 'Momentary'];
const VAR_SOURCES = ['Mod Wheel', 'Expression', 'Speed CC'];
const CAB_MODELS = ['Rotor 22', 'Rotor 45', 'Single Rotor', 'Tone Cabinet'];
const ROTOR_CONFIGS = ['Horn + Drum', 'Horn Only', 'Drum Only', 'Stationary'];
const SPEED_MODES = ['2-Speed', '3-Speed', 'Variable'];
const SPEEDS = ['Stop', 'Slow', 'Fast'];
const ROTOR_SELS = ['Follow', 'Stop', 'Slow', 'Fast'];
const DIRS = ['CCW', 'CW'];
const HORN_TYPES = ['Dual (1 Dummy)', 'Dual Active'];
const HORN_MOUTHS = ['Stock', 'Short'];
const DRUM_BAFFLES = ['Stock', 'Open', 'Foam'];
const CAB_BACKS = ['Closed', 'Open'];
const MIC_LAYOUTS = ['Mono', 'Stereo Horn', 'Wide Stereo', '3-Mic', 'Room Pair'];
const OVERSAMPLE = ['1x', '2x', '4x'];
const WAH_SRCS = ['Knob', 'Expression', 'Mod Wheel', 'CC'];

// footages, in drawbar order, and their tip colours (brown = sub-octave / fifth, white = octaves,
// black = the other harmonics), as on the instrument
const FOOTAGES = ["16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"];
const FOOTAGE_SHORT = ['16', '5⅓', '8', '4', '2⅔', '2', '1⅗', '1⅓', '1'];
const FOOTAGE_TIPS = ['brown', 'brown', 'white', 'white', 'black', 'white', 'black', 'black', 'white'];
const MANUAL_KEYS = 61;
const PEDAL_KEYS = 25;

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
function midiNoteName(n) { return NOTE_NAMES[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1); }

// ---- builders (same shapes as Params.h) ----------------------------------------
const P = (id, name, group, min, max, def, unit, extra) =>
  Object.assign({ id, name, group, type: 'float', min, max, def, unit: unit || '' }, extra || {});
const L = (id, name, group, min, max, def, unit) => P(id, name, group, min, max, def, unit, { skew: 'log' });
const I = (id, name, group, min, max, def, unit) => P(id, name, group, min, max, def, unit || 'count', { type: 'int', step: 1 });
const B = (id, name, group, def) => ({ id, name, group, type: 'bool', min: 0, max: 1, step: 1, def: def ? 1 : 0, unit: '' });
const C = (id, name, group, choices, def) =>
  ({ id, name, group, type: 'choice', min: 0, max: choices.length - 1, step: 1, def, choices, unit: '' });
const BIP = { bipolar: true };

// 9 drawbars of one set. Factory sets: upper A 00 8800 000, B 88 8000 000; lower A 00 8000 000, B 83 8000 000.
const DRAWBAR_DEFAULTS = {
  upA: [0, 0, 8, 8, 0, 0, 0, 0, 0], upB: [8, 8, 8, 0, 0, 0, 0, 0, 0],
  loA: [0, 0, 8, 0, 0, 0, 0, 0, 0], loB: [8, 3, 8, 0, 0, 0, 0, 0, 0],
};
function drawbarSet(prefix, hostPrefix) {
  return FOOTAGES.map((f, i) =>
    P(`${prefix}${i + 1}`, `${hostPrefix} ${f}`, 'drawbar', 0, 8, DRAWBAR_DEFAULTS[prefix][i], 'bar'));
}
// manual 0 upper / 1 lower, set 0 = A / 1 = B, foot 0..8
function drawbarId(manual, set, foot) { return `${manual ? 'lo' : 'up'}${set ? 'B' : 'A'}${foot + 1}`; }

const PARAM_DEFS = [
  // ---- global
  C('organModel', 'Organ Model', 'global', ORGAN_MODELS, 0),
  P('ampLevel', 'Level', 'global', -30, 6, 0, 'dB'),
  P('master', 'Master', 'output', 0, 1, 0.7, '%', { noPreset: true }),
  P('tuneA', 'Tuning A', 'global', 430, 450, 440, 'lfohz'),
  C('mains', 'Mains', 'global', MAINS, 0),
  I('transpose', 'Transpose', 'global', -12, 12, 0, 'st'),
  B('motorStart', 'Start', 'motor', 1),
  B('motorRun', 'Run', 'motor', 1),
  C('velMode', 'Velocity', 'play', VEL_MODES, 0),
  P('velAmount', 'Vel Amount', 'play', 0, 1, 0.5, '%'),
  // ---- drawbars
  ...drawbarSet('upA', 'Upper A'),
  ...drawbarSet('upB', 'Upper B'),
  ...drawbarSet('loA', 'Lower A'),
  ...drawbarSet('loB', 'Lower B'),
  P('pd16', "Pedal 16'", 'drawbar', 0, 8, 5, 'bar'),
  P('pd8', "Pedal 8'", 'drawbar', 0, 8, 3, 'bar'),
  C('upKey', 'Upper Preset Key', 'drawbar', PRESET_KEYS, 11),
  C('loKey', 'Lower Preset Key', 'drawbar', PRESET_KEYS, 11),
  // ---- percussion
  B('percOn', 'Percussion', 'perc', 0),
  C('percVol', 'Volume', 'perc', PERC_VOLS, 0),
  C('percDecay', 'Decay', 'perc', PERC_DECAYS, 0),
  C('percHarm', 'Harmonic', 'perc', PERC_HARMS, 1),
  L('percFast', 'Fast Time', 'perc', 0.05, 3, 0.3, 's'),
  L('percSlow', 'Slow Time', 'perc', 0.2, 8, 1.0, 's'),
  // ---- vibrato / chorus
  B('vibUpper', 'Upper', 'vib', 0),
  B('vibLower', 'Lower', 'vib', 0),
  C('vibMode', 'Vibrato', 'vib', VIB_MODES, 5),
  // ---- manual character
  P('clickUp', 'Click Upper', 'char', 0, 1, 0.5, '%'),
  P('clickLo', 'Click Lower', 'char', 0, 1, 0.5, '%'),
  P('clickPd', 'Click Pedal', 'char', 0, 1, 0.3, '%'),
  B('keyUpClick', 'Key-Up Click', 'char', 1),
  P('leakUp', 'Leak Upper', 'char', 0, 1, 0.3, '%'),
  P('leakLo', 'Leak Lower', 'char', 0, 1, 0.3, '%'),
  P('ageUp', 'Age Upper', 'char', 0, 1, 0.3, '%'),
  P('ageLo', 'Age Lower', 'char', 0, 1, 0.3, '%'),
  // ---- pedals / lower
  P('pedTone', 'Pedal Tone', 'pedal', 0, 1, 0.5, '%'),
  B('pedSustain', 'Pedal Sustain', 'pedal', 0),
  L('pedDecay', 'Pedal Decay', 'pedal', 0.05, 4, 0.6, 's'),
  B('loSustain', 'Lower Sustain', 'pedal', 0),
  L('loSusTime', 'Lower Sus Time', 'pedal', 0.05, 4, 0.8, 's'),
  B('manualBass', 'Manual Bass', 'pedal', 0),
  // ---- preamp
  P('drive', 'Drive', 'preamp', 0, 1, 0.2, '%'),
  I('exprCC', 'Expression CC', 'midi', 1, 119, 11),
  // ---- MIDI
  I('chUpper', 'Upper Ch', 'midi', 1, 16, 1),
  I('chLower', 'Lower Ch', 'midi', 1, 16, 2),
  I('chPedal', 'Pedal Ch', 'midi', 1, 16, 3),
  B('splitMode', 'Split', 'midi', 0),
  I('splitPoint', 'Split Point', 'midi', 24, 96, 60, 'note'),
  I('splitLoOct', 'Lower Octave', 'midi', -2, 2, 0, 'count'),
  B('pedalZone', 'Pedal Zone', 'midi', 0),
  I('pedalSplit', 'Pedal Split', 'midi', 24, 72, 48, 'note'),
  I('pedMidiBase', 'Pedal Base', 'midi', 0, 96, 36, 'note'),
  B('ksOn', 'Preset Keyswitches', 'midi', 0),
  C('ccMap', 'Drawbar CC Map', 'midi', CC_MAPS, 1),
  C('speedWheel', 'Mod Wheel Speed', 'midi', SPEED_WHEELS, 1),
  I('speedCC', 'Speed CC', 'midi', 0, 127, 0, 'count'),
  B('speedSustain', 'Sustain → Speed', 'midi', 0),
  I('motorCC', 'Motor Off CC', 'midi', 0, 127, 0, 'count'),
  C('varSource', 'Variable Source', 'midi', VAR_SOURCES, 0),
  // ---- rotary cabinet
  C('cabModel', 'Cabinet', 'cab', CAB_MODELS, 0),
  B('cabBypass', 'Bypass', 'cab', 0),
  C('rotorConfig', 'Rotors', 'cab', ROTOR_CONFIGS, 0),
  C('speedMode', 'Speed Mode', 'cab', SPEED_MODES, 0),
  C('speed', 'Speed', 'cab', SPEEDS, 1),
  C('hornSpeedSel', 'Horn Speed', 'cab', ROTOR_SELS, 0),
  C('drumSpeedSel', 'Drum Speed', 'cab', ROTOR_SELS, 0),
  B('drumOff', 'Drum Off', 'cab', 0),
  P('hornSlow', 'Horn Slow', 'cab', 0.1, 3, 0.8, 'lfohz'),
  P('hornFast', 'Horn Fast', 'cab', 2, 10, 6.7, 'lfohz'),
  L('hornAccel', 'Horn Accel', 'cab', 0.1, 12, 1.0, 's'),
  L('hornBrake', 'Horn Brake', 'cab', 0.1, 12, 1.5, 's'),
  P('drumSlow', 'Drum Slow', 'cab', 0.1, 3, 0.7, 'lfohz'),
  P('drumFast', 'Drum Fast', 'cab', 2, 10, 5.8, 'lfohz'),
  L('drumAccel', 'Drum Accel', 'cab', 0.1, 12, 4.0, 's'),
  L('drumBrake', 'Drum Brake', 'cab', 0.1, 12, 5.0, 's'),
  C('hornDir', 'Horn Dir', 'cab', DIRS, 0),
  C('drumDir', 'Drum Dir', 'cab', DIRS, 1),
  B('brakePos', 'Brake Position', 'cab', 0),
  P('brakeAngle', 'Stop Angle', 'cab', 0, 360, 0, 'deg'),
  C('hornType', 'Horn', 'cab', HORN_TYPES, 0),
  C('hornMouth', 'Horn Mouth', 'cab', HORN_MOUTHS, 0),
  C('drumBaffle', 'Drum Baffle', 'cab', DRUM_BAFFLES, 0),
  L('xover', 'Crossover', 'cab', 300, 3000, 800, 'Hz'),
  P('hornDrumBal', 'Horn/Drum', 'cab', -1, 1, 0, 'bip', BIP),
  C('cabBack', 'Back', 'cab', CAB_BACKS, 0),
  C('micLayout', 'Mics', 'cab', MIC_LAYOUTS, 1),
  P('micDist', 'Mic Distance', 'cab', 0, 1, 0.4, '%'),
  P('micAngle', 'Mic Angle', 'cab', 0, 180, 90, 'deg'),
  P('micHorn', 'Horn Mic', 'cab', 0, 1, 1, '%'),
  P('micDrum', 'Drum Mic', 'cab', 0, 1, 1, '%'),
  P('micRoom', 'Room Mic', 'cab', 0, 1, 0, '%'),
  C('ampOS', 'Amp OS', 'cab', OVERSAMPLE, 1),
  P('width', 'Width', 'cab', 0, 2, 1, '%'),
  // ---- outboard
  B('odOn', 'Overdrive', 'fx', 0),
  P('odDrive', 'Drive', 'fx', 0, 1, 0.5, '%'),
  P('odTone', 'Tone', 'fx', 0, 1, 0.5, '%'),
  P('odLevel', 'Level', 'fx', -24, 6, 0, 'dB'),
  B('wahOn', 'Wah', 'fx', 0),
  P('wahPos', 'Position', 'fx', 0, 1, 0.5, '%'),
  C('wahSrc', 'Source', 'fx', WAH_SRCS, 0),
  I('wahCC', 'Wah CC', 'fx', 0, 127, 4, 'count'),
  B('echoOn', 'Tape Echo', 'fx', 0),
  L('echoTime', 'Time', 'fx', 0.03, 1.2, 0.35, 's'),
  P('echoFb', 'Feedback', 'fx', 0, 0.95, 0.35, '%'),
  P('echoMix', 'Mix', 'fx', 0, 1, 0.25, '%'),
  P('echoWow', 'Wow', 'fx', 0, 1, 0.3, '%'),
  P('echoTone', 'Tone', 'fx', 0, 1, 0.5, '%'),
  // ---- reverb
  B('revOn', 'Reverb', 'fx', 0),
  P('revMix', 'Mix', 'fx', 0, 1, 0.2, '%'),
  L('revDecay', 'Decay', 'fx', 0.5, 6, 2.0, 's'),
  P('revTone', 'Tone', 'fx', 0, 1, 0.5, '%'),
];

const PARAM_BY_ID = Object.fromEntries(PARAM_DEFS.map(d => [d.id, d]));

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

function defaultParamValues() {
  return Object.fromEntries(PARAM_DEFS.map(d => [d.id, d.def]));
}

// value <-> 0..1 normalized (same mapping the VST host sees)
function paramToNorm(d, v) {
  if (d.skew === 'log') return clamp(Math.log(v / d.min) / Math.log(d.max / d.min), 0, 1);
  return clamp((v - d.min) / (d.max - d.min), 0, 1);
}

function paramFromNorm(d, n) {
  n = clamp(n, 0, 1);
  let v = d.skew === 'log' ? d.min * Math.pow(d.max / d.min, n) : d.min + n * (d.max - d.min);
  if (d.step) v = Math.round(v / d.step) * d.step;
  return clamp(v, d.min, d.max);
}

function formatParam(d, v) {
  if (d.type === 'choice') return d.choices[clamp(Math.round(v), 0, d.choices.length - 1)];
  if (d.type === 'bool') return v >= 0.5 ? 'On' : 'Off';
  const sgn = (x) => (x > 0 ? '+' : '');
  switch (d.unit) {
    case 's':     return v < 1 ? `${Math.round(v * 1000)} ms` : `${v.toFixed(2)} s`;
    case 'Hz':    return v >= 1000 ? `${(v / 1000).toFixed(v >= 10000 ? 1 : 2)} kHz` : `${Math.round(v)} Hz`;
    case 'lfohz': return `${v.toFixed(v < 1 ? 3 : v < 10 ? 2 : 1)} Hz`;
    case 'st':    return Math.abs(v - Math.round(v)) > 0.05 ? `${sgn(v)}${v.toFixed(1)} st` : `${sgn(v)}${Math.round(v)} st`;
    case 'ct':    return `${sgn(v)}${Math.round(v)} ct`;
    case '%':     return `${Math.round(v * 100)}%`;
    case 'bip':   return `${v > 0.004 ? '+' : ''}${Math.round(v * 100)}%`;
    case 'dB':    return `${v > 0.05 ? '+' : ''}${v.toFixed(1)} dB`;
    case 'deg':   return `${Math.round(v)}°`;
    case 'ms':    return `${Math.round(v)} ms`;
    case 'count': return String(Math.round(v));
    case 'note':  return midiNoteName(Math.round(v));
    case 'bar':   return Math.abs(v - Math.round(v)) < 0.05 ? String(Math.round(v)) : v.toFixed(1);
    case 'oct':   return `${v > 0.005 ? '+' : ''}${v.toFixed(1)} oct`;
    default:      return v.toFixed(2);
  }
}

// The registration string of 9 drawbar values, grouped like the instrument's chart: "88 8000 000"
function registrationString(vals) {
  const d = vals.map(v => String(clamp(Math.round(v), 0, 8))).join('');
  return `${d.slice(0, 2)} ${d.slice(2, 6)} ${d.slice(6)}`;
}
// "888000000", "88 8000 000" or fewer digits (the rest = 0) -> 9 values, or null if not a registration
function parseRegistration(text) {
  const digits = String(text).replace(/[\s\-_.]/g, '');
  if (!/^[0-8]{1,9}$/.test(digits)) return null;
  return Array.from({ length: 9 }, (_, i) => (i < digits.length ? Number(digits[i]) : 0));
}
