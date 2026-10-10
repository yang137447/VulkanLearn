[CmdletBinding()]
param(
    [ValidateSet('beauty', 'linear', 'specular', 'basecolor', 'normal', 'direct')]
    [string]$View = 'beauty',
    [string]$Scene = 'Maps/SC_lookdev/SC_lookdev.json',
    [int]$TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$config = Get-Content (Join-Path $root 'config/config.json') -Raw | ConvertFrom-Json
$output = Join-Path $config.resourcePath 'Generated/Lookdev'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$name = "lookdev-$View-$stamp"
$image = Join-Path $config.resourcePath "Generated/Screenshots/$name.bmp"
$toneMap = if ($View -eq 'linear') { 0 } else { 3 }
$scene = $Scene
if ($View -eq 'direct') { $scene = 'Generated/Lookdev/SC_lookdev_direct.json' }
$arguments = "--initial-scene $scene --no-dev-ui --console-command `"tonemap $toneMap`" --console-command `"bloom strength 0`" --console-command `"screenshot $name.bmp`" --console-command-delay 240"
if ($View -eq 'specular') { $arguments += ' --console-command "debugview 10"' }
if ($View -eq 'basecolor') { $arguments += ' --console-command "debugview 1"' }
if ($View -eq 'normal') { $arguments += ' --console-command "debugview 3"' }
$process = $null
try {
    $process = Start-Process -FilePath (Join-Path $root 'build/bin/main.exe') `
        -WorkingDirectory $root -ArgumentList $arguments -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $output "$name.log") `
        -RedirectStandardError (Join-Path $output "$name.err.log") -PassThru
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) { throw "Renderer exited before capture; see $output/$name.log" }
        $ready = (Test-Path -LiteralPath $image) -and
            ((Get-Content (Join-Path $output "$name.log") -Tail 12) -match 'Screenshot saved:')
    } until ($ready -or (Get-Date) -ge $deadline)
    if (-not $ready) { throw "Timed out waiting for screenshot; see $output/$name.log" }
    $log = Get-Content (Join-Path $output "$name.log") -Raw
    $errors = Get-Content (Join-Path $output "$name.err.log") -Raw
    if (($log + $errors) -match 'Validation Error:|\[Diagnostics\]\[Error\]') {
        throw "Renderer reported an error during capture; see $output/$name.log"
    }
    Write-Output $image
}
finally {
    # The capture process belongs to this script, not an existing user's renderer.
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
}
