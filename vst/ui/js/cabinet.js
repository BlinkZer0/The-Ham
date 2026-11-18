// ============================================================================
//  CabinetView: the animated cutaway of the four cabinets.
//   - the rotor angles come from the engine (`rotor` events, 30 Hz, angle in turns,
//     0 = facing the front); every frame draws the angle extrapolated from the latest
//     report with its speed (no lag at a steady speed). When a new report disagrees with
//     the extrapolation, the difference becomes an offset that fades out over ~80 ms,
//     so the drawing never jumps
//   - a horn is drawn on an elliptical orbit (a front view of a vertical axis): its
//     mouth faces you at angle 0; the drum shows its slats and scoop opening sliding by
//   - at speed, ghost copies of the horn trail it (motion blur)
//   - switching cabinets cross-fades the drawings; the layout options show: horn only /
//     drum only / stationary, dual active horns, open back, mic stands per mic layout
//   - it costs nothing when nothing moves: the loop stops when the window is hidden, the
//     page isn't shown, or the rotors stand still and the drawing has caught up
// ============================================================================
'use strict';

const CAB_W = 280, CAB_H = 330;
const SVGNS = 'http://www.w3.org/2000/svg';

// Geometry per cabinet model (front view, viewBox 280 x 330).
const CAB_GEOMS = [
  { name: 'ROTOR 22', body: [50, 10, 180, 312], hornWin: [60, 46, 160, 98], hornC: [140, 98, 62, 13], amp: 160,
    drumWin: [60, 190, 160, 122], drumC: [140, 252, 62, 70], horn: true, drum: true },
  { name: 'ROTOR 45', body: [44, 70, 192, 252], hornWin: [54, 100, 172, 80], hornC: [140, 142, 66, 12], amp: 192,
    drumWin: [54, 214, 172, 98], drumC: [140, 264, 66, 58], horn: true, drum: true },
  { name: 'SINGLE ROTOR', body: [55, 110, 170, 212], hornWin: null, hornC: null, amp: 136,
    drumWin: [65, 160, 150, 150], drumC: [140, 236, 66, 100], horn: false, drum: true, foam: true },
  { name: 'TONE CABINET', body: [60, 10, 160, 312], hornWin: null, hornC: null, amp: 272,
    drumWin: null, drumC: null, horn: false, drum: false, tone: true },
];

