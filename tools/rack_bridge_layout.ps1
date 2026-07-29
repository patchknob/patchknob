# Rack widget-layout extraction: parses ModuleWidget code and emits control
# positions/sizes in Rack px so the SDL renderer reproduces the panel 1:1.
#
# Coordinate system: Rack renders SVGs at 75 DPI, so 1 px == 25.4/75 mm and a
# 3U panel is 380 px tall.  mm2px() therefore multiplies by 75/25.4
# (2.9527559...), NOT 3.0 -- the difference is a visible 1.6% drift.
#
# Widget sizes are measured from the actual SVG assets (Rack component library
# + plugin res/) instead of name heuristics wherever possible.

$script:RackBridgeDataTable = [System.Data.DataTable]::new()
$script:RackMmToPx = 75.0 / 25.4
$script:RackSvgSizeCache = @{}
$script:RackComponentCatalogCache = @{}
$script:RackPluginWidgetCache = @{}

function ConvertTo-RackInvariantString([double]$Value) {
    return $Value.ToString([Globalization.CultureInfo]::InvariantCulture)
}

function Get-RackDefaultSystemRoot {
    return (Join-Path (Split-Path -Parent $PSScriptRoot) 'vendor\Cardinal\src\Rack')
}

# ---- SVG measurement --------------------------------------------------------

function ConvertTo-RackPxFromSvgLength([string]$Number, [string]$Unit) {
    $value = [double]::Parse($Number, [Globalization.CultureInfo]::InvariantCulture)
    switch ($Unit) {
        'mm' { return $value * $script:RackMmToPx }
        'cm' { return $value * 10.0 * $script:RackMmToPx }
        'in' { return $value * 75.0 }
        'pt' { return $value * 75.0 / 72.0 }
        default { return $value }   # px / user units: Rack treats them as px
    }
}

