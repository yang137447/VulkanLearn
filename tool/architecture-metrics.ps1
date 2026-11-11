<#
.SYNOPSIS
    Emits deterministic JSON architecture metrics for the VulkanLearn repository.

.DESCRIPTION
    Implements the P0 baseline of documents/plan/architecture/ue-runtime-editor-split-plan.md.
    Every tracked-file metric is derived from `git ls-files`, so untracked and build output
    never pollute the numbers. Filesystem enumeration is used only for artifacts that are
    intentionally gitignored (shader/spv) or for an explicit packaged-game directory.

    Counting method (also embedded in the emitted `method` object):
      - text files are decoded as UTF-8 (BOM tolerated, invalid bytes replaced);
      - CRLF and lone CR are normalized to LF before counting;
      - `lines` is the number of LF characters, plus one when the file is non-empty and
        does not end with a newline (final unterminated line is still a line);
      - `source` file set is every tracked `source/**/*.h`, `*.hpp`, `*.cpp`;
      - `source/editor/**` is the editor subtree of that set;
      - runtime `.cpp` units are tracked `source/**/*.cpp` outside `source/editor/**`;
      - include edges are `#include "<path>"` directives in tracked source text files
        outside `source/editor/**` whose quoted path contains `editor/`;
      - build targets come from `add_library` / `add_executable` in the top-level
        `CMakeLists.txt` and `source/CMakeLists.txt` only (best effort, deterministic order).

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's directory.

.PARAMETER OutputPath
    JSON output path relative to RepoRoot (or absolute). Defaults to
    `artifacts/architecture-metrics.json`, which is gitignored. Pass an empty string to
    print to stdout only.

.PARAMETER PackagePath
    Optional packaged game directory. When supplied, reports recursive file count and bytes;
    otherwise `packagedGame` is null. A supplied but missing path is an error.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tool\architecture-metrics.ps1

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tool\architecture-metrics.ps1 `
        -PackagePath artifacts\game-package
#>
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$OutputPath = "artifacts/architecture-metrics.json",
    [string]$PackagePath = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

function Write-Utf8File {
    param(
        [string]$Path,
        [string]$Text
    )

    $parent = Split-Path -Parent $Path
    if ($parent -and !(Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Get-TrackedFiles {
    param([string]$Root)

    $lines = & git -C $Root -c core.quotepath=false ls-files
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed in '$Root'."
    }

    $result = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $result.Add(($line -replace '\\', '/'))
    }
    return $result.ToArray()
}

function Test-UnderPath {
    param(
        [string]$Path,
        [string]$Prefix
    )

    return $Path.StartsWith($Prefix, [System.StringComparison]::Ordinal)
}

function Get-TextMetrics {
    param(
        [string]$AbsolutePath,
        [System.Collections.Generic.Dictionary[string, object]]$Cache
    )

    if ($Cache.ContainsKey($AbsolutePath)) {
        return $Cache[$AbsolutePath]
    }

    if (!(Test-Path -LiteralPath $AbsolutePath)) {
        return $null
    }

    $encoding = [System.Text.UTF8Encoding]::new($false, $false)
    $text = [System.IO.File]::ReadAllText($AbsolutePath, $encoding)
    $normalized = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    $newlineCount = 0
    foreach ($character in $normalized.ToCharArray()) {
        if ($character -eq "`n") {
            $newlineCount++
        }
    }
    if ($normalized.Length -gt 0 -and !$normalized.EndsWith("`n", [System.StringComparison]::Ordinal)) {
        $newlineCount++
    }

    $metrics = [pscustomobject]@{
        lines = [long]$newlineCount
        bytes = [long][System.IO.FileInfo]::new($AbsolutePath).Length
    }
    $Cache[$AbsolutePath] = $metrics
    return $metrics
}

