// JS engine speed check (Node): renders a few cases with the web build's engine and prints CPU
// use as % of real time (one core). The last case is the C++ --bench worst case.
//   node tests/js-bench.js [--quick]
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const vst = path.join(__dirname, '..');
// Runs in Node's main context (a vm sandbox context makes every global lookup such as Math.sin
// go through an interceptor: about 10x slower than a browser's AudioWorklet).
globalThis.sampleRate = 48000;
const ctx = globalThis;
const run = (file, tail) => vm.runInThisContext(fs.readFileSync(path.join(vst, file), 'utf8') + (tail || ''), { filename: path.join(vst, file) });
run('ui/js/params.js', ';globalThis.defaultParamValues = defaultParamValues;');
run('webhost/factory.js', ';globalThis.hamFactoryPresets = hamFactoryPresets;');
run('webhost/dsp-worklet.js', `
  ;globalThis.AudioWorkletProcessor = class { constructor() { this.port = { postMessage() {}, onmessage: null }; } };
  globalThis.registerProcessor = (n, c) => { globalThis.Proc = c; };
  synthWorkletModule();`);
const factory = ctx.hamFactoryPresets(JSON.parse(fs.readFileSync(path.join(vst, 'presets/factory_presets.json'), 'utf8')));
const preset = (name) => factory.find(p => p.name === name).values;

// keys: [[manual, key], ...]
function bench(label, values, keys, seconds) {
  const v = Object.assign(ctx.defaultParamValues(), values);
  const p = new ctx.Proc({ processorOptions: { params: v } });
  const L = new Float32Array(128), R = new Float32Array(128);
  for (const [m, k] of keys) p.port.onmessage({ data: { type: 'keyOn', manual: m, key: k, vel: 100 } });
  const blocks = Math.round(seconds * 48000 / 128);
  for (let b = 0; b < 40; b++) p.process([], [[L, R]]);          // warm-up (JIT)
  const t0 = performance.now();
  for (let b = 0; b < blocks; b++) p.process([], [[L, R]]);
  const ms = performance.now() - t0;
  console.log(`${label.padEnd(60)} ${(100 * ms / (blocks * 128 / 48)).toFixed(1).padStart(6)}%`);
}

const trio = [[0, 24], [0, 28], [0, 31], [1, 12], [1, 19], [2, 0]];
bench('Trio Classic, one upper key', preset('Trio Classic'), [[0, 24]], 3);
bench('Trio Classic, upper + lower chords + pedal (6 keys)', preset('Trio Classic'), trio, 3);
bench('Full Organ (Fast), 6 keys', preset('Full Organ'), trio, 3);
bench('Grind Comp (drive, overdrive, Rotor 45), 6 keys', preset('Grind Comp'), trio, 3);
bench('Psychedelic Swirl (tape echo, C3 both), 6 keys', preset('Psychedelic Swirl'), trio, 3);
bench('Bach Plenum (Tone Cabinet, reverb), 6 keys', preset('Bach Plenum'), trio, 3);
if (!process.argv.includes('--quick')) {
  // the C++ --bench worst case: every key held, all drawbars, percussion, chorus on both manuals,
  // leakage + Age 100%, sustain, Fast rotors, 4x amp, dual horns, 3 mics + room, every pedal, reverb
  const all = [];
  for (let k = 0; k < 61; k++) all.push([0, k], [1, k]);
  for (let k = 0; k < 25; k++) all.push([2, k]);
  const bars = {};
  for (const m of ['up', 'lo']) for (const s of ['A', 'B']) for (let f = 1; f <= 9; f++) bars[`${m}${s}${f}`] = 8;
  bench('worst case: 61 + 61 + 25 keys, everything on, 4x', Object.assign(bars, {
    pd16: 8, pd8: 8, percOn: 1, vibUpper: 1, vibLower: 1, vibMode: 5, leakUp: 1, leakLo: 1, ageUp: 1, ageLo: 1,
    pedSustain: 1, loSustain: 1, clickUp: 1, clickLo: 1, speed: 2, ampOS: 2, drive: 0.7, hornType: 1, micLayout: 3, micRoom: 1,
    odOn: 1, wahOn: 1, echoOn: 1, revOn: 1 }), all, 3);
}
