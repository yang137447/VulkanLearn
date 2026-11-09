<#
.SYNOPSIS
    Run the UE Niagara -> VulkanLearn acceptance audit.

.DESCRIPTION
    Static mode checks the 49 derived targets, README mappings, PS/gallery JSON
    and texture/mesh/MI references. -Smoke also launches every gallery once,
    kills it after the screenshot lands (main.exe has no quit path), and records
    Scene.LoadFailed / EngineLoop init failed / VUID / particle frame stats and
    the performance budget. -WriteBudget merges this run into the budget baseline.

.EXAMPLE
    .\run_particle_acceptance.ps1

.EXAMPLE
    .\run_particle_acceptance.ps1 -Smoke -Only SC_particle_ue_misc

.EXAMPLE
    .\run_particle_acceptance.ps1 -Smoke -WriteBudget
#>
param(
    [switch]$Smoke,
    [string]$Only = "",
    [string]$UeRoot = "D:\YYBWorkSpace\UnrealProject\effect\Content\NiagaraExamples",
    [string]$ResourceRoot = "",
    [string]$OutDir = "artifacts/particle-niagara-acceptance",
    [string]$MainExe = "",
    [double]$TimeoutSeconds = 90,
    [string]$BudgetFile = "",
    [double]$BudgetMargin = 20,
    [switch]$WriteBudget
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$script = Join-Path $PSScriptRoot "particle_niagara_acceptance.py"

$arguments = @(
    $script,
    "--ue-root", $UeRoot,
    "--out", $OutDir,
    "--smoke-timeout", $TimeoutSeconds
)
if ($ResourceRoot) {
    $arguments += @("--resource-root", $ResourceRoot)
}
if ($MainExe) {
    $arguments += @("--main-exe", $MainExe)
}
if ($BudgetFile) {
    $arguments += @("--budget-file", $BudgetFile)
}
if ($BudgetMargin -ne 20) {
    $arguments += @("--budget-margin", "$BudgetMargin")
}
if ($Smoke) {
    $arguments += "--smoke"
}
if ($WriteBudget) {
    $arguments += "--write-budget"
}
if ($Only) {
    $arguments += @("--only", $Only)
}

Push-Location $repoRoot
try {
    & py -3 @arguments
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
