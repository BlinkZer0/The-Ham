# ============================================================================
#  Screenshots of the plugin UI for the README: serves vst/ui with
#  tests/screenshot-mock.js standing in for the JUCE bridge (it feeds a factory
#  preset, lit keys, spinning rotors and the stats), then captures each page in
#  headless Edge. Writes docs/screenshots/<page>.png.
#  Usage: powershell -ExecutionPolicy Bypass -File screenshots.ps1 [-Port 8792] [-Out <folder>]
# ============================================================================
param([int]$Port = 8792, [string]$Out = '')
$ErrorActionPreference = 'Stop'
$vst = Split-Path $PSScriptRoot -Parent
$repo = Split-Path $vst -Parent
$ui = Join-Path $vst 'ui'
if (-not $Out) { $Out = Join-Path $repo 'docs\screenshots' }
New-Item -ItemType Directory -Force $Out | Out-Null
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }

# the page: the real index.html with the factory presets and the mock before params.js
$factory = [IO.File]::ReadAllText((Join-Path $vst 'presets\factory_presets.json'))
$page = [IO.File]::ReadAllText((Join-Path $ui 'index.html'))
$inject = '<script id="factory-json" type="application/json">' + $factory.Replace('</script', '<\/script') + '</script>' + "`n" +
          '<script src="__tests__/screenshot-mock.js"></script>' + "`n" + '<script src="js/params.js"></script>'
$page = $page.Replace('<script src="js/params.js"></script>', $inject)
$pageBytes = [Text.UTF8Encoding]::new($false).GetBytes($page)

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
$mime = @{ '.html' = 'text/html; charset=utf-8'; '.js' = 'text/javascript; charset=utf-8'; '.css' = 'text/css; charset=utf-8'; '.svg' = 'image/svg+xml' }
$serving = $true
$server = {
  param($listener, $ui, $tests, $pageBytes, $mime)
  while ($true) {
    try { $ctx = $listener.GetContext() } catch { break }
    $path = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath).TrimStart('/')
    $res = $ctx.Response
    if ($path -eq 'shot.html') {
      $res.ContentType = $mime['.html']; $res.OutputStream.Write($pageBytes, 0, $pageBytes.Length)
    } else {
      $file = if ($path -like '__tests__/*') { Join-Path $tests ($path.Substring(10)) } else { Join-Path $ui $path }
      if (Test-Path $file -PathType Leaf) {
        $bytes = [IO.File]::ReadAllBytes($file)
        $res.ContentType = $mime[[IO.Path]::GetExtension($file)]
        $res.OutputStream.Write($bytes, 0, $bytes.Length)
      } else { $res.StatusCode = 404 }
    }
    $res.Close()
  }
}
$job = [PowerShell]::Create().AddScript($server).AddArgument($listener).AddArgument($ui).AddArgument($PSScriptRoot).AddArgument($pageBytes).AddArgument($mime)
$handle = $job.BeginInvoke()
$profile = Join-Path $env:TEMP 'ham-screenshots-profile'
try {
  foreach ($p in 'drawbars', 'voice', 'cabinet', 'play', 'fx') {
    $png = Join-Path $Out "$p.png"
    if (Test-Path $png) { Remove-Item $png -Force }
    # headless pages count as hidden: keep timers and animation frames running, then give the page 2.5 s
    # Edge reports the written file on stderr; that is not an error
    $ErrorActionPreference = 'Continue'
    & $edge --headless=new --disable-gpu "--user-data-dir=$profile" --window-size=1300,1010 --hide-scrollbars `
        --disable-background-timer-throttling --disable-renderer-backgrounding --disable-backgrounding-occluded-windows `
        --disable-features=CalculateNativeWinOcclusion --virtual-time-budget=2500 "--screenshot=$png" `
        "http://localhost:$Port/shot.html?page=$p" 2>&1 | Out-Null
    $ErrorActionPreference = 'Stop'
    if (Test-Path $png) { Write-Host "wrote $png" } else { Write-Host "FAILED $p" }
  }
} finally {
  $listener.Stop()
  $job.Stop() | Out-Null
  $job.Dispose()
}
