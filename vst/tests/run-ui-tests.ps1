# ============================================================================
#  UI tests: serves vst/ui over http with tests/ui-tests.js injected (a mocked
#  JUCE bridge plus the checks), opens it in headless Edge in real time, and
#  waits for the page to POST its results to /result.
#  Usage: powershell -ExecutionPolicy Bypass -File run-ui-tests.ps1 [-Port 8791]
# ============================================================================
param([int]$Port = 8791, [int]$TimeoutSec = 90)
$ErrorActionPreference = 'Stop'
$vst = Split-Path $PSScriptRoot -Parent
$ui = Join-Path $vst 'ui'
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }

# the test page: the real index.html with the test script before params.js
$page = [IO.File]::ReadAllText((Join-Path $ui 'index.html'))
$page = $page.Replace('<script src="js/params.js"></script>', '<script src="__tests__/ui-tests.js"></script>' + "`n" + '<script src="js/params.js"></script>')
$pageBytes = [Text.UTF8Encoding]::new($false).GetBytes($page)

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
$profile = Join-Path $env:TEMP 'ham-ui-tests-profile'
# headless pages count as hidden: keep timers and animation frames running
$proc = Start-Process -FilePath $edge -PassThru -ArgumentList @('--headless=new', '--disable-gpu', "--user-data-dir=$profile",
    '--window-size=1300,1050', '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
    '--disable-backgrounding-occluded-windows', '--disable-features=CalculateNativeWinOcclusion', "http://localhost:$Port/test.html")
$mime = @{ '.html' = 'text/html; charset=utf-8'; '.js' = 'text/javascript; charset=utf-8'; '.css' = 'text/css; charset=utf-8'; '.svg' = 'image/svg+xml' }
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
      $result = [IO.StreamReader]::new($ctx.Request.InputStream).ReadToEnd()
    } elseif ($path -eq 'test.html') {
      $res.ContentType = $mime['.html']
      $res.OutputStream.Write($pageBytes, 0, $pageBytes.Length)
    } else {
      $file = if ($path -like '__tests__/*') { Join-Path $PSScriptRoot ($path.Substring(10)) } else { Join-Path $ui $path }
      if (Test-Path $file -PathType Leaf) {
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
if (-not $result) { Write-Host 'UI TESTS: no result (timeout)'; exit 1 }
Write-Host $result
if ($result.StartsWith('PASSED')) { Write-Host 'UI TESTS PASSED'; exit 0 }
Write-Host 'UI TESTS FAILED'; exit 1
