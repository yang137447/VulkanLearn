[CmdletBinding()]
param(
    [string]$PackagePath = "",
    [string]$ProjectRoot = "",
    [string]$VerifyTool = "",
    [int]$SmokeFrames = 120
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    throw "validate-game-package: $Message"
}

function Read-Json([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "missing JSON file: $Path"
    }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Get-UiReferences([string]$Path) {
    $text = Get-Content -LiteralPath $Path -Raw
    $references = New-Object System.Collections.Generic.List[string]
    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    if ($extension -eq ".rml") {
        foreach ($match in [regex]::Matches($text, '(?:href|src)\s*=\s*"([^"]+)"')) {
            $references.Add($match.Groups[1].Value)
        }
    }
    elseif ($extension -eq ".rcss") {
        $urlPattern = [regex]@'
url\(\s*['"]?([^'")\s]+)['"]?\s*\)
'@
        $importPattern = [regex]@'
@import\s+(?:url\(\s*)?['"]([^'"]+)['"]
'@
        foreach ($match in $urlPattern.Matches($text)) { $references.Add($match.Groups[1].Value) }
        foreach ($match in $importPattern.Matches($text)) { $references.Add($match.Groups[1].Value) }
    }
    return $references
}

function Assert-UiManifestClosure([string]$UiRoot, [string]$ManifestName, [bool]$RuntimeManifest) {
    $manifestPath = Join-Path $UiRoot $ManifestName
    $manifest = Read-Json $manifestPath
    if ($manifest.format -ne 1 -or $null -eq $manifest.files) {
        Fail "$ManifestName must be format 1 with a files array"
    }
    $listed = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $manifest.files) {
        if ($file -match '(^[\\/])|(^[A-Za-z]:)|(\.\.)') {
            Fail "$ManifestName contains an unsafe path: $file"
        }
        [void]$listed.Add(($file -replace '\\', '/'))
    }
    if ($RuntimeManifest -and -not $listed.Contains("localization.json")) {
        Fail "runtime UI manifest must list localization.json"
    }
    foreach ($file in $manifest.files) {
        $relative = $file -replace '\\', '/'
        $absolute = Join-Path $UiRoot $relative
        if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) {
            Fail "$ManifestName lists a missing file: $relative"
        }
        if ($RuntimeManifest -and ($relative.StartsWith("editor-") -or $relative.StartsWith("widgets/"))) {
            Fail "runtime UI manifest contains editor-only content: $relative"
        }
        foreach ($reference in Get-UiReferences $absolute) {
            if ([string]::IsNullOrEmpty($reference) -or $reference.StartsWith("#") -or
                $reference -match '^(data|https?|vl)://') {
                continue
            }
            if ($reference -match '^(/|\\)|^[A-Za-z]:') {
                Fail "UI reference must be relative: $relative -> $reference"
            }
            $target = [System.IO.Path]::GetFullPath((Join-Path (Split-Path $absolute -Parent) $reference))
            $uiFull = [System.IO.Path]::GetFullPath($UiRoot).TrimEnd('\', '/')
            if (-not $target.StartsWith($uiFull + [System.IO.Path]::DirectorySeparatorChar,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                Fail "UI reference escapes the asset root: $relative -> $reference"
            }
            $targetRelative = $target.Substring($uiFull.Length + 1).Replace('\', '/')
            if (-not $listed.Contains($targetRelative)) {
                Fail "UI manifest closure is incomplete: $relative references $reference ($targetRelative)"
            }
        }
    }
}

function Assert-VerifyTool([string]$Path) {
    if (-not [string]::IsNullOrEmpty($Path) -and (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    $candidate = Join-Path $PSScriptRoot "..\build\bin\vl_export_game.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }
    return ""
}

if ([string]::IsNullOrEmpty($ProjectRoot)) {
    $ProjectRoot = Split-Path $PSScriptRoot -Parent
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)

if ([string]::IsNullOrEmpty($PackagePath)) {
    $PackagePath = $env:VL_GAME_PACKAGE
}

if ([string]::IsNullOrEmpty($PackagePath)) {
    $uiRoot = Join-Path $ProjectRoot "ui"
    Assert-UiManifestClosure $uiRoot "runtime-ui.manifest.json" $true
    Assert-UiManifestClosure $uiRoot "editor-ui.manifest.json" $false
    Write-Host "validate-game-package: manifest closure checks passed (no package path supplied)"
    exit 0
}

$PackagePath = [System.IO.Path]::GetFullPath($PackagePath)
if (-not (Test-Path -LiteralPath $PackagePath -PathType Container)) {
    Fail "package directory does not exist: $PackagePath"
}

$exportManifest = Read-Json (Join-Path $PackagePath "game-export-manifest.json")
if ($exportManifest.flavor -ne "game" -or $exportManifest.uiFallback -eq $true) {
    Fail "package is not a game-flavor export (flavor=$($exportManifest.flavor), uiFallback=$($exportManifest.uiFallback))"
}
foreach ($file in $exportManifest.files) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackagePath $file.path) -PathType Leaf)) {
        Fail "game-export-manifest.json lists a missing file: $($file.path)"
    }
}

$marker = Read-Json (Join-Path $PackagePath "vl_build_flavor.json")
if ($marker.editorEnabled -ne $false -or $marker.flavor -ne "game") {
    Fail "vl_build_flavor.json is not a game/editorEnabled=false marker"
}

$graph = Read-Json (Join-Path $PackagePath "config/renderGraphConfig.json")
$selectionMask = $false
foreach ($resource in $graph.resources) {
    if ($resource.name -eq "selectionMask") { $selectionMask = $true }
}
if (-not $selectionMask) { Fail "renderGraphConfig.json lost selectionMask" }
foreach ($pass in $graph.passes) {
    if ($pass.name -ieq "editorGrid" -or $pass.type -ieq "editorGrid") {
        Fail "renderGraphConfig.json still contains editorGrid"
    }
}

foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $PackagePath "ui") -Recurse -File) {
    $relative = $entry.FullName.Substring($PackagePath.Length + 1).Replace('\', '/').ToLowerInvariant()
    if ($relative.StartsWith("ui/editor-") -or $relative.StartsWith("ui/widgets/") -or
        $relative.StartsWith("ui/icons/")) {
        Fail "editor UI content leaked into package: $relative"
    }
}
foreach ($forbidden in @(
    "shader/glsl/pass/editorGrid.vert",
    "shader/glsl/pass/editorGrid.frag",
    "shader/glsl/pass/M_editorGrid.json",
    "resources/Common/Materials/Pass/MI_editorGrid.json")) {
    if (Test-Path -LiteralPath (Join-Path $PackagePath $forbidden)) {
        Fail "editor shader content leaked into package: $forbidden"
    }
}

$shaderManifest = Read-Json (Join-Path $PackagePath "shader/runtime-shader.manifest.json")
$allowedSpv = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
foreach ($artifact in $shaderManifest.artifacts.PSObject.Properties.Value) {
    foreach ($output in $artifact.outputs.PSObject.Properties.Value) {
        $relative = "shader/" + ($output.path -replace '\\', '/')
        if (-not (Test-Path -LiteralPath (Join-Path $PackagePath $relative) -PathType Leaf)) {
            Fail "retained shader output is missing: $relative"
        }
        [void]$allowedSpv.Add($relative)
    }
}
foreach ($material in $shaderManifest.materials) {
    $relative = "shader/glsl/" + ($material.path -replace '\\', '/')
    if (-not (Test-Path -LiteralPath (Join-Path $PackagePath $relative) -PathType Leaf)) {
        Fail "retained M_ asset is missing: $relative"
    }
}
foreach ($generated in $shaderManifest.generatedAtStartup) {
    $relative = "shader/glsl/" + ($generated.path -replace '\\', '/')
    if (Test-Path -LiteralPath (Join-Path $PackagePath $relative)) {
        Fail "generated shader include was packaged: $relative"
    }
}
$cachePath = "shader/" + ($shaderManifest.cache.path -replace '\\', '/')
if (-not (Test-Path -LiteralPath (Join-Path $PackagePath $cachePath) -PathType Leaf)) {
    Fail "packaged shader cache is missing: $cachePath"
}
[void]$allowedSpv.Add($cachePath)
foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $PackagePath "shader/spv") -Recurse -File) {
    $relative = $entry.FullName.Substring($PackagePath.Length + 1).Replace('\', '/')
    if (-not $allowedSpv.Contains($relative)) {
        Fail "shader/spv contains a file outside runtime-shader.manifest.json: $relative"
    }
}

foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $PackagePath "resources") -Recurse -File -Filter "*.json") {
    $value = Read-Json $entry.FullName
    if ($null -eq $value.material -or $value.material -isnot [string]) { continue }
    $parts = ($value.material -replace '\\', '/') -split '/'
    $glslIndex = [Array]::IndexOf($parts, "glsl")
    if ($glslIndex -lt 0 -or $glslIndex -ge ($parts.Length - 1)) {
        Fail "material instance has a non-GLSL material path: $($entry.FullName)"
    }
    $materialRelative = ($parts[($glslIndex + 1)..($parts.Length - 1)] -join '/')
    if (-not (Test-Path -LiteralPath (Join-Path $PackagePath "shader/glsl/$materialRelative") -PathType Leaf)) {
        Fail "shipped material instance references a missing material: $materialRelative"
    }
}

