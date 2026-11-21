<#
.SYNOPSIS
Configure a VulkanLearn build that hosts C# blueprints, including sandboxed/CI hosts.

.DESCRIPTION
dotnet/MSBuild 在 Codex/CI 沙箱里通常只能写工作区根目录；worktree 和资源仓库
往往在工作区之外，托管构建会在 MSBuild 临时目录或 obj/输出上失败。本脚本
把 VULKANLEARN_SCRIPT_BUILD_ROOT 指到主 checkout 的 artifacts 下，并打开
C# 宿主；之后的普通 `cmake --build` 不需要任何环境变量。

主 checkout 通过 `git worktree list` 的第一项解析——这是仓库约定的
`VulkanLearn-worktrees/<topic>` 布局下唯一稳定可写的根。

.EXAMPLE
  powershell -NoProfile -File tool\script-build\configure-csharp-build.ps1
  powershell -NoProfile -File tool\script-build\configure-csharp-build.ps1 -BuildDir build-cloth-csharp
#>
param(
    [string]$BuildDir = "build-csharp",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType = "Release",
    [string]$Dotnet = $env:VULKANLEARN_DOTNET_EXECUTABLE,
    [string]$BuildRoot = $env:VULKANLEARN_SCRIPT_BUILD_ROOT,
    [switch]$PrintOnly
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$worktreeLines = & git -C $repo worktree list --porcelain
$mainCheckout = ($worktreeLines | Where-Object { $_ -like "worktree *" } |
    Select-Object -First 1) -replace "^worktree ", ""
if (-not $mainCheckout) {
    $mainCheckout = $repo
}

if (-not $BuildRoot) {
    $topic = Split-Path (Split-Path $repo -Parent) -Leaf
    $BuildRoot = Join-Path $mainCheckout (Join-Path "artifacts\script-build" $topic)
}
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null

if (-not $Dotnet) {
    $candidates = @(
        (Join-Path $mainCheckout "artifacts\dotnet-sdk\dotnet.exe"),
        "C:\Program Files\dotnet\dotnet.exe"
    )
    $Dotnet = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Dotnet -or -not (Test-Path $Dotnet)) {
    throw "找不到 dotnet.exe；用 -Dotnet 或 VULKANLEARN_DOTNET_EXECUTABLE 指定 .NET 11 SDK。"
}

$configureArgs = @(
    "-S", $repo,
    "-B", (Join-Path $repo $BuildDir),
    "-G", "MinGW Makefiles",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DVULKANLEARN_ENABLE_CSHARP_BLUEPRINTS=ON",
    "-DVULKANLEARN_DOTNET_EXECUTABLE=$($Dotnet -replace '\\', '/')",
    "-DVULKANLEARN_SCRIPT_BUILD_ROOT=$($BuildRoot -replace '\\', '/')"
)
if ($env:VULKANLEARN_DEPS_ROOT) {
    $configureArgs += "-DVULKANLEARN_DEPS_MODE=prebuilt"
    $configureArgs += "-DVULKANLEARN_DEPS_ROOT=$($env:VULKANLEARN_DEPS_ROOT -replace '\\', '/')"
}

Write-Host "repo        : $repo"
Write-Host "main        : $mainCheckout"
Write-Host "build dir   : $(Join-Path $repo $BuildDir)"
Write-Host "managed root: $BuildRoot"
Write-Host "dotnet      : $Dotnet"
Write-Host ""
Write-Host "cmake $($configureArgs -join ' ')"

if ($PrintOnly) {
    return
}

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "cmake configure failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "构建：cmake --build $(Join-Path $repo $BuildDir) --target main_editor main_game vl_gameplay -j 4"