function Get-RackSvgSizePx([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) { return $null }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if ($script:RackSvgSizeCache.ContainsKey($resolved)) { return $script:RackSvgSizeCache[$resolved] }
    $result = $null
    try {
        $text = Get-Content -LiteralPath $resolved -Raw
        $svgTag = [regex]::Match($text, '<svg\b[^>]*>')
        if ($svgTag.Success) {
            $tag = $svgTag.Value
            $widthMatch = [regex]::Match($tag, 'width\s*=\s*"([0-9.]+)\s*([a-z]*)"')
            $heightMatch = [regex]::Match($tag, 'height\s*=\s*"([0-9.]+)\s*([a-z]*)"')
            if ($widthMatch.Success -and $heightMatch.Success) {
                $result = [PSCustomObject]@{
                    width = ConvertTo-RackPxFromSvgLength $widthMatch.Groups[1].Value $widthMatch.Groups[2].Value
                    height = ConvertTo-RackPxFromSvgLength $heightMatch.Groups[1].Value $heightMatch.Groups[2].Value
                }
            }
            if (-not $result) {
                $viewBox = [regex]::Match($tag, 'viewBox\s*=\s*"\s*[0-9.eE+-]+\s+[0-9.eE+-]+\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s*"')
                if ($viewBox.Success) {
                    $result = [PSCustomObject]@{
                        width = [double]::Parse($viewBox.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
                        height = [double]::Parse($viewBox.Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture)
                    }
                }
            }
        }
    }
    catch { $result = $null }
    $script:RackSvgSizeCache[$resolved] = $result
    return $result
}

# ---- widget catalogs: struct name -> {svg size, base class} -----------------

function Get-RackStructSlices([string]$Contents) {
    $slices = @()
    $structMatches = [regex]::Matches($Contents,
        '(?m)^\s*(?:struct|class)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*([^\{;]+))?\{')
    for ($index = 0; $index -lt $structMatches.Count; ++$index) {
        $start = $structMatches[$index].Index
        $end = if ($index + 1 -lt $structMatches.Count) { $structMatches[$index + 1].Index } else { $Contents.Length }
        $bases = @()
        if ($structMatches[$index].Groups[2].Success) {
            foreach ($base in ($structMatches[$index].Groups[2].Value -split ',')) {
                $name = (($base -replace '\b(public|private|protected|virtual)\b', '') -replace '<.*$', '').Trim()
                $name = $name -replace '^.*::', ''
                if ($name -match '^[A-Za-z_][A-Za-z0-9_]*$') { $bases += $name }
            }
        }
        $slices += [PSCustomObject]@{
            name = $structMatches[$index].Groups[1].Value
            bases = $bases
            body = $Contents.Substring($start, $end - $start)
        }
    }
    return $slices
}

function Get-RackWidgetCatalog([string]$Contents, [scriptblock]$ResolveAsset) {
    $catalog = @{}
    foreach ($slice in Get-RackStructSlices $Contents) {
        $size = $null
        $svgMatch = [regex]::Match($slice.body, '"((?:res/)?[^"]*\.svg)"')
        if ($svgMatch.Success) {
            $size = Get-RackSvgSizePx (& $ResolveAsset $svgMatch.Groups[1].Value)
        }
        if (-not $size) {
            $boxMatch = [regex]::Match($slice.body,
                '(?:this\s*->\s*)?box\.size\s*=\s*(mm2px\s*\(\s*)?(?:math::)?Vec\s*\(\s*([0-9.]+)f?\s*,\s*([0-9.]+)f?\s*\)')
            if ($boxMatch.Success) {
                $scale = if ($boxMatch.Groups[1].Success -and $boxMatch.Groups[1].Value) { $script:RackMmToPx } else { 1.0 }
                $size = [PSCustomObject]@{
                    width = [double]::Parse($boxMatch.Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture) * $scale
                    height = [double]::Parse($boxMatch.Groups[3].Value, [Globalization.CultureInfo]::InvariantCulture) * $scale
                }
            }
        }
        if (-not $catalog.ContainsKey($slice.name)) {
            $catalog[$slice.name] = [PSCustomObject]@{ size = $size; bases = $slice.bases }
        }
    }
    return $catalog
}

function Get-RackComponentCatalog([string]$SystemRoot) {
    if ([string]::IsNullOrWhiteSpace($SystemRoot)) { $SystemRoot = Get-RackDefaultSystemRoot }
    if ($script:RackComponentCatalogCache.ContainsKey($SystemRoot)) {
        return $script:RackComponentCatalogCache[$SystemRoot]
    }
    $catalog = @{}
    $header = Join-Path $SystemRoot 'include\componentlibrary.hpp'
    if (Test-Path -LiteralPath $header) {
        $contents = Get-Content -LiteralPath $header -Raw
        $catalog = Get-RackWidgetCatalog $contents {
            param($asset)
            Join-Path $SystemRoot $asset
        }.GetNewClosure()
    }
    $script:RackComponentCatalogCache[$SystemRoot] = $catalog
    return $catalog
}

function Get-RackPluginWidgetCatalog([string]$PluginRoot) {
    if ([string]::IsNullOrWhiteSpace($PluginRoot)) { return @{} }
    if ($script:RackPluginWidgetCache.ContainsKey($PluginRoot)) {
        return $script:RackPluginWidgetCache[$PluginRoot]
    }
    $sources = ''
    $sourceRoot = Join-Path $PluginRoot 'src'
    if (Test-Path -LiteralPath $sourceRoot) {
        $files = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in @('.h', '.hpp', '.hh', '.cpp', '.cc') -and $_.Length -lt 512KB } |
            Select-Object -First 160)
        $builder = [System.Text.StringBuilder]::new()
        foreach ($file in $files) {
            [void]$builder.AppendLine((Get-Content -LiteralPath $file.FullName -Raw))
        }
        $sources = $builder.ToString()
    }
    $catalog = Get-RackWidgetCatalog $sources {
        param($asset)
        Join-Path $PluginRoot $asset
    }.GetNewClosure()
    $script:RackPluginWidgetCache[$PluginRoot] = $catalog
    return $catalog
}

