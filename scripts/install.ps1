[CmdletBinding()]
param(
    [string]$GamePath = 'C:\Program Files (x86)\Steam\steamapps\common\TrackMania United',
    [string]$ModDll,
    [string]$InputDll,
    [switch]$RefreshOpenXrLoader
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($ModDll)) {
    $ModDll = Join-Path $scriptRoot '..\build\d3d9.dll'
}
if ([string]::IsNullOrWhiteSpace($InputDll)) {
    $InputDll = Join-Path $scriptRoot '..\build\dinput8.dll'
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
$resolvedInputDll = [System.IO.Path]::GetFullPath($InputDll)
$gameExecutable = Join-Path $resolvedGamePath 'TmForever.exe'
$destinationProxy = Join-Path $resolvedGamePath 'd3d9.dll'
$proxyBackup = Join-Path $resolvedGamePath 'd3d9.before-tmoxr.dll'
$destinationInputProxy = Join-Path $resolvedGamePath 'dinput8.dll'
$inputProxyBackup = Join-Path $resolvedGamePath 'dinput8.before-tmoxr.dll'
$destinationLoader = Join-Path $resolvedGamePath 'openxr_loader.dll'

if (-not (Test-Path -LiteralPath $gameExecutable -PathType Leaf)) {
    throw "TmForever.exe was not found at '$gameExecutable'. Pass the correct -GamePath."
}
if (-not (Test-Path -LiteralPath $resolvedModDll -PathType Leaf)) {
    throw "The built mod DLL was not found at '$resolvedModDll'. Build target d3d9 first or pass -ModDll."
}
if ((Get-PeMachine -Path $resolvedModDll) -ne $x86Machine) {
    throw "'$resolvedModDll' is not a Win32 DLL. Build from an x86 MSVC environment."
}
if (-not (Test-Path -LiteralPath $resolvedInputDll -PathType Leaf)) {
    throw "The virtual gamepad DLL was not found at '$resolvedInputDll'. Build target dinput8 first or pass -InputDll."
}
if ((Get-PeMachine -Path $resolvedInputDll) -ne $x86Machine) {
    throw "'$resolvedInputDll' is not a Win32 DLL. Build from an x86 MSVC environment."
}

if ((Test-Path -LiteralPath $destinationProxy -PathType Leaf) -and
    -not (Test-Path -LiteralPath $proxyBackup)) {
    Copy-Item -LiteralPath $destinationProxy -Destination $proxyBackup
    Write-Host "Preserved existing d3d9.dll as '$proxyBackup'."
}
Copy-Item -LiteralPath $resolvedModDll -Destination $destinationProxy -Force
Write-Host "Installed TrackMania OpenXR proxy: '$destinationProxy'."

if ((Test-Path -LiteralPath $destinationInputProxy -PathType Leaf) -and
    -not (Test-Path -LiteralPath $inputProxyBackup)) {
    Copy-Item -LiteralPath $destinationInputProxy -Destination $inputProxyBackup
    Write-Host "Preserved existing dinput8.dll as '$inputProxyBackup'."
}
Copy-Item -LiteralPath $resolvedInputDll -Destination $destinationInputProxy -Force
Write-Host "Installed OpenXR virtual gamepad proxy: '$destinationInputProxy'."

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