function Measure-FileSet {
    param(
        [string]$Root,
        [string[]]$RelativePaths,
        [string[]]$Extensions,
        [switch]$BytesOnly,
        [System.Collections.Generic.Dictionary[string, object]]$Cache,
        [System.Collections.Generic.List[string]]$Missing
    )

    $files = 0
    $lines = [long]0
    $bytes = [long]0
    foreach ($relative in $RelativePaths) {
        $extension = [System.IO.Path]::GetExtension($relative)
        if ($Extensions -and ($Extensions -notcontains $extension)) {
            continue
        }

        $absolute = Join-Path $Root $relative
        if ($BytesOnly) {
            if (!(Test-Path -LiteralPath $absolute)) {
                $Missing.Add($relative) | Out-Null
                continue
            }
            $files++
            $bytes += [long](Get-Item -LiteralPath $absolute).Length
            continue
        }

        $metrics = Get-TextMetrics -AbsolutePath $absolute -Cache $Cache
        if ($null -eq $metrics) {
            $Missing.Add($relative) | Out-Null
            continue
        }

        $files++
        $lines += $metrics.lines
        $bytes += $metrics.bytes
    }

    return [pscustomobject]@{
        files = [int]$files
        lines = [long]$lines
        bytes = [long]$bytes
    }
}

function Get-BuildTargets {
    param(
        [string]$Root,
        [string[]]$CMakeFiles
    )

    $targets = [System.Collections.Generic.List[object]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($relative in $CMakeFiles) {
        $absolute = Join-Path $Root $relative
        if (!(Test-Path -LiteralPath $absolute)) {
            continue
        }

        $text = [System.IO.File]::ReadAllText(
            $absolute,
            [System.Text.UTF8Encoding]::new($false, $false)
        )
        $regex = [regex]'(?m)^\s*add_(library|executable)\s*\(\s*([A-Za-z0-9_.\+\-]+)'
        foreach ($match in $regex.Matches($text)) {
            $kind = if ($match.Groups[1].Value -eq "library") { "library" } else { "executable" }
            $name = $match.Groups[2].Value
            $key = "$relative|$kind|$name"
            if ($seen.Add($key)) {
                $targets.Add([pscustomobject]@{
                    name = $name
                    type = $kind
                    file = $relative
                }) | Out-Null
            }
        }
    }

    return @($targets | Sort-Object -Property name, type, file -CaseSensitive)
}

function Get-IncludeEdges {
    param(
        [string]$Root,
        [string[]]$Candidates,
        [System.Collections.Generic.List[string]]$Missing
    )

    $total = 0
    $renderEditor = 0
    $uiWidgetsEditor = 0
    $bareEditor = 0
    $files = [System.Collections.Generic.List[object]]::new()
    $regex = [regex]'(?m)^\s*#\s*include\s*"([^"]+)"'

    foreach ($relative in $Candidates) {
        $absolute = Join-Path $Root $relative
        if (!(Test-Path -LiteralPath $absolute)) {
            $Missing.Add($relative) | Out-Null
            continue
        }

        $encoding = [System.Text.UTF8Encoding]::new($false, $false)
        $text = [System.IO.File]::ReadAllText($absolute, $encoding)
        $count = 0
        foreach ($match in $regex.Matches($text)) {
            $included = $match.Groups[1].Value -replace '\\', '/'
            if (!$included.Contains("editor/")) {
                continue
            }
            $count++
            if ($included.Contains("render/editor/")) {
                $renderEditor++
            }
            if ($included.Contains("ui/widgets/editor/")) {
                $uiWidgetsEditor++
            }
            if ($included.StartsWith("editor/", [System.StringComparison]::Ordinal)) {
                $bareEditor++
            }
        }

        if ($count -gt 0) {
            $files.Add([pscustomobject]@{
                path = $relative
                count = $count
            }) | Out-Null
        }
    }

    foreach ($file in $files) {
        $total += $file.count
    }

    return [pscustomobject]@{
        total = $total
        renderEditor = $renderEditor
        uiWidgetsEditor = $uiWidgetsEditor
        bareEditor = $bareEditor
        files = @($files | Sort-Object -Property path -CaseSensitive)
    }
}

function Measure-Directory {
    param(
        [string]$AbsolutePath,
        [string[]]$Include,
        [string]$Filter
    )

    $result = [pscustomobject]@{
        files = 0
        bytes = [long]0
    }
    if (!(Test-Path -LiteralPath $AbsolutePath)) {
        return $result
    }

    $items = Get-ChildItem -LiteralPath $AbsolutePath -Recurse -File -Include $Include -Filter $Filter -ErrorAction Stop
    $files = 0
    $bytes = [long]0
    foreach ($item in $items) {
        $files++
        $bytes += $item.Length
    }
    return [pscustomobject]@{
        files = [int]$files
        bytes = [long]$bytes
    }
}

$tracked = Get-TrackedFiles -Root $RepoRoot
$missingTracked = [System.Collections.Generic.List[string]]::new()
$textCache = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)

