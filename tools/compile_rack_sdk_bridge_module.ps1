param(
    [Parameter(Mandatory = $true)][string]$PluginRoot,
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$ModuleType,
    [string]$ModuleSlug = "",
    [string]$OutputRoot = "",
    [string]$BoundaryPattern = "",
    [string]$Packer = "",
    [string]$CMake = "cmake",
    [ValidateRange(64, 4096)][int]$MaxDimension = 4096
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -ge 7) { $PSNativeCommandUseErrorActionPreference = $false }
. (Join-Path $PSScriptRoot 'rack_bridge_layout.ps1')

function Invoke-NativeTool([scriptblock]$Command, [string]$LogPath = '') {
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = [System.Collections.Generic.List[string]]::new()
        & $Command 2>&1 | ForEach-Object {
            Write-Host $_
            [void]$lines.Add([string]$_)
        }
        if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
            $lines | Set-Content -LiteralPath $LogPath -Encoding utf8
        }
        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function To-CMakePath([string]$Path) {
    return $Path.Replace('\', '/')
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
    return (Get-RelativeProjectPath $PluginRoot $best.path)
}

function Get-RelativeProjectPath([string]$BasePath, [string]$TargetPath) {
    $baseUri = [Uri]::new(($BasePath.TrimEnd('\', '/') + '\'))
    $targetUri = [Uri]::new($TargetPath)
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString()).Replace('\', '/')
}

function Resolve-PanelAsset([string]$Contents, [string]$PluginRoot,
                            [string]$ModuleSlug, [string]$ModuleType) {
    $fallback = Find-PanelAsset $PluginRoot $ModuleSlug $ModuleType
    if (-not [string]::IsNullOrWhiteSpace($fallback)) { return $fallback }
    return Get-PanelAsset $Contents
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$pluginRootPath = (Resolve-Path -LiteralPath $PluginRoot).Path
$sourcePath = if ([IO.Path]::IsPathRooted($Source)) { $Source } else { Join-Path $pluginRootPath $Source }
$sourcePath = (Resolve-Path -LiteralPath $sourcePath).Path
$pluginJsonPath = Join-Path $pluginRootPath 'plugin.json'
if (-not (Test-Path -LiteralPath $pluginJsonPath)) { throw "Missing plugin.json: $pluginJsonPath" }

$plugin = Get-Content -LiteralPath $pluginJsonPath -Raw | ConvertFrom-Json
$safePlugin = ($plugin.slug -replace '[^A-Za-z0-9_.-]', '_')
$safeType = ($ModuleType -replace '[^A-Za-z0-9_.-]', '_')
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $projectRoot 'modules' }
$pluginOutputPath = Join-Path $OutputRoot $safePlugin
if ([string]::IsNullOrWhiteSpace($ModuleSlug)) { $ModuleSlug = $ModuleType }
$safeModuleSlug = ($ModuleSlug -replace '[^A-Za-z0-9_.-]', '_')
$outputPath = Join-Path $pluginOutputPath $safeModuleSlug
New-Item -ItemType Directory -Force -Path $pluginOutputPath | Out-Null
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

if ([string]::IsNullOrWhiteSpace($Packer)) { $Packer = Join-Path $projectRoot 'build\cardinal_svg_packer.exe' }
if (-not (Test-Path -LiteralPath $Packer)) { throw "Build cardinal_svg_packer before compiling bridge modules: $Packer" }

$assetPack = Join-Path $pluginOutputPath "$safePlugin.s24svgpak"
if (-not (Test-Path -LiteralPath $assetPack)) {
    $exitCode = Invoke-NativeTool { & $Packer $pluginRootPath $assetPack $MaxDimension }
    if ($exitCode -ne 0) { throw "SVG bridge build failed with exit code $exitCode" }
}

$content = Get-Content -LiteralPath $sourcePath -Raw
$panelAsset = Resolve-PanelAsset $content $pluginRootPath $ModuleSlug $ModuleType

# 1:1 layout: the panel SVG is the size authority; widget sizes come from the
# component-library / plugin SVGs resolved inside Get-RackBridgeLayout.
$panelWidthPx = 0.0
$panelHeightPx = 0.0
if (-not [string]::IsNullOrWhiteSpace($panelAsset)) {
    $panelSvgSize = Get-RackSvgSizePx (Join-Path $pluginRootPath $panelAsset)
    if ($panelSvgSize) {
        $panelWidthPx = $panelSvgSize.width
        $panelHeightPx = $panelSvgSize.height
    }
}
$panelLayout = Get-RackBridgeLayout (Get-RackBridgeLayoutSource $sourcePath) $ModuleType `
    $pluginRootPath '' $panelWidthPx $panelHeightPx

# Keep #include directives that resolve to files inside the plugin (shared DSP
# helpers, module structs declared in sibling headers).  Strip everything else:
# Rack SDK headers come from the compat layer, std headers from the preamble.
$sourceDir = Split-Path -Parent $sourcePath
$pluginSrcDir = Join-Path $pluginRootPath 'src'
$includeBases = @($sourceDir, $pluginSrcDir, $pluginRootPath) | Where-Object { Test-Path -LiteralPath $_ }
$content = [regex]::Replace($content, '(?m)^[ \t]*#[ \t]*include[ \t]*([<"])([^">\r\n]+)[">][^\r\n]*(\r?\n)?', {
    param($match)
    $target = $match.Groups[2].Value
    foreach ($base in $includeBases) {
        try {
            if (Test-Path -LiteralPath (Join-Path $base $target)) { return $match.Value }
        }
        catch {}
    }
    return ''
}.GetNewClosure())
$content = [regex]::Replace($content,
    '(ParamQuantity\*\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*)paramQuantities\[([^\]]+)\]',
    '$1&paramQuantities[$2]')
$content = [regex]::Replace($content, 'paramQuantities\[([^\]]+)\]->', 'paramQuantities[$1].')
if ([string]::IsNullOrWhiteSpace($BoundaryPattern)) {
    # First widget/GUI definition marks the end of the DSP text.  Requires an
    # inheritance clause or opening brace so forward declarations inside the
    # DSP region do not cut early.
    $BoundaryPattern = '(?m)^(?:struct|class)\s+[A-Za-z_][A-Za-z0-9_]*(?:Widget|Display|Knob|Port|Button|Meter|Panel|Screw|Slider|Switch)\s*(?::|\{|$)'
}
$boundary = [regex]::Match($content, $BoundaryPattern)
if (-not $boundary.Success) { throw "No DSP/UI boundary found in $sourcePath. Supply -BoundaryPattern for this module." }
$dsp = $content.Substring(0, $boundary.Index)
# Close scopes the cut left open (modules wrapped in their own namespace)
$openBraces = [regex]::Matches($dsp, '\{').Count
$closeBraces = [regex]::Matches($dsp, '\}').Count
if ($openBraces -gt $closeBraces) {
    $dsp += "`n" + ('}' * ($openBraces - $closeBraces)) + " // PatchKnob: close scopes cut at UI boundary`n"
}

# Definitions for extern Model*/pluginInstance declarations that plugin-local
# headers bring in.  Null models are safe: expander checks short-circuit on the
# module pointer before comparing models.
$stubNames = [System.Collections.Generic.SortedSet[string]]::new()
$stubScanTexts = [System.Collections.Generic.List[string]]::new()
[void]$stubScanTexts.Add($dsp)
if (Test-Path -LiteralPath $pluginSrcDir) {
    foreach ($headerFile in Get-ChildItem -LiteralPath $pluginSrcDir -Recurse -File -ErrorAction SilentlyContinue |
             Where-Object { $_.Extension -in @('.h', '.hpp', '.hh') -and $_.Length -lt 512KB }) {
        [void]$stubScanTexts.Add((Get-Content -LiteralPath $headerFile.FullName -Raw))
    }
}
foreach ($scanText in $stubScanTexts) {
    foreach ($externMatch in [regex]::Matches($scanText,
        '(?m)^\s*extern\s+(?:rack::)?(?:plugin::)?Model\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*;')) {
        [void]$stubNames.Add($externMatch.Groups[1].Value)
    }
}
$stubDefinitions = [System.Text.StringBuilder]::new()
[void]$stubDefinitions.AppendLine('rack::plugin::Plugin* pluginInstance = nullptr;')
foreach ($stubName in $stubNames) {
    [void]$stubDefinitions.AppendLine("rack::plugin::Model* $stubName = nullptr;")
}

$bridgeSource = @"
#include "rack_sdk_compat.hpp"

// std preamble: replaces the module's stripped system includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace rack;

$dsp

$($stubDefinitions.ToString())

#if defined(_WIN32)
#define S24_RACK_EXPORT __declspec(dllexport)
#else
#define S24_RACK_EXPORT __attribute__((visibility("default")))
#endif

extern "C" S24_RACK_EXPORT std::uint32_t PatchKnob_rack_bridge_api_version()
{
    return 1;
}

extern "C" S24_RACK_EXPORT rack::engine::Module* PatchKnob_rack_bridge_create()
{
    return new $ModuleType();
}

extern "C" S24_RACK_EXPORT void PatchKnob_rack_bridge_destroy(rack::engine::Module* module)
{
    delete module;
}
"@
$bridgeSourcePath = Join-Path $outputPath 'bridge_module.cpp'
$bridgeSource | Set-Content -LiteralPath $bridgeSourcePath -Encoding utf8

$rackInclude = To-CMakePath (Join-Path $projectRoot 'src\engine\rack')
$sourceDirInclude = To-CMakePath $sourceDir
$pluginSrcInclude = To-CMakePath $pluginSrcDir
$pluginRootInclude = To-CMakePath $pluginRootPath
$pluginRootDefine = To-CMakePath $pluginRootPath
$target = "PatchKnob_rack_bridge_" + ($safePlugin -replace '[^A-Za-z0-9_]', '_') + "_" + ($safeModuleSlug -replace '[^A-Za-z0-9_]', '_')
# Include order matters: plugin dirs first so a plugin's own headers win, the
# rack replica dir last so <rack.hpp>/<dsp/*.hpp> resolve to the compat shims.
$cmakeText = @"
cmake_minimum_required(VERSION 3.16)
project(PatchKnobRackSdkBridge LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(aux_sources.cmake OPTIONAL)
add_library($target SHARED bridge_module.cpp `$`{S24_AUX_SOURCES})
target_include_directories($target PRIVATE "$sourceDirInclude" "$pluginSrcInclude" "$pluginRootInclude" "$rackInclude")
target_compile_definitions($target PRIVATE "S24_RACK_PLUGIN_ROOT=\"$pluginRootDefine\"")
target_compile_options($target PRIVATE -Wall -msse4.2 -include "$rackInclude/rack_sdk_compat.hpp")
set_target_properties($target PROPERTIES
    PREFIX ""
    OUTPUT_NAME "$safePlugin-$safeModuleSlug"
    RUNTIME_OUTPUT_DIRECTORY "`$`{CMAKE_CURRENT_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "`$`{CMAKE_CURRENT_SOURCE_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "`$`{CMAKE_CURRENT_SOURCE_DIR}")
"@
$cmakePath = Join-Path $outputPath 'CMakeLists.txt'
$cmakeText | Set-Content -LiteralPath $cmakePath -Encoding utf8

$moduleInfo = @($plugin.modules | Where-Object { $_.slug -eq $ModuleSlug } | Select-Object -First 1)[0]
$moduleName = if ($moduleInfo -and -not [string]::IsNullOrWhiteSpace($moduleInfo.name)) { $moduleInfo.name } else { $ModuleSlug }
$moduleTags = if ($moduleInfo) { @($moduleInfo.tags) } else { @() }
$moduleCategory = if ($moduleTags.Count -gt 0) { "$($plugin.name) / $($moduleTags[0])" } else { "$($plugin.name) / Bridge" }
$manifest = [PSCustomObject]@{
    format = 'PatchKnob.rack-sdk-bridge/v1'
    plugin = $plugin.slug
    moduleSlug = $ModuleSlug
    slug = "$($plugin.slug).$ModuleSlug"
    name = $moduleName
    category = $moduleCategory
    moduleType = $ModuleType
    source = $sourcePath
    assetPack = "../$safePlugin.s24svgpak"
    panelAsset = $panelAsset
    panelWidth = $panelLayout.panelWidth
    panelHeight = $panelLayout.panelHeight
    params = @($panelLayout.params)
    inputs = @($panelLayout.inputs)
    outputs = @($panelLayout.outputs)
    lights = @($panelLayout.lights)
    library = "$safePlugin-$safeModuleSlug.dll"
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputPath 'bridge_module.json') -Encoding utf8

$buildPath = Join-Path $outputPath 'build'
$buildLogPath = Join-Path $outputPath 'build.log'
$auxCMakePath = Join-Path $outputPath 'aux_sources.cmake'
Remove-Item -LiteralPath $auxCMakePath -Force -ErrorAction SilentlyContinue
$exitCode = Invoke-NativeTool { & $CMake -S $outputPath -B $buildPath -DCMAKE_BUILD_TYPE=Release } $buildLogPath
if ($exitCode -ne 0) { throw "Bridge CMake configuration failed: $ModuleType" }
$exitCode = Invoke-NativeTool { & $CMake --build $buildPath --config Release } $buildLogPath
if ($exitCode -ne 0) {
    # Multi-TU plugins: DSP helpers live in sibling .cpp files.  On undefined
    # references, retry with the plugin's non-module sources compiled in
    # (skipping createModel registration files and pluginInstance definitions,
    # which would duplicate the bridge stubs).
    $logText = if (Test-Path -LiteralPath $buildLogPath) { Get-Content -LiteralPath $buildLogPath -Raw } else { '' }
    $auxSources = @()
    if ($logText -match 'undefined reference' -and (Test-Path -LiteralPath $pluginSrcDir)) {
        foreach ($candidate in Get-ChildItem -LiteralPath $pluginSrcDir -Recurse -File -ErrorAction SilentlyContinue |
                 Where-Object { $_.Extension -in @('.cpp', '.cc') -and $_.Length -lt 1MB }) {
            if ($candidate.FullName -eq $sourcePath) { continue }
            $candidateText = Get-Content -LiteralPath $candidate.FullName -Raw
            if ($candidateText -match 'createModel\s*<') { continue }
            if ($candidateText -match '(?m)^\s*(?:rack::)?Plugin\s*\*\s*pluginInstance\s*[;=]') { continue }
            if ($candidateText -match '(?m)^\s*void\s+init\s*\(\s*(?:rack::)?(?:plugin::)?Plugin\s*\*') { continue }
            $auxSources += (To-CMakePath $candidate.FullName)
        }
    }
    if ($auxSources.Count -gt 0) {
        $auxList = ($auxSources | ForEach-Object { "    `"$_`"" }) -join "`r`n"
        "set(S24_AUX_SOURCES`r`n$auxList`r`n)" | Set-Content -LiteralPath $auxCMakePath -Encoding utf8
        $exitCode = Invoke-NativeTool { & $CMake -S $outputPath -B $buildPath -DCMAKE_BUILD_TYPE=Release } $buildLogPath
        if ($exitCode -eq 0) {
            $exitCode = Invoke-NativeTool { & $CMake --build $buildPath --config Release } $buildLogPath
        }
    }
    if ($exitCode -ne 0) { throw "Bridge DSP compilation failed: $ModuleType" }
}

Write-Host "Bridge module compiled: $(Join-Path $outputPath "$safePlugin-$safeModuleSlug.dll")"
Write-Host "Bridge assets: $assetPack"
