[CmdletBinding()]
param(
    [string]$DepsRoot = "D:\YYBWorkSpace\VulkanLearn-deps",
    [switch]$Apply,
    [double]$BudgetGB = 20,
    [string[]]$KeepConfigs = @("Debug", "Release"),
    [string[]]$BuildRoots = @(),
    [string]$CurrentPrefix = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Write-Fail([string]$Message) {
    Write-Host ("deps-gc: " + $Message) -ForegroundColor Red
    exit 1
}

function Get-DirectorySize([string]$Path) {
    $sum = (Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum).Sum
    if ($null -eq $sum) { return 0 }
    return [double]$sum
}

function Format-Size([double]$Bytes) {
    if ($Bytes -ge 1GB) { return ("{0:N2} GB" -f ($Bytes / 1GB)) }
    return ("{0:N1} MB" -f ($Bytes / 1MB))
}

# A stamp written through a long path must still match a root passed in 8.3 form
# (for example %TEMP% expansion). Compare the last three segments as a fallback.
function Test-PrefixMatch([string]$StampPrefix, [string]$ActualPath) {
    if (-not $StampPrefix) { return $false }
    $stampFull = [System.IO.Path]::GetFullPath($StampPrefix).TrimEnd("\", "/")
    $actualFull = [System.IO.Path]::GetFullPath($ActualPath).TrimEnd("\", "/")
    if ($stampFull -ieq $actualFull) { return $true }
    $stampSegments = $stampFull -split '[\\/]'
    $actualSegments = $actualFull -split '[\\/]'
    if ($stampSegments.Count -lt 3 -or $actualSegments.Count -lt 3) { return $false }
    for ($index = 1; $index -le 3; $index++) {
        if ($stampSegments[$stampSegments.Count - $index] -ine $actualSegments[$actualSegments.Count - $index]) {
            return $false
        }
    }
    return $true
}

function Test-PrefixReferenced([string]$Haystack, [string]$PrefixPath, [string]$Toolchain, [string]$Config) {
    if ($Haystack.IndexOf($PrefixPath, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { return $true }
    $suffix = "\" + $Toolchain + "\" + $Config
    return ($Haystack.IndexOf($suffix, [System.StringComparison]::OrdinalIgnoreCase) -ge 0)
}

function Assert-SafeRoot([string]$Root) {
    $full = [System.IO.Path]::GetFullPath($Root)
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        Write-Fail ("deps root does not exist: " + $full)
    }
    $pathRoot = [System.IO.Path]::GetPathRoot($full)
    if ($full.TrimEnd("\", "/") -eq $pathRoot.TrimEnd("\", "/")) {
        Write-Fail ("refusing to operate on a drive root: " + $full)
    }
    if ($full.TrimEnd("\", "/").Length -lt 12) {
        Write-Fail ("refusing to operate on a short/ambiguous root: " + $full)
    }
    if ($full.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Fail ("deps root must live outside the repository: " + $full)
    }
    $forbidden = @($env:USERPROFILE, $env:WINDIR, $env:SystemDrive + "\Program Files")
    foreach ($candidate in $forbidden) {
        if ($candidate -and
            ($full.TrimEnd("\", "/") -ieq ([System.IO.Path]::GetFullPath($candidate).TrimEnd("\", "/")))) {
            Write-Fail ("refusing to operate on a system directory: " + $full)
        }
    }
    return $full
}

function Assert-SafePrefix([string]$Root, [string]$PrefixPath) {
    $full = [System.IO.Path]::GetFullPath($PrefixPath)
    $rootWithSeparator = $Root.TrimEnd("\", "/") + "\"
    if (-not $full.StartsWith($rootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Fail ("prefix is outside the deps root: " + $full)
    }
    $relative = $full.Substring($rootWithSeparator.Length).Trim("\", "/")
    $parts = $relative -split '[\\/]'
    if ($parts.Count -ne 2) {
        Write-Fail ("prefix must be <deps-root>/<toolchain-id>/<config>: " + $full)
    }
    if ($parts[1] -notin @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")) {
        Write-Fail ("unexpected configuration directory: " + $full)
    }
    $item = Get-Item -LiteralPath $full -Force
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        Write-Fail ("refusing to delete a reparse point: " + $full)
    }
    return $full
}

$resolvedRoot = Assert-SafeRoot $DepsRoot

$prefixes = @()
foreach ($toolchainDir in (Get-ChildItem -LiteralPath $resolvedRoot -Directory -Force)) {
    if ($toolchainDir.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        Write-Host ("deps-gc: skipping reparse point " + $toolchainDir.FullName)
        continue
    }
    foreach ($configDir in (Get-ChildItem -LiteralPath $toolchainDir.FullName -Directory -Force -ErrorAction SilentlyContinue)) {
        $stampPath = Join-Path $configDir.FullName "deps-stamp.json"
        if (-not (Test-Path -LiteralPath $stampPath)) { continue }
        $stamp = $null
        try {
            $stamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
        }
        catch {
            $stamp = $null
        }
        $lastUsed = [datetime]::MinValue
        if ($null -ne $stamp -and $stamp.lastUsed) {
            try { $lastUsed = [datetime]$stamp.lastUsed } catch { $lastUsed = [datetime]::MinValue }
        }
        $prefixes += [pscustomobject]@{
            Path        = $configDir.FullName
            StampPath   = $stampPath
            Stamp       = $stamp
            Toolchain   = $toolchainDir.Name
            Config      = $configDir.Name
            Valid       = ($null -ne $stamp -and $stamp.schema -eq "vulkanlearn-deps-stamp/v1" -and
                           (Test-PrefixMatch ([string]$stamp.prefix) $configDir.FullName))
            LastUsed    = $lastUsed
            SizeBytes   = [double]0
            Referenced  = $false
            Decision    = ""
            Reason      = ""
        }
    }
}

if ($prefixes.Count -eq 0) {
    Write-Fail ("no dependency prefixes found under " + $resolvedRoot + "; refusing to operate on an empty root")
}

$buildRootsToScan = @()
if ($BuildRoots.Count -gt 0) {
    $buildRootsToScan = @($BuildRoots | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
}
else {
    $buildRootsToScan += @(Get-ChildItem -LiteralPath $repoRoot -Directory -Filter "build*" -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName })
    $worktreeRoot = Split-Path (Split-Path $repoRoot -Parent) -Parent
    if (Test-Path -LiteralPath $worktreeRoot) {
        foreach ($topic in (Get-ChildItem -LiteralPath $worktreeRoot -Directory -ErrorAction SilentlyContinue)) {
            $checkout = Join-Path $topic.FullName "VulkanLearn"
            if (-not (Test-Path -LiteralPath $checkout)) { continue }
            $buildRootsToScan += @(Get-ChildItem -LiteralPath $checkout -Directory -Filter "build*" -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName })
        }
    }
}
$buildRootsToScan = @($buildRootsToScan | Sort-Object -Unique)

$referenceText = [System.Text.StringBuilder]::new()
foreach ($buildRoot in $buildRootsToScan) {
    $cachePath = Join-Path $buildRoot "CMakeCache.txt"
    if (Test-Path -LiteralPath $cachePath) {
        [void]$referenceText.AppendLine([System.IO.File]::ReadAllText($cachePath))
    }
    $buildStamp = Join-Path $buildRoot "deps-stamp.json"
    if (Test-Path -LiteralPath $buildStamp) {
        [void]$referenceText.AppendLine([System.IO.File]::ReadAllText($buildStamp))
    }
}
$haystack = $referenceText.ToString()

foreach ($prefix in $prefixes) {
    $prefix.Referenced = Test-PrefixReferenced $haystack $prefix.Path $prefix.Toolchain $prefix.Config
}

$validPrefixes = @($prefixes | Where-Object { $_.Valid })
if ($validPrefixes.Count -eq 0) {
    Write-Fail ("no valid deps-stamp.json found under " + $resolvedRoot)
}

$currentToolchain = ""
$activeConfig = ""
if ($CurrentPrefix) {
    $currentFull = [System.IO.Path]::GetFullPath($CurrentPrefix)
    $match = $prefixes | Where-Object { Test-PrefixMatch $currentFull $_.Path }
    if (-not $match) {
        Write-Fail ("-CurrentPrefix is not a stamped prefix under the deps root: " + $currentFull)
    }
    $currentToolchain = $match.Toolchain
    $activeConfig = $match.Config
}
else {
    $newest = $validPrefixes | Sort-Object -Property LastUsed -Descending | Select-Object -First 1
    $currentToolchain = $newest.Toolchain
    $activeConfig = $newest.Config
}

# KeepConfigs is the hard-keep set; the most recently used config (normally the daily
# RelWithDebInfo preset) is protected as well so budget pressure never deletes it first.
$effectiveKeepConfigs = @($KeepConfigs)
if ($activeConfig -and $activeConfig -notin $effectiveKeepConfigs) {
    $effectiveKeepConfigs += $activeConfig
}

$otherToolchains = @($validPrefixes |
    Where-Object { $_.Toolchain -ne $currentToolchain } |
    Group-Object -Property Toolchain |
    Sort-Object -Property @{ Expression = { ($_.Group | Measure-Object -Property LastUsed -Maximum).Maximum } } -Descending)
$previousToolchain = if ($otherToolchains.Count -gt 0) { $otherToolchains[0].Name } else { "" }

foreach ($prefix in $prefixes) {
    $prefix.SizeBytes = Get-DirectorySize $prefix.Path
}

$totalBytes = ($prefixes | Measure-Object -Property SizeBytes -Sum).Sum
$budgetBytes = $BudgetGB * 1GB

foreach ($prefix in $prefixes) {
    if (-not $prefix.Valid) {
        $prefix.Decision = "keep-invalid"
        $prefix.Reason = "stamp missing, unreadable or prefix mismatch"
    }
    elseif ($prefix.Referenced) {
        $prefix.Decision = "keep-referenced"
        $prefix.Reason = "a build tree references this prefix"
    }
    elseif ($prefix.Toolchain -eq $currentToolchain -and $prefix.Config -in $effectiveKeepConfigs) {
        $prefix.Decision = "keep-current"
        $prefix.Reason = "current toolchain, active config"
    }
    elseif ($previousToolchain -and $prefix.Toolchain -eq $previousToolchain) {
        $prefix.Decision = "keep-previous"
        $prefix.Reason = "one previous toolchain is retained"
    }
    else {
        $prefix.Decision = "candidate"
        $prefix.Reason = "not referenced and outside the keep set"
    }
}

$keptBytes = ($prefixes | Where-Object { $_.Decision -ne "candidate" } |
    Measure-Object -Property SizeBytes -Sum).Sum
if ($null -eq $keptBytes) { $keptBytes = 0 }

$candidates = @($prefixes | Where-Object { $_.Decision -eq "candidate" } |
    Sort-Object -Property LastUsed)
foreach ($prefix in $candidates) {
    if (($keptBytes + $prefix.SizeBytes) -le $budgetBytes) {
        $prefix.Decision = "keep-budget"
        $prefix.Reason = "fits the remaining budget"
        $keptBytes += $prefix.SizeBytes
    }
    else {
        $prefix.Decision = "delete"
        $prefix.Reason = "over budget; least recently used"
    }
}
if ($keptBytes -gt $budgetBytes) {
    Write-Host (("deps-gc: WARNING hard-kept prefixes are {0} over the {1} budget; " +
        "raise -BudgetGB or trim -KeepConfigs") -f (Format-Size ($keptBytes - $budgetBytes)), (Format-Size $budgetBytes))
}

Write-Host ("deps-gc: root={0}" -f $resolvedRoot)
Write-Host ("deps-gc: prefixes={0} total={1} budget={2}" -f
    $prefixes.Count, (Format-Size $totalBytes), (Format-Size $budgetBytes))
Write-Host ("deps-gc: current toolchain={0} previous={1}" -f
    $currentToolchain, $(if ($previousToolchain) { $previousToolchain } else { "<none>" }))
Write-Host ("deps-gc: hard-keep configs={0} active={1}" -f
    ($effectiveKeepConfigs -join ","), $activeConfig)
Write-Host ("deps-gc: build trees scanned={0}" -f $buildRootsToScan.Count)
Write-Host ""

foreach ($prefix in ($prefixes | Sort-Object -Property Toolchain, Config)) {
    Write-Host ("  {0,-14} {1,-28} {2,-14} {3,9}  lastUsed={4:yyyy-MM-dd}  {5} ({6})" -f
        $prefix.Decision.ToUpperInvariant(), $prefix.Toolchain, $prefix.Config,
        (Format-Size $prefix.SizeBytes), $(if ($prefix.LastUsed -eq [datetime]::MinValue) { "never" } else { $prefix.LastUsed.ToString("yyyy-MM-dd") }),
        $prefix.Path, $prefix.Reason)
}

$toDelete = @($prefixes | Where-Object { $_.Decision -eq "delete" })
if ($toDelete.Count -eq 0) {
    Write-Host ""
    Write-Host "deps-gc: nothing to delete"
    exit 0
}

if (-not $Apply) {
    Write-Host ""
    Write-Host ("deps-gc: dry run; {0} prefix(es) would be deleted, pass -Apply to delete" -f $toDelete.Count)
    exit 0
}

foreach ($prefix in $toDelete) {
    $safe = Assert-SafePrefix $resolvedRoot $prefix.Path
    if (Test-PrefixReferenced $haystack $safe $prefix.Toolchain $prefix.Config) {
        Write-Fail ("refusing to delete a referenced prefix: " + $safe)
    }
    Write-Host ("deps-gc: removing " + $safe)
    Remove-Item -LiteralPath $safe -Recurse -Force
    $toolchainDir = Split-Path -Parent $safe
    $relative = $toolchainDir.Substring($resolvedRoot.TrimEnd("\", "/").Length).Trim("\", "/")
    if ($relative -and ($relative -notmatch '[\\/]')) {
        $remaining = @(Get-ChildItem -LiteralPath $toolchainDir -Force -ErrorAction SilentlyContinue)
        if ($remaining.Count -eq 0) {
            $toolchainItem = Get-Item -LiteralPath $toolchainDir -Force
            if (-not ($toolchainItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                Remove-Item -LiteralPath $toolchainDir -Force
                Write-Host ("deps-gc: removed empty toolchain directory " + $toolchainDir)
            }
        }
    }
}
Write-Host ("deps-gc: removed {0} prefix(es)" -f $toDelete.Count)
exit 0
