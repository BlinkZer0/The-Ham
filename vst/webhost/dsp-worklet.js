// ============================================================================
//  The Ham DSP engine: JavaScript twin of vst/Source/SynthEngine.h and
//  vst/Source/dsp/*.h (plus EngineParamsFromValues.h and MidiRouting.h). Kept
//  line-for-line equivalent (same control rate, same RNG, same maths, same order
//  of operations) and checked against the C++ build by vst/tests/run-parity.ps1.
//
//  This whole function is stringified and loaded as an AudioWorklet module (or
//  run on the main thread as a ScriptProcessor fallback), so it must not
//  reference anything outside itself: only `sampleRate`, `AudioWorkletProcessor`
//  and `registerProcessor` come from the host.
//
//  Performance notes: nothing allocates on the audio path. The contact-event heap
//  is a struct of typed arrays, the ramps and pickups are preallocated objects,
//  and the per-sample wheel values live in one reused Float64Array.
// ============================================================================
function synthWorkletModule() {
  'use strict';

  // ======================================================== dsp/Common.h ===
  const kPi = 3.141592653589793, kTwoPi = 6.283185307179586;
  const kChunk = 32;   // control rate: modulation once per 32 samples, per-sample ramps in between

  const clampd = (v, lo, hi) => (v < lo ? lo : (v > hi ? hi : v));
  const clampi = (v, lo, hi) => (v < lo ? lo : (v > hi ? hi : v));
  const dbToGain = (db) => Math.pow(10, db / 20);
  const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x)); // std::lround: halves away from zero

  // Deterministic RNG (identical sequence in C++: 32-bit LCG). Same as Brass Knuckles.
  class Rng {
    constructor() { this.s = 0x2545F491; }
    seed(v) { this.s = v >>> 0; }
    next() { this.s = (Math.imul(this.s, 1664525) + 1013904223) >>> 0; return this.s; }
    uni() { return (this.next() >>> 8) * (1 / 16777216); }   // [0, 1)
    bi() { return this.uni() * 2 - 1; }                        // [-1, 1)
  }

  // Output safety clipper: exactly linear up to 0.8 (-1.9 dBFS), then a tanh knee that never exceeds 1.
  function safetyClip(x) {
    const a = Math.abs(x);
    if (a <= 0.8) return x;
    const y = 0.8 + 0.2 * Math.tanh((a - 0.8) / 0.2);
    return x < 0 ? -y : y;
  }

  // Drawbar position (0..8, fractional allowed) -> gain. Step 8 = 0 dB, about -3 dB per step, 0 = off.
  function drawbarGain(pos) {
    if (pos <= 0) return 0;
    if (pos >= 1) return Math.pow(10, (pos - 8) * 3 / 20);
    return pos * 0.08912509381337455;   // 10^(-21/20)
  }

  // Biquad, transposed direct form II; designs from the RBJ cookbook.
  class Biquad {
    constructor() { this.b0 = 1; this.b1 = 0; this.b2 = 0; this.a1 = 0; this.a2 = 0; this.s1 = 0; this.s2 = 0; }
    process(x) {
      const y = this.b0 * x + this.s1;
      this.s1 = this.b1 * x - this.a1 * y + this.s2;
      this.s2 = this.b2 * x - this.a2 * y;
      return y;
    }
    reset() { this.s1 = this.s2 = 0; }
    setAllpass(f0, q, sr) {
      const w = kTwoPi * f0 / sr, alpha = Math.sin(w) / (2 * q), cw = Math.cos(w), a0 = 1 + alpha;
      this.b0 = (1 - alpha) / a0; this.b1 = -2 * cw / a0; this.b2 = 1;
      this.a1 = -2 * cw / a0; this.a2 = (1 - alpha) / a0;
    }
    setLowpass(f0, q, sr) {
      const w = kTwoPi * f0 / sr, alpha = Math.sin(w) / (2 * q), cw = Math.cos(w), a0 = 1 + alpha;
      this.b0 = (1 - cw) / 2 / a0; this.b1 = (1 - cw) / a0; this.b2 = this.b0;
      this.a1 = -2 * cw / a0; this.a2 = (1 - alpha) / a0;
    }
    setHighpass(f0, q, sr) {
      const w = kTwoPi * f0 / sr, alpha = Math.sin(w) / (2 * q), cw = Math.cos(w), a0 = 1 + alpha;
      this.b0 = (1 + cw) / 2 / a0; this.b1 = -(1 + cw) / a0; this.b2 = this.b0;
      this.a1 = -2 * cw / a0; this.a2 = (1 - alpha) / a0;
    }
    setPeak(f0, q, db, sr) {
      const A = Math.pow(10, db / 40);
      const w = kTwoPi * f0 / sr, alpha = Math.sin(w) / (2 * q), cw = Math.cos(w), a0 = 1 + alpha / A;
      this.b0 = (1 + alpha * A) / a0; this.b1 = -2 * cw / a0; this.b2 = (1 - alpha * A) / a0;
      this.a1 = -2 * cw / a0; this.a2 = (1 - alpha / A) / a0;
    }
  }

  // One-pole low-pass: y += a (x - y), a from the corner frequency.
  const onePoleCoef = (hz, sr) => 1 - Math.exp(-kTwoPi * hz / sr);

  // ==================================================== dsp/Tonewheels.h ===
  const kNumWheels = 91;
  const kGearNum = [85, 71, 67, 105, 103, 84, 74, 98, 96, 88, 67, 108];
  const kGearDen = [104, 82, 73, 108, 100, 77, 64, 80, 74, 64, 46, 70];
  const kMotorRevPerSec = 20;   // 1200 rpm on 60 Hz mains

  const wheelGear = (w) => (w <= 84 ? (w - 1) % 12 : (w - 85) + 5);
  const wheelTeeth = (w) => (w <= 84 ? (2 << (((w - 1) / 12) | 0)) : 192);
  function wheelBaseHz(w) {
    const g = wheelGear(w);
    return kMotorRevPerSec * wheelTeeth(w) * kGearNum[g] / kGearDen[g];
  }
  function wheelLevel(w) {
    if (w <= 48) return 1;
    return dbToGain(-4 * (w - 48) / 43);
  }
  function wheelThird(w) {
    if (w <= 12) return 0.05;
    if (w <= 24) return 0.01;
    return 0;
  }
  function wheelPartner(w) {
    if (w >= 1 && w <= 36) return w + 48;
    if (w >= 49 && w <= 84) return w - 48;
    if (w >= 42 && w <= 48) return w + 43;
    if (w >= 85 && w <= 91) return w - 43;
    return 0;
  }
  const wheelHarmonicRoom = (w) => (w <= 43 ? 1 : Math.max(0, 1 - (w - 43) / 20));
  const mainsRatio = (mainsChoice) => (mainsChoice === 1 ? 50 / 60 : 1);

  // ======================================================== dsp/Wiring.h ===
  const kManualKeys = 61, kPedalKeys = 25, kMaxKeys = 61, kContacts = 9, kBuses = 9, kManuals = 3;
  const kNumFootages = 9;
  const kFootageOffset = [-12, 7, 0, 12, 19, 24, 28, 31, 36];

  function makeKeyWiring() {
    const c = [];
    for (let i = 0; i < kContacts; ++i) c.push({ wheel: 0, bus: 0, gain: 0 });
    return { c, count: 0 };
  }
  function clearKeyWiring(kw) {
    for (let i = 0; i < kContacts; ++i) { kw.c[i].wheel = 0; kw.c[i].bus = 0; kw.c[i].gain = 0; }
    kw.count = 0;
  }

  function b3ManualWheel(key, foot) {
    let t = key + 13 + kFootageOffset[foot];
    while (t < 13) t += 12;
    while (t > kNumWheels) t -= 12;
    return t;
  }

  // B-3 key taper in dB (setBfree's taperingModel): [first key of the next range, dB] ...
  const kTaper = [
    [[12, -10], [17, -7], [24, -3.5], [36, 0], [48, 3.5], [99, 7]],
    [[15, -3.5], [38, 0], [50, 3.5], [99, 7]],
    [[17, -7], [22, -3.5], [37, 0], [49, 3.5], [99, 7]],
    [[17, -3.5], [39, 0], [99, -3.5]],
    [[14, 7], [20, 3.5], [40, 0], [50, -3.5], [99, -7]],
    [[12, 7], [15, 3.5], [41, 0], [54, -3.5], [99, -7]],
    [[14, 3.5], [42, 0], [50, -3.5], [99, -7]],
    [[43, 0], [48, -3.5], [99, -7]],
    [[43, 0], [99, -7]],
  ];
  function b3TaperDb(key, foot) {
    const t = kTaper[foot];
    for (let i = 0; ; ++i) if (key < t[i][0]) return t[i][1];
  }

  const kM3FirstKey = 5, kM3LastKey = 48, kM3Pedals = 13;
  function m3ManualWheel(key, foot) {
    let t = key + 13 + kFootageOffset[foot];
    while (t < 10) t += 12;
    while (t > kNumWheels) t -= 12;
    return t;
  }

  function buildManualWiring(w, organModel, manual) {
    const m3 = organModel === 3;
    for (let k = 0; k < kMaxKeys; ++k) {
      const kw = w[k];
      clearKeyWiring(kw);
      if (m3) {
        if (k < kM3FirstKey || k > kM3LastKey) continue;
        for (let f = manual === 1 ? 1 : 0; f < kContacts; ++f) {
          const c = kw.c[kw.count++];
          c.wheel = m3ManualWheel(k, f); c.bus = f; c.gain = 1;
        }
        continue;
      }
      for (let f = 0; f < kContacts; ++f) {
        const c = kw.c[f];
        c.wheel = b3ManualWheel(k, f); c.bus = f; c.gain = dbToGain(b3TaperDb(k, f));
      }
      kw.count = kContacts;
    }
  }

  function buildPedalWiring(w, organModel) {
    const m3 = organModel === 3;
    const wheel = (t) => { if (m3) while (t < 10) t += 12; return t; };
    const set = (c, wh, bus, gain) => { c.wheel = wh; c.bus = bus; c.gain = gain; };
    for (let k = 0; k < kMaxKeys; ++k) {
      const kw = w[k];
      clearKeyWiring(kw);
      if (k >= (m3 ? kM3Pedals : kPedalKeys)) continue;
      set(kw.c[0], wheel(k + 1), 0, 1);
      set(kw.c[1], wheel(k + 13), 0, 0.35);
      set(kw.c[2], wheel(k + 20), 0, 0.15);
      set(kw.c[3], wheel(k + 13), 1, 1);
      set(kw.c[4], wheel(k + 25), 1, 0.30);
      set(kw.c[5], wheel(k + 32), 1, 0.12);
      kw.count = 6;
    }
  }

  // Fixed registrations of the preset keys C# .. A (the factory chart; also in webhost/factory.js).
  const kFactoryRegistrations = [
    [0, 0, 5, 3, 2, 0, 0, 0, 0], [0, 0, 4, 4, 3, 2, 0, 0, 0], [0, 0, 8, 7, 4, 0, 0, 0, 0],
    [0, 0, 4, 5, 4, 4, 2, 2, 2], [0, 0, 5, 4, 0, 3, 0, 0, 0], [0, 0, 4, 6, 7, 5, 3, 0, 0],
    [0, 0, 5, 6, 4, 4, 3, 2, 0], [0, 0, 6, 8, 7, 6, 5, 4, 0], [3, 2, 7, 6, 4, 5, 2, 2, 2],
  ];

  // ======================================================= dsp/Scanner.h ===
  class Scanner {
    constructor() {
      this.line = [];
      for (let i = 0; i < Scanner.kSections; ++i) this.line.push(new Biquad());
      this.lp = new Biquad();
      this.tap = new Float64Array(Scanner.kSections + 1);
      this.rc = 1; this.rs = 0; this.pc = 1; this.ps = 0;
    }
    prepare(sr) {
      for (const s of this.line) { s.setAllpass(Scanner.kSectionHz, Scanner.kSectionQ, sr); s.reset(); }
      this.lp.setLowpass(Scanner.kLineLowpassHz, 0.7071067811865476, sr);
      this.lp.reset();
      this.pc = 1; this.ps = 0;
      this.setRate(Scanner.kRateHz, sr);
    }
    setRate(hz, sr) {
      const inc = kTwoPi * hz / sr;
      this.rc = Math.cos(inc);
      this.rs = Math.sin(inc);
    }
    renormalize() {
      const k = 1.5 - 0.5 * (this.pc * this.pc + this.ps * this.ps);
      this.pc *= k; this.ps *= k;
    }
    process(x, depth) {
      const c2 = this.pc * this.rc - this.ps * this.rs, s2 = this.ps * this.rc + this.pc * this.rs;
      this.pc = c2; this.ps = s2;
      const tap = this.tap, line = this.line;
      tap[0] = x;
      for (let k = 0; k < Scanner.kSections; ++k) tap[k + 1] = line[k].process(tap[k]);
      const p = depth * (1 - this.pc) * 0.5;
      let i = Math.trunc(p);
      if (i >= Scanner.kSections) i = Scanner.kSections - 1;
      const f = p - i;
      return this.lp.process(tap[i] + f * (tap[i + 1] - tap[i]));
    }
  }
  Scanner.kSections = 18;
  Scanner.kSectionHz = 7900; Scanner.kSectionQ = 0.6;
  Scanner.kRateHz = 6.87;
  Scanner.kLineLowpassHz = 6500;

  // ======================================================== dsp/Preamp.h ===
  class Preamp {
    constructor() {
      this.aB = 0; this.aT = 0; this.zB = 0; this.zT = 0;
      this.gB = 1; this.gM = 1; this.gT = 1; this.iB = 0; this.iM = 0; this.iT = 0;
      this.tB = 1; this.tM = 1; this.tT = 1;
    }
    prepare(sr) {
      this.aB = Math.exp(-kTwoPi * Preamp.kBassHz / sr);
      this.aT = Math.exp(-kTwoPi * Preamp.kTrebleHz / sr);
      this.zB = this.zT = 0;
      this.gB = this.gM = this.gT = this.tB = this.tM = this.tT = 1;
      this.iB = this.iM = this.iT = 0;
    }
    setExpression(e, chunk) {
      this.gB = this.tB; this.gM = this.tM; this.gT = this.tT;
      const closed = 1 - clampd(e, 0, 1);
      this.tB = closed === 0 ? 1 : dbToGain(Preamp.kClosedBassDb * closed);
      this.tM = closed === 0 ? 1 : dbToGain(Preamp.kClosedMidDb * closed);
      this.tT = closed === 0 ? 1 : dbToGain(Preamp.kClosedTrebleDb * closed);
      this.iB = (this.tB - this.gB) / chunk; this.iM = (this.tM - this.gM) / chunk; this.iT = (this.tT - this.gT) / chunk;
    }
    process(x) {
      this.zB = x + this.aB * (this.zB - x);
      this.zT = x + this.aT * (this.zT - x);
      const y = this.gB * this.zB + this.gM * (this.zT - this.zB) + this.gT * (x - this.zT);
      this.gB += this.iB; this.gM += this.iM; this.gT += this.iT;
      return y;
    }
  }
  Preamp.kBassHz = 200; Preamp.kTrebleHz = 2500;
  Preamp.kClosedBassDb = -14; Preamp.kClosedMidDb = -26; Preamp.kClosedTrebleDb = -32;

  // =================================================== dsp/Oversampler.h ===
  class HalfbandDesign {
    constructor(numCoefs, transition) {
      this.n = numCoefs;
      this.c = new Float64Array(12);
      const n = this.n, order = n * 2 + 1;
      let k = Math.tan((1 - 2 * transition) * kPi / 4);
      k *= k;
      const kk = Math.sqrt(Math.sqrt(1 - k * k));
      const e = 0.5 * (1 - kk) / (1 + kk);
      const e4 = e * e * e * e;
      const q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)));
      for (let idx = 0; idx < n; ++idx) {
        const cc = idx + 1;
        let num = 0, den = 0, t;
        let i = 0, j = 1;
        do { t = Math.pow(q, i * (i + 1)) * Math.sin((i * 2 + 1) * cc * kPi / order) * j; num += t; j = -j; ++i; } while (Math.abs(t) > 1e-100);
        num *= Math.pow(q, 0.25);
        i = 1; j = -1;
        do { t = Math.pow(q, i * i) * Math.cos(i * 2 * cc * kPi / order) * j; den += t; j = -j; ++i; } while (Math.abs(t) > 1e-100);
        den += 0.5;
        const ww = num / den, w2 = ww * ww;
        const x = Math.sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2);
        this.c[idx] = (1 - x) / (1 + x);
      }
    }
  }
  const halfbandA = new HalfbandDesign(8, 0.04);
  const halfbandB = new HalfbandDesign(4, 0.14);

  // Two all-pass chains (even / odd coefficients) run side by side, as in HIIR's StageProc.
  class HalfbandState {
    constructor() { this.x = new Float64Array(12); this.y = new Float64Array(12); this.o0 = 0; this.o1 = 0; }
    reset() { this.x.fill(0); this.y.fill(0); }
    run(d, s0, s1) {
      const n = d.n, c = d.c, x = this.x, y = this.y;
      let i = 0;
      for (; i + 1 < n; i += 2) {
        const t0 = (s0 - y[i]) * c[i] + x[i];
        const t1 = (s1 - y[i + 1]) * c[i + 1] + x[i + 1];
        x[i] = s0; x[i + 1] = s1;
        y[i] = t0; y[i + 1] = t1;
        s0 = t0; s1 = t1;
      }
      if (i < n) {
        const t0 = (s0 - y[i]) * c[i] + x[i];
        x[i] = s0; y[i] = t0; s0 = t0;
      }
      this.o0 = s0; this.o1 = s1;
    }
    // one input -> two outputs (o0, o1) at twice the rate
    up(d, inp) { this.run(d, inp, inp); }
    // two inputs -> one output at half the rate
    down(d, in0, in1) { this.run(d, in1, in0); return 0.5 * (this.o0 + this.o1); }
  }

  // ADAA shaper: g(x) = f(x + b) - f(b), f(x) = x / (1 + |x|), F = |x| - ln(1 + |x|).
  class Shaper {
    constructor() { this.bias = 0; this.x1 = 0; }
    reset() { this.x1 = 0; }
    static f(x) { return x / (1 + Math.abs(x)); }
    static F(x) { const a = Math.abs(x); return a - Math.log(1 + a); }
    G(x) { return Shaper.F(x + this.bias) - Shaper.f(this.bias) * x; }
    process(x) {
      const d = x - this.x1;
      let y;
      if (Math.abs(d) < 1e-6) y = Shaper.f(0.5 * (x + this.x1) + this.bias) - Shaper.f(this.bias);
      else y = (this.G(x) - this.G(this.x1)) / d;
      this.x1 = x;
      return y;
    }
    slope() { const a = 1 + Math.abs(this.bias); return 1 / (a * a); }
  }

  class OversampledShaper {
    constructor() {
      this.upA = new HalfbandState(); this.upB = new HalfbandState(); this.downA = new HalfbandState(); this.downB = new HalfbandState();
      this.s1 = new Shaper(); this.s2 = new Shaper(); this.s4 = new Shaper();
    }
    reset() { this.upA.reset(); this.upB.reset(); this.downA.reset(); this.downB.reset(); this.s1.reset(); this.s2.reset(); this.s4.reset(); }
    setBias(b) { this.s1.bias = this.s2.bias = this.s4.bias = b; }
    static latency(factor) { return factor >= 4 ? OversampledShaper.kLatency4x : factor === 2 ? OversampledShaper.kLatency2x : 0; }
    process(x, factor) {
      if (factor <= 1) return this.s1.process(x);
      const A = halfbandA;
      this.upA.up(A, x);
      const a0 = this.upA.o0, a1 = this.upA.o1;
      if (factor === 2) {
        const y0 = this.s2.process(a0);
        const y1 = this.s2.process(a1);
        return this.downA.down(A, y0, y1);
      }
      const B = halfbandB;
      this.upB.up(B, a0);
      const b0 = this.upB.o0, b1 = this.upB.o1;
      this.upB.up(B, a1);
      const b2 = this.upB.o0, b3 = this.upB.o1;
      const y0 = this.s4.process(b0);
      const y1 = this.s4.process(b1);
      const y2 = this.s4.process(b2);
      const y3 = this.s4.process(b3);
      const c0 = this.downB.down(B, y0, y1);
      const c1 = this.downB.down(B, y2, y3);
      return this.downA.down(A, c0, c1);
    }
  }
  OversampledShaper.kLatency2x = 3; OversampledShaper.kLatency4x = 4;

  // ====================================================== dsp/Outboard.h ===
  class Overdrive {
    constructor() { this.shaper = new Shaper(); this.lp = 0; this.a = 1; this.gain = 1; this.outGain = 1; }
    reset() { this.shaper.reset(); this.lp = 0; this.shaper.bias = 0.12; }
    set(drive, tone, levelDb, sr) {
      this.gain = dbToGain(36 * clampd(drive, 0, 1));
      this.a = onePoleCoef(800 * Math.pow(10, clampd(tone, 0, 1)), sr);
      this.outGain = dbToGain(levelDb) / (Math.sqrt(this.gain) * this.shaper.slope());
    }
    process(x) {
      const y = this.shaper.process(x * this.gain);
      this.lp += this.a * (y - this.lp);
      return this.lp * this.outGain;
    }
  }

  class Wah {
    constructor() { this.ic1 = 0; this.ic2 = 0; this.g = 0.05; this.gInc = 0; this.gTarget = 0.05; }
    reset() { this.ic1 = this.ic2 = 0; }
    setPosition(pos, sr) {
      this.g = this.gTarget;
      const fc = 350 * Math.pow(2, 2.7 * clampd(pos, 0, 1));
      this.gTarget = Math.tan(kPi * fc / sr);
      this.gInc = (this.gTarget - this.g) / kChunk;
    }
    process(x) {
      const gg = this.g;
      this.g += this.gInc;
      const kK = Wah.kK;
      const a1 = 1 / (1 + gg * (gg + kK)), a2 = gg * a1, a3 = gg * a2;
      const v3 = x - this.ic2;
      const v1 = a1 * this.ic1 + a2 * v3;
      const v2 = this.ic2 + a2 * this.ic1 + a3 * v3;
      this.ic1 = 2 * v1 - this.ic1;
      this.ic2 = 2 * v2 - this.ic2;
      return 2 * kK * v1 + 0.2 * v2;
    }
  }
  Wah.kK = 0.25;

  class TapeEcho {
    constructor() {
      this.buf = new Float64Array(1); this.mask = 0; this.w = 0;
      this.delay = 0; this.delayTarget = 0;
      this.wowC = 1; this.wowS = 0; this.wowRc = 1; this.wowRs = 0; this.flC = 1; this.flS = 0; this.flRc = 1; this.flRs = 0;
      this.lp = 0; this.aLp = 1; this.hp = 0; this.aHp = 0; this.fb = 0; this.mix = 0; this.wow = 0;
      this.sr = 48000; this.first = true;
    }
    prepare(sampleRate) {
      this.sr = sampleRate;
      let n = 1;
      while (n < Math.trunc(1.35 * this.sr)) n <<= 1;
      this.buf = new Float64Array(n);
      this.mask = n - 1;
      this.w = 0;
      this.delay = this.delayTarget = 0.35 * this.sr;
      this.wowC = 1; this.wowS = 0; this.flC = 1; this.flS = 0;
      const i1 = kTwoPi * 0.6 / this.sr, i2 = kTwoPi * 5.5 / this.sr;
      this.wowRc = Math.cos(i1); this.wowRs = Math.sin(i1); this.flRc = Math.cos(i2); this.flRs = Math.sin(i2);
      this.aHp = onePoleCoef(120, this.sr);
      this.lp = this.hp = 0;
      this.first = true;
    }
    reset() { this.buf.fill(0); this.lp = this.hp = 0; }
    set(timeSec, feedback, mixAmt, wowAmt, tone) {
      this.delayTarget = clampd(timeSec, 0.03, 1.2) * this.sr;
      if (this.first) { this.delay = this.delayTarget; this.first = false; }
      this.fb = clampd(feedback, 0, 0.95);
      this.mix = clampd(mixAmt, 0, 1);
      this.wow = clampd(wowAmt, 0, 1);
      this.aLp = onePoleCoef(1500 * Math.pow(2, 2.4 * clampd(tone, 0, 1)), this.sr);
      let k = 1.5 - 0.5 * (this.wowC * this.wowC + this.wowS * this.wowS); this.wowC *= k; this.wowS *= k;
      k = 1.5 - 0.5 * (this.flC * this.flC + this.flS * this.flS); this.flC *= k; this.flS *= k;
    }
    process(x) {
      this.delay += (this.delayTarget - this.delay) * 0.0002;
      let c = this.wowC * this.wowRc - this.wowS * this.wowRs, s = this.wowS * this.wowRc + this.wowC * this.wowRs; this.wowC = c; this.wowS = s;
      c = this.flC * this.flRc - this.flS * this.flRs; s = this.flS * this.flRc + this.flC * this.flRs; this.flC = c; this.flS = s;
      const d = this.delay * (1 + this.wow * (0.004 * this.wowS + 0.0012 * this.flS));
      const buf = this.buf, mask = this.mask;
      const pos = this.w - d;
      const fl = Math.floor(pos);
      const i = fl | 0;
      const f = pos - fl;
      const y = buf[i & mask] + f * (buf[(i + 1) & mask] - buf[i & mask]);
      this.lp += this.aLp * (y - this.lp);
      this.hp += this.aHp * (this.lp - this.hp);
      const rep = this.lp - this.hp;
      const sat = rep / (1 + Math.abs(rep));
      this.w = (this.w + 1) & mask;
      buf[this.w] = x + this.fb * sat * 1.15;
      return x + this.mix * rep;
    }
  }

  // ====================================================== dsp/Cabinets.h ===
  // Field order as in the C++ struct: defaults, then the fixed voicing.
  const kCabinetModels = [
    // Rotor 22
    { rotorConfig: 0, hornSlow: 0.8, hornFast: 6.7, hornAccel: 1.0, hornBrake: 1.5, drumSlow: 0.7, drumFast: 5.8, drumAccel: 4.0, drumBrake: 5.0,
      drumBaffle: 0, xover: 800, cabBack: 0, micLayout: 1,
      hornRadius: 0.15, drumRadius: 0.10, hornAm: 0.55, drumAm: 0.45, hornLpFacing: 12000, hornLpAway: 3000, drumLpFacing: 2500, drumLpAway: 900,
      boxHz: 100, boxDb: 4.0, boxQ: 0.9, wallMs: 1.4, ampGainDb: 6.0, ampBias: 0.20, ampHpHz: 40, ampLpHz: 7000, reverbMix: 0.0, stationary: false, outGain: 0.80,
      spkHpHz: 35, spkLpHz: 9000, midHz: 2500, midDb: 1.0, midQ: 1.0 },
    // Rotor 45
    { rotorConfig: 0, hornSlow: 0.85, hornFast: 6.8, hornAccel: 0.9, hornBrake: 1.4, drumSlow: 0.75, drumFast: 6.0, drumAccel: 3.5, drumBrake: 4.5,
      drumBaffle: 0, xover: 800, cabBack: 0, micLayout: 1,
      hornRadius: 0.15, drumRadius: 0.10, hornAm: 0.60, drumAm: 0.50, hornLpFacing: 9000, hornLpAway: 2500, drumLpFacing: 2200, drumLpAway: 800,
      boxHz: 200, boxDb: 6.0, boxQ: 1.4, wallMs: 0.6, ampGainDb: 15.0, ampBias: 0.25, ampHpHz: 70, ampLpHz: 6000, reverbMix: 0.0, stationary: false, outGain: 0.80,
      spkHpHz: 130, spkLpHz: 5500, midHz: 1200, midDb: 8.0, midQ: 1.0 },
    // Single Rotor
    { rotorConfig: 2, hornSlow: 0.9, hornFast: 6.0, hornAccel: 1.2, hornBrake: 2.0, drumSlow: 0.9, drumFast: 6.0, drumAccel: 1.2, drumBrake: 2.0,
      drumBaffle: 2, xover: 800, cabBack: 0, micLayout: 2,
      hornRadius: 0.15, drumRadius: 0.10, hornAm: 0.40, drumAm: 0.75, hornLpFacing: 6000, hornLpAway: 2000, drumLpFacing: 4500, drumLpAway: 1200,
      boxHz: 300, boxDb: 4.0, boxQ: 1.0, wallMs: 0.7, ampGainDb: 10.0, ampBias: 0.30, ampHpHz: 120, ampLpHz: 5000, reverbMix: 0.0, stationary: false, outGain: 0.90,
      spkHpHz: 180, spkLpHz: 4000, midHz: 900, midDb: 8.0, midQ: 0.9 },
    // Tone Cabinet
    { rotorConfig: 3, hornSlow: 0.8, hornFast: 6.7, hornAccel: 1.0, hornBrake: 1.5, drumSlow: 0.7, drumFast: 5.8, drumAccel: 4.0, drumBrake: 5.0,
      drumBaffle: 0, xover: 800, cabBack: 0, micLayout: 2,
      hornRadius: 0.15, drumRadius: 0.10, hornAm: 0.0, drumAm: 0.0, hornLpFacing: 12000, hornLpAway: 12000, drumLpFacing: 4000, drumLpAway: 4000,
      boxHz: 100, boxDb: 2.0, boxQ: 1.0, wallMs: 2.5, ampGainDb: 3.0, ampBias: 0.10, ampHpHz: 40, ampLpHz: 9000, reverbMix: 0.30, stationary: true, outGain: 0.80,
      spkHpHz: 45, spkLpHz: 13000, midHz: 3500, midDb: 4.0, midQ: 0.7 },
  ];
  const cabinetModel = (i) => kCabinetModels[i < 0 ? 0 : (i > 3 ? 3 : i)];

  // ======================================================== dsp/Rotary.h ===
  function newCabSettings() {
    return { model: 0, rotorConfig: 0, hornType: 0, hornMouth: 0, drumBaffle: 0, cabBack: 0, micLayout: 1,
             xover: 800, balance: 0, micDist: 0.4, micAngle: 90, micHorn: 1, micDrum: 1, micRoom: 0 };
  }
  const kCabSettingKeys = ['model', 'rotorConfig', 'hornType', 'hornMouth', 'drumBaffle', 'cabBack', 'micLayout', 'xover', 'balance',
                           'micDist', 'micAngle', 'micHorn', 'micDrum', 'micRoom'];
  function cabSettingsEqual(a, b) {
    for (const k of kCabSettingKeys) if (a[k] !== b[k]) return false;
    return true;
  }
  function newPickup() {
    return { cphi: 1, sphi: 0, base: 0, rad: 0, gain: 0, wL: 0, wR: 0, am: 0, aFacing: 1, aAway: 1, flip: false, beam2: false, z: 0 };
  }
  function clearPickup(p) {
    p.cphi = 1; p.sphi = 0; p.base = 0; p.rad = 0; p.gain = 0; p.wL = 0; p.wR = 0; p.am = 0; p.aFacing = 1; p.aAway = 1; p.flip = false; p.beam2 = false; p.z = 0;
  }

  class RotaryCabinet {
    constructor() {
      this.sr = 48000;
      this.buf = [new Float64Array(1), new Float64Array(1)];
      this.mask = 0; this.w = 0;
      this.xo = [new Biquad(), new Biquad(), new Biquad(), new Biquad()];
      this.voice = [new Biquad(), new Biquad(), new Biquad(), new Biquad(), new Biquad()];
      this.box = new Biquad();
      this.rotor = [{ pc: 1, ps: 0, rc: 1, rs: 0 }, { pc: 1, ps: 0, rc: 1, rs: 0 }];
      this.pickups = [[], []];
      for (let r = 0; r < 2; ++r) for (let k = 0; k < RotaryCabinet.kMaxPickups; ++k) this.pickups[r].push(newPickup());
      this.numPickups = [0, 0];
      this.hornGain = 1; this.drumGain = 1; this.outGain = 1;
      this.settings = newCabSettings();
      this.configured = false;
      this.L = 0; this.R = 0;
    }
    prepare(sampleRate) {
      this.sr = sampleRate;
      let n = 1;
      while (n < Math.trunc(0.05 * this.sr)) n <<= 1;
      this.buf = [new Float64Array(n), new Float64Array(n)];
      this.mask = n - 1;
      this.w = 0;
      for (const r of this.rotor) { r.pc = 1; r.ps = 0; r.rc = 1; r.rs = 0; }
      for (const f of this.xo) f.reset();
      for (const v of this.voice) v.reset();
      this.box.reset();
      this.configured = false;
    }
    reset() {
      for (const b of this.buf) b.fill(0);
      for (const f of this.xo) f.reset();
      this.box.reset();
      for (const list of this.pickups) for (const p of list) p.z = 0;
    }
    configure(s) {
      if (this.configured && cabSettingsEqual(s, this.settings)) return;
      for (const k of kCabSettingKeys) this.settings[k] = s[k];
      this.configured = true;
      const sr = this.sr;
      const M = cabinetModel(s.model);
      this.xo[0].setLowpass(s.xover, 0.7071067811865476, sr);
      this.xo[1].setLowpass(s.xover, 0.7071067811865476, sr);
      this.xo[2].setHighpass(s.xover, 0.7071067811865476, sr);
      this.xo[3].setHighpass(s.xover, 0.7071067811865476, sr);
      this.box.setPeak(M.boxHz, M.boxQ, M.boxDb * (s.cabBack === 1 ? 0.3 : 1), sr);
      this.voice[0].setHighpass(M.spkHpHz, 0.5412, sr);
      this.voice[1].setHighpass(M.spkHpHz, 1.3066, sr);
      this.voice[2].setLowpass(Math.min(M.spkLpHz, 0.45 * sr), 0.5412, sr);
      this.voice[3].setLowpass(Math.min(M.spkLpHz, 0.45 * sr), 1.3066, sr);
      this.voice[4].setPeak(M.midHz, M.midQ, M.midDb, sr);
      this.hornGain = s.balance > 0 ? 1 : 1 + s.balance;
      this.drumGain = s.balance < 0 ? 1 : 1 - s.balance;
      this.outGain = M.outGain;
      this.buildPickups(s, M);
    }
    setRotor(i, angle, inc) {
      const r = this.rotor[i];
      r.pc = Math.cos(angle); r.ps = Math.sin(angle);
      r.rc = Math.cos(inc); r.rs = Math.sin(inc);
    }
    process(x) {
      const voice = this.voice;
      for (let i = 0; i < 5; ++i) x = voice[i].process(x);
      let lo, hi;
      const cfg = this.settings.rotorConfig;
      if (cfg === 1) { hi = x; lo = 0; }
      else if (cfg === 2) { hi = 0; lo = x; }
      else {
        lo = this.xo[1].process(this.xo[0].process(x));
        hi = this.xo[3].process(this.xo[2].process(x));
      }
      this.w = (this.w + 1) & this.mask;
      this.buf[0][this.w] = hi * this.hornGain;
      this.buf[1][this.w] = this.box.process(lo) * this.drumGain;

      let L = 0, R = 0;
      for (let ri = 0; ri < 2; ++ri) {
        const r = this.rotor[ri];
        const c2 = r.pc * r.rc - r.ps * r.rs, s2 = r.ps * r.rc + r.pc * r.rs;
        r.pc = c2; r.ps = s2;
        const b = this.buf[ri];
        const list = this.pickups[ri], n = this.numPickups[ri];
        for (let k = 0; k < n; ++k) {
          const p = list[k];
          let c = r.pc * p.cphi + r.ps * p.sphi;
          if (p.flip) c = -c;
          const t = 0.5 + 0.5 * c;
          const v = this.read(b, p.base + p.rad * (1 - c));
          p.z += (p.aAway + (p.aFacing - p.aAway) * t) * (v - p.z);
          const shape = p.beam2 ? t * t : t;
          const s = (1 - p.am + p.am * shape) * p.gain * p.z;
          L += p.wL * s;
          R += p.wR * s;
        }
      }
      this.L = L * this.outGain;
      this.R = R * this.outGain;
    }
    pickupCount(rotorIndex) { return this.numPickups[rotorIndex]; }

    // 4-point Hermite read, d samples back from the newest sample (d >= 2)
    read(b, d) {
      const mask = this.mask;
      const pos = this.w - d;
      const fl = Math.floor(pos);
      const i = fl | 0;
      const f = pos - fl;
      const xm1 = b[(i - 1) & mask], x0 = b[i & mask];
      const x1 = b[(i + 1) & mask], x2 = b[(i + 2) & mask];
      const c1 = 0.5 * (x1 - xm1);
      const c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2;
      const c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
      return ((c3 * f + c2) * f + c1) * f + x0;
    }

    buildPickups(s, M) {
      const sr = this.sr;
      const ms = sr / 1000;
      const shortMouth = s.hornMouth === 1, open = s.cabBack === 1;
      const hornR = M.hornRadius * (shortMouth ? 0.9 : 1);
      const drumR = M.drumRadius * (s.drumBaffle === 2 ? 0.5 : 1);
      const hornAm = Math.min(1, M.hornAm * (shortMouth ? 1.15 : 1));
      const drumAm = M.drumAm * (s.drumBaffle === 1 ? 0.5 : (s.drumBaffle === 2 ? 0.7 : 1));
      const hornLp = shortMouth ? 1.4 : 1;
      const drumLp = s.drumBaffle === 1 ? 1.6 : (s.drumBaffle === 2 ? 0.6 : 1);
      const amScale = 1 - 0.5 * s.micDist, reflScale = 0.6 + 0.8 * s.micDist;
      const base = (0.3 + 2 * s.micDist) * ms;
      const wall = M.wallMs * (open ? 1.5 : 1) * ms, backGain = open ? 0.45 : 0.22;
      const half = 0.5 * s.micAngle * kPi / 180;

      const mic = (phi, wL, wR, amMul, reflMul) => ({ phi, wL, wR, amMul, reflMul });
      const hornMics = [], drumMics = [];
      switch (s.micLayout) {
        case 0: hornMics.push(mic(0, 1, 1, 1, 1)); drumMics.push(mic(0, 1, 1, 1, 1)); break;
        case 2: hornMics.push(mic(half, 1, 0, 1, 1), mic(-half, 0, 1, 1, 1));
                drumMics.push(mic(half, 1, 0, 1, 1), mic(-half, 0, 1, 1, 1)); break;
        case 3: hornMics.push(mic(half, 1, 0, 1, 1), mic(-half, 0, 1, 1, 1));
                drumMics.push(mic(0, 1, 1, 1.0, 0.5)); break;                       // drum close-miked at its port
        case 4: hornMics.push(mic(0, 1, 0, 0.6, 1.6), mic(kPi, 0, 1, 0.6, 1.6));    // room pair: front / back
                drumMics.push(mic(0, 1, 0, 0.6, 1.6), mic(kPi, 0, 1, 0.6, 1.6)); break;
        default: hornMics.push(mic(half, 1, 0, 1, 1), mic(-half, 0, 1, 1, 1));
                 drumMics.push(mic(0, 1, 1, 0.7, 1)); break;                       // stereo horn + mono drum
      }

      for (let ri = 0; ri < 2; ++ri) {
        const horn = ri === 0;
        const list = this.pickups[ri];
        let n = 0;
        const rad = (horn ? hornR : drumR) / RotaryCabinet.kSpeedOfSound * sr;
        const am = horn ? hornAm : drumAm;
        const aF = onePoleCoef(Math.min(0.45 * sr, (horn ? M.hornLpFacing * hornLp : M.drumLpFacing * drumLp)), sr);
        const aA = onePoleCoef(Math.min(0.45 * sr, (horn ? M.hornLpAway * hornLp : M.drumLpAway * drumLp)), sr);
        const level = horn ? s.micHorn : s.micDrum;
        const mics = horn ? hornMics : drumMics;
        const copies = horn && s.hornType === 1 ? 2 : 1;   // dual active horns
        const copyGain = copies === 2 ? 0.75 : 1;
        for (let copy = 0; copy < copies; ++copy)
          for (let m = 0; m < mics.length; ++m) {
            const mc = mics[m];
            const add = (phi, extra, gain, facing, away, amMul) => {
              if (n >= RotaryCabinet.kMaxPickups) return;
              const p = list[n++];
              clearPickup(p);
              p.cphi = Math.cos(phi); p.sphi = Math.sin(phi);
              p.base = base + extra; p.rad = rad; p.gain = gain * level * copyGain;
              p.wL = mc.wL; p.wR = mc.wR;
              p.am = clampd(am * amScale * mc.amMul * amMul, 0, 1);
              p.aFacing = facing; p.aAway = away;
              p.flip = copy === 1; p.beam2 = horn;
            };
            add(mc.phi, 0, 1, aF, aA, 1);                                               // direct
            add(mc.phi + kPi, wall, backGain * reflScale * mc.reflMul, aA, aA, 1);        // back wall
            add(mc.phi + 0.5 * kPi, 0.6 * ms, 0.15 * reflScale * mc.reflMul, aA, aA, 1); // side wall
          }
        if (s.micRoom > 0) {   // room mic: late, dark, decorrelated
          const aRoom = onePoleCoef(3000, sr);
          const g = 0.6 * s.micRoom * level;
          for (let side = 0; side < 2 && n < RotaryCabinet.kMaxPickups; ++side) {
            const p = list[n++];
            clearPickup(p);
            const phi = side === 0 ? 0.5 * kPi : -0.5 * kPi;
            p.cphi = Math.cos(phi); p.sphi = Math.sin(phi);
            p.base = base + (side === 0 ? 9.0 : 12.5) * ms; p.rad = rad; p.gain = g;
            p.wL = side === 0 ? 1 : 0; p.wR = side === 0 ? 0 : 1;
            p.am = 0.3 * am; p.aFacing = aRoom; p.aAway = aRoom; p.beam2 = horn;
          }
        }
        this.numPickups[ri] = n;
      }
    }
  }
  RotaryCabinet.kMaxPickups = 16;
  RotaryCabinet.kSpeedOfSound = 343;

  // ================================================== dsp/SpringReverb.h ===
  class Spring {
    constructor() {
      this.buf = new Float64Array(8); this.mask = 7; this.w = 0; this.len = 1;
      this.apX = new Float64Array(Spring.kAllpasses); this.apY = new Float64Array(Spring.kAllpasses);
      this.lp = 0; this.hp = 0; this.aLp = 1; this.aHp = 0; this.g = 0;
    }
    prepare(sr, ms) {
      this.len = Math.max(4, Math.trunc(ms * sr / 1000));
      let n = 1;
      while (n < this.len + 4) n <<= 1;
      this.buf = new Float64Array(n);
      this.mask = n - 1;
      this.w = 0;
      this.reset();
      this.aHp = onePoleCoef(40, sr);
    }
    reset() { this.buf.fill(0); this.apX.fill(0); this.apY.fill(0); this.lp = this.hp = 0; }
    set(decaySec, tone, sr) {
      this.g = Math.pow(10, -3 * (this.len / sr) / Math.max(0.1, decaySec));
      this.aLp = onePoleCoef(2000 * Math.pow(2, 1.6 * clampd(tone, 0, 1)), sr);
    }
    process(x) {
      const apX = this.apX, apY = this.apY, kD = Spring.kDisperse;
      let y = this.buf[(this.w - this.len) & this.mask];
      for (let i = 0; i < Spring.kAllpasses; ++i) {
        const o = kD * (y - apY[i]) + apX[i];
        apX[i] = y; apY[i] = o; y = o;
      }
      this.lp += this.aLp * (y - this.lp);
      this.hp += this.aHp * (this.lp - this.hp);
      const out = this.lp - this.hp;
      this.w = (this.w + 1) & this.mask;
      this.buf[this.w] = x + this.g * out;
      return out;
    }
  }
  Spring.kAllpasses = 8;
  Spring.kDisperse = 0.62;

  class SpringReverb {
    constructor() { this.s = [new Spring(), new Spring(), new Spring(), new Spring()]; this.inHp = 0; this.aIn = 0; this.wl = 0; this.wr = 0; }
    prepare(sr) {
      const ms = [27.3, 41.7, 33.1, 45.9];
      for (let i = 0; i < 4; ++i) this.s[i].prepare(sr, ms[i]);
      this.aIn = onePoleCoef(200, sr);
      this.inHp = 0;
    }
    reset() { for (const x of this.s) x.reset(); this.inHp = 0; }
    set(decay, tone, sr) { for (const x of this.s) x.set(decay, tone, sr); }
    // mono in, stereo wet out (wl, wr)
    process(x) {
      this.inHp += this.aIn * (x - this.inHp);
      const inp = 0.5 * (x - this.inHp);
      const s = this.s;
      this.wl = 0.5 * (s[0].process(inp) + s[1].process(inp));
      this.wr = 0.5 * (s[2].process(inp) + s[3].process(inp));
    }
  }

  // ======================================================= SynthEngine.h ===
  const kEventCapacity = 1 << 16;
  const kNumWheelSlots = kNumWheels + 1;
  const kMaxRamps = 2048;

  function newEngineParams() {
    const p = {
      organModel: 0, ampGain: 1, master: 0.7, tuneRatio: 1, mains: 0, transpose: 0,
      motorStart: true, motorRun: true, velMode: 0, velAmount: 0.5,
      bars: [[new Float64Array(kNumFootages), new Float64Array(kNumFootages)], [new Float64Array(kNumFootages), new Float64Array(kNumFootages)]],
      pedalBars: [5, 3], presetKey: [11, 11], regs: [[], []],
      click: [0.5, 0.5, 0.3], keyUpClick: true,
      percOn: false, percSoft: false, percSlow: false, percThird: true, percFastTime: 0.3, percSlowTime: 1.0,
      vibUpper: false, vibLower: false, vibMode: 5,
      leak: [0.3, 0.3], age: [0.3, 0.3],
      pedTone: 0.5, pedDecay: 0.6, loSusTime: 0.8, pedSustain: false, loSustain: false, manualBass: false,
      channel: [0, 1, 2], splitMode: false, splitPoint: 60, splitLoOct: 0, pedalZone: false, pedalSplit: 48, pedMidiBase: 36,
      motorCC: 0, exprCC: 11,
      speedWheel: 1, speedCC: 0, varSource: 0, speedSustain: false,
      drive: 0.2, cabModel: 0, rotorConfig: 0, speedMode: 0, speed: 1, hornSel: 0, drumSel: 0,
      cabBypass: false, drumOff: false, brakePos: false,
      hornSlow: 0.8, hornFast: 6.7, hornAccel: 1.0, hornBrake: 1.5, drumSlow: 0.7, drumFast: 5.8, drumAccel: 4.0, drumBrake: 5.0,
      hornDir: 0, drumDir: 1, brakeAngle: 0,
      hornType: 0, hornMouth: 0, drumBaffle: 0, cabBack: 0, micLayout: 1, ampOS: 1,
      xover: 800, hornDrumBal: 0, micDist: 0.4, micAngle: 90, micHorn: 1, micDrum: 1, micRoom: 0, width: 1,
      odOn: false, wahOn: false, echoOn: false, revOn: false,
      odDrive: 0.5, odTone: 0.5, odLevel: 0, wahPos: 0.5, wahSrc: 0, wahCC: 4,
      echoTime: 0.35, echoFb: 0.35, echoMix: 0.25, echoWow: 0.3, echoTone: 0.5,
      revMix: 0.2, revDecay: 2.0, revTone: 0.5,
    };
    for (let m = 0; m < 2; ++m)
      for (let k = 0; k < 9; ++k) {
        const r = new Int32Array(kNumFootages);
        for (let f = 0; f < kNumFootages; ++f) r[f] = kFactoryRegistrations[k][f];
        p.regs[m].push(r);
      }
    return p;
  }

  // A value ramped per sample across one 32-sample chunk (set once per chunk).
  class ChunkRamp {
    constructor() { this.cur = 0; this.target = 0; this.inc = 0; }
    set(t) { this.cur = this.target; this.target = t; this.inc = (t - this.cur) / kChunk; }
    jump(t) { this.cur = this.target = t; this.inc = 0; }
    next() { const v = this.cur; this.cur += this.inc; return v; }
  }

  function newKeyState() {
    const ks = { refs: 0, down: false, gen: 0, contact: new Float64Array(kContacts), applied: new Float64Array(kContacts), rampSlot: new Int16Array(kContacts) };
    ks.rampSlot.fill(-1);
    return ks;
  }
  function newRamp() { return { manual: 0, key: 0, contact: 0, from: 0, to: 0, pos: 0, len: 1, shape: 0, cur: 0, mult: 1 }; }

  const keyCount = (m) => (m === 2 ? kPedalKeys : kManualKeys);
  const busCountOf = (m) => (m === 2 ? 2 : kBuses);
  const depthFor = (vibMode) => 6 * (vibMode % 3 + 1);

  class SynthEngine {
    constructor() {
      this.params = newEngineParams();
      this.sr = 48000;
      this.rng = new Rng();

      // generator
      this.wc = new Float64Array(kNumWheelSlots); this.ws = new Float64Array(kNumWheelSlots);
      this.rc = new Float64Array(kNumWheelSlots); this.rs = new Float64Array(kNumWheelSlots);
      this.level = new Float64Array(kNumWheelSlots); this.second = new Float64Array(kNumWheelSlots); this.third = new Float64Array(kNumWheelSlots);
      this.wheelRnd = new Float64Array(kNumWheelSlots); this.floorW = new Float64Array(kNumWheelSlots);
      this.motorSpeed = 1; this.freqFactor = -1; this.ampCur = 0; this.ampInc = 0;
      this.genAge = -1;
      this.contactAge = new Float64Array(kManuals);
      this.motorCcOff = false;

      // keys, contacts, busbars
      this.wiring = []; this.contactRnd = []; this.contactGain = []; this.keys = []; this.busG = []; this.busLeak = [];
      this.busCount = []; this.barCur = []; this.barTarget = []; this.barInc = [];
      for (let m = 0; m < kManuals; ++m) {
        const w = [], cr = [], cg = [], ks = [], bg = [], bl = [];
        for (let k = 0; k < kMaxKeys; ++k) { w.push(makeKeyWiring()); cr.push(new Float64Array(kContacts)); cg.push(new Float64Array(kContacts)); ks.push(newKeyState()); }
        for (let b = 0; b < kBuses; ++b) { bg.push(new Float64Array(kNumWheelSlots)); bl.push(new Float64Array(kNumWheelSlots)); }
        this.wiring.push(w); this.contactRnd.push(cr); this.contactGain.push(cg); this.keys.push(ks); this.busG.push(bg); this.busLeak.push(bl);
        this.busCount.push(new Int32Array(kBuses));
        this.barCur.push(new Float64Array(kBuses)); this.barTarget.push(new Float64Array(kBuses)); this.barInc.push(new Float64Array(kBuses));
      }
      this.manualContacts = new Int32Array(kManuals); this.downCount = new Int32Array(kManuals);
      this.noteMap = new Int16Array(16 * 128);
      this.leakEff = new Float64Array(kManuals);
      this.floorUp = 0; this.floorLo = 0;
      this.coupledPedal = -1;
      this.mapM = -1; this.mapKey = -1;

      // contact events (binary min-heap by time, seq; a struct of arrays) and running contact ramps
      this.evTime = new Float64Array(kEventCapacity); this.evSeq = new Uint32Array(kEventCapacity); this.evGen = new Uint32Array(kEventCapacity);
      this.evManual = new Int16Array(kEventCapacity); this.evKey = new Int16Array(kEventCapacity); this.evContact = new Int16Array(kEventCapacity);
      this.evRamp = new Int32Array(kEventCapacity); this.evLevel = new Float64Array(kEventCapacity); this.evShape = new Int8Array(kEventCapacity);
      this.heapSize = 0;
      this.top = { time: 0, seq: 0, gen: 0, manual: 0, key: 0, contact: 0, ramp: 0, level: 0, shape: 0 };
      this.ramps = [];
      for (let i = 0; i < kMaxRamps; ++i) this.ramps.push(newRamp());
      this.numRamps = 0;
      this.seq = 0;
      this.now = 0;
      this.chunkPos = 0;

      // voice
      this.percEnv = 0; this.percMult = 1; this.percBus = 4;
      this.percGain = new ChunkRamp(); this.vibU = new ChunkRamp(); this.vibL = new ChunkRamp(); this.vibDepth = new ChunkRamp(); this.vibChorus = new ChunkRamp();
      this.scanner = new Scanner();
      this.scanRate = -1;
      this.pedLp = new Biquad();
      this.pedTone = -1;
      this.preamp = new Preamp();

      // cabinet, amp, outboard, reverb
      this.amp = new OversampledShaper();
      this.ampHp = 0; this.ampHpA = 0; this.tubeGain = 1; this.ampNorm = 1; this.ampLpHz = -1;
      this.ampPin = 0; this.ampPout = 0; this.ampPowA = 0;
      this.ampMakeup = new ChunkRamp();
      this.ampLp = new Biquad();
      this.ampFactor = 2; this.lastAmpFactor = -1;
      this.cab = new RotaryCabinet();
      this.cs = newCabSettings();
      this.od = new Overdrive();
      this.wah = new Wah();
      this.echo = new TapeEcho();
      this.rev = new SpringReverb();
      this.revSet = [0, 0];
      this.odMix = new ChunkRamp(); this.wahMix = new ChunkRamp(); this.echoOnR = new ChunkRamp(); this.revMixR = new ChunkRamp(); this.widthR = new ChunkRamp();
      this.ccVal = new Float64Array(128);
      this.speedState = 1; this.lastSpeedParam = 1;
      this.momentaryFast = false; this.lastWheelFast = false; this.rotorsStarted = false;
      this.speedCCValue = 0; this.hornAngle = 0; this.drumAngle = 0; this.hornHz = 0; this.drumHz = 0;
      this.builtModel = 0;

      // controllers / output
      this.expression = 1; this.exprSmooth = 1; this.modWheel = 0;
      this.sustain = false;
      this.masterGain = 0.7;
      this.peak = 0;

      // scratch
      this.x = new Float64Array(kNumWheelSlots);
      this.out = new Float64Array(kManuals);
    }

    prepare(sampleRate) {
      const p = this.params;
      this.sr = sampleRate;
      this.rng.seed(0x48414d31);   // "HAM1"
      for (let w = 1; w <= kNumWheels; ++w) {
        const ph = kTwoPi * this.rng.uni();
        this.wc[w] = Math.cos(ph);
        this.ws[w] = Math.sin(ph);
      }
      for (let w = 1; w <= kNumWheels; ++w) {
        this.wheelRnd[w] = this.rng.bi();
        this.floorW[w] = SynthEngine.kFloorBase * (0.3 + 0.7 * w / kNumWheels) * (0.5 + 0.5 * this.rng.uni());
      }
      for (let m = 0; m < kManuals; ++m)
        for (let k = 0; k < kMaxKeys; ++k)
          for (let c = 0; c < kContacts; ++c) this.contactRnd[m][k][c] = this.rng.bi();
      this.builtModel = p.organModel;
      buildManualWiring(this.wiring[0], this.builtModel, 0);
      buildManualWiring(this.wiring[1], this.builtModel, 1);
      buildPedalWiring(this.wiring[2], this.builtModel);
      this.genAge = -1;
      this.contactAge[0] = this.contactAge[1] = this.contactAge[2] = -1;
      this.updateAgeTables();
      this.motorSpeed = 1;
      this.ampCur = this.ampInc = 0;
      this.freqFactor = -1;
      this.masterGain = p.master;
      this.now = 0;
      this.chunkPos = 0;
      this.seq = 0;
      this.resetKeys();
      this.expression = this.exprSmooth = 1;
      this.modWheel = 0;
      this.sustain = false;
      this.motorCcOff = false;
      this.peak = 0;
      this.percEnv = 0;
      this.percMult = 1;
      this.scanner.prepare(this.sr);
      this.scanRate = -1;
      this.preamp.prepare(this.sr);
      this.pedTone = -1;
      this.pedLp.reset();
      for (const r of [this.vibU, this.vibL, this.vibDepth, this.vibChorus, this.percGain]) r.jump(0);
      this.vibU.jump(p.vibUpper ? 1 : 0);
      this.vibL.jump(p.vibLower ? 1 : 0);
      this.vibDepth.jump(depthFor(p.vibMode));
      this.vibChorus.jump(p.vibMode >= 3 ? 1 : 0);
      for (let m = 0; m < kManuals; ++m) { this.barCur[m].fill(0); this.barTarget[m].fill(0); this.barInc[m].fill(0); }
      this.coupledPedal = -1;

      this.amp.reset();
      this.ampHp = 0;
      this.ampPin = this.ampPout = 0;
      this.ampMakeup.jump(1);
      this.ampLp.reset();
      this.ampLpHz = -1;
      this.cab.prepare(this.sr);
      this.od.reset();
      this.wah.reset();
      this.echo.prepare(this.sr);
      this.rev.prepare(this.sr);
      this.revSet[0] = -1; this.revSet[1] = -1;
      this.odMix.jump(p.odOn ? 1 : 0);
      this.wahMix.jump(p.wahOn ? 1 : 0);
      this.echoOnR.jump(p.echoOn ? 1 : 0);
      this.revMixR.jump(0);
      this.widthR.jump(p.width);
      this.ccVal.fill(0);
      this.speedState = p.speed;
      this.lastSpeedParam = p.speed;
      this.momentaryFast = this.lastWheelFast = false;
      this.speedCCValue = 0;
      this.rotorsStarted = false;
      this.hornAngle = 0;
      this.drumAngle = 0;
      this.hornHz = this.drumHz = 0;
    }

    static latencySamples(ampOS) { return OversampledShaper.latency(ampOS === 2 ? 4 : (ampOS === 1 ? 2 : 1)); }

    setParams(p) { this.params = p; }

    // ---- MIDI ----------------------------------------------------------------
    noteOn(note, vel, ch) {
      if (vel <= 0) { this.noteOff(note, ch); return; }
      this.mapNote(note, ch);
      const m = this.mapM, key = this.mapKey;
      if (m < 0) return;
      this.noteMap[((ch & 15) << 7) | (note & 127)] = m * kMaxKeys + key;
      this.keyOn(m, key, vel);
    }

    noteOff(note, ch) {
      const idx = ((ch & 15) << 7) | (note & 127);
      const slot = this.noteMap[idx];
      if (slot < 0) return;
      const m = (slot / kMaxKeys) | 0, key = slot % kMaxKeys;
      this.noteMap[idx] = -1;
      this.keyOff(m, key);
    }

    keyOn(m, key, vel) {
      if (m < 0 || m >= kManuals || key < 0 || key >= keyCount(m)) return;
      const ks = this.keys[m][key];
      if (ks.refs++ > 0) return;
      ks.down = true;
      if (m === 0 && this.downCount[0] === 0) this.percEnv = 1;
      ++this.downCount[m];
      this.scheduleContacts(m, key, true, vel);
      if (m === 1 && this.params.manualBass) this.updateManualBass(vel);
    }

    keyOff(m, key) {
      if (m < 0 || m >= kManuals || key < 0 || key >= keyCount(m)) return;
      const ks = this.keys[m][key];
      if (ks.refs <= 0) return;
      if (--ks.refs > 0) return;
      ks.down = false;
      --this.downCount[m];
      this.scheduleContacts(m, key, false, 0);
      if (m === 1 && this.params.manualBass) this.updateManualBass(100);
    }

    setController(cc, value, ch) {
      const p = this.params;
      if (cc >= 0 && cc < 128) this.ccVal[cc] = value;
      if (cc === 1) {
        this.modWheel = value;
        const up = value >= 0.5;
        if (p.speedMode !== 2) {
          if (p.speedWheel === 1 && up !== this.lastWheelFast) this.speedState = up ? 2 : 1;
          if (p.speedWheel === 2) this.momentaryFast = up;
        }
        this.lastWheelFast = up;
      }
      if (p.speedCC > 0 && cc === p.speedCC) {
        this.speedCCValue = value;
        if (p.speedMode === 1) this.speedState = value < 1 / 3 ? 0 : (value < 2 / 3 ? 1 : 2);
        else if (p.speedMode === 0) this.speedState = value >= 0.5 ? 2 : 1;
      }
      if (cc === 64 && p.speedSustain && p.speedMode !== 2) this.speedState = value >= 0.5 ? 2 : 1;
      if (cc === p.exprCC) this.expression = value;
      if (cc === 64) this.sustain = value >= 0.5;
      if (p.motorCC > 0 && cc === p.motorCC) this.motorCcOff = value >= 0.5;
      if (cc === 120) this.panic();
      if (cc === 123) this.allNotesOff();
      if (cc === 121) { this.modWheel = 0; this.expression = 1; this.sustain = false; }
    }

    setExpression(v) { this.expression = clampd(v, 0, 1); }

    allNotesOff() {
      this.noteMap.fill(-1);
      for (let m = 0; m < kManuals; ++m)
        for (let k = 0; k < keyCount(m); ++k)
          if (this.keys[m][k].refs > 0) {
            this.keys[m][k].refs = 1;
            this.keyOff(m, k);
          }
    }

    panic() { this.resetKeys(); }

    // ---- render ----------------------------------------------------------------
    render(L, R, numSamples) {
      let pos = 0;
      while (pos < numSamples) {
        if (this.chunkPos === 0) this.updateControl();
        while (this.heapSize > 0 && this.evTime[0] <= this.now) this.applyEvent(this.popEvent());
        let n = Math.min(numSamples - pos, kChunk - this.chunkPos);
        if (this.heapSize > 0) n = Math.min(n, this.evTime[0] - this.now);
        this.renderSamples(L, R, pos, n);
        pos += n;
        this.chunkPos = (this.chunkPos + n) % kChunk;
        this.now += n;
      }
    }

    // ---- state for the UI / tests ---------------------------------------------
    keyDown(m, key) { return this.keys[m][key].down; }
    modWheelValue() { return this.modWheel; }
    expressionValue() { return this.expression; }
    sustainDown() { return this.sustain; }
    motorValue() { return this.motorSpeed; }
    percussionLevel() { return this.percEnv; }
    hornTurns() { return this.hornAngle / kTwoPi; }
    drumTurns() { return this.drumAngle / kTwoPi; }
    hornSpeed() { return this.hornHz; }
    drumSpeed() { return this.drumHz; }
    speedSwitch() { return this.momentaryFast ? 2 : this.speedState; }
    takePeak() { const p = this.peak; this.peak = 0; return p; }
    pendingEvents() { return this.heapSize + this.numRamps; }
    wheelHz(w) { return wheelBaseHz(w) * this.freqFactor; }
    wheelPhase(w) { return Math.atan2(this.ws[w], this.wc[w]); }
    sampleTime() { return this.now; }

    // ---- MIDI -> manual / key ----------------------------------------------------
    mapNote(note, ch) {
      const p = this.params;
      let m = -1, key = -1;
      if (p.splitMode) {
        if (ch !== p.channel[0]) { this.mapM = -1; return; }
        if (p.pedalZone && note < p.pedalSplit) { m = 2; key = note - p.pedMidiBase + p.transpose; }
        else if (note < p.splitPoint) { m = 1; key = note - 36 + 12 * p.splitLoOct + p.transpose; }
        else { m = 0; key = note - 36 + p.transpose; }
      } else {
        for (let i = 0; i < kManuals; ++i)
          if (ch === p.channel[i]) { m = i; break; }
        if (m < 0) { this.mapM = -1; return; }
        key = m === 2 ? note - p.pedMidiBase + p.transpose : note - 36 + p.transpose;
      }
      if (key < 0 || key >= keyCount(m)) m = -1;
      this.mapM = m; this.mapKey = key;
    }

    updateManualBass(vel) {
      let lowest = -1;
      for (let k = 0; k < kManualKeys; ++k)
        if (this.keys[1][k].refs > 0) { lowest = k; break; }
      let pedal = lowest;
      while (pedal >= kPedalKeys) pedal -= 12;
      if (pedal === this.coupledPedal) return;
      if (this.coupledPedal >= 0) this.keyOff(2, this.coupledPedal);
      this.coupledPedal = pedal;
      if (pedal >= 0) this.keyOn(2, pedal, vel);
    }

    // ---- contacts: the key click ----------------------------------------------------
    scheduleContacts(m, key, close, vel) {
      const p = this.params, sr = this.sr, rng = this.rng;
      const ks = this.keys[m][key];
      ks.gen = (ks.gen + 1) >>> 0;
      const kw = this.wiring[m][key];
      if (!close && ((m === 2 && p.pedSustain) || (m === 1 && p.loSustain))) {
        const len = Math.max(2, Math.trunc((m === 2 ? p.pedDecay : p.loSusTime) * sr));
        for (let i = 0; i < kw.count; ++i)
          this.pushEvent(this.now, ks.gen, m, key, i, len, 0, 1);
        return;
      }
      let c = p.click[m];
      if (!close && !p.keyUpClick) c = 0;
      let target = close ? 1 : 0;
      if (close && p.velMode !== 0) {
        const v = 1 - p.velAmount + p.velAmount * vel / 127;
        if (p.velMode === 1) target = v;
        else c *= v;
      }
      const msToSmp = sr / 1000;
      const spread = (0.2 + 3.8 * c) * msToSmp;
      const ramp = Math.trunc((1 - c) * 3 * msToSmp);
      for (let i = 0; i < kw.count; ++i) {
        const from = ks.contact[i];
        const t0 = this.now + Math.trunc(rng.uni() * spread);
        this.pushEvent(t0, ks.gen, m, key, i, ramp, target, 0);
        let t = t0 + ramp;
        const bounces = c > 0 ? Math.trunc(rng.uni() * (1 + 3 * c)) : 0;
        for (let b = 0; b < bounces; ++b) {
          const flip = t + 1 + Math.trunc((0.03 + 0.25 * rng.uni()) * c * msToSmp);
          t = flip + 1 + Math.trunc((0.03 + 0.25 * rng.uni()) * msToSmp);
          this.pushEvent(flip, ks.gen, m, key, i, 0, from, 0);
          this.pushEvent(t, ks.gen, m, key, i, 0, target, 0);
        }
      }
    }

    applyEvent(e) {
      const ks = this.keys[e.manual][e.key];
      if (e.gen !== ks.gen) return;
      if (e.ramp <= 1) {
        this.stopRamp(e.manual, e.key, e.contact);
        this.setContact(e.manual, e.key, e.contact, e.level);
        return;
      }
      let slot = ks.rampSlot[e.contact];
      if (slot < 0) {
        if (this.numRamps >= kMaxRamps) { this.setContact(e.manual, e.key, e.contact, e.level); return; }
        slot = this.numRamps++;
        ks.rampSlot[e.contact] = slot;
      }
      const from = ks.contact[e.contact];
      const r = this.ramps[slot];
      r.manual = e.manual; r.key = e.key; r.contact = e.contact;
      r.from = from; r.to = e.level; r.pos = 0; r.len = e.ramp; r.shape = e.shape;
      r.cur = from; r.mult = e.shape === 1 ? Math.exp(-6.907755278982137 / e.ramp) : 1;
    }

    stopRamp(m, key, contact) {
      const ks = this.keys[m][key];
      const slot = ks.rampSlot[contact];
      if (slot < 0) return;
      ks.rampSlot[contact] = -1;
      if (slot !== --this.numRamps) {
        const src = this.ramps[this.numRamps], r = this.ramps[slot];
        r.manual = src.manual; r.key = src.key; r.contact = src.contact; r.from = src.from; r.to = src.to;
        r.pos = src.pos; r.len = src.len; r.shape = src.shape; r.cur = src.cur; r.mult = src.mult;
        this.keys[r.manual][r.key].rampSlot[r.contact] = slot;
      }
    }

    stepRamps() {
      for (let i = 0; i < this.numRamps;) {
        const r = this.ramps[i];
        ++r.pos;
        const m = r.manual, key = r.key, contact = r.contact;
        const done = r.pos >= r.len;
        let lvl;
        if (done) lvl = r.to;
        else if (r.shape === 1) { r.cur *= r.mult; lvl = r.cur; }
        else {
          const x = r.pos / r.len;
          lvl = r.from + (r.to - r.from) * (x * x * (3 - 2 * x));
        }
        this.setContact(m, key, contact, lvl);
        if (done) this.stopRamp(m, key, contact);
        else ++i;
      }
    }

    setContact(m, key, i, lvl) {
      const ks = this.keys[m][key];
      const old = ks.contact[i];
      if (old === lvl) return;
      const c = this.wiring[m][key].c[i];
      const g = this.contactGain[m][key][i];
      const delta = lvl * g - old * ks.applied[i];
      ks.contact[i] = lvl;
      ks.applied[i] = lvl === 0 ? 0 : g;
      const row = this.busG[m][c.bus];
      row[c.wheel] += delta;
      const lk = this.busLeak[m];
      const prt = wheelPartner(c.wheel);
      if (prt) lk[c.bus][prt] += delta;
      if (c.bus > 0) lk[c.bus - 1][c.wheel] += 0.5 * delta;
      if (c.bus + 1 < busCountOf(m)) lk[c.bus + 1][c.wheel] += 0.5 * delta;
      const bc = this.busCount[m];
      if (old === 0 && lvl !== 0) { ++bc[c.bus]; ++this.manualContacts[m]; }
      else if (old !== 0 && lvl === 0) {
        if (--bc[c.bus] === 0) row.fill(0);
        if (--this.manualContacts[m] === 0) for (let b = 0; b < kBuses; ++b) lk[b].fill(0);
      }
    }

    resetKeys() {
      this.heapSize = 0;
      this.numRamps = 0;
      this.noteMap.fill(-1);
      for (let m = 0; m < kManuals; ++m)
        for (let k = 0; k < kMaxKeys; ++k) {
          const ks = this.keys[m][k];
          ks.refs = 0; ks.down = false; ks.gen = (ks.gen + 1) >>> 0;
          ks.contact.fill(0); ks.applied.fill(0); ks.rampSlot.fill(-1);
        }
      for (let m = 0; m < kManuals; ++m) {
        for (let b = 0; b < kBuses; ++b) { this.busG[m][b].fill(0); this.busLeak[m][b].fill(0); }
        this.busCount[m].fill(0);
      }
      this.manualContacts.fill(0);
      this.downCount.fill(0);
      this.coupledPedal = -1;
    }

    // binary min-heap ordered by (time, seq)
    evCopy(dst, src) {
      this.evTime[dst] = this.evTime[src]; this.evSeq[dst] = this.evSeq[src]; this.evGen[dst] = this.evGen[src];
      this.evManual[dst] = this.evManual[src]; this.evKey[dst] = this.evKey[src]; this.evContact[dst] = this.evContact[src];
      this.evRamp[dst] = this.evRamp[src]; this.evLevel[dst] = this.evLevel[src]; this.evShape[dst] = this.evShape[src];
    }
    evSet(i, time, seq, gen, manual, key, contact, ramp, level, shape) {
      this.evTime[i] = time; this.evSeq[i] = seq; this.evGen[i] = gen;
      this.evManual[i] = manual; this.evKey[i] = key; this.evContact[i] = contact;
      this.evRamp[i] = ramp; this.evLevel[i] = level; this.evShape[i] = shape;
    }

    pushEvent(time, gen, manual, key, contact, ramp, level, shape) {
      const seq = this.seq;
      this.seq = (this.seq + 1) >>> 0;
      if (this.heapSize >= kEventCapacity) {   // full (never in practice): apply now
        const t = this.top;
        t.time = time; t.seq = seq; t.gen = gen; t.manual = manual; t.key = key; t.contact = contact; t.ramp = ramp; t.level = level; t.shape = shape;
        this.applyEvent(t);
        return;
      }
      let i = this.heapSize++;
      while (i > 0) {
        const parent = (i - 1) >> 1;
        const pt = this.evTime[parent];
        if (!(time < pt || (time === pt && seq < this.evSeq[parent]))) break;
        this.evCopy(i, parent);
        i = parent;
      }
      this.evSet(i, time, seq, gen, manual, key, contact, ramp, level, shape);
    }

    popEvent() {
      const t = this.top;
      t.time = this.evTime[0]; t.seq = this.evSeq[0]; t.gen = this.evGen[0]; t.manual = this.evManual[0]; t.key = this.evKey[0];
      t.contact = this.evContact[0]; t.ramp = this.evRamp[0]; t.level = this.evLevel[0]; t.shape = this.evShape[0];
      const last = --this.heapSize;
      const lt = this.evTime[last], ls = this.evSeq[last];
      let i = 0;
      for (;;) {
        let child = 2 * i + 1;
        if (child >= this.heapSize) break;
        if (child + 1 < this.heapSize) {
          const a = this.evTime[child + 1], b = this.evTime[child];
          if (a < b || (a === b && this.evSeq[child + 1] < this.evSeq[child])) ++child;
        }
        const ct = this.evTime[child];
        if (!(ct < lt || (ct === lt && this.evSeq[child] < ls))) break;
        this.evCopy(i, child);
        i = child;
      }
      if (this.heapSize > 0 && i !== last) this.evCopy(i, last);
      return t;
    }

    // ---- Age ------------------------------------------------------------------------
    updateAgeTables() {
      const p = this.params;
      const ag = Math.max(p.age[0], p.age[1]);
      if (ag !== this.genAge) {
        this.genAge = ag;
        for (let w = 1; w <= kNumWheels; ++w) {
          this.level[w] = wheelLevel(w) * (ag === 0 ? 1 : dbToGain(ag * SynthEngine.kAgeWheelDb * this.wheelRnd[w]));
          this.second[w] = ag * 0.02 * wheelHarmonicRoom(w);
          this.third[w] = wheelThird(w) + ag * 0.03 * wheelHarmonicRoom(w);
        }
      }
      for (let m = 0; m < kManuals; ++m) {
        const a = m === 2 ? p.age[1] : p.age[m];
        if (a === this.contactAge[m]) continue;
        this.contactAge[m] = a;
        for (let k = 0; k < kMaxKeys; ++k)
          for (let i = 0; i < kContacts; ++i) {
            const r = this.contactRnd[m][k][i];
            this.contactGain[m][k][i] = this.wiring[m][k].c[i].gain * (a === 0 ? 1 : dbToGain(a * SynthEngine.kAgeContactDb * r));
          }
      }
    }

    // ---- control rate (every 32 samples) --------------------------------------------
    updateControl() {
      const p = this.params, sr = this.sr;
      const dt = kChunk / sr;
      if (p.organModel !== this.builtModel) this.rebuildModel();

      let target = p.motorStart ? 1 : (p.motorRun && this.motorSpeed > 0.25 ? 1 : 0);
      if (this.motorCcOff) target = 0;
      const tau = target > this.motorSpeed ? 0.25 : 1.5;
      this.motorSpeed += (target - this.motorSpeed) * (1 - Math.exp(-dt / tau));
      if (target === 0 && this.motorSpeed < 1e-4) this.motorSpeed = 0;
      if (target === 1 && this.motorSpeed > 1 - 1e-9) this.motorSpeed = 1;

      const ff = p.tuneRatio * mainsRatio(p.mains) * this.motorSpeed;
      if (ff !== this.freqFactor) {
        this.freqFactor = ff;
        for (let w = 1; w <= kNumWheels; ++w) {
          const inc = kTwoPi * wheelBaseHz(w) * ff / sr;
          this.rc[w] = Math.cos(inc);
          this.rs[w] = Math.sin(inc);
        }
      }
      const wc = this.wc, ws = this.ws;
      for (let w = 1; w <= kNumWheels; ++w) {
        const k = 1.5 - 0.5 * (wc[w] * wc[w] + ws[w] * ws[w]);
        wc[w] *= k;
        ws[w] *= k;
      }
      this.ampInc = (this.motorSpeed - this.ampCur) / kChunk;
      this.updateAgeTables();

      const percActive = p.percOn && p.presetKey[0] === 11;
      this.percGain.set(percActive ? (p.percSoft ? SynthEngine.kPercSoft : 1) : 0);
      this.percBus = p.percThird ? 4 : 3;
      const percT = p.percSlow ? p.percSlowTime : p.percFastTime;
      this.percMult = Math.exp(-6.907755278982137 / (Math.max(0.01, percT) * sr));
      const upperScale = percActive && !p.percSoft ? SynthEngine.kPercNormalBars : 1;

      for (let m = 0; m < 2; ++m) {
        const pk = p.presetKey[m];
        for (let b = 0; b < kBuses; ++b) {
          let pos = 0;
          if (pk === 10 || pk === 11) pos = p.bars[m][pk - 10][b];
          else if (pk >= 1 && pk <= 9) pos = p.regs[m][pk - 1][b];
          if (m === 0 && percActive && b === 8) pos = 0;
          this.setBarTarget(m, b, drawbarGain(pos) * (m === 0 ? upperScale : 1));
        }
      }
      this.setBarTarget(2, 0, drawbarGain(p.pedalBars[0]));
      this.setBarTarget(2, 1, drawbarGain(p.pedalBars[1]));
      for (let b = 2; b < kBuses; ++b) this.setBarTarget(2, b, 0);

      for (let m = 0; m < kManuals; ++m) {
        const s = m === 2 ? 1 : m;
        this.leakEff[m] = p.leak[s] * (1 + p.age[s]) * SynthEngine.kLeakBase;
      }
      this.floorUp = p.leak[0] * (1 + p.age[0]);
      this.floorLo = p.leak[1] * (1 + p.age[1]);

      const rate = Scanner.kRateHz * mainsRatio(p.mains) * this.motorSpeed;
      if (rate !== this.scanRate) { this.scanRate = rate; this.scanner.setRate(rate, sr); }
      this.scanner.renormalize();
      this.vibU.set(p.vibUpper ? 1 : 0);
      this.vibL.set(p.vibLower ? 1 : 0);
      this.vibDepth.set(depthFor(p.vibMode));
      this.vibChorus.set(p.vibMode >= 3 ? 1 : 0);

      if (p.pedTone !== this.pedTone) {
        this.pedTone = p.pedTone;
        this.pedLp.setLowpass(120 * Math.pow(2, 4 * clampd(this.pedTone, 0, 1)), 0.7071067811865476, sr);
      }

      this.exprSmooth += (this.expression - this.exprSmooth) * (1 - Math.exp(-dt / 0.012));
      this.preamp.setExpression(p.wahOn && p.wahSrc === 1 ? 1 : this.exprSmooth, kChunk);

      if (!p.manualBass && this.coupledPedal >= 0) { this.keyOff(2, this.coupledPedal); this.coupledPedal = -1; }
      else if (p.manualBass && this.coupledPedal < 0 && this.downCount[1] > 0) this.updateManualBass(100);

      this.updateCabinet(dt);
    }

    // ---- rotary cabinet, amp, outboard, reverb (control rate) -------------------------
    updateCabinet(dt) {
      const p = this.params, sr = this.sr;
      const M = cabinetModel(p.cabModel);

      if (p.speed !== this.lastSpeedParam) { this.speedState = p.speed; this.lastSpeedParam = p.speed; }
      let state = this.momentaryFast ? 2 : this.speedState;
      if (p.speedMode === 0 && state === 0) state = 1;
      let variable = 0;
      if (p.speedMode === 2)
        variable = clampd(p.varSource === 0 ? this.modWheel : (p.varSource === 1 ? this.exprSmooth : this.speedCCValue), 0, 1);

      const stationary = p.cabBypass || p.rotorConfig === 3 || M.stationary;
      for (let i = 0; i < 2; ++i) {
        const horn = i === 0;
        const slow = horn ? p.hornSlow : p.drumSlow, fast = horn ? p.hornFast : p.drumFast;
        const accel = horn ? p.hornAccel : p.drumAccel, brake = horn ? p.hornBrake : p.drumBrake;
        let target = p.speedMode === 2 ? slow + (fast - slow) * variable
                                       : (state === 2 ? fast : (state === 1 ? slow : 0));
        const sel = horn ? p.hornSel : p.drumSel;
        if (sel === 1) target = 0;
        else if (sel === 2) target = slow;
        else if (sel === 3) target = fast;
        if (!horn && p.drumOff) target = 0;

        let hz = horn ? this.hornHz : this.drumHz;
        let angle = horn ? this.hornAngle : this.drumAngle;
        if (stationary) {
          hz = 0;
          angle = M.stationary ? (horn ? 0.7 : -0.5) : 0;
        } else if (!this.rotorsStarted) hz = target;
        else {
          const spread = Math.max(0.01, Math.abs(fast - slow));
          const tauUp = accel / Math.log(Math.max(1.5, spread / (0.02 * Math.max(0.01, fast))));
          const tauDn = brake / Math.log(Math.max(1.5, spread / (0.02 * Math.max(0.01, slow))));
          const tau = target > hz ? tauUp : tauDn;
          hz += (target - hz) * (1 - Math.exp(-dt / Math.max(0.01, tau)));
          if (target === 0 && hz < 1e-4) hz = 0;
          if (horn && target === 0 && p.brakePos && hz < 0.4) {
            const dir = p.hornDir === 0 ? 1 : -1;
            let remaining = dir * (p.brakeAngle * kPi / 180 - angle);
            remaining -= kTwoPi * Math.floor(remaining / kTwoPi);
            if (remaining < 0.01 || remaining > kTwoPi - 0.01) { hz = 0; angle = p.brakeAngle * kPi / 180; }
            else hz = Math.min(Math.max(hz, 0.15), Math.max(0.15, 1.5 * remaining / kTwoPi));
          }
        }
        const dir = (horn ? p.hornDir : p.drumDir) === 0 ? 1 : -1;
        const inc = dir * kTwoPi * hz / sr;
        this.cab.setRotor(i, angle, inc);
        angle += inc * kChunk;
        angle -= kTwoPi * Math.floor(angle / kTwoPi);
        if (horn) { this.hornHz = hz; this.hornAngle = angle; } else { this.drumHz = hz; this.drumAngle = angle; }
      }
      this.rotorsStarted = true;

      const cs = this.cs;
      cs.model = p.cabModel; cs.rotorConfig = p.rotorConfig; cs.hornType = p.hornType; cs.hornMouth = p.hornMouth;
      cs.drumBaffle = p.drumBaffle; cs.cabBack = p.cabBack; cs.micLayout = p.micLayout; cs.xover = p.xover;
      cs.balance = p.hornDrumBal; cs.micDist = p.micDist; cs.micAngle = p.micAngle; cs.micHorn = p.micHorn;
      cs.micDrum = p.micDrum; cs.micRoom = p.micRoom;
      this.cab.configure(cs);

      this.tubeGain = dbToGain(30 * clampd(p.drive, 0, 1) + M.ampGainDb);
      this.amp.setBias(M.ampBias);
      this.ampNorm = 1 / (this.tubeGain * this.amp.s1.slope());
      this.ampHpA = onePoleCoef(M.ampHpHz, sr);
      if (M.ampLpHz !== this.ampLpHz) { this.ampLpHz = M.ampLpHz; this.ampLp.setLowpass(this.ampLpHz, 0.7071067811865476, sr); }
      this.ampFactor = p.ampOS === 2 ? 4 : (p.ampOS === 1 ? 2 : 1);
      this.ampPowA = 1 - Math.exp(-1 / (0.3 * sr));
      if (this.ampPin > 1e-12 && this.ampPout > 1e-12) this.ampMakeup.set(clampd(Math.sqrt(this.ampPin / this.ampPout), 1, 20));
      else this.ampMakeup.set(this.ampMakeup.target);
      if (this.ampFactor !== this.lastAmpFactor) { this.amp.reset(); this.lastAmpFactor = this.ampFactor; }

      this.od.set(p.odDrive, p.odTone, p.odLevel, sr);
      this.odMix.set(p.odOn ? 1 : 0);
      const wahPos = p.wahSrc === 1 ? this.exprSmooth : (p.wahSrc === 2 ? this.modWheel : (p.wahSrc === 3 ? this.ccVal[clampi(p.wahCC, 0, 127)] : p.wahPos));
      this.wah.setPosition(wahPos, sr);
      this.wahMix.set(p.wahOn ? 1 : 0);
      this.echo.set(p.echoTime, p.echoFb, p.echoMix, p.echoWow, p.echoTone);
      this.echoOnR.set(p.echoOn ? 1 : 0);

      const builtIn = (p.cabBypass ? 0 : M.reverbMix) + (p.organModel === 2 ? 0.15 : 0);
      this.revMixR.set(clampd(builtIn + (p.revOn ? p.revMix : 0), 0, 1));
      const decay = p.revOn ? p.revDecay : 2.2, tone = p.revOn ? p.revTone : 0.5;
      if (decay !== this.revSet[0] || tone !== this.revSet[1]) { this.revSet[0] = decay; this.revSet[1] = tone; this.rev.set(decay, tone, sr); }
      this.widthR.set(p.width);
    }

    rebuildModel() {
      this.builtModel = this.params.organModel;
      this.resetKeys();
      buildManualWiring(this.wiring[0], this.builtModel, 0);
      buildManualWiring(this.wiring[1], this.builtModel, 1);
      buildPedalWiring(this.wiring[2], this.builtModel);
      this.contactAge[0] = this.contactAge[1] = this.contactAge[2] = -1;
      this.updateAgeTables();
    }

    setBarTarget(m, b, g) {
      this.barCur[m][b] = this.barTarget[m][b];
      this.barTarget[m][b] = g;
      this.barInc[m][b] = (g - this.barCur[m][b]) / kChunk;
    }

    // ---- audio --------------------------------------------------------------------
    renderSamples(L, R, off, n) {
      const p = this.params;
      const outGain = SynthEngine.kOutScale * p.ampGain;
      const floorOn = this.floorUp > 0 || this.floorLo > 0;
      const x = this.x, out = this.out;
      const wc = this.wc, ws = this.ws, rc = this.rc, rs = this.rs, level = this.level, second = this.second, third = this.third, floorW = this.floorW;
      const kStage = SynthEngine.kStage;
      for (let i = 0; i < n; ++i) {
        if (this.numRamps > 0) this.stepRamps();

        const motorAmp = this.ampCur;
        this.ampCur += this.ampInc;
        let floorSum = 0;
        for (let w = 1; w <= kNumWheels; ++w) {
          const c = wc[w], s = ws[w];
          const c2 = c * rc[w] - s * rs[w];
          const s2 = s * rc[w] + c * rs[w];
          wc[w] = c2;
          ws[w] = s2;
          const h2 = second[w], h3 = third[w];
          let v = s2;
          if (h2 !== 0) v += h2 * 2 * s2 * c2;
          if (h3 !== 0) v += h3 * (3 * s2 - 4 * s2 * s2 * s2);
          x[w] = motorAmp * level[w] * v;
          floorSum += floorW[w] * x[w];
        }
        if (!floorOn) floorSum = 0;

        out[0] = 0; out[1] = 0; out[2] = 0;
        let percBusValue = 0;
        for (let m = 0; m < kManuals; ++m) {
          const le = this.leakEff[m];
          const barCur = this.barCur[m], barInc = this.barInc[m], busCount = this.busCount[m];
          for (let b = 0; b < kBuses; ++b) {
            const g = barCur[b];
            barCur[b] = g + barInc[b];
            if (busCount[b] === 0) continue;
            const row = this.busG[m][b];
            let bus = 0;
            if (le === 0)
              for (let w = 1; w <= kNumWheels; ++w) bus += row[w] * x[w];
            else {
              const lk = this.busLeak[m][b];
              for (let w = 1; w <= kNumWheels; ++w) bus += (row[w] + le * lk[w]) * x[w];
            }
            if (m === 0 && b === this.percBus) percBusValue = bus;
            out[m] += g * bus;
          }
        }
        const up = out[0] + this.floorUp * floorSum, lo = out[1] + this.floorLo * floorSum;

        const perc = this.percGain.next() * this.percEnv * percBusValue;
        this.percEnv *= this.percMult;

        const vu = this.vibU.next(), vl = this.vibL.next();
        const vibIn = up * vu + lo * vl;
        const dry = up * (1 - vu) + lo * (1 - vl);
        const cm = this.vibChorus.next();
        const scanned = this.scanner.process(vibIn, this.vibDepth.next());
        const vib = scanned * (1 - 0.5 * cm) + vibIn * 0.5 * cm;

        const pd = this.pedLp.process(out[2]);
        let sig = this.preamp.process(dry + vib + perc + pd);

        sig += this.odMix.next() * (this.od.process(sig * kStage) / kStage - sig);
        sig += this.wahMix.next() * (this.wah.process(sig) - sig);
        sig += this.echoOnR.next() * (this.echo.process(sig * kStage) / kStage - sig);

        let l, r;
        if (p.cabBypass) l = r = sig;
        else {
          this.ampHp += this.ampHpA * (sig - this.ampHp);
          const inp = sig - this.ampHp;
          const raw = this.amp.process(this.tubeGain * kStage * inp, this.ampFactor) * this.ampNorm / kStage;
          this.ampPin += this.ampPowA * (inp * inp - this.ampPin);
          this.ampPout += this.ampPowA * (raw * raw - this.ampPout);
          const a = this.ampLp.process(raw * this.ampMakeup.next());
          this.cab.process(a);
          l = this.cab.L; r = this.cab.R;
        }

        this.rev.process(0.5 * (l + r));
        const rm = this.revMixR.next();
        l += rm * this.rev.wl;
        r += rm * this.rev.wr;
        const wd = this.widthR.next();
        const mid = 0.5 * (l + r), side = 0.5 * (l - r) * wd;
        l = mid + side;
        r = mid - side;

        this.masterGain += (p.master - this.masterGain) * SynthEngine.kMasterSmooth;
        const g = outGain * this.masterGain;
        if (R !== null) {
          const fl = Math.fround(safetyClip(l * g)), fr = Math.fround(safetyClip(r * g));
          L[off + i] = fl;
          R[off + i] = fr;
          this.peak = Math.max(this.peak, Math.max(Math.abs(fl), Math.abs(fr)));
        } else {
          const f = Math.fround(safetyClip(0.5 * (l + r) * g));
          L[off + i] = f;
          this.peak = Math.max(this.peak, Math.abs(f));
        }
      }
    }
  }
  SynthEngine.kOutScale = 0.06;
  SynthEngine.kPercNormalBars = 0.60512;
  SynthEngine.kPercSoft = 0.5012;
  SynthEngine.kLeakBase = 0.0316;
  SynthEngine.kFloorBase = 0.0005;
  SynthEngine.kAgeContactDb = 3.0;
  SynthEngine.kAgeWheelDb = 2.0;
  SynthEngine.kStage = 0.06;
  SynthEngine.kMasterSmooth = 0.002;

  // ============================================ EngineParamsFromValues.h ===
  // Raw parameter values (by id) -> the engine's snapshot. Values are rounded to float first,
  // as the plugin's parameters are floats. The fixed preset-key registrations (p.regs) are not
  // parameters: the processor fills them.
  const drawbarId = (manual, set, foot) => `${manual ? 'lo' : 'up'}${set ? 'B' : 'A'}${foot + 1}`;
  function makeEngineParams(values, p) {
    const num = (id) => Math.fround(+values[id]);
    const choice = (id, max) => clampi(lround(num(id)), 0, max);
    const on = (id) => num(id) >= 0.5;
    const inum = (id) => lround(num(id));

    p.organModel = choice('organModel', 3);
    p.ampGain = Math.pow(10, num('ampLevel') / 20);
    p.master = num('master');
    p.tuneRatio = num('tuneA') / 440;
    p.mains = choice('mains', 1);
    p.transpose = inum('transpose');
    p.motorStart = on('motorStart');
    p.motorRun = on('motorRun');
    p.velMode = choice('velMode', 2);
    p.velAmount = num('velAmount');

    for (let m = 0; m < 2; ++m)
      for (let s = 0; s < 2; ++s)
        for (let f = 0; f < kNumFootages; ++f)
          p.bars[m][s][f] = num(drawbarId(m, s, f));
    p.pedalBars[0] = num('pd16');
    p.pedalBars[1] = num('pd8');
    p.presetKey[0] = choice('upKey', 11);
    p.presetKey[1] = choice('loKey', 11);

    p.click[0] = num('clickUp');
    p.click[1] = num('clickLo');
    p.click[2] = num('clickPd');
    p.keyUpClick = on('keyUpClick');

    p.percOn = on('percOn');
    p.percSoft = choice('percVol', 1) === 1;
    p.percSlow = choice('percDecay', 1) === 1;
    p.percThird = choice('percHarm', 1) === 1;
    p.percFastTime = num('percFast');
    p.percSlowTime = num('percSlow');
    p.vibUpper = on('vibUpper');
    p.vibLower = on('vibLower');
    p.vibMode = choice('vibMode', 5);
    p.leak[0] = num('leakUp');
    p.leak[1] = num('leakLo');
    p.age[0] = num('ageUp');
    p.age[1] = num('ageLo');
    p.pedTone = num('pedTone');
    p.pedSustain = on('pedSustain');
    p.pedDecay = num('pedDecay');
    p.loSustain = on('loSustain');
    p.loSusTime = num('loSusTime');
    p.manualBass = on('manualBass');

    p.channel[0] = inum('chUpper') - 1;
    p.channel[1] = inum('chLower') - 1;
    p.channel[2] = inum('chPedal') - 1;
    p.splitMode = on('splitMode');
    p.splitPoint = inum('splitPoint');
    p.splitLoOct = inum('splitLoOct');
    p.pedalZone = on('pedalZone');
    p.pedalSplit = inum('pedalSplit');
    p.pedMidiBase = inum('pedMidiBase');
    p.motorCC = inum('motorCC');
    p.exprCC = inum('exprCC');

    p.speedWheel = choice('speedWheel', 2);
    p.speedCC = inum('speedCC');
    p.speedSustain = on('speedSustain');
    p.varSource = choice('varSource', 2);

    p.drive = num('drive');
    p.cabModel = choice('cabModel', 3);
    p.cabBypass = on('cabBypass');
    p.rotorConfig = choice('rotorConfig', 3);
    p.speedMode = choice('speedMode', 2);
    p.speed = choice('speed', 2);
    p.hornSel = choice('hornSpeedSel', 3);
    p.drumSel = choice('drumSpeedSel', 3);
    p.drumOff = on('drumOff');
    p.hornSlow = num('hornSlow'); p.hornFast = num('hornFast');
    p.hornAccel = num('hornAccel'); p.hornBrake = num('hornBrake');
    p.drumSlow = num('drumSlow'); p.drumFast = num('drumFast');
    p.drumAccel = num('drumAccel'); p.drumBrake = num('drumBrake');
    p.hornDir = choice('hornDir', 1);
    p.drumDir = choice('drumDir', 1);
    p.brakePos = on('brakePos');
    p.brakeAngle = num('brakeAngle');
    p.hornType = choice('hornType', 1);
    p.hornMouth = choice('hornMouth', 1);
    p.drumBaffle = choice('drumBaffle', 2);
    p.xover = num('xover');
    p.hornDrumBal = num('hornDrumBal');
    p.cabBack = choice('cabBack', 1);
    p.micLayout = choice('micLayout', 4);
    p.micDist = num('micDist');
    p.micAngle = num('micAngle');
    p.micHorn = num('micHorn');
    p.micDrum = num('micDrum');
    p.micRoom = num('micRoom');
    p.ampOS = choice('ampOS', 2);
    p.width = num('width');

    p.odOn = on('odOn'); p.odDrive = num('odDrive'); p.odTone = num('odTone'); p.odLevel = num('odLevel');
    p.wahOn = on('wahOn'); p.wahPos = num('wahPos'); p.wahSrc = choice('wahSrc', 3); p.wahCC = inum('wahCC');
    p.echoOn = on('echoOn'); p.echoTime = num('echoTime'); p.echoFb = num('echoFb'); p.echoMix = num('echoMix');
    p.echoWow = num('echoWow'); p.echoTone = num('echoTone');
    p.revOn = on('revOn'); p.revMix = num('revMix'); p.revDecay = num('revDecay'); p.revTone = num('revTone');
    return p;
  }

  // ======================================================= MidiRouting.h ===
  // How a raw MIDI message reaches the engine (the plugin's routeMidi). The drawbar CC maps, MIDI
  // learn and the keyswitches act on parameters, so they live in the host (web-host.js), as in the
  // plugin's processor.
  function routeMidi(e, d) {
    const st = d[0];
    if (st >= 0xf0 || st < 0x80) return;
    const ch = st & 15, cmd = st & 0xf0;
    if (cmd === 0x90 && d[2] > 0) e.noteOn(d[1], d[2], ch);
    else if (cmd === 0x80 || cmd === 0x90) e.noteOff(d[1], ch);
    else if (cmd === 0xb0) {
      if (d[1] === 120) e.panic();
      else if (d[1] === 123) e.allNotesOff();
      else e.setController(d[1], d[2] / 127, ch);
    }
  }

  // ==================================================== the audio node ===
  // Messages in: param {id, value} · params {values} · registrations {regs} · midi {data} ·
  //   noteOn / noteOff {note, vel, ch} (raw MIDI notes) · keyOn / keyOff {manual, key, vel} (the on-screen
  //   keyboards) · cc {cc, value, ch} · expression {value} · allNotesOff · panic
  // Messages out: stats (about 30 times a second): peak, sustain, modWheel, expression, motor, perc,
  //   speed, the rotor angles and speeds, and the keys held on each manual (two 32-bit masks each).
  class SynthProcessor extends AudioWorkletProcessor {
    constructor(options) {
      super();
      const po = (options && options.processorOptions) || {};
      this.values = Object.assign({}, po.params || {});
      this.regs = po.regs || null;
      this.engine = new SynthEngine();
      this.P = newEngineParams();
      this.dirty = true;
      this.applyParams();                  // parameters first, then prepare (as the plugin does)
      this.engine.prepare(sampleRate);
      this.statSamples = 0;
      this.statEvery = Math.max(128, Math.floor(sampleRate / 30));
      this.keyBits = new Uint32Array(6);
      this.port.onmessage = (e) => this.onMessage(e.data);
    }

    onMessage(m) {
      const e = this.engine;
      switch (m.type) {
        case 'param': this.values[m.id] = m.value; this.dirty = true; break;
        case 'params': Object.assign(this.values, m.values); this.dirty = true; break;
        case 'registrations': this.regs = m.regs; this.dirty = true; break;
        case 'midi': this.applyParams(); routeMidi(e, m.data); break;
        case 'noteOn': this.applyParams(); e.noteOn(m.note | 0, m.vel | 0, m.ch | 0); break;
        case 'noteOff': this.applyParams(); e.noteOff(m.note | 0, m.ch | 0); break;
        case 'keyOn': this.applyParams(); e.keyOn(m.manual | 0, m.key | 0, m.vel | 0); break;
        case 'keyOff': this.applyParams(); e.keyOff(m.manual | 0, m.key | 0); break;
        case 'cc': this.applyParams(); e.setController(m.cc | 0, +m.value, m.ch | 0); break;
        case 'expression': this.applyParams(); e.setExpression(+m.value); break;
        case 'allNotesOff': this.applyParams(); e.allNotesOff(); break;
        case 'panic': this.applyParams(); e.panic(); break;
      }
    }

    applyParams() {
      if (!this.dirty) return;
      makeEngineParams(this.values, this.P);
      if (this.regs)
        for (let m = 0; m < 2; ++m)
          for (let k = 0; k < 9; ++k)
            for (let f = 0; f < kNumFootages; ++f) {
              const row = this.regs[m] && this.regs[m][k];
              const v = row ? (row[f] | 0) : kFactoryRegistrations[k][f];
              this.P.regs[m][k][f] = clampi(v, 0, 8);
            }
      this.engine.setParams(this.P);
      this.dirty = false;
    }

    process(inputs, outputs) {
      const out = outputs[0];
      const L = out && out[0];
      if (!L) return true;
      const R = out.length > 1 ? out[1] : null;
      this.applyParams();
      this.engine.render(L, R, L.length);

      this.statSamples += L.length;
      if (this.statSamples >= this.statEvery) {
        this.statSamples = 0;
        const e = this.engine, bits = this.keyBits;
        for (let m = 0; m < 3; ++m) {
          let lo = 0, hi = 0;
          const n = keyCount(m);
          for (let k = 0; k < n; ++k)
            if (e.keyDown(m, k)) { if (k < 32) lo |= (1 << k); else hi |= (1 << (k - 32)); }
          bits[2 * m] = lo >>> 0; bits[2 * m + 1] = hi >>> 0;
        }
        this.port.postMessage({
          type: 'stats', peak: e.takePeak(), sustain: e.sustainDown(), modWheel: e.modWheelValue(), expression: e.expressionValue(),
          motor: e.motorValue(), perc: e.percussionLevel(), speed: e.speedSwitch(),
          hornTurns: e.hornTurns(), drumTurns: e.drumTurns(), hornHz: e.hornSpeed(), drumHz: e.drumSpeed(),
          keys: Array.from(bits),
        });
      }
      return true;
    }
  }

  registerProcessor('the-ham', SynthProcessor);
}
