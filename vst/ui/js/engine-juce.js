// ============================================================================
//  JUCE bridge: talks to the C++ processor through JUCE 8's native
//  integration (window.__JUCE__.backend). In the web build (phase 6),
//  vst/webhost/web-host.js provides the same backend on top of Web Audio.
// ============================================================================
'use strict';

class JuceEngine {
  constructor() {
    this.backend = (window.__JUCE__ && window.__JUCE__.backend) || null;
    this.connected = !!this.backend;
    this.handlers = {};
    if (this.backend) {
      for (const ev of ['params', 'notes', 'stats', 'config', 'presetList', 'preset', 'saveResult', 'message', 'midiInputs', 'rotor', 'registrations', 'learn']) {
        this.backend.addEventListener(ev, (data) => { if (this.handlers[ev]) this.handlers[ev](data); });
      }
    }
  }

  on(event, fn) { this.handlers[event] = fn; }
  emit(id, payload) { if (this.backend) this.backend.emitEvent(id, payload || {}); }

  uiReady() { this.emit('uiReady'); }

  // parameters
  setParam(id, value) { this.emit('setParam', { id, value }); }
  beginGesture(id) { this.emit('gesture', { id, begin: true }); }
  endGesture(id) { this.emit('gesture', { id, begin: false }); }

  // performance: manual 0 upper, 1 lower, 2 pedals; key = index on that manual (0 = its lowest C)
  noteOn(manual, key, vel) { this.emit('noteOn', { manual, key, vel }); }
  noteOff(manual, key) { this.emit('noteOff', { manual, key }); }
  panic() { this.emit('panic'); }
  expression(value) { this.emit('expression', { value }); }   // on-screen expression pedal 0..1

  // fixed registrations of the preset keys C#..A (key 0..8), e.g. bars = "00 5320 000"
  setRegistration(manual, key, bars) { this.emit('setRegistration', { manual, key, bars }); }

  // MIDI learn: arm a parameter (the next CC moved is assigned to it), forget it, or cancel
  midiLearn(id) { this.emit('midiLearn', { id }); }
  midiForget(id) { this.emit('midiForget', { id }); }
  learnCancel() { this.emit('learnCancel'); }

  // pick a cabinet model: the host sets it and loads its defaults (speeds, ramps, rotors, baffle, crossover, mics)
  cabinetModel(model) { this.emit('cabinetModel', { model }); }

  // right-click on a control -> the host's parameter menu (automation, MIDI learn...)
  paramMenu(id, x, y) { this.emit('paramMenu', { id, x: Math.round(x), y: Math.round(y) }); }

  // web build only (the plugin takes MIDI from the host)
  selectMidi(id) { this.emit('selectMidi', { id }); }

  // presets
  loadPreset(id) { this.emit('loadPreset', { id }); }
  savePreset(name, overwrite) { this.emit('savePreset', { name, overwrite: !!overwrite }); }
  deletePreset(id) { this.emit('deletePreset', { id }); }
  loadPresetFile() { this.emit('loadPresetFile'); }
  openPresetFolder() { this.emit('openPresetFolder'); }
}
