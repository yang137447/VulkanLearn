# run_paper_case.ps1 —— 论文 case 的采图 harness（shading-model-alignment-plan §1.5.3）
#
# 做四件事，任何一步失败就非零退出：
#   1. 用 `--initial-scene` + 排队命令（tonemap 0 / bloom 0）启动 main.exe；
#   2. 按 `-Scenes`/`-Shots` 的配对逐张采图：需要切场景时用控制台注入 `loadworld`，
#      等世界事务提交后再注入 `screenshot`；
#   3. 校验产物存在，并跑 `measure_plot_curve.py panel` 的四角 / 铺满检查；
#   4. 把日志与截图归档到 `artifacts/<Name>/<timestamp>/`。
#
# 为什么要控制台注入：`--console-command` 的整批命令在 delay 帧后**一次性**提交，
# 而 `screenshot` 只保留一个待处理路径（后一条覆盖前一条），所以"一个进程里切场景
# 分别截图"没法用启动参数表达。详见 tool/validation/console_inject.ps1。
#
# 例：
#   .\tool\validation\run_paper_case.ps1 -Name M-07 -PanelMode uv `
#     -Scene 'Maps/SC_paper_case_brdf_plot_uv/SC_paper_case_brdf_plot_uv.json' `
#     -Shots  m07_mode3_uv, m07_mode2_g1, m07_mode0_cloth, m07_mode1_hair `
#     -Scenes '', 'Maps/SC_paper_case_brdf_plot_g1/SC_paper_case_brdf_plot_g1.json',
#              'Maps/SC_paper_case_plot_cloth_solo/SC_paper_case_plot_cloth_solo.json',
#              'Maps/SC_paper_case_plot_hair_solo/SC_paper_case_plot_hair_solo.json'

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Scene,
    [Parameter(Mandatory)][string[]]$Shots,
    [string[]]$Scenes = @(),
    [string]$Name = 'paper-case',
    [int]$Delay = 180,
    [int]$LoadWaitSeconds = 8,
    [int]$ShotWaitSeconds = 4,
    [int]$ProductTimeoutSeconds = 60,
    [ValidateSet('uv', 'curve', 'skip')][string]$PanelMode = 'curve',
    [ValidateSet('uv', 'curve', 'skip')][string[]]$PanelModes = @(),
    [string]$ConfigPath = 'config/config.json',
    [string]$ExePath = 'build/bin/main.exe',
    [string]$ArtifactRoot = 'artifacts',
    [switch]$ForceShaderRebuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'console_inject.ps1')

$configFullPath = Join-Path $repoRoot $ConfigPath
$config = Get-Content $configFullPath -Raw | ConvertFrom-Json
$resourcePath = $config.resourcePath
$screenshotDir = Join-Path $resourcePath 'Generated\Screenshots'
$exeFullPath = Join-Path $repoRoot $ExePath
if (-not (Test-Path $exeFullPath)) { throw "executable not found: $exeFullPath" }

if ($Scenes.Count -gt 0 -and $Scenes.Count -ne $Shots.Count) {
    throw "-Scenes must be empty or have exactly one entry per -Shots entry"
}
if ($PanelModes.Count -gt 0 -and $PanelModes.Count -ne $Shots.Count) {
    throw "-PanelModes must be empty or have exactly one entry per -Shots entry"
}

$measureToolPath = Join-Path $PSScriptRoot 'measure_plot_curve.py'

$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$outDir = Join-Path $repoRoot (Join-Path $ArtifactRoot (Join-Path $Name $timestamp))
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Wait-ForFile {
    param([string]$Path, [int]$TimeoutSeconds)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Path) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

$problemList = New-Object System.Collections.Generic.List[string]
$logPath = Join-Path $outDir '01-run.log'
$errPath = Join-Path $outDir '01-run.err.log'

$argumentLine = '--initial-scene "{0}" --console-command "tonemap 0" --console-command "bloom strength 0" --console-command-delay {1} --no-dev-ui' -f $Scene, $Delay
if ($ForceShaderRebuild) { $argumentLine = '--shader-force-rebuild ' + $argumentLine }

