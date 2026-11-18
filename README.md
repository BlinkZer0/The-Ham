# The Ham: v0.1 (phase 1 of 6)

The Ham is a tonewheel organ modeled on the B-3, to be played through rotary cabinets modeled on the 122 and its
relatives. It is a model of the real instrument's architecture, not an oscillator-per-note synth: 91 tonewheels run all the
time on a gear train, every key closes nine contacts onto busbars, and the drawbars mix the busbars. It is the third
instrument in the family after [E-Piano](https://github.com/BlinkZer0/E-PianoVST) and
[Brass Knuckles](https://github.com/BlinkZer0/Brass-Knuckles), and is built the same way.

| Build | Where | Status |
|---|---|---|
| **VST3 plugin** + Standalone | [`vst/`](vst/README.md) | phase 1: tonewheel generator, wiring, drawbars, key click, manuals and pedals; all automated checks pass |
| **Web build** | `web/index.html` (also served at https://blinkzer0.github.io/The-Ham/) | phase 6 |

The plan, the phases and the measured results are in [docs/PLAN.md](docs/PLAN.md). The house rules are in
[docs/CONVENTIONS.md](docs/CONVENTIONS.md).

## What it does so far (phase 1)

### The tonewheel generator
- **91 wheels that never stop.** They are shared by every key and never reset by a key press, so chords are phase-locked
  and a re-struck note joins its wheel wherever it is.
- **Tuned by the gear train:** f = 20 rev/s × gear ratio × teeth. A is exactly 440 Hz, and the other notes sit up to
  2 cents off equal temperament, as on the instrument. **Tuning** A = 430–450 Hz, **60/50 Hz mains** (a 60 Hz organ on
  50 Hz runs 5/6 slow), **Transpose** ±12.
- The lowest wheels carry a little 3rd harmonic. The upper wheels fall off gently (filter capacitors).
- **Motor Start / Run:** Start spins the generator up; Run keeps it turning but can't start it. With both off (or a
  **Motor Off CC**) it coasts down: the power-off pitch dive.

### Keys, contacts and the key click
- Each key has **9 contacts** wired to wheels by the B-3 chart, **foldback** included. The bottom octave of the 16' folds
  up and the upper footages fold down near the top. Wheels 1–12 belong to the pedals. Key taper is per key range and
  footage.
- **Key click from the contacts themselves:** the nine contacts close at slightly different moments and bounce. No
  click sample or noise is added. **Click** per manual (0% = smooth onsets) and **Key-Up click** on/off.

### Drawbars
- Real **drawbars**: they pull down out of the panel, have 9 detents, are numbered 1–8 on the shaft, and have
  brown/white/black tips by footage. Drag (Shift = continuous), mouse wheel, arrow keys or 0–8, double-click = default.
  Every drawbar is host-automatable.
- **Two sets per manual** (A♯ and B preset keys) plus **pedal 16' and 8'**. The active set is lit. Each set shows its
  **registration string** (`88 8000 000`), and you can type one in.
- About **3 dB per step** (8 = 0 dB, 1 = −21 dB, 0 = off).
- **Preset keys** in reverse colours left of each manual: C cancel, C♯–A fixed registrations, A♯ set A, B set B.
- **Drawbar CC maps** for hardware controllers: B4D (CC 12–20 / 21–29, the default), Nord Electro (16–24 / 70–78),
  VK / CX-3 (70–78).

### Manuals and MIDI
- **Upper** (61 keys, channel 1), **Lower** (61, channel 2), **Pedals** (25, channel 3), each channel settable, or
  **Split** for one keyboard (lower | upper, optional pedal zone). Pedal MIDI Base sets the lowest pedal's note.
- **Velocity off** by default (organ keys are switches); optionally to volume or to click.
- On-screen manuals, preset keys and pedalboard; computer keys A–' play the upper manual.

### Coming next
Percussion, scanner vibrato/chorus, leakage, Age, preamp and expression (phase 2). The rotary cabinets, outboard pedals
and reverb (phase 3). The animated cabinet (phase 4). 50 presets (phase 5). The web build (phase 6). Their parameters
already exist and are saved with presets, so projects made now keep loading.

## Build and install

Requirements: Windows, Visual Studio 2022 or 2026 with the C++ workload, Git, and the WebView2 runtime (built into
Windows 11). JUCE 8.0.17 is reused from Brass Knuckles' or E-Piano's build folder, or downloaded on the first build.

```
vst\build.bat      build the VST3, the Standalone app and the test tool (Release)
vst\install.bat    copy "The Ham.vst3" to C:\Program Files\Common Files\VST3 (asks for admin)
```

In the host, The Ham is an instrument with MIDI in and stereo out. All 143 parameters can be automated, and the factory
presets are host programs. Right-click any control for the host's parameter menu.

## Presets

10 starter presets (two per category: Jazz, Gospel, Rock, Blues & Soul, Ballad/Classic/Theatre), level-matched to within
0.05 dB. The 50 come in phase 5. User presets are `.hampreset` files (plain JSON, versioned) in
**`Documents\The Ham\Presets`**. Drawbars are saved as whole steps.

## Status and known limitations

- **Still to confirm by hand** in Minihost Modular: everything in phase 1 (see "What to listen for" in
  [docs/PLAN.md](docs/PLAN.md)).
- The output is the dry generator, mono on both channels, until the cabinet arrives in phase 3.
- Pedal drawbar mixtures and the per-wheel voicing are approximations. No published data was found yet; see the
  reference table in the plan.
- Windows only for now. The code is portable JUCE, but no macOS build has been set up.

"B-3" and "122" name the instruments this is modeled on. The plugin, its UI and its presets use generic names.

## Repository layout

```
vst/                      VST3 plugin (JUCE 8): Source/, ui/, presets/, tests/  (webhost/ in phase 6)
docs/                     CONVENTIONS.md, PLAN.md, screenshots/ (index.html in phase 6)
start.bat, serve.ps1      local server (serve.ps1 -Ui previews the UI without an engine)
```
