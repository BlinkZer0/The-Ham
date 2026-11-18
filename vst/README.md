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

## Signal flow (phase 1)

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
  -> sum of 3 manuals x 0.06 x patch level x master (smoothed) -> safety clip (linear below -1.9 dBFS) -> L = R
```

There is no voice allocation: the cost is the 91 wheels plus one dot product per busbar that has a closed contact.

## In the host

- Instrument (Instrument|Synth), MIDI in, stereo out. **143 automatable parameters** with stable IDs. Drawbar and knob
  drags record as single automation gestures.
- **Right-click any control** for the host's parameter menu. The Standalone app gets a small fallback menu with
  *Reset to default*. Shift+right-click opens DevTools.
- MIDI: notes on channels 1 / 2 / 3 (upper / lower / pedals, settable) or Split; drawbar CCs (map on the PLAY · MIDI
  page); expression (CC11 or the Expression CC; used from phase 2); sustain (CC64, phase 2); Motor Off CC (≥ 64 = off);
  all notes / sound off. Events are applied at the exact sample they arrive.
- **Programs**: the factory presets appear in the host's program list as `Category - Name`.
- **State**: the project saves every parameter plus the selected preset.

## Presets

- **Factory**: [presets/factory_presets.json](presets/factory_presets.json) uses **templates** (`organ` → `tone`)
  plus per-preset overrides. `ampLevel` comes from `tests\level-match.ps1`.
- **User folder**: `Documents\The Ham\Presets`, subfolders = categories, rescanned every ~2 s. SAVE asks before
  replacing a file. The × on a user preset moves it to the Recycle Bin.
- **File format** (`.hampreset`, plain JSON). Missing parameters load at their defaults, values are clamped, master is
  never stored, drawbars are written as whole steps. `PresetManager::migrate()` brings older versions up to date:
  ```json
  { "format": "The Ham Preset", "version": 1, "name": "My Organ", "category": "User",
    "params": { "upB1": 8, "upB2": 8, "upB3": 8, "percOn": 1, "clickUp": 0.6 } }
  ```

## Tests

| Command | Checks |
|---|---|
| `TheHamTests.exe` | parameter table, factory presets (ranges, 5 categories, templates resolved, drawbars whole steps), preset file round trip, partial / old / garbage files, the organ checks below, every preset renders (not silent, no clipping, silent after release), loudness of every preset within ±1 dB of −18 dB |
| `TheHamTests.exe --organ` | 35 organ checks: all 91 wheel frequencies against the gear formula and published values; A = 440.000 Hz; measured pitch for 8', folded 16', 50 Hz mains, A = 450 and transpose; every key × footage on both manuals against Electric Druid's foldback table; wheels 1–12 pedal-only; taper range; **phase continuity** (a re-pressed key and staggered chord presses are bit-identical to fresh presses once the contacts settle); key click (100% vs 0%, contact spread and bounce, key-up click); drawbar law (8 → 5 = −9 dB, 1 = −21 dB, 0 = silent); preset keys; channel and split routing; note-off after a transpose change; motor (pitch dive, Run can't start it, Start in 1 s, Motor Off CC); chromatic and white-key glissandos; CC maps |
| `TheHamTests.exe --host-check "<path>\The Ham.vst3"` | loads the built VST3 like a DAW: scans as an instrument, all parameters named, notes + a drawbar CC make sound while parameters are automated, state restores exactly, programs switch |
| `TheHamTests.exe --bench` | all 61 + 61 + 25 keys held with every drawbar out: 2.5% of one core; one key: 1.3% |
| `TheHamTests.exe --loudness` | loudness of every factory preset (JSON) |
| `TheHamTests.exe --render "<Category/Name>" out.wav [demo\|chord\|gliss\|click]` | offline render of a preset |
| `TheHamTests.exe --render-all <folder>` | every preset, demo phrase |
| `TheHamTests.exe --dump-params` | the C++ parameter table as JSON |
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
  EngineParamsFromValues.h   parameter values -> engine snapshot (plugin + tests)
  MidiRouting.h              MIDI message -> engine (plugin + parity tool)
  CcMap.h                    drawbar CC maps for hardware controllers
  PresetManager.*            factory templates, user folder, .hampreset read/write/migrate
  PluginProcessor.*          APVTS, MIDI, CC maps -> parameters, UI key FIFO, programs, state
  PluginEditor.*             WebView2 host + JS <-> C++ bridge
ui/                          HTML/CSS/SVG/JS UI, embedded in the binary
  js/params.js               parameter definitions (UI side)
  js/drawbar.js              Drawbar and DrawbarSet (registration string)
  js/piano.js                manuals, pedalboard, preset keys
  js/knob.js, controls.js    knob, toggle, segmented, dropdown (shared with Brass Knuckles)
  js/engine-juce.js          the bridge
  js/app.js                  UI wiring
presets/factory_presets.json
tests/                       PresetTests.cpp, OrganTests.cpp, EngineTools.cpp, HostCheck.cpp, TestRender.h,
                             check-params.ps1, level-match.ps1
```

### Bridge events

| UI → host | host → UI (30 Hz, only changes) |
|---|---|
| `uiReady`, `setParam {id,value}`, `gesture {id,begin}` | `params {id:value,…}` |
| `noteOn {manual,key,vel}`, `noteOff {manual,key}`, `panic` | `notes {on:[[manual,key],…],off:[…]}` |
| `loadPreset {id}`, `savePreset {name,overwrite}`, `deletePreset {id}`, `loadPresetFile`, `openPresetFolder`, `paramMenu {id,x,y}` | `presetList {presets,folder}`, `preset {id,name,category}`, `saveResult {ok,exists,name,error}`, `message {text}` |
| | `stats {peak,sustain,modWheel,expression,motor}`, `config {…}` |

`rotor {hornAngle,drumAngle,hornHz,drumHz}` arrives with the cabinet (phases 3–4).

## Troubleshooting

- **Live UI editing:** set `HAM_UI_DIR` to this repo's `vst\ui` folder before starting the host or Standalone. The UI
  then loads from disk: edit the HTML/CSS/JS and reopen the plugin window. `HAM_UI_PAGE=voice` (or `drawbars`, `cabinet`,
  `play`, `fx`) opens that page first. `serve.ps1 -Ui` in the repo root previews the UI in a browser without an engine.
- **DevTools:** Shift + right-click in the plugin window, then *Inspect*.
- **Blank window:** the WebView2 runtime is missing or blocked. Its data folder is `%TEMP%\TheHamVST-WebView2`;
  deleting it is safe.
- **A manual is silent:** check its preset key (C = cancel silences it) and the MIDI channels on the PLAY · MIDI page.
