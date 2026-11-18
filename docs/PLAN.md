# The Ham: build plan

Status (2026-11-18): plan approved. **Phase 1 done** (skeleton + tonewheel core); waiting for hand-testing before phase 2.

Phase 1 notes:
- The full parameter table (143 parameters, every phase) is defined and frozen now, so projects saved during testing keep
  loading. Parameters for later phases exist and are stored in presets but do nothing yet; their UI panels carry a
  "PHASE 2" / "PHASE 3" tag.
- **Wiring confirmed by two independent sources.** setBfree's default B-3 generator (`TG_91FB12`) gives the rule: wheel =
  key + 13 + footage offset (−12, +7, 0, +12, +19, +24, +28, +31, +36), below 13 up an octave, above 91 down. Electric
  Druid's foldback table gives the same ranges (16' 13–24 then 13–61; 2⅔' repeats the top C; 2' from key 56, 1⅗' from
  52, 1⅓' from 49, 1' from 44, 1-based). The test compares every key × footage on both manuals with Electric Druid's
  ranges, not with the rule. One correction to the extracted table: it lists the 1⅓' row as 44–91, 80–91 (60 keys). The
  top C's 1⅓' (wheel 104) has to fold twice, to wheel 80, as in the 1' row's second run, so the test expects that.
- **Wheels 1–12 are used only by the pedals**, and the bottom octave of the manuals' 16' folds up to wheels 13–24.
  Electric Druid: the lowest octave "are the complex tonewheels and are only available on the pedals".
- **Top-octave gearing confirmed.** Electric Druid's worked example "C# = 20 × 192 × (74/64) = 4440 Hz" uses the F♯
  ratio for C♯8. This is the shaft rule in decision 1 above.
- Key taper: setBfree's `taperingModel()` (steps of 3.5 dB from −10 to +7 dB per key range and footage), reproduced as data
  in `dsp/Wiring.h`.
- Pedal drawbars are composite mixtures (16' = 16' + 0.35 × 8' + 0.15 × 5⅓'; 8' = 8' + 0.30 × 4' + 0.12 × 2⅔'). This is
  The Ham's own approximation, because setBfree gives the pedals 9 plain busbars. It gets tuned by ear in phase 2.
- Wheel voicing: flat to wheel 48, then a gentle fall to −4 dB at wheel 91 (the filter-capacitor-loaded upper wheels).
  Wheels 1–12 carry a 3rd harmonic at −26 dB, and wheels 13–24 at −40 dB. These are approximations: no per-wheel factory
  data has been found yet.
- **Key click.** A key schedules 9 contact events, each at its own sample within 0.2–4 ms (by Click). Each contact either
  makes at once (a step) or ramps smoothly (smoothstep, up to 3 ms, + − × only), then bounces 0–3 times. Measured on a
  low 8' key: Click 100% puts the attack 37 dB above the steady tone in the high band, Click 0% 1.5 dB (no click). Key-up
  click on vs off: 42 dB. A press with full bounce schedules about 40 contact events. A binary heap holds up to 65 536
  pending events.
- Motor: Start spins the generator up (τ 0.25 s, 98% of speed in 1 s). Run keeps it turning but cannot start it from
  rest. With both off, or the Motor Off CC, it coasts down (τ 1.5 s). Pickup output is proportional to speed, so the
  level dives with the pitch.
- **Drawbar CC maps**, from Apple's EVB3 manual (its "MIDI controller assignments" table lists the modes of the common
  hardware): B4D upper 12–20 / lower 21–29 (the default), Nord Electro 16–24 / 70–78, Roland VK / Korg CX-3 70–78. I
  found no citable CC tables for Hammond SK or Crumar, so they aren't in the list yet. Choice lists are append-only, so
  they can be added later. A mapped CC moves the active set's drawbar through the message thread, so the host and the
  UI see it.
- Preset-key fixed registrations (C♯–A) use the B-3 factory chart as commonly published, on both manuals for now. Phase 2
  checks the chart and adds the lower manual's.
