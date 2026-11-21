// ============================================================================
//  Factory presets for the web build: resolves vst/presets/factory_presets.json
//  exactly like PresetManager::loadFactory (C++):
//    preset values = parameter defaults <- template chain (base first) <- own params
//  Values are clamped to the parameter range (not rounded), like applyParamObject.
//  Also the preset-key registrations block, the .hampreset file format and the
//  preset-file migration steps (twin of PresetManager::migrate).
//  Needs params.js (PARAM_BY_ID, PARAM_DEFS, clamp, parseRegistration, registrationString).
// ============================================================================
'use strict';

const HAM_PRESET_VERSION = 1;
const HAM_PRESET_FORMAT = 'The Ham Preset';
const HAM_PRESET_EXT = '.hampreset';

// Fixed registrations of the preset keys C#..A: the factory chart (dsp/Wiring.h kFactoryRegistrations;
// the engine has its own copy inside dsp-worklet.js).
const HAM_FACTORY_REGISTRATIONS = [
  [0, 0, 5, 3, 2, 0, 0, 0, 0],   // C#  Stopped Flute
  [0, 0, 4, 4, 3, 2, 0, 0, 0],   // D   Dulciana
  [0, 0, 8, 7, 4, 0, 0, 0, 0],   // D#  French Horn
  [0, 0, 4, 5, 4, 4, 2, 2, 2],   // E   Salicional
  [0, 0, 5, 4, 0, 3, 0, 0, 0],   // F   Flutes 8' & 4'
  [0, 0, 4, 6, 7, 5, 3, 0, 0],   // F#  Oboe Horn
  [0, 0, 5, 6, 4, 4, 3, 2, 0],   // G   Swell Diapason
  [0, 0, 6, 8, 7, 6, 5, 4, 0],   // G#  Trumpet
  [3, 2, 7, 6, 4, 5, 2, 2, 2],   // A   Full Swell
];

// [manual 0 upper / 1 lower][key 0..8][footage], a fresh copy of the factory chart
function hamFactoryRegistrations() {
  return [0, 1].map(() => HAM_FACTORY_REGISTRATIONS.map(r => r.slice()));
}

// Copies every known { paramId: number } into out (master excluded). Returns how many were found.
function hamApplyParamObject(obj, out) {
  let found = 0;
  if (!obj || typeof obj !== 'object') return 0;
  for (const id of Object.keys(obj)) {
    const d = PARAM_BY_ID[id];
    const raw = obj[id];
    if (!d || id === 'master' || !(typeof raw === 'number' || typeof raw === 'boolean')) continue;
    out[id] = clamp(Number(raw), d.min, d.max);
    ++found;
  }
  return found;
}

// "registrations": { "upper": ["00 5320 000", ... 9 strings, keys C#..A], "lower": [...] }
// Entries that are missing or not a registration keep the factory value (PresetManager readRegistrations).
function hamReadRegistrations(obj) {
  const r = hamFactoryRegistrations();
  if (!obj || typeof obj !== 'object') return r;
  ['upper', 'lower'].forEach((name, m) => {
    const list = obj[name];
    if (!Array.isArray(list)) return;
    for (let k = 0; k < Math.min(9, list.length); ++k) {
      const bars = parseRegistration(list[k]);
      if (bars) r[m][k] = bars;
    }
  });
  return r;
}

function hamRegistrationsToJson(r) {
  return { upper: r[0].map(registrationString), lower: r[1].map(registrationString) };
}

// [{ name, category, values, registrations }] where values holds only what the template chain + preset set.
function hamFactoryPresets(json) {
  const templates = (json && json.templates) || {};
  const applyTemplate = (name, out, depth) => {
    if (!name || depth > 8) return;
    const t = templates[name];
    if (!t || typeof t !== 'object') { console.warn(`unknown preset template "${name}"`); return; }
    applyTemplate(t.base, out, depth + 1);
    hamApplyParamObject(t.params, out);
  };
  const list = [];
  for (const cat of (json && json.categories) || []) {
    for (const p of cat.presets || []) {
      const values = {};
      applyTemplate(p.base, values, 0);
      hamApplyParamObject(p.params, values);
      list.push({ name: String(p.name), category: String(cat.name), values, registrations: hamReadRegistrations(p.registrations) });
    }
  }
  if (!list.length) list.push({ name: 'Init', category: 'Factory', values: {}, registrations: hamFactoryRegistrations() });
  return list;
}

// Brings a preset object written by an older version up to HAM_PRESET_VERSION (in place).
// Same steps as PresetManager::migrate. Plain parameter additions need no step.
function hamMigratePreset(json) {
  if (!json || typeof json !== 'object') return json;
  let version = Number.isFinite(Number(json.version)) ? Math.trunc(Number(json.version)) : 1;
  version = Math.max(version, 1);
  // while (version < HAM_PRESET_VERSION) { switch (version) { case 1: /* 1 -> 2 */ break; } ++version; }
  if (version < HAM_PRESET_VERSION) version = HAM_PRESET_VERSION;
  json.version = version;
  return json;
}
