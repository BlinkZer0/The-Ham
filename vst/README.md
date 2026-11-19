# The Ham: VST3 plugin (JUCE 8)

This folder is the source of truth for The Ham:
- **C++ DSP**: the plugin's sound engine (`Source/SynthEngine.h`, `Source/dsp/`).
- **HTML/SVG UI**: shown inside the plugin through WebView2.
- **Web host** (phase 6): the same UI in a browser with a JS twin of the engine.

## Build

```
build.bat            VST3 + Standalone + TheHamTests (Release)
build.bat Debug
install.bat          copies "The Ham.vst3" to C:\Program Files\Common Files\VST3 (asks for admin)
```

Build files go to `%LOCALAPPDATA%\TheHamVST` so OneDrive doesn't sync them. An existing JUCE 8.0.17 download from
Brass Knuckles (`%LOCALAPPDATA%\BrassKnucklesVST\deps\juce-src`) or E-Piano (`%LOCALAPPDATA%\EPianoVST\deps\juce-src`)
is reused.

- `...\build\TheHamVST_artefacts\Release\VST3\The Ham.vst3`
- `...\build\TheHamVST_artefacts\Release\Standalone\The Ham.exe`: *Options → Audio/MIDI Settings* for the audio
  device and MIDI input
- `...\build\TheHamTests_artefacts\Release\TheHamTests.exe`

## Signal flow (phase 3)

```
motor (Start / Run, 60/50 Hz mains, tuning A, pitch dive; evaluated every 32 samples)
  -> 91 tonewheels: rotating phasors (renormalized every 32 samples), never reset by keys,
     per-wheel level, 3rd harmonic on wheels 1-24, amplitude follows motor speed
keys (MIDI channel per manual or Split; on-screen keyboards direct)
  -> 9 contacts per key (pedals: 6, two composite drawbars), wired by the chart in dsp/Wiring.h
     each contact: its own moment (0.2-4 ms spread), a step or a smooth ramp, 0-3 bounces  = the key click
     contact events sit in a binary heap and are applied at their exact sample
  -> busbars: busG[manual][bus][wheel] += contact level x taper      (changes only at contact events)
  -> drawbars of the active registration (preset key: cancel / fixed / set A / set B), ramped over 32 samples
     + leakage: busLeak[manual][bus][wheel] (compartment partner on the same busbar, the same wheel on the
       neighbouring busbars at half) x Leak x (1 + Age); + a floor of every wheel (-66 dB each at Leak 100%)
  upper (+ floor) --+-- Vibrato Upper --> scanner: 18 all-pass sections (taps 0..18), rotor 6.87 Hz x mains x motor,
  lower (+ floor) --+-- Vibrato Lower -->   position = depth x (1 - cos) / 2, adjacent taps blended; 6.5 kHz low-pass;
                                            V = scanned, C = (scanned + straight) / 2; depth V1 / V2 / V3 = 6 / 12 / 18
  upper busbar 4' (Second) or 2 2/3' (Third) x percussion envelope (-60 dB over Fast / Slow; restarts only when no
       upper key was held; set B only; takes the 1' busbar; Normal = upper drawbars x 0.605, Soft = x 0.501)
  pedals -> pedal tone low-pass (120 Hz .. 1.9 kHz)
  -> sum -> preamp: bands at 200 Hz / 2.5 kHz, expression closed = bass -14 / mids -26 / treble -32 dB
  -> outboard (each optional, crossfaded): overdrive (ADAA, 0..36 dB) -> wah (TPT band-pass) -> tape echo
  -> cabinet (or Bypass): tube amp (Drive; ADAA 1x / 2x / 4x with IIR half-bands; level-matched over ~300 ms)
     -> Linkwitz-Riley crossover -> horn / drum delay lines (drum through the box resonance)
     -> pickups per mic: direct + back wall + side wall; Doppler (radius (1 - c) / 343 m/s), directional low-pass,
        AM 1 - d + d ((1 + c) / 2)^n; dual horns at +180 degrees; room mic: two late dark pickups -> L, R
  -> spring reverb (4 dispersive springs; built into Tone Cabinet / A-100) -> width (M/S)
  -> x 0.06 x patch level x master (smoothed) -> safety clip (linear below -1.9 dBFS) -> L, R
The analog stages run at kStage = 0.06 per full drawbar on one key (a full-organ chord is ~15x that).
Rotors: speed toward the target with tau from the Accel / Brake times (= time to within 2% of the new speed);
horn brake-to-angle below 0.4 Hz; angles and speeds go to the UI as `rotor` events.
Age: contact levels +-3 dB and wheel levels +-2 dB (fixed random pattern) x Age, 2nd / 3rd harmonics on wheels <= 63.
Sustain (pedals, lower): a released key's contacts fade exponentially to -60 dB over the sustain time.
Manual bass: the lowest held lower key also holds its pedal.
```

