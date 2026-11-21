# ============================================================================
#  UI tests in headless Edge:
#   1. plugin UI: vst/ui served over http with tests/ui-tests.js injected (a mocked
#      JUCE bridge plus the checks), in real time (requestAnimationFrame runs);
#      the page POSTs its results to /result
#   2. web build: web/index.html (built first by webhost/build-web.ps1) with
#      tests/web-tests.js appended, twice: opened from disk (file://, ScriptProcessor
#      audio, Edge's virtual time, results read from the DOM dump) and served over
#      http (AudioWorklet audio in real time: stats, rotor events, the cabinet drawing)
#  Usage: powershell -ExecutionPolicy Bypass -File run-ui-tests.ps1 [-Port 8791] [-Plugin | -Web]
# ============================================================================
param([int]$Port = 8791, [int]$TimeoutSec = 90, [switch]$Plugin, [switch]$Web)
$ErrorActionPreference = 'Stop'
$vst = Split-Path $PSScriptRoot -Parent
$repo = Split-Path $vst -Parent
$ui = Join-Path $vst 'ui'
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }
$utf8 = [Text.UTF8Encoding]::new($false)
$mime = @{ '.html' = 'text/html; charset=utf-8'; '.js' = 'text/javascript; charset=utf-8'; '.css' = 'text/css; charset=utf-8'; '.svg' = 'image/svg+xml' }
$tmp = Join-Path $env:TEMP 'theham-uitests'
New-Item -ItemType Directory -Force $tmp | Out-Null
if (-not $Plugin -and -not $Web) { $Plugin = $true; $Web = $true }

# Serves test.html (the given bytes), __tests__/* from this folder and everything else from $root (if any),
# in real time, until the page POSTs its result to /result. Returns the result text.
function Invoke-ServedRun([string]$label, [byte[]]$pageBytes, [string]$root) {
  $listener = [System.Net.HttpListener]::new()
  $listener.Prefixes.Add("http://localhost:$Port/")
  $listener.Start()
  $profile = Join-Path $tmp "profile-$label"
  # headless pages count as hidden: keep timers and animation frames running; let audio start without a gesture
  $proc = Start-Process -FilePath $edge -PassThru -ArgumentList @('--headless=new', '--disable-gpu', '--no-first-run', "--user-data-dir=$profile",
      '--window-size=1300,1050', '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
      '--disable-backgrounding-occluded-windows', '--disable-features=CalculateNativeWinOcclusion',
      '--autoplay-policy=no-user-gesture-required', "http://localhost:$Port/test.html")
  $result = $null
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  try {
    while (-not $result -and (Get-Date) -lt $deadline) {
      $task = $listener.GetContextAsync()
      while (-not $task.AsyncWaitHandle.WaitOne(200)) { if ((Get-Date) -gt $deadline) { break } }
      if (-not $task.IsCompleted) { break }
      $ctx = $task.Result
      $path = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath).TrimStart('/')
      $res = $ctx.Response
      if ($ctx.Request.HttpMethod -eq 'POST') {
        $result = [IO.StreamReader]::new($ctx.Request.InputStream, [Text.Encoding]::UTF8).ReadToEnd()
      } elseif ($path -eq 'test.html') {
        $res.ContentType = $mime['.html']
        $res.OutputStream.Write($pageBytes, 0, $pageBytes.Length)
      } else {
        $file = if ($path -like '__tests__/*') { Join-Path $PSScriptRoot ($path.Substring(10)) } elseif ($root) { Join-Path $root $path } else { '' }
        if ($file -and (Test-Path $file -PathType Leaf)) {
          $bytes = [IO.File]::ReadAllBytes($file)
          $res.ContentType = $mime[[IO.Path]::GetExtension($file)]
          $res.OutputStream.Write($bytes, 0, $bytes.Length)
        } else { $res.StatusCode = 404 }
      }
      $res.Close()
    }
  } finally {
    $listener.Stop()
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
  }
  if (-not $result) { return "FAILED`nno result within $TimeoutSec s ($label)" }
  return $result
}

# Opened from disk: Edge's virtual time runs the page quickly and the results are read from the DOM dump.
function Invoke-DumpRun([string]$label, [string]$pagePath) {
  $profile = Join-Path $tmp "profile-$label"
  $domFile = Join-Path $tmp "dom-$label.txt"
  Start-Process -FilePath $edge -Wait -RedirectStandardOutput $domFile -RedirectStandardError (Join-Path $tmp "err-$label.txt") -ArgumentList @(
      '--headless=new', '--disable-gpu', '--no-first-run', "--user-data-dir=$profile", '--window-size=1300,1050',
      '--virtual-time-budget=900000', '--dump-dom', ('file:///' + ($pagePath -replace '\\', '/')))
  $dom = [IO.File]::ReadAllText($domFile, $utf8)
  if ($dom -notmatch '(?s)<pre id="ham-test-out">(.*?)</pre>') { return "FAILED`nno output in the DOM dump ($label, see $domFile)" }
  return [Net.WebUtility]::HtmlDecode($Matches[1])
}

$fail = 0; $count = 0
function Report([string]$label, [string]$result) {
  Write-Host "[$label]"
  $lines = $result -split "`n"
  if ($lines[0] -notmatch '^(PASSED|FAILED)') { $script:fail++; Write-Host "  FAIL $result"; return }
  foreach ($l in $lines[1..($lines.Count - 1)]) {
    if (-not $l.Trim()) { continue }
    $script:count++
    if ($l.StartsWith('FAIL')) { $script:fail++ }
    Write-Host "  $l"
  }
}

if ($Plugin) {
  # the test page: the real index.html with the test script before params.js
  $page = [IO.File]::ReadAllText((Join-Path $ui 'index.html'), $utf8)
  $page = $page.Replace('<script src="js/params.js"></script>', '<script src="__tests__/ui-tests.js"></script>' + "`n" + '<script src="js/params.js"></script>')
  Report 'plugin UI, mocked bridge, served' (Invoke-ServedRun 'plugin' $utf8.GetBytes($page) $ui)
}

if ($Web) {
  & (Join-Path $vst 'webhost\build-web.ps1') | Out-Null
  $html = [IO.File]::ReadAllText((Join-Path $repo 'web\index.html'), $utf8)
  $tests = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'web-tests.js'), $utf8)
  # error collector first (before any app script), test script last
  $collector = '<script>window.__hamErrors = []; window.addEventListener("error", (e) => window.__hamErrors.push(String(e.message || e)));' +
               ' window.addEventListener("unhandledrejection", (e) => window.__hamErrors.push("unhandled rejection: " + String(e.reason)));</script>'
  $html = $html.Replace('<head>', "<head>`n$collector").Replace('</body>', "<script>`n$tests`n</script>`n</body>")
  $pagePath = Join-Path $tmp 'web.html'
  [IO.File]::WriteAllText($pagePath, $html, $utf8)
  Report 'web build, from disk (file://)' (Invoke-DumpRun 'web-file' $pagePath)
  Report 'web build, served (http)' (Invoke-ServedRun 'web-http' $utf8.GetBytes($html) '')
}

if ($fail -eq 0) { Write-Host "UI TESTS PASSED ($count checks)"; exit 0 }
Write-Host "UI TESTS: $fail of $count FAILED"; exit 1
