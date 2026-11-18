// ============================================================================
//  Parameter controls besides the knob. Same conventions as Knob:
//  setValue(v) updates the display, onChange(v) fires on user edits.
//    ToggleButton - bool param   (click; double-click = default)
//    Segmented    - choice param (click; wheel / arrow keys step; double-click = default)
//    SelectBox    - choice param (native dropdown; wheel steps; double-click = default)
//    ModWheel     - SVG performance wheel (drag, wheel, arrow keys, double-click = 0)
// ============================================================================
'use strict';

class ToggleButton {
  constructor(def, value, onChange, label) {
    this.def = def;
    this.onChange = onChange;
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'power';
    b.innerHTML = `<span class="led"></span><span>${label || 'ON'}</span>`;
    b.addEventListener('click', () => this.onChange(this.value >= 0.5 ? 0 : 1));
    this.el = b;
    this.setValue(value);
  }
  setValue(v) {
    this.value = v;
    this.el.classList.toggle('on', v >= 0.5);
    this.el.setAttribute('aria-pressed', v >= 0.5);
  }
}

class Segmented {
  constructor(def, value, onChange, labels) {
    this.def = def;
    this.onChange = onChange;
    const wrap = document.createElement('div');
    wrap.className = 'seg';
    wrap.setAttribute('role', 'radiogroup');
    wrap.setAttribute('aria-label', def.name);
    this.buttons = (labels || def.choices).map((label, i) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.textContent = label;
      b.setAttribute('role', 'radio');
      b.addEventListener('click', () => this.pick(i));
      wrap.appendChild(b);
      return b;
    });
    wrap.addEventListener('wheel', (e) => { e.preventDefault(); this.pick(this.value + (e.deltaY < 0 ? 1 : -1)); }, { passive: false });
    wrap.addEventListener('keydown', (e) => {
      const dir = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
      if (dir) { e.preventDefault(); e.stopPropagation(); this.pick(this.value + dir); this.buttons[this.value].focus(); }
    });
    wrap.addEventListener('dblclick', () => this.pick(this.def.def));
    this.el = wrap;
    this.setValue(value);
  }
  pick(i) {
    i = clamp(Math.round(i), 0, this.buttons.length - 1);
    if (i !== this.value) this.onChange(i);
  }
  setValue(v) {
    this.value = clamp(Math.round(v), 0, this.buttons.length - 1);
    this.buttons.forEach((b, i) => { b.classList.toggle('on', i === this.value); b.setAttribute('aria-checked', i === this.value); });
  }
}

class SelectBox {
  constructor(def, value, onChange, labels) {
    this.def = def;
    this.onChange = onChange;
    const s = document.createElement('select');
    s.className = 'param-select';
    s.setAttribute('aria-label', def.name);
    (labels || def.choices).forEach((label, i) => s.appendChild(new Option(label, String(i))));
    s.addEventListener('change', () => this.onChange(Number(s.value)));
    s.addEventListener('wheel', (e) => {
      e.preventDefault();
      const i = clamp(this.value + (e.deltaY < 0 ? -1 : 1), 0, s.options.length - 1);
      if (i !== this.value) this.onChange(i);
    }, { passive: false });
    s.addEventListener('dblclick', () => { if (this.value !== this.def.def) this.onChange(this.def.def); });
    s.addEventListener('keydown', (e) => e.stopPropagation()); // arrow keys belong to the dropdown, not the piano
    this.el = s;
    this.setValue(value);
  }
  setValue(v) {
    this.value = clamp(Math.round(v), 0, this.el.options.length - 1);
    this.el.value = String(this.value);
  }
}