There is no voice allocation: the cost is the 91 wheels plus one dot product per busbar that has a closed contact.

## In the host

- Instrument (Instrument|Synth), MIDI in, stereo out. **143 automatable parameters** with stable IDs. Drawbar and knob
  drags record as single automation gestures.
- **Right-click any control** for the host's parameter menu. The Standalone app gets a small fallback menu with
  *Reset to default*. Shift+right-click opens DevTools.
- MIDI: notes on channels 1 / 2 / 3 (upper / lower / pedals, settable) or Split; drawbar CCs (map on the PLAY · MIDI
  page); expression (CC11 or the Expression CC); Motor Off CC (≥ 64 = off); preset-key keyswitches (MIDI 24–35, when on);
  MIDI-learned CCs (they win over the drawbar maps); all notes / sound off. Events are applied at the exact sample
  they arrive. Learned CCs and keyswitches change parameters through the message thread, so the host records them.
- **State** also holds the fixed preset-key registrations and the MIDI-learn map.
- **Latency:** the amp's oversampling filters delay the output 3 samples at 2× and 4 at 4× (0 at 1×). The plugin
  reports this to the host and updates it when Amp OS changes.
- **Rotor speed** from the mod wheel (Half Moon / Momentary), the Speed CC, the sustain pedal (when enabled), or the
  Speed switch; Variable mode sweeps it from the mod wheel, expression or Speed CC.
- **Programs**: the factory presets appear in the host's program list as `Category - Name`.
- **State**: the project saves every parameter plus the selected preset.

## Presets

- **Factory**: [presets/factory_presets.json](presets/factory_presets.json): 50 presets (5 categories × 10) built from
  **templates** plus per-preset overrides. `organ` holds the manual defaults (set B, pedals 5/3, vibrato off, Drive 20%);
  `cab22`, `cab45`, `cabSingle` and `cabTone` each carry one cabinet model's defaults from `dsp/Cabinets.h` (rotors,
  speeds, ramps, baffle, crossover, back, mics, Slow / Fast / Stop), and the tests fail if the two drift apart, so a
  preset loads the whole cabinet, as picking the model in the UI does. The vibrato is never a template's: every preset
  states both switches. `ampLevel` comes from `tests\level-match.ps1`; `TheHamTests --probe-preset` measures one
  preset with overrides while you work on it.
- **User folder**: `Documents\The Ham\Presets`, subfolders = categories, rescanned every ~2 s. SAVE asks before
  replacing a file. The × on a user preset moves it to the Recycle Bin.
- **File format** (`.hampreset`, plain JSON). Missing parameters load at their defaults, values are clamped, master is
  never stored, drawbars are written as whole steps. `PresetManager::migrate()` brings older versions up to date:
  ```json
  { "format": "The Ham Preset", "version": 1, "name": "My Organ", "category": "User",
    "params": { "upB1": 8, "upB2": 8, "upB3": 8, "percOn": 1, "clickUp": 0.6 },
    "registrations": { "upper": ["00 5320 000", "00 4432 000", "..."], "lower": ["..."] } }
  ```
  `registrations` = the fixed preset keys C♯–A per manual; missing (or unreadable) entries get the factory chart.

## Tests

