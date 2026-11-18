// ============================================================================
//  SVG rotary knob.
//  Drag up/down (Shift = fine), mouse wheel, arrow keys, double-click = default.
//  270° sweep. Bipolar params draw their arc from 12 o'clock.
// ============================================================================
'use strict';

const KNOB_SWEEP = 135; // degrees either side of 12 o'clock

function svgPolar(cx, cy, r, deg) {
  const a = (deg - 90) * Math.PI / 180;
  return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
}

function svgArc(cx, cy, r, a0, a1) {
  if (Math.abs(a1 - a0) < 0.5) return '';
  const lo = Math.min(a0, a1), hi = Math.max(a0, a1);
  const [x0, y0] = svgPolar(cx, cy, r, lo);
  const [x1, y1] = svgPolar(cx, cy, r, hi);
  const large = hi - lo > 180 ? 1 : 0;
  return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}

class Knob {
  // onGesture(true/false) brackets a drag so the DAW records it as one automation gesture.
  constructor(def, value, onChange, onGesture) {
    this.def = def;
    this.onChange = onChange;
    this.onGesture = onGesture || null;
    this.value = value;
    this.drag = null;

    const el = document.createElement('div');
    el.className = 'knob';
    let ticks = '';
    if (def.type === 'choice') {
      const steps = def.max - def.min;
      for (let i = 0; i <= steps; i++) {
        const [x, y] = svgPolar(32, 32, 31, -KNOB_SWEEP + (2 * KNOB_SWEEP * i) / steps);
        ticks += `<circle class="k-tick" cx="${x.toFixed(2)}" cy="${y.toFixed(2)}" r="1.4"/>`;
      }
    }
    el.innerHTML = `
      <svg class="knob-svg" viewBox="0 0 64 64" tabindex="0" role="slider"
           aria-label="${def.name}" aria-valuemin="${def.min}" aria-valuemax="${def.max}">
        <path class="k-track" d="${svgArc(32, 32, 26, -KNOB_SWEEP, KNOB_SWEEP)}"/>
        ${ticks}
        <path class="k-value" d=""/>
        <circle class="k-face" cx="32" cy="32" r="19"/>
        <g class="k-rot"><line class="k-pointer" x1="32" y1="24" x2="32" y2="15"/></g>
      </svg>
      <div class="knob-value"></div>
      <div class="knob-label">${def.name}</div>`;
    this.el = el;
    this.svg = el.querySelector('svg');
    this.valuePath = el.querySelector('.k-value');
    this.rot = el.querySelector('.k-rot');
    this.readout = el.querySelector('.knob-value');
    this.bindEvents();
    this.render();
  }

  get norm() { return paramToNorm(this.def, this.value); }

  setValue(v, notify = false) {
    v = clamp(v, this.def.min, this.def.max);
    if (v === this.value) return;
    this.value = v;
    this.render();
    if (notify && this.onChange) this.onChange(v);
  }

  setNorm(n) { this.setValue(paramFromNorm(this.def, n), true); }

  nudge(dir, fine) {
    const d = this.def;
    if (d.step) this.setValue(this.value + dir * d.step, true);
    else this.setNorm(this.norm + dir * (fine ? 0.002 : 0.01));
  }

  render() {
    const d = this.def;
    const angle = -KNOB_SWEEP + this.norm * 2 * KNOB_SWEEP;
    const startAngle = d.bipolar ? 0 : -KNOB_SWEEP;
    this.valuePath.setAttribute('d', svgArc(32, 32, 26, startAngle, angle));
    this.rot.setAttribute('transform', `rotate(${angle.toFixed(2)} 32 32)`);
    this.readout.textContent = formatParam(d, this.value);
    this.svg.setAttribute('aria-valuenow', String(this.value));
    this.svg.setAttribute('aria-valuetext', formatParam(d, this.value));
  }

  bindEvents() {
    const svg = this.svg;
    svg.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      svg.setPointerCapture(e.pointerId);
      svg.focus();
      this.drag = { y: e.clientY, norm: this.norm };
      this.el.classList.add('active');
      if (this.onGesture) this.onGesture(true);
    });
    svg.addEventListener('pointermove', (e) => {
      if (!this.drag) return;
      const dy = this.drag.y - e.clientY;
      this.drag.y = e.clientY;
      this.drag.norm = clamp(this.drag.norm + dy * (e.shiftKey ? 0.001 : 0.005), 0, 1);
      this.setNorm(this.drag.norm);
    });
    const end = () => {
      if (!this.drag) return;
      this.drag = null;
      this.el.classList.remove('active');
      if (this.onGesture) this.onGesture(false);
    };
    svg.addEventListener('pointerup', end);
    svg.addEventListener('pointercancel', end);
    svg.addEventListener('dblclick', () => this.setValue(this.def.def, true));
    svg.addEventListener('wheel', (e) => {
      e.preventDefault();
      this.nudge(e.deltaY < 0 ? 1 : -1, e.shiftKey);
    }, { passive: false });
    svg.addEventListener('keydown', (e) => {
      const dir = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
      if (dir) { e.preventDefault(); e.stopPropagation(); this.nudge(dir, e.shiftKey); }
    });
  }
}
