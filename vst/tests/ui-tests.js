// ============================================================================
//  UI tests: the plugin UI (vst/ui) with a mocked JUCE bridge, in headless Edge,
//  served over http in real time (requestAnimationFrame runs). Run with
//  tests\run-ui-tests.ps1, which injects this file before params.js, serves the
//  page and collects the result the page POSTs to /result.
//  Checks: drawbars, registration strings, preset keys, keyboards, host -> UI
//  params / notes, the preset-key registration editor, MIDI learn, the expression
//  pedal, rocker tabs, undo / redo, A/B compare, randomize, the organ-model badge,
//  the cabinet picker, and the animated cabinet following the `rotor` events
//  (angle tracking, frames, cost per frame, idling).
//  Phase 6 extends this for the web build.
// ============================================================================
window.addEventListener("error", e => fetch("/result", { method: "POST", body: "ERROR " + e.message + " line " + e.lineno + " " + (e.filename || "") }));
window.addEventListener("unhandledrejection", e => fetch("/result", { method: "POST", body: "REJECTION " + (e.reason && e.reason.stack || e.reason) }));
window.__log = []; window.__h = {};
window.__JUCE__ = { backend: {
  emitEvent(id, p) { window.__log.push([id, JSON.parse(JSON.stringify(p || {}))]); },
  addEventListener(ev, fn) { window.__h[ev] = fn; } } };
