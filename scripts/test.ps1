<#
.SYNOPSIS
    Runs the SNDS C++ tests (ctest) and the worker Python tests (pytest).

.DESCRIPTION
    Python tests run with worker\.venv\Scripts\python.exe directly (no activation). They are skipped with
    a message when the worker environment has not been bootstrapped yet.

.PARAMETER Config
    Build configuration whose C++ tests to run: Debug (default) or Release.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [switch]$SkipCpp,
    [switch]$SkipPython
)

. (Join-Path $PSScriptRoot '_common.ps1')

Invoke-SndsScript {
    $failed = @()

    if (-not $SkipCpp) {
        $ctest = Assert-SndsCommand -Name 'ctest' -Hint 'ctest ships with CMake; install CMake 3.25 or newer.'
        if (-not (Test-Path -LiteralPath (Get-SndsExecutable -Name 'snds_tests' -Config $Config))) {
            throw "The C++ tests ($Config) are not built. Run: scripts\build.ps1 -Config $Config"
        }
        Write-SndsStep "C++ tests ($Config)"
        $exitCode = Invoke-SndsNative -FilePath $ctest -ArgumentList @('--test-dir', $SndsBuildDir, '-C', $Config, '--output-on-failure') -AllowFailure
        if ($exitCode -ne 0) {
            $failed += "C++ tests ($Config)"
        }
    }

    if (-not $SkipPython) {
        if (-not (Test-Path -LiteralPath $SndsWorkerPython)) {
            Write-SndsWarning "Skipping Python tests: $SndsWorkerPython does not exist. Run scripts\bootstrap.ps1 first."
        }
        else {
            Write-SndsStep 'Python worker tests'
            Push-Location -LiteralPath $SndsWorkerDir
            try {
                $exitCode = Invoke-SndsNative -FilePath $SndsWorkerPython -ArgumentList @('-m', 'pytest') -AllowFailure
            }
            finally {
                Pop-Location
            }
            if ($exitCode -ne 0) {
                $failed += 'Python worker tests'
            }
        }
    }

    if ($failed.Count -gt 0) {
        throw "Failed: $($failed -join ', ')"
    }
    Write-SndsStep 'All requested tests passed'
}