function svgEl(tag, attrs, parent) {
  const e = document.createElementNS(SVGNS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (parent) parent.appendChild(e);
  return e;
}

class CabinetView {
  constructor(container) {
    this.model = 0;
    this.prevModel = -1;
    this.opts = { rotorConfig: 0, hornType: 0, cabBack: 0, micLayout: 1, micAngle: 90, micDist: 0.4, hornDir: 0, drumDir: 1, drumBaffle: 0 };
    this.rotor = { hornAngle: 0, drumAngle: 0, hornHz: 0, drumHz: 0, t: performance.now() };
    this.shown = { horn: 0, drum: 0 };     // drawn angles (turns)
    this.offset = { horn: 0, drum: 0 };    // correction fading out after each report
    this.lastFrame = 0;
    this.running = false;
    this.visible = true;                   // set by the page
    this.frames = 0;                       // for the tests / CPU measurement
    this.busyMs = 0;

    const svg = svgEl('svg', { class: 'cab-svg', viewBox: `18 0 ${CAB_W - 36} ${CAB_H}`, role: 'img', 'aria-label': 'Rotary cabinet' });
    svg.innerHTML = `
      <defs>
        <linearGradient id="cabWood" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0%" stop-color="#3b200f"/><stop offset="45%" stop-color="#5a3219"/><stop offset="100%" stop-color="#2e180b"/>
        </linearGradient>
        <linearGradient id="cabInside" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#0d0705"/><stop offset="100%" stop-color="#1a0f0a"/>
        </linearGradient>
        <linearGradient id="cabOpen" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#3a2c22"/><stop offset="100%" stop-color="#241812"/>
        </linearGradient>
        <linearGradient id="hornMetal" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#f3efe6"/><stop offset="50%" stop-color="#b9b3a6"/><stop offset="100%" stop-color="#6f6a61"/>
        </linearGradient>
        <linearGradient id="drumFace" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0%" stop-color="#241710"/><stop offset="50%" stop-color="#6b4a33"/><stop offset="100%" stop-color="#241710"/>
        </linearGradient>
        <pattern id="foamDots" width="5" height="5" patternUnits="userSpaceOnUse">
          <rect width="5" height="5" fill="#4a3a2c"/><circle cx="2" cy="2" r="1" fill="#5d4a39"/>
        </pattern>
      </defs>`;
    this.svg = svg;
    this.layers = CAB_GEOMS.map((g, i) => this.buildLayer(g, i));
    this.mics = svgEl('g', { class: 'cab-mics' }, svg);
    this.label = svgEl('text', { class: 'cab-label', x: CAB_W / 2, y: CAB_H - 2 }, svg);
    container.appendChild(svg);

    document.addEventListener('visibilitychange', () => this.kick());
    this.setModel(0, true);
    this.applyOptions();
  }

  // ---------------------------------------------------------------- build ----
  buildLayer(g, index) {
    const L = svgEl('g', { class: 'cab-layer', 'data-model': index }, this.svg);
    const [bx, by, bw, bh] = g.body;
    svgEl('rect', { class: 'cab-body', x: bx, y: by, width: bw, height: bh, rx: 6 }, L);
    // louvres on top
    for (let i = 0; i < 4; i++) svgEl('line', { class: 'cab-louvre', x1: bx + 14, x2: bx + bw - 14, y1: by + 8 + i * 6, y2: by + 8 + i * 6 }, L);
    const layer = { g: L, geom: g, horns: [], ghosts: [], slats: [] };
    const win = (r) => svgEl('rect', { class: 'cab-win', x: r[0], y: r[1], width: r[2], height: r[3], rx: 3 }, L);
    if (g.hornWin) layer.hornWin = win(g.hornWin);
    if (g.drumWin) layer.drumWin = win(g.drumWin);

    // amp chassis with glowing tubes
    const amp = svgEl('g', { class: 'cab-amp' }, L);
    svgEl('rect', { x: bx + 20, y: g.amp, width: bw - 40, height: 18, rx: 2, class: 'cab-amp-box' }, amp);
    layer.tubes = [0, 1, 2, 3].map(i => svgEl('circle', { class: 'cab-tube', cx: bx + 40 + i * ((bw - 80) / 3), cy: g.amp + 9, r: 4 }, amp));

    if (g.horn) {
      const [cx, cy] = g.hornC;
      layer.hornGroup = svgEl('g', { class: 'horn-assy' }, L);
      for (let i = 0; i < 3; i++) layer.ghosts.push(svgEl('path', { class: 'horn-ghost' }, layer.hornGroup));
      for (let i = 0; i < 2; i++) {
        const body = svgEl('path', { class: 'horn-body' }, layer.hornGroup);
        const mouth = svgEl('ellipse', { class: 'horn-mouth' }, layer.hornGroup);
        layer.horns.push({ body, mouth });
      }
      svgEl('circle', { class: 'horn-hub', cx, cy, r: 6 }, layer.hornGroup);
      svgEl('rect', { class: 'horn-driver', x: cx - 9, y: cy + 8, width: 18, height: 22, rx: 2 }, layer.hornGroup);
    }
    if (g.drum) {
      const [cx, cy, rx, h] = g.drumC;
      layer.drumGroup = svgEl('g', { class: 'drum-assy' }, L);
      layer.drumFace = svgEl('rect', { class: g.foam ? 'drum-face foam' : 'drum-face', x: cx - rx, y: cy - h / 2, width: 2 * rx, height: h }, layer.drumGroup);
      for (let i = 0; i < 12; i++) layer.slats.push(svgEl('line', { class: 'drum-slat', y1: cy - h / 2 + 3, y2: cy + h / 2 - 3 }, layer.drumGroup));
      layer.scoop = svgEl('rect', { class: 'drum-scoop', y: cy - h / 2 + 6, height: h - 12, rx: 3 }, layer.drumGroup);
      svgEl('ellipse', { class: 'drum-rim', cx, cy: cy - h / 2, rx, ry: 7 }, layer.drumGroup);
      svgEl('ellipse', { class: 'drum-rim bottom', cx, cy: cy + h / 2, rx, ry: 7 }, layer.drumGroup);
    }
    if (g.tone) {
      // fixed speakers behind a grille, a reverb tank below
      svgEl('rect', { class: 'tone-grille', x: bx + 14, y: by + 40, width: bw - 28, height: 200, rx: 4 }, L);
      [[140, 90, 30], [140, 175, 30], [100, 225, 14], [180, 225, 14]].forEach(([x, y, r]) => {
        svgEl('circle', { class: 'tone-cone', cx: x, cy: y, r }, L);
        svgEl('circle', { class: 'tone-dust', cx: x, cy: y, r: r * 0.3 }, L);
      });
      svgEl('rect', { class: 'tone-tank', x: bx + 22, y: 250, width: bw - 44, height: 16, rx: 2 }, L);
      let d = '';
      for (let i = 0; i <= 24; i++) d += `${i ? 'L' : 'M'}${bx + 28 + i * ((bw - 56) / 24)} ${i % 2 ? 254 : 262}`;
      svgEl('path', { class: 'tone-spring', d }, L);
    }
    return layer;
  }

  // ---------------------------------------------------------- inputs ----
  setModel(i, now) {
    i = clamp(Math.round(i), 0, 3);
    if (i === this.model && !now) return;
    this.prevModel = now ? -1 : this.model;
    this.model = i;
    this.layers.forEach((l, k) => l.g.classList.toggle('active', k === i));
    this.fadeUntil = performance.now() + 400;
    this.applyOptions();
    this.kick();
  }

  setOptions(o) { Object.assign(this.opts, o); this.applyOptions(); this.kick(); }

  // a rotor report from the engine
  setRotor(r) {
    const now = performance.now();
    const before = this.predict (now);
    this.rotor = { hornAngle: r.hornAngle, drumAngle: r.drumAngle, hornHz: r.hornHz, drumHz: r.drumHz, t: now };
    const after = this.predict (now);
    for (const k of ['horn', 'drum']) {
      let d = before[k] + this.offset[k] - after[k];
      d -= Math.round(d);                      // shortest way round
      this.offset[k] = d;
    }
    this.kick();
  }

  // the angle (turns) the last report implies at time t
  predict(t) {
    const r = this.rotor, o = this.opts;
    const dt = Math.min(Math.max(0, (t - r.t) / 1000), 0.25);
    return { horn: r.hornAngle + (o.hornDir === 0 ? 1 : -1) * r.hornHz * dt,
             drum: r.drumAngle + (o.drumDir === 0 ? 1 : -1) * r.drumHz * dt };
  }

  setVisible(v) { this.visible = v; this.kick(); }

  applyOptions() {
    const o = this.opts;
    this.layers.forEach((l) => {
      const g = l.geom;
      const showHorn = g.horn && o.rotorConfig !== 2, showDrum = g.drum && o.rotorConfig !== 1;
      if (l.hornGroup) l.hornGroup.style.display = showHorn ? '' : 'none';
      if (l.drumGroup) l.drumGroup.style.display = showDrum ? '' : 'none';
      [l.hornWin, l.drumWin].forEach(w => w && w.classList.toggle('open', o.cabBack === 1));
      if (l.drumFace) l.drumFace.classList.toggle('open-baffle', o.drumBaffle === 1);
    });
    this.label.textContent = `${CAB_GEOMS[this.model].name}${o.cabBack === 1 ? ' · OPEN BACK' : ''}${o.rotorConfig === 3 && this.model < 3 ? ' · STATIONARY' : ''}`;
    this.drawMics();
    this.drawRotors(true);
  }

  // mic stands around the cabinet: left / right by the mic angle, further out with distance
  drawMics() {
    const o = this.opts, g = CAB_GEOMS[this.model];
    this.mics.innerHTML = '';
    const hornY = g.hornC ? g.hornC[1] : (g.drumC ? g.drumC[1] - 30 : 90);
    const drumY = g.drumC ? g.drumC[1] : 200;
    const half = (o.micAngle / 2) * Math.PI / 180;
    const side = (d) => 40 + 68 * Math.sin(half) + 24 * o.micDist + d;
    const mic = (x, y, label, far) => {
      const m = svgEl('g', { class: 'mic' + (far ? ' far' : '') }, this.mics);
      svgEl('line', { class: 'mic-stand', x1: x, y1: y + 6, x2: x, y2: CAB_H - 12 }, m);
      svgEl('rect', { class: 'mic-head', x: x - 3.5, y: y - 6, width: 7, height: 13, rx: 3.5 }, m);
      if (label) svgEl('text', { class: 'mic-label', x, y: y - 9 }, m).textContent = label;
    };
    const cx = CAB_W / 2;
    switch (o.micLayout) {
      case 0: mic(cx + 30, hornY, 'M'); if (g.drum) mic(cx + 30, drumY, ''); break;
      case 2: mic(cx - side(0), hornY, 'L'); mic(cx + side(0), hornY, 'R'); if (g.drum) { mic(cx - side(0), drumY, 'L'); mic(cx + side(0), drumY, 'R'); } break;
      case 3: mic(cx - side(0), hornY, 'L'); mic(cx + side(0), hornY, 'R'); if (g.drum) mic(cx + 34, drumY + 10, 'D'); break;
      case 4: mic(cx - 112, hornY - 30, 'F', true); mic(cx + 112, hornY - 30, 'B', true); break;
      default: mic(cx - side(0), hornY, 'L'); mic(cx + side(0), hornY, 'R'); if (g.drum) mic(cx + 30, drumY, 'D'); break;
    }
  }

  // ---------------------------------------------------------- animation ----
  kick() {
    if (this.running || document.hidden || !this.visible) return;
    this.running = true;
    this.lastFrame = 0;
    requestAnimationFrame((t) => this.frame(t));
  }

  frame(t) {
    if (document.hidden || !this.visible) { this.running = false; return; }
    const t0 = performance.now();
    const moving = this.advance(t);
    this.drawRotors(false);
    this.frames++;
    this.busyMs += performance.now() - t0;
    if (moving || t < this.fadeUntil) requestAnimationFrame((tt) => this.frame(tt));
    else this.running = false;
  }

  // extrapolated angle + the fading correction
  advance(t) {
    const r = this.rotor;
    const p = this.predict(t);
    const fade = Math.exp(-Math.max(0, t - (this.lastFrame || t)) / 80);
    this.lastFrame = t;
    let moving = r.hornHz > 0 || r.drumHz > 0;
    for (const k of ['horn', 'drum']) {
      this.offset[k] *= fade;
      if (Math.abs(this.offset[k]) < 1e-4) this.offset[k] = 0; else moving = true;
      const a = p[k] + this.offset[k];
      this.shown[k] = a - Math.floor(a);
    }
    this.frameTime = t;
    return moving;
  }

  hornPath(cx, cy, rx, ry, a) {
    // a in radians; 0 = mouth toward the viewer. Projected tip, mouth height by facing
    const sx = Math.sin(a), z = Math.cos(a);
    const tx = cx + rx * sx, ty = cy + ry * z * 0.6;
    const hw = 3, mh = 9 + 2 * Math.max(0, z);
    return { d: `M${cx.toFixed(1)} ${(cy - hw).toFixed(1)} L${tx.toFixed(1)} ${(ty - mh).toFixed(1)} L${tx.toFixed(1)} ${(ty + mh).toFixed(1)} L${cx.toFixed(1)} ${(cy + hw).toFixed(1)} Z`,
             tx, ty, z, mh };
  }

  drawRotors(force) {
    const o = this.opts;
    const stationary = o.rotorConfig === 3;
    const hornA = (stationary ? 0 : this.shown.horn) * 2 * Math.PI, drumA = (stationary ? 0 : this.shown.drum) * 2 * Math.PI;
    const active = [this.model];
    if (this.prevModel >= 0 && performance.now() < this.fadeUntil) active.push(this.prevModel);
    const hz = this.rotor.hornHz;
    for (const m of active) {
      const l = this.layers[m], g = l.geom;
      if (g.horn && l.hornGroup.style.display !== 'none') {
        const [cx, cy, rx, ry] = g.hornC;
        // back horn first: draw order by facing
        const pair = [0, 1].map(i => ({ i, p: this.hornPath(cx, cy, rx, ry, hornA + i * Math.PI) }));
        pair.sort((a, b) => a.p.z - b.p.z);
        pair.forEach(({ i, p }, order) => {
          const h = l.horns[order];
          h.body.setAttribute('d', p.d);
          const dummy = i === 1 && o.hornType === 0;
          h.body.classList.toggle('dummy', dummy);
          h.mouth.classList.toggle('dummy', dummy);
          h.mouth.setAttribute('cx', p.tx.toFixed(1));
          h.mouth.setAttribute('cy', p.ty.toFixed(1));
          h.mouth.setAttribute('rx', (1.5 + 8 * Math.max(0, p.z)).toFixed(1));
          h.mouth.setAttribute('ry', p.mh.toFixed(1));
          h.mouth.style.opacity = p.z > -0.1 ? '1' : '0';
        });
        l.hornGroup.dataset.angle = (hornA / (2 * Math.PI)).toFixed(4);   // drawn angle (turns), for the tests
        // motion blur: ghosts trailing the active horn at speed
        const blur = stationary ? 0 : clamp((hz - 1.5) / 5, 0, 1);
        l.ghosts.forEach((gh, k) => {
          if (blur <= 0) { gh.style.display = 'none'; return; }
          gh.style.display = '';
          const back = (o.hornDir === 0 ? 1 : -1) * hz * 2 * Math.PI * 0.012 * (k + 1);
          gh.setAttribute('d', this.hornPath(cx, cy, rx, ry, hornA - back).d);
          gh.style.opacity = (blur * (0.32 - k * 0.09)).toFixed(3);
        });
      }
      if (g.drum && l.drumGroup.style.display !== 'none') {
        const [cx, , rx] = g.drumC;
        l.slats.forEach((s, k) => {
          const a = drumA + k * Math.PI / 6;
          const z = Math.cos(a), x = cx + rx * Math.sin(a);
          s.setAttribute('x1', x.toFixed(1)); s.setAttribute('x2', x.toFixed(1));
          s.style.opacity = z > 0 ? (0.25 + 0.6 * z).toFixed(2) : '0';
        });
        const z = Math.cos(drumA), x = cx + rx * Math.sin(drumA);
        const w = 6 + 44 * Math.max(0, z);
        l.scoop.setAttribute('x', (x - w / 2).toFixed(1));
        l.scoop.setAttribute('width', w.toFixed(1));
        l.scoop.style.opacity = (0.15 + 0.85 * Math.max(0, z)).toFixed(2);
        l.drumGroup.dataset.angle = (drumA / (2 * Math.PI)).toFixed(4);
      }
    }
  }

  setDrive(d) { this.layers.forEach(l => l.tubes.forEach(t => { t.style.opacity = (0.45 + 0.55 * d).toFixed(2); })); }
}