# ---- widget classification --------------------------------------------------

function Get-RackWidgetBaseName([string]$WidgetType) {
    $name = $WidgetType.Trim()
    $name = $name -replace '<.*$', ''            # strip template args
    $name = $name -replace '^.*::', ''           # strip namespaces
    return $name.Trim()
}

function Get-RackBridgeHeuristicSize([string]$WidgetType, [string]$Kind) {
    $w = 22.0; $h = 22.0
    if ($Kind -eq 'input' -or $Kind -eq 'output' -or $WidgetType -match 'Port|Jack') {
        $w = 24.0; $h = 24.0
    }
    elseif ($Kind -eq 'light') {
        if ($WidgetType -match 'Tiny') { $w = 4.0; $h = 4.0 }
        elseif ($WidgetType -match 'Small') { $w = 6.0; $h = 6.0 }
        elseif ($WidgetType -match 'Medium') { $w = 8.0; $h = 8.0 }
        elseif ($WidgetType -match 'Large|Big') { $w = 12.0; $h = 12.0 }
        else { $w = 8.0; $h = 8.0 }
    }
    elseif ($WidgetType -match 'Slider|Fader|Slide') {
        $w = 14.0; $h = 60.0
        if ($WidgetType -match 'Long') { $h = 96.0 }
        elseif ($WidgetType -match 'Short') { $h = 44.0 }
        elseif ($WidgetType -match 'Horizontal|HSlider') { $w = 60.0; $h = 14.0 }
    }
    elseif ($WidgetType -match 'Button|Arrow|LEDBezel|TL1105|Push') {
        $w = 14.0; $h = 14.0
    }
    elseif ($WidgetType -match 'CKSS|CKD|NKK|Switch|Toggle') {
        $w = 12.0; $h = 20.0
    }
    elseif ($WidgetType -match 'Tiny') { $w = 12.0; $h = 12.0 }
    elseif ($WidgetType -match 'Small|Trimpot') { $w = 18.0; $h = 18.0 }
    elseif ($WidgetType -match 'Big|Rogan1|Large|Huge') { $w = 36.0; $h = 36.0 }
    elseif ($WidgetType -match 'Rogan|Davies') { $w = 30.0; $h = 30.0 }
    return [PSCustomObject]@{ width = $w; height = $h }
}

function Get-RackBridgeSize([string]$WidgetType, [string]$Kind,
                            [hashtable]$PluginCatalog, [hashtable]$ComponentCatalog) {
    $size = $null
    $name = Get-RackWidgetBaseName $WidgetType
    for ($hop = 0; $hop -lt 6 -and -not [string]::IsNullOrWhiteSpace($name); ++$hop) {
        $entry = $null
        if ($PluginCatalog -and $PluginCatalog.ContainsKey($name)) { $entry = $PluginCatalog[$name] }
        elseif ($ComponentCatalog -and $ComponentCatalog.ContainsKey($name)) { $entry = $ComponentCatalog[$name] }
        if (-not $entry) { break }
        if ($entry.size) { $size = $entry.size; break }
        $name = if (@($entry.bases).Count -gt 0) { @($entry.bases)[0] } else { '' }
    }
    if (-not $size) { $size = Get-RackBridgeHeuristicSize $WidgetType $Kind }
    return [PSCustomObject]@{
        width = $size.width
        height = $size.height
        radius = [Math]::Min($size.width, $size.height) * 0.5
    }
}

function Get-RackBridgeStyle([string]$WidgetType, [string]$Kind) {
    if ($WidgetType -match 'Slider|Fader|Slide') { return 'slider' }
    if ($WidgetType -match 'Button|Bezel|TL1105|PB61303|CKD6|Push|Momentary') { return 'button' }
    if ($WidgetType -match 'Switch|CKSS|NKK|Toggle') { return 'switch' }
    if ($WidgetType -match 'LED|Light') { if ($Kind -eq 'param') { return 'button' } }
    return 'knob'
}

