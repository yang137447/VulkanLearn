param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$ResourceRoot,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$ResourceRoot = (Resolve-Path -LiteralPath $ResourceRoot).Path
$StageRoot = Join-Path $ResourceRoot 'generated\bistro_godot_source'
if ((Test-Path -LiteralPath $StageRoot) -and -not $Force) { throw "Stage already exists: $StageRoot (use -Force to replace it)" }
if (Test-Path -LiteralPath $StageRoot) {
    $resolvedStage = (Resolve-Path -LiteralPath $StageRoot).Path
    $resolvedResource = (Resolve-Path -LiteralPath $ResourceRoot).Path
    if (-not $resolvedStage.StartsWith($resolvedResource, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to remove a stage outside ResourceRoot: $resolvedStage" }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
$copied = [System.Collections.Generic.List[string]]::new()
function Copy-StageFile {
    param([string]$RelativePath)
    $source = Join-Path $SourceRoot $RelativePath
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Staged source file does not exist: $RelativePath" }
    $destination = Join-Path $StageRoot $RelativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $copied.Add($RelativePath)
}
$renderGlbs = Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'Meshes') -Recurse -Filter '*.glb' | Where-Object { $_.FullName -notmatch '\\CollisionOcclusion\\' }
foreach ($file in $renderGlbs) {
    $relative = $file.FullName.Substring((Join-Path $SourceRoot 'Meshes\').Length)
    Copy-StageFile (Join-Path 'Meshes' $relative)
}
$renderTscn = Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'Scenes') -Recurse -Filter '*.tscn' | Where-Object {
    $text = Get-Content -LiteralPath $_.FullName -Raw
    $text -match 'res://Meshes/(?!CollisionOcclusion/).+\.(glb|res)'
}
foreach ($file in $renderTscn) {
    $relative = $file.FullName.Substring((Join-Path $SourceRoot 'Scenes\').Length)
    Copy-StageFile (Join-Path 'Scenes' $relative)
}
foreach ($relative in @('MainScene.tscn', 'ATTRIBUTION', 'LICENSE-ASSETS', 'README.md')) { Copy-StageFile $relative }
Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'Scenes\FillOut') -Filter '*.tscn' -File | ForEach-Object { Copy-StageFile (Join-Path 'Scenes\FillOut' $_.Name) }
$materialNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($relative in $copied | Where-Object { $_ -like 'Scenes\*.tscn' }) {
    $text = Get-Content -LiteralPath (Join-Path $StageRoot $relative) -Raw
    foreach ($match in [regex]::Matches($text, 'path="res://Materials/([^"]+\.tres)"')) { $materialNames.Add($match.Groups[1].Value.Replace('/', '\')) | Out-Null }
}
$textureNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($relative in $materialNames) {
    Copy-StageFile (Join-Path 'Materials' $relative)
    $text = Get-Content -LiteralPath (Join-Path $StageRoot (Join-Path 'Materials' $relative)) -Raw
    foreach ($match in [regex]::Matches($text, 'path="res://Textures/([^"]+)"')) { $textureNames.Add($match.Groups[1].Value.Replace('/', '\')) | Out-Null }
}
foreach ($relative in $textureNames) { Copy-StageFile (Join-Path 'Textures' $relative) }
$manifest = [ordered]@{
    name = 'Amazon Lumberyard Bistro Godot Modular Source Stage'
    sourceRoot = $SourceRoot
    generatedAt = (Get-Date).ToString('o')
    renderGlbCount = @($renderGlbs).Count
    renderSceneCount = @($renderTscn).Count
    materialCount = $materialNames.Count
    textureCount = $textureNames.Count
    files = @($copied | Sort-Object -Unique)
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $StageRoot 'manifest.json') -Encoding utf8
Write-Output ($manifest | ConvertTo-Json -Depth 3)
