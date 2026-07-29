param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$Stable16Source
)

$ErrorActionPreference = 'Stop'
$sources = @(
    'VCO.cpp', 'VCF.cpp', 'VCA-1.cpp', 'ADSR.cpp', 'LFO.cpp', 'Mixer.cpp',
    '8vert.cpp', 'Merge.cpp', 'MidSide.cpp', 'Octave.cpp', 'Split.cpp', 'Sum.cpp',
    'VCA.cpp', 'VCMixer.cpp', 'Mutes.cpp', 'Pulses.cpp', 'Random.cpp', 'SEQ3.cpp',
    'SequentialSwitch.cpp'
)
$factories = @(
    @{ Type = 'VCO'; Function = 'makeVCO' },
    @{ Type = 'VCF'; Function = 'makeVCF' },
    @{ Type = 'VCA_1'; Function = 'makeVCA1' },
    @{ Type = 'ADSR'; Function = 'makeADSR' },
    @{ Type = 'LFO'; Function = 'makeLFO' },
    @{ Type = 'Mixer'; Function = 'makeMixer' },
    @{ Type = '_8vert'; Function = 'make8vert' },
    @{ Type = 'Merge'; Function = 'makeMerge' },
    @{ Type = 'MidSide'; Function = 'makeMidSide' },
    @{ Type = 'Octave'; Function = 'makeOctave' },
    @{ Type = 'Split'; Function = 'makeSplit' },
    @{ Type = 'Sum'; Function = 'makeSum' },
    @{ Type = 'VCA'; Function = 'makeVCA2' },
    @{ Type = 'VCMixer'; Function = 'makeVCMixer' },
    @{ Type = 'Mutes'; Function = 'makeMutes' },
    @{ Type = 'Pulses'; Function = 'makePulses' },
    @{ Type = 'Random'; Function = 'makeRandom' },
    @{ Type = 'SEQ3'; Function = 'makeSEQ3' },
    @{ Type = 'SequentialSwitch<1, 4>'; Function = 'makeSequentialSwitch1' },
    @{ Type = 'SequentialSwitch<4, 1>'; Function = 'makeSequentialSwitch2' }
)
if ($Stable16Source) {
    $factories += @{ Type = 'Stable16'; Function = 'makeStable16' }
}

$sourceEntries = @()
foreach ($source in $sources) {
    $sourceEntries += @{ Name = $source; Path = Join-Path $SourceRoot $source }
}
if ($Stable16Source) {
    $sourceEntries += @{ Name = 'Stable16.cpp'; Path = $Stable16Source }
}

$outputDirectory = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$stream = [IO.StreamWriter]::new($Output, $false, [Text.UTF8Encoding]::new($false))
try {
    $stream.WriteLine('#include "fundamental_bridge.h"')
    $stream.WriteLine('#include "rack_sdk_compat.hpp"')
    $stream.WriteLine('#include <memory>')
    $stream.WriteLine('using namespace rack;')
    foreach ($sourceEntry in $sourceEntries) {
        $source = $sourceEntry.Name
        $content = Get-Content -LiteralPath $sourceEntry.Path -Raw
        $content = [regex]::Replace($content, '(?m)^#include[^\r\n]*(\r?\n)?', '')
        $content = [regex]::Replace($content,
            '(ParamQuantity\*\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*)paramQuantities\[([^\]]+)\]',
            '$1&paramQuantities[$2]')
        $content = [regex]::Replace($content, 'paramQuantities\[([^\]]+)\]->', 'paramQuantities[$1].')
        $content = [regex]::Replace($content, '(?m)^\s*DEBUG\([^\r\n]*\);\s*(\r?\n)?', '')
        $boundaryPattern = '(?m)^struct\s+[A-Za-z_][A-Za-z0-9_]*(Widget|Display|Knob|Port|Button|Meter)\b'
        if ($source -eq 'SequentialSwitch.cpp') {
            $boundaryPattern = '(?m)^struct\s+LightButtonTriSwitch\b'
        }
        $widget = [regex]::Match($content, $boundaryPattern)
        if (-not $widget.Success) { throw "No DSP/UI boundary found in $source" }
        $stream.WriteLine($content.Substring(0, $widget.Index))
    }
    $stream.WriteLine('namespace rackx { namespace fundamental {')
    foreach ($factory in $factories) {
        $stream.WriteLine("std::unique_ptr<rack::engine::Module> $($factory.Function)() { return std::make_unique<$($factory.Type)>(); }")
    }
    $stream.WriteLine('} }')
}
finally {
    $stream.Dispose()
}
