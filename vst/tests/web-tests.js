// ============================================================================
//  Headless UI tests for the web build (run by tests/run-ui-tests.ps1, which
//  appends this script to a copy of web/index.html and loads it in headless
//  Edge, from disk and served). They drive the real UI (wheel, clicks, keys)
//  and check what reaches the web host and the DSP (window.__hamWebHost taps
//  the messages), the preset store, the MIDI paths the host handles itself
//  (drawbar CC maps, keyswitches, MIDI learn), a render by the page's own
//  engine, and - served, in real time - the AudioWorklet's stats and rotor
//  events and the cabinet drawing following them.
//  Results: "PASSED" / "FAILED" + one line per check, in a pre element
//  (id ham-test-out) and, when served, POSTed to /result.
// ============================================================================
(function () {
  'use strict';
  const res = [];
  const ok = (name, cond, detail) => res.push((cond ? 'ok   ' : 'FAIL ') + name + (!cond && detail !== undefined ? `  [${detail}]` : ''));
  const sleep = (ms) => new Promise(r => setTimeout(r, ms));
  const $ = (s) => document.querySelector(s);
  const $$ = (s) => Array.from(document.querySelectorAll(s));
  const served = location.protocol !== 'file:';
  const host = window.__hamWebHost;
  const backend = window.__JUCE__.backend;
  const sent = (type) => host.sent.filter(m => m.type === type);
  const lastSent = (type) => sent(type).slice(-1)[0];
  const clearSent = () => { host.sent.length = 0; };
  const messages = [];
  backend.addEventListener('message', (m) => messages.push(m.text));
  const presetButtons = () => $$('.preset-btn');

  async function finish() {
    const failed = res.some(x => x.startsWith('FAIL'));
    const text = (failed ? 'FAILED' : 'PASSED') + '\n' + res.join('\n');
    const pre = document.createElement('pre');
    pre.id = 'ham-test-out';
    pre.textContent = text;
    document.body.appendChild(pre);
    document.title = failed ? 'FAILED' : 'PASSED';
    if (served) await fetch('/result', { method: 'POST', body: text }).catch(() => {});
  }

  async function run() {
    for (let i = 0; i < 100 && !host.audioMode; i++) await sleep(50);   // the AudioWorklet module loads asynchronously
    host.tap = true;

    // ---- start-up ------------------------------------------------------------
    ok('no script errors during start-up', window.__hamErrors.length === 0, window.__hamErrors.join(' | '));
    ok('bridge connected (config received, web mode)', document.body.classList.contains('web'), document.body.className);
    ok(`audio engine started: ${host.audioMode || 'none'} (${served ? 'served: AudioWorklet' : 'from disk: ScriptProcessor'})`,
       served ? host.audioMode === 'AudioWorklet' : host.audioMode === 'ScriptProcessor');
    ok('50 factory presets in 5 categories', host.factory.length === 50 && new Set(host.factory.map(p => p.category)).size === 5,
       `${host.factory.length} / ${new Set(host.factory.map(p => p.category)).size}`);
    ok('starts on the first factory preset (Trio Classic: upper B 88 8000 000, C3 upper)',
       host.current.name === 'Trio Classic' && host.values.upB1 === 8 && host.values.upB4 === 0 && host.values.vibUpper === 1 && host.values.vibMode === 5);
    ok('the preset browser shows the 10 Jazz presets', presetButtons().length === 10, presetButtons().length);
    const presetParams = PARAM_DEFS.filter(d => !d.noPreset);   // master is the host's (no control in the UI)
    ok('every preset parameter has a control', presetParams.every(d => $(`[data-param="${d.id}"]`)), presetParams.filter(d => !$(`[data-param="${d.id}"]`)).map(d => d.id).join(' '));
    {
      const T = ['cab22', 'cab45', 'cabSingle', 'cabTone'];
      let good = true, detail = '';
      for (let m = 0; m < 4; m++) {
        const t = FACTORY_JSON.templates[T[m]].params;
        for (const [id, v] of Object.entries(host.cabinetDefaults[m])) if (t[id] !== v) { good = false; detail += `${T[m]}.${id} ${t[id]} vs ${v}; `; }
        if (t.cabModel !== m) { good = false; detail += `${T[m]}.cabModel; `; }
      }
      ok('the cabinet defaults table matches the factory cabinet templates (which the C++ tests check against Cabinets.h)', good, detail);
    }

    // ---- controls reach the host and the DSP -----------------------------------
    clearSent();
    $('.drawbar[data-param="upB1"] svg').dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
    ok('drawbar wheel: upB1 8 -> 7 in the host, one param message to the DSP',
       host.values.upB1 === 7 && sent('param').length === 1 && lastSent('param').id === 'upB1' && lastSent('param').value === 7);
    clearSent();
    {   // a rocker tab: clicking its lower third switches it off
      const rk = $('[data-param="percOn"]'), rr = rk.getBoundingClientRect();
      rk.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, clientX: rr.left + rr.width / 2, clientY: rr.bottom - 2 }));
    }
    ok('percussion rocker (lower half) -> percOn 0 (Trio Classic has it on)', host.values.percOn === 0 && !!lastSent('param') && lastSent('param').id === 'percOn');
    $('#undo-btn').click();
    ok('undo restores percOn 1', host.values.percOn === 1);

    // ---- presets ---------------------------------------------------------------
    clearSent();
    $$('.cat-tab').find(t => t.textContent === 'Gospel').click();
    presetButtons().find(b => b.title === 'Full Organ').click();
    await sleep(50);
    ok('loading Full Organ: Fast, upper 88 8888 888, params + registrations sent to the DSP',
       host.current.name === 'Full Organ' && host.values.speed === 2 && host.values.upB9 === 8 && sent('params').length >= 1 && sent('registrations').length >= 1);
    ok('the UI follows (drawbar upB9 at 8, preset name shown)',
       $('.drawbar[data-param="upB9"] svg').getAttribute('aria-valuenow') === '8' && $('#preset-name').textContent.startsWith('Full Organ'));
    $('#preset-next').click();
    await sleep(50);
    ok('NEXT steps to Hymn Swell (Tone Cabinet, stationary)', host.current.name === 'Hymn Swell' && host.values.cabModel === 3 && host.values.rotorConfig === 3);

    clearSent();
    const regInputs = $$('.small-reg');
    regInputs[0].value = '888';
    regInputs[0].dispatchEvent(new Event('change'));
    ok('typed preset-key registration -> host regs (upper C# = 888000000) + registrations message',
       host.regs[0][0].join('') === '888000000' && sent('registrations').length === 1 && regInputs[0].value === '88 8000 000');

    clearSent();
    const cm = $('select[data-param="cabModel"]');
    cm.value = '1';
    cm.dispatchEvent(new Event('change'));
    await sleep(50);
    ok('picking Rotor 45 loads its 14 defaults (hornFast 6.8, drumAccel 3.5, mics Stereo Horn) in one params message',
       host.values.cabModel === 1 && host.values.hornFast === 6.8 && host.values.drumAccel === 3.5 && host.values.micLayout === 1
       && sent('params').length === 1 && Object.keys(lastSent('params').values).length === 14);
    ok('the Rotor 45 drawing becomes active', $('.cab-layer[data-model="1"]').classList.contains('active'));

    // save in this browser, change, reload, delete
    localStorage.removeItem('ham.userPresets.v1');
    backend.emitEvent('savePreset', { name: 'Web Test', overwrite: false });
    await sleep(50);
    ok('SAVE stores a user preset in this browser and selects it', host.current.id === 'local:Web Test' && $$('.cat-tab').some(t => t.textContent === 'User'));
    const saved = host.values.upB1;
    $('.drawbar[data-param="upB1"] svg').dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
    const changed = host.values.upB1;
    backend.emitEvent('loadPreset', { id: 'local:Web Test' });
    await sleep(50);
    ok('reloading it restores the drawbar', changed !== saved && host.values.upB1 === saved, `${saved} -> ${changed} -> ${host.values.upB1}`);
    backend.emitEvent('savePreset', { name: 'Web Test', overwrite: false });
    await sleep(50);
    const save2 = $('#modal-title').textContent;
    ok('saving the same name again asks to replace', !$('#modal').classList.contains('hidden') && /Replace/i.test(save2), save2);
    $('#modal-cancel').click();
    backend.emitEvent('deletePreset', { id: 'local:Web Test' });
    await sleep(50);
    ok('DELETE removes it', !$$('.preset-btn').some(b => b.title === 'Web Test') && messages.some(m => m.includes('Deleted')));

    // the .hampreset text: the plugin's format, drawbars as whole steps, registrations block
    {
      const t = JSON.parse(host.presetText('Round Trip'));
      ok('.hampreset text: format, version 1, drawbars as integers, registrations block',
         t.format === 'The Ham Preset' && t.version === 1 && Number.isInteger(t.params.upB1) && t.registrations.upper[0] === '88 8000 000' && t.params.master === undefined);
      t.params.upB1 = 3; t.params.bogus = 1; t.params.hornFast = 99; t.version = 0;
      t.registrations.upper[0] = 'not a registration';
      clearSent();
      host.loadPresetText(JSON.stringify(t), 'Round Trip.hampreset');
      await sleep(20);
      ok('loading it: unknown params ignored, out-of-range clamped (hornFast 10), old version migrated, bad registration keeps the factory chart',
         host.values.upB1 === 3 && host.values.hornFast === 10 && host.regs[0][0].join('') === '005320000' && host.current.category === 'File');
      let refused = '';
      try { host.loadPresetText('{"name":"x","params":{"nothing":1}}', 'foreign.json'); } catch (e) { refused = e.message; }
      ok('a file without The Ham settings is refused', /No The Ham settings/.test(refused), refused);
    }

    // ---- keys, pedal, panic ------------------------------------------------------
    clearSent();
    const key = $('#kb-upper [data-idx="24"]');
    const kr = key.getBoundingClientRect();
    const svg = key.ownerSVGElement;
    svg.setPointerCapture = () => {};
    svg.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true, clientX: kr.left + kr.width / 2, clientY: kr.top + kr.height * 0.8, pointerId: 7 }));
    svg.dispatchEvent(new PointerEvent('pointerup', { bubbles: true, pointerId: 7 }));
    ok('on-screen upper key 24 -> keyOn {manual 0, key 24} + keyOff', sent('keyOn').length === 1 && sent('keyOn')[0].manual === 0 && sent('keyOn')[0].key === 24 && sent('keyOff').length === 1);
    clearSent();
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', bubbles: true }));
    window.dispatchEvent(new KeyboardEvent('keyup', { key: 'k', bubbles: true }));
    ok('computer key K plays upper key 36 (A = the C at 24)', sent('keyOn').length === 1 && sent('keyOn')[0].key === 36 && sent('keyOff').length === 1);
    clearSent();
    $('.expr-pedal svg').dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
    ok('expression pedal -> expression message below 1', !!lastSent('expression') && lastSent('expression').value < 1);
    clearSent();
    $('#panic-btn').click();
    ok('PANIC -> panic message', sent('panic').length === 1);

    // ---- MIDI: the host's own paths, then the engine's ----------------------------
    clearSent();
    host.midi([0x90, 60, 100]); host.midi([0x80, 60, 0]); host.midi([0xb0, 11, 64]);
    ok('MIDI note on / off / CC11 -> midi messages (routed inside the engine)', sent('midi').length === 3 && sent('midi')[0].data[1] === 60 && sent('midi')[2].data[1] === 11);
    clearSent();
    host.midi([0xb0, 12, 127]); host.midi([0xb0, 21, 0]);
    ok('B4D drawbar CCs 12 / 21 move upper 16\' and lower 16\' of the active set (B) as parameters, not MIDI',
       host.values.upB1 === 8 && host.values.loB1 === 0 && sent('param').length === 2 && sent('midi').length === 0);
    $('[data-param="ksOn"]').click();
    clearSent();
    host.midi([0x90, 25, 100]); host.midi([0x80, 25, 0]);
    ok('keyswitch: note 25 on the upper channel presses preset key C# (upKey 1) and never sounds', host.values.upKey === 1 && sent('midi').length === 0 && lastSent('param').id === 'upKey');
    $('[data-param="ksOn"]').click();
    $('#learn-btn').click();
    $('.drawbar[data-param="loB3"]').dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true, button: 0 }));
    ok('LEARN: clicking a drawbar arms it', host.learn.armed === 'loB3', host.learn.armed);
    clearSent();
    host.midi([0xb0, 20, 64]);
    await sleep(20);
    ok('the next CC (20) at 64 is learned, moves the drawbar (loB3 = 8 x 64 / 127) and the control is marked CC 20',
       host.learn.map.loB3 === 20 && Math.abs(host.values.loB3 - 8 * 64 / 127) < 1e-6 && $('.drawbar[data-param="loB3"]').dataset.cc === 'CC 20' && sent('midi').length === 0);
    $('#learn-btn').click();
    ok('the learned map is kept in this browser', (localStorage.getItem('ham.midiLearn') || '').includes('20=loB3'));
    backend.emitEvent('midiForget', { id: 'loB3' });
    ok('forgetting it clears the map', host.learn.map.loB3 === undefined);

    // ---- the page's own DSP renders --------------------------------------------------
    {
      let Proc = null;
      const shims = { sampleRate: 48000, AudioWorkletProcessor: class { constructor() { this.port = { postMessage() {}, onmessage: null }; } },
                      registerProcessor: (n, c) => { Proc = c; } };
      new Function('AudioWorkletProcessor', 'registerProcessor', 'sampleRate', `(${synthWorkletModule.toString()})();`)(shims.AudioWorkletProcessor, shims.registerProcessor, shims.sampleRate);
      const render = (presetName, keys, blocks) => {
        const values = Object.assign(defaultParamValues(), host.factory.find(p => p.name === presetName).values);
        const p = new Proc({ processorOptions: { params: values } });
        for (const [m, k] of keys) p.port.onmessage({ data: { type: 'keyOn', manual: m, key: k, vel: 100 } });
        const L = new Float32Array(128), R = new Float32Array(128);
        let acc = 0, bad = false;
        for (let b = 0; b < blocks; b++) {
          p.process([], [[L, R]]);
          for (let i = 0; i < 128; i++) { acc += L[i] * L[i] + R[i] * R[i]; if (!Number.isFinite(L[i]) || !Number.isFinite(R[i])) bad = true; }
        }
        return { rms: Math.sqrt(acc / (2 * blocks * 128)), bad };
      };
      const a = render('Trio Classic', [[0, 24], [0, 28], [0, 31], [2, 0]], 200);
      ok(`the page's engine renders Trio Classic (RMS ${(20 * Math.log10(a.rms + 1e-12)).toFixed(1)} dBFS, finite)`, a.rms > 0.003 && !a.bad);
      const b = render('Bach Plenum', [[0, 24], [1, 12]], 200);
      ok(`and Bach Plenum (Tone Cabinet + reverb; RMS ${(20 * Math.log10(b.rms + 1e-12)).toFixed(1)} dBFS)`, b.rms > 0.003 && !b.bad);
      const c = render('Trio Classic', [], 100);
      ok('silence with no keys down (the leakage floor is far below -60 dBFS)', c.rms < 0.001 && !c.bad, c.rms);
    }

    // ---- served: the AudioWorklet runs in real time ------------------------------------
    if (served) {
      let lastStats = null, lastRotor = null, peak = 0, rotorEvents = 0;
      backend.addEventListener('stats', (s) => { lastStats = s; peak = Math.max(peak, s.peak || 0); });
      backend.addEventListener('rotor', (r) => { lastRotor = Object.assign({ t: performance.now() }, r); rotorEvents++; });
      $$('.cat-tab').find(t => t.textContent === 'Jazz').click();
      presetButtons().find(b => b.title === 'Trio Classic').click();
      $('.page-tab[data-page="cabinet"]').click();
      await sleep(100);
      const n0 = host.statsCount;
      await sleep(1000);
      ok(`stats arrive from the audio thread (${host.statsCount - n0} in 1 s, about 30 expected)`, host.statsCount - n0 >= 15);
      peak = 0;
      backend.emitEvent('noteOn', { manual: 0, key: 24, vel: 100 });
      backend.emitEvent('noteOn', { manual: 2, key: 0, vel: 100 });
      await sleep(700);
      ok(`a key held on the worklet produces sound (peak ${peak.toFixed(3)})`, peak > 0.002);
      ok('the engine reports the held keys back (notes event lights the keys)', $('#kb-upper [data-idx="24"]').classList.contains('down') && $('#kb-pedal [data-idx="0"]').classList.contains('down'));
      // Fast: the horn spins up within about a second; the drawing follows the reported angles
      $$('.seg[data-param="speed"] button')[2].click();
      await sleep(1500);
      const view = window.__cabView;
      const hg = $('.cab-layer[data-model="0"] .horn-assy');
      ok(`rotor events arrive (${rotorEvents}); the horn is near Fast (${lastRotor ? lastRotor.hornHz.toFixed(2) : '-'} Hz of 6.7) and the drum is still accelerating (${lastRotor ? lastRotor.drumHz.toFixed(2) : '-'} Hz)`,
         rotorEvents > 20 && lastRotor && lastRotor.hornHz > 5.5 && lastRotor.drumHz > 1 && lastRotor.drumHz < 5.5);
      // The drawing follows the engine's reports: the drawn horn advances at the reported rate, and stays within an
      // audio buffer's worth of lag of each report (a report's angle is the audio thread's, a few ms ahead of the
      // ear and of the message's arrival, so the reports jitter by that much; the drawing smooths it over ~80 ms).
      const dir = host.values.hornDir === 0 ? 1 : -1;
      const samples = [];
      for (let k = 0; k < 20; k++) {
        await sleep(40);
        if (!lastRotor || !view) continue;
        const truth = lastRotor.hornAngle + dir * lastRotor.hornHz * (view.frameTime - lastRotor.t) / 1000;
        let d = Number(hg.dataset.angle) - truth; d -= Math.round(d);
        samples.push({ t: view.frameTime, a: Number(hg.dataset.angle), d });
      }
      let worst = 0, turns = 0;
      for (let i = 1; i < samples.length; i++) {
        let da = samples[i].a - samples[i - 1].a; da -= Math.round(da);   // under half a turn per step at 6.7 Hz
        turns += da;
        if (i > 2) worst = Math.max(worst, Math.abs(samples[i].d));
      }
      const secs = samples.length > 1 ? (samples[samples.length - 1].t - samples[0].t) / 1000 : 1;
      const rate = dir * turns / secs;
      ok(`the drawn horn spins at the engine's rate (${rate.toFixed(2)} Hz drawn, ${lastRotor ? lastRotor.hornHz.toFixed(2) : '-'} Hz reported) and stays within ${(worst * 360).toFixed(1)} deg of the reports`,
         samples.length > 10 && lastRotor && Math.abs(rate - lastRotor.hornHz) < 0.1 * lastRotor.hornHz && worst < 0.1 && view.running);
      ok(`the speed lamp shows FAST and the rpm readout runs (${$('#rpm-readout').textContent})`,
         $('.speed-lamp[data-s="2"]').classList.contains('on') && /HORN [1-9]\d* /.test($('#rpm-readout').textContent));
      backend.emitEvent('noteOff', { manual: 0, key: 24 });
      backend.emitEvent('noteOff', { manual: 2, key: 0 });
      await sleep(300);
      ok('releasing the keys: the engine reports them up', !$('#kb-upper [data-idx="24"]').classList.contains('down'));
    }

    host.tap = false;
    await finish();
  }

  window.addEventListener('load', () => setTimeout(() => run().catch(async (e) => { res.push('FAIL exception: ' + (e.stack || e)); await finish(); }), 300));
})();