- UI notes travel as `{manual, key, vel}` through a lock-free FIFO into the next block, not through
  `MidiKeyboardState`, so the on-screen keyboards play their own manual whatever the MIDI channels or split are. The
  editor lights keys from the engine's own key state, sent as `notes {on: [[manual, key]…], off: […]}`.
- Measured: 10 starter presets level-matched to −18.02 .. −17.96 dB. CPU (`--bench`, Release, one core, 48 kHz):
  **2.5%** with all 61 + 61 + 25 keys held and every drawbar out, **1.3%** with one key (the generator always runs). That
  bench clips the safety limiter (peak 1.00) at the default patch level; it is a CPU test, not a playing case.
- Checks: 35 organ checks + preset / file / render / loudness tests (`TheHamTests`), `--host-check` (scan, MIDI + drawbar
  CC, automation, exact state restore, programs), `check-params.ps1` (143 parameters match), and a headless-Edge smoke
  test of the UI events with a mocked bridge (drawbar wheel, typed registration, preset key, on-screen key, host-side
  drawbar change, note lighting). The UI smoke test is not committed yet. It becomes part of `run-ui-tests.ps1` in phase 6.

What to listen for in Minihost Modular (phase 1):
- **Phase-locked chords:** hold a chord and add notes. Nothing restarts, and repeated notes join the waveform where it
  already is. A fast glissando or palm smear sounds even, with no stutter.
- **Key click:** Click Upper at 100% gives a clear, slightly different click on every press. At 0% notes start
  softly. Key-Up on/off changes the release.