# ---- expression evaluation --------------------------------------------------

function ConvertTo-RackNumber([string]$Expression, [hashtable]$Constants,
                              [hashtable]$Arrays, [hashtable]$Variables) {
    if ([string]::IsNullOrWhiteSpace($Expression)) { return $null }
    $expr = $Expression
    $expr = $expr -replace '//.*$', ''
    $expr = $expr -replace '/\*.*?\*/', ''
    $expr = $expr -replace '\bbox\.size\.x\b', 'panelWidth'
    $expr = $expr -replace '\bbox\.size\.y\b', 'panelHeight'
    $expr = $expr -replace '\(float\)|\(double\)|\(int\)', ''
    $expr = $expr -replace '\bstatic_cast<[^>]+>', ''

    # mm2px()/px2mm() inline (innermost-first so nesting unwinds)
    $factor = ConvertTo-RackInvariantString $script:RackMmToPx
    while ($expr -match 'mm2px\s*\(') {
        $next = [regex]::Replace($expr, 'mm2px\s*\(([^()]*)\)', "((`$1)*$factor)")
        if ($next -eq $expr) { break }
        $expr = $next
    }
    while ($expr -match 'px2mm\s*\(') {
        $next = [regex]::Replace($expr, 'px2mm\s*\(([^()]*)\)', "((`$1)/$factor)")
        if ($next -eq $expr) { break }
        $expr = $next
    }

    # qualified constants (Class::kMember) must substitute BEFORE the
    # namespace-qualifier strip erases their keys
    foreach ($name in ($Constants.Keys | Where-Object { $_ -like '*::*' } | Sort-Object Length -Descending)) {
        $expr = [regex]::Replace($expr, "(?<![A-Za-z0-9_:])$([regex]::Escape($name))(?![A-Za-z0-9_])",
            (ConvertTo-RackInvariantString $Constants[$name]))
    }
    $expr = $expr -replace '\b[A-Za-z_][A-Za-z0-9_]*::([A-Za-z_][A-Za-z0-9_]*)\b', '$1'

    foreach ($name in ($Variables.Keys | Sort-Object Length -Descending)) {
        $expr = [regex]::Replace($expr, "(?<![A-Za-z0-9_])$([regex]::Escape($name))(?![A-Za-z0-9_])",
            (ConvertTo-RackInvariantString $Variables[$name]))
    }
    foreach ($name in ($Arrays.Keys | Sort-Object Length -Descending)) {
        $pattern = "$([regex]::Escape($name))\s*\[\s*([^\]]+)\s*\]"
        $expr = [regex]::Replace($expr, $pattern, {
            param($match)
            $indexValue = ConvertTo-RackNumber $match.Groups[1].Value $Constants $Arrays $Variables
            if ($null -eq $indexValue) { return '0' }
            $index = [int][Math]::Round($indexValue)
            $values = @($Arrays[$name])
            if ($index -lt 0 -or $index -ge $values.Count) { return '0' }
            return (ConvertTo-RackInvariantString $values[$index])
        })
    }
    foreach ($name in ($Constants.Keys | Sort-Object Length -Descending)) {
        $expr = [regex]::Replace($expr, "(?<![A-Za-z0-9_])$([regex]::Escape($name))(?![A-Za-z0-9_])",
            (ConvertTo-RackInvariantString $Constants[$name]))
    }

    # numeric literal suffixes -- AFTER identifier substitution so names ending
    # in 'f' (e.g. "yHalf") are not mangled
    $expr = $expr -replace '(?<=[0-9.])f\b', ''
    $expr = $expr -replace '(?<=[0-9])u\b', ''

    if ($expr -match '[A-Za-z_]') { return $null }
    try {
        $value = $script:RackBridgeDataTable.Compute($expr, $null)
        return [double]$value
    }
    catch {
        return $null
    }
}

# ---- enum / constant harvesting ---------------------------------------------

