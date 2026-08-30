[CmdletBinding()]
param(
    [string]$TMLoaderPath = (Join-Path $env:LOCALAPPDATA 'TMLoader'),
    [string]$GamePath = 'C:\Program Files (x86)\Steam\steamapps\common\TrackMania United',
    [string]$ModDll,
    [string]$OpenXrLoader,
    [string]$Version = '0.1.0-dev'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($ModDll)) {
    $ModDll = Join-Path $repositoryRoot 'build\d3d9.dll'
}
if ([string]::IsNullOrWhiteSpace($OpenXrLoader)) {
    $OpenXrLoader = Join-Path $repositoryRoot 'dlls\32-bit\openxr_loader.dll'
}

function Get-PeMachine {
    param([Parameter(Mandatory)][string]$Path)

    $stream = [System.IO.File]::Open(
        $Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
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

$resolvedLoaderPath = [System.IO.Path]::GetFullPath($TMLoaderPath)
$resolvedModDll = [System.IO.Path]::GetFullPath($ModDll)
$resolvedOpenXrLoader = [System.IO.Path]::GetFullPath($OpenXrLoader)
$databasePath = Join-Path $resolvedLoaderPath 'database\TmForever'
if (-not (Test-Path -LiteralPath (Join-Path $resolvedLoaderPath 'TMLoader.exe') -PathType Leaf) -or
    -not (Test-Path -LiteralPath $databasePath -PathType Container)) {
    throw "A TrackMania ModLoader installation was not found at '$resolvedLoaderPath'."
}
foreach ($binary in @($resolvedModDll, $resolvedOpenXrLoader)) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Required Win32 DLL not found: '$binary'."
    }
    if ((Get-PeMachine -Path $binary) -ne 0x014c) {
        throw "'$binary' is not a Win32 DLL."
    }
}

$productPath = Join-Path $databasePath 'products\TMOXR'
$versionPath = Join-Path $productPath $Version
New-Item -ItemType Directory -Path $versionPath -Force | Out-Null

$productDescription = @'
name: TrackMania OpenXR
author: TrackMania OpenXR contributors
type: modification
description: Native stereoscopic OpenXR rendering and headset tracking for TrackMania United Forever.
'@
$versionDescription = @"
executable: TMOXR.dll
priority: 1000000000
dependencies:
  - id: CoreMod
    version: ^1.0.11
changelog: Local development build with TrackMania ModLoader support
"@
[System.IO.File]::WriteAllText(
    (Join-Path $productPath 'description.yaml'), $productDescription.Trim() + "`r`n")
[System.IO.File]::WriteAllText(
    (Join-Path $versionPath 'description.yaml'), $versionDescription.Trim() + "`r`n")

Copy-Item -LiteralPath $resolvedModDll -Destination (Join-Path $versionPath 'TMOXR.dll') -Force
Copy-Item -LiteralPath $resolvedOpenXrLoader -Destination (Join-Path $versionPath 'openxr_loader.dll') -Force

$destinationConfiguration = Join-Path $versionPath 'TMOXR.ini'
if (-not (Test-Path -LiteralPath $destinationConfiguration -PathType Leaf)) {
    $gameConfiguration = Join-Path ([System.IO.Path]::GetFullPath($GamePath)) 'TMOXR.ini'
    $sourceConfiguration = if (Test-Path -LiteralPath $gameConfiguration -PathType Leaf) {
        $gameConfiguration
    } else {
        Join-Path $repositoryRoot 'TMOXR.ini'
    }
    Copy-Item -LiteralPath $sourceConfiguration -Destination $destinationConfiguration
    Write-Host "Installed editable configuration from '$sourceConfiguration'."
}
else {
    Write-Host "Preserved existing TMLoader configuration '$destinationConfiguration'."
}

Write-Host "Installed local TMLoader product 'TrackMania OpenXR' $Version at '$versionPath'."
Write-Host 'Restart TrackMania ModLoader, select the TrackMania OpenXR diamond for the desired profile, and launch that profile.'
