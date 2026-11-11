[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$Prefix = "",
    [string]$Config = "",
    [switch]$Check,
    [switch]$Write,
    [switch]$UpdateLock,
    [switch]$NoTouch,
    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repoRoot "ThirdParty\manifest.json"
}
$ManifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
$lockPath = Join-Path $repoRoot "ThirdParty\deps.lock"
$patchRoot = Join-Path $repoRoot "ThirdParty\patches"

function Write-Fail([string]$Message) {
    Write-Host ("deps-stamp: " + $Message) -ForegroundColor Red
    exit 1
}

function Get-Sha256Text([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace "-", "").ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Get-Sha256File([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-RelativePath([string]$BasePath, [string]$Path) {
    $baseFull = [System.IO.Path]::GetFullPath($BasePath).TrimEnd("\", "/")
    $pathFull = [System.IO.Path]::GetFullPath($Path)
    if ($pathFull.StartsWith($baseFull + "\", [System.StringComparison]::OrdinalIgnoreCase)) {
        return $pathFull.Substring($baseFull.Length + 1)
    }
    return $pathFull
}

function ConvertTo-JsonString([string]$Value) {
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    foreach ($ch in $Value.ToCharArray()) {
        switch ([int][char]$ch) {
            34 { [void]$builder.Append('\"'); continue }
            92 { [void]$builder.Append('\\'); continue }
            8 { [void]$builder.Append('\b'); continue }
            12 { [void]$builder.Append('\f'); continue }
            10 { [void]$builder.Append('\n'); continue }
            13 { [void]$builder.Append('\r'); continue }
            9 { [void]$builder.Append('\t'); continue }
            default {
                if ([int][char]$ch -lt 32) {
                    [void]$builder.Append(('\u{0:x4}' -f [int][char]$ch))
                }
                else {
                    [void]$builder.Append($ch)
                }
            }
        }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function ConvertTo-DeterministicValue($Value, [int]$Depth) {
    $indent = "  " * $Depth
    $childIndent = "  " * ($Depth + 1)

    if ($null -eq $Value) { return "null" }
    if ($Value -is [bool]) { if ($Value) { return "true" } else { return "false" } }
    if ($Value -is [string]) { return (ConvertTo-JsonString $Value) }
    if ($Value -is [datetime]) {
        return (ConvertTo-JsonString ($Value.ToUniversalTime().ToString("o")))
    }
    if ($Value -is [int] -or $Value -is [long] -or $Value -is [int64] -or
        $Value -is [double] -or $Value -is [decimal] -or $Value -is [single]) {
        return $Value.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    }

    if ($Value -is [System.Collections.IDictionary]) {
        $keys = @($Value.Keys | ForEach-Object { [string]$_ }) | Sort-Object -CaseSensitive
        if ($keys.Count -eq 0) { return "{}" }
        $parts = foreach ($key in $keys) {
            $child = ConvertTo-DeterministicValue $Value[$key] ($Depth + 1)
            $childIndent + (ConvertTo-JsonString $key) + ": " + $child
        }
        return "{`n" + ($parts -join ",`n") + "`n" + $indent + "}"
    }

    if ($Value -is [System.Collections.IEnumerable] -and -not ($Value -is [string])) {
        $items = @($Value)
        if ($items.Count -eq 0) { return "[]" }
        $parts = foreach ($item in $items) {
            $child = ConvertTo-DeterministicValue $item ($Depth + 1)
            $childIndent + $child
        }
        return "[`n" + ($parts -join ",`n") + "`n" + $indent + "]"
    }

    $properties = @($Value.PSObject.Properties | Where-Object { $_.MemberType -in @("NoteProperty", "Property") })
    if ($properties.Count -gt 0) {
        $names = @($properties | ForEach-Object { $_.Name }) | Sort-Object -CaseSensitive
        $parts = foreach ($name in $names) {
            $child = ConvertTo-DeterministicValue $Value.$name ($Depth + 1)
            $childIndent + (ConvertTo-JsonString $name) + ": " + $child
        }
        return "{`n" + ($parts -join ",`n") + "`n" + $indent + "}"
    }

    return (ConvertTo-JsonString $Value.ToString())
}

function ConvertTo-DeterministicJson($Value) {
    return (ConvertTo-DeterministicValue $Value 0)
}

function Read-CMakeCache([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Fail ("CMakeCache.txt not found: " + $Path)
    }
    $map = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line -match '^([A-Za-z0-9_.+\-]+):([A-Za-z]+)=(.*)$') {
            $map[$Matches[1]] = $Matches[3]
        }
    }
    return $map
}

function Get-CacheValue($Cache, [string]$Name, [string]$Default = "") {
    if ($Cache.ContainsKey($Name)) {
        return [string]$Cache[$Name]
    }
    return $Default
}

function Invoke-CompilerText([string]$Compiler, [string[]]$Arguments) {
    try {
        $output = & $Compiler @Arguments 2>&1
        return ($output | Out-String)
    }
    catch {
        return ""
    }
}

function Get-CompilerVersion([string]$Compiler) {
    $text = Invoke-CompilerText $Compiler @("--version")
    if ($text -match '(\d+\.\d+\.\d+)') { return $Matches[1] }
    if ($text -match '(\d+\.\d+)') { return $Matches[1] }
    return "unknown"
}

function Get-CompilerTarget([string]$Compiler) {
    $text = Invoke-CompilerText $Compiler @("-dumpmachine")
    $line = ($text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
    if ($line) { return $line.Trim() }
    return "unknown"
}

function Get-CompilerCrt([string]$Compiler) {
    $binDir = Split-Path -Parent $Compiler
    $runtime = Join-Path $binDir "libstdc++-6.dll"
    $objdump = Join-Path $binDir "objdump.exe"
    if (-not (Test-Path -LiteralPath $objdump)) {
        $command = Get-Command objdump -ErrorAction SilentlyContinue
        if ($command) { $objdump = $command.Source }
    }
    if ((Test-Path -LiteralPath $runtime) -and (Test-Path -LiteralPath $objdump)) {
        try {
            $imports = (& $objdump -p $runtime 2>$null | Out-String)
            if ($imports -match "api-ms-win-crt-") { return "ucrt" }
            if ($imports -match "msvcrt\.dll") { return "msvcrt" }
        }
        catch {
            # fall through to the -v probe
        }
    }
    $verbose = Invoke-CompilerText $Compiler @("-v")
    if ($verbose -match "--with-default-msvcrt=([A-Za-z0-9_]+)") { return $Matches[1].ToLowerInvariant() }
    if ($verbose -match "ucrt") { return "ucrt" }
    return "unknown"
}

function Get-FlatMap($Value, [string]$PathPrefix = "") {
    $map = @{}
    if ($null -eq $Value) {
        $map[$PathPrefix] = "<null>"
        return $map
    }
    if ($Value -is [System.Collections.IDictionary]) {
        foreach ($key in $Value.Keys) {
            $childPrefix = if ($PathPrefix) { $PathPrefix + "." + $key } else { [string]$key }
            $child = Get-FlatMap $Value[$key] $childPrefix
            foreach ($entry in $child.GetEnumerator()) { $map[$entry.Key] = $entry.Value }
        }
        return $map
    }
    $properties = @($Value.PSObject.Properties | Where-Object { $_.MemberType -in @("NoteProperty", "Property") })
    if ($properties.Count -gt 0 -and -not ($Value -is [string]) -and -not ($Value -is [ValueType])) {
        foreach ($property in $properties) {
            $childPrefix = if ($PathPrefix) { $PathPrefix + "." + $property.Name } else { $property.Name }
            $child = Get-FlatMap $property.Value $childPrefix
            foreach ($entry in $child.GetEnumerator()) { $map[$entry.Key] = $entry.Value }
        }
        return $map
    }
    $map[$PathPrefix] = [string]$Value
    return $map
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne "vulkanlearn-deps-manifest/v1") {
    Write-Fail ("unsupported manifest schema: " + $manifest.schema)
}
$manifestText = ConvertTo-DeterministicJson $manifest
$manifestSha256 = Get-Sha256Text $manifestText

$optionProjection = foreach ($dependency in ($manifest.dependencies | Sort-Object -Property name)) {
    [ordered]@{
        name             = $dependency.name
        declaredOptions  = $dependency.cmake.declaredOptions
        featureOptions   = $dependency.cmake.featureOptions
        superbuildRequired = $dependency.cmake.superbuildRequired
    }
}
$optionText = ConvertTo-DeterministicJson @($optionProjection)
$optionSetSha256 = Get-Sha256Text $optionText

$patchEntries = @()
if (Test-Path -LiteralPath $patchRoot) {
    $patchEntries = Get-ChildItem -LiteralPath $patchRoot -Recurse -File |
        Sort-Object -Property FullName |
        ForEach-Object {
            [ordered]@{
                path   = (Get-RelativePath $repoRoot $_.FullName).Replace("\", "/")
                sha256 = Get-Sha256File $_.FullName
                bytes  = $_.Length
            }
        }
}
$patchSetSha256 = Get-Sha256Text (ConvertTo-DeterministicJson @($patchEntries))

# -UpdateLock regenerates ThirdParty/deps.lock from the manifest and patch set.
if ($UpdateLock) {
    $lockBody = [ordered]@{
        schema        = "vulkanlearn-deps-lock/v1"
        lockVersion   = 1
        generatedBy   = "tool/deps-stamp.ps1 -UpdateLock"
        normalization = "UTF-8 without BOM; object keys sorted ordinal; arrays in declared order; 2-space indent; LF"
        manifest      = [ordered]@{
            path   = "ThirdParty/manifest.json"
            schema = $manifest.schema
            sha256 = $manifestSha256
        }
        optionSet     = [ordered]@{
            project    = "dependencies[].cmake.{declaredOptions,featureOptions,superbuildRequired}, sorted by name"
            sha256     = $optionSetSha256
        }
        patches       = [ordered]@{
            root   = "ThirdParty/patches"
            files  = @($patchEntries)
            sha256 = $patchSetSha256
        }
    }
    $lockSha256 = Get-Sha256Text (ConvertTo-DeterministicJson $lockBody)
    $lockBody["lockSha256"] = $lockSha256
    [System.IO.File]::WriteAllText($lockPath, (ConvertTo-DeterministicJson $lockBody) + "`n",
        [System.Text.UTF8Encoding]::new($false))
    Write-Host ("deps-stamp: wrote " + $lockPath)
    Write-Host ("deps-stamp: manifest=" + $manifestSha256)
    Write-Host ("deps-stamp: optionSet=" + $optionSetSha256)
    Write-Host ("deps-stamp: patches=" + $patchSetSha256 + " (" + @($patchEntries).Count + " files)")
    Write-Host ("deps-stamp: lock=" + $lockSha256)
    exit 0
}

if (-not (Test-Path -LiteralPath $lockPath)) {
    Write-Fail "ThirdParty/deps.lock is missing; run tool/deps-stamp.ps1 -UpdateLock"
}
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ($lock.schema -ne "vulkanlearn-deps-lock/v1") {
    Write-Fail ("unsupported lock schema: " + $lock.schema)
}
if ($lock.manifest.sha256 -ne $manifestSha256) {
    Write-Fail ("manifest changed since deps.lock was generated; run tool/deps-stamp.ps1 -UpdateLock (lock=" +
        $lock.manifest.sha256 + " current=" + $manifestSha256 + ")")
}
if ($lock.optionSet.sha256 -ne $optionSetSha256) {
    Write-Fail ("option set changed since deps.lock was generated; run tool/deps-stamp.ps1 -UpdateLock")
}
if ($lock.patches.sha256 -ne $patchSetSha256) {
    Write-Fail ("patch set changed since deps.lock was generated; run tool/deps-stamp.ps1 -UpdateLock")
}

if ($Check -and $Write) {
    Write-Fail "-Check and -Write are mutually exclusive"
}
if (-not $BuildDir) {
    Write-Fail "-BuildDir is required (source of compiler, generator and effective flags)"
}
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$cache = Read-CMakeCache (Join-Path $BuildDir "CMakeCache.txt")

if (-not $Config) {
    $Config = Get-CacheValue $cache "CMAKE_BUILD_TYPE" ""
}
if (-not $Config) {
    Write-Fail "configuration is empty; pass -Config or set CMAKE_BUILD_TYPE in the build tree"
}
if ($Config -notin @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")) {
    Write-Fail ("unsupported configuration: " + $Config)
}

$compiler = Get-CacheValue $cache "CMAKE_CXX_COMPILER" ""
if (-not $compiler) {
    Write-Fail "CMAKE_CXX_COMPILER is missing from CMakeCache.txt"
}
$compiler = $compiler.Replace("/", "\")
$compilerId = Get-CacheValue $cache "CMAKE_CXX_COMPILER_ID" ""
if (-not $compilerId) {
    if ((Split-Path -Leaf $compiler) -match "^(g\+\+|c\+\+|gcc)") { $compilerId = "GNU" }
}
$compilerVersion = Get-CompilerVersion $compiler
$compilerTarget = Get-CompilerTarget $compiler
$crt = "n/a"
if ($compilerId -eq "GNU" -or $compilerTarget -match "mingw") {
    $crt = Get-CompilerCrt $compiler
}

$platform = "win64"
$parsedTarget = $compilerTarget
if ($parsedTarget -match "^(aarch64|arm64)|^(x86_64|amd64)") {
    $platform = if ($parsedTarget -match "^(aarch64|arm64)") { "win-arm64" } else { "win64" }
}

$toolchainId = if ($compilerId -eq "GNU") {
    "win64-mingw-" + $compilerVersion + "-" + $crt
}
else {
    $platform + "-" + $compilerId.ToLowerInvariant() + "-" + $compilerVersion
}

$cmakeVersion = "unknown"
if ($cache.ContainsKey("CMAKE_CACHE_MAJOR_VERSION") -and
    $cache.ContainsKey("CMAKE_CACHE_MINOR_VERSION") -and
    $cache.ContainsKey("CMAKE_CACHE_PATCH_VERSION")) {
    $cmakeVersion = (Get-CacheValue $cache "CMAKE_CACHE_MAJOR_VERSION") + "." +
        (Get-CacheValue $cache "CMAKE_CACHE_MINOR_VERSION") + "." +
        (Get-CacheValue $cache "CMAKE_CACHE_PATCH_VERSION")
}

$flagNames = @(
    "CMAKE_C_FLAGS", ("CMAKE_C_FLAGS_" + $Config),
    "CMAKE_CXX_FLAGS", ("CMAKE_CXX_FLAGS_" + $Config),
    "CMAKE_EXE_LINKER_FLAGS", ("CMAKE_EXE_LINKER_FLAGS_" + $Config),
    "CMAKE_SHARED_LINKER_FLAGS", ("CMAKE_SHARED_LINKER_FLAGS_" + $Config),
    "CMAKE_MODULE_LINKER_FLAGS", ("CMAKE_MODULE_LINKER_FLAGS_" + $Config)
)
$flags = [ordered]@{}
foreach ($name in $flagNames) {
    $flags[$name] = Get-CacheValue $cache $name ""
}

$featureValues = [ordered]@{}
foreach ($feature in $manifest.featureOptions) {
    $featureValues[$feature.name] = Get-CacheValue $cache $feature.name "<unset>"
}

$identity = [ordered]@{
    platform     = $platform
    toolchainId  = $toolchainId
    compiler     = [ordered]@{
        id      = $compilerId
        version = $compilerVersion
        path    = $compiler
        target  = $compilerTarget
        crt     = $crt
    }
    generator    = Get-CacheValue $cache "CMAKE_GENERATOR" "unknown"
    cmakeVersion = $cmakeVersion
    config       = $Config
}

$inputs = [ordered]@{
    flags           = $flags
    featureOptions  = $featureValues
    manifestSha256  = $lock.manifest.sha256
    optionSetSha256 = $lock.optionSet.sha256
    patchSetSha256  = $lock.patches.sha256
    lockSha256      = [string]$lock.lockSha256
}

$identityPayload = [ordered]@{
    identity = $identity
    inputs   = $inputs
}
$identitySha256 = Get-Sha256Text (ConvertTo-DeterministicJson $identityPayload)

if (-not $Prefix) {
    Write-Fail "-Prefix is required"
}
$resolvedPrefix = [System.IO.Path]::GetFullPath($Prefix)
if ($resolvedPrefix.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    Write-Fail ("dependency prefixes are machine-wide and must live outside the repository: " + $resolvedPrefix)
}
$prefixLeaf = Split-Path -Leaf $resolvedPrefix
$prefixToolchain = Split-Path -Leaf (Split-Path -Parent $resolvedPrefix)
if ($prefixLeaf -ne $Config -or $prefixToolchain -ne $toolchainId) {
    Write-Fail ("prefix must be <deps-root>/" + $toolchainId + "/" + $Config + "; got " + $resolvedPrefix)
}

$stampPath = Join-Path $resolvedPrefix "deps-stamp.json"

if (-not $Check -and -not $Write) {
    $preview = [ordered]@{
        schema         = "vulkanlearn-deps-stamp/v1"
        prefix         = $resolvedPrefix
        identity       = $identity
        inputs         = $inputs
        identitySha256 = $identitySha256
    }
    Write-Output (ConvertTo-DeterministicJson $preview)
    exit 0
}

if ($Write) {
    if (-not (Test-Path -LiteralPath $resolvedPrefix)) {
        [void](New-Item -ItemType Directory -Path $resolvedPrefix -Force)
    }
    $now = [datetime]::UtcNow.ToString("o")
    $createdAt = $now
    if (Test-Path -LiteralPath $stampPath) {
        $existing = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
        if ($existing.schema -eq "vulkanlearn-deps-stamp/v1" -and $existing.identitySha256 -eq $identitySha256) {
            $createdAt = [string]$existing.createdAt
        }
    }
    $stamp = [ordered]@{
        schema         = "vulkanlearn-deps-stamp/v1"
        prefix         = $resolvedPrefix
        createdAt      = $createdAt
        lastUsed       = $now
        identity       = $identity
        inputs         = $inputs
        identitySha256 = $identitySha256
    }
    [System.IO.File]::WriteAllText($stampPath, (ConvertTo-DeterministicJson $stamp) + "`n",
        [System.Text.UTF8Encoding]::new($false))
    Write-Host ("deps-stamp: wrote " + $stampPath)
    Write-Host ("deps-stamp: toolchain=" + $toolchainId + " config=" + $Config)
    Write-Host ("deps-stamp: identity=" + $identitySha256)
    exit 0
}

if (-not (Test-Path -LiteralPath $stampPath)) {
    Write-Fail ("no deps-stamp.json in prefix: " + $resolvedPrefix)
}
$stamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
if ($stamp.schema -ne "vulkanlearn-deps-stamp/v1") {
    Write-Fail ("unsupported stamp schema in " + $stampPath)
}
$storedPayload = [ordered]@{ identity = $stamp.identity; inputs = $stamp.inputs }
$storedHash = Get-Sha256Text (ConvertTo-DeterministicJson $storedPayload)
if ($storedHash -ne [string]$stamp.identitySha256) {
    Write-Host ("deps-stamp: stamp is internally inconsistent; identitySha256 does not cover its own fields: " +
        $stampPath) -ForegroundColor Red
    Write-Host ("  stored=" + $stamp.identitySha256 + " recomputed=" + $storedHash)
    exit 1
}
if ($stamp.identitySha256 -ne $identitySha256) {
    $stampFlat = Get-FlatMap $storedPayload
    $currentFlat = Get-FlatMap $identityPayload
    $differences = @()
    foreach ($key in ($stampFlat.Keys | Sort-Object)) {
        if (-not $currentFlat.ContainsKey($key)) {
            $differences += ("  " + $key + ": stamp=" + $stampFlat[$key] + " current=<missing>")
        }
        elseif ($stampFlat[$key] -ne $currentFlat[$key]) {
            $differences += ("  " + $key + ": stamp=" + $stampFlat[$key] + " current=" + $currentFlat[$key])
        }
    }
    foreach ($key in ($currentFlat.Keys | Sort-Object)) {
        if (-not $stampFlat.ContainsKey($key)) {
            $differences += ("  " + $key + ": stamp=<missing> current=" + $currentFlat[$key])
        }
    }
    Write-Host ("deps-stamp: mismatch in " + $stampPath) -ForegroundColor Red
    $differences | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
    if ($differences.Count -gt 40) {
        Write-Host ("  ... and " + ($differences.Count - 40) + " more")
    }
    exit 1
}

if (-not $NoTouch) {
    $stamp.lastUsed = [datetime]::UtcNow.ToString("o")
    [System.IO.File]::WriteAllText($stampPath, (ConvertTo-DeterministicJson $stamp) + "`n",
        [System.Text.UTF8Encoding]::new($false))
}
Write-Host ("deps-stamp: OK " + $toolchainId + " " + $Config + " " + $identitySha256)
exit 0
