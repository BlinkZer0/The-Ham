# ============================================================================
#  Level-matches the factory presets: renders each one (TheHamTests
#  --loudness), then rewrites every preset's "ampLevel" in
#  presets/factory_presets.json so all of them land on the same loudness.
#  Run it again after changing a preset's sound. Build the tests first.
#  Usage: powershell -ExecutionPolicy Bypass -File level-match.ps1 [-TargetDb -18] [-DryRun]
# ============================================================================
param([double]$TargetDb = -18, [switch]$DryRun)
$ErrorActionPreference = 'Stop'

$vst = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $env:LOCALAPPDATA 'TheHamVST\build\TheHamTests_artefacts\Release\TheHamTests.exe'
if (-not (Test-Path $exe)) { throw "Build TheHamTests first: $exe" }
$file = Join-Path $vst 'presets\factory_presets.json'

$loud = (& $exe --loudness | Out-String) | ConvertFrom-Json
$text = [IO.File]::ReadAllText($file)

# Every preset in "categories" carries exactly one "ampLevel"; templates never do.
$start = $text.IndexOf('"categories"')
$head = $text.Substring(0, $start)
$body = $text.Substring($start)
$found = [regex]::Matches($body, '"ampLevel":\s*(-?[0-9.]+)')
if ($found.Count -ne $loud.Count) { throw "found $($found.Count) ampLevel entries for $($loud.Count) presets: every preset needs one" }

$i = 0
$body = [regex]::Replace($body, '"ampLevel":\s*(-?[0-9.]+)', {
    param($m)
    $p = $loud[$script:i]; $script:i++
    $noAmpDb = 20 * [Math]::Log10([Math]::Max([double]$p.loudnessNoAmp, 1e-9))
    $new = [Math]::Round([Math]::Max([double]-30.0, [Math]::Min([double]6.0, [double]($TargetDb - $noAmpDb))), 1)
    Write-Host ('{0,-26} {1,-16} {2,7:F2} dB -> ampLevel {3,6:F1}{4}' -f $p.name, $p.category, $p.loudnessDb, $new,
        $(if ($TargetDb - $noAmpDb -gt 6 -or $TargetDb - $noAmpDb -lt -30) { '  (CLAMPED: change the sound)' } else { '' }))
    '"ampLevel": ' + $new.ToString([Globalization.CultureInfo]::InvariantCulture)
})

if ($DryRun) { Write-Host 'dry run: nothing written'; exit 0 }
[IO.File]::WriteAllText($file, $head + $body, [Text.UTF8Encoding]::new($false))
Write-Host "Wrote $file. Rebuild so the plugin and tests pick it up."
