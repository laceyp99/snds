<#
.SYNOPSIS
    Builds and runs the C++ tests (CTest) and runs the Python worker tests (pytest).
.DESCRIPTION
    Both suites run with SNDS_HOME pointing at build\test-home, so tests never touch the real
    SNDS data directory. Both suites always run; the script fails if either fails.
.PARAMETER Config
    Debug (default) or Release, for the C++ tests.
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Debug',
    [switch] $SkipCpp,
    [switch] $SkipPython
)

. "$PSScriptRoot\common.ps1"
trap { Write-Host "test failed: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }

$testHome = Join-Path $SndsBuildDir 'test-home'
if (Test-Path -LiteralPath $testHome) {
    Remove-Item -LiteralPath $testHome -Recurse -Force
}
$null = New-Item -ItemType Directory -Path $testHome -Force

$previousHome = $env:SNDS_HOME
$env:SNDS_HOME = $testHome
$failures = @()

try {
    if (-not $SkipCpp) {
        & "$PSScriptRoot\build.ps1" -Config $Config
        if ($LASTEXITCODE -ne 0) {
            $failures += 'C++ build'
        } else {
            Write-Step "Running C++ tests ($Config)"
            try {
                Invoke-Native (Get-CTest) @('--test-dir', $SndsBuildDir, '-C', $Config, '--output-on-failure', '--parallel', '4')
            } catch {
                $failures += 'C++ tests'
            }
        }
    }

    if (-not $SkipPython) {
        Write-Step 'Running Python worker tests'
        if (-not (Test-Path -LiteralPath $SndsWorkerPython)) {
            throw "The worker environment is missing ($SndsWorkerPython). Run scripts\bootstrap.ps1."
        }
        try {
            # The folder argument makes pytest use worker\pyproject.toml as its configuration.
            Invoke-Native $SndsWorkerPython @('-m', 'pytest', $SndsWorkerRoot)
        } catch {
            $failures += 'Python tests'
        }
    }
} finally {
    $env:SNDS_HOME = $previousHome
}

if ($failures.Count -gt 0) {
    Write-Host "Failed: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'All tests passed.' -ForegroundColor Green
exit 0
