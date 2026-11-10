[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build-vscode/development',
    [string]$Screenshot = '',
    [switch]$NoDeveloperUi,
    [switch]$Skinned,
    [switch]$BindPose,
    [switch]$Blueprint,
    [switch]$Feet
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$previewBuildRoot = $BuildDirectory
if (-not [IO.Path]::IsPathRooted($previewBuildRoot)) {
    $previewBuildRoot = Join-Path $root $previewBuildRoot
}
$previewExecutable = Join-Path $previewBuildRoot 'bin/main.exe'
if (-not (Test-Path -LiteralPath $previewExecutable -PathType Leaf)) {
    throw "Missing preview executable: $previewExecutable. Build Development with cmake --preset development and cmake --build --preset development, or select -BuildDirectory explicitly."
}
$config = Get-Content (Join-Path $root 'config/config.json') -Raw | ConvertFrom-Json
$resourceRoot = $config.resourcePath
if (-not [IO.Path]::IsPathRooted($resourceRoot)) {
    $resourceRoot = Join-Path $root $resourceRoot
}
$resourceRoot = [IO.Path]::GetFullPath($resourceRoot)
$scenePath = 'Maps/SC_tressfx_female/SC_tressfx_female.json'
if ($BindPose -and $Blueprint) { throw 'BindPose and Blueprint select different review scenes.' }
if ($Feet -and $BindPose) { throw 'Feet uses the skinned Walk Blueprint; BindPose selects a different review scene.' }
if ($Feet) { $scenePath = 'Maps/SC_tressfx_female/SC_tressfx_female_feet.json' }
elseif ($BindPose) { $scenePath = 'Maps/SC_tressfx_female/SC_tressfx_female_bind.json' }
elseif ($Skinned -and $Blueprint) { $scenePath = 'Maps/SC_tressfx_female/SC_tressfx_female_skinned_blueprint.json' }
elseif ($Skinned) { $scenePath = 'Maps/SC_tressfx_female/SC_tressfx_female_skinned.json' }
elseif ($Blueprint) { $scenePath = 'Maps/SC_tressfx_female/SC_tressfx_female_blueprint.json' }
if (-not (Test-Path -LiteralPath (Join-Path $resourceRoot $scenePath))) {
    throw "Missing scene: $scenePath. Create the static package with import_female_scene.py or the skinned package with build_female_skinned_scene.py."
}
$arguments = @(
    '--initial-scene', $scenePath,
    '--frame-limit', '60',
    '--console-command', 'tonemap 3',
    '--console-command', 'bloom strength 0',
    '--console-command-delay', '180'
)
if ($NoDeveloperUi) { $arguments += '--no-dev-ui' }
else { $arguments += '--dev-ui' }
if ($Screenshot) { $arguments += @('--console-command', "screenshot $Screenshot") }

# 显示预设只作用于本进程；正式场景和共享 config 不因审阅/截图被改写。
Push-Location $root
try {
    Write-Host "Female preview executable: $previewExecutable"
    & $previewExecutable @arguments
    if ($LASTEXITCODE -ne 0) { throw "Female scene renderer exited with code $LASTEXITCODE" }
}
finally {
    Pop-Location
}
