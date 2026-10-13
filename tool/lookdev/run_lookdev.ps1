[CmdletBinding()]
param(
    [string]$Screenshot = '',
    [switch]$NoDeveloperUi
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$config = Get-Content (Join-Path $root 'config/config.json') -Raw | ConvertFrom-Json
$scene = Join-Path $config.resourcePath 'Maps/SC_lookdev_article/SC_lookdev_article.json'
$hdr = Join-Path $config.resourcePath 'Common/Environments/UE_AssetsPreview/EpicQuadPanorama_Gray.hdr'
if (-not (Test-Path -LiteralPath $scene) -or -not (Test-Path -LiteralPath $hdr)) {
    throw 'Import UE assets first: tool/lookdev/convert_assets_preview.ps1'
}
$arguments = @(
    '--initial-scene', 'Maps/SC_lookdev_article/SC_lookdev_article.json',
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
