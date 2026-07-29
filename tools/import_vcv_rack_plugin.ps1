param(
    [Parameter(Mandatory = $true)][string]$PluginRoot,
    [string]$OutputRoot = "",
    [string]$Packer = "",
    [string]$CMake = "cmake",
    [ValidateRange(64, 4096)][int]$MaxDimension = 4096,
    [switch]$SkipAssets,
    [switch]$SkipLibrary
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -ge 7) { $PSNativeCommandUseErrorActionPreference = $false }
. (Join-Path $PSScriptRoot 'rack_bridge_layout.ps1')

function Invoke-NativeTool([scriptblock]$Command) {
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Command 2>&1 | ForEach-Object { Write-Host $_ }
        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function Get-RelativeProjectPath([string]$BasePath, [string]$TargetPath) {
    $baseUri = [Uri]::new(($BasePath.TrimEnd('\', '/') + '\'))
    $targetUri = [Uri]::new($TargetPath)
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString()).Replace('\', '/')
}

function Get-PanelAsset([string]$Contents) {
    $patterns = @(
        '(?s)setPanel\s*\(\s*(?:(?:APP\s*->\s*window\s*->\s*loadSvg|createPanel|Svg::load)\s*\(\s*)?asset::plugin\s*\(\s*pluginInstance\s*,\s*"([^"]+?\.svg)"',
        '(?s)prepareThemes\s*\(\s*asset::plugin\s*\(\s*pluginInstance\s*,\s*"([^"]+?\.svg)"'
    )
    foreach ($pattern in $patterns) {
        $match = [regex]::Match($Contents, $pattern)
        if ($match.Success) { return $match.Groups[1].Value }
    }
    return ''
}

function Find-PanelAsset([string]$PluginRoot, [string]$ModuleSlug, [string]$ModuleType) {
    $resRoot = Join-Path $PluginRoot 'res'
    if (-not (Test-Path -LiteralPath $resRoot)) { return '' }
    $typeName = (($ModuleType -replace '.*::', '') -replace 'Widget.*$', '')
    $names = @($ModuleSlug, $typeName) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    $matches = foreach ($asset in Get-ChildItem -LiteralPath $resRoot -Recurse -Filter *.svg -File) {
        $stem = [IO.Path]::GetFileNameWithoutExtension($asset.Name)
        $score = 0
        foreach ($name in $names) {
            if ($stem -ieq $name) { $score = [Math]::Max($score, 100) }
            elseif ($stem -like "*$name*") { $score = [Math]::Max($score, 80) }
        }
        if ($score -gt 0) {
            [PSCustomObject]@{ score = $score; length = $asset.FullName.Length; path = $asset.FullName }
        }
    }
    $best = $matches | Sort-Object -Property @(
        @{ Expression = 'score'; Descending = $true },
        @{ Expression = 'length'; Ascending = $true }
    ) | Select-Object -First 1
    if (-not $best) { return '' }
    return Get-RelativeProjectPath $PluginRoot $best.path
}

function Resolve-PanelAsset([string]$Contents, [string]$PluginRoot,
                            [string]$ModuleSlug, [string]$ModuleType) {
    $fallback = Find-PanelAsset $PluginRoot $ModuleSlug $ModuleType
    if (-not [string]::IsNullOrWhiteSpace($fallback)) { return $fallback }
    return Get-PanelAsset $Contents
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$pluginRootPath = (Resolve-Path -LiteralPath $PluginRoot).Path
$pluginJsonPath = Join-Path $pluginRootPath 'plugin.json'
if (-not (Test-Path -LiteralPath $pluginJsonPath)) { throw "Missing plugin.json: $pluginJsonPath" }

if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $projectRoot 'modules' }
if ([string]::IsNullOrWhiteSpace($Packer)) { $Packer = Join-Path $projectRoot 'build\cardinal_svg_packer.exe' }
if (-not (Test-Path -LiteralPath $Packer)) { throw "Build cardinal_svg_packer before importing: $Packer" }
if (-not $SkipLibrary -and -not (Get-Command $CMake -ErrorAction SilentlyContinue)) { throw "CMake was not found: $CMake" }

$plugin = Get-Content -LiteralPath $pluginJsonPath -Raw | ConvertFrom-Json
$safeSlug = ($plugin.slug -replace '[^A-Za-z0-9_.-]', '_')
$destination = Join-Path $OutputRoot $safeSlug
New-Item -ItemType Directory -Force -Path $destination | Out-Null

$models = @{}
$sourceFiles = Get-ChildItem -LiteralPath (Join-Path $pluginRootPath 'src') -Recurse -File -Include *.cpp,*.cc,*.cxx
foreach ($sourceFile in $sourceFiles) {
    $contents = Get-Content -LiteralPath $sourceFile.FullName -Raw
    if ([string]::IsNullOrWhiteSpace($contents)) { continue }
    foreach ($match in [regex]::Matches($contents, 'createModel\s*<\s*([^,>]+)\s*,\s*([^>]+)>\s*\(\s*"([^"]+)"\s*\)')) {
        $moduleSlug = $match.Groups[3].Value
        $panelAsset = Resolve-PanelAsset $contents $pluginRootPath $moduleSlug $match.Groups[1].Value.Trim()
        $panelWidthPx = 0.0
        $panelHeightPx = 0.0
        if (-not [string]::IsNullOrWhiteSpace($panelAsset)) {
            $panelSvgSize = Get-RackSvgSizePx (Join-Path $pluginRootPath $panelAsset)
            if ($panelSvgSize) {
                $panelWidthPx = $panelSvgSize.width
                $panelHeightPx = $panelSvgSize.height
            }
        }
        $layout = Get-RackBridgeLayout (Get-RackBridgeLayoutSource $sourceFile.FullName) $match.Groups[1].Value.Trim() `
            $pluginRootPath '' $panelWidthPx $panelHeightPx
        $models[$match.Groups[3].Value] = [PSCustomObject]@{
            source = Get-RelativeProjectPath $pluginRootPath $sourceFile.FullName
            dspType = $match.Groups[1].Value.Trim()
            widgetType = $match.Groups[2].Value.Trim()
            panelAsset = $panelAsset
            panelWidth = $layout.panelWidth
            panelHeight = $layout.panelHeight
            params = @($layout.params)
            inputs = @($layout.inputs)
            outputs = @($layout.outputs)
            lights = @($layout.lights)
        }
    }
}

$modules = @()
foreach ($module in $plugin.modules) {
    $model = $models[$module.slug]
    $modules += [PSCustomObject]@{
        slug = $module.slug
        name = $module.name
        description = $module.description
        category = if ($module.tags.Count) { $module.tags[0] } else { 'Utility' }
        source = if ($model) { $model.source } else { '' }
        dspType = if ($model) { $model.dspType } else { '' }
        widgetType = if ($model) { $model.widgetType } else { '' }
        panelAsset = if ($model) { $model.panelAsset } else { '' }
        panelWidth = if ($model) { $model.panelWidth } else { 0 }
        panelHeight = if ($model) { $model.panelHeight } else { 380 }
        params = if ($model) { @($model.params) } else { @() }
        inputs = if ($model) { @($model.inputs) } else { @() }
        outputs = if ($model) { @($model.outputs) } else { @() }
        lights = if ($model) { @($model.lights) } else { @() }
        portingState = 'requires-rack-sdk-bridge'
    }
}

$packPath = Join-Path $destination "$safeSlug.s24svgpak"
if (-not $SkipAssets) {
    $exitCode = Invoke-NativeTool { & $Packer $pluginRootPath $packPath $MaxDimension }
    if ($exitCode -ne 0) { throw "SVG packer failed with exit code $exitCode" }
}

$manifest = [PSCustomObject]@{
    format = 'PatchKnob.rack-import/v1'
    plugin = [PSCustomObject]@{
        slug = $plugin.slug
        name = $plugin.name
        version = $plugin.version
        license = $plugin.license
        sourceUrl = $plugin.sourceUrl
    }
    sourceRoot = $pluginRootPath
    assetPack = if ($SkipAssets) { '' } else { Get-RelativeProjectPath $destination $packPath }
    library = "$safeSlug.dll"
    modules = $modules
}
$manifestPath = Join-Path $destination "$safeSlug.s24rack.json"
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8

if (-not $SkipLibrary) {
    $targetName = "PatchKnob_rack_" + ($safeSlug -replace '[^A-Za-z0-9_]', '_')
    $manifestText = Get-Content -LiteralPath $manifestPath -Raw
    $entrySource = @"
#include <cstdint>

#if defined(_WIN32)
#define S24_RACK_EXPORT __declspec(dllexport)
#else
#define S24_RACK_EXPORT __attribute__((visibility("default")))
#endif

namespace {
const char kManifest[] = R"S24RACK(
$manifestText
)S24RACK";
const char kAssetPack[] = "$safeSlug.s24svgpak";
}

extern "C" S24_RACK_EXPORT std::uint32_t PatchKnob_rack_module_api_version()
{
    return 1;
}

extern "C" S24_RACK_EXPORT const char* PatchKnob_rack_module_manifest()
{
    return kManifest;
}

extern "C" S24_RACK_EXPORT const char* PatchKnob_rack_module_asset_pack()
{
    return kAssetPack;
}
"@
    $entryPath = Join-Path $destination 'module_entry.cpp'
    $entrySource | Set-Content -LiteralPath $entryPath -Encoding utf8
    $cmakeSource = @'
cmake_minimum_required(VERSION 3.16)
project(PatchKnobRackModule LANGUAGES CXX)
add_library(@TARGET@ SHARED module_entry.cpp)
target_compile_features(@TARGET@ PRIVATE cxx_std_17)
set_target_properties(@TARGET@ PROPERTIES
    PREFIX ""
    OUTPUT_NAME "@OUTPUT@"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
'@.Replace('@TARGET@', $targetName).Replace('@OUTPUT@', $safeSlug)
    $cmakePath = Join-Path $destination 'CMakeLists.txt'
    $cmakeSource | Set-Content -LiteralPath $cmakePath -Encoding utf8
    $libraryBuild = Join-Path $destination 'build'
    $exitCode = Invoke-NativeTool { & $CMake -S $destination -B $libraryBuild }
    if ($exitCode -ne 0) { throw "Module CMake configuration failed: $safeSlug" }
    $exitCode = Invoke-NativeTool { & $CMake --build $libraryBuild --config Release }
    if ($exitCode -ne 0) { throw "Module library build failed: $safeSlug" }
}

Write-Host "Imported $($modules.Count) $($plugin.name) module descriptors"
Write-Host "Manifest: $manifestPath"
if (-not $SkipAssets) { Write-Host "Assets: $packPath" }
if (-not $SkipLibrary) { Write-Host "Library: $(Join-Path $destination "$safeSlug.dll")" }