| Command | Checks |
|---|---|
| `TheHamTests.exe` | parameter table, factory presets (ranges, 5 categories, templates resolved, drawbars whole steps), preset file round trip, partial / old / garbage files, the organ checks below, every preset renders (not silent, no clipping, silent after release), loudness of every preset within ±1 dB of −18 dB |
| `TheHamTests.exe --organ` | 35 organ checks: all 91 wheel frequencies against the gear formula and published values; A = 440.000 Hz; measured pitch for 8', folded 16', 50 Hz mains, A = 450 and transpose; every key × footage on both manuals against Electric Druid's foldback table; wheels 1–12 pedal-only; taper range; **phase continuity** (a re-pressed key and staggered chord presses are bit-identical to fresh presses once the contacts settle); key click (100% vs 0%, contact spread and bounce, key-up click); drawbar law (8 → 5 = −9 dB, 1 = −21 dB, 0 = silent); preset keys; channel and split routing; note-off after a transpose change; motor (pitch dive, Run can't start it, Start in 1 s, Motor Off CC); chromatic and white-key glissandos; CC maps |
| `TheHamTests.exe --voice` | 26 voice checks: percussion decay (−30 dB at half of Fast and Slow), pitch (Second / Third), single trigger, set B only, 1' cancel, Normal −4.36 dB on the drawbars, Soft −6 dB; scanner off steady, depth V1 < V2 < V3 (±13.7 / ±27.4 / ±41.1 cents), rate 6.87 Hz, C3 = (V3 + dry) / 2 exactly, per-manual switches; expression quieter, darker, bass cut less than mids; leakage partner −30 dB (+ voicing), the floor; Age harmonics and level spread; pedal and lower sustain times; manual bass; editable preset-key registrations; keyswitch and learn rules |
| `TheHamTests.exe --cabinet` | 33 cabinet checks: rotors start at speed; Slow→Fast within 2% after exactly Accel (1.001 s / 4.000 s), Fast→Slow after Brake (1.501 / 5.000 s), horn leads, drum lags; steady speeds; 3-speed Stop and brake to 90.0°; 2-speed Stop = Slow; overrides, Drum Off, Stationary, directions; half moon, momentary, sustain, Speed CC, 3-speed CC, Variable; Doppler (±41 cents Fast, ±5 Slow), AM (10.6 dB Fast, 0.01 dB stationary), stereo (horn L/R correlation 0.39), mono / Width 0 / Bypass identical channels; no click on Slow↔Fast; mono compatibility of 4 cabinets × 5 layouts (worst band −6.2 dB); amp aliasing at C6 (1× −53, 2× −77, 4× −78 dB) and measured vs reported latency; overdrive, wah, tape echo, spring reverb; organ models |
| `TheHamTests.exe --cab-compare [folder]` | the same chord through each cabinet: level, centroid, L/R correlation, band profile; with a folder, writes it as WAVs (Slow, then Fast) to audition |
| `TheHamTests.exe --host-cab "<path>\The Ham.vst3"` | the built plugin with its Cabinet parameter at each model: how different the output is |
| `TheHamTests.exe --host-rotor "<path>\The Ham.vst3"` | the built plugin, one instance, its rotor parameters changed through the host API while a note is held: Fast and Slow modulate at the Horn Fast / Slow rates, Stop stops, a changed Horn Fast changes the rate, Bypass and the Tone Cabinet have no modulation (6 checks) |
| `TheHamTests.exe --mod-probe` | what modulates the sound and how much: the envelope swing (dB) and rate (Hz) of a held tone and of a chord under the chorus (C1 / C3 / V3), each rotor speed and the Tone Cabinet |
| `TheHamTests.exe --probe-preset "<Category/Name>" [id=value …]` | one factory preset with overrides, through the preset checks' own measurements: loudness, render level, peak, release tail and the idle floor |
| `tests\screenshots.ps1` | the five UI pages captured in headless Edge with a stand-in bridge (a preset, lit keys, spinning rotors) → `docs/screenshots/*.png` |
| `TheHamTests.exe --host-check "<path>\The Ham.vst3"` | loads the built VST3 like a DAW: scans as an instrument, all parameters named, notes + a drawbar CC make sound while parameters are automated, state restores exactly, programs switch |
| `TheHamTests.exe --bench` | all 61 + 61 + 25 keys held, every drawbar out, percussion, scanner on both manuals, leakage and Age 100%, Fast rotors, 4× amp, dual horns, 3 mics + room, overdrive + wah + echo + reverb: 4.6% of one core; one key: 3.2% |
| `TheHamTests.exe --loudness` | loudness of every factory preset (JSON) |
| `TheHamTests.exe --render "<Category/Name>" out.wav [demo\|chord\|gliss\|click]` | offline render of a preset |
| `TheHamTests.exe --render-all <folder>` | every preset, demo phrase |
| `TheHamTests.exe --dump-params` | the C++ parameter table as JSON |
| `powershell -File tests\run-ui-tests.ps1` | 28 UI checks in headless Edge (served, real time, mocked bridge): drawbars, registration strings, preset keys, keyboards, host params / notes, the registration editor, MIDI learn, expression pedal, rocker tabs, undo / redo, A/B, randomize, organ badge, cabinet picker; the cabinet drawing settles on reported angles, tracks a horn spinning at 6.7 Hz within 0.2° (97 frames in 1.5 s, 0.09 ms of script per frame), draws nothing when idle or off its page |
| `powershell -File tests\check-params.ps1` | `params.js` matches `Params.h` exactly (order, IDs, ranges, defaults, units, choices) |
| `powershell -File tests\level-match.ps1` | re-renders every preset and rewrites its `ampLevel` to the loudness target |

## Layout

