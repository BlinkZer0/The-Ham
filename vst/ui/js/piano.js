// ============================================================================
//  Keyboards: a 61-key manual (or the 25-key pedalboard) as SVG.
//  Click/touch to play, drag for glissando (palm smears work: the wheels never
//  stop, so every key joins in phase). Keys are indexed on their own manual:
//  0 = its lowest C. Organ keys are switches, so the click height is only used
//  as velocity when Velocity is on.
//
//  PresetKeys: the reverse-coloured octave left of each manual (C cancel,
//  C#..A fixed registrations, A# = drawbar set A, B = set B).
// ============================================================================
'use strict';

const BLACK_PCS = new Set([1, 3, 6, 8, 10]);

class Manual {
  constructor(container, { numKeys, pedal = false, onKeyOn, onKeyOff }) {
    this.numKeys = numKeys;
    this.pedal = pedal;
    this.onKeyOn = onKeyOn;
    this.onKeyOff = onKeyOff;
    this.down = new Set();       // keys lit (any source)
    this.pointers = new Map();   // pointerId -> key
    this.build(container);
  }

  build(container) {
    const p = this.pedal;
    const WW = p ? 44 : 20, WH = p ? 92 : 78, BW = p ? 22 : 12, BH = p ? 52 : 48, TOP = 3;
    const whites = [], blacks = [];
    let wi = 0;
    for (let i = 0; i < this.numKeys; i++) {
      if (BLACK_PCS.has(i % 12)) {
        blacks.push(`<rect class="bkey" data-idx="${i}" x="${wi * WW - BW / 2}" y="${TOP}" width="${BW}" height="${BH}" rx="${p ? 3 : 1.5}"/>`);
      } else {
        whites.push(`<rect class="wkey" data-idx="${i}" x="${wi * WW + 0.75}" y="${TOP}" width="${WW - 1.5}" height="${WH}" rx="${p ? 4 : 2}"/>`);
        if (i % 12 === 0) whites.push(`<text class="klabel" x="${wi * WW + WW / 2}" y="${TOP + WH - 6}">C${p ? '' : i / 12 + 2}</text>`);
        wi++;
      }
    }
    const W = wi * WW;
    container.innerHTML = `
      <svg class="manual-svg${p ? ' pedalboard' : ''}" viewBox="0 0 ${W} ${TOP + WH + 2}" preserveAspectRatio="xMidYMid meet">
        <rect class="key-rail" x="0" y="0" width="${W}" height="${TOP}"/>
        ${whites.join('')}
        ${blacks.join('')}
      </svg>`;
    this.svg = container.querySelector('svg');
    this.keys = Array.from({ length: this.numKeys }, (_, i) => this.svg.querySelector(`[data-idx="${i}"]`));
    this.bindPointer();
  }

  setKey(key, on) {
    if (key < 0 || key >= this.numKeys) return;
    if (on) this.down.add(key); else this.down.delete(key);
    this.keys[key].classList.toggle('down', on);
  }

  clear() { this.down.forEach(k => this.keys[k].classList.remove('down')); this.down.clear(); }

  hit(x, y) {
    const t = document.elementFromPoint(x, y);
    if (!t || !this.svg.contains(t) || t.dataset.idx === undefined) return null;
    const r = t.getBoundingClientRect();
    const depth = clamp((y - r.top) / r.height, 0, 1);
    return { key: Number(t.dataset.idx), vel: Math.round(25 + depth * 102) };
  }

  bindPointer() {
    const svg = this.svg;
    const lift = (id) => {
      const k = this.pointers.get(id);
      if (k !== undefined && k !== null) this.onKeyOff(k);
      this.pointers.delete(id);
    };
    svg.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      svg.setPointerCapture(e.pointerId);
      const h = this.hit(e.clientX, e.clientY);
      if (!h) return;
      this.pointers.set(e.pointerId, h.key);
      this.onKeyOn(h.key, h.vel);
    });
    svg.addEventListener('pointermove', (e) => {
      if (!this.pointers.has(e.pointerId)) return;
      const h = this.hit(e.clientX, e.clientY);
      const cur = this.pointers.get(e.pointerId);
      const next = h ? h.key : null;
      if (next === cur) return;
      if (h) this.onKeyOn(h.key, h.vel);   // glissando: next key first, then let go of the last
      if (cur !== null) this.onKeyOff(cur);
      this.pointers.set(e.pointerId, next);
    });
    svg.addEventListener('pointerup', (e) => lift(e.pointerId));
    svg.addEventListener('pointercancel', (e) => lift(e.pointerId));
  }
}

class PresetKeys {
  // onPick(index 0..11)
  constructor(container, onPick) {
    const WW = 20, WH = 78, BW = 12, BH = 48, TOP = 3;
    const whites = [], blacks = [];
    let wi = 0;
    const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    for (let i = 0; i < 12; i++) {
      const tip = i === 0 ? 'Cancel' : i === 10 ? 'Drawbar set A' : i === 11 ? 'Drawbar set B' : `Preset ${names[i]}`;
      if (BLACK_PCS.has(i)) {
        blacks.push(`<rect class="pk pk-sharp" data-pk="${i}" x="${wi * WW - BW / 2}" y="${TOP}" width="${BW}" height="${BH}" rx="1.5"><title>${tip}</title></rect>`);
        if (i === 10) blacks.push(`<text class="pk-label sharp" x="${wi * WW}" y="${TOP + BH - 6}">A</text>`);
      } else {
        whites.push(`<rect class="pk pk-nat" data-pk="${i}" x="${wi * WW + 0.75}" y="${TOP}" width="${WW - 1.5}" height="${WH}" rx="2"><title>${tip}</title></rect>`);
        if (i === 11) whites.push(`<text class="pk-label" x="${wi * WW + WW / 2}" y="${TOP + WH - 6}">B</text>`);
        if (i === 0) whites.push(`<text class="pk-label" x="${wi * WW + WW / 2}" y="${TOP + WH - 6}">×</text>`);
        wi++;
      }
    }
    const W = wi * WW;
    container.innerHTML = `
      <svg class="pk-svg" viewBox="0 0 ${W} ${TOP + WH + 2}" preserveAspectRatio="xMidYMid meet">
        <rect class="key-rail" x="0" y="0" width="${W}" height="${TOP}"/>
        ${whites.join('')}${blacks.join('')}
      </svg>`;
    this.svg = container.querySelector('svg');
    this.keys = Array.from({ length: 12 }, (_, i) => this.svg.querySelector(`[data-pk="${i}"]`));
    this.svg.addEventListener('pointerdown', (e) => {
      const t = e.target.closest('[data-pk]');
      if (!t) return;
      e.preventDefault();
      onPick(Number(t.dataset.pk));
    });
  }
  setActive(i) { this.keys.forEach((k, j) => k.classList.toggle('down', j === Math.round(i))); }
}
