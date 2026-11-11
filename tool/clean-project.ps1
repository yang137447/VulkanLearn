[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [switch]$Apply,
    [switch]$Reconfigure,
    [string]$Keep = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$resolvedBuild = (Resolve-Path -LiteralPath $BuildDir).Path
if (-not $resolvedBuild.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "clean-project: build directory is outside the repository: $resolvedBuild"
}

function To-ExtendedPath([string]$Path) {
    if ($Path.StartsWith("\\?\")) { return $Path }
    if ($Path.Length -ge 240) { return "\\?\" + $Path }
    return $Path
}

$excluded = @("_deps", "extern", "third_party", "ThirdParty")
$projectTargetDirs = [System.IO.Directory]::EnumerateDirectories(
        (To-ExtendedPath $resolvedBuild), "*.dir",
        [System.IO.SearchOption]::AllDirectories) |
    Where-Object {
        $relative = $_.Substring($resolvedBuild.Length).TrimStart('\', '/')
        $parts = $relative -split '[\\/]'
        -not ($parts | Where-Object { $excluded -contains $_ })
    }

if (-not $projectTargetDirs) {
    throw "clean-project: no project target directories found under $resolvedBuild"
}

$patterns = @("*.obj", "*.obj.d", "*.o", "*.o.d", "*.pdb", "*.ilk",
    "*.a", "*.dll.a", "*.exe", "*.dll")
$keepRegex = if ($Keep) { [regex]::new($Keep) } else { $null }

$targets = foreach ($dir in $projectTargetDirs) {
    foreach ($file in [System.IO.Directory]::EnumerateFiles(
            (To-ExtendedPath $dir), "*", [System.IO.SearchOption]::AllDirectories)) {
        $name = [System.IO.Path]::GetFileName($file)
        foreach ($pattern in $patterns) {
            if ($name -like $pattern) { $file; break }
        }
    }
}
if ($keepRegex) {
    $targets = $targets | Where-Object { -not $keepRegex.IsMatch($_) }
}
$targets = @($targets | Sort-Object -Unique)

Write-Host ("clean-project: build={0}" -f $resolvedBuild)
Write-Host ("clean-project: target dirs={0}, files={1}" -f @($projectTargetDirs).Count, $targets.Count)
if (-not $Apply) {
    $targets | Select-Object -First 20 | ForEach-Object { Write-Host ("  would remove " + $_) }
    if ($targets.Count -gt 20) { Write-Host ("  ... and {0} more" -f ($targets.Count - 20)) }
    Write-Host "clean-project: dry run; pass -Apply to delete"
    return
}

foreach ($file in $targets) {
    [System.IO.File]::Delete((To-ExtendedPath $file))
}
Write-Host ("clean-project: removed {0} files" -f $targets.Count)

if ($Reconfigure) {
    & cmake -S $repoRoot -B $resolvedBuild
    if ($LASTEXITCODE -ne 0) {
        throw "clean-project: cmake reconfigure failed with exit code $LASTEXITCODE"
    }
}
