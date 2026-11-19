// ============================================================================
//  Screenshot stand-in for the JUCE bridge (tests/screenshots.ps1 injects it before
//  params.js, with the factory presets in FACTORY_JSON). It feeds the UI what the
//  plugin would: a preset's resolved values, the preset list, lit keys, the rotor
//  angles and the stats, then the page is captured. ?page=<drawbars|voice|cabinet|
//  play|fx> picks the page and the preset that shows it best.
// ============================================================================
'use strict';
window.__h = {};
window.__JUCE__ = { backend: {
  emitEvent() {},
  addEventListener(ev, fn) { window.__h[ev] = fn; } } };

window.addEventListener('load', () => setTimeout(feed, 150));

function feed() {
  const h = window.__h;
  const factory = JSON.parse(document.getElementById('factory-json').textContent);
  const page = new URLSearchParams(location.search).get('page') || 'drawbars';
  const pick = { drawbars: 'Trio Classic', voice: 'Trio Classic', cabinet: 'Full Organ', play: 'Trio Classic', fx: 'Psychedelic Swirl' }[page] || 'Trio Classic';
  // resolve a preset: template chain (base first), then its own params
  const resolve = (name) => {
    const t = factory.templates[name];
    if (!t) return {};
    return Object.assign({}, resolve(t.base), t.params);
  };
  const list = [];
  let chosen = null, chosenIndex = 0;
  factory.categories.forEach(c => c.presets.forEach(p => {
    const i = list.length;
    list.push({ id: `factory:${i}`, name: p.name, category: c.name, factory: true });
    if (p.name === pick) { chosen = Object.assign({}, resolve(p.base), p.params); chosenIndex = i; }
  }));
  h.config({ sampleRate: 48000, blockSize: 256, wrapper: 'VST3', running: true, startPage: page });
  h.presetList({ presets: list, folder: 'Documents\\The Ham\\Presets' });
  h.preset({ id: `factory:${chosenIndex}`, name: list[chosenIndex].name, category: list[chosenIndex].category });
  h.params(chosen);
  h.registrations({ upper: ['00 5320 000', '00 4432 000', '00 8740 000', '00 4544 222', '00 5403 000', '00 4675 300', '00 5644 320', '00 6876 540', '32 7645 222'],
                    lower: ['00 5320 000', '00 4432 000', '00 8740 000', '00 4544 222', '00 5403 000', '00 4675 300', '00 5644 320', '00 6876 540', '32 7645 222'] });
  h.learn({ armed: null, map: {} });
  h.notes({ on: [[0, 24], [0, 28], [0, 31], [1, 12], [1, 19], [2, 0]], off: [] });
  const fast = chosen.speed === 2;
  h.stats({ peak: 0.42, sustain: false, modWheel: fast ? 0.9 : 0.1, expression: 0.85, motor: 1, perc: 0.3, speed: chosen.speed });
  // rotors: Fast spins (the horn blurs); Slow turns gently; the Tone Cabinet stands still
  const hornHz = chosen.cabModel === 3 ? 0 : (fast ? chosen.hornFast : chosen.hornSlow);
  const drumHz = chosen.cabModel === 3 ? 0 : (fast ? chosen.drumFast : chosen.drumSlow);
  const t0 = performance.now();
  const tick = () => {
    const t = (performance.now() - t0) / 1000;
    h.rotor({ hornAngle: (0.15 + hornHz * t) % 1, drumAngle: (0.6 - drumHz * t) % 1, hornHz, drumHz });
  };
  tick();
  setInterval(tick, 33);
}
