<#
.SYNOPSIS
    Prints an SNDS environment report for troubleshooting (safe to paste into an issue).

.DESCRIPTION
    Reports OS, toolchain, uv, worker environment, NVIDIA GPU, disk space, SNDS data folders and build
    outputs. The user profile path is shown as %USERPROFILE%; no environment variables or secrets are
    dumped. Missing items are reported, not treated as errors.

.PARAMETER Deep
    Also import torch in the worker environment to report CUDA availability (takes a few seconds).

.PARAMETER GlProbe
    Also run the OpenGL capability probe (briefly opens a small window).
#>
[CmdletBinding()]
param(
    [switch]$Deep,
    [switch]$GlProbe
)

. (Join-Path $PSScriptRoot '_common.ps1')

function Write-Section {
    param([Parameter(Mandatory = $true)][string]$Title)
    Write-Host ''
    Write-Host "[$Title]" -ForegroundColor Cyan
}

function Write-Item {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [AllowEmptyString()][AllowNull()][string]$Value
    )
    if ([string]::IsNullOrWhiteSpace($Value)) {
        $Value = '(not found)'
    }
    Write-Host ('  {0,-22} {1}' -f $Label, (Protect-SndsText -Text $Value))
}

function Get-FirstLine {
    param([string[]]$Lines)
    $first = @($Lines | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) | Select-Object -First 1
    if ($null -eq $first) {
        return ''
    }
    return $first.Trim()
}

function Get-ToolVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string[]]$ArgumentList = @('--version')
    )
    $path = Get-SndsCommandPath -Name $Name
    if (-not $path) {
        return ''
    }
    return "$(Get-FirstLine (Get-SndsNativeOutput -FilePath $path -ArgumentList $ArgumentList))  ($path)"
}

function Get-FreeSpace {
    param([Parameter(Mandatory = $true)][string]$Path)
    $qualifier = Split-Path -Path $Path -Qualifier
    $drive = Get-PSDrive -Name $qualifier.TrimEnd(':') -PSProvider FileSystem -ErrorAction SilentlyContinue
    if ($null -eq $drive) {
        return ''
    }
    return '{0}  {1:N1} GB free of {2:N1} GB' -f $qualifier, ($drive.Free / 1GB), (($drive.Used + $drive.Free) / 1GB)
}

