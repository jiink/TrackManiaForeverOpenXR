[CmdletBinding()]
param(
    [string]$GamePath = 'C:\Program Files (x86)\Steam\steamapps\common\TrackMania United',
    [string]$ModDll,
    [switch]$RefreshOpenXrLoader
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($ModDll)) {
    $ModDll = Join-Path $scriptRoot '..\build\d3d9.dll'
}
$loaderVersion = '1.1.62'
$loaderUri = "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$loaderVersion/openxr_loader_windows-$loaderVersion.zip"
$loaderArchiveSha256 = '800EC772E2F9448A26AB9F579F4914D984346DD9D0D7C007841ABE21D2C8FF2F'
$x86Machine = 0x014c

function Get-PeMachine {
    param([Parameter(Mandatory)][string]$Path)

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5a4d) { return 0 }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { return 0 }
        return $reader.ReadUInt16()
    }
    finally {
        if ($reader) { $reader.Dispose() }
        else { $stream.Dispose() }
    }
}

$resolvedGamePath = [System.IO.Path]::GetFullPath($GamePath)
$resolvedModDll = [System.IO.Path]::GetFullPath($ModDll)
$gameExecutable = Join-Path $resolvedGamePath 'TmForever.exe'
$destinationProxy = Join-Path $resolvedGamePath 'd3d9.dll'
$proxyBackup = Join-Path $resolvedGamePath 'd3d9.before-tmoxr.dll'
$destinationLoader = Join-Path $resolvedGamePath 'openxr_loader.dll'
$sourceConfiguration = Join-Path $scriptRoot '..\TMOXR.ini'
$destinationConfiguration = Join-Path $resolvedGamePath 'TMOXR.ini'

if (-not (Test-Path -LiteralPath $gameExecutable -PathType Leaf)) {
    throw "TmForever.exe was not found at '$gameExecutable'. Pass the correct -GamePath."
}
if (-not (Test-Path -LiteralPath $resolvedModDll -PathType Leaf)) {
    throw "The built mod DLL was not found at '$resolvedModDll'. Build target d3d9 first or pass -ModDll."
}
if ((Get-PeMachine -Path $resolvedModDll) -ne $x86Machine) {
    throw "'$resolvedModDll' is not a Win32 DLL. Build from an x86 MSVC environment."
}
if ((Test-Path -LiteralPath $destinationProxy -PathType Leaf) -and
    -not (Test-Path -LiteralPath $proxyBackup)) {
    Copy-Item -LiteralPath $destinationProxy -Destination $proxyBackup
    Write-Host "Preserved existing d3d9.dll as '$proxyBackup'."
}
Copy-Item -LiteralPath $resolvedModDll -Destination $destinationProxy -Force
Write-Host "Installed TrackMania OpenXR proxy: '$destinationProxy'."

if ((Test-Path -LiteralPath $sourceConfiguration -PathType Leaf) -and
    -not (Test-Path -LiteralPath $destinationConfiguration -PathType Leaf)) {
    Copy-Item -LiteralPath $sourceConfiguration -Destination $destinationConfiguration
    Write-Host "Installed editable VR camera configuration: '$destinationConfiguration'."
}
elseif ((Test-Path -LiteralPath $sourceConfiguration -PathType Leaf) -and
        (Test-Path -LiteralPath $destinationConfiguration -PathType Leaf)) {
    $sourceText = [System.IO.File]::ReadAllText($sourceConfiguration)
    $destinationText = [System.IO.File]::ReadAllText($destinationConfiguration)
    $addedSections = [System.Collections.Generic.List[string]]::new()
    foreach ($section in @('Camera.Stadium', 'Camera.Island', 'Camera.Desert', 'Camera.Rally', 'Camera.Bay', 'Camera.Coast', 'Camera.Snow')) {
        $escapedSection = [regex]::Escape($section)
        if ($destinationText -match "(?im)^\s*\[$escapedSection\]\s*$") { continue }
        $sectionMatch = [regex]::Match(
            $sourceText,
            "(?ims)^\s*\[$escapedSection\]\s*\r?\n.*?(?=^\s*\[|\z)")
        if (-not $sectionMatch.Success) { continue }
        $destinationText = $destinationText.TrimEnd() + [Environment]::NewLine + [Environment]::NewLine +
            $sectionMatch.Value.Trim() + [Environment]::NewLine
        $addedSections.Add($section)
    }
    if ($addedSections.Count -gt 0) {
        [System.IO.File]::WriteAllText($destinationConfiguration, $destinationText)
        Write-Host "Added missing vehicle camera sections to '$destinationConfiguration': $($addedSections -join ', ')."
    }
}

$loaderIsWin32 = (Test-Path -LiteralPath $destinationLoader -PathType Leaf) -and
    ((Get-PeMachine -Path $destinationLoader) -eq $x86Machine)
if ($RefreshOpenXrLoader -or -not $loaderIsWin32) {
    $temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('tmoxr-loader-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    try {
        $archivePath = Join-Path $temporaryRoot "openxr_loader_windows-$loaderVersion.zip"
        $extractPath = Join-Path $temporaryRoot 'extract'
        Write-Host "Downloading official Khronos OpenXR loader $loaderVersion..."
        Invoke-WebRequest -UseBasicParsing -Uri $loaderUri -OutFile $archivePath
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
        if ($actualHash -ne $loaderArchiveSha256) {
            throw "OpenXR loader archive checksum mismatch. Expected $loaderArchiveSha256, received $actualHash."
        }
        Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath
        $sourceLoader = Join-Path $extractPath 'Win32\bin\openxr_loader.dll'
        if (-not (Test-Path -LiteralPath $sourceLoader -PathType Leaf) -or
            (Get-PeMachine -Path $sourceLoader) -ne $x86Machine) {
            throw 'The verified archive did not contain the expected Win32\bin\openxr_loader.dll.'
        }
        Copy-Item -LiteralPath $sourceLoader -Destination $destinationLoader -Force
        Write-Host "Installed official Win32 OpenXR loader: '$destinationLoader'."
    }
    finally {
        if (Test-Path -LiteralPath $temporaryRoot) {
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
        }
    }
}
else {
    Write-Host "Existing Win32 OpenXR loader retained: '$destinationLoader'."
}

Write-Host 'Installation complete. Start the active OpenXR runtime before launching TrackMania.'
