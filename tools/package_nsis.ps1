param(
    [string]$ProjectRoot,
    [string]$BuildDir,
    [string]$OutDir,
    [string]$Makensis,
    [switch]$SkipBuild,
    [switch]$IncludeAllRackModules
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $ProjectRoot 'build'
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $BuildDir 'installer'
}

$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$OutDir = if (Test-Path -LiteralPath $OutDir) { (Resolve-Path -LiteralPath $OutDir).Path } else { New-Item -ItemType Directory -Path $OutDir | Out-Null; (Resolve-Path -LiteralPath $OutDir).Path }
$StageDir = Join-Path $OutDir 'stage'
$InstallerScript = Join-Path $ProjectRoot 'installer\patchknob.nsi'
$InstallerOut = Join-Path $OutDir 'PatchKnob-0.8.7-Setup.exe'

function Assert-UnderPath([string]$Child, [string]$Parent) {
    $childFull = [System.IO.Path]::GetFullPath($Child)
    $parentFull = [System.IO.Path]::GetFullPath($Parent).TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $childFull.StartsWith($parentFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify path outside package output: $childFull"
    }
}

function Copy-FileIfExists([string]$Source, [string]$DestinationDir) {
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        New-Item -ItemType Directory -Path $DestinationDir -Force | Out-Null
        Copy-Item -LiteralPath $Source -Destination $DestinationDir -Force
    }
}

function Copy-FilteredTree([string]$SourceRoot, [string]$DestinationRoot) {
    if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
        return
    }
    $sourceFull = (Resolve-Path -LiteralPath $SourceRoot).Path
    $allowed = @('.dll', '.json', '.s24svgpak', '.signature')
    Get-ChildItem -LiteralPath $sourceFull -Recurse -File |
        Where-Object {
            $_.FullName -notmatch '\\build\\|\\CMakeFiles\\' -and
            $allowed -contains $_.Extension
        } |
        ForEach-Object {
            $relative = $_.FullName.Substring($sourceFull.Length).TrimStart('\', '/')
            $target = Join-Path $DestinationRoot $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $target -Force
        }
}

if (-not $SkipBuild) {
    $env:PATH = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:PATH
    $tmp = Join-Path $BuildDir 'tmp'
    if (-not (Test-Path -LiteralPath $tmp)) { New-Item -ItemType Directory -Path $tmp | Out-Null }
    $env:TMP = (Resolve-Path -LiteralPath $tmp).Path
    $env:TEMP = $env:TMP
    cmake --build $BuildDir --config Release
}

if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'PatchKnob.exe') -PathType Leaf)) {
    throw "Missing built DAW executable: $(Join-Path $BuildDir 'PatchKnob.exe')"
}

Assert-UnderPath $StageDir $OutDir
if (Test-Path -LiteralPath $StageDir) {
    Remove-Item -LiteralPath $StageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $StageDir | Out-Null

$runtimeFiles = @(
    'PatchKnob.exe',
    'probe_vst2.exe',
    'probe_vst3.exe',
    'cardinal_svg.pak',
    'cardinal_svg.pak.signature',
    'csound64.dll'
)
foreach ($file in $runtimeFiles) {
    Copy-FileIfExists (Join-Path $BuildDir $file) $StageDir
}
Get-ChildItem -LiteralPath $BuildDir -File -Filter '*.dll' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $StageDir -Force
}

Copy-FileIfExists (Join-Path $ProjectRoot 'README') $StageDir
Copy-FileIfExists (Join-Path $ProjectRoot 'COPYING') $StageDir

$modulesDest = Join-Path $StageDir 'modules'
if ($IncludeAllRackModules) {
    Copy-FilteredTree (Join-Path $ProjectRoot 'modules') $modulesDest
} else {
    Copy-FilteredTree (Join-Path $ProjectRoot 'modules\Fundamental') (Join-Path $modulesDest 'Fundamental')
}
Copy-FilteredTree (Join-Path $BuildDir 'rack_imports\Fundamental') (Join-Path $modulesDest 'Fundamental')

if ([string]::IsNullOrWhiteSpace($Makensis)) {
    $cmd = Get-Command makensis -ErrorAction SilentlyContinue
    if ($cmd) { $Makensis = $cmd.Source }
}
if ([string]::IsNullOrWhiteSpace($Makensis)) {
    foreach ($candidate in @(
        'C:\Program Files (x86)\NSIS\makensis.exe',
        'C:\Program Files\NSIS\makensis.exe',
        'C:\msys64\mingw64\bin\makensis.exe'
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $Makensis = $candidate; break }
    }
}
if ([string]::IsNullOrWhiteSpace($Makensis) -or -not (Test-Path -LiteralPath $Makensis -PathType Leaf)) {
    throw "makensis.exe not found. Install NSIS or pass -Makensis <path>."
}

& $Makensis `
    "/DAPP_VERSION=0.8.7" `
    "/DSTAGE_DIR=$StageDir" `
    "/DOUT_FILE=$InstallerOut" `
    $InstallerScript

if (-not (Test-Path -LiteralPath $InstallerOut -PathType Leaf)) {
    throw "NSIS did not produce installer: $InstallerOut"
}

Write-Host "Created installer: $InstallerOut"