$sourceAll = @(
    $tracked | Where-Object {
        (Test-UnderPath $_ "source/") -and
        (@(".h", ".hpp", ".cpp") -contains [System.IO.Path]::GetExtension($_))
    }
)
$sourceEditor = @($sourceAll | Where-Object { Test-UnderPath $_ "source/editor/" })
$sourceRuntime = @($sourceAll | Where-Object { !(Test-UnderPath $_ "source/editor/") })

$sourceMetrics = Measure-FileSet `
    -Root $RepoRoot `
    -RelativePaths $sourceAll `
    -Extensions @(".h", ".hpp", ".cpp") `
    -Cache $textCache `
    -Missing $missingTracked
$editorMetrics = Measure-FileSet `
    -Root $RepoRoot `
    -RelativePaths $sourceEditor `
    -Extensions @(".h", ".hpp", ".cpp") `
    -Cache $textCache `
    -Missing $missingTracked
$runtimeMetrics = Measure-FileSet `
    -Root $RepoRoot `
    -RelativePaths $sourceRuntime `
    -Extensions @(".h", ".hpp", ".cpp") `
    -Cache $textCache `
    -Missing $missingTracked

$runtimeCppUnits = @($sourceRuntime | Where-Object { [System.IO.Path]::GetExtension($_) -eq ".cpp" }).Count
$editorCppUnits = @($sourceEditor | Where-Object { [System.IO.Path]::GetExtension($_) -eq ".cpp" }).Count
$widgetsCppUnits = @(
    $tracked | Where-Object {
        (Test-UnderPath $_ "source/ui/widgets/") -and
        ([System.IO.Path]::GetExtension($_) -eq ".cpp")
    }
).Count

$uiFiles = @($tracked | Where-Object { Test-UnderPath $_ "ui/" })
$uiMetrics = Measure-FileSet `
    -Root $RepoRoot `
    -RelativePaths $uiFiles `
    -Extensions @() `
    -BytesOnly `
    -Cache $textCache `
    -Missing $missingTracked

$glslFiles = @($tracked | Where-Object { Test-UnderPath $_ "shader/glsl/" })
$glslMetrics = Measure-FileSet `
    -Root $RepoRoot `
    -RelativePaths $glslFiles `
    -Extensions @() `
    -Cache $textCache `
    -Missing $missingTracked

$spv = Measure-Directory -AbsolutePath (Join-Path $RepoRoot "shader/spv") -Include @("*.spv") -Filter "*"
$cachePath = Join-Path $RepoRoot "shader/spv/shader-build-cache.json"
$cachePresent = Test-Path -LiteralPath $cachePath
$cacheBytes = if ($cachePresent) { [long](Get-Item -LiteralPath $cachePath).Length } else { $null }