function Get-RackBridgeIdMap([string]$Contents, [string]$ModuleType) {
    $ids = @{}
    foreach ($enumMatch in [regex]::Matches($Contents, '(?s)enum\s+[A-Za-z_][A-Za-z0-9_]*\s*\{(.*?)\};')) {
        $value = 0
        $pendingEnumName = $null
        $body = $enumMatch.Groups[1].Value -replace '(?s)/\*.*?\*/', ''
        $body = $body -replace '//.*', ''
        foreach ($rawItem in ($body -split ',')) {
            $item = $rawItem.Trim()
            if ($item -match '^ENUMS\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*$') {
                $pendingEnumName = $matches[1]
                continue
            }
            if ($pendingEnumName) {
                $countExpr = ($item -replace '\).*$','').Trim()
                $count = ConvertTo-RackNumber $countExpr $ids @{} @{}
                if ($null -eq $count -or $count -lt 1) { $count = 1 }
                $ids[$pendingEnumName] = $value
                if (-not [string]::IsNullOrWhiteSpace($ModuleType)) { $ids["$ModuleType::$pendingEnumName"] = $value }
                $value += [int][Math]::Round($count)
                $pendingEnumName = $null
                continue
            }
            if ($item -notmatch '^([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*(.+))?$') { continue }
            $name = $matches[1]
            if (-not [string]::IsNullOrWhiteSpace($matches[2])) {
                $explicit = ConvertTo-RackNumber $matches[2] $ids @{} @{}
                if ($null -ne $explicit) { $value = [int][Math]::Round($explicit) }
            }
            $ids[$name] = $value
            if (-not [string]::IsNullOrWhiteSpace($ModuleType)) { $ids["$ModuleType::$name"] = $value }
            ++$value
        }
    }
    return $ids
}

function Add-RackBridgeConstants([string]$Contents, [hashtable]$Constants, [hashtable]$Arrays) {
    $Constants['RACK_GRID_WIDTH'] = 15.0
    $Constants['RACK_GRID_HEIGHT'] = 380.0
    $Constants['kRACK_GRID_WIDTH'] = 15.0
    $Constants['kRACK_GRID_HEIGHT'] = 380.0
    $Constants['kRACK_JACK_HALF_SIZE'] = 3.5
    if (-not $Constants.ContainsKey('panelHeight')) { $Constants['panelHeight'] = 380.0 }
    if (-not $Constants.ContainsKey('panelWidth')) { $Constants['panelWidth'] = 120.0 }
    $Constants['SmallKnob::kHalfSize'] = 10.0
    $Constants['BigKnob::kHalfSize'] = 18.0
    $Constants['MediumKnob::kHalfSize'] = 13.0
    $Constants['RACK_MM'] = $script:RackMmToPx

    foreach ($arrayMatch in [regex]::Matches($Contents, '(?s)\b(?:static\s+)?(?:const(?:expr)?\s+)?(?:float|double|int)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[[^\]]*\]\s*=\s*\{([^}]+)\}')) {
        $values = @()
        foreach ($rawValue in ($arrayMatch.Groups[2].Value -split ',')) {
            $value = ConvertTo-RackNumber $rawValue $Constants $Arrays @{}
            if ($null -ne $value) { $values += $value }
        }
        if ($values.Count -gt 0) { $Arrays[$arrayMatch.Groups[1].Value] = $values }
    }

    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($constMatch in [regex]::Matches($Contents, '(?m)\b(?:static\s+)?(?:const(?:expr)?\s+)?(?:float|double|int)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);')) {
            $name = $constMatch.Groups[1].Value
            if ($Constants.ContainsKey($name)) { continue }
            $value = ConvertTo-RackNumber $constMatch.Groups[2].Value $Constants $Arrays @{}
            if ($null -ne $value) {
                $Constants[$name] = $value
                $changed = $true
            }
        }
        # class-scoped layout constants (struct BigKnob { static constexpr
        # float kHalfSize = ...; }) referenced as BigKnob::kHalfSize
        foreach ($slice in Get-RackStructSlices $Contents) {
            foreach ($memberMatch in [regex]::Matches($slice.body,
                '(?m)\bstatic\s+(?:const(?:expr)?\s+)+(?:const\s+)?(?:float|double|int)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);')) {
                $qualified = "$($slice.name)::$($memberMatch.Groups[1].Value)"
                if ($Constants.ContainsKey($qualified)) { continue }
                $value = ConvertTo-RackNumber $memberMatch.Groups[2].Value $Constants $Arrays @{}
                if ($null -ne $value) {
                    $Constants[$qualified] = $value
                    $changed = $true
                }
            }
        }
    }

    $boxMatch = [regex]::Match($Contents, 'box\.size\s*=\s*(?:math::)?Vec\s*\(\s*([^,]+)\s*,\s*([^)]+)\)')
    if ($boxMatch.Success) {
        $width = ConvertTo-RackNumber $boxMatch.Groups[1].Value $Constants $Arrays @{}
        $height = ConvertTo-RackNumber $boxMatch.Groups[2].Value $Constants $Arrays @{}
        if ($null -ne $width -and $width -gt 0) { $Constants['panelWidth'] = $width }
        if ($null -ne $height -and $height -gt 0) { $Constants['panelHeight'] = $height }
    }
}

