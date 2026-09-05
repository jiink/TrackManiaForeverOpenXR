[CmdletBinding()]
param(
    [string]$TMLoaderPath = (Join-Path $env:LOCALAPPDATA 'TMLoader'),
    [string]$ModDll,
    [string]$OpenXrLoader,
    [string]$Version = '0.1.0-dev'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($ModDll)) {
    $ModDll = Join-Path $repositoryRoot 'build\TMFOXR.dll'
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

$productPath = Join-Path $databasePath 'products\TMFOXR'
$versionPath = Join-Path $productPath $Version
New-Item -ItemType Directory -Path $versionPath -Force | Out-Null

$productDescription = @'
name: TrackMania Forever OpenXR
author: jiink
type: modification
description: Native stereoscopic OpenXR rendering and headset tracking for TrackMania United Forever. Settings are stored in Documents\TrackMania\TMFOXR.ini.
'@
$versionDescription = @"
executable: TMFOXR.dll
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

Copy-Item -LiteralPath $resolvedModDll -Destination (Join-Path $versionPath 'TMFOXR.dll') -Force
Copy-Item -LiteralPath $resolvedOpenXrLoader -Destination (Join-Path $versionPath 'openxr_loader.dll') -Force
$sourceImGuiLicense = Join-Path (Split-Path -Parent $resolvedModDll) 'DearImGui-LICENSE.txt'
if (Test-Path -LiteralPath $sourceImGuiLicense -PathType Leaf) {
    Copy-Item -LiteralPath $sourceImGuiLicense -Destination (Join-Path $versionPath 'DearImGui-LICENSE.txt') -Force
}

$documentsPath = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
if ([string]::IsNullOrWhiteSpace($documentsPath)) {
    throw 'Windows did not provide a Documents folder for the persistent TMFOXR configuration.'
}
$userDataPath = Join-Path $documentsPath 'TrackMania'
New-Item -ItemType Directory -Path $userDataPath -Force | Out-Null
$destinationConfiguration = Join-Path $userDataPath 'TMFOXR.ini'
if (-not (Test-Path -LiteralPath $destinationConfiguration -PathType Leaf)) {
    $sourceConfiguration = Join-Path $repositoryRoot 'TMFOXR.defaults.ini'
    Copy-Item -LiteralPath $sourceConfiguration -Destination $destinationConfiguration
    Write-Host "Installed editable TMFOXR configuration: '$destinationConfiguration'."
}
else {
    Write-Host "Preserved existing TMFOXR configuration '$destinationConfiguration'."
}
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'TMFOXR.defaults.ini') -Destination (Join-Path $versionPath 'TMFOXR.defaults.ini') -Force

Write-Host "Installed local TMLoader product 'TrackMania Forever OpenXR' $Version at '$versionPath'."
Write-Host 'Restart TrackMania ModLoader, select the TrackMania Forever OpenXR diamond for the desired profile, and launch that profile.'
