[CmdletBinding()]
param(
    [string]$Screenshot = '',
    [switch]$NoDeveloperUi
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$config = Get-Content (Join-Path $root 'config/config.json') -Raw | ConvertFrom-Json
$scene = Join-Path $config.resourcePath 'Maps/SC_lookdev/SC_lookdev.json'
$hdr = Join-Path $config.resourcePath 'Generated/Lookdev/studio_neutral.hdr'
if (-not (Test-Path -LiteralPath $scene) -or -not (Test-Path -LiteralPath $hdr)) {
    throw 'Build lookdev assets first: python tool/lookdev/build_scene.py'
}
$arguments = @(
    '--initial-scene', 'Maps/SC_lookdev/SC_lookdev.json',
    '--console-command', 'tonemap 3',
    '--console-command', 'bloom strength 0',
    '--console-command-delay', '180'
)
if ($NoDeveloperUi) { $arguments += '--no-dev-ui' }
else { $arguments += '--dev-ui' }
if ($Screenshot) { $arguments += @('--console-command', "screenshot $Screenshot") }

# Only this process receives the scene/postprocess preset; shared config/MI stay unchanged.
Push-Location $root
try {
    & (Join-Path $root 'build/bin/main.exe') @arguments
    if ($LASTEXITCODE -ne 0) { throw "Lookdev renderer exited with code $LASTEXITCODE" }
}
finally {
    Pop-Location
}