$resolvedVerifyTool = Assert-VerifyTool $VerifyTool
if ([string]::IsNullOrEmpty($resolvedVerifyTool)) {
    Fail "vl_export_game.exe --verify-package is required for digest verification; pass -VerifyTool"
}
$verifyOutput = & $resolvedVerifyTool --verify-package $PackagePath 2>&1
if ($LASTEXITCODE -ne 0) {
    Fail "vl_export_game --verify-package failed: $($verifyOutput -join ' ')"
}

$gameExecutable = Join-Path $PackagePath "main_game.exe"
if (-not (Test-Path -LiteralPath $gameExecutable -PathType Leaf)) {
    Fail "game package must contain main_game.exe"
}
Push-Location $PackagePath
try {
    $smokeOutput = & $gameExecutable --exit-after-frames $SmokeFrames --no-dev-ui 2>&1
    $smokeExit = $LASTEXITCODE
}
finally {
    Pop-Location
}
$smokeText = $smokeOutput -join "`n"
if ($smokeExit -ne 0) {
    Fail "packaged game smoke exited with $smokeExit`n$smokeText"
}
if ($smokeText -match "Shader cache miss:") {
    Fail "packaged game reported a shader cache miss`n$smokeText"
}
if ($smokeText -match "Validation Error|VUID|THREADING") {
    Fail "packaged game reported Vulkan validation/threading errors`n$smokeText"
}
$missMatch = [regex]::Match($smokeText, "misses=(\d+)")
if (-not $missMatch.Success -or [int]$missMatch.Groups[1].Value -ne 0) {
    Fail "packaged game did not report misses=0`n$smokeText"
}

Write-Host "validate-game-package: package '$PackagePath' passed all checks"
exit 0
