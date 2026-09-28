<#
.SYNOPSIS
    Launches the built SNDS app.

.DESCRIPTION
    The app finds the worker project by walking up from build\vs2022\bin\<Config>\SNDS.exe to this
    checkout, and keeps its data under %LOCALAPPDATA%\snds (or an absolute SNDS_DATA_DIR).

.PARAMETER Config
    Debug (default) or Release.

.PARAMETER Wait
    Wait for the app to exit and return its exit code.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [switch]$Wait,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AppArguments = @()
)

. (Join-Path $PSScriptRoot '_common.ps1')

Invoke-SndsScript {
    $app = Get-SndsExecutable -Name 'SNDS' -Config $Config
    if (-not (Test-Path -LiteralPath $app)) {
        throw "SNDS ($Config) is not built. Run: scripts\build.ps1 -Config $Config"
    }

    $startArguments = @{ FilePath = $app; PassThru = $true; WorkingDirectory = $SndsRepoRoot }
    if ($AppArguments.Count -gt 0) {
        $startArguments['ArgumentList'] = $AppArguments
    }

    Write-SndsStep "Starting $app"
    $process = Start-Process @startArguments
    Write-Host "SNDS is running (process $($process.Id)). Logs: $(Join-Path (Get-SndsDataRoot) 'logs')"

    if ($Wait) {
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) {
            throw "SNDS exited with code $($process.ExitCode)."
        }
    }
}
