param(
    [string]$PluginsRoot = "",
    [string]$OutputRoot = "",
    [string[]]$Plugin = @(),
    [switch]$All,
    [switch]$IncludeFundamental,
    [Alias('BridgeModule')][string[]]$CompileBridge = @(),
    [switch]$BridgeAll,
    [string[]]$BridgeCategory = @(),
    [ValidateRange(0, 10000)][int]$MaxBridgeModules = 0,
    [string]$BridgeReport = "",
    [switch]$SkipImport,
    [switch]$SkipCompletedBridges,
    [switch]$SkipKnownBridgeFailures,
    [switch]$FailOnBridgeErrors,
    [string]$CMake = "cmake",
    [ValidateRange(64, 4096)][int]$MaxDimension = 4096
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($PluginsRoot)) { $PluginsRoot = Join-Path $projectRoot 'vendor\Cardinal\plugins' }
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $projectRoot 'modules' }
$pluginsRootPath = (Resolve-Path -LiteralPath $PluginsRoot).Path
$importer = Join-Path $PSScriptRoot 'import_vcv_rack_plugin.ps1'
$bridgeCompiler = Join-Path $PSScriptRoot 'compile_rack_sdk_bridge_module.ps1'
$packer = Join-Path $projectRoot 'build\cardinal_svg_packer.exe'
if (-not (Test-Path -LiteralPath $packer)) { throw "Build cardinal_svg_packer before importing modules: $packer" }
if (($CompileBridge.Count -gt 0 -or $BridgeAll) -and -not (Test-Path -LiteralPath $bridgeCompiler)) {
    throw "Missing Rack SDK bridge compiler: $bridgeCompiler"
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$available = Get-ChildItem -LiteralPath $pluginsRootPath -Directory | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.FullName 'plugin.json')
}
if ($All) {
    $Plugin = @($available.Name | Where-Object { $IncludeFundamental -or $_ -ne 'Fundamental' })
}
if ($Plugin.Count -eq 0) { throw 'Specify -Plugin <slug> or use -All.' }
$bridgeFailures = @()
$bridgeScheduled = 0

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

function Write-BridgeFailure([string]$BridgeOutputPath, [string]$PluginSlug,
                             [string]$ModuleSlug, [string]$Message) {
    New-Item -ItemType Directory -Force -Path $BridgeOutputPath | Out-Null
    [PSCustomObject]@{
        plugin = $PluginSlug
        module = $ModuleSlug
        error = $Message
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $BridgeOutputPath 'bridge_error.json') -Encoding utf8
}

