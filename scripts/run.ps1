<#
.SYNOPSIS
    Launches the built SNDS app, waits for it to close, and returns its exit code.
.PARAMETER Config
    Debug (default) or Release.
.PARAMETER AppArguments
    Passed to the app unchanged, for example: scripts\run.ps1 -Config Release --smoke-exit
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Debug',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $AppArguments = @()
)

. "$PSScriptRoot\common.ps1"
trap { Write-Host "run failed: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }

$exe = Get-AppExecutable $Config
if (-not (Test-Path -LiteralPath $exe)) {
    throw "$(Protect-Path $exe) does not exist. Run scripts\build.ps1 -Config $Config first."
}

# Quote each argument by the rules CommandLineToArgvW uses to split it again.
$quoted = foreach ($argument in $AppArguments) {
    if ($argument -and $argument -notmatch '[\s"]') {
        $argument
    } else {
        '"' + (($argument -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
    }
}

Write-Step "Running build\SNDS_artefacts\$Config\SNDS.exe $($quoted -join ' ')"
$startArguments = @{ FilePath = $exe; PassThru = $true }
if ($quoted) {
    $startArguments.ArgumentList = ($quoted -join ' ')
}
$process = Start-Process @startArguments
$null = $process.Handle  # keeps the handle so ExitCode is available after exit
$process.WaitForExit()
Write-Host "SNDS exited with code $($process.ExitCode)."
exit $process.ExitCode