class ModWheel {
  // label / title: also used as the on-screen breath controller ("BREATH")
  constructor(onChange, onGesture, label, title) {
    this.onChange = onChange;
    this.onGesture = onGesture || null;
    this.value = 0;
    this.drag = null;
    const H = 110, W = 26, T = 16;
    this.H = H; this.T = T;
    const ridges = Array.from({ length: 14 }, (_, i) => `<line x1="4" x2="${W - 4}" y1="${6 + i * 7.5}" y2="${6 + i * 7.5}"/>`).join('');
    const el = document.createElement('div');
    el.className = 'wheel-ctl';
    el.innerHTML = `
      <svg class="wheel-svg" viewBox="0 0 ${W} ${H}" tabindex="0" role="slider" aria-label="${title || 'Mod wheel'}"
           aria-valuemin="0" aria-valuemax="127">
        <rect class="wh-slot" x="0.5" y="0.5" width="${W - 1}" height="${H - 1}" rx="5"/>
        <g class="wh-ridges">${ridges}</g>
        <rect class="wh-fill" x="3" width="${W - 6}" rx="3"/>
        <rect class="wh-thumb" x="2" width="${W - 4}" height="${T}" rx="3"/>
      </svg>
      <span class="ctl-label">${label || 'MOD'}</span>
      <span class="wheel-value">0</span>`;
    this.el = el;
    this.svg = el.querySelector('svg');
    this.thumb = el.querySelector('.wh-thumb');
    this.fill = el.querySelector('.wh-fill');
    this.readout = el.querySelector('.wheel-value');
    this.bind();
    this.render();
  }

  set(v, notify) {
    v = clamp(v, 0, 1);
    if (v === this.value) return;
    this.value = v;
    this.render();
    if (notify) this.onChange(v);
  }
  setValue(v) { if (!this.drag) this.set(v, false); }

  render() {
    const y = (this.H - this.T - 4) * (1 - this.value) + 2;
    this.thumb.setAttribute('y', y.toFixed(1));
    this.fill.setAttribute('y', (y + this.T / 2).toFixed(1));
    this.fill.setAttribute('height', Math.max(0, this.H - 3 - (y + this.T / 2)).toFixed(1));
    const midi = Math.round(this.value * 127);
    this.readout.textContent = midi;
    this.svg.setAttribute('aria-valuenow', String(midi));
  }

  bind() {
    const svg = this.svg;
    const fromY = (clientY) => {
      const r = svg.getBoundingClientRect();
      return 1 - (clientY - r.top - (this.T / 2) * (r.height / this.H)) / (r.height * (this.H - this.T) / this.H);
    };
    svg.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      svg.setPointerCapture(e.pointerId);
      svg.focus();
      this.drag = { y: e.clientY, v: this.value };
      if (this.onGesture) this.onGesture(true);
      if (!e.shiftKey) this.set(fromY(e.clientY), true); // click jumps, Shift-drag is relative + fine
    });
    svg.addEventListener('pointermove', (e) => {
      if (!this.drag) return;
      if (e.shiftKey) {
        this.drag.v = clamp(this.drag.v + (this.drag.y - e.clientY) * 0.002, 0, 1);
        this.drag.y = e.clientY;
        this.set(this.drag.v, true);
      } else {
        this.drag.y = e.clientY;
        this.drag.v = fromY(e.clientY);
        this.set(this.drag.v, true);
      }
    });
    const end = () => { if (!this.drag) return; this.drag = null; if (this.onGesture) this.onGesture(false); };
    svg.addEventListener('pointerup', end);
    svg.addEventListener('pointercancel', end);
    svg.addEventListener('dblclick', () => this.set(0, true));
    svg.addEventListener('wheel', (e) => { e.preventDefault(); this.set(this.value + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 1 / 127 : 8 / 127), true); }, { passive: false });
    svg.addEventListener('keydown', (e) => {
      const dir = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
      if (dir) { e.preventDefault(); e.stopPropagation(); this.set(this.value + dir * (e.shiftKey ? 1 / 127 : 8 / 127), true); }
    });
  }
}

