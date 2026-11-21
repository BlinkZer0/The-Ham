// ============================================================================
//  JS half of the DSP parity test. Renders tests/parity_scenarios.json with the
//  JS engine (webhost/dsp-worklet.js), through the same SynthProcessor and
//  message path the web build uses, and returns the same statistics as the C++
//  tool (TheHamTests --parity): RMS per channel in 16 windows + peak.
//
//  Loaded by run-parity.ps1 into headless Edge (after params.js, factory.js and
//  dsp-worklet.js). Also runs under Node for quick checks:
//    node tests/parity-runner.js            (prints the JSON)
// ============================================================================
'use strict';

function hamParityRun(scenarios, factoryJson, Proc) {
  const WINDOWS = 16, BLOCK = 128;
  const factory = {};
  for (const p of hamFactoryPresets(factoryJson)) { factory[`${p.category}/${p.name}`] = p; if (!factory[p.name]) factory[p.name] = p; }
  const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

  const out = [];
  for (const sc of scenarios) {
    const values = defaultParamValues();
    let regs = hamFactoryRegistrations();
    if (sc.preset) {
      if (!factory[sc.preset]) throw new Error(`unknown preset ${sc.preset}`);
      Object.assign(values, factory[sc.preset].values);
      regs = factory[sc.preset].registrations;
    }
    for (const [id, v] of Object.entries(sc.params || {})) {
      if (!(id in values)) throw new Error(`unknown param ${id}`);
      values[id] = v;
    }
    const p = new Proc({ processorOptions: { params: values, regs } });
    const send = (m) => p.port.onmessage({ data: m });
    const param = (id, value) => { if (!(id in values)) throw new Error(`unknown param ${id}`); send({ type: 'param', id, value }); };
    const L = new Float32Array(BLOCK), R = new Float32Array(BLOCK);
    const per = Math.max(1, Math.floor(sc.blocks * BLOCK / WINDOWS));
    const sl = new Float64Array(WINDOWS), sr = new Float64Array(WINDOWS);
    let peak = 0;
    const byBlock = new Map();
    for (const ev of sc.events || []) { if (!byBlock.has(ev[0])) byBlock.set(ev[0], []); byBlock.get(ev[0]).push(ev); }
    const t0 = now();
    for (let b = 0; b < sc.blocks; b++) {
      for (const ev of byBlock.get(b) || []) {
        const a = ev.slice(2);
        switch (ev[1]) {
          case 'on': send({ type: 'noteOn', note: a[0], vel: a[1], ch: a[2] || 0 }); break;
          case 'off': send({ type: 'noteOff', note: a[0], ch: a[1] || 0 }); break;
          case 'cc': send({ type: 'cc', cc: a[0], value: a[1], ch: a[2] || 0 }); break;
          case 'manual': send(a[2] > 0 ? { type: 'keyOn', manual: a[0], key: a[1], vel: a[2] } : { type: 'keyOff', manual: a[0], key: a[1] }); break;
          case 'expression': send({ type: 'expression', value: a[0] }); break;
          case 'param': param(a[0], a[1]); break;
          case 'presetKey': param(a[0] === 0 ? 'upKey' : 'loKey', a[1]); break;
          case 'motor': param('motorStart', a[0]); param('motorRun', a[1]); break;
          case 'speed': param('speed', a[0]); break;
          case 'midi': send({ type: 'midi', data: a[0] }); break;
          case 'allOff': send({ type: 'allNotesOff' }); break;
          case 'panic': send({ type: 'panic' }); break;
          default: throw new Error(`unknown event type ${ev[1]}`);
        }
      }
      p.process([], [[L, R]]);
      for (let i = 0; i < BLOCK; i++) {
        const w = Math.min(WINDOWS - 1, Math.floor((b * BLOCK + i) / per));
        sl[w] += L[i] * L[i]; sr[w] += R[i] * R[i];
        peak = Math.max(peak, Math.abs(L[i]), Math.abs(R[i]));
      }
    }
    const ms = now() - t0;
    out.push({ name: sc.name, rmsL: [...sl].map(x => Math.sqrt(x / per)), rmsR: [...sr].map(x => Math.sqrt(x / per)), peak,
               ms, audioMs: sc.blocks * BLOCK / 48 });
  }
  return out;
}

// ---- Node entry point ----------------------------------------------------------------
if (typeof module !== 'undefined' && require.main === module) {
  const fs = require('fs'), path = require('path'), vm = require('vm');
  const vst = path.join(__dirname, '..');
  // main context: a vm sandbox makes every global lookup (Math.sin...) ~10x slower
  globalThis.sampleRate = 48000;
  const run = (file, tail) => vm.runInThisContext(fs.readFileSync(path.join(vst, file), 'utf8') + (tail || ''), { filename: path.join(vst, file) });
  run('ui/js/params.js', ';globalThis.defaultParamValues = defaultParamValues;');
  run('webhost/factory.js', ';globalThis.hamFactoryPresets = hamFactoryPresets; globalThis.hamFactoryRegistrations = hamFactoryRegistrations;');
  run('webhost/dsp-worklet.js', `
    ;globalThis.AudioWorkletProcessor = class { constructor() { this.port = { postMessage() {}, onmessage: null }; } };
    globalThis.registerProcessor = (n, c) => { globalThis.Proc = c; };
    synthWorkletModule();`);
  const scenarios = JSON.parse(fs.readFileSync(path.join(__dirname, 'parity_scenarios.json'), 'utf8'));
  const factory = JSON.parse(fs.readFileSync(path.join(vst, 'presets/factory_presets.json'), 'utf8'));
  const res = hamParityRun(scenarios, factory, globalThis.Proc);
  process.stdout.write(JSON.stringify(res));
}
