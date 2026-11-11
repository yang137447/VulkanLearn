<#
.SYNOPSIS
    Builds the six heavy VulkanLearn dependencies into the machine-wide prefix.

.DESCRIPTION
    Configures ThirdParty/ as a superbuild, installs SDL3, Assimp, OpenEXR
    (with Imath/OpenJPH), RmlUi, FreeType and Jolt into
    <DepsRoot>/<toolchain-id>/<Config>, then writes and verifies deps-stamp.json.
    The engine consumes the prefix with VULKANLEARN_DEPS_MODE=prebuilt.
    Feature switches default to OFF so the prefix matches a game-only Release
    consumer; pass -BuildTests/-Editor to match a different engine feature set.

.PARAMETER Config
    Dependency/engine configuration. Must match the consuming build's CMAKE_BUILD_TYPE.

.PARAMETER DepsRoot
    Machine-wide root outside the repository. Defaults to
    $env:VULKANLEARN_DEPS_ROOT, D:\YYBWorkSpace\VulkanLearn-deps, or the user profile.

.PARAMETER Force
    Ignore the "prefix is up to date" shortcut and rebuild incrementally.

.PARAMETER Clean
    Delete the superbuild directory and the per-dependency build trees first.

.PARAMETER Jobs
    Parallel build jobs passed to every dependency.
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "RelWithDebInfo",
    [string]$DepsRoot = "",
    [switch]$Force,
    [switch]$Clean,
    [ValidateRange(1, 64)]
    [int]$Jobs = 4,
    [switch]$AnimationTools,
    [switch]$BuildTests,
    [switch]$CSharpBlueprints,
    [switch]$Editor,
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$manifestPath = Join-Path $repoRoot "ThirdParty\manifest.json"
$stampScript = Join-Path $PSScriptRoot "deps-stamp.ps1"
if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot "build-deps"
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
if (-not $BuildDir.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "build-deps: build directory must stay inside the repository: $BuildDir"
}

if (-not $DepsRoot) {
    if ($env:VULKANLEARN_DEPS_ROOT) {
        $DepsRoot = $env:VULKANLEARN_DEPS_ROOT
    }
    elseif (Test-Path -LiteralPath "D:\YYBWorkSpace") {
        $DepsRoot = "D:\YYBWorkSpace\VulkanLearn-deps"
    }
    else {
        $DepsRoot = Join-Path $env:USERPROFILE "VulkanLearn-deps"
    }
}
$DepsRoot = [System.IO.Path]::GetFullPath($DepsRoot)
if ($DepsRoot.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "build-deps: dependency root must be machine-wide and outside the repository: $DepsRoot"
}

function Get-CompilerText([string]$Compiler, [string[]]$Arguments) {
    try {
        return (& $Compiler @Arguments 2>&1 | Out-String)
    }
    catch {
        return ""
    }
}

function Get-ToolchainId {
    $cxx = (Get-Command g++ -ErrorAction Stop).Source
    $versionText = Get-CompilerText $cxx @("--version")
    if ($versionText -match '(\d+\.\d+\.\d+)') {
        $version = $Matches[1]
    }
    elseif ($versionText -match '(\d+\.\d+)') {
        $version = $Matches[1]
    }
    else {
        throw "build-deps: cannot determine the g++ version from '$cxx'."
    }

    $crt = "unknown"
    $binDir = Split-Path -Parent $cxx
    $runtime = Join-Path $binDir "libstdc++-6.dll"
    $objdump = Join-Path $binDir "objdump.exe"
    if (-not (Test-Path -LiteralPath $objdump)) {
        $objdumpCommand = Get-Command objdump -ErrorAction SilentlyContinue
        if ($objdumpCommand) { $objdump = $objdumpCommand.Source }
    }
    if ((Test-Path -LiteralPath $runtime) -and (Test-Path -LiteralPath $objdump)) {
        $imports = (& $objdump -p $runtime 2>$null | Out-String)
        if ($imports -match "api-ms-win-crt-") { $crt = "ucrt" }
        elseif ($imports -match "msvcrt\.dll") { $crt = "msvcrt" }
    }
    if ($crt -eq "unknown") {
        $verbose = Get-CompilerText $cxx @("-v")
        if ($verbose -match "--with-default-msvcrt=([A-Za-z0-9_]+)") {
            $crt = $Matches[1].ToLowerInvariant()
        }
        elseif ($verbose -match "ucrt") {
            $crt = "ucrt"
        }
    }
    return "win64-mingw-$version-$crt"
}

function Get-Dependency([string]$Name) {
    $dependency = $manifest.dependencies | Where-Object { $_.name -eq $Name } | Select-Object -First 1
    if (-not $dependency) {
        throw "build-deps: manifest has no dependency named '$Name'."
    }
    return $dependency
}

function Invoke-Checked([string]$Description, [scriptblock]$Action) {
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "build-deps: $Description failed with exit code $LASTEXITCODE."
    }
}

