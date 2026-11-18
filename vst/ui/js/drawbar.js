// ============================================================================
//  Drawbar: a bar that pulls DOWN out of the panel, 9 detents (0..8).
//  The shaft is numbered 1..8; the number at the panel lip is the setting.
//  Tip colour by footage: brown (16', 5 1/3'), white (8', 4', 2', 1'),
//  black (2 2/3', 1 3/5', 1 1/3').
//  Same conventions as Knob: drag (snaps to detents; Shift = continuous, for
//  automation), mouse wheel / arrow keys (Shift = fine), double-click = default,
//  gesture begin/end around drags. setValue() from the host animates (~150 ms).
//
//  DrawbarSet: 9 drawbars + an editable registration string ("88 8000 000").
// ============================================================================
'use strict';

const DB_W = 30, DB_STEP = 21, DB_SLOT = 24, DB_TIP = 30;
const DB_H = DB_SLOT + 8 * DB_STEP + DB_TIP + 6;
const DB_ANIM_MS = 150;

class Drawbar {
  constructor(def, value, tip, label, onChange, onGesture) {
    this.def = def;
    this.onChange = onChange;
    this.onGesture = onGesture || null;
    this.value = value;     // logical value (what the host has)
    this.shown = value;     // drawn value (animates towards value)
    this.drag = null;
    this.anim = null;

    const nums = [];
    for (let k = 1; k <= 8; k++) {
      const y = -(k - 0.5) * DB_STEP;
      nums.push(`<line class="db-tick" x1="9" x2="21" y1="${(-(k) * DB_STEP).toFixed(1)}" y2="${(-(k) * DB_STEP).toFixed(1)}"/>`);
      nums.push(`<text class="db-num" x="${DB_W / 2}" y="${(y + 4).toFixed(1)}">${k}</text>`);
    }
    const uid = `db${Drawbar.next = (Drawbar.next || 0) + 1}`;
    const el = document.createElement('div');
    el.className = 'drawbar';
    el.innerHTML = `
      <svg class="db-svg" viewBox="0 0 ${DB_W} ${DB_H}" tabindex="0" role="slider"
           aria-label="${def.name}" aria-valuemin="0" aria-valuemax="8">
        <defs><clipPath id="${uid}-clip"><rect x="0" y="${DB_SLOT}" width="${DB_W}" height="${DB_H}"/></clipPath></defs>
        <g clip-path="url(#${uid}-clip)">
          <g class="db-shaft">
            <rect class="db-body" x="8" y="${-8 * DB_STEP - 4}" width="14" height="${8 * DB_STEP + 6}" rx="1"/>
            ${nums.join('')}
          </g>
        </g>
        <g class="db-tipg">
          <rect class="db-tip tip-${tip}" x="4" y="0" width="22" height="${DB_TIP}" rx="5"/>
          <rect class="db-tip-shine" x="6.5" y="3" width="17" height="7" rx="3"/>
          <rect class="db-tip-ring" x="4" y="0" width="22" height="4" rx="2"/>
        </g>
        <rect class="db-slot" x="5" y="${DB_SLOT - 6}" width="20" height="7" rx="2"/>
        <text class="db-foot" x="${DB_W / 2}" y="11">${label}</text>
      </svg>`;
    this.el = el;
    this.svg = el.querySelector('svg');
    this.shaft = el.querySelector('.db-shaft');
    this.tipg = el.querySelector('.db-tipg');
    this.bindEvents();
    this.draw();
  }

  // from the host / a preset: animate to the new position
  setValue(v) {
    v = clamp(v, 0, 8);
    if (v === this.value && !this.anim) return;
    this.value = v;
    this.label();
    if (this.drag) return;
    if (Math.abs(v - this.shown) < 0.01 || document.hidden) { this.stopAnim(); this.shown = v; this.draw(); return; }
    this.animateTo(v);
  }

  animateTo(v) {
    this.stopAnim();
    const from = this.shown, t0 = performance.now();
    const step = (t) => {
      const x = clamp((t - t0) / DB_ANIM_MS, 0, 1);
      const e = 1 - (1 - x) * (1 - x) * (1 - x);   // ease out
      this.shown = from + (v - from) * e;
      this.draw();
      this.anim = x < 1 ? requestAnimationFrame(step) : null;
    };
    this.anim = requestAnimationFrame(step);
  }
  stopAnim() { if (this.anim) cancelAnimationFrame(this.anim); this.anim = null; }

  // a change made here: draw at once, tell the host
  set(v, notify) {
    v = clamp(v, 0, 8);
    this.stopAnim();
    const changed = v !== this.value;
    this.value = v;
    this.shown = v;
    this.draw();
    if (changed && notify) this.onChange(v);
  }

  draw() {
    const len = this.shown * DB_STEP;
    this.shaft.setAttribute('transform', `translate(0 ${(DB_SLOT + len).toFixed(2)})`);
    this.tipg.setAttribute('transform', `translate(0 ${(DB_SLOT - 2 + len).toFixed(2)})`);
    this.label();
  }

