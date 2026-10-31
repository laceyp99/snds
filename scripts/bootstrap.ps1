<#
.SYNOPSIS
    Sets up the SNDS worker environment and downloads the separation model.
.DESCRIPTION
    Syncs worker\.venv from worker\uv.lock with a uv-managed Python, then downloads and verifies
    the pinned htdemucs and beat_this (BPM) checkpoints into the SNDS data directory (SNDS_HOME, default
    %LOCALAPPDATA%\snds). Safe to re-run: a verified checkpoint is not downloaded again.
.PARAMETER Compute
    PyTorch build to install: auto (cuda if nvidia-smi works, else cpu), cpu, or cuda.
.PARAMETER SkipModel
    Set up the environment only; do not download the checkpoint.
#>
param(
    [ValidateSet('auto', 'cpu', 'cuda')]
    [string] $Compute = 'auto',
    [switch] $SkipModel
)

. "$PSScriptRoot\common.ps1"
trap { Write-Host "bootstrap failed: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }

$uv = Get-RequiredCommand 'uv' 'Install uv from https://docs.astral.sh/uv/getting-started/installation/ and open a new terminal.'

if ($Compute -eq 'auto') {
    if (Test-NvidiaGpu) {
        $Compute = 'cuda'
        Write-Step 'NVIDIA driver detected: installing the CUDA build of PyTorch'
    } else {
        $Compute = 'cpu'
        Write-Step 'No NVIDIA driver detected: installing the CPU build of PyTorch'
    }
}

Write-Step "Syncing the worker environment ($Compute) into worker\.venv"
Invoke-Native $uv @('sync', '--project', $SndsWorkerRoot, '--locked', '--extra', $Compute, '--managed-python')

if ($SkipModel) {
    Write-Step 'Skipping the model download (-SkipModel)'
} else {
    Write-Step "Fetching the htdemucs and beat_this checkpoints into $(Protect-Path (Get-SndsHome))"
    $fetch = Invoke-WorkerJson 'fetch-model'
    if ($fetch.ExitCode -ne 0 -or -not $fetch.Payload -or -not $fetch.Payload.ok) {
        $reason = if ($fetch.Payload -and $fetch.Payload.problems) { $fetch.Payload.problems[0].message } else { $fetch.Text }
        throw "Model download failed: $reason"
    }
    $state = if (@($fetch.Payload.models | Where-Object { $_.downloaded }).Count -gt 0) { 'downloaded and verified' } else { 'already present and verified' }
    Write-Host "Checkpoints $state." -ForegroundColor Green
}

Write-Step 'Checking worker readiness'
$readiness = Invoke-WorkerJson 'readiness'
$report = $readiness.Payload
if (-not $report) {
    throw "readiness printed no report (exit code $($readiness.ExitCode))."
}

$device = $report.compute.device
if ($report.compute.cuda_device) {
    $device = "$device ($($report.compute.cuda_device))"
}
Write-Host "PyTorch $($report.packages.torch), $($report.compute.torch_build) build; separation device: $device"

$problems = @($report.problems)
if ($SkipModel) {
    $problems = @($problems | Where-Object { $_.code -notlike 'model_*' -and $_.code -notlike 'bpm_model_*' })
}
foreach ($problem in $problems) {
    Write-Host "  [$($problem.code)] $($problem.message)" -ForegroundColor Yellow
}

if ($problems.Count -gt 0) {
    Write-Host 'The worker is not ready. Run scripts\diagnostics.ps1 for details.' -ForegroundColor Red
    exit 1
}

$summary = if ($report.ready) { 'The worker is ready.' } else { 'The worker environment is ready; the model was skipped.' }
Write-Host $summary -ForegroundColor Green
exit 0