function Ensure-GitCheckout([string]$Name, [string]$Path, [string]$Url, [string]$Revision) {
    if (Test-Path -LiteralPath (Join-Path $Path ".git")) {
        Write-Host ("build-deps: source {0} already present at {1}" -f $Name, $Path)
        return
    }
    if (Test-Path -LiteralPath $Path) {
        $entries = @(Get-ChildItem -LiteralPath $Path -Force)
        if ($entries.Count -gt 0) {
            Write-Host ("build-deps: source {0} already present (not a git checkout) at {1}" -f $Name, $Path)
            return
        }
    }

    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force)
    if (Test-Path -LiteralPath $Path) {
        [System.IO.Directory]::Delete($Path, $true)
    }
    if ($Revision -match '^[0-9a-f]{40}$') {
        [void](New-Item -ItemType Directory -Path $Path -Force)
        Invoke-Checked "git init ($Name)" { git -C $Path init --quiet }
        Invoke-Checked "git remote ($Name)" { git -C $Path remote add origin $Url }
        Invoke-Checked "git fetch ($Name)" { git -C $Path fetch --depth 1 --quiet origin $Revision }
        Invoke-Checked "git checkout ($Name)" { git -C $Path checkout --quiet FETCH_HEAD }
    }
    else {
        Invoke-Checked "git clone ($Name)" {
            git clone --depth 1 --branch $Revision --quiet $Url $Path
        }
    }
}