# ---- loop expansion ---------------------------------------------------------

function Get-RackBridgeLoops([string]$Contents, [hashtable]$Constants, [hashtable]$Arrays) {
    $loops = @{}
    foreach ($loopMatch in [regex]::Matches($Contents,
        'for\s*\(\s*(?:int|unsigned|size_t|auto)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\d+)\s*;\s*\1\s*(<=|<)\s*([^;]+);')) {
        $name = $loopMatch.Groups[1].Value
        $start = [int]$loopMatch.Groups[2].Value
        $limit = ConvertTo-RackNumber $loopMatch.Groups[4].Value $Constants $Arrays @{}
        if ($null -eq $limit) { continue }
        $end = [int][Math]::Round($limit)
        if ($loopMatch.Groups[3].Value -eq '<') { $end -= 1 }
        if ($end -lt $start) { continue }
        $end = [Math]::Min($end, $start + 63)
        if (-not $loops.ContainsKey($name)) { $loops[$name] = @($start..$end) }
    }
    return $loops
}

function Get-RackBridgeLoopCombos([string]$Statement, [hashtable]$Loops) {
    $active = @()
    foreach ($name in $Loops.Keys) {
        if ($Statement -match "(?<![A-Za-z0-9_])$([regex]::Escape($name))(?![A-Za-z0-9_])") {
            $active += $name
        }
        if ($active.Count -ge 2) { break }
    }
    if ($active.Count -eq 0) { return ,@(@{}) }
    $combos = @()
    if ($active.Count -eq 1) {
        foreach ($value in $Loops[$active[0]]) { $combos += ,@{ $active[0] = [double]$value } }
    }
    else {
        foreach ($first in $Loops[$active[0]]) {
            foreach ($second in $Loops[$active[1]]) {
                $combos += ,@{ $active[0] = [double]$first; $active[1] = [double]$second }
                if ($combos.Count -ge 256) { return $combos }
            }
        }
    }
    return $combos
}

# ---- control extraction -----------------------------------------------------

