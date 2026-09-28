# Shared helpers for the SNDS PowerShell entry points. Dot-source only:
#     . (Join-Path $PSScriptRoot '_common.ps1')
# Compatible with Windows PowerShell 5.1 and PowerShell 7. Tools are always called directly by path or
# from PATH; nothing here activates a virtual environment or touches global Python packages.

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$SndsRepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$SndsConfigurePreset = 'vs2022'
$SndsBuildDir = Join-Path $SndsRepoRoot 'build\vs2022'
$SndsWorkerDir = Join-Path $SndsRepoRoot 'worker'
$SndsWorkerPython = Join-Path $SndsWorkerDir '.venv\Scripts\python.exe'

# Pinned user-level formatter, run through "uv tool run" so no global install is needed.
$SndsClangFormatVersion = '23.1.1'

function Write-SndsStep {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-SndsWarning {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Host "WARNING: $Message" -ForegroundColor Yellow
}

# Runs a script body, converting any terminating error into a readable message and exit code 1.
function Invoke-SndsScript {
    param([Parameter(Mandatory = $true)][scriptblock]$Body)
    try {
        & $Body
    }
    catch {
        Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
    exit 0
}

# Runs a native command, streaming its output. Throws on a non-zero exit code unless -AllowFailure,
# and always returns the exit code. Native stderr (progress from uv, cmake, ...) never becomes a
# terminating PowerShell error.
function Invoke-SndsNative {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [switch]$AllowFailure
    )
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $FilePath @ArgumentList | Out-Host
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "Command failed with exit code ${exitCode}: $FilePath $($ArgumentList -join ' ')"
    }
    return $exitCode
}

# Runs a native command and returns its combined output as strings (for version queries).
function Get-SndsNativeOutput {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @()
    )
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $FilePath @ArgumentList 2>&1 | ForEach-Object { "$_" })
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    return $output
}

function Get-SndsCommandPath {
    param([Parameter(Mandatory = $true)][string]$Name)
    $command = Get-Command -Name $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command) {
        return $null
    }
    return $command.Path
}

function Assert-SndsCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Hint
    )
    $path = Get-SndsCommandPath -Name $Name
    if (-not $path) {
        throw "$Name was not found on PATH. $Hint"
    }
    return $path
}

# Returns the newest Visual Studio 2022 instance with the x64 C++ toolset, or $null.
function Get-SndsVisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        return $null
    }
    $json = Get-SndsNativeOutput -FilePath $vswhere -ArgumentList @(
        '-latest', '-products', '*', '-version', '[17.0,18.0)',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-format', 'json', '-utf8')
    $instances = @(($json -join "`n") | ConvertFrom-Json)
    if ($instances.Count -eq 0) {
        return $null
    }
    return $instances[0]
}

# Mirrors AppPaths::forCurrentUser(): absolute SNDS_DATA_DIR, else %LOCALAPPDATA%\snds.
function Get-SndsDataRoot {
    $override = $env:SNDS_DATA_DIR
    if ($override) {
        $override = $override.Trim().Trim('"')
        if ([System.IO.Path]::IsPathRooted($override)) {
            return $override
        }
    }
    return Join-Path $env:LOCALAPPDATA 'snds'
}

function Get-SndsExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Config
    )
    return Join-Path $SndsBuildDir "bin\$Config\$Name.exe"
}

function Invoke-SndsConfigure {
    param([switch]$Clean)
    $cmake = Assert-SndsCommand -Name 'cmake' -Hint 'Install CMake 3.25 or newer (for example "uv tool install cmake").'
    if ($null -eq (Get-SndsVisualStudio)) {
        throw 'Visual Studio 2022 (or Build Tools) with the "Desktop development with C++" workload was not found.'
    }
    if ($Clean -and (Test-Path -LiteralPath $SndsBuildDir)) {
        Write-SndsStep "Removing $SndsBuildDir"
        Remove-Item -LiteralPath $SndsBuildDir -Recurse -Force
    }
    Write-SndsStep "Configuring ($SndsConfigurePreset)"
    Push-Location -LiteralPath $SndsRepoRoot
    try {
        Invoke-SndsNative -FilePath $cmake -ArgumentList @('--preset', $SndsConfigurePreset) | Out-Null
    }
    finally {
        Pop-Location
    }
}

# Replaces the user profile path in text shown by diagnostics.
function Protect-SndsText {
    param([AllowEmptyString()][string]$Text)
    if ([string]::IsNullOrEmpty($Text) -or -not $env:USERPROFILE) {
        return $Text
    }
    $pattern = [regex]::Escape($env:USERPROFILE.TrimEnd('\'))
    return [regex]::Replace($Text, $pattern, '%USERPROFILE%', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
}
