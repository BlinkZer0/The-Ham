# ============================================================================
#  DSP parity: renders every scenario in tests/parity_scenarios.json with the
#  C++ engine (TheHamTests --parity) and with the JS engine (webhost/dsp-worklet.js,
#  through the web build's SynthProcessor) in headless Edge, then compares RMS
#  per channel in 16 windows + peak.
#  Also prints how fast each engine rendered (% of real time, one core).
#  Usage: powershell -ExecutionPolicy Bypass -File run-parity.ps1 [-Tolerance 1e-4] [-Full]
# ============================================================================
param([double]$Tolerance = 1e-4, [switch]$Full)
$ErrorActionPreference = 'Stop'

$vst = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $env:LOCALAPPDATA 'TheHamVST\build\TheHamTests_artefacts\Release\TheHamTests.exe'
if (-not (Test-Path $exe)) { throw "Build TheHamTests first: $exe" }
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found (needed to run the JS engine headless)' }

$tmp = Join-Path $env:TEMP 'theham-parity'
New-Item -ItemType Directory -Force $tmp | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$read = { param($p) [IO.File]::ReadAllText((Join-Path $vst $p), $utf8) }

# ---- C++ -------------------------------------------------------------------
Push-Location $PSScriptRoot
try { $cppJson = (& $exe --parity 'parity_scenarios.json' | Out-String).Trim() } finally { Pop-Location }
if (-not $cppJson.StartsWith('[')) { throw "C++ parity run failed: $cppJson" }

# ---- JS (same scenarios, same factory presets) + comparison, in headless Edge ----
$page = @'
<!doctype html><html><head><meta charset="utf-8"><title>running</title></head><body><pre id="out"></pre>
<script>@@PARAMS_JS@@</script>
<script>@@FACTORY_JS@@</script>
<script>@@DSP_JS@@</script>
<script>@@RUNNER_JS@@</script>
<script>@@COMPARE_JS@@</script>
<script>
try {
  const sampleRate = 48000;
  let Proc = null;
  class AudioWorkletProcessor { constructor() { this.port = { postMessage() {}, onmessage: null }; } }
  const registerProcessor = (n, c) => { Proc = c; };
  eval('(' + synthWorkletModule.toString() + ')()');
  const js = hamParityRun(@@SCENARIOS_JSON@@, @@FACTORY_JSON@@, Proc);
  const r = hamParityCompare(@@CPP_JSON@@, js, @@TOLERANCE@@);
  document.getElementById('out').textContent = JSON.stringify(r);
  document.title = 'done';
} catch (e) { document.title = 'ERROR ' + (e.stack || e); }
</script></body></html>
'@
$page = $page.Replace('@@PARAMS_JS@@', (& $read 'ui\js\params.js')).
              Replace('@@FACTORY_JS@@', (& $read 'webhost\factory.js')).
              Replace('@@DSP_JS@@', (& $read 'webhost\dsp-worklet.js')).
              Replace('@@RUNNER_JS@@', (& $read 'tests\parity-runner.js')).
              Replace('@@COMPARE_JS@@', (& $read 'tests\parity-compare.js')).
              Replace('@@SCENARIOS_JSON@@', (& $read 'tests\parity_scenarios.json')).
              Replace('@@FACTORY_JSON@@', (& $read 'presets\factory_presets.json')).
              Replace('@@CPP_JSON@@', $cppJson).
              Replace('@@TOLERANCE@@', $Tolerance.ToString([Globalization.CultureInfo]::InvariantCulture))
$pagePath = Join-Path $tmp 'parity.html'
[IO.File]::WriteAllText($pagePath, $page, $utf8)
$domFile = Join-Path $tmp 'dom.txt'
Start-Process -FilePath $edge -Wait -RedirectStandardOutput $domFile -RedirectStandardError (Join-Path $tmp 'err.txt') -ArgumentList @(
    '--headless=new', '--disable-gpu', '--no-first-run', "--user-data-dir=$tmp\profile", '--virtual-time-budget=600000', '--dump-dom',
    ('file:///' + ($pagePath -replace '\\', '/')))
$dom = [IO.File]::ReadAllText($domFile)
if ($dom -match '(?s)<title>ERROR(.*?)</title>') { throw "JS engine error: $([Net.WebUtility]::HtmlDecode($Matches[1]))" }
if ($dom -notmatch '(?s)<pre id="out">(.*?)</pre>') { throw 'JS run produced no output (see ' + $domFile + ')' }
$r = [Net.WebUtility]::HtmlDecode($Matches[1]) | ConvertFrom-Json

# ---- report ------------------------------------------------------------------
$lines = @($r.lines)
if ($Full) { $lines | ForEach-Object { Write-Host $_ } }
else {
    Write-Host $lines[0]
    $lines[1..($lines.Count - 3)] | Where-Object { $_ -notmatch ' ok$' } | ForEach-Object { Write-Host $_ }
    Write-Host ('({0} scenarios ok; -Full lists them all)' -f @($lines | Where-Object { $_ -match ' ok$' }).Count)
    Write-Host $lines[-2]
    Write-Host $lines[-1]
}
if ($r.failed -eq 0) { exit 0 } else { exit 1 }