$includeCandidates = @(
    $tracked | Where-Object {
        (Test-UnderPath $_ "source/") -and
        !(Test-UnderPath $_ "source/editor/") -and
        (@(".h", ".hpp", ".cpp") -contains [System.IO.Path]::GetExtension($_))
    }
)
$includeEdges = Get-IncludeEdges `
    -Root $RepoRoot `
    -Candidates $includeCandidates `
    -Missing $missingTracked

$packagedGame = $null
if (![string]::IsNullOrWhiteSpace($PackagePath)) {
    $packageAbsolute = if ([System.IO.Path]::IsPathRooted($PackagePath)) {
        $PackagePath
    } else {
        Join-Path $RepoRoot $PackagePath
    }
    if (!(Test-Path -LiteralPath $packageAbsolute)) {
        throw "Package path does not exist: '$packageAbsolute'."
    }

    $packageAbsolute = (Resolve-Path -LiteralPath $packageAbsolute).Path
    $packageMetrics = Measure-Directory -AbsolutePath $packageAbsolute -Include @("*") -Filter "*"
    $relativePackage = if ($packageAbsolute.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        $packageAbsolute.Substring($RepoRoot.Length).TrimStart('\', '/') -replace '\\', '/'
    } else {
        $packageAbsolute -replace '\\', '/'
    }
    $packagedGame = [pscustomobject]@{
        path = $relativePackage
        files = $packageMetrics.files
        bytes = $packageMetrics.bytes
    }
}

$cmakeTargets = Get-BuildTargets `
    -Root $RepoRoot `
    -CMakeFiles @("CMakeLists.txt", "source/CMakeLists.txt")

$gitCommit = (& git -C $RepoRoot rev-parse HEAD 2>$null)
if ($LASTEXITCODE -ne 0) {
    $gitCommit = $null
}

$report = [ordered]@{
    schemaVersion = 1
    repository = [ordered]@{
        root = ($RepoRoot -replace '\\', '/')
        gitCommit = if ($gitCommit) { $gitCommit.Trim() } else { $null }
    }
    method = [ordered]@{
        fileList = "git -C <root> -c core.quotepath=false ls-files (tracked files only)"
        textEncoding = "UTF-8 with BOM tolerated; invalid byte sequences replaced, never thrown"
        newlineCounting = "normalize CRLF and lone CR to LF, count LF; add one when non-empty and the file does not end with a newline"
        sourceExtensions = @(".h", ".hpp", ".cpp")
        sourceScope = "tracked files under source/"
        editorSubtree = "source/editor/"
        runtimeCppUnits = "tracked source/**/*.cpp outside source/editor/**"
        editorCppUnits = "tracked source/editor/**/*.cpp"
        widgetCppUnits = "tracked source/ui/widgets/**/*.cpp"
        assetBytes = "sum of tracked file sizes; ui/ and shader/glsl/ include every file type"
        buildTargets = "add_library/add_executable parsed from CMakeLists.txt and source/CMakeLists.txt only"
        shaderArtifacts = "filesystem enumeration; shader/spv is gitignored build output"
        packagedGame = "filesystem recursion of -PackagePath; null when not supplied"
        includeEdges = "quoted #include path containing 'editor/' in tracked source text files outside source/editor/"
        exclusions = "extern/, build*/, artifacts/ are never counted; untracked files are invisible via git ls-files"
        ordering = "all arrays sorted ascending by path or name with PowerShell case-sensitive comparison (ASCII keys)"
    }
    source = [ordered]@{
        files = $sourceMetrics.files
        lines = $sourceMetrics.lines
    }
    editor = [ordered]@{
        files = $editorMetrics.files
        lines = $editorMetrics.lines
        cppUnits = [int]$editorCppUnits
    }
    runtime = [ordered]@{
        files = $runtimeMetrics.files
        lines = $runtimeMetrics.lines
        cppUnits = [int]$runtimeCppUnits
    }
    uiWidgets = [ordered]@{
        cppUnits = [int]$widgetsCppUnits
    }
    uiAssets = [ordered]@{
        files = $uiMetrics.files
        bytes = $uiMetrics.bytes
    }
    shaderGlsl = [ordered]@{
        files = $glslMetrics.files
        bytes = $glslMetrics.bytes
    }
    shaderArtifacts = [ordered]@{
        spvFiles = $spv.files
        spvBytes = $spv.bytes
        buildCache = [ordered]@{
            present = [bool]$cachePresent
            bytes = $cacheBytes
        }
    }
    buildTargets = @($cmakeTargets)
    includeEdges = [ordered]@{
        total = [int]$includeEdges.total
        renderEditor = [int]$includeEdges.renderEditor
        uiWidgetsEditor = [int]$includeEdges.uiWidgetsEditor
        bareEditor = [int]$includeEdges.bareEditor
        files = @($includeEdges.files)
    }
    packagedGame = $packagedGame
    missingTrackedFiles = @($missingTracked | Sort-Object -CaseSensitive -Unique)
}

$json = $report | ConvertTo-Json -Depth 10

if (![string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputAbsolute = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
        $OutputPath
    } else {
        Join-Path $RepoRoot $OutputPath
    }
    Write-Utf8File -Path $outputAbsolute -Text ($json + "`n")
}

Write-Output $json
