<#
.SYNOPSIS
    Builds SNDS (app, C++ tests, spikes). Configures first when no build tree exists.

.PARAMETER Config
    Debug (default) or Release.

.PARAMETER Target
    Optional CMake target names (for example snds_app, snds_tests, snds_gl_probe).
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [string[]]$Target = @()
)

. (Join-Path $PSScriptRoot '_common.ps1')

Invoke-SndsScript {
    $cmake = Assert-SndsCommand -Name 'cmake' -Hint 'Install CMake 3.25 or newer (for example "uv tool install cmake").'

    if (-not (Test-Path -LiteralPath (Join-Path $SndsBuildDir 'CMakeCache.txt'))) {
        Write-SndsStep 'No build tree yet: configuring first'
        Invoke-SndsConfigure
    }

    $arguments = @('--build', $SndsBuildDir, '--config', $Config, '--parallel')
    if ($Target.Count -gt 0) {
        $arguments += '--target'
        $arguments += $Target
    }

    Write-SndsStep "Building $Config"
    Invoke-SndsNative -FilePath $cmake -ArgumentList $arguments | Out-Null

    $app = Get-SndsExecutable -Name 'SNDS' -Config $Config
    if (Test-Path -LiteralPath $app) {
        Write-SndsStep "Built $app"
    }
}
