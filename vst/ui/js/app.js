// ============================================================================
//  UI wiring, shared by the plugin and the web build.
//   - talks only to JuceEngine (engine-juce.js). In the plugin that is JUCE's
//     native bridge to C++; in the web build vst/webhost/web-host.js provides
//     the same bridge on top of Web Audio + Web MIDI (phase 6).
//   - the host owns all parameter state; automation, preset loads and MIDI
//     notes flow back in as events
//   - the whole panel scales to fit the window
// ============================================================================
(function () {
  'use strict';

  const $ = (sel) => document.querySelector(sel);
  function el(tag, cls, html) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html !== undefined) e.innerHTML = html;
    return e;
  }
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* storage unavailable */ } },
  };

  // ---------------------------------------------------------------- state ---
  const values = defaultParamValues();      // replaced by the plugin's state on connect
  let presetList = [];                      // [{id, name, category, factory}] factory first, then user
  let presetFolder = '';
  let current = { id: '', name: '—', category: '' };
  let presetDirty = false;
  let activeCat = null;

  const engine = new JuceEngine();
  const knobs = {};
  const bound = {};             // id -> [controls with setValue()]
  const dragging = new Set();   // params being dragged in the UI
  const lastLocal = {};         // id -> time of last UI-side change
  const refreshers = [];        // functions run after values change: (changedIds:Set|null) => void

  // ---------------------------------------------------- parameter plumbing ---
  function applyValue(id, v) {
    values[id] = v;
    if (knobs[id]) knobs[id].setValue(v);
    (bound[id] || []).forEach(c => c.setValue(v));
  }

  function refresh(ids) { refreshers.forEach(fn => fn(ids)); }

  // A change made in this UI
  function setParam(id, v) {
    lastLocal[id] = performance.now();
    values[id] = v;
    if (knobs[id]) knobs[id].setValue(v);
    (bound[id] || []).forEach(c => { if (!(c instanceof Drawbar)) c.setValue(v); });
    engine.setParam(id, v);
    if (!PARAM_BY_ID[id].noPreset && !presetDirty) { presetDirty = true; renderPresetName(); }
    refresh(new Set([id]));
  }

  const gesture = {
    begin(id) { dragging.add(id); engine.beginGesture(id); },
    end(id) { dragging.delete(id); engine.endGesture(id); },
  };
  const gestureFn = (id, begin) => (begin ? gesture.begin(id) : gesture.end(id));

  function addKnob(parent, id) {
    const k = new Knob(PARAM_BY_ID[id], values[id], (v) => setParam(id, v), (begin) => gestureFn(id, begin));
    knobs[id] = k;
    k.el.dataset.param = id;
    parent.appendChild(k.el);
    return k;
  }

  // Bind a non-knob control (ToggleButton / Segmented / SelectBox) to a param
  function control(Cls, id, ...extra) {
    const c = new Cls(PARAM_BY_ID[id], values[id], (v) => setParam(id, v), ...extra);
    (bound[id] = bound[id] || []).push(c);
    c.el.dataset.param = id;
    return c;
  }

  // Changes coming from the plugin (automation, presets, state restore)
  engine.on('params', (obj) => {
    const now = performance.now();
    const changed = new Set();
    for (const id in obj) {
      if (!PARAM_BY_ID[id]) continue;
      // don't fight the user's hand: skip params being dragged or changed here in the last 300 ms
      if (dragging.has(id) || (lastLocal[id] !== undefined && now - lastLocal[id] < 300)) continue;
      applyValue(id, obj[id]);
      changed.add(id);
    }
    if (changed.size) refresh(changed);
  });

  // ------------------------------------------------------------ presets ----
  const esc = (s) => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  engine.on('presetList', ({ presets, folder }) => {
    presetList = presets;
    presetFolder = folder;
    if (!activeCat || !categories().includes(activeCat)) activeCat = current.category || (presets[0] && presets[0].category);
    renderBrowser();
  });

  engine.on('preset', (p) => {
    const changed = p.id !== current.id || p.name !== current.name;
    current = p;
    if (changed) presetDirty = false;
    if (p.category && categories().includes(p.category)) activeCat = p.category;
    renderPresetName();
    renderBrowser();
  });

  engine.on('message', ({ text }) => logMidi(text));

  function categories() {
    const cats = [];
    for (const p of presetList) if (!cats.includes(p.category)) cats.push(p.category);
    if (!presetList.some(p => !p.factory) && !cats.includes('User')) cats.push('User');
    return cats;
  }

  function loadPreset(p) {
    presetDirty = false;
    current = { id: p.id, name: p.name, category: p.category };
    for (const id in lastLocal) delete lastLocal[id]; // let all incoming values through
    renderPresetName();
    renderBrowser();
    engine.loadPreset(p.id); // the plugin applies the values and sends them back
  }

  function stepPreset(dir) {
    if (!presetList.length) return;
    const i = presetList.findIndex(p => p.id === current.id);
    const next = i < 0 ? 0 : (i + dir + presetList.length) % presetList.length;
    loadPreset(presetList[next]);
  }

  function renderPresetName() {
    $('#preset-name').textContent = current.name + (presetDirty ? ' *' : '');
    $('#preset-cat').textContent = (current.category || 'Preset').toUpperCase();
  }

  const tabsEl = $('#cat-tabs');
  const gridEl = $('#preset-grid');
  function renderBrowser() {
    const cats = categories();
    tabsEl.innerHTML = '';
    for (const c of cats) {
      const t = el('button', 'cat-tab', esc(c));
      t.type = 'button';
      t.setAttribute('role', 'tab');
      t.setAttribute('aria-selected', c === activeCat);
      t.classList.toggle('active', c === activeCat);
      t.classList.toggle('has-current', c === current.category);
      t.addEventListener('click', () => { activeCat = c; renderBrowser(); });
      tabsEl.appendChild(t);
    }
    gridEl.innerHTML = '';
    const items = presetList.filter(p => p.category === activeCat);
    if (!items.length) {
      gridEl.appendChild(el('div', 'grid-empty',
        `No user presets yet. Click <b>SAVE</b> to store the current sound.<br>
         Preset files in <span class="path">${esc(presetFolder)}</span> show up here automatically.`));
      return;
    }
    for (const p of items) {
      const b = el('button', 'preset-btn', `<span class="pb-name">${esc(p.name)}</span>`);
      b.type = 'button';
      b.title = p.name;
      b.classList.toggle('selected', p.id === current.id);
      b.addEventListener('click', () => loadPreset(p));
      if (!p.factory) {
        const del = el('span', 'pb-del', '×');
        del.title = 'Delete preset';
        del.addEventListener('click', (e) => {
          e.stopPropagation();
          openModal({
            title: 'Delete preset', body: `Move <b>${esc(p.name)}</b> to the Recycle Bin?`,
            ok: 'DELETE', danger: true, onOk: () => engine.deletePreset(p.id),
          });
        });
        b.appendChild(del);
      }
      gridEl.appendChild(b);
    }
  }

  function savePrompt() {
    openModal({
      title: 'Save preset',
      body: `Saved to <span class="path">${esc(presetFolder)}</span>`,
      input: current.category === 'User' || !current.id.startsWith('factory:') ? current.name : `${current.name} (copy)`,
      ok: 'SAVE',
      onOk: (name) => { if (name.trim()) engine.savePreset(name.trim(), false); },
    });
  }
  engine.on('saveResult', ({ ok, exists, name, error }) => {
    if (ok) { presetDirty = false; renderPresetName(); logMidi(`Saved preset "${name}"`); }
    else if (exists) {
      openModal({
        title: 'Replace preset?', body: `A preset named <b>${esc(name)}</b> already exists. Replace it?`,
        ok: 'REPLACE', danger: true, onOk: () => engine.savePreset(name, true),
      });
    } else logMidi(`Save failed: ${error}`);
  });

  $('#preset-prev').addEventListener('click', () => stepPreset(-1));
  $('#preset-next').addEventListener('click', () => stepPreset(1));
  $('#preset-save').addEventListener('click', savePrompt);
  $('#preset-load').addEventListener('click', () => engine.loadPresetFile());
  $('#preset-folder').addEventListener('click', () => engine.openPresetFolder());

  // ------------------------------------------------------------- modal -----
  const modal = $('#modal'), modalInput = $('#modal-input'), modalOk = $('#modal-ok');
  let modalOnOk = null;
  function openModal({ title, body, input, ok, danger, onOk }) {
    $('#modal-title').textContent = title;
    $('#modal-body').innerHTML = body || '';
    modalOk.textContent = ok || 'OK';
    modalOk.classList.toggle('danger', !!danger);
    modalInput.style.display = input === undefined ? 'none' : '';
    modalInput.value = input || '';
    modalOnOk = onOk;
    modal.classList.remove('hidden');
    if (input !== undefined) { modalInput.focus(); modalInput.select(); } else modalOk.focus();
  }
  function closeModal(accept) {
    if (modal.classList.contains('hidden')) return;
    modal.classList.add('hidden');
    const fn = modalOnOk;
    modalOnOk = null;
    if (accept && fn) fn(modalInput.value);
  }
  modalOk.addEventListener('click', () => closeModal(true));
  $('#modal-cancel').addEventListener('click', () => closeModal(false));
  modal.addEventListener('pointerdown', (e) => { if (e.target === modal) closeModal(false); });
  modal.addEventListener('keydown', (e) => {
    e.stopPropagation(); // typing a name must not play notes
    if (e.key === 'Enter') { e.preventDefault(); closeModal(true); }
    else if (e.key === 'Escape') { e.preventDefault(); closeModal(false); }
  });
  modal.addEventListener('keyup', (e) => e.stopPropagation());

  // ------------------------------------------------------------- pages -----
  const PAGE_HINTS = {
    drawbars: 'Upper and lower manuals: drawbar sets A (A# key) and B (B key) · pedals 16\' and 8\'',
    voice: 'Key click is live now · percussion, scanner vibrato, leakage, age and the preamp arrive in phase 2',
    cabinet: 'Rotary cabinets arrive in phase 3 (the settings are stored with presets already)',
    play: 'Organ model, tuning, motor · MIDI channels (upper 1, lower 2, pedals 3) or Split · drawbar CC maps',
    fx: 'Overdrive -> wah -> tape echo before the cabinet, spring reverb after it (phase 3)',
  };
  let activePage = location.hash.slice(1) || store.get('ham.page') || 'drawbars'; // #voice opens a page directly (tests)
  function showPage(name) {
    if (!PAGE_HINTS[name]) name = 'drawbars';
    activePage = name;
    store.set('ham.page', name);
    document.querySelectorAll('.page-tab').forEach(t => t.classList.toggle('active', t.dataset.page === name));
    document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === name));
    $('#page-hint').textContent = PAGE_HINTS[name];
    refresh(null);
  }
  document.querySelectorAll('.page-tab').forEach(t => t.addEventListener('click', () => showPage(t.dataset.page)));

  function makePanel(title, extraHeader, phase) {
    const p = el('div', 'panel');
    const h = el('div', 'panel-head', `<span class="panel-title">${title}</span>`);
    if (phase) h.appendChild(el('span', 'phase-tag', `PHASE ${phase}`));
    if (extraHeader) (Array.isArray(extraHeader) ? extraHeader : [extraHeader]).forEach(x => h.appendChild(x.el || x));
    p.appendChild(h);
    return p;
  }
  function knobRow(parent, ids, cls) {
    const row = el('div', 'knob-row' + (cls ? ' ' + cls : ''));
    ids.forEach(id => addKnob(row, id));
    parent.appendChild(row);
    return row;
  }
  function field(label, ctl) {
    const f = el('div', 'field', `<span class="ctl-label">${label}</span>`);
    f.appendChild(ctl.el);
    return f;
  }
  function fields(parent, list, three) {
    const g = el('div', 'field-grid' + (three ? ' three' : ''));
    list.forEach(([label, ctl]) => g.appendChild(field(label, ctl)));
    parent.appendChild(g);
    return g;
  }
  function onOff(panel, id) {   // panel dims while its switch is off
    refreshers.push((ids) => { if (!ids || ids.has(id)) panel.classList.toggle('off', values[id] < 0.5); });
  }

  // ====================================================== DRAWBARS page ====
  const sets = [[], []];   // [manual][set] DrawbarSet
  {
    const stage = $('#db-stage');
    const bindBars = (set) => set.bars.forEach((b, i) => { (bound[set.ids[i]] = bound[set.ids[i]] || []).push(b); });
    const manualBox = (m, title) => {
      const box = el('div', 'db-manual', `<div class="db-manual-title">${title}</div>`);
      const row = el('div', 'db-manual-sets');
      for (let s = 0; s < 2; s++) {
        const ids = FOOTAGES.map((_, f) => drawbarId(m, s, f));
        const set = new DrawbarSet(s === 0 ? 'A# · SET A' : 'B · SET B', ids, values, (id, v) => setParam(id, v), gestureFn);
        bindBars(set);
        sets[m][s] = set;
        row.appendChild(set.el);
      }
      box.appendChild(row);
      return box;
    };
    stage.appendChild(manualBox(0, 'UPPER MANUAL'));
    const pedalBox = el('div', 'db-manual db-pedal', '<div class="db-manual-title">PEDAL</div>');
    const pedal = new DrawbarSet('BASS', ['pd16', 'pd8'], values, (id, v) => setParam(id, v), gestureFn, { small: true, feet: [0, 2] });
    bindBars(pedal);
    pedal.setActive(true);
    pedalBox.appendChild(pedal.el);
    stage.appendChild(pedalBox);
    stage.appendChild(manualBox(1, 'LOWER MANUAL'));

    const keyName = (k) => ['C (cancel)', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A'][k] || '';
    refreshers.push((ids) => {
      for (let m = 0; m < 2; m++) {
        const keyId = m ? 'loKey' : 'upKey';
        if (ids && !ids.has(keyId) && ![...ids].some(id => id.startsWith(m ? 'lo' : 'up'))) continue;
        const k = Math.round(values[keyId]);
        sets[m][0].setActive(k === 10);
        sets[m][1].setActive(k === 11);
        sets[m].forEach(s => s.renderString());
        sets[m][0].el.title = sets[m][1].el.title = k < 10 ? `Preset key ${keyName(k)} is down: these drawbars are not sounding` : '';
      }
      pedal.renderString();
    });
  }

  // ========================================================= VOICE page ====
  {
    const r1 = $('#voice-row-1'), r2 = $('#voice-row-2');
    {
      const p = makePanel('PERCUSSION', control(ToggleButton, 'percOn'), 2);
      const sw = el('div', 'switch-row');
      sw.appendChild(control(Segmented, 'percVol').el);
      sw.appendChild(control(Segmented, 'percDecay').el);
      sw.appendChild(control(Segmented, 'percHarm', ['2nd', '3rd']).el);
      p.appendChild(sw);
      knobRow(p, ['percFast', 'percSlow']);
      p.appendChild(el('div', 'panel-note', 'Upper manual, drawbar set B only · single trigger · cancels the 1\' bar'));
      onOff(p, 'percOn');
      r1.appendChild(p);
    }
    {
      const p = makePanel('VIBRATO · CHORUS', null, 2);
      const sw = el('div', 'switch-row left');
      sw.appendChild(control(ToggleButton, 'vibUpper', 'UPPER').el);
      sw.appendChild(control(ToggleButton, 'vibLower', 'LOWER').el);
      p.appendChild(sw);
      p.appendChild(control(Segmented, 'vibMode').el);
      p.appendChild(el('div', 'panel-note', 'Scanner on a multi-tap LC line (about 6.9 Hz) · V = scanned, C = scanned + dry'));
      r1.appendChild(p);
    }
    {
      const p = makePanel('KEY CLICK', control(ToggleButton, 'keyUpClick', 'KEY-UP'));
      knobRow(p, ['clickUp', 'clickLo', 'clickPd']);
      p.appendChild(el('div', 'panel-note', 'The 9 contacts of a key close at different moments and bounce · 0% = smooth, no click'));
      r1.appendChild(p);
    }
    {
      const p = makePanel('PEDALS · SUSTAIN', null, 2);
      knobRow(p, ['pedTone', 'pedDecay', 'loSusTime']);
      const sw = el('div', 'switch-row left');
      sw.appendChild(control(ToggleButton, 'pedSustain', 'PEDAL SUS').el);
      sw.appendChild(control(ToggleButton, 'loSustain', 'LOWER SUS').el);
      sw.appendChild(control(ToggleButton, 'manualBass', 'MANUAL BASS').el);
      p.appendChild(sw);
      r2.appendChild(p);
    }
    {
      const p = makePanel('LEAKAGE · AGE · PREAMP', null, 2);
      knobRow(p, ['leakUp', 'leakLo', 'ageUp', 'ageLo', 'drive']);
      r2.appendChild(p);
    }
    refreshers.push(() => {
      $('#voice-led').classList.toggle('on', values.percOn >= 0.5 || values.vibUpper >= 0.5 || values.vibLower >= 0.5);
    });
  }

  // ======================================================= CABINET page ====
  {
    const r1 = $('#cab-row-1'), r2 = $('#cab-row-2');
    {
      const p = makePanel('CABINET', control(ToggleButton, 'cabBypass', 'BYPASS'), 3);
      fields(p, [['Model', control(SelectBox, 'cabModel')], ['Rotors', control(SelectBox, 'rotorConfig')]]);
      p.appendChild(control(Segmented, 'speed').el);
      p.appendChild(control(Segmented, 'speedMode').el);
      r1.appendChild(p);
    }
    {
      const p = makePanel('HORN', null, 3);
      knobRow(p, ['hornSlow', 'hornFast', 'hornAccel', 'hornBrake', 'brakeAngle']);
      fields(p, [['Speed', control(SelectBox, 'hornSpeedSel')], ['Direction', control(Segmented, 'hornDir')],
                 ['Horns', control(SelectBox, 'hornType')], ['Mouth', control(Segmented, 'hornMouth')]]);
      const sw = el('div', 'switch-row left');
      sw.appendChild(control(ToggleButton, 'brakePos', 'STOP AT ANGLE').el);
      p.appendChild(sw);
      r1.appendChild(p);
    }
    {
      const p = makePanel('DRUM', control(ToggleButton, 'drumOff', 'DRUM OFF'), 3);
      knobRow(p, ['drumSlow', 'drumFast', 'drumAccel', 'drumBrake']);
      fields(p, [['Speed', control(SelectBox, 'drumSpeedSel')], ['Direction', control(Segmented, 'drumDir')],
                 ['Baffle', control(Segmented, 'drumBaffle')]]);
      r1.appendChild(p);
    }
    {
      const p = makePanel('SPEAKERS', null, 3);
      knobRow(p, ['xover', 'hornDrumBal']);
      fields(p, [['Back', control(Segmented, 'cabBack')], ['Amp OS', control(Segmented, 'ampOS')]]);
      r2.appendChild(p);
    }
    {
      const p = makePanel('MICS', null, 3);
      p.appendChild(control(Segmented, 'micLayout').el);
      knobRow(p, ['micDist', 'micAngle', 'micHorn', 'micDrum', 'micRoom', 'width']);
      r2.appendChild(p);
    }
    {
      const p = makePanel('SPEED CONTROL', null, 3);
      fields(p, [['Mod Wheel', control(Segmented, 'speedWheel', ['Off', 'Half', 'Mom.'])], ['Variable', control(SelectBox, 'varSource')]]);
      knobRow(p, ['speedCC']);
      const sw = el('div', 'switch-row left');
      sw.appendChild(control(ToggleButton, 'speedSustain', 'SUSTAIN PEDAL').el);
      p.appendChild(sw);
      r2.appendChild(p);
    }
    refreshers.push(() => $('#cabinet-led').classList.toggle('on', values.cabBypass < 0.5));
  }

  // ========================================================= PLAY page =====
  {
    const r1 = $('#play-row-1'), r2 = $('#play-row-2');
    {
      const p = makePanel('ORGAN');
      p.appendChild(control(Segmented, 'organModel').el);
      knobRow(p, ['tuneA', 'transpose', 'ampLevel']);
      const sw = el('div', 'switch-row left');
      sw.appendChild(el('span', 'ctl-label', 'MOTOR'));
      sw.appendChild(control(ToggleButton, 'motorStart', 'START').el);
      sw.appendChild(control(ToggleButton, 'motorRun', 'RUN').el);
      sw.appendChild(el('span', 'ctl-label', 'MAINS'));
      sw.appendChild(control(Segmented, 'mains').el);
      p.appendChild(sw);
      r1.appendChild(p);
    }
    {
      const p = makePanel('VELOCITY');
      p.appendChild(control(Segmented, 'velMode').el);
      knobRow(p, ['velAmount']);
      p.appendChild(el('div', 'panel-note', 'Organ keys are switches: velocity is off by default. Volume or Click use it.'));
      r1.appendChild(p);
    }
    {
      const p = makePanel('MIDI CHANNELS');
      knobRow(p, ['chUpper', 'chLower', 'chPedal', 'pedMidiBase']);
      p.appendChild(el('div', 'panel-note', 'Lowest manual C = MIDI 36 · Pedal Base = the MIDI note of the lowest pedal'));
      r1.appendChild(p);
    }
    {
      const p = makePanel('CONTROLLERS');
      fields(p, [['Drawbar CC map', control(SelectBox, 'ccMap')]]);
      knobRow(p, ['exprCC', 'motorCC']);
      const sw = el('div', 'switch-row left');
      sw.appendChild(control(ToggleButton, 'ksOn', 'PRESET KEYSWITCHES').el);
      p.appendChild(sw);
      p.appendChild(el('div', 'panel-note', 'Drawbar CCs move the active set of each manual. Motor Off CC: value ≥ 64 = power off (pitch dive).'));
      r2.appendChild(p);
    }
    {
      const p = makePanel('SPLIT (ONE KEYBOARD)', control(ToggleButton, 'splitMode'));
      knobRow(p, ['splitPoint', 'splitLoOct', 'pedalSplit']);
      const sw = el('div', 'switch-row left');
      sw.appendChild(control(ToggleButton, 'pedalZone', 'PEDAL ZONE').el);
      p.appendChild(sw);
      p.appendChild(el('div', 'panel-note', 'On the upper channel: lower manual below the split point, upper above; pedals below the pedal split.'));
      onOff(p, 'splitMode');
      r2.appendChild(p);
    }
    refreshers.push(() => $('#play-led').classList.toggle('on', values.splitMode >= 0.5 || values.manualBass >= 0.5));
  }

  // =========================================================== FX page =====
  {
    const r1 = $('#fx-row-1');
    const fx = (title, onId, ids, extra) => {
      const p = makePanel(title, control(ToggleButton, onId), 3);
      knobRow(p, ids);
      if (extra) extra(p);
      onOff(p, onId);
      r1.appendChild(p);
    };
    fx('OVERDRIVE', 'odOn', ['odDrive', 'odTone', 'odLevel']);
    fx('WAH', 'wahOn', ['wahPos', 'wahCC'], (p) => fields(p, [['Source', control(SelectBox, 'wahSrc')]]));
    fx('TAPE ECHO', 'echoOn', ['echoTime', 'echoFb', 'echoMix', 'echoWow', 'echoTone']);
    fx('SPRING REVERB', 'revOn', ['revMix', 'revDecay', 'revTone']);
    refreshers.push(() => $('#fx-led').classList.toggle('on', values.odOn >= 0.5 || values.wahOn >= 0.5 || values.echoOn >= 0.5 || values.revOn >= 0.5));
  }

  // ---------------------------------------------------------- notes / UI ---
  const monitor = $('#midi-monitor');
  const midiLed = $('#midi-led');
  let ledTimer = 0;
  function logMidi(text) { monitor.textContent = text; }
  function flashLed() {
    midiLed.classList.add('on');
    clearTimeout(ledTimer);
    ledTimer = setTimeout(() => midiLed.classList.remove('on'), 90);
  }

  const MANUAL_NAMES = ['UPPER', 'LOWER', 'PEDAL'];
  const keyLabel = (m, k) => (m === 2 ? `pedal ${NOTE_NAMES[k % 12]}${Math.floor(k / 12) + 1}` : `${NOTE_NAMES[k % 12]}${Math.floor(k / 12) + 2}`);
  const manuals = [
    new Manual($('#kb-upper'), { numKeys: MANUAL_KEYS, onKeyOn: (k, v) => keyOn(0, k, v), onKeyOff: (k) => keyOff(0, k) }),
    new Manual($('#kb-lower'), { numKeys: MANUAL_KEYS, onKeyOn: (k, v) => keyOn(1, k, v), onKeyOff: (k) => keyOff(1, k) }),
    new Manual($('#kb-pedal'), { numKeys: PEDAL_KEYS, pedal: true, onKeyOn: (k, v) => keyOn(2, k, v), onKeyOff: (k) => keyOff(2, k) }),
  ];
  function keyOn(m, k, vel) { engine.noteOn(m, k, vel); manuals[m].setKey(k, true); }
  function keyOff(m, k) { engine.noteOff(m, k); manuals[m].setKey(k, false); }

  // reverse-coloured preset keys left of each manual
  const presetKeys = [
    new PresetKeys($('#pk-upper'), (i) => setParam('upKey', i)),
    new PresetKeys($('#pk-lower'), (i) => setParam('loKey', i)),
  ];
  presetKeys[0].svg.dataset.param = 'upKey';
  presetKeys[1].svg.dataset.param = 'loKey';
  refreshers.push((ids) => {
    if (!ids || ids.has('upKey')) presetKeys[0].setActive(values.upKey);
    if (!ids || ids.has('loKey')) presetKeys[1].setActive(values.loKey);
  });

  engine.on('notes', ({ on, off }) => {
    off.forEach(([m, k]) => manuals[m] && manuals[m].setKey(k, false));
    on.forEach(([m, k]) => manuals[m] && manuals[m].setKey(k, true));
    flashLed();
    if (on.length) logMidi(`KEY ON   ${on.map(([m, k]) => `${MANUAL_NAMES[m]} ${keyLabel(m, k)}`).join('  ')}`);
    else if (off.length) logMidi(`KEY OFF  ${off.map(([m, k]) => `${MANUAL_NAMES[m]} ${keyLabel(m, k)}`).join('  ')}`);
  });

  // Computer keyboard plays the upper manual: A = the C at kbBase
  let kbBase = 24;
  function setOctave(base) {
    base = clamp(base, 0, 36);
    for (const [k, key] of heldKeys) { keyOff(0, key); heldKeys.delete(k); }
    kbBase = base;
    $('#octave-readout').textContent = `C${base / 12 + 2}–F${base / 12 + 3}`;
    store.set('ham.octave', String(base));
  }
  $('#oct-down').addEventListener('click', () => setOctave(kbBase - 12));
  $('#oct-up').addEventListener('click', () => setOctave(kbBase + 12));
  const KEYMAP = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11,
                   k: 12, o: 13, l: 14, p: 15, ';': 16, "'": 17 };
  const heldKeys = new Map();
  window.addEventListener('keydown', (e) => {
    if (e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
    if (/^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName) || !modal.classList.contains('hidden')) return;
    if (e.target.closest && e.target.closest('.drawbar')) return;   // 0-8 / arrows belong to the drawbar
    const key = e.key.toLowerCase();
    if (key === 'z') { setOctave(kbBase - 12); return; }
    if (key === 'x') { setOctave(kbBase + 12); return; }
    if (key in KEYMAP && !heldKeys.has(key)) {
      const k = kbBase + KEYMAP[key];
      heldKeys.set(key, k);
      keyOn(0, k, 100);
    }
  });
  window.addEventListener('keyup', (e) => {
    const key = e.key.toLowerCase();
    if (heldKeys.has(key)) { keyOff(0, heldKeys.get(key)); heldKeys.delete(key); }
  });
  window.addEventListener('blur', () => {
    for (const [, k] of heldKeys) keyOff(0, k);
    heldKeys.clear();
  });

  $('#panic-btn').addEventListener('click', () => {
    engine.panic();
    manuals.forEach(m => m.clear());
    heldKeys.clear();
    logMidi('PANIC: all keys released');
  });

  // ------------------------------------------------------------- stats -----
  const sustainLed = $('#sustain-led'), exprMini = $('#expr-mini'), motorLed = $('#motor-led');
  engine.on('stats', (s) => {
    sustainLed.classList.toggle('on', !!s.sustain);
    if (typeof s.expression === 'number') exprMini.style.width = `${clamp(s.expression, 0, 1) * 100}%`;
    if (typeof s.motor === 'number') {
      motorLed.classList.toggle('on', s.motor > 0.995);
      motorLed.style.opacity = s.motor > 0.995 ? '' : String(0.35 + 0.65 * s.motor);
      motorLed.parentElement.title = `Tonewheel motor: ${Math.round(s.motor * 100)}% speed`;
    }
  });

  let connectedOnce = false;
  engine.on('config', ({ sampleRate, blockSize, wrapper, audio, running, startPage }) => {
    if (startPage && PAGE_HINTS[startPage] && !connectedOnce) showPage(startPage);
    const web = wrapper === 'Web';
    document.body.classList.toggle('web', web);
    const sr = sampleRate > 0 ? `${(sampleRate / 1000).toFixed(1)} kHz · ${blockSize} smp` : 'not playing yet';
    $('#audio-info').textContent = audio || `${wrapper} · ${sr}`;
    $('#audio-led').classList.toggle('on', running !== false);
    if (!connectedOnce) logMidi('Connected: play your MIDI keyboards (upper ch 1, lower ch 2, pedals ch 3), the keys, or A–\' on the computer keyboard');
    connectedOnce = true;
    fit();
  });

  // ---- MIDI input (web build) ------------------------------------------------
  const midiSelect = $('#midi-select');
  engine.on('midiInputs', ({ inputs, selected, error }) => {
    midiSelect.innerHTML = '';
    if (error) { midiSelect.appendChild(new Option('MIDI unavailable', 'all')); midiSelect.disabled = true; return; }
    midiSelect.disabled = false;
    midiSelect.appendChild(new Option(inputs.length ? `All inputs (${inputs.length})` : 'No MIDI devices', 'all'));
    inputs.forEach(d => midiSelect.appendChild(new Option(d.name + (d.state === 'connected' ? '' : ' (off)'), d.id)));
    midiSelect.value = Array.from(midiSelect.options).some(o => o.value === selected) ? selected : 'all';
  });
  midiSelect.addEventListener('change', () => engine.selectMidi(midiSelect.value));

  // --------------------------------------------------------- fit window ----
  const synth = $('.synth');
  function fit() {
    const w = synth.offsetWidth + 24, h = synth.offsetHeight + 24;
    const s = Math.min(window.innerWidth / w, window.innerHeight / h);
    synth.style.transform = `translateX(-50%) scale(${s})`;
  }
  window.addEventListener('resize', fit);

  // Right-click on any control = the host's parameter menu (automation, link to controller...).
  // Elsewhere no browser menu; Shift+right-click still opens it (Inspect).
  document.addEventListener('contextmenu', (e) => {
    if (!engine.connected || e.shiftKey) return;
    e.preventDefault();
    const ctl = e.target.closest('[data-param]');
    if (ctl) engine.paramMenu(ctl.dataset.param, e.clientX, e.clientY);
  });

  // -------------------------------------------------------------- init -----
  renderPresetName();
  renderBrowser();
  showPage(activePage);
  setOctave(Number(store.get('ham.octave')) || 24);
  refresh(null);
  fit();
  if (engine.connected) engine.uiReady();
  else logMidi('Not running inside the plugin: no audio engine connected (UI preview only)');
})();
