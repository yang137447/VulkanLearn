param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$ExecutableSha256,
    [Parameter(Mandatory=$true)][string]$LitCase,
    [Parameter(Mandatory=$true)][string]$LaunchFixture,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$RunGpu
)
$ErrorActionPreference = "Stop"
$captureExecutable = (Resolve-Path -LiteralPath $Executable).Path
$litCasePath = (Resolve-Path -LiteralPath $LitCase).Path
$fixturePath = (Resolve-Path -LiteralPath $LaunchFixture).Path
$actualHash = (Get-FileHash -LiteralPath $captureExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $ExecutableSha256.ToLowerInvariant()) { throw "Coherent native executable hash mismatch" }
$fixture = Get-Content -LiteralPath $fixturePath -Raw | ConvertFrom-Json
$case = Get-Content -LiteralPath $litCasePath -Raw | ConvertFrom-Json
if ($case.kind -ne "native-terrain-lit-replay-v1") { throw "Explicit native lit case required" }
if ($case.originalGpuPages.Count -eq 0 -or $case.actualRoiSlotFootprint.Count -eq 0) {
    throw "Real original-GPU reference pages and actual slot footprint required"
}
$targetDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $targetDirectory) { throw "Use a new output directory" }
New-Item -ItemType Directory -Path $targetDirectory | Out-Null
$captureArguments = @($fixture.argv | Select-Object -Skip 1)
$prepared = [ordered]@{
    executable = $captureExecutable
    executableSha256 = $actualHash
    argv = $captureArguments
    cwd = $fixture.cwd
    litCase = $litCasePath
    fixture = $fixturePath
    gpuAuthorizedByCallerSwitch = [bool]$RunGpu
    scope = "Explicit bounded diagnostic replay only; no P5 acceptance or automatic fallback"
}
$prepared | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $targetDirectory "command.json") -Encoding UTF8
if (-not $RunGpu) {
    Write-Output "Prepared only; GPU not started"
    return
}
$previousTesCase = $env:VULKANLEARN_TERRAIN_CAPTURE_CASE
$previousLitCase = $env:VULKANLEARN_TERRAIN_LIT_CASE
try {
    $env:VULKANLEARN_TERRAIN_CAPTURE_CASE = $fixture.environment.VULKANLEARN_TERRAIN_CAPTURE_CASE
    $env:VULKANLEARN_TERRAIN_LIT_CASE = $litCasePath
    Push-Location -LiteralPath $fixture.cwd
    try {
        & $captureExecutable @captureArguments *> (Join-Path $targetDirectory "native.log")
        $nativeExitCode = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($nativeExitCode -ne 0) { throw "Native lit capture failed exit=$nativeExitCode; retain raw log" }
}
finally {
    $env:VULKANLEARN_TERRAIN_CAPTURE_CASE = $previousTesCase
    $env:VULKANLEARN_TERRAIN_LIT_CASE = $previousLitCase
}