$process = $null
try {
    Write-Host "[run_paper_case] launching: $exeFullPath $argumentLine"
    $process = Start-Process -FilePath $exeFullPath -WorkingDirectory $repoRoot `
        -ArgumentList $argumentLine -PassThru `
        -RedirectStandardOutput $logPath -RedirectStandardError $errPath
    $process.Id | Set-Content (Join-Path $outDir '01-run.pid')

    # 启动期要编译 / 命中 shader、建 swapchain、载首个世界；等日志出现控制台就绪再注入。
    $readyDeadline = (Get-Date).AddSeconds($ProductTimeoutSeconds * 4)
    while ((Get-Date) -lt $readyDeadline) {
        if (Test-Path $logPath) {
            $content = Get-Content $logPath -Raw
            if ($content -match 'Debug console ready') { break }
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not (Test-Path $logPath) -or (Get-Content $logPath -Raw) -notmatch 'Debug console ready') {
        throw "main.exe never reported 'Debug console ready'; see $logPath"
    }

    # 首个世界是启动时加载的，给它一点稳定时间再采第一张。
    Start-Sleep -Seconds $LoadWaitSeconds

    for ($index = 0; $index -lt $Shots.Count; $index++) {
        $shotName = $Shots[$index]
        $bmpName = if ($shotName -like '*.bmp') { $shotName } else { "$shotName.bmp" }
        $bmpPath = Join-Path $screenshotDir $bmpName

        if ($index -gt 0 -and $Scenes.Count -gt 0 -and $Scenes[$index] -ne '') {
            if (Test-Path $bmpPath) { Remove-Item $bmpPath -Force }
            Write-Host "[run_paper_case] loadworld: $($Scenes[$index])"
            Send-ConsoleLine -ProcessId $process.Id -Line "loadworld $($Scenes[$index])"
            Start-Sleep -Seconds $LoadWaitSeconds
            if ((Get-Content $logPath -Raw) -match 'LoadWorld .*failed|Scene\.ResolvePathFailed') {
                $problemList.Add("loadworld failed for $($Scenes[$index])")
            }
        }

        if (Test-Path $bmpPath) { Remove-Item $bmpPath -Force }
        Write-Host "[run_paper_case] screenshot: $bmpName"
        Send-ConsoleLine -ProcessId $process.Id -Line "screenshot $bmpName"
        if (-not (Wait-ForFile -Path $bmpPath -TimeoutSeconds $ProductTimeoutSeconds)) {
            $problemList.Add("screenshot not produced: $bmpPath")
            continue
        }
        Start-Sleep -Seconds $ShotWaitSeconds

        Copy-Item $bmpPath (Join-Path $outDir $bmpName) -Force

        if ($PanelModes.Count -gt 0) { $shotPanelMode = $PanelModes[$index] } else { $shotPanelMode = $PanelMode }
        if ($shotPanelMode -ne 'skip') {
            $panelArgs = @($measureToolPath, 'panel', $bmpPath, '--panel-mode', $shotPanelMode)
            $panelOutput = & py -3 @panelArgs 2>&1
            $panelOutput | Out-File (Join-Path $outDir ("panel-{0}.txt" -f $shotName)) -Encoding utf8
            if ($LASTEXITCODE -ne 0) {
                $problemList.Add("panel check failed: $bmpName (see panel-$shotName.txt)")
            }
        }
    }
}
finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
    }
    Copy-Item $logPath (Join-Path $outDir '01-run.final.log') -Force -ErrorAction SilentlyContinue
}

$summaryPath = Join-Path $outDir 'summary.txt'
if ($problemList.Count -eq 0) {
    "PASS: $($Shots.Count) shot(s) captured, panel checks ok" | Set-Content $summaryPath
    Write-Host "[run_paper_case] PASS -> $outDir"
    exit 0
}

"FAIL:" | Set-Content $summaryPath
$problemList | ForEach-Object { "  - $_" | Add-Content $summaryPath }
Write-Host "[run_paper_case] FAIL -> $outDir"
$problemList | ForEach-Object { Write-Host "  - $_" }
exit 1
