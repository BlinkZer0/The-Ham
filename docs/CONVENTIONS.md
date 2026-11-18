# Conventions (inherited from Brass Knuckles and E-PianoVST)

The Ham follows the house style of [Brass Knuckles](https://github.com/BlinkZer0/Brass-Knuckles) v1.0 (local
clone `../Brass-Knuckles`, commit `e3da785`), which in turn follows
[E-PianoVST](https://github.com/BlinkZer0/E-PianoVST) v1.0 (local clone `../E-PianoVST`, commit `67132f2`).
Everything in Brass Knuckles' `docs/CONVENTIONS.md` applies. This file restates the rules briefly and, under
**The Ham:**, lists every place where The Ham changes or extends a pattern.

## 1. Repository layout

```
README.md                    user-facing: features, screenshots, build/install, release history, layout
.gitignore                   .vs/  build/  *.user
.gitattributes               * text=auto
start.bat -> serve.ps1       PowerShell HttpListener serving web/ on localhost:8080 (-Dev, -Ui as in Brass Knuckles)
docs/                        CONVENTIONS.md, PLAN.md, screenshots/, index.html (GitHub Pages copy of the web build)
web/index.html               GENERATED single-file web build (never edited by hand)
vst/
  CMakeLists.txt  build.bat  install.bat  README.md
  Source/                    C++ (see 3)
  ui/                        HTML/CSS/SVG/JS UI, compiled into the binary as BinaryData
  webhost/                   JS twin of the DSP, browser stand-in for the C++ bridge, build-web.ps1
  presets/factory_presets.json
  tests/
```

**The Ham:**
- `build-web.ps1` writes `web/index.html` **and** `docs/index.html` (byte-identical), so GitHub Pages serves
  `https://blinkzer0.github.io/The-Ham/`. E-Piano commits a `docs/index.html` copy but no script makes it; here the
  script does. Both files are generated and never edited by hand.
- `.gitattributes` adds `*.bat text eol=crlf` and `*.ps1 text eol=crlf`. cmd.exe can mis-handle `goto` labels in
  batch files with LF line endings, which `* text=auto` alone can produce on checkout.

## 2. Build system

- CMake 3.22+, C++17, **JUCE 8.0.17** (`GIT_TAG 8.0.17-security`), `FetchContent`, shallow. The cache variable
  `HAM_JUCE_DIR` points at a local JUCE checkout instead.
- `juce_add_plugin`: `FORMATS VST3 Standalone`, `IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`, `NEEDS_WEBVIEW2 TRUE`,
  `VST3_CATEGORIES Instrument Synth`, `EDITOR_WANTS_KEYBOARD_FOCUS TRUE`, `COPY_PLUGIN_AFTER_BUILD FALSE`.
- Defines `JUCE_WEB_BROWSER=1`, `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`, `JUCE_USE_CURL=0`,
  `JUCE_VST3_CAN_REPLACE_VST2=0`, `JUCE_DISPLAY_SPLASH_SCREEN=0`.
- One `juce_add_binary_data` target holds `ui/*.html|css|js|svg` and `presets/factory_presets.json`. BinaryData
  is looked up by file name, so every UI file name must be unique.
- Links `juce_audio_utils`, `juce_gui_extra`, `juce_dsp` and the recommended config/LTO/warning flags.
- Console app `TheHamTests` compiles the preset manager and the test sources, and includes the header-only engine.
  It is built with `JUCE_PLUGINHOST_VST3=1` for `--host-check`.
- `build.bat`: work dir `%LOCALAPPDATA%\TheHamVST` (build + deps). It finds CMake on PATH or through `vswhere`,
  adds Git to PATH if needed, and builds `TheHamVST_VST3`, `TheHamVST_Standalone` and `TheHamTests` with
  `--parallel`. Messages are prefixed `[The Ham]`.
- `install.bat`: copies `The Ham.vst3` to `%CommonProgramFiles%\VST3` through an elevated PowerShell, then checks
  that `Contents\x86_64-win\The Ham.vst3` exists.

Identity: product `The Ham`, target `TheHamVST`, company `TheHam`, manufacturer code `Tham`, plugin code `Ham1`.

**The Ham:** `build.bat` looks for an existing JUCE checkout in `%LOCALAPPDATA%\BrassKnucklesVST\deps\juce-src`,
then `%LOCALAPPDATA%\EPianoVST\deps\juce-src`, before it downloads one (Brass Knuckles only checks E-Piano).
On this machine E-Piano's copy exists.

## 3. C++ source structure

| File | Role |
|---|---|
| `Params.h` | Parameter table: `enum ParamIndex` (order = storage order) + `paramDefs()` of `{id, hostName, Kind, min, max, def, Unit, logSkew, choices}`, `formatValue`, `logRange`, `createParameterLayout()` with `ParameterID{id, 1}`. Namespace `ham`. |
| `SynthEngine.h` + `dsp/*.h` | Header-only DSP. No JUCE, no allocation after `prepare()`, no locks. `EngineParams` snapshot, deterministic `Rng` (the same LCG as Brass Knuckles), `prepare / setParams / noteOn / noteOff / setController / render(L, R, n) / panic`. |
| `EngineParamsFromValues.h` | `makeEngineParams(get)`: raw parameter values to `EngineParams`. Shared by the plugin and the tests. |
| `MidiRouting.h` | MIDI message to engine. Used by the plugin and the parity tool, and mirrored in `dsp-worklet.js`. |
| `PresetManager.*` | Factory presets from BinaryData, user folder with a signature rescan, read/write/`migrate()`. |
| `PluginProcessor.*` | APVTS, cached parameter pointers and atomics, sample-accurate MIDI, programs, state, lock-free handoff. |
| `PluginEditor.*` | `WebBrowserComponent` (WebView2), resource provider, bridge events, 30 Hz timer that pushes only changed values. |

Rules:
- `juce::ScopedNoDenormals` in `processBlock`. Feedback paths flush values below 1e-30 to exactly 0.
- The engine runs in **double** precision.
- **Control rate `kChunk = 32` samples.** Modulation is evaluated once per chunk and per-sample targets are ramped
  linearly across the chunk.
- Master gain is smoothed per sample. The output safety clipper is exactly linear below −1.9 dBFS (Brass Knuckles').
- Parameter changes made by presets go through `beginChangeGesture / setValueNotifyingHost / endChangeGesture`.
  `master` is never stored in presets.
- Nonlinear stages use E-Piano's first-order **ADAA** and Brass Knuckles' **oversampler** (`dsp/Oversampler.h`).
  Inner loops use only `+ - * /` and `sqrt`, so the C++ and JS engines stay bit-close. Sines come from rotating
  phasors, not `sin()` per sample.

**The Ham:**
- **No voices.** An organ is not a polyphonic synth. The engine is a bank of 91 always-running tonewheels plus,
  per manual, 61 keys × 9 contacts. There is no voice allocation, stealing or polyphony parameter. A held key
  closes contacts that route wheels to busbars. Section 3 of the Brass Knuckles conventions (voice allocation,
  mono/legato, glide) does not apply.
- **Key contacts are the one exception to control-rate ramps.** Each contact closes or opens at an exact sample,
  with bounce (a few on/off flips over 1–4 ms, from the seeded `Rng`). The render loop splits at contact events,
  the same way the processor already splits a block at MIDI events. Everything else, including drawbar moves,
  stays on 32-sample ramps. This is the deliberate key click from the brief, and the only intended step.
- **Oversampling latency.** The cabinet amp at 2× uses polyphase IIR half-band filters (minimum phase, so no whole
  sample of latency is reported). At 4× it uses Brass Knuckles' linear-phase FIR half-bands, and the processor
  reports that latency to the host with `setLatencySamples`. At 1× and 2× the plugin reports 0. This satisfies the
  brief's "zero added latency unless 4×".
- Model data lives in tables, one header each: `dsp/Tonewheels.h` (gear train and voicing), `dsp/Wiring.h`
  (key-to-wheel charts per organ model), `dsp/Cabinets.h` (cabinet models), `dsp/Registrations.h` (preset keys).
  This is Brass Knuckles' `Instruments.h` pattern, split up because there are four kinds of model.

## 4. Parameters

- **Append-only.** New parameters go at the end of `ParamIndex`. Indices and string IDs never change.
  `ParameterID` version is 1.
- **New parameters default to neutral** (off, zero, or "as before"), so older presets and projects sound unchanged.
- Groups come from helpers (`drawbarParams(manual, set)`) in JS and loops in C++, with index helpers
  (`drawbarParam(manual, set, footage)`).
- `Params.h` and `ui/js/params.js` are mirrored by hand. `TheHamTests --dump-params` prints the C++ table as JSON,
  and `tests/check-params.ps1` fails on any difference.
- As in Brass Knuckles phase 1, the **whole table is defined and frozen in Phase 1**, so test projects keep loading.
  Parameters for later phases exist but do nothing yet.

**The Ham:**
- Drawbars are `Float` parameters with range 0–8. The engine accepts fractional positions (automation and
  Shift-drag). Presets store integers: the save path rounds them.
- **State that is not a host parameter** goes in the plugin state and in presets as named extra blocks, the way
  E-Piano embeds extras in its state: the preset-key registrations (C♯–A for each manual), the MIDI-learn map, and
  the user's custom drawbar CC map. These are lists of numbers that a host lane can't usefully automate, and
  adding them as parameters would put roughly 200 entries in the host's list.

## 5. UI ↔ C++ bridge

Inherited unchanged. `ui/js/engine-juce.js` wraps `window.__JUCE__.backend` in a `JuceEngine` class.
- UI → host: `uiReady`, `setParam {id,value}`, `gesture {id,begin}`, `noteOn {note,vel}`, `noteOff {note}`, `panic`,
  `modWheel {value}`, `loadPreset {id}`, `savePreset {name,overwrite}`, `deletePreset {id}`, `loadPresetFile`,
  `openPresetFolder`, `paramMenu {id,x,y}`. Web only: `setTempo`, `selectMidi`.
- Host → UI (30 Hz, only changes): `params`, `notes`, `stats`, `config`, `presetList`, `preset`, `saveResult`,
  `message`. Web only: `midiInputs`.
- On `uiReady` the editor clears its `lastSent` cache, so everything is sent again.
- Drags send `gesture begin/end`. One-shot changes are wrapped in a gesture by the editor.
- Right-click a control opens the host's parameter menu, with a *Reset to default* fallback. Shift+right-click
  opens DevTools.
- UI notes go through `MidiKeyboardState` (The Ham: a FIFO instead, see below). WebView2 data folder:
  `%TEMP%\TheHamVST-WebView2`.
- Dev mode: env var **`HAM_UI_DIR`** loads the UI from disk. `HAM_UI_PAGE` opens a page directly (for screenshots).

**The Ham** extensions:
- `noteOn {manual, key, vel}` / `noteOff {manual, key}`: manual 0 upper, 1 lower, 2 pedals; key = index on that
  manual (0 = its lowest C). They travel through a lock-free FIFO into the next audio block, not through
  `MidiKeyboardState`, so the on-screen keyboards play their own manual whatever the MIDI channels or split are.
  `notes {on: [[manual, key]…], off: […]}` reports the engine's own key state, so host MIDI lights the right manual.
- **`rotor {hornAngle, drumAngle, hornHz, drumHz}`** is sent on **every** 30 Hz tick while a rotor moves, not only
  on change, because the UI extrapolates from the latest event. Angles are in turns (0–1). The UI interpolates with
  `requestAnimationFrame`.
- New events: UI → host `presetKey {manual,key}`, `setRegistration {manual,key,bars}`, `midiLearn {id}` /
  `midiForget {id}`, `motor {start,run}`. Host → UI `motor {wheelSpeed}` (0–1, for the pitch-dive display),
  `learn {id,cc}`, and `stats` grows `expression`, `speed` (stop/slow/fast) and `percArmed`.

## 6. UI code

- Plain ES, `'use strict'`, no framework, no build step. Scripts load in order:
  `params.js, engine-juce.js, knob.js, controls.js, drawbar.js, cabinet.js, piano.js, app.js`.
- All controls are SVG. `Knob` is the reference widget (drag, Shift for fine, wheel, arrows, double-click reset,
  gestures). Panels are built in `app.js` (`makePanel`, `addKnob`, `control(Class, id)`) and every control carries
  `data-param`. Pages are tabs with an LED that shows whether the page's features are active. The whole panel
  scales to fit the window.
- CSS custom properties on `:root`.

**The Ham:**
- New identity, same widget family: **dark walnut and oxblood wood chassis, cream and chrome hardware, warm amber
  accent** (`--bg`, `--panel` wood tones, `--cream`, `--chrome`, `--accent` amber). Nothing reuses the gunmetal
  and brass palette.
- **`Drawbar`** (`drawbar.js`) follows the `Knob` pattern (same gesture, wheel, arrow and double-click handling), but
  it is drawn as a real drawbar: a shaft that pulls *down* out of the panel, numbered 1–8 as it comes out, 9 detents,
  and a tip coloured by footage (brown, white or black). Changes from presets animate over about 150 ms. Above each
  set sits an editable **registration string** (`88 8000 000`).
- **`Cabinet`** (`cabinet.js`): four inline SVG cutaways with rotor angles driven by `rotor` events. It pauses on
  `visibilitychange`, and it costs nothing when nothing moves.
- `Rocker` (percussion and vibrato tabs) and `ExpressionPedal` widgets follow the same pattern.
- Pages: DRAWBARS, VOICE, CABINET, PLAY/MIDI, FX.

## 7. Presets

- Factory presets: `presets/factory_presets.json`, with **templates and per-preset overrides** (Brass Knuckles'
  format). The resolved values are defaults, then the template chain, then the preset.
- **Level-matched** with `TheHamTests --loudness` and `tests/level-match.ps1`, to within ±1 dB of −18 dB.
- User presets: `Documents\The Ham\Presets`, subfolders = categories, rescanned every ~2 s by signature. Delete
  moves the file to the Recycle Bin. SAVE asks before it replaces a file.
- File: `.hampreset`, plain JSON
  `{ "format": "The Ham Preset", "version": 1, "name", "category", "params": {id: value} }`. Missing parameters load
  at their defaults, values are clamped, and master is never stored. `PresetManager::migrate()` and
  `hamMigratePreset` (JS) bring old versions up to date, as in Brass Knuckles.
- Factory presets are also host programs (`"<Category> - <Name>"`).

**The Ham:** 50 presets (5 categories × 10). The file adds the optional blocks `"registrations"` (preset keys per
manual) and `"cc"` (MIDI learn). Neither is required: a preset without them leaves the current ones unchanged.
Drawbar values are written as integers.

## 8. Web build

Inherited unchanged: `build-web.ps1` inlines the CSS and scripts, and injects `FACTORY_JSON`, `factory.js`,
`dsp-worklet.js` and `web-host.js` just before `engine-juce.js`. It refuses code containing `</script` and adds a
GENERATED banner and the version from `CMakeLists.txt`. `web-host.js` provides `window.__JUCE__.backend`. Audio
runs in an AudioWorklet when served and through a ScriptProcessor on `file://`. The page uses Web MIDI and stores
presets in localStorage or a linked folder. `dsp-worklet.js` is one self-contained function and a
**line-for-line twin** of `SynthEngine.h` and `dsp/*.h`.

**The Ham:** the output is also copied to `docs/index.html` (section 1). `web-host.js` also emits `rotor` events at
30 Hz from the worklet's reported rotor state, so `cabinet.js` runs unchanged in both builds.

## 9. Tests

| Command | What |
|---|---|
| `TheHamTests.exe` | parameter table, presets, file round trip, old/partial/garbage files, engine checks |
| `--dump-params` | C++ parameter table as JSON (diffed by `check-params.ps1`) |
| `--parity <scenarios.json>` | C++ render of each scenario: RMS per window + peak as JSON |
| `tests\run-parity.ps1` | the same scenarios in headless Edge through `dsp-worklet.js`, compared per window (tolerance 1e-4, as in Brass Knuckles) |
| `--bench`, `--loudness`, `--render`, `--render-all`, `--host-check` | as in Brass Knuckles |
| `tests\run-ui-tests.ps1` | the web build in headless Edge, from disk and served |

Scenarios: `{ name, preset?, params?, blocks, events: [[block, type, …]] }`.

**The Ham** adds event types `manual` (channel/manual routing), `presetKey`, `motor` and `speed`. It adds organ
checks: wheel frequencies, wiring and foldback, percussion triggering, scanner depth order, rotor speed and ramp
times, the horn leading the drum, clicks on speed changes, aliasing, mono compatibility, and a UI test that the
cabinet drawing follows the `rotor` events.

## 10. Docs and release style

- README: version in the title, a one-table build overview, screenshots, "What it does" bullet lists, per-build
  sections, release history with "still to confirm by hand", known limitations, layout.
- `vst/README.md`: signal flow, host behaviour, feature tables, preset format, tests table, layout, bridge events,
  troubleshooting.
- Plain, concrete language. Numbers are measured, not guessed.

**The Ham:** "Hammond" and "Leslie" are trademarks. They never appear in the plugin name, the UI or preset
names. Use "tonewheel", "rotary", "Rotor Cab" and the like. The docs may say "modeled on the B-3 and the 122".