window.addEventListener('load', () => setTimeout(runChecks, 100));
function runChecks() {
  const res = [];
  const ok = (name, cond) => res.push((cond ? 'ok   ' : 'FAIL ') + name);
  const sets = (id) => window.__log.filter(([e, p]) => e === 'setParam' && p.id === id).map(([, p]) => p.value);
  ok('uiReady sent', window.__log.some(([e]) => e === 'uiReady'));
  // 1. wheel on upper B 16' (scroll up = push in): 8 -> 7
  const bar = document.querySelector('.drawbar[data-param="upB1"] svg');
  bar.dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
  ok('drawbar wheel -> setParam upB1 7', sets('upB1').includes(7));
  // 2. typed registration on upper B
  const input = document.querySelectorAll('.reg-input')[1];
  input.value = '808000000'; input.dispatchEvent(new Event('change'));
  ok('registration 808000000 -> upB1 8, upB2 0', sets('upB1').at(-1) === 8 && sets('upB2').at(-1) === 0);
  // 3. preset key A# on the upper manual
  const pk = document.querySelector('#pk-upper [data-pk="10"]');
  pk.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true }));
  ok('preset key A# -> setParam upKey 10', sets('upKey').includes(10));
  // 4. a key on the lower manual
  const key = document.querySelector('#kb-lower [data-idx="24"]');
  const r = key.getBoundingClientRect();
  const svg = key.ownerSVGElement;
  svg.setPointerCapture = () => {};
  svg.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true, clientX: r.left + r.width / 2, clientY: r.top + r.height * 0.8, pointerId: 7 }));
  svg.dispatchEvent(new PointerEvent('pointerup', { bubbles: true, pointerId: 7 }));
  const n = window.__log.filter(([e]) => e === 'noteOn' || e === 'noteOff');
  ok('lower key 24 -> noteOn {manual 1, key 24} + noteOff', n.length === 2 && n[0][1].manual === 1 && n[0][1].key === 24 && n[1][0] === 'noteOff');
  // 5. host moves a drawbar: animates, string follows
  window.__h.params({ loB4: 6 });
  setTimeout(async () => {
    const lb = document.querySelector('.drawbar[data-param="loB4"] svg');
    ok('host loB4 = 6 -> drawbar at 6', lb.getAttribute('aria-valuenow') === '6');
    ok('lower B string = 83 8600 000', document.querySelectorAll('.reg-input')[4].value === '83 8600 000');
    // 6. host notes -> keys light
    window.__h.notes({ on: [[2, 0]], off: [] });
    ok('notes event lights pedal key 0', document.querySelector('#kb-pedal [data-idx="0"]').classList.contains('down'));
    // 7. registrations from the host fill the editor; typing one sends setRegistration
    window.__h.registrations({ upper: ['00 5320 000','00 4432 000','00 8740 000','00 4544 222','00 5403 000','00 4675 300','00 5644 320','00 6876 540','32 7645 222'],
                               lower: Array(9).fill('00 8000 000') });
    const ri = document.querySelectorAll('.small-reg');
    ok('registrations shown (18 fields, upper C# = 00 5320 000)', ri.length === 18 && ri[0].value === '00 5320 000');
    ri[10].value = '888'; ri[10].dispatchEvent(new Event('change'));
    const sr = window.__log.filter(([e]) => e === 'setRegistration').at(-1);
    ok('typed registration -> setRegistration {1, 1, "88 8000 000"}', !!sr && sr[1].manual === 1 && sr[1].key === 1 && sr[1].bars === '88 8000 000');
    // 8. MIDI learn
    document.getElementById('learn-btn').click();
    document.querySelector('.drawbar[data-param="upB3"]').dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true, button: 0 }));
    const ml = window.__log.filter(([e]) => e === 'midiLearn').at(-1);
    ok('learn mode: clicking a drawbar arms it (midiLearn upB3)', !!ml && ml[1].id === 'upB3' && !sets('upB3').length);
    window.__h.learn({ armed: '', map: { upB3: 14 } });
    ok('learned control is marked (CC 14)', document.querySelector('.drawbar[data-param="upB3"]').dataset.cc === 'CC 14');
    document.getElementById('learn-btn').click();
    // 9. expression pedal (toe up = closing) -> expression event; the engine's value moves the drawing
    const ex = document.querySelector('.expr-pedal svg');
    ex.dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
    const ev = window.__log.filter(([e]) => e === 'expression').at(-1);
    window.__h.stats({ expression: 0.25 });
    ok('expression pedal -> expression event; stats move the shoe', !!ev && ev[1].value < 1
       && document.querySelector('.ep-shoe').style.transform.includes('-11.8'));
    // 10. rocker tabs send their parameter
    document.querySelector('[data-param="percOn"]').click();
    ok('percussion rocker -> setParam percOn 1', sets('percOn').at(-1) === 1);
    // 11. undo / redo
    const before = window.__log.length;
    document.getElementById('undo-btn').click();
    ok('undo restores percOn 0', sets('percOn').at(-1) === 0 && window.__log.length > before);
    document.getElementById('redo-btn').click();
    ok('redo sets percOn 1 again', sets('percOn').at(-1) === 1);
    // 12. A/B: B starts as a copy; a change on B; back to A restores
    document.getElementById('ab-b').click();
    document.querySelector('[data-param="vibUpper"]').click();
    const vibOnB = sets('vibUpper').at(-1);
    document.getElementById('ab-a').click();
    ok('A/B: a change on B is undone by switching back to A', vibOnB === 1 && sets('vibUpper').at(-1) === 0);
    // 13. randomize: nine bars of the upper active set, inside the family's ranges
    const n0 = window.__log.length;
    [...document.querySelectorAll('#db-tools button')].find(b => b.textContent === 'STRING').click();
    const rnd = window.__log.slice(n0).filter(([e, p]) => e === 'setParam' && /^upA\d$/.test(p.id));   // A# is down (check 3): set A
    ok('randomize STRING: the active upper set changes, 5 1/3\' = 0, 2 2/3\' in 5..8',
       rnd.some(([, p]) => p.id === 'upA5' && p.value >= 5 && p.value <= 8) && !rnd.some(([, p]) => p.id === 'upA2' && p.value !== 0));
    // 13b. the rotor speed switch: FAST sends speed 2; in 2-Speed the STOP segment is greyed out (the
    //      engine runs Slow for it), in Variable the whole switch is (the wheel / pedal / CC sets the speed)
    const speedBtns = [...document.querySelector('.seg[data-param="speed"]').querySelectorAll('button')];
    speedBtns[2].click();
    ok('speed switch FAST -> setParam speed 2', sets('speed').at(-1) === 2);
    ok('2-Speed: STOP greyed out, SLOW and FAST live', speedBtns[0].disabled && !speedBtns[1].disabled && !speedBtns[2].disabled);
    window.__h.params({ speedMode: 1 });
    ok('3-Speed: STOP live', !speedBtns[0].disabled);
    window.__h.params({ speedMode: 2 });
    ok('Variable: the speed switch is greyed out', speedBtns.every(b => b.disabled));
    window.__h.params({ speedMode: 0 });
    // 14. organ model badge (C-3 = the churchier case), cabinet model picker
    window.__h.params({ organModel: 1 });
    ok('organ model C-3: badge and case change', document.getElementById('console-badge').textContent.includes('C-3')
       && document.querySelector('.synth').classList.contains('model-c3'));
    const cm = document.querySelector('select[data-param="cabModel"]');
    cm.value = '2'; cm.dispatchEvent(new Event('change'));
    const ce = window.__log.filter(([e]) => e === 'cabinetModel').at(-1);
    ok('picking Single Rotor -> cabinetModel {2}; its drawing becomes active', !!ce && ce[1].model === 2
       && document.querySelector('.cab-layer[data-model="2"]').classList.contains('active'));
    await new Promise(r => setTimeout(r, 350));   // the UI ignores host echoes for 300 ms after a local change
    window.__h.params({ cabModel: 0 });
    // 15. the cabinet drawing follows the rotor events (real time: needs requestAnimationFrame)
    document.querySelector('.page-tab[data-page="cabinet"]').click();
    window.__h.rotor({ hornAngle: 0.25, drumAngle: 0.6, hornHz: 0, drumHz: 0 });
    await new Promise(r => setTimeout(r, 600));
    const hg = document.querySelector('.cab-layer[data-model="0"] .horn-assy'), dg = document.querySelector('.cab-layer[data-model="0"] .drum-assy');
    ok(`drawing settles on the reported angles (horn ${hg.dataset.angle}, drum ${dg.dataset.angle})`,
       Math.abs(Number(hg.dataset.angle) - 0.25) < 0.002 && Math.abs(Number(dg.dataset.angle) - 0.6) < 0.002);
    // spinning: 6.7 Hz horn reported every 33 ms; the drawn angle must track the true one
    const cab = document.querySelector('.cab-svg');
    const t0 = performance.now();
    let worst = 0;
    const view = window.__cabView;
    const f0 = view ? view.frames : 0, b0 = view ? view.busyMs : 0;
    for (let k = 0; k < 45; k++) {
      const t = (performance.now() - t0) / 1000;
      window.__h.rotor({ hornAngle: (0.25 + 6.7 * t) % 1, drumAngle: 0.6, hornHz: 6.7, drumHz: 0 });
      await new Promise(r => setTimeout(r, 33));
      // the true angle at the moment the last frame was drawn
      const truth = (0.25 + 6.7 * (view.frameTime - t0) / 1000) % 1;
      let d = Number(hg.dataset.angle) - truth; d -= Math.round(d);
      if (k > 5) worst = Math.max(worst, Math.abs(d));
    }
    const frames = view ? view.frames - f0 : 0, ms = view ? (view.busyMs - b0) / Math.max(1, frames) : -1;
    ok(`spinning horn: drawn angle within ${(worst * 360).toFixed(1)} deg of the true one; ${frames} frames in 1.5 s, ${ms.toFixed(3)} ms per frame`,
       worst < 0.02 && frames > 30);
    // still rotors and a settled drawing: the loop stops (no CPU when nothing moves)
    window.__h.rotor({ hornAngle: 0.5, drumAngle: 0.5, hornHz: 0, drumHz: 0 });
    await new Promise(r => setTimeout(r, 700));
    const fIdle = view.frames;
    await new Promise(r => setTimeout(r, 500));
    ok(`idle: no frames drawn while nothing moves (${view.frames - fIdle} in 0.5 s)`, view.frames === fIdle && !view.running);
    document.querySelector('.page-tab[data-page="drawbars"]').click();
    window.__h.rotor({ hornAngle: 0.1, drumAngle: 0.1, hornHz: 6.7, drumHz: 5.8 });
    const fOff = view.frames;
    await new Promise(r => setTimeout(r, 300));
    ok(`off its page: the drawing doesn't animate (${view.frames - fOff} frames)`, view.frames === fOff);
    window.__perf = { frames, ms };
    const out = document.getElementById('out');
    if (out) out.textContent = res.join('\n');
    document.title = res.some(x => x.startsWith('FAIL')) ? 'FAILED' : 'PASSED';
    fetch('/result', { method: 'POST', body: document.title + '\n' + res.join('\n') });
  }, 400);
}
