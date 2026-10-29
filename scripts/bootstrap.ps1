<#
.SYNOPSIS
    Creates or updates the SNDS worker Python environment (worker\.venv) with uv.

.DESCRIPTION
    Runs "uv sync --project worker --extra <cpu|cuda> --frozen" against the committed uv.lock. uv provisions
    the pinned CPython itself; nothing is activated and no global Python packages are touched.

.PARAMETER Compute
    auto (default): cuda when nvidia-smi reports an NVIDIA GPU, otherwise cpu.
    cpu / cuda: force a PyTorch build. The extras are mutually exclusive; switching replaces the other.

.PARAMETER DryRun
    Show what uv would change without modifying worker\.venv.

.EXAMPLE
    scripts\bootstrap.ps1
    scripts\bootstrap.ps1 -Compute cpu
#>
[CmdletBinding()]
param(
    [ValidateSet('auto', 'cpu', 'cuda')]
    [string]$Compute = 'auto',
    [switch]$DryRun
)

. (Join-Path $PSScriptRoot '_common.ps1')

# Reads the extra names declared in [project.optional-dependencies] of worker\pyproject.toml.
function Get-WorkerExtras {
    $pyproject = Join-Path $SndsWorkerDir 'pyproject.toml'
    if (-not (Test-Path -LiteralPath $pyproject)) {
        throw "Worker project not found: $pyproject"
    }
    $extras = @()
    $inSection = $false
    foreach ($line in (Get-Content -LiteralPath $pyproject)) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            $inSection = ($Matches[1].Trim() -eq 'project.optional-dependencies')
            continue
        }
        if ($inSection -and $line -match '^\s*([A-Za-z0-9_.-]+)\s*=') {
            $extras += $Matches[1]
        }
    }
    return $extras
}

function Resolve-ComputeExtra {
    param([Parameter(Mandatory = $true)][string]$Requested)
    if ($Requested -ne 'auto') {
        return $Requested
    }
    $nvidiaSmi = Get-SndsCommandPath -Name 'nvidia-smi'
    if (-not $nvidiaSmi) {
        Write-Host 'nvidia-smi not found: using the CPU build.'
        return 'cpu'
    }
    $gpus = @(Get-SndsNativeOutput -FilePath $nvidiaSmi -ArgumentList @('-L') | Where-Object { $_ -match '^GPU \d+:' })
    if ($gpus.Count -gt 0) {
        $gpuName = $gpus[0] -replace '\s*\(UUID:[^)]*\)', ''
        Write-Host "NVIDIA GPU detected ($gpuName): using the CUDA build."
        return 'cuda'
    }
    Write-Host 'nvidia-smi reported no GPU: using the CPU build.'
    return 'cpu'
}

function Install-WorkerEnvironment {
    param(
        [Parameter(Mandatory = $true)][string]$Uv,
        [Parameter(Mandatory = $true)][string]$Extra
    )
    $arguments = @('sync', '--project', $SndsWorkerDir, '--extra', $Extra, '--frozen')
    if ($DryRun) {
        $arguments += '--dry-run'
    }
    Write-SndsStep "uv $($arguments -join ' ')"
    Invoke-SndsNative -FilePath $Uv -ArgumentList $arguments | Out-Null
}

function Test-WorkerEnvironment {
    param([Parameter(Mandatory = $true)][string]$Extra)
    if (-not (Test-Path -LiteralPath $SndsWorkerPython)) {
        throw "uv finished but $SndsWorkerPython does not exist."
    }
    Write-SndsStep 'Checking the worker interpreter'
    $probe = 'import sys, torch; print(sys.version.split()[0], torch.__version__, torch.cuda.is_available())'
    $output = @(Get-SndsNativeOutput -FilePath $SndsWorkerPython -ArgumentList @('-c', $probe))
    if ($LASTEXITCODE -ne 0) {
        throw "The worker environment is incomplete: $($output -join ' ')"
    }
    $fields = ($output[-1]).Split(' ')
    Write-Host "Python $($fields[0]), torch $($fields[1]), CUDA available: $($fields[2])"
    if ($Extra -eq 'cuda' -and $fields[2] -ne 'True') {
        Write-SndsWarning 'The CUDA build is installed but no usable CUDA device was found; separation will use the CPU. Run "scripts\bootstrap.ps1 -Compute cpu" for the smaller CPU build.'
    }
}

# Installs the pinned htdemucs checkpoint into <data root>\models\htdemucs through the worker
# ("python -m snds_worker fetch-model"): streamed download, SHA256 verification, atomic rename.
# Idempotent: when the model is already installed it is only re-verified.
function Install-SeparationModel {
    $dataRoot = Get-SndsDataRoot
    if ($dataRoot.Length -gt 3) {
        $dataRoot = $dataRoot.TrimEnd('\')
    }
    Write-SndsStep "Installing the pinned separation model into $(Join-Path $dataRoot 'models\htdemucs')"
    $arguments = @('-u', '-X', 'utf8', '-m', 'snds_worker', 'fetch-model', '--protocol-version', '1', '--data-root', $dataRoot)
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # stdout carries the worker's NDJSON protocol lines; its stderr log goes straight to the console.
        $lines = @(& $SndsWorkerPython @arguments)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    $terminal = $null
    foreach ($line in $lines) {
        $text = "$line".Trim()
        if (-not $text.StartsWith('{')) {
            continue
        }
        try {
            $message = $text | ConvertFrom-Json
        }
        catch {
            continue
        }
        if (@('result', 'failed', 'cancelled') -contains $message.type) {
            $terminal = $message
        }
    }
    if ($exitCode -ne 0 -or $null -eq $terminal -or $terminal.type -ne 'result') {
        $reason = 'the worker ended without a result.'
        if ($null -ne $terminal -and $terminal.type -eq 'failed') {
            $reason = "$($terminal.error.code): $($terminal.error.message)"
        }
        throw "Installing the separation model failed (exit code $exitCode): $reason Re-run scripts\bootstrap.ps1 to retry."
    }
    $model = $terminal.model
    if ($model.downloaded) {
        Write-Host "Downloaded and verified $($model.path) (SHA256 $($model.sha256))."
    }
    else {
        Write-Host "Already installed; verified $($model.path) (SHA256 $($model.sha256))."
    }
}

Invoke-SndsScript {
    $uv = Assert-SndsCommand -Name 'uv' -Hint 'Install uv from https://docs.astral.sh/uv/getting-started/installation/ and open a new terminal.'
    $extras = @(Get-WorkerExtras)
    $extra = Resolve-ComputeExtra -Requested $Compute
    if ($extras -notcontains $extra) {
        throw "worker\pyproject.toml defines extras [$($extras -join ', ')], not '$extra'."
    }

    Install-WorkerEnvironment -Uv $uv -Extra $extra

    if ($DryRun) {
        Write-Host 'Dry run: worker\.venv was not modified.'
        return
    }

    Test-WorkerEnvironment -Extra $extra
    Install-SeparationModel
    Write-SndsStep 'Bootstrap complete. Next: scripts\build.ps1, then scripts\run.ps1'
}
