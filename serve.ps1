# Tiny static file server (no installs needed).
#   (default)  serves web/ (the single-file web build); builds it first if it doesn't exist yet
#   -Dev       serves vst/ and opens webhost/dev.html: the web build unbundled, straight from
#              the sources (edit vst/ui or vst/webhost and reload the page, no build step)
#   -Ui        serves vst/ui as a plain UI preview (no audio engine)
# Served over http://localhost the engine runs in an AudioWorklet (its own audio thread, lower
# latency); opened straight from disk (file://) it falls back to a ScriptProcessor.
# Usage:  powershell -ExecutionPolicy Bypass -File serve.ps1 [-Port 8080] [-Dev | -Ui] [-NoBrowser]
param([int]$Port = 8080, [switch]$Dev, [switch]$Ui, [switch]$NoBrowser)

$web = Join-Path $PSScriptRoot 'web'
if ($Dev) { $root = Join-Path $PSScriptRoot 'vst'; $start = 'webhost/dev.html' }
elseif ($Ui) { $root = Join-Path $PSScriptRoot 'vst\ui'; $start = '' }
else {
  if (-not (Test-Path (Join-Path $web 'index.html'))) {
    Write-Host 'web/index.html not found: building it (vst\webhost\build-web.ps1)'
    & (Join-Path $PSScriptRoot 'vst\webhost\build-web.ps1')
  }
  $root = $web; $start = ''
}
$root = (Resolve-Path $root).Path
$mime = @{
  '.html' = 'text/html; charset=utf-8'; '.js' = 'text/javascript; charset=utf-8'
  '.css'  = 'text/css; charset=utf-8';  '.svg' = 'image/svg+xml'; '.json' = 'application/json'
  '.png'  = 'image/png'; '.ico' = 'image/x-icon'
}

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
Write-Host "Serving $root at http://localhost:$Port/$start   (Ctrl+C to stop)"
if ($Ui) { Write-Host '(UI preview only: no audio engine)' }
if (-not $NoBrowser) { Start-Process "http://localhost:$Port/$start" }

try {
  while ($listener.IsListening) {
    $task = $listener.GetContextAsync()
    while (-not $task.AsyncWaitHandle.WaitOne(200)) { }   # keeps Ctrl+C responsive
    $ctx = $task.Result
    $rel = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath).TrimStart('/')
    if ([string]::IsNullOrEmpty($rel)) { $rel = 'index.html' }
    $file = [IO.Path]::GetFullPath((Join-Path $root $rel))
    $res = $ctx.Response
    if ($file.StartsWith($root) -and (Test-Path $file -PathType Leaf)) {
      $bytes = [IO.File]::ReadAllBytes($file)
      $ext = [IO.Path]::GetExtension($file).ToLower()
      $res.ContentType = if ($mime.ContainsKey($ext)) { $mime[$ext] } else { 'application/octet-stream' }
      $res.Headers.Add('Cache-Control', 'no-store')
      $res.OutputStream.Write($bytes, 0, $bytes.Length)
    } else {
      $res.StatusCode = 404
    }
    $res.Close()
  }
} finally {
  $listener.Stop()
}
