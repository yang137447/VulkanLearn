[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ZipPath,
    [string]$UnrealEditor = 'D:\sofeware\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe',
    [string]$ResourceRoot = '',
    [switch]$KeepExtracted
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$zip = (Resolve-Path $ZipPath).Path
$editor = (Resolve-Path $UnrealEditor).Path
$config = Get-Content (Join-Path $root 'config/config.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$resource = if ($ResourceRoot) {
    (Resolve-Path $ResourceRoot).Path
} else {
    (Resolve-Path $config.resourcePath).Path
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$work = Join-Path ([IO.Path]::GetTempPath()) "vl-assets-preview-$stamp"
$extract = Join-Path $work 'extracted'
$export = Join-Path $work 'export'

New-Item -ItemType Directory -Path $extract, $export -Force | Out-Null
try {
    Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
    $uprojects = @(Get-ChildItem -LiteralPath $extract -Filter '*.uproject' -File -Recurse)
    if ($uprojects.Count -ne 1) {
        throw "Expected exactly one .uproject in the archive, found $($uprojects.Count)."
    }
    $uproject = $uprojects[0].FullName

    $script = Join-Path $root 'tool/lookdev/export_ue_assets.py'
    $env:VL_UE_EXPORT_ROOT = $export
    $ueArguments = @(
        $uproject,
        "-ExecutePythonScript=$script",
        '-unattended',
        '-nop4',
        '-nosplash',
        '-NoSound',
        '-stdout',
        '-FullStdOutLogOutput'
    )
    Write-Host "Exporting UE assets with $editor"
    $ueLog = Join-Path $work 'unreal-editor.stdout.log'
    $ueErrorLog = Join-Path $work 'unreal-editor.stderr.log'
    $ueProcess = Start-Process -FilePath $editor `
        -WorkingDirectory (Split-Path $uproject -Parent) `
        -ArgumentList $ueArguments `
        -RedirectStandardOutput $ueLog `
        -RedirectStandardError $ueErrorLog `
        -PassThru
    $ueProcess | Wait-Process
    $ueExitCode = $ueProcess.ExitCode
    $ueProcess.Dispose()
    $manifestPath = Join-Path $export 'manifest.json'
    $exportCompleted = Test-Path -LiteralPath $manifestPath
    if (-not $exportCompleted) {
        throw "UnrealEditor exited with code $ueExitCode without producing manifest.json. See $ueLog"
    }
    if ($ueExitCode -ne 0) {
        Write-Warning "UnrealEditor returned $ueExitCode after writing manifest.json; continuing with the completed export."
    }

    Write-Host 'Importing exported assets into the VulkanLearn resource root'
    & py -3 (Join-Path $root 'tool/lookdev/import_ue_export.py') `
        --export-root $export --resource-root $resource
    if ($LASTEXITCODE -ne 0) {
        throw "import_ue_export.py exited with code $LASTEXITCODE."
    }

    Write-Host 'Generating the article-composition lookdev scene'
    & py -3 (Join-Path $root 'tool/lookdev/build_ue_preview_scene.py') `
        --resource-root $resource
    if ($LASTEXITCODE -ne 0) {
        throw "build_ue_preview_scene.py exited with code $LASTEXITCODE."
    }

    Write-Host ''
    Write-Host 'UE lookdev conversion complete.'
    Write-Host "Article scene: $resource\Maps\SC_lookdev_article\SC_lookdev_article.json"
    Write-Host "Import manifest: $resource\Generated\Import\ue_assets_preview_import.json"
}
finally {
    Remove-Item Env:VL_UE_EXPORT_ROOT -ErrorAction SilentlyContinue
    if (-not $KeepExtracted -and (Test-Path -LiteralPath $work)) {
        $removed = $false
        for ($attempt = 0; $attempt -lt 5 -and -not $removed; $attempt++) {
            try {
                Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction Stop
                $removed = $true
            } catch {
                Start-Sleep -Milliseconds 500
            }
        }
        if (-not $removed) {
            Write-Warning "Temporary files could not be removed; kept at: $work"
        }
    } elseif (Test-Path -LiteralPath $work) {
        Write-Host "Temporary conversion files kept at: $work"
    }
}
