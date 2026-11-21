param(
    [ValidateSet("manny", "uefn", "comparison")]
    [string]$Character = "manny",
    [string]$ModelPath = $env:VULKANLEARN_MOTIONBRICKS_SOMA_ONNX,
    [string]$RuntimeDll = $env:VULKANLEARN_ONNXRUNTIME_DLL
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$referenceRoot = Join-Path $repoRoot "../../../motionbricks-ref"
if (-not $ModelPath) {
    $ModelPath = Join-Path $referenceRoot "models/mmm_v6_soma_fp32.onnx"
}
if (-not $RuntimeDll) {
    $RuntimeDll = Join-Path $referenceRoot "onnxruntime/onnxruntime-win-x64-1.22.0/lib/onnxruntime.dll"
}
$env:VULKANLEARN_MOTIONBRICKS_SOMA_ONNX = (Resolve-Path -LiteralPath $ModelPath).Path
$env:VULKANLEARN_ONNXRUNTIME_DLL = (Resolve-Path -LiteralPath $RuntimeDll).Path
$scene = switch ($Character) {
    "manny" { "Maps/SC_motionbricks/SC_motionbricks_manny.json" }
    "uefn" { "Maps/SC_motionbricks/SC_motionbricks_live_probe.json" }
    "comparison" { "Maps/SC_motionbricks/SC_motionbricks_manny_comparison.json" }
}
$output = Join-Path $repoRoot "artifacts/motionbricks"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$process = Start-Process -FilePath (Join-Path $repoRoot "build/bin/main_editor.exe") `
    -WorkingDirectory $repoRoot `
    -ArgumentList @("--initial-scene", $scene, "--console-command-delay", "2", "--console-command", '"script play"') `
    -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $output "$Character-$stamp.log") `
    -RedirectStandardError (Join-Path $output "$Character-$stamp.err") -PassThru
Write-Output "MotionBricks $Character PID=$($process.Id)"