foreach ($slug in $Plugin) {
    if (-not $IncludeFundamental -and $slug -eq 'Fundamental') {
        Write-Host 'Skipping Fundamental; it is bundled by the main build. Use -IncludeFundamental to import it into modules/.'
        continue
    }
    $pluginRoot = Join-Path $pluginsRootPath $slug
    if (-not (Test-Path -LiteralPath (Join-Path $pluginRoot 'plugin.json'))) {
        throw "Unknown Cardinal plugin: $slug"
    }
    if (-not $SkipImport) {
        & $importer -PluginRoot $pluginRoot -OutputRoot $OutputRoot -Packer $packer -MaxDimension $MaxDimension
        if (-not $?) { throw "Import failed: $slug" }
    }

    $pluginInfo = Get-Content -LiteralPath (Join-Path $pluginRoot 'plugin.json') -Raw | ConvertFrom-Json
    $bridgeRequests = @($CompileBridge)
    if ($BridgeAll) {
        foreach ($module in @($pluginInfo.modules)) {
            $tags = @($module.tags)
            if ($BridgeCategory.Count -gt 0 -and @($tags | Where-Object { $BridgeCategory -contains $_ }).Count -eq 0) {
                continue
            }
            $bridgeOutputPath = Get-BridgeOutputPath $pluginInfo.slug $module.slug
            if ($SkipCompletedBridges -and (Test-CompletedBridge $bridgeOutputPath)) { continue }
            if ($SkipKnownBridgeFailures -and (Test-Path -LiteralPath (Join-Path $bridgeOutputPath 'bridge_error.json'))) { continue }
            if ($MaxBridgeModules -gt 0 -and $bridgeScheduled -ge $MaxBridgeModules) { break }
            $bridgeRequests += "$slug/$($module.slug)"
            ++$bridgeScheduled
        }
    }
    $bridgeRequests = @($bridgeRequests | Select-Object -Unique)
    if ($bridgeRequests.Count -eq 0) { continue }

    $sourceModels = @{}
    $pluginSourceTexts = [System.Collections.Generic.List[object]]::new()
    foreach ($sourceFile in Get-ChildItem -LiteralPath (Join-Path $pluginRoot 'src') -Recurse -File -Include *.cpp,*.cc,*.cxx,*.hpp,*.h,*.hh) {
        $contents = Get-Content -LiteralPath $sourceFile.FullName -Raw
        if ([string]::IsNullOrWhiteSpace($contents)) { continue }
        [void]$pluginSourceTexts.Add([PSCustomObject]@{ path = $sourceFile.FullName; text = $contents })
    }
    foreach ($sourceEntry in $pluginSourceTexts) {
        foreach ($match in [regex]::Matches($sourceEntry.text,
            'createModel\s*<\s*([^,>]+)\s*,\s*[^>]+>\s*\(\s*(?:"([^"]+)"|([A-Za-z_][A-Za-z0-9_]*))\s*\)')) {
            $moduleSlugValue = $match.Groups[2].Value
            if ([string]::IsNullOrWhiteSpace($moduleSlugValue) -and $match.Groups[3].Success) {
                # slug via identifier (e.g. createModel<...>(SlugFolding)): resolve
                # its string value anywhere in the plugin sources
                $slugName = [regex]::Escape($match.Groups[3].Value)
                foreach ($lookupEntry in $pluginSourceTexts) {
                    $lookup = [regex]::Match($lookupEntry.text,
                        "(?:#define\s+$slugName\s+|$slugName(?:\s*\[\s*\])?\s*=\s*)""([^""]+)""")
                    if ($lookup.Success) { $moduleSlugValue = $lookup.Groups[1].Value; break }
                }
            }
            if ([string]::IsNullOrWhiteSpace($moduleSlugValue)) { continue }
            if ($sourceModels.ContainsKey($moduleSlugValue)) { continue }
            $sourceModels[$moduleSlugValue] = [PSCustomObject]@{
                source = $sourceEntry.path
                moduleType = $match.Groups[1].Value.Trim()
            }
        }
    }

    foreach ($bridgeRequest in $bridgeRequests) {
        $parts = $bridgeRequest -split '[/\\:]', 2
        $moduleSlug = if ($parts.Count -eq 2) {
            if ($parts[0] -ne $slug) { continue }
            $parts[1]
        } else {
            if ($All) { throw "Use Plugin/Module with -All: $bridgeRequest" }
            $parts[0]
        }
        if ([string]::IsNullOrWhiteSpace($moduleSlug)) { throw "Invalid bridge module selector: $bridgeRequest" }

        $sourceInfo = $sourceModels[$moduleSlug]
        if (-not $sourceInfo) {
            $message = "Could not find createModel registration for $slug/$moduleSlug"
            if (-not $BridgeAll) { throw $message }
            $bridgeFailures += [PSCustomObject]@{ plugin = $slug; module = $moduleSlug; error = $message }
            Write-BridgeFailure (Get-BridgeOutputPath $pluginInfo.slug $moduleSlug) $slug $moduleSlug $message
            Write-Warning $message
            continue
        }

        try {
            & $bridgeCompiler -PluginRoot $pluginRoot -Source $sourceInfo.source -ModuleType $sourceInfo.moduleType `
                -ModuleSlug $moduleSlug -OutputRoot $OutputRoot -Packer $packer -CMake $CMake -MaxDimension $MaxDimension
            if (-not $?) { throw "Rack SDK bridge compilation failed: $slug/$moduleSlug" }
            Remove-Item -LiteralPath (Join-Path (Get-BridgeOutputPath $pluginInfo.slug $moduleSlug) 'bridge_error.json') `
                -Force -ErrorAction SilentlyContinue
        }
        catch {
            if (-not $BridgeAll) { throw }
            $bridgeFailures += [PSCustomObject]@{ plugin = $slug; module = $moduleSlug; error = $_.Exception.Message }
            Write-BridgeFailure (Get-BridgeOutputPath $pluginInfo.slug $moduleSlug) $slug $moduleSlug $_.Exception.Message
            Write-Warning "Rack SDK bridge failed for ${slug}/${moduleSlug}: $($_.Exception.Message)"
        }
    }
}

if ($BridgeAll) {
    if ([string]::IsNullOrWhiteSpace($BridgeReport)) { $BridgeReport = Join-Path $OutputRoot 'bridge-report.json' }
    $bridgeFailures | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $BridgeReport -Encoding utf8
    Write-Host "Rack SDK bridge report: $BridgeReport"
    Write-Host "Rack SDK bridge summary: $bridgeScheduled attempted, $($bridgeFailures.Count) require compatibility work"
    if ($FailOnBridgeErrors -and $bridgeFailures.Count -gt 0) {
        throw "$($bridgeFailures.Count) Rack SDK bridge module(s) require compatibility work."
    }
}
