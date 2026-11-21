# The Ham: build plan

Status (2026-11-20): plan approved. **Phases 1–6 done** (tonewheel core, organ voice, rotary cabinets, animated UI,
the 50 presets and verification, the web build); the presets and the web build await hand-testing.

Phase 6 notes (2026-11-20):
- **The web build**: `vst/webhost/` (`dsp-worklet.js`, `factory.js`, `web-host.js`, `build-web.ps1`, `dev.html`) →
  `web/index.html` and `docs/index.html` (byte-identical, 299 KB). `dsp-worklet.js` is the line-for-line twin of
  `SynthEngine.h`, `dsp/*.h`, `EngineParamsFromValues.h` and `MidiRouting.h`; nothing allocates while rendering (the
  contact-event heap is a struct of typed arrays, the ramps and pickups are preallocated objects). The processor's
  share of the plugin (drawbar CC maps, MIDI learn, keyswitches, the cabinet defaults, the registrations) lives in
  `web-host.js` on the main thread, as it lives in `PluginProcessor` and not in the engine. The worklet reports the
  stats, the rotor angles and the keys held about 30 times a second; the host turns them into the editor's `stats`,
  `rotor` and `notes` events, so `app.js` and `cabinet.js` run unchanged.
- **Parity** (`tests/Parity.cpp` → `--parity`, `parity-runner.js`, `parity-compare.js`, `run-parity.ps1`,
  `parity_scenarios.json`): 111 scenarios, 181 s of audio: 61 feature scenarios (every engine path; the list is in
  vst/README.md) and one phrase per factory preset (both chords and a pedal, the mod wheel up at 0.4 s). Event types:
  on, off, cc, manual (the on-screen keys), expression, param (a parameter change from a block on), presetKey, motor,
  speed, midi, allOff, panic. **Worst relative error 4.99·10⁻⁶** on the first run (tolerance 10⁻⁴, Brass Knuckles').
  The errors cluster near 5·10⁻⁶ where the drive and its level matching are active and near 10⁻⁸ elsewhere: `exp`,
  `pow` and `log` differ in the last bits between MSVC's CRT and V8, nothing structural. What had to match exactly:
  the parameters are applied before `prepare()` (as the plugin does), every parameter value is rounded to float
  (`Math.fround`, the plugin's parameters are floats), and the JS processor applies pending parameter values before
  every message that touches the engine (the C++ side applies a `param` event at once).
- **Speed**: the C++ engine renders the scenarios in 6.4 s, the JS engine in headless Edge in 29.4 s (4.6×, with a
  fresh engine and JIT warm-up per scenario). Steady state in Node (`tests/js-bench.js`): one key **6.6%** of one
  core, a six-key trio chord 9.5–9.7% on Trio Classic / Full Organ / Grind Comp / Psychedelic Swirl / Bach Plenum,
  the `--bench` worst case **10.4%** (C++: 3.3% / 4.9%). The generator always runs, so the cost barely depends on
  the keys down.
- **UI tests** (`tests/web-tests.js`; `run-ui-tests.ps1` now runs the plugin UI and the web build, from disk and
  served): 38 checks from disk (ScriptProcessor, Edge's virtual time) and 45 served (AudioWorklet in real time),
  listed in vst/README.md. The served run includes the cabinet drawing following the engine's own rotor events: the
  drawn horn spins at 6.69 Hz against the engine's 6.70, within 14° of each report. Reports jitter by an audio
  buffer (the audio thread renders ahead of the ear in bursts, so a report's angle is a few ms ahead of its arrival)
  and the drawing smooths that over 80 ms; the plugin-UI test, which feeds events from the page's own clock, tracks
  to 0.2°.
- Verification, all passing: `run-parity.ps1` (111 scenarios), `run-ui-tests.ps1` (111 checks: 28 + 38 + 45), the C++
  suite unchanged (presets, `--organ`, `--voice`, `--cabinet`, renders, loudness).

Fix after phase 4, second round (hand-testing: "the rotary speaker seems always active and is audibly ignorant of
parameter changes"):
- **The rotor was working.** A new check drives the *built* VST3 through the host API with a note held and measures
  the level modulation the mics see (`--host-rotor`): Fast swings 11.5 dB at 6.70 Hz (Horn Fast 6.7), Slow 11.6 dB at
  0.80 Hz, 3-speed Stop 0.27 dB, Horn Fast set to 10 Hz → 10.00 Hz, Bypass 0.18 dB, Tone Cabinet 0.23 dB. The engine,
  the parameter path (UI → `setParam` → `setValueNotifyingHost` → `readParams()` every block) and the plugin all agree.
- **What was heard was the scanner chorus.** The factory `organ` template switched **C3 on both manuals** into every
  preset. Measured on a single 880 Hz tone with the cabinet bypassed (`--mod-probe`): C3 swings the level **21.0 dB at
  13.7 Hz** (twice the 6.87 Hz scanner rate: the scanned and the straight signal cancel twice per turn), C1 7.1 dB at
  6.9 Hz, V3 0.3 dB (pure vibrato). The rotor on the same tone: Rotor 22 Slow 14.2 dB at 0.72 Hz, Fast 18.7 dB at
  6.70 Hz. On an 888 chord: C3 alone 10.7 dB at 6.86 Hz, Rotor 22 Slow 6.6 dB, Slow + C3 10.0 dB, Fast 6.5 dB at
  6.70 Hz, Fast + C3 9.9 dB (the two rates beat). So with C3 on, a 7 Hz "rotary" is always there, and no CABINET
  control touches it; switching Slow ↔ Fast adds a second modulation at nearly the same rate, which is hard to hear as
  a change. That matches the phase-4 report too ("the cabs are not changing the sound").
- Fixes: the template leaves the vibrato **off** and every preset states it (C3 on the jazz presets where it belongs,
  C1 / C2 / V-modes elsewhere, off on the shout and rock presets); in **2-Speed** mode the STOP segment is greyed out
  (the engine runs Slow for it, as before, but a click that can't change anything no longer looks ignored), and in
  **Variable** mode the whole switch is (the wheel / pedal / CC sets the speed); a troubleshooting entry in
  `vst/README.md`. Not changed: the chorus depth. Whether 21 dB on a single note is right for C3 is a by-ear question
  (the plan lists the scanner depths as estimates); `Scanner::kSectionHz`, the 6/12/18 table and the 50:50 chorus mix
  are the knobs.
- Also: the installed plugin (07:00) predates the last `dsp/Cabinets.h` voicing; run `vst\install.bat` after building.

Phase 5 notes:
- **50 presets** (5 categories × 10) in `presets/factory_presets.json`, from **templates**: `organ` (set B, pedals
  5/3, vibrato off, Drive 20%) and one per cabinet (`cab22`, `cab45`, `cabSingle`, `cabTone`) carrying that model's
  defaults from `dsp/Cabinets.h`; the preset test fails if they drift apart. The brief's registrations are not in the
  repo, so the registrations are the commonly published classic settings for each style (88 8000 000 jazz,
  88 8800 000 soul, 88 8888 888 full organ, 68 8600 000 baroque rock, 80 0008 000 hollow, 00 8000 000 flute, the
  reed / string / diapason shapes from the randomizer's families …), listed in the README to tune by ear.
  Rules met and tested: Rotor 22 in 27 presets, Rotor 45 in 9 (funk / comp / grind), Single Rotor in 6 (psychedelic /
  garage / blues), Tone Cabinet in 8 (hymn / classic / theatre); 3 Variable-speed (mod wheel); 4 outboard (overdrive,
  echo, wah); no trademark in a name. Level-matched: **−18.05 .. −17.95 dB**.
- **Release-tail check changed.** It demanded under −80 dBFS 3 s after the chord, but the tube amp's level matching
  holds its boost after a driven chord (measured up to +16 dB: Rotor 45 at Drive 0.8 with the overdrive), which lifts
  the ever-present leakage floor by that much. The tail is now compared with the same preset rendered with no keys
  (the idle floor, itself required below −60 dBFS): within 18 dB of it. A stuck contact or a reverb / echo that doesn't
  decay sits 30 dB or more above the floor, so the check still catches those. (Leak 0 removes the floor entirely.)
- **Wah Lead** needed its registration: the wah is a Q-4 band-pass with no dry path, and on 88 8800 000 with the pedal at
  toe (2.3 kHz) the level-match chord lost 12 dB (−29.9 dB, beyond the +6 dB `ampLevel` can add). With 88 8888 888
  (harmonics in the wah's band) it sits at −13.2 dB before matching.
- Tools: `--host-rotor` (above), `--mod-probe`, `--probe-preset "<Category/Name>" [id=value …]` (one preset with
  overrides through the preset checks' measurements, for working on a preset without a rebuild), and
  `tests\screenshots.ps1` (the five pages from the real UI with a stand-in bridge → `docs/screenshots/`).
- Verification, all passing: the preset suite (50 presets: ranges, whole-step drawbars, categories, templates vs
  `Cabinets.h`, the rules above, file round trips, every preset renders and is silent after release, loudness),
  `--organ` (33 ok lines, including the glissandos), `--voice` (26), `--cabinet` (31, including the aliasing at C6),
  `--host-check` (50 programs), `--host-rotor` (6), `--host-cab`, `check-params.ps1` (143), the UI tests (28: the
  speed switch's greyed segments added), `--bench`: **4.9%** of one core, one key 3.3%.

UI restyle (2026-11-18, "beautify the UI"), CSS and layout only, every widget and test unchanged:
- **A control plate.** The pages sit on a crinkle-black plate let into the walnut, the active tab joined to it, and
  every page fills it: rows share the leftover height, knob rows centre in their panel. Before, each page was as tall
  as the tallest (CABINET) and the shorter ones (FX most of all) showed bare wood under their panels.
- **Hierarchy.** Panels got a lit top edge, a ruled title, and condensed Bahnschrift lettering for labels, titles,
  tabs and buttons; readouts are amber mono on black (the preset display is an LCD with the category above the name).
  Knobs grew to 56 px with a chrome rim and a shadow; keys are ivory / ebony gradients, pedals maple; the header's
  SAVE / LOAD / FOLDER and undo / redo / A / B are joined button bars.
- **Layout.** FX is 2 × 2 with a signal-chain strip (ORGAN → OVERDRIVE → WAH → TAPE ECHO → ROTARY CABINET →
  SPRING REVERB → OUT) whose lamps show what is in circuit. The CABINET page's speed controls (mod wheel, variable
  source, speed CC, sustain pedal) joined the CABINET panel as one column beside HORN / DRUM and SPEAKERS / MICS, so
  Horn's five and Mics' six knobs sit on one line. Lone switches (Stop at Angle, Drum Off, Preset Keyswitches, Pedal
  Zone, Wah Source, Amp OS, Mic layout) moved into their panel heads. Drawbars are 28 px wide (were 26), centred
  in their well; the preset-key registration cells share the row width.
- **Scale.** The keys are shorter (62 / 70 px in the SVG, were 78 / 92), and with the shorter pages the console is
  about 1030 px tall instead of 1230: the plugin window (1120 × 960) now scales it by ~0.89 instead of ~0.77, so the
  lettering is 15% larger in the host. Checked: the UI tests (28 ok), the five screenshots re-captured.

What to listen for in Minihost Modular (phase 5):
- Run `vst\install.bat` first (the installed plugin is older than the last cabinet voicing).
- **Chorus vs rotor.** Load *Jazz / Trio Classic* (C3 upper, Rotor 22 Slow): switch VIB SWELL off on the VOICE page
  and the 7 Hz shimmer goes; switch the CABINET page Slow ↔ Fast and the horn's swirl changes from a 0.8 Hz throb to a
  6.7 Hz tremolo, with the drum following over 4 s. If C3 sounds more like a tremolo than a chorus to you, say so:
  that depth is an estimate.
- **The presets**, a category at a time. Jazz: Slow cabinets, C1–C3 chorus, soft percussion; *Walking Trio* plays
  the pedals from the lowest lower-manual key; *Wheel Swell* sweeps the speed from the mod wheel. Gospel: *Full Organ*
  and the shouts at Fast, *Hymn Swell* and *Chapel Flute* on the Tone Cabinet. Rock: Rotor 45 breaks up sooner
  (*Overdrive Stack*, *Grind Comp*), the Single Rotor presets (*Garage Fuzz*, *Psychedelic Swirl* with the tape echo),
  *Wah Lead* on the expression pedal. Blues & Soul: *Funk Comp* and *Greasy Shuffle* on Rotor 45, *Slow Blues* and
  *Juke Joint* on the Single Rotor, *Swamp Stomp* from the mod wheel. Ballad, Classic & Theatre: Tone Cabinet with the
  spring reverb (*Bach Plenum*, *Cathedral Swell*), V3 on both manuals for the theatre presets, the reed and string
  shapes. The registrations are starting points.
- **2-Speed vs 3-Speed:** in 2-Speed the STOP segment is greyed out; pick 3-Speed to stop the rotors (and STOP AT
  ANGLE to park the horn). In Variable the switch is greyed out and the mod wheel sets the speed.
- **The idle floor after a driven chord** (*Grind Comp*, *Overdrive Stack*): with nothing held, a faint high whine
  (the leakage floor through the cranked amp, about −70 dBFS). Lower Leakage on the VOICE page removes it if it
  bothers you.

Fix after phase 4 (hand-testing: "the cabs are not changing the sound"):
- The plugin did switch cabinets (checked through the built VST3 with `--host-cab`), but the four models only differed
  in a small box resonance, a little amp gain (which the amp's level matching then evened out) and slightly
  different modulation. On an ordinary chord, mostly below the 800 Hz crossover, they sounded nearly the same.
- Each model now has a **speaker and cabinet voicing** after the amp (`dsp/Cabinets.h`: 4th-order high-pass and
  low-pass for the speakers' band, plus one colour peak): Rotor 22 35 Hz–9 kHz with a deep box at 100 Hz; Rotor 45
  130 Hz–5.5 kHz, a +8 dB mid bump at 1.2 kHz, a boxier 200 Hz resonance, +15 dB amp gain (breaks up sooner); Single
  Rotor 180 Hz–4 kHz, +8 dB at 900 Hz, deeper drum AM; Tone Cabinet 45 Hz–13 kHz, +4 dB presence at 3.5 kHz, reverb
  0.30. These are still The Ham's estimates.
- New check (`--cabinet`): the same full-registration chord through each cabinet (with its defaults loaded, as the UI
  does), measured in four bands with the overall level removed. Every pair must differ by at least 3 dB in some band.
  Measured: low band Rotor 22 −10.0, Rotor 45 −14.6, Single Rotor −22.8, Tone Cabinet −15.2 dB; high band −14.2,
  −15.9, −21.7, −10.5 dB; the closest pair (22 vs 45) differs by 4.6 dB. Mono compatibility is unchanged (worst band
  −6.2 dB). Presets level-matched again.
- Tools: `--cab-compare [folder]` prints those measurements and writes the same chord through each cabinet (Slow, then
  Fast) as WAVs; `--host-cab <vst3>` checks the Cabinet parameter through the built plugin.

Phase 4 notes:
- **Animated cabinet** (`ui/js/cabinet.js`, on the CABINET page): four cutaway drawings (Rotor 22 tall, Rotor 45
  squatter, Single Rotor's foam drum alone, the Tone Cabinet's fixed speakers over a spring tank) that cross-fade over
  350 ms when the model changes.
  - The horn is drawn on an elliptical orbit (a front view of a vertical axis): its mouth faces you at angle 0, and
    the dummy horn is darker unless Dual Active. The drum shows its slats and scoop opening sliding round.
  - The drawing follows the layout options: Horn only / Drum only hide the other rotor, Stationary freezes them, an
    open back shows a lighter interior, and the mic stands move with the layout, angle and distance (L/R, D for the
    close drum mic, F/B for the room pair).
  - At speed, three ghost copies trail the horn (motion blur); the amp's tubes glow brighter with Drive. The lit
    Stop/Slow/Fast lamps and the horn/drum rpm readout come from phase 3.
- **The drawing follows the engine, without lag.** Every frame draws the angle extrapolated from the latest `rotor`
  report with its speed. When a new report disagrees, the difference becomes an offset that fades out over about
  80 ms, so nothing jumps. A first version eased 35% toward the target each frame; at Fast (0.11 turn per frame) that
  trailed by about 0.2 turn, which the UI test caught. **Measured** (headless Edge, served, real time): a horn spinning
  at 6.7 Hz, reported every 33 ms, drawn within **0.2°** of its true angle; 97 frames in 1.5 s; **0.09 ms of script per
  frame** (about 0.5% of one core at 60 fps; the browser's own drawing is not included in that number). It stops
  completely when the rotors stand still and the drawing has caught up, when the window is hidden, and when the
  CABINET page isn't shown (measured: 0 frames).
- **Rocker tabs** (`Rocker` in controls.js): percussion On / Volume / Decay / Harmonic, Vibrato Swell / Great (upper /
  lower), and the motor's Start / Run, as tablets that tilt (CSS 3D, 140 ms) toward the selected half. Click toggles,
  or click a half.
- **Expression pedal** (`ExpressionPedal`): a swell shoe seen from the side, toe down = open. It follows CC11 (from
  `stats.expression`), can be dragged, scrolled and double-clicked (= open).
- **Undo / redo** (header ↶ ↷, Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z): snapshots of every preset parameter before each change
  made in the UI. A drag, a wheel run or repeated clicks on one control within 0.8 s are one step, and a preset load
  is one step. 100 steps. Host automation isn't recorded.
- **A/B compare** (header A / B / A→B): two complete settings; switching stores the current one in its slot, and A→B
  copies. Each switch is also an undo step.
- **Randomize drawbars** (DRAWBARS page): FLUTE, DIAPASON, REED, STRING, JAZZ. A random registration inside the
  family's [min, max] per footage (The Ham's rules, in app.js), on the active set of the chosen manual (set B when a
  fixed preset key is down). The bars animate to it.
- **Organ model in the UI:** a console badge next to the brand (B-3 on legs, C-3 with closed church-style sides, A-100
  with its speaker panel, M-3 spinet). **C-3 changes only the look**: a darker oak case with an inset panel line. Its
  sound is the B-3's, as tested in phase 3.
- The UI tests are committed now instead of in phase 6: `tests/ui-tests.js` (a mocked JUCE bridge plus 25 checks) and
  `tests/run-ui-tests.ps1`. The runner serves `vst/ui` over http and runs headless Edge in real time with background
  throttling off. Headless pages count as hidden, so without those flags timers run once a second and
  requestAnimationFrame doesn't run. Phase 6 extends the runner for the web build.
- Layout: the header stays on one line (brand, badge, preset, history, A/B, indicators); the cabinet drawing takes a
  262 px column on the CABINET page, and its viewBox margins are trimmed so it fills the column.

What to listen for / look at in Minihost Modular (phase 4):
- CABINET page: switch Slow ↔ Fast. The horn should visibly reach speed well before the drum, and on braking the drum
  keeps turning after the horn has slowed. At Fast the horn blurs. The drawing should look smooth (60 fps) and match
  what you hear.
- Change the cabinet model (cross-fade), Rotors (Horn only / Drum only / Stationary), Horns (Dual Active), Back (Open),
  Mics (the stands move), Drive (the tubes glow).
- Rocker tabs on the VOICE and PLAY pages, the expression pedal following CC11, undo / redo, A/B, RANDOM, and the
  organ model badge (try C-3).
- If the plugin window feels heavy while the cabinet spins, tell me. The script costs about 0.5% of a core; WebView2's
  drawing cost is not measured.

Phase 3 notes:
- **Signal chain after the preamp:** overdrive → wah → tape echo → tube amp → crossover → horn and drum rotors → mics
  → spring reverb → width. The analog stages (amp, overdrive, the echo's saturation) work at a calibrated level,
  `kStage` = 0.06 per full drawbar on one key, so a single 8' note reaches the amp near −24 dBFS and a full-organ
  chord drives it into breakup. Without that they would have seen raw busbar levels and clipped all the time.
- **Rotor motion** is in the engine. The speed follows `dω/dt = (target − ω)/τ`, and τ is derived from the parameters
  so that **Accel / Brake = the time from one speed to within 2% of the other**. Measured: horn Slow→Fast within 2%
  after 1.001 s (Accel 1.0), drum 4.000 s (4.0); Fast→Slow 1.501 s (Brake 1.5) and 5.000 s (5.0). The horn leads;
  1.5 s into a brake the drum is still at 1.6 Hz while the horn is at 0.8. Steady speeds are exact. A freshly loaded
  cabinet starts at its speed (no spin-up).
  - Speed sources: the Speed switch (when it changes), the mod wheel (Half Moon switches on crossing the middle;
    Momentary is Fast while up), the Speed CC (2-speed: ≥ 64 Fast; 3-speed: thirds = Stop/Slow/Fast), and the
    sustain pedal. The last one moved wins, and `stats.speed` reports the result, which the lit lamps show. 2-speed
    treats Stop as Slow. Variable sweeps Slow→Fast from the mod wheel, the expression pedal or the Speed CC (measured:
    wheel at half → 3.75 Hz).
  - Per-rotor overrides (Follow/Stop/Slow/Fast), Drum Off, directions (horn and drum opposite by default),
    Stationary. **Brake to angle:** below 0.4 Hz the horn steers onto the chosen angle (at least 0.15 Hz, so it
    arrives within seconds). Measured: stops at 90.0° when asked for 90°.
- **Cabinet DSP** (`dsp/Rotary.h`): a Linkwitz-Riley crossover feeds one delay line per rotor. Each mic reads the
  direct sound plus a back-wall and a side-wall reflection. Per pickup: c = cos(rotor − mic angle); Doppler = radius ×
  (1 − c) / 343 m/s; a one-pole directional low-pass moving between the "away" and "facing" corners; AM = 1 − depth +
  depth × ((1 + c)/2)ⁿ (horn n = 2, a narrow beam; drum n = 1). Dual active horns add a copy at +180°. The mic layouts
  (mono, stereo horn + mono drum, wide stereo, 3-mic with the drum close-miked, room pair front/back) place the
  pickups. Mic distance trades AM for reflections. The Room mic adds two late (9 / 12.5 ms), dark pickups. Box
  resonance is a peaking filter on the drum path; an open back lowers it to 30%, and lengthens and raises the back
  reflection. Measured: horn Doppler ±41 cents at Fast (r·ω/c gives ±31.6; the zero-crossing measure also sees the
  reflections), ±5.1 at Slow; AM swing 10.6 dB at Fast, 0.01 dB stationary; horn L/R correlation 0.39 (stereo horn).
- **Four cabinet models** (`dsp/Cabinets.h`) as data: Rotor 22, Rotor 45 (smaller box with a 180 Hz resonance,
  +4 dB more amp gain, breaks up sooner), Single Rotor (drum only, foam baffle, 10" voicing), and Tone Cabinet
  (stationary, speakers spread at fixed angles, built-in spring reverb at 0.25). **The voicings are The Ham's
  estimates.** No measured data for these cabinets was found, so they are the thing to judge by ear. Picking a model
  in the UI sends `cabinetModel {model}`, and the processor sets the model and its defaults (speeds, ramps, rotor
  configuration, baffle, crossover, back, mic layout) as ordinary parameter changes, so the host records them.
  Automating `cabModel` alone changes only the fixed voicing.
- **Tube amp:** first-order ADAA on f(x) = x / (1 + |x|) with a bias (even harmonics), at 1×, 2× or 4×. It is
  normalised to unity small-signal gain, then **level-matched** like Brass Knuckles' drive: input against output
  power over about 300 ms, boost only, at most +26 dB. Without the level matching, heavy drive lost about 20 dB.
  Drive adds 0–30 dB on top of the model's own amp gain.
  - **Oversampling (changed from the plan):** both 2× and 4× use polyphase IIR half-bands (Valenzuela–Constantinides
    as in HIIR, WTFPL; coefficients computed at start-up from the formula, so the JS engine gets the same numbers).
    Stage A has 8 coefficients (flat to 20 kHz, −99 dB from 28 kHz) and stage B 4 (−82 dB from 72 kHz). The plan had
    a linear-phase FIR at 4× (Brass Knuckles', about 30 samples) and no delay at 2×. Any 2× filter delays a little, so
    instead: **1× = 0, 2× = 3, 4× = 4 samples**, measured (3.3 and 4.2 on a quiet 200 Hz tone) and reported to the
    host when Amp OS changes. Bypassing the cabinet doesn't change the reported value (a difference under 0.1 ms).
  - Measured aliasing, a C6 sine driven ×8: 1× −53 dB, 2× −77 dB, 4× −78 dB; driven ×2 at 4×, −78 dB. An
    unspecified function-argument evaluation order (MSVC: right to left) first fed the 4× ADAA its samples out of
    order (−69 dB). The calls are now one per statement.
- **Outboard:** overdrive (0–36 dB into the same ADAA shaper; tone 0.8–8 kHz; output scaled by 1/√gain, so it gets
  louder as it's driven, like a pedal), wah (TPT band-pass, Q 4, 350 Hz–2.3 kHz, from the knob, the expression pedal
  (which then stops being the volume pedal), the mod wheel or a CC; measured centroid 515 → 1308 Hz heel to toe), and
  tape echo (to 1.2 s, wow 0.6 Hz and flutter 5.5 Hz, darkened and saturated repeats, time changes glide; measured: the
  second repeat is −5.7 dB at feedback 0.5). Each crossfades in and out over one chunk.
- **Spring reverb:** four springs (27–46 ms loops, each with an 8-stage dispersive all-pass, a Tone low-pass and a
  DC-blocking high-pass in the loop), two per channel. The input is high-passed at 200 Hz. Measured: −33.5 dB half a
  Decay after a note (−30 expected); L/R correlation −0.27. It is built into the Tone Cabinet (0.25) and the A-100
  (+0.15). Reverb On adds its Mix on top.
- **Organ models:** C-3 sounds exactly like the B-3 (tested; the UI difference comes in phase 4). A-100 is the B-3
  voice plus its built-in reverb. M-3 has 44-key manuals (F–C), no 16' on the lower manual, 13 pedals, the A-based
  generator foldback (setBfree `TG_82FB09`, lowest wheel 10), and untapered keys. **The M-3 taper is an
  approximation**; none was found. Changing the model releases every key and re-wires.
- **Mono compatibility:** every cabinet × mic layout at Fast with the room mic on, a full chord on all manuals. In
  1/3-octave bands (125 Hz–8 kHz) the mono sum loses at most **6.2 dB** against the stereo average (Tone Cabinet,
  3-mic). The test limit is 9 dB.
- **No click on Slow↔Fast:** energy above 12 kHz in 2 ms windows. This band swings a lot all the time (up to 48× its
  median here), so the check compares the loudest window around the switches (17.6×) with the loudest elsewhere.
- Engine output is now stereo. The processor sends `rotor {hornAngle, drumAngle, hornHz, drumHz}` (angles in turns,
  0 = facing the front) on every 30 Hz tick while it changes; the UI shows the horn and drum rpm, and phase 4 animates
  the drawing from it. `stats.speed` lights the Stop/Slow/Fast lamps.
- CPU (`--bench`): every key, every drawbar, percussion, scanner on both manuals, leakage and Age 100%, Fast rotors,
  4× amp, dual active horns, 3 mics + room, overdrive + wah + echo + reverb: **4.6%** of one core. One key: 3.2%.
- Checks: 33 cabinet checks (`--cabinet`) on top of phases 1–2 (whose generator checks now run with the cabinet
  bypassed); presets level-matched again (−18.03 .. −17.96 dB); `--host-check`; `check-params.ps1`.

What to listen for in Minihost Modular (phase 3):
- **Slow ↔ Fast** (the CABINET switch, or the mod wheel above / below the middle): the horn reaches speed in about a
  second, the drum takes about four. On braking the drum keeps churning after the horn has settled. No clicks.
- **Rotor 22 vs Rotor 45 vs Single Rotor vs Tone Cabinet:** deep and wide; tighter and boxier and earlier to break
  up; narrow and watery; straight with reverb. These voicings are estimates, so tell me what sounds off.
- **Drive** (VOICE page) on Full Organ / Overdrive Stack: it should bite and break up smoothly without fizz, and stay
  at about the same loudness. Try Amp OS 1× vs 4× on high notes.
- **Mics:** Mono, Stereo Horn, Wide, 3-Mic, Room Pair, distance and angle, the Room mic; Horn only, Drum only,
  Stationary; Dual Active horns; Short mouth; Open back; brake to angle (3-speed, Stop, Stop at Angle).
- **Pedals and reverb:** overdrive into the cabinet, the wah on the expression pedal, tape echo, spring reverb.
- **Organ models:** M-3 (shorter keyboards, no lower 16'), A-100 (built-in reverb).
- **Latency:** the host should show 3 samples at 2× and 4 at 4×.

Phase 2 notes:
- **Percussion** = the upper manual's 4' (Second) or 2⅔' (Third) busbar, so it gets the same key click and foldback,
  times an envelope that falls 60 dB over the decay time. Measured: −30.0 dB at half of Fast (0.3 s) and Slow (1.0 s).
  Defaults are the brief's values. setBfree uses 1 s and 4 s (`percFastDecaySeconds`, `percSlowDecaySeconds`), and its
  definition of "decay seconds" isn't stated. The times are parameters, so they can be set by ear.
  - Single trigger: the envelope restarts only on a key press while no other upper key is held. Measured: a legato
    note 0.5 s in adds 1·10⁻⁷ (nothing). The first note after a full release retriggers.
  - Works only on drawbar set B. On, it takes the 1' busbar (the 1' bar goes silent). Normal lowers the upper
    drawbars by −4.36 dB (setBfree's `percDrawbarNormalGain` 0.60512). Soft is −6.0 dB (0.5012). Both measured exactly.
  - Percussion and pedals bypass the scanner, as on the B-3.
- **Scanner vibrato / chorus:** a line of 18 second-order all-pass sections (7.9 kHz, Q 0.6; flat magnitude, near-constant
  delay, like an LC ladder) with 19 taps. The scanner position is depth × (1 − cos rotor) / 2, and adjacent taps
  are blended linearly, which models the plate overlap. A 6.5 kHz low-pass on the scanned signal stands in for the
  line's treble loss. The rotor turns at 6.87 Hz on 60 Hz mains and follows the motor (and the pitch dive). V1/V2/V3
  sweep 6/12/18 sections. **Measured on a 440 Hz tone: ±13.7, ±27.4, ±41.1 cents**, at a measured rate of 7.0 cycles
  per second (6.87 Hz set). C1–C3 = (scanned + straight) / 2, exact to 1·10⁻⁸. Separate Upper and Lower switches
  (measured: the lower manual stays steady with only Upper on). I found no published depth figures, so the depths
  can be tuned by ear (`Scanner::kSectionHz` and the 6/12/18 table).
- **Preamp / expression** (CC11 or the Expression CC, or the on-screen EXPR pedal): three bands split by one-pole
  low-passes at 200 Hz and 2.5 kHz. With the pedal closed: bass −14, mids −26, treble −32 dB, linear in dB with the
  pedal position. The pedal is smoothed (12 ms) and ramped per sample. Open, the bands sum back to the input exactly.
  Measured on a chord with pedal bass: open −15.9 dBFS / centroid 1207 Hz, half −27.3 / 1018 Hz, closed −36.5 / 827 Hz.
  With the pedal closed, the bass fell 14.4 dB and the mids 22.1 dB.
- **Drive** has no effect until the cabinet's tube amp exists (phase 3). The UI says so.
- **Leakage** per manual (pedals follow the lower manual's): when a contact closes, its wheel's compartment partner
  (setBfree `wheelPairs`: 1–36 ↔ 49–84, 42–48 ↔ 85–91) leaks onto the same busbar. The same wheel also leaks onto the
  neighbouring busbars at half that level. At Leak 100% that is −30 dB; at the default 30%, −40 dB, which is
  setBfree's default crosstalk. Measured: the partner sits at −33.7 dB, which is −30 dB plus that wheel's own −3.8 dB
  voicing. There is also an always-present floor: every wheel at −66 dB × (0.3–1.0, rising with pitch), measured at
  −84 dBFS with no keys at 100%. The preset tests now allow it in release tails (below −80 dBFS).
- **Age** per manual: contact levels spread ±3 dB (seeded, fixed per instrument), wheel levels ±2 dB, plus 2nd and 3rd
  harmonics that are smaller on the filter-capacitor wheels (above 43), and more leakage (× (1 + Age)). The generator is
  shared, so its wear follows the higher of the two Age settings. Measured at 100%: 2nd harmonic of wheel 13 −34 dB
  (new: −109 dB), key-to-key spread in a flat-taper range 5.7 dB (new: 0.03 dB). A changed Age reaches each contact
  the next time it makes or breaks.
- **Sustain:** pedal and lower-manual sustain release the contacts along an exponential to −60 dB over the set time.
  Measured: −9.2 dB 0.1 s after release with 0.6 s (−10 expected), −14.9 dB at 0.25 s of 1 s (−15 expected).
  **Manual bass:** the lowest held lower key also plays the pedal of the same name, folded into the 25 pedals.
  Tested to follow, release and fold.
- **Preset keys:** the fixed registrations C♯–A are now **editable**, per manual, on the DRAWBARS page. They are saved
  in the project state (`registrations`) and in presets (a `"registrations"` block). A preset or project without them
  gets the factory chart. The upper chart is confirmed against hammondwiki's StandardPresets (via a page that cites it
  and the service manual). For the lower manual only the names are published (Cello, Open Diapason, …, Full Great),
  several marked "estimated", so the lower manual starts from the upper chart. **Keyswitches** (off by default): MIDI
  24–35 on a manual's channel press its preset keys and never sound; they are off in Split.
- **MIDI learn:** a LEARN button, then click a control and move a CC. One CC per control; learned CCs win over the
  drawbar maps. Right-click in learn mode forgets. Bank select, sustain and channel-mode CCs can't be learned. The map
  is part of the **project state**, not of presets: it describes your hardware, not the sound, so loading a preset
  keeps it. (CONVENTIONS.md planned a `"cc"` preset block; I changed that and noted it there.)
- New bridge events: UI → host `expression {value}`, `setRegistration {manual,key,bars}`, `midiLearn {id}`,
  `midiForget {id}`, `learnCancel`. Host → UI `registrations {upper[9], lower[9]}`, `learn {armed, map}`, and `stats.perc`.
- Measured CPU (`--bench`): all 147 keys, every drawbar, percussion, the scanner on both manuals, leakage and Age at
  100%: **3.7%** of one core. One key: 2.1%. Presets level-matched again (−18.05 .. −17.96 dB). C3 chorus and Normal
  percussion lower the starter presets by about 2 dB, which the level matching takes up.
- Checks: 26 voice checks (`--voice`) on top of phase 1's 35, a preset-file test for the registrations block,
  `--host-check`, `check-params.ps1`, and the headless UI smoke test (now 13 checks: learn mode, the registration
  editor, the expression pedal).

What to listen for in Minihost Modular (phase 2):
- **Percussion** (VOICE page; Trio Classic has it on): a bright "pop" on the first note of a phrase. Play legato and
  later notes don't pop; lift every key and the next one pops again. Fast vs Slow, Second vs Third, Soft vs Normal
  (Normal also drops the drawbars a little). Switch to set A (A♯ preset key): no percussion. Pull out the 1' bar with
  percussion on: it stays silent.
- **Vibrato / chorus:** V1–V3 should be clearly different depths at a steady 6.9 Hz wobble. C3 is the classic shimmer.
  Upper and Lower switch separately. If the depths sound wrong to you, tell me which way.
- **Expression:** CC11 (or the EXPR pedal next to the keys). Half-closed should sound darker as well as quieter, with
  the bass holding up.
- **Leakage / Age:** at 100% with no keys there's a faint high whine (the generator floor). Held notes get a faint halo.
  Age at 100%: grittier lows, slightly uneven keys.
- **Pedal / lower sustain**, **Manual Bass** (one keyboard: the lowest lower-manual note plays the pedals).
- **Preset keys:** edit a fixed registration (DRAWBARS page, under the bars), press its preset key, then save and reload
  a preset. **Keyswitches** (PLAY · MIDI) and **MIDI learn** (LEARN, click a control, move a knob).

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
| Pedal composites | none found; The Ham's approximation | to tune by ear (hand-testing) |
| Per-wheel voicing, capacitor-filtered wheels | none found; approximation in `dsp/Tonewheels.h` | to tune by ear |
| Fixed preset-key registrations | upper: hammondwiki StandardPresets (as quoted by fubsypoly-musicstudio.pages.dev/hammond-b3/vol-3, which cites it and the B-3/C-3 service manual), matches the chart used. Lower: names only, several "estimated" | upper confirmed (phase 2). Lower uses the upper chart; editable |
| Scanner line and plate pattern | no schematic values found. setBfree `vibrato.c` (scanner 7.25 Hz default) as a cross-check. Rate here 6.87 Hz (the brief's ~6.9) | modeled (phase 2); depths measured, to tune by ear |
| Percussion levels and decay | setBfree `tonegen.c` (Normal drawbar gain 0.60512, Soft 0.5012; decay 1 s / 4 s, definition not stated) | levels used. Decay: brief's 0.3 / 1.0 s to −60 dB, adjustable |
| Leakage partners and level | setBfree `wheelPairs`, default crosstalk 0.01 (−40 dB) | used (phase 2) |
| Rotor speeds and ramps | the brief's values (Rotor 22); the other models' are estimates | done (phase 3); ramp times exact to the parameters |
| Cabinet voicings (radii, AM, filters, box, amp) | none found; The Ham's estimates in `dsp/Cabinets.h` | to judge by ear |
| IIR half-band design | HIIR (Laurent de Soras, WTFPL): the Valenzuela–Constantinides formula | done (phase 3) |
| M-3 generator / keyboard | setBfree `TG_82FB09` (A-based generator, foldback at wheel 10); 44-key F–C manuals, no lower 16' | done; taper is an approximation |
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
   ±1 dB; final registrations recorded in the README. (Done: see the phase 5 notes; the brief's registrations were
   not available, so published classic settings stand in.)
   Tests: everything above for every preset, aliasing of the drive at C6 (inharmonic energy below 18 kHz, as in
   Brass Knuckles), fast chromatic and white-key glissando scenarios (no stuck contacts, no clicks beyond the key
   clicks), `--bench` (61 + 61 + 25 keys, Fast, 4×), `--host-check`. README with screenshots.
6. **Web build.** `dsp-worklet.js` (line-for-line twin), `web-host.js` (including `rotor` events), `factory.js`,
   `build-web.ps1` (→ `web/index.html` and `docs/index.html`), `start.bat` / `serve.ps1`. Parity scenarios for every
   feature and every preset at Brass Knuckles' tolerance (1e-4). Headless UI tests, including one that feeds rotor
   events and checks that the drawn horn and drum angles follow them. (Done: see the phase 6 notes.)
