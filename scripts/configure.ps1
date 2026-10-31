<#
.SYNOPSIS
    Configures the SNDS CMake build tree in build\ (Visual Studio 2022, x64).
.DESCRIPTION
    The first configure downloads the pinned JUCE archive and builds JUCE's juceaide helper,
    which takes about a minute. Later runs reuse the build tree.
#>
param()

. "$PSScriptRoot\common.ps1"
trap { Write-Host "configure failed: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }

$cmake = Get-CMake

Write-Step 'Configuring build\ (Visual Studio 17 2022, x64)'
Invoke-Native $cmake @('-S', $SndsRepoRoot, '-B', $SndsBuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
exit 0
