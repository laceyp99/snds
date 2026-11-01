# Shared helpers for the SNDS scripts. Dot-source it from a script in this folder:
#   . "$PSScriptRoot\common.ps1"
# Targets Windows PowerShell 5.1. Tools are called directly; nothing is activated and no global
# Python packages are touched.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$SndsRepoRoot = Split-Path -Parent $PSScriptRoot
$SndsBuildDir = Join-Path $SndsRepoRoot 'build'
$SndsWorkerRoot = Join-Path $SndsRepoRoot 'worker'
$SndsWorkerPython = Join-Path $SndsWorkerRoot '.venv\Scripts\python.exe'
$SndsConfigurations = @('Debug', 'Release')

# Native programs inherit the process directory, which PowerShell does not keep in sync with
# Set-Location. Align them so relative paths mean the same thing to every tool.
[Environment]::CurrentDirectory = (Get-Location -PSProvider FileSystem).ProviderPath

function Write-Step {
    param([Parameter(Mandatory)][string] $Message)
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Invoke-Native {
    # Runs a program, streams its output, and throws if it exits non-zero. stderr is never
    # redirected: PowerShell 5.1 turns redirected native stderr into error records.
    param(
        [Parameter(Mandatory)][string] $FilePath,
        [string[]] $ArgumentList = @()
    )
    $shown = @($ArgumentList | Select-Object -First 10 | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
    if ($ArgumentList.Count -gt 10) {
        $shown += "... ($($ArgumentList.Count - 10) more)"
    }
    Write-Host ("> {0} {1}" -f (Split-Path -Leaf $FilePath), ($shown -join ' ')) -ForegroundColor DarkGray
    $ErrorActionPreference = 'Continue'
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $FilePath) exited with code $LASTEXITCODE."
    }
}

function Get-RequiredCommand {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $InstallHint
    )
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) {
        throw "$Name was not found on PATH. $InstallHint"
    }
    return $command.Source
}

function Get-VisualStudioPath {
    # Returns the latest Visual Studio installation with the x64 C++ tools, or $null.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        return $null
    }
    $ErrorActionPreference = 'Continue'
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $path) {
        return $null
    }
    return @($path)[0]
}

function Get-CMake {
    # cmake.exe from PATH, else the copy bundled with Visual Studio.
    $command = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command.Source
    }
    $vs = Get-VisualStudioPath
    if ($vs) {
        $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $bundled) {
            return $bundled
        }
    }
    throw 'CMake was not found. Install CMake 3.22 or newer, or the Visual Studio "C++ CMake tools for Windows" component.'
}

function Get-CTest {
    return Join-Path (Split-Path -Parent (Get-CMake)) 'ctest.exe'
}

function Get-AppExecutable {
    param([Parameter(Mandatory)][string] $Config)
    return Join-Path $SndsBuildDir "SNDS_artefacts\$Config\SNDS.exe"
}

function Get-SndsHome {
    # Same rule as the app and the worker: SNDS_HOME if set, else %LOCALAPPDATA%\snds.
    $override = "$env:SNDS_HOME".Trim().Trim('"').Trim()
    if ($override) {
        return [IO.Path]::GetFullPath($override)
    }
    if (-not $env:LOCALAPPDATA) {
        throw 'Neither SNDS_HOME nor LOCALAPPDATA is set.'
    }
    return Join-Path $env:LOCALAPPDATA 'snds'
}

function Test-NvidiaGpu {
    # True if nvidia-smi exists and can talk to a driver.
    $smi = Get-Command nvidia-smi -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $smi) {
        return $false
    }
    $ErrorActionPreference = 'Continue'
    & $smi.Source -L *> $null
    return ($LASTEXITCODE -eq 0)
}

function Invoke-WorkerJson {
    # Runs `python -m snds_worker <command>` in the worker environment. Returns the exit code
    # and the parsed JSON object from stdout ($null if there was none). stderr passes through.
    param([Parameter(Mandatory)][string] $Command)
    if (-not (Test-Path -LiteralPath $SndsWorkerPython)) {
        throw "The worker environment is missing ($SndsWorkerPython). Run scripts\bootstrap.ps1."
    }
    $ErrorActionPreference = 'Continue'
    # -I (isolated mode) ignores PYTHON* variables and the user site folder, as the app does.
    $lines = & $SndsWorkerPython -I -m snds_worker $Command
    $exitCode = $LASTEXITCODE
    $text = (@($lines) -join "`n").Trim()
    $payload = $null
    if ($text) {
        try {
            $payload = $text | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $payload = $null
        }
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Payload = $payload; Text = $text }
}

function Protect-Path {
    # Replaces %LOCALAPPDATA% and %USERPROFILE% paths (case-insensitive, either separator) with
    # the variable name. Same rule as the app and the worker logs.
    param([Parameter(ValueFromPipeline)][AllowEmptyString()][AllowNull()][string] $Text)
    process {
        if (-not $Text) {
            return $Text
        }
        $variables = foreach ($name in 'LOCALAPPDATA', 'USERPROFILE') {
            $value = "$([Environment]::GetEnvironmentVariable($name))".Trim().TrimEnd('\', '/')
            if ($value) {
                [pscustomobject]@{ Name = $name; Value = $value }
            }
        }
        $result = $Text
        foreach ($variable in @($variables | Sort-Object { $_.Value.Length } -Descending)) {
            $parts = $variable.Value -split '[\\/]+' | Where-Object { $_ } | ForEach-Object { [regex]::Escape($_) }
            $pattern = '(?i)' + ($parts -join '[\\/]+') + '(?![\w-])'
            $result = [regex]::Replace($result, $pattern, "%$($variable.Name)%")
        }
        return $result
    }
}
