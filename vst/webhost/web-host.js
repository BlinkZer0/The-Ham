// ============================================================================
//  Web host: stands in for the plugin's C++ side when the UI runs in a
//  browser. It provides window.__JUCE__.backend with exactly the events the
//  JUCE editor uses (see PluginEditor.cpp), so vst/ui runs unchanged:
//    audio    Web Audio: AudioWorklet (or ScriptProcessor fallback on file://)
//             running dsp-worklet.js, the JS twin of SynthEngine.h
//    MIDI     Web MIDI (all inputs or one). Channel messages take the plugin's
//             path: drawbar CC maps, MIDI learn and the preset keyswitches act on
//             parameters here (as in PluginProcessor); everything else goes to the
//             engine with routeMidi's rules
//    presets  factory (FACTORY_JSON via factory.js), this browser (localStorage),
//             or a linked folder on disk (File System Access API), same
//             .hampreset format as the plugin, rescanned every 2 s
//    state    the fixed preset-key registrations, the MIDI-learn map (kept in this
//             browser), the stats and the rotor angles for the cabinet drawing
//  Needs params.js, factory.js, dsp-worklet.js and FACTORY_JSON loaded first.
// ============================================================================
(function () {
  'use strict';

  // ---------------------------------------------------------- the bridge ---
  const listeners = {};
  const handlers = {};
  const toUi = (ev, data) => queueMicrotask(() => (listeners[ev] || []).forEach(f => f(data)));
  window.__JUCE__ = {
    backend: {
      addEventListener(ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); },
      // handled synchronously so file pickers / permission prompts keep the click's user activation
      emitEvent(ev, data) { const h = handlers[ev]; if (h) h(data || {}); },
    },
  };
  const message = (text) => toUi('message', { text });
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); return true; } catch (e) { return false; } },
  };
  const round = Math.round;

  // ------------------------------------------------------ factory presets ---
  const FACTORY = hamFactoryPresets(FACTORY_JSON).map((p, i) =>
    ({ id: `factory:${i}`, name: p.name, category: p.category, factory: true, values: p.values, registrations: p.registrations }));

  // A cabinet model picked in the UI loads the model's defaults as well (PluginProcessor::applyCabinetModel):
  // the same 14 parameters, with the values of dsp/Cabinets.h. The factory templates cab22 / cab45 / cabSingle /
  // cabTone carry the same numbers (the C++ tests check them against Cabinets.h; the UI tests check this table
  // against the templates).
  const CABINET_DEFAULTS = [
    { rotorConfig: 0, hornSlow: 0.8, hornFast: 6.7, hornAccel: 1.0, hornBrake: 1.5, drumSlow: 0.7, drumFast: 5.8, drumAccel: 4.0, drumBrake: 5.0, drumBaffle: 0, xover: 800, cabBack: 0, micLayout: 1 },
    { rotorConfig: 0, hornSlow: 0.85, hornFast: 6.8, hornAccel: 0.9, hornBrake: 1.4, drumSlow: 0.75, drumFast: 6.0, drumAccel: 3.5, drumBrake: 4.5, drumBaffle: 0, xover: 800, cabBack: 0, micLayout: 1 },
    { rotorConfig: 2, hornSlow: 0.9, hornFast: 6.0, hornAccel: 1.2, hornBrake: 2.0, drumSlow: 0.9, drumFast: 6.0, drumAccel: 1.2, drumBrake: 2.0, drumBaffle: 2, xover: 800, cabBack: 0, micLayout: 2 },
    { rotorConfig: 3, hornSlow: 0.8, hornFast: 6.7, hornAccel: 1.0, hornBrake: 1.5, drumSlow: 0.7, drumFast: 5.8, drumAccel: 4.0, drumBrake: 5.0, drumBaffle: 0, xover: 800, cabBack: 0, micLayout: 2 },
  ];

  // ---------------------------------------------------------------- state ---
  const values = defaultParamValues();
  let regs = hamFactoryRegistrations();      // [manual][key 0..8][footage]
  let current = { id: '', name: '', category: '' };

  // Every preset parameter: the preset's value, else the default (like PluginProcessor::applyPreset).
  // Master is never part of a preset. Stepped parameters snap like the plugin's int / choice params.
  function presetValues(pv) {
    const out = {};
    for (const d of PARAM_DEFS) {
      if (d.noPreset) continue;
      let v = Number(pv[d.id]);
      if (pv[d.id] === undefined || pv[d.id] === null || isNaN(v)) v = d.def;
      v = clamp(v, d.min, d.max);
      if (d.step) v = round(v / d.step) * d.step;
      out[d.id] = v;
    }
    return out;
  }
  const copyRegs = (r) => r.map(m => m.map(k => k.slice()));
  function sendRegistrations() {
    audio.post({ type: 'registrations', regs });
    toUi('registrations', hamRegistrationsToJson(regs));
  }
  function applyPreset(pv, pregs, info) {
    const all = presetValues(pv);
    Object.assign(values, all);
    regs = pregs ? copyRegs(pregs) : hamFactoryRegistrations();
    audio.post({ type: 'params', values: all });
    current = info;
    toUi('params', Object.assign({}, values));
    sendRegistrations();
    toUi('preset', current);
  }

  // a fresh instance starts on the first factory preset, like the plugin
  Object.assign(values, presetValues(FACTORY[0].values));
  regs = copyRegs(FACTORY[0].registrations);
  current = { id: FACTORY[0].id, name: FACTORY[0].name, category: FACTORY[0].category };

  // ---------------------------------------------------------------- audio ---
  const audio = {
    ctx: null, node: null, port: null, mode: '', error: '',
    post(m) { if (this.port) this.port.postMessage(m); },
    get running() { return !!this.ctx && this.ctx.state === 'running'; },

    async start() {
      const ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
      this.ctx = ctx;
      ctx.onstatechange = sendConfig;
      const src = `(${synthWorkletModule.toString()})();`;
      let built = null;
      if (ctx.audioWorklet && location.protocol !== 'file:') {
        try {
          const url = URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
          try { await ctx.audioWorklet.addModule(url); } finally { URL.revokeObjectURL(url); }
          const node = new AudioWorkletNode(ctx, 'the-ham', {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
            processorOptions: { params: values, regs },
          });
          node.port.onmessage = (e) => onDspMessage(e.data);
          // an exception inside the audio thread silences the node: say so instead of failing quietly
          node.onprocessorerror = (e) => {
            audio.error = 'the audio engine stopped (processor error); reload the page';
            console.error('AudioWorklet processor error', e);
            message(`Audio engine error: ${(e && e.message) || 'processor error'}. Reload the page.`);
            sendConfig();
          };
          built = { node, port: node.port, mode: 'AudioWorklet' };
        } catch (e) { console.warn('AudioWorklet unavailable, using ScriptProcessor', e); }
      }
      if (!built) built = this.scriptProcessor(ctx, src);
      this.node = built.node; this.port = built.port; this.mode = built.mode;
      this.node.connect(ctx.destination);
      this.post({ type: 'params', values: Object.assign({}, values) }); // anything changed while starting
      this.post({ type: 'registrations', regs });
      sendConfig();
    },

    // Chrome/Edge block worklet modules on file://: run the same DSP on the main thread
    scriptProcessor(ctx, src) {
      class AudioWorkletProcessorShim { constructor() { this.port = { postMessage: onDspMessage, onmessage: null }; } }
      let Processor = null;
      new Function('AudioWorkletProcessor', 'registerProcessor', 'sampleRate', src)(
        AudioWorkletProcessorShim, (name, cls) => { Processor = cls; }, ctx.sampleRate);
      const proc = new Processor({ processorOptions: { params: values, regs } });
      const L128 = new Float32Array(128), R128 = new Float32Array(128);
      const node = ctx.createScriptProcessor(1024, 0, 2);
      node.onaudioprocess = (e) => {
        const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1);
        for (let off = 0; off < L.length; off += 128) {
          proc.process([], [[L128, R128]]);
          L.set(L128, off); R.set(R128, off);
        }
      };
      return { node, port: { postMessage: (m) => proc.port.onmessage({ data: m }) }, mode: 'ScriptProcessor' };
    },
  };

  // ------------------------------------------- DSP -> UI: stats, rotor, notes ---
  // The plugin editor polls the processor at 30 Hz; here the worklet reports at that rate.
  const lastKeys = new Uint32Array(6);
  const lastRotor = [-1, -1, -1, -1];
  let statsCount = 0;
  function onDspMessage(s) {
    if (s.type !== 'stats') return;
    ++statsCount;
    toUi('stats', { peak: s.peak, sustain: s.sustain, modWheel: s.modWheel, expression: s.expression, motor: s.motor, perc: s.perc, speed: s.speed });
    if (s.hornTurns !== lastRotor[0] || s.drumTurns !== lastRotor[1] || s.hornHz !== lastRotor[2] || s.drumHz !== lastRotor[3]) {
      lastRotor[0] = s.hornTurns; lastRotor[1] = s.drumTurns; lastRotor[2] = s.hornHz; lastRotor[3] = s.drumHz;
      toUi('rotor', { hornAngle: s.hornTurns, drumAngle: s.drumTurns, hornHz: s.hornHz, drumHz: s.drumHz });
    }
    // keys held on each manual (two 32-bit masks per manual) -> notes { on: [[manual, key]...], off: [...] }
    if (s.keys) {
      const on = [], off = [];
      for (let m = 0; m < 3; ++m)
        for (let half = 0; half < 2; ++half) {
          const bits = s.keys[2 * m + half] >>> 0, prev = lastKeys[2 * m + half];
          const flipped = (bits ^ prev) >>> 0;
          if (!flipped) continue;
          for (let k = 0; k < 32; ++k)
            if ((flipped >>> k) & 1) ((bits >>> k) & 1 ? on : off).push([m, k + 32 * half]);
          lastKeys[2 * m + half] = bits;
        }
      if (on.length || off.length) toUi('notes', { on, off });
    }
  }

  function sendConfig() {
    const c = audio.ctx;
    let text;
    if (audio.error) text = `Audio error: ${audio.error}`;
    else if (!audio.running) text = 'Click or tap anywhere to enable audio';
    else text = `Web · ${audio.mode} · ${(c.sampleRate / 1000).toFixed(1)} kHz · ~${(((c.baseLatency || 0) + (c.outputLatency || 0)) * 1000).toFixed(1)} ms`;
    toUi('config', { wrapper: 'Web', sampleRate: c ? c.sampleRate : 0, blockSize: 128, audio: text, running: audio.running,
                     startPage: new URLSearchParams(location.search).get('page') || '' });
  }

  const started = audio.start().catch((e) => { audio.error = e.message || String(e); console.error(e); sendConfig(); });
  function unlockAudio() {
    if (audio.running || audio.error) return;
    started.then(() => audio.ctx && audio.ctx.resume()).then(sendConfig).catch(() => {});
  }
  document.addEventListener('pointerdown', unlockAudio, true);
  document.addEventListener('keydown', unlockAudio, true);

  // ------------------------------------------------------- parameters ---
  // A parameter changed by the host side (MIDI learn, drawbar CC maps, keyswitches, a cabinet pick): the
  // plugin's editor echoes such changes to the UI, so this does too. UI-made changes (setParam) are not echoed.
  function snap(d, v) {
    v = clamp(Number(v), d.min, d.max);
    if (d.step) v = round(v / d.step) * d.step;
    return v;
  }
  function setParamFromHost(id, v) {
    const d = PARAM_BY_ID[id];
    if (!d) return;
    v = snap(d, v);
    values[id] = v;
    audio.post({ type: 'param', id, value: v });
    toUi('params', { [id]: v });
  }

  // ----------------------------------------------------------- MIDI learn ---
  // One CC per parameter; learnable CCs as in CcMap.h (not bank select, sustain or channel-mode messages).
  // The plugin keeps the map in the project state; here it is kept in this browser.
  const learn = { armed: '', map: {} };   // map: cc -> parameter id
  const learnableCC = (cc) => cc !== 0 && cc !== 32 && cc !== 64 && cc < 120;
  function learnedMap() {   // parameter id -> cc, as the `learn` event carries it
    const out = {};
    for (const cc of Object.keys(learn.map)) out[learn.map[cc]] = Number(cc);
    return out;
  }
  function sendLearn() { toUi('learn', { armed: learn.armed, map: learnedMap() }); }
  function saveLearn() { store.set('ham.midiLearn', Object.keys(learn.map).map(cc => `${cc}=${learn.map[cc]}`).join(';')); }
  (function restoreLearn() {
    for (const item of (store.get('ham.midiLearn') || '').split(';')) {
      const [cc, id] = item.split('=');
      if (PARAM_BY_ID[id] && Number(cc) >= 0 && Number(cc) < 128) learn.map[Number(cc)] = id;
    }
  })();
  function forgetLearn(id) {
    for (const cc of Object.keys(learn.map)) if (learn.map[cc] === id) delete learn.map[cc];
    if (learn.armed === id) learn.armed = '';
    saveLearn();
    sendLearn();
  }

  // ---------------------------------------------------- drawbar CC maps, keyswitches (CcMap.h) ---
  function drawbarForCC(map, cc) {
    const inRange = (lo) => cc >= lo && cc < lo + 9;
    switch (map) {
      case 1: if (inRange(12)) return { manual: 0, foot: cc - 12 };
              if (inRange(21)) return { manual: 1, foot: cc - 21 };
              break;
      case 2: if (inRange(16)) return { manual: 0, foot: cc - 16 };
              if (inRange(70)) return { manual: 1, foot: cc - 70 };
              break;
      case 3: if (inRange(70)) return { manual: 0, foot: cc - 70 };
              break;
      default: break;
    }
    return { manual: -1, foot: -1 };
  }
  function presetKeyswitch(note, ch, ksOn, splitMode, chUpper, chLower) {
    if (!ksOn || splitMode || note < 24 || note > 35) return { manual: -1, key: -1 };
    if (ch === chUpper) return { manual: 0, key: note - 24 };
    if (ch === chLower) return { manual: 1, key: note - 24 };
    return { manual: -1, key: -1 };
  }

  // A channel message, as PluginProcessor::processBlock treats it: MIDI learn and the keyswitches first,
  // then the drawbar CC maps, then the engine (routeMidi, in the worklet).
  function handleMidi(d) {
    const cmd = d[0] & 0xf0, ch = d[0] & 15;
    if (cmd === 0xb0) {
      const cc = d[1], val = d[2];
      if (learnableCC(cc)) {
        if (learn.armed) {
          for (const k of Object.keys(learn.map)) if (learn.map[k] === learn.armed) delete learn.map[k];
          learn.map[cc] = learn.armed;
          learn.armed = '';
          saveLearn();
          sendLearn();
        }
        const target = learn.map[cc];
        if (target) { setParamFromHost(target, paramFromNorm(PARAM_BY_ID[target], val / 127)); return; }
      }
      const t = drawbarForCC(round(values.ccMap), cc);
      if (t.manual >= 0) {
        const key = round(values[t.manual === 0 ? 'upKey' : 'loKey']);
        setParamFromHost(drawbarId(t.manual, key === 10 ? 0 : 1, t.foot), val * 8 / 127);
        return;
      }
    } else if (cmd === 0x90 || cmd === 0x80) {
      const ks = presetKeyswitch(d[1], ch, values.ksOn >= 0.5, values.splitMode >= 0.5, round(values.chUpper) - 1, round(values.chLower) - 1);
      if (ks.manual >= 0) {
        if (cmd === 0x90 && d[2] > 0) setParamFromHost(ks.manual === 0 ? 'upKey' : 'loKey', ks.key);
        return;   // keyswitches never sound
      }
    }
    sendMidi(d);
  }
  function sendMidi(data) {
    if (!audio.running && (data[0] & 0xf0) === 0x90 && data[2] > 0) message('Audio is off: click anywhere on the page once to enable it');
    audio.post({ type: 'midi', data });
  }

  // ----------------------------------------------------------------- MIDI ---
  const midi = { access: null, selected: store.get('ham.midiInput') || 'all' };
  function midiInputs() {
    return midi.access ? Array.from(midi.access.inputs.values()).map(i => ({ id: i.id, name: i.name, state: i.state })) : [];
  }
  function bindMidi() {
    if (!midi.access) return;
    for (const input of midi.access.inputs.values()) {
      const on = midi.selected === 'all' || midi.selected === input.id;
      input.onmidimessage = on ? (e) => onMidi(e.data) : null;
    }
  }
  function onMidi(d) {
    const st = d[0];
    if (st >= 0xf0 || st < 0x80) return;   // no SysEx / clock / running status
    const cmd = st & 0xf0;
    handleMidi(Array.from(d.slice(0, cmd === 0xc0 || cmd === 0xd0 ? 2 : 3)));
  }
  if (typeof navigator.requestMIDIAccess === 'function') {
    navigator.requestMIDIAccess({ sysex: false }).then((access) => {
      midi.access = access;
      access.onstatechange = () => { bindMidi(); toUi('midiInputs', { inputs: midiInputs(), selected: midi.selected }); };
      bindMidi();
      toUi('midiInputs', { inputs: midiInputs(), selected: midi.selected });
      const n = midiInputs().length;
      message(n ? `MIDI ready: ${n} input(s): ${midiInputs().map(i => i.name).join(', ')}` : 'MIDI ready: no devices found (plug one in, it appears automatically)');
    }).catch((e) => {
      toUi('midiInputs', { inputs: [], selected: 'all', error: true });
      message(`MIDI unavailable: ${e.message || e}`);
    });
  } else {
    queueMicrotask(() => toUi('midiInputs', { inputs: [], selected: 'all', error: true }));
  }

  // -------------------------------------------------------------- presets ---
  const LS_KEY = 'ham.userPresets.v1';
  const local = {
    read() { try { return JSON.parse(store.get(LS_KEY)) || {}; } catch (e) { return {}; } },
    write(obj) { return store.set(LS_KEY, JSON.stringify(obj)); },
  };
  const localPresets = () => Object.keys(local.read()).sort((a, b) => a.localeCompare(b, undefined, { numeric: true }))
    .map(name => ({ id: `local:${name}`, name, category: 'User', factory: false }));

  const fsSupported = typeof window.showDirectoryPicker === 'function';
  const folder = { handle: null, granted: false, files: [], sig: '' };
  const idb = {
    open() {
      return new Promise((res, rej) => {
        const r = indexedDB.open('the-ham', 1);
        r.onupgradeneeded = () => r.result.createObjectStore('kv');
        r.onsuccess = () => res(r.result);
        r.onerror = () => rej(r.error);
      });
    },
    async get(key) {
      const db = await this.open();
      return new Promise((res) => { const r = db.transaction('kv').objectStore('kv').get(key); r.onsuccess = () => res(r.result); r.onerror = () => res(undefined); });
    },
    async set(key, val) {
      const db = await this.open();
      return new Promise((res) => { const tx = db.transaction('kv', 'readwrite'); tx.objectStore('kv').put(val, key); tx.oncomplete = () => res(true); tx.onerror = () => res(false); });
    },
  };

  async function scanFolder() {
    if (!folder.handle || !folder.granted) return false;
    const files = [];
    async function walk(dir, path) {
      for await (const [name, h] of dir.entries()) {
        const rel = path ? `${path}/${name}` : name;
        if (h.kind === 'directory') await walk(h, rel);
        else if (name.toLowerCase().endsWith(HAM_PRESET_EXT)) {
          const f = await h.getFile();
          files.push({ id: `user:${rel}`, name: name.slice(0, -HAM_PRESET_EXT.length), category: path || 'User',
                       factory: false, handle: h, parent: dir, fileName: name, stamp: `${f.size}:${f.lastModified}` });
        }
      }
    }
    try { await walk(folder.handle, ''); } catch (e) { folder.granted = false; return true; }
    files.sort((a, b) => a.category.localeCompare(b.category, undefined, { numeric: true }) || a.name.localeCompare(b.name, undefined, { numeric: true }));
    const sig = files.map(f => `${f.id}|${f.stamp}`).join(';');
    if (sig === folder.sig) return false;
    folder.sig = sig;
    folder.files = files;
    return true;
  }

  function allPresets() { return [...FACTORY, ...folder.files, ...localPresets()]; }
  function sendPresetList() {
    toUi('presetList', {
      presets: allPresets().map(({ id, name, category, factory }) => ({ id, name, category, factory })),
      folder: folder.granted ? `${folder.handle.name} (linked folder)` : 'this browser: click FOLDER to link a folder on disk',
    });
  }

  async function linkFolder(handle, ask) {
    folder.handle = handle;
    folder.sig = '';
    let state = await handle.queryPermission({ mode: 'readwrite' });
    if (state !== 'granted' && ask) state = await handle.requestPermission({ mode: 'readwrite' });
    folder.granted = state === 'granted';
    await idb.set('presetFolder', handle);
    if (folder.granted) await scanFolder();
    sendPresetList();
    return folder.granted;
  }
  if (fsSupported && window.indexedDB) {
    idb.get('presetFolder').then(async (h) => {
      if (h && !(await linkFolder(h, false))) message(`Click FOLDER to reconnect the preset folder "${h.name}"`);
    }).catch(() => {});
  }
  setInterval(async () => { if (await scanFolder()) sendPresetList(); }, 2000); // autoload, like the plugin

  // Same rules as PresetManager::readJson: migrate, then at least one known parameter.
  function parsePresetText(text, fallbackName) {
    let json;
    try { json = JSON.parse(text); } catch (e) { throw new Error(`Not a valid preset file: ${fallbackName}`); }
    if (!json || typeof json !== 'object') throw new Error(`Not a The Ham preset: ${fallbackName}`);
    hamMigratePreset(json);
    const params = {};
    if (hamApplyParamObject(json.params, params) === 0) throw new Error(`No The Ham settings found in ${fallbackName}`);
    return { name: String(json.name || '').trim() || fallbackName, category: String(json.category || ''), params,
             registrations: hamReadRegistrations(json.registrations) };
  }
  // The file the plugin writes (PresetManager::toJson): 6 significant digits, drawbars as whole steps.
  function presetText(name, category) {
    const params = {};
    for (const d of PARAM_DEFS)
      if (!d.noPreset) params[d.id] = d.unit === 'bar' ? round(Number(values[d.id])) : +Number(values[d.id]).toPrecision(6);
    return JSON.stringify({ format: HAM_PRESET_FORMAT, version: HAM_PRESET_VERSION, name, category, params,
                            registrations: hamRegistrationsToJson(regs) }, null, 2);
  }
  const legalName = (s) => s.trim().replace(/[\\/:*?"<>|]/g, '_').slice(0, 60);

  async function loadPresetById(id) {
    const p = allPresets().find(x => x.id === id);
    if (!p) { message('Preset not found (was it moved or deleted?)'); toUi('preset', current); return; }
    try {
      const info = { id, name: p.name, category: p.category };
      if (p.factory) applyPreset(p.values, p.registrations, info);
      else {
        const text = id.startsWith('local:') ? local.read()[p.name] : await (await p.handle.getFile()).text();
        const d = parsePresetText(text, p.name);
        applyPreset(d.params, d.registrations, info);
      }
    } catch (e) { message(e.message || String(e)); toUi('preset', current); }
  }

  function loadPresetText(text, fileName) {
    const p = parsePresetText(text, fileName.replace(/\.[^.]+$/, ''));
    applyPreset(p.params, p.registrations, { id: `file:${fileName}`, name: p.name, category: 'File' });
    message(`Loaded ${fileName}`);
  }

  async function savePreset(rawName, overwrite) {
    const name = legalName(rawName);
    const result = (r) => toUi('saveResult', Object.assign({ name, ok: false, exists: false, error: '' }, r));
    if (!name) return result({ error: 'Enter a name for the preset' });
    try {
      if (folder.granted) {
        const fileName = name + HAM_PRESET_EXT;
        if (!overwrite) {
          let exists = true;
          try { await folder.handle.getFileHandle(fileName); } catch (e) { exists = false; }
          if (exists) return result({ exists: true });
        }
        const w = await (await folder.handle.getFileHandle(fileName, { create: true })).createWritable();
        await w.write(presetText(name, 'User'));
        await w.close();
        await scanFolder();
        current = { id: `user:${fileName}`, name, category: 'User' };
      } else {
        const all = local.read();
        if (!overwrite && all[name]) return result({ exists: true });
        all[name] = presetText(name, 'User');
        if (!local.write(all)) throw new Error('browser storage is unavailable (private window?)');
        current = { id: `local:${name}`, name, category: 'User' };
      }
      sendPresetList();
      toUi('preset', current);
      result({ ok: true });
    } catch (e) { result({ error: e.message || String(e) }); }
  }

  async function deletePreset(id) {
    const p = allPresets().find(x => x.id === id);
    try {
      if (!p || p.factory) throw new Error('Only user presets can be deleted');
      if (id.startsWith('local:')) { const all = local.read(); delete all[p.name]; local.write(all); }
      else { await p.parent.removeEntry(p.fileName); await scanFolder(); }
      if (current.id === id) current = { id: '', name: current.name, category: 'User' };
      message(`Deleted preset "${p.name}"`);
    } catch (e) { message(e.message || String(e)); }
    sendPresetList();
    toUi('preset', current);
  }

  const fileInput = document.createElement('input');
  fileInput.type = 'file';
  fileInput.accept = `${HAM_PRESET_EXT},application/json`;
  fileInput.style.display = 'none';
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => document.body.appendChild(fileInput));
  else document.body.appendChild(fileInput);   // dev.html loads this after the document is ready
  fileInput.addEventListener('change', async () => {
    const f = fileInput.files && fileInput.files[0];
    fileInput.value = '';
    if (!f) return;
    try { loadPresetText(await f.text(), f.name); }
    catch (e) { message(e.message || String(e)); }
  });

  async function folderButton() {
    if (!fsSupported) { message('Linking a preset folder needs Chrome or Edge: presets are saved in this browser instead.'); return; }
    try {
      if (folder.handle && !folder.granted) { if (await linkFolder(folder.handle, true)) message(`Reconnected "${folder.handle.name}"`); return; }
      const handle = await window.showDirectoryPicker({ id: 'ham-presets', mode: 'readwrite', startIn: 'documents' });
      await linkFolder(handle, true);
      message(`Preset folder linked: ${handle.name}, ${folder.files.length} preset(s). Link Documents\\The Ham\\Presets to share presets with the VST.`);
    } catch (e) { if (e && e.name !== 'AbortError') message(`Folder: ${e.message || e}`); }
  }

  // ------------------------------------------------- UI -> host events ---
  Object.assign(handlers, {
    uiReady() {
      sendConfig();
      sendPresetList();
      toUi('params', Object.assign({}, values));
      toUi('preset', current);
      toUi('registrations', hamRegistrationsToJson(regs));
      sendLearn();
      toUi('stats', { peak: 0, sustain: false, modWheel: 0, expression: 1, motor: 1, perc: 0, speed: round(values.speed) });
      if (midi.access) toUi('midiInputs', { inputs: midiInputs(), selected: midi.selected });
    },
    paramMenu() {}, // no host automation menu in a browser
    setParam({ id, value }) {
      const d = PARAM_BY_ID[id];
      if (!d) return;
      const v = snap(d, value);
      values[id] = v;
      audio.post({ type: 'param', id, value: v });
    },
    gesture() {},
    // the on-screen keyboards: manual 0 upper / 1 lower / 2 pedals, key index on that manual
    noteOn({ manual, key, vel }) {
      if (!audio.running) message('Audio is off: click anywhere on the page once to enable it');
      audio.post({ type: 'keyOn', manual: clamp(manual | 0, 0, 2), key: clamp(key | 0, 0, 60), vel: clamp(vel | 0, 1, 127) });
    },
    noteOff({ manual, key }) { audio.post({ type: 'keyOff', manual: clamp(manual | 0, 0, 2), key: clamp(key | 0, 0, 60) }); },
    panic() { audio.post({ type: 'panic' }); },
    expression({ value }) { audio.post({ type: 'expression', value: clamp(Number(value) || 0, 0, 1) }); },
    // fixed preset-key registrations: { manual: 0 / 1, key: 0..8 (C#..A), bars: "00 5320 000" }
    setRegistration({ manual, key, bars }) {
      const b = parseRegistration(bars);
      manual |= 0; key |= 0;
      if (!b || manual < 0 || manual > 1 || key < 0 || key > 8) return;
      regs[manual][key] = b;
      sendRegistrations();
    },
    midiLearn({ id }) { if (PARAM_BY_ID[id]) { learn.armed = id; sendLearn(); } },
    midiForget({ id }) { forgetLearn(id); },
    learnCancel() { learn.armed = ''; sendLearn(); },
    // a cabinet picked in the UI loads its defaults (speeds, ramps, rotors, baffle, crossover, mics)
    cabinetModel({ model }) {
      model = clamp(model | 0, 0, 3);
      const changed = Object.assign({ cabModel: model }, CABINET_DEFAULTS[model]);
      for (const id of Object.keys(changed)) { changed[id] = snap(PARAM_BY_ID[id], changed[id]); values[id] = changed[id]; }
      audio.post({ type: 'params', values: changed });
      toUi('params', changed);
    },
    selectMidi({ id }) { midi.selected = id; store.set('ham.midiInput', id); bindMidi(); },
    loadPreset({ id }) { loadPresetById(id); },
    savePreset({ name, overwrite }) { savePreset(String(name || ''), overwrite); },
    deletePreset({ id }) { deletePreset(id); },
    loadPresetFile() { fileInput.click(); },
    openPresetFolder() { folderButton(); },
  });

  // Test hook (tests/run-ui-tests.ps1): read-only view of the host state, a tap on what goes to the DSP,
  // a MIDI injection point and the preset text import.
  window.__hamWebHost = {
    values, factory: FACTORY, cabinetDefaults: CABINET_DEFAULTS,
    get regs() { return regs; }, get current() { return current; }, get audioMode() { return audio.mode; },
    get statsCount() { return statsCount; }, get learn() { return { armed: learn.armed, map: learnedMap() }; },
    sent: [], tap: false,
    midi(data) { handleMidi(Array.from(data)); },
    loadPresetText(text, fileName) { loadPresetText(text, fileName); },
    presetText(name) { return presetText(name, 'User'); },
    suspend() {
      audio.post({ type: 'panic' });
      if (audio.ctx && audio.ctx.state === 'running') audio.ctx.suspend().then(sendConfig).catch(() => {});
    },
    resume() {
      if (audio.ctx && audio.ctx.state === 'suspended') audio.ctx.resume().then(sendConfig).catch(() => {});
    },
  };
  const post = audio.post.bind(audio);
  audio.post = (m) => { if (window.__hamWebHost.tap) window.__hamWebHost.sent.push(m); post(m); };
})();
