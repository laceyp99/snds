<#
.SYNOPSIS
    Builds the SNDS app and C++ tests, configuring build\ first if needed.
.PARAMETER Config
    Debug (default) or Release.
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Debug'
)

. "$PSScriptRoot\common.ps1"
trap { Write-Host "build failed: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }

if (-not (Test-Path -LiteralPath (Join-Path $SndsBuildDir 'CMakeCache.txt'))) {
    & "$PSScriptRoot\configure.ps1"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

$cmake = Get-CMake

Write-Step "Building $Config"
Invoke-Native $cmake @('--build', $SndsBuildDir, '--config', $Config, '--parallel')
Write-Host "Built build\SNDS_artefacts\$Config\SNDS.exe" -ForegroundColor Green
exit 0