// ---------------------------------------------------------------------------
// Rocker: the organ's tablet switch, for a bool or a two-choice parameter. The tab
// tilts (CSS 3D, animated) toward the selected half; click toggles, or click a half.
// labels: [first, second] (bool: [off, on]); title: printed on the tab.
class Rocker {
  constructor(def, value, onChange, title, labels, tone) {
    this.def = def;
    this.onChange = onChange;
    this.labels = labels || (def.type === 'bool' ? ['OFF', 'ON'] : def.choices);
    const el = document.createElement('button');
    el.type = 'button';
    el.className = 'rocker' + (tone ? ' ' + tone : '');
    el.setAttribute('aria-label', def.name);
    el.innerHTML = `<span class="rocker-tab"><span class="rk-top">${this.labels[1]}</span>`
      + `<span class="rk-title">${title || def.name}</span><span class="rk-bot">${this.labels[0]}</span></span>`;
    el.addEventListener('click', (e) => {
      const r = el.getBoundingClientRect();
      const half = e.clientY < r.top + r.height * 0.38 ? 1 : (e.clientY > r.bottom - r.height * 0.38 ? 0 : (this.value >= 0.5 ? 0 : 1));
      if (half !== (this.value >= 0.5 ? 1 : 0)) this.onChange(half);
    });
    el.addEventListener('dblclick', (e) => e.preventDefault());
    this.el = el;
    this.setValue(value);
  }
  setValue(v) {
    this.value = v;
    const on = v >= 0.5;
    this.el.classList.toggle('on', on);
    this.el.setAttribute('aria-pressed', on);
    this.el.title = `${this.def.name}: ${formatParam(this.def, v)}`;
  }
}

// ---------------------------------------------------------------------------
// ExpressionPedal: a swell shoe seen from the side. Toe down = open (1). Drag up/down,
// wheel, double-click = open. Follows the engine's value (CC11) with a short animation.
class ExpressionPedal {
  constructor(onChange, onGesture) {
    this.onChange = onChange;
    this.onGesture = onGesture || null;
    this.value = 1;
    this.drag = null;
    const el = document.createElement('div');
    el.className = 'expr-pedal';
    el.innerHTML = `
      <svg viewBox="0 0 70 54" tabindex="0" role="slider" aria-label="Expression pedal" aria-valuemin="0" aria-valuemax="127">
        <rect class="ep-base" x="4" y="42" width="62" height="8" rx="3"/>
        <rect class="ep-hinge" x="8" y="34" width="8" height="10" rx="2"/>
        <g class="ep-shoe"><rect class="ep-plate" x="8" y="28" width="58" height="9" rx="3"/>
          <line class="ep-tread" x1="22" x2="62" y1="31" y2="31"/><line class="ep-tread" x1="22" x2="62" y1="34" y2="34"/></g>
      </svg>
      <span class="ctl-label">EXPR</span><span class="wheel-value">127</span>`;
    this.el = el;
    this.svg = el.querySelector('svg');
    this.shoe = el.querySelector('.ep-shoe');
    this.readout = el.querySelector('.wheel-value');
    this.bind();
    this.render();
  }
  set(v, notify) {
    v = clamp(v, 0, 1);
    if (v === this.value) return;
    this.value = v;
    this.render();
    if (notify) this.onChange(v);
  }
  setValue(v) { if (!this.drag) this.set(v, false); }
  render() {
    // closed: toe up (-17 deg); open: toe down (+4 deg), hinged at the heel
    this.shoe.style.transform = `rotate(${(-17 + 21 * this.value).toFixed(1)}deg)`;
    this.readout.textContent = Math.round(this.value * 127);
    this.svg.setAttribute('aria-valuenow', String(Math.round(this.value * 127)));
  }
  bind() {
    const svg = this.svg;
    svg.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      svg.setPointerCapture(e.pointerId);
      this.drag = { y: e.clientY, v: this.value };
      if (this.onGesture) this.onGesture(true);
    });
    svg.addEventListener('pointermove', (e) => {
      if (!this.drag) return;
      this.set(this.drag.v + (e.clientY - this.drag.y) * 0.01, true);   // push the toe down = open
    });
    const end = () => { if (!this.drag) return; this.drag = null; if (this.onGesture) this.onGesture(false); };
    svg.addEventListener('pointerup', end);
    svg.addEventListener('pointercancel', end);
    svg.addEventListener('dblclick', () => this.set(1, true));
    svg.addEventListener('wheel', (e) => { e.preventDefault(); this.set(this.value + (e.deltaY > 0 ? 1 : -1) * (e.shiftKey ? 1 / 127 : 8 / 127), true); }, { passive: false });
  }
}