  // accessible value + tooltip: the logical value, at once (the picture may still be animating)
  label() {
    const txt = formatParam(this.def, this.value);
    this.svg.setAttribute('aria-valuenow', String(this.value));
    this.svg.setAttribute('aria-valuetext', txt);
    this.el.title = `${this.def.name}: ${txt}`;
  }

  nudge(dir, fine) {
    const v = fine ? Math.round((this.value + dir * 0.1) * 10) / 10 : Math.round(this.value) + dir;
    this.set(v, true);
  }

  bindEvents() {
    const svg = this.svg;
    svg.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      svg.setPointerCapture(e.pointerId);
      svg.focus();
      const scale = svg.getBoundingClientRect().height / DB_H;
      this.drag = { y: e.clientY, start: this.value, scale };
      this.el.classList.add('active');
      if (this.onGesture) this.onGesture(true);
    });
    svg.addEventListener('pointermove', (e) => {
      if (!this.drag) return;
      const raw = this.drag.start + (e.clientY - this.drag.y) / (this.drag.scale * DB_STEP);
      const v = e.shiftKey ? Math.round(clamp(raw, 0, 8) * 100) / 100 : Math.round(clamp(raw, 0, 8));
      this.set(v, true);
    });
    const end = () => {
      if (!this.drag) return;
      this.drag = null;
      this.el.classList.remove('active');
      if (this.onGesture) this.onGesture(false);
    };
    svg.addEventListener('pointerup', end);
    svg.addEventListener('pointercancel', end);
    svg.addEventListener('dblclick', () => this.set(this.def.def, true));
    svg.addEventListener('wheel', (e) => {
      e.preventDefault();
      this.nudge(e.deltaY > 0 ? 1 : -1, e.shiftKey);   // scroll down = pull out
    }, { passive: false });
    svg.addEventListener('keydown', (e) => {
      const dir = { ArrowDown: 1, ArrowRight: 1, ArrowUp: -1, ArrowLeft: -1 }[e.key];
      if (dir) { e.preventDefault(); e.stopPropagation(); this.nudge(dir, e.shiftKey); }
      else if (/^[0-8]$/.test(e.key)) { e.preventDefault(); e.stopPropagation(); this.set(Number(e.key), true); }
    });
  }
}

// 9 drawbars with a title and an editable registration string.
class DrawbarSet {
  // ids: 9 param ids; onSet(id, v) applies one bar; gesture(id, begin)
  constructor(title, ids, values, onSet, gesture, opts) {
    opts = opts || {};
    this.ids = ids;
    this.onSet = onSet;
    const el = document.createElement('div');
    el.className = 'db-set' + (opts.small ? ' small' : '');
    el.innerHTML = `
      <div class="db-set-head">
        <span class="db-set-title">${title}</span>
        <input class="reg-input" type="text" spellcheck="false" autocomplete="off" maxlength="13"
               title="Registration: type nine digits 0-8, e.g. 888000000, then Enter">
      </div>
      <div class="db-row"></div>`;
    this.el = el;
    this.input = el.querySelector('.reg-input');
    const row = el.querySelector('.db-row');
    this.bars = ids.map((id, i) => {
      const foot = opts.feet ? opts.feet[i] : i;
      const b = new Drawbar(PARAM_BY_ID[id], values[id], FOOTAGE_TIPS[foot], FOOTAGE_SHORT[foot],
        (v) => { onSet(id, v); this.renderString(); }, (begin) => gesture(id, begin));
      b.el.dataset.param = id;
      row.appendChild(b.el);
      return b;
    });
    if (opts.noString) this.input.style.display = 'none';
    const commit = () => {
      const regs = parseRegistration(this.input.value);
      if (!regs) { this.input.classList.add('bad'); return; }
      this.input.classList.remove('bad');
      regs.slice(0, ids.length).forEach((v, i) => {
        if (v !== this.bars[i].value) { this.bars[i].animateTo(v); this.bars[i].value = v; onSet(ids[i], v); }
      });
      this.renderString();
      this.input.blur();
    };
    this.input.addEventListener('keydown', (e) => {
      e.stopPropagation();   // typing digits must not play notes
      if (e.key === 'Enter') { e.preventDefault(); commit(); }
      else if (e.key === 'Escape') { e.preventDefault(); this.renderString(); this.input.blur(); }
    });
    this.input.addEventListener('keyup', (e) => e.stopPropagation());
    this.input.addEventListener('change', commit);
    this.input.addEventListener('focus', () => this.input.select());
    this.renderString();
  }

  setActive(on) { this.el.classList.toggle('active', !!on); }

  renderString() {
    if (document.activeElement === this.input) return;
    const vals = this.bars.map(b => b.value);
    this.input.value = this.ids.length === 9 ? registrationString(vals) : vals.map(v => Math.round(v)).join(' ');
    this.input.classList.remove('bad');
  }
}