- **Foldback:** play `000000008` (1' only) and climb the keyboard. From the G above the second-highest C the pitch drops
  an octave and climbs again. Play `800000000` (16' only) at the bottom octave: it folds up and doesn't go below the
  second C.
- **Not quite equal temperament:** a sustained `888000000` chord beats very slowly, as on the real organ.
- **Motor:** switch Start and Run off (PLAY · MIDI page) for the power-off pitch dive. Switch Start on to spin up.
- **Drawbars** at 3 dB per step; typed registrations; preset keys C (silent), C♯–A, A♯ / B; a hardware drawbar
  controller in B4D mode (CC 12–20 / 21–29).
- Not yet: percussion, vibrato/chorus, leakage, age, preamp/expression (phase 2), and the rotary cabinet and FX
  (phase 3). The output is the dry generator, mono on both channels.

The Ham is a tonewheel organ modeled on the B-3 (91 tonewheels, wired keys with foldback, key click from contact
bounce, percussion, scanner vibrato/chorus), played through rotary cabinets modeled on the 122 / 147, the 145, a
Vibratone-style single rotor, and a stationary tone cabinet. It is a VST3 instrument plus a single-file web build,
in the same house style as E-Piano and Brass Knuckles (see [CONVENTIONS.md](CONVENTIONS.md)).

## Decisions that need your approval

These depart from the brief or from the conventions, or settle something the brief leaves open.

1. **Top-octave gearing (a correction to the brief).** The brief says "the top octave uses 192 teeth". Wheels
   85–91 (C8–F♯8) do use 192 teeth, but they sit on the shafts of **F, F♯, G, G♯, A, A♯ and B**, a fifth below
   (192/128 = 3/2). If the C–F♯ ratios are used instead, wheel 85 comes out at 3138.5 Hz (G7) instead of 4189.1 Hz
   (C8). I computed all 91 wheels with the corrected rule (motor 20 rev/s):
   wheel 1 = 32.6923 Hz, wheel 46 = **440.0000 Hz**, wheel 85 = 4189.0909 Hz, wheel 91 = 5924.5714 Hz. The deviation
   from equal temperament runs from −0.71 to +1.98 cents. Phase 1 checks this against the published table.
2. **Key contacts are sample-accurate** (the only exception to 32-sample ramps). The click is the real mechanism:
   each of the 9 contacts closes or opens at its own sample, with seeded bounce. **Click** scales the bounce count,
   the contact timing spread and how hard a contact makes (a step, or a smooth ramp of up to 3 ms). At 0 the contacts
   ramp smoothly within 0.2 ms of each other: no click. No noise or sample is added at any setting.
3. **Latency:** 1× and 2× report 0 (2× uses minimum-phase IIR half-bands). 4× uses Brass Knuckles' linear-phase FIRs
   and reports their latency to the host.
4. **Preset-key registrations (C♯–A), the MIDI-learn map and custom CC maps are stored as state and preset extras, not
   host parameters.** As parameters they would add about 200 entries to the host's list.
5. **MIDI note mapping:** manual key 1 (the lowest C, 8' = wheel 13 = 65.4 Hz) = MIDI 36. Pedal 1 = MIDI 36 by
   default, set by a **Pedal MIDI Base** parameter because MIDI pedalboards disagree. Preset-key keyswitches (off by
   default) use MIDI 24–35 on each manual's channel.
6. **"Randomize drawbars within the categories"** is read as: randomize inside a registration family (flute,
   diapason, reed or string shapes, each a set of rules on which footages may be up and how far). Each family has
   its own button.
7. **"Normal" percussion volume lowering the drawbars** and the **percussion decay times** start at the brief's values.
   They are then checked against the service manual's description and against reference recordings, and the final
   values are recorded here.

## Architecture

```
MIDI ch1 / ch2 / ch3, or one keyboard in Split mode ──► MidiRouting.h ──► keys
  upper 61 · lower 61 · pedals 25, each key 9 contacts (bounce, seeded Rng) ──► per-sample contact gates
  preset keys (C cancel · C♯–A fixed · A♯ set A · B set B), lower→pedal coupling, sustain options

Motor (Start/Run, 60/50 Hz, A=430–450, pitch dive) ──► wheel speed (control rate)
91 tonewheels, always running, phase-continuous, shared by every key   (dsp/Tonewheels.h)
  rotating phasors · per-wheel voicing taper · 3rd harmonic on the low wheels (Chebyshev from the phasor)
  · filter-capacitor low-pass on the upper wheels · Age (level spread, harmonics, leakage)

busbars: for each manual and footage, Σ wheel × contact gate   (wiring chart incl. foldback: dsp/Wiring.h)
  + leakage: crosstalk from each wheel's partner wheels + a quiet transformer floor (Leakage control)
  ─► drawbars (set A / set B / fixed registration; step 8 = 0 dB, about −3 dB per step, 0 = off)

  upper ─┬─► vibrato bus (manuals switched on) ─► scanner: LC delay line taps, capacitive scanner ~6.87 Hz
  lower ─┘      V1/V2/V3 = scanned output, C1/C2/C3 = scanned + dry
  upper ─► percussion (2nd or 3rd harmonic busbar, set B only, single trigger, cancels 1', bypasses the scanner)
  pedals ─► composite pedal drawbars 16' / 8' ─► pedal tone filter, sustain/decay (bypass the scanner)

sum ─► preamp: expression (CC11; cuts treble and mids more than deep bass) ─► Drive
    ─► outboard (each optional): overdrive ─► wah (expression-assignable) ─► tape echo
    ─► cabinet (one shared engine, 4 models = data tables, dsp/Cabinets.h):
         tube amp (drive, ADAA, 1×/2×/4× OS, per-cabinet voicing) ─► crossover (800 Hz) + horn/drum balance
         horn rotor(s) and drum rotor, independent inertia (accel/brake), direction, brake position
           each: Doppler (modulated fractional delay), AM, directional (angle-dependent) filter,
                 early reflections from the cabinet walls; open/closed back; horn mouth; drum baffle/foam
         mics: mono · stereo horn + mono drum · wide stereo · 3-mic · room pair (distance, angle, mix)
         Tone Cabinet: no rotors, fixed speakers + built-in spring reverb
    ─► spring reverb (optional) ─► patch level × master ─► safety clip (linear below −1.9 dBFS)
rotor state (angle, speed) ─► 30 Hz `rotor` event ─► UI cabinet animation (requestAnimationFrame)
```

Key decisions:
- **Wheels are generated once, not per key.** Each sample costs 91 phasor rotations (plus a 3rd harmonic on the low
  wheels) and one sum per busbar, however many keys are down. Holding all 147 keys costs about the same as
  holding one, apart from contact events. The `--bench` case (61 + 61 + 25 keys held, Fast, 4×) is mostly the
  cabinet.
- **Per-wheel busbar gains, updated at contact events.** A manual's output is Σ over wheels of
  `wheel(t) × g[manual][wheel]`, where `g` sums (contact gate × drawbar gain × voicing) over every contact that
  uses that wheel. A key press changes 9 entries of `g`. A drawbar move changes all of them, ramped over 32 samples.
  Phase 1 measures whether the sum costs less as `91 × busbars` or as `91 × 3 manuals` with gains folded in, and
  keeps the faster one.
- **Phase continuity:** wheel phasors are never reset by keys. They start at seeded random phases in `prepare()`
  (identical in JS). Their magnitude is renormalized once per chunk with one `sqrt`, so they don't drift.
- **Scanner vibrato** is modeled as the real circuit: a ladder of LC low-pass sections, with the voltage at each tap
  read by a rotating capacitive scanner. The scanner mixes adjacent taps by plate overlap, following the
  plate-pattern table. The LC sections are discretized with the bilinear transform at the sample rate. Vibrato depth
  1–3 selects how far up the line the scan reaches. There is no LFO-driven delay.
- **Rotors** follow `dω/dt = (ω_target − ω) / τ`, with separate τ for acceleration and braking per rotor. The
  "ramp time" parameters are defined as the time from one rate to within 2% of the other, and τ is derived from
  that, so the test can check the parameter exactly. The angle is the integral of ω. Brake-to-position steers the
  horn's final coast so it stops at the chosen angle.
- **Cabinet models are data** (like Brass Knuckles' instrument tables). Selecting one loads its defaults into the
  cabinet parameters with a single gesture, and every option stays editable. The 50/60 Hz option also scales the
  scanner and the rotor motors (they are AC motors too). That is recorded as a deliberate choice.
- **Organ models** (B-3, C-3, A-100, M-3) select wiring, voicing and taper tables. C-3 changes only the UI
  drawing. A-100 adds its built-in amp and speakers (the Tone Cabinet path with its own voicing). M-3 has 44-key
  manuals, its own taper and foldback, and no 16' on the lower manual. Its generator is read from its service
  manual in Phase 3; if it uses fewer than 91 wheels, the unused ones are muted.
- Identity: company "TheHam", codes `Tham` / `Ham1`. JUCE pin 8.0.17-security, reused from
  `%LOCALAPPDATA%\EPianoVST\deps\juce-src` (present on this machine).
- **Parameter table**: about 170 parameters, defined and frozen in Phase 1. Groups: global (organ model, tuning,
  mains, transpose, motor, level), drawbars (upper A/B, lower A/B, pedal: 38), preset key per manual, percussion,
  vibrato, click / leakage / age per manual, pedals, preamp / expression, MIDI (channels, split points, pedal zone,
  coupling, velocity mode, CC map, speed source, keyswitches), cabinet (model, rotor configuration, speed mode,
  6 speed/ramp values per rotor, directions, brake, horn, drum, crossover, balance, back, mics, amp, OS), outboard
  pedals, reverb.

## Reference data (to fetch and cite in Phase 1)

| What | Source used | Status |
|---|---|---|
| Gear ratios and teeth | the brief; setBfree `src/tonegen.c` (`gears60ratios`, top octave `select += 5; teeth = 192`); Electric Druid, "Hammond organ tonewheels" (electricdruid.net/?p=316: motor 20 rev/s, ratio table, worked example for C♯8) | done (phase 1) |
| 91 wheel frequencies | computed from the gear formula; spot values published by Electric Druid (65.38 Hz lowest manual C, 2092.31 Hz highest manual C, 4440 Hz C♯8) | done: all 91 tested, A = 440.000 Hz, −0.71 .. +1.98 cents from ET |
| Key → wheel wiring, foldback, low-wheel use | setBfree `applyManualDefaults()` (`TG_91FB12`: the rule), cross-checked against Electric Druid's foldback table (the test's reference) | done (phase 1) |
| Key taper | setBfree `taperingModel()` | done (phase 1) |
| Pedal composites | none found; The Ham's approximation | tune by ear in phase 2 |
| Per-wheel voicing, capacitor-filtered wheels | none found yet; approximation in `dsp/Tonewheels.h` | look again in phase 2 (Age) |
| Fixed preset-key registrations | B-3 factory chart as commonly published (upper) | verify and add the lower chart in phase 2 |
| Scanner line and plate pattern | service manual vibrato schematic; setBfree `vibrato.c` (scanner 7.25 Hz default, V1–V3 depth) as a cross-check | phase 2 |
| Rotor speeds and ramps | 122 / 145 service data and measured recordings (brief's values as defaults) | phase 3 |
| Drawbar controller CC maps | Apple EVB3 manual, "MIDI controller assignments" (B4D, Nord Electro, VK / CX-3) | done (phase 1); Hammond SK and Crumar still to source |

setBfree is GPL-2. Only facts are taken from it (gear ratios, footage offsets, foldback limits, taper steps), as data and
with attribution. None of its code is copied.

## File tree

```
README.md  .gitignore  .gitattributes  start.bat  serve.ps1
docs/   CONVENTIONS.md  PLAN.md  index.html (generated)  screenshots/
web/index.html                      (generated by vst/webhost/build-web.ps1)
vst/
  CMakeLists.txt  build.bat  install.bat  README.md
  Source/
    Params.h                        parameter table (mirrors ui/js/params.js)
    EngineParamsFromValues.h  MidiRouting.h  CcMap.h (drawbar CC maps)
    SynthEngine.h                   engine: motor, wheels, keys/contacts, busbars, manuals, signal chain
    dsp/ Common.h  Tonewheels.h  Wiring.h  Registrations.h  Contacts.h  Percussion.h  Scanner.h
         Preamp.h  Oversampler.h  Outboard.h  Rotor.h  Cabinets.h  SpringReverb.h
    PresetManager.*  PluginProcessor.*  PluginEditor.*
  ui/  index.html  css/style.css  svg/ (cabinet drawings, if kept out of cabinet.js)
       js/ params.js engine-juce.js knob.js controls.js drawbar.js cabinet.js piano.js app.js
  webhost/ dsp-worklet.js  factory.js  web-host.js  build-web.ps1  dev.html
  presets/factory_presets.json
  tests/ PresetTests.cpp  EngineTools.cpp  OrganTests.cpp  HostCheck.cpp  Parity.cpp  TestRender.h
         parity_scenarios.json  parity-runner.js  parity-compare.js  run-parity.ps1
         check-params.ps1  level-match.ps1  ui-tests.js  run-ui-tests.ps1  js-bench.js
```

## Dev loop

- **Engine**: the Standalone app, plus `TheHamTests --render "<Category/Name>" out.wav [scenario]` for offline renders
  that I can measure and you can audition.
- **UI**: `HAM_UI_DIR=…\vst\ui` loads the UI from disk in the plugin or Standalone. `serve.ps1 -Ui` shows the UI in a
  browser. From Phase 6, `serve.ps1 -Dev` runs the real JS engine unbundled.
- **Hand tests** in Image-Line Minihost Modular after each phase.

## Phases (each ends with a summary + "what to listen for" in Minihost Modular)

1. **Skeleton + tonewheel core.** Repo layout, CMake, build/install, the full parameter table, bridge, preset
   manager (`.hampreset`, migration, user folder), the walnut UI shell with real `Drawbar` widgets (detents, pull-down
   shaft, footage colours, registration strings, Shift fine, wheel, arrows, double-click, gestures) and the three
   keyboards. Engine: motor and 91 wheels with gear-ratio tuning, 60/50 Hz, A=430–450, transpose, wiring table with
   foldback, drawbars, contacts with bounce (key click, key-up click), upper/lower/pedals on channels 1/2/3, Split,
   default drawbar CC map, velocity off by default.
   Tests: every wheel against the gear formula and the published table; every key × footage against the wiring
   chart (foldback included); a key press never resets a wheel's phase (two presses of the same key, phase compared
   with an unpressed render); A = 440.000 Hz at wheel 46; `--dump-params` diff; preset round trip; `--render`.
2. **Organ voice.** Percussion (single trigger: legato does not retrigger, retrigger only after every upper key is up;
   set B only; cancels 1'; Normal lowers the drawbars; decay Fast/Slow), scanner vibrato/chorus (V1–3, C1–3, per-manual
   switches), preamp and expression with frequency-dependent taper, Drive, Age, leakage, preset keys and keyswitches,
   drawbar sets A/B, pedal tone/sustain/decay, lower→pedal coupling, lower sustain (off by default), velocity
   options, MIDI learn.
   Tests: percussion trigger rules, decay times, 1' cancel; scanner depth order V1 < V2 < V3 (pitch deviation measured
   by zero crossings); chorus = vibrato + dry; expression darkens (centroid falls) as well as quietens.
3. **Rotary cabinets.** Shared rotor engine, the 4 cabinet models, all speed modes (2-speed, 3-speed with brake,
   Variable), separate horn/drum switching, Drum off, directions, brake position, every speaker-layout option, mic
   layouts, tube amp with oversampling (latency reported at 4×), spring reverb, motor Start/Run and pitch dive,
   outboard overdrive / wah / tape echo, organ models (B-3, C-3, A-100, M-3), Bypass.
   Tests: each rotor reaches its target speed within ±2% in its ramp time; the horn reaches Fast before the drum; the
   drum lags while braking; no clicks on Slow↔Fast (Brass Knuckles' click detector); mono sum of every cabinet × mic
   layout (no notch deeper than a limit set and recorded in this phase, in 1/3-octave bands vs the stereo sum);
   latency reported matches the measured delay.
4. **Animated UI.** The four cabinet SVGs (cross-fade on model change, layout options reflected: horn only, dual
   active horns, open back, mic stands), horn and drum driven by `rotor` events with `requestAnimationFrame`,
   motion blur / spoke ghosting at Fast, lit Stop/Slow/Fast switch, horn rpm readout, pause when hidden; drawbar
   animation on preset change, rocker tabs, expression pedal; pages DRAWBARS, VOICE, CABINET, PLAY/MIDI, FX with LEDs;
   A/B compare, undo/redo, randomize drawbars; MIDI-learn UI and CC-map menu.
   Check: the animation's CPU cost in the plugin window is measured and recorded.
5. **Presets + verification → VST v1.0 candidate.** The 50 presets (registrations from the brief as starting
   points; every cabinet in ≥ 6 presets; Single Rotor for psychedelic / garage / some blues, Rotor 45 for funk / comp /
   grind, Tone Cabinet for classic / theatre / hymn; ≥ 3 Variable-speed and ≥ 2 outboard presets), level-matched to
   ±1 dB; final registrations recorded in the README.
   Tests: everything above for every preset, aliasing of the drive at C6 (inharmonic energy below 18 kHz, as in
   Brass Knuckles), fast chromatic and white-key glissando scenarios (no stuck contacts, no clicks beyond the key
   clicks), `--bench` (61 + 61 + 25 keys, Fast, 4×), `--host-check`. README with screenshots.
6. **Web build.** `dsp-worklet.js` (line-for-line twin), `web-host.js` (including `rotor` events), `factory.js`,
   `build-web.ps1` (→ `web/index.html` and `docs/index.html`), `start.bat` / `serve.ps1`. Parity scenarios for every
   feature and every preset at Brass Knuckles' tolerance (1e-4). Headless UI tests, including one that feeds rotor
   events and checks that the drawn horn and drum angles follow them.