```
CMakeLists.txt  build.bat  install.bat
Source/
  Params.h                   parameter table (143 params; mirrors ui/js/params.js)
  SynthEngine.h              engine: motor, wheels, keys/contacts (event heap, ramps), busbars, drawbars
  dsp/Common.h               RNG, safety clipper, drawbar law
  dsp/Tonewheels.h           gear train, wheel frequencies, voicing
  dsp/Wiring.h               key -> wheel charts (foldback, taper), pedal composites, preset-key registrations
  dsp/Scanner.h              scanner vibrato / chorus (all-pass line, rotor), biquads
  dsp/Preamp.h               preamp / expression (three-band)
  dsp/Oversampler.h          IIR half-bands (HIIR design), ADAA shaper, the oversampled amp
  dsp/Rotary.h               the rotary cabinet: crossover, rotor delay lines, pickups (Doppler, AM, filters, reflections)
  dsp/Cabinets.h             cabinet models (voicing + parameter defaults)
  dsp/Outboard.h             overdrive, wah, tape echo
  dsp/SpringReverb.h         spring reverb
  EngineParamsFromValues.h   parameter values -> engine snapshot (plugin + tests)
  MidiRouting.h              MIDI message -> engine (plugin + parity tool)
  CcMap.h                    drawbar CC maps, preset-key keyswitches, learnable CCs
  PresetManager.*            factory templates, user folder, .hampreset read/write/migrate
  PluginProcessor.*          APVTS, MIDI, CC maps -> parameters, UI key FIFO, programs, state
  PluginEditor.*             WebView2 host + JS <-> C++ bridge
ui/                          HTML/CSS/SVG/JS UI, embedded in the binary
  js/params.js               parameter definitions (UI side)
  js/drawbar.js              Drawbar and DrawbarSet (registration string)
  js/cabinet.js              CabinetView: the animated cabinet cutaways, driven by `rotor` events
  js/piano.js                manuals, pedalboard, preset keys
  js/knob.js, controls.js    knob, toggle, segmented, dropdown (shared with Brass Knuckles); Rocker, ExpressionPedal
  js/engine-juce.js          the bridge
  js/app.js                  UI wiring
presets/factory_presets.json
tests/                       PresetTests.cpp, OrganTests.cpp, VoiceTests.cpp, CabinetTests.cpp, EngineTools.cpp, HostCheck.cpp, TestRender.h,
                             check-params.ps1, level-match.ps1, ui-tests.js, run-ui-tests.ps1
```

### Bridge events

| UI → host | host → UI (30 Hz, only changes) |
|---|---|
| `uiReady`, `setParam {id,value}`, `gesture {id,begin}` | `params {id:value,…}` |
| `noteOn {manual,key,vel}`, `noteOff {manual,key}`, `panic`, `expression {value}` | `notes {on:[[manual,key],…],off:[…]}` |
| `setRegistration {manual,key,bars}`, `midiLearn {id}`, `midiForget {id}`, `learnCancel` | `registrations {upper[9],lower[9]}`, `learn {armed,map{id:cc}}` |
| `cabinetModel {model}` (sets the model and loads its defaults) | `rotor {hornAngle,drumAngle,hornHz,drumHz}` (turns, Hz; every tick while it changes) |
| `loadPreset {id}`, `savePreset {name,overwrite}`, `deletePreset {id}`, `loadPresetFile`, `openPresetFolder`, `paramMenu {id,x,y}` | `presetList {presets,folder}`, `preset {id,name,category}`, `saveResult {ok,exists,name,error}`, `message {text}` |
| | `stats {peak,sustain,modWheel,expression,motor,perc,speed}`, `config {…}` |

The cabinet drawing animates from the `rotor` events: each frame draws the angle extrapolated from the latest report
with its speed; a disagreeing report becomes an offset that fades out over ~80 ms.

## Troubleshooting

- **Live UI editing:** set `HAM_UI_DIR` to this repo's `vst\ui` folder before starting the host or Standalone. The UI
  then loads from disk: edit the HTML/CSS/JS and reopen the plugin window. `HAM_UI_PAGE=voice` (or `drawbars`, `cabinet`,
  `play`, `fx`) opens that page first. `serve.ps1 -Ui` in the repo root previews the UI in a browser without an engine.
- **DevTools:** Shift + right-click in the plugin window, then *Inspect*.
- **Blank window:** the WebView2 runtime is missing or blocked. Its data folder is `%TEMP%\TheHamVST-WebView2`;
  deleting it is safe.
- **A manual is silent:** check its preset key (C = cancel silences it) and the MIDI channels on the PLAY · MIDI page.
- **The rotary seems to run all the time and the CABINET page doesn't change it:** look at the VOICE page first. The
  scanner chorus (VIB SWELL / VIB GREAT, C1–C3) turns at 6.9 Hz, which sounds much like a fast horn, and it is not
  touched by any cabinet control. The CABINET page shows what the rotors are doing: the STOP / SLOW / FAST lamps and the
  horn and drum rpm. In 2-Speed mode STOP is greyed out (that switch has no Stop: pick 3-Speed); in Variable mode the
  whole switch is, because the mod wheel, pedal or CC sets the speed. `TheHamTests --host-rotor` measures the rotor
  through the built plugin; `--mod-probe` shows the chorus and the rotors side by side.
