// Compares C++ and JS parity results (JSON arrays from TheHamTests --parity and
// parity-runner.js). Used by run-parity.ps1; also runs under Node:
//   node parity-compare.js cpp.json js.json [tolerance]
// Error per scenario = max over windows and channels of |a - b| / max(a, b, 1e-4), and the
// same for the peak. Prints a table and returns { failed, worst, lines }.
'use strict';

function hamParityCompare(cpp, js, tolerance) {
  const lines = [];
  let failed = 0, worst = 0;
  const rel = (a, b) => Math.abs(a - b) / Math.max(Math.abs(a), Math.abs(b), 1e-4);
  if (cpp.length !== js.length) { lines.push(`scenario count differs: C++ ${cpp.length}, JS ${js.length}`); failed++; }
  let cppMs = 0, jsMs = 0, audioMs = 0;
  lines.push(`${'scenario'.padEnd(64)} ${'max rel err'.padStart(11)} ${'peak'.padStart(7)} ${'C++ cpu'.padStart(8)} ${'JS cpu'.padStart(8)}  result`);
  for (let i = 0; i < Math.min(cpp.length, js.length); i++) {
    const c = cpp[i], j = js[i];
    let e = rel(c.peak, j.peak);
    for (const ch of ['rmsL', 'rmsR']) for (let w = 0; w < c[ch].length; w++) e = Math.max(e, rel(c[ch][w], j[ch][w]));
    const ok = c.name === j.name && e <= tolerance;
    if (!ok) failed++;
    worst = Math.max(worst, e);
    cppMs += c.ms; jsMs += j.ms; audioMs += c.audioMs;
    const pct = (ms) => `${(100 * ms / c.audioMs).toFixed(1)}%`;
    lines.push(`${c.name.slice(0, 64).padEnd(64)} ${e.toExponential(2).padStart(11)} ${c.peak.toFixed(4).padStart(7)} ${pct(c.ms).padStart(8)} ${pct(j.ms).padStart(8)}  ${ok ? 'ok' : 'FAIL'}`);
  }
  lines.push(`render time for ${(audioMs / 1000).toFixed(0)} s of audio: C++ ${(cppMs / 1000).toFixed(1)} s, JS ${(jsMs / 1000).toFixed(1)} s (JS/C++ = ${(jsMs / Math.max(cppMs, 1)).toFixed(1)}x)`);
  lines.push(failed === 0 ? `PARITY OK (${cpp.length} scenarios, worst ${worst.toExponential(2)}, tolerance ${tolerance})`
                          : `PARITY FAILED in ${failed} scenario(s) (tolerance ${tolerance})`);
  return { failed, worst, lines };
}

if (typeof module !== 'undefined' && require.main === module) {
  const fs = require('fs');
  const [cppFile, jsFile, tol] = process.argv.slice(2);
  const r = hamParityCompare(JSON.parse(fs.readFileSync(cppFile, 'utf8')), JSON.parse(fs.readFileSync(jsFile, 'utf8')), Number(tol) || 1e-4);
  console.log(r.lines.join('\n'));
  process.exit(r.failed ? 1 : 0);
}
