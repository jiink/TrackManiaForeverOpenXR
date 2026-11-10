# Writes the two TrackMania ModLoader description.yaml files for the TMFOXR
# product. Dot-sourced by install-tmloader.ps1 and package-release.ps1 so a
# local install and a release package describe the mod identically.

function Write-TMFOXRTMLoaderDescriptions {
    param(
        [Parameter(Mandatory)][string]$ProductPath,
        [Parameter(Mandatory)][string]$VersionPath,
        [Parameter(Mandatory)][string]$Changelog
    )

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
changelog: $Changelog
"@
    [System.IO.File]::WriteAllText(
        (Join-Path $ProductPath 'description.yaml'), $productDescription.Trim() + "`r`n")
    [System.IO.File]::WriteAllText(
        (Join-Path $VersionPath 'description.yaml'), $versionDescription.Trim() + "`r`n")
}