Invoke-SndsScript {
    Write-Host "SNDS diagnostics  $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"

    Write-Section 'System'
    $os = Get-CimInstance -ClassName Win32_OperatingSystem
    $cpu = Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1
    Write-Item 'OS' "$($os.Caption) $($os.Version) ($($os.OSArchitecture))"
    Write-Item 'CPU' "$($cpu.Name.Trim()) ($($cpu.NumberOfCores) cores, $($cpu.NumberOfLogicalProcessors) threads)"
    Write-Item 'Memory' ('{0:N1} GB' -f ($os.TotalVisibleMemorySize / 1MB))
    Write-Item 'PowerShell' "$($PSVersionTable.PSVersion) ($($PSVersionTable.PSEdition))"
    foreach ($adapter in @(Get-CimInstance -ClassName Win32_VideoController)) {
        Write-Item 'Display adapter' "$($adapter.Name) (driver $($adapter.DriverVersion))"
    }

    Write-Section 'Repository'
    Write-Item 'Root' $SndsRepoRoot
    $git = Get-SndsCommandPath -Name 'git'
    if ($git) {
        $commit = Get-FirstLine (Get-SndsNativeOutput -FilePath $git -ArgumentList @('-C', $SndsRepoRoot, 'rev-parse', '--short', 'HEAD'))
        $branch = Get-FirstLine (Get-SndsNativeOutput -FilePath $git -ArgumentList @('-C', $SndsRepoRoot, 'rev-parse', '--abbrev-ref', 'HEAD'))
        $changes = @(Get-SndsNativeOutput -FilePath $git -ArgumentList @('-C', $SndsRepoRoot, 'status', '--porcelain')).Count
        Write-Item 'Commit' "$commit on $branch ($changes changed paths)"
    }
    else {
        Write-Item 'Commit' ''
    }

    Write-Section 'Toolchain'
    Write-Item 'git' (Get-ToolVersion -Name 'git')
    Write-Item 'cmake' (Get-ToolVersion -Name 'cmake')
    Write-Item 'ninja' (Get-ToolVersion -Name 'ninja')
    $vs = Get-SndsVisualStudio
    if ($null -ne $vs) {
        Write-Item 'Visual Studio' "$($vs.displayName) $($vs.installationVersion)"
        $msvcRoot = Join-Path $vs.installationPath 'VC\Tools\MSVC'
        $toolsets = @(Get-ChildItem -LiteralPath $msvcRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        Write-Item 'MSVC toolsets' ($toolsets -join ', ')
    }
    else {
        Write-Item 'Visual Studio' ''
    }
    Write-Item 'uv' (Get-ToolVersion -Name 'uv')
    Write-Item 'clang-format (pinned)' "$SndsClangFormatVersion via uv tool run"

    Write-Section 'Worker'
    Write-Item 'Project' $(if (Test-Path -LiteralPath (Join-Path $SndsWorkerDir 'pyproject.toml')) { Join-Path $SndsWorkerDir 'pyproject.toml' } else { '' })
    Write-Item 'Lockfile' $(if (Test-Path -LiteralPath (Join-Path $SndsWorkerDir 'uv.lock')) { Join-Path $SndsWorkerDir 'uv.lock' } else { '' })
    if (Test-Path -LiteralPath $SndsWorkerPython) {
        Write-Item 'Python' "$(Get-FirstLine (Get-SndsNativeOutput -FilePath $SndsWorkerPython -ArgumentList @('--version')))  ($SndsWorkerPython)"
        # Python snippets avoid double quotes: Windows PowerShell 5.1 does not escape them for native commands.
        $versionsScript = @'
import importlib.metadata as m
for name in ('torch', 'demucs', 'numpy', 'soundfile', 'pytest', 'ruff'):
    try:
        print(name + '=' + m.version(name))
    except m.PackageNotFoundError:
        print(name + '=(not installed)')
'@
        foreach ($line in @(Get-SndsNativeOutput -FilePath $SndsWorkerPython -ArgumentList @('-c', $versionsScript))) {
            $parts = $line.Split('=', 2)
            if ($parts.Count -eq 2) {
                Write-Item $parts[0] $parts[1]
            }
        }
        if ($Deep) {
            $cudaScript = 'import torch; print(torch.cuda.is_available(), torch.version.cuda, torch.cuda.get_device_name(0) if torch.cuda.is_available() else ''-'')'
            Write-Item 'torch CUDA' (Get-FirstLine (Get-SndsNativeOutput -FilePath $SndsWorkerPython -ArgumentList @('-c', $cudaScript)))
        }
    }
    else {
        Write-Item 'Python' "missing ($SndsWorkerPython); run scripts\bootstrap.ps1"
    }

    Write-Section 'NVIDIA'
    $nvidiaSmi = Get-SndsCommandPath -Name 'nvidia-smi'
    if ($nvidiaSmi) {
        foreach ($line in @(Get-SndsNativeOutput -FilePath $nvidiaSmi -ArgumentList @('--query-gpu=name,driver_version,memory.total', '--format=csv,noheader'))) {
            Write-Item 'GPU' $line
        }
    }
    else {
        Write-Item 'nvidia-smi' ''
    }

    Write-Section 'Storage'
    $dataRoot = Get-SndsDataRoot
    Write-Item 'Repository drive' (Get-FreeSpace -Path $SndsRepoRoot)
    Write-Item 'Data drive' (Get-FreeSpace -Path $dataRoot)

    Write-Section 'SNDS data'
    Write-Item 'Root' $dataRoot
    foreach ($name in @('settings', 'cache', 'jobs', 'logs', 'models')) {
        $folder = Join-Path $dataRoot $name
        if (Test-Path -LiteralPath $folder) {
            $bytes = (Get-ChildItem -LiteralPath $folder -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
            if ($null -eq $bytes) {
                $bytes = 0
            }
            Write-Item $name ('{0}  ({1:N1} MB)' -f $folder, ($bytes / 1MB))
        }
        else {
            Write-Item $name "$folder (not created yet)"
        }
    }

    Write-Section 'Build outputs'
    foreach ($config in @('Debug', 'Release')) {
        foreach ($name in @('SNDS', 'snds_tests', 'snds_gl_probe')) {
            $exe = Get-SndsExecutable -Name $name -Config $config
            if (Test-Path -LiteralPath $exe) {
                Write-Item "$config $name" "$exe  ($((Get-Item -LiteralPath $exe).LastWriteTime.ToString('yyyy-MM-dd HH:mm')))"
            }
            else {
                Write-Item "$config $name" ''
            }
        }
    }

    if ($GlProbe) {
        Write-Section 'OpenGL probe'
        $probe = Get-SndsExecutable -Name 'snds_gl_probe' -Config 'Release'
        if (-not (Test-Path -LiteralPath $probe)) {
            $probe = Get-SndsExecutable -Name 'snds_gl_probe' -Config 'Debug'
        }
        if (Test-Path -LiteralPath $probe) {
            foreach ($line in @(Get-SndsNativeOutput -FilePath $probe -ArgumentList @('--run-ms=3000'))) {
                Write-Host "  $(Protect-SndsText -Text $line)"
            }
        }
        else {
            Write-Item 'snds_gl_probe' 'not built; run scripts\build.ps1'
        }
    }
}
