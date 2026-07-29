param(
    [string]$PluginsRoot = "",
    [string]$OutputRoot = "",
    [string]$CMake = "cmake",
    [ValidateRange(64, 4096)][int]$MaxDimension = 4096,
    [switch]$SkipImport,
    [ValidateRange(1, 500)][int]$BatchSize = 48,
    [ValidateRange(0, 10000)][int]$MaxBatches = 0,
    [switch]$RetryKnownFailures,
    [switch]$FailOnBridgeErrors
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($PluginsRoot)) { $PluginsRoot = Join-Path $projectRoot 'vendor\Cardinal\plugins' }
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $projectRoot 'modules' }

$builder = Join-Path $PSScriptRoot 'build_cardinal_modules.ps1'
$pluginsRootPath = (Resolve-Path -LiteralPath $PluginsRoot).Path
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

function Get-BridgeOutputPath([string]$PluginSlug, [string]$ModuleSlug) {
    $safePluginSlug = ($PluginSlug -replace '[^A-Za-z0-9_.-]', '_')
    $safeModuleSlug = ($ModuleSlug -replace '[^A-Za-z0-9_.-]', '_')
    return Join-Path (Join-Path $OutputRoot $safePluginSlug) $safeModuleSlug
}

function Test-CompletedBridge([string]$BridgeOutputPath) {
    $manifestPath = Join-Path $BridgeOutputPath 'bridge_module.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { return $false }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        return -not [string]::IsNullOrWhiteSpace($manifest.library) -and
            (Test-Path -LiteralPath (Join-Path $BridgeOutputPath $manifest.library))
    }
    catch { return $false }
}

function Get-PendingBridgeCount {
    $pending = 0
    Get-ChildItem -LiteralPath $pluginsRootPath -Directory | ForEach-Object {
        if ($_.Name -eq 'Fundamental') { return }
        $pluginJson = Join-Path $_.FullName 'plugin.json'
        if (-not (Test-Path -LiteralPath $pluginJson)) { return }
        $plugin = Get-Content -LiteralPath $pluginJson -Raw | ConvertFrom-Json
        foreach ($module in @($plugin.modules)) {
            $bridgeOutputPath = Get-BridgeOutputPath $plugin.slug $module.slug
            if (Test-CompletedBridge $bridgeOutputPath) { continue }
            if (-not $RetryKnownFailures -and
                (Test-Path -LiteralPath (Join-Path $bridgeOutputPath 'bridge_error.json'))) { continue }
            ++$pending
        }
    }
    return $pending
}

if (-not $SkipImport) {
    & $builder -PluginsRoot $PluginsRoot -OutputRoot $OutputRoot -All -CMake $CMake -MaxDimension $MaxDimension
    if (-not $?) { throw 'Cardinal module import failed.' }
}

$batch = 0
while ($true) {
    $pending = Get-PendingBridgeCount
    if ($pending -eq 0) { break }
    if ($MaxBatches -gt 0 -and $batch -ge $MaxBatches) { break }

    ++$batch
    $report = Join-Path $OutputRoot ("bridge-report-batch-{0:D4}.json" -f $batch)
    & $builder -PluginsRoot $PluginsRoot -OutputRoot $OutputRoot -All -BridgeAll `
        -BridgeReport $report -CMake $CMake -MaxDimension $MaxDimension `
        -MaxBridgeModules $BatchSize -SkipImport -SkipCompletedBridges `
        -SkipKnownBridgeFailures:(-not $RetryKnownFailures) -FailOnBridgeErrors:$FailOnBridgeErrors
    if (-not $?) { throw 'Cardinal bridge batch failed.' }
    Write-Host "Cardinal bridge batch $batch complete; $(Get-PendingBridgeCount) modules remain pending."
}

Write-Host "Cardinal bridge sweep complete: $(Get-PendingBridgeCount) modules remain pending."