function Add-RackBridgeControl([System.Collections.ArrayList]$Controls, [hashtable]$Seen,
                               [string]$Kind, [string]$WidgetType, [bool]$Centered,
                               [bool]$Millimeters, [string]$XExpr, [string]$YExpr, [string]$IdExpr,
                               [hashtable]$Constants, [hashtable]$Arrays, [hashtable]$Variables,
                               [hashtable]$PluginCatalog, [hashtable]$ComponentCatalog) {
    $x = ConvertTo-RackNumber $XExpr $Constants $Arrays $Variables
    $y = ConvertTo-RackNumber $YExpr $Constants $Arrays $Variables
    $id = ConvertTo-RackNumber $IdExpr $Constants $Arrays $Variables
    if ($null -eq $x -or $null -eq $y -or $null -eq $id) { return }
    if ($Millimeters) {
        $x *= $script:RackMmToPx
        $y *= $script:RackMmToPx
    }
    $size = Get-RackBridgeSize $WidgetType $Kind $PluginCatalog $ComponentCatalog
    if (-not $Centered) {
        $x += $size.width * 0.5
        $y += $size.height * 0.5
    }
    $key = "$Kind/$([int][Math]::Round($id))"
    if ($Seen.ContainsKey($key)) { return }
    $Seen[$key] = $true
    [void]$Controls.Add([PSCustomObject]@{
        id = [int][Math]::Round($id)
        x = [Math]::Round($x, 3)
        y = [Math]::Round($y, 3)
        radius = [Math]::Round($size.radius, 3)
        style = Get-RackBridgeStyle $WidgetType $Kind
        width = [Math]::Round($size.width, 3)
        height = [Math]::Round($size.height, 3)
        widget = $WidgetType
    })
}

function Remove-RackBridgeComments([string]$Contents) {
    $Contents = [regex]::Replace($Contents, '(?s)/\*.*?\*/', ' ')
    return [regex]::Replace($Contents, '//[^\r\n]*', '')
}

