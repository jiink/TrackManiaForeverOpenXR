<#
.SYNOPSIS
Packages a built TMFOXR into the two release zips:

  TMFOXR-<Name>.zip                 d3d9.dll, openxr_loader.dll, TMFOXR.defaults.ini,
                                    DearImGui-LICENSE.txt (unzip next to TmForever.exe)
  TMFOXR-<Name>-for-TMLoader.zip    TMFOXR\description.yaml and TMFOXR\<TMLoaderVersion>\...
                                    (unzip into %LOCALAPPDATA%\TMLoader\database\TmForever\products)

.EXAMPLE
cmake --build build --target d3d9
powershell -ExecutionPolicy Bypass -File .\scripts\package-release.ps1 -Name v9 -TMLoaderVersion 9.0.0
#>
[CmdletBinding()]
param(
    # Used in the zip file names, e.g. "v9" or "nightly".
    [Parameter(Mandatory)][string]$Name,
    # The TMLoader version folder, e.g. "9.0.0" or "9.0.0-dev.42".
    [Parameter(Mandatory)][string]$TMLoaderVersion,
    [string]$Changelog = "TMFOXR $Name",
    [string]$BuildDir,
    [string]$OutputDir,
    # A Win32 openxr_loader.dll to ship. When omitted, the pinned official
    # Khronos loader is downloaded and its checksum verified.
    [string]$OpenXrLoader
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Keep in sync with scripts/install.ps1.
$loaderVersion = '1.1.62'
$loaderUri = "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$loaderVersion/openxr_loader_windows-$loaderVersion.zip"
$loaderArchiveSha256 = '800EC772E2F9448A26AB9F579F4914D984346DD9D0D7C007841ABE21D2C8FF2F'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $repositoryRoot 'build' }
if ([string]::IsNullOrWhiteSpace($OutputDir)) { $OutputDir = Join-Path $repositoryRoot 'dist' }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)

. (Join-Path $scriptRoot 'tmloader-description.ps1')
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-PeMachine {
    param([Parameter(Mandatory)][string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) { return 0 }
    $peOffset = [BitConverter]::ToUInt32($bytes, 0x3c)
    if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) { return 0 }
    return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

# ZipFile.CreateFromDirectory on Windows PowerShell 5.1 stores '\' separators,
# which some extractors treat as part of the file name. Write '/' explicitly.
function New-ZipFromDirectory {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    $sourceRoot = [System.IO.Path]::GetFullPath($Source).TrimEnd('\') + '\'
    $zip = [System.IO.Compression.ZipFile]::Open($Destination, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $Source -Recurse -File | Sort-Object FullName) {
            $entryName = $file.FullName.Substring($sourceRoot.Length).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $file.FullName, $entryName, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
    finally { $zip.Dispose() }
}

function Assert-Win32Dll {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file not found: '$Path'." }
    if ((Get-PeMachine -Path $Path) -ne 0x014c) { throw "'$Path' is not a Win32 DLL." }
}

$modDll = Join-Path $BuildDir 'd3d9.dll'
$imguiLicense = Join-Path $BuildDir 'DearImGui-LICENSE.txt'
$defaultsIni = Join-Path $repositoryRoot 'TMFOXR.defaults.ini'
Assert-Win32Dll -Path $modDll
foreach ($file in @($imguiLicense, $defaultsIni)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file not found: '$file'." }
}

$stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("tmfoxr-package-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stagingRoot | Out-Null
try {
    if ([string]::IsNullOrWhiteSpace($OpenXrLoader)) {
        $archivePath = Join-Path $stagingRoot "openxr_loader_windows-$loaderVersion.zip"
        Write-Host "Downloading official Khronos OpenXR loader $loaderVersion..."
        Invoke-WebRequest -Uri $loaderUri -OutFile $archivePath -UseBasicParsing
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
        if ($actualHash -ne $loaderArchiveSha256) {
            throw "OpenXR loader archive checksum mismatch. Expected $loaderArchiveSha256, received $actualHash."
        }
        $extractPath = Join-Path $stagingRoot 'openxr-loader'
        [System.IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $extractPath)
        $OpenXrLoader = Join-Path $extractPath 'Win32\bin\openxr_loader.dll'
    }
    Assert-Win32Dll -Path $OpenXrLoader

    # Plain package: everything sits next to TmForever.exe.
    $plainDir = Join-Path $stagingRoot 'plain'
    New-Item -ItemType Directory -Path $plainDir | Out-Null
    Copy-Item -LiteralPath $modDll -Destination (Join-Path $plainDir 'd3d9.dll')
    Copy-Item -LiteralPath $OpenXrLoader -Destination (Join-Path $plainDir 'openxr_loader.dll')
    Copy-Item -LiteralPath $defaultsIni -Destination (Join-Path $plainDir 'TMFOXR.defaults.ini')
    Copy-Item -LiteralPath $imguiLicense -Destination (Join-Path $plainDir 'DearImGui-LICENSE.txt')

    # TMLoader package: the same binary named TMFOXR.dll inside a product folder.
    $tmloaderDir = Join-Path $stagingRoot 'tmloader'
    $productPath = Join-Path $tmloaderDir 'TMFOXR'
    $versionPath = Join-Path $productPath $TMLoaderVersion
    New-Item -ItemType Directory -Path $versionPath | Out-Null
    Write-TMFOXRTMLoaderDescriptions -ProductPath $productPath -VersionPath $versionPath -Changelog $Changelog
    Copy-Item -LiteralPath $modDll -Destination (Join-Path $versionPath 'TMFOXR.dll')
    Copy-Item -LiteralPath $OpenXrLoader -Destination (Join-Path $versionPath 'openxr_loader.dll')
    Copy-Item -LiteralPath $defaultsIni -Destination (Join-Path $versionPath 'TMFOXR.defaults.ini')
    Copy-Item -LiteralPath $imguiLicense -Destination (Join-Path $versionPath 'DearImGui-LICENSE.txt')

    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $outputs = @(
        @{ Source = $plainDir; Zip = Join-Path $OutputDir "TMFOXR-$Name.zip" },
        @{ Source = $tmloaderDir; Zip = Join-Path $OutputDir "TMFOXR-$Name-for-TMLoader.zip" }
    )
    foreach ($output in $outputs) {
        if (Test-Path -LiteralPath $output.Zip) { Remove-Item -LiteralPath $output.Zip -Force }
        New-ZipFromDirectory -Source $output.Source -Destination $output.Zip
        Write-Host "Created '$($output.Zip)'."
    }
}
finally {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
}
