# ============================================================================
#  Checks that vst/ui/js/params.js matches vst/Source/Params.h exactly:
#  same order, IDs, types, ranges, defaults, units, log skew, choice lists and
#  "not in presets" flags. The C++ table comes from TheHamTests
#  --dump-params; the JS table is evaluated in headless Edge.
#  Usage: powershell -ExecutionPolicy Bypass -File check-params.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'
$vst = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $env:LOCALAPPDATA 'TheHamVST\build\TheHamTests_artefacts\Release\TheHamTests.exe'
if (-not (Test-Path $exe)) { throw "Build TheHamTests first: $exe" }
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found (needed to evaluate params.js headless)' }

$cpp = (& $exe --dump-params | Out-String) | ConvertFrom-Json

$tmp = Join-Path $env:TEMP 'ham-check-params'
New-Item -ItemType Directory -Force $tmp | Out-Null
$page = @'
<!doctype html><html><head><title>running</title></head><body><pre id="out"></pre>
<script>PARAMS_JS</script>
<script>
try {
  const out = PARAM_DEFS.map(d => ({ id: d.id, type: d.type, min: d.min, max: d.max, def: d.def, unit: d.unit || '',
    log: d.skew === 'log', noPreset: !!d.noPreset, choices: d.choices || [] }));
  document.getElementById('out').textContent = JSON.stringify(out);
  document.title = 'done';
} catch (e) { document.title = 'ERROR ' + (e.stack || e); }
</script></body></html>
'@
$page = $page.Replace('PARAMS_JS', [IO.File]::ReadAllText((Join-Path $vst 'ui\js\params.js')))
$pagePath = Join-Path $tmp 'params.html'
[IO.File]::WriteAllText($pagePath, $page, [Text.UTF8Encoding]::new($false))
$domFile = Join-Path $tmp 'dom.txt'
Start-Process -FilePath $edge -Wait -RedirectStandardOutput $domFile -RedirectStandardError (Join-Path $tmp 'err.txt') -ArgumentList @(
    '--headless=new', '--disable-gpu', "--user-data-dir=$tmp\profile", '--dump-dom', ('file:///' + ($pagePath -replace '\\', '/')))
$dom = [IO.File]::ReadAllText($domFile)
if ($dom -match '(?s)<title>ERROR(.*?)</title>') { throw "params.js error: $([Net.WebUtility]::HtmlDecode($Matches[1]))" }
if ($dom -notmatch '(?s)<pre id="out">(.*?)</pre>') { throw 'params.js produced no output' }
$js = [Net.WebUtility]::HtmlDecode($Matches[1]) | ConvertFrom-Json

$fail = 0
function Report($msg) { $script:fail++; Write-Host "MISMATCH $msg" }
if ($cpp.Count -ne $js.Count) { Report "count: C++ $($cpp.Count), JS $($js.Count)" }
$n = [Math]::Min($cpp.Count, $js.Count)
for ($i = 0; $i -lt $n; $i++) {
    $c = $cpp[$i]; $j = $js[$i]
    $at = "#$i $($c.id)"
    if ($c.id -ne $j.id) { Report "$at id: C++ '$($c.id)', JS '$($j.id)'"; continue }
    foreach ($f in 'type', 'unit', 'log', 'noPreset') { if ("$($c.$f)" -ne "$($j.$f)") { Report "$at ${f}: C++ '$($c.$f)', JS '$($j.$f)'" } }
    foreach ($f in 'min', 'max', 'def') {
        $a = [double]$c.$f; $b = [double]$j.$f
        if ([Math]::Abs($a - $b) -gt 1e-6 * [Math]::Max(1, [Math]::Abs($b))) { Report "$at ${f}: C++ $a, JS $b" }
    }
    if ((@($c.choices) -join '|') -ne (@($j.choices) -join '|')) { Report "$at choices differ" }
}
if ($fail -eq 0) { Write-Host "PARAMS OK ($($cpp.Count) parameters match)"; exit 0 }
Write-Host "PARAMS MISMATCH: $fail difference(s)"; exit 1
