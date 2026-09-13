param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$BuildDirectory = "build"
)

$ErrorActionPreference = "Stop"

function New-ValidationLogDirectory {
    param([string]$Root)

    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $directory = Join-Path $Root "artifacts/ue-lite-validation/$timestamp"
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    return $directory
}

function Write-LogTail {
    param(
        [string]$LogPath,
        [string[]]$Patterns
    )

    if (!(Test-Path -LiteralPath $LogPath)) {
        return
    }

    $matches = Select-String -LiteralPath $LogPath -Pattern $Patterns -ErrorAction SilentlyContinue
    if ($matches.Count -eq 0) {
        Get-Content -LiteralPath $LogPath -Tail 12
        return
    }

    $matches | Select-Object -Last 16 | ForEach-Object {
        Write-Host "    $($_.Line)"
    }
}

function Invoke-ValidationStep {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$LogPath,
        [string[]]$SummaryPatterns
    )

    Write-Host "==> $Name"
    Write-Host "    log: $LogPath"

    $stderrPath = "$LogPath.stderr"
    Remove-Item -LiteralPath $LogPath, $stderrPath -Force -ErrorAction SilentlyContinue

    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $Arguments `
        -RedirectStandardOutput $LogPath `
        -RedirectStandardError $stderrPath `
        -WindowStyle Hidden `
        -Wait `
        -PassThru

    if ((Test-Path -LiteralPath $stderrPath) -and ((Get-Item -LiteralPath $stderrPath).Length -gt 0)) {
        Add-Content -LiteralPath $LogPath -Value ""
        Add-Content -LiteralPath $LogPath -Value "----- STDERR -----"
        Get-Content -LiteralPath $stderrPath | Add-Content -LiteralPath $LogPath
    }

    Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    $exitCode = $process.ExitCode

    Write-Host "    exit code: $exitCode"
    Write-LogTail -LogPath $LogPath -Patterns $SummaryPatterns

    if ($exitCode -ne 0) {
        throw "Validation step failed: $Name (exit code $exitCode). See log: $LogPath"
    }
}

Push-Location -LiteralPath $RepoRoot
try {
    # 旧实现清理（2026-09-12）前，这里要求作者提供的
    # Common/Profiles/Hair/hairAzimuthalLut.json 存在；Hair 实现与它的 profile 目录都已删除，
    # 该前置检查随之移除（它只是 preflight，不参与任何 test step）。

    $logDirectory = New-ValidationLogDirectory -Root $RepoRoot
    $mainExe = Join-Path $RepoRoot (Join-Path $BuildDirectory "bin/main.exe")
    $boundaryAudit = Join-Path $RepoRoot "tool/ue-lite-boundary-audit.ps1"

    $commonRuntimePatterns = @(
        '\[Diagnostics\]\[(Info|Error|Warning)\]',
        'retire queue max pending',
        'LoadWorld failed',
        'LoadWorld path resolve failed'
    )

    Invoke-ValidationStep `
        -Name "Build" `
        -FilePath "cmake" `
        -Arguments @("--build", $BuildDirectory, "-j") `
        -LogPath (Join-Path $logDirectory "00-build.log") `
        -SummaryPatterns @('Built target main', 'error', 'failed')

    if (!(Test-Path -LiteralPath $mainExe)) {
        throw "Missing executable after build: $mainExe. Check CMake output in the build log."
    }

    Invoke-ValidationStep `
        -Name "Static boundary audit" `
        -FilePath "powershell" `
        -Arguments @("-ExecutionPolicy", "Bypass", "-File", $boundaryAudit) `
        -LogPath (Join-Path $logDirectory "01-boundary-audit.log") `
        -SummaryPatterns @('UE-Lite boundary audit passed', 'UE-Lite boundary audit failed')

    Invoke-ValidationStep `
        -Name "CTest" `
        -FilePath "ctest" `
        -Arguments @("--test-dir", $BuildDirectory, "--output-on-failure") `
        -LogPath (Join-Path $logDirectory "02-ctest.log") `
        -SummaryPatterns @('tests passed', 'tests failed', 'Test project')

    Invoke-ValidationStep `
        -Name "Shader manual reload transaction" `
        -FilePath $mainExe `
        -Arguments @("--shader-reload-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "03-shader-reload.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Shader compute reload transaction" `
        -FilePath $mainExe `
        -Arguments @("--shader-compute-reload-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "05-shader-compute-reload.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "World graph transaction" `
        -FilePath $mainExe `
        -Arguments @("--world-graph-transaction-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "09-world-graph-transaction.log") `
        -SummaryPatterns $commonRuntimePatterns

    Write-Host "UE-Lite final validation passed."
    Write-Host "Logs: $logDirectory"
}
finally {
    Pop-Location
}
