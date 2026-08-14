param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$BuildDirectory = "build",
    [int]$FrameSmokeCount = 120,
    [int]$ReloadStressCount = 100,
    [int]$EnvironmentStressCount = 3,
    [string]$EnvironmentStressScene = "scenes/SC_speedtree.json",
    [int]$LightStressCount = 3,
    [int]$ResizeStressCount = 6,
    [int]$GraphReloadStressCount = 6,
    [string]$ReloadStressScene = "scenes/SC_speedtree.json"
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
    $logDirectory = New-ValidationLogDirectory -Root $RepoRoot
    $mainExe = Join-Path $RepoRoot (Join-Path $BuildDirectory "bin/main.exe")
    $boundaryAudit = Join-Path $RepoRoot "tool/ue-lite-boundary-audit.ps1"

    $commonRuntimePatterns = @(
        '\[Diagnostics\]\[(Info|Error|Warning)\]',
        'Frame smoke test completed',
        'Environment update stress completed',
        'World reload stress completed',
        'World reload failure rollback test completed',
        'Resize stress completed',
        'Render graph reload stress completed',
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
        -Name "Shader automatic reload transaction" `
        -FilePath $mainExe `
        -Arguments @("--shader-auto-reload-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "04-shader-auto-reload.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Shader compute reload transaction" `
        -FilePath $mainExe `
        -Arguments @("--shader-compute-reload-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "05-shader-compute-reload.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Shader definition reload transaction" `
        -FilePath $mainExe `
        -Arguments @("--shader-definition-reload-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "06-shader-definition-reload.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Shader UI reload transaction" `
        -FilePath $mainExe `
        -Arguments @("--shader-ui-reload-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "07-shader-ui-reload.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Shader shutdown in-flight transaction" `
        -FilePath $mainExe `
        -Arguments @("--shader-shutdown-inflight-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "08-shader-shutdown-inflight.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "World graph transaction" `
        -FilePath $mainExe `
        -Arguments @("--world-graph-transaction-test", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "09-world-graph-transaction.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Frame smoke" `
        -FilePath $mainExe `
        -Arguments @("--framesmoke", $FrameSmokeCount.ToString(), "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "10-framesmoke.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Environment update stress" `
        -FilePath $mainExe `
        -Arguments @(
            "--environmentstress",
            $EnvironmentStressCount.ToString(),
            "--initial-scene",
            $EnvironmentStressScene,
            "--exit-after-tests",
            "--no-dev-ui"
        ) `
        -LogPath (Join-Path $logDirectory "11-environmentstress.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "World reload stress" `
        -FilePath $mainExe `
        -Arguments @("--reloadstress", $ReloadStressScene, $ReloadStressCount.ToString(), "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "12-reloadstress.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Bad scene rollback" `
        -FilePath $mainExe `
        -Arguments @("--reloadfail", "scenes/DOES_NOT_EXIST.json", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "13-reloadfail-scene.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Bad material rollback" `
        -FilePath $mainExe `
        -Arguments @("--reloadfail-material", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "14-reloadfail-material.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Bad mesh rollback" `
        -FilePath $mainExe `
        -Arguments @("--reloadfail-mesh", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "15-reloadfail-mesh.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Bad texture rollback" `
        -FilePath $mainExe `
        -Arguments @("--reloadfail-texture", "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "16-reloadfail-texture.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Light buffer retire" `
        -FilePath $mainExe `
        -Arguments @("--lightstress", $LightStressCount.ToString(), "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "17-lightstress.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Resize stress" `
        -FilePath $mainExe `
        -Arguments @("--resizestress", $ResizeStressCount.ToString(), "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "18-resizestress.log") `
        -SummaryPatterns $commonRuntimePatterns

    Invoke-ValidationStep `
        -Name "Render graph reload stress" `
        -FilePath $mainExe `
        -Arguments @("--graphreloadstress", $GraphReloadStressCount.ToString(), "--exit-after-tests") `
        -LogPath (Join-Path $logDirectory "19-graphreloadstress.log") `
        -SummaryPatterns $commonRuntimePatterns

    Write-Host "UE-Lite final validation passed."
    Write-Host "Logs: $logDirectory"
}
finally {
    Pop-Location
}
