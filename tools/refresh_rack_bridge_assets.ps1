param(
    [string]$OutputRoot = "",
    # Only refresh modules whose bridge DLL exists (skip the ~800 failed
    # manifests that will never load).  Default on -- that is the useful set.
    [bool]$LoadableOnly = $true
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'rack_bridge_layout.ps1')
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $projectRoot 'modules' }
$outputRootPath = (Resolve-Path -LiteralPath $OutputRoot).Path

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

function Get-RelativeProjectPath([string]$BasePath, [string]$TargetPath) {
    $baseUri = [Uri]::new(($BasePath.TrimEnd('\', '/') + '\'))
    $targetUri = [Uri]::new($TargetPath)
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString()).Replace('\', '/')
}

function Find-PluginRoot([string]$SourcePath) {
    $directory = Split-Path -Parent $SourcePath
    while (-not [string]::IsNullOrWhiteSpace($directory)) {
        if (Test-Path -LiteralPath (Join-Path $directory 'plugin.json')) { return $directory }
        $parent = Split-Path -Parent $directory
        if ($parent -eq $directory) { break }
        $directory = $parent
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

$updated = 0
$missing = 0
$skipped = 0
Get-ChildItem -LiteralPath $outputRootPath -Recurse -Filter bridge_module.json -File | ForEach-Object {
    $manifestPath = $_.FullName
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($LoadableOnly) {
        if ([string]::IsNullOrWhiteSpace($manifest.library) -or
            -not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $manifestPath) $manifest.library))) {
            ++$skipped
            return
        }
    }
    if ([string]::IsNullOrWhiteSpace($manifest.source) -or -not (Test-Path -LiteralPath $manifest.source)) {
        ++$missing
        return
    }

    $source = Get-Content -LiteralPath $manifest.source -Raw
    $pluginRoot = Find-PluginRoot $manifest.source
    $panelAsset = Find-PanelAsset $pluginRoot $manifest.moduleSlug $manifest.moduleType
    if ([string]::IsNullOrWhiteSpace($panelAsset)) { $panelAsset = Get-PanelAsset $source }
    $panelWidthPx = 0.0
    $panelHeightPx = 0.0
    if (-not [string]::IsNullOrWhiteSpace($panelAsset) -and -not [string]::IsNullOrWhiteSpace($pluginRoot)) {
        $panelSvgSize = Get-RackSvgSizePx (Join-Path $pluginRoot $panelAsset)
        if ($panelSvgSize) {
            $panelWidthPx = $panelSvgSize.width
            $panelHeightPx = $panelSvgSize.height
        }
    }
    $panelLayout = Get-RackBridgeLayout (Get-RackBridgeLayoutSource $manifest.source) $manifest.moduleType `
        $pluginRoot '' $panelWidthPx $panelHeightPx
    $manifest | Add-Member -NotePropertyName panelAsset -NotePropertyValue $panelAsset -Force
    Add-RackBridgeLayoutMembers $manifest $panelLayout
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    ++$updated
}

Write-Host "Refreshed panel assets for $updated bridge manifests; $missing sources unavailable; $skipped not loadable."
