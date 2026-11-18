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
  const synthEl = document.querySelector('.synth');
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
    if (!replaying && values[id] !== v && !PARAM_BY_ID[id].noPreset) remember(id);
    lastLocal[id] = performance.now();
    values[id] = v;
    if (knobs[id]) knobs[id].setValue(v);
    // a drawbar being dragged draws itself; programmatic changes (undo, A/B, random) animate it
    (bound[id] || []).forEach(c => { if (replaying || !(c instanceof Drawbar)) c.setValue(v); });
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
    remember('preset:' + p.id);
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
    voice: 'Percussion: set B, single trigger · scanner V1-V3 / C1-C3 per manual · leakage and Age per manual · Drive into the cabinet amp',
    cabinet: 'Picking a cabinet loads its speeds, rotors and mics · every option stays editable · mod wheel above the middle = Fast',
    play: 'Organ model, tuning, motor · MIDI channels (upper 1, lower 2, pedals 3) or Split · drawbar CC maps',
    fx: 'Overdrive -> wah -> tape echo before the cabinet (the classic rock chain), spring reverb after it',
  };
  let cabView = null;   // the animated cabinet (CABINET page)
  let activePage = location.hash.slice(1) || store.get('ham.page') || 'drawbars'; // #voice opens a page directly (tests)
  function showPage(name) {
    if (!PAGE_HINTS[name]) name = 'drawbars';
    activePage = name;
    store.set('ham.page', name);
    document.querySelectorAll('.page-tab').forEach(t => t.classList.toggle('active', t.dataset.page === name));
    document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === name));
    $('#page-hint').textContent = PAGE_HINTS[name];
    if (cabView) cabView.setVisible(name === 'cabinet');
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

    // Fixed registrations of the preset keys C#..A: editable, saved with the project and in presets
    const regBox = el('section', 'reg-editor');
    const regInputs = [[], []];
    const KEY_NAMES = ['C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A'];
    ['UPPER', 'LOWER'].forEach((title, m) => {
      const row = el('div', 'reg-row', `<span class="reg-row-title">${title} PRESET KEYS</span>`);
      KEY_NAMES.forEach((name, k) => {
        const cell = el('label', 'reg-cell', `<span class="reg-key">${name}</span>`);
        const input = el('input', 'reg-input small-reg');
        input.type = 'text'; input.spellcheck = false; input.maxLength = 13;
        input.title = `${title.toLowerCase()} manual, preset key ${name}: type nine digits 0-8 and press Enter`;
        const commit = () => {
          const regs = parseRegistration(input.value);
          if (!regs) { input.classList.add('bad'); return; }
          input.classList.remove('bad');
          input.value = registrationString(regs);
          engine.setRegistration(m, k, input.value);
          input.blur();
        };
        input.addEventListener('keydown', (e) => {
          e.stopPropagation();
          if (e.key === 'Enter') { e.preventDefault(); commit(); }
          else if (e.key === 'Escape') { e.preventDefault(); input.value = input.dataset.v || ''; input.blur(); }
        });
        input.addEventListener('keyup', (e) => e.stopPropagation());
        input.addEventListener('change', commit);
        input.addEventListener('focus', () => input.select());
        cell.appendChild(input);
        row.appendChild(cell);
        regInputs[m].push({ cell, input });
      });
      regBox.appendChild(row);
    });
    $('#db-stage').after(regBox);
    engine.on('registrations', ({ upper, lower }) => {
      [upper, lower].forEach((list, m) => list.forEach((text, k) => {
        const { input } = regInputs[m][k];
        input.dataset.v = text;
        if (document.activeElement !== input) { input.value = text; input.classList.remove('bad'); }
      }));
    });
    refreshers.push((ids) => {
      if (ids && !ids.has('upKey') && !ids.has('loKey')) return;
      [values.upKey, values.loKey].forEach((pk, m) =>
        regInputs[m].forEach(({ cell }, k) => cell.classList.toggle('active', Math.round(pk) === k + 1)));
    });
  }

  // ========================================================= VOICE page ====
  let percLamp = null;
  {
    const r1 = $('#voice-row-1'), r2 = $('#voice-row-2');
    {
      const lamp = el('span', 'led perc-lamp');
      lamp.title = 'Percussion envelope';
      const p = makePanel('PERCUSSION', [lamp]);
      percLamp = lamp;
      const sw = el('div', 'rocker-row');
      sw.appendChild(control(Rocker, 'percOn', 'PERC', ['OFF', 'ON'], 'cream').el);
      sw.appendChild(control(Rocker, 'percVol', 'VOLUME', ['NORMAL', 'SOFT'], 'cream').el);
      sw.appendChild(control(Rocker, 'percDecay', 'DECAY', ['FAST', 'SLOW'], 'cream').el);
      sw.appendChild(control(Rocker, 'percHarm', 'HARMONIC', ['SECOND', 'THIRD'], 'cream').el);
      p.appendChild(sw);
      knobRow(p, ['percFast', 'percSlow']);
      p.appendChild(el('div', 'panel-note', 'Upper manual, drawbar set B only · single trigger · cancels the 1\' bar'));
      onOff(p, 'percOn');
      r1.appendChild(p);
    }
    {
      const p = makePanel('VIBRATO · CHORUS');
      const sw = el('div', 'switch-row left');
      sw.className = 'rocker-row';
      sw.appendChild(control(Rocker, 'vibUpper', 'VIB SWELL', ['OFF', 'ON'], 'cream').el);
      sw.appendChild(control(Rocker, 'vibLower', 'VIB GREAT', ['OFF', 'ON'], 'cream').el);
      p.appendChild(sw);
      p.appendChild(control(Segmented, 'vibMode').el);
      p.appendChild(el('div', 'panel-note', 'A scanner reads an 18-section delay line at 6.9 Hz · V = scanned, C = scanned + straight · percussion and pedals bypass it'));
      r1.appendChild(p);
    }
    {
      const p = makePanel('KEY CLICK', control(ToggleButton, 'keyUpClick', 'KEY-UP'));
      knobRow(p, ['clickUp', 'clickLo', 'clickPd']);
      p.appendChild(el('div', 'panel-note', 'The 9 contacts of a key close at different moments and bounce · 0% = smooth, no click'));
      r1.appendChild(p);
    }
    {
      const p = makePanel('PEDALS · SUSTAIN');
      knobRow(p, ['pedTone', 'pedDecay', 'loSusTime']);
      const sw = el('div', 'switch-row left');
      sw.appendChild(control(ToggleButton, 'pedSustain', 'PEDAL SUS').el);
      sw.appendChild(control(ToggleButton, 'loSustain', 'LOWER SUS').el);
      sw.appendChild(control(ToggleButton, 'manualBass', 'MANUAL BASS').el);
      p.appendChild(sw);
      r2.appendChild(p);
    }
    {
      const p = makePanel('LEAKAGE · AGE · DRIVE');
      knobRow(p, ['leakUp', 'leakLo', 'ageUp', 'ageLo', 'drive']);
      p.appendChild(el('div', 'panel-note', 'Leakage: neighbouring wheels bleed in (and a quiet floor of all of them). Age: uneven levels, more harmonics, more leakage. <b>Drive</b>: into the cabinet\'s tube amp.'));
      r2.appendChild(p);
    }
    refreshers.push(() => {
      $('#voice-led').classList.toggle('on', values.percOn >= 0.5 || values.vibUpper >= 0.5 || values.vibLower >= 0.5);
    });
  }

  // ======================================================= CABINET page ====
  {
    const r1 = $('#cab-row-1'), r2 = $('#cab-row-2');
    cabView = new CabinetView($('#cab-view'));
    window.__cabView = cabView;   // the UI tests read its frame counter
    const cabOpts = ['rotorConfig', 'hornType', 'cabBack', 'micLayout', 'micAngle', 'micDist', 'hornDir', 'drumDir', 'drumBaffle'];
    refreshers.push((ids) => {
      if (!ids || ids.has('cabModel')) cabView.setModel(values.cabModel);
      if (!ids || cabOpts.some(id => ids.has(id)))
        cabView.setOptions(Object.fromEntries(cabOpts.map(id => [id, Math.round(values[id] * (id === 'micDist' ? 100 : 1)) / (id === 'micDist' ? 100 : 1)])));
      if (!ids || ids.has('drive')) cabView.setDrive(values.drive);
    });
    {
      const p = makePanel('CABINET', control(ToggleButton, 'cabBypass', 'BYPASS'));
      // picking a model asks the host to load its defaults too
      const model = new SelectBox(PARAM_BY_ID.cabModel, values.cabModel, (v) => {
        lastLocal.cabModel = performance.now(); applyValue('cabModel', v); engine.cabinetModel(v);
        presetDirty = true; renderPresetName(); refresh(new Set(['cabModel']));
      });
      model.el.dataset.param = 'cabModel';
      (bound.cabModel = bound.cabModel || []).push(model);
      fields(p, [['Model', model], ['Rotors', control(SelectBox, 'rotorConfig')]]);
      p.appendChild(control(Segmented, 'speed').el);
      p.appendChild(control(Segmented, 'speedMode').el);
      const lamps = el('div', 'speed-lamps', ['STOP', 'SLOW', 'FAST'].map((t, i) => `<span class="speed-lamp" data-s="${i}"><span class="led"></span>${t}</span>`).join('')
        + '<span class="rpm" id="rpm-readout">—</span>');
      lamps.title = 'The speed the rotors are heading for (from the switch, the mod wheel, a CC or the sustain pedal)';
      p.appendChild(lamps);
      r1.appendChild(p);
    }
    {
      const p = makePanel('HORN');
      knobRow(p, ['hornSlow', 'hornFast', 'hornAccel', 'hornBrake', 'brakeAngle']);
      fields(p, [['Speed', control(SelectBox, 'hornSpeedSel')], ['Direction', control(Segmented, 'hornDir')],
                 ['Horns', control(SelectBox, 'hornType')], ['Mouth', control(Segmented, 'hornMouth')]]);
      const sw = el('div', 'switch-row left');
      sw.appendChild(control(ToggleButton, 'brakePos', 'STOP AT ANGLE').el);
      p.appendChild(sw);
      r1.appendChild(p);
    }
    {
      const p = makePanel('DRUM', control(ToggleButton, 'drumOff', 'DRUM OFF'));
      knobRow(p, ['drumSlow', 'drumFast', 'drumAccel', 'drumBrake']);
      fields(p, [['Speed', control(SelectBox, 'drumSpeedSel')], ['Direction', control(Segmented, 'drumDir')],
                 ['Baffle', control(Segmented, 'drumBaffle')]]);
      r1.appendChild(p);
    }
    {
      const p = makePanel('SPEAKERS');
      knobRow(p, ['xover', 'hornDrumBal']);
      fields(p, [['Back', control(Segmented, 'cabBack')], ['Amp OS', control(Segmented, 'ampOS')]]);
      r2.appendChild(p);
    }
    {
      const p = makePanel('MICS');
      p.appendChild(control(Segmented, 'micLayout').el);
      knobRow(p, ['micDist', 'micAngle', 'micHorn', 'micDrum', 'micRoom', 'width']);
      r2.appendChild(p);
    }
    {
      const p = makePanel('SPEED CONTROL');
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
      sw.appendChild(control(Rocker, 'motorStart', 'START', ['OFF', 'ON'], 'dark').el);
      sw.appendChild(control(Rocker, 'motorRun', 'RUN', ['OFF', 'ON'], 'dark').el);
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
      const p = makePanel(title, control(ToggleButton, onId));
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

  // ------------------------------------------------- undo / redo, A/B --------
  // Snapshots of every preset parameter, taken before a change made in this UI (a drag, a
  // wheel run or repeated clicks on one control within 0.8 s count as one step) and before
  // a preset load. Host automation is not recorded. Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z).
  var replaying = false;
  const hist = { undo: [], redo: [], lastId: null, lastTime: 0 };
  function snapshot() {
    const o = {};
    PARAM_DEFS.forEach(d => { if (!d.noPreset) o[d.id] = values[d.id]; });
    return o;
  }
  function remember(id) {
    const now = performance.now();
    if (id && id === hist.lastId && now - hist.lastTime < 800) { hist.lastTime = now; return; }
    hist.undo.push(snapshot());
    if (hist.undo.length > 100) hist.undo.shift();
    hist.redo.length = 0;
    hist.lastId = id; hist.lastTime = now;
    renderHistory();
  }
  function restore(snap) {
    replaying = true;
    try { for (const id in snap) if (values[id] !== snap[id]) setParam(id, snap[id]); }
    finally { replaying = false; }
    hist.lastId = null;
    refresh(null);
  }
  function undo() {
    if (!hist.undo.length) return;
    hist.redo.push(snapshot());
    restore(hist.undo.pop());
    renderHistory();
    logMidi('Undo');
  }
  function redo() {
    if (!hist.redo.length) return;
    hist.undo.push(snapshot());
    restore(hist.redo.pop());
    renderHistory();
    logMidi('Redo');
  }
  function renderHistory() {
    $('#undo-btn').disabled = !hist.undo.length;
    $('#redo-btn').disabled = !hist.redo.length;
  }
  $('#undo-btn').addEventListener('click', undo);
  $('#redo-btn').addEventListener('click', redo);
  window.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey || e.metaKey) || /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName)) return;
    const k = e.key.toLowerCase();
    if (k === 'z' && !e.shiftKey) { e.preventDefault(); undo(); }
    else if (k === 'y' || (k === 'z' && e.shiftKey)) { e.preventDefault(); redo(); }
  });

  // A/B compare: two complete settings; switching stores the current one in its slot.
  const ab = { active: 'A', A: null, B: null };
  function abSwitch(slot) {
    if (slot === ab.active) return;
    ab[ab.active] = snapshot();
    if (!ab[slot]) ab[slot] = snapshot();
    remember('ab');
    restore(ab[slot]);
    ab.active = slot;
    renderAb();
    logMidi(`Comparing: ${slot}`);
  }
  function renderAb() {
    $('#ab-a').classList.toggle('on', ab.active === 'A');
    $('#ab-b').classList.toggle('on', ab.active === 'B');
    $('#ab-copy').textContent = ab.active === 'A' ? 'A→B' : 'B→A';
  }
  $('#ab-a').addEventListener('click', () => abSwitch('A'));
  $('#ab-b').addEventListener('click', () => abSwitch('B'));
  $('#ab-copy').addEventListener('click', () => {
    const other = ab.active === 'A' ? 'B' : 'A';
    ab[other] = snapshot();
    logMidi(`Copied ${ab.active} to ${other}`);
  });

  // ----------------------------------------------------- randomize drawbars ---
  // A random registration inside a family (The Ham's rules: [min, max] per footage,
  // 16' .. 1'), on the active set of the chosen manual (set B when a fixed preset key is down).
  const FAMILIES = {
    FLUTE:    [[0, 3], [0, 2], [6, 8], [0, 6], [0, 3], [0, 4], [0, 0], [0, 2], [0, 3]],
    DIAPASON: [[2, 6], [0, 3], [6, 8], [5, 8], [2, 5], [3, 6], [0, 3], [0, 3], [0, 3]],
    REED:     [[0, 4], [0, 2], [5, 8], [3, 7], [5, 8], [3, 6], [4, 8], [2, 6], [0, 4]],
    STRING:   [[0, 2], [0, 0], [3, 6], [4, 7], [5, 8], [5, 8], [4, 7], [4, 7], [3, 6]],
    JAZZ:     [[8, 8], [6, 8], [8, 8], [0, 8], [0, 4], [0, 4], [0, 2], [0, 3], [0, 8]],
  };
  let randomManual = 0;
  {
    const tools = $('#db-tools');
    tools.appendChild(el('span', 'ctl-label', 'RANDOM'));
    const which = new Segmented({ name: 'Randomize manual', def: 0 }, 0, (i) => { randomManual = i; which.setValue(i); }, ['UPPER', 'LOWER']);
    tools.appendChild(which.el);
    Object.keys(FAMILIES).forEach(fam => {
      const b = el('button', 'pd-btn small', fam);
      b.type = 'button';
      b.title = `A random ${fam.toLowerCase()} registration on the ${'active set'}`;
      b.addEventListener('click', () => {
        const m = randomManual, key = Math.round(values[m ? 'loKey' : 'upKey']);
        const set = key === 10 ? 0 : 1;
        remember('random');
        replaying = true;
        try {
          FAMILIES[fam].forEach(([lo, hi], f) => setParam(drawbarId(m, set, f), lo + Math.floor(Math.random() * (hi - lo + 1))));
        } finally { replaying = false; }
        sets[m].forEach(x => x.renderString());
        logMidi(`Random ${fam.toLowerCase()}: ${registrationString(FOOTAGES.map((_, f) => values[drawbarId(m, set, f)]))}`);
      });
      tools.appendChild(b);
    });
  }

  // ---------------------------------------------------- organ model badge ---
  // C-3: the same instrument in a churchier case (closed sides) - it changes only the look.
  const CONSOLES = [
    { name: 'B-3', svg: '<rect x="3" y="4" width="30" height="12" rx="1"/><rect x="5" y="16" width="26" height="3"/><rect x="4" y="19" width="2" height="11"/><rect x="30" y="19" width="2" height="11"/><rect x="10" y="27" width="16" height="2"/>' },
    { name: 'C-3', svg: '<path d="M3 30 V7 Q18 0 33 7 V30 Z"/><rect class="cb-hole" x="8" y="11" width="20" height="4"/><rect class="cb-hole" x="11" y="20" width="14" height="7" rx="1"/>' },
    { name: 'A-100', svg: '<rect x="3" y="4" width="30" height="26" rx="1"/><rect class="cb-hole" x="7" y="8" width="22" height="4"/><circle class="cb-hole" cx="12" cy="22" r="4"/><circle class="cb-hole" cx="24" cy="22" r="4"/>' },
    { name: 'M-3', svg: '<rect x="6" y="10" width="24" height="9" rx="1"/><rect x="6" y="19" width="24" height="2"/><rect x="7" y="21" width="2" height="9"/><rect x="27" y="21" width="2" height="9"/>' },
  ];
  refreshers.push((ids) => {
    if (ids && !ids.has('organModel')) return;
    const m = clamp(Math.round(values.organModel), 0, 3);
    $('#console-badge').innerHTML = `<svg viewBox="0 0 36 32" aria-hidden="true">${CONSOLES[m].svg}</svg><span>${CONSOLES[m].name}</span>`;
    $('#console-badge').title = `Organ model: ${CONSOLES[m].name}${m === 1 ? ' (sounds like the B-3; only the case differs)' : ''}`;
    synthEl.classList.toggle('model-c3', m === 1);
  });

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

  // ------------------------------------------------- expression pedal ------
  let exprDragging = false;
  const exprPedal = new ExpressionPedal((v) => engine.expression(v), (b) => { exprDragging = b; });
  exprPedal.el.title = 'Expression (swell) pedal: toe down = open. Follows CC11 (or the Expression CC). Closing it cuts the mids and treble more than the bass.';
  $('#expr-slot').replaceWith(exprPedal.el);

  // ----------------------------------------------------------- MIDI learn ------
  // LEARN on: click a control to arm it, then move a knob or fader on your controller.
  // Right-click a control (in learn mode) to forget its CC. Esc or LEARN again: leave.
  let learnMode = false, learnArmed = '', learnMap = {};
  const learnBtn = $('#learn-btn');
  function setLearnMode(on) {
    learnMode = on;
    document.body.classList.toggle('learning', on);
    learnBtn.classList.toggle('on', on);
    if (!on && learnArmed) engine.learnCancel();
    logMidi(on ? 'MIDI LEARN: click a control, then move a knob / fader on your controller · right-click = forget · Esc = done' : 'MIDI learn off');
  }
  learnBtn.addEventListener('click', () => setLearnMode(!learnMode));
  window.addEventListener('keydown', (e) => { if (e.key === 'Escape' && learnMode) setLearnMode(false); });
  document.addEventListener('pointerdown', (e) => {
    if (!learnMode || e.target.closest('#learn-btn')) return;
    const ctl = e.target.closest('[data-param]');
    if (!ctl) return;
    e.preventDefault(); e.stopPropagation();
    if (e.button === 2) { engine.midiForget(ctl.dataset.param); return; }
    engine.midiLearn(ctl.dataset.param);
  }, true);
  engine.on('learn', ({ armed, map }) => {
    if (learnArmed && !armed && map[learnArmed] !== undefined)
      logMidi(`Learned: CC ${map[learnArmed]} → ${PARAM_BY_ID[learnArmed].name}`);
    learnArmed = armed;
    learnMap = map;
    document.querySelectorAll('[data-param]').forEach(n => {
      const id = n.dataset.param;
      n.classList.toggle('learn-armed', id === armed);
      n.classList.toggle('learned', map[id] !== undefined);
      if (map[id] !== undefined) n.dataset.cc = `CC ${map[id]}`; else delete n.dataset.cc;
    });
    if (armed) logMidi(`MIDI LEARN: move a controller for ${PARAM_BY_ID[armed].name}…`);
  });

  // rotors: rpm readout (the animated cabinet drawing comes in phase 4)
  const rpmEl = $('#rpm-readout');
  engine.on('rotor', (r) => {
    if (rpmEl) rpmEl.textContent = `HORN ${Math.round(r.hornHz * 60)} · DRUM ${Math.round(r.drumHz * 60)} rpm`;
    if (cabView) cabView.setRotor(r);
  });

  // ------------------------------------------------------------- stats -----
  const sustainLed = $('#sustain-led'), exprMini = $('#expr-mini'), motorLed = $('#motor-led');
  engine.on('stats', (s) => {
    sustainLed.classList.toggle('on', !!s.sustain);
    if (typeof s.expression === 'number') {
      exprMini.style.width = `${clamp(s.expression, 0, 1) * 100}%`;
      if (!exprDragging) exprPedal.setValue(s.expression);
    }
    if (typeof s.speed === 'number')
      document.querySelectorAll('.speed-lamp').forEach(l => l.classList.toggle('on', Number(l.dataset.s) === s.speed));
    if (typeof s.perc === 'number' && percLamp) {
      percLamp.classList.toggle('on', s.perc > 0.02 && values.percOn >= 0.5);
      percLamp.style.opacity = values.percOn >= 0.5 ? String(0.3 + 0.7 * clamp(s.perc * 3, 0, 1)) : '';
    }
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
    if (learnMode) return;
    const ctl = e.target.closest('[data-param]');
    if (ctl) engine.paramMenu(ctl.dataset.param, e.clientX, e.clientY);
  });

  // -------------------------------------------------------------- init -----
  renderHistory();
  renderAb();
  renderPresetName();
  renderBrowser();
  showPage(activePage);
  setOctave(Number(store.get('ham.octave')) || 24);
  refresh(null);
  fit();
  if (engine.connected) engine.uiReady();
  else logMidi('Not running inside the plugin: no audio engine connected (UI preview only)');
})();