function Ensure-TarSource([string]$Name, [string]$Path, [string]$Url, [string]$Sha256) {
    if (Test-Path -LiteralPath $Path) {
        $existing = @(Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0) {
            Write-Host ("build-deps: source {0} already present at {1}" -f $Name, $Path)
            return
        }
    }
    if (Test-Path -LiteralPath (Join-Path $Path "CMakeLists.txt")) {
        Write-Host ("build-deps: source {0} already present at {1}" -f $Name, $Path)
        return
    }

    $parent = Split-Path -Parent $Path
    [void](New-Item -ItemType Directory -Path $parent -Force)
    if (Test-Path -LiteralPath $Path) {
        [System.IO.Directory]::Delete($Path, $true)
    }

    $archive = Join-Path $parent ($Name + ".tar.gz")
    Write-Host ("build-deps: downloading {0}" -f $Url)
    Invoke-WebRequest -Uri $Url -OutFile $archive -UseBasicParsing
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($Sha256 -and $actual -ne $Sha256.ToLowerInvariant()) {
        [System.IO.File]::Delete($archive)
        throw "build-deps: $Name archive hash mismatch (expected $Sha256, got $actual)."
    }

    $extractRoot = Join-Path $parent ($Name + ".extract")
    if (Test-Path -LiteralPath $extractRoot) {
        [System.IO.Directory]::Delete($extractRoot, $true)
    }
    [void](New-Item -ItemType Directory -Path $extractRoot -Force)
    Invoke-Checked "tar extract ($Name)" { tar -xzf $archive -C $extractRoot }
    $children = @(Get-ChildItem -LiteralPath $extractRoot -Force)
    if ($children.Count -ne 1 -or -not $children[0].PSIsContainer) {
        throw "build-deps: unexpected archive layout for $Name in $extractRoot."
    }
    [System.IO.Directory]::Move($children[0].FullName, $Path)
    [System.IO.Directory]::Delete($extractRoot, $true)
    [System.IO.File]::Delete($archive)
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$toolchainId = Get-ToolchainId
$prefix = Join-Path $DepsRoot (Join-Path $toolchainId $Config)
$sourceRoot = Join-Path $DepsRoot "_sources"
$dependencyBuildRoot = Join-Path $DepsRoot ("_build\" + $toolchainId + "\" + $Config)

Write-Host ("build-deps: repo={0}" -f $repoRoot)
Write-Host ("build-deps: toolchain={0}" -f $toolchainId)
Write-Host ("build-deps: config={0}" -f $Config)
Write-Host ("build-deps: prefix={0}" -f $prefix)

if ($Clean) {
    foreach ($path in @($BuildDir, $dependencyBuildRoot)) {
        if (Test-Path -LiteralPath $path) {
            Write-Host ("build-deps: removing {0}" -f $path)
            [System.IO.Directory]::Delete($path, $true)
        }
    }
}

$stampPath = Join-Path $prefix "deps-stamp.json"
$configFile = Join-Path $prefix "share\vl_deps\vl_depsConfig.cmake"
if (-not $Force -and (Test-Path -LiteralPath $stampPath) -and
    (Test-Path -LiteralPath $configFile) -and
    (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt"))) {
    & $stampScript -BuildDir $BuildDir -Prefix $prefix -Config $Config -Check -NoTouch
    if ($LASTEXITCODE -eq 0) {
        Write-Host "build-deps: prefix is up to date; pass -Force to rebuild."
        exit 0
    }
    Write-Host "build-deps: stamp check failed; rebuilding."
}

# Source preparation -----------------------------------------------------
foreach ($name in @("SDL3", "Assimp", "FreeType", "RmlUi")) {
    $dependency = Get-Dependency $name
    Ensure-GitCheckout $name (Join-Path $repoRoot $dependency.sourceDir) `
        $dependency.origin.url $dependency.revision
}

$openexr = Get-Dependency "OpenEXR"
Ensure-GitCheckout "OpenEXR" (Join-Path $sourceRoot "openexr") `
    $openexr.origin.url $openexr.revision

$imath = Get-Dependency "Imath"
Ensure-GitCheckout "Imath" (Join-Path $sourceRoot "imath") `
    $imath.origin.url $imath.revision

$jolt = Get-Dependency "Jolt"
if ($jolt.origin.type -eq "url" -and $jolt.origin.sha256) {
    Ensure-TarSource "jolt" (Join-Path $sourceRoot "jolt") `
        $jolt.origin.url $jolt.origin.sha256
}
else {
    Ensure-GitCheckout "Jolt" (Join-Path $sourceRoot "jolt") `
        $jolt.origin.url $jolt.revision
}

$ozz = $manifest.dependencies | Where-Object { $_.name -eq "Ozz" } | Select-Object -First 1
if ($ozz -and $ozz.origin -and $ozz.origin.url -and $ozz.origin.sha256) {
    Ensure-TarSource "ozz" (Join-Path $sourceRoot "ozz") `
        $ozz.origin.url $ozz.origin.sha256
}
else {
    Write-Host "build-deps: warning: ozz is not described by a pinned archive in the manifest."
}

$googletest = $manifest.dependencies | Where-Object { $_.name -eq "googletest" } | Select-Object -First 1
if ($googletest -and $googletest.origin -and $googletest.origin.url) {
    Ensure-GitCheckout "googletest" (Join-Path $sourceRoot "googletest") `
        $googletest.origin.url $googletest.revision
}

# Configure --------------------------------------------------------------
$featureArgs = @(
    ("-DVULKANLEARN_BUILD_ANIMATION_TOOLS=" + $(if ($AnimationTools) { "ON" } else { "OFF" })),
    ("-DVULKANLEARN_BUILD_TESTS=" + $(if ($BuildTests) { "ON" } else { "OFF" })),
    ("-DVULKANLEARN_ENABLE_CSHARP_BLUEPRINTS=" + $(if ($CSharpBlueprints) { "ON" } else { "OFF" })),
    ("-DVULKANLEARN_ENABLE_EDITOR=" + $(if ($Editor) { "ON" } else { "OFF" }))
)

$configureArgs = @(
    "-S", (Join-Path $repoRoot "ThirdParty"),
    "-B", $BuildDir,
    "-G", "MinGW Makefiles",
    ("-DCMAKE_BUILD_TYPE=" + $Config),
    ("-DVULKANLEARN_DEPS_ROOT=" + $DepsRoot),
    ("-DVULKANLEARN_DEPS_CONFIG=" + $Config),
    ("-DVULKANLEARN_DEPS_JOBS=" + $Jobs),
    ("-DVULKANLEARN_DEPS_BUILD_ROOT=" + (Join-Path $DepsRoot "_build")),
    ("-DVULKANLEARN_DEPS_SOURCES=" + $sourceRoot)
) + $featureArgs

if ($Config -eq "RelWithDebInfo") {
    # Keep line tables but avoid multi-GB dependency DLLs and the engine's
    # >2 GB RelWithDebInfo executable.
    $configureArgs += @(
        "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG",
        "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG")
}

Write-Host "build-deps: configuring"
Invoke-Checked "cmake configure" { & cmake @configureArgs }

# Validates the manifest/lock/prefix identity without writing a stamp.
& $stampScript -BuildDir $BuildDir -Prefix $prefix -Config $Config | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "build-deps: dependency lock validation failed."
}

Write-Host ("build-deps: building with {0} jobs" -f $Jobs)
Invoke-Checked "cmake build" { & cmake --build $BuildDir --parallel $Jobs }

Write-Host "build-deps: writing stamp"
& $stampScript -BuildDir $BuildDir -Prefix $prefix -Config $Config -Write
if ($LASTEXITCODE -ne 0) {
    throw "build-deps: failed to write deps-stamp.json."
}

& $stampScript -BuildDir $BuildDir -Prefix $prefix -Config $Config -Check -NoTouch
if ($LASTEXITCODE -ne 0) {
    throw "build-deps: written stamp does not verify."
}

if (-not (Test-Path -LiteralPath $configFile)) {
    throw "build-deps: vl_depsConfig.cmake was not installed at $configFile."
}

$dllCount = @(Get-ChildItem -LiteralPath (Join-Path $prefix "bin") -Filter "*.dll" -File -ErrorAction SilentlyContinue).Count
$libCount = @(Get-ChildItem -LiteralPath (Join-Path $prefix "lib") -File -ErrorAction SilentlyContinue).Count
Write-Host ("build-deps: done. prefix={0} dlls={1} libs={2}" -f $prefix, $dllCount, $libCount)