function Get-RackBridgeLayout([string]$Contents, [string]$ModuleType,
                              [string]$PluginRoot = '', [string]$SystemRoot = '',
                              [double]$PanelWidthPx = 0, [double]$PanelHeightPx = 0) {
    $clean = Remove-RackBridgeComments $Contents
    $constants = Get-RackBridgeIdMap $clean $ModuleType
    if ($PanelWidthPx -gt 0) { $constants['panelWidth'] = $PanelWidthPx }
    if ($PanelHeightPx -gt 0) { $constants['panelHeight'] = $PanelHeightPx }
    $arrays = @{}
    Add-RackBridgeConstants $clean $constants $arrays
    $loops = Get-RackBridgeLoops $clean $constants $arrays
    $componentCatalog = Get-RackComponentCatalog $SystemRoot
    $pluginCatalog = Get-RackPluginWidgetCatalog $PluginRoot

    $params = [System.Collections.ArrayList]::new()
    $inputs = [System.Collections.ArrayList]::new()
    $outputs = [System.Collections.ArrayList]::new()
    $lights = [System.Collections.ArrayList]::new()
    $seen = @{}

    # one nesting level inside Vec() coordinate expressions
    $coordExpr = '(?:[^,()]|\((?:[^()]|\([^()]*\))*\))+'
    $position = "(?<mm>mm2px\s*\(\s*)?(?:math::)?Vec\s*\(\s*(?<x>$coordExpr),\s*(?<y>$coordExpr)\)\s*\)?"
    $idExpr = '(?<id>[^,()]+(?:\([^()]*\))?[^,()]*)'

    # whole statements so multi-line addParam(...) calls parse
    foreach ($statement in (($clean -replace '\s+', ' ') -split ';')) {
        if ($statement -notmatch 'create(?:LightParamCentered|ParamCentered|Param|InputCentered|Input|OutputCentered|Output|LightCentered|Light)\s*<') { continue }
        foreach ($vars in (Get-RackBridgeLoopCombos $statement $loops)) {
            if ($statement -match "createLightParamCentered<(?<type>[^>]+(?:<[^>]*>)?)>\s*\(\s*$position\s*,\s*module\s*,\s*(?<pid>[^,]+)\s*,\s*(?<lid>[^)]+)\)") {
                $mm = -not [string]::IsNullOrWhiteSpace($matches['mm'])
                Add-RackBridgeControl $params $seen 'param' $matches['type'] $true $mm $matches['x'] $matches['y'] $matches['pid'] $constants $arrays $vars $pluginCatalog $componentCatalog
                Add-RackBridgeControl $lights $seen 'light' $matches['type'] $true $mm $matches['x'] $matches['y'] $matches['lid'] $constants $arrays $vars $pluginCatalog $componentCatalog
                continue
            }
            if ($statement -match "createParam(?<center>Centered)?<(?<type>[^>]+(?:<[^>]*>)?)>\s*\(\s*$position\s*,\s*module\s*,\s*$idExpr\s*[,)]") {
                $mm = -not [string]::IsNullOrWhiteSpace($matches['mm'])
                Add-RackBridgeControl $params $seen 'param' $matches['type'] ([bool]$matches['center']) $mm $matches['x'] $matches['y'] $matches['id'] $constants $arrays $vars $pluginCatalog $componentCatalog
            }
            if ($statement -match "createInput(?<center>Centered)?<(?<type>[^>]+(?:<[^>]*>)?)>\s*\(\s*$position\s*,\s*module\s*,\s*$idExpr\s*[,)]") {
                $mm = -not [string]::IsNullOrWhiteSpace($matches['mm'])
                Add-RackBridgeControl $inputs $seen 'input' $matches['type'] ([bool]$matches['center']) $mm $matches['x'] $matches['y'] $matches['id'] $constants $arrays $vars $pluginCatalog $componentCatalog
            }
            if ($statement -match "createOutput(?<center>Centered)?<(?<type>[^>]+(?:<[^>]*>)?)>\s*\(\s*$position\s*,\s*module\s*,\s*$idExpr\s*[,)]") {
                $mm = -not [string]::IsNullOrWhiteSpace($matches['mm'])
                Add-RackBridgeControl $outputs $seen 'output' $matches['type'] ([bool]$matches['center']) $mm $matches['x'] $matches['y'] $matches['id'] $constants $arrays $vars $pluginCatalog $componentCatalog
            }
            if ($statement -match "createLight(?<center>Centered)?<(?<type>[^>]+(?:<[^>]*>)?)>\s*\(\s*$position\s*,\s*module\s*,\s*$idExpr\s*[,)]") {
                $mm = -not [string]::IsNullOrWhiteSpace($matches['mm'])
                Add-RackBridgeControl $lights $seen 'light' $matches['type'] ([bool]$matches['center']) $mm $matches['x'] $matches['y'] $matches['id'] $constants $arrays $vars $pluginCatalog $componentCatalog
            }
        }
    }

    return [PSCustomObject]@{
        panelWidth = [Math]::Round([double]$constants['panelWidth'], 3)
        panelHeight = [Math]::Round([double]$constants['panelHeight'], 3)
        params = @($params | Sort-Object id)
        inputs = @($inputs | Sort-Object id)
        outputs = @($outputs | Sort-Object id)
        lights = @($lights | Sort-Object id)
    }
}

function Add-RackBridgeLayoutMembers([psobject]$Manifest, [psobject]$Layout) {
    $Manifest | Add-Member -NotePropertyName panelWidth -NotePropertyValue $Layout.panelWidth -Force
    $Manifest | Add-Member -NotePropertyName panelHeight -NotePropertyValue $Layout.panelHeight -Force
    $Manifest | Add-Member -NotePropertyName params -NotePropertyValue @($Layout.params) -Force
    $Manifest | Add-Member -NotePropertyName inputs -NotePropertyValue @($Layout.inputs) -Force
    $Manifest | Add-Member -NotePropertyName outputs -NotePropertyValue @($Layout.outputs) -Force
    $Manifest | Add-Member -NotePropertyName lights -NotePropertyValue @($Layout.lights) -Force
}

function Get-RackBridgeLayoutSource([string]$SourcePath) {
    $contents = Get-Content -LiteralPath $SourcePath -Raw
    $dir = Split-Path -Parent $SourcePath
    foreach ($header in Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue |
             Where-Object { $_.Extension -in @('.h', '.hpp', '.hh') }) {
        $contents += "`n"
        $contents += Get-Content -LiteralPath $header.FullName -Raw
    }
    return $contents
}
