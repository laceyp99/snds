<#
.SYNOPSIS
    Configures the SNDS CMake build tree (build\vs2022) with the vs2022 preset.

.DESCRIPTION
    JUCE is fetched at the pinned commit and verified by SHA256 during the first configure.

.PARAMETER Clean
    Delete build\vs2022 first for a from-scratch configure.
#>
[CmdletBinding()]
param(
    [switch]$Clean
)

. (Join-Path $PSScriptRoot '_common.ps1')

Invoke-SndsScript {
    Invoke-SndsConfigure -Clean:$Clean
    Write-SndsStep "Configured $SndsBuildDir. Next: scripts\build.ps1"
}
